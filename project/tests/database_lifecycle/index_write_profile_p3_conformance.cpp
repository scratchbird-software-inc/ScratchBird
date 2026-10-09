#include "mga_relation_store/mga_metadata_record_codec.hpp"
#include "dml/mga_relation_read_view.hpp"
#include "../support/binary_uuid_fixture.hpp"
#include "../support/engine_statement_fixture.hpp"
#include "../support/owned_temp_directory.hpp"
#include "../support/metric_projection_fixture.hpp"
#include "metric_bound_definition.hpp"
#include "database_lifecycle.hpp"
#include "database_lifecycle_test_memory.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "crud_support/crud_store.hpp"
#include "dml/insert_batch.hpp"
#include "dml/update_batch.hpp"
#include "index_family_registry.hpp"
#include "index_management.hpp"
#include "index_metrics.hpp"
#include "metric_registry.hpp"
#include "runtime_platform.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace index_api = scratchbird::core::index;
namespace metrics = scratchbird::core::metrics;
namespace platform = scratchbird::core::platform;

[[noreturn]] void Fail(std::string_view message) {
  throw std::runtime_error(std::string(message));
}

void Require(bool condition, std::string_view message) {
  if (!condition) { Fail(message); }
}

platform::TypedUuid TypedUuid(platform::UuidKind kind, unsigned char salt) {
  platform::TypedUuid uuid;
  uuid.kind = kind;
  uuid.value.bytes[0] = 0x01;
  uuid.value.bytes[1] = 0x9e;
  uuid.value.bytes[6] = 0x70;
  uuid.value.bytes[8] = 0x80;
  uuid.value.bytes[15] = salt;
  return uuid;
}

api::CrudTableRecord Table(const api::EngineRequestContext& context) {
  api::CrudTableRecord table;
  table.creator_tx = context.local_transaction_id;
  table.table_uuid = scratchbird::tests::FixtureUuid(1486, 205);
  table.default_name = "profile_table";
  table.columns.push_back({"id", "canonical=int64"});
  table.columns.push_back({"name", "canonical=character"});
  table.columns.push_back({"payload", "canonical=character"});
  return table;
}

api::CrudIndexRecord Index(const api::EngineRequestContext& context,
                           scratchbird::core::platform::Uuid uuid,
                           std::string column,
                           std::string family,
                           bool unique) {
  api::CrudIndexRecord index;
  index.creator_tx = context.local_transaction_id;
  index.index_uuid = std::move(uuid);
  index.table_uuid = scratchbird::tests::FixtureUuid(1486, 205);
  index.column_name = std::move(column);
  index.family = std::move(family);
  index.profile = api::kCrudIndexProfileRowStoreScalarBtreeV1;
  index.unique = unique;
  if (unique) { index.key_envelopes.push_back("unique"); }
  return index;
}

struct Fixture {
  scratchbird::tests::OwnedTempDirectory temporary;
  api::EngineRequestContext context;
  std::unique_ptr<scratchbird::tests::FixtureEngineSession> session;
  api::CrudTableRecord table;
  api::MgaRelationStorageDescriptor descriptor;
  api::MgaRelationReadView state;

