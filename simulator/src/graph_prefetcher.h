#ifndef GRAPH_PREFETCHER_H
#define GRAPH_PREFETCHER_H

#include "stats.h"
#include "memory_hierarchy.h"
#include "g_std/g_string.h"
#include "timing_event.h"

// For graph prefetcher
enum GraphDataMask {
    DEST_NODE, WEIGHT_VALUE, DEST_PROPERTY, POS_END
};

enum GraphDataIndex {
    OFFSET_INDEX, EDGE_INDEX, WEIGHT_INDEX, PROPERTY_INDEX, SRC_NODE_INDEX, UPDATES_SIZE_INDEX, SRC_PROPERTY_INDEX, DEST_NODE_INDEX, WEIGHT_VALUE_INDEX, DEST_PROPERTY_INDEX
};

#define GRAPH_DATA_READY ((1 << DEST_NODE) | (1 << WEIGHT_VALUE) | (1 << DEST_PROPERTY))
#define GRAPH_PREFETCHER_ELE_SIZE 8

// Record real data about the node, used by intrumentation
struct GraphSrcInfo
{
    int64_t src_node; // source node to set
    int64_t src_property; // source property to get
};

struct GraphDestInfo
{
    int64_t dest_node; // dest node to get
    int64_t weight_value; // weight value to get
    int64_t dest_property; // dest property to get
};

struct GraphPrefetcherParams
{
    uintptr_t offset;
    uintptr_t edge;
    uintptr_t weight;
    uintptr_t property;
};

struct GraphPrefetcherUnit
{
    std::vector<GraphSrcInfo> srcInfo;
    std::vector<GraphDestInfo> destInfo;
    int readyBits; // record which destInfo is ready to get

    GraphPrefetcherUnit() : readyBits(GRAPH_DATA_READY) {}
};


// struct to store information about the source and destination nodes, used by performance simulation
struct SrcInfo
{
    Address property;
    Address offsetStart;
    Address offsetEnd;
    uint32_t numNeighbors;
};

struct DestInfo
{
    Address edge;
    Address weight;
    Address property;
};

// GraphPrefetcher is a prefetcher specialized for graph processing.

class GraphPrefetcher : public BaseCache {

    private:
        Counter profLoad;
        Counter profStore;
        
        MemObject* parent;
        uint32_t childId;
        uint32_t srcId; // should match the core
        g_string name;
        uint32_t reqFlags;

        lock_t lock;

        uint32_t nEntries;
        uint32_t numMSHRs;
        uint32_t latency;
        
        g_vector<SrcInfo> srcInfo; // related info of src nodes
        g_vector<DestInfo> destInfo; // related info of dest nodes
        
        struct SrcEntryInfo
        {
            uint64_t propertyAvailCycle; // record the cycle when the property is availCycle
            uint64_t offsetAvailCycle; // record the cycle when the offset is availCycle
            TimingEvent* endEvent; // record the end event of the src access
            TimingEvent* offsetEndEvent; // record the end event of the start offset access
        };
        

        struct DestEntryInfo
        {
            int readyBits; // record which destInfo is ready to get
            // record the cycle when the data is ready
            uint64_t edgeReadyCycle;
            uint64_t weightReadyCycle;
            uint64_t propertyReadyCycle;
            TimingEvent* edgeEndEv; // record the end event of the edge access
            TimingEvent* weightEndEv; // record the end event of the weight access
            TimingEvent* propertyEndEv; // record the end event of the property access
        };

        SrcEntryInfo srcEntry;
        g_vector<DestEntryInfo> destArray;
        uint32_t startIndex;
        uint32_t endIndex;

        void srcAccess(uint64_t cycle, Address offset, TimingRecord &acc);
        void destAccess(uint64_t cycle, TimingEvent* startEv);

    public:
        GraphPrefetcher(const g_string& _name, const uint32_t _nEntries, const uint32_t _latency)
         : name(_name), nEntries(_nEntries), numMSHRs(0), latency(_latency) {
            destArray.resize(nEntries);
            reqFlags = 0;
            srcId = -1;
            startIndex = 0;
            endIndex = 0;
            futex_init(&lock);
        }
        
        void pushSrcInfo(SrcInfo src);
        void pushDestInfo(DestInfo dest);

        void setSourceId(uint32_t id) {srcId = id;}
        void setFlags(uint32_t flags) { reqFlags = flags; }
        void initStats(AggregateStat* parentStat);
        const char* getName() {return name.c_str();}
        void setParents(uint32_t _childId, const g_vector<MemObject*>& parents, Network* network);
        void setChildren(const g_vector < BaseCache * >&children, Network * network)
        {
            panic("[%s] GraphPrefetcher::setChildren cannot be called -- it cannot have children!", name.c_str());
        }
        uint64_t access(MemReq& req);
        uint64_t load(Address offset, uint64_t cycle);
        uint64_t store(Address offset, uint64_t cycle);
        uint64_t invalidate(const InvReq& req);
};


#endif