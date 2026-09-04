// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "prepared_execution_template.hpp"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <utility>

namespace scratchbird::engine::executor {
namespace {

using scratchbird::engine::internal_api::EngineApiU64;
using scratchbird::engine::internal_api::EngineColumnDefinition;
using scratchbird::engine::internal_api::EngineDescriptor;

constexpr const char* kOk = "SB_PREPARED_TEMPLATE_OK";

bool Contains(const std::vector<std::string>& values, const std::string& value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

std::vector<std::string> Sorted(std::vector<std::string> values) {
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
  return values;
}

std::string ProfileSetDigest(
    const internal_api::EngineProfileSet& profile_set) {
  std::vector<std::string> parts;
  for (const auto& name : profile_set.names) parts.push_back("name:" + name);
  for (const auto& encoded : profile_set.encoded_profiles) {
    parts.push_back("profile:" + encoded);
  }
  return PreparedTemplateStableDigest(parts);
}

std::string DescriptorText(const EngineDescriptor& descriptor) {
  return descriptor.descriptor_uuid.canonical + ":" + descriptor.descriptor_kind + ":" +
         descriptor.canonical_type_name + ":" + descriptor.encoded_descriptor;
}

std::string EpochText(const PreparedTemplateEpochs& epochs) {
  std::ostringstream out;
  out << "catalog=" << epochs.catalog_epoch
      << "|security=" << epochs.security_epoch
      << "|policy_resource=" << epochs.policy_resource_epoch
      << "|name_resolution=" << epochs.name_resolution_epoch;
  return out.str();
}

bool IsCanonicalUuid(const std::string_view value) {
  if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
      value[18] != '-' || value[23] != '-') {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) continue;
    const auto ch = static_cast<unsigned char>(value[index]);
    if (!std::isxdigit(ch) || std::isupper(ch)) return false;
  }
  return value != "00000000-0000-0000-0000-000000000000";
}

std::string LocalTransactionIdsText(const std::vector<std::uint64_t>& values) {
  std::ostringstream out;
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (index != 0) out << ',';
    out << values[index];
  }
  return out.str();
}

std::string MgaStatementContextText(
    const PhysicalMgaStatementContext& context) {
  std::ostringstream out;
  out << "statement=" << context.statement_uuid
      << "|owner_uuid=" << context.owning_transaction_uuid
      << "|snapshot=" << context.statement_snapshot_uuid
      << "|metadata_snapshot=" << context.statement_metadata_snapshot_uuid
      << "|owner_local=" << context.owning_local_transaction_id
      << "|committed_high_water=" << context.visible_committed_high_watermark
      << "|oldest_active=" << context.oldest_active_transaction_id
      << "|oldest_interesting=" << context.oldest_interesting_transaction_id
      << "|oldest_snapshot=" << context.oldest_snapshot_transaction_id
      << "|retention_horizon=" << context.retention_horizon_transaction_id
      << "|active_excluded="
      << LocalTransactionIdsText(
             context.active_excluded_local_transaction_ids)
      << "|in_doubt_excluded="
      << LocalTransactionIdsText(
             context.in_doubt_excluded_local_transaction_ids)
      << "|snapshot_kind=" << context.snapshot_kind
      << "|inventory_next="
      << context.publication_inventory_next_local_transaction_id
      << "|inventory_authoritative="
      << (context.inventory_authoritative ? "true" : "false")
      << "|complete=" << (context.complete ? "true" : "false")
      << "|current=" << (context.current ? "true" : "false");
  return out.str();
}

bool CatalogEpochUuidIndependent(
    const std::string& catalog_epoch_uuid,
    const PhysicalMgaStatementContext& statement_context) {
  return catalog_epoch_uuid != statement_context.statement_uuid &&
         catalog_epoch_uuid != statement_context.owning_transaction_uuid &&
         catalog_epoch_uuid != statement_context.statement_snapshot_uuid &&
         catalog_epoch_uuid !=
             statement_context.statement_metadata_snapshot_uuid;
}

struct PreparedMgaResolutionCheck {
  bool ok = false;
  std::string diagnostic_code;
  std::string detail;
  PhysicalMgaStatementContext current_statement_context;
};

PreparedMgaResolutionCheck ResolveExactPreparedMgaStatementContext(
    const CanonicalExecutionMgaAuthority& authority,
    const internal_api::EngineRequestContext* engine_context) {
  PreparedMgaResolutionCheck check;
  if (authority.origin !=
          CanonicalMgaAuthorityOrigin::kEngineTransactionInventory ||
      !authority.resolve_current) {
    check.diagnostic_code = "SB_PREPARED_TEMPLATE_MGA_AUTHORITY_REQUIRED";
    check.detail =
        "statement-bound engine MGA authority and current resolver are required";
    return check;
  }
  if (!PhysicalMgaStatementContextValid(authority.statement_context)) {
    check.diagnostic_code =
        "SB_PREPARED_TEMPLATE_MGA_STATEMENT_CONTEXT_INVALID";
    check.detail = "statement-bound MGA context is incomplete or malformed";
    return check;
  }
  if (engine_context != nullptr &&
      (authority.statement_context.statement_uuid !=
           engine_context->statement_uuid.canonical ||
       authority.statement_context.owning_transaction_uuid !=
           engine_context->transaction_uuid.canonical ||
       authority.statement_context.statement_snapshot_uuid !=
           engine_context->statement_snapshot_uuid.canonical ||
       authority.statement_context.statement_metadata_snapshot_uuid !=
           engine_context->statement_metadata_snapshot_uuid.canonical ||
       authority.statement_context.owning_local_transaction_id !=
           engine_context->local_transaction_id ||
       authority.statement_context.visible_committed_high_watermark !=
           engine_context->snapshot_visible_through_local_transaction_id)) {
    check.diagnostic_code =
        "SB_PREPARED_TEMPLATE_MGA_REQUEST_CONTEXT_MISMATCH";
    check.detail =
        "statement-bound MGA identity or owner does not match the engine request context";
    return check;
  }

  const auto current = authority.resolve_current();
  if (!current.diagnostic.ok) {
    check.diagnostic_code = current.diagnostic.diagnostic_code.empty()
                                ? "SB_PREPARED_TEMPLATE_MGA_CURRENT_RESOLUTION_REFUSED"
                                : current.diagnostic.diagnostic_code;
    check.detail = current.diagnostic.detail.empty()
                       ? "engine transaction inventory refused current statement resolution"
                       : current.diagnostic.detail;
    return check;
  }
  if (!PhysicalMgaStatementContextValid(current.statement_context)) {
    check.diagnostic_code =
        "SB_PREPARED_TEMPLATE_MGA_CURRENT_CONTEXT_INVALID";
    check.detail =
        "engine transaction inventory returned an incomplete or noncurrent statement context";
    return check;
  }
  if (!PhysicalMgaStatementContextEqual(authority.statement_context,
                                        current.statement_context)) {
    check.diagnostic_code =
        "SB_PREPARED_TEMPLATE_MGA_CURRENT_CONTEXT_MISMATCH";
    check.detail =
        "current engine statement MGA vector differs from the statement-bound vector";
    return check;
  }
  check.ok = true;
  check.diagnostic_code = kOk;
  check.current_statement_context = current.statement_context;
  return check;
}