  Fixture() {
    namespace db = scratchbird::storage::database;
    namespace uuid = scratchbird::core::uuid;
    db::DatabaseCreateConfig create;
    create.path = (temporary.path() / "write_profiles.sbdb").string();
    const auto now = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    create.database_uuid = uuid::GenerateEngineIdentityV7(platform::UuidKind::database, now).value;
    create.filespace_uuid = uuid::GenerateEngineIdentityV7(platform::UuidKind::filespace, now).value;
    create.creation_unix_epoch_millis = now;
    create.page_size = 8192;
    scratchbird::tests::ConfigureCredentialedFixtureBootstrap(create);
    const auto created = db::CreateDatabaseFile(create);
    if (!created.ok()) std::cerr << created.diagnostic.diagnostic_code << ':'
                                << created.diagnostic.message_key << '\n';
    Require(created.ok(), "write-profile database creation failed");
    context = scratchbird::tests::BootstrapFixtureOwnerContext(create);
    api::EngineBeginTransactionRequest begin;
    begin.context = context;
    begin.isolation_level = "read_committed";
    const auto begun = api::EngineBeginTransaction(begin);
    Require(begun.ok, "write-profile transaction begin failed");
    context.local_transaction_id = begun.local_transaction_id;
    context.transaction_uuid = begun.transaction_uuid;
    context.snapshot_visible_through_local_transaction_id = begun.snapshot_visible_through_local_transaction_id;
    context.transaction_isolation_level = begun.isolation_level;
    table = Table(context);
    Require(!scratchbird::tests::PublishMgaTableFixture(
                context, table, {"int64", "character", "character"}).error,
            "write-profile catalog publication failed");
    const auto loaded = api::LoadMgaRelationStorageDescriptor(context, table.table_uuid);
    Require(loaded.ok, "write-profile native descriptor read failed");
    descriptor = loaded.descriptor;
    session = std::make_unique<scratchbird::tests::FixtureEngineSession>(context);
  }

  void LoadState() {
    const auto loaded = api::LoadMgaRelationStoreState(context);
    Require(loaded.ok, "write-profile native relation read failed");
    state = api::BuildMgaRelationReadView(loaded.state);
    const auto visible = api::FindVisibleMgaTable(state, table.table_uuid, context.local_transaction_id);
    Require(visible.has_value(), "write-profile published table is not visible");
    table = *visible;
    const auto rows = api::VisibleMgaRowsForContext(state, table.table_uuid, context);
    Require(rows.size() == 1 && rows.front().values.size() == 3,
            "write-profile planning changed the retained seed rows");
    const auto id = std::find_if(rows.front().values.begin(), rows.front().values.end(),
                                 [](const auto& field) { return field.first == "id"; });
    Require(id != rows.front().values.end() && id->second == std::string("\1\0\0\0\0\0\0\0", 8),
            "write-profile fixture did not retain exact native INT64");
  }

  void Rollback() {
    api::EngineRollbackTransactionRequest rollback;
    rollback.context = context;
    Require(api::EngineRollbackTransaction(rollback).ok, "write-profile rollback failed");
  }
};

api::EngineRowValue InputRow(const Fixture& fixture, std::int64_t id, std::string name) {
  api::EngineRowValue row;
  api::EngineTypedValue id_value;
  id_value.descriptor = fixture.descriptor.columns.at(0).value_descriptor;
  id_value.binary_value.resize(8);
  for (unsigned i = 0; i < 8; ++i)
    id_value.binary_value[i] = static_cast<std::uint8_t>(static_cast<std::uint64_t>(id) >> (i * 8));
  id_value.setState(api::EngineValueState::value);
  row.fields.push_back({"id", id_value});
  api::EngineTypedValue name_value;
  name_value.descriptor = fixture.descriptor.columns.at(1).value_descriptor;
  name_value.encoded_value = std::move(name);
  name_value.setState(api::EngineValueState::value);
  row.fields.push_back({"name", name_value});
  return row;
}

auto InsertRequest(const Fixture& fixture) {
  scratchbird::tests::FixtureEngineRequest<api::EngineInsertRowsRequest> request(
      *fixture.session, fixture.context);
  request.target_table.uuid = scratchbird::tests::FixtureUuid(1486, 205);
  request.target_schema.uuid = fixture.context.current_schema_uuid;
  request.estimated_row_count = 2;
  request.input_rows.push_back(InputRow(fixture, 1, "alpha"));
  request.input_rows.push_back(InputRow(fixture, 2, "beta"));
  return request;
}

