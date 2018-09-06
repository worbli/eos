/**
 *  @file
 *  @copyright defined in eos/LICENSE.txt
 */
#include "eosio.system.hpp"

#include <eosiolib/eosio.hpp>
#include <eosiolib/print.hpp>
#include <eosiolib/datastream.hpp>
#include <eosiolib/serialize.hpp>
#include <eosiolib/multi_index.hpp>
#include <eosiolib/privileged.h>
#include <eosiolib/transaction.hpp>

#include <eosio.token/eosio.token.hpp>


#include <cmath>
#include <map>

namespace eosiosystem {
   using eosio::asset;
   using eosio::indexed_by;
   using eosio::const_mem_fun;
   using eosio::bytes;
   using eosio::print;
   using eosio::permission_level;
   using std::map;
   using std::pair;

   static constexpr time refund_delay = 3*24*3600;
   static constexpr time refund_expiration_time = 3600;

   struct user_resources {
      account_name  owner;
      asset         net_weight;
      asset         cpu_weight;
      asset         ram_stake;
      int64_t       ram_bytes = 0;

      uint64_t primary_key()const { return owner; }

      // explicit serialization macro is not necessary, used here only to improve compilation time
      EOSLIB_SERIALIZE( user_resources, (owner)(net_weight)(cpu_weight)(ram_stake)(ram_bytes) )
   };


   /**
    *  Every user 'from' has a scope/table that uses every receipient 'to' as the primary key.
    */
   struct delegated_bandwidth {
      account_name  from;
      account_name  to;
      asset         net_weight;
      asset         cpu_weight;

      uint64_t  primary_key()const { return to; }

      // explicit serialization macro is not necessary, used here only to improve compilation time
      EOSLIB_SERIALIZE( delegated_bandwidth, (from)(to)(net_weight)(cpu_weight) )

   };

   struct refund_request {
      account_name  owner;
      time          request_time;
      eosio::asset  net_amount;
      eosio::asset  cpu_amount;
      eosio::asset  ram_amount;
      uint64_t      ram_bytes;

      uint64_t  primary_key()const { return owner; }

      // explicit serialization macro is not necessary, used here only to improve compilation time
      EOSLIB_SERIALIZE( refund_request, (owner)(request_time)(net_amount)(cpu_amount)(ram_amount)(ram_bytes) )
   };

   /**
    *  These tables are designed to be constructed in the scope of the relevant user, this
    *  facilitates simpler API for per-user queries
    */
   typedef eosio::multi_index< N(userres), user_resources>      user_resources_table;
   typedef eosio::multi_index< N(delband), delegated_bandwidth> del_bandwidth_table;
   typedef eosio::multi_index< N(refunds), refund_request>      refunds_table;


  void system_contract::updateram( account_name payer, account_name receiver, uint32_t bytes, asset quant) {
    
      _gstate.total_ram_bytes_reserved += uint64_t(bytes);
      _gstate.total_ram_stake          += quant.amount;

        user_resources_table  userres( _self, receiver );
        auto res_itr = userres.find( receiver );

        if( res_itr ==  userres.end() ) {
        res_itr = userres.emplace( receiver, [&]( auto& res ) {
              res.owner = receiver;
              res.ram_bytes = bytes;
              res.ram_stake = quant;
             });
        } else {
        userres.modify( res_itr, receiver, [&]( auto& res ) {
              res.ram_bytes += bytes;
              res.ram_stake += quant;
             });
        }
        set_resource_limits( res_itr->owner, res_itr->ram_bytes, res_itr->net_weight.amount, res_itr->cpu_weight.amount );

 
      // create refund or update from existing refund
      if ( N(eosio.stake) != receiver ) { //for eosio both transfer and refund make no sense
         refunds_table refunds_tbl( _self, receiver );
         auto req = refunds_tbl.find( receiver );

         //create/update/delete refund
         auto ram_balance = quant;
         bool need_deferred_trx = false;


         if ( req != refunds_tbl.end() ) { //need to update refund
            refunds_tbl.modify( req, 0, [&]( refund_request& r ) {
                r.request_time = now();
                r.ram_amount -= ram_balance;
                r.ram_bytes -= bytes;
                if ( r.ram_amount < asset(0) ) {
                   ram_balance = -r.ram_amount;
                   r.ram_amount = asset(0);
                   r.ram_bytes = 0;
                } else {
                   ram_balance = asset(0);
                }                  
             });

             eosio_assert( asset(0) <= req->ram_amount, "negative ram refund amount" ); //should never happen

             if ( req->net_amount == asset(0) && req->cpu_amount == asset(0) && req->ram_bytes == 0 &&
                req->ram_amount == asset(0) ) {
                refunds_tbl.erase( req );
                need_deferred_trx = false;
             } else {
                need_deferred_trx = true;
             }
         } 


         if ( need_deferred_trx ) {
            eosio::transaction out;
            out.actions.emplace_back( permission_level{ receiver, N(active) }, _self, N(refund), receiver );
            out.delay_sec = refund_delay;
            cancel_deferred( receiver ); // TODO: Remove this line when replacing deferred trxs is fixed
            out.send( receiver, receiver, true );
         } else {
            cancel_deferred( receiver );
         }

         auto transfer_amount = ram_balance;
         if ( asset(0) < transfer_amount ) {
            INLINE_ACTION_SENDER(eosio::token, transfer)( N(eosio.token), {payer, N(active)},
               { payer, N(eosio.stake), asset(transfer_amount), std::string("stake ram") } );
         }
      }
      // update voting
  }

