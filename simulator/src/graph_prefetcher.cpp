#include "graph_prefetcher.h"
#include "zsim.h"

void GraphPrefetcher::pushSrcInfo(SrcInfo src)
{
    srcInfo.push_back(src);
}

void GraphPrefetcher::pushDestInfo(DestInfo dest)
{
    destInfo.push_back(dest);
}

void GraphPrefetcher::initStats(AggregateStat* parentStat)
{
    AggregateStat *s = new AggregateStat();
    s->init(name.c_str(), "Graph prefetcher stats");
    profLoad.init("load", "loads");
    s->append(&profLoad);
    profStore.init("stpre", "stores");
    s->append(&profStore);
    parentStat->append(s);
}

void GraphPrefetcher::setParents(uint32_t _childId, const g_vector < MemObject * >&parents, Network * network)
{
    childId = _childId;
    if (parents.size() != 1)
        panic("Must have one parent");
    if (network)
        panic("Network not handled");
    parent = parents[0];
}

uint64_t GraphPrefetcher::access(MemReq& req)
{
    panic("Not implemented");
}

void GraphPrefetcher::srcAccess(uint64_t cycle, Address offset, TimingRecord &acc)
{
    assert(!srcInfo.empty());
    uint64_t reqCycle = cycle;
    // record the the produced events during src accesses
    EventRecorder* evRec = zinfo->eventRecorders[srcId];
    TimingRecord srcAcc, offsetAcc0, offsetAcc1;
    DelayEvent* startEv;
    DummyEvent *offsetEndEv, *srcEndEv;
    srcAcc.clear();
    offsetAcc0.clear();
    offsetAcc1.clear();
    // Start the access to the source node
    MESIState state = MESIState::I;
    Address propertyAddr = srcInfo.front().property >> lineBits;
    Address offsetStartAddr = srcInfo.front().offsetStart >> lineBits;
    Address offsetEndAddr = srcInfo.front().offsetEnd >> lineBits;
    MemReq req = {propertyAddr, GETS, childId, &state, reqCycle, &lock,
                    state, srcId, reqFlags | MemReq::PREFETCH};
    uint64_t propertyRespCycle = parent->access(req); // access the property of the source node
    srcEntry.propertyAvailCycle = propertyRespCycle;

    if((evRec))
    {
        startEv = new (evRec) DelayEvent(0);
        offsetEndEv = new (evRec) DummyEvent(false);
        srcEndEv = new (evRec) DummyEvent(false);
        startEv->setMinStartCycle(reqCycle);
        acc.startEvent = startEv;
        acc.endEvent = nullptr;
        srcEntry.offsetEndEvent = offsetEndEv;

        if(evRec->hasRecord())
        {
            srcAcc = evRec->popRecord();
            assert(srcAcc.reqCycle >= reqCycle);
            DelayEvent* dSrcAccEv = new (evRec) DelayEvent(srcAcc.reqCycle - srcAcc.reqCycle);
            dSrcAccEv->setMinStartCycle(reqCycle);
            startEv->addChild(dSrcAccEv, evRec)->addChild(srcAcc.startEvent, evRec);
            srcAcc.endEvent->addChild(srcEndEv, evRec);
        }
        DelayEvent *dSrcEv = new (evRec) DelayEvent(propertyRespCycle - reqCycle);
        dSrcEv->setMinStartCycle(reqCycle);
        srcEndEv->setMinStartCycle(propertyRespCycle);
        startEv->addChild(dSrcEv, evRec)->addChild(srcEndEv, evRec);
        srcEntry.endEvent = srcEndEv;
    }

    req.lineAddr = offsetStartAddr;
    uint64_t offsetRespCycle = parent->access(req);

    if((evRec && evRec->hasRecord()))
    {
        offsetAcc0 = evRec->popRecord();
        assert(offsetAcc0.reqCycle >= reqCycle);
        DelayEvent* dOffsetAccEv = new (evRec) DelayEvent(offsetAcc0.reqCycle - reqCycle);
        dOffsetAccEv->setMinStartCycle(reqCycle);
        startEv->addChild(dOffsetAccEv, evRec)->addChild(offsetAcc0.startEvent, evRec);
        offsetAcc0.endEvent->addChild(offsetEndEv, evRec);
    }
    if(offsetStartAddr != offsetEndAddr)
    {
        req.lineAddr = offsetEndAddr;
        offsetRespCycle = max(parent->access(req), offsetRespCycle);
        if((evRec && evRec->hasRecord()))
        {
            offsetAcc1 = evRec->popRecord();
            assert(offsetAcc1.reqCycle >= reqCycle);
            DelayEvent* dOffsetAccEv = new (evRec) DelayEvent(offsetAcc1.reqCycle - reqCycle);
            dOffsetAccEv->setMinStartCycle(reqCycle);
            startEv->addChild(dOffsetAccEv, evRec)->addChild(offsetAcc1.startEvent, evRec);
            offsetAcc1.endEvent->addChild(offsetEndEv, evRec);
        }
    }
    srcEntry.offsetAvailCycle = offsetRespCycle;
    if((evRec))
    {
        // make sure the offsetendEv is linked
        offsetEndEv->setMinStartCycle(offsetRespCycle);
        DelayEvent *dOffsetEndEv = new (evRec) DelayEvent(offsetRespCycle - reqCycle);
        dOffsetEndEv->setMinStartCycle(reqCycle);
        startEv->addChild(dOffsetEndEv, evRec)->addChild(offsetEndEv, evRec);
    }

    srcInfo.erase(srcInfo.begin());
}

