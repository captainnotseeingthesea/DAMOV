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

#include "ooo_core.h"
#include <algorithm>
#include <queue>
#include <string>
#include "bithacks.h"
#include "decoder.h"
#include "filter_cache.h"
#include "zsim.h"

/* Uncomment to induce backpressure to the IW when the load/store buffers fill up. In theory, more detailed,
 * but sometimes much slower (as it relies on range poisoning in the IW, potentially O(n^2)), and in practice
 * makes a negligible difference (ROB backpressures).
 */
//#define LSU_IW_BACKPRESSURE

#define DEBUG_MSG(args...)
//#define DEBUG_MSG(args...) info(args)

// Core parameters
// TODO(dsm): Make OOOCore templated, subsuming these

// Stages --- more or less matched to Westmere, but have not seen detailed pipe diagrams anywhare
#define FETCH_STAGE 1
#define DECODE_STAGE 4  // NOTE: Decoder adds predecode delays to decode
#define ISSUE_STAGE 7
#define DISPATCH_STAGE 13  // RAT + ROB + RS, each is easily 2 cycles

#define L1D_LAT 4  // fixed, and FilterCache does not include L1 delay
#define FETCH_BYTES_PER_CYCLE 16
#define ISSUES_PER_CYCLE 4
#define RF_READS_PER_CYCLE 3

//top-down
// uint64_t lastCommitCycleLoad = 0;
// uint64_t lastCommitCycleLoad_prev = 0;

// uint64_t lastCommitCycleStore_prev = 0;
// uint64_t lastCommitCycleStore = 0;
// uint64_t lastCommitCycleOther = 0;


OOOCore::OOOCore(FilterCache* _l1i, FilterCache* _l1d, GraphPrefetcher* _graphPrefetcher, g_string& _name) : Core(_name, _graphPrefetcher), l1i(_l1i), l1d(_l1d), cRec(0, _name) {
    decodeCycle = DECODE_STAGE;  // allow subtracting from it
    curCycle = 0;
    phaseEndCycle = zinfo->phaseLength;

    for (uint32_t i = 0; i < MAX_REGISTERS; i++) {
        regScoreboard[i] = 0;
    }
    prevBbl = nullptr;

    lastStoreCommitCycle = 0;
    lastStoreAddrCommitCycle = 0;
    curCycleRFReads = 0;
    curCycleIssuedUops = 0;
    branchPc = 0;

    instrs = uops = branchUops = fpAddSubUops = fpMulDivUops = bbls = approxInstrs = mispredBranches = predBranches = 0;
    mispredPenalty = opExecuted = 0, loadStallsTotal = 0, storeStallsTotal = 0;
    graphLoadStallsTotal = allSallsTotal = 0;
    lastLoadStallCycle = lastStoreStallCycle = lastGraphLoadStalCycle = lastStallCycle = 0;
    offsetLoad = edgeLoad = weightLoad = propertyLoad = propertyStore = graphDataLoadLatency = 0;

    for (uint32_t i = 0; i < FWD_ENTRIES; i++) fwdArray[i].set((Address)(-1L), 0);

}

