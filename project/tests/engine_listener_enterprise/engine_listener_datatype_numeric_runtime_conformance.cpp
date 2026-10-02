#include "../support/engine_evidence_fixture.hpp"
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "datatype_catalog_manifest.hpp"
#include "datatype_descriptor.hpp"
#include "datatype_operations.hpp"
#include "domain_support/domain_store.hpp"
#include "memory.hpp"
#include "query/expression_api.hpp"
#include "sbl_numeric.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace dt = scratchbird::core::datatypes;
namespace memory = scratchbird::core::memory;
namespace numeric = scratchbird::libraries::sbl_numeric;
namespace uuid = scratchbird::core::uuid;

using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::u64;

constexpr u64 kBaseMillis = 1771200000000ull;
constexpr scratchbird::core::platform::u32 kPageSize = 16384;

struct DatabaseFixture {
  std::filesystem::path root;
  std::filesystem::path path;
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
};

struct CleanupDir {
  std::filesystem::path root;
  ~CleanupDir() {
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  }
};

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

std::string DiagnosticText(const api::EngineApiResult& result) {
  if (result.diagnostics.empty()) { return {}; }
  const auto& diagnostic = result.diagnostics.front();
  return diagnostic.code + ":" + diagnostic.message_key + ":" + diagnostic.detail;
}

void RequireApiOk(const api::EngineApiResult& result, std::string_view message) {
  if (result.ok) { return; }
  std::cerr << message << ": " << DiagnosticText(result) << '\n';
  std::exit(EXIT_FAILURE);
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && scratchbird::tests::EvidenceTextEquals(evidence.evidence_id, id)) {
      return true;
    }
  }
  return false;
}

TypedUuid MakeUuid(UuidKind kind, u64 offset) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, kBaseMillis + offset);
  return generated.ok() ? generated.value : TypedUuid{};
}

bool HasEvidence(const api::EngineApiResult& result, std::string_view kind,
                 const api::EngineUuid& identity) {
  for (const auto& item : result.evidence)
    if (item.evidence_kind == kind &&
        scratchbird::tests::EvidenceIdentityEquals(item.evidence_id, identity)) return true;
  return false;
}

api::EngineUuid NativeIdentity(TypedUuid typed_uuid) {
  return typed_uuid.value;
}

api::EngineUuid NativeIdentity(UuidKind kind, u64 offset) {
  return NativeIdentity(MakeUuid(kind, offset));
}

dt::DatatypeOperationValue Value(dt::CanonicalTypeId type_id, std::string value) {
  return {type_id, std::move(value), false};
}

scratchbird::engine::ExecutionTypeDescriptor Descriptor(
    dt::CanonicalTypeId type_id) {
  const auto manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
  Require(manifest.ok(), "listener datatype catalog authority unavailable");
  const auto row = dt::LookupDatatypeCatalogRow(manifest.manifest, type_id);
  Require(row.ok() && row.manifest.descriptor_rows.size() == 1,
          "listener datatype descriptor authority unavailable");
  dt::CatalogExecutionTypeMetadata metadata;
  metadata.descriptor_uuid = row.manifest.descriptor_rows.front().descriptor_uuid;
  metadata.descriptor_epoch =
      row.manifest.descriptor_rows.front().descriptor_epoch;
  const auto built =
      dt::LookupExecutionTypeDescriptorFromCatalog(type_id, metadata);
  Require(built.ok(), "listener execution descriptor build failed");
  return built.descriptor;
}

dt::DatatypeOperationValue DecimalValue(std::string_view lexical) {
  const auto encoded = numeric::EncodeExactDecimalLittleEndian(lexical);
  Require(encoded.ok, "listener exact decimal fixture encoding failed");
  auto value = Value(
      dt::CanonicalTypeId::decimal,
      std::string(reinterpret_cast<const char*>(encoded.canonical_bytes.data()),
                  encoded.canonical_bytes.size()));
  static const auto descriptor = Descriptor(dt::CanonicalTypeId::decimal);
  value.descriptor = descriptor;
  return value;
}

dt::DatatypeOperationValue DecimalFloatValue(std::string_view lexical) {
  const auto encoded = numeric::EncodeDecimal128LittleEndian(lexical, true);
  Require(encoded.bytes.has_value(),
          "listener exact decimal_float BID fixture encoding failed");
  auto value = Value(dt::CanonicalTypeId::decimal_float, {});
  if (encoded.bytes) {
    value.encoded_value.assign(
        reinterpret_cast<const char*>(encoded.bytes->data()),
        encoded.bytes->size());
  }
  static const auto descriptor = Descriptor(dt::CanonicalTypeId::decimal_float);
  value.descriptor = descriptor;
  return value;
}

