#include "../support/engine_evidence_fixture.hpp"
#include "../support/engine_statement_fixture.hpp"
#include "../support/published_ddl_table_fixture.hpp"
#include "../support/uuid_index_key_oracle.hpp"
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_dirty_manifest.hpp"
#include "api_diagnostics.hpp"
#include "database_format.hpp"
#include "database_lifecycle.hpp"
#include "datatype_operations.hpp"
#include "sbl_numeric.hpp"
#include "index_key_encoding.hpp"
#include "dml/native_bulk_ingest_api.hpp"
#include "dml/select_api.hpp"
#include "dml/mga_relation_read_view.hpp"
#include "memory.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "physical_mga_cow_store.hpp"
#include "hash_digest.hpp"
#include "sblr_dispatch_server.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "sblr_opcode_stream.hpp"
#include "sblr_opcode_registry.hpp"
#include "sblr_to_sbsql.hpp"
#include "scratchbird/engine/sblr_envelope.hpp"
#include "session_registry.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"
#include "wire/public_result_packet.hpp"
#include "wire/parser_server_ipc/parser_server_client.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace memory = scratchbird::core::memory;
namespace platform = scratchbird::core::platform;
namespace server = scratchbird::server;
namespace sbps = scratchbird::server::sbps;
namespace sblr = scratchbird::engine::sblr;
namespace uuid = scratchbird::core::uuid;
namespace dt = scratchbird::core::datatypes;

[[noreturn]] void Fail(std::string_view message) {
  throw std::runtime_error(std::string(message));
}

void Require(bool condition, std::string_view message) {
  if (!condition) Fail(message);
}

api::CrudStoredValue ScalarLogicalPayload(std::string_view frame) {
  // Independent SBCLKEY2 oracle: one component, explicit state, exact length.
  // Do not use the production decoder to validate the production encoder.
  Require(frame.size() >= 17 && frame.substr(0, 8) == "SBCLKEY2" &&
              frame[8] == 1 && frame[9] == 0 && frame[10] == 0 && frame[11] == 0,
          "CDP-040 scalar logical payload frame header/arity invalid");
  const auto state = static_cast<unsigned char>(frame[12]);
  std::uint32_t width = 0;
  for (unsigned i = 0; i != 4; ++i)
    width |= static_cast<std::uint32_t>(static_cast<unsigned char>(frame[13 + i])) << (8 * i);
  Require(state <= 1 && width == frame.size() - 17 && (state == 0 || width == 0),
          "CDP-040 scalar logical payload state/length invalid");
  return {static_cast<api::EngineValueState>(state), std::string(frame.substr(17))};
}

memory::AllocationPolicy MemoryPolicy() {
  memory::AllocationPolicy policy;
  policy.policy_name = "cdp_native_bulk_ingest_api_gate";
  policy.hard_limit_bytes = 64ull * 1024ull * 1024ull;
  policy.soft_limit_bytes = 48ull * 1024ull * 1024ull;
  policy.per_context_limit_bytes = 32ull * 1024ull * 1024ull;
  policy.page_buffer_pool_limit_bytes = 16ull * 1024ull * 1024ull;
  policy.track_allocations = true;
  policy.zero_memory_on_release = true;
  return policy;
}

void ConfigureMemoryFixture() {
  const auto configured = memory::ConfigureDefaultMemoryManagerForFixture(
      MemoryPolicy(), "cdp_native_bulk_ingest_api_gate");
  Require(configured.ok(), "CDP-040 memory fixture configuration failed");
  Require(configured.fixture_mode, "CDP-040 memory fixture mode was not active");
}

template <typename TResult>
void RequireOk(const TResult& result, std::string_view message) {
  if (!result.ok) {
    if (!result.diagnostics.empty()) {
      std::cerr << result.diagnostics.front().code << ':'
                << result.diagnostics.front().detail << '\n';
    }
    Fail(message);
  }
}

platform::u64 NowMillis() {
  return static_cast<platform::u64>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

platform::u64 UniqueMillis() {
  static platform::u64 counter = 0;
  return NowMillis() + (++counter * 1000);
}

platform::TypedUuid NewTypedUuid(platform::UuidKind kind, platform::u64 salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, UniqueMillis() + salt);
  Require(generated.ok(), "CDP-040 UUID generation failed");
  return generated.value;
}

api::EngineUuid NewIdentity(platform::UuidKind kind, platform::u64 salt) {
  return NewTypedUuid(kind, salt).value;
}


struct Fixture {
  std::filesystem::path dir;
  std::filesystem::path database_path;
  api::EngineUuid database_uuid;
  api::EngineUuid table_uuid;
  api::EngineUuid index_uuid;
  platform::u64 salt = 0;
  mutable platform::u64 physical_page = 0;
  platform::u32 page_size = 0;
  api::EngineRequestContext owner_context;
  std::shared_ptr<scratchbird::tests::FixtureEngineSession> session;

  Fixture() = default;
  Fixture(const Fixture&) = delete;
  Fixture& operator=(const Fixture&) = delete;
  Fixture(Fixture&& other)
      : dir(std::exchange(other.dir, {})), database_path(std::move(other.database_path)),
        database_uuid(other.database_uuid), table_uuid(other.table_uuid),
        index_uuid(other.index_uuid), salt(other.salt), physical_page(other.physical_page), page_size(other.page_size),
        owner_context(std::move(other.owner_context)), session(std::move(other.session)) {}

  ~Fixture() {
    session.reset();
    if (!dir.empty()) {
      std::error_code ignored;
      std::filesystem::remove_all(dir, ignored);
    }
  }
};

void InitializeOwnedFixtureDirectory(Fixture& fixture, const std::string& name) {
  const auto path = std::filesystem::temp_directory_path() /
      ("scratchbird_cdp040_" + name + "_" +
       uuid::UuidToString(NewIdentity(platform::UuidKind::object, 1)));
  Require(std::filesystem::create_directory(path),
          "CDP-040 fresh owned fixture directory creation failed");
  fixture.dir = path;
  fixture.database_path = path / "cdp040.sbdb";
}

api::EngineTypedValue TextValue(std::string value) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "character";
  typed.descriptor.encoded_descriptor = "canonical=character";
  typed.encoded_value = std::move(value);
  return typed;
}

api::EngineTypedValue ScalarValue(std::string canonical_type_name,
                                  std::string value) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = canonical_type_name;
  typed.descriptor.encoded_descriptor = "canonical=" + canonical_type_name;
  const bool signed_integer = canonical_type_name == "int8" ||
      canonical_type_name == "int16" || canonical_type_name == "int32" ||
      canonical_type_name == "int64" || canonical_type_name == "int128";
  const bool unsigned_integer = canonical_type_name == "uint8" ||
      canonical_type_name == "uint16" || canonical_type_name == "uint32" ||
      canonical_type_name == "uint64" || canonical_type_name == "uint128";
  if (signed_integer || unsigned_integer) {
    const auto width_name = canonical_type_name.substr(signed_integer ? 3 : 4);
    const std::size_t width = width_name == "8" ? 1 : width_name == "16" ? 2 :
        width_name == "32" ? 4 : width_name == "64" ? 8 : width_name == "128" ? 16 : 0;
    Require(width != 0 && !value.empty(), "invalid integer fixture type/value");
    const bool negative = value.front() == '-';
    Require(!negative || signed_integer, "negative unsigned fixture value");
    typed.binary_value.assign(width, 0);
    Require(value.size() > (negative ? 1u : 0u), "empty integer fixture magnitude");
    // Independent decimal-to-LE fixture construction, including 128-bit bounds;
    // no engine parser, native-width extension or production codec as oracle.
    for (std::size_t index = negative ? 1 : 0; index < value.size(); ++index) {
      Require(value[index] >= '0' && value[index] <= '9', "nondecimal integer fixture");
      unsigned carry = value[index] - '0';
      for (auto& byte : typed.binary_value) {
        const unsigned next = byte * 10u + carry;
        byte = static_cast<std::uint8_t>(next);
        carry = next >> 8;
      }
      Require(carry == 0, "integer fixture overflow");
    }
    if (signed_integer) {
      const auto high = typed.binary_value.back();
      Require(high < 0x80 || (negative && high == 0x80 &&
          std::all_of(typed.binary_value.begin(), typed.binary_value.end() - 1,
                      [](auto byte) { return byte == 0; })), "signed fixture overflow");
    }
    if (negative) {
      unsigned carry = 1;
      for (auto& byte : typed.binary_value) {
        const unsigned next = static_cast<unsigned>(static_cast<std::uint8_t>(~byte)) + carry;
        byte = static_cast<std::uint8_t>(next);
        carry = next >> 8;
      }
    }
  } else if (canonical_type_name == "boolean") {
    Require(value == "true" || value == "false", "invalid boolean fixture value");
    typed.binary_value = {static_cast<std::uint8_t>(value == "true")};
  } else {
    typed.encoded_value = std::move(value);
  }
  return typed;
}

std::int64_t ReadInt64Payload(const api::CrudStoredValue& value) {
  Require(value.isPresent() && value.bytes.size() == 8,
          "CDP-040 retained int64 payload is not PRESENT binary LE8");
  std::uint64_t bits = 0;
  for (unsigned index = 0; index < 8; ++index)
    bits |= static_cast<std::uint64_t>(static_cast<unsigned char>(value.bytes[index])) << (index * 8);
  return std::bit_cast<std::int64_t>(bits);
}

api::EngineTypedValue BinaryScalarValue(std::string canonical_type_name,
                                        std::vector<std::uint8_t> value) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = canonical_type_name;
  typed.descriptor.encoded_descriptor = "canonical=" + canonical_type_name;
  typed.binary_value = std::move(value);
  return typed;
}

api::EngineTypedValue IdentityScalarValue(std::string type, const platform::Uuid& value) {
  return BinaryScalarValue(std::move(type), {value.bytes.begin(), value.bytes.end()});
}

api::EngineTypedValue NativeRealFixtureValue(std::string type, std::uint64_t bits, unsigned width) {
  std::vector<std::uint8_t> bytes;
  for (unsigned i = 0; i < width; ++i) bytes.push_back(bits >> (8 * i));
  return BinaryScalarValue(std::move(type), std::move(bytes));
}

api::EngineTypedValue NullValue(std::string canonical_type_name) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = canonical_type_name;
  typed.descriptor.encoded_descriptor = "canonical=" + canonical_type_name;
  typed.is_null = true;
  return typed;
}

api::EngineRowValue Row(std::string id, std::string payload) {
  api::EngineRowValue row;
  row.requested_row_uuid = NewIdentity(platform::UuidKind::object, 900);
  row.fields.push_back({"id", TextValue(std::move(id))});
  row.fields.push_back({"payload", TextValue(std::move(payload))});
  return row;
}

bool HasEvidence(const std::vector<api::EngineEvidenceReference>& evidence,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& item : evidence) {
    if (item.evidence_kind == kind && scratchbird::tests::EvidenceTextEquals(item.evidence_id, id)) return true;
  }
  return false;
}

api::EngineApiU64 EvidenceU64(const std::vector<api::EngineEvidenceReference>& evidence,
                              std::string_view kind) {
  for (const auto& item : evidence) {
    if (item.evidence_kind != kind) { continue; }
    try {
      return static_cast<api::EngineApiU64>(std::stoull(std::get<std::string>(item.evidence_id)));
    } catch (...) {
      return 0;
    }
  }
  return 0;
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

void RequireDiagnostic(const api::EngineApiResult& result,
                       std::string_view expected_code,
                       std::string_view expected_detail,
                       std::string_view message) {
  Require(!result.ok, message);
  Require(!result.diagnostics.empty(), message);
  const auto& diagnostic = result.diagnostics.front();
  if (diagnostic.code != expected_code) {
    std::cerr << "expected=" << expected_code << " actual=" << diagnostic.code << '\n';
  }
  Require(diagnostic.code == expected_code, message);
  if (!expected_detail.empty()) {
    if (diagnostic.detail.find(expected_detail) == std::string::npos) {
      std::cerr << "expected_detail=" << expected_detail << " actual_detail=" << diagnostic.detail << '\n';
    }
    Require(diagnostic.detail.find(expected_detail) != std::string::npos, message);
  }
}

api::EngineRequestContext BaseContext(const Fixture& fixture, std::string request_id) {
  auto context = fixture.owner_context;
  context.request_id = std::move(request_id);
  context.identifier_profile_uuid = "sbsql_v3";
  context.language_context.language_tag = "en";
  context.language_context.default_language_tag = "en";
  return context;
}

// Publish actual native column identities using the explicitly declared types;
// the compatibility descriptor strings are never reconstructed as UUID authority.
api::EngineApiDiagnostic PublishFixtureTable(
    api::EngineRequestContext& context, const api::CrudTableRecord& table) {
  std::vector<std::string> types;
  for (const auto& column : table.columns) {
    api::CatalogColumnMetadata metadata;
    Require(api::AdmitCatalogColumnMetadata(column.second, &metadata),
            "CDP-040 fixture column metadata invalid");
    const auto type = metadata.text.find("canonical");
    Require(type != metadata.text.end(), "CDP-040 fixture column type missing");
    types.push_back(type->second);
  }
  const auto published = scratchbird::tests::PublishDdlTableFixture(context, table, types);
  Require(published.table_uuid == table.table_uuid && published.columns.size() == table.columns.size(),
          "CDP-040 actual DDL table publication changed identity or shape");
  return api::MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

void StartFixtureSession(Fixture& fixture, const api::EngineRequestContext& metadata) {
  fixture.owner_context.current_schema_uuid = metadata.current_schema_uuid;
  fixture.session = std::make_shared<scratchbird::tests::FixtureEngineSession>(
      BaseContext(fixture, "cdp040-session"));
}

api::EngineRequestContext Begin(const Fixture& fixture, std::string request_id) {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(fixture, std::move(request_id));
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
  RequireOk(begun, "CDP-040 begin transaction failed");
  auto context = request.context;
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = begun.isolation_level;
  return context;
}

void Commit(const api::EngineRequestContext& context) {
  api::EngineCommitTransactionRequest request;
  request.context = context;
  RequireOk(api::EngineCommitTransaction(request), "CDP-040 commit failed");
}

void Rollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  RequireOk(api::EngineRollbackTransaction(request), "CDP-040 rollback failed");
}

api::CrudTableRecord Table(const Fixture& fixture,
                           const api::EngineRequestContext& context) {
  api::CrudTableRecord table;
  table.creator_tx = context.local_transaction_id;
  table.table_uuid = fixture.table_uuid;
  table.default_name = "cdp_native_bulk_ingest";
  table.columns.push_back({"id", "canonical=int64"});
  table.columns.push_back({"payload", "canonical=character"});
  return table;
}

api::CrudTableRecord TypedScalarTable(const Fixture& fixture,
                                      const api::EngineRequestContext& context) {
  api::CrudTableRecord table;
  table.creator_tx = context.local_transaction_id;
  table.table_uuid = fixture.table_uuid;
  table.default_name = "cdp_native_bulk_ingest_typed_scalar";
  table.columns.push_back({"id", "canonical=int64"});
  table.columns.push_back({"flag_value", "canonical=boolean"});
  table.columns.push_back({"tiny_i", "canonical=int8"});
  table.columns.push_back({"short_i", "canonical=int16"});
  table.columns.push_back({"int_i", "canonical=int32"});
  table.columns.push_back({"huge_i", "canonical=int128"});
  table.columns.push_back({"tiny_u", "canonical=uint8"});
  table.columns.push_back({"short_u", "canonical=uint16"});
  table.columns.push_back({"int_u", "canonical=uint32"});
  table.columns.push_back({"big_u", "canonical=uint64"});
  table.columns.push_back({"huge_u", "canonical=uint128"});
  table.columns.push_back({"bfloat_16", "canonical=bfloat16"});
  table.columns.push_back({"real_16", "canonical=real16"});
  table.columns.push_back({"real_32", "canonical=real32"});
  table.columns.push_back({"real_64", "canonical=real64"});
  table.columns.push_back({"real_128", "canonical=real128"});
  table.columns.push_back({"uuid_value", "canonical=uuid"});
  table.columns.push_back({"ip_value", "canonical=ip_address"});
  table.columns.push_back({"prefix_value", "canonical=network_prefix"});
  table.columns.push_back({"mac_value", "canonical=mac_address"});
  table.columns.push_back({"enum_value", "canonical=enum_value"});
  table.columns.push_back({"date_value", "canonical=date"});
  table.columns.push_back({"time_value", "canonical=time"});
  table.columns.push_back({"timestamp_value", "canonical=timestamp"});
  table.columns.push_back({"interval_value", "canonical=interval"});
  table.columns.push_back({"binary_value", "canonical=binary"});
  table.columns.push_back({"payload", "canonical=character"});
  return table;
}

api::CrudIndexRecord Index(const Fixture& fixture,
                           const api::EngineRequestContext& context) {
  api::CrudIndexRecord index;
  index.creator_tx = context.local_transaction_id;
  index.index_uuid = fixture.index_uuid;
  index.table_uuid = fixture.table_uuid;
  index.column_name = "payload";
  index.family = api::kCrudIndexFamilyBtree;
  index.profile = api::kCrudIndexProfileRowStoreScalarBtreeV1;
  return index;
}

api::CrudIndexRecord IdIndex(const Fixture& fixture,
                             const api::EngineRequestContext& context) {
  api::CrudIndexRecord index;
  index.creator_tx = context.local_transaction_id;
  index.index_uuid = fixture.index_uuid;
  index.table_uuid = fixture.table_uuid;
  index.column_name = "id";
  index.family = api::kCrudIndexFamilyBtree;
  index.profile = api::kCrudIndexProfileRowStoreScalarBtreeV1;
  return index;
}

const std::vector<std::string>& TypedScalarIndexedColumns() {
  static const std::vector<std::string> columns = {
      "id",
      "flag_value",
      "tiny_i",
      "short_i",
      "int_i",
      "huge_i",
      "tiny_u",
      "short_u",
      "int_u",
      "big_u",
      "huge_u",
      "bfloat_16",
      "real_16",
      "real_32",
      "real_64",
      "real_128",
      "uuid_value",
      "ip_value",
      "prefix_value",
      "mac_value",
      "enum_value",
      "date_value",
      "time_value",
      "timestamp_value",
      "interval_value",
      "binary_value",
      "payload"};
  return columns;
}

api::CrudIndexRecord TypedScalarUniqueIndex(
    const Fixture& fixture,
    const api::EngineRequestContext& context,
    const std::string& column_name,
    platform::u64 salt) {
  api::CrudIndexRecord index;
  index.creator_tx = context.local_transaction_id;
  index.index_uuid = NewIdentity(platform::UuidKind::object, salt);
  index.table_uuid = fixture.table_uuid;
  index.column_name = column_name;
  index.family = api::kCrudIndexFamilyBtree;
  index.profile = api::kCrudIndexProfileRowStoreScalarBtreeV1;
  index.unique = true;
  return index;
}

Fixture MakeFixture(std::string name, platform::u64 salt) {
  Fixture fixture;
  fixture.salt = salt;
  InitializeOwnedFixtureDirectory(fixture, name);

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = NewTypedUuid(platform::UuidKind::database, salt + 1);
  create.filespace_uuid = NewTypedUuid(platform::UuidKind::filespace, salt + 2);
  create.creation_unix_epoch_millis = UniqueMillis();
  scratchbird::tests::ConfigureCredentialedFixtureBootstrap(create);
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "CDP-040 database create failed");

  fixture.database_uuid = create.database_uuid.value;
  fixture.owner_context = scratchbird::tests::BootstrapFixtureOwnerContext(create);
  fixture.page_size = create.page_size;
  fixture.table_uuid = NewIdentity(platform::UuidKind::object, salt + 10);
  fixture.index_uuid = NewIdentity(platform::UuidKind::object, salt + 11);

  auto metadata = Begin(fixture, "cdp040-metadata");
  const auto table = PublishFixtureTable(metadata, Table(fixture, metadata));
  Require(!table.error, "CDP-040 table metadata append failed");
  const auto index = api::AppendMgaIndexMetadata(metadata, Index(fixture, metadata));
  Require(!index.error, "CDP-040 index metadata append failed");
  Commit(metadata);
  StartFixtureSession(fixture, metadata);
  return fixture;
}

Fixture MakeInt64IndexFixture(std::string name, platform::u64 salt,
                              std::string_view integer_type = "int64") {
  Fixture fixture;
  fixture.salt = salt;
  InitializeOwnedFixtureDirectory(fixture, name);

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = NewTypedUuid(platform::UuidKind::database, salt + 1);
  create.filespace_uuid = NewTypedUuid(platform::UuidKind::filespace, salt + 2);
  create.creation_unix_epoch_millis = UniqueMillis();
  scratchbird::tests::ConfigureCredentialedFixtureBootstrap(create);
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "CDP-040 int64 index database create failed");

  fixture.database_uuid = create.database_uuid.value;
  fixture.owner_context = scratchbird::tests::BootstrapFixtureOwnerContext(create);
  fixture.page_size = create.page_size;
  fixture.table_uuid = NewIdentity(platform::UuidKind::object, salt + 10);
  fixture.index_uuid = NewIdentity(platform::UuidKind::object, salt + 11);

  auto metadata = Begin(fixture, "cdp040-int64-index-metadata");
  auto definition = Table(fixture, metadata);
  definition.columns.front().second = "canonical=" + std::string(integer_type);
  const auto table = PublishFixtureTable(metadata, definition);
  Require(!table.error, "CDP-040 int64 index table metadata append failed");
  const auto index = api::AppendMgaIndexMetadata(metadata, IdIndex(fixture, metadata));
  Require(!index.error, "CDP-040 int64 index metadata append failed");
  Commit(metadata);
  StartFixtureSession(fixture, metadata);
  return fixture;
}

