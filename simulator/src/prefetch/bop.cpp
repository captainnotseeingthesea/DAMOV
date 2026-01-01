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

#include "bop.h"

#include "associative_array_impl.h"
#include "base/intmath.h"
#include "zsim.h"
#include "log.h"

BOP::BOP(const g_string &_name, const BOPPrefetcherParams &p)
    : Prefetcher(_name, p),
      scoreMax(p.score_max), roundMax(p.round_max),
      badScore(p.bad_score), rrEntries(p.rr_size),
      tagMask((1 << p.tag_bits) - 1),
      issuePrefetchRequests(false), bestOffset(1), phaseBestOffset(0),
      bestScore(0), round(0)
{
    if (!isPowerOf2(rrEntries)) {
        panic("%s: number of RR entries is not power of 2\n", _name);
    }
    if (!isPowerOf2(blkSize)) {
        panic("%s: cache line size is not power of 2\n", _name);
    }

    rrLeft.resize(rrEntries);
    rrRight.resize(rrEntries);

    int offset_count = p.offsets.size();
    maxOffsetCount = p.negative_offsets_enable ? 2*p.offsets.size() : p.offsets.size();


    for (int i = 0; i < offset_count; i++) {
        offsetsList.emplace_back(p.offsets[i], (uint8_t) 0);
        if (p.negative_offsets_enable) {
            offsetsList.emplace_back(-p.offsets[i], (uint8_t) 0);
        }
    }

    bestOffset = offsetsList.back().first;

    offsetsListIterator = offsetsList.begin();

    filter = new boost::compute::detail::lru_cache<Address, Address>(p.pf_filter_size);
}

unsigned int
BOP::hash(Address addr, unsigned int way) const
{
    Address hash1 = addr >> way;
    Address hash2 = hash1 >> floorLog2(rrEntries);
    return (hash1 ^ hash2) & (Address)(rrEntries - 1);
}

void
BOP::insertIntoRR(Address addr, rrEntry entry, unsigned int way)
{
    int index;
    switch (way) {
        case RRWay::Left:
            index = hash(addr, RRWay::Left);
            rrLeft[index] = entry;
            break;
        case RRWay::Right:
            index = hash(addr, RRWay::Right);
            rrRight[index] = entry;
            break;
    }
}

void
BOP::resetScores()
{
    for (auto& it : offsetsList) {
        it.second = 0;
    }
}

inline Address
BOP::tag(Address addr) const
{
    return (addr >> lBlkSize) & tagMask;
}

bool
BOP::testRR(Address addr, uint64_t reqCyle, int16_t offset) const
{
    unsigned int index = hash(addr, RRWay::Left);
    if (rrLeft[index].tag == addr) {
        return (rrLeft[index].readyTick <= reqCyle);
    }

    index = hash(addr, RRWay::Right);
    if (rrRight[index].tag == addr) {
        return (rrRight[index].readyTick <= reqCyle);
    }

    return false;
}

void
BOP::bestOffsetLearning(Address addr, uint64_t reqCycle)
{
    int16_t offset = (*offsetsListIterator).first;

    Address lookup_addr = addr - offset;

    // There was a hit in the RR table, increment the score for this offset
    if (testRR(lookup_addr, reqCycle, offset)) {
        // DBG("Address %#lx found in the RR table\n", lookup_addr);
        (*offsetsListIterator).second++;
        if ((*offsetsListIterator).second > bestScore) {
            bestScore = (*offsetsListIterator).second;
            phaseBestOffset = (*offsetsListIterator).first;
        }
    }

    // Move the offset iterator forward to prepare for the next time
    offsetsListIterator++;

    /*
     * All the offsets in the list were visited meaning that a learning
     * phase finished. Check if
     */
    if (offsetsListIterator == offsetsList.end()) {
        offsetsListIterator = offsetsList.begin();
        round++;

        // Check if its time to re-calculate the best offset
        if ((bestScore >= scoreMax) || (round == roundMax)) {
            round = 0;

            /*
            * If the current best score (bestScore) has exceed the threshold to
            * enable prefetching (badScore), reset the learning structures and
            * enable prefetch generation
            */
            if (bestScore > badScore) {
                issuePrefetchRequests = true;
            } else {
                issuePrefetchRequests = false;
            }
            bestOffset = phaseBestOffset;
            round = 0;
            bestScore = 0;
            phaseBestOffset = 0;
            resetScores();
        }
        else if ((round >= roundMax/2) && (bestOffset != phaseBestOffset) && (bestScore <= badScore)) {
            issuePrefetchRequests = false;
        }
    }
}

void
BOP::calculatePrefetch(const PrefetchInfo &pfi, uint64_t &respCycle, g_vector<AddrPriority> &addresses)
{
    Address addr = blockAddress(pfi.getAddr());
    Address tag_x = tag(addr);
    uint64_t reqCycle = pfi.getReqCycle();

    // DBG("Train prefetcher with addr %#lx tag %#lx\n", addr, tag_x);

    insertIntoRR(tag_x, {tag_x, respCycle}, RRWay::Left);

    /*
     * Go through the nth offset and update the score, the best score and the
     * current best offset if a better one is found
     */
    bestOffsetLearning(tag_x, reqCycle);

    // This prefetcher is a degree 1 prefetch, so it will only generate one
    // prefetch at most per access
    if (issuePrefetchRequests) {
        Address prefetch_addr = addr + (bestOffset << lBlkSize);
        sendPFWithFilter(pfi, prefetch_addr, addresses, 32);
    }
}

bool
BOP::sendPFWithFilter(const PrefetchInfo &pfi, Address addr, g_vector<AddrPriority> &addresses, int prio)
{
    if(filter->contains(addr)) {
        return false;
    } else {
        filter->insert(addr, addr);
        addresses.push_back(AddrPriority(addr, prio));
        DBG("Generated prefetch %#lx, trigger address %#lx, pc %#lx", addr, blockAddress(pfi.getAddr()), pfi.getPC());
        return true;
    }
}

void
BOP::notifyFill(MemReq req, const uint64_t respCycle)
{
    if(req.flags & MemReq::PREFETCH)
    {
        Address tag_y = tag(req.accessInfo.addr);

        if (issuePrefetchRequests) {
            insertIntoRR(req.accessInfo.addr, {tag_y - bestOffset, respCycle}, RRWay::Right);
        }
    }
}