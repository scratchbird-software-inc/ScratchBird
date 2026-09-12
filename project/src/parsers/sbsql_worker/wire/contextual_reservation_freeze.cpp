// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "wire/contextual_operand_freeze.hpp"

namespace scratchbird::parser::sbsql {
namespace {
using CanonicalBytes = std::vector<std::uint8_t>;
struct ReservationLengthOverflow {};
std::uint32_t ReservationCount(std::size_t size) {
  if (size > std::numeric_limits<std::uint32_t>::max()) throw ReservationLengthOverflow{};
  return static_cast<std::uint32_t>(size);
}
void CanonicalAppendU16(CanonicalBytes* out, std::uint16_t value) {
  for (unsigned shift = 0; shift < 16; shift += 8) out->push_back(static_cast<std::uint8_t>(value >> shift));
}
void CanonicalAppendU32(CanonicalBytes* out, std::uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8) out->push_back(static_cast<std::uint8_t>(value >> shift));
}
void CanonicalAppendU64(CanonicalBytes* out, std::uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8) out->push_back(static_cast<std::uint8_t>(value >> shift));
}
void CanonicalAppendText(CanonicalBytes* out, std::string_view value) {
  CanonicalAppendU32(out, ReservationCount(value.size()));
  out->insert(out->end(), value.begin(), value.end());
}
void CanonicalAppendUuid(CanonicalBytes* out, const core::platform::Uuid& value) {
  out->insert(out->end(), value.bytes.begin(), value.bytes.end());
}
void ContextualAppendOptionalUuidV3(CanonicalBytes* out, const std::optional<core::platform::Uuid>& value) {
  CanonicalAppendU16(out, value ? 1 : 0);
  if (value) CanonicalAppendUuid(out, *value);
}
void ContextualAppendUuidVectorV3(CanonicalBytes* out, const std::vector<core::platform::Uuid>& values) {
  CanonicalAppendU32(out, ReservationCount(values.size()));
  for (const auto& value : values) CanonicalAppendUuid(out, value);
}
void ContextualAppendOptionalTextV2(
    CanonicalBytes* out, const std::optional<std::string>& value) {
  CanonicalAppendU16(out, value.has_value() ? 1 : 0);
  if (value.has_value()) CanonicalAppendText(out, *value);
}

void ContextualAppendU32VectorV2(
    CanonicalBytes* out, const std::vector<std::uint32_t>& values) {
  CanonicalAppendU32(out, ReservationCount(values.size()));
  for (const auto value : values) CanonicalAppendU32(out, value);
}

void ContextualAppendStringVectorV2(
    CanonicalBytes* out, const std::vector<std::string>& values) {
  CanonicalAppendU32(out, ReservationCount(values.size()));
  for (const auto& value : values) CanonicalAppendText(out, value);
}

void ContextualAppendOptionalU32V2(
    CanonicalBytes* out, const std::optional<std::uint32_t>& value) {
  CanonicalAppendU16(out, value.has_value() ? 1 : 0);
  CanonicalAppendU32(out, value.value_or(0));
}

void ContextualAppendOptionalU64V2(
    CanonicalBytes* out, const std::optional<std::uint64_t>& value) {
  CanonicalAppendU16(out, value.has_value() ? 1 : 0);
  CanonicalAppendU64(out, value.value_or(0));
}

void ContextualAppendSourceRangeV2(CanonicalBytes* out,
                                   const SourceRange& range) {
  CanonicalAppendU64(out, range.offset);
  CanonicalAppendU64(out, range.length);
  CanonicalAppendU64(out, range.line);
  CanonicalAppendU64(out, range.column);
  CanonicalAppendU64(out, range.end_line);
  CanonicalAppendU64(out, range.end_column);
}

void ContextualAppendIdentifierV2(CanonicalBytes* out,
                                  const NativeIdentifierAstNode& identifier) {
  CanonicalAppendText(out, identifier.spelling);
  CanonicalAppendU16(out, identifier.quoted ? 1 : 0);
  ContextualAppendSourceRangeV2(out, identifier.range);
}

void ContextualAppendIdentifierVectorV2(
    CanonicalBytes* out,
    const std::vector<NativeIdentifierAstNode>& identifiers) {
  CanonicalAppendU32(out, ReservationCount(identifiers.size()));
  for (const auto& identifier : identifiers) {
    ContextualAppendIdentifierV2(out, identifier);
  }
}

void ContextualAppendIdentifierMatrixV2(
    CanonicalBytes* out,
    const std::vector<std::vector<NativeIdentifierAstNode>>& identifiers) {
  CanonicalAppendU32(out, ReservationCount(identifiers.size()));
  for (const auto& row : identifiers) {
    ContextualAppendIdentifierVectorV2(out, row);
  }
}

