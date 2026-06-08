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

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace catalog = scratchbird::core::catalog;
namespace index_api = scratchbird::core::index;
namespace plan_api = scratchbird::engine::internal_api;
namespace platform = scratchbird::core::platform;

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

plan_api::EngineRequestContext Context(const std::filesystem::path& path,
                                       std::uint64_t tx,
                                       std::uint64_t visible_through = 0) {
  plan_api::EngineRequestContext context;
  context.database_path = path.string();
  context.database_uuid.canonical = "database-dblc-013v";
  context.principal_uuid.canonical = "principal-dblc-013v";
  context.transaction_uuid.canonical = "txn-" + std::to_string(tx);
  context.local_transaction_id = tx;
  context.request_id = "request-" + std::to_string(tx);
  context.security_context_present = true;
  context.snapshot_visible_through_local_transaction_id =
      visible_through == 0 ? tx : visible_through;
  context.catalog_generation_id = 7;
  context.resource_epoch = 3;
  return context;
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

void TestOptimizerPlanCacheInvalidationAndRecovery() {
  const auto path = TestPath("plan_cache");
  Cleanup(path);
  const auto descriptor = ReadyBuiltDescriptor();
  const auto statistics = RefreshStatistics(descriptor, 60);

  plan_api::EngineOptimizerCachePlanRequest cache;
  cache.context = Context(path, 61);
  cache.plan_uuid = "plan-dblc-013v-001";
  cache.query_fingerprint = "query:catalog-index-lookup:v1";
  cache.relation_uuid = "relation-sys-catalog-index-definitions";
  cache.index_uuid = "index-dblc-013v";
  cache.plan_shape_digest = "shape:index-point-lookup:v1";
  cache.index_descriptor = descriptor;
  cache.statistics = statistics;
  const auto cached = plan_api::EngineOptimizerCachePlan(cache);
  RequireOk(cached, "DBLC-013V plan cache write failed");
  Require(cached.entry.statistics_generation == statistics.statistics_generation,
          "DBLC-013V cached plan statistics generation mismatch");

  plan_api::EngineOptimizerValidateCachedPlanRequest validate;
  validate.context = Context(path, 62);
  validate.plan_uuid = cache.plan_uuid;
  validate.query_fingerprint = cache.query_fingerprint;
  validate.index_uuid = cache.index_uuid;
  validate.current_index_generation = statistics.index_generation;
  validate.current_statistics_generation = statistics.statistics_generation;
  validate.current_catalog_generation_id = statistics.catalog_generation_id;
  validate.current_resource_epochs = statistics.resource_epochs;
  const auto hit = plan_api::EngineOptimizerValidateCachedPlan(validate);
  RequireOk(hit, "DBLC-013V plan cache hit failed");
  Require(hit.cache_hit, "DBLC-013V plan cache did not report hit");

  auto epoch_mismatch = validate;
  epoch_mismatch.context = Context(path, 63);
  ++epoch_mismatch.current_resource_epochs.charset_epoch;
  RequireDiagnostic(plan_api::EngineOptimizerValidateCachedPlan(epoch_mismatch),
                    plan_api::kOptimizerPlanDiagnosticEpochMismatch,
                    "DBLC-013V charset epoch mismatch did not refuse cached plan");

  plan_api::EngineOptimizerInvalidatePlanCacheRequest invalidate;
  invalidate.context = Context(path, 64);
  invalidate.index_uuid = cache.index_uuid;
  invalidate.reason = "index_rebuild_generation";
  invalidate.new_index_generation = statistics.index_generation + 1;
  invalidate.new_statistics_generation = statistics.statistics_generation;
  invalidate.new_catalog_generation_id = statistics.catalog_generation_id;
  invalidate.new_resource_epochs = statistics.resource_epochs;
  const auto invalidated = plan_api::EngineOptimizerInvalidatePlanCache(invalidate);
  RequireOk(invalidated, "DBLC-013V plan cache invalidation failed");
  Require(invalidated.state.invalidation_events == 1,
          "DBLC-013V plan invalidation event was not replayed");

  validate.context = Context(path, 65);
  RequireDiagnostic(plan_api::EngineOptimizerValidateCachedPlan(validate),
                    plan_api::kOptimizerPlanDiagnosticCacheInvalidated,
                    "DBLC-013V invalidated cached plan was accepted");

  plan_api::EngineOptimizerRecoverPlanCacheRequest recover;
  recover.context = Context(path, 66);
  const auto recovered = plan_api::EngineOptimizerRecoverPlanCache(recover);
  RequireOk(recovered, "DBLC-013V optimizer plan recovery failed");
  Require(recovered.state.recovered_from_persisted_evidence,
          "DBLC-013V plan recovery evidence was not classified");
  Require(!recovered.recovery_snapshot_uuid.empty(),
          "DBLC-013V plan recovery snapshot UUID missing");

  Cleanup(path);
}

}  // namespace

int main() {
  TestIndexBuildDropRebuildTransitions();
  TestStatisticsEpochsAndStaleRefusal();
  TestCatalogProfileCoupling();
  TestCrashRecoveryClassification();
  TestOptimizerPlanCacheInvalidationAndRecovery();
  return EXIT_SUCCESS;
}
