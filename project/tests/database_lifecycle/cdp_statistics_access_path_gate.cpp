// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "dml/insert_api.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "access_path_full.hpp"
#include "query/plan_api.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace opt = scratchbird::engine::optimizer;
namespace plan = scratchbird::engine::planner;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) Fail(message);
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

platform::TypedUuid NewUuid(platform::UuidKind kind, platform::u64 salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, NowMillis() + salt);
  Require(generated.ok(), "CDP-022 UUID generation failed");
  return generated.value;
}

std::string NewUuidText(platform::UuidKind kind, platform::u64 salt) {
  return uuid::UuidToString(NewUuid(kind, salt).value);
}

api::EngineTypedValue TextValue(std::string value) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "character";
  typed.descriptor.encoded_descriptor = "canonical=character";
  typed.encoded_value = std::move(value);
  return typed;
}

api::EngineRowValue Row(std::string id, std::string note) {
  api::EngineRowValue row;
  row.fields.push_back({"id", TextValue(std::move(id))});
  row.fields.push_back({"note", TextValue(std::move(note))});
  return row;
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view value) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && evidence.evidence_id == value) return true;
  }
  return false;
}

void DumpResult(const api::EnginePlanOperationResult& result) {
  std::cerr << "plan_kind=" << result.plan_kind << '\n';
  for (const auto& evidence : result.evidence) {
    std::cerr << evidence.evidence_kind << ':' << evidence.evidence_id << '\n';
  }
}

struct Fixture {
  std::filesystem::path dir;
  std::filesystem::path database_path;
  std::string database_uuid;
  std::string table_uuid;
  std::string index_uuid;
  api::EngineRequestContext context;

  ~Fixture() {
    std::error_code ignored;
    if (!dir.empty()) std::filesystem::remove_all(dir, ignored);
  }
};

api::EngineRequestContext BaseContext(const Fixture& fixture, std::string request_id) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = std::move(request_id);
  context.database_path = fixture.database_path.string();
  context.database_uuid.canonical = fixture.database_uuid;
  context.principal_uuid.canonical = NewUuidText(platform::UuidKind::principal, 1000);
  context.session_uuid.canonical = NewUuidText(platform::UuidKind::object, 1001);
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

api::EngineRequestContext Begin(const Fixture& fixture, std::string request_id) {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(fixture, std::move(request_id));
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
  RequireOk(begun, "CDP-022 begin transaction failed");
  auto context = request.context;
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = begun.isolation_level;
  return context;
}

api::CrudTableRecord Table(const Fixture& fixture) {
  api::CrudTableRecord table;
  table.creator_tx = fixture.context.local_transaction_id;
  table.table_uuid = fixture.table_uuid;
  table.default_name = "cdp_statistics_access_path";
  table.columns.push_back({"id", "canonical=character"});
  table.columns.push_back({"note", "canonical=character"});
  return table;
}

api::CrudIndexRecord CoveringIdIndex(const Fixture& fixture) {
  api::CrudIndexRecord index;
  index.creator_tx = fixture.context.local_transaction_id;
  index.index_uuid = fixture.index_uuid;
  index.table_uuid = fixture.table_uuid;
  index.column_name = "id";
  index.family = api::kCrudIndexFamilyBtree;
  index.profile = api::kCrudIndexProfileRowStoreScalarBtreeV1;
  index.unique = true;
  index.key_envelopes.push_back("id");
  index.key_envelopes.push_back("unique");
  index.include_columns.push_back("note");
  return index;
}

Fixture MakeFixture() {
  Fixture fixture;
  fixture.dir = std::filesystem::temp_directory_path() /
                ("scratchbird_cdp022_" + std::to_string(NowMillis()));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "cdp022.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = NewUuid(platform::UuidKind::database, 10);
  create.filespace_uuid = NewUuid(platform::UuidKind::filespace, 11);
  create.creation_unix_epoch_millis = NowMillis();
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "CDP-022 database create failed");

  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  fixture.table_uuid = NewUuidText(platform::UuidKind::object, 20);
  fixture.index_uuid = NewUuidText(platform::UuidKind::object, 21);
  fixture.context = Begin(fixture, "cdp022-metadata");

  const auto table = api::AppendMgaTableMetadata(fixture.context, Table(fixture));
  Require(!table.error, "CDP-022 table metadata append failed");
  const auto index = api::AppendMgaIndexMetadata(fixture.context, CoveringIdIndex(fixture));
  Require(!index.error, "CDP-022 index metadata append failed");

  std::vector<api::EngineRowValue> rows;
  rows.reserve(128);
  for (int i = 0; i < 128; ++i) {
    rows.push_back(Row("id-" + std::to_string(i), "note-" + std::to_string(i)));
  }

  api::EngineInsertRowsRequest insert;
  insert.context = fixture.context;
  insert.context.request_id = "cdp022-insert";
  insert.target_table.uuid.canonical = fixture.table_uuid;
  insert.target_table.object_kind = "table";
  insert.input_rows = std::move(rows);
  insert.estimated_row_count = insert.input_rows.size();
  const auto inserted = api::EngineInsertRows(insert);
  RequireOk(inserted, "CDP-022 fixture insert failed");
  Require(inserted.inserted_count == 128, "CDP-022 fixture insert count mismatch");
  return fixture;
}