auto UpdateRequest(const Fixture& fixture) {
  scratchbird::tests::FixtureEngineRequest<api::EngineUpdateRowsRequest> request(
      *fixture.session, fixture.context);
  request.target_table.uuid = scratchbird::tests::FixtureUuid(1486, 205);
  request.update_predicate.predicate_kind = "column_eq";
  request.update_predicate.canonical_predicate_envelope = "name";
  api::EngineTypedValue value;
  value.descriptor = fixture.descriptor.columns.at(1).value_descriptor;
  value.encoded_value = "bravo";
  value.setState(api::EngineValueState::value);
  request.assignments.push_back({"name", value});
  return request;
}

bool HasInsertAction(const api::IndexMaintenancePlan& plan,
                     api::InsertIndexMaintenanceAction action) {
  for (const auto& entry : plan.entries) {
    if (entry.action == action) { return true; }
  }
  return false;
}

bool HasUpdateAction(const api::UpdateIndexMaintenancePlan& plan,
                     api::UpdateIndexMaintenanceAction action) {
  for (const auto& entry : plan.entries) {
    if (entry.action == action) { return true; }
  }
  return false;
}

bool HasStep(const index_api::IndexManagementPlan& plan, std::string_view step) {
  return std::find(plan.steps.begin(), plan.steps.end(), step) != plan.steps.end();
}

void TestIndexFamilyAndManagementMatrix() {
  std::set<std::string> expected = {
      "btree", "unique_btree", "expression", "partial", "covering", "hash",
      "bitmap", "brin_zone", "bloom", "full_text", "gin", "inverted",
      "ngram", "sparse_wand", "spatial", "rtree", "gist", "spgist",
      "vector_exact", "vector_hnsw", "vector_ivf", "columnar_zone",
      "document_path", "graph", "temporary_work", "in_memory",
      "reference_emulated", "advanced_vector_policy_blocked"};

  for (const auto& descriptor : index_api::BuiltinIndexFamilyDescriptors()) {
    Require(expected.erase(descriptor.id) == 1, "unexpected or duplicate index family descriptor");
    Require(descriptor.family_uuid.valid(), "index family UUID is invalid");
    Require(!descriptor.metrics_prefix.empty(), "index family metrics prefix is missing");
    Require(!descriptor.diagnostics_prefix.empty(), "index family diagnostics prefix is missing");
    if (descriptor.family != index_api::IndexFamily::policy_blocked) {
      Require(descriptor.requires_mga_recheck, "index family does not require MGA recheck");
      Require(!descriptor.packet_path.empty(), "index family packet path is missing");
    }
  }
  Require(expected.empty(), "index family registry is missing expected families");
  Require(index_api::IsPolicyBlockedIndexFamily(index_api::IndexFamily::policy_blocked),
          "policy-blocked index family did not fail closed");

  index_api::IndexManagementRequest invalid;
  invalid.operation = index_api::IndexManagementOperation::create;
  invalid.family = index_api::IndexFamily::btree;
  invalid.policy_allows_mutation = true;
  auto plan = index_api::PlanIndexManagementOperation(invalid);
  Require(!plan.ok(), "invalid index management request was admitted");
  Require(plan.diagnostic.diagnostic_code == "SB-INDEX-MANAGEMENT-INVALID-REQUEST",
          "invalid index management diagnostic mismatch");

  index_api::IndexManagementRequest create;
  create.operation = index_api::IndexManagementOperation::create;
  create.family = index_api::IndexFamily::btree;
  create.index_uuid = TypedUuid(platform::UuidKind::object, 0x51);
  plan = index_api::PlanIndexManagementOperation(create);
  Require(!plan.ok(), "index create was admitted without mutation policy");
  Require(plan.diagnostic.diagnostic_code == "SB-INDEX-MANAGEMENT-MUTATION-REFUSED",
          "index mutation refusal diagnostic mismatch");

  create.policy_allows_mutation = true;
  plan = index_api::PlanIndexManagementOperation(create);
  Require(plan.ok(), "index create was not admitted with mutation policy");
  Require(HasStep(plan, "write_catalog_evidence_before_success"),
          "index create omitted catalog evidence step");
  Require(HasStep(plan, "publish_index_resource_epoch"),
          "index create omitted resource epoch publication step");
}

