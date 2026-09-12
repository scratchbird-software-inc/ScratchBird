// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "descriptor_value_runtime.hpp"
#include "result_cursor_plan_memory_governance.hpp"
#include "runtime_consumption_evidence.hpp"

#include <cstddef>
#include <atomic>
#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace scratchbird::engine::executor {

namespace memory = scratchbird::core::memory;
using PreparedUuid = internal_api::EngineUuid;

enum class PreparedTemplateFailureKind { kNone, kAdmission, kAllocation, kContentHash, kIdentity };
class PreparedContentHashFailure final : public std::exception {
 public:
  const char* what() const noexcept override { return "prepared content SHA-256 calculation failed"; }
};

// SEARCH_KEY: SB_EXECUTOR_PREPARED_TEMPLATE_METADATA_ONLY
// Prepared execution templates cache descriptor and slot metadata only.
// MGA visibility/finality and authorization remain engine-owned statement-use
// checks. Transaction visibility is deliberately absent from shared identity.

struct PreparedTemplateEpochs {
  scratchbird::engine::internal_api::EngineApiU64 catalog_epoch = 0;
  scratchbird::engine::internal_api::EngineApiU64 security_epoch = 0;
  scratchbird::engine::internal_api::EngineApiU64 policy_resource_epoch = 0;
  scratchbird::engine::internal_api::EngineApiU64 name_resolution_epoch = 0;
  bool operator==(const PreparedTemplateEpochs&) const = default;
};

struct PreparedDescriptorSlot {
  std::string stable_name;
  scratchbird::engine::internal_api::EngineDescriptor descriptor;
  std::uint32_t ordinal = 0;
  bool operator==(const PreparedDescriptorSlot&) const = default;
};

struct PreparedFieldOffset {
  std::string descriptor_slot;
  std::string field_name;
  std::size_t byte_offset = 0;
  std::size_t byte_width = 0;
  bool operator==(const PreparedFieldOffset&) const = default;
};

struct PreparedResultShapeDescriptor {
  std::string result_kind;
  std::vector<PreparedDescriptorSlot> columns;
  std::string digest;
  bool operator==(const PreparedResultShapeDescriptor&) const = default;
};

struct PreparedPredicateSlot {
  std::string stable_name;
  std::string descriptor_slot;
  bool required = true;
  bool operator==(const PreparedPredicateSlot&) const = default;
};

struct PreparedParameterSlot {
  std::string stable_name;
  scratchbird::engine::internal_api::EngineDescriptor descriptor;
  std::uint32_t ordinal = 0;
  bool required = true;
  bool operator==(const PreparedParameterSlot&) const = default;
};

struct PreparedIndexDescriptor {
  PreparedUuid index_uuid;
  PreparedUuid relation_uuid;
  std::string descriptor_digest;
  std::vector<PreparedUuid> key_column_uuids;
  std::vector<PreparedUuid> covered_column_uuids;
  bool visibility_native = false;
  bool operator==(const PreparedIndexDescriptor&) const = default;
};

struct PreparedPinnedDescriptorReference {
  std::string cache_key;
  PreparedUuid catalog_epoch_uuid;
  PreparedUuid descriptor_uuid;
  PreparedUuid object_uuid;
  PreparedUuid index_uuid;
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
  bool operator==(const PreparedPinnedDescriptorReference&) const = default;
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
  bool operator==(const PreparedSecurityVisibilityPolicyMetadata&) const = default;
};

struct PreparedTemplateKey {
  std::string operation_id;
  std::string sblr_digest_or_trace_key;
  PreparedUuid catalog_epoch_uuid;
  std::string descriptor_set_digest;
  std::string pinned_descriptor_set_digest;
  std::string result_shape_digest;
  PreparedTemplateEpochs epochs;
  std::vector<PreparedUuid> dependency_uuids;
  bool operator==(const PreparedTemplateKey&) const = default;
};

struct PreparedTemplateMemoryOwnership;
struct PreparedTemplateUseValidationResult;
class PreparedTemplateStatementUseReceipt;
struct PreparedExecutionTemplate {
 private:
  friend class PreparedTemplateCache;
  friend PreparedTemplateUseValidationResult RevalidatePreparedTemplateStatementUse(
      const PreparedExecutionTemplate&,
      const std::shared_ptr<const PreparedTemplateStatementUseReceipt>&);
  // Declared first, destroyed last, after the retained metadata containers.
  std::shared_ptr<PreparedTemplateMemoryOwnership> memory_owner_;
  std::atomic<bool> usable_{true};
 public:
  PreparedExecutionTemplate() = default;
  PreparedExecutionTemplate(const PreparedExecutionTemplate&) = delete;
  PreparedExecutionTemplate& operator=(const PreparedExecutionTemplate&) = delete;
  PreparedUuid template_id;
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
  PreparedUuid prepared_memory_lease_id;
  PreparedUuid descriptor_snapshot_memory_lease_id;
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
  bool operator==(const PreparedTemplateAdmission&) const = default;
};

