// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

// Cache/diagnostic unit checks only. Durable equivalence is verified by the
// separate real client/listener/parser/server differential gate.
#include "dml/direct_bulk_append_cache.hpp"
#include "dml/insert_batch.hpp"
#include "index_key_encoding.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <iostream>
#include <map>
#include <set>
#include <string>

namespace api = scratchbird::engine::internal_api;
namespace cache = api::dml::detail;
namespace idx = scratchbird::core::index;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

void Require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

std::string TypedKey(const std::string& index_uuid, unsigned char value) {
  idx::IndexKeyEncodingComponent component;
  const auto identity = uuid::ParseTypedUuid(UuidKind::object, index_uuid);
  Require(identity.ok(), "test index identity invalid");
  component.type_descriptor_uuid = identity.value;
  component.payload = {0x80, 0, 0, 0, 0, 0, 0, value};
  const auto encoded = idx::EncodeIndexKey({component}, {});
  Require(encoded.ok(), "test typed key encoding failed");
  std::string text = "SBKOHEX:";
  constexpr char digits[] = "0123456789abcdef";
  for (const auto byte : encoded.encoded) {
    text += digits[byte >> 4];
    text += digits[byte & 15];
  }
  return text;
}

int main() {
  api::CrudIndexRecord index;
  index.index_uuid = "019f3000-0000-7000-8000-000000000201";
  index.table_uuid = "019f3000-0000-7000-8000-000000000101";
  index.column_name = "id";
  index.key_envelopes = {"id", "unique"};
  index.unique = true;
  index.family = api::kCrudIndexFamilyBtree;
  index.profile = api::kCrudIndexProfileRowStoreScalarBtreeV1;
  api::EngineRequestContext context;
  context.local_transaction_id = 7;
  api::MgaRelationReadView view;
  view.transactions[7] = "active";
  const auto entry = [&](std::string key, std::string value) {
    api::CrudIndexEntryRecord result;
    result.creator_tx = 7;
    result.index_uuid = index.index_uuid;
    result.table_uuid = index.table_uuid;
    result.key_value = std::move(key);
    result.payload_value = std::move(value);
    return result;
  };
  const auto typed8 = TypedKey(index.index_uuid, 8);
  cache::DirectStoreAppendIndexEntryCache(context, index.table_uuid, 2, view,
                                        {entry("6", "6"), entry(typed8, "8")});
  std::map<std::string, std::set<std::string>> keys;
  std::map<std::string, std::map<std::string, api::CrudIndexEntryRecord>> entries;
  Require(cache::DirectBuildAppendIndexConflictCaches(context, index.table_uuid, 2,
      {index}, {{{"id", "6"}}, {{"id", "8"}}, {{"id", "9"}}}, &keys, &entries),
      "mixed key cache lookup failed");
  Require(keys[index.index_uuid] == std::set<std::string>{"6", "8"},
          "raw key match hid a typed key conflict");
  Require(entries[index.index_uuid].contains("6") &&
          entries[index.index_uuid].contains(typed8), "physical entries were lost");
  keys.clear();
  Require(cache::DirectBuildAppendIndexConflictCaches(context, index.table_uuid, 2,
      {index}, {}, &keys, nullptr), "typed-only lookup failed");
  Require(keys[index.index_uuid] == std::set<std::string>{"6", "8"},
          "typed-only lookup did not return the complete logical key set");

  api::MgaExactIndexEntryAppendBatch batch;
  batch.index = index;
  batch.table_uuid = index.table_uuid;
  batch.entries.push_back({TypedKey(index.index_uuid, 9), "9", {}, {}});
  cache::DirectAppendIndexBatchesToCache(context, index.table_uuid, 2, 1,
                                        {batch}, {}, false);
  keys.clear();
  Require(cache::DirectBuildAppendIndexConflictCaches(context, index.table_uuid, 3,
      {index}, {{{"id", "9"}}}, &keys, nullptr), "incremental lookup failed");
  Require(keys[index.index_uuid] == std::set<std::string>{"9"},
          "projection missed a newly appended typed key");
  keys.clear();
  Require(!cache::DirectBuildAppendIndexConflictCaches(context, index.table_uuid, 2,
      {index}, {}, &keys, nullptr), "stale row count accepted");
  auto other = context;
  other.local_transaction_id = 8;
  Require(!cache::DirectBuildAppendIndexConflictCaches(other, index.table_uuid, 3,
      {index}, {}, &keys, nullptr), "another transaction accepted the cache");
  cache::DirectStoreAppendIndexEntryCache(context, index.table_uuid, 1, view,
                                        {entry("6", "6")});
  Require(cache::DirectBuildAppendIndexConflictCaches(context, index.table_uuid, 1,
      {index}, {}, &keys, nullptr), "replacement cache lookup failed");
  Require(keys[index.index_uuid] == std::set<std::string>{"6"},
          "replacement cache retained stale logical keys");

  // Eviction after proof but before publication cannot turn an append delta
  // into a complete cache for a nonempty relation.
  cache::DirectEvictAppendIndexEntryCache(context, index.table_uuid);
  cache::DirectAppendIndexBatchesToCache(context, index.table_uuid, 1, 1, {}, {});
  Require(!cache::DirectAppendIndexEntryCacheAvailable(context, index.table_uuid, 2),
          "cache loss certified an incomplete append delta");
  // A genuinely empty baseline needs no prior entries and may be initialized.
  cache::DirectAppendIndexBatchesToCache(context, index.table_uuid, 0, 0, {}, {});
  Require(cache::DirectAppendIndexEntryCacheAvailable(context, index.table_uuid, 0),
          "empty cache initialization refused");
  cache::DirectEvictAppendIndexEntryCache(context, index.table_uuid);
  cache::DirectAppendIndexEntriesToCache(context, index.table_uuid, 1, 1,
                                        {entry("9", "9")});
  Require(!cache::DirectAppendIndexEntryCacheAvailable(context, index.table_uuid, 2),
          "scalar cache delta hid missing prior entries");
  cache::DirectAppendIndexEntriesToCache(context, index.table_uuid, 0, 1,
                                        {entry("9", "9")});
  keys.clear();
  Require(cache::DirectBuildAppendIndexConflictCaches(context, index.table_uuid, 1,
      {index}, {}, &keys, nullptr) && keys[index.index_uuid] == std::set<std::string>{"9"},
      "scalar cache empty-baseline rebuild failed");

  api::CrudTableRecord table;
  table.table_uuid = index.table_uuid;
  table.columns = {{"id", "canonical=int64;primary_key=false"}};
  for (const bool primary : {false, true}) {
    if (primary) index.key_envelopes.push_back("primary_key");
    api::InsertBatchContext batch_context;
    batch_context.index_plan.entries.push_back({index});
    Require(!api::ValidateInsertBatchUniquePreflight(&batch_context, table,
                {{"id", "6"}}).error, "first key refused");
    const auto duplicate = api::ValidateInsertBatchUniquePreflight(
        &batch_context, table, {{"id", "6"}});
    const auto expected = api::UniqueConflictDiagnostic(table, index);
    Require(duplicate.error && duplicate.code == expected.code &&
            duplicate.message_key == expected.message_key &&
            duplicate.detail == expected.detail, "constraint diagnostics differ");
    Require(duplicate.code == (primary ? "CLI.CONSTRAINT_PRIMARY_KEY_VIOLATION"
                                      : "CLI.CONSTRAINT_UNIQUE_VIOLATION"),
            "wrong constraint class");
  }
  index.key_envelopes = {"id", "unique"};
  table.columns = {{"id", "canonical=int64;primary_key=true"}};
  Require(api::UniqueConflictDiagnostic(table, index).code ==
              "CLI.CONSTRAINT_PRIMARY_KEY_VIOLATION",
          "bound primary-key descriptor was not classified");
  std::cout << "dml_unique_key_equivalence_unit=passed\n";
}