Fixture MakeTypedScalarFixture(std::string name,
                               platform::u64 salt,
                               bool with_typed_scalar_indexes = false) {
  Fixture fixture;
  fixture.salt = salt;
  InitializeOwnedFixtureDirectory(fixture, name);

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = NewTypedUuid(platform::UuidKind::database, salt + 1);
  create.filespace_uuid = NewTypedUuid(platform::UuidKind::filespace, salt + 2);
  create.creation_unix_epoch_millis = UniqueMillis();
  scratchbird::tests::ConfigureCredentialedFixtureBootstrap(create);
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "CDP-040 typed scalar database create failed");

  fixture.database_uuid = create.database_uuid.value;
  fixture.owner_context = scratchbird::tests::BootstrapFixtureOwnerContext(create);
  fixture.page_size = create.page_size;
  fixture.table_uuid = NewIdentity(platform::UuidKind::object, salt + 10);

  auto metadata = Begin(fixture, "cdp040-typed-scalar-metadata");
  const auto table = PublishFixtureTable(
      metadata, TypedScalarTable(fixture, metadata));
  Require(!table.error, "CDP-040 typed scalar table metadata append failed");
  if (with_typed_scalar_indexes) {
    platform::u64 index_salt = salt + 200;
    for (const auto& column : TypedScalarIndexedColumns()) {
      const auto index = api::AppendMgaIndexMetadata(
          metadata, TypedScalarUniqueIndex(fixture, metadata, column, index_salt++));
      Require(!index.error,
              "CDP-040 typed scalar index metadata append failed");
    }
  }
  Commit(metadata);
  StartFixtureSession(fixture, metadata);
  return fixture;
}

std::vector<api::EngineRowValue> Rows(std::string prefix, int count) {
  std::vector<api::EngineRowValue> rows;
  rows.reserve(static_cast<std::size_t>(count));
  for (int index = 0; index < count; ++index) {
    rows.push_back(Row(std::to_string(index + 1),
                       prefix + "-payload-" + std::to_string(index + 1)));
  }
  return rows;
}

api::EngineRowValue Int64IndexedRow(std::int64_t value, platform::u64 salt) {
  api::EngineRowValue row;
  row.requested_row_uuid =
      NewIdentity(platform::UuidKind::object, salt);
  row.fields.push_back({"id", ScalarValue("int64", std::to_string(value))});
  row.fields.push_back({"payload",
                        TextValue("int64-index-payload-" +
                                  std::to_string(value))});
  return row;
}

api::EngineRowValue Int64IndexedRow(std::int64_t value) {
  return Int64IndexedRow(value, static_cast<platform::u64>(1180 + value));
}

api::EngineRowValue NullInt64IndexedRow() {
  api::EngineRowValue row;
  row.requested_row_uuid =
      NewIdentity(platform::UuidKind::object, 1179);
  row.fields.push_back({"id", NullValue("int64")});
  row.fields.push_back({"payload", TextValue("int64-index-payload-null")});
  return row;
}

api::EngineRowValue NullCharacterRow() {
  api::EngineRowValue row;
  row.requested_row_uuid =
      NewIdentity(platform::UuidKind::object, 1199);
  row.fields.push_back({"id", NullValue("int64")});
  row.fields.push_back({"payload", TextValue("null-character-payload")});
  return row;
}

api::EngineRowValue TypedScalarRow(int index) {
  api::EngineRowValue row;
  row.requested_row_uuid =
      NewIdentity(platform::UuidKind::object, 1200 + index);
  row.fields.push_back({"id", ScalarValue("int64", std::to_string(index))});
  row.fields.push_back({"flag_value", ScalarValue("boolean", index % 2 == 0 ? "true" : "false")});
  row.fields.push_back({"tiny_i", ScalarValue("int8", index % 2 == 0 ? "12" : "-12")});
  row.fields.push_back({"short_i", ScalarValue("int16", index % 2 == 0 ? "1024" : "-1024")});
  row.fields.push_back({"int_i", ScalarValue("int32", index % 2 == 0 ? "2428" : "1973")});
  row.fields.push_back({"huge_i", ScalarValue("int128", index % 2 == 0 ? "170141183460469231731687303715884105727" : "-170141183460469231731687303715884105728")});
  row.fields.push_back({"tiny_u", ScalarValue("uint8", "250")});
  row.fields.push_back({"short_u", ScalarValue("uint16", "65000")});
  row.fields.push_back({"int_u", ScalarValue("uint32", "4000000000")});
  row.fields.push_back({"big_u", ScalarValue("uint64", "18446744073709551615")});
  row.fields.push_back({"huge_u", ScalarValue("uint128", "340282366920938463463374607431768211455")});
  row.fields.push_back({"bfloat_16", NativeRealFixtureValue("bfloat16", index % 2 == 0 ? 0x4060u : 0xc060u, 2)});
  row.fields.push_back({"real_16", NativeRealFixtureValue("real16", index % 2 == 0 ? 0x4480u : 0xc480u, 2)});
  row.fields.push_back({"real_32", NativeRealFixtureValue("real32", index % 2 == 0 ? 0x3fa00000u : 0xbfa00000u, 4)});
  row.fields.push_back({"real_64", NativeRealFixtureValue("real64", index % 2 == 0 ? 0x4004000000000000ull : 0xc004000000000000ull, 8)});
  row.fields.push_back({"real_128", BinaryScalarValue("real128", std::vector<std::uint8_t>{
      static_cast<std::uint8_t>(index), 1, 2, 3, 4, 5, 6, 7,
      8, 9, 10, 11, 12, 13, 14, 15})});
  row.fields.push_back({"uuid_value", IdentityScalarValue("uuid", NewIdentity(platform::UuidKind::object, 1300 + index))});
  row.fields.push_back({"ip_value", BinaryScalarValue("ip_address", std::vector<std::uint8_t>{
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 192, 0, 2,
      static_cast<std::uint8_t>(index)})});
  row.fields.push_back({"prefix_value", BinaryScalarValue("network_prefix", std::vector<std::uint8_t>{
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 198, 51,
      static_cast<std::uint8_t>(100 + index), 0, 24, 4})});
  row.fields.push_back({"mac_value", BinaryScalarValue("mac_address", std::vector<std::uint8_t>{
      0, 0, 2, 0, 0, 0, 0, static_cast<std::uint8_t>(index)})});
  row.fields.push_back({"enum_value", IdentityScalarValue("enum_value", NewIdentity(platform::UuidKind::object, 1400 + index))});
  row.fields.push_back({"date_value", ScalarValue("date", index % 2 == 0 ? "2026-06-25" : "1970-01-01")});
  row.fields.push_back({"time_value", ScalarValue("time", index % 2 == 0 ? "23:59:58.123456789" : "00:00:01")});
  row.fields.push_back({"timestamp_value", ScalarValue("timestamp", index % 2 == 0 ? "2026-06-25T12:34:56.123456789Z" : "1970-01-01T00:00:01")});
  row.fields.push_back({"interval_value", ScalarValue("interval", index % 2 == 0 ? "P1DT2H3M4S" : "3600")});
  row.fields.push_back({"binary_value", ScalarValue("binary", "binary-payload-" + std::to_string(index))});
  row.fields.push_back({"payload", TextValue("typed-scalar-payload-" + std::to_string(index))});
  return row;
}

std::vector<api::EngineRowValue> TypedScalarRows() {
  std::vector<api::EngineRowValue> rows;
  rows.push_back(TypedScalarRow(1));
  rows.push_back(TypedScalarRow(2));
  return rows;
}

const std::vector<std::string>& DescriptorPayloadTypeNames() {
  static const std::vector<std::string> types = {
      "decimal",
      "decimal_float",
      "bit_string",
      "blob",
      "document",
      "json_document",
      "binary_json_document",
      "bson_document",
      "xml_document",
      "hstore_document",
      "object_document",
      "flattened_object_document",
      "set_value",
      "array",
      "list",
      "map",
      "row",
      "composite",
      "variant",
      "range",
      "multirange",
      "token_stream",
      "search_query",
      "search_rank_feature",
      "search_completion",
      "search_percolator",
      "geometry",
      "geography",
      "point",
      "shape",
      "raster",
      "vector",
      "dense_vector",
      "sparse_vector",
      "binary_vector",
      "quantized_vector",
      "graph_node",
      "graph_edge",
      "graph_path",
      "time_series_value",
      "columnar_segment",
      "aggregate_state",
      "hll_sketch",
      "bloom_filter",
      "quantile_sketch",
      "histogram_sketch",
      "ranking_summary",
      "vector_summary",
      "lob_locator",
      "external_file_locator",
      "remote_object_locator",
      "bridge_handle",
      "cursor_handle",
      "system_reference",
      "cursor",
      "result_set",
      "table_value"};
  return types;
}

const std::vector<std::string>& OpaqueRenderOnlyPayloadTypeNames() {
  static const std::vector<std::string> types = {
      "opaque_extension"};
  return types;
}

std::string DescriptorPayloadColumnName(std::size_t index) {
  return "descriptor_payload_" + std::to_string(index);
}

std::vector<std::uint8_t> DescriptorPayloadForType(const std::string& type,
                                                   int row_index,
                                                   std::size_t type_index) {
  if (type == "decimal") {
    const auto encoded = scratchbird::libraries::sbl_numeric::EncodeExactDecimalLittleEndian(
        std::to_string(row_index) + ".25");
    Require(encoded.ok, "descriptor decimal fixture encoding failed");
    return {encoded.canonical_bytes.begin(), encoded.canonical_bytes.end()};
  }
  if (type == "json_document") {
    const std::string json = "{\"value\":" + std::to_string(row_index) + "}";
    return {json.begin(), json.end()};
  }
  if (type == "decimal_float") {
    const auto encoded = scratchbird::libraries::sbl_numeric::EncodeDecimal128LittleEndian(
        std::to_string(row_index) + ".25");
    Require(encoded.bytes.has_value(), "descriptor decimal128 fixture encoding failed");
    return {encoded.bytes->begin(), encoded.bytes->end()};
  }
  if (type == "list") {
    const std::string text = std::to_string(row_index);
    std::vector<std::uint8_t> payload{'S','B','T','L','0','0','0','1',1,0,0,0,1};
    for (unsigned i = 0; i < 4; ++i)
      payload.push_back(static_cast<std::uint8_t>(text.size() >> (8*i)));
    payload.insert(payload.end(), text.begin(), text.end());
    return payload;
  }
  if (type == "geometry") {
    std::vector<std::uint8_t> payload{'S','B','P','1',1,2,0,0};
    for (double coordinate : {static_cast<double>(row_index), 0.0}) {
      const auto bits = std::bit_cast<std::uint64_t>(coordinate);
      for (unsigned i = 0; i < 8; ++i)
        payload.push_back(static_cast<std::uint8_t>(bits >> (56-8*i)));
    }
    return payload;
  }
  std::size_t size = 16;
  if (type == "blob") {
    size = 24;
  }
  std::vector<std::uint8_t> payload(size);
  for (std::size_t index = 0; index < payload.size(); ++index) {
    payload[index] = static_cast<std::uint8_t>(
        (row_index * 17 + static_cast<int>(type_index) + static_cast<int>(index)) & 0xff);
  }
  return payload;
}

api::CrudTableRecord DescriptorPayloadTable(const Fixture& fixture,
                                            const api::EngineRequestContext& context) {
  api::CrudTableRecord table;
  table.creator_tx = context.local_transaction_id;
  table.table_uuid = fixture.table_uuid;
  table.default_name = "cdp_native_bulk_ingest_descriptor_payload";
  const auto& types = DescriptorPayloadTypeNames();
  for (std::size_t index = 0; index < types.size(); ++index) {
    table.columns.push_back(
        {DescriptorPayloadColumnName(index), "canonical=" + types[index]});
  }
  return table;
}

api::CrudIndexRecord DescriptorPayloadUniqueIndex(
    const Fixture& fixture,
    const api::EngineRequestContext& context,
    const std::string& column_name,
    platform::u64 salt) {
  api::CrudIndexRecord index;
  index.creator_tx = context.local_transaction_id;
  index.index_uuid = NewIdentity(platform::UuidKind::object, salt);
  index.table_uuid = fixture.table_uuid;
  index.column_name = column_name;
  index.family = api::kCrudIndexFamilyBtree;
  index.profile = api::kCrudIndexProfileRowStoreScalarBtreeV1;
  index.unique = true;
  return index;
}

Fixture MakeDescriptorPayloadFixture(std::string name,
                                     platform::u64 salt,
                                     bool with_descriptor_indexes = false) {
  Fixture fixture;
  fixture.salt = salt;
  InitializeOwnedFixtureDirectory(fixture, name);

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = NewTypedUuid(platform::UuidKind::database, salt + 1);
  create.filespace_uuid = NewTypedUuid(platform::UuidKind::filespace, salt + 2);
  create.creation_unix_epoch_millis = UniqueMillis();
  scratchbird::tests::ConfigureCredentialedFixtureBootstrap(create);
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "CDP-040 descriptor payload database create failed");

  fixture.database_uuid = create.database_uuid.value;
  fixture.owner_context = scratchbird::tests::BootstrapFixtureOwnerContext(create);
  fixture.page_size = create.page_size;
  fixture.table_uuid = NewIdentity(platform::UuidKind::object, salt + 10);

  auto metadata = Begin(fixture, "cdp040-descriptor-payload-metadata");
  const auto table = PublishFixtureTable(
      metadata, DescriptorPayloadTable(fixture, metadata));
  Require(!table.error, "CDP-040 descriptor payload table metadata append failed");
  if (with_descriptor_indexes) {
    platform::u64 index_salt = salt + 200;
    const auto& types = DescriptorPayloadTypeNames();
    for (std::size_t index = 0; index < types.size(); ++index) {
      const auto created_index = api::AppendMgaIndexMetadata(
          metadata,
          DescriptorPayloadUniqueIndex(fixture,
                                       metadata,
                                       DescriptorPayloadColumnName(index),
                                       index_salt++));
      Require(!created_index.error,
              "CDP-040 descriptor payload index metadata append failed");
    }
  }
  Commit(metadata);
  StartFixtureSession(fixture, metadata);
  return fixture;
}

api::EngineRowValue DescriptorPayloadRow(int row_index) {
  api::EngineRowValue row;
  row.requested_row_uuid =
      NewIdentity(platform::UuidKind::object, 1700 + row_index);
  const auto& types = DescriptorPayloadTypeNames();
  for (std::size_t index = 0; index < types.size(); ++index) {
    row.fields.push_back(
        {DescriptorPayloadColumnName(index),
         BinaryScalarValue(types[index],
                           DescriptorPayloadForType(types[index], row_index, index))});
  }
  return row;
}

std::vector<api::EngineRowValue> DescriptorPayloadRows() {
  std::vector<api::EngineRowValue> rows;
  rows.push_back(DescriptorPayloadRow(1));
  rows.push_back(DescriptorPayloadRow(2));
  return rows;
}

scratchbird::tests::FixtureEngineRequest<api::EngineExecuteNativeBulkIngestRequest> NativeRequest(
    const Fixture& fixture,
    const api::EngineRequestContext& context,
    std::vector<api::EngineRowValue> rows) {
  scratchbird::tests::FixtureEngineRequest<api::EngineExecuteNativeBulkIngestRequest>
      request(*fixture.session, context);
  request.target_table.uuid = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.canonical_rows = std::move(rows);
  request.estimated_row_count = static_cast<api::EngineApiU64>(request.canonical_rows.size());
  request.import_policy.reject_mode = "fail_fast";
  request.import_policy.reject_payload_policy = "diagnostic_only";
  request.import_policy.resume_policy = "fail_closed";
  // This physical-page component probe owns a destination beyond the current
  // node file; bootstrap/catalog pages must never be overwritten by a fixed
  // test page number. Reuse that page for this fixture's subsequent mutations.
  if (fixture.physical_page == 0) {
    Require(fixture.page_size != 0, "CDP-040 fixture page geometry unavailable");
    fixture.physical_page =
        (std::filesystem::file_size(fixture.database_path) + fixture.page_size - 1) /
        fixture.page_size + 64;
  }
  request.option_envelopes.push_back("physical_mga_cow.required=true");
  request.option_envelopes.push_back("physical_mga_cow.page_number=" +
                                    std::to_string(fixture.physical_page));
  return request;
}

api::EngineApiU64 SelectCount(const Fixture& fixture,
                              const api::EngineRequestContext& context) {
  api::EngineSelectRowsRequest request;
  request.context = context;
  request.source_object.uuid = fixture.table_uuid;
  request.source_object.object_kind = "table";
  request.select_projection.canonical_projection_envelopes.push_back("id");
  const auto selected = api::EngineSelectRows(request);
  RequireOk(selected, "CDP-040 select failed");
  return selected.visible_count;
}

platform::TypedUuid RelationUuid(const Fixture& fixture) {
  const auto parsed =
      uuid::MakeTypedUuid(platform::UuidKind::object, fixture.table_uuid);
  Require(parsed.ok(), "CDP-040 table UUID validation failed");
  return parsed.value;
}

db::PhysicalMgaCowReadResult ReadPhysicalPage(const Fixture& fixture,
                                              platform::u64 page_number) {
  db::PhysicalMgaCowReadRequest request;
  request.database_path = fixture.database_path.string();
  request.relation_uuid = RelationUuid(fixture);
  request.page_number = page_number;
  request.use_latest_committed_snapshot = true;
  const auto read = db::ReadPhysicalMgaCowRows(request);
  if (!read.ok()) {
    std::cerr << read.diagnostic.diagnostic_code << ':'
              << read.diagnostic.message_key << '\n';
  }
  Require(read.ok(), "CDP-040 physical typed row-page read failed");
  return read;
}

sblr::SblrOperand Operand(std::string type, std::string name, std::string value) {
  sblr::SblrOperand operand;
  operand.type = std::move(type);
  operand.name = std::move(name);
  operand.value = std::move(value);
  return operand;
}

sblr::SblrOperationEnvelope NativeEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope("dml.execute_native_bulk_ingest",
                                         "SBLR_DML_EXECUTE_NATIVE_BULK_INGEST",
                                         "CDP-040-NATIVE-BULK-INGEST");
  const auto* identity =
      sblr::LookupSblrOperation("dml.execute_native_bulk_ingest");
  Require(identity != nullptr && identity->code != 0,
          "CDP-040 native ingest canonical opcode identity missing");
  envelope.opcode_code = identity->code;
  envelope.parser_package_uuid = NewIdentity(platform::UuidKind::object, 2000);
  envelope.registry_snapshot_uuid = NewIdentity(platform::UuidKind::object, 2001);
  envelope.parser_resolved_names_to_uuids = true;
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.result_shape = "engine_api_result";
  envelope.diagnostic_shape = "engine_api_diagnostic_vector";
  return envelope;
}

sblr::SblrOperationEnvelope NativeRoundTripEnvelope() {
  return NativeEnvelope();
}

api::EngineApiRequest SblrApiRequest(const Fixture& fixture,
                                     std::vector<api::EngineRowValue> rows) {
  api::EngineApiRequest request;
  request.target_object.uuid = fixture.table_uuid;
  request.target_object.object_kind = "table";
  request.rows = std::move(rows);
  request.option_envelopes.push_back("estimated_row_count:" + std::to_string(request.rows.size()));
  return request;
}

void AppendU16(std::vector<std::uint8_t>* out, std::uint16_t value) {
  out->push_back(static_cast<std::uint8_t>(value));
  out->push_back(static_cast<std::uint8_t>(value >> 8u));
}

void AppendU32(std::vector<std::uint8_t>* out, std::uint32_t value) {
  for (unsigned byte = 0; byte < 4; ++byte) {
    out->push_back(static_cast<std::uint8_t>(value >> (byte * 8u)));
  }
}

void AppendU64(std::vector<std::uint8_t>* out, std::uint64_t value) {
  for (unsigned byte = 0; byte < 8; ++byte) {
    out->push_back(static_cast<std::uint8_t>(value >> (byte * 8u)));
  }
}

void StoreU16(std::array<std::uint8_t, 132>* out,
              std::size_t offset,
              std::uint16_t value) {
  (*out)[offset] = static_cast<std::uint8_t>(value);
  (*out)[offset + 1] = static_cast<std::uint8_t>(value >> 8u);
}

void StoreU32(std::array<std::uint8_t, 132>* out,
              std::size_t offset,
              std::uint32_t value) {
  for (unsigned byte = 0; byte < 4; ++byte) {
    (*out)[offset + byte] =
        static_cast<std::uint8_t>(value >> (byte * 8u));
  }
}

