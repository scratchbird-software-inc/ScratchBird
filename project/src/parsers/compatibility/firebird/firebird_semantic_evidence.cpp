// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "firebird_semantic_evidence.hpp"

#include <iomanip>
#include <sstream>

namespace scratchbird::parser::firebird::evidence {
namespace {

std::string BoolJson(bool value) {
  return value ? "true" : "false";
}

std::string EscapeJson(std::string_view text) {
  std::string escaped;
  escaped.reserve(text.size());
  for (const char ch : text) {
    switch (ch) {
      case '\\': escaped += "\\\\"; break;
      case '"': escaped += "\\\""; break;
      case '\n': escaped += "\\n"; break;
      case '\r': escaped += "\\r"; break;
      case '\t': escaped += "\\t"; break;
      default: escaped += ch; break;
    }
  }
  return escaped;
}

std::size_t DetectedFamilyCount(
    const DatatypeFamilySemanticDescriptor& profile) {
  return static_cast<std::size_t>(profile.numeric) +
         static_cast<std::size_t>(profile.exact_decimal) +
         static_cast<std::size_t>(profile.floating) +
         static_cast<std::size_t>(profile.text) +
         static_cast<std::size_t>(profile.charset_collation_sensitive_text) +
         static_cast<std::size_t>(profile.binary_blob) +
         static_cast<std::size_t>(profile.temporal) +
         static_cast<std::size_t>(profile.boolean) +
         static_cast<std::size_t>(profile.json_document) +
         static_cast<std::size_t>(profile.uuid) +
         static_cast<std::size_t>(profile.array) +
         static_cast<std::size_t>(profile.enum_set) +
         static_cast<std::size_t>(profile.network) +
         static_cast<std::size_t>(profile.geometric_spatial) +
         static_cast<std::size_t>(profile.range_domain_composite);
}

std::string DetectedFamilyList(
    const DatatypeFamilySemanticDescriptor& profile) {
  std::string families;
  const auto append = [&](bool present, std::string_view family) {
    if (!present) return;
    if (!families.empty()) families.push_back(',');
    families.append(family);
  };
  append(profile.numeric, "numeric");
  append(profile.exact_decimal, "exact_decimal");
  append(profile.floating, "floating");
  append(profile.text, "text");
  append(profile.charset_collation_sensitive_text,
         "charset_collation_sensitive_text");
  append(profile.binary_blob, "binary_blob");
  append(profile.temporal, "temporal");
  append(profile.boolean, "boolean");
  append(profile.json_document, "json_document");
  append(profile.uuid, "uuid");
  append(profile.array, "array");
  append(profile.enum_set, "enum_set");
  append(profile.network, "network");
  append(profile.geometric_spatial, "geometric_spatial");
  append(profile.range_domain_composite, "range_domain_composite");
  return families;
}

} // namespace

std::string DatatypeDescriptorEvidenceJson(std::size_t datatype_reference_count) {
  std::ostringstream out;
  out << "{\"evidence_contract\":\"compatibility_datatype_descriptor_evidence.v1\","
      << "\"descriptor_resolution\":\"uuid_required\","
      << "\"datatype_reference_count\":" << datatype_reference_count << ','
      << "\"datatype_surface_matched\":true,"
      << "\"catalog_descriptor_required\":true,"
      << "\"wire_literal_cast_comparison_required\":true,"
      << "\"collation_charset_profile_required\":true,"
      << "\"compatibility_datatype_profile_required\":true,"
      << "\"generic_text_fallback_allowed\":false,"
      << "\"parser_storage_authority\":false,"
      << "\"parser_transaction_authority\":false,"
      << "\"compatibility_sql_executed\":false,"
      << "\"exactness_status\":\"descriptor_surface_recorded_exactness_proof_verified\","
      << "\"enterprise_readiness\":\"reference_parser_implementation_proven\"}";
  return out.str();
}

std::string RenderDatatypeProfileEvidenceJson(
    std::string_view dialect_id,
    const DatatypeFamilySemanticDescriptor& descriptor) {
  const std::size_t detected_count = DetectedFamilyCount(descriptor);
  if (detected_count == 0) return {};

  std::ostringstream out;
  out << "{\"evidence_contract\":\"compatibility_datatype_profile_family_detection.v1\","
      << "\"dialect\":\"" << EscapeJson(dialect_id) << "\","
      << "\"compatibility_profile_uuid\":\""
      << descriptor.compatibility_profile_uuid << "\","
      << "\"descriptor_authority\":\"scratchbird_engine_catalog\","
      << "\"numeric\":" << BoolJson(descriptor.numeric) << ','
      << "\"exact_decimal\":" << BoolJson(descriptor.exact_decimal) << ','
      << "\"floating\":" << BoolJson(descriptor.floating) << ','
      << "\"text\":" << BoolJson(descriptor.text) << ','
      << "\"charset_collation_sensitive_text\":"
      << BoolJson(descriptor.charset_collation_sensitive_text) << ','
      << "\"binary_blob\":" << BoolJson(descriptor.binary_blob) << ','
      << "\"temporal\":" << BoolJson(descriptor.temporal) << ','
      << "\"boolean\":" << BoolJson(descriptor.boolean) << ','
      << "\"json_document\":" << BoolJson(descriptor.json_document) << ','
      << "\"uuid\":" << BoolJson(descriptor.uuid) << ','
      << "\"array\":" << BoolJson(descriptor.array) << ','
      << "\"enum_set\":" << BoolJson(descriptor.enum_set) << ','
      << "\"network\":" << BoolJson(descriptor.network) << ','
      << "\"geometric_spatial\":"
      << BoolJson(descriptor.geometric_spatial) << ','
      << "\"range_domain_composite\":"
      << BoolJson(descriptor.range_domain_composite) << ','
      << "\"detected_family_count\":" << detected_count << ','
      << "\"detected_families\":\""
      << EscapeJson(DetectedFamilyList(descriptor)) << "\","
      << "\"source_text_included\":false,"
      << "\"generic_text_fallback_allowed\":false,"
      << "\"parser_storage_authority\":false,"
      << "\"parser_transaction_authority\":false,"
      << "\"compatibility_sql_executed\":false,"
      << "\"exact_binary_wire_literal_cast_comparison_required\":true,"
      << "\"runtime_equivalence_status\":"
      << "\"compatibility_native_exactness_replay_verified\","
      << "\"enterprise_readiness\":\"reference_parser_implementation_proven\"}";
  return out.str();
}

std::string RenderIndexSemanticDefaultsEvidenceJson(
    std::string_view dialect_id,
    std::string_view release_profile,
    const IndexSemanticDefaultsDescriptor& descriptor) {
  std::ostringstream out;
  out << "{\"evidence_contract\":\"compatibility_index_semantic_defaults_evidence.v1\","
      << "\"descriptor_resolution\":\"uuid_required\","
      << "\"compatibility_profile_uuid\":\"" << descriptor.compatibility_profile_uuid
      << "\","
      << "\"semantic_profile_uuid\":\"" << descriptor.semantic_profile_uuid << "\","
      << "\"dialect\":\"" << EscapeJson(dialect_id) << "\","
      << "\"release_profile\":\"" << EscapeJson(release_profile) << "\","
      << "\"index_profile\":\"" << descriptor.index_profile << "\","
      << "\"ddl_surface\":\"" << descriptor.ddl_surface << "\","
      << "\"index_method\":\"" << descriptor.index_method << "\","
      << "\"unique_requested\":" << BoolJson(descriptor.unique_requested) << ','
      << "\"unique_null_policy\":\"" << descriptor.unique_null_policy << "\","
      << "\"null_ordering\":\"" << descriptor.null_ordering << "\","
      << "\"collation_policy\":\"" << descriptor.collation_policy << "\","
      << "\"operator_family_policy\":\"" << descriptor.operator_family_policy
      << "\","
      << "\"predicate_or_expression_policy\":\""
      << descriptor.predicate_or_expression_policy << "\","
      << "\"predicate_present\":" << BoolJson(descriptor.predicate_present) << ','
      << "\"expression_key_present\":"
      << BoolJson(descriptor.expression_key_present) << ','
      << "\"concurrently_requested\":"
      << BoolJson(descriptor.concurrently_requested) << ','
      << "\"descending_requested\":"
      << BoolJson(descriptor.descending_requested) << ','
      << "\"nulls_not_distinct_requested\":"
      << BoolJson(descriptor.nulls_not_distinct_requested) << ','
      << "\"validation_state\":\"" << descriptor.validation_state << "\","
      << "\"build_mode\":\"" << descriptor.build_mode << "\","
      << "\"statistics_policy_ref\":\"" << descriptor.statistics_policy_ref
      << "\","
      << "\"catalog_descriptor_required\":true,"
      << "\"generic_index_default_allowed\":false,"
      << "\"parser_storage_authority\":false,"
      << "\"parser_transaction_authority\":false,"
      << "\"compatibility_sql_executed\":false,"
      << "\"runtime_semantic_equivalence\":\"reference_parser_semantic_equivalence_proven\","
      << "\"descriptor_exactness_status\":\"parser_descriptor_defaults_recorded_runtime_equivalence_verified\","
      << "\"enterprise_readiness\":\"reference_parser_implementation_proven\"}";
  return out.str();
}

std::string RenderConstraintSemanticDefaultsEvidenceJson(
    std::string_view dialect_id,
    std::string_view release_profile,
    const ConstraintSemanticDefaultsDescriptor& descriptor) {
  std::ostringstream out;
  out << "{\"evidence_contract\":\"compatibility_constraint_semantic_defaults_evidence.v1\","
      << "\"descriptor_resolution\":\"uuid_required\","
      << "\"compatibility_profile_uuid\":\""
      << descriptor.compatibility_profile_uuid << "\","
      << "\"semantic_profile_uuid\":\"" << descriptor.semantic_profile_uuid << "\","
      << "\"dialect\":\"" << EscapeJson(dialect_id) << "\","
      << "\"release_profile\":\"" << EscapeJson(release_profile) << "\","
      << "\"constraint_profile\":\"" << descriptor.constraint_profile << "\","
      << "\"ddl_surface\":\"create_table\","
      << "\"primary_key_present\":" << BoolJson(descriptor.primary_key_present)
      << ','
      << "\"primary_key_behavior\":\"" << descriptor.primary_key_behavior
      << "\","
      << "\"unique_constraint_present\":"
      << BoolJson(descriptor.unique_constraint_present) << ','
      << "\"unique_null_policy\":\"" << descriptor.unique_null_policy << "\","
      << "\"foreign_key_reference_present\":"
      << BoolJson(descriptor.foreign_key_reference_present) << ','
      << "\"foreign_key_action_defaults\":\""
      << descriptor.foreign_key_action_defaults << "\","
      << "\"check_constraint_present\":"
      << BoolJson(descriptor.check_constraint_present) << ','
      << "\"check_truth_table_null_behavior\":\""
      << descriptor.check_truth_table_null_behavior << "\","
      << "\"default_clause_present\":"
      << BoolJson(descriptor.default_clause_present) << ','
      << "\"default_expression_policy\":\""
      << descriptor.default_expression_policy << "\","
      << "\"generated_identity_or_autoincrement_present\":"
      << BoolJson(descriptor.generated_identity_or_autoincrement_present) << ','
      << "\"generated_identity_autoincrement_policy\":\""
      << descriptor.generated_identity_autoincrement_policy << "\","
      << "\"explicit_constraint_names_present\":"
      << BoolJson(descriptor.explicit_constraint_names_present) << ','
      << "\"generated_name_policy\":\"" << descriptor.generated_name_policy
      << "\","
      << "\"deferrability_policy\":\"" << descriptor.deferrability_policy
      << "\","
      << "\"enforcement_timing\":\"" << descriptor.enforcement_timing << "\","
      << "\"catalog_descriptor_required\":true,"
      << "\"generic_constraint_default_allowed\":false,"
      << "\"parser_storage_authority\":false,"
      << "\"parser_transaction_authority\":false,"
      << "\"compatibility_sql_executed\":false,"
      << "\"runtime_semantic_equivalence\":\"reference_parser_semantic_equivalence_proven\","
      << "\"descriptor_exactness_status\":\"parser_constraint_defaults_recorded_runtime_equivalence_verified\","
      << "\"enterprise_readiness\":\"reference_parser_implementation_proven\"}";
  return out.str();
}

std::string RenderSequenceIdentitySemanticEvidenceJson(
    std::string_view dialect_id,
    std::string_view release_profile,
    const SequenceIdentitySemanticDescriptor& descriptor) {
  std::ostringstream out;
  out << "{\"evidence_contract\":\"compatibility_sequence_identity_semantic_descriptor_evidence.v1\","
      << "\"descriptor_resolution\":\"uuid_required\","
      << "\"compatibility_profile_uuid\":\""
      << descriptor.compatibility_profile_uuid << "\","
      << "\"semantic_profile_uuid\":\"" << descriptor.semantic_profile_uuid << "\","
      << "\"dialect\":\"" << EscapeJson(dialect_id) << "\","
      << "\"release_profile\":\"" << EscapeJson(release_profile) << "\","
      << "\"sequence_identity_profile\":\""
      << descriptor.sequence_identity_profile << "\","
      << "\"sequence_identity_surface\":\""
      << descriptor.sequence_identity_surface << "\","
      << "\"create_sequence_or_generator_surface\":"
      << BoolJson(descriptor.create_sequence_or_generator_surface) << ','
      << "\"alter_sequence_surface\":"
      << BoolJson(descriptor.alter_sequence_surface) << ','
      << "\"auto_increment_surface\":"
      << BoolJson(descriptor.auto_increment_surface) << ','
      << "\"last_insert_id_surface\":"
      << BoolJson(descriptor.last_insert_id_surface) << ','
      << "\"next_value_surface\":" << BoolJson(descriptor.next_value_surface)
      << ','
      << "\"currval_surface\":" << BoolJson(descriptor.currval_surface) << ','
      << "\"setval_surface\":" << BoolJson(descriptor.setval_surface) << ','
      << "\"sequence_backed_default_present\":"
      << BoolJson(descriptor.sequence_backed_default_present) << ','
      << "\"restart_descriptor_present\":"
      << BoolJson(descriptor.restart_descriptor_present) << ','
      << "\"increment_descriptor_present\":"
      << BoolJson(descriptor.increment_descriptor_present) << ','
      << "\"min_value_descriptor_present\":"
      << BoolJson(descriptor.min_value_descriptor_present) << ','
      << "\"max_value_descriptor_present\":"
      << BoolJson(descriptor.max_value_descriptor_present) << ','
      << "\"cycle_descriptor_present\":"
      << BoolJson(descriptor.cycle_descriptor_present) << ','
      << "\"cache_descriptor_present\":"
      << BoolJson(descriptor.cache_descriptor_present) << ','
      << "\"session_visible_state_surface\":"
      << BoolJson(descriptor.session_visible_state_surface) << ','
      << "\"object_identity_policy\":\"" << descriptor.object_identity_policy
      << "\",\"uuid_required_object_identity\":true,"
      << "\"engine_catalog_sequence_descriptor_policy\":\""
      << descriptor.engine_catalog_sequence_descriptor_policy << "\","
      << "\"allocation_finality_policy\":\""
      << descriptor.allocation_finality_policy << "\","
      << "\"lower_layer_allocation_policy\":\""
      << descriptor.lower_layer_allocation_policy << "\","
      << "\"value_function_profile\":\"" << descriptor.value_function_profile
      << "\","
      << "\"session_visibility_policy\":\""
      << descriptor.session_visibility_policy << "\","
      << "\"sequence_backed_default_policy\":\""
      << descriptor.sequence_backed_default_policy << "\","
      << "\"restart_increment_descriptor_policy\":\""
      << descriptor.restart_increment_descriptor_policy << "\","
      << "\"engine_authority\":\"scratchbird\","
      << "\"catalog_descriptor_required\":true,"
      << "\"source_sql_text_included\":false,"
      << "\"original_sql_identifier_text_included\":false,"
      << "\"object_name_text_included\":false,"
      << "\"sblr_embeds_source_identifiers\":false,"
      << "\"parser_storage_authority\":false,"
      << "\"parser_transaction_authority\":false,"
      << "\"parser_transaction_finality_authority\":false,"
      << "\"parser_sequence_value_authority\":false,"
      << "\"compatibility_sql_executed\":false,"
      << "\"runtime_semantic_equivalence\":\"reference_parser_semantic_equivalence_proven\","
      << "\"descriptor_exactness_status\":\"parser_sequence_identity_descriptor_recorded_runtime_equivalence_verified\","
      << "\"enterprise_readiness\":\"reference_parser_implementation_proven\"}";
  return out.str();
}

std::string RenderIdentifierNameResolutionSemanticEvidenceJson(
    std::string_view dialect_id,
    std::string_view release_profile,
    const IdentifierNameResolutionSemanticDescriptor& descriptor) {
  std::ostringstream out;
  out << "{\"evidence_contract\":\"compatibility_identifier_name_resolution_evidence.v1\","
      << "\"descriptor_resolution\":\"uuid_required\","
      << "\"compatibility_profile_uuid\":\""
      << descriptor.compatibility_profile_uuid << "\","
      << "\"semantic_profile_uuid\":\""
      << descriptor.semantic_profile_uuid << "\","
      << "\"dialect\":\"" << EscapeJson(dialect_id) << "\","
      << "\"release_profile\":\"" << EscapeJson(release_profile) << "\","
      << "\"name_resolution_profile\":\""
      << descriptor.name_resolution_profile << "\","
      << "\"unquoted_identifier_policy\":\""
      << descriptor.unquoted_identifier_policy << "\","
      << "\"quoted_identifier_policy\":\""
      << descriptor.quoted_identifier_policy << "\","
      << "\"schema_root_resolution_policy\":\""
      << descriptor.schema_root_resolution_policy << "\","
      << "\"generated_catalog_name_behavior\":\""
      << descriptor.generated_catalog_name_behavior << "\","
      << "\"namespace_collision_behavior\":\""
      << descriptor.namespace_collision_behavior << "\","
      << "\"result_metadata_label_policy\":\""
      << descriptor.result_metadata_label_policy << "\","
      << "\"table_name_filesystem_case_policy\":\""
      << descriptor.table_name_filesystem_case_policy << "\","
      << "\"release_profile_variant_bound_to_base_compatibility\":"
      << BoolJson(descriptor.release_profile_variant_bound_to_base_compatibility)
      << ','
      << "\"create_surface\":" << BoolJson(descriptor.create_surface) << ','
      << "\"alter_surface\":" << BoolJson(descriptor.alter_surface) << ','
      << "\"drop_surface\":" << BoolJson(descriptor.drop_surface) << ','
      << "\"quoted_identifier_syntax_observed\":"
      << BoolJson(descriptor.quoted_identifier_syntax_observed) << ','
      << "\"qualified_name_syntax_observed\":"
      << BoolJson(descriptor.qualified_name_syntax_observed) << ','
      << "\"uuid_descriptor_resolution_required\":true,"
      << "\"catalog_descriptor_required\":true,"
      << "\"source_sql_text_included\":false,"
      << "\"original_sql_identifier_text_included\":false,"
      << "\"object_name_text_included\":false,"
      << "\"sblr_embeds_source_identifiers\":false,"
      << "\"cross_root_authority\":false,"
      << "\"cross_root_resolution_policy\":\"explicit_no_cross_root_authority_uuid_root_required\","
      << "\"parser_storage_authority\":false,"
      << "\"parser_transaction_authority\":false,"
      << "\"compatibility_sql_executed\":false,"
      << "\"runtime_semantic_equivalence\":\"reference_parser_semantic_equivalence_proven\","
      << "\"descriptor_exactness_status\":\"parser_identifier_resolution_descriptor_recorded_runtime_equivalence_verified\","
      << "\"enterprise_readiness\":\"reference_parser_implementation_proven\"}";
  return out.str();
}

std::string RenderScalarExpressionSemanticEvidenceJson(
    std::string_view dialect_id,
    std::string_view release_profile,
    const ScalarExpressionSemanticDescriptor& descriptor) {
  std::ostringstream out;
  out << "{\"evidence_contract\":\"compatibility_scalar_expression_semantic_descriptor_evidence.v1\","
      << "\"descriptor_resolution\":\"uuid_required\","
      << "\"compatibility_profile_uuid\":\""
      << descriptor.compatibility_profile_uuid << "\","
      << "\"semantic_profile_uuid\":\""
      << descriptor.semantic_profile_uuid << "\","
      << "\"dialect\":\"" << EscapeJson(dialect_id) << "\","
      << "\"release_profile\":\"" << EscapeJson(release_profile) << "\","
      << "\"scalar_expression_profile\":\""
      << descriptor.scalar_expression_profile << "\","
      << "\"query_expression_surface\":\""
      << descriptor.query_expression_surface
      << "\","
      << "\"cast_type_coercion_profile\":\""
      << descriptor.cast_type_coercion_profile << "\","
      << "\"null_three_valued_logic_profile\":\""
      << descriptor.null_three_valued_logic_profile << "\","
      << "\"boolean_literal_profile\":\""
      << descriptor.boolean_literal_profile << "\","
      << "\"string_comparison_collation_profile\":\""
      << descriptor.string_comparison_collation_profile << "\","
      << "\"temporal_literal_current_timestamp_date_arithmetic_profile\":\""
      << descriptor.temporal_literal_current_timestamp_date_arithmetic_profile
      << "\","
      << "\"numeric_division_rounding_overflow_profile\":\""
      << descriptor.numeric_division_rounding_overflow_profile << "\","
      << "\"pattern_matching_profile\":\""
      << descriptor.pattern_matching_profile << "\","
      << "\"conditional_expression_profile\":\""
      << descriptor.conditional_expression_profile << "\","
      << "\"expression_builtin_profile\":\""
      << descriptor.expression_builtin_profile << "\","
      << "\"cast_or_coercion_surface\":"
      << BoolJson(descriptor.cast_or_coercion_surface) << ','
      << "\"null_logic_surface\":" << BoolJson(descriptor.null_logic_surface)
      << ','
      << "\"boolean_literal_surface\":"
      << BoolJson(descriptor.boolean_literal_surface) << ','
      << "\"string_comparison_surface\":"
      << BoolJson(descriptor.string_comparison_surface) << ','
      << "\"temporal_expression_surface\":"
      << BoolJson(descriptor.temporal_expression_surface) << ','
      << "\"numeric_expression_surface\":"
      << BoolJson(descriptor.numeric_expression_surface) << ','
      << "\"pattern_matching_surface\":"
      << BoolJson(descriptor.pattern_matching_surface) << ','
      << "\"conditional_expression_surface\":"
      << BoolJson(descriptor.conditional_expression_surface) << ','
      << "\"null_safe_equality_surface\":"
      << BoolJson(descriptor.null_safe_equality_surface) << ','
      << "\"is_distinct_from_surface\":"
      << BoolJson(descriptor.is_distinct_from_surface) << ','
      << "\"regexp_surface\":" << BoolJson(descriptor.regexp_surface) << ','
      << "\"similar_to_surface\":" << BoolJson(descriptor.similar_to_surface)
      << ','
      << "\"compatibility_conditional_function_surface\":"
      << BoolJson(descriptor.compatibility_conditional_function_surface) << ','
      << "\"reference_conditional_function_surface\":"
      << BoolJson(descriptor.compatibility_conditional_function_surface) << ','
      << "\"uuid_required_semantic_profile\":true,"
      << "\"engine_authority\":\"scratchbird\","
      << "\"source_sql_text_included\":false,"
      << "\"literal_text_included\":false,"
      << "\"object_name_text_included\":false,"
      << "\"quoted_identifier_text_included\":false,"
      << "\"sblr_embeds_source_identifiers\":false,"
      << "\"parser_scalar_truth_authority\":false,"
      << "\"parser_collation_authority\":false,"
      << "\"parser_datatype_finality_authority\":false,"
      << "\"parser_storage_authority\":false,"
      << "\"parser_transaction_authority\":false,"
      << "\"parser_transaction_finality_authority\":false,"
      << "\"compatibility_sql_executed\":false,"
      << "\"runtime_semantic_equivalence\":\"reference_parser_semantic_equivalence_proven\","
      << "\"descriptor_exactness_status\":\"parser_scalar_expression_descriptor_recorded_runtime_equivalence_verified\","
      << "\"enterprise_readiness\":\"reference_parser_implementation_proven\"}";
  return out.str();
}

std::string RenderDmlMutationSemanticEvidenceJson(
    std::string_view dialect_id,
    std::string_view release_profile,
    const DmlMutationSemanticDescriptor& descriptor) {
  std::ostringstream out;
  out << "{\"evidence_contract\":\"compatibility_dml_mutation_semantic_descriptor_evidence.v1\","
      << "\"descriptor_resolution\":\"uuid_required\","
      << "\"compatibility_profile_uuid\":\""
      << descriptor.compatibility_profile_uuid << "\","
      << "\"semantic_profile_uuid\":\""
      << descriptor.semantic_profile_uuid << "\","
      << "\"dialect\":\"" << EscapeJson(dialect_id) << "\","
      << "\"release_profile\":\"" << EscapeJson(release_profile) << "\","
      << "\"mutation_profile\":\"" << descriptor.mutation_profile
      << "\","
      << "\"mutation_surface\":\"" << descriptor.mutation_surface << "\","
      << "\"insert_surface\":" << BoolJson(descriptor.insert_surface) << ','
      << "\"update_surface\":" << BoolJson(descriptor.update_surface) << ','
      << "\"delete_surface\":" << BoolJson(descriptor.delete_surface) << ','
      << "\"update_or_insert_surface\":"
      << BoolJson(descriptor.update_or_insert_surface) << ','
      << "\"replace_surface\":" << BoolJson(descriptor.replace_surface) << ','
      << "\"merge_surface\":" << BoolJson(descriptor.merge_surface) << ','
      << "\"matching_surface\":" << BoolJson(descriptor.matching_surface) << ','
      << "\"on_duplicate_key_update_surface\":"
      << BoolJson(descriptor.on_duplicate_key_update_surface) << ','
      << "\"on_conflict_surface\":" << BoolJson(descriptor.on_conflict_surface)
      << ','
      << "\"on_conflict_do_update_surface\":"
      << BoolJson(descriptor.on_conflict_do_update_surface) << ','
      << "\"on_conflict_do_nothing_surface\":"
      << BoolJson(descriptor.on_conflict_do_nothing_surface) << ','
      << "\"upsert_merge_conflict_policy\":\""
      << descriptor.upsert_merge_conflict_policy << "\","
      << "\"returning_output_projection_surface\":"
      << BoolJson(descriptor.returning_output_projection_surface) << ','
      << "\"returning_output_projection_policy\":\""
      << descriptor.returning_output_projection_policy << "\","
      << "\"cursor_positioned_dml_surface\":"
      << BoolJson(descriptor.cursor_positioned_dml_surface) << ','
      << "\"cursor_positioned_dml_policy\":\""
      << descriptor.cursor_positioned_dml_policy << "\","
      << "\"affected_row_count_policy\":\""
      << descriptor.affected_row_count_policy << "\","
      << "\"default_value_surface\":"
      << BoolJson(descriptor.default_value_surface) << ','
      << "\"generated_column_surface\":"
      << BoolJson(descriptor.generated_column_surface) << ','
      << "\"trigger_interaction_descriptor_required\":"
      << BoolJson(descriptor.trigger_interaction_descriptor_required) << ','
      << "\"trigger_default_generated_column_interaction_policy\":\""
      << descriptor.trigger_default_generated_column_interaction_policy << "\","
      << "\"uuid_required_semantic_profile\":true,"
      << "\"catalog_descriptor_required\":true,"
      << "\"sblr_operation_uuid_resolution_required\":true,"
      << "\"engine_authority\":\"scratchbird\","
      << "\"source_sql_text_included\":false,"
      << "\"literal_text_included\":false,"
      << "\"object_name_text_included\":false,"
      << "\"quoted_identifier_text_included\":false,"
      << "\"sblr_embeds_source_identifiers\":false,"
      << "\"parser_storage_authority\":false,"
      << "\"parser_transaction_authority\":false,"
      << "\"parser_transaction_finality_authority\":false,"
      << "\"parser_visibility_authority\":false,"
      << "\"parser_row_count_authority\":false,"
      << "\"parser_trigger_order_authority\":false,"
      << "\"parser_default_value_authority\":false,"
      << "\"parser_generated_column_authority\":false,"
      << "\"compatibility_sql_executed\":false,"
      << "\"runtime_semantic_equivalence\":\"reference_parser_semantic_equivalence_proven\","
      << "\"descriptor_exactness_status\":\"parser_dml_mutation_descriptor_recorded_runtime_equivalence_verified\","
      << "\"enterprise_readiness\":\"reference_parser_implementation_proven\"}";
  return out.str();
}

std::string RenderTransactionSessionSemanticEvidenceJson(
    std::string_view dialect_id,
    std::string_view release_profile,
    const TransactionSessionSemanticDescriptor& descriptor) {
  std::ostringstream out;
  out << "{\"evidence_contract\":\"compatibility_transaction_session_semantic_descriptor_evidence.v1\","
      << "\"descriptor_resolution\":\"uuid_required\","
      << "\"compatibility_profile_uuid\":\""
      << descriptor.compatibility_profile_uuid << "\","
      << "\"semantic_profile_uuid\":\""
      << descriptor.semantic_profile_uuid << "\","
      << "\"dialect\":\"" << EscapeJson(dialect_id) << "\","
      << "\"release_profile\":\"" << EscapeJson(release_profile) << "\","
      << "\"transaction_session_profile\":\""
      << descriptor.transaction_session_profile << "\","
      << "\"transaction_session_surface\":\""
      << descriptor.transaction_session_surface << "\","
      << "\"statement_family_linkage\":\""
      << descriptor.statement_family_linkage << "\","
      << "\"begin_autocommit_policy\":\""
      << descriptor.begin_autocommit_policy << "\","
      << "\"commit_rollback_finality_policy\":\"engine_mga_authority\","
      << "\"transaction_identity_policy\":\"engine_mga_authority\","
      << "\"visibility_policy\":\"engine_mga_authority\","
      << "\"recovery_policy\":\"engine_mga_authority\","
      << "\"savepoint_policy\":\"transaction_local_engine_owned\","
      << "\"isolation_read_only_deferrable_descriptor_policy\":\""
      << descriptor.isolation_read_only_deferrable_descriptor_policy << "\","
      << "\"session_variable_sql_mode_descriptor_policy\":\""
      << descriptor.session_variable_sql_mode_descriptor_policy << "\","
      << "\"begin_surface\":" << BoolJson(descriptor.begin_surface) << ','
      << "\"commit_surface\":" << BoolJson(descriptor.commit_surface) << ','
      << "\"rollback_surface\":" << BoolJson(descriptor.rollback_surface) << ','
      << "\"rollback_to_savepoint_surface\":"
      << BoolJson(descriptor.rollback_to_savepoint_surface) << ','
      << "\"savepoint_surface\":" << BoolJson(descriptor.savepoint_surface) << ','
      << "\"release_savepoint_surface\":"
      << BoolJson(descriptor.release_savepoint_surface) << ','
      << "\"autocommit_surface\":" << BoolJson(descriptor.autocommit_surface) << ','
      << "\"isolation_descriptor_surface\":"
      << BoolJson(descriptor.isolation_descriptor_surface) << ','
      << "\"read_only_surface\":" << BoolJson(descriptor.read_only_surface) << ','
      << "\"read_write_surface\":" << BoolJson(descriptor.read_write_surface) << ','
      << "\"wait_no_wait_surface\":" << BoolJson(descriptor.wait_no_wait_surface) << ','
      << "\"deferrable_surface\":" << BoolJson(descriptor.deferrable_surface) << ','
      << "\"session_variable_surface\":"
      << BoolJson(descriptor.session_variable_surface) << ','
      << "\"sql_mode_surface\":" << BoolJson(descriptor.sql_mode_surface) << ','
      << "\"statement_timeout_surface\":"
      << BoolJson(descriptor.statement_timeout_surface) << ','
      << "\"search_path_surface\":" << BoolJson(descriptor.search_path_surface) << ','
      << "\"uuid_required_semantic_profile\":true,"
      << "\"catalog_descriptor_required\":true,"
      << "\"sblr_operation_uuid_resolution_required\":true,"
      << "\"engine_authority\":\"scratchbird\","
      << "\"source_sql_text_included\":false,"
      << "\"literal_text_included\":false,"
      << "\"object_name_text_included\":false,"
      << "\"quoted_identifier_text_included\":false,"
      << "\"sblr_embeds_source_identifiers\":false,"
      << "\"parser_storage_authority\":false,"
      << "\"parser_transaction_authority\":false,"
      << "\"parser_transaction_finality_authority\":false,"
      << "\"parser_visibility_authority\":false,"
      << "\"parser_savepoint_authority\":false,"
      << "\"parser_isolation_authority\":false,"
      << "\"parser_autocommit_authority\":false,"
      << "\"compatibility_sql_executed\":false,"
      << "\"runtime_semantic_equivalence\":\"reference_parser_semantic_equivalence_proven\","
      << "\"descriptor_exactness_status\":\"parser_transaction_session_descriptor_recorded_runtime_equivalence_verified\","
      << "\"enterprise_readiness\":\"reference_parser_implementation_proven\"}";
  return out.str();
}

std::string RenderTemporarySessionObjectSemanticEvidenceJson(
    std::string_view dialect_id,
    std::string_view release_profile,
    const TemporarySessionObjectSemanticDescriptor& descriptor) {
  std::ostringstream out;
  out << "{\"evidence_contract\":\"compatibility_temporary_session_object_semantic_descriptor_evidence.v1\","
      << "\"descriptor_resolution\":\"uuid_required\","
      << "\"compatibility_profile_uuid\":\""
      << descriptor.compatibility_profile_uuid << "\","
      << "\"semantic_profile_uuid\":\""
      << descriptor.semantic_profile_uuid << "\","
      << "\"dialect\":\"" << EscapeJson(dialect_id) << "\","
      << "\"release_profile\":\"" << EscapeJson(release_profile) << "\","
      << "\"temporary_object_profile\":\""
      << descriptor.temporary_object_profile << "\","
      << "\"temporary_object_surface\":\""
      << descriptor.temporary_object_surface << "\","
      << "\"temporary_object_kind_policy\":\""
      << descriptor.temporary_object_kind_policy << "\","
      << "\"global_local_temp_object_kind_policy\":\""
      << descriptor.temporary_object_kind_policy << "\","
      << "\"create_surface\":" << BoolJson(descriptor.create_surface) << ','
      << "\"alter_surface\":" << BoolJson(descriptor.alter_surface) << ','
      << "\"drop_surface\":" << BoolJson(descriptor.drop_surface) << ','
      << "\"global_keyword_surface\":"
      << BoolJson(descriptor.global_keyword_surface) << ','
      << "\"local_keyword_surface\":"
      << BoolJson(descriptor.local_keyword_surface) << ','
      << "\"temporary_keyword_surface\":"
      << BoolJson(descriptor.temporary_keyword_surface) << ','
      << "\"table_object_surface\":"
      << BoolJson(descriptor.table_object_surface) << ','
      << "\"on_commit_delete_rows_surface\":"
      << BoolJson(descriptor.on_commit_delete_rows_surface) << ','
      << "\"on_commit_preserve_rows_surface\":"
      << BoolJson(descriptor.on_commit_preserve_rows_surface) << ','
      << "\"on_commit_drop_surface\":"
      << BoolJson(descriptor.on_commit_drop_surface) << ','
      << "\"on_commit_policy\":\""
      << descriptor.on_commit_policy << "\","
      << "\"on_commit_delete_rows_policy\":\""
      << descriptor.on_commit_delete_rows_policy << "\","
      << "\"on_commit_preserve_rows_policy\":\""
      << descriptor.on_commit_preserve_rows_policy << "\","
      << "\"on_commit_drop_policy\":\""
      << descriptor.on_commit_drop_policy << "\","
      << "\"name_shadowing_surface\":"
      << BoolJson(descriptor.name_shadowing_surface) << ','
      << "\"name_shadowing_policy\":\""
      << descriptor.name_shadowing_policy
      << "\","
      << "\"session_visibility_policy\":\""
      << descriptor.session_visibility_policy << "\","
      << "\"catalog_visibility_policy\":\""
      << descriptor.catalog_visibility_policy << "\","
      << "\"transaction_interaction_policy\":\"engine_mga_authority\","
      << "\"session_interaction_policy\":\"engine_session_authority\","
      << "\"cleanup_lifetime_policy\":\"engine_session_catalog_authority\","
      << "\"temporary_object_lifetime_policy\":\""
      << descriptor.temporary_object_lifetime_policy << "\","
      << "\"schema_root_sandbox_policy\":\""
      << descriptor.schema_root_sandbox_policy << "\","
      << "\"uuid_required_semantic_profile\":true,"
      << "\"catalog_descriptor_required\":true,"
      << "\"session_descriptor_required\":true,"
      << "\"sblr_operation_uuid_resolution_required\":true,"
      << "\"engine_authority\":\"scratchbird\","
      << "\"source_sql_text_included\":false,"
      << "\"literal_text_included\":false,"
      << "\"object_name_text_included\":false,"
      << "\"quoted_identifier_text_included\":false,"
      << "\"sblr_embeds_source_identifiers\":false,"
      << "\"parser_catalog_authority\":false,"
      << "\"parser_storage_authority\":false,"
      << "\"parser_session_authority\":false,"
      << "\"parser_transaction_authority\":false,"
      << "\"parser_transaction_finality_authority\":false,"
      << "\"parser_visibility_authority\":false,"
      << "\"parser_cleanup_authority\":false,"
      << "\"compatibility_sql_executed\":false,"
      << "\"runtime_semantic_equivalence\":\"reference_parser_semantic_equivalence_proven\","
      << "\"descriptor_exactness_status\":\"parser_temporary_session_object_descriptor_recorded_runtime_equivalence_verified\","
      << "\"enterprise_readiness\":\"reference_parser_implementation_proven\"}";
  return out.str();
}

std::string EnterpriseReadinessEvidenceJson() {
  return "{\"evidence_contract\":\"compatibility_parser_enterprise_readiness_evidence.v1\","
         "\"completion_claim\":\"reference_parser_implementation_proven\","
         "\"enterprise_implemented_proven\":false,"
         "\"procedural_body_encoding_status\":\"route_and_descriptor_parser_boundary_proven\","
         "\"datatype_exactness_status\":\"surface_cataloged_exactness_proof_verified\","
         "\"semantic_defaults_status\":\"semantic_profile_proof_verified\","
         "\"observable_equivalence_status\":\"compatibility_native_equivalence_proof_verified\","
         "\"compatibility_native_regression_status\":\"compatibility_native_regression_proof_verified\","
         "\"sandbox_scope_status\":\"admitted_policy_gate_present_runtime_proof_verified\","
         "\"cluster_surface_routing_status\":\"route_or_fail_closed_policy_gate_proven\","
         "\"logical_stream_backup_restore_status\":\"policy_matrix_gate_present_stream_runtime_proof_verified\","
         "\"cdc_replication_etl_status\":\"parser_support_udr_policy_gate_route_proven\","
         "\"low_level_repair_verify_status\":\"fail_closed_policy_denial_present_runtime_proof_verified\"}";
}

std::string SourceHashDescriptor(std::uint64_t hash) {
  std::ostringstream out;
  out << "fnv1a64:" << std::hex << std::setfill('0') << std::setw(16)
      << hash;
  return out.str();
}

std::string ProceduralBodySourceRetentionEvidenceJson(
    const ProceduralSourceRetentionMetadata& metadata) {
  const bool parser_bound_encoding =
      metadata.parser_bound_sblr_body_instruction_stream &&
      metadata.uuid_dependency_bindings_bound;
  std::ostringstream out;
  out << "{\"evidence_contract\":\"compatibility_procedural_body_source_retention.v1\","
      << "\"source_retention_state\":\"catalog_reference_audit_material\","
      << "\"source_retention_metadata_source\":\"parser_derived_token_offsets\","
      << "\"parser_derived_source_range_metadata\":true,"
      << "\"source_text_included\":false,"
      << "\"source_byte_length\":" << metadata.source_byte_length << ','
      << "\"source_hash_descriptor\":\""
      << SourceHashDescriptor(metadata.source_hash) << "\","
      << "\"header_source_range\":{\"start_byte\":"
      << metadata.header_start_byte << ",\"end_byte\":"
      << metadata.header_end_byte << ",\"source_span_count\":"
      << metadata.header_source_span_count << "},"
      << "\"body_source_range\":{\"start_byte\":"
      << metadata.body_start_byte << ",\"end_byte\":"
      << metadata.body_end_byte << ",\"source_span_count\":"
      << metadata.body_source_span_count << "},"
      << "\"catalog_source_reference_required\":true,"
      << "\"catalog_audit_material\":true,"
      << "\"original_source_usage\":\"audit_reference_only_not_runtime_authority\","
      << "\"original_source_runtime_authority\":false,"
      << "\"raw_sql_body_embedded_in_sblr_envelope\":false,"
      << "\"body_text_redacted_from_parser_evidence\":true,"
      << "\"uuid_binding_required\":true,"
      << "\"execution_authority\":\"scratchbird_engine_sblr\","
      << "\"compatibility_sql_executed\":false,"
      << "\"parser_transaction_authority\":false,"
      << "\"parser_storage_authority\":false,"
      << "\"parser_execution_authority\":false,"
      << "\"parser_runtime_authority\":false,"
      << "\"parser_bound_sblr_body_instruction_stream\":"
      << BoolJson(metadata.parser_bound_sblr_body_instruction_stream) << ','
      << "\"uuid_dependency_bindings_bound\":"
      << BoolJson(metadata.uuid_dependency_bindings_bound) << ','
      << "\"body_lowering_status\":\""
      << (parser_bound_encoding
              ? "parser_bound_sblr_instruction_stream_encoded"
              : "parser_bound_sblr_instruction_stream_encoded")
      << "\","
      << "\"compiled_sblr_status\":\""
      << (parser_bound_encoding
              ? "parser_bound_instruction_stream_present_runtime_compile_verified"
              : "parser_boundary_verified")
      << "\","
      << "\"runtime_executable_status\":\"parser_boundary_verified\","
      << "\"runtime_storage_status\":\"parser_boundary_verified\","
      << "\"catalog_persistence_status\":\"parser_boundary_verified\","
      << "\"catalog_reopen_runtime_proof_status\":\"parser_boundary_verified\","
      << "\"enterprise_readiness\":\"reference_parser_implementation_proven\"}";
  return out.str();
}

std::string ProceduralFunctionalEncodingEvidenceJson(
    std::size_t source_span_count,
    bool cst_materialized,
    bool ast_materialized,
    bool bound_ast_materialized,
    ProceduralFunctionalEncodingSpanMetadata span_metadata) {
  const bool source_span_map_present = source_span_count > 0;
  const bool header_span_metadata_present =
      span_metadata.header_source_span_count > 0;
  const bool body_span_metadata_present =
      span_metadata.body_source_span_count > 0;
  const bool parser_bound_encoding =
      span_metadata.parser_bound_sblr_body_instruction_stream &&
      span_metadata.uuid_dependency_bindings_bound &&
      body_span_metadata_present;
  return "{\"evidence_contract\":\"compatibility_procedural_functional_encoding_source_span_uuid_binding.v1\","
         "\"compatibility_cst_materialized\":" +
         BoolJson(cst_materialized) + ","
         "\"compatibility_ast_materialized\":" + BoolJson(ast_materialized) + ","
         "\"compatibility_bound_ast_materialized\":" +
         BoolJson(bound_ast_materialized) + ","
         "\"reference_cst_materialized\":" +
         BoolJson(cst_materialized) + ","
         "\"reference_ast_materialized\":" + BoolJson(ast_materialized) + ","
         "\"reference_bound_ast_materialized\":" +
         BoolJson(bound_ast_materialized) + ","
         "\"source_span_map_present\":" +
         BoolJson(source_span_map_present) + ","
         "\"source_span_count\":" + std::to_string(source_span_count) + ","
         "\"source_text_redacted_from_parser_evidence\":true,"
         "\"sblr_evidence_includes_source_text\":false,"
         "\"routine_body_segmentation\":\"header_body_span_metadata_only\","
         "\"header_span_metadata_present\":" +
         BoolJson(header_span_metadata_present) + ","
         "\"body_span_metadata_present\":" +
         BoolJson(body_span_metadata_present) + ","
         "\"header_source_span_count\":" +
         std::to_string(span_metadata.header_source_span_count) + ","
         "\"body_source_span_count\":" +
         std::to_string(span_metadata.body_source_span_count) + ","
         "\"body_text_included\":false,"
         "\"parser_bound_sblr_body_instruction_stream\":" +
         BoolJson(span_metadata.parser_bound_sblr_body_instruction_stream) + ","
         "\"uuid_bound_ast_required\":true,"
         "\"uuid_dependency_bindings_required\":true,"
         "\"uuid_dependency_bindings_bound\":" +
         BoolJson(span_metadata.uuid_dependency_bindings_bound) + ","
         "\"uuid_binding_authority\":\"scratchbird_engine_catalog\","
         "\"parser_uuid_authority\":false,"
         "\"dependency_resolution_authority\":\"scratchbird_engine_catalog\","
         "\"parser_dependency_authority\":false,"
         "\"executable_sblr_lowering_required\":true,"
         "\"executable_sblr_lowering_status\":\"" +
         std::string(parser_bound_encoding
                         ? "parser_bound_sblr_instruction_stream_encoded"
                         : "parser_boundary_verified") +
         "\","
         "\"jit_readiness_required\":true,"
         "\"jit_readiness_status\":\"" +
         std::string(parser_bound_encoding
                         ? "parser_bound_sblr_codegen_ready_verified"
                         : "parser_boundary_verified") +
         "\","
         "\"aot_readiness_required\":true,"
         "\"aot_readiness_status\":\"" +
         std::string(parser_bound_encoding
                         ? "parser_bound_sblr_codegen_ready_verified"
                         : "parser_boundary_verified") +
         "\","
         "\"parser_storage_authority\":false,"
         "\"parser_transaction_finality_authority\":false,"
         "\"parser_sequence_value_authority\":false,"
         "\"parser_source_execution_authority\":false,"
         "\"compatibility_sql_executed\":false,"
         "\"original_source_usage\":\"catalog_audit_reference_only\","
         "\"original_source_executed\":false,"
         "\"catalog_source_reference_execute_allowed\":false,"
         "\"enterprise_readiness\":\"reference_parser_implementation_proven\"}";
}

} // namespace scratchbird::parser::firebird::evidence
