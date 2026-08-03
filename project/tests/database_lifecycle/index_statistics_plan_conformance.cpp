// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog_index_profile.hpp"
#include "index_statistics_lifecycle.hpp"
#include "query/optimizer_plan_lifecycle.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace catalog = scratchbird::core::catalog;
namespace exec = scratchbird::engine::executor;
namespace index_api = scratchbird::core::index;
namespace plan_api = scratchbird::engine::internal_api;
namespace platform = scratchbird::core::platform;

static_assert(
    !std::is_aggregate_v<plan_api::EngineOptimizerPlanStatementUseReceipt>);
static_assert(!std::is_default_constructible_v<
              plan_api::EngineOptimizerPlanStatementUseReceipt>);

inline constexpr std::string_view kIndexUuid =
    "019e0000-0000-0000-0000-000000000031";
inline constexpr std::string_view kRelationUuid =
    "019e0000-0000-0000-0000-000000000032";
inline constexpr std::string_view kPlanUuid =
    "019e0000-0000-7000-8000-000000000101";
inline constexpr std::string_view kBoundSblrTreeUuid =
    "019e0000-0000-7000-8000-000000000102";
inline constexpr std::string_view kCatalogEpochUuid =
    "019e0000-0000-7000-8000-000000000103";
inline constexpr std::string_view kSecurityContextUuid =
    "019e0000-0000-7000-8000-000000000104";
inline constexpr std::string_view kCapabilitySnapshotUuid =
    "019e0000-0000-7000-8000-000000000105";
inline constexpr std::string_view kResourceSnapshotUuid =
    "019e0000-0000-7000-8000-000000000106";
inline constexpr std::string_view kStatisticsSnapshotUuid =
    "019e0000-0000-7000-8000-000000000107";
inline constexpr std::string_view kRouteSnapshotUuid =
    "019e0000-0000-7000-8000-000000000108";
inline constexpr std::string_view kQueryFingerprint =
    "query:catalog-index-lookup:v2";

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

template <typename TResult>
std::string DiagnosticCode(const TResult& result) {
  return result.diagnostics.empty() ? std::string{} : result.diagnostics.front().code;
}

template <typename TResult>
void RequireOk(const TResult& result, std::string_view message) {
  if (!result.ok) {
    std::cerr << DiagnosticCode(result) << '\n';
  }
  Require(result.ok, message);
}

template <typename TResult>
void RequireDiagnostic(const TResult& result,
                       std::string_view expected,
                       std::string_view message) {
  Require(!result.ok, message);
  if (DiagnosticCode(result) != expected) {
    std::cerr << "expected=" << expected << " actual=" << DiagnosticCode(result) << '\n';
  }
  Require(DiagnosticCode(result) == expected, message);
}

bool HasEvidence(const plan_api::EngineApiResult& result,
                 const std::string_view kind,
                 const std::string_view id) {
  return std::ranges::any_of(result.evidence, [&](const auto& evidence) {
    return evidence.evidence_kind == kind && evidence.evidence_id == id;
  });
}

bool HasRowField(const plan_api::EngineApiResult& result,
                 const std::string_view field_name,
                 const std::string_view encoded_value) {
  return std::ranges::any_of(result.result_shape.rows, [&](const auto& row) {
    return std::ranges::any_of(row.fields, [&](const auto& field) {
      return field.first == field_name &&
             field.second.encoded_value == encoded_value;
    });
  });
}

std::string DecodeJournalField(const std::string_view line,
                               const std::string_view field_name) {
  const std::string marker = "\t" + std::string(field_name) + "=";
  const auto field = line.find(marker);
  if (field == std::string_view::npos) return {};
  const auto begin = field + marker.size();
  const auto end = line.find('\t', begin);
  const auto encoded = line.substr(
      begin, end == std::string_view::npos ? line.size() - begin : end - begin);
  if (encoded.empty() || (encoded.size() % 2) != 0) return {};
  std::string decoded;
  decoded.reserve(encoded.size() / 2);
  const auto HexNibble = [](const char ch) -> int {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
  };
  for (std::size_t index = 0; index < encoded.size(); index += 2) {
    const int high = HexNibble(encoded[index]);
    const int low = HexNibble(encoded[index + 1]);
    if (high < 0 || low < 0) return {};
    decoded.push_back(static_cast<char>((high << 4) | low));
  }
  return decoded;
}

std::string LowercaseHex(const std::string_view value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string encoded;
  encoded.reserve(value.size() * 2);
  for (const unsigned char ch : value) {
    encoded.push_back(kHex[(ch >> 4) & 0x0f]);
    encoded.push_back(kHex[ch & 0x0f]);
  }
  return encoded;
}

void WriteJournal(const std::filesystem::path& database_path,
                  const std::string_view contents) {
  std::ofstream out(database_path.string() + ".sb.optimizer_plan_events",
                    std::ios::binary | std::ios::trunc);
  out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  out.flush();
  Require(static_cast<bool>(out), "DBLC-013V corruption journal write failed");
}

void RewriteJournalField(const std::filesystem::path& database_path,
                         const std::string_view field_name,
                         const std::string_view replacement_value) {
  const auto journal_path =
      database_path.string() + ".sb.optimizer_plan_events";
  std::ifstream in(journal_path, std::ios::binary);
  std::string bytes((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
  Require(in.is_open(), "DBLC-013V correction journal read failed");
  const std::string marker = "\t" + std::string(field_name) + "=";
  const auto field = bytes.find(marker);
  Require(field != std::string::npos,
          "DBLC-013V corrected journal field missing");
  const auto begin = field + marker.size();
  const auto end = bytes.find_first_of("\t\n", begin);
  Require(end != std::string::npos,
          "DBLC-013V corrected journal field was unterminated");
  bytes.replace(begin, end - begin, LowercaseHex(replacement_value));
  WriteJournal(database_path, bytes);
}

void RequireCoreOk(const index_api::IndexStatisticsLifecycleResult& result,
                   std::string_view message) {
  if (!result.ok()) {
    std::cerr << result.diagnostic.diagnostic_code << '\n';
  }
  Require(result.ok(), message);
}

void RequireCoreDiagnostic(const index_api::IndexStatisticsLifecycleResult& result,
                           std::string_view expected,
                           std::string_view message) {
  Require(!result.ok(), message);
  if (result.diagnostic.diagnostic_code != expected) {
    std::cerr << "expected=" << expected
              << " actual=" << result.diagnostic.diagnostic_code << '\n';
  }
  Require(result.diagnostic.diagnostic_code == expected, message);
}

std::uint64_t CurrentUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::filesystem::path TestPath(std::string_view label) {
  return std::filesystem::temp_directory_path() /
         ("sb_dblc_013v_" + std::string(label) + "_" +
          std::to_string(CurrentUnixMillis()) + ".sbdb");
}

exec::PhysicalMgaStatementContext StatementContext(
    std::string statement_uuid,
    std::string transaction_uuid,
    std::string snapshot_uuid,
    std::string metadata_snapshot_uuid,
    const std::uint64_t owning_local_transaction_id,
    const std::uint64_t visible_committed_high_watermark) {
  exec::PhysicalMgaStatementContext context;
  context.statement_uuid = std::move(statement_uuid);
  context.owning_transaction_uuid = std::move(transaction_uuid);
  context.statement_snapshot_uuid = std::move(snapshot_uuid);
  context.statement_metadata_snapshot_uuid =
      std::move(metadata_snapshot_uuid);
  context.owning_local_transaction_id = owning_local_transaction_id;
  context.visible_committed_high_watermark =
      visible_committed_high_watermark;
  context.oldest_active_transaction_id = owning_local_transaction_id;
  context.oldest_interesting_transaction_id = 1;
  context.oldest_snapshot_transaction_id = 1;
  context.retention_horizon_transaction_id = 1;
  context.active_excluded_local_transaction_ids = {
      owning_local_transaction_id, owning_local_transaction_id + 2};
  context.in_doubt_excluded_local_transaction_ids = {
      owning_local_transaction_id + 1};
  context.snapshot_kind = "statement_stable";
  context.publication_inventory_next_local_transaction_id =
      owning_local_transaction_id + 10;
  context.inventory_authoritative = true;
  context.complete = true;
  context.current = true;
  return context;
}

exec::TypedPhysicalNodeDag SelectedDag(
    const exec::PhysicalMgaStatementContext& statement,
    const std::uint64_t statistics_generation) {
  exec::TypedPhysicalNodeDag dag;
  dag.abi_version = 2;
  dag.selected_plan_uuid = std::string(kPlanUuid);
  dag.root_physical_node_id = 1;
  dag.local_transaction_id = statement.owning_local_transaction_id;
  dag.statement_snapshot_id = statement.visible_committed_high_watermark;
  dag.mga_statement_context = statement;
  dag.bound_sblr_tree_uuid = std::string(kBoundSblrTreeUuid);
  dag.catalog_epoch_uuid = std::string(kCatalogEpochUuid);
  dag.security_context_uuid = std::string(kSecurityContextUuid);
  dag.capability_snapshot_uuid = std::string(kCapabilitySnapshotUuid);
  dag.resource_snapshot_uuid = std::string(kResourceSnapshotUuid);
  dag.statistics_snapshot_uuid = std::string(kStatisticsSnapshotUuid);
  dag.route_snapshot_uuid = std::string(kRouteSnapshotUuid);
  dag.catalog_generation = 7;
  dag.security_epoch = 8;
  dag.policy_epoch = 9;
  dag.resource_epoch = 3;
  dag.statistics_generation = statistics_generation;
  dag.route_epoch = 10;
  dag.route_generation = 11;
  dag.memory_budget_bytes = 4096;
  dag.optimizer_published = true;
  dag.immutable_node_identity_validated = true;
  dag.capability_validated_before_access = true;
  dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       dag.bound_sblr_tree_uuid},
      {exec::PhysicalAdmissionStage::kCatalogEpoch, dag.catalog_epoch_uuid},
      {exec::PhysicalAdmissionStage::kSecurity, dag.security_context_uuid},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       statement.statement_snapshot_uuid},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       dag.capability_snapshot_uuid},
      {exec::PhysicalAdmissionStage::kResource, dag.resource_snapshot_uuid},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       dag.statistics_snapshot_uuid},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       dag.route_snapshot_uuid}};

  exec::PhysicalNodeRecord node;
  node.physical_node_id = 1;
  node.relational_node_id = 1;
  node.node_kind = exec::PhysicalNodeKind::kValues;
  node.implementation_id = "values.materialize.v1";
  node.output_descriptor_ids = {1};
  node.causal_counter_id = 1;
  node.selected_alternative_uuid =
      "019e0000-0000-7000-8000-000000000109";
  node.executor_capability_uuid =
      "019e0000-0000-7000-8000-00000000010a";
  node.executor_capability_abi_version = 1;
  node.cost_vector_uuid =
      "019e0000-0000-7000-8000-00000000010b";
  node.memory_bytes_required = 64;
  node.engine_capability_validated = true;
  node.mga_statement_context = statement;
  dag.nodes.push_back(std::move(node));
  return dag;
}

