#include <eosio/chain/block_header_state.hpp>
#include <eosio/chain/block_header_state_utils.hpp>
#include <eosio/chain/hotstuff/instant_finality_extension.hpp>
#include <eosio/chain/exceptions.hpp>
#include <limits>

namespace eosio::chain {

producer_authority block_header_state::get_scheduled_producer(block_timestamp_type t) const {
   return detail::get_scheduled_producer(proposer_policy->proposer_schedule.producers, t);
}

#warning Add last_proposed_finalizer_policy_generation to snapshot_block_header_state_v3, see header file TODO
   
block_header_state_core block_header_state_core::next(const uint32_t last_qc_block_num, bool is_last_qc_strong) const {
   // no state change if last_qc_block_num is the same
   if (last_qc_block_num == this->last_qc_block_num) {
      return {*this};
   }

   EOS_ASSERT(last_qc_block_num > this->last_qc_block_num, block_validate_exception,
              "new last_qc_block_num must be greater than old last_qc_block_num");

   auto old_last_qc_block_num            = this->last_qc_block_num;
   auto old_final_on_strong_qc_block_num = this->final_on_strong_qc_block_num;

   block_header_state_core result{*this};

   if (is_last_qc_strong) {
      // last QC is strong. We can progress forward.

      // block with old final_on_strong_qc_block_num becomes irreversible
      if (old_final_on_strong_qc_block_num.has_value()) {
         result.last_final_block_num = *old_final_on_strong_qc_block_num;
      }

      // next block which can become irreversible is the block with
      // old last_qc_block_num
      if (old_last_qc_block_num.has_value()) {
         result.final_on_strong_qc_block_num = *old_last_qc_block_num;
      }
   } else {
      // new final_on_strong_qc_block_num should not be present
      result.final_on_strong_qc_block_num.reset();

      // new last_final_block_num should be the same as the old last_final_block_num
   }

   // new last_qc_block_num is always the input last_qc_block_num.
   result.last_qc_block_num = last_qc_block_num;

   return result;
}


block_header_state block_header_state::next(block_header_state_input& input) const {
   block_header_state result;

   // header
   // ------
   result.header = block_header {
      .timestamp         = input.timestamp, // [greg todo] do we have to do the slot++ stuff from the legacy version?
      .producer          = input.producer,
      .previous          = input.parent_id,
      .transaction_mroot = input.transaction_mroot,
      .action_mroot      = input.action_mroot,
      //.schedule_version = ?, [greg todo]
      //.new_producers = ?     [greg todo]
   };

   // core
   // ----
   if (input.qc_info)
      result.core = core.next(input.qc_info->last_qc_block_num, input.qc_info->is_last_qc_strong);
   else
      result.core = core;

   // add block header extensions
   // ---------------------------
   if (input.new_finalizer_policy)
      ++input.new_finalizer_policy->generation;

   std::optional<qc_info_t> qc_info = input.qc_info;
   if (!qc_info) {
      // [greg todo]: copy the one from the previous block (look in header.header_extensions)
   }
   
   emplace_extension(result.header.header_extensions, instant_finality_extension::extension_id(),
                     fc::raw::pack(instant_finality_extension{qc_info,
                                                              std::move(input.new_finalizer_policy),
                                                              std::move(input.new_proposer_policy)}));
               
   return result;
}


} // namespace eosio::chain