void OOOCore::initStats(AggregateStat* parentStat) {
    AggregateStat* coreStat = new AggregateStat();
    coreStat->init(name.c_str(), "Core stats");

    auto x = [this]() { return cRec.getUnhaltedCycles(curCycle); };
    LambdaStat<decltype(x)>* cyclesStat = new LambdaStat<decltype(x)>(x);
    cyclesStat->init("cycles", "Simulated unhalted cycles");

    auto y = [this]() { return cRec.getContentionCycles(); };
    LambdaStat<decltype(y)>* cCyclesStat = new LambdaStat<decltype(y)>(y);
    cCyclesStat->init("cCycles", "Cycles due to contention stalls");

    ProxyStat* instrsStat = new ProxyStat();
    instrsStat->init("instrs", "Simulated instructions", &instrs);
    ProxyStat* uopsStat = new ProxyStat();
    uopsStat->init("uops", "Retired micro-ops", &uops);
    ProxyStat* branchUopsStat = new ProxyStat();
    branchUopsStat->init("branchUops", "Retired branch micro-ops", &branchUops);
    ProxyStat* fpAddSubUopsStat = new ProxyStat();
    fpAddSubUopsStat->init("fpAddSubUops", "Retired floating point add and sub micro-ops", &fpAddSubUops);
    ProxyStat* fpMulDivUopsStat = new ProxyStat();
    fpMulDivUopsStat->init("fpMulDivUops", "Retired floating point mul and div micro-ops", &fpMulDivUops);
    ProxyStat* bblsStat = new ProxyStat();
    bblsStat->init("bbls", "Basic blocks", &bbls);
    ProxyStat* approxInstrsStat = new ProxyStat();
    approxInstrsStat->init("approxInstrs", "Instrs with approx uop decoding", &approxInstrs);
    ProxyStat* mispredBranchesStat = new ProxyStat();
    mispredBranchesStat->init("mispredBranches", "Mispredicted branches", &mispredBranches);
    // xuanyi
    ProxyStat* predBranchesStat = new ProxyStat();
    predBranchesStat->init("predBranches", "Predicted branches", &predBranches);

    ProxyStat* mispredInstrsStat = new ProxyStat();
    mispredInstrsStat->init("mispredInstrs", "Instructions executed in wrong path", &mispredInstrs); // top-down
    ProxyStat* mispredPenaltyStat = new ProxyStat();
    mispredPenaltyStat->init("mispredPenalty", "Cycles delayed due to branch misprediction", &mispredPenalty); // top-down
    ProxyStat* opExecutedStat = new ProxyStat();
    opExecutedStat->init("opExecuted", "cycles with high (more than 3) number of operations executed", &opExecuted); // top-down
    ProxyStat* loadStallsTotalStat = new ProxyStat();
    loadStallsTotalStat->init("loadStallsTotal", "stalls due to load", &loadStallsTotal); // top-down
    ProxyStat* storeStallsTotalStat = new ProxyStat();
    storeStallsTotalStat->init("storeStallsTotal", "stalls due to store", &storeStallsTotal); // top-down
    ProxyStat* graphLoadStallsTotalStat = new ProxyStat();
    graphLoadStallsTotalStat->init("graphLoadStallsTotal", "stalls due to graph load", &graphLoadStallsTotal); // top-down
    ProxyStat* allStallsTotalStat = new ProxyStat();
    allStallsTotalStat->init("allSallsTotal", "total stalls", &allSallsTotal); // top-down

    ProxyStat* graphDataLoadLatencyStat = new ProxyStat();
    graphDataLoadLatencyStat->init("graphDataLoadLatency", "Latency due to graph load", &graphDataLoadLatency);

    coreStat->append(cyclesStat);
    coreStat->append(cCyclesStat);
    coreStat->append(instrsStat);
    coreStat->append(uopsStat);
    coreStat->append(branchUopsStat);
    coreStat->append(fpAddSubUopsStat);
    coreStat->append(fpMulDivUopsStat);
    coreStat->append(bblsStat);
    coreStat->append(approxInstrsStat);
    coreStat->append(mispredBranchesStat);
    coreStat->append(predBranchesStat);
    coreStat->append(mispredInstrsStat);
    coreStat->append(mispredPenaltyStat);
    coreStat->append(opExecutedStat);
    coreStat->append(allStallsTotalStat);
    coreStat->append(loadStallsTotalStat);
    coreStat->append(storeStallsTotalStat);
    coreStat->append(graphLoadStallsTotalStat);
    coreStat->append(graphDataLoadLatencyStat);


#ifdef OOO_STALL_STATS
    profFetchStalls.init("fetchStalls",  "Fetch stalls");  coreStat->append(&profFetchStalls);
    profDecodeStalls.init("decodeStalls", "Decode stalls"); coreStat->append(&profDecodeStalls);
    profIssueStalls.init("issueStalls",  "Issue stalls");  coreStat->append(&profIssueStalls);
#endif

    spatial_l.init("spatialLocality", "Spatial Locality times 10000");
    coreStat->append(&spatial_l);
    temporal_l.init("temporalLocality", "Temporal Locality times 10000");
    coreStat->append(&temporal_l);

    ProxyStat* totalLoadLatencyStat = new ProxyStat();
    totalLoadLatencyStat->init("totalLoadLatency", "latency induced by loads", &totalLoadLatency);
    ProxyStat* totalStoreLatencyStat = new ProxyStat();
    totalStoreLatencyStat->init("totalStoreLatency", "latency induced by stores", &totalStoreLatency);
    ProxyStat* totalLoadStat = new ProxyStat();
    totalLoadStat->init("totalLoad", "num of total loads", &totalLoad);
    ProxyStat* totalStoreStat = new ProxyStat();
    totalStoreStat->init("totalStore", "num of total stores", &totalStore);
    auto loadFunc = [this](){return totalLoad > 0 ? totalLoadLatency / totalLoad : 0;};
    LambdaStat<decltype(loadFunc)>* averageLoadLatencyStat = new LambdaStat<decltype(loadFunc)>(loadFunc);
    averageLoadLatencyStat->init("averageLoadLatency", "average load latency");
    auto storeFunc = [this](){return totalStore > 0 ? totalStoreLatency / totalStore : 0;};
    LambdaStat<decltype(storeFunc)>* averageStoreLatencyStat = new LambdaStat<decltype(storeFunc)>(storeFunc);
    averageStoreLatencyStat->init("averageStoreLatency", "average store latency");

    auto graphLoadFunc = [this](){
        uint64_t totalGraphLoad = offsetLoad + edgeLoad + weightLoad + propertyLoad;
        return totalGraphLoad > 0 ? graphDataLoadLatency / totalGraphLoad : 0;};
    LambdaStat<decltype(graphLoadFunc)>* averageGraphLoadLatencyStat = new LambdaStat<decltype(graphLoadFunc)>(graphLoadFunc);
    averageGraphLoadLatencyStat->init("averageGraphLoadLatency", "average graph load latency");

    // graph data access monitor
    ProxyStat* offsetLoadStat = new ProxyStat();
    offsetLoadStat->init("offsetLoad", "num of graph offset loads", &offsetLoad);
    ProxyStat* edgeLoadStat = new ProxyStat();
    edgeLoadStat->init("edgeLoad", "num of graph edge loads", &edgeLoad);
    ProxyStat* weightLoadStat = new ProxyStat();
    weightLoadStat->init("weightLoad", "num of graph weight loads", &weightLoad);
    ProxyStat* propertyLoadStat = new ProxyStat();
    propertyLoadStat->init("propertyLoad", "num of graph property loads", &propertyLoad);
    ProxyStat* propertyStoreStat = new ProxyStat();
    propertyStoreStat->init("propertyStore", "num of graph property stores", &propertyStore);

    coreStat->append(totalLoadLatencyStat);
    coreStat->append(totalStoreLatencyStat);
    coreStat->append(totalLoadStat);
    coreStat->append(totalStoreStat);
    coreStat->append(averageLoadLatencyStat);
    coreStat->append(averageStoreLatencyStat);
    coreStat->append(averageGraphLoadLatencyStat);
    coreStat->append(offsetLoadStat);
    coreStat->append(edgeLoadStat);
    coreStat->append(weightLoadStat);
    coreStat->append(propertyLoadStat);
    coreStat->append(propertyStoreStat);

    parentStat->append(coreStat);
}

