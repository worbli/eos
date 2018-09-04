The following steps must be taken for the example script to work.

0. Create wallet
0. Create account for eosio.token
0. Create account for scott
0. Create account for exchange
0. Set token contract on eosio.token
0. Create EOS token
0. Issue initial tokens to scott

**Note**:
Deleting the `transactions.txt` file will prevent replay from working.


### Create wallet
`worbli wallet create`

### Create account steps
`worbli create key`

`worbli create key`

`worbli wallet import  --private-key <private key from step 1>`

`worbli wallet import  --private-key <private key from step 2>`

`worbli create account eosio <account_name> <public key from step 1> <public key from step 2>`

### Set contract steps
`worbli set contract eosio.token /contracts/eosio.token -p eosio.token@active`

### Create EOS token steps
`worbli push action eosio.token create '{"issuer": "eosio.token", "maximum_supply": "100000.0000 EOS", "can_freeze": 1, "can_recall": 1, "can_whitelist": 1}' -p eosio.token@active`

### Issue token steps
`worbli push action eosio.token issue '{"to": "scott", "quantity": "900.0000 EOS", "memo": "testing"}' -p eosio.token@active`
