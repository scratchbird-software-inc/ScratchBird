// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "hash_digest.hpp"
#include "canonical_query_execute.hpp"
#include "crud_support/crud_store.hpp"
#include "database_lifecycle.hpp"
#include "datatype_catalog_manifest.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "model_family_coordinator.hpp"
#include "nosql/nosql_provider_generation_store.hpp"
#include "nosql/search_api.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace dt = scratchbird::core::datatypes;
namespace exec = scratchbird::engine::executor;
namespace hash = scratchbird::core::hash;
namespace opt = scratchbird::engine::optimizer;
namespace platform = scratchbird::core::platform;
namespace sblr = scratchbird::engine::sblr;
namespace uuid = scratchbird::core::uuid;

constexpr std::string_view kCollection =
    "70000000-0000-4000-8000-000000000078";
constexpr std::string_view kJoinObject =
    "70000000-0000-4000-8000-000000000079";
constexpr std::string_view kAnalyzer =
    "70000000-0000-4000-8000-000000000110";
constexpr std::string_view kRanking =
    "70000000-0000-4000-8000-000000000111";
constexpr std::string_view kAnalyzerDigest =
    "9033908d159ddd442f2042467fd49e0a12b47679f7514e9aa6e55488e151d316";
constexpr std::string_view kTermsDigest =
    "95d0eb4c5f76b066ed03018c3311df84c5ae871245a160f8862dbb7b0f6c7fc5";
constexpr std::string_view kPhraseDigest =
    "2859a2da86040e7375f739670f8ad922f7d42b3f4dc581d5841373c63c591f0b";
constexpr std::string_view kFilterDigest =
    "1662d09fbb86ba60df6581b6bfe283ea17cd2e1760d580127723772ae768ca27";
constexpr std::string_view kFuzzyDigest =
    "6a77a05cf4f2245e5ecb97fcb2a00044f2c4407c43d8454a6b249a37c7d3727c";
constexpr std::string_view kEmptyDigest =
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

bool Require(const bool condition, const std::string_view detail) {
  if (!condition) std::cerr << "RCP-078: " << detail << '\n';
  return condition;
}

std::string TestUuid(const std::uint64_t suffix) {
  std::ostringstream out;
  out << "70000000-0000-4000-8000-" << std::hex << std::setw(12)
      << std::setfill('0') << suffix;
  return out.str();
}

std::uint64_t NowMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::string GeneratedUuid(const platform::UuidKind kind,
                          const std::uint64_t salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, NowMillis() + salt);
  return generated.ok() ? uuid::UuidToString(generated.value.value)
                        : std::string{};
}

bool UuidV7Text(const std::string_view value) {
  return value.size() == 36 && value[8] == '-' && value[13] == '-' &&
         value[14] == '7' && value[18] == '-' && value[23] == '-';
}

std::string CoreTypeUuid(const std::string_view stable_name) {
  const auto manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
  if (!manifest.ok()) return {};
  const auto found = std::ranges::find_if(
      manifest.manifest.descriptor_rows,
      [&](const auto& row) { return row.stable_name == stable_name; });
  return found == manifest.manifest.descriptor_rows.end()
             ? std::string{}
             : uuid::UuidToString(found->descriptor_uuid.value);
}

std::string CoreRuntimeTypeUuid(const std::string_view stable_name) {
  const auto manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
  if (!manifest.ok()) return {};
  const auto count = std::ranges::count_if(
      manifest.manifest.descriptor_rows,
      [&](const auto& row) { return row.stable_name == stable_name; });
  const auto descriptor = std::ranges::find_if(
      manifest.manifest.descriptor_rows,
      [&](const auto& row) { return row.stable_name == stable_name; });
  if (count != 1 || descriptor == manifest.manifest.descriptor_rows.end() ||
      !descriptor->descriptor_uuid.valid()) {
    return {};
  }
  const auto descriptor_uuid =
      uuid::UuidToString(descriptor->descriptor_uuid.value);
  const auto identity = dt::LookupDatatypeTypeCodecIdentityV1(
      "019d0000-0000-7000-8000-00000000d701",
      manifest.manifest.catalog_epoch, 1, descriptor_uuid,
      descriptor->descriptor_epoch);
  return identity.ok ? identity.row.type_uuid : descriptor_uuid;
}

enum class FixtureKind { kBase, kEmpty, kInvalid, kDuplicate };

struct SearchFixture {
  std::filesystem::path directory;
  std::filesystem::path database_path;
  std::string database_uuid;
  std::string filespace_uuid;
  std::string schema_uuid;
  std::string principal_uuid;
  std::string session_uuid;
  api::MgaRelationStorageDescriptor storage;
  api::MgaRelationStorageDescriptor join_storage;
  api::EngineRequestContext reader;
  api::EngineRequestContext active_writer;
  bool reader_active = false;
  bool writer_active = false;

  ~SearchFixture() {
    if (reader_active) {
      api::EngineRollbackTransactionRequest request;
      request.context = reader;
      (void)api::EngineRollbackTransaction(request);
    }
    if (writer_active) {
      api::EngineRollbackTransactionRequest request;
      request.context = active_writer;
      (void)api::EngineRollbackTransaction(request);
    }
    std::error_code ignored;
    if (!directory.empty()) std::filesystem::remove_all(directory, ignored);
  }
};

api::EngineRequestContext BaseContext(const SearchFixture& fixture,
                                      std::string request_id) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = std::move(request_id);
  context.database_path = fixture.database_path.string();
  context.database_uuid.canonical = fixture.database_uuid;
  context.default_root_uuid.canonical = fixture.filespace_uuid;
  context.current_schema_uuid.canonical = fixture.schema_uuid;
  context.principal_uuid.canonical = fixture.principal_uuid;
  context.session_uuid.canonical = fixture.session_uuid;
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.identifier_profile_uuid = "sbsql_v3";
  context.language_context.language_tag = "en";
  context.language_context.default_language_tag = "en";
  return context;
}

bool Begin(const SearchFixture& fixture, std::string request_id,
           api::EngineRequestContext* context) {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(fixture, std::move(request_id));
  request.isolation_level = "repeatable_read";
  const auto begun = api::EngineBeginTransaction(request);
  if (!begun.ok || context == nullptr) return false;
  *context = request.context;
  context->local_transaction_id = begun.local_transaction_id;
  context->transaction_uuid = begun.transaction_uuid;
  context->snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context->transaction_isolation_level = begun.isolation_level;
  return true;
}

bool Commit(const api::EngineRequestContext& context) {
  api::EngineCommitTransactionRequest request;
  request.context = context;
  return api::EngineCommitTransaction(request).ok;
}

bool Rollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  return api::EngineRollbackTransaction(request).ok;
}

bool PublishSnapshot(api::EngineRequestContext* context) {
  if (context == nullptr) return false;
  context->statement_uuid.canonical =
      GeneratedUuid(platform::UuidKind::object, 0x700);
  api::EnginePublishStatementSnapshotRequest request;
  request.context = *context;
  const auto published = api::EnginePublishStatementSnapshot(request);
  if (!published.ok) {
    std::cerr << "RCP-078 snapshot failed";
    if (!published.diagnostics.empty()) {
      std::cerr << ":" << published.diagnostics.front().code << ':'
                << published.diagnostics.front().detail;
    }
    std::cerr << '\n';
    return false;
  }
  if (published.statement_uuid.canonical !=
          context->statement_uuid.canonical ||
      published.transaction_uuid.canonical !=
          context->transaction_uuid.canonical ||
      !UuidV7Text(published.statement_uuid.canonical) ||
      !UuidV7Text(published.statement_snapshot_uuid.canonical) ||
      !UuidV7Text(published.transaction_uuid.canonical)) {
    return false;
  }
  context->statement_snapshot_uuid = published.statement_snapshot_uuid;
  context->snapshot_visible_through_local_transaction_id =
      published.snapshot_vector.visible_committed_high_watermark;
  return true;
}

void AddAuthorization(api::EngineRequestContext* context) {
  auto& authorization = context->authorization_context;
  authorization.present = true;
  authorization.authority_uuid.canonical = TestUuid(0x704);
  authorization.principal_uuid = context->principal_uuid;
  authorization.security_epoch = context->security_epoch;
  authorization.policy_epoch = 1;
  authorization.catalog_generation_id = context->catalog_generation_id;
  authorization.effective_subjects.push_back(
      {context->principal_uuid, "principal"});
  api::EngineMaterializedAuthorizationGrant grant;
  grant.grant_uuid.canonical = TestUuid(0x706);
  grant.subject_uuid = context->principal_uuid;
  grant.subject_kind = "principal";
  grant.target_uuid.canonical = std::string(kCollection);
  grant.right = "SELECT";
  grant.security_epoch = context->security_epoch;
  authorization.grants.push_back(std::move(grant));
  api::EngineMaterializedAuthorizationGrant join_grant;
  join_grant.grant_uuid.canonical = TestUuid(0x707);
  join_grant.subject_uuid = context->principal_uuid;
  join_grant.subject_kind = "principal";
  join_grant.target_uuid.canonical = std::string(kJoinObject);
  join_grant.right = "SELECT";
  join_grant.security_epoch = context->security_epoch;
  authorization.grants.push_back(std::move(join_grant));
}

bool AppendRow(const api::EngineRequestContext& context,
               const std::string& row_uuid, const std::string& body,
               const std::string& category, const std::uint64_t version_suffix,
               std::uint64_t* generation = nullptr) {
  api::CrudRowVersionRecord row;
  row.creator_tx = context.local_transaction_id;
  row.table_uuid = std::string(kCollection);
  row.row_uuid = row_uuid;
  row.version_uuid = TestUuid(version_suffix);
  row.values = {{"body", body}, {"category", category}};
  return !api::AppendMgaRowVersion(context, row, generation).error;
}

bool AppendJoinRow(const api::EngineRequestContext& context,
                   const std::string& row_uuid,
                   const std::string& document_uuid,
                   const std::string& payload,
                   const std::uint64_t version_suffix) {
  api::CrudRowVersionRecord row;
  row.creator_tx = context.local_transaction_id;
  row.table_uuid = std::string(kJoinObject);
  row.row_uuid = row_uuid;
  row.version_uuid = TestUuid(version_suffix);
  row.values = {{"join_document_uuid", document_uuid}, {"payload", payload}};
  return !api::AppendMgaRowVersion(context, row, nullptr).error;
}

bool BuildFixture(const FixtureKind kind, SearchFixture* fixture) {
  if (fixture == nullptr) return false;
  const auto salt = NowMillis() % 1'000'000 +
                    static_cast<std::uint64_t>(kind) * 10'000;
  fixture->directory = std::filesystem::temp_directory_path() /
                       ("scratchbird_rcp078_search_" + std::to_string(salt));
  std::error_code filesystem_error;
  std::filesystem::create_directories(fixture->directory, filesystem_error);
  if (filesystem_error) return Require(false, "fixture directory creation failed");
  fixture->database_path = fixture->directory / "search.sbdb";
  const auto database_uuid = uuid::GenerateEngineIdentityV7(
      platform::UuidKind::database, NowMillis() + salt + 1);
  const auto filespace_uuid = uuid::GenerateEngineIdentityV7(
      platform::UuidKind::filespace, NowMillis() + salt + 2);
  if (!database_uuid.ok() || !filespace_uuid.ok())
    return Require(false, "fixture database identities failed");
  db::DatabaseCreateConfig create;
  create.path = fixture->database_path.string();
  create.database_uuid = database_uuid.value;
  create.filespace_uuid = filespace_uuid.value;
  create.creation_unix_epoch_millis = NowMillis();
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  if (!db::CreateDatabaseFile(create).ok())
    return Require(false, "fixture database creation failed");
  fixture->database_uuid = uuid::UuidToString(database_uuid.value.value);
  fixture->filespace_uuid = uuid::UuidToString(filespace_uuid.value.value);
  fixture->schema_uuid = GeneratedUuid(platform::UuidKind::object, salt + 10);
  fixture->principal_uuid =
      GeneratedUuid(platform::UuidKind::principal, salt + 11);
  fixture->session_uuid = GeneratedUuid(platform::UuidKind::object, salt + 12);
  const auto text_type = CoreTypeUuid("character");
  if (text_type.empty()) return Require(false, "fixture TEXT type missing");

  api::EngineRequestContext metadata;
  if (!Begin(*fixture, "rcp078-search-metadata", &metadata))
    return Require(false, "fixture metadata begin failed");
  api::CrudTableRecord table;
  table.creator_tx = metadata.local_transaction_id;
  table.table_uuid = std::string(kCollection);
  table.default_name = "rcp078_search_collection";
  table.columns = {
      {"body", "canonical=text;type_uuid=" + text_type + ";nullable=false"},
      {"category",
       "canonical=text;type_uuid=" + text_type + ";nullable=false"},
  };
  const auto table_append = api::AppendMgaTableMetadata(metadata, table);
  const auto descriptor_ensure = api::EnsureMgaRelationStorageDescriptor(
      metadata, table, {}, &fixture->storage);
  api::CrudTableRecord join_table;
  join_table.creator_tx = metadata.local_transaction_id;
  join_table.table_uuid = std::string(kJoinObject);
  join_table.default_name = "rcp078_search_join";
  join_table.columns = {
      {"join_document_uuid",
       "canonical=uuid;type_uuid=" + CoreTypeUuid("uuid") +
           ";nullable=false"},
      {"payload", "canonical=text;type_uuid=" + text_type +
                      ";nullable=false"},
  };
  const auto join_table_append =
      api::AppendMgaTableMetadata(metadata, join_table);
  const auto join_descriptor_ensure =
      api::EnsureMgaRelationStorageDescriptor(
          metadata, join_table, {}, &fixture->join_storage);
  if (table_append.error || descriptor_ensure.error ||
      join_table_append.error || join_descriptor_ensure.error ||
      !Commit(metadata))
    return Require(false, "fixture descriptor persistence failed");

  api::EngineRequestContext writer;
  if (!Begin(*fixture, "rcp078-search-writer", &writer))
    return Require(false, "fixture writer begin failed");
  bool seeded = true;
  if (kind == FixtureKind::kBase) {
    seeded =
        AppendRow(writer, TestUuid(1), "alpha beta beta gamma", "a", 0x801) &&
        AppendRow(writer, TestUuid(2), "alpha beta delta", "a", 0x802) &&
        AppendRow(writer, TestUuid(3), "alpha gamma delta delta", "b", 0x803) &&
        AppendRow(writer, TestUuid(4), "alpa beta epsilon", "b", 0x804) &&
        AppendJoinRow(writer, TestUuid(0x91), TestUuid(1), "matched", 0x891) &&
        AppendJoinRow(writer, TestUuid(0x92), TestUuid(0x99), "unmatched",
                      0x892);
  } else if (kind == FixtureKind::kEmpty) {
    seeded = AppendRow(writer, TestUuid(7), "", "z", 0x807);
  } else if (kind == FixtureKind::kInvalid) {
    seeded = AppendRow(writer, TestUuid(9), "caf\xc3\xa9", "a", 0x809);
  } else {
    seeded = AppendRow(writer, TestUuid(8), "alpha", "d1", 0x8081) &&
             AppendRow(writer, TestUuid(8), "beta", "d2", 0x8082);
  }
  if (!seeded || !Commit(writer))
    return Require(false, "fixture visible rows failed");

  if (kind == FixtureKind::kBase) {
    if (!Begin(*fixture, "rcp078-search-active-other",
               &fixture->active_writer) ||
        !AppendRow(fixture->active_writer, TestUuid(5),
                   "alpha beta beta beta", "hidden", 0x805)) {
      return Require(false, "fixture active-other row failed");
    }
    fixture->writer_active = true;
    api::EngineRequestContext rolled;
    if (!Begin(*fixture, "rcp078-search-rolled-back", &rolled) ||
        !AppendRow(rolled, TestUuid(6), "alpha beta", "hidden", 0x806) ||
        !Rollback(rolled)) {
      return Require(false, "fixture rolled-back row failed");
    }
  }

  if (!Begin(*fixture, "rcp078-search-reader", &fixture->reader) ||
      !PublishSnapshot(&fixture->reader)) {
    return Require(false, "fixture reader snapshot failed");
  }
  fixture->reader_active = true;
  fixture->reader.statement_timestamp = "2026-08-11T01:02:03Z";
  fixture->reader.statement_metadata_snapshot_engine_owned = true;
  fixture->reader.statement_metadata_snapshot_uuid.canonical =
      GeneratedUuid(platform::UuidKind::object, salt + 0x702);
  fixture->reader
      .statement_metadata_snapshot_visible_through_local_transaction_id =
      fixture->reader.snapshot_visible_through_local_transaction_id;
  fixture->reader.catalog_epoch_uuid.canonical = TestUuid(0x705);
  fixture->reader.optimizer_capability_snapshot_uuid.canonical = TestUuid(0x710);
  fixture->reader.optimizer_resource_snapshot_uuid.canonical = TestUuid(0x711);
  fixture->reader.optimizer_route_snapshot_uuid.canonical = TestUuid(0x712);
  fixture->reader.optimizer_route_epoch = 1;
  fixture->reader.optimizer_route_generation = 1;
  fixture->reader.optimizer_memory_budget_bytes = 16 * 1024 * 1024;
  fixture->reader.optimizer_maximum_candidate_count = 4096;
  fixture->reader.optimizer_maximum_memo_groups = 4096;
  fixture->reader.optimizer_maximum_search_steps = 16384;
  fixture->reader.optimizer_maximum_planning_time_ns = 1'000'000'000;
  fixture->reader.current_monotonic_ns = std::to_string(NowMillis());
  fixture->reader.query_cancellation_requested = [] { return false; };
  AddAuthorization(&fixture->reader);
  return true;
}

