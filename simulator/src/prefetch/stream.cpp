#include "stream.h"

#include "associative_array_impl.h"
#include "base/intmath.h"
#include "zsim.h"
#include "log.h"

Stream::Stream(const g_string &_name, const StreamParams &p)
    : Prefetcher(_name, p),
      depth(p.stream_depth),
      badPreNum(0),
      enableAutoDepth(p.enable_auto_depth),
      enableL3StreamPre(p.enable_l3_stream_pre),
      stream_array(p.stream_entries, p.stream_entries, p.stream_indexing_policy,
                   p.stream_replacement_policy, STREAMEntry()),
      pfFilterSize(p.pfFilterSize)
{
    filter = new boost::compute::detail::lru_cache<Address, Address>(pfFilterSize);
}
void Stream::calculatePrefetch(const PrefetchInfo &pfi, uint64_t &respCycle, g_vector<AddrPriority> &addresses)
{
    Address vaddr = pfi.getAddr();
    Address block_addr = blockAddress(vaddr);
    bool in_active_page = false;
    bool decr = false;

    if (pfi.isCacheMiss() && (filter->contains(block_addr)))
    {
        badPreNum++;
    }
    STREAMEntry *entry = streamLookup(pfi, in_active_page, decr);
    /** TODO: add the adaptive adjustment of the depth */
    // if ((issuedPrefetches >= VALIDITYCHECKINTERVAL) && (enableAutoDepth)) {
    //     if ((double)late_num / issuedPrefetches >= LATECOVERAGE) {
    //         if (depth != DEPTHRIGHT)
    //             depth = depth << DEPTHSTEP;
    //     }
    //     if (badPreNum > LATEMISSTHRESHOLD) {
    //         badPreNum = 0;
    //         if (depth != DEPTHLEFT) {
    //             depth = depth >> DEPTHSTEP;
    //         }
    //     }
    //     issuedPrefetches = 0;
    // }

    if (in_active_page)
    {
        Address pf_stream_l1 = decr ? block_addr - depth * blkSize : block_addr + depth * blkSize;
        sendPFWithFilter(pfi, pf_stream_l1, addresses, 1);
    }
}

Stream::STREAMEntry *
Stream::streamLookup(const PrefetchInfo &pfi, bool &in_active_page, bool &decr)
{
    Address vaddr = pfi.getAddr();
    Address vaddr_tag_num = tagAddress(vaddr);
    Address vaddr_offset = tagOffset(vaddr);

    STREAMEntry *entry = stream_array.findEntry(regionHashTag(vaddr_tag_num));
    STREAMEntry *entry_plus = stream_array.findEntry(regionHashTag(vaddr_tag_num + 1));
    STREAMEntry *entry_min = stream_array.findEntry(regionHashTag(vaddr_tag_num - 1));

    if (entry)
    {
        stream_array.accessEntry(entry);
        uint64_t region_bit_accessed = 1UL << vaddr_offset;
        if (entry_plus)
            entry->decrMode = true;
        if ((entry_plus || entry_min) || (entry->cnt > ACTIVETHRESHOLD))
            entry->active = true;
        in_active_page = entry->active;
        decr = entry->decrMode;
        if (!(entry->bitVec & region_bit_accessed))
        {
            entry->cnt += 1;
        }
        return entry;
    }
    entry = stream_array.findVictim(0);

    in_active_page = (entry_plus || entry_min);
    decr = entry_plus != nullptr;
    entry->tag = regionHashTag(vaddr_tag_num);
    entry->decrMode = entry_plus != nullptr;
    entry->bitVec = 1UL << vaddr_offset;
    entry->cnt = 1;
    entry->active = (entry_plus != nullptr) || (entry_min != nullptr);
    stream_array.insertEntry(regionHashTag(vaddr_tag_num), entry);
    return entry;
}
bool Stream::sendPFWithFilter(const PrefetchInfo &pfi, Address addr, g_vector<AddrPriority> &addresses, int prio)
{
    if (filter->contains(addr))
    {
        return false;
    }
    else
    {
        DBG("Generated prefetch %#lx, trigger address %#lx", addr, pfi.getAddr());
        filter->insert(addr, 0);
        addresses.push_back(AddrPriority(addr, prio));
        return true;
    }
}