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

#ifndef AMPM_PREFETCHER_H_
#define AMPM_PREFETCHER_H_

#include "associative_array_impl.h"
#include "prefetch/prefetcher.h"
#include "timing_event.h"
#include "replacement_policies/lru_rp.h"
#include "indexing_policies/set_associative.h"

struct AccessMapPatternMatchingParams
{
    /** Cacheline size used by the prefetcher */
    unsigned block_size;
    /** Limit the stride checking to -limitStride/+limitStride */
    unsigned limit_stride;
    /** Maximum number of prefetch generated */
    unsigned start_degree;
    /** Amount of memory covered by a hot zone */
    uint64_t hot_zone_size;

    unsigned access_map_table_entries;
    unsigned access_map_table_assoc;
    BaseIndexingPolicy *access_map_table_indexing_policy;
    BaseReplPolicy *access_map_table_replacement_policy;

    /** A prefetch coverage factor bigger than this is considered high */
    double high_coverage_threshold;
    /** A prefetch coverage factor smaller than this is considered low */
    double low_coverage_threshold;
    /** A prefetch accuracy factor bigger than this is considered high */
    double high_accuracy_threshold;
    /** A prefetch accuracy factor smaller than this is considered low */
    double low_accuracy_threshold;
    /** A cache hit ratio bigger than this is considered high */
    double high_cache_hit_threshold;
    /** A cache hit ratio smaller than this is considered low */
    double low_cache_hit_threshold;
    /** Cycles in an epoch period */
    uint64_t epoch_cycles;
    /** Off chip memory latency to use for the epoch bandwidth calculation */
    Tick offchip_memory_latency;
};

class AccessMapPatternMatching : public GlobAlloc
{
    /** Cacheline size used by the prefetcher using this object */
    const unsigned blkSize;
    /** Limit the stride checking to -limitStride/+limitStride */
    const unsigned limitStride;
    /** Maximum number of prefetch generated */
    const unsigned startDegree;
    /** Amount of memory covered by a hot zone */
    const uint64_t hotZoneSize;
    /** A prefetch coverage factor bigger than this is considered high */
    const double highCoverageThreshold;
    /** A prefetch coverage factor smaller than this is considered low */
    const double lowCoverageThreshold;
    /** A prefetch accuracy factor bigger than this is considered high */
    const double highAccuracyThreshold;
    /** A prefetch accuracy factor smaller than this is considered low */
    const double lowAccuracyThreshold;
    /** A cache hit ratio bigger than this is considered high */
    const double highCacheHitThreshold;
    /** A cache hit ratio smaller than this is considered low */
    const double lowCacheHitThreshold;
    /** Cycles in an epoch period */
    const uint64_t epochCycles;
    /** Off chip memory latency to use for the epoch bandwidth calculation */
    const Tick offChipMemoryLatency;

    /** Data type representing the state of a cacheline in the access map */
    enum AccessMapState
    {
        AM_INIT,
        AM_PREFETCH,
        AM_ACCESS,
        AM_INVALID
    };

    /** AccessMapEntry data type */
    struct AccessMapEntry : public TaggedEntry
    {
        /** vector containing the state of the cachelines in this zone */
        g_vector<AccessMapState> states;

        AccessMapEntry(size_t num_entries)
            : TaggedEntry(), states(num_entries, AM_INIT)
        {
        }

        void
        invalidate() override
        {
            TaggedEntry::invalidate();
            for (auto &entry : states)
            {
                entry = AM_INIT;
            }
        }
    };
    /** Access map table */
    AssociativeArray<AccessMapEntry> accessMapTable;

    /**
     * Number of good prefetches
     * - State transitions from PREFETCH to ACCESS
     */
    uint64_t numGoodPrefetches;
    /**
     * Number of prefetches issued
     * - State transitions from INIT to PREFETCH
     */
    uint64_t numTotalPrefetches;
    /**
     * Number of raw cache misses
     * - State transitions from INIT or PREFETCH to ACCESS
     */
    uint64_t numRawCacheMisses;
    /**
     * Number of raw cache hits
     * - State transitions from ACCESS to ACCESS
     */
    uint64_t numRawCacheHits;
    /** Current degree */
    unsigned degree;
    /** Current useful degree */
    unsigned usefulDegree;

