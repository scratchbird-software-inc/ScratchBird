// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "prepared_execution_template.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
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

std::string DescriptorText(const EngineDescriptor& descriptor) {
  return descriptor.descriptor_uuid.canonical + ":" + descriptor.descriptor_kind + ":" +
         descriptor.canonical_type_name + ":" + descriptor.encoded_descriptor;
}

std::string EpochText(const PreparedTemplateEpochs& epochs) {
  std::ostringstream out;
  out << "catalog=" << epochs.catalog_epoch
      << "|security=" << epochs.security_epoch
      << "|policy_resource=" << epochs.policy_resource_epoch
      << "|name_resolution=" << epochs.name_resolution_epoch
      << "|visibility_relevant=" << (epochs.transaction_visibility_epoch_relevant ? "true" : "false")
      << "|visibility=" << epochs.transaction_visibility_epoch;
  return out.str();
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
        descriptor.descriptor_set_digest.empty() ||
        descriptor.object_uuid.empty() ||
        descriptor.security_policy_identity.empty() ||
        descriptor.redaction_policy_identity.empty()) {
      return "pinned descriptor cache key, object UUID, descriptor digest, and policy identities are required";
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
  if (key.epochs.transaction_visibility_epoch_relevant &&
      context.snapshot_visible_through_local_transaction_id != key.epochs.transaction_visibility_epoch) {
    return BindFailure(prepared_template,
                       "SB_PREPARED_TEMPLATE_STALE_VISIBILITY_EPOCH",
                       "visibility snapshot epoch changed from " +
                           std::to_string(key.epochs.transaction_visibility_epoch) + " to " +
                           std::to_string(context.snapshot_visible_through_local_transaction_id));
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

  PreparedTemplateBindResult result;
  result.ok = true;
  result.diagnostic_code = kOk;
  (void)prepared_template;
  result.evidence = {
      "prepared_template_cached_metadata_only=true",
      "mga_visibility_recheck=preserved",
      "mga_finality_authority=engine_transaction_inventory",
      "security_authorization_recheck=preserved",
      "pinned_descriptor_snapshots_consumed=" + std::to_string(prepared_template.pinned_descriptors.size()),
      "pinned_descriptor_set_digest_rechecked=" + prepared_template.key.pinned_descriptor_set_digest,
      "catalog_epoch_rechecked=" + std::to_string(context.catalog_generation_id),
      "security_epoch_rechecked=" + std::to_string(context.security_epoch),
      "policy_resource_epoch_rechecked=" + std::to_string(context.resource_epoch),
      "name_resolution_epoch_rechecked=" + std::to_string(context.name_resolution_epoch),
      "visibility_snapshot_epoch_rechecked=" +
          std::to_string(context.snapshot_visible_through_local_transaction_id),
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
