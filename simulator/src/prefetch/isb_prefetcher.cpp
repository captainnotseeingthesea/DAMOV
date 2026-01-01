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

#include "isb_prefetcher.h"

#include "associative_array_impl.h"
#include "base/intmath.h"
#include "zsim.h"

ISBPrefetcher::ISBPrefetcher(const g_string &_name, const ISBPrefetcherParams &p)
    : Prefetcher(_name, p),
      chunkSize(p.chunk_size),
      prefetchCandidatesPerEntry(p.prefetch_candidates_per_entry),
      degree(p.degree),
      trainingUnit(p.training_unit_assoc, p.training_unit_entries,
                   p.training_unit_indexing_policy,
                   p.training_unit_replacement_policy),
      psAddressMappingCache(p.address_map_cache_assoc,
                            p.address_map_cache_entries,
                            p.ps_address_map_cache_indexing_policy,
                            p.ps_address_map_cache_replacement_policy,
                            AddressMappingEntry(prefetchCandidatesPerEntry,
                                                p.num_counter_bits)),
      spAddressMappingCache(p.address_map_cache_assoc,
                            p.address_map_cache_entries,
                            p.sp_address_map_cache_indexing_policy,
                            p.sp_address_map_cache_replacement_policy,
                            AddressMappingEntry(prefetchCandidatesPerEntry,
                                                p.num_counter_bits)),
      structuralAddressCounter(0)
{
    assert(isPowerOf2(prefetchCandidatesPerEntry));
}

void ISBPrefetcher::calculatePrefetch(const PrefetchInfo &pfi, uint64_t &respCycle, g_vector<AddrPriority> &addresses)
{
    // This prefetcher requires a PC
    Address pc = pfi.getPC();
    if (pc == 0)
    {
        return;
    }

    Address addr = blockIndex(pfi.getAddr());

    // Training, if the entry exists, then we found a correlation between
    // the entry lastAddress (named as correlated_addr_A) and the address of
    // the current access (named as correlated_addr_B)
    TrainingUnitEntry *entry = trainingUnit.findEntry(pc);
    bool correlated_addr_found = false;
    Address correlated_addr_A = 0;
    Address correlated_addr_B = 0;
    if (entry != nullptr)
    {
        trainingUnit.accessEntry(entry);
        correlated_addr_found = true;
        correlated_addr_A = entry->lastAddress;
        correlated_addr_B = addr;
    }
    else
    {
        entry = trainingUnit.findVictim(pc);
        assert(entry != nullptr);

        trainingUnit.insertEntry(pc, entry);
    }
    // Update the entry
    entry->lastAddress = addr;

    if (correlated_addr_found && correlated_addr_A != correlated_addr_B)
    {
        // If a correlation was found, update the Physical-to-Structural
        // table accordingly
        AddressMapping &mapping_A = getPSMapping(correlated_addr_A);
        AddressMapping &mapping_B = getPSMapping(correlated_addr_B);
        if (mapping_A.counter > 0 && mapping_B.counter > 0)
        {
            // Entry for A and B
            if (mapping_B.address == (mapping_A.address + 1))
            {
                mapping_B.counter++;
            }
            else
            {
                if (mapping_B.counter == 1)
                {
                    // Counter would hit 0, reassign address while keeping
                    // counter at 1
                    mapping_B.address = mapping_A.address + 1;
                    addStructuralToPhysicalEntry(mapping_B.address, correlated_addr_B);
                }
                else
                {
                    mapping_B.counter--;
                }
            }
        }
        else
        {
            if (mapping_A.counter == 0)
            {
                // if A is not valid, generate a new structural address
                mapping_A.counter++;
                mapping_A.address = structuralAddressCounter;
                structuralAddressCounter += chunkSize;
                addStructuralToPhysicalEntry(mapping_A.address, correlated_addr_A);
            }
            mapping_B.counter.reset();
            mapping_B.counter++;
            mapping_B.address = mapping_A.address + 1;
            // update SP-AMC
            addStructuralToPhysicalEntry(mapping_B.address, correlated_addr_B);
        }
    }

    // Use the PS mapping to predict future accesses using the current address
    // - Look for the structured address
    // - if it exists, use it to generate prefetches for the subsequent
    //   addresses in ascending order, as many as indicated by the degree
    //   (given the structured address S, prefetch S+1, S+2, .. up to S+degree)
    Address amc_address = addr / prefetchCandidatesPerEntry;
    Address map_index = addr % prefetchCandidatesPerEntry;
    AddressMappingEntry *ps_am = psAddressMappingCache.findEntry(amc_address);
    if (ps_am != nullptr)
    {
        AddressMapping &mapping = ps_am->mappings[map_index];
        if (mapping.counter > 0)
        {
            Address sp_address = mapping.address / prefetchCandidatesPerEntry;
            Address sp_index = mapping.address % prefetchCandidatesPerEntry;
            AddressMappingEntry *sp_am =
                spAddressMappingCache.findEntry(sp_address);
            if (sp_am == nullptr)
            {
                // The entry has been evicted, can not generate prefetches
                return;
            }
            for (unsigned d = 1;
                 d <= degree && (sp_index + d) < prefetchCandidatesPerEntry;
                 d += 1)
            {
                AddressMapping &spm = sp_am->mappings[sp_index + d];
                // generate prefetch
                if (spm.counter > 0)
                {
                    Address pf_addr = spm.address << lBlkSize;
                    addresses.push_back(AddrPriority(pf_addr, 0));
                    DBG("Generated prefetch: %lx", pf_addr);
                }
            }
        }
    }
}

ISBPrefetcher::AddressMapping &
ISBPrefetcher::getPSMapping(Address paddr)
{
    Address amc_address = paddr / prefetchCandidatesPerEntry;
    Address map_index = paddr % prefetchCandidatesPerEntry;
    AddressMappingEntry *ps_entry =
        psAddressMappingCache.findEntry(amc_address);
    if (ps_entry != nullptr)
    {
        // A PS-AMC line already exists
        psAddressMappingCache.accessEntry(ps_entry);
    }
    else
    {
        ps_entry = psAddressMappingCache.findVictim(amc_address);
        assert(ps_entry != nullptr);

        psAddressMappingCache.insertEntry(amc_address, ps_entry);
    }
    return ps_entry->mappings[map_index];
}

void ISBPrefetcher::addStructuralToPhysicalEntry(
    Address structural_address, Address physical_address)
{
    Address amc_address = structural_address / prefetchCandidatesPerEntry;
    Address map_index = structural_address % prefetchCandidatesPerEntry;
    AddressMappingEntry *sp_entry =
        spAddressMappingCache.findEntry(amc_address);
    if (sp_entry != nullptr)
    {
        spAddressMappingCache.accessEntry(sp_entry);
    }
    else
    {
        sp_entry = spAddressMappingCache.findVictim(amc_address);
        assert(sp_entry != nullptr);

        spAddressMappingCache.insertEntry(amc_address, sp_entry);
    }
    AddressMapping &mapping = sp_entry->mappings[map_index];
    mapping.address = physical_address;
    mapping.counter.reset();
    mapping.counter++;
}