uint64_t OOOCore::getOffloadInstrs() const {return offload_instrs;}
uint64_t OOOCore::getInstrs() const {return instrs;}
uint64_t OOOCore::getPhaseCycles() const {return curCycle % zinfo->phaseLength;}

void OOOCore::contextSwitch(int32_t gid) {
    if (gid == -1) {
        // Do not execute previous BBL, as we were context-switched
        prevBbl = nullptr;

        // Invalidate virtually-addressed filter caches
        l1i->contextSwitch();
        l1d->contextSwitch();
    }
}

InstrFuncPtrs OOOCore::GetFuncPtrs() {
    return {LoadFunc, StoreFunc, BblFunc, BranchFunc, PredLoadFunc, PredStoreFunc, OffloadBegin, OffloadEnd, PrefetcherLoadSrcFunc, PrefetcherLoadDestFunc, FPTR_ANALYSIS, {0} };
}

void OOOCore::OffloadBegin(THREADID tid) {
    static_cast<OOOCore*>(cores[tid])->offloadFunction_begin();
}
void OOOCore::OffloadEnd(THREADID tid) {
    static_cast<OOOCore*>(cores[tid])->offloadFunction_end();
}

void OOOCore::PrefetcherLoadSrcFunc(THREADID tid, SrcInfo src) {
    static_cast<OOOCore*>(cores[tid])->prefetcherLoadSrc(src);
}

void OOOCore::PrefetcherLoadDestFunc(THREADID tid, DestInfo dest) {
    static_cast<OOOCore*>(cores[tid])->prefetcherLoadDest(dest);
}

inline void OOOCore::load(Address addr, uint32_t size, Address pc) {
    AccessInfo::DataType dataType = AccessInfo::DATA;

    // check whether the data is graph data
    if(zinfo->configGraph){
        if((void *)addr >= zinfo->graphRegion.offsetStart && (void *)addr < zinfo->graphRegion.offsetEnd)
        {
            dataType = AccessInfo::OFFSET;
            ++offsetLoad;
        }
        else if((void *)addr >= zinfo->graphRegion.edgeStart && (void *)addr < zinfo->graphRegion.edgeEnd)
        {
            dataType = AccessInfo::EDGE;
            ++edgeLoad;
        }
        else if((void *)addr >= zinfo->graphRegion.weightStart && (void *)addr < zinfo->graphRegion.weightEnd)
        {
            dataType = AccessInfo::WEIGHT;
            ++weightLoad;
        }
        else if((void *)addr >= zinfo->graphRegion.propertyStart && (void *)addr < zinfo->graphRegion.propertyEnd)
        {
            dataType = AccessInfo::PROPERTY;
            ++propertyLoad;
        }
    }
    if(graphPrefetcherEnabled && inGraphPrefetcherAddr((void *)addr))
    {
        Address offset = ((Address)addr - (Address)zinfo->graphPrefetcherAddr) / GRAPH_PREFETCHER_ELE_SIZE;
        edgeLoad += (offset == DEST_NODE_INDEX);
        weightLoad += (offset == WEIGHT_VALUE_INDEX);
        propertyLoad += (offset == SRC_PROPERTY_INDEX || offset == DEST_PROPERTY_INDEX);
    }
    uint32_t index = loads + stores;
    accesses[index] = {addr, pc, size, dataType, AccessInfo::LOAD};
    loads++;
}