struct PreparedTemplatePrepareResult {
  PreparedTemplateFailureKind failure_kind = PreparedTemplateFailureKind::kNone;
  bool ok = false;
  bool reused_existing_template = false;
  std::string diagnostic_code;
  std::string detail;
  std::shared_ptr<const PreparedExecutionTemplate> prepared_template;
};

struct PreparedTemplateMemoryGovernanceRequest {
  // The governor and ledger must outlive every returned template/use receipt.
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
  CanonicalExecutionMgaAuthority mga_authority;
  std::string descriptor_set_digest;
  std::string result_shape_digest;
  std::vector<PreparedUuid> dependency_uuids;
  std::vector<std::string> available_predicate_slots;
  std::vector<std::string> available_parameter_slots;
};

// SEARCH_KEY: SB_EXECUTOR_PREPARED_TEMPLATE_STATEMENT_USE_RECEIPT
// A successful bind issues an immutable receipt for one exact statement MGA
// vector. The resolver is retained so the vector can be compared in full
// immediately before executable use. It is never stored in the shared cache.
struct PreparedTemplateUseValidationResult;

class PreparedTemplateStatementUseReceipt {
 public:
  const PreparedUuid& receipt_id() const noexcept { return receipt_id_; }
  const PreparedUuid& catalog_epoch_uuid() const noexcept {
    return catalog_epoch_uuid_;
  }
  const PhysicalMgaStatementContext& statement_context() const noexcept {
    return statement_context_;
  }
  CanonicalMgaAuthorityOrigin authority_origin() const noexcept {
    return authority_origin_;
  }

 private:
  friend class PreparedTemplateCache;
  friend PreparedTemplateUseValidationResult
  RevalidatePreparedTemplateStatementUse(
      const PreparedExecutionTemplate& prepared_template,
      const std::shared_ptr<const PreparedTemplateStatementUseReceipt>&
          receipt);

  PreparedTemplateStatementUseReceipt() = default;
  static std::shared_ptr<PreparedTemplateStatementUseReceipt> Create();

  PreparedUuid receipt_id_;
  PreparedUuid prepared_template_id_;
  PreparedUuid catalog_epoch_uuid_;
  std::shared_ptr<const PreparedExecutionTemplate> prepared_owner_;
  PhysicalMgaStatementContext statement_context_;
  CanonicalMgaCurrentResolver resolve_current_;
  CanonicalMgaAuthorityOrigin authority_origin_ =
      CanonicalMgaAuthorityOrigin::kMissing;
  bool metadata_dependencies_revalidated_ = false;
  bool security_authorization_recheck_preserved_ = false;
};

struct PreparedTemplateBindResult {
  PreparedTemplateFailureKind failure_kind = PreparedTemplateFailureKind::kNone;
  bool ok = false;
  std::string diagnostic_code;
  std::string detail;
  std::vector<std::string> evidence;
  std::shared_ptr<const PreparedExecutionTemplate> prepared_template;
  std::shared_ptr<const PreparedTemplateStatementUseReceipt>
      statement_use_receipt;
};

struct PreparedTemplateUseValidationResult {
  PreparedTemplateFailureKind failure_kind = PreparedTemplateFailureKind::kNone;
  bool ok = false;
  std::string diagnostic_code;
  std::string detail;
  std::vector<std::string> evidence;
  std::shared_ptr<const PreparedTemplateStatementUseReceipt>
      executable_receipt;
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
std::string PreparedAuthorizationDigest(const PreparedUuid& principal, const PreparedUuid& role);
std::string PreparedDescriptorSetDigest(
    const std::vector<scratchbird::engine::internal_api::EngineDescriptor>& descriptors,
    const std::vector<scratchbird::engine::internal_api::EngineColumnDefinition>& columns);
std::string PreparedResultShapeDigest(const PreparedResultShapeDescriptor& result_shape);
std::string PreparedDependencyDigest(std::vector<PreparedUuid> dependency_uuids);
std::string PreparedPinnedDescriptorDigest(
    const std::vector<PreparedPinnedDescriptorReference>& pinned_descriptors);
PreparedTemplateUseValidationResult RevalidatePreparedTemplateStatementUse(
    const PreparedExecutionTemplate& prepared_template,
    const std::shared_ptr<const PreparedTemplateStatementUseReceipt>& receipt);

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