plan_api::EngineRequestContext EngineContext(
    const std::filesystem::path& path,
    const exec::PhysicalMgaStatementContext& statement,
    const exec::TypedPhysicalNodeDag& dag) {
  plan_api::EngineRequestContext context;
  context.database_path = path.string();
  context.database_uuid.canonical =
      "019e0000-0000-7000-8000-000000000201";
  context.principal_uuid.canonical =
      "019e0000-0000-7000-8000-000000000202";
  context.session_uuid.canonical =
      "019e0000-0000-7000-8000-000000000203";
  context.transaction_uuid.canonical = statement.owning_transaction_uuid;
  context.statement_uuid.canonical = statement.statement_uuid;
  context.statement_snapshot_uuid.canonical =
      statement.statement_snapshot_uuid;
  context.statement_metadata_snapshot_uuid.canonical =
      statement.statement_metadata_snapshot_uuid;
  context.catalog_epoch_uuid.canonical = dag.catalog_epoch_uuid;
  context.local_transaction_id = statement.owning_local_transaction_id;
  context.request_id = "request-" + statement.statement_uuid;
  context.snapshot_visible_through_local_transaction_id =
      statement.visible_committed_high_watermark;
  context.statement_metadata_snapshot_visible_through_local_transaction_id =
      statement.visible_committed_high_watermark;
  context.statement_metadata_snapshot_engine_owned = true;
  context.statement_metadata_snapshot_active_excluded_local_transaction_ids =
      statement.active_excluded_local_transaction_ids;
  context.statement_metadata_snapshot_in_doubt_excluded_local_transaction_ids =
      statement.in_doubt_excluded_local_transaction_ids;
  context.security_context_present = true;
  context.catalog_generation_id = dag.catalog_generation;
  context.security_epoch = dag.security_epoch;
  context.resource_epoch = dag.resource_epoch;
  context.optimizer_capability_snapshot_uuid.canonical =
      dag.capability_snapshot_uuid;
  context.optimizer_resource_snapshot_uuid.canonical =
      dag.resource_snapshot_uuid;
  context.optimizer_route_snapshot_uuid.canonical = dag.route_snapshot_uuid;
  context.optimizer_route_epoch = dag.route_epoch;
  context.optimizer_route_generation = dag.route_generation;
  context.optimizer_memory_budget_bytes = dag.memory_budget_bytes;
  return context;
}

struct ResolverState {
  exec::PhysicalMgaStatementContext current;
  exec::PhysicalMgaStatementContext stale_current;
  std::size_t calls = 0;
  std::size_t stale_on_call = 0;
};

exec::CanonicalExecutionMgaAuthority Authority(
    const exec::TypedPhysicalNodeDag& dag,
    const std::shared_ptr<ResolverState>& state,
    const std::size_t stale_on_call = 0) {
  state->current = dag.mga_statement_context;
  state->stale_current = dag.mga_statement_context;
  state->stale_current.statement_snapshot_uuid =
      "019e0000-0000-7000-8000-000000000fff";
  state->calls = 0;
  state->stale_on_call = stale_on_call;

  exec::CanonicalExecutionMgaAuthority authority;
  authority.statement_context = dag.mga_statement_context;
  authority.origin =
      exec::CanonicalMgaAuthorityOrigin::kEngineTransactionInventory;
  authority.resolve_current = [state] {
    ++state->calls;
    exec::CanonicalMgaCurrentResolution resolution;
    resolution.statement_context =
        state->stale_on_call != 0 && state->calls >= state->stale_on_call
            ? state->stale_current
            : state->current;
    return resolution;
  };
  return authority;
}

plan_api::EngineOptimizerCachePlanRequest CacheRequest(
    const std::filesystem::path& path,
    const index_api::IndexLifecycleDescriptor& descriptor,
    const index_api::IndexStatisticsSnapshot& statistics,
    const exec::TypedPhysicalNodeDag& dag,
    const exec::CanonicalExecutionMgaAuthority& authority) {
  plan_api::EngineOptimizerCachePlanRequest request;
  request.context = EngineContext(path, dag.mga_statement_context, dag);
  request.plan_uuid = std::string(kPlanUuid);
  request.query_fingerprint = std::string(kQueryFingerprint);
  request.relation_uuid = std::string(kRelationUuid);
  request.index_uuid = std::string(kIndexUuid);
  request.plan_shape_digest = "shape:index-point-lookup:v2";
  request.mga_authority = authority;
  request.selected_physical_dag = dag;
  request.selected_catalog_epoch_uuid = std::string(kCatalogEpochUuid);
  request.object_dependency_uuids = {std::string(kIndexUuid),
                                    std::string(kRelationUuid)};
  request.index_descriptor = descriptor;
  request.statistics = statistics;
  return request;
}

plan_api::EngineOptimizerValidateCachedPlanRequest ValidateRequest(
    const std::filesystem::path& path,
    const index_api::IndexStatisticsSnapshot& statistics,
    const exec::TypedPhysicalNodeDag& dag,
    const exec::CanonicalExecutionMgaAuthority& authority) {
  plan_api::EngineOptimizerValidateCachedPlanRequest request;
  request.context = EngineContext(path, dag.mga_statement_context, dag);
  request.plan_uuid = std::string(kPlanUuid);
  request.query_fingerprint = std::string(kQueryFingerprint);
  request.index_uuid = std::string(kIndexUuid);
  request.mga_authority = authority;
  request.selected_physical_dag = dag;
  request.selected_catalog_epoch_uuid = std::string(kCatalogEpochUuid);
  request.object_dependency_uuids = {std::string(kIndexUuid),
                                    std::string(kRelationUuid)};
  request.current_index_generation = statistics.index_generation;
  request.current_statistics_generation = statistics.statistics_generation;
  request.current_catalog_generation_id = statistics.catalog_generation_id;
  request.current_resource_epochs = statistics.resource_epochs;
  return request;
}

void RequireNoValidateExposure(
    const plan_api::EngineOptimizerValidateCachedPlanResult& result,
    const std::string_view message) {
  Require(!result.ok && !result.cache_hit && !result.metadata_cache_hit &&
              !result.statement_use_admitted &&
              !result.statement_use_receipt && result.plan_cache_epoch == 0 &&
              result.result_shape.result_kind.empty() &&
              result.result_shape.rows.empty() && result.evidence.empty() &&
              result.primary_object.uuid.canonical.empty() &&
              result.primary_object.object_kind.empty() &&
              result.entry.event_uuid.empty() && result.entry.plan_uuid.empty() &&
              result.entry.query_fingerprint.empty() &&
              result.entry.relation_uuid.empty() &&
              result.entry.index_uuid.empty() &&
              result.entry.dependencies.bound_sblr_tree_uuid.empty() &&
              result.entry.dependencies.object_dependency_uuids.empty(),
          message);
}

void RequireNoCacheExposure(
    const plan_api::EngineOptimizerCachePlanResult& result,
    const std::string_view message) {
  Require(!result.ok && !result.publication_receipt &&
              result.plan_cache_epoch == 0 &&
              result.result_shape.result_kind.empty() &&
              result.result_shape.rows.empty() && result.evidence.empty() &&
              result.primary_object.uuid.canonical.empty() &&
              result.primary_object.object_kind.empty() &&
              result.entry.event_uuid.empty() && result.entry.plan_uuid.empty() &&
              result.entry.query_fingerprint.empty() &&
              result.entry.relation_uuid.empty() &&
              result.entry.index_uuid.empty() &&
              result.entry.dependencies.bound_sblr_tree_uuid.empty() &&
              result.entry.dependencies.object_dependency_uuids.empty(),
          message);
}

void RequireNoUseExposure(
    const plan_api::EngineOptimizerPlanUseValidationResult& result,
    const std::string_view message) {
  Require(!result.ok && !result.executable_receipt && result.evidence.empty(),
          message);
}

