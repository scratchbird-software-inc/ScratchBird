// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_statistics_lifecycle.hpp"

#include <algorithm>
#include <utility>

namespace scratchbird::core::index {
namespace {

namespace catalog = scratchbird::core::catalog;
using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status OkStatus() { return {StatusCode::ok, Severity::info, Subsystem::engine}; }
Status RefuseStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::engine};
}

IndexStatisticsLifecycleResult LifecycleOk() {
  IndexStatisticsLifecycleResult result;
  result.status = OkStatus();
  result.diagnostic = MakeIndexStatisticsLifecycleDiagnostic(
      result.status,
      kIndexStatisticsDiagnosticOk,
      "index.statistics.ok");
  result.admitted = true;
  return result;
}

IndexStatisticsLifecycleResult LifecycleRefusal(const char* code,
                                                std::string message_key,
                                                std::string detail = {}) {
  IndexStatisticsLifecycleResult result;
  result.status = RefuseStatus();
  result.diagnostic = MakeIndexStatisticsLifecycleDiagnostic(
      result.status, code, std::move(message_key), std::move(detail));
  return result;
}

bool DescriptorHasIdentity(const IndexLifecycleDescriptor& descriptor) {
  return descriptor.index_uuid.valid() && descriptor.table_uuid.valid() &&
         descriptor.family != IndexFamily::unknown;
}

bool IsReadableLifecycleState(IndexStatisticsLifecycleState state) {
  return state == IndexStatisticsLifecycleState::ready;
}

u64 NextGeneration(u64 left, u64 right) {
  return std::max(left, right) + 1;
}

double SelectivityFor(u64 row_count, u64 distinct_key_count) {
  if (row_count == 0 || distinct_key_count == 0) {
    return 1.0;
  }
  const double lower_bound = 1.0 / static_cast<double>(row_count);
  const double estimate = 1.0 / static_cast<double>(distinct_key_count);
  return std::clamp(estimate, lower_bound, 1.0);
}

bool ProfileSupportsDeclaredPurpose(const catalog::CatalogPhysicalIndexProfile& profile) {
  if (!profile.authoritative || !profile.supports_mga_snapshot_visibility) {
    return false;
  }
  if (profile.supports_uuid_exact_lookup) {
    return profile.method == catalog::CatalogIndexMethod::hash_equality;
  }
  return catalog::CatalogIndexProfileHasOrderedNeed(profile) ||
         profile.supports_catalog_generation_visibility ||
         profile.supports_transaction_history ||
         profile.supports_name_resolution;
}

void CopyCatalogProfileBinding(IndexLifecycleDescriptor* descriptor,
                               const catalog::CatalogPhysicalIndexProfile& profile) {
  descriptor->catalog_profile.physical_profile_key = profile.index_name;
  descriptor->catalog_profile.catalog_table_path = profile.table_path;
  descriptor->catalog_profile.catalog_profile_authoritative = profile.authoritative;
  descriptor->catalog_profile.catalog_profile_supports_mga_snapshot_visibility =
      profile.supports_mga_snapshot_visibility;
  descriptor->catalog_profile.catalog_profile_supports_exact_lookup =
      profile.supports_uuid_exact_lookup;
  descriptor->catalog_profile.catalog_profile_supports_generation_visibility =
      profile.supports_catalog_generation_visibility;
}

IndexStatisticsLifecycleResult RefuseUnlessMutatingContext(const IndexStatisticsLifecycleRequest& request) {
  if (request.local_transaction_id == 0) {
    return LifecycleRefusal(kIndexStatisticsDiagnosticMgaTransactionRequired,
                            "index.statistics.mga_transaction_required",
                            "local_transaction_id");
  }
  if (!DescriptorHasIdentity(request.descriptor)) {
    return LifecycleRefusal(kIndexStatisticsDiagnosticInvalidRequest,
                            "index.statistics.invalid_request",
                            "descriptor_identity");
  }
  return LifecycleOk();
}

}  // namespace

const char* IndexStatisticsLifecycleStateName(IndexStatisticsLifecycleState state) {
  switch (state) {
    case IndexStatisticsLifecycleState::absent: return "absent";
    case IndexStatisticsLifecycleState::build_pending: return "build_pending";
    case IndexStatisticsLifecycleState::building: return "building";
    case IndexStatisticsLifecycleState::ready: return "ready";
    case IndexStatisticsLifecycleState::dropping: return "dropping";
    case IndexStatisticsLifecycleState::dropped: return "dropped";
    case IndexStatisticsLifecycleState::stale: return "stale";
    case IndexStatisticsLifecycleState::suspect: return "suspect";
    case IndexStatisticsLifecycleState::quarantine: return "quarantine";
  }
  return "unknown";
}