api::EnginePredicateEnvelope Predicate(std::string kind,
                                       std::vector<std::string> values) {
  api::EnginePredicateEnvelope predicate;
  predicate.predicate_kind = std::move(kind);
  predicate.canonical_predicate_envelope = "id";
  for (auto& value : values) predicate.bound_values.push_back(TextValue(std::move(value)));
  return predicate;
}

api::EnginePlanOperationResult Plan(Fixture& fixture,
                                    api::EnginePredicateEnvelope predicate,
                                    std::vector<std::string> options = {}) {
  api::EnginePlanOperationRequest request;
  request.context = fixture.context;
  request.context.request_id = "cdp022-plan";
  request.target_object.uuid.canonical = fixture.table_uuid;
  request.target_object.object_kind = "table";
  request.predicate = std::move(predicate);
  request.option_envelopes = std::move(options);
  return api::EnginePlanOperation(request);
}

opt::OptimizerStatsIdentity FreshIdentity(const std::string& object_uuid,
                                          const std::string& statistic_uuid,
                                          platform::u64 visibility_epoch) {
  opt::OptimizerStatsIdentity identity;
  identity.object_uuid = object_uuid;
  identity.statistic_uuid = statistic_uuid;
  identity.stats_epoch = 7;
  identity.catalog_epoch = 3;
  identity.transaction_visibility_epoch = visibility_epoch;
  identity.freshness = opt::OptimizerStatsFreshnessState::kFresh;
  identity.source = opt::StatisticSource::kCatalogExact;
  identity.confidence = opt::CostConfidence::kHigh;
  return identity;
}

opt::TableCardinalityStats CanonicalTableStats(const Fixture& fixture) {
  opt::TableCardinalityStats stats;
  stats.identity = FreshIdentity(fixture.table_uuid,
                                 fixture.table_uuid + ":table-statistics",
                                 fixture.context.local_transaction_id);
  stats.row_count = 128;
  stats.visible_row_count = 128;
  stats.page_count = 8;
  stats.average_row_bytes = 96;
  return stats;
}

opt::IndexStats CanonicalIndexStats(const Fixture& fixture) {
  opt::IndexStats stats;
  stats.identity = FreshIdentity(fixture.index_uuid,
                                 fixture.index_uuid + ":index-statistics",
                                 fixture.context.local_transaction_id);
  stats.index_uuid = fixture.index_uuid;
  stats.relation_uuid = fixture.table_uuid;
  stats.index_family = "btree";
  stats.key_column_uuids = {fixture.table_uuid + ":id"};
  stats.covered_column_uuids = {fixture.table_uuid + ":id",
                                fixture.table_uuid + ":note"};
  stats.unique = true;
  stats.covering = true;
  stats.height = 2;
  stats.leaf_pages = 4;
  stats.distinct_keys = 128;
  stats.clustering_factor = 0.95;
  stats.fragmentation_ratio = 0.0;
  stats.visibility_coverage = 1.0;
  stats.predicate_coverage = 1.0;
  stats.equality_lookup_supported = true;
  stats.ordered_range_supported = true;
  stats.route_benchmark_clean = true;
  return stats;
}

opt::AccessPathPlanningRequest CanonicalRequest(const Fixture& fixture,
                                                std::string predicate_kind) {
  opt::AccessPathPlanningRequest request;
  request.relation_uuid = fixture.table_uuid;
  request.predicate_kind = std::move(predicate_kind);
  request.descriptor_digest = "canonical=character";
  request.visibility_proven = true;
  request.grants_proven = true;
  request.base_row_mga_recheck_planned = true;
  request.base_row_security_recheck_planned = true;
  request.index_visibility_native = true;
  request.table_stats = CanonicalTableStats(fixture);
  request.candidate_indexes = {CanonicalIndexStats(fixture)};
  return request;
}

