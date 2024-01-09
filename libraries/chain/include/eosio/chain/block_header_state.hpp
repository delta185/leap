#pragma once
#include <eosio/chain/block_header.hpp>
#include <eosio/chain/incremental_merkle.hpp>
#include <eosio/chain/protocol_feature_manager.hpp>
#include <eosio/chain/hotstuff/hotstuff.hpp>
#include <eosio/chain/hotstuff/finalizer_policy.hpp>
#include <eosio/chain/hotstuff/instant_finality_extension.hpp>
#include <eosio/chain/chain_snapshot.hpp>
#include <future>

namespace eosio::chain {

namespace detail { struct schedule_info; };

struct building_block_input {
   block_id_type                     parent_id;
   block_timestamp_type              timestamp;
   account_name                      producer;
   vector<digest_type>               new_protocol_feature_activations;
};

struct qc_data_t {
   quorum_certificate qc;                                  // Comes from traversing branch from parent and calling get_best_qc()
                                                           // assert(qc->block_num <= num_from_id(previous));
   qc_info_t          qc_info;                             // describes the above qc
};
      
// this struct can be extracted from a building block
struct block_header_state_input : public building_block_input {
   digest_type                       transaction_mroot;    // Comes from std::get<checksum256_type>(building_block::trx_mroot_or_receipt_digests)
   digest_type                       action_mroot;         // Compute root from  building_block::action_receipt_digests
   std::optional<proposer_policy>    new_proposer_policy;  // Comes from building_block::new_proposer_policy
   std::optional<finalizer_policy>   new_finalizer_policy; // Comes from building_block::new_finalizer_policy
   std::optional<qc_info_t>          qc_info;              // Comes from traversing branch from parent and calling get_best_qc()
                                                           // assert(qc->block_num <= num_from_id(previous));
};

struct block_header_state_core {
   uint32_t                last_final_block_num = 0;       // last irreversible (final) block.
   std::optional<uint32_t> final_on_strong_qc_block_num;   // will become final if this header achives a strong QC.
   std::optional<uint32_t> last_qc_block_num;              //
   uint32_t                finalizer_policy_generation;    // 

   block_header_state_core next(uint32_t last_qc_block_num, bool is_last_qc_strong) const;
};

struct block_header_state {
   // ------ data members ------------------------------------------------------------
   block_id_type                       id;
   block_header                        header;
   protocol_feature_activation_set_ptr activated_protocol_features;

   block_header_state_core             core;
   incremental_merkle_tree             proposal_mtree;
   incremental_merkle_tree             finality_mtree;

   finalizer_policy_ptr                finalizer_policy; // finalizer set + threshold + generation, supports `digest()`
   proposer_policy_ptr                 proposer_policy;  // producer authority schedule, supports `digest()`

   flat_map<uint32_t, proposer_policy_ptr>  proposer_policies;
   flat_map<uint32_t, finalizer_policy_ptr> finalizer_policies;

   // ------ functions -----------------------------------------------------------------
   digest_type           compute_finalizer_digest() const;
   block_timestamp_type  timestamp() const { return header.timestamp; }
   account_name          producer() const  { return header.producer; }
   const block_id_type&  previous() const  { return header.previous; }
   uint32_t              block_num() const { return block_header::num_from_id(previous()) + 1; }
   const producer_authority_schedule& active_schedule_auth()  const { return proposer_policy->proposer_schedule; }
   const producer_authority_schedule& pending_schedule_auth() const { return proposer_policies.rbegin()->second->proposer_schedule; } // [greg todo]
   
   block_header_state next(block_header_state_input& data) const;
   
   // block descending from this need the provided qc in the block extension
   bool is_needed(const quorum_certificate& qc) const {
      return !core.last_qc_block_num || qc.block_height > *core.last_qc_block_num;
   }

   flat_set<digest_type> get_activated_protocol_features() const { return activated_protocol_features->protocol_features; }
   detail::schedule_info prev_pending_schedule() const;
   producer_authority get_scheduled_producer(block_timestamp_type t) const;
   uint32_t active_schedule_version() const;
   std::optional<producer_authority_schedule>& new_pending_producer_schedule() { static std::optional<producer_authority_schedule> x; return x; } //  [greg todo] 
   signed_block_header make_block_header(const checksum256_type& transaction_mroot,
                                         const checksum256_type& action_mroot,
                                         const std::optional<producer_authority_schedule>& new_producers,
                                         vector<digest_type>&& new_protocol_feature_activations,
                                         const protocol_feature_set& pfs) const;
};

using block_header_state_ptr = std::shared_ptr<block_header_state>;

}

// [greg todo] which members need to be serialized to disk when saving fork_db
// obviously many are missing below.
FC_REFLECT( eosio::chain::block_header_state,  (id))