void StoreU64(std::array<std::uint8_t, 132>* out,
              std::size_t offset,
              std::uint64_t value) {
  for (unsigned byte = 0; byte < 8; ++byte) {
    (*out)[offset + byte] =
        static_cast<std::uint8_t>(value >> (byte * 8u));
  }
}

std::array<std::uint8_t, 16> UuidBytes(const platform::Uuid& value,
                                       std::string_view message) {
  Require(!value.is_nil(), message);
  return value.bytes;
}

void AppendUuid(std::vector<std::uint8_t>* out,
                const std::array<std::uint8_t, 16>& value) {
  out->insert(out->end(), value.begin(), value.end());
}

void AppendBytes(std::vector<std::uint8_t>* out,
                 const std::vector<std::uint8_t>& value) {
  AppendU64(out, value.size());
  out->insert(out->end(), value.begin(), value.end());
}

std::vector<std::uint8_t> CanonicalU16(std::uint16_t value) {
  std::vector<std::uint8_t> out;
  AppendU16(&out, value);
  return out;
}

std::vector<std::uint8_t> CanonicalU32(std::uint32_t value) {
  std::vector<std::uint8_t> out;
  AppendU32(&out, value);
  return out;
}

std::vector<std::uint8_t> CanonicalU64(std::uint64_t value) {
  std::vector<std::uint8_t> out;
  AppendU64(&out, value);
  return out;
}

std::vector<std::uint8_t> CanonicalStruct(std::uint32_t tag,
                                          std::uint8_t value) {
  std::vector<std::uint8_t> out;
  AppendU32(&out, tag);
  AppendU16(&out, 1);
  AppendU16(&out, 0);
  AppendU64(&out, 1);
  out.push_back(value);
  return out;
}

std::vector<std::uint8_t> CanonicalOptionalUuid(
    const std::array<std::uint8_t, 16>& value) {
  std::vector<std::uint8_t> out{1};
  AppendUuid(&out, value);
  return out;
}

std::vector<std::uint8_t> NativeRowPacket(
    const std::vector<api::EngineRowValue>& rows) {
  Require(!rows.empty(), "CDP-040 native packet rowset was empty");
  const auto& columns = rows.front().fields;
  Require(!columns.empty(), "CDP-040 native packet columns were empty");
  for (const auto& row : rows) {
    Require(row.fields.size() == columns.size(),
            "CDP-040 native packet row shape drifted");
    for (std::size_t column = 0; column < columns.size(); ++column) {
      Require(row.fields[column].first == columns[column].first &&
                  !row.fields[column].second.is_null &&
                  row.fields[column].second.binary_value.empty(),
              "CDP-040 native packet requires the exact non-null text cohort");
    }
  }

  std::vector<std::uint8_t> out{'S', 'B', 'N', 'R'};
  AppendU16(&out, 2);
  AppendU16(&out, 0);
  AppendU64(&out, rows.size());
  AppendU32(&out, static_cast<std::uint32_t>(columns.size()));
  for (std::size_t column = 0; column < columns.size(); ++column) {
    out.push_back(1);  // scratchbird.native_rows.v2 text
  }
  for (const auto& [name, ignored] : columns) {
    (void)ignored;
    AppendU32(&out, static_cast<std::uint32_t>(name.size()));
    out.insert(out.end(), name.begin(), name.end());
  }
  const std::size_t null_bitmap_bytes = (columns.size() + 7u) / 8u;
  for (const auto& row : rows) {
    out.insert(out.end(), null_bitmap_bytes, 0);
    for (const auto& [ignored, value] : row.fields) {
      (void)ignored;
      AppendU32(&out, static_cast<std::uint32_t>(value.encoded_value.size()));
      out.insert(out.end(), value.encoded_value.begin(),
                 value.encoded_value.end());
    }
  }
  return out;
}

sblr::SblrOperationEnvelope CanonicalPackageFrame(
    bool begin,
    const server::ServerSessionRecord& session,
    const platform::Uuid& registry_snapshot_uuid,
    const std::array<std::uint8_t, 16>& package_uuid) {
  const std::string operation_id =
      begin ? "engine.op.package_begin" : "engine.op.package_end";
  const auto* identity = sblr::LookupSblrOperation(operation_id);
  Require(identity != nullptr && identity->code != 0,
          "CDP-040 package framing identity was unavailable");
  auto frame = sblr::MakeSblrEnvelope(
      operation_id, identity->opcode,
      begin ? "CDP-040-package-begin" : "CDP-040-package-end");
  frame.opcode_code = identity->code;
  frame.result_shape =
      identity->result_contract.empty() ? "void" : identity->result_contract;
  frame.diagnostic_shape = "diagnostic_vector";
  frame.requires_security_context = identity->requires_security_context;
  frame.requires_transaction_context = identity->requires_transaction_context;
  frame.requires_cluster_authority = identity->requires_cluster_authority;
  frame.parser_package_uuid =
      platform::Uuid{session.admitted_parser_package_uuid};
  frame.parser_package_version_major =
      session.admitted_parser_package_version_major;
  frame.parser_package_version_minor =
      session.admitted_parser_package_version_minor;
  frame.parser_package_version_patch =
      session.admitted_parser_package_version_patch;
  frame.registry_snapshot_uuid = registry_snapshot_uuid;
  frame.parser_resolved_names_to_uuids = true;
  sblr::SblrOperand operand;
  operand.ordinal = 1;
  operand.type = begin ? "package.header" : "package.footer";
  operand.name = "package_descriptor";
  operand.value_kind = sblr::SblrValueKind::descriptor_ref;
  operand.value_body.assign(package_uuid.begin(), package_uuid.end());
  frame.operands.push_back(std::move(operand));
  return frame;
}

struct CanonicalSubmissionParts {
  std::vector<std::uint8_t> container;
  std::vector<std::uint8_t> ingress;
};

CanonicalSubmissionParts CanonicalNativeBulkContainer(
    const Fixture& fixture,
    const server::ServerSessionRecord& session,
    const server::ServerStatementContextRecord& statement,
    const std::vector<api::EngineRowValue>& rows,
    bool enabled) {
  const auto* identity =
      sblr::LookupSblrOperation("dml.execute_native_bulk_ingest");
  Require(identity != nullptr && identity->code != 0 &&
              identity->opcode == "SBLR_DML_EXECUTE_NATIVE_BULK_INGEST",
          "CDP-040 native bulk canonical identity was unavailable");
  const auto package_uuid =
      UuidBytes(statement.view.bound_ast_uuid,
                "CDP-040 bound AST package UUID was malformed");
  const auto text_profile = std::find_if(
      statement.view.descriptor_profiles.begin(),
      statement.view.descriptor_profiles.end(), [](const auto& profile) {
        return profile.profile_kind == scratchbird::server_engine_bridge::
                                           StatementDescriptorProfileKind::kTextNonNull &&
               profile.slot == 0 && !profile.nullable;
      });
  Require(text_profile != statement.view.descriptor_profiles.end(),
          "CDP-040 statement receipt lacked the text operand profile");
  const auto text_type_uuid =
      UuidBytes(text_profile->type_uuid,
                "CDP-040 text operand type UUID was malformed");

  auto operation = sblr::MakeSblrEnvelope(
      identity->operation_id, identity->opcode,
      "CDP-040-native-server-public-abi");
  operation.opcode_code = identity->code;
  operation.result_shape = identity->result_contract.empty()
                               ? "engine.api.result.v1"
                               : identity->result_contract;
  operation.diagnostic_shape = "diagnostic_vector";
  operation.requires_security_context = identity->requires_security_context;
  operation.requires_transaction_context = identity->requires_transaction_context;
  operation.requires_cluster_authority = identity->requires_cluster_authority;
  operation.parser_package_uuid =
      platform::Uuid{session.admitted_parser_package_uuid};
  operation.parser_package_version_major =
      session.admitted_parser_package_version_major;
  operation.parser_package_version_minor =
      session.admitted_parser_package_version_minor;
  operation.parser_package_version_patch =
      session.admitted_parser_package_version_patch;
  operation.registry_snapshot_uuid = statement.view.catalog_epoch_uuid;
  operation.parser_resolved_names_to_uuids = true;
  const auto append_text = [&](std::string name, std::string value) {
    sblr::SblrOperand operand;
    operand.ordinal = static_cast<std::uint32_t>(operation.operands.size() + 1);
    operand.type = "text";
    operand.name = std::move(name);
    operand.value_kind = sblr::SblrValueKind::literal_typed;
    operand.value_body.assign(text_type_uuid.begin(), text_type_uuid.end());
    AppendU64(&operand.value_body, value.size());
    operand.value_body.insert(operand.value_body.end(), value.begin(), value.end());
    operation.operands.push_back(std::move(operand));
  };
  sblr::SblrOperand target;
  target.type = "uuid";
  target.name = "target_object_uuid";
  target.ordinal = 1;
  target.value_kind = sblr::SblrValueKind::uuid_ref;
  target.value_body.assign(fixture.table_uuid.bytes.begin(), fixture.table_uuid.bytes.end());
  operation.operands.push_back(std::move(target));
  append_text("target_object_kind", "table");
  append_text("native_bulk_ingest", "true");
  append_text("native_bulk_ingest_enabled", enabled ? "true" : "false");
  append_text("physical_mga_cow", "false");
  append_text("insert_trace.rows", "false");
  append_text("insert_values_row_count", std::to_string(rows.size()));
  append_text("insert_values_column_count",
              std::to_string(rows.front().fields.size()));
  append_text("insert_values_column_list_present", "false");
  append_text("native_row_packet_required", "true");
  append_text("native_row_packet_format", "scratchbird.native_rows.v2");
  append_text("insert_values_parser_executes_sql", "false");
  for (std::size_t column = 0; column < rows.front().fields.size(); ++column) {
    append_text("insert_values_descriptor_column_" + std::to_string(column),
                rows.front().fields[column].first);
  }
  append_text("sblr.canonical_rowset_shared_shape", "true");
  append_text("sblr.rowset_default_markers_absent", "true");

  sblr::SblrOpcodeStream stream;
  stream.package_descriptor_uuid = statement.view.bound_ast_uuid;
  stream.registry_snapshot_uuid = statement.view.catalog_epoch_uuid;
  stream.operations.push_back(CanonicalPackageFrame(
      true, session, statement.view.catalog_epoch_uuid, package_uuid));
  stream.operations.push_back(std::move(operation));
  stream.operations.push_back(CanonicalPackageFrame(
      false, session, statement.view.catalog_epoch_uuid, package_uuid));
  auto stream_bytes = sblr::EncodeSblrOpcodeStream(stream);
  Require(!stream_bytes.empty(),
          "CDP-040 canonical native bulk opcode stream encoding failed");

  const auto database_uuid =
      UuidBytes(fixture.database_uuid, "CDP-040 database UUID was malformed");
  const auto dialect_uuid = session.admitted_dialect_profile_uuid;
  const auto parser_uuid = session.admitted_parser_package_uuid;
  const auto registry_uuid =
      UuidBytes(statement.view.catalog_epoch_uuid,
                "CDP-040 catalog epoch UUID was malformed");
  const auto statement_uuid =
      UuidBytes(statement.view.statement_uuid,
                "CDP-040 statement UUID was malformed");
  scratchbird::engine::SblrCanonicalContainer container;
  std::copy(database_uuid.begin(), database_uuid.end(),
            container.canonical_anchor.begin());
  std::copy(dialect_uuid.begin(), dialect_uuid.end(),
            container.canonical_anchor.begin() + 16);
  std::copy(parser_uuid.begin(), parser_uuid.end(),
            container.canonical_anchor.begin() + 32);
  StoreU32(&container.canonical_anchor, 48, 1);
  StoreU64(&container.canonical_anchor, 52, 1);
  StoreU64(&container.canonical_anchor, 60, 1);
  StoreU64(&container.canonical_anchor, 68, 1);
  std::copy(registry_uuid.begin(), registry_uuid.end(),
            container.canonical_anchor.begin() + 76);
  StoreU64(&container.canonical_anchor, 92, 1);
  StoreU16(&container.canonical_anchor, 100, 1);
  std::copy(statement_uuid.begin(), statement_uuid.end(),
            container.canonical_anchor.begin() + 116);
  container.operation_payload = stream_bytes;
  auto container_bytes = scratchbird::engine::EncodeSblrContainer(container);
  Require(!container_bytes.empty(),
          "CDP-040 canonical native bulk container encoding failed");

  scratchbird::engine::SblrExecutionEnvelopeV1 ingress;
  auto& fields = ingress.fields;
  fields[0].assign(statement_uuid.begin(), statement_uuid.end());
  fields[1] = CanonicalU16(1);
  fields[2] = CanonicalU16(0);
  fields[3] = CanonicalU32(0x00010001);
  fields[4] = CanonicalU16(1);
  fields[5] = {1};
  AppendU64(&fields[5], stream_bytes.size());
  fields[5].insert(fields[5].end(), stream_bytes.begin(), stream_bytes.end());
  fields[6] = {0};
  fields[7] = {1};
  AppendU32(&fields[7], scratchbird::engine::SblrCrc32c(
                                stream_bytes.data(), stream_bytes.size()));
  fields[8] = CanonicalU64(stream_bytes.size());
  fields[9] = CanonicalU16(1);
  fields[10] = CanonicalOptionalUuid(dialect_uuid);
  fields[11] = CanonicalOptionalUuid(session.effective_user_uuid);
  fields[12] = CanonicalStruct(0x1001, 1);
  fields[13] = CanonicalStruct(0x1002, 2);
  fields[14] = {0};
  fields[15] = CanonicalU64(1);
  fields[16] = CanonicalU32(0);
  fields[17] = CanonicalU32(0);
  fields[18] = CanonicalU32(0);
  fields[19] = {0};
  fields[20] = CanonicalU32(0);
  fields[21] = CanonicalStruct(0x1005, 5);
  fields[22] = {0};
  fields[23] = {0};
  fields[24] = {0};
  fields[25] = CanonicalU16(0);
  fields[26] = {0};
  fields[27] = {0};
  auto ingress_bytes =
      scratchbird::engine::EncodeSblrExecutionEnvelopeV1(ingress);
  Require(!ingress_bytes.empty(),
          "CDP-040 canonical native bulk execution envelope encoding failed");

  return {std::move(container_bytes), std::move(ingress_bytes)};
}

server::HostedEngineState MakeEngineState(const Fixture& fixture) {
  server::HostedEngineState state;
  state.engine_context_active = true;
  server::HostedDatabaseSnapshot database;
  database.state = server::HostedDatabaseState::kOpen;
  database.database_created = true;
  database.database_open = true;
  database.database_path = fixture.database_path.string();
  database.database_uuid = fixture.database_uuid;
  state.databases.push_back(database);
  return state;
}

server::ServerSessionRegistry MakeServerRegistry(
    const Fixture& fixture,
    const api::EngineRequestContext& context,
    std::array<std::uint8_t, 16>* session_uuid) {
  server::ServerTransactionState transaction;
  transaction.local_transaction_id = context.local_transaction_id;
  transaction.transaction_uuid = context.transaction_uuid;
  transaction.snapshot_visible_through_local_transaction_id =
      context.snapshot_visible_through_local_transaction_id;
  transaction.transaction_timestamp = context.transaction_timestamp;
  transaction.isolation_level = context.transaction_isolation_level;

  server::ServerSessionRecord session;
  session.session_uuid =
      UuidBytes(context.session_uuid,
                "CDP-040 server session UUID was malformed");
  session.connection_uuid = session.session_uuid;
  session.server_channel_uuid = sbps::MakeUuidV7Bytes();
  session.channel_state = server::ServerChannelState::kReady;
  session.session_binding_present = true;
  session.transaction_routing_v2_negotiated = true;
  session.auth_context_uuid = sbps::MakeUuidV7Bytes();
  session.principal_uuid =
      UuidBytes(context.principal_uuid,
                "CDP-040 server principal UUID was malformed");
  session.effective_user_uuid = session.principal_uuid;
  session.admitted_parser_package_uuid = sbps::MakeUuidV7Bytes();
  session.admitted_dialect_profile_uuid = sbps::MakeUuidV7Bytes();
  session.admitted_parser_package_version_major = 1;
  session.database_path = fixture.database_path.string();
  session.database_uuid = fixture.database_uuid;
  session.catalog_generation = context.catalog_generation_id;
  session.security_epoch = context.security_epoch;
  session.policy_generation = 1;
  session.resource_epoch = context.resource_epoch;
  session.name_resolution_epoch = context.name_resolution_epoch;
  session.local_transaction_id = context.local_transaction_id;
  session.default_local_transaction_id = context.local_transaction_id;
  session.snapshot_visible_through_local_transaction_id =
      context.snapshot_visible_through_local_transaction_id;
  session.transaction_uuid = context.transaction_uuid;
  session.transaction_timestamp = context.transaction_timestamp;
  session.default_transaction_isolation_level =
      context.transaction_isolation_level;
  session.transactions_by_local_id.emplace(context.local_transaction_id,
                                            std::move(transaction));
  *session_uuid = session.session_uuid;

  server::ServerSessionRegistry registry;
  registry.channel_state = server::ServerChannelState::kReady;
  registry.physical_channel_by_connection_uuid[
      scratchbird::core::platform::Uuid{session.connection_uuid}] =
      session.server_channel_uuid;
  registry.sessions_by_uuid.emplace(
      scratchbird::core::platform::Uuid{session.session_uuid}, std::move(session));
  return registry;
}

sbps::Frame AcquireStatementFrame(
    const std::array<std::uint8_t, 16>& session_uuid,
    const api::EngineRequestContext& context) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(
      sbps::MessageType::kAcquireStatementContextRequest);
  frame.header.payload_schema_id =
      sbps::kSchemaAcquireStatementContextRequestV7;
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.connection_uuid = session_uuid;
  frame.header.session_uuid = session_uuid;
  AppendU16(&frame.payload, 7);
  AppendUuid(&frame.payload, session_uuid);
  AppendU64(&frame.payload, context.local_transaction_id);
  AppendUuid(&frame.payload,
             UuidBytes(context.transaction_uuid,
                       "CDP-040 transaction UUID was malformed"));
  return frame;
}

sbps::Frame CanonicalNativeServerFrame(
    server::ServerSessionRegistry* registry,
    const server::HostedEngineState& engine_state,
    const Fixture& fixture,
    const api::EngineRequestContext& context,
    const std::array<std::uint8_t, 16>& session_uuid,
    const std::vector<api::EngineRowValue>& rows,
    bool enabled = true) {
  const auto acquired = server::HandleAcquireStatementContext(
      registry, engine_state, AcquireStatementFrame(session_uuid, context));
  if (!acquired.accepted) {
    for (const auto& diagnostic : acquired.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
      for (const auto& field : diagnostic.fields) {
        std::cerr << field.key << '=' << field.value << '\n';
      }
    }
  }
  Require(acquired.accepted &&
              acquired.response_schema_id ==
                  sbps::kSchemaAcquireStatementContextResultV7 &&
              acquired.payload.size() >= 115 &&
              acquired.payload[0] == 7 && acquired.payload[1] == 0 &&
              acquired.payload[2] == 1,
          "CDP-040 server statement context acquisition failed");
  std::array<std::uint8_t, 16> statement_uuid{};
  std::copy_n(acquired.payload.begin() + 3, statement_uuid.size(),
              statement_uuid.begin());
  const auto statement_identity = platform::Uuid{statement_uuid};
  const auto statement =
      registry->statement_contexts_by_statement_uuid.find(statement_identity);
  Require(statement != registry->statement_contexts_by_statement_uuid.end() &&
              statement->second.receipt && !statement->second.released,
          "CDP-040 private statement receipt was not retained by the server");
  const auto session = registry->sessions_by_uuid.find(
      scratchbird::core::platform::Uuid{session_uuid});
  Require(session != registry->sessions_by_uuid.end(),
          "CDP-040 server session disappeared after statement acquisition");
  const auto submission = CanonicalNativeBulkContainer(
      fixture, session->second, statement->second, rows, enabled);
  const auto packet = NativeRowPacket(rows);

  sbps::Frame frame;
  frame.header.message_type =
      static_cast<std::uint16_t>(sbps::MessageType::kExecuteSblr);
  frame.header.payload_schema_id = sbps::kSchemaExecuteCanonicalSblrV1;
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.connection_uuid = session_uuid;
  frame.header.session_uuid = session_uuid;
  AppendUuid(&frame.payload, session_uuid);
  AppendUuid(&frame.payload, std::array<std::uint8_t, 16>{});
  frame.payload.push_back(0);
  frame.payload.push_back(1);  // selected transaction route
  AppendU64(&frame.payload, context.local_transaction_id);
  AppendUuid(&frame.payload,
             UuidBytes(context.transaction_uuid,
                       "CDP-040 transaction UUID was malformed"));
  AppendUuid(&frame.payload, statement_uuid);
  AppendBytes(&frame.payload, submission.container);
  AppendBytes(&frame.payload, submission.ingress);
  AppendBytes(&frame.payload, packet);
  return frame;
}

std::uint16_t ReadU16(const std::vector<std::uint8_t>& data, std::size_t offset) {
  return static_cast<std::uint16_t>(data[offset]) |
         static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[offset + 1]) << 8u);
}

