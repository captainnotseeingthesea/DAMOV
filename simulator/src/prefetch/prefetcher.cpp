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

#include "prefetch/prefetcher.h"
#include "zsim.h"
#include "base/intmath.h"

PrefetchInfo::PrefetchInfo(MemReq req, bool miss)
    : address(req.accessInfo.addr), paddress(req.lineAddr), pc(req.accessInfo.pc), size(req.accessInfo.size),
      requestorId(req.srcId), childId(req.childId),
      write(req.type == GETX), cacheMiss(miss), reqCycle(req.cycle) {}

PrefetchInfo::PrefetchInfo(PrefetchInfo const &pfi, Address addr)
    : address(addr), paddress(pfi.paddress), pc(pfi.pc), size(pfi.size),
      requestorId(pfi.requestorId), childId(pfi.childId),
      write(pfi.write), cacheMiss(pfi.cacheMiss), reqCycle(pfi.reqCycle) {}

PrefetchInfo::PrefetchInfo(const Address &addr, const Address &paddr, const Address &pc, uint64_t reqCycle,
                           uint32_t reqSize, uint32_t reqId, uint32_t cId,
                           bool isWrite, bool isCacheMiss)
    : address(addr), paddress(paddr), pc(pc), reqCycle(reqCycle),
      size(reqSize), requestorId(reqId), childId(cId),
      write(isWrite), cacheMiss(isCacheMiss)
{
}

Prefetcher::Prefetcher(const g_string &_name, const PrefetcherParams &p)
    : name(_name), blkSize(p.blkSize),
      lBlkSize(floorLog2(blkSize)), pageSize(p.pageSize),
      lPageSize(floorLog2(pageSize)),
      requestorId(0), issuedPrefetches(0), usefulPrefetches(0),
      onMiss(p.on_miss), onRead(p.on_read),
      onWrite(p.on_write), onData(p.on_data), onInst(p.on_inst),
      prefetchOnAccess(p.prefetch_on_access), prefetchOnPfHit(p.prefetch_on_pf_hit) {}

bool Prefetcher::inCache(Address addr) const
{
    return cache->inCache(addr);
}

bool Prefetcher::hasBeenPrefetched(Address addr) const
{
    return cache->hasBeenPrefetched(addr);
}

bool Prefetcher::hasEverBeenPrefetched(Address addr) const
{
    return cache->hasEverBeenPrefetched(addr);
}

bool Prefetcher::samePage(Address a, Address b) const
{
    return roundDown(a, pageSize) == roundDown(b, pageSize);
}

Address Prefetcher::pageIndex(Address a) const
{
    return a >> lPageSize;
}

Address Prefetcher::blockAddress(Address a) const
{
    return a & ~((Address)blkSize - 1);
}

Address Prefetcher::blockIndex(Address a) const
{
    return a >> lBlkSize;
}

Address Prefetcher::pageAddress(Address a) const
{
    return roundDown(a, pageSize);
}

Address Prefetcher::pageOffset(Address a) const
{
    return a & (pageSize - 1);
}

Address Prefetcher::pageIthBlockAddress(Address page, uint32_t blockIndex) const
{
    return page + (blockIndex << lBlkSize);
}

bool Prefetcher::observeAccess(const Address addr, const uint32_t flags, const AccessType type, bool miss) const
{
    bool fetch = flags & MemReq::IFETCH;
    bool read = type == GETS;

    if (!miss)
    {
        if (prefetchOnPfHit)
            return hasEverBeenPrefetched(addr);
        if (!prefetchOnAccess)
            return false;
    }
    if (fetch && !onInst)
        return false;
    if (!fetch && !onData)
        return false;
    if (!fetch && read && !onRead)
        return false;
    if (!fetch && !read && !onWrite)
        return false;

    if (onMiss)
    {
        return miss;
    }

    return true;
}