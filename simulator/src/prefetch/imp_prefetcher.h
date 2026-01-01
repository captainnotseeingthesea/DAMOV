/** $lic$
 * Copyright (C) 2012-2015 by Massachusetts Institute of Technology
 * Copyright (C) 2010-2013 by The Board of Trustees of Stanford University
 *
 * This file is part of zsim.
 *
 * zsim is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, version 2.
 *
 * If you use this software in your research, we request that you reference
 * the zsim paper ("ZSim: Fast and Accurate Microarchitectural Simulation of
 * Thousand-Core Systems", Sanchez and Kozyrakis, ISCA-40, June 2013) as the
 * source of the simulator in any publications that use this software, and that
 * you send us a citation of your work.
 *
 * zsim is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * Implementation of the Indirect Memory Prefetcher
 *
 * References:
 * IMP: Indirect memory prefetcher.
 * Yu, X., Hughes, C. J., Satish, N., & Devadas, S. (2015, December).
 * In Proceedings of the 48th International Symposium on Microarchitecture
 * (pp. 178-190). ACM.
 */

#ifndef IMP_PREFETCHER_H_
#define IMP_PREFETCHER_H_

#include <bitset>
#include "g_std/g_string.h"
#include "stats.h"
#include "prefetch/prefetcher.h"
#include "g_std/g_vector.h"
#include "associative_array_impl.h"
#include "replacement_policies/lru_rp.h"
#include "indexing_policies/set_associative.h"
#include <boost/compute/detail/lru_cache.hpp>
#include "base/sat_counter.h"

struct IMPPrefetcherParams : public PrefetcherParams
{
    unsigned pt_table_entries;
    unsigned pt_table_assoc;
    unsigned num_stream_counter_bits;
    BaseIndexingPolicy* pt_table_indexing_policy;
    BaseReplPolicy* pt_table_replacement_policy;
    unsigned max_prefetch_distance;
    unsigned num_indirect_counter_bits;
    unsigned ipd_table_entries;
    unsigned ipd_table_assoc;
    BaseIndexingPolicy* ipd_table_indexing_policy;
    BaseReplPolicy* ipd_table_replacement_policy;
    g_vector<int> shift_values;
    unsigned addr_array_len;
    unsigned prefetch_threshold;
    unsigned stream_counter_threshold;
    unsigned streaming_distance;
};

class IMPPrefetcher : public Prefetcher
{
private:

    /** Maximum number of prefetches generated per event */
    const uint32_t maxPrefetchDistance;
    /** Shift values considered */
    const g_vector<int> shiftValues;
    /** Counter threshold to start prefetching */
    const uint32_t prefetchThreshold;
    /** streamCounter value to trigger the streaming prefetcher */
    const uint32_t streamCounterThreshold;
    /** Number of prefetches generated when using the streaming prefetcher */
    const uint32_t streamingDistance;

    /** Prefetch Table Entry */
    struct PrefetchTableEntry : public TaggedEntry
    {
        /* Stream table fields */

        /** Accessed address */
        Address address;

        int64_t stride;

        /** Confidence counter of the stream */
        SatCounter8 streamCounter;

        /* Indirect table fields */

        /** Enable bit of the indirect fields */
        bool enabled;
        /** Current index value */
        int64_t index;
        /** BaseAddr detected */
        Address baseAddr;
        /** Shift detected */
        int shift;
        /** Confidence counter of the indirect fields */
        SatCounter8 indirectCounter;
        /**
         * This variable is set to indicate that there has been at least one
         * match with the current index value. This information is later used
         * when a new index is updated. If there were no increases in the
         * indirectCounter, the counter is decremented.
         */
        bool increasedIndirectCounter;

        PrefetchTableEntry(unsigned indirect_counter_bits, unsigned stream_counter_bits)
            : TaggedEntry(), address(0), stride(0), streamCounter(stream_counter_bits),
              enabled(false), index(0), baseAddr(0), shift(0),
              indirectCounter(indirect_counter_bits),
              increasedIndirectCounter(false)
        {
        }

        void invalidate() override
        {
            TaggedEntry::invalidate();
            address = 0;
            stride = 0;
            streamCounter.reset();
            enabled = false;
            index = 0;
            baseAddr = 0;
            shift = 0;
            indirectCounter.reset();
            increasedIndirectCounter = false;
        }
    };

    /** Prefetch table */
    AssociativeArray<PrefetchTableEntry> prefetchTable;