   /**
    *  This action will buy an exact amount of ram and bill the payer the current market price.
    */
   void system_contract::buyrambytes( account_name payer, account_name receiver, uint32_t bytes ) {

     // Todo: Take closer look at type casts

      const asset token_supply   = token( N(eosio.token)).get_supply(symbol_type(system_token_symbol).name() );
      const uint64_t token_precision = token_supply.symbol.precision();
      const uint64_t bytes_per_token = uint64_t((_gstate.max_ram_size / (double)token_supply.amount) * pow(10,token_precision));
      
      auto amount = int64_t((bytes * pow(10,token_precision)) / bytes_per_token);

      updateram( payer, receiver, bytes, asset(amount, token_supply.symbol));
   }


   /**
    *  When buying ram the payer irreversiblly transfers quant to system contract and only
    *  the receiver may reclaim the tokens via the sellram action. The receiver pays for the
    *  storage of all database records associated with this action.
    *
    *  RAM is a scarce resource whose supply is defined by global properties max_ram_size. RAM is
    *  priced using the bancor algorithm such that price-per-byte with a constant reserve ratio of 100:1.
    */
   void system_contract::buyram( account_name payer, account_name receiver, asset quant )
   {
      require_auth( payer );
      eosio_assert( quant.amount > 0, "must purchase a positive amount" );


      const asset token_supply   = token( N(eosio.token)).get_supply(symbol_type(system_token_symbol).name() );
      const uint64_t token_precision = token_supply.symbol.precision();
      const uint64_t bytes_per_token = uint64_t((_gstate.max_ram_size / (double)token_supply.amount) * pow(10,token_precision));
      
      uint64_t bytes_out = uint64_t(bytes_per_token * quant.amount / pow(10,token_precision));

      eosio_assert( bytes_out > 0, "must reserve a positive amount" );

      updateram( payer, receiver, bytes_out, quant);      

   }


