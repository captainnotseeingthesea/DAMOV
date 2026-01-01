#include "prefetch/graph_prefetcher.h"
#include "zsim.h"

void GraphPrefetcher::pushSrcInfo(SrcInfo src)
{
    uint64_t reqCycle = max(worklistSetCycle, srcTable.minAllocCycle());
    bool stallByCPU = srcTable.stallType();
    EventRecorder* evRec = zinfo->eventRecorders[srcId];
    // Start the access to the source node
    MESIState state = MESIState::I;
    Address srcAddr = src.src >> lineBits;
    Address propertyAddr = src.property >> lineBits;
    Address offsetStartAddr = src.offsetStart >> lineBits;
    Address offsetEndAddr = src.offsetEnd >> lineBits;
    AccessInfo accessInfo = {src.property, 0, 0, AccessInfo::NONE, AccessInfo::OTHER};

    // record the stall cycle
    if(reqCycle - worklistSetCycle > 0 && stallByCPU)
        ProfStallCycle += (reqCycle - worklistSetCycle);

    // control the request bandwidth
    if(reqCycle <= lastAccessCycle)
    {
        reqCycle = lastAccessCycle + 1;
    }
    lastAccessCycle = reqCycle;

    MemReq req = {srcAddr | procMask, accessInfo, GETS, childId, &state, reqCycle, &lock,
                    state, srcId, reqFlags | MemReq::PREFETCH};
    uint64_t srcRespCycle = reqCycle;
    if(lastWorklistLineAddr.lineAddr != srcAddr)
    {
        srcRespCycle = parent->access(req); // access the node id from the worklist
        lastWorklistLineAddr = {srcAddr, srcRespCycle};
        profTemporalBufferWrite++;
    }
    else
    {
        profTemporalBufferRead++;
        srcRespCycle = max(reqCycle, lastWorklistLineAddr.readyCycle);
    }

    if((evRec))
    {
        if(evRec->hasRecord())
        {
            TimingRecord tr = evRec->popRecord();
            tr.startEvent->releaseEvent();
        }
    }

    req.lineAddr = propertyAddr | procMask;
    req.cycle = srcRespCycle + 1;
    uint64_t propertyRespCycle = parent->access(req); // access the property of the source node

    if((evRec))
    {
        if(evRec->hasRecord())
        {
            TimingRecord tr = evRec->popRecord();
            tr.startEvent->releaseEvent();
        }
    }

    req.lineAddr = offsetStartAddr | procMask;
    uint64_t offsetRespCycle = parent->access(req);

    if((evRec))
    {
        if(evRec->hasRecord())
        {
            TimingRecord tr = evRec->popRecord();
            tr.startEvent->releaseEvent();
        }
    }

    if(offsetStartAddr != offsetEndAddr)
    {
        req.lineAddr = offsetEndAddr | procMask;
        uint64_t respCycle = parent->access(req);
        offsetRespCycle = max(respCycle, offsetRespCycle);
        if((evRec))
        {
            if(evRec->hasRecord())
            {
                TimingRecord tr = evRec->popRecord();
                tr.startEvent->releaseEvent();
            }
        }
    }
    offsetAvailCycle = offsetRespCycle;
    DBG("request src data at cycle: %lu, resp node: %lu, offset: %lu, property: %lu", reqCycle, srcRespCycle, offsetRespCycle, propertyRespCycle);
    srcEntryInfo.push_back((SrcEntryInfo){srcRespCycle, propertyRespCycle, SRC_DATA_READY});
    profRootFetch++;
    profPrefetch += 4;
}