    /** Indirect Pattern Detector entrt */
    struct IndirectPatternDetectorEntry : public TaggedEntry
    {
        /** address tag */
        Address tag;
        /** First index */
        int64_t idx1;
        /** Second index */
        int64_t idx2;
        /** Valid bit for the second index */
        bool secondIndexSet;
        /** Number of misses currently recorded */
        uint32_t numMisses;
        /**
         * Potential BaseAddr candidates for each recorded miss.
         * The number of candidates per miss is determined by the number of
         * elements in the shiftValues array.
         */
        g_vector<g_vector<Address>> baseAddr;

        IndirectPatternDetectorEntry(uint32_t num_addresses,
            uint32_t num_shifts)
            : idx1(0), idx2(0), secondIndexSet(false),
              numMisses(0),
              baseAddr(num_addresses, g_vector<Address>(num_shifts))
        {
        }

        void
        invalidate() override
        {
            TaggedEntry::invalidate();
            idx1 = 0;
            idx2 = 0;
            secondIndexSet = false;
            numMisses = 0;
        }
    };
    /** Indirect Pattern Detector (IPD) table */
    AssociativeArray<IndirectPatternDetectorEntry> ipd;

    IndirectPatternDetectorEntry *ipdEntryTrackingMisses;

    const unsigned filterSize{32};
    boost::compute::detail::lru_cache<Address, Address> blockLRUFilter;

    /**
     * Allocate or update an entry in the IPD
     * @param pt_entry Pointer to the associated page table entry
     * @param index Detected first index value
     */
    void allocateOrUpdateIPDEntry(const PrefetchTableEntry *pt_entry,
                                  int64_t index);
    /**
     * Update an IPD entry with a detected miss address, when the first index
     * is being tracked
     * @param miss_addr The address that caused the miss
     */
    void trackMissIndex1(Address miss_addr);

    /**
     * Update an IPD entry with a detected miss address, when the second index
     * is being tracked
     * @param miss_addr The address that caused the miss
     */
    void trackMissIndex2(Address miss_addr);

    /**
     * Checks if an access to the cache matches any active PT entry, if so,
     * the indirect confidence counter is incremented
     * @param addr address of the access
     */
    void checkAccessMatchOnActiveEntries(Address addr);

public:
    explicit IMPPrefetcher(const g_string &_name, const IMPPrefetcherParams &p);
    ~IMPPrefetcher() = default;

    void calculatePrefetch(const PrefetchInfo &pfi, uint64_t &respCycle, g_vector<AddrPriority> &addresses) override;

    static IMPPrefetcherParams buildParams(Config &config, const std::string &prefix) {
        IMPPrefetcherParams p;
        static_cast<PrefetcherParams&>(p) = Prefetcher::buildParams(config, prefix);

        p.pt_table_entries = config.get<uint32_t>(prefix + "pt_entries", 16);
        p.pt_table_assoc = config.get<uint32_t>(prefix + "pt_assoc", 16);
        p.num_stream_counter_bits = config.get<uint32_t>(prefix + "counter_bits", 3);

        p.pt_table_indexing_policy = new SetAssociative(p.pt_table_assoc, p.pt_table_entries, 1);
        p.pt_table_replacement_policy = new LRU();

        p.max_prefetch_distance = config.get<uint32_t>(prefix + "max_distance", 16);
        p.num_indirect_counter_bits = config.get<uint32_t>(prefix + "counter_bits", 3);

        p.ipd_table_entries = config.get<uint32_t>(prefix + "ipd_entries", 4);
        p.ipd_table_assoc = config.get<uint32_t>(prefix + "ipd_assoc", 4);
        p.ipd_table_indexing_policy = new SetAssociative(p.ipd_table_assoc, p.ipd_table_entries, 1);
        p.ipd_table_replacement_policy = new LRU();

        std::string shift_str = config.get<const char *>(prefix + "shift_values", "2,3,4,-3");
        std::stringstream ss(shift_str);
        std::string item;
        while (std::getline(ss, item, ',')) {
            p.shift_values.push_back(std::stoi(item));
        }

        p.addr_array_len = config.get<uint32_t>(prefix + "addr_array_len", 4);
        p.prefetch_threshold = config.get<uint32_t>(prefix + "prefetch_threshold", 2);
        p.stream_counter_threshold = config.get<uint32_t>(prefix + "stream_counter_threshold", 4);
        p.streaming_distance = config.get<uint32_t>(prefix + "streaming_distance", 4);

        return p;
    }
};

#endif // IMP_PREFETCHER_H_
