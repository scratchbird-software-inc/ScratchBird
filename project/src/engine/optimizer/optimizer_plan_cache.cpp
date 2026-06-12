// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_plan_cache.hpp"

#include <algorithm>
#include <array>
#include <iomanip>
#include <sstream>
#include <utility>

#include <openssl/sha.h>

namespace scratchbird::engine::optimizer {
namespace {

constexpr const char* kDiagHit = "SB_OPTIMIZER_PLAN_CACHE_HIT";
constexpr const char* kDiagMiss = "SB_OPTIMIZER_PLAN_CACHE_MISS";
constexpr const char* kDiagStaleEpoch = "SB_OPTIMIZER_PLAN_CACHE_STALE_EPOCH";
constexpr const char* kDiagIncompatibleParameterShape =
    "SB_OPTIMIZER_PLAN_CACHE_INCOMPATIBLE_PARAMETER_SHAPE";
constexpr const char* kDiagMemoryGrantMismatch = "SB_OPTIMIZER_PLAN_CACHE_MEMORY_GRANT_MISMATCH";
constexpr const char* kDiagSecurityPolicyMismatch =
    "SB_OPTIMIZER_PLAN_CACHE_REDACTION_SECURITY_POLICY_MISMATCH";
constexpr const char* kDiagOptimizerControlPolicyMismatch =
    "SB_OPTIMIZER_PLAN_CACHE_OPTIMIZER_CONTROL_POLICY_MISMATCH";
constexpr const char* kDiagRouteCapabilityMismatch =
    "SB_OPTIMIZER_PLAN_CACHE_ROUTE_CAPABILITY_MISMATCH";
constexpr const char* kDiagDependencyInvalidated =
    "SB_OPTIMIZER_PLAN_CACHE_DEPENDENCY_INVALIDATED";
constexpr const char* kDiagUnknownInvalidationKind =
    "SB_OPTIMIZER_PLAN_CACHE_UNKNOWN_INVALIDATION_KIND";
constexpr const char* kDiagAuthorityUnsafe = "SB_OPTIMIZER_PLAN_CACHE_AUTHORITY_UNSAFE";
constexpr const char* kDiagEnterpriseKeyIncomplete =
    "SB_OPTIMIZER_PLAN_CACHE_ENTERPRISE_KEY_INCOMPLETE";
constexpr const char* kDiagEnterprisePlanUnsafe =
    "SB_OPTIMIZER_PLAN_CACHE_ENTERPRISE_PLAN_UNSAFE";
constexpr const char* kDiagEnterprisePersistenceUnsafe =
    "SB_OPTIMIZER_PLAN_CACHE_ENTERPRISE_PERSISTENCE_UNSAFE";
constexpr const char* kDiagEnterprisePersistenceDigestMismatch =
    "SB_OPTIMIZER_PLAN_CACHE_ENTERPRISE_PERSISTENCE_DIGEST_MISMATCH";
constexpr const char* kDiagEnterpriseOk = "SB_OPTIMIZER_PLAN_CACHE_ENTERPRISE_OK";

std::string Sha256Hex(const std::string& payload) {
  std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
  SHA256(reinterpret_cast<const unsigned char*>(payload.data()), payload.size(), digest.data());
  std::ostringstream out;
  for (const auto byte : digest) {
    out << std::hex << std::setw(2) << std::setfill('0')
        << static_cast<unsigned int>(byte);
  }
  return out.str();
}

void AppendSorted(std::ostringstream& out, const char* label, std::vector<std::string> values) {
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
  out << '|' << label << '=';
  for (const auto& value : values) out << value << ';';
}

bool CandidateMentions(const PlanCandidate& candidate, const std::string& dependency_uuid) {
  if (dependency_uuid.empty()) return false;
  if (candidate.candidate_id.find(dependency_uuid) != std::string::npos) return true;
  for (const auto& fact : candidate.required_facts) if (fact.find(dependency_uuid) != std::string::npos) return true;
  return false;
}

bool CandidateIsClusterPath(const PlanCandidate& candidate) {
  return candidate.cluster_candidate ||
         candidate.access_kind == scratchbird::engine::planner::PhysicalAccessKind::kClusterFragmentScan ||
         candidate.access_kind == scratchbird::engine::planner::PhysicalAccessKind::kRemoteNodePushdown ||
         candidate.candidate_id.find("cluster") != std::string::npos ||
         candidate.candidate_id.find("remote") != std::string::npos;
}

bool ContainsClusterOrRemoteToken(const std::string& value) {
  return value.find("cluster") != std::string::npos ||
         value.find("remote") != std::string::npos;
}

bool IsPlaceholderPlanCacheValue(const std::string& value) {
  return value.empty() ||
         value == "redaction:default" ||
         value == "parameters:unbound" ||
         value == "memory:default" ||
         value == "grant:default" ||
         value == "result-contract-v1" ||
         value.find(":default") != std::string::npos ||
         value.find("placeholder") != std::string::npos;
}

bool SortedVectorsDiffer(std::vector<std::string> left, std::vector<std::string> right) {
  std::sort(left.begin(), left.end());
  left.erase(std::unique(left.begin(), left.end()), left.end());
  std::sort(right.begin(), right.end());
  right.erase(std::unique(right.begin(), right.end()), right.end());
  return left != right;
}

bool SameStatementFamily(const OptimizerPlanCacheKeyInput& left,
                         const OptimizerPlanCacheKeyInput& right) {
  return left.operation_id == right.operation_id && left.sblr_digest == right.sblr_digest;
}

bool StaleEpochOrStatsMismatch(const OptimizerPlanCacheKeyInput& cached,
                               const OptimizerPlanCacheKeyInput& requested) {
  return cached.catalog_epoch != requested.catalog_epoch ||
         cached.stats_epoch != requested.stats_epoch ||
         cached.resource_epoch != requested.resource_epoch ||
         cached.name_resolution_epoch != requested.name_resolution_epoch ||
         cached.compatibility_epoch != requested.compatibility_epoch ||
         cached.format_compatibility_epoch != requested.format_compatibility_epoch ||
         cached.statistics_snapshot_id != requested.statistics_snapshot_id ||
         cached.catalog_stats_digest != requested.catalog_stats_digest ||
         cached.descriptor_set_digest != requested.descriptor_set_digest;
}

bool ParameterShapeMismatch(const OptimizerPlanCacheKeyInput& cached,
                            const OptimizerPlanCacheKeyInput& requested) {
  return cached.parameter_shape_digest != requested.parameter_shape_digest;
}

bool OptimizerControlPolicyMismatch(const OptimizerPlanCacheKeyInput& cached,
                                    const OptimizerPlanCacheKeyInput& requested) {
  return cached.normalized_optimizer_controls_digest !=
         requested.normalized_optimizer_controls_digest;
}

bool MemoryGrantMismatch(const OptimizerPlanCacheKeyInput& cached,
                         const OptimizerPlanCacheKeyInput& requested) {
  return cached.memory_policy_epoch != requested.memory_policy_epoch ||
         cached.memory_feedback_generation != requested.memory_feedback_generation ||
         cached.memory_grant_class != requested.memory_grant_class ||
         cached.memory_grant_digest != requested.memory_grant_digest;
}

bool SecurityPolicyMismatch(const OptimizerPlanCacheKeyInput& cached,
                            const OptimizerPlanCacheKeyInput& requested) {
  return cached.security_epoch != requested.security_epoch ||
         cached.redaction_epoch != requested.redaction_epoch ||
         cached.policy_epoch != requested.policy_epoch ||
         cached.security_policy_digest != requested.security_policy_digest ||
         cached.redaction_route_digest != requested.redaction_route_digest;
}

bool RouteCapabilityMismatch(const OptimizerPlanCacheKeyInput& cached,
                             const OptimizerPlanCacheKeyInput& requested) {
  return cached.route_epoch != requested.route_epoch ||
         cached.executor_capability_set_id != requested.executor_capability_set_id ||
         cached.route_capability_digest != requested.route_capability_digest;
}

bool DependencySetMismatch(const OptimizerPlanCacheKeyInput& cached,
                           const OptimizerPlanCacheKeyInput& requested) {
  return SortedVectorsDiffer(cached.object_uuids, requested.object_uuids) ||
         SortedVectorsDiffer(cached.function_uuids, requested.function_uuids) ||
         SortedVectorsDiffer(cached.index_uuids, requested.index_uuids) ||
         SortedVectorsDiffer(cached.filespace_uuids, requested.filespace_uuids) ||
         SortedVectorsDiffer(cached.dependency_digests, requested.dependency_digests);
}

bool CachedPlanSafeForReuse(const CachedOptimizerPlan& plan) {
  return plan.metadata_only &&
         plan.mga_visibility_recheck_required &&
         plan.security_recheck_required &&
         !plan.parser_or_reference_finality_authority;
}

void AddValidationFailure(OptimizerPlanCacheEnterpriseValidation* result,
                          const std::string& diagnostic_code,
                          const std::string& evidence) {
  if (result == nullptr) return;
  if (result->diagnostic_code.empty() || result->diagnostic_code == kDiagEnterpriseOk) {
    result->diagnostic_code = diagnostic_code;
  }
  result->evidence.push_back(evidence);
  result->ok = false;
}

void AddValidationEvidence(OptimizerPlanCacheEnterpriseValidation* result,
                           std::string evidence) {
  if (result == nullptr) return;
  result->evidence.push_back(std::move(evidence));
}

void AddLookupEvidence(OptimizerPlanCacheLookupResult* result,
                       std::string diagnostic_code,
                       std::string detail) {
  if (result == nullptr) return;
  result->diagnostic_code = std::move(diagnostic_code);
  result->evidence.push_back(std::move(detail));
}

bool PlanMentionsDependency(const CachedOptimizerPlan& plan, const std::string& dependency_uuid) {
  if (dependency_uuid.empty()) { return false; }
  if (plan.cache_key.find(dependency_uuid) != std::string::npos) { return true; }
  for (const auto& candidate : plan.result.candidates) {
    if (CandidateMentions(candidate, dependency_uuid)) { return true; }
  }
  return false;
}

bool EventInvalidatesAll(const OptimizerInvalidationEvent& event) {
  return event.event_kind == "catalog_epoch" ||
         event.event_kind == "security_epoch" ||
         event.event_kind == "redaction_epoch" ||
         event.event_kind == "policy_epoch" ||
         event.event_kind == "stats_epoch" ||
         event.event_kind == "catalog_stats_epoch" ||
         event.event_kind == "stats_refresh" ||
         event.event_kind == "stats_stale" ||
         event.event_kind == "statistics_refresh" ||
         event.event_kind == "security_policy_change" ||
         event.event_kind == "redaction_policy_change" ||
         event.event_kind == "redaction_route_change" ||
         event.event_kind == "route_capability_change" ||
         event.event_kind == "executor_capability_change" ||
         event.event_kind == "metric_epoch_advance" ||
         event.event_kind == "nosql_generation_publish" ||
         event.event_kind == "nosql_generation_retire" ||
         event.event_kind == "nosql_compaction" ||
         event.event_kind == "nosql_family_compaction" ||
         event.event_kind == "memory_policy_change" ||
         event.event_kind == "memory_grant_policy_change" ||
         event.event_kind == "memory_feedback_generation" ||
         event.event_kind == "memory_feedback_publish" ||
         event.event_kind == "optimizer_control_policy_change" ||
         event.event_kind == "optimizer_control_epoch" ||
         event.event_kind == "compatibility_epoch" ||
         event.event_kind == "format_compatibility_epoch" ||
         event.event_kind == "format_change" ||
         event.event_kind == "catalog_create_ambiguous_path" ||
         event.event_kind == "cluster_unavailable";
}

bool EventInvalidatesByDependency(const OptimizerInvalidationEvent& event) {
  return event.event_kind == "catalog_create" ||
         event.event_kind == "catalog_alter" ||
         event.event_kind == "catalog_drop" ||
         event.event_kind == "index_change" ||
         event.event_kind == "policy_change" ||
         event.event_kind == "datatype_descriptor_change" ||
         event.event_kind == "domain_change" ||
         event.event_kind == "collation_change" ||
         event.event_kind == "udr_change" ||
         event.event_kind == "function_change" ||
         event.event_kind == "filespace_profile_change";
}

std::string CanonicalPlanKeyPayload(const OptimizerPlanCacheKeyInput& input) {
  return BuildOptimizerPlanCacheKey(input);
}

std::string CanonicalPersistencePayload(const OptimizerPlanCachePersistenceEnvelope& envelope) {
  std::ostringstream out;
  out << "schema=" << envelope.schema_version
      << "|source=" << envelope.persistence_source
      << "|storage_scope=" << envelope.request.storage_scope_uuid
      << "|principal=" << envelope.request.persisted_by_principal_uuid
      << "|persisted_epoch=" << envelope.request.persisted_epoch
      << "|catalog_epoch=" << envelope.request.catalog_epoch
      << "|stats_epoch=" << envelope.request.stats_epoch
      << "|security_epoch=" << envelope.request.security_epoch
      << "|redaction_epoch=" << envelope.request.redaction_epoch
      << "|policy_epoch=" << envelope.request.policy_epoch
      << "|resource_epoch=" << envelope.request.resource_epoch
      << "|route_epoch=" << envelope.request.route_epoch
      << "|memory_policy_epoch=" << envelope.request.memory_policy_epoch
      << "|memory_feedback_generation=" << envelope.request.memory_feedback_generation
      << "|durable_catalog=" << (envelope.request.durable_catalog_persistence ? 1 : 0)
      << "|mga_committed=" << (envelope.request.mga_transaction_committed ? 1 : 0)
      << "|security_redaction_evidence="
      << (envelope.request.security_redaction_evidence_present ? 1 : 0)
      << "|fixture=" << (envelope.request.fixture_or_test_only ? 1 : 0)
      << "|cluster_projection="
      << (envelope.request.cluster_route_projection_present ? 1 : 0);
  std::vector<std::string> plan_payloads;
  plan_payloads.reserve(envelope.plans.size());
  for (const auto& plan : envelope.plans) {
    std::ostringstream plan_out;
    plan_out << plan.cache_key
             << "|plan_id=" << plan.result.plan_id
             << "|diagnostic=" << plan.result.diagnostic_code
             << "|created_epoch=" << plan.created_epoch
             << "|key=" << CanonicalPlanKeyPayload(plan.key_input);
    plan_payloads.push_back(plan_out.str());
  }
  AppendSorted(out, "plans", std::move(plan_payloads));
  return out.str();
}

OptimizerPlanCacheEnterpriseValidation ValidatePersistenceEnvelope(
    const OptimizerPlanCachePersistenceEnvelope& envelope,
    bool require_digest_match) {
  OptimizerPlanCacheEnterpriseValidation validation;
  validation.ok = true;
  validation.diagnostic_code = kDiagEnterpriseOk;

  if (envelope.schema_version != 1) {
    AddValidationFailure(&validation, kDiagEnterprisePersistenceUnsafe,
                         "enterprise_plan_cache_persistence_schema_unsupported");
  }
  if (envelope.persistence_source != "engine_optimizer_plan_cache_catalog") {
    AddValidationFailure(&validation, kDiagEnterprisePersistenceUnsafe,
                         "enterprise_plan_cache_persistence_source_not_catalog");
  }
  if (envelope.request.storage_scope_uuid.empty()) {
    AddValidationFailure(&validation, kDiagEnterprisePersistenceUnsafe,
                         "enterprise_plan_cache_storage_scope_missing");
  }
  if (envelope.request.persisted_by_principal_uuid.empty()) {
    AddValidationFailure(&validation, kDiagEnterprisePersistenceUnsafe,
                         "enterprise_plan_cache_persisting_principal_missing");
  }
  if (envelope.request.persisted_epoch == 0 ||
      envelope.request.catalog_epoch == 0 ||
      envelope.request.stats_epoch == 0 ||
      envelope.request.security_epoch == 0 ||
      envelope.request.redaction_epoch == 0 ||
      envelope.request.policy_epoch == 0 ||
      envelope.request.resource_epoch == 0 ||
      envelope.request.route_epoch == 0 ||
      envelope.request.memory_policy_epoch == 0 ||
      envelope.request.memory_feedback_generation == 0) {
    AddValidationFailure(&validation, kDiagEnterprisePersistenceUnsafe,
                         "enterprise_plan_cache_persistence_epoch_missing");
  }
  if (!envelope.request.durable_catalog_persistence) {
    AddValidationFailure(&validation, kDiagEnterprisePersistenceUnsafe,
                         "enterprise_plan_cache_not_durable_catalog_persistence");
  }
  if (!envelope.request.mga_transaction_committed) {
    AddValidationFailure(&validation, kDiagEnterprisePersistenceUnsafe,
                         "enterprise_plan_cache_mga_transaction_not_committed");
  }
  if (!envelope.request.security_redaction_evidence_present) {
    AddValidationFailure(&validation, kDiagEnterprisePersistenceUnsafe,
                         "enterprise_plan_cache_security_redaction_evidence_missing");
  }
  if (envelope.request.fixture_or_test_only) {
    AddValidationFailure(&validation, kDiagEnterprisePersistenceUnsafe,
                         "enterprise_plan_cache_fixture_persistence_refused");
  }
  if (envelope.request.cluster_route_projection_present) {
    AddValidationFailure(&validation, kDiagEnterprisePersistenceUnsafe,
                         "enterprise_plan_cache_cluster_projection_refused");
  }

  for (const auto& plan : envelope.plans) {
    auto plan_validation = ValidateEnterpriseCachedOptimizerPlan(plan);
    if (!plan_validation.ok) {
      AddValidationFailure(&validation, plan_validation.diagnostic_code,
                           "enterprise_plan_cache_persisted_plan_invalid:" +
                               plan.result.plan_id);
      validation.evidence.insert(validation.evidence.end(),
                                 plan_validation.evidence.begin(),
                                 plan_validation.evidence.end());
    }
  }

  if (require_digest_match) {
    const auto expected = BuildOptimizerPlanCachePersistenceDigest(envelope);
    if (envelope.envelope_digest_algorithm != "sha256-v1" ||
        envelope.envelope_digest != expected) {
      AddValidationFailure(&validation, kDiagEnterprisePersistenceDigestMismatch,
                           "enterprise_plan_cache_persistence_digest_mismatch");
    }
  }

  if (validation.ok) {
    validation.evidence.push_back("enterprise_plan_cache_persistence_validated=true");
  }
  return validation;
}

}  // namespace

std::string BuildNormalizedOptimizerPolicyControlDigest(
    const scratchbird::engine::planner::OptimizerPolicyMetadata& policy) {
  std::ostringstream out;
  out << "source=" << policy.policy_source_kind
      << "|policy_epoch=" << policy.policy_epoch
      << "|plan_profile=" << policy.normalized_controls.plan_profile_id
      << "|join_search=" << policy.normalized_controls.join_search_policy_id
      << "|memory_policy=" << policy.normalized_controls.memory_policy_id
      << "|spill_policy=" << policy.normalized_controls.spill_policy_id
      << "|parallelism=" << policy.normalized_controls.parallelism_policy_id
      << "|what_if=" << policy.normalized_controls.what_if_policy_id;
  auto controls = policy.normalized_controls.safe_control_ids;
  controls.insert(controls.end(), policy.safe_control_ids.begin(),
                  policy.safe_control_ids.end());
  AppendSorted(out, "safe_controls", std::move(controls));
  return out.str();
}

std::string BuildOptimizerPlanCacheKey(const OptimizerPlanCacheKeyInput& input) {
  std::ostringstream out;
  out << "op=" << input.operation_id
      << "|sblr=" << input.sblr_digest
      << "|desc=" << input.descriptor_set_digest
      << "|stats=" << input.statistics_snapshot_id
      << "|catalog_stats=" << input.catalog_stats_digest
      << "|cost=" << input.cost_profile_id
      << "|exec=" << input.executor_capability_set_id
      << "|route_cap=" << input.route_capability_digest
      << "|security_policy=" << input.security_policy_digest
      << "|redaction_route=" << input.redaction_route_digest
      << "|optimizer_controls=" << input.normalized_optimizer_controls_digest
      << "|param_shape=" << input.parameter_shape_digest
      << "|memory_grant_class=" << input.memory_grant_class
      << "|memory_grant=" << input.memory_grant_digest
      << "|catalog_epoch=" << input.catalog_epoch
      << "|stats_epoch=" << input.stats_epoch
      << "|security_epoch=" << input.security_epoch
      << "|redaction_epoch=" << input.redaction_epoch
      << "|policy_epoch=" << input.policy_epoch
      << "|resource_epoch=" << input.resource_epoch
      << "|name_resolution_epoch=" << input.name_resolution_epoch
      << "|memory_policy_epoch=" << input.memory_policy_epoch
      << "|memory_feedback_generation=" << input.memory_feedback_generation
      << "|compatibility_epoch=" << input.compatibility_epoch
      << "|format_compatibility_epoch=" << input.format_compatibility_epoch
      << "|route_epoch=" << input.route_epoch;
  AppendSorted(out, "objects", input.object_uuids);
  AppendSorted(out, "functions", input.function_uuids);
  AppendSorted(out, "indexes", input.index_uuids);
  AppendSorted(out, "filespaces", input.filespace_uuids);
  AppendSorted(out, "dependency_digests", input.dependency_digests);
  return out.str();
}

OptimizerPlanCacheEnterpriseValidation ValidateEnterpriseOptimizerPlanCacheKeyInput(
    const OptimizerPlanCacheKeyInput& input) {
  OptimizerPlanCacheEnterpriseValidation validation;
  validation.ok = true;
  validation.diagnostic_code = kDiagEnterpriseOk;

  const std::vector<std::pair<const char*, std::string>> required_strings = {
      {"operation_id", input.operation_id},
      {"sblr_digest", input.sblr_digest},
      {"descriptor_set_digest", input.descriptor_set_digest},
      {"statistics_snapshot_id", input.statistics_snapshot_id},
      {"catalog_stats_digest", input.catalog_stats_digest},
      {"cost_profile_id", input.cost_profile_id},
      {"executor_capability_set_id", input.executor_capability_set_id},
      {"route_capability_digest", input.route_capability_digest},
      {"security_policy_digest", input.security_policy_digest},
      {"redaction_route_digest", input.redaction_route_digest},
      {"normalized_optimizer_controls_digest", input.normalized_optimizer_controls_digest},
      {"parameter_shape_digest", input.parameter_shape_digest},
      {"memory_grant_class", input.memory_grant_class},
      {"memory_grant_digest", input.memory_grant_digest},
  };
  for (const auto& [label, value] : required_strings) {
    if (IsPlaceholderPlanCacheValue(value)) {
      AddValidationFailure(&validation, kDiagEnterpriseKeyIncomplete,
                           std::string("enterprise_plan_cache_key_missing_or_placeholder:") +
                               label);
    }
    if (ContainsClusterOrRemoteToken(value)) {
      AddValidationFailure(&validation, kDiagRouteCapabilityMismatch,
                           std::string("enterprise_plan_cache_cluster_route_refused:") +
                               label);
    }
  }

  if (input.catalog_epoch == 0 ||
      input.stats_epoch == 0 ||
      input.security_epoch == 0 ||
      input.redaction_epoch == 0 ||
      input.policy_epoch == 0 ||
      input.resource_epoch == 0 ||
      input.name_resolution_epoch == 0 ||
      input.memory_policy_epoch == 0 ||
      input.memory_feedback_generation == 0 ||
      input.compatibility_epoch == 0 ||
      input.format_compatibility_epoch == 0 ||
      input.route_epoch == 0) {
    AddValidationFailure(&validation, kDiagEnterpriseKeyIncomplete,
                         "enterprise_plan_cache_key_epoch_missing");
  }

  if (input.dependency_digests.empty()) {
    AddValidationFailure(&validation, kDiagEnterpriseKeyIncomplete,
                         "enterprise_plan_cache_dependency_digest_missing");
  }
  if (input.object_uuids.empty() && input.function_uuids.empty() &&
      input.index_uuids.empty() && input.filespace_uuids.empty()) {
    AddValidationFailure(&validation, kDiagEnterpriseKeyIncomplete,
                         "enterprise_plan_cache_dependency_scope_missing");
  }
  for (const auto& digest : input.dependency_digests) {
    if (IsPlaceholderPlanCacheValue(digest)) {
      AddValidationFailure(&validation, kDiagEnterpriseKeyIncomplete,
                           "enterprise_plan_cache_dependency_digest_placeholder");
    }
    if (ContainsClusterOrRemoteToken(digest)) {
      AddValidationFailure(&validation, kDiagRouteCapabilityMismatch,
                           "enterprise_plan_cache_cluster_dependency_refused");
    }
  }

  if (validation.ok) {
    AddValidationEvidence(&validation,
                          "enterprise_plan_cache_key_complete=true");
    AddValidationEvidence(&validation,
                          "enterprise_plan_cache_bind_profile_digest_present=true");
    AddValidationEvidence(&validation,
                          "enterprise_plan_cache_memory_feedback_generation_present=true");
    AddValidationEvidence(&validation,
                          "enterprise_plan_cache_route_capability_digest_present=true");
  }
  return validation;
}

OptimizerPlanCacheEnterpriseValidation ValidateEnterpriseCachedOptimizerPlan(
    const CachedOptimizerPlan& plan) {
  auto validation = ValidateEnterpriseOptimizerPlanCacheKeyInput(plan.key_input);
  if (!validation.ok) return validation;
  validation.diagnostic_code = kDiagEnterpriseOk;

  const auto expected_key = BuildOptimizerPlanCacheKey(plan.key_input);
  if (plan.cache_key != expected_key) {
    AddValidationFailure(&validation, kDiagEnterprisePlanUnsafe,
                         "enterprise_plan_cache_key_mismatch");
  }
  if (!plan.result.ok || plan.result.plan_id.empty() ||
      plan.result.diagnostic_code.empty()) {
    AddValidationFailure(&validation, kDiagEnterprisePlanUnsafe,
                         "enterprise_plan_cache_result_evidence_missing");
  }
  if (!CachedPlanSafeForReuse(plan)) {
    AddValidationFailure(&validation, kDiagAuthorityUnsafe,
                         "enterprise_plan_cache_authority_unsafe");
  }
  if (plan.created_epoch == 0) {
    AddValidationFailure(&validation, kDiagEnterprisePlanUnsafe,
                         "enterprise_plan_cache_created_epoch_missing");
  }
  if (plan.created_epoch > plan.key_input.catalog_epoch) {
    AddValidationFailure(&validation, kDiagEnterprisePlanUnsafe,
                         "enterprise_plan_cache_created_epoch_future");
  }
  for (const auto& candidate : plan.result.candidates) {
    if (CandidateIsClusterPath(candidate)) {
      AddValidationFailure(&validation, kDiagRouteCapabilityMismatch,
                           "enterprise_plan_cache_cluster_candidate_refused");
    }
  }

  if (validation.ok) {
    validation.evidence.push_back("enterprise_plan_cache_plan_safe_for_reuse=true");
    validation.evidence.push_back("mga_visibility_recheck=preserved");
    validation.evidence.push_back("security_authorization_recheck=preserved");
  }
  return validation;
}

std::string BuildOptimizerPlanCachePersistenceDigest(
    const OptimizerPlanCachePersistenceEnvelope& envelope) {
  return "sha256:" + Sha256Hex(CanonicalPersistencePayload(envelope));
}

std::uint64_t EpochOrFallback(std::uint64_t value, std::uint64_t fallback) {
  return value == 0 ? fallback : value;
}

OptimizerPlanCacheKeyInput BuildOptimizerPlanCacheKeyInput(const BoundOptimizerRequest& request,
                                                           std::string cost_profile_id,
                                                           std::vector<std::string> object_uuids,
                                                           std::vector<std::string> function_uuids,
                                                           std::vector<std::string> index_uuids,
                                                           std::vector<std::string> filespace_uuids) {
  OptimizerPlanCacheKeyInput input;
  input.operation_id = request.context.operation_id;
  input.sblr_digest = request.context.sblr_digest;
  input.descriptor_set_digest = request.context.descriptor_set_digest;
  input.statistics_snapshot_id = request.context.statistics_snapshot_id;
  input.catalog_stats_digest = request.context.statistics_snapshot_id;
  input.cost_profile_id = std::move(cost_profile_id);
  input.executor_capability_set_id = request.context.executor_capability_set_id;
  input.route_capability_digest = request.context.executor_capability_set_id;
  input.security_policy_digest = "security_epoch:" + std::to_string(request.context.security_epoch);
  input.redaction_route_digest = "redaction:default";
  input.normalized_optimizer_controls_digest =
      BuildNormalizedOptimizerPolicyControlDigest(request.logical_plan.optimizer_policy);
  input.parameter_shape_digest = "parameters:unbound";
  input.memory_grant_class = "memory:default";
  input.memory_grant_digest = "memory:default";
  input.catalog_epoch = request.context.catalog_epoch;
  input.stats_epoch = EpochOrFallback(request.context.stats_epoch, request.context.catalog_epoch);
  input.security_epoch = request.context.security_epoch;
  input.redaction_epoch = EpochOrFallback(request.context.redaction_epoch,
                                         request.context.security_epoch);
  input.policy_epoch = EpochOrFallback(request.context.policy_epoch,
                                      request.logical_plan.optimizer_policy.policy_epoch);
  input.resource_epoch = EpochOrFallback(request.context.resource_epoch,
                                        input.policy_epoch);
  input.name_resolution_epoch = EpochOrFallback(request.context.name_resolution_epoch,
                                               request.context.catalog_epoch);
  input.memory_policy_epoch = EpochOrFallback(request.context.memory_policy_epoch,
                                             input.policy_epoch);
  input.memory_feedback_generation = request.context.memory_feedback_generation;
  input.compatibility_epoch = request.context.catalog_epoch;
  input.format_compatibility_epoch = request.context.catalog_epoch;
  input.route_epoch = EpochOrFallback(request.context.route_epoch, request.context.catalog_epoch);
  input.object_uuids = std::move(object_uuids);
  input.function_uuids = std::move(function_uuids);
  input.index_uuids = std::move(index_uuids);
  input.filespace_uuids = std::move(filespace_uuids);
  input.dependency_digests = {
      input.descriptor_set_digest,
      input.catalog_stats_digest,
      input.security_policy_digest,
      input.redaction_route_digest,
      input.normalized_optimizer_controls_digest,
      input.memory_grant_digest,
  };
  return input;
}

OptimizerProductionPlanCacheKeyResult BuildProductionOptimizerPlanCacheKeyInput(
    const OptimizerProductionPlanCacheKeyRequest& request) {
  OptimizerProductionPlanCacheKeyResult result;
  const auto request_validation =
      ValidateBoundOptimizerRequest(request.bound_request);
  if (!request_validation.ok || request.parser_or_reference_authority_claimed) {
    result.diagnostic_code =
        "SB_OPTIMIZER_PLAN_CACHE_PRODUCTION_REQUEST_REFUSED";
    result.evidence = request_validation.diagnostics;
    if (request.parser_or_reference_authority_claimed) {
      result.evidence.push_back(
          "production_plan_cache_parser_or_reference_authority_refused");
    }
    return result;
  }
  if (request.cluster_route_requested) {
    result.diagnostic_code = kDiagRouteCapabilityMismatch;
    result.evidence.push_back(
        "production_plan_cache_cluster_route_requires_external_provider");
    return result;
  }
  if (request.compatibility_epoch == 0 ||
      request.format_compatibility_epoch == 0) {
    result.diagnostic_code = kDiagEnterpriseKeyIncomplete;
    result.evidence.push_back(
        "production_plan_cache_compatibility_epoch_missing");
    return result;
  }

  result.input.operation_id = request.bound_request.context.operation_id;
  result.input.sblr_digest = request.bound_request.context.sblr_digest;
  result.input.descriptor_set_digest =
      request.bound_request.context.descriptor_set_digest;
  result.input.statistics_snapshot_id =
      request.bound_request.context.statistics_snapshot_id;
  result.input.catalog_stats_digest = request.catalog_stats_digest;
  result.input.cost_profile_id = request.cost_profile_id;
  result.input.executor_capability_set_id =
      request.bound_request.context.executor_capability_set_id;
  result.input.route_capability_digest = request.route_capability_digest;
  result.input.security_policy_digest = request.security_policy_digest;
  result.input.redaction_route_digest = request.redaction_route_digest;
  result.input.normalized_optimizer_controls_digest =
      BuildNormalizedOptimizerPolicyControlDigest(
          request.bound_request.logical_plan.optimizer_policy);
  result.input.parameter_shape_digest = request.parameter_shape_digest;
  result.input.memory_grant_class = request.memory_grant_class;
  result.input.memory_grant_digest = request.memory_grant_digest;
  result.input.catalog_epoch = request.bound_request.context.catalog_epoch;
  result.input.stats_epoch = request.bound_request.context.stats_epoch;
  result.input.security_epoch = request.bound_request.context.security_epoch;
  result.input.redaction_epoch = request.bound_request.context.redaction_epoch;
  result.input.policy_epoch = request.bound_request.context.policy_epoch;
  result.input.resource_epoch = request.bound_request.context.resource_epoch;
  result.input.name_resolution_epoch =
      request.bound_request.context.name_resolution_epoch;
  result.input.memory_policy_epoch =
      request.bound_request.context.memory_policy_epoch;
  result.input.memory_feedback_generation =
      request.bound_request.context.memory_feedback_generation;
  result.input.compatibility_epoch = request.compatibility_epoch;
  result.input.format_compatibility_epoch =
      request.format_compatibility_epoch;
  result.input.route_epoch = request.bound_request.context.route_epoch;
  result.input.object_uuids = request.object_uuids;
  result.input.function_uuids = request.function_uuids;
  result.input.index_uuids = request.index_uuids;
  result.input.filespace_uuids = request.filespace_uuids;
  result.input.dependency_digests = request.dependency_digests;

  const auto key_validation =
      ValidateEnterpriseOptimizerPlanCacheKeyInput(result.input);
  result.diagnostic_code = key_validation.diagnostic_code;
  result.evidence = key_validation.evidence;
  if (!key_validation.ok) {
    result.ok = false;
    return result;
  }
  result.ok = true;
  result.evidence.push_back("production_plan_cache_key_complete=true");
  result.evidence.push_back("production_plan_cache_sblr_digest_bound=true");
  result.evidence.push_back("production_plan_cache_parameter_shape_digest_bound=true");
  result.evidence.push_back("production_plan_cache_dependency_digests_bound=true");
  result.evidence.push_back("production_plan_cache_redaction_route_digest_bound=true");
  result.evidence.push_back("production_plan_cache_memory_grant_digest_bound=true");
  result.evidence.push_back(
      "production_plan_cache_parser_or_reference_authority=false");
  result.evidence.push_back(
      "production_plan_cache_mga_finality_authority=engine_transaction_inventory");
  return result;
}

void OptimizerPlanCache::Put(CachedOptimizerPlan plan) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (plan.cache_key.empty()) {
    plan.cache_key = BuildOptimizerPlanCacheKey(plan.key_input);
  }
  if (plan.key_input.operation_id.empty() && !plan.cache_key.empty()) {
    plan.key_input.operation_id = plan.result.plan_id;
  }
  plan.valid = true;
  plan.invalidated_by_dependency = false;
  plan.invalidation_diagnostic_code.clear();
  plan.invalidation_event_kind.clear();
  plan.invalidation_dependency_uuid.clear();
  plans_[plan.cache_key] = std::move(plan);
  ++stats_.puts;
}