bool EngineUuidExecutionCohortAuthority() {
  SearchFixture fixture;
  if (!Require(BuildFixture(FixtureKind::kBase, &fixture),
               "UUIDv7 cohort fixture construction failed")) {
    return false;
  }
  api::EngineResolveStatementSnapshotRequest resolve;
  resolve.context = fixture.reader;
  const auto resolved = api::EngineResolveStatementSnapshot(resolve);
  const auto owning_transaction_uuid =
      resolved.ok
          ? uuid::UuidToString(
                resolved.snapshot_vector.owning_transaction_uuid.value)
          : std::string{};
  const bool live_exact =
      resolved.ok &&
      resolved.statement_uuid.canonical ==
          fixture.reader.statement_uuid.canonical &&
      resolved.statement_snapshot_uuid.canonical ==
          fixture.reader.statement_snapshot_uuid.canonical &&
      owning_transaction_uuid == fixture.reader.transaction_uuid.canonical &&
      resolved.snapshot_vector.owning_transaction.value ==
          fixture.reader.local_transaction_id &&
      resolved.snapshot_vector.visible_committed_high_watermark ==
          fixture.reader.snapshot_visible_through_local_transaction_id &&
      resolved.snapshot_vector.inventory_authoritative &&
      resolved.snapshot_vector.complete &&
      UuidV7Text(fixture.reader.statement_uuid.canonical) &&
      UuidV7Text(fixture.reader.statement_snapshot_uuid.canonical) &&
      UuidV7Text(fixture.reader.statement_metadata_snapshot_uuid.canonical) &&
      UuidV7Text(fixture.reader.transaction_uuid.canonical);

  api::EngineRequestContext refused_context;
  if (!Begin(fixture, "rcp078-search-v4-statement-refusal",
             &refused_context)) {
    return Require(false, "UUIDv4 refusal transaction begin failed");
  }
  refused_context.statement_uuid.canonical = TestUuid(0x700);
  api::EnginePublishStatementSnapshotRequest refused_request;
  refused_request.context = refused_context;
  const auto refused = api::EnginePublishStatementSnapshot(refused_request);
  const bool exact_refusal =
      !refused.ok && !refused.diagnostics.empty() &&
      std::ranges::any_of(refused.diagnostics, [](const auto& diagnostic) {
        return diagnostic.detail.find("statement_uuid_malformed") !=
               std::string::npos;
      });
  const bool rolled_back = Rollback(refused_context);
  return Require(live_exact && exact_refusal && rolled_back,
                 "approved engine-issued UUIDv7 execution cohort drifted");
}

std::vector<api::EngineDescriptor> OutputDescriptors() {
  const auto uuid_type = CoreTypeUuid("uuid");
  const auto uint64_type = CoreTypeUuid("uint64");
  const auto real64_type = CoreTypeUuid("real64");
  const std::array<std::string, 5> types{uuid_type, uuid_type, uint64_type,
                                         real64_type, uint64_type};
  const std::array<std::string_view, 5> names{"uuid", "uuid", "uint64",
                                               "real64", "uint64"};
  std::vector<api::EngineDescriptor> descriptors;
  for (std::size_t index = 0; index < types.size(); ++index) {
    api::EngineDescriptor descriptor;
    descriptor.descriptor_uuid.canonical = TestUuid(0x200 + index);
    descriptor.descriptor_kind = "scalar";
    descriptor.canonical_type_name = std::string(names[index]);
    descriptor.encoded_descriptor =
        "type_uuid=" + types[index] + ";nullability=non_null";
    descriptors.push_back(std::move(descriptor));
  }
  return descriptors;
}

api::EngineBoundSearchReadRequestV1 SearchRequest(
    const SearchFixture& fixture, const api::EngineBoundSearchOperationV1 operation,
    std::string query, const std::uint32_t top_k) {
  api::EngineBoundSearchReadRequestV1 request;
  request.context = fixture.reader;
  request.collection_uuid = std::string(kCollection);
  request.expected_descriptor_uuid = fixture.storage.descriptor_uuid.canonical;
  request.expected_descriptor_generation =
      fixture.storage.descriptor_generation;
  request.selected_alternative_uuid = TestUuid(0x720);
  request.selected_provider_uuid = TestUuid(0x721);
  request.selected_capability_uuid = TestUuid(0x722);
  request.selected_implementation_id = "physical_search_rank_scan_v1";
  request.operation = operation;
  request.physical_route =
      api::EngineBoundSearchPhysicalRouteV1::kExactCorpusScan;
  request.bound_query_text = std::move(query);
  request.fuzzy_maximum_edits =
      operation == api::EngineBoundSearchOperationV1::kFuzzy ? 1 : 0;
  request.top_k = top_k;
  request.analyzer_uuid = std::string(kAnalyzer);
  request.analyzer_generation = 7;
  request.analyzer_pipeline_sha256 = std::string(kAnalyzerDigest);
  request.output_descriptors = OutputDescriptors();
  request.maximum_scanned_row_versions = 64;
  request.maximum_decoded_bytes = 1U << 20U;
  request.maximum_tokens = 1024;
  request.maximum_positions = 1024;
  request.maximum_candidates = 64;
  request.maximum_scored_rows = 64;
  request.maximum_output_rows = 64;
  request.maximum_memory_bytes = 1U << 20U;
  request.cancellation_requested = [] { return false; };
  return request;
}

std::string ResultBytes(const api::EngineBoundSearchReadResultV1& result) {
  std::string bytes;
  for (const auto& row : result.rows) {
    bytes += row.document_uuid + '\t' + row.analyzer_uuid + '\t' +
             std::to_string(row.analyzer_generation) + '\t' +
             row.encoded_score + '\t' + std::to_string(row.rank) + '\n';
  }
  return bytes;
}

std::string Digest(const std::string& bytes) {
  const auto digest = hash::ComputeSha256Digest(
      reinterpret_cast<const platform::byte*>(bytes.data()), bytes.size());
  return digest.ok() ? hash::HexLower(digest.digest) : std::string{};
}

bool ExactSuccess(const api::EngineBoundSearchReadResultV1& result,
                  const std::string_view digest,
                  const std::size_t rows) {
  return result.ok && result.rows.size() == rows &&
         result.output_descriptors.size() == 5 &&
         result.full_corpus_exact_recheck_complete &&
         result.base_row_mga_recheck_complete &&
         result.security_recheck_complete && result.execution_resource_acquired &&
         result.cleanup_count == 1 && Digest(ResultBytes(result)) == digest;
}

bool ExactRefusal(const api::EngineBoundSearchReadResultV1& result,
                  const std::string_view diagnostic, const bool acquired) {
  return !result.ok && result.rows.empty() &&
         result.diagnostic.code == diagnostic &&
         result.execution_resource_acquired == acquired &&
         result.cleanup_count == (acquired ? 1U : 0U);
}

