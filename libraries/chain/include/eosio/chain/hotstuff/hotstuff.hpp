#pragma once
#include <eosio/chain/block_header.hpp>
#include <fc/bitutil.hpp>
#include <fc/crypto/bls_private_key.hpp>
#include <fc/crypto/bls_public_key.hpp>
#include <fc/crypto/bls_signature.hpp>

#include <boost/dynamic_bitset.hpp>

#include <mutex>

namespace eosio::chain {

   using hs_bitset = boost::dynamic_bitset<uint32_t>;
   using bls_key_map_t = std::map<fc::crypto::blslib::bls_public_key, fc::crypto::blslib::bls_private_key>;

   inline digest_type get_digest_to_sign(const block_id_type& block_id, uint8_t phase_counter, const fc::sha256& final_on_qc) {
      digest_type h1 = digest_type::hash( std::make_pair( std::cref(block_id), phase_counter ) );
      digest_type h2 = digest_type::hash( std::make_pair( std::cref(h1), std::cref(final_on_qc) ) );
      return h2;
   }

   inline uint64_t compute_height(uint32_t block_height, uint32_t phase_counter) {
      return (uint64_t{block_height} << 32) | phase_counter;
   }

   struct view_number {
      view_number() : bheight(0), pcounter(0) {}
      explicit view_number(uint32_t block_height, uint8_t phase_counter) : bheight(block_height), pcounter(phase_counter) {}
      auto operator<=>(const view_number&) const = default;
      friend std::ostream& operator<<(std::ostream& os, const view_number& vn) {
         os << "view_number(" << vn.bheight << ", " << vn.pcounter << ")\n";
         return os;
      }

      uint32_t block_height() const { return bheight; }
      uint8_t phase_counter() const { return pcounter; }
      uint64_t get_key() const { return compute_height(bheight, pcounter); }
      std::string to_string() const { return std::to_string(bheight) + "::" + std::to_string(pcounter); }

      uint32_t bheight;
      uint8_t pcounter;
   };

   struct quorum_certificate_message {
      fc::sha256                          proposal_id;
      std::vector<uint32_t>               strong_votes; //bitset encoding, following canonical order
      std::vector<uint32_t>               weak_votes;   //bitset encoding, following canonical order
      fc::crypto::blslib::bls_signature   active_agg_sig;
   };

   struct vote_message {
      fc::sha256                          proposal_id; //vote on proposal
      bool                                strong{false};
      fc::crypto::blslib::bls_public_key  finalizer_key;
      fc::crypto::blslib::bls_signature   sig;
   };

   struct hs_proposal_message {
      fc::sha256                          proposal_id; //vote on proposal
      block_id_type                       block_id;
      fc::sha256                          parent_id; //new proposal
      fc::sha256                          final_on_qc;
      quorum_certificate_message          justify; //justification
      uint8_t                             phase_counter = 0;
      mutable std::optional<digest_type>  digest;

      digest_type get_proposal_digest() const {
         if (!digest)
            digest.emplace(get_digest_to_sign(block_id, phase_counter, final_on_qc));
         return *digest;
      };

      uint32_t block_num() const { return block_header::num_from_id(block_id); }
      uint64_t get_key() const { return compute_height(block_header::num_from_id(block_id), phase_counter); };

      view_number get_view_number() const { return view_number(block_header::num_from_id(block_id), phase_counter); };
   };

   struct hs_new_view_message {
      quorum_certificate_message   high_qc; //justification
   };

   struct hs_message {
      std::variant<vote_message, hs_proposal_message, hs_new_view_message> msg;
   };

   enum class hs_message_warning {
      discarded,               // default code for dropped messages (irrelevant, redundant, ...)
      duplicate_signature,     // same message signature already seen
      invalid_signature,       // invalid message signature
      invalid                  // invalid message (other reason)
   };

   enum class vote_status {
      success,
      duplicate,
      unknown_public_key,
      invalid_signature,
      unknown_block
   };

   using bls_public_key  = fc::crypto::blslib::bls_public_key;
   using bls_signature   = fc::crypto::blslib::bls_signature;
   using bls_private_key = fc::crypto::blslib::bls_private_key;

   // -------------------- valid_quorum_certificate -------------------------------------------------
   class valid_quorum_certificate {
   public:
      valid_quorum_certificate(const std::vector<uint32_t>& strong_votes, //bitset encoding, following canonical order
                               const std::vector<uint32_t>& weak_votes,   //bitset encoding, following canonical order
                               const bls_signature& sig);