std::string UInt64Hex(std::uint64_t value) {
  std::ostringstream out;
  out << std::hex << std::setw(16) << std::setfill('0') << value;
  return out.str();
}

PreparedTemplatePrepareResult PrepareFailure(std::string code, std::string detail) {
  PreparedTemplatePrepareResult result;
  result.ok = false;
  result.reused_existing_template = false;
  result.diagnostic_code = std::move(code);
  result.detail = std::move(detail);
  return result;
}

PreparedTemplateBindResult BindFailure(const PreparedExecutionTemplate& prepared_template,
                                       std::string code,
                                       std::string detail) {
  (void)prepared_template;
  PreparedTemplateBindResult result;
  result.ok = false;
  result.diagnostic_code = std::move(code);
  result.detail = std::move(detail);
  return result;
}

bool DependencySetMatches(std::vector<std::string> expected, std::vector<std::string> actual) {
  return Sorted(std::move(expected)) == Sorted(std::move(actual));
}

std::optional<std::string> FirstMissingRequiredPredicate(const PreparedExecutionTemplate& prepared_template,
                                                        const PreparedTemplateBindContext& bind_context) {
  for (const auto& slot : prepared_template.predicate_slots) {
    if (slot.required && !Contains(bind_context.available_predicate_slots, slot.stable_name)) {
      return slot.stable_name;
    }
  }
  return std::nullopt;
}

std::optional<std::string> FirstMissingRequiredParameter(const PreparedExecutionTemplate& prepared_template,
                                                        const PreparedTemplateBindContext& bind_context) {
  for (const auto& slot : prepared_template.parameter_slots) {
    if (slot.required && !Contains(bind_context.available_parameter_slots, slot.stable_name)) {
      return slot.stable_name;
    }
  }
  return std::nullopt;
}

std::optional<std::string> UnsafePinnedDescriptor(
    const std::vector<PreparedPinnedDescriptorReference>& pinned_descriptors) {
  for (const auto& descriptor : pinned_descriptors) {
    if (descriptor.cache_key.empty() ||
        descriptor.catalog_epoch_uuid.empty() ||
        descriptor.descriptor_set_digest.empty() ||
        descriptor.object_uuid.empty() ||
        descriptor.security_policy_identity.empty() ||
        descriptor.redaction_policy_identity.empty()) {
      return "pinned descriptor cache key, catalog epoch UUID, object UUID, descriptor digest, and policy identities are required";
    }
    if (!descriptor.read_only_snapshot ||
        !descriptor.security_recheck_required ||
        !descriptor.visibility_recheck_required ||
        descriptor.finality_authority_cached) {
      return "pinned descriptors must be read-only metadata and preserve MGA/security rechecks";
    }
  }
  return std::nullopt;
}

std::optional<PreparedTemplatePrepareResult> ValidateAndCanonicalizeAdmission(
    PreparedTemplateAdmission* admission) {
  if (admission->key.operation_id.empty()) {
    return PrepareFailure("SB_PREPARED_TEMPLATE_OPERATION_ID_REQUIRED",
                          "operation_id is required");
  }
  if (admission->key.sblr_digest_or_trace_key.empty()) {
    return PrepareFailure("SB_PREPARED_TEMPLATE_SBLR_DIGEST_REQUIRED",
                          "SBLR digest or trace key is required");
  }
  if (!IsCanonicalUuid(admission->key.catalog_epoch_uuid)) {
    return PrepareFailure("SB_PREPARED_TEMPLATE_CATALOG_EPOCH_UUID_REQUIRED",
                          "a canonical catalog epoch UUID is required");
  }
  if (admission->key.descriptor_set_digest.empty() ||
      admission->key.result_shape_digest.empty()) {
    return PrepareFailure("SB_PREPARED_TEMPLATE_DESCRIPTOR_DIGEST_REQUIRED",
                          "descriptor set and result shape digests are required");
  }
  if (admission->descriptor_slots.empty()) {
    return PrepareFailure("SB_PREPARED_TEMPLATE_DESCRIPTOR_SLOT_REQUIRED",
                          "at least one descriptor slot is required");
  }
  if (admission->result_shape.digest.empty()) {
    admission->result_shape.digest =
        PreparedResultShapeDigest(admission->result_shape);
  }
  if (admission->result_shape.digest != admission->key.result_shape_digest) {
    return PrepareFailure("SB_PREPARED_TEMPLATE_RESULT_SHAPE_MISMATCH",
                          "result shape digest does not match the cache key");
  }
  if (!admission->policy_metadata.cached_metadata_only ||
      !admission->policy_metadata.security_recheck_required ||
      !admission->policy_metadata.visibility_recheck_required ||
      admission->policy_metadata.finality_authority_cached) {
    return PrepareFailure("SB_PREPARED_TEMPLATE_POLICY_METADATA_UNSAFE",
                          "prepared template policy metadata must preserve bind-time security and visibility rechecks");
  }
  if (const auto unsafe = UnsafePinnedDescriptor(admission->pinned_descriptors);
      unsafe.has_value()) {
    return PrepareFailure("SB_PREPARED_TEMPLATE_PINNED_DESCRIPTOR_UNSAFE",
                          *unsafe);
  }
  if (admission->key.pinned_descriptor_set_digest.empty()) {
    admission->key.pinned_descriptor_set_digest =
        PreparedPinnedDescriptorDigest(admission->pinned_descriptors);
  }
  return std::nullopt;
}