OptimizerPlanCacheEnterpriseValidation OptimizerPlanCache::PutEnterprise(
    CachedOptimizerPlan plan) {
  if (plan.cache_key.empty()) {
    plan.cache_key = BuildOptimizerPlanCacheKey(plan.key_input);
  }
  const auto validation = ValidateEnterpriseCachedOptimizerPlan(plan);
  if (!validation.ok) return validation;
  Put(std::move(plan));
  return validation;
}

OptimizerPlanCacheEnterpriseValidation OptimizerPlanCache::PutEnterpriseGoverned(
    CachedOptimizerPlan plan,
    OptimizerPlanCacheMemoryGovernanceRequest governance) {
  if (plan.cache_key.empty()) {
    plan.cache_key = BuildOptimizerPlanCacheKey(plan.key_input);
  }
  auto validation = ValidateEnterpriseCachedOptimizerPlan(plan);
  if (!validation.ok) return validation;
  if (governance.governor == nullptr || governance.ledger == nullptr) {
    AddValidationFailure(&validation,
                         "SB_OPTIMIZER_PLAN_CACHE_MEMORY_GOVERNANCE_REQUIRED",
                         "optimizer_plan_cache_memory_governor_and_ledger_required");
    return validation;
  }
  if (governance.estimated_plan_bytes == 0) {
    AddValidationFailure(&validation,
                         "SB_OPTIMIZER_PLAN_CACHE_MEMORY_BYTES_REQUIRED",
                         "optimizer_plan_cache_estimated_plan_bytes_required");
    return validation;
  }
  governance.scope.plan_cache_key = plan.cache_key;
  if (governance.scope.database_id.empty()) {
    AddValidationFailure(&validation,
                         "SB_OPTIMIZER_PLAN_CACHE_MEMORY_SCOPE_REQUIRED",
                         "optimizer_plan_cache_database_scope_required");
    return validation;
  }
  if (governance.scope.session_id.empty()) {
    governance.scope.session_id = "optimizer-plan-cache-global-session";
  }
  if (governance.epochs.catalog_epoch == 0) {
    governance.epochs.catalog_epoch = plan.key_input.catalog_epoch;
  }
  if (governance.epochs.security_epoch == 0) {
    governance.epochs.security_epoch = plan.key_input.security_epoch;
  }
  if (governance.epochs.redaction_epoch == 0) {
    governance.epochs.redaction_epoch = plan.key_input.redaction_epoch;
  }
  if (governance.epochs.policy_epoch == 0) {
    governance.epochs.policy_epoch = plan.key_input.policy_epoch;
  }
  if (governance.epochs.resource_epoch == 0) {
    governance.epochs.resource_epoch = plan.key_input.resource_epoch;
  }
  if (governance.epochs.descriptor_epoch == 0) {
    governance.epochs.descriptor_epoch = plan.key_input.catalog_epoch;
  }
  if (governance.epochs.memory_policy_epoch == 0) {
    governance.epochs.memory_policy_epoch = plan.key_input.memory_policy_epoch;
  }
  if (governance.provenance.source ==
      memory::HierarchicalMemoryBudgetProvenanceSource::unknown) {
    governance.provenance.source =
        memory::HierarchicalMemoryBudgetProvenanceSource::runtime_policy;
    governance.provenance.source_label = "engine.optimizer.plan_cache";
  }

  memory::ResultCursorPlanMemoryLeaseRequest lease;
  lease.surface = memory::ResultCursorPlanMemorySurface::plan_cache_entry;
  lease.ledger = governance.ledger;
  lease.policy = governance.policy;
  lease.scope = governance.scope;
  lease.epochs = governance.epochs;
  lease.provenance = governance.provenance;
  lease.memory_class = "ceic_020.optimizer_plan_cache";
  lease.owner_id = "optimizer.plan_cache:" + plan.cache_key;
  lease.route_label = plan.key_input.operation_id;
  lease.requested_bytes = governance.estimated_plan_bytes;
  lease.cluster_route_requested = governance.cluster_route_requested;
  auto acquired = governance.governor->Acquire(std::move(lease));
  if (!acquired.ok()) {
    AddValidationFailure(&validation,
                         acquired.diagnostic.diagnostic_code.empty()
                             ? "SB_OPTIMIZER_PLAN_CACHE_MEMORY_RESERVATION_REFUSED"
                             : acquired.diagnostic.diagnostic_code,
                         "optimizer_plan_cache_memory_reservation_refused");
    validation.evidence.insert(validation.evidence.end(),
                               acquired.evidence.begin(),
                               acquired.evidence.end());
    return validation;
  }
  plan.memory_governed = true;
  plan.memory_reserved_bytes = governance.estimated_plan_bytes;
  plan.memory_lease_id = acquired.lease_id;
  plan.memory_scope = governance.scope;
  plan.memory_governance_evidence = acquired.evidence;
  std::string prior_lease_id;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto existing = plans_.find(plan.cache_key);
    if (existing != plans_.end() && existing->second.memory_governed &&
        !existing->second.memory_lease_id.empty()) {
      prior_lease_id = existing->second.memory_lease_id;
    }
  }
  if (!prior_lease_id.empty()) {
    auto released_prior = governance.governor->Release(
        prior_lease_id,
        memory::ResultCursorPlanMemoryReleaseReason::eviction);
    validation.evidence.insert(validation.evidence.end(),
                               released_prior.evidence.begin(),
                               released_prior.evidence.end());
    if (!released_prior.status.ok()) {
      (void)governance.governor->Release(
          acquired.lease_id,
          memory::ResultCursorPlanMemoryReleaseReason::explicit_release);
      AddValidationFailure(&validation,
                           released_prior.diagnostic.diagnostic_code.empty()
                               ? "SB_OPTIMIZER_PLAN_CACHE_PRIOR_MEMORY_RELEASE_REFUSED"
                               : released_prior.diagnostic.diagnostic_code,
                           "optimizer_plan_cache_prior_memory_release_refused");
      return validation;
    }
  }
  Put(std::move(plan));
  validation.evidence.push_back("CEIC-020_OPTIMIZER_PLAN_CACHE_MEMORY_GOVERNED");
  validation.evidence.push_back("optimizer_plan_cache_memory_lease_id=" +
                                acquired.lease_id);
  validation.evidence.insert(validation.evidence.end(),
                             acquired.evidence.begin(),
                             acquired.evidence.end());
  return validation;
}

