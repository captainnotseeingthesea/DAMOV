/**
 * Copyright (c) 2019 Metempsy Technology Consulting
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

 /**
  * Implementation of the Spatio-Temporal Memory Streaming Prefetcher (STeMS)
  * Reference:
  *    Spatio-temporal memory streaming.
  *    Somogyi, S., Wenisch, T. F., Ailamaki, A., & Falsafi, B. (2009).
  *    ACM SIGARCH Computer Architecture News, 37(3), 69-80.
  *
  * Notes:
  * - The functionality described in the paper as Streamed Value Buffer (SVB)
  *   is not implemented here, as this is handled by the QueuedPrefetcher class
  */

#ifndef __PREFETCH_SPATIO_TEMPORAL_MEMORY_STREAMING_HH__
#define __PREFETCH_SPATIO_TEMPORAL_MEMORY_STREAMING_HH__

#include <vector>

#include "g_std/g_string.h"
#include "stats.h"
#include "prefetch/prefetcher.h"
#include "associative_array_impl.h"
#include "replacement_policies/lru_rp.h"
#include "indexing_policies/set_associative.h"
#include "base/sat_counter.h"
#include "g_std/g_vector.h"
#include "base/circular_queue.h"

struct STeMSPrefetcherParams : public PrefetcherParams
{
    unsigned spatial_region_size;
    unsigned active_generation_table_entries;
    unsigned active_generation_table_assoc;
    BaseIndexingPolicy *active_generation_table_indexing_policy;
    BaseReplPolicy *active_generation_table_replacement_policy;

    unsigned pattern_sequence_table_entries;
    unsigned pattern_sequence_table_assoc;
    BaseIndexingPolicy *pattern_sequence_table_indexing_policy;
    BaseReplPolicy *pattern_sequence_table_replacement_policy;

    unsigned region_miss_order_buffer_entries;
    bool add_duplicate_entries_to_rmob;
    unsigned reconstruction_entries;
};

class STeMS : public Prefetcher
{
    /** Size of each spatial region */
    const size_t spatialRegionSize;
    /** log_2 of the spatial region size */
    const size_t spatialRegionSizeBits;
    /** Number of reconstruction entries */
    const unsigned int reconstructionEntries;

    /**
     * Entry data type for the Active Generation Table (AGT) and the Pattern
     * Sequence Table (PST)
     */
    struct ActiveGenerationTableEntry : public TaggedEntry
    {
        /** Physical address of the spatial region */
        Address paddress;
        /** PC that started this generation */
        Address pc;
        /** Counter to keep track of the interleaving between sequences */
        unsigned int seqCounter;
        /** Trigger offset */
        unsigned int triggerOffset;

        /** Sequence entry data type */
        struct SequenceEntry
        {
            /** 2-bit confidence counter */
            SatCounter8 counter;
            /** Offset, in cache lines, within the spatial region */
            unsigned int offset;
            /** Intearleaving position on the global access sequence */
            unsigned int delta;
            SequenceEntry() : counter(2), offset(0), delta(0)
            {}
        };
        /** Sequence of accesses */
        g_vector<SequenceEntry> sequence;

        ActiveGenerationTableEntry(int num_positions)
          : TaggedEntry(), paddress(0), pc(0),
            seqCounter(0), sequence(num_positions)
        {
        }

        void
        invalidate() override
        {
            TaggedEntry::invalidate();
            paddress = 0;
            pc = 0;
            seqCounter = 0;
            for (auto &seq_entry : sequence) {
                seq_entry.counter.reset();
                seq_entry.offset = 0;
                seq_entry.delta = 0;
            }
        }

        /**
         * Update the entry data with an entry from a generation that just
         * ended. This operation can not be done with the copy constructor,
         * becasuse the TaggedEntry component must not be copied.
         * @param e entry which generation has ended
         */
        void update(ActiveGenerationTableEntry const &e)
        {
            paddress = e.paddress;
            pc = e.pc;
            seqCounter = e.seqCounter;
            sequence = e.sequence;
        }