void PopulatePreparedTemplate(PreparedExecutionTemplate* prepared_template,
                              PreparedTemplateAdmission admission,
                              const std::string& canonical_key) {
  prepared_template->template_id = PreparedTemplateStableDigest({canonical_key});
  prepared_template->key = std::move(admission.key);
  prepared_template->descriptor_slots = std::move(admission.descriptor_slots);
  prepared_template->field_offsets = std::move(admission.field_offsets);
  prepared_template->result_shape = std::move(admission.result_shape);
  prepared_template->predicate_slots = std::move(admission.predicate_slots);
  prepared_template->parameter_slots = std::move(admission.parameter_slots);
  prepared_template->index_descriptors = std::move(admission.index_descriptors);
  prepared_template->pinned_descriptors = std::move(admission.pinned_descriptors);
  prepared_template->policy_metadata = std::move(admission.policy_metadata);
}

bool PreparedEpochStale(const memory::ResultCursorPlanMemoryEpochs& record,
                        const memory::ResultCursorPlanMemoryEpochs& current) {
  return (current.catalog_epoch != 0 &&
          record.catalog_epoch != current.catalog_epoch) ||
         (current.security_epoch != 0 &&
          record.security_epoch != current.security_epoch) ||
         (current.redaction_epoch != 0 &&
          record.redaction_epoch != current.redaction_epoch) ||
         (current.policy_epoch != 0 &&
          record.policy_epoch != current.policy_epoch) ||
         (current.resource_epoch != 0 &&
          record.resource_epoch != current.resource_epoch) ||
         (current.descriptor_epoch != 0 &&
          record.descriptor_epoch != current.descriptor_epoch) ||
         (current.memory_policy_epoch != 0 &&
          record.memory_policy_epoch != current.memory_policy_epoch);
}

void FillPreparedGovernanceEpochsFromKey(
    const PreparedTemplateKey& key,
    memory::ResultCursorPlanMemoryEpochs* epochs) {
  if (epochs->catalog_epoch == 0) {
    epochs->catalog_epoch = key.epochs.catalog_epoch;
  }
  if (epochs->security_epoch == 0) {
    epochs->security_epoch = key.epochs.security_epoch;
  }
  if (epochs->redaction_epoch == 0) {
    epochs->redaction_epoch = key.epochs.security_epoch;
  }
  if (epochs->policy_epoch == 0) {
    epochs->policy_epoch = key.epochs.policy_resource_epoch;
  }
  if (epochs->resource_epoch == 0) {
    epochs->resource_epoch = key.epochs.policy_resource_epoch;
  }
  if (epochs->descriptor_epoch == 0) {
    epochs->descriptor_epoch = key.epochs.catalog_epoch;
  }
  if (epochs->memory_policy_epoch == 0) {
    epochs->memory_policy_epoch = key.epochs.policy_resource_epoch;
  }
}

}  // namespace

std::shared_ptr<PreparedTemplateStatementUseReceipt>
PreparedTemplateStatementUseReceipt::Create() {
  struct ConstructionAccess final : PreparedTemplateStatementUseReceipt {};
  return std::make_shared<ConstructionAccess>();
}

std::string PreparedTemplateStableDigest(const std::vector<std::string>& parts) {
  std::uint64_t hash = 1469598103934665603ull;
  for (const auto& part : parts) {
    for (const unsigned char ch : part) {
      hash ^= static_cast<std::uint64_t>(ch);
      hash *= 1099511628211ull;
    }
    hash ^= 0xffu;
    hash *= 1099511628211ull;
  }
  return "fnv1a64:" + UInt64Hex(hash);
}

std::string PreparedDescriptorSetDigest(const std::vector<EngineDescriptor>& descriptors,
                                        const std::vector<EngineColumnDefinition>& columns) {
  std::vector<std::string> parts;
  parts.reserve(descriptors.size() + columns.size());
  for (const auto& descriptor : descriptors) {
    parts.push_back("descriptor:" + DescriptorText(descriptor));
  }
  for (const auto& column : columns) {
    parts.push_back("column:" + column.requested_column_uuid.canonical + ":" +
                    std::to_string(column.ordinal) + ":" + DescriptorText(column.descriptor) + ":" +
                    (column.nullable ? "nullable" : "required"));
  }
  return PreparedTemplateStableDigest(parts);
}

std::string PreparedResultShapeDigest(const PreparedResultShapeDescriptor& result_shape) {
  std::vector<std::string> parts;
  parts.push_back("kind:" + result_shape.result_kind);
  for (const auto& column : result_shape.columns) {
    parts.push_back("column:" + column.stable_name + ":" + std::to_string(column.ordinal) + ":" +
                    DescriptorText(column.descriptor));
  }
  return PreparedTemplateStableDigest(parts);
}

std::string PreparedDependencyDigest(std::vector<std::string> dependency_uuids) {
  return PreparedTemplateStableDigest(Sorted(std::move(dependency_uuids)));
}