bool DirectOutcomeMatrix() {
  SearchFixture base;
  if (!Require(BuildFixture(FixtureKind::kBase, &base),
               "BASE fixture construction failed")) {
    return false;
  }
  bool passed = true;
  const auto terms = api::EngineBoundSearchReadV1(SearchRequest(
      base, api::EngineBoundSearchOperationV1::kTerms, "alpha beta", 3));
  passed &= Require(ExactSuccess(terms, kTermsDigest, 3),
                    "SX-01 terms stream drifted");
  const auto phrase = api::EngineBoundSearchReadV1(SearchRequest(
      base, api::EngineBoundSearchOperationV1::kPhrase, "alpha beta", 2));
  passed &= Require(ExactSuccess(phrase, kPhraseDigest, 2),
                    "SX-02 phrase stream drifted");
  const auto fuzzy = api::EngineBoundSearchReadV1(SearchRequest(
      base, api::EngineBoundSearchOperationV1::kFuzzy, "alpha", 2));
  passed &= Require(ExactSuccess(fuzzy, kFuzzyDigest, 2),
                    "SX-03 fuzzy stream drifted");
  auto filtered_request = SearchRequest(
      base, api::EngineBoundSearchOperationV1::kTerms, "alpha beta", 2);
  filtered_request.filter = {true, "a"};
  const auto filtered = api::EngineBoundSearchReadV1(filtered_request);
  passed &= Require(ExactSuccess(filtered, kFilterDigest, 2),
                    "SX-04 structured filter stream drifted");
  auto miss_request = filtered_request;
  miss_request.filter.category_text = "missing";
  const auto miss = api::EngineBoundSearchReadV1(miss_request);
  passed &= Require(ExactSuccess(miss, kEmptyDigest, 0),
                    "SX-05 filter miss did not publish empty typed batch");
  auto over_top_request = SearchRequest(
      base, api::EngineBoundSearchOperationV1::kPhrase, "alpha beta", 50);
  const auto over_top = api::EngineBoundSearchReadV1(over_top_request);
  passed &= Require(over_top.ok && over_top.rows.size() == 2 &&
                        Digest(ResultBytes(over_top)) == kPhraseDigest,
                    "SX-07 top-k greater than survivors drifted");
  passed &= Require(fuzzy.rows.size() == 2 &&
                        fuzzy.rows[0].document_uuid == TestUuid(2) &&
                        fuzzy.rows[1].document_uuid == TestUuid(4) &&
                        fuzzy.rows[0].encoded_score ==
                            fuzzy.rows[1].encoded_score,
                    "SX-08 fuzzy score tie was not UUID ordered");

  auto wrong_query = SearchRequest(
      base, api::EngineBoundSearchOperationV1::kTerms, "---", 2);
  passed &= Require(
      ExactRefusal(api::EngineBoundSearchReadV1(wrong_query),
                   "SB_MODEL_SEARCH_QUERY_TYPE_REFUSED_V1", false),
      "SX-09 zero-token query did not fail before access");
  auto token_limit = SearchRequest(
      base, api::EngineBoundSearchOperationV1::kTerms,
      "a b c d e f g h i j k l m n o p q", 2);
  passed &= Require(
      ExactRefusal(api::EngineBoundSearchReadV1(token_limit),
                   "SB_MODEL_SEARCH_QUERY_TOKEN_LIMIT_REFUSED_V1", false),
      "SX-10 query token limit did not fail before access");
  auto fuzzy_edit = SearchRequest(
      base, api::EngineBoundSearchOperationV1::kFuzzy, "alpha", 2);
  fuzzy_edit.fuzzy_maximum_edits = 2;
  passed &= Require(
      ExactRefusal(api::EngineBoundSearchReadV1(fuzzy_edit),
                   "SB_MODEL_SEARCH_QUERY_TOKEN_LIMIT_REFUSED_V1", false),
      "SX-10 fuzzy edit substitution was accepted");

  auto analyzer = SearchRequest(
      base, api::EngineBoundSearchOperationV1::kTerms, "alpha", 2);
  analyzer.analyzer_pipeline_sha256.back() = '0';
  passed &= Require(
      ExactRefusal(api::EngineBoundSearchReadV1(analyzer),
                   "SB_MODEL_SEARCH_ANALYZER_BINDING_REQUIRED_V1", false),
      "SX-12 analyzer substitution was accepted");
  auto bad_filter = SearchRequest(
      base, api::EngineBoundSearchOperationV1::kTerms, "alpha", 2);
  bad_filter.filter = {true, std::string(4097, 'x')};
  passed &= Require(
      ExactRefusal(api::EngineBoundSearchReadV1(bad_filter),
                   "SB_MODEL_SEARCH_FILTER_REFUSED_V1", false),
      "SX-13 oversized filter was accepted");
  auto zero_top = SearchRequest(
      base, api::EngineBoundSearchOperationV1::kTerms, "alpha", 0);
  passed &= Require(
      ExactRefusal(api::EngineBoundSearchReadV1(zero_top),
                   "SB_MODEL_SEARCH_TOP_K_REFUSED_V1", false),
      "SX-14 zero top-k was accepted");
  auto bounded_top = SearchRequest(
      base, api::EngineBoundSearchOperationV1::kTerms, "alpha", 3);
  bounded_top.maximum_output_rows = 2;
  passed &= Require(
      ExactRefusal(api::EngineBoundSearchReadV1(bounded_top),
                   "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1", false),
      "SX-14 result-bound top-k was accepted");

  SearchFixture empty;
  passed &= Require(BuildFixture(FixtureKind::kEmpty, &empty),
                    "EMPTY_BODY fixture construction failed");
  if (passed) {
    const auto empty_result = api::EngineBoundSearchReadV1(SearchRequest(
        empty, api::EngineBoundSearchOperationV1::kTerms, "alpha", 3));
    passed &= Require(ExactSuccess(empty_result, kEmptyDigest, 0),
                      "SX-06/SX-15 empty collection/body drifted");
  }

  SearchFixture invalid;
  passed &= Require(BuildFixture(FixtureKind::kInvalid, &invalid),
                    "INVALID_BODY fixture construction failed");
  if (passed) {
    const auto invalid_result = api::EngineBoundSearchReadV1(SearchRequest(
        invalid, api::EngineBoundSearchOperationV1::kTerms, "alpha", 3));
    passed &= Require(
        ExactRefusal(invalid_result, "SB_MODEL_SEARCH_DOCUMENT_INVALID_V1",
                     true),
        "SX-11 invalid acquired document did not fail atomically");
  }

  SearchFixture duplicate;
  passed &= Require(BuildFixture(FixtureKind::kDuplicate, &duplicate),
                    "DUPLICATE_VISIBLE fixture construction failed");
  if (passed) {
    const auto duplicate_result = api::EngineBoundSearchReadV1(SearchRequest(
        duplicate, api::EngineBoundSearchOperationV1::kTerms, "alpha", 3));
    passed &= Require(
        ExactRefusal(
            duplicate_result,
            "SB_MODEL_SEARCH_DUPLICATE_VISIBLE_DOCUMENT_UUID_REFUSED_V1", true),
        "SX-16 duplicate visible document did not fail atomically");
  }

  auto typed = SearchRequest(
      base, api::EngineBoundSearchOperationV1::kTerms, "alpha", 2);
  std::swap(typed.output_descriptors[3], typed.output_descriptors[4]);
  passed &= Require(
      ExactRefusal(api::EngineBoundSearchReadV1(typed),
                   "SB_MODEL_TYPED_EXCHANGE_INVALID_V1", false),
      "SX-25 typed output substitution was accepted");
  auto overflow = SearchRequest(
      base, api::EngineBoundSearchOperationV1::kTerms, "alpha", 2);
  overflow.maximum_memory_bytes = 1;
  passed &= Require(
      ExactRefusal(api::EngineBoundSearchReadV1(overflow),
                   "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1", false),
      "SX-27 pre-access resource overflow was accepted");
  auto acquired_bound = SearchRequest(
      base, api::EngineBoundSearchOperationV1::kTerms, "alpha", 2);
  acquired_bound.maximum_tokens = 1;
  passed &= Require(
      ExactRefusal(api::EngineBoundSearchReadV1(acquired_bound),
                   "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1", true),
      "SX-28 acquired token bound did not clean up once");
  auto cancelled = SearchRequest(
      base, api::EngineBoundSearchOperationV1::kTerms, "alpha", 2);
  cancelled.cancellation_requested = [] { return true; };
  passed &= Require(
      ExactRefusal(api::EngineBoundSearchReadV1(cancelled),
                   "SB_MODEL_EXECUTION_CANCELLED_V1", false),
      "SX-29 pre-access cancellation did not fail atomically");

  const auto replay = api::EngineBoundSearchReadV1(SearchRequest(
      base, api::EngineBoundSearchOperationV1::kTerms, "alpha beta", 3));
  passed &= Require(ExactSuccess(replay, kTermsDigest, 3) &&
                        ResultBytes(replay) == ResultBytes(terms) &&
                        replay.scanned_row_version_count ==
                            terms.scanned_row_version_count &&
                        replay.cleanup_count == terms.cleanup_count,
                    "SX-31 deterministic replay drifted");
  return passed;
}

api::EngineNoSqlProviderGenerationMetadata KatMetadata() {
  api::EngineNoSqlProviderGenerationMetadata metadata;
  metadata.family = api::EngineNoSqlProviderFamily::kSearch;
  metadata.provider_id = TestUuid(0x100);
  metadata.database_identity = "database-identity-search-kat-v1";
  metadata.database_uuid = TestUuid(0x101);
  metadata.collection_uuid = std::string(kCollection);
  metadata.generation_uuid = TestUuid(0x102);
  metadata.generation_id = 8;
  metadata.descriptor_epoch = 11;
  metadata.security_epoch = 13;
  metadata.redaction_epoch = 13;
  metadata.catalog_epoch = 17;
  metadata.publish_state = "published";
  metadata.validation_state = "validated";
  metadata.backup_metadata_ref = "kat-backup-v1";
  metadata.restore_metadata_ref = "kat-restore-v1";
  metadata.repair_metadata_ref = "kat-repair-v1";
  metadata.support_bundle_evidence_id = "kat-support-bundle-v1";
  metadata.search_segment_candidate_present = true;
  metadata.search_segment_capability_uuid =
      "328af2f6-4305-8320-a753-0a3c3952d067";
  metadata.search_segment_index_uuid = TestUuid(0x103);
  metadata.search_segment_uuid = TestUuid(0x104);
  metadata.search_segment_base_relation_uuid = std::string(kCollection);
  metadata.search_segment_base_relation_generation = 19;
  metadata.search_segment_relation_descriptor_uuid = TestUuid(0x105);
  metadata.search_segment_relation_descriptor_generation = 23;
  metadata.search_segment_body_column_uuid = TestUuid(0x106);
  metadata.search_segment_body_descriptor_uuid = TestUuid(0x107);
  metadata.search_segment_body_type_uuid = TestUuid(0x108);
  metadata.search_segment_category_column_uuid = TestUuid(0x109);
  metadata.search_segment_category_descriptor_uuid = TestUuid(0x113);
  metadata.search_segment_category_type_uuid = TestUuid(0x114);
  metadata.search_segment_search_type_descriptor_uuid = TestUuid(0x115);
  metadata.search_segment_search_type_descriptor_generation = 29;
  metadata.search_segment_analyzer_uuid = std::string(kAnalyzer);
  metadata.search_segment_analyzer_generation = 7;
  metadata.search_segment_analyzer_pipeline_sha256 =
      std::string(kAnalyzerDigest);
  metadata.search_segment_tokenizer_uuid = TestUuid(0x116);
  metadata.search_segment_tokenizer_generation = 7;
  metadata.search_segment_language_profile_uuid = TestUuid(0x117);
  metadata.search_segment_language_profile_generation = 7;
  metadata.search_segment_ranking_model_uuid = std::string(kRanking);
  metadata.search_segment_ranking_model_generation = 7;
  metadata.search_segment_phrase_profile_uuid = TestUuid(0x118);
  metadata.search_segment_phrase_profile_generation = 7;
  metadata.search_segment_query_syntax_profile_uuid = TestUuid(0x119);
  metadata.search_segment_query_syntax_profile_generation = 7;
  metadata.search_segment_index_profile_id = "sb_full_text_positioned_v1";
  metadata.search_segment_generation = 31;
  metadata.search_segment_position_payload_present = true;
  metadata.search_segment_checksum_valid = true;
  metadata.search_segment_sealed_generation = true;
  metadata.search_segment_publish_attestation_state =
      "SEARCH_SEGMENT_SECTION_9_FULL_CORPUS_EXACT_V1";
  metadata.search_segment_statement_uuid = TestUuid(0x700);
  metadata.search_segment_statement_snapshot_uuid = TestUuid(0x701);
  metadata.search_segment_statement_metadata_snapshot_uuid = TestUuid(0x702);
  metadata.search_segment_owning_transaction_uuid = TestUuid(0x703);
  metadata.search_segment_local_transaction_id = 800;
  metadata.search_segment_snapshot_visible_through_local_transaction_id = 900;
  metadata.search_segment_security_context_uuid = TestUuid(0x704);
  metadata.search_segment_catalog_epoch_uuid = TestUuid(0x705);
  metadata.search_segment_exact_fallback_available = true;
  metadata.search_segment_full_corpus_exact_recheck_required = true;
  metadata.search_segment_residual_recheck_required = true;
  metadata.search_segment_base_row_mga_recheck_required = true;
  metadata.search_segment_security_recheck_required = true;
  return metadata;
}

void AppendLengthPrefixed(const std::string_view value, std::string* out) {
  out->append(std::to_string(value.size()));
  out->push_back(':');
  out->append(value);
}

std::string LocalKatSeed(
    const api::EngineNoSqlProviderGenerationMetadata& metadata) {
  std::string seed;
  const auto field = [&](const std::string_view name,
                         const std::string_view value) {
    AppendLengthPrefixed(name, &seed);
    AppendLengthPrefixed(value, &seed);
  };
  const auto number = [&](const std::string_view name,
                          const std::uint64_t value) {
    field(name, std::to_string(value));
  };
  const auto boolean = [&](const std::string_view name, const bool value) {
    field(name, value ? "true" : "false");
  };
  AppendLengthPrefixed("SCRATCHBIRD.SEARCH_SEGMENT_CAPABILITY_BINDING.V1",
                       &seed);
  field("family", api::EngineNoSqlProviderFamilyName(metadata.family));
  field("provider_id", metadata.provider_id);
  field("database_identity", metadata.database_identity);
  field("database_uuid", metadata.database_uuid);
  field("collection_uuid", metadata.collection_uuid);
  field("generation_uuid", metadata.generation_uuid);
  number("generation_id", metadata.generation_id);
  number("descriptor_epoch", metadata.descriptor_epoch);
  number("security_epoch", metadata.security_epoch);
  number("redaction_epoch", metadata.redaction_epoch);
  number("catalog_epoch", metadata.catalog_epoch);
  field("publish_state", metadata.publish_state);
  field("validation_state", metadata.validation_state);
  boolean("provider_claims_transaction_finality_authority",
          metadata.provider_claims_transaction_finality_authority);
  boolean("provider_claims_visibility_authority",
          metadata.provider_claims_visibility_authority);
  boolean("search_segment_candidate_present",
          metadata.search_segment_candidate_present);
#define SB_SEARCH_KAT_FIELD(name) field(#name, metadata.name)
#define SB_SEARCH_KAT_NUMBER(name) number(#name, metadata.name)
#define SB_SEARCH_KAT_BOOL(name) boolean(#name, metadata.name)
  SB_SEARCH_KAT_FIELD(search_segment_index_uuid);
  SB_SEARCH_KAT_FIELD(search_segment_uuid);
  SB_SEARCH_KAT_FIELD(search_segment_base_relation_uuid);
  SB_SEARCH_KAT_NUMBER(search_segment_base_relation_generation);
  SB_SEARCH_KAT_FIELD(search_segment_relation_descriptor_uuid);
  SB_SEARCH_KAT_NUMBER(search_segment_relation_descriptor_generation);
  SB_SEARCH_KAT_FIELD(search_segment_body_column_uuid);
  SB_SEARCH_KAT_FIELD(search_segment_body_descriptor_uuid);
  SB_SEARCH_KAT_FIELD(search_segment_body_type_uuid);
  SB_SEARCH_KAT_FIELD(search_segment_category_column_uuid);
  SB_SEARCH_KAT_FIELD(search_segment_category_descriptor_uuid);
  SB_SEARCH_KAT_FIELD(search_segment_category_type_uuid);
  SB_SEARCH_KAT_FIELD(search_segment_search_type_descriptor_uuid);
  SB_SEARCH_KAT_NUMBER(search_segment_search_type_descriptor_generation);
  SB_SEARCH_KAT_FIELD(search_segment_analyzer_uuid);
  SB_SEARCH_KAT_NUMBER(search_segment_analyzer_generation);
  SB_SEARCH_KAT_FIELD(search_segment_analyzer_pipeline_sha256);
  SB_SEARCH_KAT_FIELD(search_segment_tokenizer_uuid);
  SB_SEARCH_KAT_NUMBER(search_segment_tokenizer_generation);
  SB_SEARCH_KAT_FIELD(search_segment_language_profile_uuid);
  SB_SEARCH_KAT_NUMBER(search_segment_language_profile_generation);
  SB_SEARCH_KAT_FIELD(search_segment_ranking_model_uuid);
  SB_SEARCH_KAT_NUMBER(search_segment_ranking_model_generation);
  SB_SEARCH_KAT_FIELD(search_segment_phrase_profile_uuid);
  SB_SEARCH_KAT_NUMBER(search_segment_phrase_profile_generation);
  SB_SEARCH_KAT_FIELD(search_segment_query_syntax_profile_uuid);
  SB_SEARCH_KAT_NUMBER(search_segment_query_syntax_profile_generation);
  SB_SEARCH_KAT_FIELD(search_segment_index_profile_id);
  SB_SEARCH_KAT_NUMBER(search_segment_generation);
  SB_SEARCH_KAT_BOOL(search_segment_position_payload_present);
  SB_SEARCH_KAT_BOOL(search_segment_checksum_valid);
  SB_SEARCH_KAT_BOOL(search_segment_sealed_generation);
  SB_SEARCH_KAT_FIELD(search_segment_publish_attestation_state);
  SB_SEARCH_KAT_FIELD(search_segment_statement_uuid);
  SB_SEARCH_KAT_FIELD(search_segment_statement_snapshot_uuid);
  SB_SEARCH_KAT_FIELD(search_segment_statement_metadata_snapshot_uuid);
  SB_SEARCH_KAT_FIELD(search_segment_owning_transaction_uuid);
  SB_SEARCH_KAT_NUMBER(search_segment_local_transaction_id);
  SB_SEARCH_KAT_NUMBER(
      search_segment_snapshot_visible_through_local_transaction_id);
  SB_SEARCH_KAT_FIELD(search_segment_security_context_uuid);
  SB_SEARCH_KAT_FIELD(search_segment_catalog_epoch_uuid);
  SB_SEARCH_KAT_BOOL(search_segment_exact_fallback_available);
  SB_SEARCH_KAT_BOOL(search_segment_full_corpus_exact_recheck_required);
  SB_SEARCH_KAT_BOOL(search_segment_residual_recheck_required);
  SB_SEARCH_KAT_BOOL(search_segment_base_row_mga_recheck_required);
  SB_SEARCH_KAT_BOOL(search_segment_security_recheck_required);
  SB_SEARCH_KAT_BOOL(search_segment_index_claims_visibility_authority);
  SB_SEARCH_KAT_BOOL(search_segment_index_claims_transaction_finality_authority);
  SB_SEARCH_KAT_BOOL(search_segment_parser_claims_visibility_authority);
  SB_SEARCH_KAT_BOOL(search_segment_parser_claims_transaction_finality_authority);
  SB_SEARCH_KAT_BOOL(search_segment_client_claims_visibility_authority);
  SB_SEARCH_KAT_BOOL(search_segment_client_claims_transaction_finality_authority);
  SB_SEARCH_KAT_BOOL(search_segment_reference_claims_visibility_authority);
  SB_SEARCH_KAT_BOOL(
      search_segment_reference_claims_transaction_finality_authority);
  SB_SEARCH_KAT_BOOL(search_segment_wal_claims_visibility_authority);
  SB_SEARCH_KAT_BOOL(search_segment_wal_claims_transaction_finality_authority);
#undef SB_SEARCH_KAT_FIELD
#undef SB_SEARCH_KAT_NUMBER
#undef SB_SEARCH_KAT_BOOL
  return seed;
}