std::optional<CachedOptimizerPlan> OptimizerPlanCache::Get(const std::string& cache_key) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = plans_.find(cache_key);
  if (it == plans_.end() || !it->second.valid) {
    ++stats_.misses;
    return std::nullopt;
  }
  ++stats_.hits;
  return it->second;
}

OptimizerPlanCacheLookupResult OptimizerPlanCache::Lookup(const OptimizerPlanCacheKeyInput& input) {
  std::lock_guard<std::mutex> lock(mutex_);
  OptimizerPlanCacheLookupResult result;
  result.cache_key = BuildOptimizerPlanCacheKey(input);
  auto exact = plans_.find(result.cache_key);
  if (exact != plans_.end() && exact->second.valid) {
    if (!CachedPlanSafeForReuse(exact->second)) {
      ++stats_.misses;
      result.plan = exact->second;
      AddLookupEvidence(&result, kDiagAuthorityUnsafe,
                        "optimizer_plan_cache_authority_unsafe");
      return result;
    }
    ++stats_.hits;
    result.hit = true;
    result.plan = exact->second;
    result.diagnostic_code = kDiagHit;
    result.evidence.push_back("optimizer_plan_cache_hit");
    result.evidence.push_back("cached_plan_metadata_only=true");
    result.evidence.push_back(exact->second.mga_visibility_recheck_required
                                  ? "mga_visibility_recheck=preserved"
                                  : "mga_visibility_recheck=missing");
    result.evidence.push_back(exact->second.security_recheck_required
                                  ? "security_authorization_recheck=preserved"
                                  : "security_authorization_recheck=missing");
    result.evidence.push_back(exact->second.parser_or_reference_finality_authority
                                  ? "parser_or_reference_finality_authority=true"
                                  : "mga_finality_authority=engine_transaction_inventory");
    return result;
  }
  ++stats_.misses;

  if (exact != plans_.end() && !exact->second.valid) {
    result.plan = exact->second;
    AddLookupEvidence(&result,
                      exact->second.invalidation_diagnostic_code.empty()
                          ? kDiagDependencyInvalidated
                          : exact->second.invalidation_diagnostic_code,
                      "optimizer_plan_cache_dependency_invalidation");
    if (!exact->second.invalidation_event_kind.empty()) {
      result.evidence.push_back("invalidation_kind=" + exact->second.invalidation_event_kind);
    }
    if (!exact->second.invalidation_dependency_uuid.empty()) {
      result.evidence.push_back("invalidation_dependency=" + exact->second.invalidation_dependency_uuid);
    }
    return result;
  }

  for (const auto& [key, cached] : plans_) {
    (void)key;
    if (!SameStatementFamily(cached.key_input, input)) continue;
    result.plan = cached;
    if (!CachedPlanSafeForReuse(cached)) {
      AddLookupEvidence(&result, kDiagAuthorityUnsafe,
                        "optimizer_plan_cache_authority_unsafe");
      return result;
    }
    if (!cached.valid || DependencySetMismatch(cached.key_input, input)) {
      AddLookupEvidence(&result, kDiagDependencyInvalidated,
                        "optimizer_plan_cache_dependency_invalidation");
      return result;
    }
    if (StaleEpochOrStatsMismatch(cached.key_input, input)) {
      AddLookupEvidence(&result, kDiagStaleEpoch, "optimizer_plan_cache_stale_epoch");
      return result;
    }
    if (ParameterShapeMismatch(cached.key_input, input)) {
      AddLookupEvidence(&result, kDiagIncompatibleParameterShape,
                        "optimizer_plan_cache_incompatible_parameter_shape");
      return result;
    }
    if (OptimizerControlPolicyMismatch(cached.key_input, input)) {
      AddLookupEvidence(&result, kDiagOptimizerControlPolicyMismatch,
                        "optimizer_plan_cache_optimizer_control_policy_mismatch");
      return result;
    }
    if (MemoryGrantMismatch(cached.key_input, input)) {
      AddLookupEvidence(&result, kDiagMemoryGrantMismatch,
                        "optimizer_plan_cache_memory_grant_mismatch");
      return result;
    }
    if (SecurityPolicyMismatch(cached.key_input, input)) {
      AddLookupEvidence(&result, kDiagSecurityPolicyMismatch,
                        "optimizer_plan_cache_redaction_security_policy_mismatch");
      return result;
    }
    if (RouteCapabilityMismatch(cached.key_input, input)) {
      AddLookupEvidence(&result, kDiagRouteCapabilityMismatch,
                        "optimizer_plan_cache_route_capability_mismatch");
      return result;
    }
  }

  AddLookupEvidence(&result, kDiagMiss, "optimizer_plan_cache_miss");
  return result;
}