std::string PreparedPinnedDescriptorDigest(
    const std::vector<PreparedPinnedDescriptorReference>& pinned_descriptors) {
  if (pinned_descriptors.empty()) return {};
  std::vector<std::string> parts;
  parts.reserve(pinned_descriptors.size());
  for (const auto& descriptor : pinned_descriptors) {
    std::ostringstream out;
    out << "cache_key=" << descriptor.cache_key
        << "|catalog_epoch_uuid=" << descriptor.catalog_epoch_uuid
        << "|descriptor_uuid=" << descriptor.descriptor_uuid
        << "|object_uuid=" << descriptor.object_uuid
        << "|index_uuid=" << descriptor.index_uuid
        << "|descriptor_set_digest=" << descriptor.descriptor_set_digest
        << "|catalog_epoch=" << descriptor.catalog_epoch
        << "|security_epoch=" << descriptor.security_epoch
        << "|resource_policy_epoch=" << descriptor.resource_policy_epoch
        << "|name_resolution_epoch=" << descriptor.name_resolution_epoch
        << "|stats_epoch=" << descriptor.stats_epoch
        << "|security_policy_identity=" << descriptor.security_policy_identity
        << "|redaction_policy_identity=" << descriptor.redaction_policy_identity
        << "|read_only_snapshot=" << (descriptor.read_only_snapshot ? "true" : "false")
        << "|security_recheck_required=" << (descriptor.security_recheck_required ? "true" : "false")
        << "|visibility_recheck_required=" << (descriptor.visibility_recheck_required ? "true" : "false")
        << "|finality_authority_cached=" << (descriptor.finality_authority_cached ? "true" : "false");
    parts.push_back(out.str());
  }
  return PreparedTemplateStableDigest(Sorted(std::move(parts)));
}

std::string PreparedTemplateCanonicalKey(const PreparedTemplateKey& key) {
  std::ostringstream out;
  out << "operation=" << key.operation_id
      << "|sblr=" << key.sblr_digest_or_trace_key
      << "|catalog_epoch_uuid=" << key.catalog_epoch_uuid
      << "|descriptor_set=" << key.descriptor_set_digest
      << "|pinned_descriptor_set=" << key.pinned_descriptor_set_digest
      << "|result_shape=" << key.result_shape_digest
      << '|' << EpochText(key.epochs)
      << "|dependencies=" << PreparedDependencyDigest(key.dependency_uuids);
  return out.str();
}

PreparedTemplatePrepareResult PreparedTemplateCache::Prepare(PreparedTemplateAdmission admission) {
  if (auto failure = ValidateAndCanonicalizeAdmission(&admission);
      failure.has_value()) {
    return *failure;
  }

  const std::string canonical_key = PreparedTemplateCanonicalKey(admission.key);
  std::lock_guard<std::mutex> lock(mutex_);
  if (const auto existing = templates_.find(canonical_key); existing != templates_.end()) {
    PreparedTemplatePrepareResult result;
    result.ok = true;
    result.reused_existing_template = true;
    result.diagnostic_code = kOk;
    result.prepared_template = existing->second;
    return result;
  }

  auto prepared_template = std::make_shared<PreparedExecutionTemplate>();
  PopulatePreparedTemplate(prepared_template.get(),
                           std::move(admission),
                           canonical_key);

  templates_.emplace(canonical_key, prepared_template);

  PreparedTemplatePrepareResult result;
  result.ok = true;
  result.reused_existing_template = false;
  result.diagnostic_code = kOk;
  result.prepared_template = std::move(prepared_template);
  return result;
}