   /**
    *  The system contract now buys and sells RAM allocations at prevailing market prices.
    *  This may result in traders buying RAM today in anticipation of potential shortages
    *  tomorrow. Overall this will result in the market balancing the supply and demand
    *  for RAM over time.
    */
   void system_contract::sellram( account_name account, int64_t bytes ) {
      require_auth( account );
      eosio_assert( bytes > 0, "cannot sell negative byte" );

      user_resources_table  userres( _self, account );
      auto res_itr = userres.find( account );
      eosio_assert( res_itr != userres.end(), "no resource row" );
      eosio_assert( res_itr->ram_bytes >= bytes, "insufficient quota" );
       
      asset tokens_out;
      int64_t ram_bytes = res_itr->ram_bytes;
      float_t token_per_bytes = res_itr->ram_stake.amount / (float_t)ram_bytes;
      int64_t tokens = token_per_bytes * bytes;

      tokens_out = asset{tokens};

      eosio_assert( tokens_out.amount > 1, "token amount received from selling ram is too low" );

      _gstate.total_ram_bytes_reserved -= static_cast<decltype(_gstate.total_ram_bytes_reserved)>(bytes); // bytes > 0 is asserted above
      _gstate.total_ram_stake          -= tokens_out.amount;

      //// this shouldn't happen, but just in case it does we should prevent it
      eosio_assert( _gstate.total_ram_stake >= 0, "error, attempt to unstake more tokens than previously staked" );

      userres.modify( res_itr, account, [&]( auto& res ) {
          res.ram_bytes -= bytes;
          res.ram_stake -= tokens_out;
      });
      set_resource_limits( res_itr->owner, res_itr->ram_bytes, res_itr->net_weight.amount, res_itr->cpu_weight.amount );
      
            // create refund or update from existing refund
      if ( N(eosio.stake) != account ) { //for eosio both transfer and refund make no sense
         refunds_table refunds_tbl( _self, account );
         auto req = refunds_tbl.find( account );

         //create/update/delete refund
         auto ram_balance = tokens_out;
         bool need_deferred_trx = false;

            if ( req != refunds_tbl.end() ) { //need to update refund
               refunds_tbl.modify( req, 0, [&]( refund_request& r ) {
                  r.request_time = now();
                  r.ram_amount += ram_balance;   
                  r.ram_bytes += bytes;               
               });

               if ( req->net_amount == asset(0) && req->cpu_amount == asset(0) && req->ram_bytes == 0 &&
                  req->ram_amount == asset(0) ) {
                  refunds_tbl.erase( req );
                  need_deferred_trx = false;
               } else {
                  need_deferred_trx = true;
               }

            } else { //need to create refund
               refunds_tbl.emplace( account, [&]( refund_request& r ) {
                  r.owner = account;
                  r.ram_amount = ram_balance;
                  r.ram_bytes = bytes;
                  r.request_time = now();
               });
               need_deferred_trx = true;
            } // else stake increase requested with no existing row in refunds_tbl -> nothing to do with refunds_tbl      

         if ( need_deferred_trx ) {
            eosio::transaction out;
            out.actions.emplace_back( permission_level{ account, N(active) }, _self, N(refund), account );
            out.delay_sec = refund_delay;
            cancel_deferred( account ); // TODO: Remove this line when replacing deferred trxs is fixed
            out.send( account, account, true );
         } else {
            cancel_deferred( account );
         }
      }

      // need to update voting power
    
   }

   void validate_b1_vesting( int64_t stake ) {
      const int64_t base_time = 1527811200; /// 2018-06-01
      const int64_t max_claimable = 100'000'000'0000ll;
      const int64_t claimable = int64_t(max_claimable * double(now()-base_time) / (10*seconds_per_year) );

      eosio_assert( max_claimable - claimable <= stake, "b1 can only claim their tokens over 10 years" );
   }