std::uint64_t ReadU64(const std::vector<std::uint8_t>& data, std::size_t offset) {
  std::uint64_t value = 0;
  for (int byte = 7; byte >= 0; --byte) {
    value <<= 8u;
    value |= data[offset + static_cast<std::size_t>(byte)];
  }
  return value;
}

std::string ReadProtocolString(const std::vector<std::uint8_t>& data,
                               std::size_t* offset) {
  Require(*offset + 2 <= data.size(), "CDP-040 protocol string length missing");
  auto length = static_cast<std::uint64_t>(ReadU16(data, *offset));
  *offset += 2;
  if (length == 0xffffu) {
    Require(*offset + 8 <= data.size(), "CDP-040 long protocol string length missing");
    length = ReadU64(data, *offset);
    *offset += 8;
  }
  Require(*offset + length <= data.size(), "CDP-040 protocol string payload truncated");
  std::string out(reinterpret_cast<const char*>(data.data() + *offset),
                  static_cast<std::size_t>(length));
  *offset += static_cast<std::size_t>(length);
  return out;
}

scratchbird::parser::ipc::ServerExecutionResult DecodeExecuteResultPayload(
    const std::vector<std::uint8_t>& payload) {
  scratchbird::parser::ipc::ServerExecutionResult result;
  scratchbird::parser::ipc::MessageVectorSet messages;
  Require(scratchbird::parser::ipc::DecodeExecuteResultPayloadV2ForTest(payload, &result, &messages),
          "CDP-040 complete native V2 execution response failed parser decoding");
  Require(messages.diagnostics.empty(), "CDP-040 accepted result has unexpected diagnostic messages");
  return result;
}

void TestApiAndSblrAcceptedRoutes() {
  auto fixture = MakeFixture("accepted", 1000);

  auto api_context = Begin(fixture, "cdp040-api-accepted");
  auto api_result = api::EngineExecuteNativeBulkIngest(
      NativeRequest(fixture, api_context, Rows("api", 3)));
  RequireOk(api_result, "CDP-040 API native bulk ingest failed");
  Require(api_result.accepted_rows == 3 && api_result.inserted_rows == 3,
          "CDP-040 API native bulk ingest row counts drifted");
  Require(HasEvidence(api_result.evidence, "native_bulk_ingest", "enabled"),
          "CDP-040 API native ingest evidence missing");
  Require(HasEvidence(api_result.evidence, "native_bulk_ingest_route", "engine_internal_api"),
          "CDP-040 API route evidence missing");
  Require(HasEvidence(api_result.evidence, "native_bulk_ingest_source", "binary_typed_rows"),
          "CDP-040 API binary typed source evidence missing");
  Require(HasEvidence(api_result.evidence, "parser_finality_authority", "false"),
          "CDP-040 parser finality evidence missing");
  Require(HasEvidence(api_result.evidence, "reference_finality_authority", "false"),
          "CDP-040 reference finality evidence missing");
  Require(HasEvidence(api_result.evidence, "native_bulk_ingest_lane", "direct_physical"),
          "CDP-040 direct physical lane evidence missing");
  Require(HasEvidence(api_result.evidence, "native_bulk_ingest_delegate", "none"),
          "CDP-040 native ingest delegated instead of using direct lane");
  Require(HasEvidence(api_result.evidence, "direct_physical_bulk_operation", "native_bulk"),
          "CDP-040 native ingest direct physical operation was not native_bulk");
  Require(HasEvidence(api_result.evidence,
                      "direct_physical_append_index_cache_bypass",
                      "native_bulk_single_window"),
          "CDP-040 native ingest did not use the native_bulk single-window path");
  Require(HasEvidence(api_result.evidence,
                      "bulk_constraint_proof_route_selected",
                      "direct_physical_bulk.native_empty_target"),
          "CDP-040 native ingest did not use the native empty-target constraint proof");
  Require(EvidenceU64(api_result.evidence,
                      "direct_physical_bulk_row_page_int64_cells") == 3,
          "CDP-040 native ingest did not write id values as int64 row-page cells");
  Require(EvidenceU64(api_result.evidence,
                      "direct_physical_bulk_row_page_character_cells") == 3,
          "CDP-040 native ingest did not write payload values as character row-page cells");
  Require(EvidenceU64(api_result.evidence,
                      "mga_hot_append_index_materialization_jobs_queued") >= 1,
          "CDP-040 native ingest did not queue index materialization work");
  Require(EvidenceU64(api_result.evidence,
                      "mga_hot_append_index_materialization_jobs_completed") ==
              EvidenceU64(api_result.evidence,
                          "mga_hot_append_index_materialization_jobs_queued"),
          "CDP-040 native ingest did not wait for queued index materialization");
  Require(EvidenceU64(api_result.evidence,
                      "mga_hot_append_index_materialized_entries") == 3,
          "CDP-040 native ingest index materialized entry count mismatch");
  Require(EvidenceU64(api_result.evidence,
                      "mga_hot_append_index_materialization_inline_jobs") +
              EvidenceU64(api_result.evidence,
                          "mga_hot_append_index_materialization_worker_count") >=
          1,
          "CDP-040 native ingest did not materialize index work");
  Require(HasEvidence(api_result.evidence,
                      "mga_hot_append_index_materialization_commit_barrier",
                      "flush_waited"),
          "CDP-040 native ingest index materialization was not commit-barriered");
  Require(SelectCount(fixture, api_context) == 3,
          "CDP-040 API native ingest rows not visible in writer transaction");
  Commit(api_context);

  auto sblr_context = Begin(fixture, "cdp040-sblr-accepted");
  sblr::SblrDispatchRequest dispatch;
  scratchbird::tests::FixtureEngineStatement statement(*fixture.session, sblr_context);
  dispatch.context = statement.context;
  dispatch.envelope = NativeEnvelope();
  dispatch.api_request = SblrApiRequest(fixture, Rows("sblr", 2));
  const auto sblr_result = sblr::DispatchSblrOperation(dispatch);
  if (!(sblr_result.accepted && sblr_result.envelope_validated &&
        sblr_result.dispatched_to_api && sblr_result.api_result.ok)) {
    std::cerr << "accepted=" << sblr_result.accepted
              << " envelope_validated=" << sblr_result.envelope_validated
              << " dispatched_to_api=" << sblr_result.dispatched_to_api
              << " api_ok=" << sblr_result.api_result.ok << '\n';
    if (!sblr_result.diagnostics.empty()) {
      std::cerr << "dispatch_diagnostic=" << sblr_result.diagnostics.front().code
                << ':' << sblr_result.diagnostics.front().message << '\n';
    }
    if (!sblr_result.api_result.diagnostics.empty()) {
      std::cerr << "api_diagnostic=" << sblr_result.api_result.diagnostics.front().code
                << ':' << sblr_result.api_result.diagnostics.front().detail << '\n';
    }
    Fail("CDP-040 SBLR native bulk ingest route failed");
  }
  Require(HasEvidence(sblr_result.api_result.evidence, "native_bulk_ingest", "enabled"),
          "CDP-040 SBLR native ingest evidence missing");
  Require(SelectCount(fixture, sblr_context) == 5,
          "CDP-040 SBLR native ingest rows not visible in writer transaction");
  Rollback(sblr_context);
}

void TestNullAndCharacterRowPageStorage() {
  auto fixture = MakeFixture("null_character", 1450);
  auto context = Begin(fixture, "cdp040-null-character-storage");
  const auto result = api::EngineExecuteNativeBulkIngest(
      NativeRequest(fixture, context, {NullCharacterRow()}));
  RequireOk(result, "CDP-040 null/character native bulk ingest failed");
  Require(result.accepted_rows == 1 && result.inserted_rows == 1,
          "CDP-040 null/character row counts drifted");
  Require(HasEvidence(result.evidence,
                      "transactional_relation_store_route",
                      "normal_dml.direct_physical_bulk_append.v1"),
          "CDP-040 native ingest bypassed the canonical relation store");
  Require(EvidenceU64(result.evidence,
                      "direct_physical_bulk_row_page_null_cells") == 1,
          "CDP-040 null cell was not stored as typed null");
  Require(EvidenceU64(result.evidence,
                      "direct_physical_bulk_row_page_character_cells") == 1,
          "CDP-040 character cell did not remain typed character");
  Require(SelectCount(fixture, context) == 1,
          "CDP-040 null/character row was not visible in writer transaction");
  Commit(context);

  const auto page = ReadPhysicalPage(fixture, fixture.physical_page);
  Require(page.visible_rows.size() == 1,
          "CDP-040 null/character physical row-page visible count drifted");
  api::EngineApiU64 null_cells = 0;
  api::EngineApiU64 character_cells = 0;
  for (const auto& row : page.visible_rows) {
    for (const auto& cell : row.cells) {
      if (cell.value.is_null &&
          cell.column_ordinal == 1 &&
          cell.value.type_id == dt::CanonicalTypeId::int64 &&
          cell.value.payload.empty() && !cell.value.payload_is_toast_reference) {
        ++null_cells;
      }
      if (!cell.value.is_null &&
          cell.value.type_id == dt::CanonicalTypeId::character) {
        ++character_cells;
      }
    }
  }
  Require(null_cells == 1,
          "CDP-040 recovered NULL lost its int64 column type or carried payload/reference state");
  Require(character_cells == 1,
          "CDP-040 recovered character row-page cell was not typed");
}

void TestTypedInt64IndexKeysUseBinaryOrder() {
  auto fixture = MakeInt64IndexFixture("typed_int64_index", 1475);
  auto context = Begin(fixture, "cdp040-typed-int64-index-key");
  const auto result = api::EngineExecuteNativeBulkIngest(
      NativeRequest(fixture, context, {Int64IndexedRow(10), Int64IndexedRow(2)}));
  RequireOk(result, "CDP-040 typed int64 index native bulk ingest failed");
  Require(EvidenceU64(result.evidence,
                      "direct_index_key_typed_candidates") == 2,
          "CDP-040 typed int64 index did not inspect both typed keys");
  Require(EvidenceU64(result.evidence,
                      "direct_index_key_typed_encoded") == 2,
          "CDP-040 typed int64 index did not encode both keys");
  Require(EvidenceU64(result.evidence,
                      "direct_index_key_typed_fallback") == 0,
          "CDP-040 typed int64 index fell back to display text");
  Require(EvidenceU64(result.evidence,
                      "direct_index_key_sbkobin_keys") == 2,
          "CDP-040 typed int64 index did not emit store-safe SBKOBIN keys");

  const auto loaded = api::LoadMgaRelationStoreState(context);
  if (!loaded.ok && !loaded.diagnostic.code.empty()) {
    std::cerr << loaded.diagnostic.code << ':'
              << loaded.diagnostic.detail << '\n';
  }
  Require(loaded.ok, "CDP-040 typed int64 index state reload failed");
  std::string key_two;
  std::string key_ten;
  for (const auto& entry : loaded.state.index_entries) {
    if (entry.index_uuid != fixture.index_uuid) {
      continue;
    }
    const auto payload = ScalarLogicalPayload(entry.payload_value);
    Require(payload.isPresent(), "CDP-040 int64 index payload became NULL");
    if (ReadInt64Payload(payload) == 2) {
      key_two = entry.key_value;
    } else if (ReadInt64Payload(payload) == 10) {
      key_ten = entry.key_value;
    }
  }
  Require(key_two.rfind("SBKOBIN:", 0) == 0,
          "CDP-040 int64 value 2 index key was not SBKOBIN");
  Require(key_ten.rfind("SBKOBIN:", 0) == 0,
          "CDP-040 int64 value 10 index key was not SBKOBIN");
  Require(key_two < key_ten,
          "CDP-040 typed int64 index key sorted as display text");
  Commit(context);
}

void TestTypedInt64IndexKeysUseFullSignedSortOrder() {
  auto fixture = MakeInt64IndexFixture("typed_int64_signed_sort", 1480);
  auto context = Begin(fixture, "cdp040-typed-int64-signed-sort-key");
  const auto result = api::EngineExecuteNativeBulkIngest(
      NativeRequest(fixture,
                    context,
                    {Int64IndexedRow(std::numeric_limits<std::int64_t>::min(), 11800),
                     Int64IndexedRow(-257, 11801),
                     Int64IndexedRow(-1, 11802),
                     Int64IndexedRow(0, 11803),
                     Int64IndexedRow(2, 11804),
                     Int64IndexedRow(256, 11805),
                     Int64IndexedRow(std::numeric_limits<std::int64_t>::max(), 11806)}));
  RequireOk(result, "CDP-040 typed int64 signed sort native bulk ingest failed");
  Require(EvidenceU64(result.evidence,
                      "direct_index_key_typed_candidates") == 7,
          "CDP-040 typed int64 signed sort did not inspect every key");
  Require(EvidenceU64(result.evidence,
                      "direct_index_key_typed_encoded") == 7,
          "CDP-040 typed int64 signed sort did not encode every key");
  Require(EvidenceU64(result.evidence,
                      "direct_index_key_typed_fallback") == 0,
          "CDP-040 typed int64 signed sort fell back to display text");
  Require(EvidenceU64(result.evidence,
                      "direct_index_key_sbkobin_keys") == 7,
          "CDP-040 typed int64 signed sort did not emit SBKOBIN keys");

  const auto loaded = api::LoadMgaRelationStoreState(context);
  Require(loaded.ok, "CDP-040 typed int64 signed sort state reload failed");
  std::map<std::int64_t, std::string> keys_by_value;
  for (const auto& entry : loaded.state.index_entries) {
    if (entry.index_uuid == fixture.index_uuid) {
      const auto payload = ScalarLogicalPayload(entry.payload_value);
      Require(payload.isPresent(), "CDP-040 signed int64 index payload became NULL");
      keys_by_value[ReadInt64Payload(payload)] = entry.key_value;
    }
  }
  const std::vector<std::int64_t> order = {
      std::numeric_limits<std::int64_t>::min(), -257, -1, 0, 2, 256,
      std::numeric_limits<std::int64_t>::max()};
  Require(keys_by_value.size() == order.size(), "signed int64 key membership changed");
  for (const auto& value : order) {
    Require(keys_by_value[value].rfind("SBKOBIN:", 0) == 0,
            "CDP-040 signed int64 sort key was not SBKOBIN");
  }
  for (std::size_t index = 1; index < order.size(); ++index) {
    Require(keys_by_value[order[index - 1]] < keys_by_value[order[index]],
            "CDP-040 signed int64 persisted key order was not numeric");
  }
  Commit(context);
}

void TestTypedNullIndexKeyUsesNullOrder() {
  auto fixture = MakeInt64IndexFixture("typed_null_index", 1485);
  auto context = Begin(fixture, "cdp040-typed-null-index-key");
  const auto result = api::EngineExecuteNativeBulkIngest(
      NativeRequest(fixture, context, {NullInt64IndexedRow(), Int64IndexedRow(0)}));
  RequireOk(result, "CDP-040 typed null index native bulk ingest failed");
  Require(EvidenceU64(result.evidence,
                      "direct_index_key_typed_candidates") == 2,
          "CDP-040 typed null index did not inspect both typed keys");
  Require(EvidenceU64(result.evidence,
                      "direct_index_key_typed_encoded") == 2,
          "CDP-040 typed null index did not encode both keys");
  Require(EvidenceU64(result.evidence,
                      "direct_index_key_typed_fallback") == 0,
          "CDP-040 typed null index fell back to display text");
  Require(EvidenceU64(result.evidence,
                      "direct_index_key_sbkobin_keys") == 2,
          "CDP-040 typed null index did not emit SBKOBIN keys");

  const auto loaded = api::LoadMgaRelationStoreState(context);
  Require(loaded.ok, "CDP-040 typed null index state reload failed");
  std::string key_null;
  std::string key_zero;
  for (const auto& entry : loaded.state.index_entries) {
    if (entry.index_uuid != fixture.index_uuid) {
      continue;
    }
    const auto payload = ScalarLogicalPayload(entry.payload_value);
    if (payload.isSqlNull()) {
      key_null = entry.key_value;
    } else if (ReadInt64Payload(payload) == 0) {
      key_zero = entry.key_value;
    }
  }
  Require(key_null.rfind("SBKOBIN:", 0) == 0,
          "CDP-040 null index key was not SBKOBIN");
  Require(key_zero.rfind("SBKOBIN:", 0) == 0,
          "CDP-040 value index key beside null was not SBKOBIN");
  Require(key_null < key_zero,
          "CDP-040 typed null index key did not sort before non-null value");
  Commit(context);

  auto null_context = Begin(fixture, "cdp040-typed-distinct-nulls");
  auto first_null = NullInt64IndexedRow();
  auto second_null = NullInt64IndexedRow();
  first_null.requested_row_uuid = {};
  second_null.requested_row_uuid = {};
  auto null_request = NativeRequest(fixture, null_context, {first_null, second_null});
  null_request.option_envelopes.push_back("physical_mga_cow.row_offset=2");
  const auto nulls = api::EngineExecuteNativeBulkIngest(null_request);
  RequireOk(nulls, "actual SQL NULLs lost nulls-distinct unique-index semantics");
  Require(nulls.inserted_rows == 2 && SelectCount(fixture, null_context) == 4,
          "actual SQL NULL duplicate rows were omitted");
  Rollback(null_context);
}

void TestNativeUint16IndexReopen() {
  auto fixture = MakeInt64IndexFixture("uint16_binary_reopen", 1490, "uint16");
  const std::vector<unsigned> values{0, 1, 42, 255, 256, 0x3234, 32768, 65535};
  std::vector<api::EngineRowValue> rows;
  for (auto value : values) {
    api::EngineRowValue row;
    row.fields = {{"id", ScalarValue("uint16", std::to_string(value))},
                  {"payload", TextValue("uint16-index")}};
    rows.push_back(std::move(row));
  }
  auto writer = Begin(fixture, "uint16-binary-insert");
  const auto inserted = api::EngineExecuteNativeBulkIngest(NativeRequest(fixture, writer, rows));
  RequireOk(inserted, "uint16 binary indexed insert failed");
  Require(inserted.inserted_rows == values.size() &&
              EvidenceU64(inserted.evidence, "direct_index_key_typed_fallback") == 0 &&
              EvidenceU64(inserted.evidence, "direct_index_key_sbkobin_keys") == values.size(),
          "uint16 binary indexed insert lost rows or used text fallback");
  Commit(writer);
  fixture.session.reset();
  fixture.session = std::make_shared<scratchbird::tests::FixtureEngineSession>(
      BaseContext(fixture, "uint16-binary-reopen"));
  auto reader = Begin(fixture, "uint16-binary-read");
  const auto stored = api::LoadMgaRelationStoreState(reader);
  Require(stored.ok, "uint16 committed index replay failed");
  std::map<unsigned, std::string> keys;
  for (const auto& entry : stored.state.index_entries) {
    if (entry.index_uuid != fixture.index_uuid) continue;
    const auto payload = ScalarLogicalPayload(entry.payload_value);
    Require(payload.isPresent() && payload.bytes.size() == 2 &&
                entry.key_value.starts_with("SBKOBIN:"), "uint16 index lost native LE2 framing");
    const auto value = static_cast<unsigned char>(payload.bytes[0]) |
        (static_cast<unsigned>(static_cast<unsigned char>(payload.bytes[1])) << 8);
    Require(keys.emplace(value, entry.key_value).second, "uint16 replay duplicated a key");
  }
  Require(keys.size() == values.size() && SelectCount(fixture, reader) == values.size(),
          "uint16 reopened row or index count changed");
  for (std::size_t ordinal = 0; ordinal < values.size(); ++ordinal) {
    Require(keys.contains(values[ordinal]), "uint16 reopen changed stored value bits");
    if (ordinal) Require(keys.at(values[ordinal-1]) < keys.at(values[ordinal]),
                         "uint16 persisted index order is not unsigned numeric");
  }
  Rollback(reader);
}