std::string DatatypeDiagnosticDetail(
    const scratchbird::core::platform::DiagnosticRecord& diagnostic) {
  for (const auto& argument : diagnostic.arguments) {
    if (argument.key == "detail") {
      const auto* text = argument.text();
      return text == nullptr ? std::string{} : *text;
    }
  }
  return {};
}

std::string SortKey(dt::CanonicalTypeId type_id, std::string value) {
  dt::DatatypeSortKeyRequest request;
  request.value = Value(type_id, std::move(value));
  const auto result = dt::MakeDatatypeSortKey(request);
  Require(result.ok(), "numeric sort key generation failed");
  return result.sort_key;
}

std::string StableHash(dt::CanonicalTypeId type_id, std::string value) {
  dt::DatatypeHashRequest request;
  request.value = Value(type_id, std::move(value));
  const auto result = dt::HashDatatypeValue(request);
  Require(result.ok(), "numeric hash generation failed");
  return result.stable_hash_hex;
}

void RequireCompare(dt::CanonicalTypeId type_id,
                    std::string left,
                    std::string right,
                    int expected) {
  dt::DatatypeComparisonRequest request;
  request.left = Value(type_id, std::move(left));
  request.right = Value(type_id, std::move(right));
  const auto result = dt::CompareDatatypeValues(request);
  Require(result.ok(), "numeric comparison failed");
  Require(result.comparison == expected, "numeric comparison result mismatch");
}