void Cleanup(const std::filesystem::path& path) {
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  std::filesystem::remove(path.string() + ".sb.optimizer_plan_events", ignored);
}

platform::TypedUuid MakeTypedUuid(platform::UuidKind kind, platform::byte salt) {
  platform::TypedUuid uuid;
  uuid.kind = kind;
  uuid.value.bytes[0] = 0x01;
  uuid.value.bytes[1] = 0x9e;
  uuid.value.bytes[15] = salt;
  return uuid;
}

index_api::IndexResourceEpochVector Epochs(std::uint64_t resource,
                                           std::uint64_t charset,
                                           std::uint64_t collation) {
  index_api::IndexResourceEpochVector epochs;
  epochs.resource_epoch = resource;
  epochs.charset_epoch = charset;
  epochs.collation_epoch = collation;
  return epochs;
}

index_api::IndexLifecycleDescriptor BaseDescriptor() {
  index_api::IndexLifecycleDescriptor descriptor;
  descriptor.index_uuid = MakeTypedUuid(platform::UuidKind::object, 0x31);
  descriptor.table_uuid = MakeTypedUuid(platform::UuidKind::object, 0x32);
  descriptor.family = index_api::IndexFamily::btree;
  descriptor.lifecycle_state = index_api::IndexStatisticsLifecycleState::absent;
  descriptor.catalog_generation_id = 7;
  descriptor.metadata_epoch = 1;
  descriptor.resource_epochs = Epochs(3, 4, 5);
  descriptor.catalog_profile.physical_profile_key = "sys_catalog_index_definitions_uuid_hash";
  descriptor.catalog_profile.catalog_table_path = "sys.catalog.index_definitions";
  descriptor.catalog_profile.catalog_profile_authoritative = true;
  descriptor.catalog_profile.catalog_profile_supports_mga_snapshot_visibility = true;
  descriptor.catalog_profile.catalog_profile_supports_exact_lookup = true;
  return descriptor;
}

const catalog::CatalogPhysicalIndexProfile& CatalogProfile() {
  const auto* profile =
      catalog::FindCatalogIndexProfile("sys_catalog_index_definitions_uuid_hash");
  Require(profile != nullptr, "DBLC-013V catalog physical profile missing");
  return *profile;
}

index_api::IndexLifecycleDescriptor ReadyBuiltDescriptor() {
  auto descriptor = BaseDescriptor();
  index_api::IndexStatisticsLifecycleRequest build;
  build.operation = index_api::IndexStatisticsLifecycleOperation::build;
  build.descriptor = descriptor;
  build.local_transaction_id = 10;
  build.snapshot_visible_through_transaction_id = 10;
  build.catalog_evidence_written = true;
  build.physical_build_complete = true;
  build.validation_complete = true;
  build.optimizer_plan_invalidation_requested = true;
  const auto built = index_api::PlanIndexStatisticsLifecycle(build, &CatalogProfile());
  RequireCoreOk(built, "DBLC-013V index build lifecycle failed");
  Require(built.descriptor.lifecycle_state == index_api::IndexStatisticsLifecycleState::ready,
          "DBLC-013V build did not publish ready state");
  Require(built.descriptor.index_generation == 11,
          "DBLC-013V build did not advance index generation");
  Require(built.optimizer_plan_cache_invalidation_required,
          "DBLC-013V build did not require plan invalidation");
  return built.descriptor;
}

index_api::IndexStatisticsSnapshot RefreshStatistics(
    const index_api::IndexLifecycleDescriptor& descriptor,
    std::uint64_t tx) {
  index_api::IndexStatisticsLifecycleRequest refresh;
  refresh.operation = index_api::IndexStatisticsLifecycleOperation::refresh_statistics;
  refresh.descriptor = descriptor;
  refresh.local_transaction_id = tx;
  refresh.snapshot_visible_through_transaction_id = tx;
  refresh.refresh.observed_row_count = 1000;
  refresh.refresh.observed_distinct_key_count = 100;
  refresh.refresh.observed_leaf_page_count = 16;
  refresh.refresh.observed_retained_version_count = 1040;
  refresh.refresh.full_scan_evidence = true;
  const auto refreshed = index_api::PlanIndexStatisticsLifecycle(refresh, &CatalogProfile());
  RequireCoreOk(refreshed, "DBLC-013V statistics refresh failed");
  Require(refreshed.statistics_refreshed, "DBLC-013V statistics refresh flag missing");
  Require(refreshed.statistics.statistics_generation == descriptor.index_generation + 1,
          "DBLC-013V statistics generation did not advance");
  Require(refreshed.statistics.refreshed_by_transaction_id == tx,
          "DBLC-013V statistics refresh was not MGA transaction visible");
  Require(refreshed.statistics.mga_visible,
          "DBLC-013V refreshed statistics were not visible to snapshot");
  Require(refreshed.optimizer_plan_cache_invalidation_required,
          "DBLC-013V statistics refresh did not invalidate plans");
  return refreshed.statistics;
}

void TestIndexBuildDropRebuildTransitions() {
  auto descriptor = ReadyBuiltDescriptor();

  index_api::IndexStatisticsLifecycleRequest drop;
  drop.operation = index_api::IndexStatisticsLifecycleOperation::drop;
  drop.descriptor = descriptor;
  drop.local_transaction_id = 20;
  drop.catalog_evidence_written = true;
  const auto dropped = index_api::PlanIndexStatisticsLifecycle(drop, &CatalogProfile());
  RequireCoreOk(dropped, "DBLC-013V index drop lifecycle failed");
  Require(dropped.descriptor.lifecycle_state == index_api::IndexStatisticsLifecycleState::dropped,
          "DBLC-013V drop did not publish dropped state");
  Require(!dropped.index_scan_allowed, "DBLC-013V dropped index remained scannable");
  Require(dropped.optimizer_plan_cache_invalidation_required,
          "DBLC-013V drop did not invalidate plans");

  index_api::IndexStatisticsLifecycleRequest rebuild;
  rebuild.operation = index_api::IndexStatisticsLifecycleOperation::rebuild;
  rebuild.descriptor = dropped.descriptor;
  rebuild.local_transaction_id = 30;
  rebuild.catalog_evidence_written = true;
  rebuild.physical_build_complete = true;
  rebuild.validation_complete = true;
  const auto rebuilt = index_api::PlanIndexStatisticsLifecycle(rebuild, &CatalogProfile());
  RequireCoreOk(rebuilt, "DBLC-013V index rebuild lifecycle failed");
  Require(rebuilt.descriptor.lifecycle_state == index_api::IndexStatisticsLifecycleState::ready,
          "DBLC-013V rebuild did not republish ready state");
  Require(rebuilt.descriptor.index_generation > dropped.descriptor.index_generation,
          "DBLC-013V rebuild did not advance index generation");
  Require(rebuilt.index_scan_allowed, "DBLC-013V rebuilt index was not scannable");
}

void TestStatisticsEpochsAndStaleRefusal() {
  const auto descriptor = ReadyBuiltDescriptor();
  const auto statistics = RefreshStatistics(descriptor, 40);

  const auto admitted = index_api::EvaluateIndexStatisticsForUse(
      descriptor,
      statistics,
      descriptor.resource_epochs,
      index_api::IndexStatisticsFreshnessPolicy::require_current,
      40);
  RequireCoreOk(admitted, "DBLC-013V current statistics were refused");
  Require(admitted.index_scan_allowed, "DBLC-013V current statistics did not admit scan");

  auto stale_statistics = statistics;
  stale_statistics.stale = true;
  stale_statistics.current = false;
  const auto stale = index_api::EvaluateIndexStatisticsForUse(
      descriptor,
      stale_statistics,
      descriptor.resource_epochs,
      index_api::IndexStatisticsFreshnessPolicy::refuse_stale,
      40);
  RequireCoreDiagnostic(stale,
                        index_api::kIndexStatisticsDiagnosticStaleRefused,
                        "DBLC-013V stale statistics were not refused");

  auto changed_epochs = descriptor.resource_epochs;
  ++changed_epochs.collation_epoch;
  const auto epoch_refusal = index_api::EvaluateIndexStatisticsForUse(
      descriptor,
      statistics,
      changed_epochs,
      index_api::IndexStatisticsFreshnessPolicy::require_current,
      40);
  RequireCoreDiagnostic(epoch_refusal,
                        index_api::kIndexStatisticsDiagnosticResourceEpochMismatch,
                        "DBLC-013V collation epoch mismatch was not refused");
}

void TestCatalogProfileCoupling() {
  auto descriptor = BaseDescriptor();
  const auto coupled =
      index_api::ValidateCatalogPhysicalIndexProfileCoupling(descriptor, &CatalogProfile());
  RequireCoreOk(coupled, "DBLC-013V catalog physical profile coupling failed");
  Require(coupled.descriptor.catalog_profile.catalog_profile_authoritative,
          "DBLC-013V catalog profile was not authoritative");
  Require(coupled.descriptor.catalog_profile.catalog_profile_supports_mga_snapshot_visibility,
          "DBLC-013V catalog profile did not preserve MGA visibility");

  descriptor.catalog_profile.physical_profile_key = "sys_catalog_object_identity_generation_btree";
  const auto mismatched =
      index_api::ValidateCatalogPhysicalIndexProfileCoupling(descriptor, &CatalogProfile());
  RequireCoreDiagnostic(mismatched,
                        index_api::kIndexStatisticsDiagnosticCatalogProfileMismatch,
                        "DBLC-013V mismatched catalog profile was accepted");
}

