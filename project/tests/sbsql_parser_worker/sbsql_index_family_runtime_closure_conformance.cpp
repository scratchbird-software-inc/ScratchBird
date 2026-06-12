// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "crud_support/crud_store.hpp"
#include "database_lifecycle.hpp"
#include "ddl/create_api.hpp"
#include "dml/delete_api.hpp"
#include "dml/insert_api.hpp"
#include "dml/select_api.hpp"
#include "dml/update_api.hpp"
#include "index_access_method.hpp"
#include "index_management.hpp"
#include "index_ordered_access.hpp"
#include "index_statistics_lifecycle.hpp"
#include "index_verification.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "sblr_dispatch.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace index_api = scratchbird::core::index;
namespace platform = scratchbird::core::platform;
namespace sblr = scratchbird::engine::sblr;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) { Fail(message); }
}

std::uint64_t CurrentUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

platform::TypedUuid GeneratedUuid(UuidKind kind, std::uint64_t salt) {
  const auto generated =
      uuid::GenerateEngineIdentityV7(kind, 1779811300000ull + salt);
  Require(generated.ok(), "index family runtime UUID generation failed");
  return generated.value;
}

std::string GeneratedUuidText(UuidKind kind, std::uint64_t salt) {
  return uuid::UuidToString(GeneratedUuid(kind, salt).value);
}

const std::string& SchemaUuid() {
  static const std::string value = GeneratedUuidText(UuidKind::object, 101);
  return value;
}

const std::string& TableUuid() {
  static const std::string value = GeneratedUuidText(UuidKind::object, 102);
  return value;
}

const std::string& BtreeIndexUuid() {
  static const std::string value = GeneratedUuidText(UuidKind::object, 201);
  return value;
}

const std::string& BitmapIndexUuid() {
  static const std::string value = GeneratedUuidText(UuidKind::object, 202);
  return value;
}

const std::string& ExpressionIndexUuid() {
  static const std::string value = GeneratedUuidText(UuidKind::object, 203);
  return value;
}

const std::string& PartialIndexUuid() {
  static const std::string value = GeneratedUuidText(UuidKind::object, 204);
  return value;
}

std::filesystem::path TestDatabasePath() {
  return std::filesystem::temp_directory_path() /
         ("sbsql_index_family_runtime_closure_" +
          std::to_string(CurrentUnixMillis()) + ".sbdb");
}

void RemoveDatabaseArtifacts(const std::filesystem::path& path) {
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  for (const auto suffix : {".sb.api_events",
                            ".sb.crud_events",
                            ".sb.domain_events",
                            ".sb.name_events",
                            ".sb.transaction_inventory",
                            ".sb.mga_row_versions",
                            ".sb.mga_relation_metadata",
                            ".sb.mga_index_entries",
                            ".sb.mga_relation_descriptors",
                            ".sb.mga_large_values",
                            ".sb.mga_savepoints",
                            ".dirty.manifest",
                            ".recovery.evidence",
                            ".sb.owner.lock"}) {
    std::filesystem::remove(path.string() + suffix, ignored);
  }
}

std::string CreateMinimalDatabase(const std::filesystem::path& path) {
  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid = GeneratedUuid(UuidKind::database, 1);
  create.filespace_uuid = GeneratedUuid(UuidKind::filespace, 2);
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1779811300002;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "index family runtime database create failed");
  return uuid::UuidToString(create.database_uuid.value);
}

api::EngineRequestContext EngineContext(const std::filesystem::path& path,
                                        const std::string& database_uuid) {
  api::EngineRequestContext context;
  context.request_id = "sbsql-index-family-runtime-closure";
  context.database_path = path.string();
  context.database_uuid.canonical = database_uuid;
  context.session_uuid.canonical = GeneratedUuidText(UuidKind::object, 11);
  context.principal_uuid.canonical = GeneratedUuidText(UuidKind::principal, 12);
  context.current_schema_uuid.canonical = SchemaUuid();
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.trace_tags.push_back("right:CATALOG_MUTATE");
  context.trace_tags.push_back("CBQ-005");
  return context;
}

api::EngineRequestContext BeginTransaction(api::EngineRequestContext context) {
  api::EngineBeginTransactionRequest request;
  request.context = context;
  request.isolation_level = "read_committed";
  const auto result = api::EngineBeginTransaction(request);
  Require(result.ok, "transaction.begin failed for index family runtime closure");
  context.local_transaction_id = result.local_transaction_id;
  context.transaction_uuid = result.transaction_uuid;
  return context;
}

