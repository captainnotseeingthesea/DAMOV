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

#ifndef __PREFETCHER_H_
#define __PREFETCHER_H_

#include "bithacks.h"
#include "g_std/g_string.h"
#include "memory_hierarchy.h"
#include "cache.h"
#include "config.h"

// #define DBG(args...) info(args)
#define DBG(args...)

/**
 * Class containing the information needed by the prefetch to train and
 * generate new prefetch requests.
 */
class PrefetchInfo
{
    /** The address used to train and generate prefetches */
    Address address;
    /** The program counter that generated this address. */
    /** Physical address, needed because address can be virtual */
    Address paddress;
    Address pc;
    /** The request size in bytes */
    uint32_t size;
    /** The requestor ID that generated this address. should match the core*/
    uint32_t requestorId;
    /** The child ID of the cache. */
    uint32_t childId;
    /** Whether this event comes from a write request */
    bool write;
    /** Whether this event comes from a cache miss */
    bool cacheMiss;

    /** Request cycle for the request */
    uint64_t reqCycle;

    bool everPrefetched{false};

    bool pfFirstHit{false};

    bool pfHit{false};

    bool storePFTrain{false};

public:
    /**
     * Obtains the address value of this Prefetcher address.
     * @return the addres value.
     */
    Address getAddr() const
    {
        return address;
    }

    /**
     * Gets the physical address of the request
     * @return physical address of the request
     */
    Address getPaddr() const
    {
        return paddress;
    }

    /**
     * Returns the program counter that generated this request.
     * @return the pc value
     */
    Address getPC() const
    {
        return pc;
    }

    /**
     * Returns the size of the request in bytes
     * @return the size of the request in bytes
     */
    uint32_t getSize() const
    {
        return size;
    }

    /**
     * Gets the requestor ID that generated this address
     * @return the requestor ID that generated this address
     */
    uint32_t getRequestorId() const
    {
        return requestorId;
    }

    /**
     * Gets the child ID belonging to the cache
     * @return the child ID
     */
    uint32_t getChildId() const
    {
        return childId;
    }

    uint64_t getReqCycle() const
    {
        return reqCycle;
    }

    /**
     * Checks if the request that caused this prefetch event was a write
     * request come from committed store inst
     * @return true if the request causing this event is a write request
     */
    bool isWrite() const
    {
        return write;
    }

    // is come from store prefetch train trigger
    bool isStore() const
    {
        return storePFTrain;
    }

    /**
     * Check if this event comes from a cache miss
     * @result true if this event comes from a cache miss
     */
    bool isCacheMiss() const
    {
        return cacheMiss;
    }

    /**
     * Gets the associated data of the request triggering the event
     * @param Byte ordering of the stored data
     * @return the data
     */
    template <typename T>
    inline T get() const
    {
        if (address == 0)
        {
            panic("PrefetchInfo::get called with a request with no data.");
        }
        return *(T *)address;
    }

    /**
     * Check for equality
     * @param pfi PrefetchInfo to compare against
     * @return True if this object and the provided one are equal
     */
    bool sameAddr(PrefetchInfo const &pfi) const
    {
        return this->getAddr() == pfi.getAddr();
    }

    bool sameAddr(Address address) const
    {
        return this->getAddr() == address;
    }

    bool isEverPrefetched() const { return everPrefetched; }

    void setEverPrefetched(bool prefetched) { everPrefetched = prefetched; }

    bool isPfHit() const { return pfHit; }

    void setPfHit(bool hit) { pfHit = hit; }

    bool isPfFirstHit() const { return pfFirstHit; }

    void setPfFirstHit(bool hit) { pfFirstHit = hit; }

    void setStorePftrain(bool s) { storePFTrain = s; }

    /**
     * Constructs a PrefetchInfo using a PacketPtr.
     * @param pkt PacketPtr used to generate the PrefetchInfo
     * @param miss whether this event comes from a cache miss
     */
    PrefetchInfo(MemReq req, bool miss);
    /**
     * Constructs a PrefetchInfo using a new address value and
     * another PrefetchInfo as a reference.
     * @param pfi PrefetchInfo used to generate this new object
     * @param addr the address value of the new object
     */
    PrefetchInfo(PrefetchInfo const &pfi, Address addr);

    PrefetchInfo(const Address &addr, const Address &paddr, const Address &pc,
        uint64_t reqCycle, uint32_t reqSize, uint32_t reqId, uint32_t cId,
        bool isWrite, bool isCacheMiss);

    ~PrefetchInfo() = default;

