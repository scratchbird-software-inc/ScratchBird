// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_prepared_template.hpp"

#include <algorithm>
#include <sstream>
#include <utility>

namespace scratchbird::engine::sblr {

namespace exec = scratchbird::engine::executor;
namespace api = scratchbird::engine::internal_api;

namespace {

bool EmptyUuid(const api::EngineUuid& uuid) {
  return uuid.canonical.empty();
}

void AddUuid(std::vector<std::string>* out, const api::EngineUuid& uuid) {
  if (out != nullptr && !EmptyUuid(uuid)) out->push_back(uuid.canonical);
}

std::vector<std::string> UniqueSorted(std::vector<std::string> values) {
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
  return values;
}

bool HasPredicateSlot(const std::vector<exec::PreparedPredicateSlot>& slots,
                      const std::string& stable_name) {
  return std::any_of(slots.begin(), slots.end(), [&](const exec::PreparedPredicateSlot& slot) {
    return slot.stable_name == stable_name;
  });
}

bool HasParameterSlot(const std::vector<exec::PreparedParameterSlot>& slots,
                      const std::string& stable_name) {
  return std::any_of(slots.begin(), slots.end(), [&](const exec::PreparedParameterSlot& slot) {
    return slot.stable_name == stable_name;
  });
}

std::string ProfileDigest(const api::EngineProfileSet& profile_set) {
  std::vector<std::string> parts;
  for (const auto& name : profile_set.names) parts.push_back("name:" + name);
  for (const auto& encoded : profile_set.encoded_profiles) parts.push_back("profile:" + encoded);
  return exec::PreparedTemplateStableDigest(parts);
}

std::string DescriptorSlotName(const api::EngineColumnDefinition& column, std::size_t fallback) {
  if (!column.requested_column_uuid.canonical.empty()) return column.requested_column_uuid.canonical;
  if (!column.names.empty() && !column.names.front().normalized_lookup_key.empty()) {
    return column.names.front().normalized_lookup_key;
  }
  if (!column.names.empty() && !column.names.front().name.empty()) return column.names.front().name;
  return "column:" + std::to_string(fallback);
}

std::vector<exec::PreparedDescriptorSlot> DescriptorSlotsFromRequest(const api::EngineApiRequest& request) {
  std::vector<exec::PreparedDescriptorSlot> slots;
  slots.reserve(request.columns.empty() ? request.descriptors.size() : request.columns.size());
  if (!request.columns.empty()) {
    for (std::size_t i = 0; i < request.columns.size(); ++i) {
      exec::PreparedDescriptorSlot slot;
      slot.stable_name = DescriptorSlotName(request.columns[i], i);
      slot.descriptor = request.columns[i].descriptor;
      slot.ordinal = request.columns[i].ordinal == 0 ? static_cast<std::uint32_t>(i) : request.columns[i].ordinal;
      slots.push_back(std::move(slot));
    }
    return slots;
  }

  for (std::size_t i = 0; i < request.descriptors.size(); ++i) {
    exec::PreparedDescriptorSlot slot;
    slot.stable_name = request.descriptors[i].descriptor_uuid.canonical.empty()
                           ? "descriptor:" + std::to_string(i)
                           : request.descriptors[i].descriptor_uuid.canonical;
    slot.descriptor = request.descriptors[i];
    slot.ordinal = static_cast<std::uint32_t>(i);
    slots.push_back(std::move(slot));
  }
  return slots;
}

std::vector<exec::PreparedFieldOffset> FieldOffsetsFromSlots(
    const std::vector<exec::PreparedDescriptorSlot>& slots) {
  std::vector<exec::PreparedFieldOffset> offsets;
  offsets.reserve(slots.size());
  for (std::size_t i = 0; i < slots.size(); ++i) {
    exec::PreparedFieldOffset offset;
    offset.descriptor_slot = slots[i].stable_name;
    offset.field_name = slots[i].stable_name;
    offset.byte_offset = i * 16;
    offset.byte_width = 16;
    offsets.push_back(std::move(offset));
  }
  return offsets;
}

std::vector<exec::PreparedPredicateSlot> PredicateSlotsFromRequest(
    const SblrOperationEnvelope& envelope,
    const api::EngineApiRequest& request,
    const std::vector<exec::PreparedDescriptorSlot>& descriptor_slots) {
  std::vector<exec::PreparedPredicateSlot> slots;
  const std::string descriptor_slot = descriptor_slots.empty() ? std::string{} : descriptor_slots.front().stable_name;
  if (!request.predicate.predicate_kind.empty() ||
      !request.predicate.canonical_predicate_envelope.empty()) {
    exec::PreparedPredicateSlot slot;
    slot.stable_name = request.predicate.predicate_kind.empty()
                           ? "predicate:canonical"
                           : "predicate:" + request.predicate.predicate_kind;
    slot.descriptor_slot = descriptor_slot;
    slot.required = true;
    slots.push_back(std::move(slot));
  }
  for (const auto& operand : envelope.operands) {
    if (operand.type != "predicate_slot") continue;
    const std::string stable_name = operand.name.empty() ? operand.value : operand.name;
    if (stable_name.empty() || HasPredicateSlot(slots, stable_name)) continue;
    exec::PreparedPredicateSlot slot;
    slot.stable_name = stable_name;
    slot.descriptor_slot = operand.value.empty() ? descriptor_slot : operand.value;
    slot.required = true;
    slots.push_back(std::move(slot));
  }
  return slots;
}

std::vector<exec::PreparedParameterSlot> ParameterSlotsFromRequest(
    const SblrOperationEnvelope& envelope,
    const api::EngineApiRequest& request) {
  std::vector<exec::PreparedParameterSlot> slots;
  for (std::size_t i = 0; i < request.predicate.bound_values.size(); ++i) {
    exec::PreparedParameterSlot slot;
    slot.stable_name = "param:" + std::to_string(i);
    slot.descriptor = request.predicate.bound_values[i].descriptor;
    slot.ordinal = static_cast<std::uint32_t>(i);
    slot.required = true;
    slots.push_back(std::move(slot));
  }
  for (const auto& operand : envelope.operands) {
    if (operand.type != "parameter_slot") continue;
    const std::string stable_name = operand.name.empty() ? operand.value : operand.name;
    if (stable_name.empty() || HasParameterSlot(slots, stable_name)) continue;
    exec::PreparedParameterSlot slot;
    slot.stable_name = stable_name;
    slot.ordinal = static_cast<std::uint32_t>(slots.size());
    slot.required = true;
    slots.push_back(std::move(slot));
  }
  return slots;
}

std::vector<exec::PreparedIndexDescriptor> IndexDescriptorsFromRequest(const api::EngineApiRequest& request) {
  std::vector<exec::PreparedIndexDescriptor> indexes;
  indexes.reserve(request.indexes.size());
  for (const auto& index : request.indexes) {
    exec::PreparedIndexDescriptor prepared;
    prepared.index_uuid = index.requested_index_uuid.canonical;
    prepared.relation_uuid = request.target_object.uuid.canonical;
    prepared.descriptor_digest = exec::PreparedTemplateStableDigest(
        {"index_kind:" + index.index_kind,
         "physical_profile:" + index.physical_profile,
         exec::PreparedTemplateStableDigest(index.key_envelopes)});
    prepared.key_column_uuids = index.key_envelopes;
    prepared.covered_column_uuids = index.key_envelopes;
    prepared.visibility_native = true;
    indexes.push_back(std::move(prepared));
  }
  return indexes;
}

std::vector<std::string> DependenciesFromRequest(const api::EngineApiRequest& request) {
  std::vector<std::string> dependencies;
  AddUuid(&dependencies, request.target_database.uuid);
  AddUuid(&dependencies, request.target_schema.uuid);
  AddUuid(&dependencies, request.target_object.uuid);
  AddUuid(&dependencies, request.bound_object_identity.object_uuid);
  AddUuid(&dependencies, request.bound_object_identity.resolved_schema_uuid);
  AddUuid(&dependencies, request.bound_object_identity.parent_object_uuid);
  for (const auto& object : request.related_objects) AddUuid(&dependencies, object.uuid);
  for (const auto& column : request.columns) AddUuid(&dependencies, column.requested_column_uuid);
  for (const auto& index : request.indexes) AddUuid(&dependencies, index.requested_index_uuid);
  return UniqueSorted(std::move(dependencies));
}

SblrPreparedTemplateBuildResult BuildFailure(std::string code, std::string detail) {
  SblrPreparedTemplateBuildResult result;
  result.ok = false;
  result.diagnostic_code = std::move(code);
  result.detail = std::move(detail);
  return result;
}

std::vector<std::string> PredicateSlotNames(const std::vector<exec::PreparedPredicateSlot>& slots) {
  std::vector<std::string> names;
  names.reserve(slots.size());
  for (const auto& slot : slots) names.push_back(slot.stable_name);
  return names;
}

std::vector<std::string> ParameterSlotNames(const std::vector<exec::PreparedParameterSlot>& slots) {
  std::vector<std::string> names;
  names.reserve(slots.size());
  for (const auto& slot : slots) names.push_back(slot.stable_name);
  return names;
}

}  // namespace

SblrPreparedTemplateBuildResult BuildPreparedTemplateFromSblr(const SblrOperationEnvelope& envelope,
                                                             const api::EngineRequestContext& context,
                                                             const api::EngineApiRequest& request) {
  const auto validation = ValidateSblrEnvelope(envelope);
  if (!validation.ok) {
    const std::string code = validation.diagnostics.empty()
                                 ? "SB_SBLR_PREPARED_TEMPLATE_INVALID_ENVELOPE"
                                 : validation.diagnostics.front().code;
    const std::string detail = validation.diagnostics.empty()
                                   ? "SBLR envelope validation failed"
                                   : validation.diagnostics.front().message;
    return BuildFailure(code, detail);
  }
  if (!request.operation_id.empty() && request.operation_id != envelope.operation_id) {
    return BuildFailure("SB_SBLR_PREPARED_TEMPLATE_OPERATION_MISMATCH",
                        "engine API operation_id does not match the SBLR operation envelope");
  }

  auto descriptor_slots = DescriptorSlotsFromRequest(request);
  if (descriptor_slots.empty()) {
    return BuildFailure("SB_SBLR_PREPARED_TEMPLATE_DESCRIPTOR_REQUIRED",
                        "engine API descriptors or columns are required");
  }

  exec::PreparedResultShapeDescriptor result_shape;
  result_shape.result_kind = envelope.result_shape;
  result_shape.columns = descriptor_slots;
  result_shape.digest = exec::PreparedResultShapeDigest(result_shape);

  exec::PreparedTemplateAdmission admission;
  admission.descriptor_slots = descriptor_slots;
  admission.field_offsets = FieldOffsetsFromSlots(admission.descriptor_slots);
  admission.result_shape = result_shape;
  admission.predicate_slots = PredicateSlotsFromRequest(envelope, request, admission.descriptor_slots);
  admission.parameter_slots = ParameterSlotsFromRequest(envelope, request);
  admission.index_descriptors = IndexDescriptorsFromRequest(request);
  admission.policy_metadata.security_policy_digest = ProfileDigest(request.policy_profile);
  admission.policy_metadata.visibility_policy_digest =
      exec::PreparedTemplateStableDigest({"visibility_epoch:" + std::to_string(context.snapshot_visible_through_local_transaction_id),
                                          "isolation:" + context.transaction_isolation_level});
  admission.policy_metadata.authorization_policy_digest =
      exec::PreparedTemplateStableDigest({"principal:" + context.principal_uuid.canonical,
                                          "role:" + context.current_role_uuid.canonical});
  admission.policy_metadata.requires_security_context = envelope.requires_security_context;
  admission.policy_metadata.requires_transaction_context = envelope.requires_transaction_context;

  const std::vector<std::string> dependencies = DependenciesFromRequest(request);
  admission.key.operation_id = envelope.operation_id;
  admission.key.sblr_digest_or_trace_key = envelope.trace_key.empty()
                                               ? exec::PreparedTemplateStableDigest({EncodeSblrEnvelope(envelope)})
                                               : envelope.trace_key;
  admission.key.descriptor_set_digest = exec::PreparedDescriptorSetDigest(request.descriptors, request.columns);
  admission.key.result_shape_digest = result_shape.digest;
  admission.key.epochs.catalog_epoch = context.catalog_generation_id;
  admission.key.epochs.security_epoch = context.security_epoch;
  admission.key.epochs.policy_resource_epoch = context.resource_epoch;
  admission.key.epochs.name_resolution_epoch = context.name_resolution_epoch;
  admission.key.epochs.transaction_visibility_epoch = context.snapshot_visible_through_local_transaction_id;
  admission.key.epochs.transaction_visibility_epoch_relevant =
      envelope.requires_transaction_context || context.snapshot_visible_through_local_transaction_id != 0;
  admission.key.dependency_uuids = dependencies;

  exec::PreparedTemplateBindContext bind_context;
  bind_context.engine_context = context;
  bind_context.request = request;
  bind_context.descriptor_set_digest = admission.key.descriptor_set_digest;
  bind_context.result_shape_digest = admission.key.result_shape_digest;
  bind_context.dependency_uuids = dependencies;
  bind_context.available_predicate_slots = PredicateSlotNames(admission.predicate_slots);
  bind_context.available_parameter_slots = ParameterSlotNames(admission.parameter_slots);

  SblrPreparedTemplateBuildResult result;
  result.ok = true;
  result.diagnostic_code = "SB_SBLR_PREPARED_TEMPLATE_OK";
  result.admission = std::move(admission);
  result.bind_context = std::move(bind_context);
  result.evidence = {
      "sblr_prepared_template_source=operation_envelope",
      "parser_sql_text_authority=false",
      "uuid_bound_descriptors_authority=true",
      "engine_context_epoch_bound=true",
  };
  return result;
}

exec::PreparedTemplatePrepareResult PrepareSblrExecutionTemplate(exec::PreparedTemplateCache* cache,
                                                                 const SblrOperationEnvelope& envelope,
                                                                 const api::EngineRequestContext& context,
                                                                 const api::EngineApiRequest& request) {
  if (cache == nullptr) {
    exec::PreparedTemplatePrepareResult result;
    result.ok = false;
    result.diagnostic_code = "SB_SBLR_PREPARED_TEMPLATE_CACHE_REQUIRED";
    result.detail = "prepared template cache is required";
    return result;
  }
  auto build = BuildPreparedTemplateFromSblr(envelope, context, request);
  if (!build.ok) {
    exec::PreparedTemplatePrepareResult result;
    result.ok = false;
    result.diagnostic_code = std::move(build.diagnostic_code);
    result.detail = std::move(build.detail);
    return result;
  }
  return cache->Prepare(std::move(build.admission));
}

}  // namespace scratchbird::engine::sblr