void TestNativeNarrowRealIndexReopen() {
  for (const std::string type : {"bfloat16", "real16", "real32"}) {
    auto fixture = MakeInt64IndexFixture(type + "_binary_reopen", 1495, type);
    const unsigned width = type == "real32" ? 4 : 2;
    const std::uint32_t sign = std::uint32_t{1} << (width * 8 - 1);
    const std::uint32_t exponent = type == "bfloat16" ? 0x7f80u
        : type == "real16" ? 0x7c00u : 0x7f800000u;
    const std::uint32_t one = type == "bfloat16" ? 0x3f80u
        : type == "real16" ? 0x3c00u : 0x3f800000u;
    const std::uint32_t minimum_normal = type == "bfloat16" ? 0x80u
        : type == "real16" ? 0x400u : 0x800000u;
    // Independently ordered finite bounds, both zeros, and both sides of the
    // gradual-underflow boundary. Input is exclusively the native LE carrier.
    const std::vector<std::uint32_t> values{
        sign | (exponent - 1), sign | one, sign | minimum_normal,
        sign | (minimum_normal - 1), sign | 1u, sign, 0u,
        1u, minimum_normal - 1, minimum_normal, one, exponent - 1};
    const auto row_for = [&](std::uint32_t bits) {
      std::vector<std::uint8_t> bytes;
      for (unsigned i = 0; i < width; ++i) bytes.push_back(bits >> (8 * i));
      api::EngineRowValue row;
      row.fields = {{"id", BinaryScalarValue(type, std::move(bytes))},
                    {"payload", TextValue("native-finite-real-index")}};
      return row;
    };
    std::vector<api::EngineRowValue> rows;
    for (const auto bits : values) rows.push_back(row_for(bits));
    auto writer = Begin(fixture, "narrow-real-binary-insert");
    const auto inserted = api::EngineExecuteNativeBulkIngest(NativeRequest(fixture, writer, rows));
    RequireOk(inserted, type + " indexed native insert failed");
    Require(inserted.inserted_rows == values.size() &&
                EvidenceU64(inserted.evidence, "direct_index_key_typed_fallback") == 0 &&
                EvidenceU64(inserted.evidence, "direct_index_key_sbkobin_keys") == values.size(),
            type + " indexed insert lost rows or used display-text fallback");
    Commit(writer);
    fixture.session.reset();
    fixture.session = std::make_shared<scratchbird::tests::FixtureEngineSession>(
        BaseContext(fixture, "narrow-real-reopen"));
    auto reader = Begin(fixture, "narrow-real-read");
    const auto stored = api::LoadMgaRelationStoreState(reader);
    Require(stored.ok, type + " committed index replay failed");
    std::map<std::uint32_t, std::string> keys;
    for (const auto& entry : stored.state.index_entries) {
      if (entry.index_uuid != fixture.index_uuid) continue;
      const auto payload = ScalarLogicalPayload(entry.payload_value);
      Require(payload.isPresent() && payload.bytes.size() == width &&
                  entry.key_value.starts_with("SBKOBIN:"), type + " lost native payload framing");
      std::uint32_t bits = 0;
      for (unsigned i = 0; i < width; ++i)
        bits |= std::uint32_t(static_cast<unsigned char>(payload.bytes[i])) << (8 * i);
      Require(keys.emplace(bits, entry.key_value).second, type + " replay duplicated carrier bits");
    }
    Require(keys.size() == values.size() && SelectCount(fixture, reader) == values.size(),
            type + " reopened row/index count changed");
    for (std::size_t i = 0; i < values.size(); ++i) {
      Require(keys.contains(values[i]), type + " reopen altered stored bits");
      if (i) Require(i == 6 ? keys.at(values[i - 1]) == keys.at(values[i])
                           : keys.at(values[i - 1]) < keys.at(values[i]),
                     type + " persisted order differs from numeric finite order");
    }
    Rollback(reader);
    for (const auto special : {exponent, exponent | 1u, exponent | sign}) {
      auto transaction = Begin(fixture, "narrow-real-special-refusal");
      const auto refused = api::EngineExecuteNativeBulkIngest(
          NativeRequest(fixture, transaction, {row_for(special)}));
      Require(!refused.ok && !refused.diagnostics.empty(),
              type + " ordinary index admitted a nonfinite value");
      Require(SelectCount(fixture, transaction) == values.size(),
              type + " nonfinite index refusal changed visible rows");
      Rollback(transaction);
    }
  }
}

void TestNativeNetworkIndexReopen() {
  using Bytes = std::vector<std::uint8_t>;
  for (const std::string type : {"ip_address", "network_prefix", "mac_address"}) {
    auto fixture = MakeInt64IndexFixture(type + "_binary_reopen", 1497, type);
    const bool prefix = type == "network_prefix";
    const unsigned width = prefix ? 18 : type == "ip_address" ? 16 : 8;
    std::vector<Bytes> values;
    if (prefix) {
      values = {
          {0,0,0,0,0,0,0,0,0,0,255,255,0,0,0,0,0,4},
          {0,0,0,0,0,0,0,0,0,0,255,255,0,0,0,0,8,4},
          {0,0,0,0,0,0,0,0,0,0,255,255,10,0,0,0,8,4},
          {0,0,0,0,0,0,0,0,0,0,255,255,10,1,0,0,16,4},
          {0,0,0,0,0,0,0,0,0,0,255,255,255,255,255,255,32,4},
          {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,6},
          {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,128,6},
          {0x20,1,0x0d,0xb8,0,0,0,0,0,0,0,0,0,0,0,0,32,6},
          {0x20,1,0x0d,0xb8,0,0,0,0,0,0,0,0,0,0,0,0,64,6}};
    } else {
      values = {Bytes(width, 0), Bytes(width, 255)};
      for (unsigned i = 0; i < width; ++i) {
        auto bytes = Bytes(width, 0); bytes[i] = 0x80; values.push_back(bytes);
      }
      if (width == 16) values.push_back({0,0,0,0,0,0,0,0,0,0,255,255,192,0,2,1});
      else values.push_back({0,0,8,0,43,1,2,3});
    }
    const auto material = [&](const Bytes& bytes) {
      if (!prefix) return bytes;
      Bytes ordered{bytes[17]}; ordered.insert(ordered.end(), bytes.begin(), bytes.begin() + 16);
      ordered.push_back(bytes[16]); return ordered;
    };
    std::sort(values.begin(), values.end(), [&](const auto& a, const auto& b) { return material(a) < material(b); });
    const auto row_for = [&](const Bytes& bytes) {
      api::EngineRowValue row;
      row.fields = {{"id", BinaryScalarValue(type, bytes)}, {"payload", TextValue("native-network-index")}};
      return row;
    };
    std::vector<api::EngineRowValue> rows;
    for (const auto& value : values) rows.push_back(row_for(value));
    auto writer = Begin(fixture, "network-binary-insert");
    const auto inserted = api::EngineExecuteNativeBulkIngest(NativeRequest(fixture, writer, rows));
    RequireOk(inserted, type + " indexed native insert failed");
    Require(inserted.inserted_rows == values.size() &&
                EvidenceU64(inserted.evidence, "direct_index_key_typed_fallback") == 0 &&
                EvidenceU64(inserted.evidence, "direct_index_key_sbkobin_keys") == values.size(),
            type + " indexed insert lost rows or used display-text fallback");
    Commit(writer);
    fixture.session.reset();
    fixture.session = std::make_shared<scratchbird::tests::FixtureEngineSession>(BaseContext(fixture, "network-reopen"));
    auto reader = Begin(fixture, "network-read");
    const auto stored = api::LoadMgaRelationStoreState(reader);
    Require(stored.ok, type + " committed index replay failed");
    std::map<Bytes, std::string> keys;
    for (const auto& entry : stored.state.index_entries) {
      if (entry.index_uuid != fixture.index_uuid) continue;
      const auto payload = ScalarLogicalPayload(entry.payload_value);
      Require(payload.isPresent() && payload.bytes.size() == width && entry.key_value.starts_with("SBKOBIN:"),
              type + " lost native payload framing");
      Require(keys.emplace(Bytes(payload.bytes.begin(), payload.bytes.end()), entry.key_value).second,
              type + " replay duplicated carrier bytes");
    }
    Require(keys.size() == values.size() && SelectCount(fixture, reader) == values.size(),
            type + " reopened row/index count changed");
    for (std::size_t i = 0; i < values.size(); ++i) {
      Require(keys.contains(values[i]), type + " reopen altered stored bytes");
      if (i) Require(keys.at(values[i - 1]) < keys.at(values[i]), type + " persisted key ordering is wrong");
    }
    Rollback(reader);
    if (prefix) {
      for (unsigned mutation = 0; mutation < 4; ++mutation) {
        auto invalid = values.front();
        if (mutation == 0) invalid[17] = 9;
        if (mutation == 1) invalid[16] = 33;
        if (mutation == 2) invalid[15] = 1;
        if (mutation == 3) invalid[10] = 0;
        auto transaction = Begin(fixture, "network-prefix-refusal");
        const auto refused = api::EngineExecuteNativeBulkIngest(NativeRequest(fixture, transaction, {row_for(invalid)}));
        Require(!refused.ok && !refused.diagnostics.empty(), "invalid network prefix reached index publication");
        Require(SelectCount(fixture, transaction) == values.size(), "invalid prefix refusal changed visible rows");
        Rollback(transaction);
      }
    }
  }
}

void TestTypedScalarIndexKeysUseBinaryPayloads() {
  auto fixture =
      MakeTypedScalarFixture("typed_scalar_index_keys", 1575, true);
  auto context = Begin(fixture, "cdp040-typed-scalar-index-keys");
  const auto result = api::EngineExecuteNativeBulkIngest(
      NativeRequest(fixture, context, {TypedScalarRow(1)}));
  RequireOk(result, "CDP-040 typed scalar index native bulk ingest failed");
  const auto expected =
      static_cast<api::EngineApiU64>(TypedScalarIndexedColumns().size());
  const auto candidates =
      EvidenceU64(result.evidence, "direct_index_key_typed_candidates");
  const auto encoded =
      EvidenceU64(result.evidence, "direct_index_key_typed_encoded");
  const auto fallback =
      EvidenceU64(result.evidence, "direct_index_key_typed_fallback");
  const auto sbkobin =
      EvidenceU64(result.evidence, "direct_index_key_sbkobin_keys");
  Require(candidates == expected,
          "CDP-040 typed scalar indexes did not inspect every typed key: expected " +
              std::to_string(expected) + " got " +
              std::to_string(candidates));
  Require(encoded == expected,
          "CDP-040 typed scalar indexes did not encode every typed key: expected " +
              std::to_string(expected) + " got " + std::to_string(encoded));
  Require(fallback == 0,
          "CDP-040 typed scalar indexes fell back to display text: " +
              std::to_string(fallback));
  Require(sbkobin == expected,
          "CDP-040 typed scalar indexes did not emit SBKOBIN keys: expected " +
              std::to_string(expected) + " got " + std::to_string(sbkobin));

  const auto loaded = api::LoadMgaRelationStoreState(context);
  Require(loaded.ok, "CDP-040 typed scalar index state reload failed");
  api::EngineApiU64 sbkobin_count = 0;
  for (const auto& entry : loaded.state.index_entries) {
    if (entry.table_uuid == fixture.table_uuid &&
        entry.key_value.rfind("SBKOBIN:", 0) == 0) {
      ++sbkobin_count;
    }
  }
  Require(sbkobin_count == expected,
          "CDP-040 typed scalar persisted index keys were not all SBKOBIN");
  Commit(context);
}

void TestTypedScalarRowPageStorage() {
  auto fixture = MakeTypedScalarFixture("typed_scalar", 1500);
  auto context = Begin(fixture, "cdp040-typed-scalar-storage");
  const auto result = api::EngineExecuteNativeBulkIngest(
      NativeRequest(fixture, context, TypedScalarRows()));
  RequireOk(result, "CDP-040 typed scalar native bulk ingest failed");
  Require(result.accepted_rows == 2 && result.inserted_rows == 2,
          "CDP-040 typed scalar row counts drifted");
  Require(EvidenceU64(result.evidence,
                      "direct_physical_bulk_row_page_typed_binary_cells") == 52,
          "CDP-040 typed scalar row-page cells fell back to text");
  Require(EvidenceU64(result.evidence,
                      "direct_physical_bulk_row_page_typed_cells.boolean") == 2,
          "CDP-040 boolean cells were not stored as typed binary");
  Require(EvidenceU64(result.evidence,
                      "direct_physical_bulk_row_page_typed_cells.int8") == 2,
          "CDP-040 int8 cells were not stored as typed binary");
  Require(EvidenceU64(result.evidence,
                      "direct_physical_bulk_row_page_typed_cells.int16") == 2,
          "CDP-040 int16 cells were not stored as typed binary");
  Require(EvidenceU64(result.evidence,
                      "direct_physical_bulk_row_page_typed_cells.int32") == 2,
          "CDP-040 int32 cells were not stored as typed binary");
  Require(EvidenceU64(result.evidence,
                      "direct_physical_bulk_row_page_typed_cells.int64") == 2,
          "CDP-040 int64 cells were not stored as typed binary");
  Require(EvidenceU64(result.evidence,
                      "direct_physical_bulk_row_page_typed_cells.int128") == 2,
          "CDP-040 int128 cells were not stored as 16-byte binary");
  Require(EvidenceU64(result.evidence,
                      "direct_physical_bulk_row_page_typed_cells.uint8") == 2,
          "CDP-040 uint8 cells were not stored as typed binary");
  Require(EvidenceU64(result.evidence,
                      "direct_physical_bulk_row_page_typed_cells.uint16") == 2,
          "CDP-040 uint16 cells were not stored as typed binary");
  Require(EvidenceU64(result.evidence,
                      "direct_physical_bulk_row_page_typed_cells.uint32") == 2,
          "CDP-040 uint32 cells were not stored as typed binary");
  Require(EvidenceU64(result.evidence,
                      "direct_physical_bulk_row_page_typed_cells.uint64") == 2,
          "CDP-040 uint64 cells were not stored as typed binary");
  Require(EvidenceU64(result.evidence,
                      "direct_physical_bulk_row_page_typed_cells.uint128") == 2,
          "CDP-040 uint128 cells were not stored as 16-byte binary");
  Require(EvidenceU64(result.evidence,
                      "direct_physical_bulk_row_page_typed_cells.bfloat16") == 2,
          "CDP-040 bfloat16 cells were not stored as 2-byte binary");
  Require(EvidenceU64(result.evidence,
                      "direct_physical_bulk_row_page_typed_cells.real16") == 2,
          "CDP-040 real16 cells were not stored as 2-byte binary");
  Require(EvidenceU64(result.evidence,
                      "direct_physical_bulk_row_page_typed_cells.real32") == 2,
          "CDP-040 real32 cells were not stored as typed binary");
  Require(EvidenceU64(result.evidence,
                      "direct_physical_bulk_row_page_typed_cells.real64") == 2,
          "CDP-040 real64 cells were not stored as typed binary");
  Require(EvidenceU64(result.evidence,
                      "direct_physical_bulk_row_page_typed_cells.real128") == 2,
          "CDP-040 real128 cells were not stored as 16-byte binary");
  Require(EvidenceU64(result.evidence,
                      "direct_physical_bulk_row_page_typed_cells.uuid") == 2,
          "CDP-040 UUID cells were not stored as binary16");
  Require(EvidenceU64(result.evidence,
                      "direct_physical_bulk_row_page_typed_cells.ip_address") == 2,
          "CDP-040 IP address cells were not stored as 16-byte binary");
  Require(EvidenceU64(result.evidence,
                      "direct_physical_bulk_row_page_typed_cells.network_prefix") == 2,
          "CDP-040 network prefix cells were not stored as 18-byte binary");
  Require(EvidenceU64(result.evidence,
                      "direct_physical_bulk_row_page_typed_cells.mac_address") == 2,
          "CDP-040 MAC address cells were not stored as 8-byte binary");
  Require(EvidenceU64(result.evidence,
                      "direct_physical_bulk_row_page_typed_cells.enum_value") == 2,
          "CDP-040 enum cells were not stored as binary16");
  Require(EvidenceU64(result.evidence,
                      "direct_physical_bulk_row_page_typed_cells.date") == 2,
          "CDP-040 date cells were not stored as 4-byte binary");
  Require(EvidenceU64(result.evidence,
                      "direct_physical_bulk_row_page_typed_cells.time") == 2,
          "CDP-040 time cells were not stored as 8-byte binary");
  Require(EvidenceU64(result.evidence,
                      "direct_physical_bulk_row_page_typed_cells.timestamp") == 2,
          "CDP-040 timestamp cells were not stored as 16-byte binary");
  Require(EvidenceU64(result.evidence,
                      "direct_physical_bulk_row_page_typed_cells.interval") == 2,
          "CDP-040 interval cells were not stored as 16-byte binary");
  Require(EvidenceU64(result.evidence,
                      "direct_physical_bulk_row_page_typed_cells.binary") == 2,
          "CDP-040 binary cells were not stored as typed binary");
  Require(EvidenceU64(result.evidence,
                      "direct_physical_bulk_row_page_character_cells") == 2,
          "CDP-040 character cells did not remain typed character");
  Require(SelectCount(fixture, context) == 2,
          "CDP-040 typed scalar rows were not visible in writer transaction");
  const auto loaded = api::LoadMgaRelationStoreState(context);
  Require(loaded.ok,
          "CDP-040 typed scalar scoped-row state reload failed");
  bool saw_int32_1973 = false;
  bool saw_int32_2428 = false;
  for (const auto& row : loaded.state.row_versions) {
    if (row.table_uuid != fixture.table_uuid || row.deleted) continue;
    for (const auto& [name, value] : row.values) {
      if (name != "int_i") continue;
      Require(value.isPresent() && value.bytes.size() == 4,
              "CDP-040 scoped INT32 lost its native four-byte carrier");
      saw_int32_1973 = saw_int32_1973 || value.bytes == std::string("\xb5\x07\x00\x00", 4);
      saw_int32_2428 = saw_int32_2428 || value.bytes == std::string("\x7c\x09\x00\x00", 4);
    }
  }
  Require(saw_int32_1973 && saw_int32_2428,
          "CDP-040 scoped typed-row persistence changed native INT32 bytes");
  Commit(context);

  const auto page = ReadPhysicalPage(fixture, fixture.physical_page);
  Require(page.visible_rows.size() == 2,
          "CDP-040 typed scalar physical row-page visible count drifted");
  std::map<dt::CanonicalTypeId, api::EngineApiU64> recovered_counts;
  for (const auto& row : page.visible_rows) {
    for (const auto& cell : row.cells) {
      if (!cell.value.is_null &&
          cell.value.type_id != dt::CanonicalTypeId::character) {
        ++recovered_counts[cell.value.type_id];
      }
    }
  }
  const std::vector<dt::CanonicalTypeId> expected_recovered_types = {
      dt::CanonicalTypeId::int64,
      dt::CanonicalTypeId::boolean,
      dt::CanonicalTypeId::int8,
      dt::CanonicalTypeId::int16,
      dt::CanonicalTypeId::int32,
      dt::CanonicalTypeId::int128,
      dt::CanonicalTypeId::uint8,
      dt::CanonicalTypeId::uint16,
      dt::CanonicalTypeId::uint32,
      dt::CanonicalTypeId::uint64,
      dt::CanonicalTypeId::uint128,
      dt::CanonicalTypeId::bfloat16,
      dt::CanonicalTypeId::real16,
      dt::CanonicalTypeId::real32,
      dt::CanonicalTypeId::real64,
      dt::CanonicalTypeId::real128,
      dt::CanonicalTypeId::uuid,
      dt::CanonicalTypeId::ip_address,
      dt::CanonicalTypeId::network_prefix,
      dt::CanonicalTypeId::mac_address,
      dt::CanonicalTypeId::enum_value,
      dt::CanonicalTypeId::date,
      dt::CanonicalTypeId::time,
      dt::CanonicalTypeId::timestamp,
      dt::CanonicalTypeId::interval,
      dt::CanonicalTypeId::binary,
  };
  for (const auto type_id : expected_recovered_types) {
    Require(recovered_counts[type_id] == 2,
            "CDP-040 recovered typed scalar row-page cell was not typed");
  }
}