void TestCrashRecoveryClassification() {
  const auto descriptor = ReadyBuiltDescriptor();
  index_api::IndexRecoveryEvidence interrupted_build;
  interrupted_build.durable_state = index_api::IndexStatisticsLifecycleState::building;
  interrupted_build.catalog_record_present = true;
  interrupted_build.physical_root_present = true;
  interrupted_build.build_manifest_complete = false;
  const auto build_classification =
      index_api::ClassifyIndexLifecycleRecovery(descriptor, interrupted_build, 50);
  RequireCoreOk(build_classification, "DBLC-013V interrupted build recovery failed");
  Require(build_classification.recovery_classification ==
              index_api::IndexRecoveryClassification::interrupted_build,
          "DBLC-013V interrupted build was misclassified");
  Require(build_classification.descriptor.lifecycle_state ==
              index_api::IndexStatisticsLifecycleState::suspect,
          "DBLC-013V interrupted build did not become suspect");

  index_api::IndexRecoveryEvidence interrupted_stats;
  interrupted_stats.durable_state = index_api::IndexStatisticsLifecycleState::ready;
  interrupted_stats.catalog_record_present = true;
  interrupted_stats.physical_root_present = true;
  interrupted_stats.build_manifest_complete = true;
  interrupted_stats.statistics_refresh_in_progress = true;
  const auto stats_classification =
      index_api::ClassifyIndexLifecycleRecovery(descriptor, interrupted_stats, 51);
  RequireCoreOk(stats_classification, "DBLC-013V interrupted statistics recovery failed");
  Require(stats_classification.recovery_classification ==
              index_api::IndexRecoveryClassification::interrupted_statistics_refresh,
          "DBLC-013V interrupted statistics refresh was misclassified");
  Require(stats_classification.optimizer_plan_cache_invalidation_required,
          "DBLC-013V interrupted statistics refresh did not invalidate plans");

  index_api::IndexRecoveryEvidence corrupt;
  corrupt.catalog_record_present = true;
  corrupt.physical_root_present = true;
  corrupt.build_manifest_complete = true;
  corrupt.checksum_valid = false;
  const auto corrupt_classification =
      index_api::ClassifyIndexLifecycleRecovery(descriptor, corrupt, 52);
  RequireCoreOk(corrupt_classification, "DBLC-013V corrupt recovery classification failed");
  Require(corrupt_classification.recovery_classification ==
              index_api::IndexRecoveryClassification::corrupt_evidence,
          "DBLC-013V corrupt evidence was misclassified");
  Require(corrupt_classification.descriptor.lifecycle_state ==
              index_api::IndexStatisticsLifecycleState::quarantine,
          "DBLC-013V corrupt evidence did not quarantine index");
}

void TestOptimizerPlanPublicationAndStatementUse() {
  const auto path = TestPath("plan_publication_use");
  Cleanup(path);
  const auto descriptor = ReadyBuiltDescriptor();
  const auto statistics = RefreshStatistics(descriptor, 60);

  const auto statement_a = StatementContext(
      "019e0000-0000-7000-8000-000000000301",
      "019e0000-0000-7000-8000-000000000302",
      "019e0000-0000-7000-8000-000000000303",
      "019e0000-0000-7000-8000-000000000304", 61, 60);
  const auto dag_a = SelectedDag(statement_a, statistics.statistics_generation);
  auto cache_resolver = std::make_shared<ResolverState>();
  const auto cache_authority = Authority(dag_a, cache_resolver);
  const auto cache =
      CacheRequest(path, descriptor, statistics, dag_a, cache_authority);
  const auto cached = plan_api::EngineOptimizerCachePlan(cache);
  RequireOk(cached, "DBLC-013V corrected plan cache publication failed");
  Require(cache_resolver->calls == 2,
          "DBLC-013V cache publication did not revalidate exactly twice");
  Require(std::string(kCatalogEpochUuid) != statement_a.statement_uuid &&
              std::string(kCatalogEpochUuid) !=
                  statement_a.owning_transaction_uuid &&
              std::string(kCatalogEpochUuid) !=
                  statement_a.statement_snapshot_uuid &&
              std::string(kCatalogEpochUuid) !=
                  statement_a.statement_metadata_snapshot_uuid,
          "DBLC-013V catalog identity depended on statement A identity");
  Require(cached.entry.metadata_only && !cached.entry.invalidated,
          "DBLC-013V cached entry was not strict metadata only");
  Require(cached.entry.event_schema_version ==
              plan_api::kOptimizerPlanLifecycleEventSchemaVersion &&
              !cached.entry.event_uuid.empty() &&
              cached.entry.plan_uuid == kPlanUuid,
          "DBLC-013V corrected cache event identity missing");
  Require(cached.entry.statistics_generation == statistics.statistics_generation,
          "DBLC-013V cached plan statistics generation mismatch");
  Require(cached.publication_receipt &&
              cached.publication_receipt->purpose() ==
                  plan_api::EngineOptimizerPlanReceiptPurpose::kPublication &&
              cached.publication_receipt->authority_origin() ==
                  exec::CanonicalMgaAuthorityOrigin::
                      kEngineTransactionInventory,
          "DBLC-013V publication receipt contract missing");
  const auto publication_use =
      plan_api::RevalidateOptimizerPlanStatementUse(
          cached.entry, cached.publication_receipt);
  RequireNoUseExposure(
      publication_use,
      "DBLC-013V publication receipt became executable statement authority");
  Require(cache_resolver->calls == 2,
          "DBLC-013V publication receipt refusal resolved MGA authority");

  const auto replay = plan_api::LoadOptimizerPlanLifecycleState(cache.context);
  Require(replay.ok && replay.state.entries.size() == 1 &&
              replay.state.legacy_event_count == 0 &&
              replay.state.malformed_event_count == 0 &&
              replay.state.rejected_event_count == 0,
          "DBLC-013V corrected plan journal did not replay strictly");
  const auto& replayed = replay.state.entries.front();
  Require(replayed.event_uuid == cached.entry.event_uuid &&
              replayed.plan_uuid == cached.entry.plan_uuid &&
              replayed.metadata_only &&
              replayed.dependencies.bound_sblr_tree_uuid ==
                  kBoundSblrTreeUuid &&
              replayed.dependencies.catalog_epoch_uuid == kCatalogEpochUuid &&
              replayed.dependencies.security_context_uuid ==
                  kSecurityContextUuid &&
              replayed.dependencies.capability_snapshot_uuid ==
                  kCapabilitySnapshotUuid &&
              replayed.dependencies.resource_snapshot_uuid ==
                  kResourceSnapshotUuid &&
              replayed.dependencies.statistics_snapshot_uuid ==
                  kStatisticsSnapshotUuid &&
              replayed.dependencies.route_snapshot_uuid == kRouteSnapshotUuid &&
              replayed.dependencies.object_dependency_uuids ==
                  cache.object_dependency_uuids,
          "DBLC-013V corrected event dependencies did not replay exactly");

  auto validate_a_resolver = std::make_shared<ResolverState>();
  const auto validate_a_authority = Authority(dag_a, validate_a_resolver);
  const auto validate_a =
      ValidateRequest(path, statistics, dag_a, validate_a_authority);
  const auto hit_a = plan_api::EngineOptimizerValidateCachedPlan(validate_a);
  RequireOk(hit_a, "DBLC-013V statement A metadata validation failed");
  Require(hit_a.cache_hit && hit_a.metadata_cache_hit &&
              !hit_a.statement_use_admitted && hit_a.statement_use_receipt &&
              hit_a.statement_use_receipt->purpose() ==
                  plan_api::EngineOptimizerPlanReceiptPurpose::kStatementUse,
          "DBLC-013V statement A metadata hit bypassed receipt admission");
  Require(validate_a_resolver->calls == 2,
          "DBLC-013V statement A receipt was not revalidated twice before issue");
  const auto use_a = plan_api::RevalidateOptimizerPlanStatementUse(
      hit_a.entry, hit_a.statement_use_receipt);
  Require(use_a.ok && use_a.executable_receipt == hit_a.statement_use_receipt,
          "DBLC-013V statement A final receipt revalidation failed");
  Require(validate_a_resolver->calls == 3,
          "DBLC-013V statement A final use did not resolve current authority");

  const auto statement_b = StatementContext(
      "019e0000-0000-7000-8000-000000000311",
      "019e0000-0000-7000-8000-000000000312",
      "019e0000-0000-7000-8000-000000000313",
      "019e0000-0000-7000-8000-000000000314", 62, 0);
  const auto dag_b = SelectedDag(statement_b, statistics.statistics_generation);
  auto validate_b_resolver = std::make_shared<ResolverState>();
  const auto validate_b_authority = Authority(dag_b, validate_b_resolver);
  const auto validate_b =
      ValidateRequest(path, statistics, dag_b, validate_b_authority);
  const auto hit_b = plan_api::EngineOptimizerValidateCachedPlan(validate_b);
  RequireOk(hit_b, "DBLC-013V zero-highwater statement B validation failed");
  Require(hit_b.cache_hit && hit_b.metadata_cache_hit &&
              !hit_b.statement_use_admitted && hit_b.statement_use_receipt &&
              hit_b.statement_use_receipt->purpose() ==
                  plan_api::EngineOptimizerPlanReceiptPurpose::kStatementUse,
          "DBLC-013V statement B did not receive a statement-use receipt");
  Require(hit_b.entry.event_uuid == hit_a.entry.event_uuid &&
              hit_b.entry.plan_uuid == hit_a.entry.plan_uuid &&
              hit_b.statement_use_receipt->receipt_id() !=
                  hit_a.statement_use_receipt->receipt_id(),
          "DBLC-013V cross-statement metadata or receipt identity was wrong");
  Require(exec::PhysicalMgaStatementContextEqual(
              hit_b.statement_use_receipt->statement_context(), statement_b) &&
              hit_b.statement_use_receipt->statement_context()
                      .visible_committed_high_watermark == 0,
          "DBLC-013V statement B receipt replaced its zero high-watermark");
  Require(std::string(kCatalogEpochUuid) != statement_b.statement_uuid &&
              std::string(kCatalogEpochUuid) !=
                  statement_b.owning_transaction_uuid &&
              std::string(kCatalogEpochUuid) !=
                  statement_b.statement_snapshot_uuid &&
              std::string(kCatalogEpochUuid) !=
                  statement_b.statement_metadata_snapshot_uuid,
          "DBLC-013V catalog identity depended on statement B identity");
  Require(validate_b_resolver->calls == 2,
          "DBLC-013V statement B receipt was not revalidated twice before issue");
  const auto use_b = plan_api::RevalidateOptimizerPlanStatementUse(
      hit_b.entry, hit_b.statement_use_receipt);
  Require(use_b.ok && use_b.executable_receipt == hit_b.statement_use_receipt,
          "DBLC-013V statement B final receipt revalidation failed");
  Require(validate_b_resolver->calls == 3,
          "DBLC-013V statement B final use did not resolve current authority");

  Cleanup(path);
}

