// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_prepared_template.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "uuid.hpp"
#include <map>
#include <optional>

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string_view>
#include <utility>

namespace scratchbird::engine::sblr {

namespace exec = scratchbird::engine::executor;
namespace api = scratchbird::engine::internal_api;

namespace {

bool EmptyUuid(const api::EngineUuid& uuid) {
  return uuid.is_nil();
}

bool IsCanonicalUuid(const api::EngineUuid& value) {
  return core::uuid::IsEngineIdentityUuid(value);
}

void AddUuid(std::vector<api::EngineUuid>* out, const api::EngineUuid& uuid) {
  if (out != nullptr && !EmptyUuid(uuid)) out->push_back(uuid);
}

std::vector<api::EngineUuid> UniqueSorted(std::vector<api::EngineUuid> values) {
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
    slot.stable_name = "descriptor:" + std::to_string(i);
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

std::optional<std::vector<exec::PreparedIndexDescriptor>> IndexDescriptorsFromRequest(
    const api::EngineRequestContext& context, const api::EngineApiRequest& request) {
  std::vector<exec::PreparedIndexDescriptor> indexes;
  if (request.indexes.empty()) return indexes;
  std::map<std::string, api::EngineUuid> columns;
  const auto add_column = [&](const std::string& name, const api::EngineUuid& uuid) {
    if (name.empty() || !IsCanonicalUuid(uuid)) return false;
    const auto [found, inserted] = columns.emplace(name, uuid);
    return inserted || found->second == uuid;
  };
  // Existing relations take their UUID/name bindings from the engine descriptor.
  // New relation declarations already carry bound column identities in request.
  const auto relation = api::LoadMgaRelationStorageDescriptor(context, request.target_object.uuid);
  if (relation.ok) {
    for (const auto& column : relation.descriptor.columns)
      if (!add_column(column.canonical_name_key, column.column_uuid)) return std::nullopt;
  }
  for (const auto& column : request.columns) {
    for (const auto& name : column.names) {
      const auto& key = name.normalized_lookup_key.empty() ? name.name : name.normalized_lookup_key;
      if (!add_column(key, column.requested_column_uuid)) return std::nullopt;
    }
  }
  const auto bind_column = [&](std::string_view name, std::vector<api::EngineUuid>* output) {
    const auto found = columns.find(std::string(name));
    if (found == columns.end()) return false;
    if (std::find(output->begin(), output->end(), found->second) == output->end())
      output->push_back(found->second);
    return true;
  };
  for (const auto& index : request.indexes) {
    exec::PreparedIndexDescriptor prepared;
    prepared.index_uuid = index.requested_index_uuid;
    prepared.relation_uuid = request.target_object.uuid;
    prepared.descriptor_digest = exec::PreparedTemplateStableDigest(
        {"index_kind:" + index.index_kind, "physical_profile:" + index.physical_profile,
         exec::PreparedTemplateStableDigest(index.key_envelopes)});
    for (const auto& encoded : index.key_envelopes) {
      std::string_view key(encoded);
      if (key == "unique" || key == "primary_key" || key == "where_true") continue;
      auto* destination = &prepared.key_column_uuids;
      if (key.starts_with("include:")) {
        key.remove_prefix(8);
        destination = &prepared.covered_column_uuids;
        while (true) {
          const auto comma = key.find(',');
          if (!bind_column(key.substr(0, comma), destination)) return std::nullopt;
          if (comma == std::string_view::npos) break;
          key.remove_prefix(comma + 1);
        }
        continue;
      }
      if (key.starts_with("where_eq:") || key.starts_with("where_mod_eq:")) {
        key.remove_prefix(key.starts_with("where_eq:") ? 9 : 13);
        if (!bind_column(key.substr(0, key.find(':')), &prepared.covered_column_uuids)) return std::nullopt;
        continue;
      }
      if (key.starts_with("sum:")) {
        key.remove_prefix(4);
        const auto colon = key.find(':');
        if (colon == std::string_view::npos || !bind_column(key.substr(0, colon), destination) ||
            !bind_column(key.substr(colon + 1), destination)) return std::nullopt;
        continue;
      }
      if (key.starts_with("cast:")) {
        key.remove_prefix(5); key = key.substr(0, key.find(':'));
      } else {
        for (const std::string_view prefix : {"desc:", "lower:", "upper:", "length:", "identity:"}) {
          if (key.starts_with(prefix)) { key.remove_prefix(prefix.size()); break; }
        }
        for (const std::string_view function : {"lower(", "upper(", "length("}) {
          if (key.starts_with(function) && key.ends_with(')')) {
            key.remove_prefix(function.size()); key.remove_suffix(1); break;
          }
        }
      }
      if (!bind_column(key, destination)) return std::nullopt;
    }
    for (const auto& uuid : prepared.key_column_uuids)
      if (std::find(prepared.covered_column_uuids.begin(), prepared.covered_column_uuids.end(), uuid) == prepared.covered_column_uuids.end())
        prepared.covered_column_uuids.push_back(uuid);
    prepared.visibility_native = true;
    indexes.push_back(std::move(prepared));
  }
  return indexes;
}

std::vector<api::EngineUuid> DependenciesFromRequest(const api::EngineApiRequest& request) {
  std::vector<api::EngineUuid> dependencies;
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
                                                             const api::EngineApiRequest& request,
                                                             exec::CanonicalExecutionMgaAuthority
                                                                 mga_authority) {
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
  if (!IsCanonicalUuid(context.catalog_epoch_uuid)) {
    return BuildFailure("SB_SBLR_PREPARED_TEMPLATE_CATALOG_EPOCH_UUID_REQUIRED",
                        "engine context must carry a canonical catalog epoch UUID");
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
  auto index_descriptors = IndexDescriptorsFromRequest(context, request);
  if (!index_descriptors) return BuildFailure("SB_PREPARED_TEMPLATE_DESCRIPTOR_MISMATCH",
      "index dependency has no unambiguous native column binding");
  admission.index_descriptors = std::move(*index_descriptors);
  admission.policy_metadata.security_policy_digest = ProfileDigest(request.policy_profile);
  admission.policy_metadata.visibility_policy_digest =
      exec::PreparedTemplateStableDigest(
          {"visibility_recheck:engine_statement_use",
           "isolation:" + context.transaction_isolation_level});
  admission.policy_metadata.authorization_policy_digest =
      exec::PreparedAuthorizationDigest(context.principal_uuid, context.current_role_uuid);
  admission.policy_metadata.requires_security_context = envelope.requires_security_context;
  admission.policy_metadata.requires_transaction_context = envelope.requires_transaction_context;

  const std::vector<api::EngineUuid> dependencies = DependenciesFromRequest(request);
  admission.key.operation_id = envelope.operation_id;
  admission.key.sblr_digest_or_trace_key = envelope.trace_key.empty()
                                               ? exec::PreparedTemplateStableDigest({EncodeSblrEnvelope(envelope)})
                                               : envelope.trace_key;
  admission.key.catalog_epoch_uuid = context.catalog_epoch_uuid;
  admission.key.descriptor_set_digest = exec::PreparedDescriptorSetDigest(request.descriptors, request.columns);
  admission.key.result_shape_digest = result_shape.digest;
  admission.key.epochs.catalog_epoch = context.catalog_generation_id;
  admission.key.epochs.security_epoch = context.security_epoch;
  admission.key.epochs.policy_resource_epoch = context.resource_epoch;
  admission.key.epochs.name_resolution_epoch = context.name_resolution_epoch;
  admission.key.dependency_uuids = dependencies;

  exec::PreparedTemplateBindContext bind_context;
  bind_context.engine_context = context;
  bind_context.request = request;
  bind_context.mga_authority = std::move(mga_authority);
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
      "catalog_epoch_uuid_bound=true",
      "shared_template_transaction_visibility_authority=false",
  };
  result.identity_evidence.push_back({"catalog_epoch_uuid", context.catalog_epoch_uuid});
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
