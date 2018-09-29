#include "eosio.system.hpp"

#include <eosio.token/eosio.token.hpp>

namespace eosiosystem {

   const int64_t  min_activated_stake   = 150'000'000'0000;
   const double   continuous_rate       = 0.0582729; // 6% annual rate
   const uint32_t blocks_per_year       = 52*7*24*2*3600;   // half seconds per year
   const uint32_t seconds_per_year      = 52*7*24*3600;
   const uint32_t blocks_per_day        = 2 * 24 * 3600;
   const uint32_t blocks_per_hour       = 2 * 3600;
   const uint64_t useconds_per_day      = 24 * 3600 * uint64_t(1000000);
   const uint64_t useconds_per_year     = seconds_per_year*1000000ll;
   const uint64_t useconds_per_min      = 60 * uint64_t(1000000);


   void system_contract::onblock( block_timestamp timestamp, account_name producer ) {
      using namespace eosio;

      require_auth(N(eosio));
      auto ct = current_time();

      /** until activated no new rewards are paid */
      if( !_gstate.is_producer_schedule_active )
         return;

      if( _gstate.last_inflation_distribution == 0 ) /// start the presses
         _gstate.last_inflation_distribution = ct;    

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


      /// only distribute inflation once a day
      if( ct - _gstate.last_inflation_distribution > useconds_per_day ) {
         const asset token_supply   = token( N(eosio.token)).get_supply(symbol_type(system_token_symbol).name() ); 
         const auto usecs_since_last_fill = ct - _gstate.last_inflation_distribution;
         auto new_tokens = static_cast<int64_t>( (continuous_rate * double(token_supply.amount) * double(usecs_since_last_fill)) / double(useconds_per_year) );

         auto to_producers       = new_tokens / 6;
         auto to_savings         = to_producers;
         auto to_usage           = new_tokens - (to_producers + to_savings);

         INLINE_ACTION_SENDER(eosio::token, issue)( N(eosio.token), {{N(eosio),N(active)}},
                                                    {N(eosio), asset(new_tokens), std::string("issue tokens for producer pay and savings and usage")} );

         INLINE_ACTION_SENDER(eosio::token, transfer)( N(eosio.token), {N(eosio),N(active)},
                                                       { N(eosio), N(eosio.saving), asset(to_savings), "unallocated inflation" } );

         INLINE_ACTION_SENDER(eosio::token, transfer)( N(eosio.token), {N(eosio),N(active)},
                                                       { N(eosio), N(eosio.ppay), asset(to_producers), "fund producer account" } );

         INLINE_ACTION_SENDER(eosio::token, transfer)( N(eosio.token), {N(eosio),N(active)},
                                                       { N(eosio), N(eosio.usage), asset(to_usage), "fund usage account" } );

        std::vector< account_name > active_producers;
        for( const auto& p : _producers ) {
            if( p.active() ) {
                active_producers.emplace_back( p.owner );
            }                
        }    

        eosio_assert( active_producers.size() == _gstate.last_producer_schedule_size, "active_producers must equal last_producer_schedule_size" );   

        uint64_t earned_pay = uint64_t(to_producers / active_producers.size());
        for( const auto& p : active_producers ) {

            auto pay_itr = _producer_pay.find( p );        

            if( pay_itr ==  _producer_pay.end() ) {
                pay_itr = _producer_pay.emplace( p, [&]( auto& pay ) {
                    pay.owner = p;
                    pay.earned_pay = earned_pay;
                });
            } else {
                _producer_pay.modify( pay_itr, 0, [&]( auto& pay ) {
                    pay.earned_pay += earned_pay;
                });
            }              
        }   

        _gstate.last_inflation_distribution = ct;

      }

   }

   using namespace eosio;
   void system_contract::claimrewards( const account_name& owner ) {
      require_auth(owner);

      const auto& prod = _producers.get( owner );
      eosio_assert( prod.active(), "producer does not have an active key" );
                    
      auto ct = current_time();

      //producer_pay_table  pay_tbl( _self, _self );
      auto pay = _producer_pay.find( owner );
      eosio_assert( pay != _producer_pay.end(), "producer pay request not found" );
      eosio_assert( ct - prod.last_claim_time > useconds_per_day, "already claimed rewards within past day" );

      uint64_t earned_pay = pay->earned_pay;

     _producer_pay.erase( pay );

      _producers.modify( prod, 0, [&](auto& p) {
          p.last_claim_time = ct;
      });

      if( earned_pay > 0 ) {
         INLINE_ACTION_SENDER(eosio::token, transfer)( N(eosio.token), {N(eosio.ppay),N(active)},
                                                       { N(eosio.ppay), owner, asset(earned_pay), std::string("producer block pay") } );
      }

   }

} //namespace eosiosystem