void GraphPrefetcher::destAccess(uint64_t cycle, TimingEvent* startEv)
{
    assert(!destInfo.empty());
    uint64_t respCycle, reqCycle = cycle;
    // record the the produced events during dest accesses
    EventRecorder* evRec = zinfo->eventRecorders[srcId];
    TimingRecord edgeAcc, weightAcc, propertyAcc;
    edgeAcc.clear();
    weightAcc.clear();
    propertyAcc.clear();
    DelayEvent *reqEv;
    DummyEvent *edgeEndEv, *weightEndEv, *propertyEndEv;

    MESIState state = I;
    Address edgeAddr = destInfo.front().edge >> lineBits;
    // wait until the edge offset is available
    reqCycle = max(reqCycle, srcEntry.offsetAvailCycle);
    MemReq req = {edgeAddr, GETS, 0, &state, reqCycle, &lock,
                    state, srcId, reqFlags | MemReq::PREFETCH};
    uint64_t edgeRespCycle = parent->access(req);
    if(evRec)
    {
        edgeEndEv = new (evRec) DummyEvent(false);
        edgeEndEv->setMinStartCycle(edgeRespCycle);
        DelayEvent* dReqEv = new (evRec) DelayEvent(reqCycle - cycle);
        reqEv = new (evRec) DelayEvent(0);
        reqEv->setMinStartCycle(reqCycle);
        dReqEv->setMinStartCycle(cycle);
        startEv->addChild(dReqEv, evRec)->addChild(reqEv, evRec);
        if(srcEntry.offsetEndEvent && srcEntry.offsetEndEvent->canAddChild())
        {
            srcEntry.offsetEndEvent->addChild(reqEv, evRec);
        }
        if(evRec->hasRecord())
        {
            edgeAcc = evRec->popRecord();
            assert(edgeAcc.reqCycle >= reqCycle);
            DelayEvent* dEdgeAccEv = new (evRec) DelayEvent(edgeAcc.reqCycle - reqCycle);
            dEdgeAccEv->setMinStartCycle(reqCycle);
            reqEv->addChild(dEdgeAccEv, evRec)->addChild(edgeAcc.startEvent, evRec);
            edgeAcc.endEvent->addChild(edgeEndEv, evRec);
        }
        DelayEvent* dEdgeEndEv = new (evRec) DelayEvent(edgeRespCycle - reqCycle);
        dEdgeEndEv->setMinStartCycle(reqCycle);
        reqEv->addChild(dEdgeEndEv, evRec)->addChild(edgeEndEv, evRec);
        destArray[endIndex].edgeEndEv = edgeEndEv;
    }

    Address weightAddr = destInfo.front().weight >> lineBits;
    req.cycle = ++reqCycle;
    req.lineAddr = weightAddr;
    uint64_t weightRespCycle = parent->access(req);
    if(evRec)
    {
        weightEndEv = new (evRec) DummyEvent(false);
        weightEndEv->setMinStartCycle(weightRespCycle);
        DelayEvent* dReqEv = new (evRec) DelayEvent(reqCycle - cycle);
        reqEv = new (evRec) DelayEvent(0);
        reqEv->setMinStartCycle(reqCycle);
        dReqEv->setMinStartCycle(cycle);
        startEv->addChild(dReqEv, evRec)->addChild(reqEv, evRec);
        if(srcEntry.offsetEndEvent && srcEntry.offsetEndEvent->canAddChild())
        {
            srcEntry.offsetEndEvent->addChild(reqEv, evRec);
        }
        if(evRec->hasRecord())
        {
            weightAcc = evRec->popRecord();
            assert(weightAcc.reqCycle >= reqCycle);
            DelayEvent* dWeightAccEv = new (evRec) DelayEvent(weightAcc.reqCycle - reqCycle);
            dWeightAccEv->setMinStartCycle(reqCycle);
            reqEv->addChild(dWeightAccEv, evRec)->addChild(weightAcc.startEvent, evRec);
            weightAcc.endEvent->addChild(weightEndEv, evRec);
        }
        DelayEvent* dWeightEndEv = new (evRec) DelayEvent(weightRespCycle - reqCycle);
        dWeightEndEv->setMinStartCycle(reqCycle);
        reqEv->addChild(dWeightEndEv, evRec)->addChild(weightEndEv, evRec);
        destArray[endIndex].weightEndEv = weightEndEv;
    }

    Address propertyAddr = destInfo.front().property >> lineBits;
    req.cycle = reqCycle = edgeRespCycle;
    req.lineAddr = propertyAddr;
    uint64_t propertyRespCycle = parent->access(req);
    if(evRec)
    {
        propertyEndEv = new (evRec) DummyEvent(false);
        propertyEndEv->setMinStartCycle(propertyRespCycle);
        DelayEvent* dPropertyEndEv = new (evRec) DelayEvent(propertyRespCycle - reqCycle);
        dPropertyEndEv->setMinStartCycle(reqCycle);
        edgeEndEv->addChild(dPropertyEndEv, evRec)->addChild(propertyEndEv, evRec);
        if(evRec->hasRecord())
        {
            propertyAcc = evRec->popRecord();
            assert(propertyAcc.reqCycle >= reqCycle);
            DelayEvent* dPropertyAccEv = new (evRec) DelayEvent(propertyAcc.reqCycle - reqCycle);
            dPropertyAccEv->setMinStartCycle(reqCycle);
            edgeEndEv->addChild(dPropertyAccEv, evRec)->addChild(propertyAcc.startEvent, evRec);
            propertyAcc.endEvent->addChild(propertyEndEv, evRec);
        }
        destArray[endIndex].propertyEndEv = propertyEndEv;
    }
    // set the dest info
    destArray[endIndex] = {GRAPH_DATA_READY, edgeRespCycle, weightRespCycle, propertyRespCycle, edgeEndEv, weightEndEv, propertyEndEv};
    destInfo.erase(destInfo.begin());
    endIndex = (endIndex + 1) % nEntries;
}