void OOOCore::store(Address addr, uint32_t size, Address pc) {
    AccessInfo::DataType dataType = AccessInfo::DATA;
    // check whether the data is graph data
    if(zinfo->configGraph){
        if((void *)addr >= zinfo->graphRegion.propertyStart && (void *)addr < zinfo->graphRegion.propertyEnd)
        {
            ++propertyStore;
            uint64_t v = (addr - (Address)zinfo->graphRegion.propertyStart) / 8;
            zinfo->affected_vertex[v] = 1;
            dataType = AccessInfo::PROPERTY;
        }
    }
    uint32_t index = loads + stores;
    accesses[index] = {addr, pc, size, dataType, AccessInfo::STORE};
    stores++;
}

// Predicated loads and stores call this function, gets recorded as a 0-cycle op.
// Predication is rare enough that we don't need to model it perfectly to be accurate (i.e. the uops still execute, retire, etc), but this is needed for correctness.
void OOOCore::predFalseMemOp() {
    // I'm going to go out on a limb and assume just loads are predicated (this will not fail silently if it's a store)
    uint32_t index = loads + stores;
    accesses[index] = {-1L, 0, 0, AccessInfo::DATA, AccessInfo::LOAD};
    loads++;
}

void OOOCore::branch(Address pc, bool taken, Address takenNpc, Address notTakenNpc) {
    branchPc = pc;
    branchTaken = taken;
    branchTakenNpc = takenNpc;
    branchNotTakenNpc = notTakenNpc;
}