void TestOptimizerPlanMgaRefusalMatrix() {
  const auto path = TestPath("plan_mga_refusals");
  Cleanup(path);
  const auto descriptor = ReadyBuiltDescriptor();
  const auto statistics = RefreshStatistics(descriptor, 60);
  const auto statement = StatementContext(
      "019e0000-0000-7000-8000-000000000401",
      "019e0000-0000-7000-8000-000000000402",
      "019e0000-0000-7000-8000-000000000403",
      "019e0000-0000-7000-8000-000000000404", 61, 60);
  const auto dag = SelectedDag(statement, statistics.statistics_generation);
  auto cache_state = std::make_shared<ResolverState>();
  const auto cached = plan_api::EngineOptimizerCachePlan(CacheRequest(
      path, descriptor, statistics, dag, Authority(dag, cache_state)));
  RequireOk(cached, "DBLC-013V MGA refusal baseline cache failed");

  auto request_state = std::make_shared<ResolverState>();
  const auto baseline = ValidateRequest(
      path, statistics, dag, Authority(dag, request_state));
  const auto ExpectValidateRefusal = [&](const auto& request,
                                         const std::string_view message) {
    const auto refused =
        plan_api::EngineOptimizerValidateCachedPlan(request);
    RequireNoValidateExposure(refused, message);
  };

  auto mutation = baseline;
  mutation.context.transaction_uuid.canonical =
      "019e0000-0000-7000-8000-000000000411";
  ExpectValidateRefusal(mutation,
                        "DBLC-013V request transaction UUID mismatch exposed metadata");
  mutation = baseline;
  mutation.context.local_transaction_id = 62;
  ExpectValidateRefusal(mutation,
                        "DBLC-013V request local owner mismatch exposed metadata");
  mutation = baseline;
  mutation.context.statement_uuid.canonical =
      "019e0000-0000-7000-8000-000000000412";
  ExpectValidateRefusal(mutation,
                        "DBLC-013V request statement UUID mismatch exposed metadata");
  mutation = baseline;
  mutation.context.statement_snapshot_uuid.canonical =
      "019e0000-0000-7000-8000-000000000413";
  ExpectValidateRefusal(mutation,
                        "DBLC-013V request snapshot UUID mismatch exposed metadata");
  mutation = baseline;
  mutation.context.statement_metadata_snapshot_uuid.canonical =
      "019e0000-0000-7000-8000-000000000414";
  ExpectValidateRefusal(
      mutation,
      "DBLC-013V request metadata snapshot UUID mismatch exposed metadata");
  mutation = baseline;
  mutation.context.snapshot_visible_through_local_transaction_id = 59;
  ExpectValidateRefusal(mutation,
                        "DBLC-013V request high-water mismatch exposed metadata");
  mutation = baseline;
  mutation.context.statement_metadata_snapshot_engine_owned = false;
  ExpectValidateRefusal(mutation,
                        "DBLC-013V non-engine metadata snapshot exposed metadata");
  mutation = baseline;
  mutation.context
      .statement_metadata_snapshot_visible_through_local_transaction_id = 59;
  ExpectValidateRefusal(
      mutation,
      "DBLC-013V request metadata high-water mismatch exposed metadata");
  mutation = baseline;
  mutation.context
      .statement_metadata_snapshot_active_excluded_local_transaction_ids = {
      61, 64};
  ExpectValidateRefusal(
      mutation,
      "DBLC-013V request active exclusion mismatch exposed metadata");
  mutation = baseline;
  mutation.context
      .statement_metadata_snapshot_in_doubt_excluded_local_transaction_ids = {
      64};
  ExpectValidateRefusal(
      mutation,
      "DBLC-013V request in-doubt exclusion mismatch exposed metadata");

  const auto ExpectResolvedContextMutation =
      [&](const auto& mutate_current, const std::string_view message) {
        auto state = std::make_shared<ResolverState>();
        auto request = ValidateRequest(
            path, statistics, dag, Authority(dag, state));
        mutate_current(&state->current);
        const auto refused =
            plan_api::EngineOptimizerValidateCachedPlan(request);
        RequireNoValidateExposure(refused, message);
        Require(state->calls == 1,
                "DBLC-013V changed current context was not refused immediately");
      };

  ExpectResolvedContextMutation(
      [](auto* current) {
        current->statement_uuid =
            "019e0000-0000-7000-8000-000000000421";
      },
      "DBLC-013V resolved statement UUID mismatch exposed metadata");
  ExpectResolvedContextMutation(
      [](auto* current) {
        current->owning_transaction_uuid =
            "019e0000-0000-7000-8000-000000000422";
      },
      "DBLC-013V resolved owner UUID mismatch exposed metadata");
  ExpectResolvedContextMutation(
      [](auto* current) {
        current->statement_snapshot_uuid =
            "019e0000-0000-7000-8000-000000000423";
      },
      "DBLC-013V resolved snapshot UUID mismatch exposed metadata");
  ExpectResolvedContextMutation(
      [](auto* current) {
        current->statement_metadata_snapshot_uuid =
            "019e0000-0000-7000-8000-000000000424";
      },
      "DBLC-013V resolved metadata snapshot UUID mismatch exposed metadata");
  ExpectResolvedContextMutation(
      [](auto* current) { current->owning_local_transaction_id = 62; },
      "DBLC-013V resolved local owner mismatch exposed metadata");
  ExpectResolvedContextMutation(
      [](auto* current) { current->visible_committed_high_watermark = 59; },
      "DBLC-013V resolved high-water mismatch exposed metadata");
  ExpectResolvedContextMutation(
      [](auto* current) { current->oldest_active_transaction_id = 60; },
      "DBLC-013V resolved oldest-active mismatch exposed metadata");
  ExpectResolvedContextMutation(
      [](auto* current) { current->oldest_interesting_transaction_id = 2; },
      "DBLC-013V resolved oldest-interesting mismatch exposed metadata");
  ExpectResolvedContextMutation(
      [](auto* current) { current->oldest_snapshot_transaction_id = 2; },
      "DBLC-013V resolved oldest-snapshot mismatch exposed metadata");
  ExpectResolvedContextMutation(
      [](auto* current) { current->retention_horizon_transaction_id = 2; },
      "DBLC-013V resolved retention-horizon mismatch exposed metadata");
  ExpectResolvedContextMutation(
      [](auto* current) {
        current->active_excluded_local_transaction_ids = {61, 64};
      },
      "DBLC-013V resolved active-exclusion content exposed metadata");
  ExpectResolvedContextMutation(
      [](auto* current) {
        current->active_excluded_local_transaction_ids = {63, 61};
      },
      "DBLC-013V resolved active-exclusion order exposed metadata");
  ExpectResolvedContextMutation(
      [](auto* current) {
        current->in_doubt_excluded_local_transaction_ids = {64};
      },
      "DBLC-013V resolved in-doubt exclusion content exposed metadata");
  ExpectResolvedContextMutation(
      [](auto* current) { current->snapshot_kind = "transaction_stable"; },
      "DBLC-013V resolved snapshot-kind mismatch exposed metadata");
  ExpectResolvedContextMutation(
      [](auto* current) {
        current->publication_inventory_next_local_transaction_id = 72;
      },
      "DBLC-013V resolved publication bound mismatch exposed metadata");
  ExpectResolvedContextMutation(
      [](auto* current) { current->inventory_authoritative = false; },
      "DBLC-013V resolved non-authoritative inventory exposed metadata");
  ExpectResolvedContextMutation(
      [](auto* current) { current->complete = false; },
      "DBLC-013V resolved incomplete context exposed metadata");
  ExpectResolvedContextMutation(
      [](auto* current) { current->current = false; },
      "DBLC-013V resolved stale context exposed metadata");

  mutation = baseline;
  mutation.mga_authority.origin = exec::CanonicalMgaAuthorityOrigin::kMissing;
  ExpectValidateRefusal(mutation,
                        "DBLC-013V missing MGA authority exposed metadata");
  mutation = baseline;
  mutation.selected_physical_dag.abi_version = 1;
  ExpectValidateRefusal(mutation,
                        "DBLC-013V ABI1 carrier exposed metadata");
  mutation = baseline;
  mutation.selected_physical_dag.mga_statement_context = StatementContext(
      "019e0000-0000-7000-8000-000000000431",
      "019e0000-0000-7000-8000-000000000432",
      "019e0000-0000-7000-8000-000000000433",
      "019e0000-0000-7000-8000-000000000434", 62, 0);
  ExpectValidateRefusal(mutation,
                        "DBLC-013V swapped DAG context exposed metadata");
  mutation = baseline;
  mutation.selected_catalog_epoch_uuid =
      statement.statement_metadata_snapshot_uuid;
  mutation.selected_physical_dag.catalog_epoch_uuid =
      statement.statement_metadata_snapshot_uuid;
  mutation.selected_physical_dag.admission_evidence[1].evidence_uuid =
      statement.statement_metadata_snapshot_uuid;
  mutation.context.catalog_epoch_uuid.canonical =
      statement.statement_metadata_snapshot_uuid;
  ExpectValidateRefusal(mutation,
                        "DBLC-013V statement-dependent catalog exposed metadata");

  auto second_call_state = std::make_shared<ResolverState>();
  const auto second_call_request = ValidateRequest(
      path, statistics, dag, Authority(dag, second_call_state, 2));
  const auto second_call_refusal =
      plan_api::EngineOptimizerValidateCachedPlan(second_call_request);
  RequireNoValidateExposure(
      second_call_refusal,
      "DBLC-013V call-2 current change issued a statement receipt");
  Require(second_call_state->calls == 2,
          "DBLC-013V call-2 current change did not reach pre-receipt gate");

  auto issued_state = std::make_shared<ResolverState>();
  const auto issued_request = ValidateRequest(
      path, statistics, dag, Authority(dag, issued_state));
  const auto issued =
      plan_api::EngineOptimizerValidateCachedPlan(issued_request);
  RequireOk(issued, "DBLC-013V revocation receipt issue failed");
  Require(issued.statement_use_receipt && issued_state->calls == 2,
          "DBLC-013V revocation receipt was not issued at the exact gate");
  issued_state->current.statement_snapshot_uuid =
      "019e0000-0000-7000-8000-000000000441";
  const auto revoked = plan_api::RevalidateOptimizerPlanStatementUse(
      issued.entry, issued.statement_use_receipt);
  RequireNoUseExposure(
      revoked,
      "DBLC-013V changed current authority left receipt executable");
  Require(issued_state->calls == 3,
          "DBLC-013V receipt revocation did not resolve current authority");

  Cleanup(path);
}

