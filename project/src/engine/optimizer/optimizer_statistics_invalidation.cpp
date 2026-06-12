// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_statistics_invalidation.hpp"

#include <algorithm>
#include <set>
#include <utility>

namespace scratchbird::engine::optimizer {
namespace {

StatisticsContractStatus Status(bool ok, std::string code, std::string detail) {
  StatisticsContractStatus status;
  status.ok = ok;
  status.diagnostic_code = std::move(code);
  status.detail = std::move(detail);
  return status;
}

OptimizerStatisticsInvalidationResult Refuse(std::string code, std::string evidence) {
  OptimizerStatisticsInvalidationResult result;
  result.accepted = false;
  result.diagnostic_code = std::move(code);
  result.evidence.push_back(std::move(evidence));
  return result;
}

bool ContainsAll(std::vector<std::string> actual,
                 std::vector<std::string> required) {
  std::sort(actual.begin(), actual.end());
  actual.erase(std::unique(actual.begin(), actual.end()), actual.end());
  std::sort(required.begin(), required.end());
  required.erase(std::unique(required.begin(), required.end()), required.end());
  return std::includes(actual.begin(), actual.end(),
                       required.begin(), required.end());
}

std::string EventKind(OptimizerStatisticsInvalidationKind kind) {
  switch (kind) {
    case OptimizerStatisticsInvalidationKind::kCatalogGeneration:
      return "catalog_epoch";
    case OptimizerStatisticsInvalidationKind::kSecurityGeneration:
      return "security_epoch";
    case OptimizerStatisticsInvalidationKind::kRedactionGeneration:
      return "redaction_epoch";
    case OptimizerStatisticsInvalidationKind::kPolicyGeneration:
      return "policy_epoch";
    case OptimizerStatisticsInvalidationKind::kResourceGeneration:
      return "resource_epoch";
    case OptimizerStatisticsInvalidationKind::kNameResolutionGeneration:
      return "name_resolution_epoch";
    case OptimizerStatisticsInvalidationKind::kAnalyzeGeneration:
      return "analyze_generation";
    case OptimizerStatisticsInvalidationKind::kStatsRefresh:
      return "stats_refresh";
    case OptimizerStatisticsInvalidationKind::kStorageMetricGeneration:
      return "storage_metric_generation";
    case OptimizerStatisticsInvalidationKind::kRuntimeMetricGeneration:
      return "runtime_metric_generation";
    case OptimizerStatisticsInvalidationKind::kIndexGeneration:
      return "index_generation";
  }
  return "stats_refresh";
}

bool KindAuthorityPresent(const OptimizerStatisticsInvalidationRequest& request) {
  const auto& authority = request.authority;
  switch (request.kind) {
    case OptimizerStatisticsInvalidationKind::kCatalogGeneration:
    case OptimizerStatisticsInvalidationKind::kPolicyGeneration:
    case OptimizerStatisticsInvalidationKind::kResourceGeneration:
    case OptimizerStatisticsInvalidationKind::kNameResolutionGeneration:
      return authority.catalog_generation_authority;
    case OptimizerStatisticsInvalidationKind::kSecurityGeneration:
      return authority.security_generation_authority;
    case OptimizerStatisticsInvalidationKind::kRedactionGeneration:
      return authority.redaction_generation_authority;
    case OptimizerStatisticsInvalidationKind::kAnalyzeGeneration:
    case OptimizerStatisticsInvalidationKind::kStatsRefresh:
      return authority.analyze_generation_authority;
    case OptimizerStatisticsInvalidationKind::kStorageMetricGeneration:
    case OptimizerStatisticsInvalidationKind::kRuntimeMetricGeneration:
      return authority.metric_generation_authority;
    case OptimizerStatisticsInvalidationKind::kIndexGeneration:
      return authority.index_generation_authority;
  }
  return false;
}

bool KindGenerationPresent(const OptimizerStatisticsInvalidationRequest& request) {
  switch (request.kind) {
    case OptimizerStatisticsInvalidationKind::kCatalogGeneration:
      return request.catalog_epoch != 0;
    case OptimizerStatisticsInvalidationKind::kSecurityGeneration:
      return request.security_epoch != 0;
    case OptimizerStatisticsInvalidationKind::kRedactionGeneration:
      return request.redaction_epoch != 0;
    case OptimizerStatisticsInvalidationKind::kPolicyGeneration:
    case OptimizerStatisticsInvalidationKind::kResourceGeneration:
    case OptimizerStatisticsInvalidationKind::kNameResolutionGeneration:
      return request.resource_epoch != 0 || request.catalog_epoch != 0;
    case OptimizerStatisticsInvalidationKind::kAnalyzeGeneration:
      return request.analyze_generation != 0 && request.stats_epoch != 0;
    case OptimizerStatisticsInvalidationKind::kStatsRefresh:
      return request.stats_epoch != 0;
    case OptimizerStatisticsInvalidationKind::kStorageMetricGeneration:
    case OptimizerStatisticsInvalidationKind::kRuntimeMetricGeneration:
      return request.metric_generation != 0;
    case OptimizerStatisticsInvalidationKind::kIndexGeneration:
      return request.index_generation != 0 && !request.index_uuid.empty();
  }
  return false;
}

bool UnsafeAuthorityPresent(const OptimizerStatisticsInvalidationAuthority& authority) {
  return authority.parser_or_reference_authority ||
         authority.client_finality_authority ||
         authority.client_visibility_authority ||
         authority.metric_finality_authority ||
         authority.metric_visibility_authority ||
         authority.external_recovery_authority ||
         authority.cluster_authority ||
         authority.fixture_or_synthetic_source;
}

}  // namespace

const char* OptimizerStatisticsInvalidationKindName(
    OptimizerStatisticsInvalidationKind kind) {
  switch (kind) {
    case OptimizerStatisticsInvalidationKind::kCatalogGeneration:
      return "catalog_generation";
    case OptimizerStatisticsInvalidationKind::kSecurityGeneration:
      return "security_generation";
    case OptimizerStatisticsInvalidationKind::kRedactionGeneration:
      return "redaction_generation";
    case OptimizerStatisticsInvalidationKind::kPolicyGeneration:
      return "policy_generation";
    case OptimizerStatisticsInvalidationKind::kResourceGeneration:
      return "resource_generation";
    case OptimizerStatisticsInvalidationKind::kNameResolutionGeneration:
      return "name_resolution_generation";
    case OptimizerStatisticsInvalidationKind::kAnalyzeGeneration:
      return "analyze_generation";
    case OptimizerStatisticsInvalidationKind::kStatsRefresh:
      return "stats_refresh";
    case OptimizerStatisticsInvalidationKind::kStorageMetricGeneration:
      return "storage_metric_generation";
    case OptimizerStatisticsInvalidationKind::kRuntimeMetricGeneration:
      return "runtime_metric_generation";
    case OptimizerStatisticsInvalidationKind::kIndexGeneration:
      return "index_generation";
  }
  return "stats_refresh";
}

OptimizerStatisticsInvalidationResult DispatchOptimizerStatisticsInvalidation(
    const OptimizerStatisticsInvalidationRequest& request,
    OptimizerPinnedStatsDescriptorCache* cache) {
  if (cache == nullptr) {
    return Refuse("SB_OPT_STATS_INVALIDATION_CACHE_REQUIRED", "cache_required");
  }
  const auto& authority = request.authority;
  if (!authority.engine_runtime_scope || !authority.optimizer_cache_owner) {
    return Refuse("SB_OPT_STATS_INVALIDATION_AUTHORITY_REQUIRED",
                  "engine_scope_and_cache_owner_required");
  }
  if (!KindAuthorityPresent(request)) {
    return Refuse("SB_OPT_STATS_INVALIDATION_KIND_AUTHORITY_REQUIRED",
                  OptimizerStatisticsInvalidationKindName(request.kind));
  }
  if (UnsafeAuthorityPresent(authority)) {
    return Refuse("SB_OPT_STATS_INVALIDATION_UNSAFE_AUTHORITY",
                  "unsafe_authority_refused");
  }
  if (request.evidence_digest.empty()) {
    return Refuse("SB_OPT_STATS_INVALIDATION_EVIDENCE_REQUIRED",
                  "evidence_digest_required");
  }
  if (!KindGenerationPresent(request)) {
    return Refuse("SB_OPT_STATS_INVALIDATION_GENERATION_REQUIRED",
                  OptimizerStatisticsInvalidationKindName(request.kind));
  }

  StatsInvalidationEvent event;
  event.event_kind = EventKind(request.kind);
  event.object_uuid = request.object_uuid;
  event.index_uuid = request.index_uuid;
  event.security_policy_identity = request.security_policy_identity;
  event.redaction_policy_identity = request.redaction_policy_identity;
  event.new_catalog_epoch = request.catalog_epoch;
  event.new_stats_epoch = request.stats_epoch;
  event.reason = request.reason.empty()
                     ? OptimizerStatisticsInvalidationKindName(request.kind)
                     : request.reason;
  auto invalidated = cache->Invalidate(event);

  OptimizerStatisticsInvalidationResult result;
  result.accepted = true;
  result.diagnostic_code = "SB_OPT_STATS_INVALIDATION_DISPATCHED";
  result.invalidated_entries = std::move(invalidated.invalidated_entries);
  result.evidence.push_back("kind=" +
                            std::string(OptimizerStatisticsInvalidationKindName(request.kind)));
  result.evidence.push_back("event_kind=" + event.event_kind);
  result.evidence.push_back("evidence_digest=" + request.evidence_digest);
  result.evidence.push_back("parser_or_reference_authority=false");
  result.evidence.push_back("cluster_authority=false");
  return result;
}

std::vector<StatisticsContractStatus> ValidatePinnedStatsForBenchmarkCleanAdmission(
    const OptimizerPinnedStatsBenchmarkCleanRequest& request) {
  std::vector<StatisticsContractStatus> statuses;
  const auto key_status = ValidateOptimizerPinnedStatsDescriptorKey(request.snapshot.key);
  if (!key_status.ok) {
    statuses.push_back(Status(false, key_status.diagnostic_code, key_status.detail));
  }
  if (request.snapshot.key.catalog_epoch < request.required_catalog_epoch ||
      request.snapshot.key.security_epoch < request.required_security_epoch ||
      request.snapshot.key.resource_policy_epoch < request.required_resource_policy_epoch ||
      request.snapshot.key.name_resolution_epoch < request.required_name_resolution_epoch ||
      request.snapshot.key.stats_epoch < request.required_stats_epoch) {
    statuses.push_back(Status(false,
                              "SB_OPT_STATS_BENCHMARK_CLEAN_STALE_EPOCH",
                              request.snapshot.key.descriptor_set_digest));
  }
  if (!request.required_descriptor_set_digest.empty() &&
      request.snapshot.key.descriptor_set_digest != request.required_descriptor_set_digest) {
    statuses.push_back(Status(false,
                              "SB_OPT_STATS_BENCHMARK_CLEAN_DESCRIPTOR_MISMATCH",
                              request.snapshot.key.descriptor_set_digest));
  }
  if (!request.required_security_policy_identity.empty() &&
      request.snapshot.key.security_policy_identity != request.required_security_policy_identity) {
    statuses.push_back(Status(false,
                              "SB_OPT_STATS_BENCHMARK_CLEAN_SECURITY_POLICY_MISMATCH",
                              request.snapshot.key.security_policy_identity));
  }
  if (!request.required_redaction_policy_identity.empty() &&
      request.snapshot.key.redaction_policy_identity != request.required_redaction_policy_identity) {
    statuses.push_back(Status(false,
                              "SB_OPT_STATS_BENCHMARK_CLEAN_REDACTION_POLICY_MISMATCH",
                              request.snapshot.key.redaction_policy_identity));
  }
  if (!ContainsAll(request.snapshot.key.object_uuids, request.required_object_uuids)) {
    statuses.push_back(Status(false,
                              "SB_OPT_STATS_BENCHMARK_CLEAN_OBJECT_SCOPE_MISMATCH",
                              request.snapshot.key.descriptor_set_digest));
  }
  if (!ContainsAll(request.snapshot.key.index_uuids, request.required_index_uuids)) {
    statuses.push_back(Status(false,
                              "SB_OPT_STATS_BENCHMARK_CLEAN_INDEX_SCOPE_MISMATCH",
                              request.snapshot.key.descriptor_set_digest));
  }
  if (!request.snapshot.read_only_snapshot ||
      !request.snapshot.mga_visibility_recheck_required ||
      !request.snapshot.security_recheck_required ||
      request.snapshot.finality_authority_cached) {
    statuses.push_back(Status(false,
                              "SB_OPT_STATS_BENCHMARK_CLEAN_UNSAFE_SNAPSHOT",
                              request.snapshot.key.descriptor_set_digest));
  }
  auto snapshot_statuses = ValidateOptimizerStatsSnapshot(request.snapshot.stats_snapshot);
  for (const auto& status : snapshot_statuses) {
    if (!status.ok) statuses.push_back(status);
  }
  if (statuses.empty()) {
    statuses.push_back(Status(true,
                              "SB_OPT_STATS_BENCHMARK_CLEAN_PINNED_OK",
                              request.snapshot.key.descriptor_set_digest));
  }
  return statuses;
}

}  // namespace scratchbird::engine::optimizer