void TestInsertWriteProfiles(const Fixture& fixture) {
  const auto& table = fixture.table;
  const auto& state = fixture.state;
  const std::vector<api::CrudIndexRecord> indexes = {
      Index(fixture.context, scratchbird::tests::FixtureUuid(1486, 202), "name", api::kCrudIndexFamilyBtree, false),
      Index(fixture.context, scratchbird::tests::FixtureUuid(1486, 201), "id", api::kCrudIndexFamilyBtree, true)};

  auto unsafe_request = InsertRequest(fixture);
  unsafe_request.option_envelopes.push_back("feature.secondary_index_delta_ledger=enabled");
  auto unsafe_context = api::BeginInsertBatchContext(unsafe_request, state, table, indexes);
  if (!unsafe_context.accepted) {
    for (const auto& d : unsafe_context.diagnostics) std::cerr << d.code << ':' << d.detail << '\n';
    std::cerr << unsafe_context.fallback_reason << '\n';
  }
  Require(unsafe_context.accepted, "insert context with unsafe delta option should still use exact path");
  Require(!unsafe_context.delta_ledger_policy.enabled,
          "insert delta ledger became enabled without MGA safety proofs");
  Require(!HasInsertAction(unsafe_context.index_plan, api::InsertIndexMaintenanceAction::committed_delta_ledger),
          "insert selected delta ledger without reader/cleanup/recovery proofs");
  Require(HasInsertAction(unsafe_context.index_plan,
                          api::InsertIndexMaintenanceAction::synchronous_exact_probe_then_insert),
          "insert unique index preflight was not selected");

  auto safe_request = InsertRequest(fixture);
  safe_request.option_envelopes.push_back("runtime.deferred_secondary_index=enabled");
  safe_request.option_envelopes.push_back("feature.secondary_index_delta_ledger=enabled");
  safe_request.option_envelopes.push_back("delta_ledger.reader_overlay=enabled");
  safe_request.option_envelopes.push_back("delta_ledger.cleanup_horizon_bound=true");
  safe_request.option_envelopes.push_back("delta_ledger.recovery_classifiable=true");
  safe_request.context.transaction_policy_snapshot_uuid = scratchbird::tests::FixtureUuid(1486, 302);
  safe_request.context.transaction_policy_snapshot_generation = 1;
  auto safe_context = api::BeginInsertBatchContext(safe_request, state, table, indexes);
  Require(safe_context.accepted, "insert context with safe delta proofs was refused");
  Require(safe_context.delta_ledger_policy.enabled, "insert delta ledger proofs were not accepted");
  Require(HasInsertAction(safe_context.index_plan, api::InsertIndexMaintenanceAction::committed_delta_ledger),
          "insert did not select committed delta ledger after proofs");
  Require(safe_context.policy_snapshot_uuid == scratchbird::tests::FixtureUuid(1486, 302), "insert policy snapshot was not bound");

  auto bulk_request = InsertRequest(fixture);
  bulk_request.strict_bulk_load_requested = true;
  auto bulk_context = api::BeginInsertBatchContext(bulk_request, state, table, indexes);
  Require(!bulk_context.accepted, "strict bulk load was admitted without policy");
  Require(bulk_context.fallback_reason == "strict_bulk_load_policy_not_enabled",
          "strict bulk load fallback reason mismatch");

  safe_context.memory_policy.context_budget_bytes = 1;
  const auto memory = api::ValidateInsertBatchMemoryBudget(safe_context, 1024);
  Require(memory.error, "insert memory budget overflow was not refused");
  Require(memory.detail == "dml.insert_rows:insert_batch_memory_budget_exceeded",
          "insert memory budget diagnostic mismatch");
}