void TestNativeUuidCompositeIndex() {
  auto fixture = MakeFixture("uuid_compound_binary", 1750);
  fixture.table_uuid = NewIdentity(platform::UuidKind::object, 1760);
  fixture.index_uuid = NewIdentity(platform::UuidKind::object, 1761);
  auto metadata = Begin(fixture, "uuid-compound-metadata");
  api::CrudTableRecord table;
  table.creator_tx = metadata.local_transaction_id;
  table.table_uuid = fixture.table_uuid;
  table.default_name = "uuid_compound_values";
  table.columns = {{"left", "canonical=uuid"}, {"right", "canonical=uuid"}};
  auto index = IdIndex(fixture, metadata);
  index.column_name = "left";
  index.key_envelopes = {"left", "right", "unique"};
  index.unique = true;
  Require(!PublishFixtureTable(metadata, table).error, "compound UUID catalog publication failed");
  Require(!api::AppendMgaIndexMetadata(metadata, index).error, "compound UUID index publication failed");
  Commit(metadata);

  std::vector<api::EngineRowValue> rows;
  std::vector<std::pair<platform::Uuid, platform::Uuid>> values;
  for (unsigned position = 0; position < 16; ++position) {
    platform::Uuid left, right;
    left.bytes[position] = 0x1f;
    right.bytes.fill(0xff);
    right.bytes[15 - position] = 0;
    values.push_back({left, right});
    values.push_back({left, left}); // Same leading key; suffix must distinguish.
  }
  values.push_back({{}, {}});
  for (const auto& [left, right] : values) {
    api::EngineRowValue row;
    row.fields = {{"left", IdentityScalarValue("uuid", left)},
                  {"right", IdentityScalarValue("uuid", right)}};
    rows.push_back(std::move(row));
  }
  auto writer = Begin(fixture, "uuid-compound-insert");
  auto request = NativeRequest(fixture, writer, rows);
  request.option_envelopes.push_back("physical_mga_cow.rows_per_page=8");
  const auto inserted = api::EngineExecuteNativeBulkIngest(request);
  RequireOk(inserted, "compound UUID insert failed");
  Require(inserted.inserted_rows == rows.size() &&
              EvidenceU64(inserted.evidence, "direct_index_key_sbkobin_keys") == rows.size(),
          "compound UUID insert lost rows or used an untyped physical key");
  Commit(writer);
  fixture.session.reset();
  fixture.session = std::make_shared<scratchbird::tests::FixtureEngineSession>(
      BaseContext(fixture, "uuid-compound-reopen"));
  auto reader = Begin(fixture, "uuid-compound-reader");
  const auto loaded = api::LoadMgaRelationStoreState(reader);
  Require(loaded.ok, "compound UUID durable index reload failed");
  const auto state = api::BuildMgaRelationReadView(loaded.state);
  const auto manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
  const auto datatype = dt::LookupDatatypeCatalogRow(manifest.manifest, dt::CanonicalTypeId::uuid);
  Require(datatype.ok() && datatype.manifest.descriptor_rows.size() == 1,
          "compound UUID comparison descriptor unavailable");
  std::set<std::string> expected_keys, actual_keys;
  for (const auto& [left, right] : values) {
    // Independent byte oracle: no call to the producer's key encoder. Each
    // component has nonnull rank, scalar kind, binary descriptor and epoch,
    // absent collation and zero-escaped complete UUID comparison payload.
    std::string expected = "SBKOBIN:SBKO";
    for (const auto& value : {left, right}) {
      expected.append("\x7f\0\0\0\1", 5);
      const auto& descriptor = datatype.manifest.descriptor_rows.front();
      expected.push_back(static_cast<char>(descriptor.descriptor_uuid.kind));
      expected.append(reinterpret_cast<const char*>(descriptor.descriptor_uuid.value.bytes.data()), 16);
      for (int shift = 56; shift >= 0; shift -= 8)
        expected.push_back(static_cast<char>((descriptor.descriptor_epoch >> shift) & 0xff));
      expected.push_back('\0');
      scratchbird::tests::AppendEscapedUuidIndexOracle(expected, descriptor.descriptor_uuid.value,
          descriptor.descriptor_epoch,
          std::string_view(reinterpret_cast<const char*>(value.bytes.data()), 16));
    }
    expected_keys.insert(std::move(expected));
  }
  for (const auto& entry : state.index_entries) {
    if (entry.index_uuid == fixture.index_uuid) actual_keys.insert(entry.key_value);
  }
  Require(actual_keys == expected_keys, "compound UUID physical replay differs from descriptor-bound key oracle");
  for (const auto& [left, right] : values) {
    for (const std::string kind : {"columns_all_equal", "column_equals", "column_in_list", "column_range"}) {
    api::EnginePredicateEnvelope predicate;
    predicate.predicate_kind = kind;
    predicate.canonical_predicate_envelope = kind == "columns_all_equal" ? "left,right" : "left";
    predicate.bound_values = {IdentityScalarValue("uuid", left)};
    if (kind == "columns_all_equal") predicate.bound_values.push_back(IdentityScalarValue("uuid", right));
    if (kind == "column_range") predicate.bound_values.push_back(IdentityScalarValue("uuid", left));
    const auto expected_count = kind == "columns_all_equal" ? 1u :
        static_cast<unsigned>(std::count_if(values.begin(), values.end(),
            [&](const auto& pair) { return pair.first == left; }));
    const auto found = api::IndexedMgaRowsForPredicateForContext(
        state, fixture.table_uuid, predicate, reader, 0);
    Require(found.ok && found.index_used && !found.index_refused && found.rows.size() == expected_count &&
                std::all_of(found.rows.begin(), found.rows.end(),
                    [&](const auto& row) { return api::CrudRowMatchesPredicate(row, predicate); }),
            "compound UUID durable index did not return the exact tuple without scan fallback");
    }
  }
  // A corrupt projection is an index integrity error, never a successful
  // empty result. Inject into a read snapshot; durable pages remain untouched.
  for (const std::string malformed : {std::string("legacy\x1f" "tuple"), std::string("SBCLKEY1")}) {
    auto corrupt = state;
    bool changed = false;
    for (auto& entry : corrupt.index_entries) {
      if (entry.index_uuid != fixture.index_uuid) continue;
      entry.payload_value = malformed;
      changed = true;
      break;
    }
    Require(changed, "compound corruption probe found no persisted index entry");
    api::EnginePredicateEnvelope predicate;
    predicate.predicate_kind = "column_equals";
    predicate.canonical_predicate_envelope = "left";
    predicate.bound_values = {IdentityScalarValue("uuid", values.front().first)};
    const auto refused = api::IndexedMgaRowsForPredicateForContext(
        corrupt, fixture.table_uuid, predicate, reader, 0);
    constexpr std::string_view expected_detail =
        "mga.secondary_index_delta_overlay:compound_index_logical_key_framing_invalid";
    if (refused.ok || !refused.index_refused || !refused.rows.empty() ||
        refused.diagnostic.detail != expected_detail) {
      std::cerr << "compound corruption: ok=" << refused.ok << " index_refused="
                << refused.index_refused << " rows=" << refused.rows.size()
                << " diagnostic=" << refused.diagnostic.code << ':'
                << refused.diagnostic.detail << '\n';
    }
    Require(!refused.ok && refused.index_refused && refused.rows.empty() &&
                refused.diagnostic.code == "SB_ENGINE_API_INVALID_REQUEST" &&
                refused.diagnostic.detail == expected_detail,
            "malformed compound projection published an empty success or scan fallback");
  }
  Require(SelectCount(fixture, reader) == values.size(), "compound UUID reopened row count changed");
  Rollback(reader);
  for (const std::size_t duplicate : {std::size_t{0}, values.size() - 1}) {
    auto context = Begin(fixture, "uuid-compound-duplicate");
    const auto refused = api::EngineExecuteNativeBulkIngest(
        NativeRequest(fixture, context, {rows[duplicate]}));
    Require(!refused.ok && !refused.diagnostics.empty() &&
                refused.diagnostics.front().code == "CLI.CONSTRAINT_UNIQUE_VIOLATION",
            "compound UUID committed duplicate was not rejected as a unique conflict");
    Require(SelectCount(fixture, context) == values.size(), "compound UUID refused duplicate changed rows");
    Rollback(context);
  }
}

void TestNativeUuidBinaryIndexAndValues(bool unique = true) {
  auto fixture = MakeFixture(unique ? "uuid_binary" : "uuid_binary_nonunique", 1650);
  fixture.table_uuid = NewIdentity(platform::UuidKind::object, 1660);
  fixture.index_uuid = NewIdentity(platform::UuidKind::object, 1661);
  auto metadata = Begin(fixture, "uuid-binary-metadata");
  api::CrudTableRecord table;
  table.creator_tx = metadata.local_transaction_id;
  table.table_uuid = fixture.table_uuid;
  table.default_name = "uuid_binary_values";
  table.columns = {{"id", "canonical=int64"}, {"value", "canonical=uuid"}};
  auto index = IdIndex(fixture, metadata);
  index.column_name = "value";
  index.unique = unique;
  Require(!PublishFixtureTable(metadata, table).error, "UUID bulk catalog publication failed");
  Require(!api::AppendMgaIndexMetadata(metadata, index).error, "UUID bulk index publication failed");
  Commit(metadata);

  std::vector<platform::Uuid> values(9);
  values[1].bytes.fill(0xff);
  values[2].bytes = {0x00,0x11,0x22,0x33,0x44,0x55,0x46,0x77,0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff};
  values[3].bytes = {0xff,0xee,0xdd,0xcc,0xbb,0xaa,0x19,0x88,0x87,0x66,0x55,0x44,0x33,0x22,0x11,0x00};
  values[4].bytes.fill(0x55);
  values[5].bytes.fill(0xaa);
  // UUID data can contain legacy NULL and composite delimiter bytes anywhere.
  values[6].bytes = {'<','N','U','L','L','>',0,1,2,3,4,5,6,7,8,9};
  values[7].bytes = {9,8,7,6,5,4,3,2,1,0,'<','N','U','L','L','>'};
  values[8].bytes = {0x1f,'<','N','U','L','L','>',0x1f,0,0xff,1,2,3,4,5,6};
  for (unsigned bit = 0; bit < 128; ++bit) {
    platform::Uuid value;
    value.bytes[bit / 8] = static_cast<std::uint8_t>(1u << (bit % 8));
    values.push_back(value);
  }
  std::vector<api::EngineRowValue> rows;
  for (std::size_t i = 0; i < values.size(); ++i) {
    api::EngineRowValue row;
    row.fields = {{"id", ScalarValue("int64", std::to_string(i))},
                  {"value", IdentityScalarValue("uuid", values[i])}};
    rows.push_back(std::move(row));
  }
  auto context = Begin(fixture, "uuid-binary-insert");
  auto request = NativeRequest(fixture, context, rows);
  request.option_envelopes.push_back("physical_mga_cow.rows_per_page=8");
  const auto inserted = api::EngineExecuteNativeBulkIngest(request);
  RequireOk(inserted, "UUID binary bulk insert failed");
  Require(inserted.inserted_rows == values.size() &&
              EvidenceU64(inserted.evidence, "direct_index_key_typed_fallback") == 0 &&
              EvidenceU64(inserted.evidence, "direct_index_key_sbkobin_keys") == values.size(),
          "UUID bulk used a textual index fallback or lost rows");
  Commit(context);

  // Close the engine session and create an independent one; the physical read
  // below additionally validates the committed row-page bytes.
  fixture.session.reset();
  fixture.session = std::make_shared<scratchbird::tests::FixtureEngineSession>(
      BaseContext(fixture, "uuid-binary-reopen"));
  auto reader = Begin(fixture, "uuid-binary-read");
  const auto stored = api::LoadMgaRelationStoreState(reader);
  Require(stored.ok, "UUID binary index replay failed");
  const auto manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
  const auto type = dt::LookupDatatypeCatalogRow(manifest.manifest, dt::CanonicalTypeId::uuid);
  Require(type.ok() && type.manifest.descriptor_rows.size() == 1, "UUID datatype absent");
  const auto descriptor = type.manifest.descriptor_rows.front().descriptor_uuid;
  std::set<std::string> expected_values, actual_values;
  std::map<std::string, std::string> native_keys;
  for (const auto& value : values)
    expected_values.emplace(reinterpret_cast<const char*>(value.bytes.data()), 16);
  std::size_t entries = 0;
  for (const auto& entry : stored.state.index_entries) {
    if (entry.index_uuid != fixture.index_uuid) continue;
    ++entries;
    const auto payload = ScalarLogicalPayload(entry.payload_value);
    Require(payload.isPresent() && payload.bytes.size() == 16 &&
                expected_values.contains(payload.bytes),
            "UUID index logical payload is not exact binary16");
    // Independent IPL/DPE bytes, without invoking either production encoder.
    std::string expected = "SBKOBIN:SBKO";
    expected.append("\x7f\0\0\0\1\4", 6);
    expected.append(reinterpret_cast<const char*>(descriptor.value.bytes.data()), 16);
    const auto generation = type.manifest.descriptor_rows.front().descriptor_epoch;
    for (int shift = 56; shift >= 0; shift -= 8)
      expected.push_back(static_cast<char>((generation >> shift) & 0xff));
    expected.push_back('\0');
    scratchbird::tests::AppendEscapedUuidIndexOracle(expected, descriptor.value, generation, payload.bytes);
    Require(entry.key_value == expected,
            "stored UUID key differs from independent canonical binary encoding");
    native_keys.emplace(payload.bytes, entry.key_value);
    actual_values.insert(payload.bytes);
  }
  Require(entries == values.size() && actual_values == expected_values,
          "UUID index replay omitted or duplicated a value");

  // A stored key is not sufficient: exercise each actual indexed lookup and
  // retained row predicate. No SELECT scan fallback can satisfy these checks.
  const auto predicate_state = api::BuildMgaRelationReadView(stored.state);
  for (std::size_t ordinal = 0; ordinal < values.size(); ++ordinal) {
    const std::string expected(reinterpret_cast<const char*>(values[ordinal].bytes.data()), 16);
    for (const std::string kind : {"column_equals", "columns_all_equal", "column_in_list", "column_range"}) {
      api::EnginePredicateEnvelope predicate;
      predicate.predicate_kind = kind;
      predicate.canonical_predicate_envelope = "value";
      predicate.bound_values.push_back(IdentityScalarValue("uuid", values[ordinal]));
      if (kind == "column_range") predicate.bound_values.push_back(predicate.bound_values.front());
      const auto indexed = api::IndexedMgaRowsForPredicateForContext(
          predicate_state, fixture.table_uuid, predicate, reader, 0);
      Require(indexed.ok && indexed.index_used && !indexed.index_refused &&
                  indexed.rows.size() == 1 &&
                  api::CrudFieldValue(indexed.rows.front().values, "value") == expected,
              "UUID indexed predicate lost or aliased binary value: " + kind + ":" + std::to_string(ordinal));
      Require(api::CrudRowMatchesPredicate(indexed.rows.front(), predicate),
              "UUID indexed row predicate disagrees with index");
      if (ordinal < 9) {
        api::EngineSelectRowsRequest select;
        select.context = reader;
        select.source_object.uuid = fixture.table_uuid;
        select.source_object.object_kind = "table";
        select.select_predicate = predicate;
        select.select_projection.canonical_projection_envelopes.push_back("id");
        const auto selected = api::EngineSelectRows(select);
        RequireOk(selected, "UUID SELECT predicate failed");
        const bool index_evidence = std::ranges::any_of(selected.evidence, [](const auto& item) {
          return item.evidence_kind == "index_lookup";
        });
        Require(selected.visible_count == 1 && index_evidence,
                "UUID SELECT required scan fallback instead of its actual index: " + kind);
      }
    }
  }

  // Bounds are raw bytes; all values fall between nil and max, without a
  // numeric conversion or system UUID version/variant validation.
  api::EnginePredicateEnvelope full_range;
  full_range.predicate_kind = "column_range";
  full_range.canonical_predicate_envelope = "value";
  full_range.bound_values = {IdentityScalarValue("uuid", values[0]), IdentityScalarValue("uuid", values[1])};
  const auto ranged = api::IndexedMgaRowsForPredicateForContext(
      predicate_state, fixture.table_uuid, full_range, reader, 0);
  Require(ranged.ok && ranged.index_used && ranged.rows.size() == values.size(),
          "UUID index range did not preserve the complete 128-bit value domain");
  api::CrudRowVersionRecord numeric_looking;
  numeric_looking.values = {{"value", std::string(16, '9')}};
  api::EnginePredicateEnvelope numeric_predicate;
  numeric_predicate.canonical_predicate_envelope = "value";
  numeric_predicate.bound_values = {BinaryScalarValue("uuid", std::vector<platform::u8>(16, '9'))};
  numeric_predicate.bound_values.front().binary_value.back() = '8';
  for (const std::string kind : {"column_equals", "column_in_list", "column_in_list_scalar_compare", "column_less_equal"}) {
    numeric_predicate.predicate_kind = kind;
    Require(!api::CrudRowMatchesPredicate(numeric_looking, numeric_predicate),
            "UUID data was aliased by numeric coercion: " + kind);
  }
  numeric_predicate.predicate_kind = "column_greater";
  Require(api::CrudRowMatchesPredicate(numeric_looking, numeric_predicate),
          "UUID data byte ordering was replaced by numeric coercion");
  for (const std::size_t width : {0u, 15u, 17u, 36u}) {
    api::EngineSelectRowsRequest select;
    select.context = reader;
    select.source_object.uuid = fixture.table_uuid;
    select.source_object.object_kind = "table";
    select.select_predicate.predicate_kind = "column_equals";
    select.select_predicate.canonical_predicate_envelope = "value";
    auto malformed = IdentityScalarValue("uuid", values[2]);
    malformed.binary_value.resize(width);
    select.select_predicate.bound_values.push_back(std::move(malformed));
    const auto refused = api::EngineSelectRows(select);
    Require(!refused.ok && !refused.diagnostics.empty(), "malformed UUID predicate was not refused");
  }

  actual_values.clear();
  for (std::size_t page_index = 0; page_index < (values.size() + 7) / 8; ++page_index) {
    const auto physical = ReadPhysicalPage(fixture, fixture.physical_page + page_index);
    for (const auto& row : physical.visible_rows) {
      for (const auto& cell : row.cells) {
        if (cell.value.type_id != dt::CanonicalTypeId::uuid) continue;
        Require(!cell.value.is_null && cell.value.payload.size() == 16,
                "UUID physical cell lost its binary width or became NULL");
        actual_values.emplace(reinterpret_cast<const char*>(cell.value.payload.data()), 16);
      }
    }
  }
  Require(actual_values == expected_values, "UUID physical replay changed one or more of 128 bits");
  Rollback(reader);

  // A second index built through DDL over committed native rows must publish
  // the same exact physical/logical keys, including for unique indexes.
  const auto ddl_index_uuid = NewIdentity(platform::UuidKind::object, 1662);
  auto ddl_context = Begin(fixture, "uuid-binary-ddl-backfill");
  scratchbird::tests::FixtureEngineRequest<api::EngineCreateIndexRequest> ddl(
      *fixture.session, ddl_context);
  ddl.target_object.uuid = fixture.table_uuid;
  ddl.target_object.object_kind = "table";
  api::EngineIndexDefinition definition;
  definition.requested_index_uuid = ddl_index_uuid;
  definition.physical_profile = api::kCrudIndexProfileRowStoreScalarBtreeV1;
  definition.key_envelopes = {"value"};
  if (unique) definition.key_envelopes.push_back("unique");
  definition.names.push_back({"en", "primary", "", "uuid_ddl_backfill_idx", true});
  ddl.indexes.push_back(std::move(definition));
  RequireOk(api::EngineCreateIndex(ddl), "UUID DDL backfill failed");
  Commit(ddl_context);
  auto ddl_reader = Begin(fixture, "uuid-binary-ddl-reopen");
  const auto ddl_stored = api::LoadMgaRelationStoreState(ddl_reader);
  Require(ddl_stored.ok, "UUID DDL backfill could not be reloaded");
  std::set<std::string> ddl_values;
  std::size_t ddl_entries = 0;
  for (const auto& entry : ddl_stored.state.index_entries) {
    if (entry.index_uuid != ddl_index_uuid) continue;
    const auto logical = ScalarLogicalPayload(entry.payload_value);
    const auto original = native_keys.find(logical.bytes);
    Require(logical.isPresent() && original != native_keys.end() &&
                entry.key_value == original->second,
            "UUID DDL and native producers published different bound physical keys");
    ddl_values.insert(logical.bytes);
    ++ddl_entries;
  }
  Require(ddl_entries == values.size() && ddl_values == expected_values,
          "UUID DDL backfill omitted or duplicated committed binary values");
  Rollback(ddl_reader);

  if (!unique) return; // Duplicate refusal is a unique-index contract; all read checks ran above.

  // Nil and marker-shaped UUIDs are present data, never SQL NULL. The actual
  // committed unique index must reject them, even in a new statement/session.
  for (const std::size_t ordinal : {0u, 2u, 6u, 7u, 8u}) {
    auto duplicate_context = Begin(fixture, "uuid-binary-duplicate");
    api::EngineRowValue duplicate;
    duplicate.fields = {{"id", ScalarValue("int64", "1000")},
                        {"value", IdentityScalarValue("uuid", values[ordinal])}};
    const auto refused = api::EngineExecuteNativeBulkIngest(
        NativeRequest(fixture, duplicate_context, {duplicate}));
    Require(!refused.ok && !refused.diagnostics.empty(),
            "UUID duplicate was accepted or lacked a refusal diagnostic at ordinal " +
                std::to_string(ordinal));
    // The proof compares canonical bound keys, not raw logical tuples. Every
    // stored key above was checked against an independent byte oracle. Remove
    // only the storage-carrier tag; keep the complete SBKO comparison bytes.
    const std::string raw_uuid(reinterpret_cast<const char*>(values[ordinal].bytes.data()), 16);
    const auto comparison_key = native_keys.at(raw_uuid).substr(8);
    const auto expected_detail = "bulk_unique_proof_persisted_conflict:key_bytes=" +
        std::to_string(comparison_key.size()) + ":key_redacted=true";
    const auto proof = std::find_if(refused.diagnostics.begin(), refused.diagnostics.end(),
        [&](const auto& diagnostic) {
          return diagnostic.code == "CLI.CONSTRAINT_UNIQUE_VIOLATION" &&
              diagnostic.detail.find(expected_detail) != std::string::npos;
        });
    Require(proof != refused.diagnostics.end(), "UUID duplicate lost its real binary-key proof detail");
    Require(std::any_of(proof->identity_fields.begin(), proof->identity_fields.end(),
        [&](const auto& field) { return field.first == "index_uuid" && field.second == fixture.index_uuid; }),
        "UUID duplicate lost its bound binary index identity");
    const auto key_digest = scratchbird::core::hash::ComputeSha256Digest(
        reinterpret_cast<const platform::byte*>(comparison_key.data()), comparison_key.size());
    Require(key_digest.ok() && HasEvidence(refused.evidence, "bulk_unique_proof_conflict_key",
                "sha256:" + scratchbird::core::hash::HexLower(key_digest.digest)) &&
            EvidenceU64(refused.evidence, "bulk_unique_proof_conflict_key.bytes") == comparison_key.size() &&
            HasEvidence(refused.evidence, "bulk_unique_proof_conflict_key.redacted", "true"),
            "UUID conflict proof fingerprint did not match the independently verified physical key");
    Require(SelectCount(fixture, duplicate_context) == values.size(),
            "refused UUID duplicate changed visible rows");
    Rollback(duplicate_context);
  }

  for (const std::size_t width : {0u, 15u, 17u, 36u}) {
    auto malformed_context = Begin(fixture, "uuid-binary-invalid-width");
    api::EngineRowValue malformed;
    malformed.fields = {{"id", ScalarValue("int64", "2000")},
                        {"value", BinaryScalarValue("uuid", std::vector<std::uint8_t>(width, 0x41))}};
    const auto refused = api::EngineExecuteNativeBulkIngest(
        NativeRequest(fixture, malformed_context, {malformed}));
    Require(!refused.ok && !refused.diagnostics.empty(), "malformed UUID width was accepted");
    Require(SelectCount(fixture, malformed_context) == values.size(),
            "malformed UUID width changed visible rows");
    Rollback(malformed_context);
  }

  for (const bool null_with_payload : {false, true}) {
    auto malformed_context = Begin(fixture, "uuid-binary-ambiguous-carrier");
    auto value = IdentityScalarValue("uuid", values[2]);
    if (null_with_payload) value.is_null = true;
    else value.encoded_value = "00112233-4455-4677-8899-aabbccddeeff";
    api::EngineRowValue malformed;
    malformed.fields = {{"id", ScalarValue("int64", "3000")}, {"value", value}};
    const auto refused = api::EngineExecuteNativeBulkIngest(
        NativeRequest(fixture, malformed_context, {malformed}));
    Require(!refused.ok && !refused.diagnostics.empty(),
            "dual UUID carrier or payload-bearing SQL NULL was accepted");
    Require(SelectCount(fixture, malformed_context) == values.size(),
            "malformed UUID carrier changed visible rows");
    Rollback(malformed_context);
  }
}