void RuntimeNumericProof() {
  RequireCompare(dt::CanonicalTypeId::int128,
                 "170141183460469231731687303715884105727",
                 "170141183460469231731687303715884105726",
                 1);
  RequireCompare(dt::CanonicalTypeId::uint128,
                 "340282366920938463463374607431768211455",
                 "340282366920938463463374607431768211454",
                 1);
  const auto decimal_ten = DecimalValue("10");
  const auto decimal_two = DecimalValue("2");
  const auto decimal_compare =
      dt::CompareDatatypeValues({decimal_ten, decimal_two});
  Require(!decimal_compare.ok() &&
              decimal_compare.diagnostic.diagnostic_code ==
                  "SB_DATATYPE_COMPARISON_REJECTED" &&
              DatatypeDiagnosticDetail(decimal_compare.diagnostic) ==
                  "decimal_comparison_policy_unresolved",
          "decimal comparison did not fail at unresolved policy");
  const auto decimal_float_450 = DecimalFloatValue("4.50");
  const auto decimal_float_45 = DecimalFloatValue("4.5");
  const auto decimal_float_compare =
      dt::CompareDatatypeValues({decimal_float_450, decimal_float_45});
  Require(!decimal_float_compare.ok() &&
              decimal_float_compare.diagnostic.diagnostic_code ==
                  "SB_DATATYPE_COMPARISON_REJECTED" &&
              DatatypeDiagnosticDetail(decimal_float_compare.diagnostic) ==
                  "decimal_float_comparison_policy_unresolved" &&
              decimal_float_compare.comparison == 0,
          "decimal_float comparison did not fail at unresolved policy");
  RequireCompare(dt::CanonicalTypeId::real128, "1.500", "1.5", 0);

  Require(SortKey(dt::CanonicalTypeId::int128, "-10") <
              SortKey(dt::CanonicalTypeId::int128, "-2"),
          "int128 sort key did not order negative magnitudes");
  Require(SortKey(dt::CanonicalTypeId::int128, "2") <
              SortKey(dt::CanonicalTypeId::int128, "10"),
          "int128 sort key used lexical ordering");
  const auto decimal_two_key = dt::MakeDatatypeSortKey({decimal_two});
  const auto decimal_ten_key = dt::MakeDatatypeSortKey({decimal_ten});
  Require(!decimal_two_key.ok() && !decimal_ten_key.ok() &&
              decimal_two_key.diagnostic.diagnostic_code ==
                  "SB_DATATYPE_SORT_KEY_REJECTED" &&
              decimal_ten_key.diagnostic.diagnostic_code ==
                  "SB_DATATYPE_SORT_KEY_REJECTED" &&
              DatatypeDiagnosticDetail(decimal_two_key.diagnostic) ==
                  "decimal_sort_key_policy_unresolved" &&
              DatatypeDiagnosticDetail(decimal_ten_key.diagnostic) ==
                  "decimal_sort_key_policy_unresolved",
          "decimal sort key did not fail at unresolved policy");
  const auto decimal_float_450_key =
      dt::MakeDatatypeSortKey({decimal_float_450});
  const auto decimal_float_45_key =
      dt::MakeDatatypeSortKey({decimal_float_45});
  Require(!decimal_float_450_key.ok() && !decimal_float_45_key.ok() &&
              decimal_float_450_key.diagnostic.diagnostic_code ==
                  "SB_DATATYPE_SORT_KEY_REJECTED" &&
              decimal_float_45_key.diagnostic.diagnostic_code ==
                  "SB_DATATYPE_SORT_KEY_REJECTED" &&
              DatatypeDiagnosticDetail(decimal_float_450_key.diagnostic) ==
                  "decimal_float_sort_key_policy_unresolved" &&
              DatatypeDiagnosticDetail(decimal_float_45_key.diagnostic) ==
                  "decimal_float_sort_key_policy_unresolved" &&
              decimal_float_450_key.sort_key.empty() &&
              decimal_float_45_key.sort_key.empty(),
          "decimal_float sort key did not fail at unresolved policy");
  const auto decimal_float_450_hash =
      dt::HashDatatypeValue({decimal_float_450});
  const auto decimal_float_45_hash =
      dt::HashDatatypeValue({decimal_float_45});
  Require(!decimal_float_450_hash.ok() && !decimal_float_45_hash.ok() &&
              decimal_float_450_hash.diagnostic.diagnostic_code ==
                  "SB_DATATYPE_HASH_REJECTED" &&
              decimal_float_45_hash.diagnostic.diagnostic_code ==
                  "SB_DATATYPE_HASH_REJECTED" &&
              DatatypeDiagnosticDetail(decimal_float_450_hash.diagnostic) ==
                  "decimal_float_hash_policy_unresolved" &&
              DatatypeDiagnosticDetail(decimal_float_45_hash.diagnostic) ==
                  "decimal_float_hash_policy_unresolved" &&
              decimal_float_450_hash.stable_hash_hex.empty() &&
              decimal_float_45_hash.stable_hash_hex.empty(),
          "decimal_float hash did not fail at unresolved policy");
  Require(StableHash(dt::CanonicalTypeId::int128, "+00042") ==
              StableHash(dt::CanonicalTypeId::int128, "42"),
          "int128 hash did not use canonical numeric value");

  dt::DatatypeHashRequest invalid_hash;
  invalid_hash.value = DecimalValue("4.50");
  const auto decimal_hash = dt::HashDatatypeValue(invalid_hash);
  Require(!decimal_hash.ok() &&
              decimal_hash.diagnostic.diagnostic_code ==
                  "SB_DATATYPE_HASH_REJECTED" &&
              DatatypeDiagnosticDetail(decimal_hash.diagnostic) ==
                  "decimal_hash_policy_unresolved",
          "decimal hash did not fail at unresolved policy");
}

memory::AllocationPolicy MemoryPolicy() {
  memory::AllocationPolicy policy;
  policy.policy_name = "engine_listener_datatype_numeric_runtime_conformance";
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
      MemoryPolicy(), "engine_listener_datatype_numeric_runtime_conformance");
  Require(configured.ok(), "memory fixture configuration failed");
  Require(configured.fixture_mode, "memory fixture mode was not active");
}

DatabaseFixture CreateDatabaseFixture(const std::filesystem::path& root) {
  DatabaseFixture fixture;
  fixture.root = root;
  fixture.path = root / "eler030_numeric_domain.sbdb";
  fixture.database_uuid = MakeUuid(UuidKind::database, 30);
  fixture.filespace_uuid = MakeUuid(UuidKind::filespace, 31);
  std::filesystem::create_directories(root);

  db::DatabaseCreateConfig create;
  create.path = fixture.path.string();
  create.database_uuid = fixture.database_uuid;
  create.filespace_uuid = fixture.filespace_uuid;
  create.page_size = kPageSize;
  create.creation_unix_epoch_millis = kBaseMillis;
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "database fixture creation failed");
  return fixture;
}