inline void OOOCore::bbl(Address bblAddr, BblInfo* bblInfo) {
    if (!prevBbl) {
        // This is the 1st BBL since scheduled, nothing to simulate
        prevBbl = bblInfo;
        // Kill lingering ops from previous BBL
        loads = stores = 0;
        return;
    }

    /* Simulate execution of previous BBL */
    uint32_t bblInstrs = prevBbl->instrs;
    DynBbl* bbl = &(prevBbl->oooBbl[0]);
    prevBbl = bblInfo;

    uint32_t loadIdx = 0;
    uint32_t storeIdx = 0;

    uint32_t prevDecCycle = 0;
    uint64_t lastCommitCycle = 0;  // used to find misprediction penalty

    // Run dispatch/IW
    for (uint32_t i = 0; i < bbl->uops; i++) {
        DynUop* uop = &(bbl->uop[i]);

        // Decode stalls
        uint32_t decDiff = uop->decCycle - prevDecCycle;
        decodeCycle = MAX(decodeCycle + decDiff, uopQueue.minAllocCycle());
        if (decodeCycle > curCycle) {
            //info("Decode stall %ld %ld | %d %d", decodeCycle, curCycle, uop->decCycle, prevDecCycle);
            uint32_t cdDiff = decodeCycle - curCycle;
#ifdef OOO_STALL_STATS
            profDecodeStalls.inc(cdDiff);
#endif
            curCycleIssuedUops = 0;
            curCycleRFReads = 0;
            for (uint32_t i = 0; i < cdDiff; i++) insWindow.advancePos(curCycle);
        }
        prevDecCycle = uop->decCycle;
        uopQueue.markLeave(curCycle);

        // Implement issue width limit --- we can only issue 4 uops/cycle
        if (curCycleIssuedUops >= ISSUES_PER_CYCLE) {
#ifdef OOO_STALL_STATS
            profIssueStalls.inc();
#endif
            // info("Advancing due to uop issue width");
            curCycleIssuedUops = 0;
            curCycleRFReads = 0;
            insWindow.advancePos(curCycle);
        }
        curCycleIssuedUops++;

        // Kill dependences on invalid register
        // Using curCycle saves us two unpredictable branches in the RF read stalls code
        regScoreboard[0] = curCycle;

        uint64_t c0 = regScoreboard[uop->rs[0]];
        uint64_t c1 = regScoreboard[uop->rs[1]];

        // RF read stalls
        // if srcs are not available at issue time, we have to go thru the RF
        curCycleRFReads += ((c0 < curCycle)? 1 : 0) + ((c1 < curCycle)? 1 : 0);
        if (curCycleRFReads > RF_READS_PER_CYCLE) {
            curCycleRFReads -= RF_READS_PER_CYCLE;
            curCycleIssuedUops = 0;  // or 1? that's probably a 2nd-order detail
            insWindow.advancePos(curCycle);
        }

        ROBEntry robEntry = rob.minAllocCycle();
        uint64_t c2 = robEntry.cycle;
        uint64_t c3 = curCycle;

        uint64_t cOps = MAX(c0, c1);

        // Model RAT + ROB + RS delay between issue and dispatch
        uint64_t dispatchCycle = MAX(cOps, MAX(c2, c3) + (DISPATCH_STAGE - ISSUE_STAGE));
        //checking for top-down
        //top-down memory stalls

        // Collect stall cycles induced by ROB full
        uint64_t stallStartCycle = max(c3, lastStallCycle);
        if(stallStartCycle < c2)
        {
            if(stallStartCycle < c2)
            {
                allSallsTotal += c2 - stallStartCycle;
                lastStallCycle = c2;
            }
        }

        if(robEntry.type == ROBEntryType::GRAPHLOAD || robEntry.type == ROBEntryType::LOAD)
        {
            stallStartCycle = max(c3, lastLoadStallCycle);
            if(stallStartCycle < c2)
            {
                loadStallsTotal += c2 - stallStartCycle;
                lastLoadStallCycle = c2;
            }
            if(robEntry.type == ROBEntryType::GRAPHLOAD)
            {
                stallStartCycle = max(c3, lastGraphLoadStalCycle);
                if(stallStartCycle < c2)
                {
                    graphLoadStallsTotal += c2 - stallStartCycle;
                    lastGraphLoadStalCycle = c2;
                }
            }
        }
        else if(robEntry.type == ROBEntryType::GRAPHSTORE || robEntry.type == ROBEntryType::STORE)
        {
            stallStartCycle = max(c3, lastStoreStallCycle);
            if(stallStartCycle < c2)
            {
                storeStallsTotal += c2 - stallStartCycle;
                lastStoreStallCycle = c2;
            }
        }

        // uint64_t load_stall_diff;
        // uint64_t store_stall_diff;

        //  if((dispatchCycle > lastCommitCycleOther) & (dispatchCycle > lastStoreCommitCycle) & (lastStoreCommitCycle > lastCommitCycleOther)){
        //     if(lastStoreCommitCycle > lastCommitCycleStore_prev){
        //     	store_stall_diff = lastStoreCommitCycle - lastCommitCycleOther;
        //     	storeStallsTotal += store_stall_diff;
        //     }
        //     lastCommitCycleStore_prev = lastStoreCommitCycle;
        // }

        // if((dispatchCycle > lastCommitCycleOther) & (dispatchCycle > lastCommitCycleLoad) & (lastCommitCycleLoad > lastCommitCycleOther)){
        //     if(lastCommitCycleLoad > lastCommitCycleLoad_prev){
        //     	load_stall_diff = lastCommitCycleLoad - lastCommitCycleOther;
        //     	loadStallsTotal += load_stall_diff;
        //     }
        //     lastCommitCycleLoad_prev = lastCommitCycleLoad;
        // }

        // info("IW 0x%lx %d %ld %ld %x", bblAddr, i, c2, dispatchCycle, uop->portMask);
        // NOTE: Schedule can adjust both cur and dispatch cycles
        insWindow.schedule(curCycle, dispatchCycle, uop->portMask, uop->extraSlots);

        // If we have advanced, we need to reset the curCycle counters
        if (curCycle > c3) {
            curCycleIssuedUops = 0;
            curCycleRFReads = 0;
        }

        uint64_t commitCycle;
        bool graphPrefetcherAccess = false;
        // LSU simulation
        // NOTE: Ever-so-slightly faster than if-else if-else if-else
	switch (uop->type) {
            case UOP_GENERAL:
                {
                     commitCycle = dispatchCycle + uop->lat;
                     //top-down
                    //  if((uop->type != UOP_LOAD) && (uop->type != UOP_STORE)) lastCommitCycleOther = commitCycle;
                    rob.markRetire(commitCycle, ROBEntryType::OTHER);
                }
                break;

            case UOP_LOAD:
                {
                    // dispatchCycle = MAX(loadQueue.minAllocCycle(), dispatchCycle);
                    ++totalLoad;
                    uint64_t lqCycle = loadQueue.minAllocCycle().cycle;
                    if (lqCycle > dispatchCycle) {
#ifdef LSU_IW_BACKPRESSURE
                        insWindow.poisonRange(curCycle, lqCycle, 0x4 /*PORT_2, loads*/);
#endif
                        dispatchCycle = lqCycle;
                    }

                    // Wait for all previous store addresses to be resolved
                    dispatchCycle = MAX(lastStoreAddrCommitCycle+1, dispatchCycle);

                    uint32_t index = loadIdx + storeIdx;
                    Address addr = accesses[index].addr;
                    uint32_t size = accesses[index].size;
                    loadIdx++;

                    uint64_t reqSatisfiedCycle = dispatchCycle;
                    if(addr != -1L)
                    {
                        if(graphPrefetcherEnabled && inGraphPrefetcherAddr((void *)addr))
                        {
                            Address offset = ((Address)addr - (Address)zinfo->graphPrefetcherAddr) / GRAPH_PREFETCHER_ELE_SIZE;
                            reqSatisfiedCycle = graphPrefetcher->load(offset, dispatchCycle);
                            uint64_t latency = (reqSatisfiedCycle - dispatchCycle);
                            graphDataLoadLatency += latency;
                            graphPrefetcherAccess = true;
                        }
                        else
                        {
                            reqSatisfiedCycle = l1d->load(accesses[index], dispatchCycle) + L1D_LAT;
                        }
                    }
                    bool isGraphData = accesses[index].isGraphData();
                    cRec.record(curCycle, dispatchCycle, reqSatisfiedCycle);
                    if(zinfo->numCores == 1){
                        locality_monitor.push_address(addr,size);
                    }

                    // Enforce st-ld forwarding
                    uint32_t fwdIdx = (addr>>2) & (FWD_ENTRIES-1);
                    if (fwdArray[fwdIdx].addr == addr) {
                        // info("0x%lx FWD %ld %ld", addr, reqSatisfiedCycle, fwdArray[fwdIdx].storeCycle);
                        /* Take the MAX (see FilterCache's code) Our fwdArray
                         * imposes more stringent timing constraints than the
                         * l1d, b/c FilterCache does not change the line's
                         * availCycle on a store. This allows FilterCache to
                         * track per-line, not per-word availCycles.
                         */
                        reqSatisfiedCycle = MAX(reqSatisfiedCycle, fwdArray[fwdIdx].storeCycle);
                    }

                    commitCycle = reqSatisfiedCycle;
                    // lastCommitCycleLoad = commitCycle;
                    loadQueue.markRetire(commitCycle);
                    uint32_t loadLatency = commitCycle - dispatchCycle;
                    totalLoadLatency += loadLatency;
                    rob.markRetire(commitCycle, (isGraphData || graphPrefetcherAccess) ? ROBEntryType::GRAPHLOAD : ROBEntryType::LOAD);
                    if(isGraphData)
                    {
                        graphDataLoadLatency += loadLatency;
                        // info("reqCycle: %u, respCycle: %u, latency: %u", dispatchCycle, commitCycle, loadLatency);
                    }
                }
                break;

            case UOP_STORE:
                {
                    // dispatchCycle = MAX(storeQueue.minAllocCycle(), dispatchCycle);
                    ++totalStore;
                    uint64_t sqCycle = storeQueue.minAllocCycle().cycle;
                    if (sqCycle > dispatchCycle) {
#ifdef LSU_IW_BACKPRESSURE
                        insWindow.poisonRange(curCycle, sqCycle, 0x10 /*PORT_4, stores*/);
#endif
                        dispatchCycle = sqCycle;
                    }

                    // Wait for all previous store addresses to be resolved (not just ours :))
                    dispatchCycle = MAX(lastStoreAddrCommitCycle+1, dispatchCycle);

                    uint32_t index = loadIdx + storeIdx;
                    Address addr = accesses[index].addr;
                    uint32_t size = accesses[index].size;
                    storeIdx++;

                    if(zinfo->numCores == 1){
                        locality_monitor.push_address(addr, size);
                    }
                    uint64_t reqSatisfiedCycle = dispatchCycle;
                    if(graphPrefetcherEnabled && inGraphPrefetcherAddr((void *)addr))
                    {
                        Address offset = ((Address)addr - (Address)zinfo->graphPrefetcherAddr) / GRAPH_PREFETCHER_ELE_SIZE;
                        reqSatisfiedCycle = graphPrefetcher->store(offset, dispatchCycle);
                    }
                    else
                    {
                        reqSatisfiedCycle = l1d->store(accesses[index], dispatchCycle) + L1D_LAT;
                    }
                    bool isGraphData = accesses[index].isGraphData();
                    cRec.record(curCycle, dispatchCycle, reqSatisfiedCycle);

                    // Fill the forwarding table
                    fwdArray[(addr>>2) & (FWD_ENTRIES-1)].set(addr, reqSatisfiedCycle);

                    commitCycle = reqSatisfiedCycle;
                    lastStoreCommitCycle = MAX(lastStoreCommitCycle, reqSatisfiedCycle);
                    storeQueue.markRetire(commitCycle);
                    totalStoreLatency += commitCycle - dispatchCycle;
                    rob.markRetire(commitCycle, isGraphData ? ROBEntryType::GRAPHSTORE : ROBEntryType::STORE);
                }
                break;

            case UOP_STORE_ADDR:
                commitCycle = dispatchCycle + uop->lat;
                lastStoreAddrCommitCycle = MAX(lastStoreAddrCommitCycle, commitCycle);
                rob.markRetire(commitCycle, ROBEntryType::STOREADDR);
                break;

            //case UOP_FENCE:  //make gcc happy
            default:
                assert((UopType) uop->type == UOP_FENCE);
                commitCycle = dispatchCycle + uop->lat;
                // info("%d %ld %ld", uop->lat, lastStoreAddrCommitCycle, lastStoreCommitCycle);
                // force future load serialization
                lastStoreAddrCommitCycle = MAX(commitCycle, MAX(lastStoreAddrCommitCycle, lastStoreCommitCycle + uop->lat));
                rob.markRetire(commitCycle, ROBEntryType::OTHER);

                // info("%d %ld %ld X", uop->lat, lastStoreAddrCommitCycle, lastStoreCommitCycle);
        }

        // Record dependences
        regScoreboard[uop->rd[0]] = commitCycle;
        regScoreboard[uop->rd[1]] = commitCycle;

        lastCommitCycle = commitCycle;
    }

    instrs += bblInstrs;

    if(offload_region){
        offload_instrs += bblInstrs;
    }

    uops += bbl->uops;
    bbls++;
    approxInstrs += bbl->approxInstrs;
    branchUops += bbl->branchUops;
    fpAddSubUops += bbl->fpAddSubUops;
    fpMulDivUops += bbl->fpMulDivUops;

#ifdef BBL_PROFILING
    if (approxInstrs) Decoder::profileBbl(bbl->bblIdx);
#endif

    // Check full match between expected and actual mem ops
    // If these assertions fail, most likely, something's off in the decoder
    assert_msg(loadIdx == loads, "%s: loadIdx(%d) != loads (%d)", name.c_str(), loadIdx, loads);
    assert_msg(storeIdx == stores, "%s: storeIdx(%d) != stores (%d)", name.c_str(), storeIdx, stores);
    loads = stores = 0;

    /* Simulate frontend for branch pred + fetch of this BBL
     *
     * NOTE: We assume that the instruction length predecoder and the IQ are
     * weak enough that they can't hide any ifetch or bpred stalls. In fact,
     * predecoder stalls are incorporated in the decode stall component (see
     * decoder.cpp). So here, we compute fetchCycle, then use it to adjust
     * decodeCycle.
     */

    // Model fetch-decode delay (fixed, weak predec/IQ assumption)
    uint64_t fetchCycle = decodeCycle - (DECODE_STAGE - FETCH_STAGE);
    uint32_t lineSize = 1 << lineBits;

    // Simulate branch prediction
    if (branchPc && !branchPred.predict(branchPc, branchTaken)) {
        mispredBranches++;

        /* Simulate wrong-path fetches
         *
         * This is not for a latency reason, but sometimes it increases fetched
         * code footprint and L1I MPKI significantly. Also, we assume a perfect
         * BTB here: we always have the right address to missfetch on, and we
         * never need resteering.
         *
         * NOTE: Resteering due to BTB misses is done at the BAC unit, is
         * relatively rare, and carries an 8-cycle penalty, which should be
         * partially hidden if the branch is predicted correctly --- so we
         * don't simulate it.
         *
         * Since we don't have a BTB, we just assume the next branch is not
         * taken. With a typical branch mispred penalty of 17 cycles, we
         * typically fetch 3-4 lines in advance (16B/cycle). This sets a higher
         * limit, which can happen with branches that take a long time to
         * resolve (because e.g., they depend on a load). To set this upper
         * bound, assume a completely backpressured IQ (18 instrs), uop queue
         * (28 uops), IW (36 uops), and 16B instr length predecoder buffer. At
         * ~3.5 bytes/instr, 1.2 uops/instr, this is about 5 64-byte lines.
         */

        // info("Mispredicted branch, %ld %ld %ld | %ld %ld", decodeCycle, curCycle, lastCommitCycle,
        //         lastCommitCycle-decodeCycle, lastCommitCycle-curCycle);
        Address wrongPathAddr = branchTaken? branchNotTakenNpc : branchTakenNpc;
        uint64_t reqCycle = fetchCycle;
       //top-down
        uint64_t reqCycleBefore = reqCycle;
        for (uint32_t i = 0; i < 5*64/lineSize; i++) {
            mispredInstrs++; // top-down
            AccessInfo info = {wrongPathAddr + lineSize*i, 0, lineSize, AccessInfo::INS, AccessInfo::LOAD};
            uint64_t fetchLat = l1i->load(info, curCycle) - curCycle;
            cRec.record(curCycle, curCycle, curCycle + fetchLat);
            uint64_t respCycle = reqCycle + fetchLat;
            if (respCycle > lastCommitCycle) {
                break;
            }
            // Model fetch throughput limit
            reqCycle = respCycle + lineSize/FETCH_BYTES_PER_CYCLE;

        }

        fetchCycle = lastCommitCycle;

        //top-down
        uint64_t reqCycleAfter = reqCycle;
        mispredPenalty += (reqCycleAfter - reqCycleBefore);


    }
    else if(branchPc)
    {
        predBranches++; // xuanyi
    }
    branchPc = 0;  // clear for next BBL

    // Simulate current bbl ifetch
    Address endAddr = bblAddr + bblInfo->bytes;
    for (Address fetchAddr = bblAddr; fetchAddr < endAddr; fetchAddr += lineSize) {
        // The Nehalem frontend fetches instructions in 16-byte-wide accesses.
        // Do not model fetch throughput limit here, decoder-generated stalls already include it
        // We always call fetches with curCycle to avoid upsetting the weave
        // models (but we could move to a fetch-centric recorder to avoid this)
        AccessInfo info = {fetchAddr, 0, lineSize, AccessInfo::INS, AccessInfo::LOAD};
        uint64_t fetchLat = l1i->load(info, curCycle) - curCycle;
        cRec.record(curCycle, curCycle, curCycle + fetchLat);
        fetchCycle += fetchLat;
    }

    // If fetch rules, take into account delay between fetch and decode;
    // If decode rules, different BBLs make the decoders skip a cycle
    decodeCycle++;
    uint64_t minFetchDecCycle = fetchCycle + (DECODE_STAGE - FETCH_STAGE);
    if (minFetchDecCycle > decodeCycle) {
#ifdef OOO_STALL_STATS
        profFetchStalls.inc(decodeCycle - minFetchDecCycle);
#endif
        decodeCycle = minFetchDecCycle;
    }
}

