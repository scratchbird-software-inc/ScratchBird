// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "optimizer_request.hpp"
#include "result_cursor_plan_memory_governance.hpp"

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

namespace memory = scratchbird::core::memory;

// SEARCH_KEY: SB_OPTIMIZER_PLAN_CACHE_KEY
struct OptimizerPlanCacheKeyInput {
  std::string operation_id;
  std::string sblr_digest;
  std::string descriptor_set_digest;
  std::string statistics_snapshot_id;
  std::string catalog_stats_digest;
  std::string cost_profile_id;
  std::string executor_capability_set_id;
  std::string route_capability_digest;
  std::string security_policy_digest;
  std::string redaction_route_digest;
  std::string normalized_optimizer_controls_digest;
  std::string parameter_shape_digest;
  std::string memory_grant_class;
  std::string memory_grant_digest;
  std::uint64_t catalog_epoch = 0;
  std::uint64_t stats_epoch = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t redaction_epoch = 0;
  std::uint64_t policy_epoch = 0;
  std::uint64_t resource_epoch = 0;
  std::uint64_t name_resolution_epoch = 0;
  std::uint64_t memory_policy_epoch = 0;
  std::uint64_t memory_feedback_generation = 0;
  std::uint64_t compatibility_epoch = 0;
  std::uint64_t format_compatibility_epoch = 0;
  std::uint64_t route_epoch = 0;
  std::vector<std::string> object_uuids;
  std::vector<std::string> function_uuids;
  std::vector<std::string> index_uuids;
  std::vector<std::string> filespace_uuids;
  std::vector<std::string> dependency_digests;
};

struct CachedOptimizerPlan {
  std::string cache_key;
  OptimizerPlanCacheKeyInput key_input;
  BoundOptimizerResult result;
  std::uint64_t created_epoch = 0;
  bool valid = true;
  bool invalidated_by_dependency = false;
  std::string invalidation_diagnostic_code;
  std::string invalidation_event_kind;
  std::string invalidation_dependency_uuid;
  bool metadata_only = true;
  bool mga_visibility_recheck_required = true;
  bool security_recheck_required = true;
  bool parser_or_reference_finality_authority = false;
  bool memory_governed = false;
  std::uint64_t memory_reserved_bytes = 0;
  std::string memory_lease_id;
  memory::ResultCursorPlanMemoryScope memory_scope;
  std::vector<std::string> memory_governance_evidence;
};

struct OptimizerInvalidationEvent {
  std::string event_kind;
  std::string dependency_uuid;
  std::uint64_t event_epoch = 0;
};

struct OptimizerPlanCacheStats {
  std::uint64_t puts = 0;
  std::uint64_t hits = 0;
  std::uint64_t misses = 0;
  std::uint64_t invalidations = 0;
};

struct OptimizerPlanCacheLookupResult {
  bool hit = false;
  std::string cache_key;
  std::string diagnostic_code;
  std::vector<std::string> evidence;
  std::optional<CachedOptimizerPlan> plan;
};

struct OptimizerPlanCacheInvalidationResult {
  std::uint64_t invalidated_count = 0;
  std::string diagnostic_code;
  std::vector<std::string> evidence;
};

// SEARCH_KEY: OEIC_PLAN_CACHE_ENTERPRISE_CLOSURE
struct OptimizerPlanCacheEnterpriseValidation {
  bool ok = false;
  std::string diagnostic_code;
  std::vector<std::string> evidence;
};

struct OptimizerPlanCacheMemoryGovernanceRequest {
  memory::ResultCursorPlanMemoryGovernor* governor = nullptr;
  memory::HierarchicalMemoryBudgetLedger* ledger = nullptr;
  memory::ResultCursorPlanMemoryPolicy policy;
  memory::ResultCursorPlanMemoryScope scope;
  memory::ResultCursorPlanMemoryEpochs epochs;
  memory::HierarchicalMemoryBudgetProvenance provenance;
  std::uint64_t estimated_plan_bytes = 0;
  bool cluster_route_requested = false;
};

struct OptimizerPlanCachePersistenceRequest {
  std::string storage_scope_uuid;
  std::string persisted_by_principal_uuid;
  std::uint64_t persisted_epoch = 0;
  std::uint64_t catalog_epoch = 0;
  std::uint64_t stats_epoch = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t redaction_epoch = 0;
  std::uint64_t policy_epoch = 0;
  std::uint64_t resource_epoch = 0;
  std::uint64_t route_epoch = 0;
  std::uint64_t memory_policy_epoch = 0;
  std::uint64_t memory_feedback_generation = 0;
  bool durable_catalog_persistence = true;
  bool mga_transaction_committed = true;
  bool security_redaction_evidence_present = true;
  bool fixture_or_test_only = false;
  bool cluster_route_projection_present = false;
};

struct OptimizerPlanCachePersistenceEnvelope {
  std::uint32_t schema_version = 1;
  std::string persistence_source = "engine_optimizer_plan_cache_catalog";
  OptimizerPlanCachePersistenceRequest request;
  std::vector<CachedOptimizerPlan> plans;
  std::string envelope_digest_algorithm = "sha256-v1";
  std::string envelope_digest;
  bool ok = false;
  std::string diagnostic_code;
  std::vector<std::string> evidence;
};

