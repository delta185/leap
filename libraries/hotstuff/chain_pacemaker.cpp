#include <eosio/hotstuff/chain_pacemaker.hpp>
#include <iostream>

// comment this out to remove the core profiler
#define HS_CORE_PROFILER

namespace eosio { namespace hotstuff {

// ======================== Core profiling instrumentation =========================
#ifdef HS_CORE_PROFILER
      std::mutex        csc_mutex;
      bool              csc_started = false;
      fc::microseconds  csc_total;            // total time spent by all net threads waiting on the core lock
      fc::time_point    csc_first_time;       // first time the core has received a request
      fc::time_point    csc_last_report_time; // last time a core timing report was printed to the log
      int64_t           csc_reqs;             // total number of times the core has been entered by a net thread
      struct reqstat {      // per-core-request-type stats
         fc::microseconds   total_us;  // total time spent in this request type
         fc::microseconds   max_us;    // maximum time ever spent inside a request of this type
         int64_t            count = 0; // total requests of this type made
      };
      std::map<std::string, reqstat> reqs;
      class csc {
      public:
         fc::time_point start;       // time lock request was made
         fc::time_point start_core;  // time the core has been entered
         std::string name;
         csc(const std::string & entrypoint_name) :
            start(fc::time_point::now()), name(entrypoint_name) { }
         void core_in() {
            start_core = fc::time_point::now();
            std::lock_guard g( csc_mutex );
            ++csc_reqs;   // update total core requests
            csc_total += start_core - start; // update total core synchronization contention time
            if (! csc_started) { // one-time initialization
               csc_started = true;
               csc_first_time = start_core;
               csc_last_report_time = start_core;
            }
         }
         void core_out() {
            fc::time_point end = fc::time_point::now();
            std::lock_guard g( csc_mutex );

            // update per-request metrics
            {
               auto it = reqs.find(name);
               if (it == reqs.end()) {
                  reqs.insert({name, reqstat()});
                  it = reqs.find(name);
               }
               reqstat &req = it->second;
               ++req.count;
               fc::microseconds exectime = end - start_core;
               req.total_us += exectime;
               if (exectime > req.max_us) {
                  req.max_us = exectime;
               }
            }

            // emit full report every 10s
            fc::microseconds elapsed = end - csc_last_report_time;
            if (elapsed.count() > 10000000) { // 10-second intervals to print the report
               fc::microseconds total_us = end - csc_first_time; // total testing walltime so far since 1st request seen
               int64_t total_secs = total_us.count() / 1000000; // never zero if report interval large enough
               int64_t avgs = csc_total.count() / total_secs;
               int64_t avgr = csc_total.count() / csc_reqs;
               // core contention report
               ilog("HS-CORE: csc_total_us:${tot} csc_elapsed_s:${secs} csc_avg_us_per_s:${avgs} csc_reqs:${reqs} csc_avg_us_per_req:${avgr}", ("tot", csc_total)("secs",total_secs)("avgs", avgs)("reqs", csc_reqs)("avgr", avgr));
               fc::microseconds req_total_us; // will compute global stats for all request types
               fc::microseconds req_max_us;
               int64_t          req_count = 0;
               auto it = reqs.begin();
               while (it != reqs.end()) {
                  const std::string & req_name = it->first;
                  reqstat &req = it->second;
                  int64_t avgr = req.total_us.count() / it->second.count;
                  // per-request-type performance report
                  ilog("HS-CORE: ${rn}_total_us:${tot} ${rn}_max_us:${max} ${rn}_reqs:${reqs} ${rn}_avg_us_per_req:${avgr}", ("rn",req_name)("tot", req.total_us)("max",req.max_us)("reqs", req.count)("avgr", avgr));
                  req_total_us += req.total_us;
                  if (req_max_us < req.max_us) {
                     req_max_us = req.max_us;
                  }
                  req_count += req.count;
                  ++it;
               }
               // combined performance report
               int64_t req_avgr = req_total_us.count() / req_count;
               ilog("HS-CORE: total_us:${tot} max_us:${max} reqs:${reqs} avg_us_per_req:${avgr}", ("tot", req_total_us)("max",req_max_us)("reqs", req_count)("avgr", req_avgr));
               csc_last_report_time = end;
            }
         }
      };
#else
      struct csc {  // dummy profiler
         csc(const string & s) { }
         void core_in() { }
         void core_out() { }
      }
#endif
//===============================================================================================