void OOOCore::finish(){

      locality_monitor.calculate_locality();
      //spatial locality
      spatial_l.set(locality_monitor.get_spatial_locality()*10000);
      //
      //temporal locality
      temporal_l.set(locality_monitor.get_temporal_locality()*10000);
      //
}

// Timing simulation code
void OOOCore::join() {
    DEBUG_MSG("[%s] Joining, curCycle %ld phaseEnd %ld", name.c_str(), curCycle, phaseEndCycle);
    uint64_t targetCycle = cRec.notifyJoin(curCycle);
    if (targetCycle > curCycle) advance(targetCycle);
    phaseEndCycle = zinfo->globPhaseCycles + zinfo->phaseLength;
    // assert(targetCycle <= phaseEndCycle);
    DEBUG_MSG("[%s] Joined, curCycle %ld phaseEnd %ld", name.c_str(), curCycle, phaseEndCycle);
}

void OOOCore::leave() {
    DEBUG_MSG("[%s] Leaving, curCycle %ld phaseEnd %ld", name.c_str(), curCycle, phaseEndCycle);
    cRec.notifyLeave(curCycle);
}

void OOOCore::cSimStart() {
    uint64_t targetCycle = cRec.cSimStart(curCycle);
    assert(targetCycle >= curCycle);
    if (targetCycle > curCycle) advance(targetCycle);
}

