/**
 * Copyright (c) 2018 Metempsy Technology Consulting
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

#include "prefetch/stride_prefetcher.h"
#include "bithacks.h"

StridePrefetcher::StrideEntry::StrideEntry(const SatCounter8 &init_confidence)
    : TaggedEntry(), confidence(init_confidence)
{
    invalidate();
}

void StridePrefetcher::StrideEntry::invalidate()
{
    TaggedEntry::invalidate();
    lastAddr = 0;
    stride = 0;
    confidence.reset();
}

StridePrefetcher::StridePrefetcher(const g_string &_name, const StridePrefetcherParams &p)
    : Prefetcher(_name, p),
    initConfidence(p.confidence_counter_bits, p.initial_confidence),
    threshConf(p.confidence_threshold/100.0),
    useRequestorId(p.use_requestor_id),
    degree(p.degree),
    distance(p.distance),
    useCachelineAddr(p.use_cacheline_addr),
    pcTableInfo(p.table_assoc, p.table_entries, p.table_indexing_policy,
        p.table_replacement_policy),
    blockLRUFilter(filterSize)
{
}

StridePrefetcher::PCTable*
StridePrefetcher::findTable(uint32_t context)
{
    // Check if table for given context exists
    auto it = pcTables.find(context);
    if (it != pcTables.end())
        return &it->second;

    // If table does not exist yet, create one
    return allocateNewContext(context);
}

StridePrefetcher::PCTable*
StridePrefetcher::allocateNewContext(uint32_t context)
{
    // Create new table
    auto insertion_result = pcTables.insert(std::make_pair(context,
        PCTable(pcTableInfo.assoc, pcTableInfo.numEntries,
        pcTableInfo.indexingPolicy, pcTableInfo.replacementPolicy,
        StrideEntry(initConfidence))));

    // DBG("Adding context %i with stride entries\n", context);

    // Get iterator to new pc table, and then return a pointer to the new table
    return &(insertion_result.first->second);
}

StridePrefetcher::~StridePrefetcher()
{
    delete pcTableInfo.indexingPolicy;
    delete pcTableInfo.replacementPolicy;
}

void StridePrefetcher::calculatePrefetch(const PrefetchInfo &pfi, uint64_t &respCycle, g_vector<AddrPriority> &addresses)
{
    Address pc = pfi.getPC();

    if (pc == 0)
    {
        // DBG("StridePrefetcher: PC is 0, skipping prefetch");
        return;
    }

    Address pf_addr = useCachelineAddr ? blockAddress(pfi.getAddr()) : pfi.getAddr();

    // Ignoring recently prefetched address
    if (blockLRUFilter.contains(pf_addr))
    {
        // DBG("Ignoring recently prefetched address %#x.\n", pf_addr);
        return;
    }
    else
    {
        blockLRUFilter.insert(pf_addr, pf_addr);
    }

    RequestorID requestor_id = useRequestorId ? pfi.getRequestorId() : 0;

    // Get corresponding pc table
    PCTable* pcTable = findTable(requestor_id);

    // Search for entry in the pc table
    StrideEntry *entry = pcTable->findEntry(pc);

    if (entry != nullptr)
    {
        pcTable->accessEntry(entry);

        // Hit in table
        int new_stride = pf_addr - entry->lastAddr;

        // Do nothing on repeated memory access
        if (useCachelineAddr && new_stride == 0)
            return;

        bool stride_match = (new_stride == entry->stride);

        // Adjust confidence for stride entry
        if (stride_match)
        {
            entry->confidence++;
        }
        else
        {
            entry->confidence--;
            // If confidence has dropped below the threshold, train new stride
            if (entry->confidence.calcSaturation() < threshConf)
            {
                entry->stride = new_stride;
            }
        }

        // DBG( "Hit: PC %x req_addr %x stride %d (%s), "
        //         "conf %d\n", pc, pf_addr,
        //         new_stride, stride_match ? "match" : "change",
        //         (uint32_t)entry->confidence);


        entry->lastAddr = pf_addr;

        // Abort prefetch generation if below confidence threshold
        if (entry->confidence.calcSaturation() < threshConf)
        {
            return;
        }

        // Round strides up to at least 1 cacheline
        int prefetch_stride = entry->stride;
        if (abs(prefetch_stride) < blkSize) {
            prefetch_stride = (prefetch_stride < 0) ? -blkSize : blkSize;
        }

        Address new_addr = pf_addr + distance * prefetch_stride;
        // Generate up to degree prefetches
        for (int d = 1; d <= degree; d++) {
            new_addr += prefetch_stride;
            DBG("Generated prefetch %#lx, trigger address %#lx, pc %#lx", new_addr, pfi.getAddr(), pfi.getPC());
            addresses.push_back(AddrPriority(new_addr, 0));
        }
    }
    else
    {
        // Miss in table
        // DBG("Miss: PC %x req_addr %x\n", pc, pf_addr);
        StrideEntry *entry = pcTable->findVictim(pc);

        // Insert new entry's data
        entry->lastAddr = pf_addr;
        pcTable->insertEntry(pc, entry);
    }
}

uint32_t StridePrefetcherHashedSetAssociative::extractSet(const Address pc) const
{
    const Address hash1 = pc >> 1;
    const Address hash2 = hash1 >> tagShift;
    return (hash1 ^ hash2) & setMask;
}

Address StridePrefetcherHashedSetAssociative::extractTag(const Address addr) const
{
    return addr;
}