void ContextualAppendU64VectorV2(
    CanonicalBytes* out, const std::vector<std::uint64_t>& values) {
  CanonicalAppendU32(out, ReservationCount(values.size()));
  for (const auto value : values) CanonicalAppendU64(out, value);
}

void ContextualAppendOptionalIdentifierV2(
    CanonicalBytes* out,
    const std::optional<NativeIdentifierAstNode>& identifier) {
  CanonicalAppendU16(out, identifier.has_value() ? 1 : 0);
  if (identifier.has_value()) ContextualAppendIdentifierV2(out, *identifier);
}

void ContextualAppendOrderingTermsV2(
    CanonicalBytes* out, const std::vector<BoundOrderingAstTerm>& terms) {
  CanonicalAppendU32(out, ReservationCount(terms.size()));
  for (const auto& term : terms) {
    CanonicalAppendU32(out, term.expression_id);
    CanonicalAppendU16(out, static_cast<std::uint16_t>(term.direction));
    CanonicalAppendU16(out, static_cast<std::uint16_t>(term.null_placement));
  }
}

bool ContextualAppendDescriptorRefsV2(
    CanonicalBytes* out, const std::vector<core::platform::Uuid>& descriptor_refs,
    const std::vector<BoundDescriptorAstRecord>& descriptors,
    const std::unordered_set<std::uint32_t>& contextual_descriptor_handles) {
  if (out == nullptr || descriptor_refs.size() != descriptors.size()) {
    return false;
  }
  std::unordered_set<std::uint32_t> seen_descriptor_handles;
  std::size_t contextual_descriptor_count = 0;
  CanonicalAppendU32(out, ReservationCount(descriptor_refs.size()));
  for (std::size_t ordinal = 0; ordinal < descriptors.size(); ++ordinal) {
    const auto& descriptor = descriptors[ordinal];
    if (descriptor.descriptor_id == 0 ||
        !seen_descriptor_handles.insert(descriptor.descriptor_id).second ||
        descriptor_refs[ordinal] != descriptor.descriptor_uuid) {
      return false;
    }
    CanonicalAppendU32(out, descriptor.descriptor_id);
    const bool contextual =
        contextual_descriptor_handles.contains(descriptor.descriptor_id);
    CanonicalAppendU16(out, contextual ? 1 : 0);
    if (contextual) {
      ++contextual_descriptor_count;
      CanonicalAppendText(out, "contextual_descriptor_ref_patch_v2");
    } else {
      CanonicalAppendUuid(out, descriptor_refs[ordinal]);
    }
  }
  return contextual_descriptor_count == contextual_descriptor_handles.size();
}

}  // namespace

