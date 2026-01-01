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

#ifndef FILTER_CACHE_H_
#define FILTER_CACHE_H_

#include "bithacks.h"
#include "cache.h"
#include "galloc.h"
#include "zsim.h"
#include "prefetch/prefetcher.h"
#include "event_recorder.h"
#include "timing_event.h"

/* Extends Cache with an L0 direct-mapped cache, optimized to hell for hits
 *
 * L1 lookups are dominated by several kinds of overhead (grab the cache locks,
 * several virtual functions for the replacement policy, etc.). This
 * specialization of Cache solves these issues by having a filter array that
 * holds the most recently used line in each set. Accesses check the filter array,
 * and then go through the normal access path. Because there is one line per set,
 * it is fine to do this without grabbing a lock.
 */

class FilterCache : public Cache {
    private:
        struct FilterEntry {
            volatile Address rdAddr;
            volatile Address wrAddr;
            volatile uint64_t availCycle;

            void clear() {wrAddr = 0; rdAddr = 0; availCycle = 0;}
        };

        //Replicates the most accessed line of each set in the cache
        FilterEntry* filterArray;
        Address setMask;
        uint32_t numSets;
        uint32_t srcId; //should match the core
        uint32_t reqFlags;

        lock_t filterLock;
        uint64_t fGETSHit, fGETXHit;
        Prefetcher *l1_prefetcher;

        // Collect stats of graph data access
        uint64_t fGraphGetSHit, fGraphGetXHit;

    public:
        FilterCache(uint32_t _numSets, uint32_t _numLines, CC* _cc, CacheArray* _array,
                    ReplPolicy* _rp, uint32_t _accLat, uint32_t _invLat, bool bypass, g_string& _name, Prefetcher* _prefetcher)
                : Cache(_numLines, _cc, _array, _rp, _accLat, _invLat, bypass, _name), l1_prefetcher(_prefetcher)
        {
            numSets = _numSets;
            setMask = numSets - 1;
            
            filterArray = gm_memalign<FilterEntry>(CACHE_LINE_BYTES, numSets);
            for (uint32_t i = 0; i < numSets; i++) filterArray[i].clear();
            futex_init(&filterLock);
            fGETSHit = fGETXHit = 0;
            fGraphGetSHit = fGraphGetXHit = 0;
            srcId = -1;
            reqFlags = 0;
        }

        void setSourceId(uint32_t id) {
            srcId = id;
        }

        void setFlags(uint32_t flags) {
            reqFlags = flags;
        }

        void initStats(AggregateStat* parentStat) {
            AggregateStat* cacheStat = new AggregateStat();
            cacheStat->init(name.c_str(), "Filter cache stats");

            ProxyStat* fgetsStat = new ProxyStat();
            fgetsStat->init("fhGETS", "Filtered GETS hits", &fGETSHit);
            ProxyStat* fgetxStat = new ProxyStat();
            fgetxStat->init("fhGETX", "Filtered GETX hits", &fGETXHit);

            ProxyStat* fgraphsStat = new ProxyStat();
            fgraphsStat->init("fhGraphHit", "Filtered Graph GETX hits", &fGraphGetSHit);
            ProxyStat* fgraphxStat = new ProxyStat();
            fgraphxStat->init("fhGrapxHit", "Filtered Graph GETX hits", &fGraphGetXHit);
            cacheStat->append(fgetsStat);
            cacheStat->append(fgetxStat);
            cacheStat->append(fgraphsStat);
            cacheStat->append(fgraphxStat);

            initCacheStats(cacheStat);
            parentStat->append(cacheStat);
        }

        inline void prefetch(AccessInfo info, uint64_t curCycle, uint64_t respCycle, bool miss)
        {
            Address vAddr = info.addr;
            Address pc = info.pc;
            uint32_t size = info.size;

            Address pLineAddr = procMask | (vAddr >> lineBits);
            EventRecorder *evRec = zinfo->eventRecorders[srcId];
            // Get prefetch addresses
            if (l1_prefetcher && l1_prefetcher->observeAccess(pLineAddr, reqFlags, GETS, miss)) {
                TimingRecord demand_tr;

                g_vector<TimingRecord> trGroups;
                uint64_t reqCycle = curCycle;
                PrefetchInfo pfi(vAddr, pLineAddr, pc, reqCycle, size, srcId, 0, false, miss);
                g_vector<AddrPriority> addresses;
                l1_prefetcher->calculatePrefetch(pfi, respCycle, addresses);

                demand_tr.clear();
                if(evRec && evRec->hasRecord())
                {
                    demand_tr = evRec->popRecord();
                }
                for(AddrPriority& addr_prio : addresses)
                {
                    Address vLineAddr = addr_prio.first >> lineBits;
                    uint32_t idx = vLineAddr & setMask;
                    if(vLineAddr == filterArray[idx].rdAddr)
                    {
                        continue;
                    }
                    else
                    {
                        AccessInfo prefetchInfo = {addr_prio.first, pc, size, info.dataType, info.accessType};
                        uint64_t pfRespCycle = replace(vLineAddr, prefetchInfo, idx, reqCycle++, MemReq::PREFETCH);
                    }
                    if(evRec && evRec->hasRecord())
                    {
                        trGroups.push_back(evRec->popRecord());
                    }
                }
                if(!trGroups.empty())
                {
                    DelayEvent* startEv = new (evRec) DelayEvent(0);
                    startEv->setMinStartCycle(curCycle);
                    TimingRecord tr = {pLineAddr, curCycle, respCycle, GETS, startEv, startEv};
                    if(demand_tr.isValid())
                    {
                        startEv->addChild(demand_tr.startEvent, evRec);
                        tr.endEvent = demand_tr.endEvent;
                    }
                    for(auto &ptr : trGroups)
                    {
                        tr.startEvent->addChild(ptr.startEvent, evRec);
                    }
                    evRec->pushRecord(tr);
                }
                else if(demand_tr.isValid())
                {
                    evRec->pushRecord(demand_tr);
                }
            }
        }