// SEARCH_KEY: SB_OPTIMIZER_PRODUCTION_PLAN_CACHE_KEY_BUILDER
// Production cache keys require caller-supplied route, redaction, parameter,
// memory, dependency, and cost-profile digests. The compatibility builder is
// intentionally rejected by enterprise validation when it falls back to local
// defaults or unbound parameter placeholders.
struct OptimizerProductionPlanCacheKeyRequest {
  BoundOptimizerRequest bound_request;
  std::string catalog_stats_digest;
  std::string cost_profile_id;
  std::string route_capability_digest;
  std::string security_policy_digest;
  std::string redaction_route_digest;
  std::string parameter_shape_digest;
  std::string memory_grant_class;
  std::string memory_grant_digest;
  std::uint64_t compatibility_epoch = 0;
  std::uint64_t format_compatibility_epoch = 0;
  std::vector<std::string> object_uuids;
  std::vector<std::string> function_uuids;
  std::vector<std::string> index_uuids;
  std::vector<std::string> filespace_uuids;
  std::vector<std::string> dependency_digests;
  bool cluster_route_requested = false;
  bool parser_or_reference_authority_claimed = false;
};

struct OptimizerProductionPlanCacheKeyResult {
  bool ok = false;
  std::string diagnostic_code;
  OptimizerPlanCacheKeyInput input;
  std::vector<std::string> evidence;
};

class OptimizerPlanCache {
 public:
  void Put(CachedOptimizerPlan plan);
  OptimizerPlanCacheEnterpriseValidation PutEnterprise(CachedOptimizerPlan plan);
  OptimizerPlanCacheEnterpriseValidation PutEnterpriseGoverned(
      CachedOptimizerPlan plan,
      OptimizerPlanCacheMemoryGovernanceRequest governance);
  std::optional<CachedOptimizerPlan> Get(const std::string& cache_key);
  OptimizerPlanCacheLookupResult Lookup(const OptimizerPlanCacheKeyInput& input);
  OptimizerPlanCacheLookupResult LookupEnterprise(const OptimizerPlanCacheKeyInput& input);
  std::uint64_t Invalidate(const OptimizerInvalidationEvent& event);
  OptimizerPlanCacheInvalidationResult InvalidateWithEvidence(const OptimizerInvalidationEvent& event);
  OptimizerPlanCacheInvalidationResult InvalidateWithGovernedMemory(
      const OptimizerInvalidationEvent& event,
      memory::ResultCursorPlanMemoryGovernor* governor);
  OptimizerPlanCacheInvalidationResult ShrinkGovernedMemory(
      const std::string& database_id,
      std::uint64_t target_bytes,
      memory::ResultCursorPlanMemoryGovernor* governor);
  OptimizerPlanCachePersistenceEnvelope ExportPersistenceEnvelope(
      const OptimizerPlanCachePersistenceRequest& request) const;
  OptimizerPlanCacheEnterpriseValidation ImportPersistenceEnvelope(
      const OptimizerPlanCachePersistenceEnvelope& envelope);
  void Clear();
  OptimizerPlanCacheStats Stats() const;

 private:
  mutable std::mutex mutex_;
  std::map<std::string, CachedOptimizerPlan> plans_;
  OptimizerPlanCacheStats stats_;
};

std::string BuildOptimizerPlanCacheKey(const OptimizerPlanCacheKeyInput& input);
std::string BuildNormalizedOptimizerPolicyControlDigest(
    const scratchbird::engine::planner::OptimizerPolicyMetadata& policy);
OptimizerPlanCacheKeyInput BuildOptimizerPlanCacheKeyInput(const BoundOptimizerRequest& request,
                                                           std::string cost_profile_id,
                                                           std::vector<std::string> object_uuids = {},
                                                           std::vector<std::string> function_uuids = {},
                                                           std::vector<std::string> index_uuids = {},
                                                           std::vector<std::string> filespace_uuids = {});
OptimizerProductionPlanCacheKeyResult BuildProductionOptimizerPlanCacheKeyInput(
    const OptimizerProductionPlanCacheKeyRequest& request);
bool OptimizerPlanDependsOnEvent(const CachedOptimizerPlan& plan, const OptimizerInvalidationEvent& event);
bool OptimizerInvalidationEventKindRecognized(const std::string& event_kind);
std::string OptimizerInvalidationDiagnosticCode(const OptimizerInvalidationEvent& event);
OptimizerInvalidationEvent OptimizerInvalidationEventForMutation(std::string mutation_source,
                                                                 std::string dependency_uuid,
                                                                 std::uint64_t event_epoch);
OptimizerPlanCacheEnterpriseValidation ValidateEnterpriseOptimizerPlanCacheKeyInput(
    const OptimizerPlanCacheKeyInput& input);
OptimizerPlanCacheEnterpriseValidation ValidateEnterpriseCachedOptimizerPlan(
    const CachedOptimizerPlan& plan);
std::string BuildOptimizerPlanCachePersistenceDigest(
    const OptimizerPlanCachePersistenceEnvelope& envelope);

}  // namespace scratchbird::engine::optimizer