PreparedTemplatePrepareResult PreparedTemplateCache::PrepareGoverned(
    PreparedTemplateAdmission admission,
    PreparedTemplateMemoryGovernanceRequest governance) {
  if (auto failure = ValidateAndCanonicalizeAdmission(&admission);
      failure.has_value()) {
    return *failure;
  }
  const std::string canonical_key = PreparedTemplateCanonicalKey(admission.key);
  const std::string template_id = PreparedTemplateStableDigest({canonical_key});
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (const auto existing = templates_.find(canonical_key);
        existing != templates_.end()) {
      if (!existing->second->memory_governed) {
        return PrepareFailure("SB_PREPARED_TEMPLATE_EXISTING_UNGOVERNED",
                              "existing prepared template was not created through CEIC-020 memory governance");
      }
      PreparedTemplatePrepareResult result;
      result.ok = true;
      result.reused_existing_template = true;
      result.diagnostic_code = kOk;
      result.prepared_template = existing->second;
      return result;
    }
  }

  if (governance.governor == nullptr || governance.ledger == nullptr) {
    return PrepareFailure("SB_PREPARED_TEMPLATE_MEMORY_GOVERNANCE_REQUIRED",
                          "prepared template memory governor and ledger are required");
  }
  if (governance.estimated_template_bytes == 0) {
    return PrepareFailure("SB_PREPARED_TEMPLATE_MEMORY_BYTES_REQUIRED",
                          "prepared template estimated memory bytes are required");
  }
  if (governance.scope.database_id.empty() ||
      governance.scope.session_id.empty()) {
    return PrepareFailure("SB_PREPARED_TEMPLATE_MEMORY_SCOPE_REQUIRED",
                          "database and session scope are required for prepared template memory");
  }
  governance.scope.plan_cache_key = canonical_key;
  if (governance.scope.prepared_statement_id.empty()) {
    governance.scope.prepared_statement_id = template_id;
  }
  if (governance.estimated_descriptor_snapshot_bytes != 0 &&
      governance.scope.descriptor_snapshot_id.empty()) {
    governance.scope.descriptor_snapshot_id = "descriptor-snapshot:" + template_id;
  }
  FillPreparedGovernanceEpochsFromKey(admission.key, &governance.epochs);
  if (governance.provenance.source ==
      memory::HierarchicalMemoryBudgetProvenanceSource::unknown) {
    governance.provenance.source =
        memory::HierarchicalMemoryBudgetProvenanceSource::server_runtime_api;
    governance.provenance.source_label = "engine.executor.prepared_template";
  }

  memory::ResultCursorPlanMemoryLeaseRequest prepared_lease;
  prepared_lease.surface =
      memory::ResultCursorPlanMemorySurface::prepared_statement;
  prepared_lease.ledger = governance.ledger;
  prepared_lease.policy = governance.policy;
  prepared_lease.scope = governance.scope;
  prepared_lease.epochs = governance.epochs;
  prepared_lease.provenance = governance.provenance;
  prepared_lease.memory_class = "ceic_020.prepared_execution_template";
  prepared_lease.owner_id = "executor.prepared_template:" + template_id;
  prepared_lease.route_label = admission.key.operation_id;
  prepared_lease.requested_bytes = governance.estimated_template_bytes;
  prepared_lease.cluster_route_requested = governance.cluster_route_requested;
  auto prepared_acquired = governance.governor->Acquire(std::move(prepared_lease));
  if (!prepared_acquired.ok()) {
    auto failure = PrepareFailure(
        prepared_acquired.diagnostic.diagnostic_code.empty()
            ? "SB_PREPARED_TEMPLATE_MEMORY_RESERVATION_REFUSED"
            : prepared_acquired.diagnostic.diagnostic_code,
        "prepared template memory reservation refused");
    return failure;
  }

  std::string descriptor_lease_id;
  std::vector<std::string> descriptor_evidence;
  if (governance.estimated_descriptor_snapshot_bytes != 0) {
    memory::ResultCursorPlanMemoryLeaseRequest descriptor_lease;
    descriptor_lease.surface =
        memory::ResultCursorPlanMemorySurface::descriptor_snapshot;
    descriptor_lease.ledger = governance.ledger;
    descriptor_lease.policy = governance.policy;
    descriptor_lease.scope = governance.scope;
    descriptor_lease.epochs = governance.epochs;
    descriptor_lease.provenance = governance.provenance;
    descriptor_lease.memory_class = "ceic_020.prepared_descriptor_snapshot";
    descriptor_lease.owner_id = "executor.prepared_descriptor:" + template_id;
    descriptor_lease.route_label = admission.key.operation_id;
    descriptor_lease.requested_bytes =
        governance.estimated_descriptor_snapshot_bytes;
    descriptor_lease.cluster_route_requested = governance.cluster_route_requested;
    auto descriptor_acquired =
        governance.governor->Acquire(std::move(descriptor_lease));
    if (!descriptor_acquired.ok()) {
      (void)governance.governor->Release(
          prepared_acquired.lease_id,
          memory::ResultCursorPlanMemoryReleaseReason::explicit_release);
      return PrepareFailure(
          descriptor_acquired.diagnostic.diagnostic_code.empty()
              ? "SB_PREPARED_TEMPLATE_DESCRIPTOR_MEMORY_RESERVATION_REFUSED"
              : descriptor_acquired.diagnostic.diagnostic_code,
          "prepared descriptor snapshot memory reservation refused");
    }
    descriptor_lease_id = descriptor_acquired.lease_id;
    descriptor_evidence = descriptor_acquired.evidence;
  }

  auto prepared_template = std::make_shared<PreparedExecutionTemplate>();
  PopulatePreparedTemplate(prepared_template.get(),
                           std::move(admission),
                           canonical_key);
  prepared_template->memory_governed = true;
  prepared_template->memory_reserved_bytes =
      governance.estimated_template_bytes +
      governance.estimated_descriptor_snapshot_bytes;
  prepared_template->prepared_memory_lease_id = prepared_acquired.lease_id;
  prepared_template->descriptor_snapshot_memory_lease_id =
      std::move(descriptor_lease_id);
  prepared_template->memory_scope = governance.scope;
  prepared_template->memory_epochs = governance.epochs;
  prepared_template->memory_governance_evidence = prepared_acquired.evidence;
  prepared_template->memory_governance_evidence.insert(
      prepared_template->memory_governance_evidence.end(),
      descriptor_evidence.begin(),
      descriptor_evidence.end());
  prepared_template->memory_governance_evidence.push_back(
      "CEIC-020_PREPARED_TEMPLATE_MEMORY_GOVERNED");

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (const auto existing = templates_.find(canonical_key);
        existing != templates_.end()) {
      (void)governance.governor->Release(
          prepared_acquired.lease_id,
          memory::ResultCursorPlanMemoryReleaseReason::explicit_release);
      if (!prepared_template->descriptor_snapshot_memory_lease_id.empty()) {
        (void)governance.governor->Release(
            prepared_template->descriptor_snapshot_memory_lease_id,
            memory::ResultCursorPlanMemoryReleaseReason::explicit_release);
      }
      if (!existing->second->memory_governed) {
        return PrepareFailure("SB_PREPARED_TEMPLATE_EXISTING_UNGOVERNED",
                              "existing prepared template was not created through CEIC-020 memory governance");
      }
      PreparedTemplatePrepareResult result;
      result.ok = true;
      result.reused_existing_template = true;
      result.diagnostic_code = kOk;
      result.prepared_template = existing->second;
      return result;
    }
    templates_.emplace(canonical_key, prepared_template);
  }

  PreparedTemplatePrepareResult result;
  result.ok = true;
  result.reused_existing_template = false;
  result.diagnostic_code = kOk;
  result.prepared_template = std::move(prepared_template);
  return result;
}

std::uint64_t PreparedTemplateCache::InvalidateGovernedByEpoch(
    const memory::ResultCursorPlanMemoryEpochs& current_epochs,
    memory::ResultCursorPlanMemoryGovernor* governor) {
  struct Eviction {
    std::string cache_key;
    std::string prepared_lease_id;
    std::string descriptor_lease_id;
  };
  std::vector<Eviction> evictions;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [cache_key, prepared_template] : templates_) {
      if (prepared_template->memory_governed &&
          PreparedEpochStale(prepared_template->memory_epochs, current_epochs)) {
        evictions.push_back({cache_key,
                             prepared_template->prepared_memory_lease_id,
                             prepared_template->descriptor_snapshot_memory_lease_id});
      }
    }
    for (const auto& eviction : evictions) {
      templates_.erase(eviction.cache_key);
    }
  }
  if (governor != nullptr) {
    for (const auto& eviction : evictions) {
      if (!eviction.prepared_lease_id.empty()) {
        (void)governor->Release(
            eviction.prepared_lease_id,
            memory::ResultCursorPlanMemoryReleaseReason::epoch_invalidation);
      }
      if (!eviction.descriptor_lease_id.empty()) {
        (void)governor->Release(
            eviction.descriptor_lease_id,
            memory::ResultCursorPlanMemoryReleaseReason::epoch_invalidation);
      }
    }
  }
  return static_cast<std::uint64_t>(evictions.size());
}

