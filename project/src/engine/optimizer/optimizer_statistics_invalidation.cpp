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
#include <new>
#include <type_traits>

namespace scratchbird::engine::optimizer {
namespace {

StatisticsContractStatus Status(bool ok, std::string code, std::string detail) {
  StatisticsContractStatus status;
  status.ok = ok;
  status.diagnostic_code = ok ? "" : "SB-STAT-0001";
  status.reason = ok ? StatisticsContractReason::kNone
      : (code.find("EPOCH") != std::string::npos ? StatisticsContractReason::kEpochInvalid
         : StatisticsContractReason::kValueInvalid);
  status.detail = std::move(code) + ": " + std::move(detail);
  return status;
}

OptimizerStatisticsInvalidationResult Refuse(std::string code, std::string evidence) {
  OptimizerStatisticsInvalidationResult result;
  result.accepted = false;
  result.failure = OptimizerStatisticsInvalidationFailure::kInvalidRequest;
  result.diagnostic_code = "SB-STAT-0001";
  result.evidence.push_back(std::move(code) + ": " + std::move(evidence));
  return result;
}

bool ContainsAll(std::vector<planner::CanonicalPlannerUuid> actual,
                 std::vector<planner::CanonicalPlannerUuid> required) {
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
  return "unknown";
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
      return request.policy_epoch != 0;
    case OptimizerStatisticsInvalidationKind::kResourceGeneration:
      return request.resource_epoch != 0;
    case OptimizerStatisticsInvalidationKind::kNameResolutionGeneration:
      return request.name_resolution_epoch != 0;
    case OptimizerStatisticsInvalidationKind::kAnalyzeGeneration:
      return request.analyze_generation != 0 && request.stats_epoch != 0;
    case OptimizerStatisticsInvalidationKind::kStatsRefresh:
      return request.stats_epoch != 0;
    case OptimizerStatisticsInvalidationKind::kStorageMetricGeneration:
    case OptimizerStatisticsInvalidationKind::kRuntimeMetricGeneration:
      return request.metric_generation != 0;
    case OptimizerStatisticsInvalidationKind::kIndexGeneration:
      return request.index_generation != 0 && scratchbird::core::uuid::IsEngineIdentityUuid(request.index_uuid);
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

static OptimizerStatisticsInvalidationResult DispatchInvalidationImpl(
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

  for (const auto& id : {request.object_uuid, request.index_uuid, request.filespace_uuid,
                         request.security_policy_identity, request.redaction_policy_identity}) {
    if (!id.is_nil() && !scratchbird::core::uuid::IsEngineIdentityUuid(id))
      return Refuse("SB_OPT_STATS_INVALIDATION_IDENTITY", "invalid system UUID");
  }
  std::string_view digest = request.evidence_digest;
  if (digest.starts_with("sha256:")) digest.remove_prefix(7);
  if (digest.size() != 64 || !std::ranges::all_of(digest, [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
      })) return Refuse("SB_OPT_STATS_INVALIDATION_EVIDENCE_REQUIRED", "invalid SHA256 content digest");

  StatsInvalidationEvent event;
  event.event_kind = EventKind(request.kind);
  event.object_uuid = request.object_uuid;
  event.index_uuid = request.index_uuid;
  event.filespace_uuid = request.filespace_uuid;
  event.security_policy_identity = request.security_policy_identity;
  event.redaction_policy_identity = request.redaction_policy_identity;
  event.new_catalog_epoch = request.catalog_epoch;
  event.new_stats_epoch = request.stats_epoch;
  event.reason = request.reason.empty()
                     ? OptimizerStatisticsInvalidationKindName(request.kind)
                     : request.reason;
  std::uint64_t generation = 0;
  using Kind = OptimizerStatisticsInvalidationKind;
  switch (request.kind) {
    case Kind::kCatalogGeneration: generation = request.catalog_epoch; break;
    case Kind::kSecurityGeneration: generation = request.security_epoch; break;
    case Kind::kRedactionGeneration: generation = request.redaction_epoch; break;
    case Kind::kPolicyGeneration: generation = request.policy_epoch; break;
    case Kind::kResourceGeneration: generation = request.resource_epoch; break;
    case Kind::kNameResolutionGeneration: generation = request.name_resolution_epoch; break;
    case Kind::kAnalyzeGeneration: generation = request.analyze_generation; break;
    case Kind::kStatsRefresh: generation = request.stats_epoch; break;
    case Kind::kStorageMetricGeneration:
    case Kind::kRuntimeMetricGeneration: generation = request.metric_generation; break;
    case Kind::kIndexGeneration: generation = request.index_generation; break;
  }
  using Scope = OptimizerStatsGenerationScope;
  for (const auto& [scope, id] : {
      std::pair{Scope::kObject, request.object_uuid},
      {Scope::kIndex, request.index_uuid},
      {Scope::kFilespace, request.filespace_uuid},
      {Scope::kSecurityPolicy, request.security_policy_identity},
      {Scope::kRedactionPolicy, request.redaction_policy_identity}}) {
    if (!id.is_nil()) event.generations.push_back({{request.kind, scope, id}, generation});
  }
  if (event.generations.empty())
    event.generations.push_back({{request.kind, Scope::kNode, {}}, generation});

  OptimizerStatisticsInvalidationResult result;

  result.evidence.push_back("kind=" +
                            std::string(OptimizerStatisticsInvalidationKindName(request.kind)));
  result.evidence.push_back("event_kind=" + event.event_kind);
  result.evidence.push_back("evidence_digest=" + request.evidence_digest);
  result.evidence.push_back("parser_or_reference_authority=false");
  result.evidence.push_back("cluster_authority=false");
  // Response storage is complete before the cache's atomic commit.
  auto invalidated = cache->Invalidate(event);
  if (!invalidated.accepted)
    return Refuse("SB_OPT_STATS_INVALIDATION_EVENT", "invalid generation/scope binding");
  static_assert(std::is_nothrow_move_assignable_v<decltype(result.invalidated_entries)>);
  result.invalidated_entries = std::move(invalidated.invalidated_entries);
  result.accepted = true;
  return result;
}

OptimizerStatisticsInvalidationResult DispatchOptimizerStatisticsInvalidation(
    const OptimizerStatisticsInvalidationRequest& request,
    OptimizerPinnedStatsDescriptorCache* cache) {
  try {
    return DispatchInvalidationImpl(request, cache);
  } catch (const std::bad_alloc&) {
    OptimizerStatisticsInvalidationResult result;
    result.failure = OptimizerStatisticsInvalidationFailure::kResourceExhausted;
    return result;
  } catch (...) {
    OptimizerStatisticsInvalidationResult result;
    result.failure = OptimizerStatisticsInvalidationFailure::kInternalFailure;
    return result;
  }
}

std::vector<StatisticsContractStatus> ValidatePinnedStatsForBenchmarkCleanAdmission(
    const OptimizerPinnedStatsBenchmarkCleanRequest& request) {
  std::vector<StatisticsContractStatus> statuses;
  const auto valid_ids = [](const auto& ids) {
    std::set<planner::CanonicalPlannerUuid> unique;
    return std::ranges::all_of(ids, [&](const auto& id) {
      return scratchbird::core::uuid::IsEngineIdentityUuid(id) && unique.insert(id).second;
    });
  };
  if (!request.required_catalog_epoch || !request.required_security_epoch ||
      !request.required_resource_policy_epoch || !request.required_name_resolution_epoch ||
      !request.required_stats_epoch || request.required_descriptor_set_digest.empty() ||
      !scratchbird::core::uuid::IsEngineIdentityUuid(request.required_security_policy_identity) ||
      !scratchbird::core::uuid::IsEngineIdentityUuid(request.required_redaction_policy_identity) ||
      request.required_object_uuids.empty() || !valid_ids(request.required_object_uuids) ||
      !valid_ids(request.required_index_uuids)) {
    statuses.push_back(Status(false, "SB_OPT_STATS_BENCHMARK_CLEAN_CONTEXT",
                              "complete current context and distinct binary scopes required"));
  }
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
  if (!request.required_security_policy_identity.is_nil() &&
      request.snapshot.key.security_policy_identity != request.required_security_policy_identity) {
    statuses.push_back(Status(false,
                              "SB_OPT_STATS_BENCHMARK_CLEAN_SECURITY_POLICY_MISMATCH",
                              "binary security policy differs"));
    statuses.back().object_uuid = request.snapshot.key.security_policy_identity;
  }
  if (!request.required_redaction_policy_identity.is_nil() &&
      request.snapshot.key.redaction_policy_identity != request.required_redaction_policy_identity) {
    statuses.push_back(Status(false,
                              "SB_OPT_STATS_BENCHMARK_CLEAN_REDACTION_POLICY_MISMATCH",
                              "binary redaction policy differs"));
    statuses.back().object_uuid = request.snapshot.key.redaction_policy_identity;
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
    if (!status.ok) {
      auto normalized = Status(false, status.diagnostic_code, status.detail);
      normalized.object_uuid = status.object_uuid;
      if (status.reason != StatisticsContractReason::kNone) normalized.reason = status.reason;
      statuses.push_back(std::move(normalized));
    }
  }
  std::size_t records_checked = 0;
  const auto check_sources = [&](const auto& records) {
    records_checked += records.size();
    for (const auto& record : records) {
      if (!OptimizerStatsIdentityIsUsable(record.identity) ||
          (record.identity.source != StatisticSource::kCatalogExact &&
           record.identity.source != StatisticSource::kCatalogSample)) {
        auto status = Status(false, "SB_OPT_STATS_BENCHMARK_CLEAN_SOURCE",
                             "benchmark inputs require usable catalog provenance");
        status.reason = StatisticsContractReason::kSourceInvalid;
        status.object_uuid = record.identity.object_uuid;
        statuses.push_back(std::move(status));
      }
      if (record.identity.catalog_epoch < request.required_catalog_epoch ||
          record.identity.stats_epoch < request.required_stats_epoch) {
        auto status = Status(false, "SB_OPT_STATS_BENCHMARK_CLEAN_STALE_EPOCH",
                             "payload predates required context");
        status.object_uuid = record.identity.object_uuid;
        statuses.push_back(std::move(status));
      }
    }
  };
  check_sources(request.snapshot.stats_snapshot.tables);
  check_sources(request.snapshot.stats_snapshot.columns);
  check_sources(request.snapshot.stats_snapshot.histograms);
  check_sources(request.snapshot.stats_snapshot.mcv);
  check_sources(request.snapshot.stats_snapshot.extended_stats);
  check_sources(request.snapshot.stats_snapshot.indexes);
  check_sources(request.snapshot.stats_snapshot.expressions);
  check_sources(request.snapshot.stats_snapshot.page_filespaces);
  if (records_checked == 0) {
    auto status = Status(false, "SB_OPT_STATS_BENCHMARK_CLEAN_SOURCE",
                         "missing observations are not measured zero");
    status.reason = StatisticsContractReason::kMissing;
    statuses.push_back(std::move(status));
  }
  if (statuses.empty()) {
    statuses.push_back(Status(true,
                              "SB_OPT_STATS_BENCHMARK_CLEAN_PINNED_OK",
                              request.snapshot.key.descriptor_set_digest));
  }
  return statuses;
}

}  // namespace scratchbird::engine::optimizer
