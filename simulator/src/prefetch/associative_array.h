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

#ifndef __ASSOCIATIVE_ARRAY_H__
#define __ASSOCIATIVE_ARRAY_H__

#include "galloc.h"
#include "replacement_policies/base.h"
#include "memory_hierarchy.h"
#include "indexing_policies/base.h"
#include "g_std/g_vector.h"
#include "replacement_policies/tagged_entry.h"

template <class Entry>
class AssociativeArray : public GlobAlloc
{
    static_assert(std::is_base_of<TaggedEntry, Entry>::value,
                  "Entry must derive from TaggedEntry");

    /** Associativity of the container */
    const int associativity;
    /**
     * Total number of entries, entries are organized in sets of the provided
     * associativity. The number of associative sets is obtained by dividing
     * numEntries by associativity.
     */
    const int numEntries;
    /** Pointer to the indexing policy */
    BaseIndexingPolicy *const indexingPolicy;
    /** Pointer to the replacement policy */
    BaseReplPolicy *const replacementPolicy;
    /** Vector containing the entries of the container */
    g_vector<Entry> entries;

public:
    /**
     * Public constructor
     * @param assoc number of elements in each associative set
     * @param num_entries total number of entries of the container, the number
     *   of sets can be calculated dividing this balue by the 'assoc' value
     * @param idx_policy indexing policy
     * @param rpl_policy replacement policy
     * @param init_val initial value of the elements of the set
     */
    AssociativeArray(int assoc, int num_entries, BaseIndexingPolicy *idx_policy,
                     BaseReplPolicy *rpl_policy, Entry const &init_val = Entry());

    AssociativeArray(AssociativeArray &&other) noexcept
        : associativity(other.associativity),
          numEntries(other.numEntries),
          indexingPolicy(other.indexingPolicy),
          replacementPolicy(other.replacementPolicy),
          entries(std::move(other.entries))
    {}

    ~AssociativeArray() = default;

    /**
     * Find an entry within the set
     * @param addr key element
     * @return returns a pointer to the wanted entry or nullptr if it does not
     *  exist.
     */
    Entry *findEntry(Address addr) const;

    /**
     * Do an access to the entry, this is required to
     * update the replacement information data.
     * @param entry the accessed entry
     */
    void accessEntry(Entry *entry);

    /**
     * Find a victim to be replaced
     * @param addr key to select the possible victim
     * @result entry to be victimized
     */
    Entry *findVictim(Address addr);

    /**
     * Indicate that an entry has just been inserted
     * @param addr key of the container
     * @param entry pointer to the container entry to be inserted
     * @param reset indicates if the replacement data should be reset, for example, place to the MRU position
     */
    void insertEntry(Address addr, Entry *entry, bool with_reset = true);

    /**
     * Invalidate an entry and its respective replacement data.
     *
     * @param entry Entry to be invalidated.
     */
    void invalidate(Entry *entry);

    /** Iterator types */
    using const_iterator = typename g_vector<Entry>::const_iterator;
    using iterator = typename g_vector<Entry>::iterator;

    /**
     * Returns an iterator to the first entry of the dictionary
     * @result iterator to the first element
     */
    iterator begin()
    {
        return entries.begin();
    }

    /**
     * Returns an iterator pointing to the end of the the dictionary
     * (placeholder element, should not be accessed)
     * @result iterator to the end element
     */
    iterator end()
    {
        return entries.end();
    }

    /**
     * Returns an iterator to the first entry of the dictionary
     * @result iterator to the first element
     */
    const_iterator begin() const
    {
        return entries.begin();
    }

    /**
     * Returns an iterator pointing to the end of the the dictionary
     * (placeholder element, should not be accessed)
     * @result iterator to the end element
     */
    const_iterator end() const
    {
        return entries.end();
    }
};

#endif