std::string LocalCapabilityUuid(const hash::Digest256& digest) {
  std::array<std::uint8_t, 16> bytes{};
  std::copy_n(digest.begin(), bytes.size(), bytes.begin());
  bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0fU) | 0x80U);
  bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3fU) | 0x80U);
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10) out << '-';
    out << std::setw(2) << static_cast<unsigned>(bytes[index]);
  }
  return out.str();
}

bool CarrierKat() {
  const auto metadata = KatMetadata();
  const auto seed = LocalKatSeed(metadata);
  const auto digest = hash::ComputeSha256Digest(
      reinterpret_cast<const platform::byte*>(seed.data()), seed.size());
  if (!Require(seed.size() == 4248, "KAT seed length drifted") ||
      !Require(digest.ok(), "KAT seed hash failed") ||
      !Require(hash::HexLower(digest.digest) ==
                   "328af2f64305332067530a3c3952d067507aeb207b3bc55ee6097be9ec394361",
               "KAT SHA-256 drifted") ||
      !Require(LocalCapabilityUuid(digest.digest) ==
                   "328af2f6-4305-8320-a753-0a3c3952d067",
               "test-local KAT UUID drifted") ||
      !Require(api::DeriveSearchSegmentCapabilityUuidV1(metadata) ==
                   "328af2f6-4305-8320-a753-0a3c3952d067",
               "production KAT UUID drifted") ||
      !Require(api::ValidateSearchSegmentCapabilityBindingV1(metadata),
               "production KAT binding was refused")) {
    return false;
  }
  auto corrupt = metadata;
  corrupt.search_segment_capability_uuid.back() = '9';
  return Require(!api::ValidateSearchSegmentCapabilityBindingV1(corrupt),
                 "corrupt search capability binding was accepted");
}

using Pairs = std::vector<std::pair<std::string, std::string>>;

std::string PairValue(const Pairs& pairs, const std::string_view key) {
  const auto found = std::ranges::find_if(
      pairs, [&](const auto& pair) { return pair.first == key; });
  return found == pairs.end() ? std::string{} : found->second;
}

bool SetPair(Pairs* pairs, const std::string_view key,
             const std::string_view value) {
  if (pairs == nullptr) return false;
  const auto found = std::ranges::find_if(
      *pairs, [&](const auto& pair) { return pair.first == key; });
  if (found == pairs->end()) return false;
  found->second = value;
  return true;
}

bool Contains(const std::vector<std::string_view>& fields,
              const std::string_view field) {
  return std::ranges::find(fields, field) != fields.end();
}

const std::vector<std::string_view>& SearchBoolFields() {
  static const std::vector<std::string_view> fields{
      "search_segment_candidate_present",
      "search_segment_position_payload_present",
      "search_segment_checksum_valid",
      "search_segment_sealed_generation",
      "search_segment_exact_fallback_available",
      "search_segment_full_corpus_exact_recheck_required",
      "search_segment_residual_recheck_required",
      "search_segment_base_row_mga_recheck_required",
      "search_segment_security_recheck_required",
      "search_segment_index_claims_visibility_authority",
      "search_segment_index_claims_transaction_finality_authority",
      "search_segment_parser_claims_visibility_authority",
      "search_segment_parser_claims_transaction_finality_authority",
      "search_segment_client_claims_visibility_authority",
      "search_segment_client_claims_transaction_finality_authority",
      "search_segment_reference_claims_visibility_authority",
      "search_segment_reference_claims_transaction_finality_authority",
      "search_segment_wal_claims_visibility_authority",
      "search_segment_wal_claims_transaction_finality_authority",
  };
  return fields;
}

const std::vector<std::string_view>& NumericSeedFields() {
  static const std::vector<std::string_view> fields{
      "generation_id", "descriptor_epoch", "security_epoch",
      "redaction_epoch", "catalog_epoch",
      "search_segment_base_relation_generation",
      "search_segment_relation_descriptor_generation",
      "search_segment_search_type_descriptor_generation",
      "search_segment_analyzer_generation",
      "search_segment_tokenizer_generation",
      "search_segment_language_profile_generation",
      "search_segment_ranking_model_generation",
      "search_segment_phrase_profile_generation",
      "search_segment_query_syntax_profile_generation",
      "search_segment_generation", "search_segment_local_transaction_id",
      "search_segment_snapshot_visible_through_local_transaction_id",
  };
  return fields;
}

const std::vector<std::string_view>& SearchCarrierFields() {
  static const std::vector<std::string_view> fields{
      "search_segment_candidate_present", "search_segment_capability_uuid",
      "search_segment_index_uuid", "search_segment_uuid",
      "search_segment_base_relation_uuid",
      "search_segment_base_relation_generation",
      "search_segment_relation_descriptor_uuid",
      "search_segment_relation_descriptor_generation",
      "search_segment_body_column_uuid",
      "search_segment_body_descriptor_uuid",
      "search_segment_body_type_uuid",
      "search_segment_category_column_uuid",
      "search_segment_category_descriptor_uuid",
      "search_segment_category_type_uuid",
      "search_segment_search_type_descriptor_uuid",
      "search_segment_search_type_descriptor_generation",
      "search_segment_analyzer_uuid", "search_segment_analyzer_generation",
      "search_segment_analyzer_pipeline_sha256",
      "search_segment_tokenizer_uuid", "search_segment_tokenizer_generation",
      "search_segment_language_profile_uuid",
      "search_segment_language_profile_generation",
      "search_segment_ranking_model_uuid",
      "search_segment_ranking_model_generation",
      "search_segment_phrase_profile_uuid",
      "search_segment_phrase_profile_generation",
      "search_segment_query_syntax_profile_uuid",
      "search_segment_query_syntax_profile_generation",
      "search_segment_index_profile_id", "search_segment_generation",
      "search_segment_position_payload_present",
      "search_segment_checksum_valid", "search_segment_sealed_generation",
      "search_segment_publish_attestation_state",
      "search_segment_statement_uuid", "search_segment_statement_snapshot_uuid",
      "search_segment_statement_metadata_snapshot_uuid",
      "search_segment_owning_transaction_uuid",
      "search_segment_local_transaction_id",
      "search_segment_snapshot_visible_through_local_transaction_id",
      "search_segment_security_context_uuid",
      "search_segment_catalog_epoch_uuid",
      "search_segment_exact_fallback_available",
      "search_segment_full_corpus_exact_recheck_required",
      "search_segment_residual_recheck_required",
      "search_segment_base_row_mga_recheck_required",
      "search_segment_security_recheck_required",
      "search_segment_index_claims_visibility_authority",
      "search_segment_index_claims_transaction_finality_authority",
      "search_segment_parser_claims_visibility_authority",
      "search_segment_parser_claims_transaction_finality_authority",
      "search_segment_client_claims_visibility_authority",
      "search_segment_client_claims_transaction_finality_authority",
      "search_segment_reference_claims_visibility_authority",
      "search_segment_reference_claims_transaction_finality_authority",
      "search_segment_wal_claims_visibility_authority",
      "search_segment_wal_claims_transaction_finality_authority",
  };
  return fields;
}

const std::vector<std::string_view>& SeedFields() {
  static const std::vector<std::string_view> fields = [] {
    std::vector<std::string_view> result{
        "family", "provider_id", "database_identity", "database_uuid",
        "collection_uuid", "generation_uuid", "generation_id",
        "descriptor_epoch", "security_epoch", "redaction_epoch",
        "catalog_epoch", "publish_state", "validation_state",
        "provider_claims_transaction_finality_authority",
        "provider_claims_visibility_authority"};
    for (const auto field : SearchCarrierFields()) {
      if (field != "search_segment_capability_uuid") result.push_back(field);
    }
    return result;
  }();
  return fields;
}

const std::vector<std::string_view>& ActiveRequiredFields() {
  static const std::vector<std::string_view> fields{
      "search_segment_capability_uuid", "search_segment_index_uuid",
      "search_segment_uuid", "search_segment_base_relation_uuid",
      "search_segment_base_relation_generation",
      "search_segment_relation_descriptor_uuid",
      "search_segment_relation_descriptor_generation",
      "search_segment_body_column_uuid",
      "search_segment_body_descriptor_uuid",
      "search_segment_body_type_uuid",
      "search_segment_category_column_uuid",
      "search_segment_category_descriptor_uuid",
      "search_segment_category_type_uuid",
      "search_segment_search_type_descriptor_uuid",
      "search_segment_search_type_descriptor_generation",
      "search_segment_analyzer_uuid", "search_segment_analyzer_generation",
      "search_segment_analyzer_pipeline_sha256",
      "search_segment_tokenizer_uuid", "search_segment_tokenizer_generation",
      "search_segment_language_profile_uuid",
      "search_segment_language_profile_generation",
      "search_segment_ranking_model_uuid",
      "search_segment_ranking_model_generation",
      "search_segment_phrase_profile_uuid",
      "search_segment_phrase_profile_generation",
      "search_segment_query_syntax_profile_uuid",
      "search_segment_query_syntax_profile_generation",
      "search_segment_index_profile_id", "search_segment_generation",
      "search_segment_position_payload_present",
      "search_segment_checksum_valid", "search_segment_sealed_generation",
      "search_segment_publish_attestation_state",
      "search_segment_statement_uuid", "search_segment_statement_snapshot_uuid",
      "search_segment_statement_metadata_snapshot_uuid",
      "search_segment_owning_transaction_uuid",
      "search_segment_local_transaction_id",
      "search_segment_snapshot_visible_through_local_transaction_id",
      "search_segment_security_context_uuid",
      "search_segment_catalog_epoch_uuid",
      "search_segment_exact_fallback_available",
      "search_segment_full_corpus_exact_recheck_required",
      "search_segment_residual_recheck_required",
      "search_segment_base_row_mga_recheck_required",
      "search_segment_security_recheck_required",
  };
  return fields;
}

std::string DefaultSearchValue(const std::string_view field) {
  if (Contains(SearchBoolFields(), field)) return "false";
  if (Contains(NumericSeedFields(), field)) return "0";
  return {};
}

std::string DistinctSeedValue(const std::string_view field,
                              const std::string& current) {
  if (field == "provider_claims_transaction_finality_authority" ||
      field == "provider_claims_visibility_authority" ||
      Contains(SearchBoolFields(), field)) {
    return current == "true" ? "false" : "true";
  }
  if (Contains(NumericSeedFields(), field)) {
    return std::to_string(std::stoull(current) + 1);
  }
  if (field == "family") return "document";
  if (current.size() == 36 && current[8] == '-' && current[13] == '-' &&
      current[18] == '-' && current[23] == '-') {
    auto changed = current;
    changed.back() = changed.back() == 'f' ? 'e' : 'f';
    return changed;
  }
  return current + ".mutated";
}