void OOOCore::cSimEnd() {
    uint64_t targetCycle = cRec.cSimEnd(curCycle);
    assert(targetCycle >= curCycle);
    if (targetCycle > curCycle) advance(targetCycle);
}

void OOOCore::advance(uint64_t targetCycle) {
    assert(targetCycle > curCycle);
    decodeCycle += targetCycle - curCycle;
    insWindow.longAdvance(curCycle, targetCycle);
    curCycleRFReads = 0;
    curCycleIssuedUops = 0;
    assert(targetCycle == curCycle);
    /* NOTE: Validation with weave mems shows that not advancing internal cycle
     * counters in e.g., the ROB does not change much; consider full-blown
     * rebases though if weave models fail to validate for some app.
     */
}

// Pin interface code
void OOOCore::LoadFunc(THREADID tid, ADDRINT addr, UINT32 size, ADDRINT pc) {static_cast<OOOCore*>(cores[tid])->load(addr, size, pc);}
void OOOCore::StoreFunc(THREADID tid, ADDRINT addr, UINT32 size, ADDRINT pc) {static_cast<OOOCore*>(cores[tid])->store(addr, size, pc);}

void OOOCore::PredLoadFunc(THREADID tid, ADDRINT addr, BOOL pred, UINT32 size, ADDRINT pc) {
    OOOCore* core = static_cast<OOOCore*>(cores[tid]);
    if (pred) core->load(addr, size, pc);
    else core->predFalseMemOp();
}