const char* IndexRecoveryClassificationName(IndexRecoveryClassification classification) {
  switch (classification) {
    case IndexRecoveryClassification::clean_ready: return "clean_ready";
    case IndexRecoveryClassification::interrupted_build: return "interrupted_build";
    case IndexRecoveryClassification::interrupted_drop: return "interrupted_drop";
    case IndexRecoveryClassification::interrupted_rebuild: return "interrupted_rebuild";
    case IndexRecoveryClassification::interrupted_statistics_refresh:
      return "interrupted_statistics_refresh";
    case IndexRecoveryClassification::stale_resource_epoch: return "stale_resource_epoch";
    case IndexRecoveryClassification::corrupt_evidence: return "corrupt_evidence";
    case IndexRecoveryClassification::quarantine_required: return "quarantine_required";
  }
  return "unknown";
}

bool IndexResourceEpochVectorValid(const IndexResourceEpochVector& epochs) {
  return epochs.resource_epoch != 0 && epochs.charset_epoch != 0 && epochs.collation_epoch != 0;
}

bool IndexResourceEpochVectorEqual(const IndexResourceEpochVector& left,
                                   const IndexResourceEpochVector& right) {
  return left.resource_epoch == right.resource_epoch &&
         left.charset_epoch == right.charset_epoch &&
         left.collation_epoch == right.collation_epoch;
}

bool IndexStatisticsVisibleToSnapshot(const IndexStatisticsSnapshot& statistics,
                                      u64 snapshot_visible_through_transaction_id) {
  if (statistics.refreshed_by_transaction_id == 0) {
    return false;
  }
  if (snapshot_visible_through_transaction_id == 0) {
    return false;
  }
  return statistics.refreshed_by_transaction_id <= snapshot_visible_through_transaction_id;
}

IndexStatisticsLifecycleResult ValidateCatalogPhysicalIndexProfileCoupling(
    const IndexLifecycleDescriptor& descriptor,
    const catalog::CatalogPhysicalIndexProfile* profile) {
  if (!DescriptorHasIdentity(descriptor)) {
    return LifecycleRefusal(kIndexStatisticsDiagnosticInvalidRequest,
                            "index.statistics.invalid_request",
                            "descriptor_identity");
  }
  if (profile == nullptr) {
    return LifecycleRefusal(kIndexStatisticsDiagnosticCatalogProfileMissing,
                            "index.statistics.catalog_profile_missing",
                            descriptor.catalog_profile.physical_profile_key);
  }
  if (descriptor.catalog_profile.physical_profile_key.empty() ||
      descriptor.catalog_profile.physical_profile_key != profile->index_name ||
      descriptor.catalog_profile.catalog_table_path.empty() ||
      descriptor.catalog_profile.catalog_table_path != profile->table_path ||
      !ProfileSupportsDeclaredPurpose(*profile)) {
    return LifecycleRefusal(kIndexStatisticsDiagnosticCatalogProfileMismatch,
                            "index.statistics.catalog_profile_mismatch",
                            descriptor.catalog_profile.physical_profile_key);
  }

  auto result = LifecycleOk();
  result.descriptor = descriptor;
  CopyCatalogProfileBinding(&result.descriptor, *profile);
  result.actions.push_back("catalog_physical_profile_bound_by_key");
  result.actions.push_back("catalog_generation_visibility_preserved");
  result.actions.push_back("mga_snapshot_visibility_preserved");
  return result;
}