void TestMalformedInlineFixedTypedValueRefuses() {
  auto fixture = MakeTypedScalarFixture("malformed_inline_fixed", 1600);
  auto context = Begin(fixture, "cdp040-malformed-inline-fixed");
  auto bad_row = TypedScalarRow(1);
  bad_row.fields.front().second.binary_value.clear();
  bad_row.fields.front().second.encoded_value = "not-an-int64";
  const auto result = api::EngineExecuteNativeBulkIngest(
      NativeRequest(fixture, context, {std::move(bad_row)}));
  Require(!result.ok, "CDP-040 malformed fixed-width value was accepted");
  Require(!result.diagnostics.empty(),
          "CDP-040 malformed fixed-width refusal did not return diagnostics");
  Rollback(context);
}

void TestDescriptorPayloadRowPageStorage() {
  auto fixture = MakeDescriptorPayloadFixture("descriptor_payload", 1800);
  auto context = Begin(fixture, "cdp040-descriptor-payload-storage");
  const auto result = api::EngineExecuteNativeBulkIngest(
      NativeRequest(fixture, context, DescriptorPayloadRows()));
  RequireOk(result, "CDP-040 descriptor payload native bulk ingest failed");
  Require(result.accepted_rows == 2 && result.inserted_rows == 2,
          "CDP-040 descriptor payload row counts drifted");
  const auto& types = DescriptorPayloadTypeNames();
  Require(EvidenceU64(result.evidence,
                      "direct_physical_bulk_row_page_typed_binary_cells") ==
              static_cast<api::EngineApiU64>(types.size() * 2),
          "CDP-040 descriptor payload cells fell back to text");
  for (const auto& type : types) {
    Require(EvidenceU64(result.evidence,
                        std::string("direct_physical_bulk_row_page_typed_cells.") + type) == 2,
            "CDP-040 descriptor payload type did not remain typed on row page");
  }
  Require(SelectCount(fixture, context) == 2,
          "CDP-040 descriptor payload rows were not visible in writer transaction");
  Commit(context);

  const auto page = ReadPhysicalPage(fixture, fixture.physical_page);
  Require(page.visible_rows.size() == 2,
          "CDP-040 descriptor physical row-page visible count drifted");
  std::map<dt::CanonicalTypeId, api::EngineApiU64> recovered_counts;
  for (const auto& row : page.visible_rows) {
    for (const auto& cell : row.cells) {
      if (!cell.value.is_null) {
        ++recovered_counts[cell.value.type_id];
      }
    }
  }
  for (const auto& type : types) {
    const auto type_id = dt::CanonicalTypeIdFromStableName(type);
    Require(type_id != dt::CanonicalTypeId::unknown,
            "CDP-040 descriptor payload fixture used an unknown canonical type");
    Require(recovered_counts[type_id] == 2,
            "CDP-040 recovered descriptor row-page cell was not typed");
  }
}

void TestDecimalIndexKeysPreserveNumericOrder(bool decimal128 = false) {
  const std::string type = decimal128 ? "decimal_float" : "decimal";
  auto fixture = MakeDescriptorPayloadFixture(type + "_numeric_order", decimal128 ? 1891 : 1890, true);
  auto context = Begin(fixture, "cdp040-decimal-order");
  const auto& types = DescriptorPayloadTypeNames();
  const auto found = std::find(types.begin(), types.end(), type);
  Require(found != types.end(), "CDP-040 decimal datatype fixture missing");
  const auto ordinal = static_cast<std::size_t>(found - types.begin());
  const auto column = DescriptorPayloadColumnName(ordinal);
  // Independent mathematical order: little-endian coefficients and scales
  // must not be mistaken for lexicographic numeric sort keys.
  const std::vector<std::string> ordered = decimal128 ? std::vector<std::string>{
      "-Infinity", "-1e6144", "-257", "-256", "-255", "-10", "-2.5", "-2.05",
      "-2", "-1.99", "-0.01", "-1e-6176", "0", "1e-6176", "0.01", "1.99",
      "2", "2.05", "2.5", "10", "255", "256", "257", "1e6144", "Infinity", "NaN"}
      : std::vector<std::string>{
      "-99999999999999999999999999999999999999", "-257", "-256", "-255", "-10",
      "-2.5", "-2.05", "-2", "-1.99", "-0.01", "-0.00000000000000000000000000000000000001",
      "0", "0.00000000000000000000000000000000000001", "0.01", "1.99", "2", "2.05", "2.5",
      "10", "255", "256", "257", "99999999999999999999999999999999999999"};
  const auto encode = [&](std::string_view lexical) -> std::vector<std::uint8_t> {
    if (decimal128) {
      const auto value = scratchbird::libraries::sbl_numeric::EncodeDecimal128LittleEndian(lexical, true);
      Require(value.bytes.has_value(), "CDP-040 decimal128 test value invalid");
      return {value.bytes->begin(), value.bytes->end()};
    }
    const auto value = scratchbird::libraries::sbl_numeric::EncodeExactDecimalLittleEndian(lexical);
    Require(value.ok, "CDP-040 exact decimal test value invalid");
    return {value.canonical_bytes.begin(), value.canonical_bytes.end()};
  };
  std::vector<api::EngineRowValue> rows;
  std::vector<api::EngineUuid> identities;
  auto first_alias = DescriptorPayloadRow(201);
  auto second_alias = DescriptorPayloadRow(202);
  first_alias.fields[ordinal].second = BinaryScalarValue(type, encode("3.25"));
  second_alias.fields[ordinal].second = BinaryScalarValue(type, encode("3.250"));
  const auto duplicate_batch = api::EngineExecuteNativeBulkIngest(
      NativeRequest(fixture, context, {first_alias, second_alias}));
  Require(!duplicate_batch.ok && std::any_of(duplicate_batch.diagnostics.begin(), duplicate_batch.diagnostics.end(),
      [](const auto& d) { return d.code == "CLI.CONSTRAINT_UNIQUE_VIOLATION"; }) &&
      SelectCount(fixture, context) == 0,
      "CDP-040 decimal equivalent values escaped empty-target batch uniqueness or left rows: " + type + ":" +
          (duplicate_batch.ok ? "accepted" : duplicate_batch.diagnostics.empty() ? "no diagnostic" :
              duplicate_batch.diagnostics.front().code + ":" + duplicate_batch.diagnostics.front().detail));
  for (std::size_t i = 0; i < ordered.size(); ++i) {
    auto row = DescriptorPayloadRow(static_cast<int>(i + 1));
    row.fields[ordinal].second = BinaryScalarValue(type, encode(ordered[i]));
    identities.push_back(row.requested_row_uuid);
    rows.push_back(std::move(row));
  }
  const auto result = api::EngineExecuteNativeBulkIngest(NativeRequest(fixture, context, rows));
  RequireOk(result, "CDP-040 decimal numeric-order ingest failed");
  const auto loaded = api::LoadMgaRelationStoreState(context);
  Require(loaded.ok, "CDP-040 decimal numeric-order state load failed");
  api::EngineUuid index_uuid;
  for (const auto& index : loaded.state.relation_metadata.indexes)
    if (index.table_uuid == fixture.table_uuid && index.column_name == column)
      index_uuid = index.index_uuid;
  Require(uuid::IsEngineIdentityUuid(index_uuid), "CDP-040 decimal index identity missing");
  std::vector<std::pair<std::string, api::EngineUuid>> keys;
  for (const auto& entry : loaded.state.index_entries)
    if (entry.index_uuid == index_uuid) keys.emplace_back(entry.key_value, entry.row_uuid);
  Require(keys.size() == ordered.size(), "CDP-040 decimal index membership count drifted");
  std::sort(keys.begin(), keys.end(), [](const auto& a, const auto& b) {
    return std::lexicographical_compare(a.first.begin(), a.first.end(), b.first.begin(), b.first.end(),
        [](unsigned char left, unsigned char right) { return left < right; });
  });
  for (std::size_t i = 0; i < keys.size(); ++i)
    Require(keys[i].second == identities[i], "CDP-040 decimal index byte order differs from numeric order");
  Commit(context);
  const auto recovered = api::LoadMgaRelationStoreState(BaseContext(fixture, "decimal-index-reload"));
  Require(recovered.ok, "CDP-040 committed decimal index state failed to reload");
  std::vector<std::pair<std::string, api::EngineUuid>> reloaded;
  for (const auto& entry : recovered.state.index_entries)
    if (entry.index_uuid == index_uuid) reloaded.emplace_back(entry.key_value, entry.row_uuid);
  std::sort(reloaded.begin(), reloaded.end(), [](const auto& a, const auto& b) {
    return std::lexicographical_compare(a.first.begin(), a.first.end(), b.first.begin(), b.first.end(),
        [](unsigned char left, unsigned char right) { return left < right; });
  });
  Require(reloaded == keys, "CDP-040 committed decimal index keys changed on reload");
  auto duplicate_context = Begin(fixture, "cdp040-decimal-equality");
  auto duplicate = DescriptorPayloadRow(100);
  duplicate.fields[ordinal].second = BinaryScalarValue(type, encode("2.50"));
  const auto refused = api::EngineExecuteNativeBulkIngest(
      NativeRequest(fixture, duplicate_context, {duplicate}));
  Require(!refused.ok && std::any_of(refused.diagnostics.begin(), refused.diagnostics.end(),
      [](const auto& diagnostic) { return diagnostic.code == "CLI.CONSTRAINT_UNIQUE_VIOLATION"; }),
      "CDP-040 decimal unique index accepted a numerically equivalent value");
  Require(SelectCount(fixture, duplicate_context) == ordered.size(),
          "CDP-040 refused decimal duplicate changed durable row membership");
  Rollback(duplicate_context);
  if (decimal128) {
    for (const auto lexical : {"-0e-6176", "0e6111", "-NaN42", "sNaN999", "250e-2"}) {
      auto attempt = Begin(fixture, "cdp040-decimal128-equivalence");
      auto row = DescriptorPayloadRow(101);
      row.fields[ordinal].second = BinaryScalarValue(type, encode(lexical));
      const auto result = api::EngineExecuteNativeBulkIngest(NativeRequest(fixture, attempt, {row}));
      Require(!result.ok && std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
          [](const auto& d) { return d.code == "CLI.CONSTRAINT_UNIQUE_VIOLATION"; }),
          "CDP-040 decimal128 cohort/zero/NaN equivalence escaped uniqueness");
      Require(SelectCount(fixture, attempt) == ordered.size(), "decimal128 duplicate changed row membership");
      Rollback(attempt);
    }
    auto attempt = Begin(fixture, "cdp040-decimal128-malformed");
    auto row = DescriptorPayloadRow(102);
    auto malformed = encode("1"); malformed[15] = 0x60;
    row.fields[ordinal].second = BinaryScalarValue(type, malformed);
    const auto refused = api::EngineExecuteNativeBulkIngest(NativeRequest(fixture, attempt, {row}));
    Require(!refused.ok && !refused.diagnostics.empty() && SelectCount(fixture, attempt) == ordered.size(),
            "noncanonical decimal128 ingest changed durable rows");
    Rollback(attempt);
  }
}

void TestDescriptorPayloadIndexKeysUseBinaryPayloads() {
  auto fixture =
      MakeDescriptorPayloadFixture("descriptor_payload_index_keys",
                                   1875,
                                   true);
  auto context = Begin(fixture, "cdp040-descriptor-payload-index-keys");
  const auto result = api::EngineExecuteNativeBulkIngest(
      NativeRequest(fixture, context, {DescriptorPayloadRow(1)}));
  RequireOk(result, "CDP-040 descriptor payload index native bulk ingest failed");
  const auto expected =
      static_cast<api::EngineApiU64>(DescriptorPayloadTypeNames().size());
  const auto candidates =
      EvidenceU64(result.evidence, "direct_index_key_typed_candidates");
  const auto encoded =
      EvidenceU64(result.evidence, "direct_index_key_typed_encoded");
  const auto fallback =
      EvidenceU64(result.evidence, "direct_index_key_typed_fallback");
  const auto sbkobin =
      EvidenceU64(result.evidence, "direct_index_key_sbkobin_keys");
  Require(candidates == expected,
          "CDP-040 descriptor payload indexes did not inspect every typed key");
  Require(encoded == expected,
          "CDP-040 descriptor payload indexes did not encode every typed key");
  Require(fallback == 0,
          "CDP-040 descriptor payload indexes fell back to display text");
  Require(sbkobin == expected,
          "CDP-040 descriptor payload indexes did not emit SBKOBIN keys");

  const auto loaded = api::LoadMgaRelationStoreState(context);
  Require(loaded.ok, "CDP-040 descriptor payload index state reload failed");
  api::EngineApiU64 sbkobin_count = 0;
  for (const auto& entry : loaded.state.index_entries) {
    if (entry.table_uuid == fixture.table_uuid &&
        entry.key_value.rfind("SBKOBIN:", 0) == 0) {
      ++sbkobin_count;
    }
  }
  Require(sbkobin_count == expected,
          "CDP-040 descriptor payload persisted index keys were not SBKOBIN");
  Commit(context);
}

api::CrudTableRecord OpaqueRenderOnlyPayloadTable(
    const Fixture& fixture,
    const api::EngineRequestContext& context,
    const std::string& type_name) {
  api::CrudTableRecord table;
  table.creator_tx = context.local_transaction_id;
  table.table_uuid = fixture.table_uuid;
  table.default_name = "cdp_native_bulk_ingest_opaque_refusal";
  table.columns.push_back({"opaque_payload", "canonical=" + type_name});
  return table;
}

Fixture MakeOpaqueRenderOnlyPayloadFixture(std::string name,
                                           platform::u64 salt,
                                           const std::string& type_name) {
  Fixture fixture;
  fixture.salt = salt;
  InitializeOwnedFixtureDirectory(fixture, name);

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = NewTypedUuid(platform::UuidKind::database, salt + 1);
  create.filespace_uuid = NewTypedUuid(platform::UuidKind::filespace, salt + 2);
  create.creation_unix_epoch_millis = UniqueMillis();
  scratchbird::tests::ConfigureCredentialedFixtureBootstrap(create);
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "CDP-040 opaque payload database create failed");

  fixture.database_uuid = create.database_uuid.value;
  fixture.owner_context = scratchbird::tests::BootstrapFixtureOwnerContext(create);
  fixture.page_size = create.page_size;
  fixture.table_uuid = NewIdentity(platform::UuidKind::object, salt + 10);

  auto metadata = Begin(fixture, "cdp040-opaque-payload-metadata");
  const auto table = PublishFixtureTable(
      metadata, OpaqueRenderOnlyPayloadTable(fixture, metadata, type_name));
  Require(!table.error, "CDP-040 opaque payload table metadata append failed");
  Commit(metadata);
  StartFixtureSession(fixture, metadata);
  return fixture;
}

api::EngineRowValue OpaqueRenderOnlyPayloadRow(const std::string& type_name) {
  api::EngineRowValue row;
  row.requested_row_uuid =
      NewIdentity(platform::UuidKind::object, 1811);
  row.fields.push_back(
      {"opaque_payload",
       BinaryScalarValue(type_name,
                         DescriptorPayloadForType(type_name, 1, 0))});
  return row;
}

void TestOpaqueRenderOnlyDescriptorPayloadRefusals() {
  platform::u64 salt = 1900;
  for (const auto& type : OpaqueRenderOnlyPayloadTypeNames()) {
    auto fixture =
        MakeOpaqueRenderOnlyPayloadFixture("opaque_payload_" + type, salt, type);
    auto context = Begin(fixture, "cdp040-opaque-payload-refusal");
    const auto result = api::EngineExecuteNativeBulkIngest(
        NativeRequest(fixture, context, {OpaqueRenderOnlyPayloadRow(type)}));
    RequireDiagnostic(result,
                      "SB_ENGINE_API_UNSUPPORTED_PROFILE",
                      "opaque_column_mutation_denied",
                      "CDP-040 opaque render-only payload mutation was accepted");
    Rollback(context);
    salt += 100;
  }
}

void TestOpaqueRenderOnlyDescriptorPayloadExplicitAllow() {
  platform::u64 salt = 1950;
  for (const auto& type : OpaqueRenderOnlyPayloadTypeNames()) {
    auto fixture =
        MakeOpaqueRenderOnlyPayloadFixture("opaque_payload_allow_" + type,
                                           salt,
                                           type);
    auto context = Begin(fixture, "cdp040-opaque-payload-explicit-allow");
    auto request = NativeRequest(fixture,
                                 context,
                                 {OpaqueRenderOnlyPayloadRow(type)});
    request.option_envelopes.push_back("bulk.allow_opaque_columns=true");
    const auto result = api::EngineExecuteNativeBulkIngest(request);
    RequireOk(result, "CDP-040 explicit opaque payload allow failed");
    Require(result.accepted_rows == 1 && result.inserted_rows == 1,
            "CDP-040 explicit opaque payload row counts drifted");
    Require(EvidenceU64(result.evidence,
                        "direct_physical_bulk_row_page_typed_binary_cells") == 1,
            "CDP-040 explicit opaque payload fell back to text");
    Require(EvidenceU64(result.evidence,
                        "direct_physical_bulk_row_page_typed_cells." + type) == 1,
            "CDP-040 explicit opaque payload did not remain typed on row page");
    Require(SelectCount(fixture, context) == 1,
            "CDP-040 explicit opaque payload row was not visible in writer transaction");
    Commit(context);

    const auto page = ReadPhysicalPage(fixture, fixture.physical_page);
    Require(page.visible_rows.size() == 1,
            "CDP-040 explicit opaque physical row-page visible count drifted");
    api::EngineApiU64 recovered_opaque_cells = 0;
    for (const auto& row : page.visible_rows) {
      for (const auto& cell : row.cells) {
        if (!cell.value.is_null &&
            cell.value.type_id == dt::CanonicalTypeId::opaque_extension) {
          ++recovered_opaque_cells;
        }
      }
    }
    Require(recovered_opaque_cells == 1,
            "CDP-040 recovered explicit opaque row-page cell was not typed");
    salt += 100;
  }
}

void TestDisabledAndInvalidRefusals() {
  auto fixture = MakeFixture("refusals", 2000);
  auto context = Begin(fixture, "cdp040-refusals");

  auto disabled_request = NativeRequest(fixture, context, Rows("disabled", 1));
  disabled_request.option_envelopes.push_back("native_bulk_ingest_enabled:false");
  const auto disabled = api::EngineExecuteNativeBulkIngest(disabled_request);
  RequireDiagnostic(disabled,
                    "DML.NATIVE_BULK_INGEST.DISABLED",
                    "native_bulk_ingest_enabled:false",
                    "CDP-040 disabled refusal diagnostic drifted");
  Require(HasEvidence(disabled.evidence, "native_bulk_ingest", "disabled"),
          "CDP-040 disabled native ingest evidence missing");
  Require(HasEvidence(disabled.evidence, "native_bulk_ingest_enabled", "false"),
          "CDP-040 disabled option evidence missing");

  auto invalid = NativeRequest(fixture, context, {});
  const auto invalid_result = api::EngineExecuteNativeBulkIngest(invalid);
  RequireDiagnostic(invalid_result,
                    "SB_ENGINE_API_INVALID_REQUEST",
                    "native_rowset_required",
                    "CDP-040 invalid native ingest diagnostic drifted");
  Require(HasEvidence(invalid_result.evidence, "native_bulk_ingest_source", "binary_typed_rows"),
          "CDP-040 invalid path lost native source evidence");
  Rollback(context);
}

