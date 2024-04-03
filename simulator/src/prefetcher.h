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

#ifndef PREFETCHER_H_
#define PREFETCHER_H_

#include <bitset>
#include "bithacks.h"
#include "g_std/g_string.h"
#include "memory_hierarchy.h"
#include "stats.h"
#include "timing_event.h"

enum { PF_INCOMING_REQ, PF_NEXT_REQ, PF_STRIDE_REQ , PF_EVENT_REQ, PF_RECORDS} ;

class StreamPrefetcherEvent : public TimingEvent {

	private:
		TimingRecord theRecords[PF_RECORDS];
		EventRecorder *evRec;

	public:
		// bound call
		uint64_t onGoingCycle;

		//weave call
		uint64_t simCycle;
		uint64_t lastEventCycle;
		uint64_t startEventcycle;


		void chainEvents(TimingRecord& toplevel ){

			if ( (theRecords[PF_INCOMING_REQ].startEvent )  &&
				  (theRecords[PF_INCOMING_REQ].endEvent ) 	) {

				if ( (theRecords[PF_NEXT_REQ].startEvent )  ) {
					if (  theRecords[PF_NEXT_REQ].startEvent == theRecords[PF_NEXT_REQ].endEvent ) {
						  theRecords[PF_INCOMING_REQ].startEvent->addChild(theRecords[PF_NEXT_REQ].startEvent,evRec);
					} else { 
						theRecords[PF_INCOMING_REQ].startEvent->addChild(theRecords[PF_NEXT_REQ].startEvent, evRec); 
						if ( theRecords[PF_NEXT_REQ].endEvent)
							theRecords[PF_INCOMING_REQ].endEvent->addChild(theRecords[PF_NEXT_REQ].endEvent,evRec); 
					}
				}
			   toplevel.startEvent  = theRecords[PF_INCOMING_REQ].startEvent;
			   toplevel.endEvent = theRecords[PF_INCOMING_REQ].endEvent;
		   }

	  }

	void coupleEvents (TimingRecord &record ){ 
		if ( onGoingCycle < record.reqCycle)   {
				startEventcycle = record.reqCycle;
				setMinStartCycle(startEventcycle);
		}

		if ( record.startEvent == record.endEvent ) {
			addChild(record.startEvent, evRec);
		} else {
			addChild(record.startEvent, evRec);
			addChild(record.endEvent, evRec);
		}
	}

	void setAccessRecord(TimingRecord& accRec, uint64_t thisCycle) {
		theRecords[PF_INCOMING_REQ] = accRec;
		coupleEvents(accRec);
	}

	void setNextFetchRecord(TimingRecord &fetchRec, uint64_t thisCycle){
		theRecords[PF_NEXT_REQ] = fetchRec;
		coupleEvents(fetchRec);
	}
		
	void setStrideFetchRecord(TimingRecord &strideRec, uint64_t thisCycle){
		theRecords[PF_STRIDE_REQ] = strideRec;
		coupleEvents(strideRec);
	}

	StreamPrefetcherEvent( int32_t domain, uint64_t startMinCycle, EventRecorder *_evRec) :
	  TimingEvent(0,0,domain),
	  evRec(_evRec)
    {
			onGoingCycle = startMinCycle ;
			startEventcycle = onGoingCycle;
			setMinStartCycle(startMinCycle);
			theRecords[PF_INCOMING_REQ].clear();
			theRecords[PF_NEXT_REQ].clear();
			theRecords[PF_STRIDE_REQ].clear();
	 }

	void simulate ( uint64_t startCycle) {
			simCycle =  startCycle;
			done(startCycle);
	}

	virtual std::string str(){
		std::string res  = "PFe: ";
		return res;
	}
};

/* Prefetcher models: Basic operation is to interpose between cache levels, issue additional accesses,
 * and keep a small table with delays; when the demand access comes, we do it and account for the
 * latency as when it was first fetched (to avoid hit latencies on partial latency overlaps).
 */

template <int32_t M, int32_t T, int32_t I>  // max value, threshold, initial
class SatCounter {
    private:
        int32_t count;
    public:
        SatCounter() : count(I) {}
        void reset() { count = I; }
        void dec() { count = MAX(count - 1, 0); }
        void inc() { count = MIN(count + 1, M); }
        bool pred() const { return count >= T; }
        uint32_t counter() const { return count; }
};

/* This is basically a souped-up version of the DLP L2 prefetcher in Nehalem: 16 stream buffers,
 * but (a) no up/down distinction, and (b) strided operation based on dominant stride detection
 * to try to subsume as much of the L1 IP/strided prefetcher as possible.
 *
 * FIXME: For now, mostly hardcoded; 64-line entries (4KB w/64-byte lines), fixed granularities, etc.
 * TODO: Adapt to use weave models
 */
class StreamPrefetcher : public BaseCache {
    private:
        struct Entry {
            // Two competing strides; at most one active
            int32_t stride;
            SatCounter<3, 2, 1> conf;

            struct AccessTimes {
                uint64_t startCycle;  // FIXME: Dead for now, we should use it for profiling
                uint64_t respCycle;

                void fill(uint32_t s, uint64_t r) { startCycle = s; respCycle = r; }
            };
	
            AccessTimes times[64];
            std::bitset<64> valid;

            uint32_t lastPos;
            uint32_t lastLastPos;
            uint32_t lastPrefetchPos;
            uint64_t lastCycle;  // updated on alloc and hit
            uint64_t ts;

            void alloc(uint64_t curCycle) {
                stride = 1;
                lastPos = 0;
                lastLastPos = 0;
                lastPrefetchPos = 0;
                conf.reset();
                valid.reset();
                lastCycle = curCycle;
            }
        };

        uint64_t timestamp;  // for LRU
        uint32_t pfEntries; // define the size of the tag and array size;

        Address *tag;
        Entry *array;

        Counter profAccesses, profPrefetches, profDoublePrefetches, profPageHits, profHits, profShortHits, profStrideSwitches, profLowConfAccs;

        MemObject* parent;
        BaseCache* child;
        uint32_t childId;
        g_string name;

	    void createArray( void ){ 
            //tag = (Address *) malloc (sizeof(Address) * pfEntries);
            //array = (Entry *) malloc (sizeof(Entry) * pfEntries);
            tag = gm_calloc<Address>(pfEntries);
            array = gm_calloc<Entry>(pfEntries);
        }

    public:
        explicit StreamPrefetcher(const g_string& _name, const int nlines, const uint32_t _nEntries) : 
               timestamp(0), pfEntries(_nEntries), name(_name) {
               createArray();
		  }

        ~StreamPrefetcher ( void ) {
            gm_free(tag);
            gm_free(array);    
        }

        void initStats(AggregateStat* parentStat);
        const char* getName() { return name.c_str();}
        void setParents(uint32_t _childId, const g_vector<MemObject*>& parents, Network* network);
        void setChildren(const g_vector<BaseCache*>& children, Network* network);

        uint64_t access(MemReq& req);
        uint64_t invalidate(const InvReq& req);
};

#endif  // PREFETCHER_H_