IndexStatisticsLifecycleResult ClassifyIndexLifecycleRecovery(
    const IndexLifecycleDescriptor& descriptor,
    const IndexRecoveryEvidence& evidence,
    u64 local_transaction_id) {
  if (local_transaction_id == 0) {
    return LifecycleRefusal(kIndexStatisticsDiagnosticMgaTransactionRequired,
                            "index.statistics.mga_transaction_required",
                            "recovery_transaction");
  }
  if (!DescriptorHasIdentity(descriptor)) {
    return LifecycleRefusal(kIndexStatisticsDiagnosticInvalidRequest,
                            "index.statistics.invalid_request",
                            "descriptor_identity");
  }

  auto result = LifecycleOk();
  result.descriptor = descriptor;
  result.descriptor.metadata_epoch = NextGeneration(descriptor.metadata_epoch, local_transaction_id);
  result.actions.push_back("record_recovery_classification_evidence");

  if (!evidence.checksum_valid || !evidence.checkpoint_reachable) {
    result.recovery_classification = IndexRecoveryClassification::corrupt_evidence;
    result.descriptor.lifecycle_state = IndexStatisticsLifecycleState::quarantine;
    result.optimizer_plan_cache_invalidation_required = true;
    result.actions.push_back("quarantine_index_until_rebuild");
    return result;
  }
  if (!evidence.resource_epoch_current) {
    result.recovery_classification = IndexRecoveryClassification::stale_resource_epoch;
    result.descriptor.lifecycle_state = IndexStatisticsLifecycleState::stale;
    result.optimizer_plan_cache_invalidation_required = true;
    result.actions.push_back("refuse_index_use_until_resource_epoch_rebuild");
    return result;
  }
  if (evidence.statistics_refresh_in_progress) {
    result.recovery_classification = IndexRecoveryClassification::interrupted_statistics_refresh;
    result.descriptor.lifecycle_state = IndexStatisticsLifecycleState::ready;
    result.optimizer_plan_cache_invalidation_required = true;
    result.actions.push_back("discard_partial_statistics_refresh");
    return result;
  }
  if (evidence.durable_state == IndexStatisticsLifecycleState::building ||
      (evidence.catalog_record_present && !evidence.build_manifest_complete)) {
    result.recovery_classification = IndexRecoveryClassification::interrupted_build;
    result.descriptor.lifecycle_state = IndexStatisticsLifecycleState::suspect;
    result.optimizer_plan_cache_invalidation_required = true;
    result.actions.push_back("resume_or_rebuild_before_publish");
    return result;
  }
  if (evidence.durable_state == IndexStatisticsLifecycleState::dropping ||
      evidence.drop_tombstone_present) {
    result.recovery_classification = IndexRecoveryClassification::interrupted_drop;
    result.descriptor.lifecycle_state = evidence.drop_tombstone_present
                                            ? IndexStatisticsLifecycleState::dropped
                                            : IndexStatisticsLifecycleState::dropping;
    result.optimizer_plan_cache_invalidation_required = true;
    result.actions.push_back("finish_drop_cleanup_after_horizon");
    return result;
  }
  if (!evidence.catalog_record_present || !evidence.physical_root_present) {
    result.recovery_classification = IndexRecoveryClassification::quarantine_required;
    result.descriptor.lifecycle_state = IndexStatisticsLifecycleState::quarantine;
    result.optimizer_plan_cache_invalidation_required = true;
    result.actions.push_back("catalog_physical_evidence_incomplete");
    return result;
  }

  result.recovery_classification = IndexRecoveryClassification::clean_ready;
  result.descriptor.lifecycle_state = IndexStatisticsLifecycleState::ready;
  result.index_scan_allowed = true;
  result.actions.push_back("publish_recovered_ready_state");
  return result;
}

IndexStatisticsLifecycleResult RefreshIndexStatistics(
    const IndexStatisticsLifecycleRequest& request) {
  const auto context = RefuseUnlessMutatingContext(request);
  if (!context.ok()) {
    return context;
  }
  if (!IsReadableLifecycleState(request.descriptor.lifecycle_state)) {
    return LifecycleRefusal(kIndexStatisticsDiagnosticLifecycleStateRefused,
                            "index.statistics.lifecycle_state_refused",
                            IndexStatisticsLifecycleStateName(request.descriptor.lifecycle_state));
  }
  if (!IndexResourceEpochVectorValid(request.descriptor.resource_epochs)) {
    return LifecycleRefusal(kIndexStatisticsDiagnosticResourceEpochMismatch,
                            "index.statistics.resource_epoch_mismatch",
                            "descriptor_resource_epochs");
  }
  if (!request.refresh.full_scan_evidence && !request.refresh.sampled_evidence) {
    return LifecycleRefusal(kIndexStatisticsDiagnosticInvalidRequest,
                            "index.statistics.invalid_request",
                            "refresh_evidence");
  }

  auto result = LifecycleOk();
  result.descriptor = request.descriptor;
  result.statistics = request.refresh.prior_statistics;
  result.statistics.index_uuid = request.descriptor.index_uuid;
  result.statistics.statistics_generation =
      NextGeneration(request.refresh.prior_statistics.statistics_generation,
                     request.descriptor.index_generation);
  result.statistics.index_generation = request.descriptor.index_generation;
  result.statistics.catalog_generation_id = request.descriptor.catalog_generation_id;
  result.statistics.refreshed_by_transaction_id = request.local_transaction_id;
  result.statistics.visible_generation = request.local_transaction_id;
  result.statistics.row_count = request.refresh.observed_row_count;
  result.statistics.distinct_key_count = request.refresh.observed_distinct_key_count;
  result.statistics.leaf_page_count = std::max<u64>(1, request.refresh.observed_leaf_page_count);
  result.statistics.retained_version_count = std::max(request.refresh.observed_retained_version_count,
                                                      request.refresh.observed_row_count);
  result.statistics.selectivity = SelectivityFor(result.statistics.row_count,
                                                 result.statistics.distinct_key_count);
  result.statistics.resource_epochs = request.descriptor.resource_epochs;
  result.statistics.physical_profile_key = request.descriptor.catalog_profile.physical_profile_key;
  result.statistics.catalog_profile_coupled =
      request.descriptor.catalog_profile.catalog_profile_authoritative &&
      request.descriptor.catalog_profile.catalog_profile_supports_mga_snapshot_visibility;
  result.statistics.current = true;
  result.statistics.stale = false;
  result.statistics.mga_visible =
      IndexStatisticsVisibleToSnapshot(result.statistics,
                                       request.snapshot_visible_through_transaction_id);
  result.statistics_refreshed = true;
  result.optimizer_plan_cache_invalidation_required = true;
  result.actions.push_back(request.refresh.full_scan_evidence
                               ? "refresh_statistics_from_full_scan_evidence"
                               : "refresh_statistics_from_sample_evidence");
  result.actions.push_back("advance_statistics_generation_with_mga_visibility");
  result.actions.push_back("invalidate_optimizer_plans_for_statistics_generation");
  return result;
}