void TestUpdateWriteProfiles(const Fixture& fixture) {
  const auto& table = fixture.table;
  const auto& state = fixture.state;
  const std::vector<api::CrudIndexRecord> indexes = {
      Index(fixture.context, scratchbird::tests::FixtureUuid(1486, 202), "name", api::kCrudIndexFamilyBtree, false),
      Index(fixture.context, scratchbird::tests::FixtureUuid(1486, 201), "id", api::kCrudIndexFamilyBtree, true)};

  auto unsafe_request = UpdateRequest(fixture);
  unsafe_request.option_envelopes.push_back("feature.secondary_index_delta_ledger=enabled");
  auto unsafe_context = api::BuildUpdateBatchContext(unsafe_request, state, table, indexes);
  Require(unsafe_context.accepted, "update context with unsafe delta option should still use exact path");
  Require(!unsafe_context.delta_ledger_policy.enabled,
          "update delta ledger became enabled without MGA safety proofs");
  Require(!HasUpdateAction(unsafe_context.index_plan, api::UpdateIndexMaintenanceAction::committed_delta_ledger),
          "update selected delta ledger without reader/cleanup/recovery proofs");
  Require(HasUpdateAction(unsafe_context.index_plan,
                          api::UpdateIndexMaintenanceAction::synchronous_exact_rewrite),
          "update exact rewrite was not selected for affected non-unique index");
  Require(HasUpdateAction(unsafe_context.index_plan,
                          api::UpdateIndexMaintenanceAction::unaffected),
          "update did not leave unaffected unique index alone");

  auto safe_request = UpdateRequest(fixture);
  safe_request.option_envelopes.push_back("runtime.deferred_secondary_index=enabled");
  safe_request.option_envelopes.push_back("feature.secondary_index_delta_ledger=enabled");
  safe_request.option_envelopes.push_back("delta_ledger.reader_overlay=enabled");
  safe_request.option_envelopes.push_back("delta_ledger.cleanup_horizon_bound=true");
  safe_request.option_envelopes.push_back("delta_ledger.recovery_classifiable=true");
  safe_request.option_envelopes.push_back("policy_snapshot_uuid=" + api::MetadataUuidBytes(scratchbird::tests::FixtureUuid(1486, 303)));
  auto safe_context = api::BuildUpdateBatchContext(safe_request, state, table, indexes);
  Require(safe_context.accepted, "update context with safe delta proofs was refused");
  Require(safe_context.delta_ledger_policy.enabled, "update delta ledger proofs were not accepted");
  Require(HasUpdateAction(safe_context.index_plan, api::UpdateIndexMaintenanceAction::committed_delta_ledger),
          "update did not select committed delta ledger after proofs");
  Require(safe_context.policy_snapshot_uuid == scratchbird::tests::FixtureUuid(1486, 303), "update policy snapshot was not bound");

  auto disabled_page_request = UpdateRequest(fixture);
  disabled_page_request.option_envelopes.push_back("feature.page_reservation=disabled");
  const auto disabled_page_context = api::BuildUpdateBatchContext(disabled_page_request, state, table, indexes);
  Require(!disabled_page_context.accepted, "update admitted with disabled page reservation");
  Require(disabled_page_context.fallback_reason == "page_reservation_disabled",
          "update page reservation fallback reason mismatch");

  auto tiny_memory_request = UpdateRequest(fixture);
  tiny_memory_request.option_envelopes.push_back("memory.context_budget_bytes=1");
  auto tiny_memory_context = api::BuildUpdateBatchContext(tiny_memory_request, state, table, indexes);
  const auto memory = api::ValidateUpdateBatchMemoryBudget(tiny_memory_context, 1024);
  Require(memory.error, "update memory budget overflow was not refused");
  Require(memory.detail == "dml.update_rows:update_batch_memory_budget_exceeded",
          "update memory budget diagnostic mismatch");
}

