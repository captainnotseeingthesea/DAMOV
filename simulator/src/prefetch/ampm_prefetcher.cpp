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

#include "bithacks.h"
#include "event_recorder.h"
#include "ampm_prefetcher.h"
#include "timing_event.h"
#include "tick_event.h"
#include "stats.h"

AccessMapPatternMatching::AccessMapPatternMatching(
    const AccessMapPatternMatchingParams &p)
    : blkSize(p.block_size), limitStride(p.limit_stride),
      startDegree(p.start_degree), hotZoneSize(p.hot_zone_size),
      highCoverageThreshold(p.high_coverage_threshold),
      lowCoverageThreshold(p.low_coverage_threshold),
      highAccuracyThreshold(p.high_accuracy_threshold),
      lowAccuracyThreshold(p.low_accuracy_threshold),
      highCacheHitThreshold(p.high_cache_hit_threshold),
      lowCacheHitThreshold(p.low_cache_hit_threshold),
      epochCycles(p.epoch_cycles),
      offChipMemoryLatency(p.offchip_memory_latency),
      accessMapTable(p.access_map_table_assoc, p.access_map_table_entries,
                     p.access_map_table_indexing_policy,
                     p.access_map_table_replacement_policy,
                     AccessMapEntry(hotZoneSize / blkSize)),
      numGoodPrefetches(0), numTotalPrefetches(0), numRawCacheMisses(0),
      numRawCacheHits(0), degree(startDegree), usefulDegree(startDegree)
{
    assert_msg(isPowerOf2(hotZoneSize),
        "the hot zone size must be a power of 2");
}

AccessMapPatternMatching::AccessMapEntry *
AccessMapPatternMatching::getAccessMapEntry(Address am_addr)
{
    AccessMapEntry *am_entry = accessMapTable.findEntry(am_addr);
    if (am_entry != nullptr) {
        accessMapTable.accessEntry(am_entry);
    } else {
        am_entry = accessMapTable.findVictim(am_addr);
        assert(am_entry != nullptr);

        accessMapTable.insertEntry(am_addr, am_entry);
    }
    return am_entry;
}

void AccessMapPatternMatching::setEntryState(AccessMapEntry &entry,
    Address block, enum AccessMapState state)
{
    enum AccessMapState old = entry.states[block];
    entry.states[block] = state;

    //do not update stats when initializing
    if (state == AM_INIT) return;

    switch (old) {
        case AM_INIT:
            if (state == AM_PREFETCH) {
                numTotalPrefetches += 1;
            } else if (state == AM_ACCESS) {
                numRawCacheMisses += 1;
            }
            break;
        case AM_PREFETCH:
            if (state == AM_ACCESS) {
                numGoodPrefetches += 1;
                numRawCacheMisses += 1;
            }
            break;
        case AM_ACCESS:
            if (state == AM_ACCESS) {
                numRawCacheHits += 1;
            }
            break;
        default:
            panic("Impossible path\n");
            break;
    }
}

void AccessMapPatternMatching::processEpochEvent()
{
    double prefetch_accuracy =
        ((double) numGoodPrefetches) / ((double) numTotalPrefetches);
    double prefetch_coverage =
        ((double) numGoodPrefetches) / ((double) numRawCacheMisses);
    double cache_hit_ratio = ((double) numRawCacheHits) /
        ((double) (numRawCacheHits + numRawCacheMisses));
    double num_requests = (double) (numRawCacheMisses - numGoodPrefetches +
        numTotalPrefetches);
    double memory_bandwidth = num_requests * offChipMemoryLatency /
        epochCycles;

    if (prefetch_coverage > highCoverageThreshold &&
        (prefetch_accuracy > highAccuracyThreshold ||
        cache_hit_ratio < lowCacheHitThreshold)) {
        usefulDegree += 1;
    } else if ((prefetch_coverage < lowCoverageThreshold &&
               (prefetch_accuracy < lowAccuracyThreshold ||
                cache_hit_ratio > highCacheHitThreshold)) ||
               (prefetch_accuracy < lowAccuracyThreshold &&
                cache_hit_ratio > highCacheHitThreshold)) {
        usefulDegree -= 1;
    }
    degree = std::max(std::min((unsigned) memory_bandwidth, usefulDegree), (unsigned)1);
    // reset epoch stats
    numGoodPrefetches = 0.0;
    numTotalPrefetches = 0.0;
    numRawCacheMisses = 0.0;
    numRawCacheHits = 0.0;
    DBG("Epoch event triggered, prefetch accuracy: %f, coverage: %f, "
         "cache hit ratio: %f, memory bandwidth: %f, degree: %u\n",
         prefetch_accuracy, prefetch_coverage, cache_hit_ratio,
         memory_bandwidth, degree);
}

uint64_t AccessMapPatternMatching::tick(uint64_t startCycle)
{
    processEpochEvent();
    return epochCycles;
}

