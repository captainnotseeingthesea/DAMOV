#ifndef __MEM_CACHE_PREFETCH_STREAM_HH__
#define __MEM_CACHE_PREFETCH_STREAM_HH__

#include "g_std/g_string.h"
#include "g_std/g_deque.h"
#include "g_std/g_list.h"

#include <boost/compute/detail/lru_cache.hpp>

#include "base/sat_counter.h"
#include "associative_array_impl.h"
#include "replacement_policies/replaceable_entry.h"
#include "indexing_policies/set_associative.h"
#include "replacement_policies/lru_rp.h"
#include "prefetch/prefetcher.h"

struct StreamParams : PrefetcherParams
{
  uint32_t stream_depth;
    bool enable_auto_depth;
    bool enable_l3_stream_pre;
    uint32_t stream_entries;
    BaseIndexingPolicy *stream_indexing_policy;
    BaseReplPolicy *stream_replacement_policy;
    uint32_t pfFilterSize;
};

class Stream : public Prefetcher
{
  protected:
  uint32_t depth;
    uint32_t badPreNum;
    bool enableAutoDepth;
    bool enableL3StreamPre;
    const uint32_t l2Ratio = 2;
    const uint32_t l3Ratio = 3;
    const uint32_t DEPTHRIGHT = 1 << 9;
    const uint32_t DEPTHLEFT = 1;
    const uint32_t DEPTHSTEP = 1;
    const uint32_t BLOCKOFFST = 6;
    const uint32_t BITVECWIDTH = 128;
    const uint32_t REGIONBITS = 7;
    const uint32_t REGIONTAGOFFSET = 10;
    const uint32_t REGIONTAGNUM = 16;
    const uint32_t ACTIVETHRESHOLD = 12;
    const uint32_t VALIDITYCHECKINTERVAL = 1000;
    const double LATECOVERAGE = 0.4;
    const uint32_t LATEMISSTHRESHOLD = 200;
    const uint32_t LATEHITTHRESHOLD = 900;
    const uint32_t LOWMASK = 0x3ff;
    const uint32_t HIGHMASK = 0x7ff;
    const uint32_t VADDRHASHOFFSET = 5;
    const uint32_t VADDRHASHOFFSETMASK = 0x1f;


    Address tagAddress(Address a) { return a >> REGIONTAGOFFSET; };
    Address vaddrHash(Address a)
    {
        int low = a & VADDRHASHOFFSETMASK;
        int mid = (a >> VADDRHASHOFFSET) & VADDRHASHOFFSETMASK;
        int high = (a >> (2 * VADDRHASHOFFSET)) & VADDRHASHOFFSETMASK;
        return low ^ mid ^ high;
    }
    Address regionHashTag(Address a)
    {
        int low = a & LOWMASK;
        int high = vaddrHash(a >> REGIONTAGOFFSET);
        return high << REGIONTAGOFFSET | low;
    }
    Address tagOffset(Address a) { return (a / blkSize) % REGIONTAGNUM; };

    class STREAMEntry : public TaggedEntry
    {
      public:
        Address tag;
        Address bitVec;
        bool active;
        int cnt;
        bool decrMode;
        STREAMEntry() : TaggedEntry(), tag(0), bitVec(0), active(false), cnt(0), decrMode(false) {}
    };
    AssociativeArray<STREAMEntry> stream_array;
    STREAMEntry *streamLookup(const PrefetchInfo &pfi, bool &in_active_page, bool &decr);
    bool sendPFWithFilter(const PrefetchInfo &pfi, Address addr, g_vector<AddrPriority> &addresses, int prio);

  public:
    boost::compute::detail::lru_cache<Address, Address> *filter;
    const unsigned pfFilterSize;
    Stream(const g_string &_name, const StreamParams &p);
    void calculatePrefetch(const PrefetchInfo &pfi, uint64_t &respCycle, g_vector<AddrPriority> &addresses) override;

    static StreamParams buildParams(Config &config, const std::string &prefix)
    {
        StreamParams p;
        static_cast<PrefetcherParams &>(p) = Prefetcher::buildParams(config, prefix);

        p.stream_depth = config.get<uint32_t>(prefix + "stream_depth", 32);
        p.enable_auto_depth = config.get<bool>(prefix + "enable_auto_depth", false);
        p.enable_l3_stream_pre = config.get<bool>(prefix + "enable_l3_stream_pre", false);
        p.stream_entries = config.get<uint32_t>(prefix + "stream_entries", 16);
        p.stream_indexing_policy = new SetAssociative(p.stream_entries, p.stream_entries, 1);
        p.stream_replacement_policy = new LRU();
        p.pfFilterSize = config.get<uint32_t>(prefix + "pf_filter_size", 256);
        return p;
    };
};
#endif