void GraphPrefetcher::pushDestInfo(DestInfo dest)
{
    uint64_t reqCycle = destTable.minAllocCycle();
    bool stallByCPU = destTable.stallType();
    EventRecorder* evRec = zinfo->eventRecorders[srcId];

    MESIState state = I;
    Address edgeAddr = dest.edge >> lineBits;
    // wait until the edge offset is available
    reqCycle = max(reqCycle, offsetAvailCycle);

    // record the stall cycle
    if(reqCycle - offsetAvailCycle > 0 && stallByCPU)
        ProfStallCycle += (reqCycle - offsetAvailCycle);

    // control the request bandwidth
    // if(reqCycle <= lastAccessCycle)
    // {
    //     reqCycle = lastAccessCycle + 1;
    // }
    // lastAccessCycle = reqCycle;
    AccessInfo accessInfo = {dest.edge, 0, 0, AccessInfo::NONE, AccessInfo::OTHER};
    MemReq req = {edgeAddr | procMask, accessInfo, GETS, childId, &state, reqCycle, &lock,
                    state, srcId, reqFlags | MemReq::PREFETCH};
    uint64_t edgeRespCycle = reqCycle;
    if(lastEdgeLineAddr.lineAddr != edgeAddr)
    {
        edgeRespCycle = parent->access(req);
        lastEdgeLineAddr = {edgeAddr, edgeRespCycle};
        profTemporalBufferWrite++;
    }
    else
    {
        edgeRespCycle = max(edgeRespCycle, lastEdgeLineAddr.readyCycle);
        profTemporalBufferRead++;
    }
    // DBG"request edge at cycle: %lu, resp: %lu",reqCycle, edgeRespCycle);
    if((evRec))
    {
        if(evRec->hasRecord())
        {
            TimingRecord tr = evRec->popRecord();
            tr.startEvent->releaseEvent();
        }
    }

    uint64_t weightRespCycle = reqCycle;
    if(zinfo->weightEnable)
    {
        Address weightAddr = dest.weight >> lineBits;
        req.lineAddr = weightAddr | procMask;
        if(lastWeightLineAddr.lineAddr != weightAddr)
        {
            weightRespCycle = parent->access(req);
            lastWeightLineAddr = {weightAddr, weightRespCycle};
            profTemporalBufferWrite++;
        }
        else
        {
            weightRespCycle = max(weightRespCycle, lastWeightLineAddr.readyCycle);
            profTemporalBufferRead++;
        }

        if((evRec))
        {
            if(evRec->hasRecord())
            {
                TimingRecord tr = evRec->popRecord();
                tr.startEvent->releaseEvent();
            }
        }
        profPrefetch += 3; // fetch edge, weight, state
    }
    else
    {
        profPrefetch += 2; // fetch edge, state
    }

    Address propertyAddr = dest.property >> lineBits;
    req.cycle = edgeRespCycle;
    req.lineAddr = propertyAddr | procMask;
    uint64_t propertyRespCycle = parent->access(req);
    if((evRec))
    {
        if(evRec->hasRecord())
        {
            TimingRecord tr = evRec->popRecord();
            tr.startEvent->releaseEvent();
        }
    }
    // set the dest info
    if(dest.valid)
    {
        destEntryInfo.push_back((DestEntryInfo){zinfo->weightEnable ? DEST_DATA_READY : DEST_DATA_READY_WITHOUT_WEIGHT, edgeRespCycle, weightRespCycle, propertyRespCycle});
    }
    else
    {
        destTable.markComplete(max(edgeRespCycle, max(weightRespCycle, propertyRespCycle)), false);
    }
    DBG("request dest data cycle: %lu, resp node: %lu, weight: %lu, property: %lu", reqCycle, edgeRespCycle, weightRespCycle, propertyRespCycle);
    profDestFetch++;
}

