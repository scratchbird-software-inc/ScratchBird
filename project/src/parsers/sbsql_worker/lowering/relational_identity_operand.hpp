// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "lowering/lowering.hpp"
#include "engine/sblr/relational_identity_codec.hpp"
#include "engine/sblr/relational_descriptor_codec.hpp"
#include "engine/sblr/sblr_bound_object_identity.hpp"
#include <algorithm>
#include <iterator>
#include <span>
#include <utility>

namespace scratchbird::parser::sbsql {

enum class BoundTargetRequirement { none, exactly_one };

struct BoundPolicyObjectIdentities {
  core::platform::Uuid policy_uuid;
  core::platform::Uuid target_object_uuid;
  core::platform::Uuid role_uuid;
  core::platform::Uuid target_schema_uuid;
  bool operator==(const BoundPolicyObjectIdentities&) const = default;
};

// The syntax/binder contract orders policy, target, optional subject, optional
// creation schema. Presence is explicit: never accept a missing subject/schema
// or silently consume an unrelated tail as that binding.
inline bool BindPolicyObjectIdentities(
    std::span<const core::platform::Uuid> resolved, bool subject_required,
    bool schema_required, BoundPolicyObjectIdentities* output) noexcept {
  const std::size_t count = 2 + static_cast<std::size_t>(subject_required) +
      static_cast<std::size_t>(schema_required);
  if (!output || resolved.size() != count ||
      !std::all_of(resolved.begin(), resolved.end(), core::uuid::IsEngineIdentityUuid))
    return false;
  BoundPolicyObjectIdentities bound;
  bound.policy_uuid = resolved[0];
  bound.target_object_uuid = resolved[1];
  if (subject_required) bound.role_uuid = resolved[2];
  if (schema_required) bound.target_schema_uuid = resolved[2 + subject_required];
  *output = bound;
  return true;
}

inline bool BindSecurityDclIdentityPair(
    std::span<const core::platform::Uuid> resolved,
    core::platform::Uuid* first, core::platform::Uuid* second) noexcept {
  if (!first || !second || first == second || resolved.size() != 2 ||
      !core::uuid::IsEngineIdentityUuid(resolved[0]) ||
      !core::uuid::IsEngineIdentityUuid(resolved[1])) return false;
  const auto first_value = resolved[0];
  const auto second_value = resolved[1];
  *first = first_value;
  *second = second_value;
  return true;
}

// Absence is an explicit operation contract, never the fallback for a failed
// lookup. Creation/session-wide operations cannot adopt a spare bound UUID.
inline bool BindOperationTargetIdentity(
    std::span<const core::platform::Uuid> resolved, BoundTargetRequirement requirement,
    core::platform::Uuid* target) noexcept {
  if (!target) return false;
  if (requirement == BoundTargetRequirement::none) {
    if (!resolved.empty()) return false;
    *target = {};
    return true;
  }
  if (requirement != BoundTargetRequirement::exactly_one || resolved.size() != 1 ||
      !core::uuid::IsEngineIdentityUuid(resolved.front())) return false;
  *target = resolved.front();
  return true;
}

// ALTER/RENAME/DROP's single-target forms resolve exactly one existing object.
// An unlabelled second identity is not schema authority. Parent/move operands
// require their own explicit binding rather than positional inference here.
inline bool BindSingleDdlTargetIdentity(
    std::span<const core::platform::Uuid> resolved, core::platform::Uuid* target) noexcept {
  return BindOperationTargetIdentity(resolved, BoundTargetRequirement::exactly_one, target);
}

// The caller's operation binding contract assigns the first slot as target
// and the remaining slots as ordered dependencies. This does not establish
// creation-reservation provenance, catalog visibility or authorization.
inline bool BindTargetWithRelatedIdentities(
    std::span<const core::platform::Uuid> resolved, core::platform::Uuid* target,
    std::span<const core::platform::Uuid>* related) noexcept {
  if (!target || !related || resolved.empty() ||
      std::ranges::any_of(resolved, [](const auto& identity) {
        return !core::uuid::IsEngineIdentityUuid(identity);
      })) return false;
  *target = resolved.front();
  *related = resolved.subspan(1);
  return true;
}

// Parser-local binding roles. These carry already-bound object identities;
// neither JSON renderings nor literal SQL UUID values are identity authority.
inline bool IsDmlObjectIdentitySlot(std::string_view name) noexcept {
  std::size_t index = 0;
  std::string_view path;
  return engine::sblr::IsBoundObjectIdentityRole(name) ||
      engine::sblr::DecodeProjectionFunctionIdentityPath(name, &path) ||
      engine::sblr::DecodeRelatedObjectIdentityIndex(name, &index);
}

inline std::optional<SblrOperand> MakeDmlObjectIdentityOperand(
    std::string_view name, const core::platform::Uuid& identity) {
  if (!IsDmlObjectIdentitySlot(name) || !core::uuid::IsEngineIdentityUuid(identity)) return std::nullopt;
  SblrOperand result;
  result.type = "uuid"; result.name = name;
  result.canonical_value_kind = static_cast<std::uint16_t>(engine::sblr::SblrValueKind::uuid_ref);
  result.canonical_value_body.assign(identity.bytes.begin(), identity.bytes.end());
  return result;
}

// Publish a complete set of bound roles, never a prefix of a rejected set.
// Membership is checked against the binder's binary identity cohort; names,
// JSON fields and parser-generated identities cannot supply that authority.
inline bool AppendBoundDmlObjectIdentities(
    SblrEnvelope* envelope,
    std::span<const std::pair<std::string_view, core::platform::Uuid>> identities) {
  if (!envelope) return false;
  std::vector<SblrOperand> staged;
  staged.reserve(identities.size());
  for (const auto& [role, identity] : identities) {
    if (std::find(envelope->resolved_object_uuids.begin(),
                  envelope->resolved_object_uuids.end(), identity) ==
            envelope->resolved_object_uuids.end() ||
        std::ranges::any_of(envelope->operands, [&](const auto& operand) {
          return operand.name == role;
        }) ||
        std::ranges::any_of(staged, [&](const auto& operand) {
          return operand.name == role;
        })) return false;
    auto operand = MakeDmlObjectIdentityOperand(role, identity);
    if (!operand) return false;
    staged.push_back(std::move(*operand));
  }
  envelope->operands.insert(envelope->operands.end(),
      std::make_move_iterator(staged.begin()), std::make_move_iterator(staged.end()));
  return true;
}

inline bool AppendBoundQueryObjectIdentities(
    SblrEnvelope* envelope, const core::platform::Uuid& target,
    std::span<const core::platform::Uuid> related) {
  if (!envelope || std::ranges::any_of(envelope->operands, [](const auto& operand) {
        return engine::sblr::IsRelatedObjectIdentityRole(operand.name);
      })) return false;
  std::vector<std::string> names;
  names.reserve(related.size());
  for (std::size_t i = 0; i < related.size(); ++i)
    names.push_back("related_object_" + std::to_string(i) + "_uuid");
  std::vector<std::pair<std::string_view, core::platform::Uuid>> identities;
  identities.reserve(related.size() + (target.is_nil() ? 0 : 1));
  if (!target.is_nil()) identities.emplace_back("target_object_uuid", target);
  for (std::size_t i = 0; i < related.size(); ++i) identities.emplace_back(names[i], related[i]);
  return AppendBoundDmlObjectIdentities(envelope, identities);
}

// Table-backed single-source adapters require a real target; nil is reserved
// for explicitly source-free profiles handled by their own lowering routes.
inline bool AppendBoundSingleQueryObjectIdentity(
    SblrEnvelope* envelope, const core::platform::Uuid& target) {
  return !target.is_nil() && AppendBoundQueryObjectIdentities(envelope, target, {});
}

inline bool DecodeDmlObjectIdentityOperand(const SblrOperand& operand, core::platform::Uuid* output) noexcept {
  if (!output || !IsDmlObjectIdentitySlot(operand.name) || operand.type != "uuid" || !operand.value.empty() ||
      operand.canonical_value_kind != static_cast<std::uint16_t>(engine::sblr::SblrValueKind::uuid_ref) ||
      operand.canonical_value_body.size() != 16) return false;
  core::platform::Uuid value;
  std::copy(operand.canonical_value_body.begin(), operand.canonical_value_body.end(), value.bytes.begin());
  if (!core::uuid::IsEngineIdentityUuid(value)) return false;
  *output = value; return true;
}

inline std::optional<core::platform::Uuid> FindDmlObjectIdentity(
    const SblrEnvelope& envelope, std::string_view role) noexcept {
  if (!IsDmlObjectIdentitySlot(role)) return std::nullopt;
  std::optional<core::platform::Uuid> found;
  for (const auto& operand : envelope.operands) {
    if (operand.name != role) continue;
    core::platform::Uuid identity;
    if (found || !DecodeDmlObjectIdentityOperand(operand, &identity) ||
        std::find(envelope.resolved_object_uuids.begin(), envelope.resolved_object_uuids.end(), identity) ==
            envelope.resolved_object_uuids.end()) return std::nullopt;
    found = identity;
  }
  return found;
}

// Check every declared identity, not just a requested first slot. A malformed
// index must not escape verification because it is not a recognized valid role.
// The parser ordinal is assigned by the wire encoder; this carrier has no
// independent ordinal/flags fields to trust.
inline bool ValidateBoundObjectIdentityOperands(const SblrEnvelope& envelope) noexcept {
  const auto related_count = static_cast<std::size_t>(std::ranges::count_if(
      envelope.operands, [](const auto& operand) {
        return engine::sblr::IsRelatedObjectIdentityRole(operand.name);
      }));
  for (const auto& operand : envelope.operands) {
    if (engine::sblr::IsLegacyCatalogIdentityAlias(operand.name)) return false;
    const bool related = engine::sblr::IsRelatedObjectIdentityRole(operand.name);
    const bool function = engine::sblr::IsProjectionFunctionIdentityRole(operand.name);
    if (!related && !function && !engine::sblr::IsBoundObjectIdentityRole(operand.name)) continue;
    if (!FindDmlObjectIdentity(envelope, operand.name)) return false;
    if (related) {
      std::size_t index = 0;
      if (!engine::sblr::DecodeRelatedObjectIdentityIndex(operand.name, &index) ||
          index >= related_count) return false;
    }
  }
  // Unique role names, exactly related_count distinct in-range indices imply
  // the complete contiguous cohort; repeated UUID values remain valid.
  return true;
}

inline std::optional<SblrOperand> MakeRelationalRowPatternOperand(
    const engine::internal_api::RelationalRowPatternRecord& pattern) {
  SblrOperand operand;
  operand.type = "relational_row_pattern_v2";
  operand.name = "slot_" + std::to_string(pattern.pattern_id);
  operand.canonical_value_kind = static_cast<std::uint16_t>(engine::sblr::SblrValueKind::relational_row_pattern);
  if (!engine::sblr::EncodeRelationalRowPatternV1(pattern, &operand.canonical_value_body)) return std::nullopt;
  return operand;
}

inline bool DecodeRelationalRowPatternOperand(
    const SblrOperand& operand, engine::internal_api::RelationalRowPatternRecord* output) {
  engine::internal_api::RelationalRowPatternRecord decoded;
  if (!output || operand.type != "relational_row_pattern_v2" || !operand.value.empty() ||
      operand.canonical_value_kind != static_cast<std::uint16_t>(engine::sblr::SblrValueKind::relational_row_pattern) ||
      !engine::sblr::DecodeRelationalRowPatternV1(operand.canonical_value_body.data(), operand.canonical_value_body.size(), &decoded) ||
      operand.name != "slot_" + std::to_string(decoded.pattern_id)) return false;
  *output = std::move(decoded);
  return true;
}

inline std::optional<SblrOperand> MakeRelationalRowPatternOperand(
    const BoundRowPatternAstRecord& pattern,
    std::vector<engine::internal_api::RelationalPropertyOrderingTerm> ordering_terms) {
  if (pattern.ordering_terms.size() != ordering_terms.size()) return std::nullopt;
  for (std::size_t i = 0; i < ordering_terms.size(); ++i) {
    const auto& source = pattern.ordering_terms[i]; const auto& resolved = ordering_terms[i];
    if (source.expression_id != resolved.expression_id ||
        static_cast<unsigned>(source.direction) + 1 != static_cast<unsigned>(resolved.direction) ||
        static_cast<unsigned>(source.null_placement) + 1 != static_cast<unsigned>(resolved.null_placement)) return std::nullopt;
  }
  engine::internal_api::RelationalRowPatternRecord wire;
  wire.pattern_id = pattern.pattern_id; wire.relation_node_id = pattern.relation_id;
  wire.partition_expression_ids = pattern.partition_expression_ids; wire.ordering_terms = std::move(ordering_terms);
  wire.measure_expression_ids = pattern.measure_expression_ids;
  wire.rows_per_match = static_cast<engine::internal_api::RelationalRowPatternRowsPerMatch>(pattern.rows_per_match);
  wire.after_match_skip = static_cast<engine::internal_api::RelationalRowPatternAfterMatchSkip>(pattern.after_match_skip);
  wire.skip_target_key = pattern.skip_target_key;
  wire.maximum_partition_rows = pattern.maximum_partition_rows; wire.maximum_active_states = pattern.maximum_active_states;
  wire.maximum_output_rows = pattern.maximum_output_rows; wire.stable_row_identity_tie_break_allowed = pattern.stable_row_identity_tie_break_allowed;
  for (const auto& source : pattern.variables) {
    engine::internal_api::RelationalRowPatternVariableRecord variable;
    variable.canonical_name_key = source.canonical_name_key; variable.minimum_occurrences = source.minimum_occurrences;
    variable.maximum_occurrences = source.maximum_occurrences; variable.reluctant = source.reluctant;
    variable.define_expression_id = source.define_expression_id; variable.define_always_true = source.define_always_true;
    wire.variables.push_back(std::move(variable));
  }
  return MakeRelationalRowPatternOperand(wire);
}

inline std::optional<SblrOperand> MakeRelationalPropertyOperand(
    const engine::internal_api::RelationalPropertyRecord& property) {
  SblrOperand operand;
  operand.type = "relational_property_v3";
  operand.name = "property";
  operand.canonical_value_kind = static_cast<std::uint16_t>(engine::sblr::SblrValueKind::relational_property);
  if (!engine::sblr::EncodeRelationalPropertyV1(property, &operand.canonical_value_body)) return std::nullopt;
  return operand;
}

inline bool DecodeRelationalPropertyOperand(
    const SblrOperand& operand, engine::internal_api::RelationalPropertyRecord* output) {
  if (!output || operand.type != "relational_property_v3" || operand.name != "property" || !operand.value.empty() ||
      operand.canonical_value_kind != static_cast<std::uint16_t>(engine::sblr::SblrValueKind::relational_property)) return false;
  return engine::sblr::DecodeRelationalPropertyV1(operand.canonical_value_body.data(), operand.canonical_value_body.size(), output);
}

inline std::optional<SblrOperand> MakeRelationalWindowDefinitionOperand(
    const engine::internal_api::RelationalWindowDefinitionRecord& definition) {
  SblrOperand operand;
  operand.type = "relational_window_definition_v2";
  operand.name = "slot_" + std::to_string(definition.window_id);
  operand.canonical_value_kind = static_cast<std::uint16_t>(engine::sblr::SblrValueKind::relational_window_definition);
  if (!engine::sblr::EncodeRelationalWindowDefinitionV1(definition, &operand.canonical_value_body)) return std::nullopt;
  return operand;
}

inline bool DecodeRelationalWindowDefinitionOperand(
    const SblrOperand& operand, engine::internal_api::RelationalWindowDefinitionRecord* output) {
  engine::internal_api::RelationalWindowDefinitionRecord decoded;
  if (!output || operand.type != "relational_window_definition_v2" || !operand.value.empty() ||
      operand.canonical_value_kind != static_cast<std::uint16_t>(engine::sblr::SblrValueKind::relational_window_definition) ||
      !engine::sblr::DecodeRelationalWindowDefinitionV1(operand.canonical_value_body.data(),
          operand.canonical_value_body.size(), &decoded) ||
      operand.name != "slot_" + std::to_string(decoded.window_id)) return false;
  *output = std::move(decoded);
  return true;
}

inline std::optional<SblrOperand> MakeRelationalWindowDefinitionOperand(
    const BoundWindowDefinitionAstRecord& definition, std::uint32_t node_id,
    std::vector<engine::internal_api::RelationalPropertyOrderingTerm> ordering_terms) {
  if (definition.ordering_terms.size() != ordering_terms.size()) return std::nullopt;
  for (std::size_t index = 0; index < ordering_terms.size(); ++index) {
    const auto& source = definition.ordering_terms[index];
    const auto& resolved = ordering_terms[index];
    if (source.expression_id != resolved.expression_id ||
        static_cast<unsigned>(source.direction) + 1 != static_cast<unsigned>(resolved.direction) ||
        static_cast<unsigned>(source.null_placement) + 1 != static_cast<unsigned>(resolved.null_placement)) return std::nullopt;
  }
  engine::internal_api::RelationalWindowDefinitionRecord wire;
  wire.window_id = definition.window_id; wire.relation_node_id = node_id;
  wire.canonical_name_key = definition.canonical_name_key;
  wire.inherited_window_id = definition.inherited_window_id;
  wire.partition_expression_ids = definition.partition_expression_ids;
  wire.ordering_terms = std::move(ordering_terms);
  if (definition.frame_unit)
    wire.frame_unit = static_cast<engine::internal_api::RelationalWindowFrameUnit>(
        static_cast<unsigned>(*definition.frame_unit) + 1);
  const auto bound = [](const std::optional<BoundWindowFrameBoundAstRecord>& value)
      -> std::optional<engine::internal_api::RelationalWindowFrameBoundRecord> {
    if (!value) return std::nullopt;
    engine::internal_api::RelationalWindowFrameBoundRecord result;
    result.bound_kind = static_cast<engine::internal_api::RelationalWindowFrameBoundKind>(
        static_cast<unsigned>(value->bound_kind) + 1);
    result.offset_expression_id = value->offset_expression_id;
    return result;
  };
  wire.frame_start = bound(definition.frame_start); wire.frame_end = bound(definition.frame_end);
  wire.exclusion = static_cast<engine::internal_api::RelationalWindowFrameExclusion>(
      static_cast<unsigned>(definition.exclusion) + 1);
  return MakeRelationalWindowDefinitionOperand(wire);
}

inline std::optional<SblrOperand> MakeRelationalWindowInvocationOperand(
    const BoundWindowInvocationAstRecord& invocation, std::uint32_t node_id) {
  engine::internal_api::RelationalWindowInvocationRecord wire;
  wire.invocation_id = invocation.invocation_id;
  wire.relation_node_id = node_id;
  wire.function_expression_id = invocation.function_expression_id;
  wire.window_definition_id = invocation.window_definition_id;
  wire.function_abi_version = invocation.function_abi_version;
  wire.builtin_id = invocation.builtin_id;
  wire.function_uuid = invocation.bound_function_uuid;
  wire.result_descriptor_id = invocation.result_descriptor_id;
  wire.output_name_utf8 = invocation.output_name_utf8.value_or("");
  wire.argument_expression_ids = invocation.argument_expression_ids;
  SblrOperand operand;
  operand.type = "relational_window_invocation_v2";
  operand.name = "slot_" + std::to_string(wire.invocation_id);
  operand.canonical_value_kind = static_cast<std::uint16_t>(engine::sblr::SblrValueKind::relational_window_invocation);
  if (!engine::sblr::EncodeRelationalWindowInvocationV1(wire, &operand.canonical_value_body)) return std::nullopt;
  return operand;
}

inline bool DecodeRelationalWindowInvocationOperand(
    const SblrOperand& operand, engine::internal_api::RelationalWindowInvocationRecord* output) {
  engine::internal_api::RelationalWindowInvocationRecord decoded;
  if (!output || operand.type != "relational_window_invocation_v2" || !operand.value.empty() ||
      operand.canonical_value_kind != static_cast<std::uint16_t>(engine::sblr::SblrValueKind::relational_window_invocation) ||
      !engine::sblr::DecodeRelationalWindowInvocationV1(operand.canonical_value_body.data(),
          operand.canonical_value_body.size(), &decoded) ||
      operand.name != "slot_" + std::to_string(decoded.invocation_id)) return false;
  *output = std::move(decoded);
  return true;
}

inline std::optional<SblrOperand> MakeRelationalNodeBindingOperand(
    const engine::sblr::RelationalNodeBindingRecord& binding) {
  SblrOperand operand;
  operand.type = "relational_node_binding_v2";
  operand.name = "slot_" + std::to_string(binding.node_id);
  operand.canonical_value_kind = static_cast<std::uint16_t>(engine::sblr::SblrValueKind::relational_node_binding);
  if (!engine::sblr::EncodeRelationalNodeBindingV1(binding, &operand.canonical_value_body)) return std::nullopt;
  return operand;
}

inline bool DecodeRelationalNodeBindingOperand(
    const SblrOperand& operand, engine::sblr::RelationalNodeBindingRecord* output) {
  engine::sblr::RelationalNodeBindingRecord decoded;
  if (!output || operand.type != "relational_node_binding_v2" || !operand.value.empty() ||
      operand.canonical_value_kind != static_cast<std::uint16_t>(engine::sblr::SblrValueKind::relational_node_binding) ||
      !engine::sblr::DecodeRelationalNodeBindingV1(operand.canonical_value_body.data(),
          operand.canonical_value_body.size(), &decoded) ||
      operand.name != "slot_" + std::to_string(decoded.node_id)) return false;
  *output = std::move(decoded);
  return true;
}

inline std::optional<SblrOperand> MakeRelationalExpressionOperand(
    const BoundExpressionAstRecord& expression) {
  engine::internal_api::RelationalExpressionRecord wire;
  wire.expression_id = expression.expression_id;
  wire.expression_kind = static_cast<engine::internal_api::RelationalExpressionKind>(
      static_cast<std::uint8_t>(expression.expression_kind) + 1);
  wire.child_expression_ids = expression.child_expression_ids;
  wire.result_descriptor_id = expression.result_descriptor_id;
  wire.function_uuid = expression.bound_function_uuid;
  wire.bound_name_uuid = expression.bound_name_uuid;
  if (expression.literal_kind)
    wire.literal_kind = static_cast<engine::internal_api::RelationalLiteralKind>(
        static_cast<std::uint8_t>(*expression.literal_kind) + 1);
  wire.operator_name = expression.canonical_operator_name;
  wire.literal_or_parameter_ref = expression.literal_or_parameter_ref;
  SblrOperand operand;
  operand.type = "relational_expression_v2";
  operand.name = "slot_" + std::to_string(expression.expression_id);
  operand.canonical_value_kind = static_cast<std::uint16_t>(engine::sblr::SblrValueKind::relational_expression);
  if (!engine::sblr::EncodeRelationalExpressionV1(wire, &operand.canonical_value_body)) return std::nullopt;
  return operand;
}

inline bool DecodeRelationalExpressionOperand(
    const SblrOperand& operand, engine::internal_api::RelationalExpressionRecord* output) {
  engine::internal_api::RelationalExpressionRecord decoded;
  if (output == nullptr || operand.type != "relational_expression_v2" || !operand.value.empty() ||
      operand.canonical_value_kind != static_cast<std::uint16_t>(engine::sblr::SblrValueKind::relational_expression) ||
      !engine::sblr::DecodeRelationalExpressionV1(operand.canonical_value_body.data(),
                                               operand.canonical_value_body.size(), &decoded) ||
      operand.name != "slot_" + std::to_string(decoded.expression_id)) return false;
  *output = std::move(decoded);
  return true;
}

inline std::optional<SblrOperand> MakeRelationalContextIdentityOperand(
    std::string_view name, const core::platform::Uuid& identity) {
  if (!engine::sblr::IsRelationalContextIdentitySlot(name) ||
      !core::uuid::IsEngineIdentityUuid(identity)) return std::nullopt;
  SblrOperand operand;
  operand.type = "uuid";
  operand.name = name;
  operand.canonical_value_kind = static_cast<std::uint16_t>(engine::sblr::SblrValueKind::uuid_ref);
  operand.canonical_value_body.assign(identity.bytes.begin(), identity.bytes.end());
  return operand;
}

}  // namespace scratchbird::parser::sbsql
