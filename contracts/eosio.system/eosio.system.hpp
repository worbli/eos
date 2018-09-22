/**
 *  @file
 *  @copyright defined in eos/LICENSE.txt
 */
#pragma once

#include <eosio.system/native.hpp>
#include <eosiolib/asset.hpp>
#include <eosiolib/time.hpp>
#include <eosiolib/privileged.hpp>
#include <eosiolib/singleton.hpp>

#include <string>

namespace eosiosystem {

   using eosio::asset;
   using eosio::indexed_by;
   using eosio::const_mem_fun;
   using eosio::block_timestamp;

   struct eosio_global_state : eosio::blockchain_parameters {
      uint64_t free_ram()const { return max_ram_size - total_ram_bytes_reserved; }

      uint64_t             max_ram_size = 64ll*1024 * 1024 * 1024;
      uint64_t             total_ram_bytes_reserved = 0;
      int64_t              total_ram_stake = 0;

      block_timestamp      last_producer_schedule_update;
      uint64_t             last_inflation_calulation = 0;
      uint64_t             last_inflation_distribution = 0;
      int64_t              total_activated_stake = 0;
      uint64_t             thresh_activated_stake_time = 0;
      uint16_t             last_producer_schedule_size = 0;
      double               total_producer_vote_weight = 0; /// the sum of all producer votes
      bool                 is_producer_schedule_active = false;
      int64_t              inflation_bucket = 0;


      // explicit serialization macro is not necessary, used here only to improve compilation time
      EOSLIB_SERIALIZE_DERIVED( eosio_global_state, eosio::blockchain_parameters,
                                (max_ram_size)(total_ram_bytes_reserved)(total_ram_stake)
                                (last_producer_schedule_update)(last_inflation_calulation)
                                (last_inflation_distribution)(total_activated_stake)
                                (thresh_activated_stake_time)(last_producer_schedule_size)(total_producer_vote_weight)
                                (is_producer_schedule_active)(inflation_bucket) )
   };

   struct producer_info {
      account_name          owner;
      eosio::public_key     producer_key; /// a packed public key object
      bool                  is_active = false;
      std::string           url;
      uint32_t              produced_blocks = 0;
      uint64_t              last_claim_time = 0;
      uint16_t              location = 0;

      uint64_t primary_key()const { return owner;                                   }
      bool     active()const      { return is_active;                               }
      void     deactivate()       { producer_key = public_key(); is_active = false; }

      // explicit serialization macro is not necessary, used here only to improve compilation time
      EOSLIB_SERIALIZE( producer_info, (owner)(producer_key)(is_active)(url)
                        (produced_blocks)(last_claim_time)(location) )
   };

   struct producer_pay {
      account_name          owner;   
      uint64_t              earned_pay;   
      uint64_t              last_claim_time = 0;

      uint64_t primary_key()const { return owner; }
     
      // explicit serialization macro is not necessary, used here only to improve compilation time
      EOSLIB_SERIALIZE( producer_pay, (owner)(earned_pay)(last_claim_time) )
   };

   typedef eosio::multi_index< N(prodpay), producer_pay >  producer_pay_table;  

   typedef eosio::multi_index< N(producers), producer_info >  producers_table;

   typedef eosio::singleton<N(global), eosio_global_state> global_state_singleton;

   //   static constexpr uint32_t     max_inflation_rate = 5;  // 5% annual inflation
   static constexpr uint32_t     seconds_per_day = 24 * 3600;
   static constexpr uint64_t     system_token_symbol = CORE_SYMBOL;

   class system_contract : public native {
      private:
         producers_table        _producers;
         global_state_singleton _global;
         producer_pay_table     _producer_pay;

         eosio_global_state     _gstate;

      public:
         system_contract( account_name s );
         ~system_contract();

         // Actions:
         void onblock( block_timestamp timestamp, account_name producer );
                      // const block_header& header ); /// only parse first 3 fields of block header

         // functions defi ned in delegate_bandwidth.cpp

         /**
          *  Stakes SYS from the balance of 'from' for the benfit of 'receiver'.
          *  If transfer == true, then 'receiver' can unstake to their account
          *  Else 'from' can unstake at any time.
          */
         void delegatebw( account_name from, account_name receiver,
                          asset stake_net_quantity, asset stake_cpu_quantity, bool transfer );


         /**
          *  Decreases the total tokens delegated by from to receiver and/or
          *  frees the memory associated with the delegation if there is nothing
          *  left to delegate.
          *
          *  This will cause an immediate reduction in net/cpu bandwidth of the
          *  receiver.
          *
          *  A transaction is scheduled to send the tokens back to 'from' after
          *  the staking period has passed. If existing transaction is scheduled, it
          *  will be canceled and a new transaction issued that has the combined
          *  undelegated amount.
          *
          *  The 'from' account loses voting power as a result of this call and
          *  all producer tallies are updated.
          */
         void undelegatebw( account_name from, account_name receiver,
                            asset unstake_net_quantity, asset unstake_cpu_quantity );


         /**
          * Increases receiver's ram quota based upon current price and quantity of
          * tokens provided. An inline transfer from receiver to system contract of
          * tokens will be executed.
          */
         void buyram( account_name buyer, account_name receiver, asset tokens );
         void buyrambytes( account_name buyer, account_name receiver, uint32_t bytes );

         /**
          *  Reduces quota my bytes and then performs an inline transfer of tokens
          *  to receiver based upon the average purchase price of the original quota.
          */
         void sellram( account_name receiver, int64_t bytes );

         /**
          *  This action is called after the delegation-period to claim all pending
          *  unstaked tokens belonging to owner
          */
         void refund( account_name owner );

         // functions defined in voting.cpp

         void regproducer( const account_name producer, const public_key& producer_key, const std::string& url, uint16_t location );

         void unregprod( const account_name producer );

         void addproducer( const account_name producer );

         void togglesched( bool is_active );

         void setram( uint64_t max_ram_size );

         void setparams( const eosio::blockchain_parameters& params );

         // functions defined in producer_pay.cpp
         void claimrewards( const account_name& owner );

         void setpriv( account_name account, uint8_t ispriv );

         void rmvproducer( account_name producer );

         // worlbi admin
         void setprods( std::vector<eosio::producer_key> schedule );
      private:
         void update_producers( block_timestamp timestamp );
         // Implementation details:

         //defind in delegate_bandwidth.cpp
         void changebw( account_name from, account_name receiver,
                        asset stake_net_quantity, asset stake_cpu_quantity, bool transfer );
        
         void updateram( account_name payer, account_name receiver, uint32_t bytes, asset quant);

         //defined in voting.hpp
         static eosio_global_state get_default_parameters();

   };

} /// eosiosystem
