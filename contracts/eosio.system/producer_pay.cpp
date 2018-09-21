#include "eosio.system.hpp"

#include <eosio.token/eosio.token.hpp>

namespace eosiosystem {

   const int64_t  min_activated_stake   = 150'000'000'0000;
   const double   continuous_rate       = 0.058269;         // 6% annual rate
   const uint32_t blocks_per_year       = 52*7*24*2*3600;   // half seconds per year
   const uint32_t seconds_per_year      = 52*7*24*3600;
   const uint32_t blocks_per_day        = 2 * 24 * 3600;
   const uint32_t blocks_per_hour       = 2 * 3600;
   const uint64_t useconds_per_day      = 24 * 3600 * uint64_t(1000000);
   const uint64_t useconds_per_year     = seconds_per_year*1000000ll;


   void system_contract::onblock( block_timestamp timestamp, account_name producer ) {
      using namespace eosio;

      require_auth(N(eosio));

      /** until activated no new rewards are paid */
      if( !_gstate.is_producer_schedule_active )
         return;

      if( _gstate.last_inflation_bucket_fill == 0 )  /// start the presses
         _gstate.last_inflation_bucket_fill = current_time();


      /**
       * At startup the initial producer may not be one that is registered / elected
       * and therefore there may be no producer object for them.
       */
      auto prod = _producers.find(producer);
      if ( prod != _producers.end() ) {
         _producers.modify( prod, 0, [&](auto& p ) {
               p.produced_blocks++;
         });
      }

      /// only update block producers once every minute, block_timestamp is in half seconds
      if( timestamp.slot - _gstate.last_producer_schedule_update.slot > 120 ) {
         update_producers( timestamp );
      }

      /// only calculate inflation once 5 minutes, block_timestamp is in half seconds
      //if( timestamp.slot - _gstate.last_inflation_calulation.slot > 600 ) {
      if( timestamp.slot - _gstate.last_inflation_calulation.slot > 60 ) {
         auto ct = current_time();
         const asset token_supply   = token( N(eosio.token)).get_supply(symbol_type(system_token_symbol).name() );
         const auto usecs_since_last_fill = ct - _gstate.last_inflation_bucket_fill;
         
         if( usecs_since_last_fill > 0 && _gstate.last_inflation_bucket_fill > 0 ) {
            auto new_tokens = static_cast<int64_t>( (continuous_rate * double(token_supply.amount + _gstate.inflation_bucket) * double(usecs_since_last_fill)) / double(useconds_per_year) );
            _gstate.inflation_bucket += new_tokens;
         }

         _gstate.last_inflation_bucket_fill = ct;
         _gstate.last_inflation_calulation = timestamp;
      }

      /// only distribute inflation once a day
      if( timestamp.slot - _gstate.last_inflation_calulation.slot > 100 ) {
         auto to_producers       = _gstate.inflation_bucket / 6;
         auto to_savings         = to_producers;
         auto to_usage           = _gstate.inflation_bucket - to_producers - to_savings;

         INLINE_ACTION_SENDER(eosio::token, issue)( N(eosio.token), {{N(eosio),N(active)}},
                                                    {N(eosio), asset(_gstate.inflation_bucket), std::string("issue tokens for producer pay and savings and usage")} );

         INLINE_ACTION_SENDER(eosio::token, transfer)( N(eosio.token), {N(eosio),N(active)},
                                                       { N(eosio), N(eosio.saving), asset(to_savings), "unallocated inflation" } );

         INLINE_ACTION_SENDER(eosio::token, transfer)( N(eosio.token), {N(eosio),N(active)},
                                                       { N(eosio), N(eosio.ppay), asset(to_producers), "fund producer bucket" } );

         INLINE_ACTION_SENDER(eosio::token, transfer)( N(eosio.token), {N(eosio),N(active)},
                                                       { N(eosio), N(eosio.usage), asset(to_usage), "fund usage bucket" } );

        _gstate.inflation_bucket = 0;

        std::vector< eosio::producer_key > active_producers;
        for( const auto& p : _producers ) {
            if( p.active() ) {
                active_producers.emplace_back( eosio::producer_key{ p.owner, p.producer_key} );
            }                
        }    

        print("------------ active_producers.size(): ", int64_t(active_producers.size()), ", _gstate.last_producer_schedule_size: ", 
        int64_t(_gstate.last_producer_schedule_size), "\n");
        eosio_assert( active_producers.size() == _gstate.last_producer_schedule_size, "active_producers must equal last_producer_schedule_size" );   

      }

   }

   using namespace eosio;
   void system_contract::claimrewards( const account_name& owner ) {
      require_auth(owner);

      const auto& prod = _producers.get( owner );
      eosio_assert( prod.active(), "producer does not have an active key" );
      print("claimrewards ...^^^^.. printing the producer names \n");
     
      for( const auto& p : _producers ) {
        print(" name=", name{p.owner}, "\n");
      }
      


      auto ct = current_time();

      eosio_assert( ct - prod.last_claim_time > useconds_per_day, "already claimed rewards within past day" );

      _producers.modify( prod, 0, [&](auto& p) {
          p.last_claim_time = ct;
      });

/**
      if( producer_per_block_pay > 0 ) {
         INLINE_ACTION_SENDER(eosio::token, transfer)( N(eosio.token), {N(eosio.bpay),N(active)},
                                                       { N(eosio.bpay), owner, asset(producer_per_block_pay), std::string("producer block pay") } );
      }
      if( producer_per_vote_pay > 0 ) {
         INLINE_ACTION_SENDER(eosio::token, transfer)( N(eosio.token), {N(eosio.vpay),N(active)},
                                                       { N(eosio.vpay), owner, asset(producer_per_vote_pay), std::string("producer vote pay") } );
      }
    **/
   }

} //namespace eosiosystem
