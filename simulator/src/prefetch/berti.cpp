#include "berti.h"

Berti::HistoryTableEntry::HistoryTableEntry(uint32_t deltaTableSize) : 
    TaggedEntry(), pc(0), counter(0), hysteresis(false) {
    deltas.resize(deltaTableSize);
}

void Berti::HistoryTableEntry::resetConfidence(bool reset_status) {
    counter = 0;
    for (auto &info : deltas) {
        info.coverageCounter = 0;
        if (reset_status) {
            info.status = NO_PREF;
        }
    }
    if (reset_status) {
        bestDelta.delta = 0;
        bestDelta.status = NO_PREF;
    }
}

void Berti::HistoryTableEntry::updateStatus() {
    uint8_t max_cov = 0;
    for (auto &info : deltas) {
        // info.status = (info.coverageCounter >= 2) ? L2_PREF : NO_PREF;
        info.status = (info.coverageCounter >= 4) ? L1_PREF : NO_PREF;
        if (info.status != NO_PREF && info.coverageCounter > max_cov) {
            max_cov = info.coverageCounter;
            bestDelta = info;
        }
    }
    if (max_cov == 0) {
        bestDelta.delta = 0;
        bestDelta.status = NO_PREF;
    }
}

Berti::Berti(const g_string &_name, const BertiParams &p) : 
    Prefetcher(_name, p),
    maxAddrListSize(p.addrlist_size),
    maxDeltaListSize(p.deltalist_size),
    maxDeltafound(p.max_deltafound),
    historyTable(p.history_table_assoc, p.history_table_entries,
                p.history_table_indexing_policy, p.history_table_replacement_policy,
                HistoryTableEntry(maxDeltaListSize)),
    aggressivePF(p.aggressive_pf),
    useByteAddr(p.use_byte_addr),
    pfFilterSize(p.pf_filter_size) {
    filter = new boost::compute::detail::lru_cache<Address, Address>(pfFilterSize);
}

void Berti::calculatePrefetch(const PrefetchInfo &pfi, uint64_t &curCycle, 
                            g_vector<AddrPriority> &addresses) {
        
    if (!pfi.isCacheMiss()) {
        HistoryTableEntry *hist_entry = historyTable.findEntry(pcHash(pfi.getPC()));
        if (hist_entry) {
            searchTimelyDeltas(*hist_entry, 0, pfi.getReqCycle(),
                              useByteAddr ? pfi.getAddr() : blockIndex(pfi.getAddr()));
        }
    }
    
    auto entry = updateHistoryTable(pfi, curCycle);
    
    if (entry) {
        if (aggressivePF) {
            for (auto &delta_info : entry->deltas) {
                if (delta_info.status != NO_PREF) {
                    Address pf_addr = useByteAddr ? pfi.getAddr() + delta_info.delta
                                                : (blockIndex(pfi.getAddr()) + delta_info.delta) << lBlkSize;
                    sendPFWithFilter(pfi, pf_addr, addresses, 32,
                                   delta_info.delta == entry->bestDelta.delta && 
                                   entry->bestDelta.coverageCounter >= 8);
                }
            }
        } else {
            if (entry->bestDelta.status != NO_PREF) {
                Address pf_addr = useByteAddr ? pfi.getAddr() + entry->bestDelta.delta
                                            : (blockIndex(pfi.getAddr()) + entry->bestDelta.delta) << lBlkSize;
                sendPFWithFilter(pfi, pf_addr, addresses, 32,
                               entry->bestDelta.coverageCounter >= 8);
            }
        }
    }

    // print prefetch info
    for (int i = 0; i < addresses.size(); i++)
    {
        DBG("Generated prefetch %#lx, trigger address %#lx, pc %#lx", addresses[i].first, blockAddress(pfi.getAddr()), pfi.getPC());
    }
}

