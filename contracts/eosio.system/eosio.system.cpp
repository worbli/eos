#include "eosio.system.hpp"
#include <eosiolib/dispatcher.hpp>

#include "producer_pay.cpp"
#include "delegate_bandwidth.cpp"
#include "voting.cpp"


namespace eosiosystem {

   system_contract::system_contract( account_name s )
   :native(s),
    _producers(_self,_self),
    _global(_self,_self),
    _producer_pay(_self,_self)

   {
      //print( "construct system\n" );
      _gstate = _global.exists() ? _global.get() : get_default_parameters();
   }

   eosio_global_state system_contract::get_default_parameters() {
      eosio_global_state dp;
      get_blockchain_parameters(dp);
      return dp;
   }


   system_contract::~system_contract() {
      //print( "destruct system\n" );
      _global.set( _gstate, _self );
      //eosio_exit(0);
   }

   void system_contract::setram( uint64_t max_ram_size ) {
      require_auth( _self );

      eosio_assert( _gstate.max_ram_size < max_ram_size, "ram may only be increased" ); /// decreasing ram might result market maker issues
      eosio_assert( max_ram_size < 1024ll*1024*1024*1024*1024, "ram size is unrealistic" );
      eosio_assert( max_ram_size > _gstate.total_ram_bytes_reserved, "attempt to set max below reserved" );

      /**
       *  Increase or decrease the amount of ram for sale based upon the change in max
       *  ram size.
       */

      _gstate.max_ram_size = max_ram_size;
      _global.set( _gstate, _self );
   }

   void system_contract::setusagelvl( uint8_t new_level ) {
      require_auth( _self );

      eosio_assert( _gstate.network_usage_level < new_level, "usage level may only be increased" ); 
      eosio_assert( new_level <= 100, "usage level cannot excced 100" );
      eosio_assert( new_level > 0, "usage level cannot be negative" );

      _gstate.network_usage_level = new_level;
      _global.set( _gstate, _self );
   }

   void system_contract::setparams( const eosio::blockchain_parameters& params ) {
      require_auth( N(eosio) );
      (eosio::blockchain_parameters&)(_gstate) = params;
      eosio_assert( 3 <= _gstate.max_authority_depth, "max_authority_depth should be at least 3" );
      set_blockchain_parameters( params );
   }

   void system_contract::setpriv( account_name account, uint8_t ispriv ) {
      require_auth( _self );
      set_privileged( account, ispriv );
   }

   void system_contract::rmvproducer( account_name producer ) {
      require_auth( _self );
      auto prod = _producers.find( producer );
      eosio_assert( prod != _producers.end(), "producer not found" );
      _producers.modify( prod, 0, [&](auto& p) {
            p.deactivate();
         });
   }

   // worbli admin
   void system_contract::setprods( std::vector<eosio::producer_key> schedule ) {
      (void)schedule; // schedule argument just forces the deserialization of the action data into vector<producer_key> (necessary check)
      require_auth( _self );

      constexpr size_t max_stack_buffer_size = 512;
      size_t size = action_data_size();
      char* buffer = (char*)( max_stack_buffer_size < size ? malloc(size) : alloca(size) );
      read_action_data( buffer, size );
      set_proposed_producers(buffer, size);
   }


   /**
    *  Called after a new account is created. This code enforces resource-limits rules
    *  for new accounts as well as new account naming conventions.
    *
    *  Account names containing '.' symbols must have a suffix equal to the name of the creator.
    *  This allows users who buy a premium name (shorter than 12 characters with no dots) to be the only ones
    *  who can create accounts with the creator's name as a suffix.
    *
    */
   void native::newaccount( account_name     creator,
                            account_name     newact
                            /*  no need to parse authorities
                            const authority& owner,
                            const authority& active*/ ) {
      require_auth( _self );

      if( creator != _self ) {
         auto tmp = newact >> 4;
         bool has_dot = false;

         for( uint32_t i = 0; i < 12; ++i ) {
           has_dot |= !(tmp & 0x1f);
           tmp >>= 5;
         }
         if( has_dot ) { // or is less than 12 characters
            auto suffix = eosio::name_suffix(newact);
            eosio_assert( suffix =! newact, "account names must be 12 characters" );        
            eosio_assert( creator == suffix, "only suffix may create this account" );       
         }
      }

      user_resources_table  userres( _self, newact);

      userres.emplace( newact, [&]( auto& res ) {
        res.owner = newact;
      });

      set_resource_limits( newact, 0, 0, 0 );
   }

} /// eosio.system


EOSIO_ABI( eosiosystem::system_contract,
     // native.hpp (newaccount definition is actually in eosio.system.cpp)
     (newaccount)(updateauth)(deleteauth)(linkauth)(unlinkauth)(canceldelay)(onerror)
     // eosio.system.cpp
     (setram)(setparams)(setpriv)(rmvproducer)(setusagelvl)
     // delegate_bandwidth.cpp
     (buyrambytes)(buyram)(sellram)(delegateram)(delegatebw)(undelegatebw)(refund)
     // voting.cpp
     (regproducer)(unregprod)(addproducer)(togglesched)
     // producer_pay.cpp
     (onblock)(claimrewards)
     // worbli admin
     (setprods)
)
