/**
 *  @file
 *  @copyright defined in eos/LICENSE.txt
 */
#include "eosio.system.hpp"

#include <eosiolib/eosio.hpp>
#include <eosiolib/crypto.h>
#include <eosiolib/print.hpp>
#include <eosiolib/datastream.hpp>
#include <eosiolib/serialize.hpp>
#include <eosiolib/multi_index.hpp>
#include <eosiolib/privileged.hpp>
#include <eosiolib/singleton.hpp>
#include <eosiolib/transaction.hpp>
#include <eosio.token/eosio.token.hpp>

#include <algorithm>
#include <cmath>

namespace eosiosystem {
   using eosio::indexed_by;
   using eosio::const_mem_fun;
   using eosio::bytes;
   using eosio::print;
   using eosio::singleton;
   using eosio::transaction;

   /**
    *  This method will create a producer_config and producer_info object for 'producer'
    *
    *  @pre producer is not already registered
    *  @pre producer to register is an account
    *  @pre authority of eosio to register
    *
    */
   void system_contract::addproducer( const account_name producer ) {
      eosio_assert( producer != N(eosio), "producer should not be eosio" );
      require_auth( N(eosio) );

      auto prod = _producers.find( producer );

      eosio_assert( prod == _producers.end(), "account already registered as a producer" );

      _producers.emplace( producer, [&]( producer_info& info ){
            info.owner         = producer;
            info.is_active     = false;
      });
   }

   void system_contract::unregprod( const account_name producer ) {
      require_auth( N(eosio) );

      const auto& prod = _producers.get( producer, "producer not found" );

      _producers.modify( prod, 0, [&]( producer_info& info ){
            info.deactivate();
      });
   }  

      /**
    *  This method will update the key and active flag to true in producer_info object for 'producer'
    *
    *  @pre producer is already registered
    *  @pre authority of producer to update
    *
    */
   void system_contract::regproducer( const account_name producer, const eosio::public_key& producer_key, const std::string& url, uint16_t location ) {
      eosio_assert( url.size() < 512, "url too long" );
      eosio_assert( producer_key != eosio::public_key(), "public key should not be the default value" );
      require_auth( producer );

      auto prod = _producers.find( producer );

      eosio_assert( prod != _producers.end(), "account is not registered as a producer" );

      _producers.modify( prod, producer, [&]( producer_info& info ){
            info.producer_key = producer_key;
            info.is_active    = true;
            info.url          = url;
            info.location     = location;
      });
   }

        /**
    *  This method will update the key and active flag to true in producer_info object for 'producer'
    *
    *  @pre producer is already registered
    *  @pre authority of producer to update
    *
    */
   void system_contract::togglesched( bool is_active ) {
      require_auth( N(eosio) );
      _gstate.is_producer_schedule_active = is_active;

   }


} /// namespace eosiosystem