void TestIndexMetrics() {
  index_api::IndexMetricIdentity identity;
  identity.index_uuid = scratchbird::tests::FixtureUuid(1274, 301);
  identity.index_family = "btree";
  identity.semantic_profile_id = "sbsql_v3";
  identity.operation = "lookup";
  identity.result = "ok";
  identity.filespace_uuid = scratchbird::tests::FixtureUuid(1486, 301);

  Require(!index_api::EnsureIndexMetricDescriptors().ok,
          "index publisher invented unbound metric descriptors");
  index_api::IndexLogicalMetricDelta unbound;
  unbound.candidates = 1;
  const auto unbound_result = index_api::PublishIndexLogicalMetrics(identity, unbound);
  Require(!unbound_result.ok && unbound_result.results.size() == 1 &&
              metrics::DefaultMetricRegistry().SnapshotCurrent().empty(),
          "index publisher emitted through a missing descriptor binding");
  const auto definitions = index_api::IndexMetricDescriptorDefinitions();
  Require(definitions.size() == 38 && metrics::DefaultMetricRegistry().SnapshotCurrent().empty(),
          "index schema discovery is incomplete or emitted observations");
  scratchbird::tests::MetricProjectionFixture fixture(
      scratchbird::tests::FixtureUuid(1486, 401), scratchbird::tests::FixtureUuid(1486, 402), 1487);
  const metrics::MetricLabelSet labels = {
      {"index_uuid", identity.index_uuid}, {"index_family", identity.index_family},
      {"semantic_profile", identity.semantic_profile_id}, {"operation", identity.operation},
      {"result", identity.result}, {"reason", identity.reason}, {"page_family", identity.page_family},
      {"filespace_uuid", identity.filespace_uuid}, {"agent_class", identity.agent_class}};
  for (const auto& definition : definitions) fixture.AdmitDefinition(definition, labels);
  Require(index_api::EnsureIndexMetricDescriptors().ok, "bound index metric schema was refused");
  for (const auto& definition : definitions) {
    auto changed = definition;
    changed.producer_owner = "foreign_producer";
    Require(!metrics::ValidateBoundMetricDefinition(metrics::DefaultMetricRegistry(), changed).ok,
            "index metric definition accepted a different producer");
    changed = definition;
    changed.value_type = definition.value_type == metrics::MetricScalarType::uint64
        ? metrics::MetricScalarType::float64 : metrics::MetricScalarType::uint64;
    Require(!metrics::ValidateBoundMetricDefinition(metrics::DefaultMetricRegistry(), changed).ok,
            "index metric definition accepted a different scalar type");
    changed = definition;
    changed.labels.front().value_type = metrics::MetricLabelType::text;
    Require(!metrics::ValidateBoundMetricDefinition(metrics::DefaultMetricRegistry(), changed).ok,
            "index metric definition accepted a textual UUID label");
  }
  const auto account = [&](const index_api::IndexMetricPublishResult& result) {
    Require(result.ok && !result.results.empty(), "index metric publication failed");
    // The first result is schema validation, not an observation.
    for (std::size_t i = 1; i < result.results.size(); ++i) fixture.Produced(result.results[i]);
  };

  index_api::IndexLogicalMetricDelta logical;
  logical.candidates = 10;
  logical.visible = 9;
  logical.rechecks = 10;
  logical.fallback_sorts = 2;
  auto published = index_api::PublishIndexLogicalMetrics(identity, logical);
  Require(published.ok, "index logical metrics publish failed");
  account(published);

  index_api::IndexPhysicalMetricDelta physical;
  physical.pages_read = 2;
  physical.pages_written = 1;
  physical.splits = 4;
  physical.merges = 5;
  physical.depth = 3;
  physical.density_ratio = 0.75;
  published = index_api::PublishIndexPhysicalMetrics(identity, physical);
  Require(published.ok, "index physical metrics publish failed");
  account(published);

  index_api::IndexMaintenanceMetricDelta maintenance;
  maintenance.operations = 1;
  maintenance.verify_failures = 2;
  maintenance.repair_actions = 3;
  maintenance.stale_resources = 4;
  maintenance.quarantine_events = 5;
  maintenance.progress_percent = 100;
  published = index_api::PublishIndexMaintenanceMetrics(identity, maintenance);
  Require(published.ok, "index maintenance metrics publish failed");
  account(published);

  index_api::IndexOptimizerMetricDelta optimizer;
  optimizer.estimate_error_ratio = 1.25;
  optimizer.stale_stats = 2; optimizer.invalidations = 3; optimizer.fallback_refusals = 4;
  account(index_api::PublishIndexOptimizerMetrics(identity, optimizer));
  index_api::IndexReferenceProfileMetricDelta reference;
  reference.profile_hits = 1; reference.profile_refusals = 2; reference.rechecks = 3;
  reference.fallback_sorts = 4; reference.order_proofs = 5;
  reference.catalog_projections = 6; reference.compatibility_diagnostics = 7;
  account(index_api::PublishIndexReferenceProfileMetrics(identity, reference));
  index_api::IndexResidencyMetricDelta residency;
  residency.resident_bytes = (std::uint64_t{1} << 53) + 1;
  residency.hits = 2; residency.misses = 3; residency.evictions = 4;
  residency.pressure_score = 0.25; residency.degraded = 5; residency.refused = 6;
  account(index_api::PublishIndexResidencyMetrics(identity, residency));
  index_api::IndexPageFilespaceMetricDelta pages;
  pages.allocation_requests = std::numeric_limits<std::uint64_t>::max();
  pages.relocation_requests = 2; pages.shrink_ready_bytes = residency.resident_bytes;
  account(index_api::PublishIndexPageFilespaceMetrics(identity, pages));

  bool saw_candidates = false;
  bool saw_depth = false;
  bool saw_maintenance = false;
  for (const auto& value : metrics::DefaultMetricRegistry().SnapshotCurrent()) {
    saw_candidates = saw_candidates || value.family == "sb_index_candidates_total";
    saw_depth = saw_depth || value.family == "sb_index_depth";
    saw_maintenance = saw_maintenance || value.family == "sb_index_maintenance_operations_total";
  }
  Require(saw_candidates, "index candidates metric snapshot missing");
  Require(saw_depth, "index depth metric snapshot missing");
  Require(saw_maintenance, "index maintenance metric snapshot missing");
  const auto current = metrics::DefaultMetricRegistry().SnapshotCurrent();
  Require(current.size() == 38, "index metric families did not all emit");
  const auto exact = [&](const char* family, metrics::MetricScalar expected) {
    const auto found = std::find_if(current.begin(), current.end(),
                                   [&](const auto& value) { return value.family == family; });
    Require(found != current.end() && found->value == expected,
            "index metric changed its native scalar type or exact value");
  };
  exact("sb_index_candidates_total", std::uint64_t{10});
  exact("sb_index_visible_candidates_total", std::uint64_t{9});
  exact("sb_index_rechecks_total", std::uint64_t{10});
  exact("sb_index_fallback_sorts_total", std::uint64_t{2});
  exact("sb_index_pages_read_total", std::uint64_t{2});
  exact("sb_index_pages_written_total", std::uint64_t{1});
  exact("sb_index_splits_observed_total", std::uint64_t{4});
  exact("sb_index_merges_observed_total", std::uint64_t{5});
  exact("sb_index_depth", std::uint64_t{3});
  exact("sb_index_density_ratio", 0.75);
  exact("sb_index_fragmentation_ratio", 0.0);
  exact("sb_index_maintenance_operations_total", std::uint64_t{1});
  exact("sb_index_verify_failures_total", std::uint64_t{2});
  exact("sb_index_repair_actions_total", std::uint64_t{3});
  exact("sb_index_stale_resources_total", std::uint64_t{4});
  exact("sb_index_quarantine_events_total", std::uint64_t{5});
  exact("sb_index_maintenance_progress_percent", 100.0);
  exact("sb_index_optimizer_estimate_error_ratio", 1.25);
  exact("sb_index_optimizer_stale_stats_total", std::uint64_t{2});
  exact("sb_index_optimizer_invalidations_total", std::uint64_t{3});
  exact("sb_index_optimizer_fallback_refusals_total", std::uint64_t{4});
  exact("sb_index_reference_profile_hits_total", std::uint64_t{1});
  exact("sb_index_reference_profile_refusals_total", std::uint64_t{2});
  exact("sb_index_reference_rechecks_total", std::uint64_t{3});
  exact("sb_index_reference_fallback_sorts_total", std::uint64_t{4});
  exact("sb_index_reference_order_proofs_total", std::uint64_t{5});
  exact("sb_index_reference_catalog_projections_total", std::uint64_t{6});
  exact("sb_index_reference_compatibility_diagnostics_total", std::uint64_t{7});
  exact("sb_index_residency_pressure_score", 0.25);
  exact("sb_index_resident_bytes", residency.resident_bytes);
  exact("sb_index_residency_hits_total", std::uint64_t{2});
  exact("sb_index_residency_misses_total", std::uint64_t{3});
  exact("sb_index_residency_evictions_total", std::uint64_t{4});
  exact("sb_index_residency_degraded_total", std::uint64_t{5});
  exact("sb_index_residency_refused_total", std::uint64_t{6});
  exact("sb_index_filespace_shrink_ready_bytes", residency.resident_bytes);
  exact("sb_index_page_allocation_requests_total", std::numeric_limits<std::uint64_t>::max());
  exact("sb_index_page_relocation_requests_total", std::uint64_t{2});
  fixture.Seal();
  fixture.VerifyAdmissionRefusals("sb_index_candidates_total", labels, std::uint64_t{1});
  auto zero = index_api::IndexLogicalMetricDelta{};
  const auto no_events = index_api::PublishIndexLogicalMetrics(identity, zero);
  Require(no_events.ok && no_events.results.size() == 1, "zero delta invented an event sample");
  // Overflow may not replace the existing sample. A later successful gauge
  // emission is reported separately; this API is not an atomic sample batch.
  pages.allocation_requests = 1;
  pages.relocation_requests = 0;
  const auto overflow = index_api::PublishIndexPageFilespaceMetrics(identity, pages);
  Require(!overflow.ok && overflow.results.size() == 3 && !overflow.results[1].ok && overflow.results[2].ok,
          "overflow counter or subsequent gauge result was misreported");
  const auto after_overflow = metrics::DefaultMetricRegistry().SnapshotCurrent();
  const auto retained = std::find_if(after_overflow.begin(), after_overflow.end(), [](const auto& value) {
    return value.family == "sb_index_page_allocation_requests_total";
  });
  Require(retained != after_overflow.end() &&
              retained->value == metrics::MetricScalar{std::numeric_limits<std::uint64_t>::max()},
          "overflow changed the existing native index counter");
  fixture.Produced(overflow.results[2]);
  fixture.Seal();
  optimizer.estimate_error_ratio = std::numeric_limits<double>::infinity();
  optimizer.stale_stats = optimizer.invalidations = optimizer.fallback_refusals = 0;
  const auto nonfinite = index_api::PublishIndexOptimizerMetrics(identity, optimizer);
  Require(!nonfinite.ok && nonfinite.results.size() == 2 && !nonfinite.results[1].ok,
          "nonfinite index metric was accepted");
  fixture.VerifyReadOnly();
  fixture.VerifyAndDrain();
}

}  // namespace

int main() {
  try {
    scratchbird::tests::database_lifecycle::ConfigureLifecycleMemoryFixture("p3-native-write-profiles");
    TestIndexFamilyAndManagementMatrix();
    Fixture fixture;
    {
      auto seed = InsertRequest(fixture);
      seed.input_rows.resize(1);
      seed.estimated_row_count = 1;
      const auto inserted = api::EngineInsertRows(seed);
      for (const auto& d : inserted.diagnostics) if (d.error) std::cerr << d.code << ':' << d.detail << '\n';
      Require(inserted.ok && inserted.inserted_count == 1, "write-profile native seed insert failed");
    }
    fixture.LoadState();
    TestInsertWriteProfiles(fixture);
    TestUpdateWriteProfiles(fixture);
    fixture.LoadState();
    TestIndexMetrics();
    fixture.Rollback();
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
