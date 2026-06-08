// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "result_cursor_plan_memory_governance.hpp"
#include "runtime_consumption_evidence.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace scratchbird::engine::executor {

namespace memory = scratchbird::core::memory;

// SEARCH_KEY: SB_EXECUTOR_PREPARED_TEMPLATE_METADATA_ONLY
// Prepared execution templates cache descriptor and slot metadata only.
// MGA visibility/finality and authorization remain engine-owned bind-time
// checks against the current request context and epochs.

struct PreparedTemplateEpochs {
  scratchbird::engine::internal_api::EngineApiU64 catalog_epoch = 0;
  scratchbird::engine::internal_api::EngineApiU64 security_epoch = 0;
  scratchbird::engine::internal_api::EngineApiU64 policy_resource_epoch = 0;
  scratchbird::engine::internal_api::EngineApiU64 name_resolution_epoch = 0;
  scratchbird::engine::internal_api::EngineApiU64 transaction_visibility_epoch = 0;
  bool transaction_visibility_epoch_relevant = false;
};

struct PreparedDescriptorSlot {
  std::string stable_name;
  scratchbird::engine::internal_api::EngineDescriptor descriptor;
  std::uint32_t ordinal = 0;
};

struct PreparedFieldOffset {
  std::string descriptor_slot;
  std::string field_name;
  std::size_t byte_offset = 0;
  std::size_t byte_width = 0;
};

struct PreparedResultShapeDescriptor {
  std::string result_kind;
  std::vector<PreparedDescriptorSlot> columns;
  std::string digest;
};

struct PreparedPredicateSlot {
  std::string stable_name;
  std::string descriptor_slot;
  bool required = true;
};

struct PreparedParameterSlot {
  std::string stable_name;
  scratchbird::engine::internal_api::EngineDescriptor descriptor;
  std::uint32_t ordinal = 0;
  bool required = true;
};

struct PreparedIndexDescriptor {
  std::string index_uuid;
  std::string relation_uuid;
  std::string descriptor_digest;
  std::vector<std::string> key_column_uuids;
  std::vector<std::string> covered_column_uuids;
  bool visibility_native = false;
};

struct PreparedPinnedDescriptorReference {
  std::string cache_key;
  std::string descriptor_uuid;
  std::string object_uuid;
  std::string index_uuid;
  std::string descriptor_set_digest;
  std::uint64_t catalog_epoch = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t resource_policy_epoch = 0;
  std::uint64_t name_resolution_epoch = 0;
  std::uint64_t stats_epoch = 0;
  std::string security_policy_identity;
  std::string redaction_policy_identity;
  bool read_only_snapshot = true;
  bool security_recheck_required = true;
  bool visibility_recheck_required = true;
  bool finality_authority_cached = false;
};

struct PreparedSecurityVisibilityPolicyMetadata {
  std::string security_policy_digest;
  std::string visibility_policy_digest;
  std::string authorization_policy_digest;
  bool requires_security_context = true;
  bool requires_transaction_context = false;
  bool cached_metadata_only = true;
  bool security_recheck_required = true;
  bool visibility_recheck_required = true;
  bool finality_authority_cached = false;
};

struct PreparedTemplateKey {
  std::string operation_id;
  std::string sblr_digest_or_trace_key;
  std::string descriptor_set_digest;
  std::string pinned_descriptor_set_digest;
  std::string result_shape_digest;
  PreparedTemplateEpochs epochs;
  std::vector<std::string> dependency_uuids;
};

struct PreparedExecutionTemplate {
  std::string template_id;
  PreparedTemplateKey key;
  std::vector<PreparedDescriptorSlot> descriptor_slots;
  std::vector<PreparedFieldOffset> field_offsets;
  PreparedResultShapeDescriptor result_shape;
  std::vector<PreparedPredicateSlot> predicate_slots;
  std::vector<PreparedParameterSlot> parameter_slots;
  std::vector<PreparedIndexDescriptor> index_descriptors;
  std::vector<PreparedPinnedDescriptorReference> pinned_descriptors;
  PreparedSecurityVisibilityPolicyMetadata policy_metadata;
  bool memory_governed = false;
  std::uint64_t memory_reserved_bytes = 0;
  std::string prepared_memory_lease_id;
  std::string descriptor_snapshot_memory_lease_id;
  memory::ResultCursorPlanMemoryScope memory_scope;
  memory::ResultCursorPlanMemoryEpochs memory_epochs;
  std::vector<std::string> memory_governance_evidence;
};

struct PreparedTemplateAdmission {
  PreparedTemplateKey key;
  std::vector<PreparedDescriptorSlot> descriptor_slots;
  std::vector<PreparedFieldOffset> field_offsets;
  PreparedResultShapeDescriptor result_shape;
  std::vector<PreparedPredicateSlot> predicate_slots;
  std::vector<PreparedParameterSlot> parameter_slots;
  std::vector<PreparedIndexDescriptor> index_descriptors;
  std::vector<PreparedPinnedDescriptorReference> pinned_descriptors;
  PreparedSecurityVisibilityPolicyMetadata policy_metadata;
};