OptimizerPlanCacheLookupResult OptimizerPlanCache::LookupEnterprise(
    const OptimizerPlanCacheKeyInput& input) {
  OptimizerPlanCacheLookupResult result;
  const auto key_validation = ValidateEnterpriseOptimizerPlanCacheKeyInput(input);
  if (!key_validation.ok) {
    result.cache_key = BuildOptimizerPlanCacheKey(input);
    result.diagnostic_code = key_validation.diagnostic_code;
    result.evidence = key_validation.evidence;
    return result;
  }

  result = Lookup(input);
  result.evidence.insert(result.evidence.end(),
                         key_validation.evidence.begin(),
                         key_validation.evidence.end());
  if (result.hit && result.plan.has_value()) {
    const auto plan_validation = ValidateEnterpriseCachedOptimizerPlan(*result.plan);
    if (!plan_validation.ok) {
      result.hit = false;
      result.diagnostic_code = plan_validation.diagnostic_code;
      result.evidence.insert(result.evidence.end(),
                             plan_validation.evidence.begin(),
                             plan_validation.evidence.end());
      return result;
    }
    result.evidence.insert(result.evidence.end(),
                           plan_validation.evidence.begin(),
                           plan_validation.evidence.end());
    result.evidence.push_back("OEIC_PLAN_CACHE_ENTERPRISE_CLOSURE");
  }
  return result;
}