void OOOCore::PredStoreFunc(THREADID tid, ADDRINT addr, BOOL pred, UINT32 size, ADDRINT pc) {
    OOOCore* core = static_cast<OOOCore*>(cores[tid]);
    if (pred) core->store(addr, size, pc);
    else core->predFalseMemOp();
}

void OOOCore::BblFunc(THREADID tid, ADDRINT bblAddr, BblInfo* bblInfo) {
    OOOCore* core = static_cast<OOOCore*>(cores[tid]);
    core->bbl(bblAddr, bblInfo);

    while (core->curCycle > core->phaseEndCycle) {
        core->phaseEndCycle += zinfo->phaseLength;

        uint32_t cid = getCid(tid);
        // NOTE: TakeBarrier may take ownership of the core, and so it will be used by some other thread. If TakeBarrier context-switches us,
        // the *only* safe option is to return inmmediately after we detect this, or we can race and corrupt core state. However, the information
        // here is insufficient to do that, so we could wind up double-counting phases.
        uint32_t newCid = TakeBarrier(tid, cid);
        // NOTE: Upon further observation, we cannot race if newCid == cid, so this code should be enough.
        // It may happen that we had an intervening context-switch and we are now back to the same core.
        // This is fine, since the loop looks at core values directly and there are no locals involved,
        // so we should just advance as needed and move on.
        if (newCid != cid) break;  /*context-switch, we do not own this context anymore*/
    }
}

void OOOCore::BranchFunc(THREADID tid, ADDRINT pc, BOOL taken, ADDRINT takenNpc, ADDRINT notTakenNpc) {
    static_cast<OOOCore*>(cores[tid])->branch(pc, taken, takenNpc, notTakenNpc);
}