api::EngineLocalizedName Name(std::string text) {
  return {"en", "primary", "", std::move(text), true};
}

api::EngineDescriptor Descriptor(std::string type) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = std::move(type);
  descriptor.encoded_descriptor = "canonical_type=" + descriptor.canonical_type_name;
  return descriptor;
}

api::EngineTypedValue TypedValue(std::string type, std::string value) {
  api::EngineTypedValue typed;
  typed.descriptor = Descriptor(std::move(type));
  typed.encoded_value = std::move(value);
  return typed;
}

api::EngineColumnDefinition Column(std::string name, std::string type, std::uint32_t ordinal) {
  api::EngineColumnDefinition column;
  column.requested_column_uuid.canonical =
      GeneratedUuidText(UuidKind::object, 300 + ordinal);
  column.names.push_back(Name(std::move(name)));
  column.descriptor = Descriptor(std::move(type));
  column.ordinal = ordinal;
  column.nullable = true;
  return column;
}

api::EngineRowValue Row(std::string row_uuid,
                        std::string id,
                        std::string name,
                        std::string status) {
  api::EngineRowValue row;
  row.requested_row_uuid.canonical = std::move(row_uuid);
  row.fields.push_back({"id", TypedValue("int64", std::move(id))});
  row.fields.push_back({"name", TypedValue("text", std::move(name))});
  row.fields.push_back({"status", TypedValue("text", std::move(status))});
  return row;
}

api::EngineIndexDefinition Index(std::string uuid_text,
                                 std::string name,
                                 std::string index_kind,
                                 std::vector<std::string> keys) {
  api::EngineIndexDefinition index;
  index.requested_index_uuid.canonical = std::move(uuid_text);
  index.names.push_back(Name(std::move(name)));
  index.index_kind = std::move(index_kind);
  index.key_envelopes = std::move(keys);
  return index;
}

bool HasEvidenceKind(const api::EngineApiResult& result, std::string_view kind) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind) { return true; }
  }
  return false;
}

bool EvidenceContains(const api::EngineApiResult& result,
                      std::string_view kind,
                      std::string_view fragment) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind &&
        evidence.evidence_id.find(fragment) != std::string::npos) {
      return true;
    }
  }
  return false;
}

std::string FirstDiagnosticDetail(const api::EngineApiResult& result) {
  return result.diagnostics.empty() ? std::string{} : result.diagnostics.front().detail;
}

void RequireOk(const api::EngineApiResult& result, std::string_view message) {
  if (!result.ok) {
    std::cerr << FirstDiagnosticDetail(result) << '\n';
  }
  Require(result.ok, message);
}

void CreateTable(const api::EngineRequestContext& context) {
  api::EngineCreateTableRequest request;
  request.context = context;
  request.target_schema.uuid.canonical = SchemaUuid();
  request.target_schema.object_kind = "schema";
  request.requested_table_uuid.canonical = TableUuid();
  request.table_names.push_back(Name("cbq005_index_family_table"));
  request.table_columns.push_back(Column("id", "int64", 0));
  request.table_columns.push_back(Column("name", "text", 1));
  request.table_columns.push_back(Column("status", "text", 2));
  auto result = api::EngineCreateTable(request);
  RequireOk(result, "create table failed for index family runtime closure");
}

void InsertRows(const api::EngineRequestContext& context,
                std::vector<api::EngineRowValue> rows) {
  api::EngineInsertRowsRequest request;
  request.context = context;
  request.target_table.uuid.canonical = TableUuid();
  request.target_table.object_kind = "table";
  request.require_generated_row_uuid = false;
  request.estimated_row_count = rows.size();
  request.input_rows = std::move(rows);
  const auto result = api::EngineInsertRows(request);
  RequireOk(result, "insert rows failed for index family runtime closure");
  Require(result.inserted_count == request.input_rows.size(),
          "insert row count mismatch");
}

api::EngineCreateIndexResult CreateIndex(const api::EngineRequestContext& context,
                                         api::EngineIndexDefinition index) {
  api::EngineCreateIndexRequest request;
  request.context = context;
  request.target_object.uuid.canonical = TableUuid();
  request.target_object.object_kind = "table";
  request.indexes.push_back(std::move(index));
  return api::EngineCreateIndex(request);
}