   void system_contract::changebw( account_name from, account_name receiver,
                                   const asset stake_net_delta, const asset stake_cpu_delta, bool transfer )
   {
      require_auth( from );
      eosio_assert( stake_net_delta != asset(0) || stake_cpu_delta != asset(0), "should stake non-zero amount" );
      eosio_assert( std::abs( (stake_net_delta + stake_cpu_delta).amount )
                     >= std::max( std::abs( stake_net_delta.amount ), std::abs( stake_cpu_delta.amount ) ),
                    "net and cpu deltas cannot be opposite signs" );


      account_name source_stake_from = from;
      if ( transfer ) {
         from = receiver;
      }

      // update stake delegated from "from" to "receiver"
      {
         del_bandwidth_table     del_tbl( _self, from);
         auto itr = del_tbl.find( receiver );
         if( itr == del_tbl.end() ) {
            itr = del_tbl.emplace( from, [&]( auto& dbo ){
                  dbo.from          = from;
                  dbo.to            = receiver;
                  dbo.net_weight    = stake_net_delta;
                  dbo.cpu_weight    = stake_cpu_delta;
               });
         }
         else {
            del_tbl.modify( itr, 0, [&]( auto& dbo ){
                  dbo.net_weight    += stake_net_delta;
                  dbo.cpu_weight    += stake_cpu_delta;
               });
         }
         eosio_assert( asset(0) <= itr->net_weight, "insufficient staked net bandwidth" );
         eosio_assert( asset(0) <= itr->cpu_weight, "insufficient staked cpu bandwidth" );
         if ( itr->net_weight == asset(0) && itr->cpu_weight == asset(0) ) {
            del_tbl.erase( itr );
         }
      } // itr can be invalid, should go out of scope

      // update totals of "receiver"
      {
         user_resources_table   totals_tbl( _self, receiver );
         auto tot_itr = totals_tbl.find( receiver );
         if( tot_itr ==  totals_tbl.end() ) {
            tot_itr = totals_tbl.emplace( from, [&]( auto& tot ) {
                  tot.owner = receiver;
                  tot.net_weight    = stake_net_delta;
                  tot.cpu_weight    = stake_cpu_delta;
               });
         } else {
            totals_tbl.modify( tot_itr, from == receiver ? from : 0, [&]( auto& tot ) {
                  tot.net_weight    += stake_net_delta;
                  tot.cpu_weight    += stake_cpu_delta;
               });
         }
         eosio_assert( asset(0) <= tot_itr->net_weight, "insufficient staked total net bandwidth" );
         eosio_assert( asset(0) <= tot_itr->cpu_weight, "insufficient staked total cpu bandwidth" );

         set_resource_limits( receiver, tot_itr->ram_bytes, tot_itr->net_weight.amount, tot_itr->cpu_weight.amount );

         if ( tot_itr->net_weight == asset(0) && tot_itr->cpu_weight == asset(0)  && tot_itr->ram_bytes == 0 && 
              tot_itr->ram_stake == asset(0) ) {
            totals_tbl.erase( tot_itr );
         }
      } // tot_itr can be invalid, should go out of scope

      // create refund or update from existing refund
      if ( N(eosio.stake) != source_stake_from ) { //for eosio both transfer and refund make no sense
         refunds_table refunds_tbl( _self, from );
         auto req = refunds_tbl.find( from );

         //create/update/delete refund
         auto net_balance = stake_net_delta;
         auto cpu_balance = stake_cpu_delta;
         bool need_deferred_trx = false;


         // net and cpu are same sign by assertions in delegatebw and undelegatebw
         // redundant assertion also at start of changebw to protect against misuse of changebw
         bool is_undelegating = (net_balance.amount + cpu_balance.amount ) < 0;
         bool is_delegating_to_self = (!transfer && from == receiver);

         if( is_delegating_to_self || is_undelegating ) {
            if ( req != refunds_tbl.end() ) { //need to update refund
               refunds_tbl.modify( req, 0, [&]( refund_request& r ) {
                  if ( net_balance < asset(0) || cpu_balance < asset(0) ) {
                     r.request_time = now();
                  }
                  r.net_amount -= net_balance;
                  if ( r.net_amount < asset(0) ) {
                     net_balance = -r.net_amount;
                     r.net_amount = asset(0);
                  } else {
                     net_balance = asset(0);
                  }
                  r.cpu_amount -= cpu_balance;
                  if ( r.cpu_amount < asset(0) ){
                     cpu_balance = -r.cpu_amount;
                     r.cpu_amount = asset(0);
                  } else {
                     cpu_balance = asset(0);
                  }
               });

               eosio_assert( asset(0) <= req->net_amount, "negative net refund amount" ); //should never happen
               eosio_assert( asset(0) <= req->cpu_amount, "negative cpu refund amount" ); //should never happen

               if ( req->net_amount == asset(0) && req->cpu_amount == asset(0) && req->ram_bytes == 0 &&
                  req->ram_amount == asset(0) ) {
                  refunds_tbl.erase( req );
                  need_deferred_trx = false;
               } else {
                  need_deferred_trx = true;
               }

            } else if ( net_balance < asset(0) || cpu_balance < asset(0) ) { //need to create refund
               refunds_tbl.emplace( from, [&]( refund_request& r ) {
                  r.owner = from;
                  if ( net_balance < asset(0) ) {
                     r.net_amount = -net_balance;
                     net_balance = asset(0);
                  } // else r.net_amount = 0 by default constructor
                  if ( cpu_balance < asset(0) ) {
                     r.cpu_amount = -cpu_balance;
                     cpu_balance = asset(0);
                  } // else r.cpu_amount = 0 by default constructor
                  r.request_time = now();
               });
               need_deferred_trx = true;
            } // else stake increase requested with no existing row in refunds_tbl -> nothing to do with refunds_tbl
         } /// end if is_delegating_to_self || is_undelegating

         if ( need_deferred_trx ) {
            eosio::transaction out;
            out.actions.emplace_back( permission_level{ from, N(active) }, _self, N(refund), from );
            out.delay_sec = refund_delay;
            cancel_deferred( from ); // TODO: Remove this line when replacing deferred trxs is fixed
            out.send( from, from, true );
         } else {
            cancel_deferred( from );
         }

         auto transfer_amount = net_balance + cpu_balance;
         if ( asset(0) < transfer_amount ) {
            INLINE_ACTION_SENDER(eosio::token, transfer)( N(eosio.token), {source_stake_from, N(active)},
               { source_stake_from, N(eosio.stake), asset(transfer_amount), std::string("stake bandwidth") } );
         }
      }

      // update voting power
      {
         asset total_update = stake_net_delta + stake_cpu_delta;
         auto from_voter = _voters.find(from);
         if( from_voter == _voters.end() ) {
            from_voter = _voters.emplace( from, [&]( auto& v ) {
                  v.owner  = from;
                  v.staked = total_update.amount;
               });
         } else {
            _voters.modify( from_voter, 0, [&]( auto& v ) {
                  v.staked += total_update.amount;
               });
         }
         eosio_assert( 0 <= from_voter->staked, "stake for voting cannot be negative");
         if( from == N(b1) ) {
            validate_b1_vesting( from_voter->staked );
         }

         if( from_voter->producers.size() || from_voter->proxy ) {
            update_votes( from, from_voter->proxy, from_voter->producers, false );
         }
      }
   }