IndexStatisticsLifecycleResult EvaluateIndexStatisticsForUse(
    const IndexLifecycleDescriptor& descriptor,
    const IndexStatisticsSnapshot& statistics,
    const IndexResourceEpochVector& runtime_epochs,
    IndexStatisticsFreshnessPolicy freshness_policy,
    u64 snapshot_visible_through_transaction_id) {
  if (!DescriptorHasIdentity(descriptor) || !statistics.index_uuid.valid()) {
    return LifecycleRefusal(kIndexStatisticsDiagnosticInvalidRequest,
                            "index.statistics.invalid_request",
                            "statistics_identity");
  }
  if (!IsReadableLifecycleState(descriptor.lifecycle_state)) {
    return LifecycleRefusal(kIndexStatisticsDiagnosticLifecycleStateRefused,
                            "index.statistics.lifecycle_state_refused",
                            IndexStatisticsLifecycleStateName(descriptor.lifecycle_state));
  }
  if (!IndexResourceEpochVectorValid(runtime_epochs) ||
      !IndexResourceEpochVectorEqual(descriptor.resource_epochs, runtime_epochs) ||
      !IndexResourceEpochVectorEqual(statistics.resource_epochs, runtime_epochs)) {
    return LifecycleRefusal(kIndexStatisticsDiagnosticResourceEpochMismatch,
                            "index.statistics.resource_epoch_mismatch",
                            "runtime_resource_epochs");
  }
  const bool generation_current =
      statistics.current && !statistics.stale &&
      statistics.index_generation == descriptor.index_generation &&
      statistics.catalog_generation_id == descriptor.catalog_generation_id &&
      statistics.physical_profile_key == descriptor.catalog_profile.physical_profile_key &&
      statistics.catalog_profile_coupled;
  const bool visible =
      IndexStatisticsVisibleToSnapshot(statistics, snapshot_visible_through_transaction_id);
  if ((!generation_current || !visible) &&
      (freshness_policy == IndexStatisticsFreshnessPolicy::refuse_stale ||
       freshness_policy == IndexStatisticsFreshnessPolicy::require_current)) {
    auto result = LifecycleRefusal(kIndexStatisticsDiagnosticStaleRefused,
                                   "index.statistics.stale_refused",
                                   descriptor.catalog_profile.physical_profile_key);
    result.stale_statistics_refused = true;
    result.optimizer_plan_cache_invalidation_required = true;
    result.actions.push_back("refuse_stale_statistics_policy");
    return result;
  }

  auto result = LifecycleOk();
  result.descriptor = descriptor;
  result.statistics = statistics;
  result.statistics.mga_visible = visible;
  result.index_scan_allowed = generation_current && visible;
  if (!generation_current || !visible) {
    result.statistics.stale = true;
    result.optimizer_plan_cache_invalidation_required = true;
    result.actions.push_back("allow_stale_statistics_only_with_refresh_request");
  } else {
    result.actions.push_back("admit_current_statistics_for_optimizer");
  }
  return result;
}