   chain_pacemaker::chain_pacemaker(controller* chain, std::set<account_name> my_producers, fc::logger& logger)
      : _chain(chain),
        _qc_chain("default"_n, this, std::move(my_producers), logger),
        _logger(logger)
   {
      _accepted_block_connection = chain->accepted_block.connect( [this]( const block_state_ptr& blk ) {
         on_accepted_block( blk );
      } );
   }

   void chain_pacemaker::get_state(finalizer_state& fs) const {
      // lock-free state version check
      uint64_t current_state_version = _qc_chain.get_state_version();
      if (_state_cache_version != current_state_version) {
         finalizer_state current_state;
         {
            csc prof("stat");
            std::lock_guard g( _hotstuff_global_mutex ); // lock IF engine to read state
            prof.core_in();
            current_state_version = _qc_chain.get_state_version(); // get potentially fresher version
            if (_state_cache_version != current_state_version) 
               _qc_chain.get_state(current_state);
            prof.core_out();
         }
         if (_state_cache_version != current_state_version) {
            std::unique_lock ul(_state_cache_mutex); // lock cache for writing
            _state_cache = current_state;
            _state_cache_version = current_state_version;
         }
      }

      std::shared_lock sl(_state_cache_mutex); // lock cache for reading
      fs = _state_cache;
   }

   name chain_pacemaker::debug_leader_remap(name n) {
/*
      // FIXME/REMOVE: simple device to test proposer/leader
      //   separation using the net code.
      // Given the name of who's going to be the proposer
      //   (which is the head block's producer), we swap the
      //   leader name here for someone else.
      // This depends on your configuration files for the
      //   various nodeos instances you are using to test,
      //   specifically the producer names associated with
      //   each nodeos instance.
      // This works for a setup with 21 producer names
      //   interleaved between two nodeos test instances.
      //   i.e. nodeos #1 has bpa, bpc, bpe ...
      //        nodeos #2 has bpb, bpd, bpf ...
      if (n == "bpa"_n) {
         n = "bpb"_n;
      } else if (n == "bpb"_n) {
         n = "bpa"_n;
      } else if (n == "bpc"_n) {
         n = "bpd"_n;
      } else if (n == "bpd"_n) {
         n = "bpc"_n;
      } else if (n == "bpe"_n) {
         n = "bpf"_n;
      } else if (n == "bpf"_n) {
         n = "bpe"_n;
      } else if (n == "bpg"_n) {
         n = "bph"_n;
      } else if (n == "bph"_n) {
         n = "bpg"_n;
      } else if (n == "bpi"_n) {
         n = "bpj"_n;
      } else if (n == "bpj"_n) {
         n = "bpi"_n;
      } else if (n == "bpk"_n) {
         n = "bpl"_n;
      } else if (n == "bpl"_n) {
         n = "bpk"_n;
      } else if (n == "bpm"_n) {
         n = "bpn"_n;
      } else if (n == "bpn"_n) {
         n = "bpm"_n;
      } else if (n == "bpo"_n) {
         n = "bpp"_n;
      } else if (n == "bpp"_n) {
         n = "bpo"_n;
      } else if (n == "bpq"_n) {
         n = "bpr"_n;
      } else if (n == "bpr"_n) {
         n = "bpq"_n;
      } else if (n == "bps"_n) {
         n = "bpt"_n;
      } else if (n == "bpt"_n) {
         n = "bps"_n;
      } else if (n == "bpu"_n) {
         // odd one out; can be whomever that is not in the same nodeos (it does not
         //   actually matter; we just want to make sure we are stressing the system by
         //   never allowing the proposer and leader to fall on the same nodeos instance).
         n = "bpt"_n;
      }
*/
      return n;
   }

   // called from main thread
   void chain_pacemaker::on_accepted_block( const block_state_ptr& blk ) {
      std::scoped_lock g( _chain_state_mutex );
      _head_block_state = blk;
      _finalizer_set = _chain->get_finalizers(); // TODO get from chainbase or from block_state
   }

   name chain_pacemaker::get_proposer() {
      std::scoped_lock g( _chain_state_mutex );
      return _head_block_state->header.producer;
   }

   name chain_pacemaker::get_leader() {
      std::unique_lock g( _chain_state_mutex );
      name n = _head_block_state->header.producer;
      g.unlock();

      // FIXME/REMOVE: testing leader/proposer separation
      n = debug_leader_remap(n);

      return n;
   }