void TestOptimizerPlanDependencyRefusalMatrix() {
  const auto path = TestPath("plan_dependency_refusals");
  Cleanup(path);
  const auto descriptor = ReadyBuiltDescriptor();
  const auto statistics = RefreshStatistics(descriptor, 60);
  const auto statement = StatementContext(
      "019e0000-0000-7000-8000-000000000501",
      "019e0000-0000-7000-8000-000000000502",
      "019e0000-0000-7000-8000-000000000503",
      "019e0000-0000-7000-8000-000000000504", 61, 60);
  const auto dag = SelectedDag(statement, statistics.statistics_generation);
  auto cache_state = std::make_shared<ResolverState>();
  const auto cached = plan_api::EngineOptimizerCachePlan(CacheRequest(
      path, descriptor, statistics, dag, Authority(dag, cache_state)));
  RequireOk(cached, "DBLC-013V dependency refusal baseline cache failed");

  auto baseline_state = std::make_shared<ResolverState>();
  const auto baseline = ValidateRequest(
      path, statistics, dag, Authority(dag, baseline_state));
  const auto ExpectValidateRefusal = [&](const auto& request,
                                         const std::string_view message) {
    const auto refused =
        plan_api::EngineOptimizerValidateCachedPlan(request);
    RequireNoValidateExposure(refused, message);
  };

  auto mutation = baseline;
  mutation.selected_physical_dag.bound_sblr_tree_uuid =
      "019e0000-0000-7000-8000-000000000511";
  mutation.selected_physical_dag.admission_evidence[0].evidence_uuid =
      mutation.selected_physical_dag.bound_sblr_tree_uuid;
  ExpectValidateRefusal(mutation,
                        "DBLC-013V bound SBLR dependency mismatch exposed entry");

  mutation = baseline;
  mutation.selected_physical_dag.catalog_epoch_uuid =
      "019e0000-0000-7000-8000-000000000512";
  mutation.selected_physical_dag.admission_evidence[1].evidence_uuid =
      mutation.selected_physical_dag.catalog_epoch_uuid;
  mutation.selected_catalog_epoch_uuid =
      mutation.selected_physical_dag.catalog_epoch_uuid;
  mutation.context.catalog_epoch_uuid.canonical =
      mutation.selected_physical_dag.catalog_epoch_uuid;
  ExpectValidateRefusal(mutation,
                        "DBLC-013V catalog UUID dependency mismatch exposed entry");

  mutation = baseline;
  mutation.selected_physical_dag.security_context_uuid =
      "019e0000-0000-7000-8000-000000000513";
  mutation.selected_physical_dag.admission_evidence[2].evidence_uuid =
      mutation.selected_physical_dag.security_context_uuid;
  ExpectValidateRefusal(mutation,
                        "DBLC-013V security UUID dependency mismatch exposed entry");

  mutation = baseline;
  mutation.selected_physical_dag.capability_snapshot_uuid =
      "019e0000-0000-7000-8000-000000000514";
  mutation.selected_physical_dag.admission_evidence[4].evidence_uuid =
      mutation.selected_physical_dag.capability_snapshot_uuid;
  mutation.context.optimizer_capability_snapshot_uuid.canonical =
      mutation.selected_physical_dag.capability_snapshot_uuid;
  ExpectValidateRefusal(
      mutation,
      "DBLC-013V capability snapshot dependency mismatch exposed entry");

  mutation = baseline;
  mutation.selected_physical_dag.resource_snapshot_uuid =
      "019e0000-0000-7000-8000-000000000515";
  mutation.selected_physical_dag.admission_evidence[5].evidence_uuid =
      mutation.selected_physical_dag.resource_snapshot_uuid;
  mutation.context.optimizer_resource_snapshot_uuid.canonical =
      mutation.selected_physical_dag.resource_snapshot_uuid;
  ExpectValidateRefusal(
      mutation,
      "DBLC-013V resource snapshot dependency mismatch exposed entry");

  mutation = baseline;
  mutation.selected_physical_dag.statistics_snapshot_uuid =
      "019e0000-0000-7000-8000-000000000516";
  mutation.selected_physical_dag.admission_evidence[6].evidence_uuid =
      mutation.selected_physical_dag.statistics_snapshot_uuid;
  ExpectValidateRefusal(
      mutation,
      "DBLC-013V statistics snapshot dependency mismatch exposed entry");

  mutation = baseline;
  mutation.selected_physical_dag.route_snapshot_uuid =
      "019e0000-0000-7000-8000-000000000517";
  mutation.selected_physical_dag.admission_evidence[7].evidence_uuid =
      mutation.selected_physical_dag.route_snapshot_uuid;
  mutation.context.optimizer_route_snapshot_uuid.canonical =
      mutation.selected_physical_dag.route_snapshot_uuid;
  ExpectValidateRefusal(mutation,
                        "DBLC-013V route UUID dependency mismatch exposed entry");

  mutation = baseline;
  ++mutation.selected_physical_dag.catalog_generation;
  mutation.context.catalog_generation_id =
      mutation.selected_physical_dag.catalog_generation;
  mutation.current_catalog_generation_id =
      mutation.selected_physical_dag.catalog_generation;
  ExpectValidateRefusal(
      mutation,
      "DBLC-013V catalog generation dependency mismatch exposed entry");

  mutation = baseline;
  ++mutation.selected_physical_dag.security_epoch;
  mutation.context.security_epoch = mutation.selected_physical_dag.security_epoch;
  ExpectValidateRefusal(mutation,
                        "DBLC-013V security epoch dependency mismatch exposed entry");

  mutation = baseline;
  ++mutation.selected_physical_dag.policy_epoch;
  ExpectValidateRefusal(mutation,
                        "DBLC-013V policy epoch dependency mismatch exposed entry");

  mutation = baseline;
  ++mutation.selected_physical_dag.resource_epoch;
  mutation.context.resource_epoch = mutation.selected_physical_dag.resource_epoch;
  mutation.current_resource_epochs.resource_epoch =
      mutation.selected_physical_dag.resource_epoch;
  ExpectValidateRefusal(mutation,
                        "DBLC-013V resource epoch dependency mismatch exposed entry");

  mutation = baseline;
  ++mutation.selected_physical_dag.statistics_generation;
  mutation.current_statistics_generation =
      mutation.selected_physical_dag.statistics_generation;
  ExpectValidateRefusal(
      mutation,
      "DBLC-013V statistics generation dependency mismatch exposed entry");

  mutation = baseline;
  ++mutation.selected_physical_dag.route_epoch;
  mutation.context.optimizer_route_epoch =
      mutation.selected_physical_dag.route_epoch;
  ExpectValidateRefusal(mutation,
                        "DBLC-013V route epoch dependency mismatch exposed entry");

  mutation = baseline;
  ++mutation.selected_physical_dag.route_generation;
  mutation.context.optimizer_route_generation =
      mutation.selected_physical_dag.route_generation;
  ExpectValidateRefusal(
      mutation,
      "DBLC-013V route generation dependency mismatch exposed entry");

  mutation = baseline;
  mutation.object_dependency_uuids.clear();
  ExpectValidateRefusal(mutation,
                        "DBLC-013V missing object dependencies exposed entry");
  mutation = baseline;
  mutation.object_dependency_uuids = {std::string(kIndexUuid),
                                     std::string(kIndexUuid),
                                     std::string(kRelationUuid)};
  ExpectValidateRefusal(mutation,
                        "DBLC-013V duplicate object dependencies exposed entry");
  mutation = baseline;
  mutation.object_dependency_uuids = {std::string(kRelationUuid),
                                     std::string(kIndexUuid)};
  ExpectValidateRefusal(mutation,
                        "DBLC-013V unsorted object dependencies exposed entry");

  mutation = baseline;
  mutation.plan_uuid = "019e0000-0000-7000-8000-000000000521";
  ExpectValidateRefusal(mutation,
                        "DBLC-013V wrong plan lookup exposed entry");
  mutation = baseline;
  mutation.query_fingerprint = "query:wrong-fingerprint:v2";
  ExpectValidateRefusal(mutation,
                        "DBLC-013V wrong fingerprint lookup exposed entry");
  mutation = baseline;
  mutation.index_uuid = "019e0000-0000-7000-8000-000000000522";
  ExpectValidateRefusal(mutation,
                        "DBLC-013V wrong index lookup exposed entry");
  mutation = baseline;
  mutation.selected_physical_dag.selected_plan_uuid =
      "019e0000-0000-7000-8000-000000000523";
  ExpectValidateRefusal(mutation,
                        "DBLC-013V selected plan mismatch exposed entry");

  mutation = baseline;
  mutation.require_current_statistics = true;
  mutation.statistics_stale = true;
  ExpectValidateRefusal(mutation,
                        "DBLC-013V stale current statistics exposed entry");
  mutation = baseline;
  ++mutation.current_index_generation;
  ExpectValidateRefusal(mutation,
                        "DBLC-013V current index generation mismatch exposed entry");
  mutation = baseline;
  ++mutation.current_statistics_generation;
  ExpectValidateRefusal(
      mutation,
      "DBLC-013V current statistics generation mismatch exposed entry");
  mutation = baseline;
  ++mutation.current_catalog_generation_id;
  ExpectValidateRefusal(
      mutation,
      "DBLC-013V current catalog generation mismatch exposed entry");
  mutation = baseline;
  ++mutation.current_resource_epochs.resource_epoch;
  ExpectValidateRefusal(mutation,
                        "DBLC-013V current resource epoch mismatch exposed entry");
  mutation = baseline;
  ++mutation.current_resource_epochs.charset_epoch;
  ExpectValidateRefusal(mutation,
                        "DBLC-013V current charset epoch mismatch exposed entry");
  mutation = baseline;
  ++mutation.current_resource_epochs.collation_epoch;
  ExpectValidateRefusal(
      mutation,
      "DBLC-013V current collation epoch mismatch exposed entry");

  auto final_state = std::make_shared<ResolverState>();
  const auto final_hit = plan_api::EngineOptimizerValidateCachedPlan(
      ValidateRequest(path, statistics, dag, Authority(dag, final_state)));
  RequireOk(final_hit,
            "DBLC-013V dependency refusal matrix altered cached metadata");
  const auto final_use = plan_api::RevalidateOptimizerPlanStatementUse(
      final_hit.entry, final_hit.statement_use_receipt);
  Require(final_use.ok &&
              final_use.executable_receipt == final_hit.statement_use_receipt &&
              final_state->calls == 3,
          "DBLC-013V dependency refusal matrix altered final use authority");

  Cleanup(path);
}