std::uint64_t OptimizerPlanCache::Invalidate(const OptimizerInvalidationEvent& event) {
  return InvalidateWithEvidence(event).invalidated_count;
}

OptimizerPlanCacheInvalidationResult OptimizerPlanCache::InvalidateWithEvidence(
    const OptimizerInvalidationEvent& event) {
  std::lock_guard<std::mutex> lock(mutex_);
  OptimizerPlanCacheInvalidationResult result;
  result.diagnostic_code = OptimizerInvalidationDiagnosticCode(event);
  result.evidence.push_back(OptimizerInvalidationEventKindRecognized(event.event_kind)
                                ? "optimizer_plan_cache_invalidation"
                                : "optimizer_plan_cache_unknown_invalidation_kind");
  result.evidence.push_back("invalidation_kind=" + event.event_kind);
  if (!event.dependency_uuid.empty()) {
    result.evidence.push_back("invalidation_dependency=" + event.dependency_uuid);
  }
  for (auto& [key, plan] : plans_) {
    (void)key;
    if (plan.valid && OptimizerPlanDependsOnEvent(plan, event)) {
      plan.valid = false;
      plan.invalidated_by_dependency = true;
      plan.invalidation_diagnostic_code = result.diagnostic_code;
      plan.invalidation_event_kind = event.event_kind;
      plan.invalidation_dependency_uuid = event.dependency_uuid;
      ++result.invalidated_count;
    }
  }
  stats_.invalidations += result.invalidated_count;
  return result;
}