api::EngineSelectRowsResult SelectRows(const api::EngineRequestContext& context,
                                       api::EnginePredicateEnvelope predicate) {
  api::EngineSelectRowsRequest request;
  request.context = context;
  request.source_object.uuid.canonical = TableUuid();
  request.source_object.object_kind = "table";
  request.select_predicate = std::move(predicate);
  return api::EngineSelectRows(request);
}

api::EnginePredicateEnvelope Predicate(std::string kind,
                                       std::string envelope,
                                       std::vector<api::EngineTypedValue> values = {}) {
  api::EnginePredicateEnvelope predicate;
  predicate.predicate_kind = std::move(kind);
  predicate.canonical_predicate_envelope = std::move(envelope);
  predicate.bound_values = std::move(values);
  return predicate;
}

api::CrudState LoadMgaCrudState(const api::EngineRequestContext& context) {
  const auto loaded = api::LoadMgaRelationStoreState(context);
  if (!loaded.ok) {
    std::cerr << loaded.diagnostic.detail << '\n';
  }
  Require(loaded.ok, "MGA relation store load failed");
  return api::BuildCrudCompatibilityStateFromMga(loaded.state);
}

std::size_t CountIndexEntries(const api::CrudState& state,
                              std::string_view index_uuid,
                              std::string_view key = {}) {
  return static_cast<std::size_t>(std::count_if(
      state.index_entries.begin(),
      state.index_entries.end(),
      [&](const api::CrudIndexEntryRecord& entry) {
        return entry.index_uuid == index_uuid && (key.empty() || entry.key_value == key);
      }));
}

const api::CrudIndexRecord& RequireIndexRecord(const api::CrudState& state,
                                               std::string_view index_uuid,
                                               std::string_view family) {
  for (const auto& index : state.indexes) {
    if (index.index_uuid == index_uuid) {
      Require(index.family == family, "persisted index family mismatch");
      return index;
    }
  }
  Fail("persisted index metadata missing");
}

index_api::IndexCandidate Candidate(std::string key, unsigned char salt) {
  index_api::IndexCandidate candidate;
  candidate.key.encoded_key = std::move(key);
  candidate.key.lossy = false;
  candidate.key.requires_recheck = true;
  candidate.locator.table_uuid = GeneratedUuid(UuidKind::object, 400);
  candidate.locator.row_uuid = GeneratedUuid(UuidKind::row, 500 + salt);
  candidate.locator.version_uuid = GeneratedUuid(UuidKind::row, 600 + salt);
  candidate.locator.local_transaction_id = 1;
  candidate.mga_visible = true;
  candidate.predicate_exact = true;
  candidate.security_visible = true;
  return candidate;
}

index_api::IndexKeyEncodingComponent KeyComponent(std::string value, unsigned ordinal = 0) {
  index_api::IndexKeyEncodingComponent component;
  component.ordinal = ordinal;
  if (ordinal == 0) {
    static const platform::TypedUuid kTypeDescriptorUuid =
        GeneratedUuid(UuidKind::object, 700);
    static const platform::TypedUuid kCollationUuid =
        GeneratedUuid(UuidKind::object, 800);
    component.type_descriptor_uuid = kTypeDescriptorUuid;
    component.collation_uuid = kCollationUuid;
  } else {
    component.type_descriptor_uuid =
        GeneratedUuid(UuidKind::object, 700 + ordinal);
    component.collation_uuid = GeneratedUuid(UuidKind::object, 800 + ordinal);
  }
  component.payload.assign(value.begin(), value.end());
  return component;
}

index_api::IndexPostingEntry Posting(unsigned char salt, std::uint64_t visible_from = 1) {
  index_api::IndexPostingEntry entry;
  entry.locator.table_uuid = GeneratedUuid(UuidKind::object, 400);
  entry.locator.row_uuid = GeneratedUuid(UuidKind::row, 900 + salt);
  entry.locator.version_uuid = GeneratedUuid(UuidKind::row, 1000 + salt);
  entry.locator.local_transaction_id = visible_from;
  entry.visible_from_transaction_id = visible_from;
  return entry;
}