std::shared_ptr<const PreparedExecutionTemplate> PreparedTemplateCache::Lookup(const PreparedTemplateKey& key) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = templates_.find(PreparedTemplateCanonicalKey(key));
  return found == templates_.end() ? nullptr : found->second;
}

PreparedTemplateBindResult PreparedTemplateCache::Bind(const PreparedExecutionTemplate& prepared_template,
                                                       const PreparedTemplateBindContext& bind_context) const {
  const auto& context = bind_context.engine_context;
  const auto& key = prepared_template.key;

  if (prepared_template.policy_metadata.requires_security_context && !context.security_context_present) {
    return BindFailure(prepared_template,
                       "SB_PREPARED_TEMPLATE_MISSING_SECURITY_CONTEXT",
                       "security context is required for this prepared template");
  }
  if (prepared_template.policy_metadata.requires_transaction_context &&
      context.transaction_uuid.canonical.empty() && context.local_transaction_id == 0) {
    return BindFailure(prepared_template,
                       "SB_PREPARED_TEMPLATE_MISSING_TRANSACTION_CONTEXT",
                       "transaction context is required for this prepared template");
  }
  if (!IsCanonicalUuid(context.catalog_epoch_uuid.canonical) ||
      context.catalog_epoch_uuid.canonical != key.catalog_epoch_uuid) {
    return BindFailure(prepared_template,
                       "SB_PREPARED_TEMPLATE_STALE_CATALOG_EPOCH_UUID",
                       "canonical catalog epoch UUID does not match the prepared template");
  }
  if (context.catalog_generation_id != key.epochs.catalog_epoch) {
    return BindFailure(prepared_template,
                       "SB_PREPARED_TEMPLATE_STALE_CATALOG_EPOCH",
                       "catalog epoch changed from " + std::to_string(key.epochs.catalog_epoch) +
                           " to " + std::to_string(context.catalog_generation_id));
  }
  if (context.security_epoch != key.epochs.security_epoch) {
    return BindFailure(prepared_template,
                       "SB_PREPARED_TEMPLATE_STALE_SECURITY_EPOCH",
                       "security epoch changed from " + std::to_string(key.epochs.security_epoch) +
                           " to " + std::to_string(context.security_epoch));
  }
  if (context.resource_epoch != key.epochs.policy_resource_epoch) {
    return BindFailure(prepared_template,
                       "SB_PREPARED_TEMPLATE_STALE_POLICY_RESOURCE_EPOCH",
                       "policy/resource epoch changed from " + std::to_string(key.epochs.policy_resource_epoch) +
                           " to " + std::to_string(context.resource_epoch));
  }
  if (context.name_resolution_epoch != key.epochs.name_resolution_epoch) {
    return BindFailure(prepared_template,
                       "SB_PREPARED_TEMPLATE_STALE_NAME_RESOLUTION_EPOCH",
                       "name-resolution epoch changed from " + std::to_string(key.epochs.name_resolution_epoch) +
                           " to " + std::to_string(context.name_resolution_epoch));
  }
  const auto expected_security_policy_digest =
      ProfileSetDigest(bind_context.request.policy_profile);
  const auto expected_visibility_policy_digest = PreparedTemplateStableDigest(
      {"visibility_recheck:engine_statement_use",
       "isolation:" + context.transaction_isolation_level});
  const auto expected_authorization_policy_digest = PreparedTemplateStableDigest(
      {"principal:" + context.principal_uuid.canonical,
       "role:" + context.current_role_uuid.canonical});
  if ((!prepared_template.policy_metadata.security_policy_digest.empty() &&
       prepared_template.policy_metadata.security_policy_digest !=
           expected_security_policy_digest) ||
      (!prepared_template.policy_metadata.visibility_policy_digest.empty() &&
       prepared_template.policy_metadata.visibility_policy_digest !=
           expected_visibility_policy_digest) ||
      (!prepared_template.policy_metadata.authorization_policy_digest.empty() &&
       prepared_template.policy_metadata.authorization_policy_digest !=
           expected_authorization_policy_digest)) {
    return BindFailure(
        prepared_template,
        "SB_PREPARED_TEMPLATE_POLICY_METADATA_MISMATCH",
        "current security, visibility, or authorization policy identity does not match the prepared metadata");
  }
  if (bind_context.descriptor_set_digest != key.descriptor_set_digest ||
      !DependencySetMatches(key.dependency_uuids, bind_context.dependency_uuids)) {
    return BindFailure(prepared_template,
                       "SB_PREPARED_TEMPLATE_DESCRIPTOR_MISMATCH",
                       "current descriptor set or dependency UUIDs do not match the prepared template");
  }
  if (bind_context.result_shape_digest != key.result_shape_digest) {
    return BindFailure(prepared_template,
                       "SB_PREPARED_TEMPLATE_RESULT_SHAPE_MISMATCH",
                       "current result shape does not match the prepared template");
  }
  if (const auto missing = FirstMissingRequiredPredicate(prepared_template, bind_context); missing.has_value()) {
    return BindFailure(prepared_template,
                       "SB_PREPARED_TEMPLATE_MISSING_PREDICATE_SLOT",
                       "missing predicate slot: " + *missing);
  }
  if (const auto missing = FirstMissingRequiredParameter(prepared_template, bind_context); missing.has_value()) {
    return BindFailure(prepared_template,
                       "SB_PREPARED_TEMPLATE_MISSING_PARAMETER_SLOT",
                       "missing parameter slot: " + *missing);
  }
  for (const auto& pinned : prepared_template.pinned_descriptors) {
    if (!pinned.descriptor_set_digest.empty() &&
        pinned.descriptor_set_digest != bind_context.descriptor_set_digest) {
      return BindFailure(prepared_template,
                         "SB_PREPARED_TEMPLATE_PINNED_DESCRIPTOR_DESCRIPTOR_MISMATCH",
                         "pinned descriptor set digest does not match current bind descriptor set");
    }
    if (pinned.catalog_epoch != 0 && pinned.catalog_epoch != context.catalog_generation_id) {
      return BindFailure(prepared_template,
                         "SB_PREPARED_TEMPLATE_PINNED_DESCRIPTOR_STALE_CATALOG_EPOCH",
                         "pinned descriptor catalog epoch changed from " +
                             std::to_string(pinned.catalog_epoch) + " to " +
                             std::to_string(context.catalog_generation_id));
    }
    if (pinned.catalog_epoch_uuid != context.catalog_epoch_uuid.canonical) {
      return BindFailure(
          prepared_template,
          "SB_PREPARED_TEMPLATE_PINNED_DESCRIPTOR_STALE_CATALOG_EPOCH_UUID",
          "pinned descriptor catalog epoch UUID does not match the current catalog epoch UUID");
    }
    if (pinned.security_epoch != 0 && pinned.security_epoch != context.security_epoch) {
      return BindFailure(prepared_template,
                         "SB_PREPARED_TEMPLATE_PINNED_DESCRIPTOR_STALE_SECURITY_EPOCH",
                         "pinned descriptor security epoch changed from " +
                             std::to_string(pinned.security_epoch) + " to " +
                             std::to_string(context.security_epoch));
    }
    if (pinned.resource_policy_epoch != 0 && pinned.resource_policy_epoch != context.resource_epoch) {
      return BindFailure(prepared_template,
                         "SB_PREPARED_TEMPLATE_PINNED_DESCRIPTOR_STALE_POLICY_RESOURCE_EPOCH",
                         "pinned descriptor policy/resource epoch changed from " +
                             std::to_string(pinned.resource_policy_epoch) + " to " +
                             std::to_string(context.resource_epoch));
    }
    if (pinned.name_resolution_epoch != 0 &&
        pinned.name_resolution_epoch != context.name_resolution_epoch) {
      return BindFailure(prepared_template,
                         "SB_PREPARED_TEMPLATE_PINNED_DESCRIPTOR_STALE_NAME_RESOLUTION_EPOCH",
                         "pinned descriptor name-resolution epoch changed from " +
                             std::to_string(pinned.name_resolution_epoch) + " to " +
                             std::to_string(context.name_resolution_epoch));
    }
  }

  const auto mga_check = ResolveExactPreparedMgaStatementContext(
      bind_context.mga_authority, &context);
  if (!mga_check.ok) {
    return BindFailure(prepared_template, mga_check.diagnostic_code,
                       mga_check.detail);
  }
  if (!CatalogEpochUuidIndependent(key.catalog_epoch_uuid,
                                   mga_check.current_statement_context)) {
    return BindFailure(
        prepared_template,
        "SB_PREPARED_TEMPLATE_CATALOG_EPOCH_UUID_NOT_INDEPENDENT",
        "catalog epoch UUID must be independent from statement and transaction MGA identities");
  }

  auto receipt = PreparedTemplateStatementUseReceipt::Create();
  receipt->prepared_template_id_ = prepared_template.template_id;
  receipt->catalog_epoch_uuid_ = key.catalog_epoch_uuid;
  receipt->statement_context_ = mga_check.current_statement_context;
  receipt->resolve_current_ = bind_context.mga_authority.resolve_current;
  receipt->authority_origin_ = bind_context.mga_authority.origin;
  receipt->metadata_dependencies_revalidated_ = true;
  receipt->security_authorization_recheck_preserved_ = true;
  receipt->receipt_id_ = PreparedTemplateStableDigest(
      {prepared_template.template_id, key.catalog_epoch_uuid,
       MgaStatementContextText(receipt->statement_context_)});

  PreparedTemplateBindResult result;
  result.ok = true;
  result.diagnostic_code = kOk;
  result.statement_use_receipt = std::move(receipt);
  result.evidence = {
      "prepared_template_cached_metadata_only=true",
      "mga_visibility_recheck=preserved",
      "mga_statement_context_exact_match=true",
      "mga_finality_authority=engine_transaction_inventory",
      "security_authorization_recheck=preserved",
      "statement_use_receipt_id=" + result.statement_use_receipt->receipt_id(),
      "statement_use_receipt_immutable=true",
      "statement_use_receipt_executable=false",
      "metadata_dependencies_revalidated=true",
      "catalog_epoch_uuid_rechecked=" + context.catalog_epoch_uuid.canonical,
      "pinned_descriptor_snapshots_consumed=" + std::to_string(prepared_template.pinned_descriptors.size()),
      "pinned_descriptor_set_digest_rechecked=" + prepared_template.key.pinned_descriptor_set_digest,
      "catalog_epoch_rechecked=" + std::to_string(context.catalog_generation_id),
      "security_epoch_rechecked=" + std::to_string(context.security_epoch),
      "policy_resource_epoch_rechecked=" + std::to_string(context.resource_epoch),
      "name_resolution_epoch_rechecked=" + std::to_string(context.name_resolution_epoch),
      "statement_uuid_rechecked=" +
          result.statement_use_receipt->statement_context().statement_uuid,
      "statement_snapshot_uuid_rechecked=" +
          result.statement_use_receipt->statement_context()
              .statement_snapshot_uuid,
      "visibility_snapshot_high_water_rechecked=" +
          std::to_string(result.statement_use_receipt->statement_context()
                             .visible_committed_high_watermark),
  };
  return result;
}

