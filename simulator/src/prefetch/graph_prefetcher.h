#ifndef GRAPH_PREFETCHER_H
#define GRAPH_PREFETCHER_H

#include "stats.h"
#include "memory_hierarchy.h"
#include "g_std/g_string.h"
#include "g_std/g_deque.h"
#include "timing_event.h"
#include "prefetch/prefetcher.h"

// For graph prefetcher
enum SrcDataMask{
    SRC_NODE, SRC_PROPERTY
};

enum DestDataMask {
    DEST_NODE, WEIGHT_VALUE, DEST_PROPERTY
};

enum GraphDataIndex {
    OFFSET_INDEX, EDGE_INDEX, WEIGHT_INDEX, PROPERTY_INDEX, SRC_NODE_INDEX, UPDATES_SIZE_INDEX, SRC_PROPERTY_INDEX, DEST_NODE_INDEX, WEIGHT_VALUE_INDEX, DEST_PROPERTY_INDEX, WORKLIST_INDEX, NUMNODE_INDEX, WORKNODE_INDEX
};

enum ALGORITHM {
    ALG_SSSP, ALG_BFS, ALG_SSWP, ALG_CC
};

#define DEST_DATA_READY ((1 << DEST_NODE) | (1 << WEIGHT_VALUE) | (1 << DEST_PROPERTY))
#define DEST_DATA_READY_WITHOUT_WEIGHT ((1 << DEST_NODE) | (1 << DEST_PROPERTY))

#define SRC_DATA_READY ((1 << SRC_NODE | 1 << SRC_PROPERTY))
#define GRAPH_PREFETCHER_ELE_SIZE 8

struct DependencyData
{
    /* data */
    uint32_t parent;
    uint32_t value;
};

// struct to store information about the source and destination nodes, used by performance simulation
struct SrcInfo
{
    Address src;
    Address property;
    Address offsetStart;
    Address offsetEnd;
};

struct DestInfo
{
    Address edge;
    Address weight;
    Address property;
    bool valid;
};

// Record real data about the node, used by intrumentation
struct GraphSrcInfo
{
    uint64_t src_node; // source node to set
    DependencyData srcData;
    uint32_t offsetStart;
    uint32_t offsetEnd;
    uint32_t curOffset;
    SrcInfo srcInfo; // save the address
    int readyBits;
};

struct GraphDestInfo
{
    uint64_t dest_node; // dest node to get
    uint64_t weight_value; // weight value to get
    DependencyData destData;
    DestInfo destInfo; // save the address
    int readyBits;
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
    uintptr_t worklist;
    uint64_t numNodes;
    bool worklistSet;
    bool numNodesSet;
    uint32_t curSrcIndex;
    GraphSrcInfo graphSrcInfo;
    GraphDestInfo graphDestInfo;
    GraphPrefetcherUnit() {}
};

class PrefetcherTable{
    using EntryType = pair<bool, uint64_t>; // waiting bit (data response or CPU stall) and cycle
    private:
        uint32_t numEntries;
        g_vector<EntryType> buf;
        uint32_t idx;
    public:
        PrefetcherTable(uint32_t _numEntries) : numEntries(_numEntries)
        {
            buf.resize(numEntries);
            for (uint32_t i = 0; i < numEntries; i++) buf[i] = {false, 0};
            idx = 0;
        }

        inline uint64_t minAllocCycle(){
            return buf[idx].second;
        }

        inline bool stallType(){
            return buf[idx].first;
        }

        inline void markComplete(uint64_t cycle, bool stallType)
        {
            buf[idx++] = {stallType, cycle};
            if (idx == numEntries) idx = 0;
        }
};

struct lineReady{
    Address lineAddr;
    uint64_t readyCycle;
};


// GraphPrefetcher is a prefetcher specialized for graph processing.
class GraphPrefetcher : public BaseCache {

    private:
        uint64_t profPrefetch;
        uint64_t profRootFetch;
        uint64_t profDestFetch;
        uint64_t profConfig;
        uint64_t profTemporalBufferRead;
        uint64_t profTemporalBufferWrite;
        uint64_t ProfStallCycle; // Record how many request cycles stall because resources limited
        // Record how many cycles CPU wait for src/dest data
        uint64_t profSrcNodeWaitCycle;
        uint64_t profSrcPropertyWaitCycle;
        uint64_t profDestNodeWaitCycle;
        uint64_t profDestWeightWaitCycle;
        uint64_t profDestPropertyWaitCycle;

        MemObject* parent;
        uint32_t childId;
        uint32_t srcId; // should match the core
        g_string name;
        uint32_t reqFlags;

        lock_t lock;

        uint32_t nEntries;
        uint32_t latency;

        uint32_t nodeSize; // number of src node to req

        uint64_t worklistSetCycle;
        uint64_t lastAccessCycle; // used to control the request bandwidth

        // used to simulate the cache line locality
        lineReady lastWorklistLineAddr; // the last access address line of the worklist
        lineReady lastEdgeLineAddr; // the last access address line of the edge array
        lineReady lastWeightLineAddr; // the last access address line of the weight array

        struct SrcEntryInfo
        {
            uint64_t nodeAvailCycle; // the cycle of the node id is available
            uint64_t propertyAvailCycle; // record the cycle when the property is availCycle
            int readyBits; // record which srcInfo is ready to get
        };

        struct DestEntryInfo
        {
            int readyBits; // record which destInfo is ready to get
            // record the cycle when the data is ready
            uint64_t edgeReadyCycle;
            uint64_t weightReadyCycle;
            uint64_t propertyReadyCycle;
        };

        // record the response time of root and neighbor data
        g_deque<SrcEntryInfo> srcEntryInfo;
        g_deque<DestEntryInfo> destEntryInfo;
        uint64_t offsetAvailCycle; // record the cycle when the offset is availCycle

        // record the available time of both prefetch table
        PrefetcherTable srcTable;
        PrefetcherTable destTable;

    public:
        GraphPrefetcher(const g_string& _name, const uint32_t _nEntries, const uint32_t _nodeSize, const uint32_t _latency)
         : name(_name), nEntries(_nEntries), latency(_latency), nodeSize(_nodeSize),
           srcTable(nodeSize), destTable(nEntries){
            worklistSetCycle = 0;
            lastAccessCycle = 0;
            reqFlags = 0;
            srcId = -1;
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