#ifndef __MEM_CACHE_PREFETCH_BERTI_HH__
#define __MEM_CACHE_PREFETCH_BERTI_HH__

#include "g_std/g_string.h"
#include "g_std/g_vector.h"
#include "g_std/g_list.h"
#include "g_std/g_unordered_map.h"

#include <boost/compute/detail/lru_cache.hpp>
#include <unordered_map>

#include "base/sat_counter.h"
#include "associative_array_impl.h"
#include "replacement_policies/replaceable_entry.h"
#include "indexing_policies/set_associative.h"
#include "replacement_policies/lru_rp.h"
#include "prefetch/prefetcher.h"

struct BertiParams : PrefetcherParams
{
    uint32_t addrlist_size;
    uint32_t deltalist_size;
    uint32_t max_deltafound;
    uint32_t history_table_entries;
    uint32_t history_table_assoc;
    BaseIndexingPolicy *history_table_indexing_policy;
    BaseReplPolicy *history_table_replacement_policy;
    bool aggressive_pf;
    bool use_byte_addr;
    uint32_t pf_filter_size;
};

class Berti : public Prefetcher
{
  protected:
    uint32_t maxAddrListSize;
    uint32_t maxDeltaListSize;
    uint32_t maxDeltafound;

    uint32_t total_prefetches {0};

        
    struct HistoryInfo {
        Address vAddr{0};
        uint64_t timestamp{0};
        
        bool operator==(const HistoryInfo& rhs) const {
            return vAddr == rhs.vAddr;
        }
    };
    
    enum DeltaStatus { L1_PREF, L2_PREF, NO_PREF };
    
    struct DeltaInfo {
        uint8_t coverageCounter;
        int64_t delta;
        DeltaStatus status;
        
        DeltaInfo() : coverageCounter(0), delta(0), status(NO_PREF) {}
    };
    
    class HistoryTableEntry : public TaggedEntry {
      public:
        Address pc;
        g_list<HistoryInfo> history;
        g_vector<DeltaInfo> deltas;
        DeltaInfo bestDelta;
        uint8_t counter;
        bool hysteresis;
        
        HistoryTableEntry(uint32_t deltaTableSize);
        void resetConfidence(bool reset_status);
        void updateStatus();
    };
    
    AssociativeArray<HistoryTableEntry> historyTable;
    
    bool aggressivePF;
    bool useByteAddr;
    
    Address pcHash(Address pc) { return pc >> 1; }
    
    int64_t lastUsedBestDelta;
    int64_t evictedBestDelta;
    
    g_unordered_map<int64_t, uint64_t> topDeltas;
    g_unordered_map<int64_t, uint64_t> evictedDeltas;
    
    void searchTimelyDeltas(HistoryTableEntry &entry, uint64_t search_latency, 
                          uint64_t demand_cycle, Address trigger_addr);
    HistoryTableEntry* updateHistoryTable(const PrefetchInfo &pfi, uint64_t curCycle);
    bool sendPFWithFilter(const PrefetchInfo &pfi, Address addr, 
                         g_vector<AddrPriority> &addresses, int prio,
                        bool using_best_delta_and_confident);
    

  public:
    boost::compute::detail::lru_cache<Address, Address> *filter;
    const unsigned pfFilterSize;
    
    Berti(const g_string &_name, const BertiParams &p);
    void calculatePrefetch(const PrefetchInfo &pfi, uint64_t &curCycle, 
                          g_vector<AddrPriority> &addresses) override;

    void notifyFill(MemReq req, const uint64_t respCycle) override;
    
    static BertiParams buildParams(Config &config, const std::string &prefix) {
        BertiParams p;
        static_cast<PrefetcherParams &>(p) = Prefetcher::buildParams(config, prefix);
        
        p.addrlist_size = config.get<uint32_t>(prefix + "addrlist_size", 6);
        p.deltalist_size = config.get<uint32_t>(prefix + "deltalist_size", 4);
        p.max_deltafound = config.get<uint32_t>(prefix + "max_deltafound", 4);
        p.history_table_entries = config.get<uint32_t>(prefix + "history_table_entries", 64);
        p.history_table_assoc = config.get<uint32_t>(prefix + "history_table_assoc", 4);
        p.history_table_indexing_policy = new SetAssociative(p.history_table_assoc, p.history_table_entries, 1);
        p.history_table_replacement_policy = new LRU();
        p.aggressive_pf = config.get<bool>(prefix + "aggressive_pf", false);
        p.use_byte_addr = config.get<bool>(prefix + "use_byte_addr", false);
        p.pf_filter_size = config.get<uint32_t>(prefix + "pf_filter_size", 32);
        
        return p;
    };
};

#endif // __MEM_CACHE_PREFETCH_BERTI_HH__