#include "ipcp.h"

uint32_t IPCP::IPTableHashedSetAssociative::extractSet(Address addr) const
{
    return (addr >> 1) & (numSets - 1);
}

Address IPCP::IPTableHashedSetAssociative::extractTag(Address addr) const
{
    return (addr >> tagShift) & ((1 << num_ip_tag_bits) - 1);
}

IPCP::IPCP(const g_string &_name, const IPCPPrefetcherParams &p)
    : Prefetcher(_name, p),
      gs_degree(p.gs_degree),
      cs_degree(p.cs_degree),
      cplx_degree(p.cplx_degree),
      cs_thre(p.cs_thre),
      cplx_thre(p.cplx_thre),
      signature_width(floorLog2(p.cspt_entries)),
      num_page_tag_bits(p.num_page_tag_bits),
      num_lines_in_region(p.num_lines_in_region),
      use_rrf(p.use_rrf),
      ipTable(p.ipt_assoc, p.ipt_entries, p.ipt_indexing_policy,
              p.ipt_replacement_policy, IPTableEntry(SatCounter8(2, 0))),
      cspt(p.cspt_entries, CSPTEntry(SatCounter8(2, 0))),
      rstable(p.rst_assoc, p.rst_entries, p.rst_indexing_policy,
              p.rst_replacement_policy, RSTEntry(SatCounter8(6, max_pos_neg_count >> 1), p.num_lines_in_region)),
      ipt_size(p.ipt_entries),
      lipt_size(floorLog2(p.ipt_entries)),
      cspt_size(p.cspt_entries),
      rst_size(p.rst_entries),
      region_offset_mask(num_lines_in_region - 1),
      spec_nl(0)
{
    if (use_rrf)
    {
        rrf = new boost::compute::detail::lru_cache<Address, Address>(p.num_rr_entries);
    }
}