IndexStatisticsLifecycleResult PlanIndexStatisticsLifecycle(
    const IndexStatisticsLifecycleRequest& request,
    const catalog::CatalogPhysicalIndexProfile* catalog_profile) {
  if (request.operation == IndexStatisticsLifecycleOperation::classify_recovery) {
    IndexRecoveryEvidence evidence;
    evidence.durable_state = request.descriptor.lifecycle_state;
    evidence.catalog_record_present = request.catalog_evidence_written;
    evidence.physical_root_present = request.physical_build_complete;
    evidence.build_manifest_complete = request.validation_complete;
    evidence.statistics_refresh_in_progress = request.statistics_refresh_requested;
    evidence.checkpoint_reachable = request.checkpoint_reachable;
    evidence.resource_epoch_current = IndexResourceEpochVectorValid(request.descriptor.resource_epochs);
    return ClassifyIndexLifecycleRecovery(request.descriptor, evidence, request.local_transaction_id);
  }

  const auto context = RefuseUnlessMutatingContext(request);
  if (!context.ok()) {
    return context;
  }
  const auto coupling = ValidateCatalogPhysicalIndexProfileCoupling(request.descriptor, catalog_profile);
  if (!coupling.ok()) {
    return coupling;
  }

  if (request.operation == IndexStatisticsLifecycleOperation::refresh_statistics) {
    auto coupled_request = request;
    coupled_request.descriptor = coupling.descriptor;
    return RefreshIndexStatistics(coupled_request);
  }

  auto result = LifecycleOk();
  result.descriptor = coupling.descriptor;
  result.descriptor.creating_transaction_id =
      request.descriptor.creating_transaction_id == 0
          ? request.local_transaction_id
          : request.descriptor.creating_transaction_id;
  result.descriptor.metadata_epoch = NextGeneration(request.descriptor.metadata_epoch,
                                                    request.local_transaction_id);
  result.optimizer_plan_cache_invalidation_required = request.optimizer_plan_invalidation_requested;

  switch (request.operation) {
    case IndexStatisticsLifecycleOperation::build:
      result.descriptor.lifecycle_state =
          request.catalog_evidence_written
              ? (request.physical_build_complete && request.validation_complete
                     ? IndexStatisticsLifecycleState::ready
                     : IndexStatisticsLifecycleState::building)
              : IndexStatisticsLifecycleState::build_pending;
      result.descriptor.index_generation =
          request.physical_build_complete && request.validation_complete
              ? NextGeneration(request.descriptor.index_generation, request.local_transaction_id)
              : request.descriptor.index_generation;
      result.index_scan_allowed =
          result.descriptor.lifecycle_state == IndexStatisticsLifecycleState::ready;
      result.optimizer_plan_cache_invalidation_required =
          result.optimizer_plan_cache_invalidation_required || result.index_scan_allowed;
      result.actions.push_back("record_catalog_build_evidence_before_publish");
      result.actions.push_back("publish_index_generation_after_validation");
      break;
    case IndexStatisticsLifecycleOperation::drop:
      result.descriptor.dropping_transaction_id = request.local_transaction_id;
      result.descriptor.lifecycle_state =
          request.catalog_evidence_written ? IndexStatisticsLifecycleState::dropped
                                           : IndexStatisticsLifecycleState::dropping;
      result.descriptor.index_generation =
          NextGeneration(request.descriptor.index_generation, request.local_transaction_id);
      result.index_scan_allowed = false;
      result.optimizer_plan_cache_invalidation_required = true;
      result.actions.push_back("record_catalog_drop_evidence");
      result.actions.push_back("retain_physical_pages_until_mga_horizon_allows_cleanup");
      break;
    case IndexStatisticsLifecycleOperation::rebuild:
      result.descriptor.lifecycle_state =
          request.physical_build_complete && request.validation_complete
              ? IndexStatisticsLifecycleState::ready
              : IndexStatisticsLifecycleState::building;
      result.descriptor.index_generation =
          request.physical_build_complete && request.validation_complete
              ? NextGeneration(request.descriptor.index_generation, request.local_transaction_id)
              : request.descriptor.index_generation;
      result.index_scan_allowed =
          result.descriptor.lifecycle_state == IndexStatisticsLifecycleState::ready;
      result.optimizer_plan_cache_invalidation_required = true;
      result.actions.push_back("build_replacement_physical_root");
      result.actions.push_back("invalidate_optimizer_plans_for_rebuild_generation");
      break;
    case IndexStatisticsLifecycleOperation::refresh_statistics:
    case IndexStatisticsLifecycleOperation::classify_recovery:
      break;
  }
  return result;
}

DiagnosticRecord MakeIndexStatisticsLifecycleDiagnostic(Status status,
                                                        std::string diagnostic_code,
                                                        std::string message_key,
                                                        std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "core.index.statistics_lifecycle");
}

}  // namespace scratchbird::core::index