PreparedTemplateUseValidationResult RevalidatePreparedTemplateStatementUse(
    const PreparedExecutionTemplate& prepared_template,
    const std::shared_ptr<const PreparedTemplateStatementUseReceipt>& receipt) {
  PreparedTemplateUseValidationResult result;
  if (!receipt || !receipt->resolve_current_) {
    result.diagnostic_code =
        "SB_PREPARED_TEMPLATE_STATEMENT_USE_RECEIPT_REQUIRED";
    result.detail =
        "an immutable statement-bound use receipt is required before executable use";
    return result;
  }
  if (receipt->prepared_template_id_ != prepared_template.template_id ||
      receipt->catalog_epoch_uuid_ !=
          prepared_template.key.catalog_epoch_uuid ||
      !receipt->metadata_dependencies_revalidated_ ||
      !receipt->security_authorization_recheck_preserved_) {
    result.diagnostic_code =
        "SB_PREPARED_TEMPLATE_STATEMENT_USE_RECEIPT_MISMATCH";
    result.detail =
        "statement-bound use receipt does not match the prepared metadata template";
    return result;
  }
  if (!IsCanonicalUuid(receipt->catalog_epoch_uuid_) ||
      !CatalogEpochUuidIndependent(receipt->catalog_epoch_uuid_,
                                   receipt->statement_context_)) {
    result.diagnostic_code =
        "SB_PREPARED_TEMPLATE_CATALOG_EPOCH_UUID_NOT_INDEPENDENT";
    result.detail =
        "catalog epoch UUID is missing or aliases an MGA statement identity";
    return result;
  }

  CanonicalExecutionMgaAuthority authority;
  authority.statement_context = receipt->statement_context_;
  authority.resolve_current = receipt->resolve_current_;
  authority.origin = receipt->authority_origin_;
  const auto mga_check =
      ResolveExactPreparedMgaStatementContext(authority, nullptr);
  if (!mga_check.ok) {
    result.diagnostic_code = mga_check.diagnostic_code;
    result.detail = mga_check.detail;
    return result;
  }

  result.ok = true;
  result.diagnostic_code = kOk;
  result.executable_receipt = receipt;
  result.evidence = {
      "statement_use_receipt_id=" + receipt->receipt_id_,
      "statement_use_receipt_executable=true",
      "mga_statement_context_exact_match_before_use=true",
      "mga_finality_authority=engine_transaction_inventory",
      "catalog_epoch_uuid_rechecked=" + receipt->catalog_epoch_uuid_,
      "statement_uuid_rechecked=" + receipt->statement_context_.statement_uuid,
      "visibility_snapshot_high_water_rechecked=" +
          std::to_string(
              receipt->statement_context_.visible_committed_high_watermark),
  };
  return result;
}