uint64_t GraphPrefetcher::load(Address offset, uint64_t cycle)
{
    uint64_t respCycle, reqCycle;
    respCycle = reqCycle = cycle;
    respCycle += latency;
    EventRecorder* evRec = zinfo->eventRecorders[srcId];
    TimingRecord acc = {offset, reqCycle, reqCycle + latency, GETS, nullptr, nullptr};
    DelayEvent* startEv; // the start event of the access
    DelayEvent* endEv; // the end event of the access
    if((evRec))
    {
        startEv = new (evRec) DelayEvent(0);
        DelayEvent *delayEv = new (evRec) DelayEvent(latency);
        endEv = new (evRec) DelayEvent(0);
        startEv->setMinStartCycle(reqCycle);
        delayEv->setMinStartCycle(reqCycle);
        endEv->setMinStartCycle(respCycle);
        startEv->addChild(delayEv, evRec)->addChild(endEv, evRec);
        acc.startEvent = startEv;
        acc.endEvent = endEv;
    }
    switch (offset)
    {
    case UPDATES_SIZE_INDEX:
        break;
    case SRC_PROPERTY_INDEX:
        respCycle = max(respCycle, srcEntry.propertyAvailCycle);
        if((evRec))
        {
            if(srcEntry.endEvent)
            {
                if(srcEntry.endEvent->canAddChild())
                {
                    srcEntry.endEvent->setFreeElem(true);
                    srcEntry.endEvent->addChild(endEv, evRec);
                }
                else
                {
                    assert(srcEntry.endEvent->hasDone());
                    srcEntry.endEvent->freeEv();
                }
                srcEntry.endEvent = nullptr;
            }
            else
            {
                assert(false);
            }
            acc.respCycle = respCycle;
        }
        break;
    case DEST_NODE_INDEX:
        respCycle = max(respCycle, destArray[startIndex].edgeReadyCycle);
        assert(destArray[startIndex].readyBits & (1 << DEST_NODE));
        destArray[startIndex].readyBits &= ~(1 << DEST_NODE); 
        if((evRec))
        {
            if(destArray[startIndex].edgeEndEv)
            {
                if(destArray[startIndex].edgeEndEv->canAddChild())
                {
                    destArray[startIndex].edgeEndEv->setFreeElem(true);
                    destArray[startIndex].edgeEndEv->addChild(endEv, evRec);
                }
                else
                {
                    assert(destArray[startIndex].edgeEndEv->hasDone());
                    destArray[startIndex].edgeEndEv->freeEv();
                }
                destArray[startIndex].edgeEndEv = nullptr;
            }
            else
            {
                assert(false);
            }

            acc.respCycle = respCycle;
        }
        break;
    case WEIGHT_VALUE_INDEX:
        respCycle = max(respCycle, destArray[startIndex].weightReadyCycle);
        assert(destArray[startIndex].readyBits & (1 << WEIGHT_VALUE));
        destArray[startIndex].readyBits &= ~(1 << WEIGHT_VALUE);
        if((evRec))
        {
            if(destArray[startIndex].weightEndEv)
            {
                if(destArray[startIndex].weightEndEv->canAddChild())
                {
                    destArray[startIndex].weightEndEv->setFreeElem(true);
                    destArray[startIndex].weightEndEv->addChild(endEv, evRec);
                }
                else
                {
                    assert(destArray[startIndex].weightEndEv->hasDone());
                    destArray[startIndex].weightEndEv->freeEv();
                }
                destArray[startIndex].weightEndEv = nullptr;
            }
            else
            {
                assert(false);
            }

            acc.respCycle = respCycle;
        }
        break;
    case DEST_PROPERTY_INDEX:
        respCycle = max(respCycle, destArray[startIndex].propertyReadyCycle);
        assert(destArray[startIndex].readyBits & (1 << DEST_PROPERTY));
        destArray[startIndex].readyBits &= ~(1 << DEST_PROPERTY);
        if((evRec))
        {
            if(destArray[startIndex].propertyEndEv)
            {
                if(destArray[startIndex].propertyEndEv->canAddChild())
                {
                    destArray[startIndex].propertyEndEv->setFreeElem(true);
                    destArray[startIndex].propertyEndEv->addChild(endEv, evRec);
                }
                else
                {
                    assert(destArray[startIndex].propertyEndEv->hasDone());
                    destArray[startIndex].propertyEndEv->freeEv();
                }
                destArray[startIndex].propertyEndEv = nullptr;
            }
            else
            {
                assert(false);
            }

            acc.respCycle = respCycle;
        }
        break;
    default:
        panic("Invalid load index");
        break;
    }
    // current entry all has been fetched
    if(destArray[startIndex].readyBits == 0)
    {
        if(offset != UPDATES_SIZE_INDEX && offset != SRC_PROPERTY_INDEX)
        {
            startIndex = (startIndex + 1) % nEntries;
        }
        // if the last entry has been fetched, fetch the next entry
        if(!destInfo.empty())
        {
            destAccess(reqCycle, startEv);
        }
        else
        {
            if(srcEntry.offsetEndEvent)
            {
                if(srcEntry.offsetEndEvent->canAddChild())
                {
                    srcEntry.offsetEndEvent->setFreeElem(true);
                }
                else
                {
                    assert(srcEntry.offsetEndEvent->hasDone());
                    srcEntry.offsetEndEvent->freeEv();
                }
                srcEntry.offsetEndEvent = nullptr;
            }

        }
    }
    if(evRec)
    {
        assert(!evRec->hasRecord());
        evRec->pushRecord(acc);
    }
    profLoad.inc();
    return respCycle;
}

uint64_t GraphPrefetcher::store(Address offset, uint64_t cycle)
{
    uint64_t reqCycle = cycle, respCycle = cycle;
    EventRecorder* evRec = zinfo->eventRecorders[srcId];
    TimingRecord acc = {offset, reqCycle, reqCycle + latency, PUTX, nullptr, nullptr};
    TimingEvent *srcEvent;
    respCycle += latency;
    switch (offset)
    {
        case OFFSET_INDEX:
        case EDGE_INDEX:
        case WEIGHT_INDEX:
        case PROPERTY_INDEX:
            break;
        case SRC_NODE_INDEX:
            srcAccess(reqCycle, offset, acc);
            if((evRec))
            {
                srcEvent = acc.startEvent;
            }
            while(!destInfo.empty() && destArray[endIndex].readyBits == 0)
            {
                destAccess(reqCycle, srcEvent);
            }
            if((evRec))
            {
                evRec->pushRecord(acc);
            }
            break;
        default:
            panic("invalid store offset");
            break;
    }
    profStore.inc();
    return respCycle;
}


uint64_t GraphPrefetcher::invalidate(const InvReq& req)
{
    panic("Not implemented");
}