void AddOptionOperand(sblr::SblrOperationEnvelope* envelope,
                      std::string name,
                      std::string value) {
  sblr::SblrOperand operand;
  operand.type = "option";
  operand.name = std::move(name);
  operand.value = std::move(value);
  envelope->operands.push_back(std::move(operand));
}

void RequireOrderedBtreeRuntimePlans() {
  index_api::OrderedAccessRequest access;
  access.family = index_api::IndexFamily::btree;
  access.intent = index_api::OrderedAccessIntent::range;
  access.lower_bound.kind = index_api::OrderedBoundKind::inclusive;
  access.lower_bound.components.push_back(KeyComponent("2"));
  access.upper_bound.kind = index_api::OrderedBoundKind::inclusive;
  access.upper_bound.components.push_back(KeyComponent("9"));
  access.require_total_order_proof = false;
  const auto range = index_api::PlanOrderedBTreeAccess(access);
  Require(range.ok() && range.shape == index_api::OrderedAccessShape::range &&
              range.decision == index_api::OrderedAccessDecision::admitted_exact &&
              range.capabilities.supports_scan &&
              range.capabilities.can_satisfy_order,
          "ordered B-tree contract plan did not expose physical scan capability");

  index_api::OrderedBuildRequest build;
  build.index_uuid = GeneratedUuid(UuidKind::object, 1100);
  build.family = index_api::IndexFamily::btree;
  build.tuple_count_estimate = 128;
  build.page_budget = 8;
  build.input_presorted = true;
  build.order_proof_valid = true;
  build.rebuild = true;
  build.policy_allows_mutation = true;
  const auto build_plan = index_api::PlanOrderedBulkBuild(build);
  Require(build_plan.ok() &&
              build_plan.mode == index_api::OrderedBuildMode::rebuild_presorted &&
              build_plan.publishes_new_root &&
              build_plan.commit_atomic,
          "ordered B-tree rebuild plan did not consume completed runtime capability");

  index_api::OrderedDuplicateLifecycleRequest lifecycle;
  lifecycle.posting_list.index_uuid = GeneratedUuid(UuidKind::object, 1101);
  lifecycle.posting_list.encoded_key = {'k'};
  lifecycle.incoming = Posting(0x63);
  auto duplicate = index_api::DecideOrderedDuplicateLifecycle(lifecycle);
  Require(duplicate.ok() &&
              duplicate.action == index_api::OrderedDuplicateLifecycleAction::create_posting_list &&
              duplicate.posting_result.posting_list.entries.size() == 1,
          "ordered posting-list create failed");

  lifecycle.posting_list = duplicate.posting_result.posting_list;
  lifecycle.incoming = Posting(0x64);
  duplicate = index_api::DecideOrderedDuplicateLifecycle(lifecycle);
  Require(duplicate.ok() &&
              duplicate.action == index_api::OrderedDuplicateLifecycleAction::append_duplicate &&
              duplicate.posting_result.posting_list.entries.size() == 2,
          "ordered duplicate append failed");

  lifecycle.posting_list = duplicate.posting_result.posting_list;
  lifecycle.incoming = lifecycle.posting_list.entries.front();
  lifecycle.incoming.visible_until_transaction_id = 5;
  lifecycle.insert = false;
  lifecycle.delete_existing = true;
  duplicate = index_api::DecideOrderedDuplicateLifecycle(lifecycle);
  Require(duplicate.ok() &&
              duplicate.action == index_api::OrderedDuplicateLifecycleAction::mark_dead,
          "ordered duplicate delete marker failed");

  lifecycle.posting_list = duplicate.posting_result.posting_list;
  lifecycle.delete_existing = false;
  lifecycle.purge_dead = true;
  lifecycle.oldest_active_transaction_id = 10;
  duplicate = index_api::DecideOrderedDuplicateLifecycle(lifecycle);
  Require(duplicate.ok() &&
              duplicate.action == index_api::OrderedDuplicateLifecycleAction::purge_dead &&
              duplicate.posting_result.posting_list.entries.size() == 1,
          "ordered duplicate purge below horizon failed");

  index_api::OrderedOverlayRequest expression;
  expression.family = index_api::IndexFamily::expression;
  expression.overlay = index_api::OrderedOverlayKind::expression;
  expression.expression_deterministic = true;
  expression.expression_resource_epoch_valid = true;
  auto overlay = index_api::DecideOrderedOverlayEligibility(expression);
  Require(overlay.ok() && overlay.use_btree_physical &&
              overlay.eligibility == index_api::OrderedOverlayEligibility::eligible_exact,
          "ordered expression overlay eligibility failed");

  index_api::OrderedOverlayRequest partial;
  partial.family = index_api::IndexFamily::partial;
  partial.overlay = index_api::OrderedOverlayKind::partial;
  partial.predicate_immutable = true;
  partial.predicate_security_safe = true;
  partial.predicate_exact = false;
  partial.can_recheck_base_row = true;
  overlay = index_api::DecideOrderedOverlayEligibility(partial);
  Require(overlay.ok() && overlay.use_btree_physical && overlay.requires_recheck,
          "ordered partial overlay eligibility failed");
}