const opt::PlanCandidate* FindCandidate(
    const std::vector<opt::PlanCandidate>& candidates,
    std::string_view candidate_id) {
  const auto candidate = std::find_if(
      candidates.begin(), candidates.end(), [&](const auto& current) {
        return current.candidate_id == candidate_id;
      });
  return candidate == candidates.end() ? nullptr : &*candidate;
}

}  // namespace

int main() {
  auto fixture = MakeFixture();

  const auto legacy_lookup = Plan(fixture, Predicate("column_equals", {"id-7"}));
  RequireOk(legacy_lookup, "CDP-022 legacy equality plan failed");
  Require(legacy_lookup.plan_kind == "table_scan",
          "CDP-022 legacy request invented catalog-backed index statistics");
  Require(HasEvidence(legacy_lookup,
                      "optimizer_access_path_fallback",
                      "stale_or_missing_relation_statistics_scan"),
          "CDP-022 legacy missing-statistics refusal evidence missing");

  const auto equality_candidates = opt::GenerateFullAccessPathCandidates(
      CanonicalRequest(fixture, "scalar_eq"));
  const auto* lookup = FindCandidate(
      equality_candidates, "CAND-OPT-INDEX:" + fixture.index_uuid);
  Require(lookup != nullptr && lookup->cost.selectable,
          "CDP-022 exact catalog statistics did not admit btree lookup");
  Require(lookup->access_kind == plan::PhysicalAccessKind::kScalarBtreeLookup,
          "CDP-022 exact catalog statistics selected the wrong lookup family");
  Require(lookup->estimated_rows == 1,
          "CDP-022 unique lookup cardinality estimate drifted");

  const auto range_candidates = opt::GenerateFullAccessPathCandidates(
      CanonicalRequest(fixture, "scalar_range"));
  const auto* range = FindCandidate(
      range_candidates, "CAND-OPT-INDEX:" + fixture.index_uuid);
  Require(range != nullptr && range->cost.selectable,
          "CDP-022 exact catalog statistics did not admit btree range");
  Require(range->access_kind == plan::PhysicalAccessKind::kScalarBtreeRange,
          "CDP-022 range predicate selected the wrong access family");

  auto covering_request = CanonicalRequest(fixture, "scalar_eq");
  covering_request.projected_column_uuids = {
      fixture.table_uuid + ":id", fixture.table_uuid + ":note"};
  covering_request.covering_payload.physical_payload_proof_present = true;
  covering_request.covering_payload.freshness_proven = true;
  covering_request.covering_payload.redaction_safe = true;
  covering_request.covering_payload.result_contract_proven = true;
  covering_request.covering_payload.index_only_admitted = true;
  const auto covering_candidates =
      opt::GenerateFullAccessPathCandidates(covering_request);
  const auto* covering = FindCandidate(
      covering_candidates, "CAND-OPT-COVERING:" + fixture.index_uuid);
  Require(covering != nullptr && covering->cost.selectable,
          "CDP-022 covered projection lacked an admitted covering candidate");
  Require(covering->access_kind == plan::PhysicalAccessKind::kCoveringIndexScan,
          "CDP-022 covered projection selected the wrong access family");

  const auto stale = Plan(fixture,
                          Predicate("column_equals", {"id-9"}),
                          {"statistics_stale:true"});
  RequireOk(stale, "CDP-022 stale-stat fallback plan failed");
  Require(stale.plan_kind == "table_scan",
          "CDP-022 stale stats did not fail safe to table scan");
  Require(HasEvidence(stale,
                      "optimizer_access_path_fallback",
                      "stale_or_missing_relation_statistics_scan"),
          "CDP-022 stale-stat fallback evidence missing");

  const auto disabled = Plan(fixture,
                             Predicate("column_equals", {"id-11"}),
                             {"optimizer_statistics:disabled"});
  RequireOk(disabled, "CDP-022 disabled statistics fallback plan failed");
  Require(disabled.plan_kind == "table_scan",
          "CDP-022 disabled statistics did not use baseline table scan");
  Require(HasEvidence(disabled,
                      "optimizer_access_path_fallback",
                      "stale_or_missing_relation_statistics_scan"),
          "CDP-022 disabled statistics fallback evidence missing");

  return EXIT_SUCCESS;
}
