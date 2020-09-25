#pragma once
#warning TODO: Replace this file with db_undo_session.hpp. Use variant.
#include <b1/chain_kv/chain_kv.hpp>
#include <chainbase/chainbase.hpp>
#include <eosio/chain/authorization_manager.hpp>
#include <eosio/chain/block_state.hpp>
#include <eosio/chain/chain_snapshot.hpp>
#include <eosio/chain/fork_database.hpp>
#include <eosio/chain/genesis_state.hpp>
#include <eosio/chain/resource_limits.hpp>
#include <eosio/chain/snapshot.hpp>

#include <eosio/chain/account_object.hpp>
#include <eosio/chain/backing_store.hpp>
#include <eosio/chain/block_summary_object.hpp>
#include <eosio/chain/code_object.hpp>
#include <eosio/chain/contract_table_objects.hpp>
#include <eosio/chain/database_header_object.hpp>
#include <eosio/chain/generated_transaction_object.hpp>
#include <eosio/chain/genesis_intrinsics.hpp>
#include <eosio/chain/global_property_object.hpp>
#include <eosio/chain/kv_chainbase_objects.hpp>
#include <eosio/chain/protocol_state_object.hpp>
#include <eosio/chain/reversible_block_object.hpp>
#include <eosio/chain/transaction_object.hpp>
#include <eosio/chain/whitelisted_intrinsics.hpp>

#include <b1/session/cache.hpp>
#include <b1/session/rocks_session.hpp>
#include <b1/session/session.hpp>
#include <b1/session/undo_stack.hpp>

// It's a fatal condition if chainbase and chain_kv get out of sync with each
// other due to exceptions.
#define CATCH_AND_EXIT_DB_FAILURE()                                                                                    \
   catch (...) {                                                                                                       \
      elog("Error while using database");                                                                              \
      std::abort();                                                                                                    \
   }

namespace eosio { namespace chain {
   using rocks_db_type = eosio::session::session<eosio::session::rocksdb_t>;
   using session_type = eosio::session::session<rocks_db_type>;

   using controller_index_set =
         index_set<account_index, account_metadata_index, account_ram_correction_index, global_property_multi_index,
                   protocol_state_multi_index, dynamic_global_property_multi_index, block_summary_multi_index,
                   transaction_multi_index, generated_transaction_multi_index, table_id_multi_index, code_index,
                   database_header_multi_index, kv_db_config_index, kv_index>;

   using contract_database_index_set = index_set<key_value_index, index64_index, index128_index, index256_index,
                                                 index_double_index, index_long_double_index>;

   // TODO:  What is combined_session used for?
   class combined_session {
    public:
      combined_session() = default;

      combined_session(chainbase::database& cb_database, eosio::session::undo_stack<rocks_db_type>* undo_stack);

      combined_session(combined_session&& src) noexcept;

      ~combined_session() { undo(); }

      combined_session& operator=(const combined_session& src) = delete;

      void push();

      void squash();

      void undo();

    private:
      std::unique_ptr<chainbase::database::session> cb_session    = {};
      eosio::session::undo_stack<rocks_db_type>*     kv_undo_stack = nullptr;
   };

   class combined_database {
    public:
      combined_database(chainbase::database& chain_db);

      combined_database(chainbase::database& chain_db,
                        const std::string& rocksdb_path,
                        bool rocksdb_create_if_missing,
                        uint32_t rocksdb_threads,
                        int rocksdb_max_open_files);

      combined_database(const combined_database& copy) = delete;
      combined_database& operator=(const combined_database& copy) = delete;

      // Save the backing_store setting to the chainbase in order to detect
      // when this setting is switched from CHAINBASE to ROCKSDB, in which
      // case, check that no KV entries already exist in the chainbase.
      // Otherwise, they would become unreachable.
      void check_backing_store_setting();

      static combined_session make_no_op_session() { return combined_session(); }

      combined_session make_session() {
         return combined_session(db, kv_undo_stack.get());
      }

      void set_revision(uint64_t revision);

      void undo();

      void commit(int64_t revision);

      void flush();

      std::unique_ptr<kv_context> create_kv_context(name receiver, kv_resource_manager resource_manager,
                                                    const kv_database_config& limits);

      void add_to_snapshot(const eosio::chain::snapshot_writer_ptr& snapshot, const eosio::chain::block_state& head,
                           const eosio::chain::authorization_manager&                    authorization,
                           const eosio::chain::resource_limits::resource_limits_manager& resource_limits) const;

      void read_from_snapshot(const snapshot_reader_ptr& snapshot, uint32_t blog_start, uint32_t blog_end,
                              eosio::chain::authorization_manager& authorization,
                              eosio::chain::resource_limits::resource_limits_manager& resource_limits,
                              eosio::chain::fork_database& fork_db, eosio::chain::block_state_ptr& head,
                              uint32_t& snapshot_head_block, const eosio::chain::chain_id_type& chain_id);

    private:
      void add_contract_tables_to_snapshot(const snapshot_writer_ptr& snapshot) const;
      void read_contract_tables_from_snapshot(const snapshot_reader_ptr& snapshot);

      backing_store_type                                         backing_store;
      chainbase::database&                                       db;
      std::unique_ptr<rocks_db_type>                             kv_database;
      std::unique_ptr<eosio::session::undo_stack<rocks_db_type>> kv_undo_stack;
   };

   std::optional<eosio::chain::genesis_state> extract_legacy_genesis_state(snapshot_reader& snapshot, uint32_t version);

   std::vector<char> make_rocksdb_undo_prefix();
   std::vector<char> make_rocksdb_contract_kv_prefix();

}} // namespace eosio::chain
