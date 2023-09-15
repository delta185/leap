#pragma once
#include <fc/crypto/bls_signature.hpp>
#include <fc/reflect/reflect.hpp>
#include <fc/reflect/variant.hpp>
#include <bls12-381/bls12-381.hpp>

namespace fc::crypto::blslib {

   namespace config {
      const std::string bls_public_key_prefix = "PUB_BLS_";
   };

   class bls_public_key
   {
      public:
 
         bls_public_key() = default;
         bls_public_key( bls_public_key&& ) = default;
         bls_public_key( const bls_public_key& ) = default;
         explicit bls_public_key( const bls12_381::g1& pkey ) {_pkey = pkey;}
         explicit bls_public_key(const std::string& base64str);

         bls_public_key& operator=(const bls_public_key&) = default;
         std::string to_string(const yield_function_t& yield = yield_function_t()) const;
         friend bool operator==(const bls_public_key& p1, const bls_public_key& p2);

         auto operator<=>(const bls_public_key&) const = default;

         bls12_381::g1 _pkey;

   }; // bls_public_key

}  // fc::crypto::blslib

namespace fc {
   void to_variant(const crypto::blslib::bls_public_key& var, variant& vo, const yield_function_t& yield = yield_function_t());

   void from_variant(const variant& var, crypto::blslib::bls_public_key& vo);
} // namespace fc

FC_REFLECT(bls12_381::g1, (x)(y)(z))
FC_REFLECT(crypto::blslib::bls_public_key, (_pkey) )