bool RawPersistenceMutationMatrix() {
  if (!Require(SeedFields().size() == 72, "seed mutation inventory drifted") ||
      !Require(SearchCarrierFields().size() == 58,
               "carrier field inventory drifted") ||
      !Require(ActiveRequiredFields().size() == 47,
               "required-active inventory drifted")) {
    return false;
  }
  const auto original_directory = std::filesystem::current_path();
  const auto unique = std::to_string(
      std::chrono::high_resolution_clock::now().time_since_epoch().count());
  const auto scratch = std::filesystem::temp_directory_path() /
                       ("scratchbird_rcp078_search_kat_" + unique);
  std::error_code filesystem_error;
  std::filesystem::create_directories(scratch, filesystem_error);
  if (filesystem_error) return Require(false, "KAT scratch creation failed");
  std::filesystem::current_path(scratch, filesystem_error);
  if (filesystem_error) return Require(false, "KAT scratch entry failed");

  api::EngineRequestContext context;
  context.request_id = "RCP-078-SEARCH-SEGMENT-CARRIER-KAT-V1";
  context.database_path = "database-identity-search-kat-v1";
  context.database_uuid.canonical = TestUuid(0x101);
  const auto store_path = std::filesystem::path(
      context.database_path + ".sb.nosql_provider_generations");
  const auto metadata = KatMetadata();
  const auto restore = [&]() {
    (void)api::CleanupNoSqlProviderGenerations(context, true);
    std::filesystem::current_path(original_directory, filesystem_error);
    std::filesystem::remove_all(scratch, filesystem_error);
  };
  if (!api::PublishNoSqlProviderGeneration(context, metadata).ok) {
    restore();
    return Require(false, "KAT carrier publication failed");
  }
  std::string baseline_bytes;
  {
    std::ifstream in(store_path, std::ios::binary);
    baseline_bytes.assign(std::istreambuf_iterator<char>(in),
                          std::istreambuf_iterator<char>());
  }
  const auto first_tab = baseline_bytes.find('\t');
  const auto second_tab = baseline_bytes.find('\t', first_tab + 1);
  const auto newline = baseline_bytes.find('\n', second_tab + 1);
  if (first_tab == std::string::npos || second_tab == std::string::npos ||
      newline == std::string::npos) {
    restore();
    return Require(false, "KAT persistence envelope malformed");
  }
  const auto baseline_pairs = api::DecodeCrudPairs(
      baseline_bytes.substr(second_tab + 1, newline - second_tab - 1));
  const auto write_bytes = [&](const std::string& bytes) {
    std::ofstream out(store_path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.flush();
    return static_cast<bool>(out);
  };
  const auto write_pairs = [&](const Pairs& pairs) {
    return write_bytes("SBNOSQLPG1\tGENERATION\t" +
                       api::EncodeCrudPairs(pairs) + "\n");
  };
  const auto evict = [&]() {
    return api::CleanupNoSqlProviderGenerations(context, false).ok;
  };
  const auto load = [&]() {
    return api::LoadNoSqlProviderGeneration(
        context, api::EngineNoSqlProviderFamily::kSearch,
        metadata.provider_id, metadata.collection_uuid);
  };
  const auto baseline_valid = [&]() {
    const auto loaded = load();
    return loaded.ok &&
           api::ValidateSearchSegmentCapabilityBindingV1(loaded.metadata) &&
           loaded.metadata.search_segment_capability_uuid ==
               metadata.search_segment_capability_uuid;
  };
  const auto run_case = [&](const std::string_view label, const Pairs& pairs) {
    if (!write_bytes(baseline_bytes) || !evict() || !baseline_valid() ||
        !write_pairs(pairs) || !evict()) {
      return Require(false, std::string(label) + " fixture transition failed");
    }
    const bool refused = !load().ok;
    const bool restored =
        write_bytes(baseline_bytes) && evict() && baseline_valid();
    return Require(refused && restored,
                   std::string(label) + " did not fail closed and restore");
  };

  bool exact = evict() && baseline_valid();
  std::size_t mutation_count = 0;
  for (const auto field : SeedFields()) {
    auto pairs = baseline_pairs;
    ++mutation_count;
    std::ostringstream id;
    id << "KAT-SEED-MUT-" << std::setw(3) << std::setfill('0')
       << mutation_count;
    const bool prepared = SetPair(
        &pairs, field, DistinctSeedValue(field, PairValue(pairs, field)));
    exact = prepared && run_case(id.str(), pairs) && exact;
  }
  {
    auto pairs = baseline_pairs;
    auto capability = PairValue(pairs, "search_segment_capability_uuid");
    capability.back() = capability.back() == '9' ? '8' : '9';
    ++mutation_count;
    exact = SetPair(&pairs, "search_segment_capability_uuid", capability) &&
            run_case("KAT-CAPABILITY-MUT-073", pairs) && exact;
  }
  auto inactive_pairs = baseline_pairs;
  for (const auto field : SearchCarrierFields()) {
    exact = SetPair(&inactive_pairs, field, DefaultSearchValue(field)) && exact;
  }
  for (const auto field : SearchCarrierFields()) {
    if (field == "search_segment_candidate_present") continue;
    auto pairs = inactive_pairs;
    auto nondefault = PairValue(baseline_pairs, field);
    if (nondefault == DefaultSearchValue(field)) nondefault = "true";
    ++mutation_count;
    std::ostringstream id;
    id << "KAT-INACTIVE-PARTIAL-" << std::setw(3) << std::setfill('0')
       << mutation_count;
    exact = SetPair(&pairs, field, nondefault) && run_case(id.str(), pairs) &&
            exact;
  }
  for (const auto field : ActiveRequiredFields()) {
    auto pairs = baseline_pairs;
    ++mutation_count;
    std::ostringstream id;
    id << "KAT-ACTIVE-MISSING-" << std::setw(3) << std::setfill('0')
       << mutation_count;
    exact = SetPair(&pairs, field, DefaultSearchValue(field)) &&
            run_case(id.str(), pairs) && exact;
  }
  exact = mutation_count == 177 && exact;

  struct SupplementalRawProbe {
    std::string_view label;
    std::string_view field;
    std::string_view value;
  };
  for (const auto& probe : std::array{
           SupplementalRawProbe{"KAT-RAW-NORMALIZATION-FAMILY-ALIAS",
                                "family", "nosql.search"},
           SupplementalRawProbe{"KAT-RAW-NORMALIZATION-BOOL-UPPERCASE",
                                "search_segment_checksum_valid", "TRUE"},
           SupplementalRawProbe{"KAT-RAW-NORMALIZATION-U64-LEADING-ZERO",
                                "generation_id", "008"},
       }) {
    auto pairs = baseline_pairs;
    exact = SetPair(&pairs, probe.field, probe.value) &&
            run_case(probe.label, pairs) && exact;
  }
  auto duplicate_key = baseline_pairs;
  duplicate_key.emplace_back("search_segment_capability_uuid",
                             metadata.search_segment_capability_uuid);
  exact = run_case("KAT-RAW-NORMALIZATION-DUPLICATE-IDENTICAL-KEY",
                   duplicate_key) &&
          exact;

  const bool duplicate_written = write_bytes(baseline_bytes + baseline_bytes);
  const bool duplicate_evicted = evict();
  const bool duplicate_refused = !load().ok;
  exact = duplicate_written && duplicate_evicted && duplicate_refused && exact;
  auto malformed_drop_pairs = baseline_pairs;
  malformed_drop_pairs.erase(
      std::remove_if(malformed_drop_pairs.begin(), malformed_drop_pairs.end(),
                     [](const auto& pair) {
                       return pair.first == "search_segment_index_profile_id";
                     }),
      malformed_drop_pairs.end());
  const bool malformed_drop_written = write_bytes(
      baseline_bytes + "SBNOSQLPG1\tDROP\t" +
      api::EncodeCrudPairs(malformed_drop_pairs) + "\n");
  const bool malformed_drop_evicted = evict();
  const bool malformed_drop_refused = !load().ok;
  exact = malformed_drop_written && malformed_drop_evicted &&
          malformed_drop_refused && exact;
  restore();
  return Require(exact, "177-case raw persistence matrix drifted");
}

std::string DescriptorTypeUuid(const api::EngineDescriptor& descriptor) {
  constexpr std::string_view prefix = "type_uuid=";
  const auto begin = descriptor.encoded_descriptor.find(prefix);
  if (begin == std::string::npos) return {};
  const auto value_begin = begin + prefix.size();
  const auto end = descriptor.encoded_descriptor.find(';', value_begin);
  return descriptor.encoded_descriptor.substr(
      value_begin, end == std::string::npos ? std::string::npos
                                             : end - value_begin);
}

api::EngineNoSqlProviderGenerationMetadata SegmentMetadata(
    const SearchFixture& fixture,
    const api::EngineBoundSearchReadRequestV1& request,
    const std::uint64_t base_generation, const std::uint64_t salt) {
  auto metadata = KatMetadata();
  metadata.provider_id = request.selected_provider_uuid;
  metadata.database_identity = fixture.reader.database_path;
  metadata.database_uuid = fixture.reader.database_uuid.canonical;
  metadata.collection_uuid = request.collection_uuid;
  metadata.generation_uuid = TestUuid(0x900 + salt);
  metadata.generation_id = 100 + salt;
  metadata.descriptor_epoch = fixture.storage.descriptor_generation;
  metadata.security_epoch = fixture.reader.security_epoch;
  metadata.redaction_epoch = fixture.reader.security_epoch;
  metadata.catalog_epoch = fixture.reader.catalog_generation_id;
  metadata.search_segment_index_uuid = TestUuid(0xa00 + salt);
  metadata.search_segment_uuid = TestUuid(0xb00 + salt);
  metadata.search_segment_base_relation_uuid = request.collection_uuid;
  metadata.search_segment_base_relation_generation = base_generation;
  metadata.search_segment_relation_descriptor_uuid =
      fixture.storage.descriptor_uuid.canonical;
  metadata.search_segment_relation_descriptor_generation =
      fixture.storage.descriptor_generation;
  metadata.search_segment_body_column_uuid =
      fixture.storage.columns[0].column_uuid.canonical;
  metadata.search_segment_body_descriptor_uuid =
      fixture.storage.columns[0].value_descriptor.descriptor_uuid.canonical;
  metadata.search_segment_body_type_uuid =
      DescriptorTypeUuid(fixture.storage.columns[0].value_descriptor);
  metadata.search_segment_category_column_uuid =
      fixture.storage.columns[1].column_uuid.canonical;
  metadata.search_segment_category_descriptor_uuid =
      fixture.storage.columns[1].value_descriptor.descriptor_uuid.canonical;
  metadata.search_segment_category_type_uuid =
      DescriptorTypeUuid(fixture.storage.columns[1].value_descriptor);
  metadata.search_segment_search_type_descriptor_uuid = TestUuid(0xc00 + salt);
  metadata.search_segment_search_type_descriptor_generation = 1;
  metadata.search_segment_analyzer_uuid = request.analyzer_uuid;
  metadata.search_segment_analyzer_generation = request.analyzer_generation;
  metadata.search_segment_analyzer_pipeline_sha256 =
      request.analyzer_pipeline_sha256;
  metadata.search_segment_statement_uuid =
      fixture.reader.statement_uuid.canonical;
  metadata.search_segment_statement_snapshot_uuid =
      fixture.reader.statement_snapshot_uuid.canonical;
  metadata.search_segment_statement_metadata_snapshot_uuid =
      fixture.reader.statement_metadata_snapshot_uuid.canonical;
  metadata.search_segment_owning_transaction_uuid =
      fixture.reader.transaction_uuid.canonical;
  metadata.search_segment_local_transaction_id =
      fixture.reader.local_transaction_id;
  metadata.search_segment_snapshot_visible_through_local_transaction_id =
      fixture.reader.snapshot_visible_through_local_transaction_id;
  metadata.search_segment_security_context_uuid =
      fixture.reader.authorization_context.authority_uuid.canonical;
  metadata.search_segment_catalog_epoch_uuid =
      fixture.reader.catalog_epoch_uuid.canonical;
  metadata.search_segment_capability_uuid =
      api::DeriveSearchSegmentCapabilityUuidV1(metadata);
  return metadata;
}

bool SegmentAndFallbackMatrix() {
  SearchFixture fixture;
  if (!Require(BuildFixture(FixtureKind::kBase, &fixture),
               "segment BASE fixture construction failed")) {
    return false;
  }
  bool passed = true;
  auto baseline_request = SearchRequest(
      fixture, api::EngineBoundSearchOperationV1::kTerms, "alpha beta", 3);
  const auto baseline = api::EngineBoundSearchReadV1(baseline_request);
  passed &= Require(ExactSuccess(baseline, kTermsDigest, 3),
                    "segment baseline stream drifted");

  auto missing = baseline_request;
  missing.physical_route =
      api::EngineBoundSearchPhysicalRouteV1::kSegmentWithExactFallback;
  missing.selected_provider_uuid = TestUuid(0xd01);
  missing.selected_capability_uuid = TestUuid(0xd02);
  const auto missing_result = api::EngineBoundSearchReadV1(missing);
  passed &= Require(ExactSuccess(missing_result, kTermsDigest, 3) &&
                        missing_result.exact_fallback_selected &&
                        !missing_result.segment_carrier_loaded,
                    "SX-20 missing-segment fallback drifted: " +
                        missing_result.diagnostic.code + ":" +
                        missing_result.diagnostic.detail);

  auto current = baseline_request;
  current.physical_route =
      api::EngineBoundSearchPhysicalRouteV1::kSegmentWithExactFallback;
  current.selected_provider_uuid = TestUuid(0xd10);
  auto current_metadata = SegmentMetadata(
      fixture, current, baseline.current_relation_base_generation, 1);
  current.selected_capability_uuid =
      current_metadata.search_segment_capability_uuid;
  passed &= Require(!current.selected_capability_uuid.empty() &&
                        api::PublishNoSqlProviderGeneration(fixture.reader,
                                                           current_metadata)
                            .ok,
                    "current segment publication failed");
  const auto current_result = api::EngineBoundSearchReadV1(current);
  passed &= Require(ExactSuccess(current_result, kTermsDigest, 3) &&
                        current_result.segment_carrier_loaded &&
                        current_result.segment_candidate_hint_selected &&
                        !current_result.exact_fallback_selected,
                    "SX-17/SX-18 candidate-nonauthority route drifted");

  auto older = baseline_request;
  older.physical_route =
      api::EngineBoundSearchPhysicalRouteV1::kSegmentWithExactFallback;
  older.selected_provider_uuid = TestUuid(0xd20);
  auto older_metadata = SegmentMetadata(
      fixture, older, baseline.current_relation_base_generation - 1, 2);
  older.selected_capability_uuid = older_metadata.search_segment_capability_uuid;
  passed &= Require(!older.selected_capability_uuid.empty() &&
                        api::PublishNoSqlProviderGeneration(fixture.reader,
                                                           older_metadata)
                            .ok,
                    "older segment publication failed");
  const auto older_result = api::EngineBoundSearchReadV1(older);
  passed &= Require(ExactSuccess(older_result, kTermsDigest, 3) &&
                        older_result.segment_carrier_loaded &&
                        older_result.exact_fallback_selected &&
                        !older_result.segment_candidate_hint_selected,
                    "SX-20 older-segment fallback drifted");

  auto substituted = baseline_request;
  substituted.physical_route =
      api::EngineBoundSearchPhysicalRouteV1::kSegmentWithExactFallback;
  substituted.selected_provider_uuid = TestUuid(0xd30);
  auto substituted_metadata = SegmentMetadata(
      fixture, substituted, baseline.current_relation_base_generation, 3);
  substituted_metadata.search_segment_statement_uuid = TestUuid(0x700);
  substituted_metadata.search_segment_capability_uuid =
      api::DeriveSearchSegmentCapabilityUuidV1(substituted_metadata);
  substituted.selected_capability_uuid =
      substituted_metadata.search_segment_capability_uuid;
  passed &= Require(
      !substituted.selected_capability_uuid.empty() &&
          api::PublishNoSqlProviderGeneration(fixture.reader,
                                              substituted_metadata)
              .ok,
      "UUIDv4-substituted live carrier publication fixture failed");
  const auto substituted_result = api::EngineBoundSearchReadV1(substituted);
  passed &= Require(
      ExactRefusal(substituted_result,
                   "SB_MODEL_PROVIDER_GENERATION_STALE_V1", false),
      "UUIDv4-substituted live carrier did not fail closed");

  const auto store_path = std::filesystem::path(
      fixture.reader.database_path + ".sb.nosql_provider_generations");
  std::string carrier_bytes;
  {
    std::ifstream in(store_path, std::ios::binary);
    carrier_bytes.assign(std::istreambuf_iterator<char>(in),
                         std::istreambuf_iterator<char>());
  }
  {
    std::ofstream out(store_path, std::ios::binary | std::ios::app);
    out.write(carrier_bytes.data(),
              static_cast<std::streamsize>(carrier_bytes.size()));
  }
  passed &= Require(!carrier_bytes.empty() &&
                        api::CleanupNoSqlProviderGenerations(fixture.reader,
                                                             false)
                            .ok,
                    "corrupt segment cohort fixture failed");
  const auto corrupt_result = api::EngineBoundSearchReadV1(current);
  passed &= Require(
      ExactRefusal(corrupt_result, "SB_MODEL_PROVIDER_GENERATION_STALE_V1",
                   false),
      "SX-22 duplicate carrier cohort did not fail before access");
  return passed;
}

api::RelationalTypeDescriptor DagDescriptor(
    const std::uint32_t descriptor_id, std::string descriptor_uuid,
    std::string type_uuid) {
  api::RelationalTypeDescriptor descriptor;
  descriptor.descriptor_id = descriptor_id;
  descriptor.descriptor_uuid = std::move(descriptor_uuid);
  descriptor.type_uuid = std::move(type_uuid);
  descriptor.nullability = api::RelationalNullability::kNonNull;
  return descriptor;
}

api::TypedRelationalDag ProductionSearchDag(
    const SearchFixture& fixture, const std::string_view query_operator,
    const std::string_view query_text, const std::uint32_t top_k) {
  const auto& context = fixture.reader;
  const auto uuid_type = CoreTypeUuid("uuid");
  const auto uint64_type = CoreTypeUuid("uint64");
  const auto real64_type = CoreTypeUuid("real64");
  const auto body_type =
      DescriptorTypeUuid(fixture.storage.columns[0].value_descriptor);
  const auto category_type =
      DescriptorTypeUuid(fixture.storage.columns[1].value_descriptor);
  api::TypedRelationalDag dag;
  dag.wire_version = 2;
  dag.bound_sblr_tree_uuid = GeneratedUuid(platform::UuidKind::object, 0xe00);
  dag.bound_catalog_epoch_uuid = context.catalog_epoch_uuid.canonical;
  dag.bound_security_context_uuid =
      context.authorization_context.authority_uuid.canonical;
  dag.statement_uuid = context.statement_uuid.canonical;
  dag.statement_timestamp = context.statement_timestamp;
  dag.owning_transaction_uuid = context.transaction_uuid.canonical;
  dag.statement_snapshot_uuid = context.statement_snapshot_uuid.canonical;
  dag.statement_metadata_snapshot_uuid =
      context.statement_metadata_snapshot_uuid.canonical;
  dag.local_transaction_id = context.local_transaction_id;
  dag.snapshot_visible_through_local_transaction_id =
      context.snapshot_visible_through_local_transaction_id;
  dag.root_node_id = 1;
  dag.descriptors = {
      DagDescriptor(101, GeneratedUuid(platform::UuidKind::object, 0xe01),
                    uuid_type),
      DagDescriptor(102, GeneratedUuid(platform::UuidKind::object, 0xe02),
                    uuid_type),
      DagDescriptor(103, GeneratedUuid(platform::UuidKind::object, 0xe03),
                    uint64_type),
      DagDescriptor(104, GeneratedUuid(platform::UuidKind::object, 0xe04),
                    real64_type),
      DagDescriptor(105, GeneratedUuid(platform::UuidKind::object, 0xe05),
                    uint64_type),
      DagDescriptor(
          106,
          fixture.storage.columns[0].value_descriptor.descriptor_uuid.canonical,
          body_type),
      DagDescriptor(
          107,
          fixture.storage.columns[1].value_descriptor.descriptor_uuid.canonical,
          category_type),
  };

  static constexpr std::array<std::string_view, 5> kNames{
      "document_uuid", "analyzer_uuid", "analyzer_generation", "score",
      "rank"};
  for (std::size_t ordinal = 0; ordinal < kNames.size(); ++ordinal) {
    api::RelationalExpressionRecord output;
    output.expression_id = static_cast<std::uint32_t>(ordinal + 1);
    output.result_descriptor_id = static_cast<std::uint32_t>(101 + ordinal);
    if (ordinal == 1) {
      output.expression_kind = api::RelationalExpressionKind::kIdentifier;
      output.bound_name_uuid = std::string(kAnalyzer);
    } else if (ordinal == 2) {
      output.expression_kind = api::RelationalExpressionKind::kLiteral;
      output.literal_kind = api::RelationalLiteralKind::kNumeric;
      output.literal_or_parameter_ref = "7";
    } else if (ordinal == 4) {
      output.expression_kind = api::RelationalExpressionKind::kLiteral;
      output.literal_kind = api::RelationalLiteralKind::kNumeric;
      output.literal_or_parameter_ref = std::to_string(top_k);
    } else {
      output.expression_kind = api::RelationalExpressionKind::kIdentifier;
      output.bound_name_uuid = std::string(kCollection);
    }
    dag.expressions.push_back(std::move(output));
    dag.outputs.push_back(
        {static_cast<std::uint32_t>(ordinal + 1), 1,
         static_cast<std::uint32_t>(ordinal + 1), std::string(kNames[ordinal]),
         static_cast<std::uint32_t>(101 + ordinal), true,
         static_cast<std::uint32_t>(ordinal)});
  }
  api::RelationalExpressionRecord alias;
  alias.expression_id = 6;
  alias.expression_kind = api::RelationalExpressionKind::kIdentifier;
  alias.result_descriptor_id = 106;
  alias.bound_name_uuid = std::string(kCollection);
  dag.expressions.push_back(std::move(alias));
  api::RelationalExpressionRecord text;
  text.expression_id = 7;
  text.expression_kind = api::RelationalExpressionKind::kLiteral;
  text.result_descriptor_id = 106;
  text.literal_kind = api::RelationalLiteralKind::kString;
  text.literal_or_parameter_ref = std::string(query_text);
  dag.expressions.push_back(std::move(text));
  api::RelationalExpressionRecord query;
  query.expression_id = 8;
  query.expression_kind = api::RelationalExpressionKind::kFunctionCall;
  query.result_descriptor_id = 106;
  query.operator_name = std::string(query_operator);
  query.child_expression_ids = {7};
  dag.expressions.push_back(std::move(query));
  api::RelationalExpressionRecord digest;
  digest.expression_id = 9;
  digest.expression_kind = api::RelationalExpressionKind::kLiteral;
  digest.result_descriptor_id = 106;
  digest.literal_kind = api::RelationalLiteralKind::kString;
  digest.literal_or_parameter_ref = std::string(kAnalyzerDigest);
  dag.expressions.push_back(std::move(digest));
  api::RelationalExpressionRecord binding;
  binding.expression_id = 10;
  binding.expression_kind = api::RelationalExpressionKind::kFunctionCall;
  binding.result_descriptor_id = 102;
  binding.operator_name = "SEARCH_ANALYZER_BINDING";
  binding.child_expression_ids = {2, 3, 9};
  dag.expressions.push_back(std::move(binding));
  api::RelationalExpressionRecord match;
  match.expression_id = 11;
  match.expression_kind = api::RelationalExpressionKind::kFunctionCall;
  match.result_descriptor_id = 101;
  match.operator_name = "SEARCH_MATCH";
  match.child_expression_ids = {6, 8, 10, 5};
  dag.expressions.push_back(std::move(match));
  api::RelationalExpressionRecord category;
  category.expression_id = 12;
  category.expression_kind = api::RelationalExpressionKind::kIdentifier;
  category.result_descriptor_id = 107;
  category.bound_name_uuid =
      fixture.storage.columns[1].column_uuid.canonical;
  dag.expressions.push_back(std::move(category));

  api::RelationalDagNode source;
  source.node_id = 1;
  source.node_kind = api::RelationalDagNodeKind::kScan;
  source.output_descriptor_ids = {101, 102, 103, 104, 105};
  for (const auto& expression : dag.expressions) {
    source.bound_expression_ids.push_back(expression.expression_id);
  }
  source.required_object_uuids = {std::string(kCollection)};
  source.semantic_variant_id = "SBLR_MODEL_SOURCE_V1";
  dag.nodes.push_back(std::move(source));
  return dag;
}

api::TypedRelationalDag ProductionSearchUnaryCompositionDag(
    const SearchFixture& fixture) {
  auto dag = ProductionSearchDag(
      fixture, "SEARCH_TERMS", "alpha beta", 3);
  dag.root_node_id = 5;
  dag.descriptors.push_back(DagDescriptor(
      108, GeneratedUuid(platform::UuidKind::object, 0xe08),
      CoreTypeUuid("boolean")));
  dag.descriptors.push_back(DagDescriptor(
      109, GeneratedUuid(platform::UuidKind::object, 0xe09),
      CoreRuntimeTypeUuid("int64")));

  std::array<std::uint32_t, 5> projected_expression_ids{};
  for (std::size_t ordinal = 0; ordinal < projected_expression_ids.size();
       ++ordinal) {
    api::RelationalExpressionRecord identifier;
    identifier.expression_id = static_cast<std::uint32_t>(42 + ordinal);
    identifier.expression_kind = api::RelationalExpressionKind::kIdentifier;
    identifier.result_descriptor_id =
        static_cast<std::uint32_t>(101 + ordinal);
    identifier.bound_name_uuid =
        dag.descriptors[ordinal].descriptor_uuid;
    projected_expression_ids[ordinal] = identifier.expression_id;
    dag.expressions.push_back(std::move(identifier));
  }

  api::RelationalExpressionRecord predicate;
  predicate.expression_id = 40;
  predicate.expression_kind = api::RelationalExpressionKind::kLiteral;
  predicate.result_descriptor_id = 108;
  predicate.literal_kind = api::RelationalLiteralKind::kBoolean;
  predicate.literal_or_parameter_ref = "TRUE";
  dag.expressions.push_back(std::move(predicate));
  api::RelationalDagNode filter;
  filter.node_id = 2;
  filter.node_kind = api::RelationalDagNodeKind::kFilter;
  filter.input_node_ids = {1};
  filter.output_descriptor_ids = {101, 102, 103, 104, 105};
  filter.bound_expression_ids = {40};
  filter.semantic_variant_id = "filter.where.v1";
  dag.nodes.push_back(std::move(filter));

  static constexpr std::array<std::string_view, 5> kNames{
      "document_uuid", "analyzer_uuid", "analyzer_generation", "score",
      "rank"};
  for (std::size_t ordinal = 0; ordinal < kNames.size(); ++ordinal) {
    dag.outputs.push_back(
        {static_cast<std::uint32_t>(20 + ordinal), 3,
         projected_expression_ids[ordinal],
         std::string(kNames[ordinal]),
         static_cast<std::uint32_t>(101 + ordinal), true,
         static_cast<std::uint32_t>(ordinal)});
  }
  api::RelationalDagNode project;
  project.node_id = 3;
  project.node_kind = api::RelationalDagNodeKind::kProject;
  project.input_node_ids = {2};
  project.output_descriptor_ids = {101, 102, 103, 104, 105};
  project.bound_expression_ids.assign(projected_expression_ids.begin(),
                                      projected_expression_ids.end());
  project.semantic_variant_id = "project.select-list.v1";
  dag.nodes.push_back(std::move(project));

  const auto ordering_uuid =
      GeneratedUuid(platform::UuidKind::object, 0xe10);
  api::RelationalPropertyRecord ordering;
  ordering.property_uuid = ordering_uuid;
  ordering.property_kind = api::RelationalPropertyKind::kOrdering;
  ordering.origin_node_id = 4;
  ordering.ordering_terms.push_back(
      {projected_expression_ids[4],
       api::RelationalPropertySortDirection::kAscending,
       api::RelationalPropertyNullPlacement::kNullsLast, {}});
  dag.properties.push_back(std::move(ordering));
  api::RelationalDagNode sort;
  sort.node_id = 4;
  sort.node_kind = api::RelationalDagNodeKind::kSort;
  sort.input_node_ids = {3};
  sort.output_descriptor_ids = {101, 102, 103, 104, 105};
  sort.bound_expression_ids = {projected_expression_ids[4]};
  sort.semantic_variant_id = "sort.required-order.v1";
  sort.required_property_uuids = {ordering_uuid};
  sort.delivered_property_uuids = {ordering_uuid};
  dag.nodes.push_back(std::move(sort));

  constexpr std::string_view kRowNumberFunctionUuid =
      "019de5fc-2400-7539-bcce-00eef3ae7220";
  api::RelationalExpressionRecord row_number;
  row_number.expression_id = 41;
  row_number.expression_kind = api::RelationalExpressionKind::kFunctionCall;
  row_number.result_descriptor_id = 109;
  row_number.function_uuid = std::string(kRowNumberFunctionUuid);
  dag.expressions.push_back(std::move(row_number));
  for (std::size_t ordinal = 0; ordinal < kNames.size(); ++ordinal) {
    dag.outputs.push_back(
        {static_cast<std::uint32_t>(30 + ordinal), 5,
         projected_expression_ids[ordinal],
         std::string(kNames[ordinal]),
         static_cast<std::uint32_t>(101 + ordinal), true,
         static_cast<std::uint32_t>(ordinal)});
  }
  dag.outputs.push_back({35, 5, 41, "row_number", 109, true, 5});
  const auto window_uuid =
      GeneratedUuid(platform::UuidKind::object, 0xe11);
  api::RelationalPropertyRecord window_property;
  window_property.property_uuid = window_uuid;
  window_property.property_kind = api::RelationalPropertyKind::kWindow;
  window_property.origin_node_id = 5;
  window_property.dependency_property_uuids = {ordering_uuid};
  window_property.window_frame_descriptor_uuid =
      GeneratedUuid(platform::UuidKind::object, 0xe12);
  dag.properties.push_back(std::move(window_property));
  api::RelationalDagNode window;
  window.node_id = 5;
  window.node_kind = api::RelationalDagNodeKind::kWindow;
  window.input_node_ids = {4};
  window.output_descriptor_ids = {101, 102, 103, 104, 105, 109};
  window.bound_expression_ids = {projected_expression_ids[4], 41};
  window.semantic_variant_id = "window.row-number.v1";
  window.required_property_uuids = {ordering_uuid};
  window.delivered_property_uuids = {ordering_uuid, window_uuid};
  dag.nodes.push_back(std::move(window));
  api::RelationalWindowDefinitionRecord definition;
  definition.window_id = 1;
  definition.relation_node_id = 5;
  definition.ordering_terms = {
      {projected_expression_ids[4],
       api::RelationalPropertySortDirection::kAscending,
       api::RelationalPropertyNullPlacement::kNullsLast, {}}};
  dag.window_definitions.push_back(std::move(definition));
  api::RelationalWindowInvocationRecord invocation;
  invocation.invocation_id = 1;
  invocation.relation_node_id = 5;
  invocation.function_expression_id = 41;
  invocation.window_definition_id = 1;
  invocation.function_abi_version = 1;
  invocation.builtin_id = "sb.window.row_number";
  invocation.function_uuid = std::string(kRowNumberFunctionUuid);
  invocation.result_descriptor_id = 109;
  invocation.output_name_utf8 = "row_number";
  dag.window_invocations.push_back(std::move(invocation));
  return dag;
}

api::TypedRelationalDag ProductionSearchCteFetchDag(
    const SearchFixture& fixture) {
  auto dag = ProductionSearchDag(
      fixture, "SEARCH_TERMS", "alpha beta", 3);
  dag.root_node_id = 3;
  api::RelationalDagNode cte;
  cte.node_id = 2;
  cte.node_kind = api::RelationalDagNodeKind::kCte;
  cte.input_node_ids = {1};
  cte.output_descriptor_ids = {101, 102, 103, 104, 105};
  cte.shareable = true;
  cte.semantic_variant_id = "cte.bound.v1";
  dag.nodes.push_back(std::move(cte));
  dag.descriptors.push_back(DagDescriptor(
      108, GeneratedUuid(platform::UuidKind::object, 0xe20),
      CoreRuntimeTypeUuid("int64")));
  api::RelationalExpressionRecord count;
  count.expression_id = 40;
  count.expression_kind = api::RelationalExpressionKind::kLiteral;
  count.result_descriptor_id = 108;
  count.literal_kind = api::RelationalLiteralKind::kNumeric;
  count.literal_or_parameter_ref = "2";
  dag.expressions.push_back(std::move(count));
  api::RelationalExpressionRecord offset;
  offset.expression_id = 41;
  offset.expression_kind = api::RelationalExpressionKind::kLiteral;
  offset.result_descriptor_id = 108;
  offset.literal_kind = api::RelationalLiteralKind::kNumeric;
  offset.literal_or_parameter_ref = "0";
  dag.expressions.push_back(std::move(offset));
  api::RelationalDagNode fetch;
  fetch.node_id = 3;
  fetch.node_kind = api::RelationalDagNodeKind::kLimit;
  fetch.input_node_ids = {2};
  fetch.output_descriptor_ids = {101, 102, 103, 104, 105};
  fetch.bound_expression_ids = {40, 41};
  fetch.semantic_variant_id = "fetch.first-rows-only-offset.v1";
  dag.nodes.push_back(std::move(fetch));
  return dag;
}

api::TypedRelationalDag ProductionSearchCountDag(
    const SearchFixture& fixture) {
  auto dag = ProductionSearchDag(
      fixture, "SEARCH_TERMS", "alpha beta", 3);
  dag.root_node_id = 2;
  dag.descriptors.push_back(DagDescriptor(
      108, GeneratedUuid(platform::UuidKind::object, 0xe30),
      CoreRuntimeTypeUuid("int64")));
  const auto count = exec::LookupCanonicalAggregateByFunctionV1(
      exec::CanonicalAggregateFunction::count);
  api::RelationalExpressionRecord aggregate;
  aggregate.expression_id = 40;
  aggregate.expression_kind = api::RelationalExpressionKind::kFunctionCall;
  aggregate.result_descriptor_id = 108;
  if (count != nullptr) aggregate.function_uuid = count->function_uuid;
  dag.expressions.push_back(std::move(aggregate));
  dag.outputs.push_back({20, 2, 40, "search_count", 108, true, 0});
  api::RelationalDagNode aggregate_node;
  aggregate_node.node_id = 2;
  aggregate_node.node_kind = api::RelationalDagNodeKind::kAggregate;
  aggregate_node.input_node_ids = {1};
  aggregate_node.output_descriptor_ids = {108};
  aggregate_node.bound_expression_ids = {40};
  aggregate_node.semantic_variant_id = "aggregate.global-count-star.v1";
  dag.nodes.push_back(std::move(aggregate_node));
  return dag;
}

api::TypedRelationalDag ProductionSearchGroupedAggregateDag(
    const SearchFixture& fixture) {
  auto dag = ProductionSearchDag(
      fixture, "SEARCH_TERMS", "alpha beta", 3);
  dag.root_node_id = 3;
  const auto int64_type = CoreRuntimeTypeUuid("int64");
  dag.descriptors.push_back(DagDescriptor(
      108, GeneratedUuid(platform::UuidKind::object, 0xe31), int64_type));
  dag.descriptors.push_back(DagDescriptor(
      109, GeneratedUuid(platform::UuidKind::object, 0xe32), int64_type));
  dag.descriptors.push_back(DagDescriptor(
      110, GeneratedUuid(platform::UuidKind::object, 0xe33), int64_type));
  auto sum_descriptor = DagDescriptor(
      111, GeneratedUuid(platform::UuidKind::object, 0xe34), int64_type);
  sum_descriptor.nullability = api::RelationalNullability::kNullable;
  dag.descriptors.push_back(std::move(sum_descriptor));

  api::RelationalExpressionRecord group_literal;
  group_literal.expression_id = 40;
  group_literal.expression_kind = api::RelationalExpressionKind::kLiteral;
  group_literal.result_descriptor_id = 108;
  group_literal.literal_kind = api::RelationalLiteralKind::kNumeric;
  group_literal.literal_or_parameter_ref = "1";
  dag.expressions.push_back(std::move(group_literal));
  api::RelationalExpressionRecord sum_literal;
  sum_literal.expression_id = 41;
  sum_literal.expression_kind = api::RelationalExpressionKind::kLiteral;
  sum_literal.result_descriptor_id = 109;
  sum_literal.literal_kind = api::RelationalLiteralKind::kNumeric;
  sum_literal.literal_or_parameter_ref = "2";
  dag.expressions.push_back(std::move(sum_literal));
  dag.outputs.push_back({20, 2, 40, "group_key", 108, true, 0});
  dag.outputs.push_back({21, 2, 41, "sum_value", 109, true, 1});
  api::RelationalDagNode project;
  project.node_id = 2;
  project.node_kind = api::RelationalDagNodeKind::kProject;
  project.input_node_ids = {1};
  project.output_descriptor_ids = {108, 109};
  project.bound_expression_ids = {40, 41};
  project.semantic_variant_id = "project.select-list.v1";
  dag.nodes.push_back(std::move(project));

  api::RelationalExpressionRecord group_key;
  group_key.expression_id = 42;
  group_key.expression_kind = api::RelationalExpressionKind::kIdentifier;
  group_key.result_descriptor_id = 108;
  group_key.bound_name_uuid = dag.descriptors[7].descriptor_uuid;
  dag.expressions.push_back(std::move(group_key));
  api::RelationalExpressionRecord sum_input;
  sum_input.expression_id = 43;
  sum_input.expression_kind = api::RelationalExpressionKind::kIdentifier;
  sum_input.result_descriptor_id = 109;
  sum_input.bound_name_uuid = dag.descriptors[8].descriptor_uuid;
  dag.expressions.push_back(std::move(sum_input));
  const auto count = exec::LookupCanonicalAggregateByFunctionV1(
      exec::CanonicalAggregateFunction::count);
  api::RelationalExpressionRecord count_expression;
  count_expression.expression_id = 44;
  count_expression.expression_kind =
      api::RelationalExpressionKind::kFunctionCall;
  count_expression.result_descriptor_id = 110;
  if (count != nullptr) count_expression.function_uuid = count->function_uuid;
  dag.expressions.push_back(std::move(count_expression));
  const auto sum = exec::LookupCanonicalAggregateByFunctionV1(
      exec::CanonicalAggregateFunction::sum);
  api::RelationalExpressionRecord sum_expression;
  sum_expression.expression_id = 45;
  sum_expression.expression_kind =
      api::RelationalExpressionKind::kFunctionCall;
  sum_expression.result_descriptor_id = 111;
  if (sum != nullptr) sum_expression.function_uuid = sum->function_uuid;
  sum_expression.child_expression_ids = {43};
  dag.expressions.push_back(std::move(sum_expression));
  dag.outputs.push_back({30, 3, 42, "group_key", 108, true, 0});
  dag.outputs.push_back({31, 3, 44, "group_count", 110, true, 1});
  dag.outputs.push_back({32, 3, 45, "rank_sum", 111, true, 2});
  api::RelationalDagNode aggregate;
  aggregate.node_id = 3;
  aggregate.node_kind = api::RelationalDagNodeKind::kAggregate;
  aggregate.input_node_ids = {2};
  aggregate.output_descriptor_ids = {108, 110, 111};
  aggregate.bound_expression_ids = {42, 44, 45};
  aggregate.semantic_variant_id =
      "aggregate.grouped-int64-key-count-sum.v1";
  dag.nodes.push_back(std::move(aggregate));
  return dag;
}

api::TypedRelationalDag ProductionSearchRecursiveDag(
    const SearchFixture& fixture) {
  auto dag = ProductionSearchCountDag(fixture);
  dag.root_node_id = 4;
  api::RelationalExpressionRecord bound;
  bound.expression_id = 41;
  bound.expression_kind = api::RelationalExpressionKind::kLiteral;
  bound.result_descriptor_id = 108;
  bound.literal_kind = api::RelationalLiteralKind::kNumeric;
  bound.literal_or_parameter_ref = "5";
  dag.expressions.push_back(std::move(bound));
  api::RelationalDagNode term;
  term.node_id = 3;
  term.node_kind = api::RelationalDagNodeKind::kCte;
  term.output_descriptor_ids = {108};
  term.semantic_variant_id = "cte.recursive-term-int64-increment.v1";
  dag.nodes.push_back(std::move(term));
  api::RelationalDagNode recursive;
  recursive.node_id = 4;
  recursive.node_kind = api::RelationalDagNodeKind::kRecursiveCte;
  recursive.input_node_ids = {2, 3};
  recursive.output_descriptor_ids = {108};
  recursive.bound_expression_ids = {41};
  recursive.semantic_variant_id =
      "cte.recursive-union-all-int64-increment.v1";
  dag.nodes.push_back(std::move(recursive));
  return dag;
}

api::TypedRelationalDag ProductionSearchSetDag(
    const SearchFixture& fixture) {
  auto dag = ProductionSearchDag(
      fixture, "SEARCH_TERMS", "alpha beta", 3);
  dag.root_node_id = 4;
  api::RelationalExpressionRecord document_uuid;
  document_uuid.expression_id = 40;
  document_uuid.expression_kind =
      api::RelationalExpressionKind::kIdentifier;
  document_uuid.result_descriptor_id = 101;
  document_uuid.bound_name_uuid = dag.descriptors.front().descriptor_uuid;
  dag.expressions.push_back(std::move(document_uuid));
  dag.outputs.push_back({20, 2, 40, "document_uuid", 101, true, 0});
  api::RelationalDagNode project;
  project.node_id = 2;
  project.node_kind = api::RelationalDagNodeKind::kProject;
  project.input_node_ids = {1};
  project.output_descriptor_ids = {101};
  project.bound_expression_ids = {40};
  project.semantic_variant_id = "project.select-list.v1";
  dag.nodes.push_back(std::move(project));
  api::RelationalExpressionRecord literal;
  literal.expression_id = 41;
  literal.expression_kind = api::RelationalExpressionKind::kLiteral;
  literal.result_descriptor_id = 101;
  literal.literal_kind = api::RelationalLiteralKind::kUuid;
  literal.literal_or_parameter_ref =
      "30000000-0000-4000-8000-000000000099";
  dag.expressions.push_back(std::move(literal));
  dag.values_rows.push_back({1, {41}});
  dag.outputs.push_back({21, 3, 41, "document_uuid", 101, true, 0});
  api::RelationalDagNode values;
  values.node_id = 3;
  values.node_kind = api::RelationalDagNodeKind::kValues;
  values.output_descriptor_ids = {101};
  values.values_row_ids = {1};
  values.semantic_variant_id = "values.literal-table.v1";
  dag.nodes.push_back(std::move(values));
  api::RelationalDagNode set;
  set.node_id = 4;
  set.node_kind = api::RelationalDagNodeKind::kSetOperation;
  set.input_node_ids = {2, 3};
  set.output_descriptor_ids = {101};
  set.semantic_variant_id = "set-operation.union-all.v1";
  dag.nodes.push_back(std::move(set));
  return dag;
}

api::TypedRelationalDag ProductionSearchMixedJoinDag(
    const SearchFixture& fixture) {
  auto dag = ProductionSearchDag(
      fixture, "SEARCH_TERMS", "alpha beta", 3);
  dag.root_node_id = 3;
  for (std::size_t ordinal = 0; ordinal < 2; ++ordinal) {
    const auto descriptor_id = static_cast<std::uint32_t>(201 + ordinal);
    const auto expression_id = static_cast<std::uint32_t>(50 + ordinal);
    const auto& column = fixture.join_storage.columns[ordinal];
    auto descriptor = DagDescriptor(
        descriptor_id,
        column.value_descriptor.descriptor_uuid.canonical,
        DescriptorTypeUuid(column.value_descriptor));
    descriptor.nullability =
        column.nullable ? api::RelationalNullability::kNullable
                        : api::RelationalNullability::kNonNull;
    dag.descriptors.push_back(std::move(descriptor));
    api::RelationalExpressionRecord expression;
    expression.expression_id = expression_id;
    expression.expression_kind = api::RelationalExpressionKind::kIdentifier;
    expression.result_descriptor_id = descriptor_id;
    expression.bound_name_uuid = column.column_uuid.canonical;
    dag.expressions.push_back(std::move(expression));
    dag.outputs.push_back(
        {expression_id, 2, expression_id, column.canonical_name_key,
         descriptor_id, true, static_cast<std::uint32_t>(ordinal)});
  }
  dag.descriptors.push_back(DagDescriptor(
      203, GeneratedUuid(platform::UuidKind::object, 0xe40),
      CoreTypeUuid("boolean")));
  api::RelationalDagNode heap;
  heap.node_id = 2;
  heap.node_kind = api::RelationalDagNodeKind::kScan;
  heap.output_descriptor_ids = {201, 202};
  heap.bound_expression_ids = {50, 51};
  heap.required_object_uuids = {
      fixture.join_storage.relation_uuid.canonical};
  heap.semantic_variant_id = "relation.source.v1";
  dag.nodes.push_back(std::move(heap));

  api::RelationalExpressionRecord equality;
  equality.expression_id = 60;
  equality.expression_kind = api::RelationalExpressionKind::kBinary;
  equality.child_expression_ids = {1, 50};
  equality.result_descriptor_id = 203;
  equality.operator_name = "=";
  dag.expressions.push_back(std::move(equality));
  const std::array<std::string_view, 7> names{
      "document_uuid", "analyzer_uuid", "analyzer_generation", "score",
      "rank", "join_document_uuid", "payload"};
  const std::array<std::uint32_t, 7> expressions{1, 2, 3, 4, 5, 50, 51};
  const std::array<std::uint32_t, 7> descriptors{101, 102, 103, 104,
                                                  105, 201, 202};
  for (std::size_t ordinal = 0; ordinal < names.size(); ++ordinal) {
    dag.outputs.push_back(
        {static_cast<std::uint32_t>(70 + ordinal), 3,
         expressions[ordinal], std::string(names[ordinal]),
         descriptors[ordinal], true, static_cast<std::uint32_t>(ordinal)});
  }
  api::RelationalDagNode join;
  join.node_id = 3;
  join.node_kind = api::RelationalDagNodeKind::kJoin;
  join.input_node_ids = {1, 2};
  join.output_descriptor_ids.assign(descriptors.begin(), descriptors.end());
  join.bound_expression_ids = {60};
  join.semantic_variant_id = "join.inner.v1";
  dag.nodes.push_back(std::move(join));
  return dag;
}

std::vector<opt::MultilegDescriptorProfileV1>
ProductionSearchMultilegProfilesV10(const api::TypedRelationalDag& dag) {
  const std::array<std::string, 5> type_uuids{
      CoreTypeUuid("uuid"), CoreTypeUuid("uint64"),
      CoreTypeUuid("real64"), CoreTypeUuid("boolean"),
      CoreTypeUuid("geometry")};
  const auto descriptor_uuid = [&](const std::uint32_t descriptor_id) {
    const auto descriptor = std::ranges::find_if(
        dag.descriptors, [&](const auto& candidate) {
          return candidate.descriptor_id == descriptor_id;
        });
    return descriptor == dag.descriptors.end() ? std::string{}
                                                : descriptor->descriptor_uuid;
  };
  const auto bound_descriptor_uuid = [&](const std::uint8_t kind,
                                         const std::uint16_t slot) {
    if (kind == 14 && slot == 0) return descriptor_uuid(101);
    if (kind == 14 && slot == 1) return descriptor_uuid(102);
    if (kind == 16 && slot == 0) return descriptor_uuid(103);
    if (kind == 16 && slot == 1) return descriptor_uuid(105);
    if (kind == 18 && slot == 0) return descriptor_uuid(104);
    return GeneratedUuid(platform::UuidKind::object,
                         0xf000 + static_cast<std::uint64_t>(kind) * 32 +
                             slot);
  };
  std::vector<opt::MultilegDescriptorProfileV1> profiles;
  profiles.reserve(320);
  for (std::uint16_t type_pair = 0; type_pair < type_uuids.size();
       ++type_pair) {
    for (std::uint16_t nullable = 0; nullable < 2; ++nullable) {
      const auto kind =
          static_cast<std::uint8_t>(14 + type_pair * 2 + nullable);
      for (std::uint16_t slot = 0; slot < 32; ++slot) {
        profiles.push_back({kind, slot, bound_descriptor_uuid(kind, slot),
                            type_uuids[type_pair], nullable != 0});
      }
    }
  }
  return profiles;
}

std::string ApiResultBytes(const api::EngineApiResult& result) {
  std::string bytes;
  for (const auto& row : result.result_shape.rows) {
    if (row.fields.size() != 5) return {};
    for (std::size_t ordinal = 0; ordinal < row.fields.size(); ++ordinal) {
      if (ordinal != 0) bytes.push_back('\t');
      bytes += row.fields[ordinal].second.encoded_value;
    }
    bytes.push_back('\n');
  }
  return bytes;
}

std::string ApiRowField(const api::EngineApiResult& result,
                        const std::size_t row,
                        const std::string_view name) {
  if (row >= result.result_shape.rows.size()) return {};
  const auto& fields = result.result_shape.rows[row].fields;
  const auto found = std::ranges::find_if(fields, [&](const auto& field) {
    return field.first == name;
  });
  return found == fields.end() ? std::string{} : found->second.encoded_value;
}

bool ProductionCanonicalRoute() {
  SearchFixture fixture;
  if (!Require(BuildFixture(FixtureKind::kBase, &fixture),
               "production BASE fixture construction failed")) {
    return false;
  }
  const auto dag = ProductionSearchDag(fixture, "SEARCH_TERMS", "alpha beta", 3);
  const auto execution =
      sblr::ExecuteCanonicalCurrentHeapQuery({fixture.reader, dag});
  const auto replay =
      sblr::ExecuteCanonicalCurrentHeapQuery({fixture.reader, dag});
  const auto unary = sblr::ExecuteCanonicalCurrentHeapQuery(
      {fixture.reader, ProductionSearchUnaryCompositionDag(fixture)});
  const auto cte_fetch = sblr::ExecuteCanonicalCurrentHeapQuery(
      {fixture.reader, ProductionSearchCteFetchDag(fixture)});
  const auto counted = sblr::ExecuteCanonicalCurrentHeapQuery(
      {fixture.reader, ProductionSearchCountDag(fixture)});
  const auto grouped = sblr::ExecuteCanonicalCurrentHeapQuery(
      {fixture.reader, ProductionSearchGroupedAggregateDag(fixture)});
  const auto recursive_dag = ProductionSearchRecursiveDag(fixture);
  const auto recursive = sblr::ExecuteCanonicalCurrentHeapQuery(
      {fixture.reader, recursive_dag});
  const auto recursive_replay = sblr::ExecuteCanonicalCurrentHeapQuery(
      {fixture.reader, recursive_dag});
  const auto set_dag = ProductionSearchSetDag(fixture);
  const auto set_union = sblr::ExecuteCanonicalCurrentHeapQuery(
      {fixture.reader, set_dag});
  const auto set_replay = sblr::ExecuteCanonicalCurrentHeapQuery(
      {fixture.reader, set_dag});
  const auto joined_dag = ProductionSearchMixedJoinDag(fixture);
  const auto direct_multileg_profiles =
      ProductionSearchMultilegProfilesV10(joined_dag);
  sblr::CanonicalObjectFreeValuesExecutionResult joined;
  bool direct_multileg_scope_exact = direct_multileg_profiles.size() == 320;
  {
    opt::MultilegDescriptorDispatchScopeV1 descriptor_scope(
        fixture.reader.statement_uuid.canonical, direct_multileg_profiles);
    direct_multileg_scope_exact &= Require(
        descriptor_scope.installed(),
        "direct search mixed-join V10 descriptor scope was not installed");
    if (descriptor_scope.installed()) {
      joined = sblr::ExecuteCanonicalCurrentHeapQuery(
          {fixture.reader, joined_dag});
    }
  }
  const auto released_scope = opt::LookupMultilegDescriptorDispatchScopeV1(
      fixture.reader.statement_uuid.canonical);
  direct_multileg_scope_exact &= Require(
      !released_scope.accepted && released_scope.profiles.empty() &&
          released_scope.diagnostic_id ==
              "SB_MODEL_RESULT_DESCRIPTOR_SCOPE_REQUIRED_V1",
      "direct search mixed-join V10 descriptor scope was not released");
  const auto diagnostic = execution.api_result.diagnostics.empty()
                              ? std::string{}
                              : execution.api_result.diagnostics.front().code +
                                    ":" +
                                    execution.api_result.diagnostics.front().detail;
  const bool exact =
      direct_multileg_scope_exact && execution.profile_matched &&
      execution.optimizer_admitted &&
      execution.optimizer_selected && execution.physical_dag_published &&
      execution.physical_dag_executed && execution.runtime_actuals_attached &&
      execution.canonical_result_published && execution.api_result.ok &&
      execution.physical_node_count == 1 &&
      execution.canonical_result_column_count == 5 &&
      execution.canonical_result_row_count == 3 &&
      Digest(ApiResultBytes(execution.api_result)) == kTermsDigest &&
      replay.api_result.ok &&
      replay.canonical_result_bytes == execution.canonical_result_bytes &&
      ApiResultBytes(replay.api_result) == ApiResultBytes(execution.api_result) &&
      unary.profile_matched && unary.optimizer_admitted &&
      unary.optimizer_selected && unary.physical_dag_published &&
      unary.physical_dag_executed && unary.runtime_actuals_attached &&
      unary.canonical_result_published && unary.api_result.ok &&
      unary.physical_node_count == 5 &&
      unary.canonical_result_column_count == 6 &&
      unary.canonical_result_row_count == 3 &&
      ApiRowField(unary.api_result, 0, "rank") == "1" &&
      ApiRowField(unary.api_result, 2, "rank") == "3" &&
      ApiRowField(unary.api_result, 0, "row_number") == "1" &&
      ApiRowField(unary.api_result, 2, "row_number") == "3" &&
      cte_fetch.api_result.ok && cte_fetch.physical_node_count == 3 &&
      cte_fetch.canonical_result_column_count == 5 &&
      cte_fetch.canonical_result_row_count == 2 &&
      ApiRowField(cte_fetch.api_result, 0, "rank") == "1" &&
      ApiRowField(cte_fetch.api_result, 1, "rank") == "2" &&
      counted.api_result.ok && counted.physical_node_count == 2 &&
      counted.canonical_result_column_count == 1 &&
      counted.canonical_result_row_count == 1 &&
      ApiRowField(counted.api_result, 0, "search_count") == "3" &&
      grouped.api_result.ok && grouped.physical_node_count == 3 &&
      grouped.canonical_result_column_count == 3 &&
      grouped.canonical_result_row_count == 1 &&
      ApiRowField(grouped.api_result, 0, "group_key") == "1" &&
      ApiRowField(grouped.api_result, 0, "group_count") == "3" &&
      ApiRowField(grouped.api_result, 0, "rank_sum") == "6" &&
      recursive.api_result.ok && recursive.physical_node_count == 4 &&
      recursive.canonical_result_column_count == 1 &&
      recursive.canonical_result_row_count == 3 &&
      ApiRowField(recursive.api_result, 0, "search_count") == "3" &&
      ApiRowField(recursive.api_result, 1, "search_count") == "4" &&
      ApiRowField(recursive.api_result, 2, "search_count") == "5" &&
      recursive_replay.api_result.ok &&
      recursive_replay.canonical_result_bytes ==
          recursive.canonical_result_bytes &&
      set_union.api_result.ok && set_union.physical_node_count == 4 &&
      set_union.canonical_result_column_count == 1 &&
      set_union.canonical_result_row_count == 4 &&
      ApiRowField(set_union.api_result, 3, "document_uuid") ==
          "30000000-0000-4000-8000-000000000099" &&
      set_replay.api_result.ok &&
      set_replay.canonical_result_bytes == set_union.canonical_result_bytes &&
      joined.api_result.ok && joined.physical_node_count == 3 &&
      joined.canonical_result_column_count == 7 &&
      joined.canonical_result_row_count == 1 &&
      ApiRowField(joined.api_result, 0, "document_uuid") == TestUuid(1) &&
      ApiRowField(joined.api_result, 0, "join_document_uuid") ==
          TestUuid(1) &&
      ApiRowField(joined.api_result, 0, "payload") == "matched";
  const auto unary_diagnostic = unary.api_result.diagnostics.empty()
                                    ? std::string{}
                                    : unary.api_result.diagnostics.front().code +
                                          ":" +
                                          unary.api_result.diagnostics.front().detail;
  const auto composition_diagnostic = [&](const auto& result) {
    return result.api_result.diagnostics.empty()
               ? std::string{}
               : result.api_result.diagnostics.front().code + ":" +
                     result.api_result.diagnostics.front().detail;
  };
  return Require(
      exact,
      "production canonical search route drifted: source=" + diagnostic +
          ";unary=" + unary_diagnostic +
          ";cte_fetch=" + composition_diagnostic(cte_fetch) +
          ";count=" + composition_diagnostic(counted) +
          ";grouped=" + composition_diagnostic(grouped) +
          ";recursive=" + composition_diagnostic(recursive) +
          ";set=" + composition_diagnostic(set_union) +
          ";join=" + composition_diagnostic(joined));
}

}  // namespace

int main() {
  if (!EngineUuidExecutionCohortAuthority() || !CarrierKat() ||
      !RawPersistenceMutationMatrix() ||
      !DirectOutcomeMatrix() || !SegmentAndFallbackMatrix()
#if defined(SB_CES05_SEARCH_PRODUCTION_QUERY_ROUTE)
      || !ProductionCanonicalRoute()
#endif
  ) {
    return 1;
  }
  std::cout << "RCP-078 search KAT, 177 raw mutations, and direct semantic "
               "outcome matrix: PASS\n";
  return 0;
}