api::EngineRequestContext Context(const DatabaseFixture& fixture,
                                  std::string request_id) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = std::move(request_id);
  context.database_path = fixture.path.string();
  context.database_uuid = NativeIdentity(fixture.database_uuid);
  context.node_uuid = NativeIdentity(UuidKind::object, 32);
  context.principal_uuid = NativeIdentity(UuidKind::principal, 33);
  context.session_uuid = NativeIdentity(UuidKind::object, 34);
  context.security_context_present = true;
  context.identifier_profile_uuid = "sbsql_v3";
  context.language_context.language_tag = "en";
  context.language_context.default_language_tag = "en";
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  return context;
}

void Begin(api::EngineRequestContext* context) {
  api::EngineBeginTransactionRequest request;
  request.context = *context;
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
  RequireApiOk(begun, "transaction begin failed");
  context->local_transaction_id = begun.local_transaction_id;
  context->transaction_uuid = begun.transaction_uuid;
  context->snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context->transaction_isolation_level = begun.isolation_level;
}

void Commit(api::EngineRequestContext* context) {
  api::EngineCommitTransactionRequest request;
  request.context = *context;
  const auto committed = api::EngineCommitTransaction(request);
  RequireApiOk(committed, "transaction commit failed");
  context->local_transaction_id = 0;
  context->transaction_uuid = {};
}

api::DomainRecord Domain(std::uint64_t creator_tx,
                         api::EngineUuid domain_uuid,
                         std::string base_type,
                         std::string check_envelope) {
  api::DomainRecord record;
  record.creator_tx = creator_tx;
  record.domain_uuid = std::move(domain_uuid);
  record.catalog_row_uuid = NativeIdentity(UuidKind::object, 40 + creator_tx);
  record.schema_uuid = NativeIdentity(UuidKind::schema, 50 + creator_tx);
  record.default_name = "eler030_numeric_domain";
  record.base_descriptor_uuid = NativeIdentity(UuidKind::object, 60 + creator_tx);
  record.base_descriptor_kind = "scalar";
  record.base_canonical_type_name = std::move(base_type);
  record.base_encoded_descriptor = "canonical=" + record.base_canonical_type_name;
  record.nullable = false;
  record.check_constraint_envelope = std::move(check_envelope);
  record.validation_hook_status = "sblr_builtin";
  return record;
}

api::EngineTypedValue TypedValue(std::string canonical_type_name,
                                 std::string encoded_value) {
  api::EngineTypedValue value;
  value.descriptor.descriptor_kind = "scalar";
  value.descriptor.canonical_type_name = std::move(canonical_type_name);
  value.descriptor.encoded_descriptor =
      "canonical=" + value.descriptor.canonical_type_name;
  value.encoded_value = std::move(encoded_value);
  value.is_null = false;
  return value;
}

void DomainPredicateProof() {
  ConfigureMemoryFixture();
  const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
  CleanupDir cleanup{std::filesystem::temp_directory_path() /
                     ("sb_eler030_" + std::to_string(unique))};
  const auto fixture = CreateDatabaseFixture(cleanup.root);

  auto writer = Context(fixture, "eler030-domain-writer");
  Begin(&writer);
  const auto domain_uuid = NativeIdentity(UuidKind::object, 70);
  const auto domain = Domain(writer.local_transaction_id,
                             domain_uuid,
                             "int128",
                             "sblr_predicate:eq:170141183460469231731687303715884105727");
  const auto appended =
      api::AppendDomainEvent(writer, api::MakeDomainCreateEvent(domain));
  Require(!appended.error, "domain create event append failed");
  Commit(&writer);

  auto reader = Context(fixture, "eler030-domain-reader");
  Begin(&reader);
  api::EngineValidateDomainValueRequest validate;
  validate.context = reader;
  validate.domain_descriptor = api::DomainDescriptor(domain);
  validate.input_value =
      TypedValue("int128", "+170141183460469231731687303715884105727");
  const auto accepted = api::EngineValidateDomainValue(validate);
  RequireApiOk(accepted, "int128 exact numeric domain predicate failed");
  Require(HasEvidence(accepted, "domain_validation", domain_uuid),
          "domain validation evidence missing");

  validate.input_value =
      TypedValue("int128", "170141183460469231731687303715884105726");
  const auto refused = api::EngineValidateDomainValue(validate);
  Require(!refused.ok, "int128 exact numeric domain predicate accepted bad value");
  Require(DiagnosticText(refused).find("domain_check_eq_failed") != std::string::npos,
          "domain numeric predicate diagnostic drifted");
}

}  // namespace

int main() {
  RuntimeNumericProof();
  DomainPredicateProof();
  std::cout << "engine_listener_datatype_numeric_runtime_conformance=passed\n";
  return EXIT_SUCCESS;
}