   void system_contract::delegatebw( account_name from, account_name receiver,
                                     asset stake_net_quantity,
                                     asset stake_cpu_quantity, bool transfer )
   {
      eosio_assert( stake_cpu_quantity >= asset(0), "must stake a positive amount" );
      eosio_assert( stake_net_quantity >= asset(0), "must stake a positive amount" );
      eosio_assert( stake_net_quantity + stake_cpu_quantity > asset(0), "must stake a positive amount" );
      eosio_assert( !transfer || from != receiver, "cannot use transfer flag if delegating to self" );

      changebw( from, receiver, stake_net_quantity, stake_cpu_quantity, transfer);
   } // delegatebw

   void system_contract::undelegatebw( account_name from, account_name receiver,
                                       asset unstake_net_quantity, asset unstake_cpu_quantity )
   {
      eosio_assert( asset() <= unstake_cpu_quantity, "must unstake a positive amount" );
      eosio_assert( asset() <= unstake_net_quantity, "must unstake a positive amount" );
      eosio_assert( asset() < unstake_cpu_quantity + unstake_net_quantity, "must unstake a positive amount" );      
      // worbli change: Removing chain activation logic for unstaking
      // eosio_assert( _gstate.total_activated_stake >= min_activated_stake,
      //              "cannot undelegate bandwidth until the chain is activated (at least 15% of all tokens participate in voting)" );

      changebw( from, receiver, -unstake_net_quantity, -unstake_cpu_quantity, false);
   } // undelegatebw


   void system_contract::refund( const account_name owner ) {
      require_auth( owner );

      refunds_table refunds_tbl( _self, owner );
      auto req = refunds_tbl.find( owner );
      eosio_assert( req != refunds_tbl.end(), "refund request not found" );
      eosio_assert( req->request_time + refund_delay <= now(), "refund is not available yet" );
      // Until now() becomes NOW, the fact that now() is the timestamp of the previous block could in theory
      // allow people to get their tokens earlier than the 3 day delay if the unstake happened immediately after many
      // consecutive missed blocks.

      INLINE_ACTION_SENDER(eosio::token, transfer)( N(eosio.token), {N(eosio.stake),N(active)},
                                                    { N(eosio.stake), req->owner, req->net_amount + req->cpu_amount + req->ram_amount, std::string("unstake") } );

      refunds_tbl.erase( req );
   }


} //namespace eosiosystem