struct PreparedTemplatePrepareResult {
  bool ok = false;
  bool reused_existing_template = false;
  std::string diagnostic_code;
  std::string detail;
  std::shared_ptr<const PreparedExecutionTemplate> prepared_template;
};

struct PreparedTemplateMemoryGovernanceRequest {
  memory::ResultCursorPlanMemoryGovernor* governor = nullptr;
  memory::HierarchicalMemoryBudgetLedger* ledger = nullptr;
  memory::ResultCursorPlanMemoryPolicy policy;
  memory::ResultCursorPlanMemoryScope scope;
  memory::ResultCursorPlanMemoryEpochs epochs;
  memory::HierarchicalMemoryBudgetProvenance provenance;
  std::uint64_t estimated_template_bytes = 0;
  std::uint64_t estimated_descriptor_snapshot_bytes = 0;
  bool cluster_route_requested = false;
};

struct PreparedTemplateBindContext {
  scratchbird::engine::internal_api::EngineRequestContext engine_context;
  scratchbird::engine::internal_api::EngineApiRequest request;
  std::string descriptor_set_digest;
  std::string result_shape_digest;
  std::vector<std::string> dependency_uuids;
  std::vector<std::string> available_predicate_slots;
  std::vector<std::string> available_parameter_slots;
};

struct PreparedTemplateBindResult {
  bool ok = false;
  std::string diagnostic_code;
  std::string detail;
  std::vector<std::string> evidence;
  std::shared_ptr<const PreparedExecutionTemplate> prepared_template;
};

// SEARCH_KEY: ORH_FIXED_ROUTE_OVERHEAD_REMOVAL
// Route-facing adapter for warmed prepared-template execution evidence. This
// observes actual prepare/bind/cache results and never executes SQL or owns
// transaction finality.
struct PreparedRouteOverheadObservation {
  std::string route_kind;
  std::string statement_family;
  std::string selected_path;
  bool benchmark_clean_candidate = false;
  const PreparedTemplatePrepareResult* prepare_result = nullptr;
  const PreparedTemplateBindResult* bind_result = nullptr;
  bool lowered_sblr_reused = false;
  bool text_rendering_suppressed = false;
  std::uint64_t repeated_parse_count = 0;
  std::uint64_t repeated_lower_count = 0;
  std::uint64_t repeated_descriptor_build_count = 0;
  std::uint64_t repeated_result_shape_build_count = 0;
  std::uint64_t repeated_text_render_count = 0;
  std::uint64_t route_latency_budget_us = 0;
  std::uint64_t route_latency_observed_us = 0;
  bool index_dependent = false;
  bool index_correctness_proven = false;
  bool parser_or_cache_executes_sql = false;
  bool parser_or_cache_owns_transaction_finality = false;
  std::string transaction_authority = "engine.mga.transaction_inventory";
  scratchbird::engine::optimizer::RuntimeOptimizedPathEvidence runtime_evidence;
  std::string fallback_reason;
  std::string diagnostic_code;
};

std::string PreparedTemplateCanonicalKey(const PreparedTemplateKey& key);
std::string PreparedTemplateStableDigest(const std::vector<std::string>& parts);
std::string PreparedDescriptorSetDigest(
    const std::vector<scratchbird::engine::internal_api::EngineDescriptor>& descriptors,
    const std::vector<scratchbird::engine::internal_api::EngineColumnDefinition>& columns);
std::string PreparedResultShapeDigest(const PreparedResultShapeDescriptor& result_shape);
std::string PreparedDependencyDigest(std::vector<std::string> dependency_uuids);
std::string PreparedPinnedDescriptorDigest(
    const std::vector<PreparedPinnedDescriptorReference>& pinned_descriptors);

class PreparedTemplateCache {
 public:
  PreparedTemplatePrepareResult Prepare(PreparedTemplateAdmission admission);
  PreparedTemplatePrepareResult PrepareGoverned(
      PreparedTemplateAdmission admission,
      PreparedTemplateMemoryGovernanceRequest governance);
  std::shared_ptr<const PreparedExecutionTemplate> Lookup(const PreparedTemplateKey& key) const;
  PreparedTemplateBindResult Bind(const PreparedExecutionTemplate& prepared_template,
                                  const PreparedTemplateBindContext& bind_context) const;
  PreparedTemplateBindResult LookupAndBind(const PreparedTemplateKey& key,
                                           const PreparedTemplateBindContext& bind_context) const;
  std::uint64_t InvalidateGovernedByEpoch(
      const memory::ResultCursorPlanMemoryEpochs& current_epochs,
      memory::ResultCursorPlanMemoryGovernor* governor);

 private:
  mutable std::mutex mutex_;
  std::map<std::string, std::shared_ptr<PreparedExecutionTemplate>> templates_;
};

scratchbird::engine::optimizer::FixedRouteOverheadEvidence
BuildFixedRouteOverheadEvidenceFromPreparedRoute(
    const PreparedRouteOverheadObservation& observation);

}  // namespace scratchbird::engine::executor