OptimizerPlanCacheInvalidationResult
OptimizerPlanCache::InvalidateWithGovernedMemory(
    const OptimizerInvalidationEvent& event,
    memory::ResultCursorPlanMemoryGovernor* governor) {
  std::vector<std::string> lease_ids;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [_, plan] : plans_) {
      if (plan.valid && plan.memory_governed && !plan.memory_lease_id.empty() &&
          OptimizerPlanDependsOnEvent(plan, event)) {
        lease_ids.push_back(plan.memory_lease_id);
      }
    }
  }
  auto result = InvalidateWithEvidence(event);
  if (governor == nullptr) {
    result.evidence.push_back(
        "CEIC-020_OPTIMIZER_PLAN_CACHE_MEMORY_GOVERNOR_MISSING");
    return result;
  }
  for (const auto& lease_id : lease_ids) {
    auto released = governor->Release(
        lease_id,
        memory::ResultCursorPlanMemoryReleaseReason::epoch_invalidation);
    result.evidence.insert(result.evidence.end(),
                           released.evidence.begin(),
                           released.evidence.end());
    if (!released.status.ok()) {
      result.evidence.push_back(
          "optimizer_plan_cache_memory_release_failed=" +
          released.diagnostic.diagnostic_code);
    }
  }
  result.evidence.push_back("CEIC-020_OPTIMIZER_PLAN_CACHE_MEMORY_RELEASED");
  return result;
}