// AMPM Prefetcher : https://github.com/charlab/dpc2/blob/master/dpc2sim/example_prefetchers/ampm_lite_prefetcher.c
void AccessMapPatternMatching::calculatePrefetch(const PrefetchInfo &pfi, g_vector<AddrPriority> &addresses)
{
    assert(addresses.empty());

    Address am_addr = pfi.getAddr() / hotZoneSize;
    Address current_block = (pfi.getAddr() % hotZoneSize) / blkSize;
    uint64_t lines_per_zone = hotZoneSize / blkSize;

    // Get the entries of the curent block (am_addr), the previous, and the
    // following ones
    AccessMapEntry *am_entry_curr = getAccessMapEntry(am_addr);
    AccessMapEntry *am_entry_prev = (am_addr > 0) ?
        getAccessMapEntry(am_addr-1) : nullptr;
    AccessMapEntry *am_entry_next = (am_addr < (MaxAddr/hotZoneSize)) ?
        getAccessMapEntry(am_addr+1) : nullptr;
    assert(am_entry_curr != am_entry_prev);
    assert(am_entry_curr != am_entry_next);
    assert(am_entry_prev != am_entry_next);
    assert(am_entry_curr != nullptr);

    //Mark the current access as Accessed
    setEntryState(*am_entry_curr, current_block, AM_ACCESS);

    /**
     * Create a contiguous copy of the 3 entries states.
     * With this, we avoid doing boundaries checking in the loop that looks
     * for prefetch candidates, mark out of range positions with AM_INVALID
     */
    g_vector<AccessMapState> states(3 * lines_per_zone);
    for (unsigned idx = 0; idx < lines_per_zone; idx += 1) {
        states[idx] =
            am_entry_prev != nullptr ? am_entry_prev->states[idx] : AM_INVALID;
        states[idx + lines_per_zone] = am_entry_curr->states[idx];
        states[idx + 2 * lines_per_zone] =
            am_entry_next != nullptr ? am_entry_next->states[idx] : AM_INVALID;
    }

    /**
     * am_entry_prev->states => states[               0 ..   lines_per_zone-1]
     * am_entry_curr->states => states[  lines_per_zone .. 2*lines_per_zone-1]
     * am_entry_next->states => states[2*lines_per_zone .. 3*lines_per_zone-1]
     */

    // index of the current_block in the new vector
    Address states_current_block = current_block + lines_per_zone;
    // consider strides 1..lines_per_zone/2
    int max_stride = limitStride == 0 ? lines_per_zone / 2 : limitStride + 1;
    for (int stride = 1; stride < max_stride; stride += 1) {
        // Test accessed positive strides
        if (checkCandidate(states, states_current_block, stride)) {
            // candidate found, current_block - stride
            Address pf_addr;
            if (stride > current_block) {
                // The index (current_block - stride) falls in the range of
                // the previous zone (am_entry_prev), adjust the address
                // accordingly
                Address blk = states_current_block - stride;
                pf_addr = (am_addr - 1) * hotZoneSize + blk * blkSize;
                setEntryState(*am_entry_prev, blk, AM_PREFETCH);
            } else {
                // The index (current_block - stride) falls within
                // am_entry_curr
                Address blk = current_block - stride;
                pf_addr = am_addr * hotZoneSize + blk * blkSize;
                setEntryState(*am_entry_curr, blk, AM_PREFETCH);
            }
            addresses.push_back(AddrPriority(pf_addr, 0));
            if (addresses.size() == degree) {
                break;
            }
        }

        // Test accessed negative strides
        if (checkCandidate(states, states_current_block, -stride)) {
            // candidate found, current_block + stride
            Address pf_addr;
            if (current_block + stride >= lines_per_zone) {
                // The index (current_block + stride) falls in the range of
                // the next zone (am_entry_next), adjust the address
                // accordingly
                Address blk = (states_current_block + stride) % lines_per_zone;
                pf_addr = (am_addr + 1) * hotZoneSize + blk * blkSize;
                setEntryState(*am_entry_next, blk, AM_PREFETCH);
            } else {
                // The index (current_block + stride) falls within
                // am_entry_curr
                Address blk = current_block + stride;
                pf_addr = am_addr * hotZoneSize + blk * blkSize;
                setEntryState(*am_entry_curr, blk, AM_PREFETCH);
            }
            addresses.push_back(AddrPriority(pf_addr, 0));
            if (addresses.size() == degree) {
                break;
            }
        }
    }
}

AMPMPrefetcher::AMPMPrefetcher(const g_string &_name, const AMPMPrefetcherParams &p, uint32_t _domain) : Prefetcher(_name, p), ampm(*p.ampm), domain(_domain)
{
    TickEvent<AccessMapPatternMatching> *tickEv = new TickEvent<AccessMapPatternMatching>(p.ampm, 0);
    tickEv->queue(0); //start the sim at time 0
    DBG("AccessMapPatternMatching::tick() will be call cyclically");
}

void AMPMPrefetcher::calculatePrefetch(const PrefetchInfo &pfi, uint64_t &respCycle, g_vector<AddrPriority> &addresses)
{
    ampm.calculatePrefetch(pfi, addresses);
}