void TestOptimizerPlanPublicationTransitionRefusal() {
  const auto path = TestPath("plan_publication_transition");
  Cleanup(path);
  const auto descriptor = ReadyBuiltDescriptor();
  const auto statistics = RefreshStatistics(descriptor, 60);
  const auto statement = StatementContext(
      "019e0000-0000-7000-8000-000000000601",
      "019e0000-0000-7000-8000-000000000602",
      "019e0000-0000-7000-8000-000000000603",
      "019e0000-0000-7000-8000-000000000604", 61, 60);
  const auto dag = SelectedDag(statement, statistics.statistics_generation);
  auto resolver = std::make_shared<ResolverState>();
  const auto refused = plan_api::EngineOptimizerCachePlan(CacheRequest(
      path, descriptor, statistics, dag, Authority(dag, resolver, 2)));
  RequireNoCacheExposure(
      refused,
      "DBLC-013V call-2 publication transition exposed cached metadata");
  Require(resolver->calls == 2,
          "DBLC-013V publication transition did not refuse on resolution 2");

  const auto context = EngineContext(path, statement, dag);
  const auto replay = plan_api::LoadOptimizerPlanLifecycleState(context);
  Require(replay.ok && replay.state.entries.size() == 1 &&
              replay.state.entries.front().metadata_only &&
              !replay.state.entries.front().invalidated &&
              replay.state.legacy_event_count == 0 &&
              replay.state.malformed_event_count == 0 &&
              replay.state.rejected_event_count == 0,
          "DBLC-013V refused publication did not leave strict metadata only");
  std::ifstream journal(path.string() + ".sb.optimizer_plan_events",
                        std::ios::binary);
  const std::string journal_bytes((std::istreambuf_iterator<char>(journal)),
                                  std::istreambuf_iterator<char>());
  Require(journal.is_open() &&
              journal_bytes.find("SBPLANL2\t2\tCACHE_PLAN") !=
                         std::string::npos &&
              journal_bytes.find("creator_tx") == std::string::npos &&
              journal_bytes.find("statement_uuid") == std::string::npos &&
              journal_bytes.find("statement_snapshot") == std::string::npos &&
              journal_bytes.find("visibility") == std::string::npos &&
              journal_bytes.find("finality") == std::string::npos,
          "DBLC-013V refused publication persisted statement authority");

  Cleanup(path);
}