OptimizerPlanCacheInvalidationResult OptimizerPlanCache::ShrinkGovernedMemory(
    const std::string& database_id,
    std::uint64_t target_bytes,
    memory::ResultCursorPlanMemoryGovernor* governor) {
  OptimizerPlanCacheInvalidationResult result;
  result.diagnostic_code = "SB_OPTIMIZER_PLAN_CACHE_GOVERNED_SHRINK";
  result.evidence.push_back("CEIC-020_OPTIMIZER_PLAN_CACHE_SHRINK");
  if (governor == nullptr) {
    result.diagnostic_code =
        "SB_OPTIMIZER_PLAN_CACHE_MEMORY_GOVERNOR_MISSING";
    result.evidence.push_back("optimizer_plan_cache_memory_governor_missing");
    return result;
  }
  std::vector<CachedOptimizerPlan> candidates;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [_, plan] : plans_) {
      if (plan.valid && plan.memory_governed &&
          plan.memory_scope.database_id == database_id) {
        candidates.push_back(plan);
      }
    }
  }
  std::sort(candidates.begin(),
            candidates.end(),
            [](const auto& left, const auto& right) {
              return left.created_epoch < right.created_epoch;
            });
  std::uint64_t current_bytes = 0;
  for (const auto& candidate : candidates) {
    current_bytes += candidate.memory_reserved_bytes;
  }
  std::uint64_t release_goal =
      current_bytes > target_bytes ? current_bytes - target_bytes : 0;
  std::uint64_t released_bytes = 0;
  for (const auto& candidate : candidates) {
    if (released_bytes >= release_goal) break;
    auto released = governor->Release(
        candidate.memory_lease_id,
        memory::ResultCursorPlanMemoryReleaseReason::shrink);
    result.evidence.insert(result.evidence.end(),
                           released.evidence.begin(),
                           released.evidence.end());
    if (released.status.ok()) {
      released_bytes += released.released_bytes;
      ++result.invalidated_count;
      std::lock_guard<std::mutex> lock(mutex_);
      auto found = plans_.find(candidate.cache_key);
      if (found != plans_.end()) {
        found->second.valid = false;
        found->second.invalidated_by_dependency = true;
        found->second.invalidation_diagnostic_code =
            "SB_OPTIMIZER_PLAN_CACHE_GOVERNED_MEMORY_SHRINK";
        found->second.invalidation_event_kind = "memory_governed_shrink";
      }
    }
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.invalidations += result.invalidated_count;
  }
  result.evidence.push_back("optimizer_plan_cache_shrink_released_bytes=" +
                            std::to_string(released_bytes));
  return result;
}

OptimizerPlanCachePersistenceEnvelope OptimizerPlanCache::ExportPersistenceEnvelope(
    const OptimizerPlanCachePersistenceRequest& request) const {
  OptimizerPlanCachePersistenceEnvelope envelope;
  envelope.request = request;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [key, plan] : plans_) {
      (void)key;
      if (plan.valid) envelope.plans.push_back(plan);
    }
  }
  auto validation = ValidatePersistenceEnvelope(envelope, false);
  envelope.ok = validation.ok;
  envelope.diagnostic_code = validation.diagnostic_code;
  envelope.evidence = validation.evidence;
  if (envelope.ok) {
    envelope.envelope_digest = BuildOptimizerPlanCachePersistenceDigest(envelope);
    envelope.evidence.push_back("enterprise_plan_cache_persistence_digest=" +
                                envelope.envelope_digest);
    envelope.evidence.push_back("OEIC_PLAN_CACHE_ENTERPRISE_CLOSURE");
  }
  return envelope;
}

OptimizerPlanCacheEnterpriseValidation OptimizerPlanCache::ImportPersistenceEnvelope(
    const OptimizerPlanCachePersistenceEnvelope& envelope) {
  auto validation = ValidatePersistenceEnvelope(envelope, true);
  if (!validation.ok) return validation;

  std::lock_guard<std::mutex> lock(mutex_);
  plans_.clear();
  for (auto plan : envelope.plans) {
    plan.valid = true;
    plan.invalidated_by_dependency = false;
    plan.invalidation_diagnostic_code.clear();
    plan.invalidation_event_kind.clear();
    plan.invalidation_dependency_uuid.clear();
    plans_[plan.cache_key] = std::move(plan);
  }
  stats_.puts += plans_.size();
  validation.evidence.push_back("enterprise_plan_cache_persistence_imported=true");
  validation.evidence.push_back("OEIC_PLAN_CACHE_ENTERPRISE_CLOSURE");
  return validation;
}

void OptimizerPlanCache::Clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto count = plans_.size();
  plans_.clear();
  stats_.invalidations += count;
}

OptimizerPlanCacheStats OptimizerPlanCache::Stats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return stats_;
}

bool OptimizerPlanDependsOnEvent(const CachedOptimizerPlan& plan, const OptimizerInvalidationEvent& event) {
  if (!plan.valid) return false;
  if (!OptimizerInvalidationEventKindRecognized(event.event_kind)) return true;
  if (EventInvalidatesAll(event)) {
    if (event.event_kind != "cluster_unavailable") { return true; }
    for (const auto& candidate : plan.result.candidates) {
      if (CandidateIsClusterPath(candidate)) { return true; }
    }
    return plan.cache_key.find("cluster") != std::string::npos || plan.cache_key.find("remote") != std::string::npos;
  }
  if (EventInvalidatesByDependency(event)) {
    return event.dependency_uuid.empty() || PlanMentionsDependency(plan, event.dependency_uuid);
  }
  if (event.dependency_uuid.empty()) { return false; }
  return PlanMentionsDependency(plan, event.dependency_uuid);
}