        inline uint64_t load(AccessInfo &info, uint64_t curCycle){
            Address vLineAddr = info.addr >> lineBits;
            uint32_t idx = vLineAddr & setMask;
            uint64_t availCycle = filterArray[idx].availCycle; //read before, careful with ordering to avoid timing races
            uint64_t respCycle;
            bool miss = true;
            if (vLineAddr == filterArray[idx].rdAddr) {
                fGETSHit++;
                respCycle = MAX(curCycle, availCycle);
                miss = false;
                fGraphGetSHit += info.isGraphData();
            } else {
                if(inCache(procMask | vLineAddr))
                {
                    miss = false;
                }
                respCycle = replace(vLineAddr, info, idx, curCycle);
            } 
            prefetch(info, curCycle, respCycle, miss);
            return respCycle;
        }

        inline uint64_t store(AccessInfo &info, uint64_t curCycle) {
            Address vLineAddr = info.addr >> lineBits;
            uint32_t idx = vLineAddr & setMask;
            uint64_t availCycle = filterArray[idx].availCycle; //read before, careful with ordering to avoid timing races
            if (vLineAddr == filterArray[idx].wrAddr) {
                fGETXHit++;
                //NOTE: Stores don't modify availCycle; we'll catch matches in the core
                //filterArray[idx].availCycle = curCycle; //do optimistic store-load forwardi
                fGraphGetXHit += (info.dataType != AccessInfo::DATA && info.dataType != AccessInfo::INS) ? 1 : 0;
                return MAX(curCycle, availCycle);
            } else {
                return replace(vLineAddr, info, idx, curCycle);
            }
        }

        uint64_t replace(Address vLineAddr, AccessInfo &info, uint32_t idx, uint64_t curCycle, uint32_t extraFlag = 0) {
            Address pLineAddr = procMask | vLineAddr;
            bool isLoad = info.accessType == AccessInfo::LOAD;
            MESIState dummyState = MESIState::I;
            Address vAddr = info.addr;
            Address pc = info.pc;
            uint32_t size = info.size;
            futex_lock(&filterLock);
            MemReq req = {pLineAddr, info, isLoad? GETS : GETX, 0, &dummyState, curCycle, &filterLock, dummyState, srcId, reqFlags | extraFlag};

            bool miss = !inCache(pLineAddr);
            uint64_t respCycle = access(req);

            //Due to the way we do the locking, at this point the old address might be invalidated, but we have the new address guaranteed until we release the lock

            //Careful with this order
            Address oldAddr = filterArray[idx].rdAddr;
            filterArray[idx].wrAddr = isLoad? -1L : vLineAddr;
            filterArray[idx].rdAddr = vLineAddr;

            //For LSU simulation purposes, loads bypass stores even to the same line if there is no conflict,
            //(e.g., st to x, ld from x+8) and we implement store-load forwarding at the core.
            //So if this is a load, it always sets availCycle; if it is a store hit, it doesn't
            if (oldAddr != vLineAddr) filterArray[idx].availCycle = respCycle;

            futex_unlock(&filterLock);
            if(l1_prefetcher && miss)
            {
                l1_prefetcher->notifyFill(req, respCycle);
            }
            return respCycle;
        }

        uint64_t invalidate(const InvReq& req) {
            Cache::startInvalidate();  // grabs cache's downLock
            futex_lock(&filterLock);
            uint32_t idx = req.lineAddr & setMask; //works because of how virtual<->physical is done...
            if ((filterArray[idx].rdAddr | procMask) == req.lineAddr) { //FIXME: If another process calls invalidate(), procMask will not match even though we may be doing a capacity-induced invalidation!
                filterArray[idx].wrAddr = -1L;
                filterArray[idx].rdAddr = -1L;
            }
            uint64_t respCycle = Cache::finishInvalidate(req); // releases cache's downLock
            futex_unlock(&filterLock);
            return respCycle;
        }

        void contextSwitch() {
            futex_lock(&filterLock);
            for (uint32_t i = 0; i < numSets; i++) filterArray[i].clear();
            futex_unlock(&filterLock);
        }
};

#endif  // FILTER_CACHE_H_