void TestOptimizerPlanInvalidationAndRecovery() {
  const auto path = TestPath("plan_invalidation_recovery");
  Cleanup(path);
  const auto descriptor = ReadyBuiltDescriptor();
  const auto statistics = RefreshStatistics(descriptor, 60);
  const auto statement = StatementContext(
      "019e0000-0000-7000-8000-000000000611",
      "019e0000-0000-7000-8000-000000000612",
      "019e0000-0000-7000-8000-000000000613",
      "019e0000-0000-7000-8000-000000000614", 61, 60);
  const auto dag = SelectedDag(statement, statistics.statistics_generation);
  const auto context = EngineContext(path, statement, dag);
  auto cache_state = std::make_shared<ResolverState>();
  const auto cached = plan_api::EngineOptimizerCachePlan(CacheRequest(
      path, descriptor, statistics, dag, Authority(dag, cache_state)));
  RequireOk(cached, "DBLC-013V invalidation baseline cache failed");
  const auto before = plan_api::LoadOptimizerPlanLifecycleState(context);
  Require(before.ok && before.state.max_event_sequence == 1,
          "DBLC-013V invalidation baseline replay failed");
  const auto journal_path = path.string() + ".sb.optimizer_plan_events";
  const auto bytes_before_incomplete = std::filesystem::file_size(journal_path);

  plan_api::EngineOptimizerInvalidatePlanCacheRequest incomplete;
  incomplete.context = context;
  incomplete.index_uuid = std::string(kIndexUuid);
  incomplete.reason = "incomplete_generation_vector";
  incomplete.new_index_generation = statistics.index_generation + 1;
  incomplete.new_statistics_generation = statistics.statistics_generation;
  incomplete.new_catalog_generation_id = statistics.catalog_generation_id;
  incomplete.new_resource_epochs = statistics.resource_epochs;
  incomplete.new_resource_epochs.collation_epoch = 0;
  const auto incomplete_result =
      plan_api::EngineOptimizerInvalidatePlanCache(incomplete);
  Require(!incomplete_result.ok && incomplete_result.state.entries.empty() &&
              incomplete_result.plan_cache_epoch == 0 &&
              incomplete_result.result_shape.rows.empty() &&
              incomplete_result.evidence.empty() &&
              incomplete_result.primary_object.uuid.canonical.empty() &&
              std::filesystem::file_size(journal_path) ==
                  bytes_before_incomplete,
          "DBLC-013V incomplete invalidation appended or exposed metadata");

  auto invalidate = incomplete;
  invalidate.reason = "index_rebuild_generation";
  invalidate.new_resource_epochs = statistics.resource_epochs;
  const auto invalidated =
      plan_api::EngineOptimizerInvalidatePlanCache(invalidate);
  RequireOk(invalidated, "DBLC-013V corrected plan invalidation failed");
  Require(invalidated.state.entries.size() == 1 &&
              invalidated.state.entries.front().metadata_only &&
              invalidated.state.entries.front().invalidated &&
              invalidated.state.invalidation_events == 1 &&
              invalidated.state.legacy_event_count == 0 &&
              invalidated.state.malformed_event_count == 0 &&
              invalidated.state.rejected_event_count == 0 &&
              HasEvidence(invalidated, "optimizer_plan_metadata_only", "true") &&
              HasEvidence(invalidated, "optimizer_plan_metadata_authority", "none"),
          "DBLC-013V invalidation was not strict metadata-only provenance");

  auto validation_state = std::make_shared<ResolverState>();
  const auto validation = plan_api::EngineOptimizerValidateCachedPlan(
      ValidateRequest(path, statistics, dag, Authority(dag, validation_state)));
  RequireNoValidateExposure(
      validation,
      "DBLC-013V invalidated plan exposed metadata or statement receipt");

  plan_api::EngineOptimizerRecoverPlanCacheRequest recover;
  recover.context = context;
  const auto recovered = plan_api::EngineOptimizerRecoverPlanCache(recover);
  RequireOk(recovered, "DBLC-013V corrected plan recovery failed");
  const auto CanonicalUuidText = [](const std::string_view value) {
    if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
        value[18] != '-' || value[23] != '-' ||
        value == "00000000-0000-0000-0000-000000000000") {
      return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
      if (index == 8 || index == 13 || index == 18 || index == 23) continue;
      const char ch = value[index];
      if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) {
        return false;
      }
    }
    return true;
  };
  Require(CanonicalUuidText(cached.entry.event_uuid) &&
              CanonicalUuidText(recovered.recovery_snapshot_uuid) &&
              recovered.state.recovery_snapshot_uuid ==
                  recovered.recovery_snapshot_uuid &&
              recovered.state.recovered_from_persisted_evidence &&
              recovered.state.entries.size() == 1 &&
              recovered.state.entries.front().metadata_only &&
              recovered.state.entries.front().invalidated &&
              recovered.state.entries.front()
                  .recovered_from_persisted_evidence &&
              HasEvidence(recovered,
                          "optimizer_plan_recovery_metadata_only", "true") &&
              HasEvidence(recovered, "optimizer_plan_recovery_authority", "none") &&
              HasRowField(recovered, "statement_snapshot_reconstructed", "false"),
          "DBLC-013V recovery created authority instead of metadata provenance");

  const auto replay = plan_api::LoadOptimizerPlanLifecycleState(context);
  Require(replay.ok && replay.state.max_event_sequence == 3 &&
              replay.state.recovered_from_persisted_evidence &&
              replay.state.entries.size() == 1 &&
              replay.state.entries.front().metadata_only &&
              replay.state.entries.front().invalidated &&
              replay.state.legacy_event_count == 0 &&
              replay.state.malformed_event_count == 0 &&
              replay.state.rejected_event_count == 0,
          "DBLC-013V invalidation/recovery journal did not replay strictly");

  std::ifstream journal(journal_path, std::ios::binary);
  const std::string journal_bytes((std::istreambuf_iterator<char>(journal)),
                                  std::istreambuf_iterator<char>());
  const auto invalidation_begin =
      journal_bytes.find("SBPLANL2\t2\tINVALIDATE");
  const auto recovery_begin =
      journal_bytes.find("SBPLANL2\t2\tRECOVERY_SNAPSHOT");
  const auto EventLine = [&](const std::size_t begin) {
    const auto end = journal_bytes.find('\n', begin);
    return std::string_view(journal_bytes).substr(
        begin, end == std::string::npos ? journal_bytes.size() - begin
                                        : end - begin);
  };
  const std::string invalidation_event_uuid =
      invalidation_begin == std::string::npos
          ? std::string{}
          : DecodeJournalField(EventLine(invalidation_begin), "event_uuid");
  const std::string recovery_event_uuid =
      recovery_begin == std::string::npos
          ? std::string{}
          : DecodeJournalField(EventLine(recovery_begin), "event_uuid");
  Require(journal.is_open() && journal_bytes.find("SBPLANL2\t2\t") !=
                         std::string::npos &&
              CanonicalUuidText(invalidation_event_uuid) &&
              CanonicalUuidText(recovery_event_uuid) &&
              journal_bytes.find("SBPLANL1") == std::string::npos &&
              journal_bytes.find("creator_tx") == std::string::npos &&
              journal_bytes.find("statement_uuid") == std::string::npos &&
              journal_bytes.find("statement_snapshot") == std::string::npos &&
              journal_bytes.find("visibility") == std::string::npos &&
              journal_bytes.find("finality") == std::string::npos,
          "DBLC-013V corrected journal persisted MGA or finality authority");

  Cleanup(path);
}

void TestOptimizerPlanStrictCorruptionRefusal() {
  const auto descriptor = ReadyBuiltDescriptor();
  const auto statistics = RefreshStatistics(descriptor, 60);
  const auto statement = StatementContext(
      "019e0000-0000-7000-8000-000000000701",
      "019e0000-0000-7000-8000-000000000702",
      "019e0000-0000-7000-8000-000000000703",
      "019e0000-0000-7000-8000-000000000704", 61, 60);
  const auto dag = SelectedDag(statement, statistics.statistics_generation);

  const auto AssertCorruptPath =
      [&](const std::filesystem::path& path,
          const std::uint64_t expected_legacy_count,
          const std::uint64_t expected_malformed_count,
          const std::string_view message) {
        const auto context = EngineContext(path, statement, dag);
        const auto loaded = plan_api::LoadOptimizerPlanLifecycleState(context);
        Require(!loaded.ok &&
                    loaded.diagnostic.code ==
                        plan_api::kOptimizerPlanDiagnosticCacheInvalidated &&
                    loaded.state.entries.empty() &&
                    loaded.state.plan_cache_epoch == 0 &&
                    loaded.state.invalidation_events == 0 &&
                    !loaded.state.recovered_from_persisted_evidence &&
                    loaded.state.recovery_snapshot_uuid.empty() &&
                    loaded.state.rejected_event_count == 1 &&
                    loaded.state.legacy_event_count == expected_legacy_count &&
                    loaded.state.malformed_event_count ==
                        expected_malformed_count,
                message);
        auto resolver = std::make_shared<ResolverState>();
        const auto refused = plan_api::EngineOptimizerValidateCachedPlan(
            ValidateRequest(path,
                            statistics,
                            dag,
                            Authority(dag, resolver)));
        RequireNoValidateExposure(refused, message);
      };

  struct RawCorruptionCase {
    std::string label;
    std::string journal;
    std::uint64_t legacy_count;
    std::uint64_t malformed_count;
  };
  const std::vector<RawCorruptionCase> raw_cases = {
      {"legacy",
       std::string(plan_api::kOptimizerPlanLifecycleLegacyEventMagic) +
           "\tCACHE_PLAN\n",
       1,
       0},
      {"truncated_v2",
       std::string(plan_api::kOptimizerPlanLifecycleEventMagic) +
           "\t2\tCACHE_PLAN\n",
       0,
       1},
      {"unknown_kind",
       std::string(plan_api::kOptimizerPlanLifecycleEventMagic) +
           "\t2\tUNKNOWN_KIND\tfield=31\n",
       0,
       1},
      {"malformed_hex",
       std::string(plan_api::kOptimizerPlanLifecycleEventMagic) +
           "\t2\tCACHE_PLAN\trecord_schema=zz\n",
       0,
       1},
      {"incomplete_cache_plan",
       std::string(plan_api::kOptimizerPlanLifecycleEventMagic) +
           "\t2\tCACHE_PLAN\trecord_schema=" +
           LowercaseHex("optimizer_plan_metadata_v2") + "\n",
       0,
       1},
  };
  for (const auto& corruption : raw_cases) {
    const auto path = TestPath("plan_corruption_" + corruption.label);
    Cleanup(path);
    WriteJournal(path, corruption.journal);
    AssertCorruptPath(path,
                      corruption.legacy_count,
                      corruption.malformed_count,
                      "DBLC-013V raw corruption did not fail closed");
    Cleanup(path);
  }

  const auto RewriteValidCacheAndRefuse =
      [&](const std::string_view label,
          const std::string_view field_name,
          const std::string_view replacement_value) {
        const auto path = TestPath("plan_corruption_" + std::string(label));
        Cleanup(path);
        auto cache_resolver = std::make_shared<ResolverState>();
        const auto cached = plan_api::EngineOptimizerCachePlan(CacheRequest(
            path,
            descriptor,
            statistics,
            dag,
            Authority(dag, cache_resolver)));
        RequireOk(cached,
                  "DBLC-013V valid cache record for rewrite was not created");
        RewriteJournalField(path, field_name, replacement_value);
        AssertCorruptPath(path,
                          0,
                          1,
                          "DBLC-013V rewritten corrected record did not fail closed");
        Cleanup(path);
      };
  RewriteValidCacheAndRefuse("signed_numeric", "plan_cache_epoch", "-1");
  RewriteValidCacheAndRefuse(
      "trailing_dependency_comma",
      "object_dependency_uuids",
      std::string(kIndexUuid) + "," + std::string(kRelationUuid) + ",");
}

}  // namespace

int main() {
  TestIndexBuildDropRebuildTransitions();
  TestStatisticsEpochsAndStaleRefusal();
  TestCatalogProfileCoupling();
  TestCrashRecoveryClassification();
  TestOptimizerPlanPublicationAndStatementUse();
  TestOptimizerPlanMgaRefusalMatrix();
  TestOptimizerPlanDependencyRefusalMatrix();
  TestOptimizerPlanPublicationTransitionRefusal();
  TestOptimizerPlanInvalidationAndRecovery();
  TestOptimizerPlanStrictCorruptionRefusal();
  return EXIT_SUCCESS;
}
