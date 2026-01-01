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
#include "imp_prefetcher.h"
#include "timing_event.h"
#include "zsim.h"

IMPPrefetcher::IMPPrefetcher(const g_string &_name, const IMPPrefetcherParams &p)
    : Prefetcher(_name, p),
      maxPrefetchDistance(p.max_prefetch_distance),
      shiftValues(p.shift_values), prefetchThreshold(p.prefetch_threshold),
      streamCounterThreshold(p.stream_counter_threshold),
      streamingDistance(p.streaming_distance),
      prefetchTable(p.pt_table_assoc, p.pt_table_entries,
                    p.pt_table_indexing_policy, p.pt_table_replacement_policy,
                    PrefetchTableEntry(p.num_indirect_counter_bits, p.num_indirect_counter_bits)),
      ipd(p.ipd_table_assoc, p.ipd_table_entries, p.ipd_table_indexing_policy,
          p.ipd_table_replacement_policy,
          IndirectPatternDetectorEntry(p.addr_array_len, shiftValues.size())),
      ipdEntryTrackingMisses(nullptr),
      blockLRUFilter(filterSize)
{
}

void IMPPrefetcher::calculatePrefetch(const PrefetchInfo &pfi, uint64_t &respCycle, g_vector<AddrPriority> &addresses)
{
    // This prefetcher requires a PC
    Address pc = pfi.getPC();
    if (pc == 0)
    {
        return;
    }

    Address addr = pfi.getAddr();
    bool miss = pfi.isCacheMiss();

    checkAccessMatchOnActiveEntries(addr);

    // First check if this is a miss, if the prefetcher is tracking misses
    if (ipdEntryTrackingMisses != nullptr && miss)
    {
        // Check if the entry tracking misses has already set its second index
        if (!ipdEntryTrackingMisses->secondIndexSet)
        {
            trackMissIndex1(addr);
        }
        else
        {
            trackMissIndex2(addr);
        }
    }
    else
    {
        // if misses are not being tracked, attempt to detect stream accesses
        PrefetchTableEntry *pt_entry = prefetchTable.findEntry(pc);
        if (pt_entry != nullptr)
        {
            prefetchTable.accessEntry(pt_entry);

            if (pt_entry->address != addr) {
                int64_t stride = addr - pt_entry->address;
                pt_entry->address = addr;
                if (pt_entry->stride == stride) {
                    pt_entry->streamCounter++;
                    if (pt_entry->streamCounter.rawCounter() >= streamCounterThreshold) {    
                        int prefetch_stride = pt_entry->stride;
                        if (abs(prefetch_stride) < blkSize) {
                            prefetch_stride = (prefetch_stride < 0) ? -blkSize : blkSize;
                        }
                        Address pfAddr = addr + prefetch_stride;
                        addresses.push_back(AddrPriority(pfAddr, 0));
                        DBG("Generate Stream Prefetch: %lx", pfAddr);
                    }
                } 
                else {
                    pt_entry->streamCounter--;
                    if (pt_entry->streamCounter.rawCounter() < streamCounterThreshold) { 
                        pt_entry->stride = stride;
                    }
                }
                // if this is a read, read the data from the cache and assume
                // it is an index (this is only possible if the data is already
                // in the cache), also, only indexes up to 8 bytes are
                // considered
                if (!miss && !pfi.isWrite() && pfi.getSize() <= 8)
                {
                    int64_t index = 0;
                    bool read_index = true;
                    switch (pfi.getSize())
                    {
                    case sizeof(uint8_t):
                        index = pfi.get<uint8_t>();
                        break;
                    case sizeof(uint16_t):
                        index = pfi.get<uint16_t>();
                        break;
                    case sizeof(uint32_t):
                        index = pfi.get<uint32_t>();
                        break;
                    case sizeof(uint64_t):
                        index = pfi.get<uint64_t>();
                        break;
                    default:
                        // Ignore non-power-of-two sizes
                        read_index = false;
                    }
                    if (read_index && !pt_entry->enabled)
                    {
                        // Not enabled (no pattern detected in this stream),
                        // add or update an entry in the pattern detector and
                        // start tracking misses
                        allocateOrUpdateIPDEntry(pt_entry, index);
                    }
                    else if (read_index)
                    {
                        // Enabled entry, update the index
                        pt_entry->index = index;
                        if (!pt_entry->increasedIndirectCounter)
                        {
                            pt_entry->indirectCounter--;
                        }
                        else
                        {
                            // Set this to false, to see if the new index
                            // has any match
                            pt_entry->increasedIndirectCounter = false;
                        }

                        // If the counter is high enough, start prefetching
                        if (pt_entry->indirectCounter > prefetchThreshold)
                        {
                            unsigned distance = maxPrefetchDistance *
                                                pt_entry->indirectCounter.calcSaturation();
                            Address pf_addr = pt_entry->baseAddr +
                                                (pt_entry->index << pt_entry->shift);
                            addresses.push_back(AddrPriority(pf_addr, 0));
                            DBG("Generate indirect Prefetch: %lx", pf_addr);
                        }
                    }
                }  
            }
        }
        else
        {
            pt_entry = prefetchTable.findVictim(pc);
            assert(pt_entry != nullptr);
            prefetchTable.insertEntry(pc, pt_entry);
            pt_entry->address = addr;
        }
    }
}