      valid_quorum_certificate() = default;
      valid_quorum_certificate(const valid_quorum_certificate&) = default;

      bool is_weak()   const { return !!_weak_votes; }
      bool is_strong() const { return !_weak_votes; }

      friend struct fc::reflector<valid_quorum_certificate>;
      std::optional<hs_bitset> _strong_votes;
      std::optional<hs_bitset> _weak_votes;
      bls_signature            _sig;
   };

   // -------------------- quorum_certificate -------------------------------------------------------
   struct quorum_certificate {
      uint32_t block_num;
      valid_quorum_certificate qc;
   };


   // -------------------- pending_quorum_certificate -------------------------------------------------
   class pending_quorum_certificate {
   public:
      enum class state_t {
         unrestricted,  // No quorum reached yet, still possible to achieve any state.
         restricted,    // Enough `weak` votes received to know it is impossible to reach the `strong` state.
         weak_achieved, // Enough `weak` + `strong` votes for a valid `weak` QC, still possible to reach the `strong` state.
         weak_final,    // Enough `weak` + `strong` votes for a valid `weak` QC, `strong` not possible anymore.
         strong         // Enough `strong` votes to have a valid `strong` QC
      };

      struct votes_t {
         hs_bitset     _bitset;
         bls_signature _sig;

         void resize(size_t num_finalizers) { _bitset.resize(num_finalizers); }
         size_t count() const { return _bitset.count(); }

         vote_status add_vote(const std::vector<uint8_t>& proposal_digest, size_t index, const bls_public_key& pubkey,
                              const bls_signature& new_sig);

         void reset(size_t num_finalizers);
      };

      pending_quorum_certificate();

      explicit pending_quorum_certificate(size_t num_finalizers, uint64_t quorum, uint64_t max_weak_sum_before_weak_final);

      // thread safe
      bool   is_quorum_met() const;

      // thread safe
      std::pair<vote_status, bool> add_vote(bool strong,
                                            const std::vector<uint8_t>& proposal_digest,
                                            size_t index,
                                            const bls_public_key& pubkey,
                                            const bls_signature& sig,
                                            uint64_t weight);

      state_t state()  const { std::lock_guard g(*_mtx); return _state; };
      valid_quorum_certificate to_valid_quorum_certificate() const;

   private:
      friend struct fc::reflector<pending_quorum_certificate>;
      friend class qc_chain;
      std::unique_ptr<std::mutex> _mtx;
      uint64_t             _quorum {0};
      uint64_t             _max_weak_sum_before_weak_final {0}; // max weak sum before becoming weak_final
      state_t              _state { state_t::unrestricted };
      uint64_t             _strong_sum {0}; // accumulated sum of strong votes so far
      uint64_t             _weak_sum {0}; // accumulated sum of weak votes so far
      votes_t              _weak_votes;
      votes_t              _strong_votes;

      // called by add_vote, already protected by mutex
      vote_status add_strong_vote(const std::vector<uint8_t>& proposal_digest,
                                  size_t index,
                                  const bls_public_key& pubkey,
                                  const bls_signature& sig,
                                  uint64_t weight);

      // called by add_vote, already protected by mutex
      vote_status add_weak_vote(const std::vector<uint8_t>& proposal_digest,
                                size_t index,
                                const bls_public_key& pubkey,
                                const bls_signature& sig,
                                uint64_t weight);
   };
} //eosio::chain


FC_REFLECT(eosio::chain::view_number, (bheight)(pcounter));
FC_REFLECT(eosio::chain::quorum_certificate_message, (proposal_id)(strong_votes)(weak_votes)(active_agg_sig));
FC_REFLECT(eosio::chain::vote_message, (proposal_id)(strong)(finalizer_key)(sig));
FC_REFLECT(eosio::chain::hs_proposal_message, (proposal_id)(block_id)(parent_id)(final_on_qc)(justify)(phase_counter));
FC_REFLECT(eosio::chain::hs_new_view_message, (high_qc));
FC_REFLECT(eosio::chain::hs_message, (msg));
FC_REFLECT(eosio::chain::valid_quorum_certificate, (_strong_votes)(_weak_votes)(_sig));
FC_REFLECT(eosio::chain::pending_quorum_certificate, (_quorum)(_max_weak_sum_before_weak_final)(_state)(_strong_sum)(_weak_sum)(_weak_votes)(_strong_votes));
FC_REFLECT(eosio::chain::pending_quorum_certificate::votes_t, (_bitset)(_sig));
FC_REFLECT(eosio::chain::quorum_certificate, (block_num)(qc));