Berti::HistoryTableEntry* Berti::updateHistoryTable(const PrefetchInfo &pfi, uint64_t curCycle) {
    
    Address training_addr = useByteAddr ? pfi.getAddr() : blockIndex(pfi.getAddr());
    HistoryTableEntry *entry = historyTable.findEntry(pcHash(pfi.getPC()));
    HistoryInfo new_info = {training_addr, curCycle};
    
    if (entry) {
        historyTable.accessEntry(entry);
        
        bool found_addr_in_hist = std::find(entry->history.begin(), entry->history.end(), new_info) != entry->history.end();
        if (!found_addr_in_hist) {
            if (entry->history.size() >= maxAddrListSize) {
                entry->history.erase(entry->history.begin());
            }
            entry->history.push_back(new_info);
            entry->hysteresis = true;
            return entry;
        }
        return nullptr;
    } else {
        entry = historyTable.findVictim(pcHash(pfi.getPC()));
        if (entry->hysteresis) {
            entry->hysteresis = false;
            historyTable.insertEntry(pcHash(entry->pc), entry, false);
        } else {
            entry->pc = pfi.getPC();
            entry->history.clear();
            entry->history.push_back(new_info);
            historyTable.insertEntry(pcHash(pfi.getPC()), entry);
        }
    }
    return nullptr;
}

void Berti::searchTimelyDeltas(HistoryTableEntry &entry, uint64_t search_latency, 
                             uint64_t demand_cycle, Address trigger_addr) {
    std::list<int64_t> new_deltas;
    int delta_thres = useByteAddr ? blkSize : 1;
    
    for (auto it = entry.history.rbegin(); it != entry.history.rend(); it++) {
        int64_t delta = trigger_addr - it->vAddr;
        
        if (labs(delta) < delta_thres) continue;
        if (it->timestamp + search_latency >= demand_cycle) continue;
        
        new_deltas.push_back(delta);
        if (new_deltas.size() >= maxDeltafound) break;
    }
    
    entry.counter++;
    
    for (auto &delta : new_deltas) {
        bool miss = true;
        for (auto &delta_info : entry.deltas) {
            if (delta_info.coverageCounter != 0 && delta_info.delta == delta) {
                delta_info.coverageCounter++;
                miss = false;
                break;
            }
        }
        if (miss) {
            int replace_idx = 0;
            for (auto i = 0; i < entry.deltas.size(); i++) {
                if (entry.deltas[replace_idx].coverageCounter >= entry.deltas[i].coverageCounter) {
                    replace_idx = i;
                }
            }
            entry.deltas[replace_idx].delta = delta;
            entry.deltas[replace_idx].coverageCounter = 1;
            entry.deltas[replace_idx].status = NO_PREF;
        }
    }
    
    if (entry.counter >= 6) {
        entry.updateStatus();
        if (entry.counter >= 16) {
            entry.resetConfidence(false);
        }
    }
}

bool Berti::sendPFWithFilter(const PrefetchInfo &pfi, Address addr, 
                           g_vector<AddrPriority> &addresses, int prio,
                           bool using_best_delta_and_confident) {
    if (filter->contains(addr)) {
        return false;
    } else {
        filter->insert(addr, 0);
        addresses.push_back(AddrPriority(addr, prio));
        return true;
    }
}

void Berti::notifyFill(MemReq req, const uint64_t respCycle) {
    if(req.is(MemReq::IFETCH) || req.accessInfo.addr == 0 || req.accessInfo.pc == 0 || req.is(MemReq::PREFETCH)) {
        return;
    }

    // fill latency
    uint64_t miss_refill_search_lat = 0;

    HistoryTableEntry *entry = historyTable.findEntry(pcHash(req.accessInfo.pc));
    if(entry)
    {
        uint64_t demand_cycle = req.cycle;
        searchTimelyDeltas(*entry, miss_refill_search_lat, demand_cycle,
            useByteAddr ? req.accessInfo.addr : blockIndex(req.accessInfo.addr));
    }
}