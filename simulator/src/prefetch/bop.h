/**
 * Copyright (c) 2018 Metempsy Technology Consulting
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * Implementation of the 'A Best-Offset Prefetcher'
 * Reference:
 *   Michaud, P. (2015, June). A best-offset prefetcher.
 *   In 2nd Data Prefetching Championship.
 */

#ifndef __MEM_CACHE_PREFETCH_BOP_HH__
#define __MEM_CACHE_PREFETCH_BOP_HH__

#include "g_std/g_string.h"
#include "g_std/g_deque.h"

#include <boost/compute/detail/lru_cache.hpp>

#include "base/sat_counter.h"
#include "associative_array_impl.h"
#include "replacement_policies/replaceable_entry.h"
#include "indexing_policies/set_associative.h"
#include "replacement_policies/lru_rp.h"
#include "prefetch/prefetcher.h"

struct BOPPrefetcherParams : PrefetcherParams
{
    /** Learning phase parameters */
    unsigned int score_max;
    unsigned int round_max;
    unsigned int bad_score;
    /** Recent requests table parameteres */
    unsigned int rr_size;
    unsigned int tag_bits;
    bool negative_offsets_enable;

    /** List of offsets to test */
    g_vector<int16_t> offsets;

    uint32_t pf_filter_size;
};

class BOP : public Prefetcher
{
private:
    enum RRWay
    {
        Left,
        Right
    };

    /** Learning phase parameters */
    const unsigned int scoreMax;
    const unsigned int roundMax;
    const unsigned int badScore;
    /** Recent requests table parameteres */
    const unsigned int rrEntries;
    const unsigned int tagMask;

    struct rrEntry
    {
        Address tag;
        uint64_t readyTick;
        rrEntry() : tag(0), readyTick(0){}
        /**
         * Constructor for the rrEntry structure
         * @param t: tag of the entry
         * @param r: ready tick of the entry
         */
        rrEntry(Address t, uint64_t r) : tag(t), readyTick(r){}
    };

    g_vector<rrEntry> rrLeft;
    g_vector<rrEntry> rrRight;

    /** Structure to save the offset and the score */
    typedef std::pair<int16_t, uint8_t> OffsetListEntry;
    g_vector<OffsetListEntry> offsetsList;

    unsigned maxOffsetCount{32};

    /** Hardware prefetcher enabled */
    bool issuePrefetchRequests;
    /** Current best offset to issue prefetches */
    int64_t bestOffset;
    /** Current best offset found in the learning phase */
    int64_t phaseBestOffset;
    /** Current test offset index */
    g_vector<OffsetListEntry>::iterator offsetsListIterator;

    /** Max score found so far */
    unsigned int bestScore;
    /** Current round */
    unsigned int round;

    /** Generate a hash for the specified address to index the RR table
     *  @param addr: address to hash
     *  @param way:  RR table to which is addressed (left/right)
     */
    unsigned int hash(Address addr, unsigned int way) const;

    /** Insert the specified address into the RR table
     *  @param addr: address to insert
     *  @param entry: The entry to insert in the RR table
     *  @param way: RR table to which the address will be inserted
     */
    void insertIntoRR(Address addr, rrEntry entry, unsigned int way);

    /** Reset all the scores from the offset list */
    void resetScores();

    /** Generate the tag for the specified address based on the tag bits
     *  and the block size
     *  @param addr: address to get the tag from
     */
    Address tag(Address addr) const;

    /** Test if @X-O is hitting in the RR table to update the
        offset score */
    bool testRR(Address, uint64_t, int16_t) const;

    /** Learning phase of the BOP. Update the intermediate values of the
        round and update the best offset if found */
    void bestOffsetLearning(Address, uint64_t);

public:
    boost::compute::detail::lru_cache<Address, Address> *filter;

    BOP(const g_string &_name, const BOPPrefetcherParams &p);
    ~BOP() = default;

    void calculatePrefetch(const PrefetchInfo &pfi, uint64_t &respCycle, g_vector<AddrPriority> &addresses) override;

    bool sendPFWithFilter(const PrefetchInfo &pfi, Address addr, g_vector<AddrPriority> &addresses, int prio);

    void notifyFill(MemReq req, const uint64_t respCycle) override;

    static BOPPrefetcherParams buildParams(
        Config &config,
        const std::string &prefix)
    {
        BOPPrefetcherParams p;
        static_cast<PrefetcherParams &>(p) = Prefetcher::buildParams(config, prefix);
        /** Learning phase parameters */
        p.score_max = config.get<unsigned int>(prefix + "max_score", 20);
        p.round_max = config.get<unsigned int>(prefix + "max_round", 50);
        p.bad_score = config.get<unsigned int>(prefix + "bad_score", 12);
        /** Recent requests table parameters */
        p.rr_size = config.get<unsigned int>(prefix + "rr_size", 256);
        p.tag_bits = config.get<unsigned int>(prefix + "tag_bits", 24);
        p.negative_offsets_enable = config.get<bool>(prefix + "negative_offsets_enable", true);
        p.pf_filter_size = config.get<uint32_t>(prefix + "pf_filter_size", 32);

        std::string offsets = config.get<const char *>(prefix + "offsets", "1 2 3 4 5 6 8 9 10 12 15 16");
        std::stringstream ss(offsets);
        std::string item;
        while (std::getline(ss, item, ' ')) {
            p.offsets.push_back(std::stoi(item));
        }
        return p;
    }
};

#endif