void RequireCoreIndexManagementAndStatistics() {
  RequireOrderedBtreeRuntimePlans();

  for (const auto family : {index_api::IndexFamily::btree,
                            index_api::IndexFamily::expression,
                            index_api::IndexFamily::partial}) {
    const auto* state =
        index_api::FindBuiltinIndexFamilyPhysicalCapabilityState(family);
    Require(state != nullptr, "index physical capability state missing");
    const auto caps = index_api::CapabilitiesForFamily(family);
    Require(state->runtime_available && state->physically_complete(),
            "ordered index family runtime capability was not promoted");
    Require(caps.supports_insert && caps.supports_update && caps.supports_delete,
            "ordered index family mutation capabilities missing after promotion");
    Require(caps.supports_scan && caps.supports_verify && caps.supports_rebuild,
            "ordered index family scan/verify/rebuild capabilities missing after promotion");
    Require(caps.requires_mga_recheck && caps.requires_security_recheck,
            "index family authority recheck flags missing");
    constexpr std::string_view kGenericMaintenanceUnsupported =
        "SB-INDEX-MAINTENANCE-GENERIC-FAMILY-UNSUPPORTED";

    index_api::IndexMaintenanceRequest verify;
    verify.index_uuid =
        GeneratedUuid(UuidKind::object, 1200 + static_cast<int>(family));
    verify.family = family;
    verify.operation = index_api::IndexMaintenanceOperation::verify;
    verify.page_budget = 1;
    const auto verify_plan = index_api::PlanIndexMaintenance(verify);
    Require(verify_plan.ok(),
            "ordered index verify maintenance did not consume completed capability");

    index_api::IndexMaintenanceRequest rebuild = verify;
    rebuild.operation = index_api::IndexMaintenanceOperation::rebuild;
    rebuild.policy_allows_mutation = true;
    const auto rebuild_plan = index_api::PlanIndexMaintenance(rebuild);
    Require(rebuild_plan.ok(),
            "ordered index rebuild maintenance did not consume completed capability");

    index_api::IndexMaintenanceRequest read_only_rebuild = rebuild;
    read_only_rebuild.read_only_database = true;
    const auto refused = index_api::PlanIndexMaintenance(read_only_rebuild);
    Require(!refused.ok() &&
                refused.diagnostic.diagnostic_code ==
                    "SB-INDEX-MAINTENANCE-MUTATION-REFUSED",
            "read-only index maintenance must refuse mutation after capability admission");

    index_api::IndexManagementRequest management;
    management.operation = index_api::IndexManagementOperation::rebalance;
    management.index_uuid = rebuild.index_uuid;
    management.family = family;
    management.policy_allows_mutation = true;
    const auto management_plan = index_api::PlanIndexManagementOperation(management);
    Require(management_plan.ok(),
            "ordered index management rebalance did not consume completed capability");
  }

  for (const auto family : {index_api::IndexFamily::bitmap}) {
    const auto* state =
        index_api::FindBuiltinIndexFamilyPhysicalCapabilityState(family);
    Require(state != nullptr, "index physical capability state missing");
    const auto caps = index_api::CapabilitiesForFamily(family);
    Require(!caps.supports_insert && !caps.supports_update && !caps.supports_delete,
            "index family mutation capabilities must not be inferred from registry acceptance");
    Require(!caps.supports_scan && !caps.supports_verify && !caps.supports_rebuild,
            "index family scan/verify/rebuild capabilities must not be inferred from registry acceptance");
    Require(caps.requires_mga_recheck && caps.requires_security_recheck,
            "index family authority recheck flags missing");
    constexpr std::string_view kGenericMaintenanceUnsupported =
        "SB-INDEX-MAINTENANCE-GENERIC-FAMILY-UNSUPPORTED";

    index_api::IndexMaintenanceRequest verify;
    verify.index_uuid =
        GeneratedUuid(UuidKind::object, 1300 + static_cast<int>(family));
    verify.family = family;
    verify.operation = index_api::IndexMaintenanceOperation::verify;
    verify.page_budget = 1;
    const auto verify_plan = index_api::PlanIndexMaintenance(verify);
    Require(!verify_plan.ok() &&
                verify_plan.diagnostic.diagnostic_code ==
                    kGenericMaintenanceUnsupported,
            "index verify maintenance must refuse specialized family through the generic surface");

    index_api::IndexMaintenanceRequest rebuild = verify;
    rebuild.operation = index_api::IndexMaintenanceOperation::rebuild;
    rebuild.policy_allows_mutation = true;
    const auto rebuild_plan = index_api::PlanIndexMaintenance(rebuild);
    Require(!rebuild_plan.ok() &&
                rebuild_plan.diagnostic.diagnostic_code ==
                    kGenericMaintenanceUnsupported,
            "index rebuild maintenance must refuse specialized family through the generic surface");

    index_api::IndexMaintenanceRequest read_only_rebuild = rebuild;
    read_only_rebuild.read_only_database = true;
    const auto refused = index_api::PlanIndexMaintenance(read_only_rebuild);
    Require(!refused.ok() &&
                refused.diagnostic.diagnostic_code ==
                    kGenericMaintenanceUnsupported,
            "read-only index maintenance must preserve generic-surface refusal before mutation admission");

    index_api::IndexManagementRequest management;
    management.operation = index_api::IndexManagementOperation::rebalance;
    management.index_uuid = rebuild.index_uuid;
    management.family = family;
    management.policy_allows_mutation = true;
    const auto management_plan = index_api::PlanIndexManagementOperation(management);
    Require(!management_plan.ok() &&
                management_plan.diagnostic.diagnostic_code ==
                    kGenericMaintenanceUnsupported,
            "index management rebalance must refuse specialized family through the generic surface");
  }

  index_api::IndexVerificationRequest verification;
  verification.expected_from_table = {Candidate("1", 0x41), Candidate("2", 0x42)};
  verification.observed_from_index = verification.expected_from_table;
  auto verified = index_api::VerifyIndexCandidateSet(verification);
  Require(verified.ok() && !verified.rebuild_required, "matching index candidate set failed verification");
  verification.observed_from_index.pop_back();
  verified = index_api::VerifyIndexCandidateSet(verification);
  Require(!verified.ok() && verified.rebuild_required &&
              verified.diagnostic.diagnostic_code == "SB-INDEX-VERIFY-MISMATCH",
          "index verification mismatch diagnostic drifted");

  index_api::IndexLifecycleDescriptor descriptor;
  descriptor.index_uuid = GeneratedUuid(UuidKind::object, 1400);
  descriptor.table_uuid = GeneratedUuid(UuidKind::object, 1401);
  descriptor.family = index_api::IndexFamily::bitmap;
  descriptor.lifecycle_state = index_api::IndexStatisticsLifecycleState::ready;
  descriptor.catalog_generation_id = 7;
  descriptor.index_generation = 3;
  descriptor.resource_epochs.resource_epoch = 11;
  descriptor.resource_epochs.charset_epoch = 12;
  descriptor.resource_epochs.collation_epoch = 13;
  descriptor.catalog_profile.physical_profile_key = "bitmap";
  descriptor.catalog_profile.catalog_profile_authoritative = true;
  descriptor.catalog_profile.catalog_profile_supports_mga_snapshot_visibility = true;
  descriptor.catalog_profile.catalog_profile_supports_exact_lookup = true;
  index_api::IndexStatisticsLifecycleRequest refresh;
  refresh.operation = index_api::IndexStatisticsLifecycleOperation::refresh_statistics;
  refresh.descriptor = descriptor;
  refresh.local_transaction_id = 99;
  refresh.snapshot_visible_through_transaction_id = 99;
  refresh.refresh.observed_row_count = 3;
  refresh.refresh.observed_distinct_key_count = 2;
  refresh.refresh.observed_leaf_page_count = 1;
  refresh.refresh.full_scan_evidence = true;
  const auto refreshed = index_api::RefreshIndexStatistics(refresh);
  Require(refreshed.ok() && refreshed.statistics_refreshed &&
              refreshed.statistics.physical_profile_key == "bitmap",
          "bitmap statistics refresh failed");
  const auto usable = index_api::EvaluateIndexStatisticsForUse(
      refreshed.descriptor,
      refreshed.statistics,
      descriptor.resource_epochs,
      index_api::IndexStatisticsFreshnessPolicy::require_current,
      99);
  Require(usable.ok() && usable.index_scan_allowed,
          "current bitmap statistics were not admitted for scan");
}