void GraphPrefetcher::initStats(AggregateStat* parentStat)
{
    AggregateStat *s = new AggregateStat();
    s->init(name.c_str(), "Graph prefetcher stats");

    ProxyStat* prefetchStat = new ProxyStat();
    prefetchStat->init("Prefetch", "number of prefetch requests", &profPrefetch);
    ProxyStat* rootFetchStat = new ProxyStat();
    rootFetchStat->init("RootFetch", "number of root node to prefetch", &profRootFetch);
    ProxyStat* destFetchStat = new ProxyStat();
    destFetchStat->init("DestFetch", "number of dest node to prefetch", &profDestFetch);
    ProxyStat* configStat = new ProxyStat();
    configStat->init("ConfigTimes", "number of configuration time", &profConfig);
    ProxyStat* tempBufferReadStat = new ProxyStat();
    tempBufferReadStat->init("TempBufferRead", "number of reads to the temporal buffer", &profTemporalBufferRead);
    ProxyStat* tempBufferWriteStat = new ProxyStat();
    tempBufferWriteStat->init("TempBufferWrite", "number of writes to the temporal buffer", &profTemporalBufferWrite);
    ProxyStat* stallCycleStat = new ProxyStat();
    stallCycleStat->init("StallCycles", "number of stall cycles due to resource limitation", &ProfStallCycle);
    auto averageNodeWaitCycle = [this](){
        return profRootFetch > 0 ? profSrcNodeWaitCycle / profRootFetch : 0;
    };
    LambdaStat<decltype(averageNodeWaitCycle)>* AverageNodeWaitCycleStat = new LambdaStat<decltype(averageNodeWaitCycle)>(averageNodeWaitCycle);
    AverageNodeWaitCycleStat->init("AverageNodeWaitCycles", "average number of cycles CPU wait for node data");

    auto averagePropertyWaitCycle = [this](){
        return profRootFetch > 0 ? profSrcPropertyWaitCycle / profRootFetch : 0;
    };
    LambdaStat<decltype(averagePropertyWaitCycle)>* AveragePropertyWaitCycleStat = new LambdaStat<decltype(averagePropertyWaitCycle)>(averagePropertyWaitCycle);
    AveragePropertyWaitCycleStat->init("AveragePropertyWaitCycles", "average number of cycles CPU wait for property data");

    auto averageDestNodeWaitCycle = [this](){
        return profDestFetch > 0 ? profDestNodeWaitCycle / profDestFetch : 0;
    };
    LambdaStat<decltype(averageDestNodeWaitCycle)>* AverageDestNodeWaitCycleStat = new LambdaStat<decltype(averageDestNodeWaitCycle)>(averageDestNodeWaitCycle);
    AverageDestNodeWaitCycleStat->init("AverageDestNodeWaitCycles", "average number of cycles CPU wait for dest node data");
    auto averageDestWeightWaitCycle = [this](){
        return profDestFetch > 0 ? profDestWeightWaitCycle / profDestFetch : 0;
    };
    LambdaStat<decltype(averageDestWeightWaitCycle)>* AverageDestWeightWaitCycleStat = new LambdaStat<decltype(averageDestWeightWaitCycle)>(averageDestWeightWaitCycle);
    AverageDestWeightWaitCycleStat->init("AverageDestWeightWaitCycles", "average number of cycles CPU wait for dest weight data");
    auto averageDestPropertyWaitCycle = [this](){
        return profDestFetch > 0 ? profDestPropertyWaitCycle / profDestFetch : 0;
    };
    LambdaStat<decltype(averageDestPropertyWaitCycle)>* AverageDestPropertyWaitCycleStat = new LambdaStat<decltype(averageDestPropertyWaitCycle)>(averageDestPropertyWaitCycle);
    AverageDestPropertyWaitCycleStat->init("AverageDestPropertyWaitCycles", "average number of cycles CPU wait for dest property data");

    s->append(prefetchStat);
    s->append(rootFetchStat);
    s->append(destFetchStat);
    s->append(configStat);
    s->append(tempBufferReadStat);
    s->append(tempBufferWriteStat);
    s->append(stallCycleStat);
    s->append(AverageNodeWaitCycleStat);
    s->append(AveragePropertyWaitCycleStat);
    s->append(AverageDestNodeWaitCycleStat);
    s->append(AverageDestWeightWaitCycleStat);
    s->append(AverageDestPropertyWaitCycleStat);
    parentStat->append(s);
}

void GraphPrefetcher::setParents(uint32_t _childId, const g_vector < MemObject * >&parents, Network * network)
{
    childId = _childId;
    if (parents.size() != 1)
        panic("Must have one parent");
    // if (network)
        // panic("Network not handled");
    parent = parents[0];
}

uint64_t GraphPrefetcher::access(MemReq& req)
{
    panic("Not implemented");
}