PreparedTemplateBindResult PreparedTemplateCache::LookupAndBind(const PreparedTemplateKey& key,
                                                                const PreparedTemplateBindContext& bind_context) const {
  const auto prepared_template = Lookup(key);
  if (!prepared_template) {
    PreparedTemplateBindResult result;
    result.ok = false;
    result.diagnostic_code = "SB_PREPARED_TEMPLATE_NOT_FOUND";
    result.detail = "prepared template cache lookup missed";
    return result;
  }
  auto result = Bind(*prepared_template, bind_context);
  if (result.ok) {
    result.prepared_template = prepared_template;
  }
  return result;
}

scratchbird::engine::optimizer::FixedRouteOverheadEvidence
BuildFixedRouteOverheadEvidenceFromPreparedRoute(
    const PreparedRouteOverheadObservation& observation) {
  namespace opt = scratchbird::engine::optimizer;

  opt::FixedRouteOverheadEvidence evidence;
  evidence.route_kind = observation.route_kind;
  evidence.statement_family = observation.statement_family;
  evidence.selected_path = observation.selected_path;
  evidence.benchmark_clean_candidate = observation.benchmark_clean_candidate;
  evidence.lowered_sblr_reused = observation.lowered_sblr_reused;
  evidence.text_rendering_suppressed = observation.text_rendering_suppressed;
  evidence.repeated_parse_count = observation.repeated_parse_count;
  evidence.repeated_lower_count = observation.repeated_lower_count;
  evidence.repeated_descriptor_build_count =
      observation.repeated_descriptor_build_count;
  evidence.repeated_result_shape_build_count =
      observation.repeated_result_shape_build_count;
  evidence.repeated_text_render_count = observation.repeated_text_render_count;
  evidence.route_latency_budget_us = observation.route_latency_budget_us;
  evidence.route_latency_observed_us = observation.route_latency_observed_us;
  evidence.index_dependent = observation.index_dependent;
  evidence.index_correctness_proven = observation.index_correctness_proven;
  evidence.parser_or_cache_executes_sql =
      observation.parser_or_cache_executes_sql;
  evidence.parser_or_cache_owns_transaction_finality =
      observation.parser_or_cache_owns_transaction_finality;
  evidence.transaction_authority = observation.transaction_authority;
  evidence.runtime_evidence = observation.runtime_evidence;
  evidence.fallback_reason = observation.fallback_reason;
  evidence.diagnostic_code =
      observation.diagnostic_code.empty()
          ? "SB_ORH_FIXED_ROUTE_OVERHEAD.PREPARED_ROUTE_OBSERVED"
          : observation.diagnostic_code;

  const auto* prepare = observation.prepare_result;
  const auto* bind = observation.bind_result;
  const bool prepared_template_reused =
      prepare != nullptr && prepare->ok && prepare->reused_existing_template &&
      prepare->prepared_template != nullptr;
  const bool bind_consumed_prepared_metadata =
      bind != nullptr && bind->ok &&
      Contains(bind->evidence, "prepared_template_cached_metadata_only=true");
  const auto* prepared_template =
      prepare != nullptr ? prepare->prepared_template.get() : nullptr;

  evidence.warmed_prepared_route =
      prepared_template_reused && bind_consumed_prepared_metadata;
  evidence.prepared_template_reused = prepared_template_reused;
  evidence.descriptor_reused =
      bind_consumed_prepared_metadata && prepared_template != nullptr &&
      !prepared_template->descriptor_slots.empty();
  evidence.result_shape_reused =
      bind_consumed_prepared_metadata && prepared_template != nullptr &&
      !prepared_template->result_shape.digest.empty();
  if (evidence.selected_path.empty() && prepared_template != nullptr) {
    evidence.selected_path = "prepared_template:" + prepared_template->template_id;
  }
  if (evidence.fallback_reason.empty() && !evidence.warmed_prepared_route) {
    evidence.fallback_reason =
        "prepared template was not reused and bound through the warmed route";
  }
  if (evidence.fallback_reason.empty() &&
      observation.runtime_evidence.runtime_consumed == false) {
    evidence.fallback_reason =
        "route evidence did not prove runtime consumption";
  }
  return evidence;
}

}  // namespace scratchbird::engine::executor