void RequireSblrCreateIndexRoute(const api::EngineRequestContext& context) {
  auto envelope = sblr::MakeSblrEnvelope("ddl.create_index",
                                         "SBLR_DDL_CREATE_INDEX",
                                         "trace.cbq005.sblr.create_bitmap_index");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.contains_sql_text = false;
  AddOptionOperand(&envelope, "index_target_uuid", TableUuid());
  AddOptionOperand(&envelope, "index_target_kind", "table");
  AddOptionOperand(&envelope, "index_object_uuid", BitmapIndexUuid());
  AddOptionOperand(&envelope, "index_name", "status_bitmap_idx");
  AddOptionOperand(&envelope, "index_profile", "bitmap");
  AddOptionOperand(&envelope, "index_key_envelope", "status");
  const sblr::SblrDispatchRequest request{context, envelope, api::EngineApiRequest{}};
  const auto result = sblr::DispatchSblrOperation(request);
  if (!result.accepted || !result.api_result.ok) {
    for (const auto& diagnostic : result.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
    }
    for (const auto& diagnostic : result.api_result.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    }
  }
  Require(result.envelope_validated && result.accepted && result.api_result.ok,
          "SBLR ddl.create_index route failed");
  Require(result.api_result.operation_id == "ddl.create_index",
          "SBLR ddl.create_index operation id drifted");
  Require(EvidenceContains(result.api_result, "index_family", "bitmap"),
          "SBLR create index route did not persist bitmap family evidence");
}

