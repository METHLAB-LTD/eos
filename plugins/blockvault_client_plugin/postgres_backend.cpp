#include "postgres_backend.hpp"
#include <boost/filesystem.hpp>
#include <fstream>
#include <iostream>
#include <limits.h>
#include <unistd.h>

namespace eosio {
namespace blockvault {

postgres_backend::postgres_backend(const std::string& options)
    : conn(options) {
   
   try {
      pqxx::work w(conn);
      w.exec("CREATE TABLE IF NOT EXISTS BlockData (watermark_bn bigint, watermark_ts bigint, lib bigint, block_num bigint, "
            "block_id bytea UNIQUE, previous_block_id bytea, block oid, block_size bigint);"
            "CREATE TABLE IF NOT EXISTS SnapshotData (watermark_bn bigint, watermark_ts bigint, snapshot oid);");
      w.commit();
   }
   catch (const pqxx::integrity_constraint_violation&) {
      // this would happen when multiple clients try to create the tables at the same time. The first one client should succeed, just ignore it. 
   }
   
   conn.prepare("serialize_transaction", "SET TRANSACTION ISOLATION LEVEL SERIALIZABLE;");

   conn.prepare(
       "insert_constructed_block",
       "INSERT INTO BlockData (watermark_bn, watermark_ts, lib, block_num, block_id, previous_block_id, block, block_size) "
       "SELECT $1, $2, $3, $1, $4, $5, $6, $7  WHERE NOT "
       "EXISTS (SELECT * FROM BlockData WHERE (watermark_bn >= $1) OR (watermark_ts >= $2) OR (lib > $3))");

   conn.prepare(
       "insert_external_block",
       "INSERT INTO BlockData (watermark_bn, watermark_ts, lib, block_num, block_id, previous_block_id, block, block_size) SELECT "
       "COALESCE((SELECT MAX(watermark_bn) FROM BlockData),0), COALESCE((SELECT MAX(watermark_ts) FROM "
       "BlockData),0), $2, $1, $3, $4, $5, $6 WHERE NOT "
       "EXISTS (SELECT * FROM BlockData WHERE lib >= $1)");

   conn.prepare("get_block_insertion_result", "SELECT block from BlockData WHERE block=$1");

   conn.prepare("insert_snapshot",
                "INSERT INTO SnapshotData (watermark_bn, watermark_ts, snapshot) SELECT $1, $2, $3 WHERE NOT EXISTS "
                "(SELECT * FROM SnapshotData WHERE watermark_bn >= $1 OR watermark_ts >= $2)");

   conn.prepare("get_snapshot_insertion_result", "SELECT snapshot from SnapshotData WHERE snapshot=$1");

   conn.prepare("get_sync_watermark", "SELECT watermark_bn, watermark_ts FROM BlockData WHERE "
                                      "previous_block_id = $1 ORDER BY watermark_bn, watermark_ts LIMIT 1");

   conn.prepare("get_latest_snapshot", "SELECT snapshot FROM SnapshotData "
                                       "ORDER BY watermark_bn DESC, watermark_ts DESC LIMIT 1");

   conn.prepare("get_blocks_since_watermark", "SELECT block, block_size FROM BlockData WHERE "
                                              "watermark_bn >= $1 AND watermark_ts >= $2"
                                              "ORDER BY block_num");

   conn.prepare("get_all_blocks", "SELECT block, block_size FROM BlockData");

   conn.prepare("delete_outdated_block_lo",
                "SELECT lo_unlink(r.block) FROM BlockData r WHERE watermark_bn <= $1 OR watermark_ts <= $2;");
   conn.prepare("delete_outdated_block_data", "DELETE FROM BlockData WHERE watermark_bn <= $1 OR watermark_ts <= $2;");
   conn.prepare("delete_outdated_snapshot_lo",
                "SELECT lo_unlink(r.snapshot) FROM SnapshotData r WHERE watermark_bn < $1 OR watermark_ts < $2;");
   conn.prepare("delete_outdated_snapshot_data",
                "DELETE FROM SnapshotData WHERE watermark_bn < $1 OR watermark_ts < $2;");

   conn.prepare("has_block", "SELECT COUNT(*) FROM BlockData WHERE block_id = $1");
} // namespace blockvault

std::basic_string_view<std::byte> to_byte_string_view(std::string_view v) {
   return std::basic_string_view<std::byte>(reinterpret_cast<const std::byte*>(v.data()), v.size());
} 

std::basic_string_view<std::byte> to_byte_string_view(const std::vector<char>& v) {
   return std::basic_string_view<std::byte>(reinterpret_cast<const std::byte*>(v.data()), v.size());
} 

bool postgres_backend::propose_constructed_block(std::pair<uint32_t, uint32_t> watermark, uint32_t lib,
                                                 const std::vector<char>& block_content, std::string_view block_id,
                                                 std::string_view previous_block_id) {
   try {
      pqxx::work w(conn);
      w.exec_prepared0("serialize_transaction");
      auto id{pqxx::blob::create(w)};
      
      w.exec_prepared0("insert_constructed_block", watermark.first, watermark.second, lib, to_byte_string_view(block_id),
                       to_byte_string_view(previous_block_id), id, block_content.size());
      auto r = w.exec_prepared("get_block_insertion_result", id);
      if (!r.empty()) {
         pqxx::blob b_w{pqxx::blob::open_w(w, id)};
         b_w.write(to_byte_string_view(block_content));
         w.commit();
         return true;
      }
   } catch (const pqxx::sql_error&) {
   } 

   return false;
}

bool postgres_backend::append_external_block(uint32_t block_num, uint32_t lib, const std::vector<char>& block_content,
                                             std::string_view block_id, std::string_view previous_block_id) {
   try {
      pqxx::work w(conn);
      w.exec_prepared0("serialize_transaction");
      auto id{pqxx::blob::create(w)};
      w.exec_prepared0("insert_external_block", block_num, lib, to_byte_string_view(block_id), to_byte_string_view(previous_block_id), id,
                       block_content.size());
      auto r = w.exec_prepared("get_block_insertion_result", id);
      if (!r.empty()) {
         pqxx::blob b_w{pqxx::blob::open_w(w, id)};
         b_w.write(to_byte_string_view(block_content));
         w.commit();
         return true;
      }
   } catch (const pqxx::sql_error&) {
   } 
   return false;
}

bool postgres_backend::propose_snapshot(std::pair<uint32_t, uint32_t> watermark, const char* snapshot_filename) {

   try {
      std::filebuf infile;

      infile.open(snapshot_filename, std::ios::in);

      pqxx::work              w(conn);
      w.exec_prepared0("serialize_transaction");
      auto id{pqxx::blob::create(w)};

      w.exec_prepared0("insert_snapshot", watermark.first, watermark.second, id);
      auto r = w.exec_prepared("get_snapshot_insertion_result", id);

      if (!r.empty()) {
         
         const int chunk_size = 4096;
         char      chunk[chunk_size];
         pqxx::blob b_w{pqxx::blob::open_w(w, id)};
         auto sz = chunk_size;
         while (sz == chunk_size) {
            sz = infile.sgetn(chunk, chunk_size);
            b_w.write(std::basic_string_view<std::byte>(reinterpret_cast<const std::byte*>(chunk), sz));
         };

         w.exec_prepared("delete_outdated_block_lo", watermark.first, watermark.second);
         w.exec_prepared("delete_outdated_block_data", watermark.first, watermark.second);
         w.exec_prepared("delete_outdated_snapshot_lo", watermark.first, watermark.second);
         w.exec_prepared("delete_outdated_snapshot_data", watermark.first, watermark.second);
      }

      w.commit();
      return !r.empty();

   } catch (const pqxx::transaction_rollback&) {
   }

   return false;
}

void retrieve_blocks(backend::sync_callback& callback, pqxx::work& trx, pqxx::result r) {
   std::basic_string<std::byte> block_data;

   for (const auto& x : r) {
      pqxx::oid               block_oid  = x[0].as<pqxx::oid>();
      uint64_t                block_size = x[1].as<uint64_t>();
      pqxx::blob b_r{pqxx::blob::open_r(trx, block_oid)};
      block_data.resize(block_size);
      b_r.read(block_data, block_size);
      callback.on_block(std::string_view{reinterpret_cast<char*>(block_data.data()), block_data.size()});
   }

   trx.commit();
}

void postgres_backend::sync(std::string_view previous_block_id, backend::sync_callback& callback) {
   pqxx::work trx(conn);

   if (previous_block_id.size()) {
      auto bid = to_byte_string_view(previous_block_id);
      auto r = trx.exec_prepared("get_sync_watermark", bid);

      if (!r.empty()) {
         retrieve_blocks(
             callback, trx,
             trx.exec_prepared("get_blocks_since_watermark", r[0][0].as<uint32_t>(), r[0][1].as<uint32_t>()));
         return;
      }

      auto row = trx.exec_prepared1("has_block", bid);
      if (row[0].as<int>() != 0)
         // in this case, the client is up-to-date, nothing to sync.  
         return;
   }

   auto r = trx.exec_prepared("get_latest_snapshot");

   if (!r.empty()) {

      fc::temp_file temp_file;
      std::string   fname = temp_file.path().string();

      pqxx::blob::to_file(trx, r[0][0].as<pqxx::oid>(),fname.c_str());
      callback.on_snapshot(fname.c_str());
   }

   retrieve_blocks(callback, trx, trx.exec_prepared("get_all_blocks"));
}

} // namespace blockvault
} // namespace eosio