bool OptimizerInvalidationEventKindRecognized(const std::string& event_kind) {
  return event_kind == "catalog_epoch" ||
         event_kind == "security_epoch" ||
         event_kind == "redaction_epoch" ||
         event_kind == "policy_epoch" ||
         event_kind == "stats_epoch" ||
         event_kind == "catalog_stats_epoch" ||
         event_kind == "stats_refresh" ||
         event_kind == "stats_stale" ||
         event_kind == "catalog_create" ||
         event_kind == "catalog_create_ambiguous_path" ||
         event_kind == "catalog_alter" ||
         event_kind == "catalog_drop" ||
         event_kind == "index_change" ||
         event_kind == "statistics_refresh" ||
         event_kind == "policy_change" ||
         event_kind == "security_policy_change" ||
         event_kind == "redaction_policy_change" ||
         event_kind == "redaction_route_change" ||
         event_kind == "datatype_descriptor_change" ||
         event_kind == "domain_change" ||
         event_kind == "collation_change" ||
         event_kind == "udr_change" ||
         event_kind == "function_change" ||
         event_kind == "filespace_profile_change" ||
         event_kind == "memory_policy_change" ||
         event_kind == "memory_grant_policy_change" ||
         event_kind == "memory_feedback_generation" ||
         event_kind == "memory_feedback_publish" ||
         event_kind == "optimizer_control_policy_change" ||
         event_kind == "optimizer_control_epoch" ||
         event_kind == "route_capability_change" ||
         event_kind == "executor_capability_change" ||
         event_kind == "compatibility_epoch" ||
         event_kind == "format_compatibility_epoch" ||
         event_kind == "format_change" ||
         event_kind == "metric_epoch_advance" ||
         event_kind == "nosql_generation_publish" ||
         event_kind == "nosql_generation_retire" ||
         event_kind == "nosql_compaction" ||
         event_kind == "nosql_family_compaction" ||
         event_kind == "cluster_unavailable";
}

std::string OptimizerInvalidationDiagnosticCode(const OptimizerInvalidationEvent& event) {
  if (!OptimizerInvalidationEventKindRecognized(event.event_kind)) {
    return kDiagUnknownInvalidationKind;
  }
  if (event.event_kind == "security_epoch" ||
      event.event_kind == "redaction_epoch" ||
      event.event_kind == "policy_epoch" ||
      event.event_kind == "policy_change" ||
      event.event_kind == "security_policy_change" ||
      event.event_kind == "redaction_policy_change" ||
      event.event_kind == "redaction_route_change") {
    return kDiagSecurityPolicyMismatch;
  }
  if (event.event_kind == "memory_policy_change" ||
      event.event_kind == "memory_grant_policy_change" ||
      event.event_kind == "memory_feedback_generation" ||
      event.event_kind == "memory_feedback_publish") {
    return kDiagMemoryGrantMismatch;
  }
  if (event.event_kind == "optimizer_control_policy_change" ||
      event.event_kind == "optimizer_control_epoch") {
    return kDiagOptimizerControlPolicyMismatch;
  }
  if (event.event_kind == "route_capability_change" ||
      event.event_kind == "executor_capability_change" ||
      event.event_kind == "cluster_unavailable") {
    return kDiagRouteCapabilityMismatch;
  }
  if (event.event_kind == "catalog_epoch" ||
      event.event_kind == "stats_epoch" ||
      event.event_kind == "catalog_stats_epoch" ||
      event.event_kind == "stats_refresh" ||
      event.event_kind == "stats_stale" ||
      event.event_kind == "statistics_refresh" ||
      event.event_kind == "metric_epoch_advance" ||
      event.event_kind == "nosql_generation_publish" ||
      event.event_kind == "nosql_generation_retire" ||
      event.event_kind == "nosql_compaction" ||
      event.event_kind == "nosql_family_compaction" ||
      event.event_kind == "compatibility_epoch" ||
      event.event_kind == "format_compatibility_epoch" ||
      event.event_kind == "format_change") {
    return kDiagStaleEpoch;
  }
  return kDiagDependencyInvalidated;
}

OptimizerInvalidationEvent OptimizerInvalidationEventForMutation(std::string mutation_source,
                                                                 std::string dependency_uuid,
                                                                 std::uint64_t event_epoch) {
  OptimizerInvalidationEvent event;
  event.dependency_uuid = std::move(dependency_uuid);
  event.event_epoch = event_epoch;
  if (mutation_source == "catalog_object_create") {
    event.event_kind = event.dependency_uuid.empty() ? "catalog_create_ambiguous_path" : "catalog_create";
  } else if (mutation_source == "catalog_object_alter") {
    event.event_kind = "catalog_alter";
  } else if (mutation_source == "catalog_object_drop") {
    event.event_kind = "catalog_drop";
  } else if (mutation_source == "index_create" || mutation_source == "index_alter" || mutation_source == "index_drop") {
    event.event_kind = "index_change";
  } else if (mutation_source == "statistics_refresh" || mutation_source == "stats_refresh") {
    event.event_kind = "statistics_refresh";
  } else if (mutation_source == "stats_stale") {
    event.event_kind = "stats_stale";
  } else if (mutation_source == "policy_mutation") {
    event.event_kind = "policy_change";
  } else if (mutation_source == "security_policy_mutation") {
    event.event_kind = "security_policy_change";
  } else if (mutation_source == "redaction_policy_mutation") {
    event.event_kind = "redaction_policy_change";
  } else if (mutation_source == "redaction_route_mutation") {
    event.event_kind = "redaction_route_change";
  } else if (mutation_source == "datatype_descriptor_mutation") {
    event.event_kind = "datatype_descriptor_change";
  } else if (mutation_source == "domain_mutation") {
    event.event_kind = "domain_change";
  } else if (mutation_source == "collation_mutation") {
    event.event_kind = "collation_change";
  } else if (mutation_source == "udr_registration" || mutation_source == "udr_alter" || mutation_source == "udr_drop") {
    event.event_kind = "udr_change";
  } else if (mutation_source == "function_create" || mutation_source == "function_alter" ||
             mutation_source == "function_drop") {
    event.event_kind = "function_change";
  } else if (mutation_source == "filespace_profile_mutation") {
    event.event_kind = "filespace_profile_change";
  } else if (mutation_source == "memory_policy_mutation") {
    event.event_kind = "memory_policy_change";
  } else if (mutation_source == "memory_grant_policy_mutation") {
    event.event_kind = "memory_grant_policy_change";
  } else if (mutation_source == "memory_feedback_publication" ||
             mutation_source == "memory_feedback_generation") {
    event.event_kind = "memory_feedback_publish";
  } else if (mutation_source == "optimizer_control_policy_mutation" ||
             mutation_source == "optimizer_control_epoch_advance") {
    event.event_kind = "optimizer_control_policy_change";
  } else if (mutation_source == "route_capability_mutation") {
    event.event_kind = "route_capability_change";
  } else if (mutation_source == "executor_capability_mutation") {
    event.event_kind = "executor_capability_change";
  } else if (mutation_source == "compatibility_epoch_advance") {
    event.event_kind = "compatibility_epoch";
  } else if (mutation_source == "format_compatibility_epoch_advance") {
    event.event_kind = "format_compatibility_epoch";
  } else if (mutation_source == "format_mutation") {
    event.event_kind = "format_change";
  } else if (mutation_source == "metric_epoch_advance") {
    event.event_kind = "metric_epoch_advance";
  } else if (mutation_source == "nosql_generation_publication" ||
             mutation_source == "nosql_generation_publish") {
    event.event_kind = "nosql_generation_publish";
  } else if (mutation_source == "nosql_generation_retirement" ||
             mutation_source == "nosql_generation_retire") {
    event.event_kind = "nosql_generation_retire";
  } else if (mutation_source == "nosql_compaction" ||
             mutation_source == "nosql_family_compaction") {
    event.event_kind = "nosql_compaction";
  } else if (mutation_source == "cluster_route_or_metric_projection_change") {
    event.event_kind = "cluster_unavailable";
  } else {
    event.event_kind = std::move(mutation_source);
  }
  return event;
}

}  // namespace scratchbird::engine::optimizer
