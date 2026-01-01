#ifndef IPCP_PREFETCHER_H_
#define IPCP_PREFETCHER_H_

#include "g_std/g_string.h"
#include "stats.h"
#include "prefetch/prefetcher.h"
#include "associative_array_impl.h"
#include "replacement_policies/lru_rp.h"
#include "indexing_policies/set_associative.h"
#include "base/sat_counter.h"
#include "g_std/g_vector.h"
#include <boost/compute/detail/lru_cache.hpp>

struct IPCPPrefetcherParams : public PrefetcherParams
{
    unsigned gs_degree;
    unsigned cs_degree;
    unsigned cplx_degree;

    unsigned cs_thre;
    unsigned cplx_thre;

    unsigned ipt_entries;
    unsigned ipt_assoc;
    BaseIndexingPolicy *ipt_indexing_policy;
    BaseReplPolicy *ipt_replacement_policy;

    unsigned cspt_entries;

    unsigned rst_entries;
    unsigned rst_assoc;
    BaseIndexingPolicy *rst_indexing_policy;
    BaseReplPolicy *rst_replacement_policy;

    bool use_rrf;
    unsigned num_rr_entries;
    unsigned num_ip_tag_bits;
    unsigned num_page_tag_bits;
    unsigned num_lines_in_region;
};

class IPCP : public Prefetcher
{
public:
    enum PrefetchType
    {
        NO_PREFETCH,
        S_TYPE,    // stream
        CS_TYPE,   // constant stride
        CPLX_TYPE, // complex stride
        NL_TYPE    // next line
    };

private:
    // Configuration parameters
    const unsigned gs_degree;
    const unsigned cs_degree;
    const unsigned cplx_degree;
    const unsigned cs_thre;
    const unsigned cplx_thre;
    const unsigned signature_width;
    const unsigned num_page_tag_bits;
    const unsigned num_lines_in_region;
    const bool use_rrf;

    const int ipt_size;
    const int lipt_size;
    const int cspt_size;
    const int rst_size;

    // Constants
    const unsigned stride_mask = (1 << 8) - 1;
    const unsigned region_offset_mask;
    const unsigned max_pos_neg_count = 64;

    // stats
    uint32_t total_prefetches {0};

    /** IP Table Entry */
    struct IPTableEntry : public TaggedEntry
    {
        uint64_t last_vpage;
        uint64_t last_line_offset;
        int64_t last_stride;
        SatCounter8 conf;
        uint16_t signature;
        uint16_t str_dir;
        bool str_valid;
        PrefetchType pref_type;

        IPTableEntry(const SatCounter8 &init_confidence) : last_vpage(0), last_line_offset(0), last_stride(0), conf(init_confidence), signature(0), str_dir(0), str_valid(false), pref_type(NO_PREFETCH) {}
    };

    /** IP Table Hashing */
    class IPTableHashedSetAssociative : public SetAssociative
    {
    private:
        uint32_t num_ip_tag_bits;

    protected:
        uint32_t extractSet(const Address addr) const override;
        Address extractTag(const Address addr) const override;

    public:
        IPTableHashedSetAssociative(unsigned _assoc, unsigned _size, unsigned _entry_size, uint32_t _num_ip_tag_bits) : SetAssociative(_assoc, _size, _entry_size), num_ip_tag_bits(_num_ip_tag_bits) {}
        ~IPTableHashedSetAssociative() = default;
    };

    /** Constant Stride Prediction Table Entry */
    struct CSPTEntry
    {
        int stride;
        SatCounter8 conf;

        CSPTEntry(const SatCounter8 &init_confidence) : stride(0), conf(init_confidence) {}
    };

    /** Region Stream Table Entry */
    struct RSTEntry : public TaggedEntry
    {
        bool tentative_dense;
        bool trained_dense;
        SatCounter8 pos_neg_count;
        bool dir;
        g_vector<uint8_t> line_access;

        RSTEntry(const SatCounter8 &init_confidence, unsigned num_lines) : tentative_dense(false), trained_dense(false), pos_neg_count(init_confidence), dir(false), line_access(num_lines, 0) {}
    };

    /** Recent Request Filter Entry */
    boost::compute::detail::lru_cache<Address, Address> *rrf = nullptr;

    // Tables
    AssociativeArray<IPTableEntry> ipTable;
    g_vector<CSPTEntry> cspt;
    AssociativeArray<RSTEntry> rstable;

    int spec_nl;

    // Helper functions
    uint16_t update_sig(uint16_t old_sig, int delta);
    uint64_t hash_page(uint64_t addr);
    uint16_t get_ip_index(uint64_t ip);
    uint16_t get_ip_tag(uint64_t ip);

public:
    IPCP(const g_string &_name, const IPCPPrefetcherParams &p);
    ~IPCP() = default;

    void calculatePrefetch(const PrefetchInfo &pfi, uint64_t &respCycle,
                           g_vector<AddrPriority> &addresses) override;

    static IPCPPrefetcherParams buildParams(Config &config, const std::string &prefix)
    {
        IPCPPrefetcherParams p;
        static_cast<PrefetcherParams &>(p) = Prefetcher::buildParams(config, prefix);

        p.gs_degree = config.get<uint32_t>(prefix + "gs_degree", 6);
        p.cs_degree = config.get<uint32_t>(prefix + "cs_degree", 3);
        p.cplx_degree = config.get<uint32_t>(prefix + "cplx_degree", 3);

        p.cs_thre = config.get<uint32_t>(prefix + "cs_thre", 1);
        p.cplx_thre = config.get<uint32_t>(prefix + "cplx_thre", 0);

        p.ipt_entries = config.get<uint32_t>(prefix + "ipt_entries", 64);
        p.ipt_assoc = config.get<uint32_t>(prefix + "ipt_assoc", 1);
        p.ipt_replacement_policy = new LRU();

        p.cspt_entries = config.get<uint32_t>(prefix + "cspt_entries", 128);

        p.rst_entries = config.get<uint32_t>(prefix + "rst_entries", 8);
        p.rst_assoc = config.get<uint32_t>(prefix + "rst_assoc", 8);
        p.rst_indexing_policy = new SetAssociative(p.rst_assoc, p.rst_entries, 1);
        p.rst_replacement_policy = new LRU();

        p.use_rrf = config.get<bool>(prefix + "use_rrf", true);
        p.num_rr_entries = config.get<uint32_t>(prefix + "num_rr_entries", 32);
        p.num_ip_tag_bits = config.get<uint32_t>(prefix + "num_ip_tag_bits", 9);
        p.num_page_tag_bits = config.get<uint32_t>(prefix + "num_page_tag_bits", 12);
        p.num_lines_in_region = config.get<uint32_t>(prefix + "num_lines_in_region", 32);

        p.ipt_indexing_policy = new IPTableHashedSetAssociative(p.ipt_assoc, p.ipt_entries, 1, p.num_ip_tag_bits);

        return p;
    }
};

#endif // IPCP_PREFETCHER_H_