   name chain_pacemaker::get_next_leader() {
      std::unique_lock g( _chain_state_mutex );
      block_timestamp_type next_block_time = _head_block_state->header.timestamp.next();
      producer_authority p_auth = _head_block_state->get_scheduled_producer(next_block_time);
      name n = p_auth.producer_name;
      g.unlock();

      // FIXME/REMOVE: testing leader/proposer separation
      n = debug_leader_remap(n);

      return n;
   }

   std::vector<name> chain_pacemaker::get_finalizers() {

#warning FIXME: Use new finalizer list in pacemaker/qc_chain.
      // Every time qc_chain wants to know what the finalizers are, we get it from the controller, which
      //   is where it's currently stored.
      //
      // TODO:
      // - solve threading. for this particular case, I don't think using _chain-> is a big deal really;
      //   set_finalizers is called once in a blue moon, and this could be solved by a simple mutex even
      //   if it is the main thread that is waiting for a lock. But maybe there's a better way to do this
      //   overall.
      // - use this information in qc_chain and delete the old code below
      // - list of string finalizer descriptions instead of eosio name now
      // - also return the keys for each finalizer, not just name/description so qc_chain can use them
      //
      std::unique_lock g( _chain_state_mutex );
      const auto& fin_set = _chain->get_finalizers(); // TODO use

      // Old code: get eosio::name from the producer schedule
      const std::vector<producer_authority>& pa_list = _head_block_state->active_schedule.producers;
      std::vector<name> pn_list;
      pn_list.reserve(pa_list.size());
      std::transform(pa_list.begin(), pa_list.end(),
                     std::back_inserter(pn_list),
                     [](const producer_authority& p) { return p.producer_name; });
      return pn_list;
   }

   block_id_type chain_pacemaker::get_current_block_id() {
      std::scoped_lock g( _chain_state_mutex );
      return _head_block_state->id;
   }

   uint32_t chain_pacemaker::get_quorum_threshold() {
      return _quorum_threshold;
   }

   // called from the main application thread
   void chain_pacemaker::beat() {
      csc prof("beat");
      std::lock_guard g( _hotstuff_global_mutex );
      prof.core_in();
      _qc_chain.on_beat();
      prof.core_out();
   }

   void chain_pacemaker::send_hs_proposal_msg(const hs_proposal_message& msg, name id) {
      hs_proposal_message_ptr msg_ptr = std::make_shared<hs_proposal_message>(msg);
      _chain->commit_hs_proposal_msg(msg_ptr);
   }

   void chain_pacemaker::send_hs_vote_msg(const hs_vote_message& msg, name id) {
      hs_vote_message_ptr msg_ptr = std::make_shared<hs_vote_message>(msg);
      _chain->commit_hs_vote_msg(msg_ptr);
   }

   void chain_pacemaker::send_hs_new_block_msg(const hs_new_block_message& msg, name id) {
      hs_new_block_message_ptr msg_ptr = std::make_shared<hs_new_block_message>(msg);
      _chain->commit_hs_new_block_msg(msg_ptr);
   }

   void chain_pacemaker::send_hs_new_view_msg(const hs_new_view_message& msg, name id) {
      hs_new_view_message_ptr msg_ptr = std::make_shared<hs_new_view_message>(msg);
      _chain->commit_hs_new_view_msg(msg_ptr);
   }

   void chain_pacemaker::on_hs_proposal_msg(const hs_proposal_message& msg) {
      csc prof("prop");
      std::lock_guard g( _hotstuff_global_mutex );
      prof.core_in();
      _qc_chain.on_hs_proposal_msg(msg);
      prof.core_out();
   }

   void chain_pacemaker::on_hs_vote_msg(const hs_vote_message& msg) {
      csc prof("vote");
      std::lock_guard g( _hotstuff_global_mutex );
      prof.core_in();
      _qc_chain.on_hs_vote_msg(msg);
      prof.core_out();
   }

   void chain_pacemaker::on_hs_new_block_msg(const hs_new_block_message& msg) {
      csc prof("nblk");
      std::lock_guard g( _hotstuff_global_mutex );
      prof.core_in();
      _qc_chain.on_hs_new_block_msg(msg);
      prof.core_out();
   }

   void chain_pacemaker::on_hs_new_view_msg(const hs_new_view_message& msg) {
      csc prof("view");
      std::lock_guard g( _hotstuff_global_mutex );
      prof.core_in();
      _qc_chain.on_hs_new_view_msg(msg);
      prof.core_out();
   }

}}