uint64_t GraphPrefetcher::load(Address offset, uint64_t cycle)
{
    uint64_t respCycle, reqCycle;
    reqCycle = respCycle = cycle + latency;
    switch (offset)
    {
    case UPDATES_SIZE_INDEX:
        break;
    case WORKNODE_INDEX:
        assert(!srcEntryInfo.empty());
        respCycle = max(respCycle, srcEntryInfo.front().nodeAvailCycle);
        assert(srcEntryInfo.front().readyBits & (1 << SRC_NODE));
        srcEntryInfo.front().readyBits &= ~(1 << SRC_NODE);
        profSrcNodeWaitCycle += (respCycle - reqCycle);
        break;
    case SRC_PROPERTY_INDEX:
        assert(!srcEntryInfo.empty());
        respCycle = max(respCycle, srcEntryInfo.front().propertyAvailCycle);
        assert(srcEntryInfo.front().readyBits & (1 << SRC_PROPERTY));
        srcEntryInfo.front().readyBits &= ~(1 << SRC_PROPERTY);
        srcTable.markComplete(respCycle, respCycle == reqCycle);
        profSrcPropertyWaitCycle += (respCycle - reqCycle);
        // DBG"get src property: req: %lu, resp %lu, latency: %lu", reqCycle, respCycle, respCycle - reqCycle);
        break;
    case DEST_NODE_INDEX:
        // DBG"Get dest data at cycle: %lu", reqCycle);
        assert(!destEntryInfo.empty());
        respCycle = max(respCycle, destEntryInfo.front().edgeReadyCycle);
        assert(destEntryInfo.front().readyBits & (1 << DEST_NODE));
        destEntryInfo.front().readyBits &= ~(1 << DEST_NODE);
        profDestNodeWaitCycle += (respCycle - reqCycle);
        break;
    case WEIGHT_VALUE_INDEX:
        assert(!destEntryInfo.empty());
        respCycle = max(respCycle, destEntryInfo.front().weightReadyCycle);
        assert(destEntryInfo.front().readyBits & (1 << WEIGHT_VALUE));
        destEntryInfo.front().readyBits &= ~(1 << WEIGHT_VALUE);
        profDestWeightWaitCycle += (respCycle - reqCycle);
        break;
    case DEST_PROPERTY_INDEX:
        assert(!destEntryInfo.empty());
        respCycle = max(respCycle, destEntryInfo.front().propertyReadyCycle);
        assert(destEntryInfo.front().readyBits & (1 << DEST_PROPERTY));
        destEntryInfo.front().readyBits &= ~(1 << DEST_PROPERTY);
        destTable.markComplete(respCycle, reqCycle == respCycle);
        profDestPropertyWaitCycle += (respCycle - reqCycle);
        // DBG"get dest property: req: %lu, resp %lu, latency: %lu", reqCycle, respCycle, respCycle - reqCycle);
        break;
    default:
        panic("Invalid load index");
        break;
    }
    if(!destEntryInfo.empty() && destEntryInfo.front().readyBits == 0)
    {
        destEntryInfo.pop_front();
    }
    if(!srcEntryInfo.empty() && srcEntryInfo.front().readyBits == 0)
    {
        srcEntryInfo.pop_front();
    }
    return respCycle;
}

uint64_t GraphPrefetcher::store(Address offset, uint64_t cycle)
{
    uint64_t respCycle = cycle + latency;
    uint64_t accCycle = respCycle;
    switch (offset)
    {
        case OFFSET_INDEX:
        case EDGE_INDEX:
        case WEIGHT_INDEX:
        case PROPERTY_INDEX:
            break;
        case SRC_NODE_INDEX:
            panic("not implemented!")
            break;
        case WORKLIST_INDEX:
            worklistSetCycle = max(worklistSetCycle, respCycle);
            break;
        case NUMNODE_INDEX:
            worklistSetCycle = max(worklistSetCycle, respCycle);
            break;
        default:
            panic("invalid store offset");
            break;
    }
    profConfig++;
    return respCycle;
}


uint64_t GraphPrefetcher::invalidate(const InvReq& req)
{
    panic("Not implemented");
}
