/*
 * Copyright (c) 2018 Inria
 * Copyright (c) 2012-2013, 2015 ARM Limited
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Copyright (c) 2005 The Regents of The University of Michigan
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
 * @file
 * Describes a strided prefetcher.
 */

#ifndef __PREFETCH_STRIDE_HH__
#define __PREFETCH_STRIDE_HH__

#include "g_std/g_string.h"
#include "g_std/g_unordered_map.h"
#include "g_std/g_vector.h"

#include <boost/compute/detail/lru_cache.hpp>

#include "base/sat_counter.h"
#include "associative_array_impl.h"
#include "replacement_policies/replaceable_entry.h"
#include "indexing_policies/set_associative.h"
#include "replacement_policies/lru_rp.h"
#include "prefetch/prefetcher.h"

class BaseIndexingPolicy;

/**
 * Override the default set associative to apply a specific hash function
 * when extracting a set.
 */
class StridePrefetcherHashedSetAssociative : public SetAssociative
{
protected:
  uint32_t extractSet(const Address addr) const override;
  Address extractTag(const Address addr) const override;

public:
  StridePrefetcherHashedSetAssociative(unsigned _assoc, unsigned _size, unsigned _entry_size)
      : SetAssociative(_assoc, _size, _entry_size)
  {
  }
  ~StridePrefetcherHashedSetAssociative() = default;
};

struct StridePrefetcherParams : public PrefetcherParams
{
  unsigned confidence_counter_bits;
  unsigned initial_confidence;
  unsigned confidence_threshold;
  bool use_requestor_id;
  uint32_t degree;
  uint32_t table_assoc;
  uint32_t table_entries;
  BaseIndexingPolicy *table_indexing_policy;
  BaseReplPolicy *table_replacement_policy;
  bool use_cacheline_addr;
  uint32_t distance;
};


class StridePrefetcher : public Prefetcher
{
protected:
  /** Initial confidence counter value for the pc tables. */
  const SatCounter8 initConfidence;

  /** Confidence threshold for prefetch generation. */
  const double threshConf;

  const bool useRequestorId;

  /** The number of prefetches to generate. */
  const uint32_t degree;

    /** How far ahead of the demand stream to start prefetching.
   *
   * Skip this number of strides ahead of the first identified
   * prefetch, then generate `degree` prefetches at `stride`
   * intervals. A value of zero indicates no skip.
   */
  const uint32_t distance;

  /**
   * Information used to create a new PC table. All of them behave equally.
   */
  const struct PCTableInfo
  {
    const uint32_t assoc;
    const uint32_t numEntries;

    BaseIndexingPolicy *const indexingPolicy;
    BaseReplPolicy *const replacementPolicy;

    PCTableInfo(uint32_t assoc, uint32_t num_entries,
                BaseIndexingPolicy *indexing_policy,
                BaseReplPolicy *repl_policy)
        : assoc(assoc), numEntries(num_entries),
          indexingPolicy(indexing_policy), replacementPolicy(repl_policy)
    {
    }
  } pcTableInfo;

  /** Tagged by hashed PCs. */
  struct StrideEntry : public TaggedEntry
  {
    StrideEntry(const SatCounter8 &init_confidence);

    void invalidate() override;

    Address lastAddr;
    int stride;
    SatCounter8 confidence;
  };

  typedef AssociativeArray<StrideEntry> PCTable;
  g_unordered_map<uint32_t, PCTable> pcTables;

  /**
     * If this parameter is set to true, then the prefetcher will operate at
     * the granularity of cache line. Otherwise it would operate on the
     * granularity of word addresses
     */
    const bool useCachelineAddr;

  /**
   * Try to find a table of entries for the given context. If none is
   * found, a new table is created.
   *
   * @param context The context to be searched for.
   * @return The table corresponding to the given context.
   */
  PCTable *findTable(uint32_t context);

  /**
   * Create a PC table for the given context.
   *
   * @param context The context of the new PC table.
   * @return The new PC table
   */
  PCTable *allocateNewContext(uint32_t context);

public:
  StridePrefetcher(const g_string &_name, const StridePrefetcherParams &p);
  ~StridePrefetcher();
  void calculatePrefetch(const PrefetchInfo &pfi, uint64_t &respCycle, g_vector<AddrPriority> &addresses) override;

  static StridePrefetcherParams buildParams(Config &config, const std::string &prefix) {
    StridePrefetcherParams p;
    static_cast<PrefetcherParams&>(p) = Prefetcher::buildParams(config, prefix);

    p.confidence_counter_bits = config.get<uint32_t>(prefix + "counter_bits", 3);
    p.initial_confidence = config.get<uint32_t>(prefix + "initial_confidence", 4);
    p.confidence_threshold = config.get<uint32_t>(prefix + "threshold", 50);
    p.use_requestor_id = config.get<bool>(prefix + "use_requestor_id", true);
    p.use_cacheline_addr = config.get<bool>(prefix + "use_cacheline_addr", false);
    p.degree = config.get<uint32_t>(prefix + "degree", 4);
    p.distance = config.get<uint32_t>(prefix + "distance", 0);
    p.table_assoc = config.get<uint32_t>(prefix + "assoc", 4);
    p.table_entries = config.get<uint32_t>(prefix + "entries", 64);
    p.table_indexing_policy = new StridePrefetcherHashedSetAssociative(p.table_assoc, p.table_entries, 1);
    p.table_replacement_policy = new LRU();
    return p;
}

private:
  const unsigned filterSize{32};
  boost::compute::detail::lru_cache<Address, Address> blockLRUFilter;
};

#endif // __PREFETCH_STRIDE_HH__