void IMPPrefetcher::allocateOrUpdateIPDEntry(const PrefetchTableEntry *pt_entry,
                                             int64_t index)
{
    // The address of the pt_entry is used to index the IPD
    Address ipd_entry_addr = (Address)pt_entry;
    IndirectPatternDetectorEntry *ipd_entry = ipd.findEntry(ipd_entry_addr);
    if (ipd_entry != nullptr)
    {
        ipd.accessEntry(ipd_entry);
        if (!ipd_entry->secondIndexSet)
        {
            // Second time we see an index, fill idx2
            ipd_entry->idx2 = index;
            ipd_entry->secondIndexSet = true;
            ipdEntryTrackingMisses = ipd_entry;
        }
        else
        {
            // Third access! no pattern has been found so far,
            // release the IPD entry
            ipd.invalidate(ipd_entry);
            ipdEntryTrackingMisses = nullptr;
        }
    }
    else
    {
        ipd_entry = ipd.findVictim(ipd_entry_addr);
        assert(ipd_entry != nullptr);
        ipd.insertEntry(ipd_entry_addr, ipd_entry);
        ipd_entry->idx1 = index;
        ipdEntryTrackingMisses = ipd_entry;
    }
}

void IMPPrefetcher::trackMissIndex1(Address miss_addr)
{
    IndirectPatternDetectorEntry *entry = ipdEntryTrackingMisses;
    // If the second index is not set, we are just filling the baseAddr
    // vector
    assert(entry->numMisses < entry->baseAddr.size());
    g_vector<Address> &ba_array = entry->baseAddr[entry->numMisses];
    int idx = 0;
    for (int shift : shiftValues)
    {
        ba_array[idx] = miss_addr - (entry->idx1 << shift);
        idx += 1;
    }
    entry->numMisses += 1;
    if (entry->numMisses == entry->baseAddr.size())
    {
        // stop tracking misses once we have tracked enough
        ipdEntryTrackingMisses = nullptr;
    }
}

void IMPPrefetcher::trackMissIndex2(Address miss_addr)
{
    IndirectPatternDetectorEntry *entry = ipdEntryTrackingMisses;
    // Second index is filled, compare the addresses generated during
    // the previous misses (using idx1) against newly generated values
    // using idx2, if a match is found, fill the additional fields
    // of the PT entry
    for (int midx = 0; midx < entry->numMisses; midx += 1)
    {
        g_vector<Address> &ba_array = entry->baseAddr[midx];
        int idx = 0;
        for (int shift : shiftValues)
        {
            if (ba_array[idx] == (miss_addr - (entry->idx2 << shift)))
            {
                // Match found!
                // Fill the corresponding pt_entry
                PrefetchTableEntry *pt_entry = (PrefetchTableEntry *)entry->getTag();
                pt_entry->baseAddr = ba_array[idx];
                pt_entry->shift = shift;
                pt_entry->enabled = true;
                pt_entry->indirectCounter.reset();
                // Release the current IPD Entry
                ipd.invalidate(entry);
                // Do not track more misses
                ipdEntryTrackingMisses = nullptr;
                return;
            }
            idx += 1;
        }
    }
}

void IMPPrefetcher::checkAccessMatchOnActiveEntries(Address addr)
{
    for (auto &pt_entry : prefetchTable)
    {
        if (pt_entry.enabled)
        {
            if (addr == pt_entry.baseAddr +
                            (pt_entry.index << pt_entry.shift))
            {
                pt_entry.indirectCounter++;
                pt_entry.increasedIndirectCounter = true;
            }
        }
    }
}