void RequireRuntimeIndexEntriesAndScans(const api::EngineRequestContext& context) {
  InsertRows(context, {Row(GeneratedUuidText(UuidKind::row, 1501), "1", "Alpha", "active"),
                       Row(GeneratedUuidText(UuidKind::row, 1502), "2", "Beta", "inactive")});

  auto result = CreateIndex(context, Index(BtreeIndexUuid(),
                                           "id_btree_idx",
                                           "btree",
                                           {"id"}));
  const auto* btree_state =
      index_api::FindBuiltinIndexFamilyPhysicalCapabilityState(
          index_api::IndexFamily::btree);
  Require(btree_state != nullptr, "B-tree capability state missing before create-index route");
  if (!btree_state->runtime_available || !btree_state->physically_complete()) {
    Require(!result.ok && !result.diagnostics.empty() &&
                result.diagnostics.front().code == btree_state->blocker_diagnostic_code,
            "B-tree create-index route must fail closed with exact capability blocker until physical implementation is complete");
    return;
  }
  RequireOk(result, "btree create index failed");
  RequireSblrCreateIndexRoute(context);
  result = CreateIndex(context, Index(ExpressionIndexUuid(),
                                      "lower_name_idx",
                                      "expression",
                                      {"lower:name"}));
  RequireOk(result, "expression create index failed");
  result = CreateIndex(context, Index(PartialIndexUuid(),
                                      "active_id_partial_idx",
                                      "partial",
                                      {"id", "where_eq:status=active"}));
  RequireOk(result, "partial create index failed");

  InsertRows(context, {Row(GeneratedUuidText(UuidKind::row, 1503), "3", "Gamma", "active")});

  api::EngineUpdateRowsRequest update;
  update.context = context;
  update.target_table.uuid.canonical = TableUuid();
  update.target_table.object_kind = "table";
  update.update_predicate = Predicate("column_equals", "id", {TypedValue("int64", "2")});
  update.assignments.push_back({"name", TypedValue("text", "Bravo")});
  update.assignments.push_back({"status", TypedValue("text", "active")});
  const auto updated = api::EngineUpdateRows(update);
  RequireOk(updated, "indexed row update failed");
  Require(updated.updated_count == 1, "indexed row update count mismatch");

  api::EngineDeleteRowsRequest delete_request;
  delete_request.context = context;
  delete_request.target_table.uuid.canonical = TableUuid();
  delete_request.target_table.object_kind = "table";
  delete_request.delete_predicate = Predicate("column_equals", "id", {TypedValue("int64", "1")});
  const auto deleted = api::EngineDeleteRows(delete_request);
  RequireOk(deleted, "indexed row delete failed");
  Require(deleted.deleted_count == 1, "indexed row delete count mismatch");

  const auto state = LoadMgaCrudState(context);
  RequireIndexRecord(state, BtreeIndexUuid(), "btree");
  RequireIndexRecord(state, BitmapIndexUuid(), "bitmap");
  RequireIndexRecord(state, ExpressionIndexUuid(), "expression");
  const auto& partial = RequireIndexRecord(state, PartialIndexUuid(), "partial");
  Require(partial.predicate_kind == "where_eq" && partial.predicate_column == "status" &&
              partial.predicate_value == "active",
          "partial index predicate metadata was not persisted");
  Require(CountIndexEntries(state, BtreeIndexUuid(), "3") == 1,
          "btree insert maintenance did not persist new key");
  Require(CountIndexEntries(state, BitmapIndexUuid(), "active") >= 3,
          "bitmap mutation path did not persist active-key entries");
  Require(CountIndexEntries(state, ExpressionIndexUuid(), "bravo") == 1,
          "expression update maintenance did not persist lower-case expression key");
  Require(CountIndexEntries(state, PartialIndexUuid(), "2") == 1,
          "partial update maintenance did not add row after predicate became true");
  Require(CountIndexEntries(state, PartialIndexUuid(), "1") == 1,
          "partial create-index maintenance did not capture existing predicate-matching row");

  auto selected = SelectRows(context, Predicate("column_range", "id", {TypedValue("int64", "2"),
                                                                        TypedValue("int64", "3")}));
  RequireOk(selected, "btree range select failed");
  Require(selected.visible_count == 2 && HasEvidenceKind(selected, "index_lookup") &&
              EvidenceContains(selected, "index_lookup", "index_family=btree"),
          "btree range select did not use index evidence");

  selected = SelectRows(context, Predicate("column_in_list", "status", {TypedValue("text", "active")}));
  RequireOk(selected, "bitmap in-list select failed");
  Require(selected.visible_count == 2 && HasEvidenceKind(selected, "index_lookup") &&
              EvidenceContains(selected, "index_lookup", "index_family=bitmap"),
          "bitmap select did not use index evidence");

  selected = SelectRows(context, Predicate("expression_equals", "lower:name", {TypedValue("text", "bravo")}));
  RequireOk(selected, "expression index select failed");
  Require(selected.visible_count == 1 && HasEvidenceKind(selected, "index_lookup") &&
              EvidenceContains(selected, "index_lookup", "index_family=expression"),
          "expression index select did not use index evidence");

  selected = SelectRows(context, Predicate("partial_index_probe", "status=active"));
  RequireOk(selected, "partial index probe failed");
  Require(selected.visible_count == 2 && HasEvidenceKind(selected, "index_lookup") &&
              EvidenceContains(selected, "index_lookup", "index_family=partial"),
          "partial index probe did not use index evidence");

  selected = SelectRows(context, Predicate("column_equals", "id", {TypedValue("int64", "1")}));
  RequireOk(selected, "deleted-row btree lookup failed");
  Require(selected.visible_count == 0 && HasEvidenceKind(selected, "index_lookup"),
          "delete tombstone did not suppress stale btree index candidate");

  const auto stats = api::EstimateMgaRelationStatistics(context, TableUuid(), true);
  Require(stats.ok && stats.statistics.relation_found &&
              stats.statistics.visible_row_estimate == 2 &&
              stats.statistics.index_store_bytes != 0 &&
              stats.statistics.table_size_bytes > stats.statistics.row_store_bytes,
          "MGA relation/index statistics did not include persisted index storage");
}

}  // namespace

int main() {
  RequireCoreIndexManagementAndStatistics();

  const auto path = TestDatabasePath();
  RemoveDatabaseArtifacts(path);
  const auto database_uuid = CreateMinimalDatabase(path);
  const auto context = BeginTransaction(EngineContext(path, database_uuid));
  CreateTable(context);
  RequireRuntimeIndexEntriesAndScans(context);
  return EXIT_SUCCESS;
}