    /**
     * Given a target cacheline, this function checks if the cachelines
     * that follow the provided stride have been accessed. If so, the line
     * is considered a good candidate.
     * @param states vector containing the states of three contiguous hot zones
     * @param current target block (cacheline)
     * @param stride access stride to obtain the reference cachelines
     * @return true if current is a prefetch candidate
     */
    inline bool checkCandidate(g_vector<AccessMapState> const &states,
                               Address current, int stride) const
    {
        enum AccessMapState tgt = states[current - stride];
        enum AccessMapState s = states[current + stride];
        enum AccessMapState s2 = states[current + 2 * stride];
        enum AccessMapState s2_p1 = states[current + 2 * stride + 1];
        return (tgt != AM_INVALID &&
                ((s == AM_ACCESS && s2 == AM_ACCESS) ||
                 (s == AM_ACCESS && s2_p1 == AM_ACCESS)));
    }

    /**
     * Obtain an AccessMapEntry  from the AccessMapTable, if the entry is not
     * found a new one is initialized and inserted.
     * @param am_addr address of the hot zone
     * @return the corresponding entry
     */
    AccessMapEntry *getAccessMapEntry(Address am_addr);

    /**
     * Updates the state of a block within an AccessMapEntry, also updates
     * the prefetcher metrics.
     * @param entry AccessMapEntry to update
     * @param block cacheline within the hot zone
     * @param state new state
     */
    void setEntryState(AccessMapEntry &entry, Address block,
                       enum AccessMapState state);

    /**
     * This event constitues the epoch of the statistics that keep track of
     * the prefetcher accuracy, when this event triggers, the prefetcher degree
     * is adjusted and the statistics counters are reset.
     */
    void processEpochEvent();

public:
    AccessMapPatternMatching(const AccessMapPatternMatchingParams &p);
    ~AccessMapPatternMatching() = default;

    uint64_t tick(uint64_t startCycle);
    void calculatePrefetch(const PrefetchInfo &pfi,
                           g_vector<AddrPriority> &addresses);
    static AccessMapPatternMatchingParams buildParams(Config &config, const std::string &prefix)
    {
        AccessMapPatternMatchingParams p;
        p.block_size = config.get<uint32_t>(prefix + "block_size", 64);
        p.limit_stride = config.get<uint32_t>(prefix + "limit_stride", 0);
        p.start_degree = config.get<uint32_t>(prefix + "start_degree", 4);
        p.hot_zone_size = config.get<uint64_t>(prefix + "hot_zone_size", 2048);
        p.access_map_table_entries = config.get<uint32_t>(prefix + "table_entries", 256);
        p.access_map_table_assoc = config.get<uint32_t>(prefix + "table_assoc", 8);
        p.high_coverage_threshold = config.get<double>(prefix + "high_coverage", 0.25);
        p.low_coverage_threshold = config.get<double>(prefix + "low_coverage", 0.125);
        p.high_accuracy_threshold = config.get<double>(prefix + "high_accuracy", 0.5);
        p.low_accuracy_threshold = config.get<double>(prefix + "low_accuracy", 0.25);
        p.high_cache_hit_threshold = config.get<double>(prefix + "high_hit_ratio", 0.875);
        p.low_cache_hit_threshold = config.get<double>(prefix + "low_hit_ratio", 0.75);
        p.epoch_cycles = config.get<uint64_t>(prefix + "epoch_cycles", 256000);
        p.offchip_memory_latency = config.get<Tick>(prefix + "offchip_latency", 100);

        p.access_map_table_indexing_policy =
            new SetAssociative(p.access_map_table_assoc, p.access_map_table_entries, 1);
        p.access_map_table_replacement_policy = new LRU();

        return p;
    }
};

struct AMPMPrefetcherParams : public PrefetcherParams
{
    AccessMapPatternMatching *ampm;
};

class AMPMPrefetcher : public Prefetcher
{
    AccessMapPatternMatching &ampm;
    uint32_t domain;

public:
    AMPMPrefetcher(const g_string &_name, const AMPMPrefetcherParams &p, uint32_t _domain);
    ~AMPMPrefetcher() = default;
    void calculatePrefetch(const PrefetchInfo &pfi, uint64_t &respCycle, g_vector<AddrPriority> &addresses) override;

    static AMPMPrefetcherParams buildParams(Config &config, const std::string &prefix)
    {
        AMPMPrefetcherParams p;
        static_cast<PrefetcherParams&>(p) = Prefetcher::buildParams(config, prefix);
        p.ampm = new AccessMapPatternMatching(AccessMapPatternMatching::buildParams(config, prefix));
        return p;
    }
};

#endif /* AMPM_PREFETCHER_H_ */ 