void TestLogicalStatementRollbackOnFaultAndCancellation() {
  {
    auto fixture = MakeFixture("logical_batch_row_append_fault", 2250);
    auto context = Begin(fixture, "cdp040-logical-batch-row-append-fault");
    auto request = NativeRequest(fixture, context, Rows("fault", 4));
    request.option_envelopes.push_back(
        "ipar.fault_injection.point=row_append");
    request.option_envelopes.push_back("result_payload_policy:summary_only");
    const auto failed = api::EngineExecuteNativeBulkIngest(request);
    Require(!failed.ok,
            "CDP-040 injected native-bulk row append fault was accepted");
    Require(HasEvidence(failed.evidence,
                        "native_bulk_logical_batch",
                        "rolled_back"),
            "CDP-040 injected native-bulk fault did not roll back its logical batch");
    Require(HasEvidence(failed.evidence,
                        "native_bulk_logical_batch_atomicity",
                        "no_partial_visibility"),
            "CDP-040 injected native-bulk fault lost no-partial-visibility evidence");
    Require(SelectCount(fixture, context) == 0,
            "CDP-040 injected native-bulk fault left partially visible rows");
    auto recovery_context = context;
    recovery_context.request_id += "-recovery";
    auto recovery_request =
        NativeRequest(fixture, recovery_context, Rows("recovered", 4));
    recovery_request.option_envelopes.push_back(
        "result_payload_policy:summary_only");
    const auto recovered = api::EngineExecuteNativeBulkIngest(recovery_request);
    RequireOk(recovered,
              "CDP-040 native-bulk recovery after statement rollback failed");
    Require(HasEvidence(recovered.evidence,
                        "native_bulk_logical_batch",
                        "published") &&
                SelectCount(fixture, context) == 4,
            "CDP-040 recovered native-bulk statement did not publish exactly once");
    Rollback(context);
  }

  {
    auto fixture = MakeFixture("logical_batch_polled_cancel", 2350);
    auto context = Begin(fixture, "cdp040-logical-batch-polled-cancel");
    int cancellation_probes = 0;
    context.query_cancellation_requested = [&cancellation_probes]() {
      return ++cancellation_probes >= 3;
    };
    auto request = NativeRequest(fixture, context, Rows("cancel", 4));
    request.option_envelopes.push_back("result_payload_policy:summary_only");
    const auto cancelled = api::EngineExecuteNativeBulkIngest(request);
    // Canonical storage also polls cancellation. The third poll is not a
    // stable publication cut point; assert the cancellation and statement
    // rollback contract, not a caller-specific diagnostic detail string.
    RequireDiagnostic(cancelled,
                      "PROCESS.CANCELLED",
                      "",
                      "CDP-040 native-bulk polled cancellation drifted");
    Require(cancellation_probes >= 3, "CDP-040 cancellation was not polled");
    Require(HasEvidence(cancelled.evidence,
                        "native_bulk_logical_batch",
                        "rolled_back"),
            "CDP-040 cancelled native-bulk statement did not roll back");
    Require(HasEvidence(cancelled.evidence,
                        "native_bulk_logical_batch_atomicity",
                        "no_partial_visibility"),
            "CDP-040 cancelled native-bulk statement lost atomicity evidence");
    context.query_cancellation_requested = {};
    Require(SelectCount(fixture, context) == 0,
            "CDP-040 cancelled native-bulk statement left partially visible rows");
    Rollback(context);
  }
}

void TestServerPublicAbiRoute() {
  auto fixture = MakeFixture("server_public_abi", 2500);
  auto context = Begin(fixture, "cdp040-server-public-abi");
  std::array<std::uint8_t, 16> session_uuid{};
  auto registry = MakeServerRegistry(fixture, context, &session_uuid);
  const auto engine_state = MakeEngineState(fixture);

  const auto execute_frame = CanonicalNativeServerFrame(
      &registry, engine_state, fixture, context, session_uuid,
      Rows("server", 2));
  const auto execute = server::HandleExecuteSblr(
      &registry,
      engine_state,
      execute_frame);
  Require(execute.accepted, "CDP-040 server public ABI native ingest was rejected");
  const auto payload = DecodeExecuteResultPayload(execute.payload);
  Require(payload.accepted && execute.response_schema_id == 4012,
          "CDP-040 server native ingest outcome/schema drifted");
  Require(payload.selected_transaction_present &&
              payload.selected_transaction.local_transaction_id == context.local_transaction_id &&
              payload.selected_transaction.transaction_uuid == context.transaction_uuid &&
              !payload.finalized_transaction_present && !payload.replacement_transaction_present,
          "CDP-040 native response lost its actual transaction owner or fabricated finality");
  Require(payload.operation_id == "dml.execute_native_bulk_ingest",
          "CDP-040 server native ingest operation id drifted");
  namespace packet = scratchbird::wire::public_result;
  const auto operation = packet::Find(payload.row_packet, "operation_id");
  Require(operation && operation->kind == packet::Kind::text &&
              operation->value == "dml.execute_native_bulk_ingest",
          "CDP-040 server native ingest did not return native operation packet");
  const auto summary = packet::Find(payload.row_packet, "row[0]");
  std::vector<packet::Field> summary_fields;
  Require(summary && summary->kind == packet::Kind::row &&
              packet::Decode(summary->value, &summary_fields) && summary_fields.size() == 3,
          "CDP-040 native ingest summary is not one exact framed three-field row");
  const auto text_equals = [](std::string_view bytes, std::string_view name, std::string_view expected) {
    const auto field = packet::Find(bytes, name);
    return field && field->kind == packet::Kind::text && field->value == expected;
  };
  const auto count_evidence = packet::Evidence(payload.row_packet, "direct_physical_bulk_row_count");
  const auto policy_evidence = packet::Evidence(payload.row_packet, "result_payload_policy");
  Require(payload.row_count == 2 &&
              text_equals(payload.row_packet, "row_count", "1") &&
              !packet::Find(payload.row_packet, "row[1]") &&
              text_equals(summary->value, "accepted_rows", "2") &&
              text_equals(summary->value, "inserted_rows", "2") &&
              text_equals(summary->value, "rejected_rows", "0") &&
              count_evidence && count_evidence->kind == packet::Kind::text && count_evidence->value == "2" &&
              policy_evidence && policy_evidence->kind == packet::Kind::text && policy_evidence->value == "summary_only",
          "CDP-040 bounded native ingest summary evidence drifted");
  Require(!Contains(payload.row_packet, "server-payload-1") &&
              !Contains(payload.row_packet, "server-payload-2"),
          "CDP-040 summary-only native ingest leaked per-row payloads");
  Require(SelectCount(fixture, context) == 2,
          "CDP-040 server public ABI native ingest rows not visible in transaction");

  const auto replay =
      server::HandleExecuteSblr(&registry, engine_state, execute_frame);
  Require(!replay.accepted && !replay.diagnostics.empty() &&
              replay.diagnostics.front().code ==
                  "PARSER_SERVER_IPC.STATEMENT_CONTEXT_RECEIPT_MISMATCH",
          "CDP-040 consumed native-bulk statement receipt replay was admitted");
  Require(SelectCount(fixture, context) == 2,
          "CDP-040 native-bulk receipt replay duplicated visible rows");

  const auto disabled = server::HandleExecuteSblr(
      &registry,
      engine_state,
      CanonicalNativeServerFrame(&registry, engine_state, fixture, context,
                                 session_uuid, Rows("server-disabled", 1),
                                 false));
  Require(!disabled.accepted, "CDP-040 server disabled native ingest was accepted");
  Require(!disabled.diagnostics.empty(), "CDP-040 server disabled native ingest lacked diagnostics");
  Require(disabled.diagnostics.front().code == "DML.NATIVE_BULK_INGEST.DISABLED",
          "CDP-040 server disabled diagnostic drifted");
  Rollback(context);
}

void TestRollbackInvisibilityAndCommittedReopenVisibility() {
  auto rollback_fixture = MakeFixture("rollback", 3000);
  auto rollback_context = Begin(rollback_fixture, "cdp040-rollback-writer");
  const auto rolled = api::EngineExecuteNativeBulkIngest(
      NativeRequest(rollback_fixture, rollback_context, Rows("rollback", 4)));
  RequireOk(rolled, "CDP-040 rollback native ingest failed before rollback");
  Rollback(rollback_context);

  auto rollback_reader = Begin(rollback_fixture, "cdp040-rollback-reader");
  Require(SelectCount(rollback_fixture, rollback_reader) == 0,
          "CDP-040 rolled-back native ingest rows became visible");
  Rollback(rollback_reader);

  auto commit_fixture = MakeFixture("commit_reopen", 4000);
  auto commit_context = Begin(commit_fixture, "cdp040-commit-writer");
  const auto committed = api::EngineExecuteNativeBulkIngest(
      NativeRequest(commit_fixture, commit_context, Rows("commit", 5)));
  RequireOk(committed, "CDP-040 committed native ingest failed");
  Commit(commit_context);

  const auto opened = db::OpenDatabaseFile({commit_fixture.database_path.string(), false, false, false});
  Require(opened.ok(), "CDP-040 committed database did not reopen");

  auto reopen_reader = Begin(commit_fixture, "cdp040-reopen-reader");
  Require(SelectCount(commit_fixture, reopen_reader) == 5,
          "CDP-040 committed native ingest rows were not visible after reopen");
  Rollback(reopen_reader);
}

std::string ReadTextFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  Require(static_cast<bool>(in), "CDP-040 could not open recovery evidence");
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void WriteTextFile(const std::filesystem::path& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  Require(static_cast<bool>(out), "CDP-040 could not open dirty manifest");
  out << text;
  out.flush();
  Require(static_cast<bool>(out), "CDP-040 could not write dirty manifest");
}

void WriteRecoverableDirtyManifest(const Fixture& fixture) {
  db::DirtyObjectManifest manifest;
  manifest.checkpoint_generation = 1;
  manifest.completed = true;
  manifest.classification_only = true;

  db::DirtyObjectManifestEntry entry;
  entry.kind = db::DirtyObjectKind::catalog_page;
  entry.object_uuid = NewTypedUuid(platform::UuidKind::object, fixture.salt + 500);
  entry.page_number = db::kCatalogPageNumber;
  entry.page_generation = 1;
  entry.object_checksum = 177;
  entry.local_transaction_id = 2;
  entry.operation_envelope_checksum = 277;
  entry.transaction_evidence_checksum = 377;
  entry.dirty = true;
  entry.authoritative = true;
  manifest.entries.push_back(entry);

  const auto built = db::BuildDirtyObjectManifest(manifest);
  if (!built.ok()) {
    std::cerr << built.diagnostic.diagnostic_code << '\n';
  }
  Require(built.ok(), "CDP-040 recoverable dirty manifest did not build");
  WriteTextFile(fixture.database_path.string() + ".dirty.manifest", built.serialized);
}

void TestReopenRecoveryEvidence() {
  auto fixture = MakeFixture("recovery", 5000);
  const auto first_open = db::OpenDatabaseFile({fixture.database_path.string(), false, false, false});
  Require(first_open.ok(), "CDP-040 recovery fixture first open failed");

  WriteRecoverableDirtyManifest(fixture);
  const auto recovered = db::OpenDatabaseFile({fixture.database_path.string(), false, false, false});
  Require(recovered.ok(), "CDP-040 dirty manifest recovery open failed");
  Require(recovered.state.startup_recovery_classification == "repaired_recovery",
          "CDP-040 dirty manifest recovery classification drifted");

  const auto evidence_path = fixture.database_path.string() + ".recovery.evidence";
  Require(std::filesystem::exists(evidence_path),
          "CDP-040 recovery evidence was not persisted");
  const auto evidence = ReadTextFile(evidence_path);
  Require(evidence.size() == 64 && evidence.starts_with("SBRECV02"),
          "CDP-040 recovery evidence is not the exact native binary record");
  platform::Uuid recovery_run;
  std::copy_n(reinterpret_cast<const std::uint8_t*>(evidence.data() + 8),
              16, recovery_run.bytes.begin());
  Require(uuid::IsEngineIdentityUuid(recovery_run),
          "CDP-040 recovery evidence omitted its native run UUID");
  const auto read_integer = [&](std::size_t offset, unsigned width) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < width; ++i)
      value |= std::uint64_t(static_cast<unsigned char>(evidence[offset + i])) << (8*i);
    return value;
  };
  std::uint64_t checksum = 1469598103934665603ULL;
  for (std::size_t i = 0; i < 56; ++i) {
    checksum ^= static_cast<unsigned char>(evidence[i]);
    checksum *= 1099511628211ULL;
  }
  // Authority is the manifest classification action, not substrings in UUID
  // octets. Arbitrary binary identities can legitimately contain ASCII WAL.
  Require(read_integer(24, 8) == 1 && read_integer(32, 8) == 1 &&
              read_integer(40, 8) != 0 && read_integer(48, 4) == 2 &&
              read_integer(52, 4) == 1 && read_integer(56, 8) == checksum,
          "CDP-040 recovery evidence lost manifest authority or completed classification integrity");

  const auto second_open = db::OpenDatabaseFile({fixture.database_path.string(), false, false, false});
  Require(second_open.ok(), "CDP-040 second recovery open failed");
  Require(ReadTextFile(evidence_path) == evidence,
          "CDP-040 recovery evidence was not idempotent");
}

void TestSblrRegistryEntry() {
  const auto* entry = sblr::LookupSblrOperation("dml.execute_native_bulk_ingest");
  Require(entry != nullptr, "CDP-040 native ingest SBLR registry entry missing");
  Require(entry->opcode == "SBLR_DML_EXECUTE_NATIVE_BULK_INGEST",
          "CDP-040 native ingest opcode drifted");
  Require(entry->category == sblr::SblrOpcodeCategory::data_mutation,
          "CDP-040 native ingest is not a data mutation operation");
  Require(entry->transaction_effect == sblr::SblrOpcodeTransactionEffect::local_write,
          "CDP-040 native ingest transaction effect drifted");
  Require(entry->requires_transaction_context,
          "CDP-040 native ingest does not require transaction context");
  Require(!entry->requires_cluster_authority && !entry->cluster_private,
          "CDP-040 native ingest incorrectly entered cluster-private scope");

  auto envelope = NativeEnvelope();
  const auto validated = sblr::ValidateSblrOpcodeForEnvelope(envelope);
  Require(validated.ok && validated.entry == entry,
          "CDP-040 native ingest SBLR opcode validation failed");

  envelope.requires_transaction_context = false;
  const auto refused = sblr::ValidateSblrOpcodeForEnvelope(envelope);
  Require(!refused.ok &&
              refused.diagnostic_id == "SB_DIAG_SBLR_TRANSACTION_CONTEXT_REQUIRED",
          "CDP-040 native ingest transaction-context refusal drifted");

  const auto round_trip_envelope = NativeRoundTripEnvelope();
  const auto round_trip_validation =
      sblr::ValidateSblrEnvelope(round_trip_envelope);
  if (!round_trip_validation.ok) {
    for (const auto& diagnostic : round_trip_validation.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
    }
  }
  Require(round_trip_validation.ok,
          "CDP-040 native ingest SBLR envelope failed canonical validation");
  const auto encoded = sblr::EncodeSblrEnvelope(round_trip_envelope);
  const auto decoded = sblr::DecodeSblrEnvelope(encoded);
  if (!decoded.ok) {
    for (const auto& diagnostic : decoded.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
    }
  }
  Require(decoded.ok, "CDP-040 native ingest encoded SBLR envelope failed decode");
  Require(decoded.envelope.operation_id == "dml.execute_native_bulk_ingest" &&
              decoded.envelope.opcode == "SBLR_DML_EXECUTE_NATIVE_BULK_INGEST",
          "CDP-040 native ingest round trip changed operation identity");

  scratchbird::parsers::sbsql::source_rendering::SblrToSbsqlOptions options;
  options.source_preserving = true;
  const auto rendered = scratchbird::parsers::sbsql::source_rendering::RenderSblrEnvelopeToSbsql(decoded.envelope, options);
  Require(!rendered.ok && !rendered.diagnostics.empty(),
          "CDP-040 native ingest SBLR-to-SBsql conversion should refuse without SQL text");
  Require(rendered.diagnostics.front().code ==
              "SB_SBLR_TO_SBSQL_SOURCE_ARTIFACT_REQUIRED",
          "CDP-040 native ingest SBLR-to-SBsql refusal diagnostic drifted");
}

}  // namespace

int main(int argc, char** argv) try {
  const bool fixed_scalar_only = argc == 2 && std::string_view(argv[1]) == "--native-fixed-scalars";
  const bool typed_null_only = argc == 2 && std::string_view(argv[1]) == "--native-typed-null";
  const bool integer_only = argc == 2 && std::string_view(argv[1]) == "--native-int64-index";
  const bool uint16_only = argc == 2 && std::string_view(argv[1]) == "--native-uint16-index";
  const bool narrow_real_only = argc == 2 && std::string_view(argv[1]) == "--native-narrow-real-index";
  const bool network_only = argc == 2 && std::string_view(argv[1]) == "--native-network-index";
  const bool ordered_only = argc == 2 && std::string_view(argv[1]) == "--native-ordered-index";
  Require(argc == 1 || fixed_scalar_only || typed_null_only || integer_only || ordered_only || uint16_only || narrow_real_only || network_only,
          "unknown native bulk gate arguments");
  ConfigureMemoryFixture();
  if (network_only) {
    TestNativeNetworkIndexReopen();
    std::cout << "native_network_index=passed canonical_order_commit_reopen_invalid_prefix_refusal\n";
    return EXIT_SUCCESS;
  }
  if (narrow_real_only) {
    TestNativeNarrowRealIndexReopen();
    std::cout << "native_narrow_real_index=passed finite_bounds_signed_zero_commit_reopen_nonfinite_refusal\n";
    return EXIT_SUCCESS;
  }
  if (uint16_only) {
    TestNativeUint16IndexReopen();
    std::cout << "native_uint16_index=passed unsigned_bounds_digit_bytes_commit_reopen\n";
    return EXIT_SUCCESS;
  }
  if (ordered_only) {
    TestNativeUint16IndexReopen();
    TestNativeNarrowRealIndexReopen();
    TestNativeNetworkIndexReopen();
    TestTypedInt64IndexKeysUseBinaryOrder();
    TestTypedInt64IndexKeysUseFullSignedSortOrder();
    TestTypedNullIndexKeyUsesNullOrder();
    TestNativeUuidBinaryIndexAndValues();
    TestNativeUuidBinaryIndexAndValues(false);
    TestNativeUuidCompositeIndex();
    std::cout << "native_bound_ordered_index=passed signed_bounds=null_uuid_unique_nonunique_compound_reopen\n";
    return EXIT_SUCCESS;
  }
  if (typed_null_only) {
    TestNullAndCharacterRowPageStorage();
    std::cout << "native_typed_null_storage=passed committed_reopen_type=int64\n";
    return EXIT_SUCCESS;
  }
  if (integer_only) {
    TestTypedInt64IndexKeysUseBinaryOrder();
    TestTypedInt64IndexKeysUseFullSignedSortOrder();
    std::cout << "native_int64_index=binary_le8 signed_bounds=passed\n";
    return EXIT_SUCCESS;
  }
  TestSblrRegistryEntry();
  TestApiAndSblrAcceptedRoutes();
  TestNullAndCharacterRowPageStorage();
  TestTypedInt64IndexKeysUseBinaryOrder();
  TestTypedInt64IndexKeysUseFullSignedSortOrder();
  TestTypedNullIndexKeyUsesNullOrder();
  TestNativeUint16IndexReopen();
  TestNativeNarrowRealIndexReopen();
  TestNativeNetworkIndexReopen();
  TestTypedScalarIndexKeysUseBinaryPayloads();
  TestTypedScalarRowPageStorage();
  TestMalformedInlineFixedTypedValueRefuses();
  TestNativeUuidBinaryIndexAndValues();
  TestNativeUuidBinaryIndexAndValues(false);
  TestNativeUuidCompositeIndex();
  if (fixed_scalar_only) {
    std::cout << "native_fixed_scalar_bulk=passed uuid_values=137\n";
    return EXIT_SUCCESS;
  }
  TestDescriptorPayloadRowPageStorage();
  TestDescriptorPayloadIndexKeysUseBinaryPayloads();
  TestOpaqueRenderOnlyDescriptorPayloadRefusals();
  TestDecimalIndexKeysPreserveNumericOrder();
  TestDecimalIndexKeysPreserveNumericOrder(true);
  TestOpaqueRenderOnlyDescriptorPayloadExplicitAllow();
  TestDisabledAndInvalidRefusals();
  TestLogicalStatementRollbackOnFaultAndCancellation();
  TestServerPublicAbiRoute();
  TestRollbackInvisibilityAndCommittedReopenVisibility();
  TestReopenRecoveryEvidence();
  std::cout << "native_bulk_ingest=passed descriptor_payload_types=" << DescriptorPayloadTypeNames().size()
            << " decimal_numeric_order_and_equality=true binary_server_result=true binary_recovery_integrity=true\n";
  return EXIT_SUCCESS;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return EXIT_FAILURE;
}
