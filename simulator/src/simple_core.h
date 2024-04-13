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

#ifndef SIMPLE_CORE_H_
#define SIMPLE_CORE_H_

//A simple core model with IPC=1 except on memory accesses

#include "core.h"
#include "memory_hierarchy.h"
#include "pad.h"

class FilterCache;
class GraphPrefetcher;

class SimpleCore : public Core {
    protected:
        FilterCache* l1i;
        FilterCache* l1d;
        GraphPrefetcher *graphPrefetcher;

        uint64_t instrs;
        uint64_t curCycle;
        uint64_t phaseEndCycle; //next stopping point
        uint64_t haltedCycles;
	    	uint64_t offload_instrs = 0; 
        bool offload_region = false;

    public:
        SimpleCore(FilterCache* _l1i, FilterCache* _l1d, GraphPrefetcher* _graphPrefetcher, g_string& _name);
        void offloadFunction_begin() {offload_region = true; }
        void offloadFunction_end() {offload_region = false; }
        int get_offload_code() { return 0; }


        uint64_t getInstrs() const {return instrs;}
        uint64_t getOffloadInstrs() const {return offload_instrs;}
        uint64_t getPhaseCycles() const;
        uint64_t getCycles() const {return curCycle - haltedCycles;}

        void initStats(AggregateStat* parentStat);
        void contextSwitch(int32_t gid);

        virtual void join();
        InstrFuncPtrs GetFuncPtrs();

    protected:
        //Simulation functions
        inline void load(Address addr, uint32_t size);
        inline void store(Address addr, uint32_t size);
        inline void bbl(Address bblAddr, BblInfo* bblInstrs);

        inline void prefetcherLoadSrc(SrcInfo src) {graphPrefetcher->pushSrcInfo(src);}
        inline void prefetcherLoadDest(DestInfo dest) {graphPrefetcher->pushDestInfo(dest);}
        
        static void OffloadBegin(THREADID tid);
        static void OffloadEnd(THREADID tid);
        // Prefetcher functions
        static void PrefetcherLoadSrcFunc(THREADID tid, SrcInfo src);
        static void PrefetcherLoadDestFunc(THREADID tid, DestInfo dest);

        static void LoadFunc(THREADID tid, ADDRINT addr, UINT32 size);
        static void StoreFunc(THREADID tid, ADDRINT addr, UINT32 size);
        static void BblFunc(THREADID tid, ADDRINT bblAddr, BblInfo* bblInfo);
        static void PredLoadFunc(THREADID tid, ADDRINT addr, BOOL pred, UINT32 size);
        static void PredStoreFunc(THREADID tid, ADDRINT addr, BOOL pred, UINT32 size);

        static void BranchFunc(THREADID, ADDRINT, BOOL, ADDRINT, ADDRINT) {}
}  ATTR_LINE_ALIGNED; //This needs to take up a whole cache line, or false sharing will be extremely frequent

#endif  // SIMPLE_CORE_H_