        /**
         * Add a new access to the sequence
         * @param offset offset in cachelines within the spatial region
         */
        void addOffset(unsigned int offset) {
            // Search for the offset in the deltas array, if it exist, update
            // the corresponding counter, if not, add the offset to the array
            for (auto &seq_entry : sequence) {
                if (seq_entry.counter > 0) {
                    if (seq_entry.offset == offset) {
                        seq_entry.counter++;
                    }
                } else {
                    // If the counter is 0 it means that this position is not
                    // being used, and we can allocate the new offset here
                    seq_entry.counter++;
                    seq_entry.offset = offset;
                    seq_entry.delta = seqCounter;
                    break;
                }
            }
            seqCounter = 0;
        }
    };

    /** Active Generation Table (AGT) */
    AssociativeArray<ActiveGenerationTableEntry> activeGenerationTable;
    /** Pattern Sequence Table (PST) */
    AssociativeArray<ActiveGenerationTableEntry> patternSequenceTable;

    /** Data type of the Region Miss Order Buffer entry */
    struct RegionMissOrderBufferEntry
    {
        /** Address of the spatial region */
        Address srAddress;
        /**
         * Address used to index the PST table, generated using the PC and the
         * offset within the spatial region
         */
        Address pstAddress;
        /** Delta within the global miss order sequence */
        unsigned int delta;
    };

    /** Region Miss Order Buffer (RMOB) */
    CircularQueue<RegionMissOrderBufferEntry> rmob;

    /** Add duplicate entries to RMOB  */
    bool addDuplicateEntriesToRMOB;

    /** Counter to keep the count of accesses between trigger accesses */
    unsigned int lastTriggerCounter;

    /** Checks if the active generations have ended */
    void checkForActiveGenerationsEnd();
    /**
     * Adds an entry to the RMOB
     * @param sr_addr Spatial region address
     * @param pst_addr Corresponding PST address
     * @param delta Number of entries skipped in the global miss order
     */
    void addToRMOB(Address sr_addr, Address pst_addr, unsigned int delta);

    /**
     * Reconstructs a sequence of accesses and generates the prefetch
     * addresses, adding them to the addresses vector
     *
     * @param rmob_it rmob position to start generating from.
     * @param addresses vector to add the addresses to be prefetched
     */
    void reconstructSequence(
        CircularQueue<RegionMissOrderBufferEntry>::iterator rmob_it,
        g_vector<AddrPriority> &addresses);

  public:
    STeMS(const g_string &_name, const STeMSPrefetcherParams &p);
    ~STeMS() = default;

    static STeMSPrefetcherParams buildParams(Config &config, const std::string &prefix)
    {
        STeMSPrefetcherParams p;
        static_cast<PrefetcherParams&>(p) = Prefetcher::buildParams(config, prefix);

        p.spatial_region_size = config.get<uint32_t>(prefix + "spatial_region_size", 2048);
        p.active_generation_table_entries = config.get<uint32_t>(prefix + "agt_entries", 64);
        p.active_generation_table_assoc = config.get<uint32_t>(prefix + "agt_assoc", 64);
        p.active_generation_table_indexing_policy =
            new SetAssociative(p.active_generation_table_assoc,
                              p.active_generation_table_entries, 1);
        p.active_generation_table_replacement_policy = new LRU();

        p.pattern_sequence_table_entries = config.get<uint32_t>(prefix + "pst_entries", 16384);
        p.pattern_sequence_table_assoc = config.get<uint32_t>(prefix + "pst_assoc", 16384);
        p.pattern_sequence_table_indexing_policy =
            new SetAssociative(p.pattern_sequence_table_assoc,
                              p.pattern_sequence_table_entries, 1);
        p.pattern_sequence_table_replacement_policy = new LRU();

        p.region_miss_order_buffer_entries = config.get<uint32_t>(prefix + "rmob_entries", 131072);
        p.add_duplicate_entries_to_rmob = config.get<bool>(prefix + "add_duplicate_entries_to_rmob", true);
        p.reconstruction_entries = config.get<uint32_t>(prefix + "reconstruction_entries", 256);

        return p;
    }

    void calculatePrefetch(const PrefetchInfo &pfi, uint64_t &respCycle, g_vector<AddrPriority> &addresses) override;
};


#endif//__PREFETCH_SPATIO_TEMPORAL_MEMORY_STREAMING_HH__