void IPCP::calculatePrefetch(const PrefetchInfo &pfi, uint64_t &respCycle,
                             g_vector<AddrPriority> &addresses)
{
    // This prefetcher requires a PC
    Address pc = pfi.getPC();
    if (pc == 0 || pfi.isWrite())
    {
        return;
    }

    Address addr = blockAddress(pfi.getAddr());

    // Calculate current page and line info
    uint64_t curr_page = hash_page(pageIndex(addr));
    uint64_t line_addr = blockIndex(addr);
    uint64_t line_offset = line_addr & 0x3F;
    uint64_t num_prefs = 0;

    // TODO : update spec nl bit when num misses crosses certain threshold

    // TODO : Updating prefetch degree based on accuracy

    // Lookup IP in IP table
    IPTableEntry *ip_entry = ipTable.findEntry(pc);
    if (ip_entry == nullptr)
    {
        // Miss in the IP table
        IPTableEntry *ip_entry = ipTable.findVictim(pc);

        // insert new entry, initialize new entry
        ip_entry->last_vpage = curr_page;
        ip_entry->last_line_offset = line_offset;
        ip_entry->last_stride = 0;
        ip_entry->conf.reset();
        ip_entry->signature = 0;
        ip_entry->str_valid = false;
        ip_entry->str_dir = 0;
        ip_entry->pref_type = NO_PREFETCH;
        ipTable.insertEntry(pc, ip_entry);
        return;
    }
    ipTable.accessEntry(ip_entry);

    // calculate the stride between the current cache line offset and the last cache line offset
    int64_t stride = 0;
    if (line_offset > ip_entry->last_line_offset)
    {
        stride = line_offset - ip_entry->last_line_offset;
    }
    else
    {
        stride = ip_entry->last_line_offset - line_offset;
        stride *= -1;
    }

    // don't do anything if same address is seen twice in a row
    if (stride == 0)
    {
        return;
    }

    int c = 0, flag = 0;

    // Checking if IP is already classified as a part of the GS class, so that for the new region we will set the tentative (spec_dense) bit.
    uint64_t last_region_id = (ip_entry->last_vpage << 1) | (ip_entry->last_line_offset >> 5);
    RSTEntry *last_rst_entry = rstable.findEntry(last_region_id);
    if (last_rst_entry != nullptr)
    {
        rstable.accessEntry(last_rst_entry);
        if (last_rst_entry->trained_dense == 1)
        {
            flag = 1;
        }
    }

    // Update RST (Region Stream Table)
    uint64_t region_id = (curr_page << 1) | (line_offset >> 5);
    RSTEntry *rst_entry = rstable.findEntry(region_id);

    if (rst_entry == nullptr)
    {
        // Allocate new RST entry
        rst_entry = rstable.findVictim(region_id);
        if (flag == 1)
        {
            rst_entry->tentative_dense = true;
        }
        else
        {
            rst_entry->tentative_dense = false;
        }
        rst_entry->trained_dense = false;
        rst_entry->dir = 0;
        rst_entry->pos_neg_count.reset();
        std::fill(rst_entry->line_access.begin(), rst_entry->line_access.end(), 0);
        rstable.insertEntry(region_id, rst_entry);
    }
    else
    {
        rstable.accessEntry(rst_entry);
        if (rst_entry->line_access[line_offset & region_offset_mask] == 0)
        {
            rst_entry->line_access[line_offset & region_offset_mask] = 1;
        }
        rst_entry->pos_neg_count += (stride > 0) ? 1 : -1;

        if (!rst_entry->trained_dense)
        {
            int count = 0;
            for (unsigned i = 0; i < num_lines_in_region; i++)
            {
                if (rst_entry->line_access[i] == 1)
                {
                    count++;
                }
            }
            if (count > (num_lines_in_region * 3 / 4))
            { // 75% of cache lines accessed
                rst_entry->trained_dense = true;
            }
        }
        if (flag == 1)
        {
            rst_entry->tentative_dense = true;
        }
        if (rst_entry->tentative_dense || rst_entry->trained_dense)
        {
            rst_entry->dir = rst_entry->pos_neg_count.calcSaturation() > 0.5 ? 1 : 0;
            ip_entry->str_valid = true;
            ip_entry->str_dir = rst_entry->dir;
        }
        else
        {
            ip_entry->str_valid = false;
        }
    }

    // page boundary learning
    if (curr_page != ip_entry->last_vpage)
    {
        if (stride < 0)
        {
            stride += num_lines_in_region;
        }
        else
        {
            stride -= num_lines_in_region;
        }
    }

    // Update CS confidence
    ip_entry->conf += (stride == ip_entry->last_stride) ? 1 : -1;
    if (ip_entry->conf.isZero())
    {
        ip_entry->last_stride = stride;
    }

    // Update CPLX signature and confidence
    uint16_t last_signature = ip_entry->signature;

    cspt[last_signature].conf += (stride == cspt[last_signature].stride) ? 1 : -1;
    if (cspt[last_signature].conf.isZero())
    {
        cspt[last_signature].stride = stride;
    }

    // Update signature
    ip_entry->signature = update_sig(last_signature, stride);

    // GS class prefetches
    if (ip_entry->str_valid)
    {
        if (gs_degree < 3)
        {
            flag = 1;
        }
        for (int i = 0; i < gs_degree; i++)
        {
            uint64_t pf_address;
            if (ip_entry->str_dir == 1)
            { // +ve stream
                pf_address = (line_addr + i + 1) << lBlkSize;
            }
            else
            { // -ve stream
                pf_address = (line_addr - i - 1) << lBlkSize;
            }

            // Check page boundary
            if (!samePage(pf_address, addr))
            {
                break;
            }
            // Add prefetch
            ip_entry->pref_type = S_TYPE;

            if (use_rrf)
            {
                assert(rrf);
                if (!rrf->contains(pf_address))
                {
                    rrf->insert(pf_address, 0);
                    addresses.push_back(AddrPriority(pf_address, 1));
                }
            }
            else
            {
                addresses.push_back(AddrPriority(pf_address, 1));
            }
            num_prefs++;
        }
    }
    else
    {
        flag = 1;
    }

    // CS class prefetches
    if (ip_entry->conf.rawCounter() > cs_thre && ip_entry->last_stride != 0 && flag == 1)
    {
        if (cs_degree < 2)
        {
            flag = 1;
        }
        else
        {
            flag = 0;
        }
        for (int i = 0; i < cs_degree; i++)
        {
            uint64_t pf_address = (line_addr + (ip_entry->last_stride * (i + 1))) << lBlkSize;

            // Check if prefetch address is in same 4 KB page
            if (!samePage(pf_address, addr))
            {
                break;
            }
            // Add prefetch
            ip_entry->pref_type = CS_TYPE;

            if (use_rrf)
            {
                assert(rrf);
                if (!rrf->contains(pf_address))
                {
                    rrf->insert(pf_address, 0);
                    addresses.push_back(AddrPriority(pf_address, 2));
                }
            }
            else
            {
                addresses.push_back(AddrPriority(pf_address, 2));
            }
            num_prefs++;
        }
    }

    // CPLX class prefetches
    if (cspt[last_signature].stride != 0 && flag == 1)
    {
        uint16_t signature = ip_entry->signature;
        int pref_offset = 0;

        for (int i = 0; i < cplx_degree; i++)
        {
            pref_offset += cspt[last_signature].stride;
            uint64_t pf_address = ((line_addr + pref_offset) << lBlkSize);

            // Check if prefetch address is in same 4 KB page
            if (!samePage(pf_address, addr))
            {
                break;
            }

            // Add prefetch
            ip_entry->pref_type = CPLX_TYPE;
            if (cspt[last_signature].conf.rawCounter() > cplx_thre)
            {
                if (use_rrf)
                {
                    assert(rrf);
                    if (!rrf->contains(pf_address))
                    {
                        rrf->insert(pf_address, 0);
                        addresses.push_back(AddrPriority(pf_address, 3));
                    }
                }
                else
                {
                    addresses.push_back(AddrPriority(pf_address, 3));
                }
                num_prefs++;
            }
            // Update signature for next iteration
            signature = update_sig(signature, cspt[last_signature].stride);
        }
    }

    // NL class prefetch if nothing else was issued
    if (num_prefs == 0 && spec_nl == 1)
    {
        uint64_t pf_address = ((addr >> lBlkSize) + 1) << lBlkSize;

        // Check page boundary
        if (samePage(pf_address, addr))
        {
            ip_entry->pref_type = NL_TYPE;

            if (use_rrf)
            {
                assert(rrf);
                if (!rrf->contains(pf_address))
                {
                    rrf->insert(pf_address, 0);
                    addresses.push_back(AddrPriority(pf_address, 4));
                }
            }
            else
            {
                addresses.push_back(AddrPriority(pf_address, 4));
            }
        }
    }

    // Update IP entry with current access info
    ip_entry->last_line_offset = line_offset;
    ip_entry->last_vpage = curr_page;

    // print prefetch info
    for (int i = 0; i < addresses.size(); i++)
    {
        DBG("Prefetching address: %lx with priority %d, triggered by: %lx", addresses[i].first, addresses[i].second, pageIndex(pfi.getAddr()));
    }
    // total_prefetches += addresses.size();
    // DBG("prefetches num: %d", total_prefetches);
}

uint16_t IPCP::update_sig(uint16_t old_sig, int delta)
{
    uint16_t new_sig = 0;
    int sig_delta = (delta < 0) ? (((-1) * delta) + (1 << 6)) : delta;
    new_sig = ((old_sig << 1) ^ sig_delta) & ((1 << signature_width) - 1);
    return new_sig;
}

uint64_t IPCP::hash_page(uint64_t addr)
{
    uint64_t hash = 0;
    while (addr != 0)
    {
        hash = hash ^ addr;
        addr = addr >> 6;
    }
    return hash & ((1 << num_page_tag_bits) - 1);
}