    bool lastPfLate{false};
};

struct PrefetcherParams
{
    /** The block size of the parent cache. */
    unsigned blkSize;

    /** The page size in bytes. */
    Address pageSize;

    /** Only consult prefetcher on cache misses? */
    bool on_miss;

    /** Consult prefetcher on reads? */
    bool on_read;

    /** Consult prefetcher on writes? */
    bool on_write;

    /** Consult prefetcher on data accesses? */
    bool on_data;

    /** Consult prefetcher on instruction accesses? */
    bool on_inst;

    /** Prefetch on every access, not just misses */
    bool prefetch_on_access;

    /** Prefetch on hit on prefetched lines */
    bool prefetch_on_pf_hit;

    
};

using AddrPriority = std::pair<Address, int32_t>;
using RequestorID = uint32_t;

class Prefetcher : public GlobAlloc
{
protected:
    Counter profAccesses, profPrefetches, profPageHits, profHits;
    g_string name;
    /** Pointr to the parent cache. */
    Cache *cache = nullptr;

    /** The block size of the parent cache. */
    unsigned blkSize;

    /** log_2(block size of the parent cache). */
    unsigned lBlkSize;

    /** Request id for prefetches */
    const RequestorID requestorId;

    const Address pageSize;

    unsigned lPageSize;

    /** Only consult prefetcher on cache misses? */
    const bool onMiss;

    /** Consult prefetcher on reads? */
    const bool onRead;

    /** Consult prefetcher on reads? */
    const bool onWrite;

    /** Consult prefetcher on data accesses? */
    const bool onData;

    /** Consult prefetcher on instruction accesses? */
    const bool onInst;

    /** Prefetch on every access, not just misses */
    const bool prefetchOnAccess;

    /** Prefetch on hit on prefetched lines */
    const bool prefetchOnPfHit;

    struct StatGroup
    {

    } prefetchStats;

    /** Total prefetches issued */
    uint64_t issuedPrefetches;
    /** Total prefetches that has been useful */
    uint64_t usefulPrefetches;

    /** Determine if address is in cache */
    bool inCache(Address addr) const;

    bool hasBeenPrefetched(Address addr) const;
    bool hasEverBeenPrefetched(Address addr) const;

    /** Determine if addresses are on the same page */
    bool samePage(Address a, Address b) const;
    /** Determine the index of the page the block in */
    Address pageIndex(Address a) const;
    /** Determine the address of the block in which a lays */
    Address blockAddress(Address a) const;
    /** Determine the address of a at block granularity */
    Address blockIndex(Address a) const;
    /** Determine the address of the page in which a lays */
    Address pageAddress(Address a) const;
    /** Determine the page-offset of a  */
    Address pageOffset(Address a) const;
    /** Build the address of the i-th block inside the page */
    Address pageIthBlockAddress(Address page, uint32_t i) const;

public:
    Prefetcher(const g_string &_name, const PrefetcherParams &p);

    ~Prefetcher() = default;

    void setCache(Cache * _cache)
    {
        cache = _cache;
    }

    // virtual void initStats(AggregateStat *parentStat) = 0;

    const char *getName() { return name.c_str(); }

    virtual void calculatePrefetch(const PrefetchInfo &pfi, uint64_t &respCycle, g_vector<AddrPriority> &addresses) = 0;

    virtual void notifyFill(MemReq req, const uint64_t respCycle) {};

    /**
     * Determine if this access should be observed
     * @param req The memory request causing the event
     * @param miss whether this event comes from a cache miss
     */
    bool observeAccess(const Address addr, const uint32_t flags, const AccessType type, bool miss) const;

    static PrefetcherParams buildParams(
        Config &config,
        const std::string &prefix)
    {
        PrefetcherParams p;
        p.blkSize = config.get<uint32_t>(prefix + "blk_size", 64);
        p.pageSize = config.get<uint32_t>(prefix + "page_bytes", 4096);
        p.on_miss = config.get<bool>(prefix + "on_miss", false);
        p.on_read = config.get<bool>(prefix + "on_read", true);
        p.on_write = config.get<bool>(prefix + "on_write", false);
        p.on_data = config.get<bool>(prefix + "on_data", true);
        p.on_inst = config.get<bool>(prefix + "on_inst", false);
        p.prefetch_on_access = config.get<bool>(prefix + "prefetch_on_access", false);
        p.prefetch_on_pf_hit = config.get<bool>(prefix + "prefetch_on_pf_hit", true);
        return p;
    }
};

#endif // __PREFETCHER_H_