std::optional<CanonicalBytes> FreezeContextualReservationSkeletonV2(
    const BoundStatement& bound, const SblrEnvelope& lowered,
    const std::unordered_set<std::uint32_t>& contextual_descriptor_handles) try {
  CanonicalBytes frozen;
  CanonicalAppendU16(&frozen, bound.bound ? 1 : 0);
  CanonicalAppendU16(&frozen, bound.native_relational_recognized ? 1 : 0);
  CanonicalAppendU32(&frozen, bound.bound_ast_format_version);
  CanonicalAppendU32(&frozen, bound.parser_api_major);
  CanonicalAppendU32(&frozen, bound.protocol_version);
  CanonicalAppendU64(&frozen, bound.catalog_epoch);
  CanonicalAppendU64(&frozen, bound.security_policy_epoch);
  CanonicalAppendU64(&frozen, bound.descriptor_epoch);
  CanonicalAppendUuid(&frozen, bound.parser_package_uuid);
  CanonicalAppendText(&frozen, bound.parser_package_version);
  CanonicalAppendText(&frozen, bound.parser_build_id);
  CanonicalAppendUuid(&frozen, bound.command_registry_snapshot_uuid);
  CanonicalAppendUuid(&frozen, bound.session_uuid);
  CanonicalAppendUuid(&frozen, bound.connection_uuid);
  CanonicalAppendUuid(&frozen, bound.database_uuid);
  CanonicalAppendUuid(&frozen, bound.dialect_profile_uuid);
  CanonicalAppendText(&frozen, bound.registry_family);
  CanonicalAppendText(&frozen, bound.operation_family);
  CanonicalAppendText(&frozen, bound.command_family);
  CanonicalAppendText(&frozen, bound.surface_key);
  CanonicalAppendText(&frozen, bound.sblr_operation_key);
  CanonicalAppendText(&frozen, bound.statement_surface_id);
  CanonicalAppendText(&frozen, bound.statement_surface_name);
  CanonicalAppendText(&frozen, bound.statement_parser_category);
  CanonicalAppendText(&frozen, bound.parser_handler_key);
  CanonicalAppendText(&frozen, bound.binding_contract_key);
  CanonicalAppendText(&frozen, bound.admission_contract_key);
  CanonicalAppendText(&frozen, bound.behavior_descriptor_key);
  CanonicalAppendText(&frozen, bound.diagnostic_key);
  CanonicalAppendText(&frozen, bound.name_resolution_authority_key);
  CanonicalAppendText(&frozen, bound.descriptor_authority_key);
  CanonicalAppendText(&frozen, bound.security_authority_key);
  CanonicalAppendText(&frozen, bound.transaction_authority_key);
  CanonicalAppendText(&frozen, bound.transaction_context);
  CanonicalAppendText(&frozen, bound.result_shape_key);
  CanonicalAppendText(&frozen, bound.diagnostic_shape_key);
  CanonicalAppendText(&frozen, bound.resource_contract_key);
  CanonicalAppendText(&frozen, bound.conformance_case_key);
  CanonicalAppendText(&frozen, bound.trace_key);
  CanonicalAppendText(&frozen, bound.edition_gate_result);
  CanonicalAppendText(&frozen, bound.profile_gate_result);
  CanonicalAppendText(&frozen, bound.granted_scope);
  CanonicalAppendU64(&frozen, bound.statement_hash);
  CanonicalAppendU16(&frozen, bound.requires_name_resolution ? 1 : 0);
  CanonicalAppendU16(&frozen, bound.requires_descriptor_authority ? 1 : 0);
  CanonicalAppendU16(&frozen, bound.requires_security_authority ? 1 : 0);
  CanonicalAppendU16(&frozen, bound.requires_transaction_authority ? 1 : 0);
  CanonicalAppendU16(&frozen, bound.requires_cluster_profile ? 1 : 0);
  CanonicalAppendU16(&frozen, bound.exact_refusal_required ? 1 : 0);
  ContextualAppendUuidVectorV3(&frozen, bound.resolved_object_uuids);
  if (!ContextualAppendDescriptorRefsV2(
          &frozen, bound.descriptor_refs, bound.native_relational.descriptors,
          contextual_descriptor_handles)) {
    return std::nullopt;
  }
  ContextualAppendStringVectorV2(&frozen, bound.policy_refs);
  ContextualAppendStringVectorV2(&frozen, bound.required_rights);
  ContextualAppendStringVectorV2(&frozen, bound.required_authority_steps);
  CanonicalAppendU16(&frozen, bound.native_relational.bound ? 1 : 0);
  CanonicalAppendUuid(&frozen, bound.native_relational.bound_ast_uuid);
  CanonicalAppendU16(&frozen, bound.native_relational.scopes.empty() ? 0 : 1);
  if (!bound.native_relational.scopes.empty())
    CanonicalAppendUuid(&frozen, bound.native_relational.scopes.front().catalog_epoch_uuid);
  CanonicalAppendUuid(&frozen, bound.native_relational.security_context_uuid);
  CanonicalAppendUuid(&frozen, bound.native_relational.statement_uuid);
  CanonicalAppendText(&frozen, bound.native_relational.statement_timestamp);
  CanonicalAppendUuid(&frozen, bound.native_relational.owning_transaction_uuid);
  CanonicalAppendUuid(&frozen, bound.native_relational.statement_snapshot_uuid);
  CanonicalAppendUuid(&frozen, bound.native_relational.statement_metadata_snapshot_uuid);
  CanonicalAppendU64(&frozen, bound.native_relational.local_transaction_id);
  CanonicalAppendU64(
      &frozen,
      bound.native_relational.snapshot_visible_through_local_transaction_id);
  CanonicalAppendU32(&frozen, bound.native_relational.root_relation_id);
  CanonicalAppendU32(&frozen, bound.native_relational.root_scope_id);

  CanonicalAppendU32(
      &frozen,
      ReservationCount(bound.native_relational.descriptors.size()));
  for (const auto& descriptor : bound.native_relational.descriptors) {
    CanonicalAppendU32(&frozen, descriptor.descriptor_id);
    const bool contextual =
        contextual_descriptor_handles.contains(descriptor.descriptor_id);
    CanonicalAppendU16(&frozen, contextual ? 1 : 0);
    if (contextual) continue;
    CanonicalAppendUuid(&frozen, descriptor.descriptor_uuid);
    CanonicalAppendUuid(&frozen, descriptor.type_uuid);
    CanonicalAppendU16(
        &frozen, static_cast<std::uint16_t>(descriptor.nullability));
    ContextualAppendOptionalUuidV3(&frozen, descriptor.collation_uuid);
    ContextualAppendOptionalTextV2(&frozen,
                                   descriptor.timezone_profile_id);
    CanonicalAppendU16(
        &frozen, descriptor.width_precision_scale.width.has_value() ? 1 : 0);
    CanonicalAppendU32(
        &frozen, descriptor.width_precision_scale.width.value_or(0));
    CanonicalAppendU16(
        &frozen,
        descriptor.width_precision_scale.precision.has_value() ? 1 : 0);
    CanonicalAppendU32(
        &frozen, descriptor.width_precision_scale.precision.value_or(0));
    CanonicalAppendU16(
        &frozen, descriptor.width_precision_scale.scale.has_value() ? 1 : 0);
    CanonicalAppendU32(
        &frozen, descriptor.width_precision_scale.scale.value_or(0));
    CanonicalAppendText(&frozen, descriptor.canonical_type_name);
    CanonicalAppendText(&frozen, descriptor.element_profile);
    CanonicalAppendU64(&frozen, descriptor.descriptor_generation);
    CanonicalAppendU64(&frozen, descriptor.type_generation);
    CanonicalAppendText(&frozen, descriptor.codec_id);
    CanonicalAppendU16(&frozen, descriptor.codec_version);
    CanonicalAppendU64(&frozen, descriptor.codec_generation);
    CanonicalAppendUuid(&frozen, descriptor.statement_receipt_uuid);
    CanonicalAppendUuid(&frozen, descriptor.datatype_catalog_snapshot_uuid);
    CanonicalAppendU64(&frozen, descriptor.datatype_catalog_generation);
    CanonicalAppendU64(&frozen, descriptor.datatype_registry_generation);
  }

  CanonicalAppendU32(
      &frozen,
      ReservationCount(bound.native_relational.expressions.size()));
  for (const auto& expression : bound.native_relational.expressions) {
    CanonicalAppendU32(&frozen, expression.expression_id);
    CanonicalAppendU16(
        &frozen, static_cast<std::uint16_t>(expression.expression_kind));
    CanonicalAppendU16(
        &frozen, expression.literal_kind.has_value()
                     ? static_cast<std::uint16_t>(*expression.literal_kind) + 1
                     : 0);
    ContextualAppendU32VectorV2(&frozen, expression.child_expression_ids);
    CanonicalAppendU32(&frozen, expression.result_descriptor_id);
    ContextualAppendOptionalUuidV3(&frozen, expression.bound_function_uuid);
    ContextualAppendOptionalUuidV3(&frozen, expression.bound_name_uuid);
    ContextualAppendOptionalTextV2(&frozen,
                                   expression.canonical_operator_name);
    ContextualAppendOptionalTextV2(&frozen,
                                   expression.literal_or_parameter_ref);
    CanonicalAppendU64(&frozen,
                       expression.structural_literal_occurrence_id);
    CanonicalAppendU64(&frozen,
                       expression.structural_parameter_occurrence_id);
    CanonicalAppendU64(&frozen,
                       expression.structural_variable_occurrence_id);
  }

  CanonicalAppendU32(
      &frozen,
      ReservationCount(bound.native_relational.values_rows.size()));
  for (const auto& row : bound.native_relational.values_rows) {
    CanonicalAppendU32(&frozen, row.row_id);
    ContextualAppendU32VectorV2(&frozen, row.expression_ids);
  }

  CanonicalAppendU32(
      &frozen,
      ReservationCount(bound.native_relational.grouping_sets.size()));
  for (const auto& grouping : bound.native_relational.grouping_sets) {
    CanonicalAppendU32(&frozen, grouping.relation_id);
    CanonicalAppendU32(&frozen, grouping.ordinal);
    ContextualAppendU32VectorV2(&frozen, grouping.expression_ids);
  }

  CanonicalAppendU32(
      &frozen, ReservationCount(bound.native_relational.window_definitions.size()));
  for (const auto& window : bound.native_relational.window_definitions) {
    CanonicalAppendU32(&frozen, window.window_id);
    ContextualAppendOptionalTextV2(&frozen, window.canonical_name_key);
    ContextualAppendOptionalU32V2(&frozen, window.inherited_window_id);
    ContextualAppendU32VectorV2(&frozen, window.partition_expression_ids);
    ContextualAppendOrderingTermsV2(&frozen, window.ordering_terms);
    CanonicalAppendU16(&frozen, window.frame_unit.has_value() ? 1 : 0);
    CanonicalAppendU16(
        &frozen, window.frame_unit.has_value()
                     ? static_cast<std::uint16_t>(*window.frame_unit)
                     : 0);
    const auto append_frame_bound = [&](const auto& frame) {
      CanonicalAppendU16(&frozen, frame.has_value() ? 1 : 0);
      if (frame.has_value()) {
        CanonicalAppendU16(
            &frozen, static_cast<std::uint16_t>(frame->bound_kind));
        ContextualAppendOptionalU32V2(&frozen, frame->offset_expression_id);
      }
    };
    append_frame_bound(window.frame_start);
    append_frame_bound(window.frame_end);
    CanonicalAppendU16(&frozen,
                       static_cast<std::uint16_t>(window.exclusion));
  }

  CanonicalAppendU32(
      &frozen, ReservationCount(bound.native_relational.window_invocations.size()));
  for (const auto& invocation : bound.native_relational.window_invocations) {
    CanonicalAppendU32(&frozen, invocation.invocation_id);
    CanonicalAppendU32(&frozen, invocation.function_expression_id);
    CanonicalAppendU32(&frozen, invocation.window_definition_id);
    ContextualAppendOptionalTextV2(&frozen, invocation.output_name_utf8);
    CanonicalAppendU16(&frozen, invocation.function_abi_version);
    CanonicalAppendText(&frozen, invocation.builtin_id);
    CanonicalAppendUuid(&frozen, invocation.bound_function_uuid);
    CanonicalAppendU32(&frozen, invocation.result_descriptor_id);
    ContextualAppendU32VectorV2(&frozen,
                                invocation.argument_expression_ids);
  }

  CanonicalAppendU32(
      &frozen,
      ReservationCount(bound.native_relational.row_patterns.size()));
  for (const auto& pattern : bound.native_relational.row_patterns) {
    CanonicalAppendU32(&frozen, pattern.pattern_id);
    CanonicalAppendU32(&frozen, pattern.relation_id);
    ContextualAppendU32VectorV2(&frozen,
                                pattern.partition_expression_ids);
    ContextualAppendOrderingTermsV2(&frozen, pattern.ordering_terms);
    CanonicalAppendU32(&frozen,
                       ReservationCount(pattern.variables.size()));
    for (const auto& variable : pattern.variables) {
      CanonicalAppendText(&frozen, variable.canonical_name_key);
      CanonicalAppendU32(&frozen, variable.minimum_occurrences);
      ContextualAppendOptionalU32V2(&frozen, variable.maximum_occurrences);
      CanonicalAppendU16(&frozen, variable.reluctant ? 1 : 0);
      ContextualAppendOptionalU32V2(&frozen,
                                    variable.define_expression_id);
      CanonicalAppendU16(&frozen, variable.define_always_true ? 1 : 0);
    }
    ContextualAppendU32VectorV2(&frozen, pattern.measure_expression_ids);
    CanonicalAppendU16(&frozen,
                       static_cast<std::uint16_t>(pattern.rows_per_match));
    CanonicalAppendU16(&frozen,
                       static_cast<std::uint16_t>(pattern.after_match_skip));
    ContextualAppendOptionalTextV2(&frozen, pattern.skip_target_key);
    CanonicalAppendU32(&frozen, pattern.maximum_partition_rows);
    CanonicalAppendU32(&frozen, pattern.maximum_active_states);
    CanonicalAppendU32(&frozen, pattern.maximum_output_rows);
    CanonicalAppendU16(
        &frozen, pattern.stable_row_identity_tie_break_allowed ? 1 : 0);
  }

  CanonicalAppendU32(
      &frozen,
      ReservationCount(bound.native_relational.outputs.size()));
  for (const auto& output : bound.native_relational.outputs) {
    CanonicalAppendU32(&frozen, output.output_id);
    CanonicalAppendU32(&frozen, output.relation_id);
    CanonicalAppendU32(&frozen, output.expression_id);
    CanonicalAppendText(&frozen, output.output_name_utf8);
    CanonicalAppendU32(&frozen, output.descriptor_id);
    CanonicalAppendU16(&frozen, output.visible ? 1 : 0);
    CanonicalAppendU32(&frozen, output.ordinal);
  }

  CanonicalAppendU32(
      &frozen,
      ReservationCount(bound.native_relational.relations.size()));
  for (const auto& relation : bound.native_relational.relations) {
    CanonicalAppendU32(&frozen, relation.relation_id);
    CanonicalAppendU16(
        &frozen, static_cast<std::uint16_t>(relation.relation_kind));
    CanonicalAppendU16(
        &frozen,
        static_cast<std::uint16_t>(relation.aggregate_grouping_form));
    CanonicalAppendU16(
        &frozen,
        static_cast<std::uint16_t>(relation.aggregate_projection_form));
    ContextualAppendU32VectorV2(&frozen, relation.input_relation_ids);
    ContextualAppendU32VectorV2(&frozen, relation.values_row_ids);
    ContextualAppendU32VectorV2(&frozen, relation.output_expression_ids);
    ContextualAppendU32VectorV2(
        &frozen, relation.grouping_key_expression_ids);
    ContextualAppendU32VectorV2(
        &frozen, relation.aggregate_expression_ids);
    ContextualAppendU32VectorV2(&frozen, relation.predicate_expression_ids);
    ContextualAppendU32VectorV2(&frozen, relation.limit_expression_ids);
    ContextualAppendU32VectorV2(
        &frozen, relation.table_function_argument_expression_ids);
    ContextualAppendU32VectorV2(&frozen, relation.window_invocation_ids);
    ContextualAppendOrderingTermsV2(&frozen, relation.ordering_terms);
    ContextualAppendU32VectorV2(&frozen, relation.bound_expression_ids);
    CanonicalAppendText(&frozen, relation.semantic_variant_id);
    ContextualAppendOptionalUuidV3(&frozen, relation.bound_object_uuid);
    CanonicalAppendU16(&frozen, relation.lateral ? 1 : 0);
  }

  CanonicalAppendU32(
      &frozen, ReservationCount(bound.native_relational.catalog_relation_sources.size()));
  for (const auto& source :
       bound.native_relational.catalog_relation_sources) {
    CanonicalAppendU32(&frozen, source.source_id);
    CanonicalAppendU16(&frozen, static_cast<std::uint16_t>(source.source_kind));
    CanonicalAppendU16(
        &frozen, static_cast<std::uint16_t>(source.resolution_state));
    ContextualAppendIdentifierVectorV2(&frozen, source.qualified_name);
    ContextualAppendOptionalIdentifierV2(&frozen, source.alias);
    CanonicalAppendU16(&frozen, source.alias_is_explicit ? 1 : 0);
    CanonicalAppendText(&frozen, source.model_family_id);
    CanonicalAppendText(&frozen, source.model_operation_id);
    ContextualAppendStringVectorV2(&frozen, source.model_operation_ids);
    ContextualAppendU32VectorV2(
        &frozen, source.model_operation_expression_ids);
    ContextualAppendOptionalIdentifierV2(&frozen, source.model_source_alias);
    ContextualAppendOptionalU32V2(
        &frozen, source.model_document_expression_id);
    ContextualAppendOptionalU32V2(&frozen, source.model_path_expression_id);
    ContextualAppendOptionalU32V2(&frozen, source.model_value_expression_id);
    ContextualAppendOptionalU32V2(&frozen, source.model_pattern_expression_id);
    ContextualAppendOptionalU32V2(
        &frozen, source.model_graph_alias_expression_id);
    ContextualAppendU32VectorV2(&frozen, source.model_key_expression_ids);
    ContextualAppendOptionalU32V2(
        &frozen, source.model_time_series_alias_expression_id);
    ContextualAppendOptionalU32V2(&frozen, source.model_range_expression_id);
    ContextualAppendOptionalU32V2(
        &frozen, source.model_range_start_expression_id);
    ContextualAppendOptionalU32V2(
        &frozen, source.model_range_end_expression_id);
    ContextualAppendOptionalU32V2(&frozen,
                                  source.model_interval_expression_id);
    ContextualAppendOptionalU32V2(&frozen,
                                  source.model_time_input_expression_id);
    ContextualAppendOptionalU32V2(&frozen, source.model_bucket_expression_id);
    ContextualAppendOptionalU32V2(
        &frozen, source.model_bucket_interval_expression_id);
    ContextualAppendOptionalU32V2(
        &frozen, source.model_bucket_time_input_expression_id);
    ContextualAppendOptionalU32V2(
        &frozen, source.model_downsample_expression_id);
    CanonicalAppendText(&frozen, source.model_time_series_aggregate_id);
    ContextualAppendOptionalU32V2(
        &frozen, source.model_vector_alias_expression_id);
    ContextualAppendOptionalU32V2(
        &frozen, source.model_vector_nearest_expression_id);
    ContextualAppendOptionalU32V2(
        &frozen, source.model_vector_query_expression_id);
    ContextualAppendOptionalU32V2(
        &frozen, source.model_vector_metric_expression_id);
    ContextualAppendOptionalU32V2(
        &frozen, source.model_vector_top_k_expression_id);
    ContextualAppendOptionalU32V2(
        &frozen, source.model_vector_filter_expression_id);
    ContextualAppendOptionalU32V2(
        &frozen, source.model_vector_metadata_predicate_expression_id);
    ContextualAppendOptionalU32V2(
        &frozen, source.model_vector_metadata_column_expression_id);
    ContextualAppendOptionalU32V2(
        &frozen, source.model_vector_metadata_value_expression_id);
    ContextualAppendOptionalIdentifierV2(
        &frozen, source.model_vector_result_alias);
    CanonicalAppendText(&frozen, source.model_vector_metric_id);
    ContextualAppendOptionalU64V2(&frozen, source.model_vector_top_k);
    ContextualAppendOptionalU32V2(
        &frozen, source.model_search_alias_expression_id);
    ContextualAppendOptionalU32V2(
        &frozen, source.model_search_match_expression_id);
    ContextualAppendOptionalU32V2(
        &frozen, source.model_search_query_expression_id);
    ContextualAppendOptionalU32V2(
        &frozen, source.model_search_text_expression_id);
    ContextualAppendOptionalU32V2(
        &frozen, source.model_search_edit_expression_id);
    ContextualAppendOptionalU32V2(
        &frozen, source.model_search_analyzer_expression_id);
    ContextualAppendOptionalU32V2(
        &frozen, source.model_search_top_k_expression_id);
    ContextualAppendOptionalU32V2(
        &frozen, source.model_search_filter_expression_id);
    ContextualAppendOptionalU32V2(
        &frozen, source.model_search_category_predicate_expression_id);
    ContextualAppendOptionalU32V2(
        &frozen, source.model_search_category_column_expression_id);
    ContextualAppendOptionalU32V2(
        &frozen, source.model_search_category_value_expression_id);
    ContextualAppendOptionalIdentifierV2(
        &frozen, source.model_search_result_alias);
    ContextualAppendIdentifierVectorV2(
        &frozen, source.model_search_analyzer_name);
    CanonicalAppendText(&frozen, source.model_search_query_kind);
    ContextualAppendOptionalU64V2(&frozen, source.model_search_top_k);
    CanonicalAppendUuid(&frozen, source.model_search_analyzer_uuid);
    CanonicalAppendU64(&frozen, source.model_search_analyzer_generation);
    ContextualAppendOptionalU32V2(
        &frozen, source.model_spatial_alias_expression_id);
    ContextualAppendOptionalU32V2(
        &frozen, source.model_spatial_operation_expression_id);
    ContextualAppendOptionalU32V2(
        &frozen, source.model_spatial_match_expression_id);
    ContextualAppendOptionalU32V2(
        &frozen, source.model_spatial_nearest_expression_id);
    ContextualAppendOptionalU32V2(
        &frozen, source.model_spatial_query_expression_id);
    ContextualAppendU32VectorV2(
        &frozen, source.model_spatial_query_expression_ids);
    ContextualAppendOptionalU32V2(
        &frozen, source.model_spatial_predicate_expression_id);
    ContextualAppendOptionalU32V2(
        &frozen, source.model_spatial_crs_expression_id);
    ContextualAppendU32VectorV2(
        &frozen, source.model_spatial_crs_expression_ids);
    ContextualAppendIdentifierVectorV2(&frozen,
                                       source.model_spatial_crs_name);
    ContextualAppendIdentifierMatrixV2(&frozen,
                                       source.model_spatial_crs_names);
    CanonicalAppendText(&frozen, source.model_spatial_predicate_id);
    ContextualAppendOptionalU32V2(
        &frozen, source.model_spatial_top_k_expression_id);
    ContextualAppendOptionalU64V2(&frozen, source.model_spatial_top_k);
    ContextualAppendUuidVectorV3(&frozen, source.model_spatial_crs_uuids);
    ContextualAppendU64VectorV2(&frozen,
                                source.model_spatial_crs_generations);
    ContextualAppendOptionalU32V2(
        &frozen, source.model_columnar_alias_expression_id);
    ContextualAppendOptionalU32V2(
        &frozen, source.model_columnar_operation_expression_id);
    ContextualAppendOptionalU32V2(
        &frozen, source.model_columnar_project_expression_id);
    ContextualAppendOptionalU32V2(
        &frozen, source.model_columnar_filter_expression_id);
    ContextualAppendOptionalU32V2(
        &frozen, source.model_columnar_predicate_expression_id);
    ContextualAppendU32VectorV2(
        &frozen, source.model_columnar_project_expression_ids);
    ContextualAppendUuidVectorV3(&frozen, source.model_columnar_project_column_uuids);
    CanonicalAppendText(&frozen, source.model_graph_direction);
    ContextualAppendOptionalU64V2(&frozen,
                                  source.model_graph_minimum_depth);
    ContextualAppendOptionalU64V2(&frozen,
                                  source.model_graph_maximum_depth);
    CanonicalAppendText(&frozen, source.model_graph_cycle_policy);
    CanonicalAppendText(&frozen, source.model_comparison_operator);
    CanonicalAppendU16(&frozen, source.model_wildcard_path ? 1 : 0);
    ContextualAppendSourceRangeV2(&frozen, source.qualified_name_range);
    ContextualAppendSourceRangeV2(&frozen, source.range);
    CanonicalAppendUuid(&frozen, source.object_uuid);
    CanonicalAppendText(&frozen, source.resolved_object_type);
    CanonicalAppendUuid(&frozen, source.resolved_schema_uuid);
    ContextualAppendOptionalUuidV3(&frozen, source.parent_object_uuid);
    CanonicalAppendU64(&frozen, source.catalog_generation_id);
    CanonicalAppendU64(&frozen, source.security_epoch);
    CanonicalAppendU64(&frozen, source.resource_epoch);
    CanonicalAppendU32(&frozen,
                       ReservationCount(source.columns.size()));
    for (const auto& column : source.columns) {
      CanonicalAppendU32(&frozen, column.ordinal);
      CanonicalAppendUuid(&frozen, column.column_uuid);
      CanonicalAppendU32(&frozen, column.descriptor_id);
      CanonicalAppendText(&frozen, column.canonical_name_key);
    }
  }

  CanonicalAppendU32(
      &frozen,
      ReservationCount(bound.native_relational.scopes.size()));
  for (const auto& scope : bound.native_relational.scopes) {
    CanonicalAppendU32(&frozen, scope.scope_id);
    ContextualAppendOptionalU32V2(&frozen, scope.parent_scope_id);
    ContextualAppendU32VectorV2(&frozen, scope.visible_relation_ids);
    ContextualAppendU32VectorV2(&frozen, scope.visible_projection_ids);
    CanonicalAppendUuid(&frozen, scope.catalog_epoch_uuid);
  }

  CanonicalAppendText(&frozen, lowered.operation_family);
  CanonicalAppendU32(&frozen, lowered.envelope_version);
  CanonicalAppendU64(&frozen, lowered.statement_hash);
  CanonicalAppendText(&frozen, lowered.surface_key);
  CanonicalAppendText(&frozen, lowered.command_family);
  CanonicalAppendText(&frozen, lowered.operation_id);
  CanonicalAppendText(&frozen, lowered.sblr_operation_key);
  CanonicalAppendText(&frozen, lowered.sblr_opcode);
  CanonicalAppendText(&frozen, lowered.engine_api_operation_id);
  CanonicalAppendText(&frozen, lowered.engine_api_function);
  CanonicalAppendText(&frozen, lowered.lifecycle_mapping_key);
  CanonicalAppendText(&frozen, lowered.result_shape_key);
  CanonicalAppendText(&frozen, lowered.diagnostic_shape_key);
  CanonicalAppendText(&frozen, lowered.resource_contract_key);
  CanonicalAppendText(&frozen, lowered.trace_key);
  CanonicalAppendText(&frozen, lowered.source_artifact_policy);
  CanonicalAppendU64(&frozen, lowered.catalog_epoch);
  CanonicalAppendU64(&frozen, lowered.security_policy_epoch);
  CanonicalAppendU64(&frozen, lowered.descriptor_epoch);
  ContextualAppendUuidVectorV3(&frozen, lowered.resolved_object_uuids);
  if (!ContextualAppendDescriptorRefsV2(
          &frozen, lowered.descriptor_refs,
          bound.native_relational.descriptors,
          contextual_descriptor_handles)) {
    return std::nullopt;
  }
  ContextualAppendStringVectorV2(&frozen, lowered.descriptor_requirements);
  ContextualAppendStringVectorV2(&frozen, lowered.policy_refs);
  ContextualAppendStringVectorV2(&frozen, lowered.required_rights);
  ContextualAppendStringVectorV2(&frozen,
                                 lowered.required_authority_steps);
  CanonicalAppendU16(&frozen, lowered.lifecycle_mapping ? 1 : 0);
  CanonicalAppendU16(&frozen, lowered.exact_emulated_diagnostic ? 1 : 0);
  CanonicalAppendU16(&frozen, lowered.real_file_effects ? 1 : 0);
  CanonicalAppendU16(&frozen, lowered.parser_executes_sql ? 1 : 0);
  const auto frozen_operands = FreezeContextualOperandsV3(
      lowered.operands, contextual_descriptor_handles);
  if (!frozen_operands) return std::nullopt;
  frozen.insert(frozen.end(), frozen_operands->begin(), frozen_operands->end());
  return frozen;
} catch (const ReservationLengthOverflow&) {
  return std::nullopt;
}

}  // namespace scratchbird::parser::sbsql

