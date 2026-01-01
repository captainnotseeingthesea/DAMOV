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
 * Implementation of the Irregular Stream Buffer prefetcher
 * Reference:
 *   Jain, A., & Lin, C. (2013, December). Linearizing irregular memory
 *   accesses for improved correlated prefetching. In Proceedings of the
 *   46th Annual IEEE/ACM International Symposium on Microarchitecture
 *   (pp. 247-259). ACM.
 */

#ifndef ISB_PREFETCHER_H_
#define ISB_PREFETCHER_H_

#include "g_std/g_string.h"
#include "stats.h"
#include "prefetch/prefetcher.h"
#include "associative_array_impl.h"
#include "replacement_policies/lru_rp.h"
#include "indexing_policies/set_associative.h"
#include "base/sat_counter.h"
#include "g_std/g_vector.h"

struct ISBPrefetcherParams : public PrefetcherParams
{
    unsigned num_counter_bits;
    unsigned chunk_size;
    unsigned degree;
    unsigned training_unit_entries;
    unsigned training_unit_assoc;
    BaseIndexingPolicy *training_unit_indexing_policy;
    BaseReplPolicy *training_unit_replacement_policy;
    unsigned prefetch_candidates_per_entry;
    unsigned address_map_cache_entries;
    unsigned address_map_cache_assoc;
    BaseIndexingPolicy *ps_address_map_cache_indexing_policy;
    BaseReplPolicy *ps_address_map_cache_replacement_policy;
    BaseIndexingPolicy *sp_address_map_cache_indexing_policy;
    BaseReplPolicy *sp_address_map_cache_replacement_policy;
};

class ISBPrefetcher : public Prefetcher
{
    /** Size in bytes of a temporal stream */
    const size_t chunkSize;
    /** Number of prefetch candidates per Physical-to-Structural entry */
    const unsigned prefetchCandidatesPerEntry;
    /** Number of maximum prefetches requests created when predicting */
    const unsigned degree;

    /**
     * Training Unit Entry datatype, it holds the last accessed address
     */
    struct TrainingUnitEntry : public TaggedEntry
    {
        Address lastAddress;
    };
    /** Map of PCs to Training unit entries */
    AssociativeArray<TrainingUnitEntry> trainingUnit;

    /** Address Mapping entry, holds an address and a confidence counter */
    struct AddressMapping
    {
        Address address;
        SatCounter8 counter;
        AddressMapping(unsigned bits) : address(0), counter(bits)
        {
        }
    };

    /**
     * Maps a set of contiguous addresses to another set of (not necessarily
     * contiguos) addresses, with their corresponding confidence counters
     */
    struct AddressMappingEntry : public TaggedEntry
    {
        g_vector<AddressMapping> mappings;
        AddressMappingEntry(size_t num_mappings, unsigned counter_bits)
            : TaggedEntry(), mappings(num_mappings, counter_bits)
        {
        }

        void
        invalidate() override
        {
            TaggedEntry::invalidate();
            for (auto &entry : mappings)
            {
                entry.address = 0;
                entry.counter.reset();
            }
        }
    };

    /** Physical-to-Structured mappings table */
    AssociativeArray<AddressMappingEntry> psAddressMappingCache;
    /** Structured-to-Physical mappings table */
    AssociativeArray<AddressMappingEntry> spAddressMappingCache;
    /**
     * Counter of allocated structural addresses, increased by "chunkSize",
     * each time a new structured address is allocated
     */
    uint64_t structuralAddressCounter;

    /**
     * Add a mapping to the Structured-to-Physica mapping table
     * @param structuralAddress structural address
     * @param physical_address corresponding physical address
     */
    void addStructuralToPhysicalEntry(Address structuralAddress,
                                      Address physical_address);

    /**
     * Obtain the Physical-to-Structured mapping entry of the given physical
     * address. If the entry does not exist a new one is allocated, replacing
     * an existing one if needed.
     * @param paddr physical address
     * @result reference to the entry
     */
    AddressMapping &getPSMapping(Address paddr);

public:
    ISBPrefetcher(const g_string &_name, const ISBPrefetcherParams &p);
    ~ISBPrefetcher() = default;

    void calculatePrefetch(const PrefetchInfo &pfi, uint64_t &respCycle, g_vector<AddrPriority> &addresses) override;

    static ISBPrefetcherParams buildParams(Config &config, const std::string &prefix) {
        ISBPrefetcherParams p;
        static_cast<PrefetcherParams&>(p) = Prefetcher::buildParams(config, prefix);

        p.num_counter_bits = config.get<uint32_t>(prefix + "counter_bits", 2);
        p.chunk_size = config.get<uint32_t>(prefix + "chunk_size", 256);
        p.degree = config.get<uint32_t>(prefix + "degree", 4);
        p.training_unit_entries = config.get<uint32_t>(prefix + "training_entries", 128);
        p.training_unit_assoc = config.get<uint32_t>(prefix + "training_assoc", 128);
        p.training_unit_indexing_policy = new SetAssociative(p.training_unit_assoc, p.training_unit_entries, 1);
        p.training_unit_replacement_policy = new LRU();

        p.prefetch_candidates_per_entry = config.get<uint32_t>(prefix + "prefetch_candidates", 16);
        p.address_map_cache_entries = config.get<uint32_t>(prefix + "addr_map_entries", 128);
        p.address_map_cache_assoc = config.get<uint32_t>(prefix + "addr_map_assoc", 128);

        p.ps_address_map_cache_indexing_policy = new SetAssociative(p.address_map_cache_assoc, p.address_map_cache_entries, 1);
        p.ps_address_map_cache_replacement_policy = new LRU();
        p.sp_address_map_cache_indexing_policy = new SetAssociative(p.address_map_cache_assoc, p.address_map_cache_entries, 1);
        p.sp_address_map_cache_replacement_policy = new LRU();

        return p;
    }
};

#endif // ISB_PREFETCHER_H_