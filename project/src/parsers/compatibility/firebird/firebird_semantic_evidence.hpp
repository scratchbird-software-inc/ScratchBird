// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace scratchbird::parser::firebird::evidence {

// This ABI contains data-only descriptors and renderers. It deliberately has
// no token model, lexer, SQL parser, command intake, dialect profile, semantic
// selector, or worker-session surface.
struct ProceduralFunctionalEncodingSpanMetadata {
  std::size_t header_source_span_count{0};
  std::size_t body_source_span_count{0};
  bool parser_bound_sblr_body_instruction_stream{false};
  bool uuid_dependency_bindings_bound{false};
};

struct ProceduralSourceRetentionMetadata {
  std::size_t source_byte_length{0};
  std::uint64_t source_hash{0};
  std::size_t header_start_byte{0};
  std::size_t header_end_byte{0};
  std::size_t body_start_byte{0};
  std::size_t body_end_byte{0};
  std::size_t header_source_span_count{0};
  std::size_t body_source_span_count{0};
  bool parser_bound_sblr_body_instruction_stream{false};
  bool uuid_dependency_bindings_bound{false};
};

// Family-owned transaction/session values consumed by the Firebird evidence
// renderer.  The owning parser classifies its syntax and supplies every
// policy/default value; this structure carries data only.
struct TransactionSessionSemanticDescriptor {
  std::string_view compatibility_profile_uuid;
  std::string_view semantic_profile_uuid;
  std::string_view transaction_session_profile;
  std::string_view transaction_session_surface;
  std::string_view statement_family_linkage;
  std::string_view begin_autocommit_policy;
  std::string_view isolation_read_only_deferrable_descriptor_policy;
  std::string_view session_variable_sql_mode_descriptor_policy;
  bool begin_surface{false};
  bool commit_surface{false};
  bool rollback_surface{false};
  bool rollback_to_savepoint_surface{false};
  bool savepoint_surface{false};
  bool release_savepoint_surface{false};
  bool autocommit_surface{false};
  bool isolation_descriptor_surface{false};
  bool read_only_surface{false};
  bool read_write_surface{false};
  bool wait_no_wait_surface{false};
  bool deferrable_surface{false};
  bool session_variable_surface{false};
  bool sql_mode_surface{false};
  bool statement_timeout_surface{false};
  bool search_path_surface{false};
};

// Family-owned temporary-object values consumed by the family evidence
// renderer.  No parser grammar, lifetime default, or visibility policy is
// selected by the evidence renderer.
struct TemporarySessionObjectSemanticDescriptor {
  std::string_view compatibility_profile_uuid;
  std::string_view semantic_profile_uuid;
  std::string_view temporary_object_profile;
  std::string_view temporary_object_surface;
  std::string_view temporary_object_kind_policy;
  std::string_view on_commit_policy;
  std::string_view on_commit_delete_rows_policy;
  std::string_view on_commit_preserve_rows_policy;
  std::string_view on_commit_drop_policy;
  std::string_view name_shadowing_policy;
  std::string_view session_visibility_policy;
  std::string_view catalog_visibility_policy;
  std::string_view temporary_object_lifetime_policy;
  std::string_view schema_root_sandbox_policy;
  bool create_surface{false};
  bool alter_surface{false};
  bool drop_surface{false};
  bool global_keyword_surface{false};
  bool local_keyword_surface{false};
  bool temporary_keyword_surface{false};
  bool table_object_surface{false};
  bool on_commit_delete_rows_surface{false};
  bool on_commit_preserve_rows_surface{false};
  bool on_commit_drop_surface{false};
  bool name_shadowing_surface{false};
};

struct DatatypeFamilySemanticDescriptor {
  std::string_view compatibility_profile_uuid;
  bool numeric{false};
  bool exact_decimal{false};
  bool floating{false};
  bool text{false};
  bool charset_collation_sensitive_text{false};
  bool binary_blob{false};
  bool temporal{false};
  bool boolean{false};
  bool json_document{false};
  bool uuid{false};
  bool array{false};
  bool enum_set{false};
  bool network{false};
  bool geometric_spatial{false};
  bool range_domain_composite{false};
};

struct IndexSemanticDefaultsDescriptor {
  std::string_view compatibility_profile_uuid;
  std::string_view semantic_profile_uuid;
  std::string_view index_profile;
  std::string_view ddl_surface;
  std::string_view index_method;
  std::string_view unique_null_policy;
  std::string_view null_ordering;
  std::string_view collation_policy;
  std::string_view operator_family_policy;
  std::string_view predicate_or_expression_policy;
  std::string_view validation_state;
  std::string_view build_mode;
  std::string_view statistics_policy_ref;
  bool unique_requested{false};
  bool predicate_present{false};
  bool expression_key_present{false};
  bool concurrently_requested{false};
  bool descending_requested{false};
  bool nulls_not_distinct_requested{false};
};

struct ConstraintSemanticDefaultsDescriptor {
  std::string_view compatibility_profile_uuid;
  std::string_view semantic_profile_uuid;
  std::string_view constraint_profile;
  std::string_view primary_key_behavior;
  std::string_view unique_null_policy;
  std::string_view foreign_key_action_defaults;
  std::string_view check_truth_table_null_behavior;
  std::string_view default_expression_policy;
  std::string_view generated_identity_autoincrement_policy;
  std::string_view generated_name_policy;
  std::string_view deferrability_policy;
  std::string_view enforcement_timing;
  bool primary_key_present{false};
  bool unique_constraint_present{false};
  bool foreign_key_reference_present{false};
  bool check_constraint_present{false};
  bool default_clause_present{false};
  bool generated_identity_or_autoincrement_present{false};
  bool explicit_constraint_names_present{false};
};

struct SequenceIdentitySemanticDescriptor {
  std::string_view compatibility_profile_uuid;
  std::string_view semantic_profile_uuid;
  std::string_view sequence_identity_profile;
  std::string_view sequence_identity_surface;
  std::string_view object_identity_policy;
  std::string_view engine_catalog_sequence_descriptor_policy;
  std::string_view allocation_finality_policy;
  std::string_view lower_layer_allocation_policy;
  std::string_view value_function_profile;
  std::string_view session_visibility_policy;
  std::string_view sequence_backed_default_policy;
  std::string_view restart_increment_descriptor_policy;
  bool create_sequence_or_generator_surface{false};
  bool alter_sequence_surface{false};
  bool auto_increment_surface{false};
  bool last_insert_id_surface{false};
  bool next_value_surface{false};
  bool currval_surface{false};
  bool setval_surface{false};
  bool sequence_backed_default_present{false};
  bool restart_descriptor_present{false};
  bool increment_descriptor_present{false};
  bool min_value_descriptor_present{false};
  bool max_value_descriptor_present{false};
  bool cycle_descriptor_present{false};
  bool cache_descriptor_present{false};
  bool session_visible_state_surface{false};
};

// Family-owned identifier-resolution values consumed by the neutral evidence
// renderer.  The common component neither folds identifiers nor selects a
// catalog/search-root policy.
struct IdentifierNameResolutionSemanticDescriptor {
  std::string_view compatibility_profile_uuid;
  std::string_view semantic_profile_uuid;
  std::string_view name_resolution_profile;
  std::string_view unquoted_identifier_policy;
  std::string_view quoted_identifier_policy;
  std::string_view schema_root_resolution_policy;
  std::string_view generated_catalog_name_behavior;
  std::string_view namespace_collision_behavior;
  std::string_view result_metadata_label_policy;
  std::string_view table_name_filesystem_case_policy;
  bool release_profile_variant_bound_to_base_compatibility{false};
  bool create_surface{false};
  bool alter_surface{false};
  bool drop_surface{false};
  bool quoted_identifier_syntax_observed{false};
  bool qualified_name_syntax_observed{false};
};

// Family-owned scalar-expression values consumed by the neutral evidence
// renderer.  All grammar recognition and semantic profile selection remains
// in the owning parser.
struct ScalarExpressionSemanticDescriptor {
  std::string_view compatibility_profile_uuid;
  std::string_view semantic_profile_uuid;
  std::string_view scalar_expression_profile;
  std::string_view query_expression_surface;
  std::string_view cast_type_coercion_profile;
  std::string_view null_three_valued_logic_profile;
  std::string_view boolean_literal_profile;
  std::string_view string_comparison_collation_profile;
  std::string_view temporal_literal_current_timestamp_date_arithmetic_profile;
  std::string_view numeric_division_rounding_overflow_profile;
  std::string_view pattern_matching_profile;
  std::string_view conditional_expression_profile;
  std::string_view expression_builtin_profile;
  bool cast_or_coercion_surface{false};
  bool null_logic_surface{false};
  bool boolean_literal_surface{false};
  bool string_comparison_surface{false};
  bool temporal_expression_surface{false};
  bool numeric_expression_surface{false};
  bool pattern_matching_surface{false};
  bool conditional_expression_surface{false};
  bool null_safe_equality_surface{false};
  bool is_distinct_from_surface{false};
  bool regexp_surface{false};
  bool similar_to_surface{false};
  bool compatibility_conditional_function_surface{false};
};

// Family-owned DML mutation values consumed by the neutral evidence renderer.
// The descriptor carries syntax observations and policy labels only; it grants
// the parser no row, trigger, storage, visibility, or transaction authority.
struct DmlMutationSemanticDescriptor {
  std::string_view compatibility_profile_uuid;
  std::string_view semantic_profile_uuid;
  std::string_view mutation_profile;
  std::string_view mutation_surface;
  std::string_view upsert_merge_conflict_policy;
  std::string_view returning_output_projection_policy;
  std::string_view cursor_positioned_dml_policy;
  std::string_view affected_row_count_policy;
  std::string_view trigger_default_generated_column_interaction_policy;
  bool insert_surface{false};
  bool update_surface{false};
  bool delete_surface{false};
  bool update_or_insert_surface{false};
  bool replace_surface{false};
  bool merge_surface{false};
  bool matching_surface{false};
  bool on_duplicate_key_update_surface{false};
  bool on_conflict_surface{false};
  bool on_conflict_do_update_surface{false};
  bool on_conflict_do_nothing_surface{false};
  bool returning_output_projection_surface{false};
  bool cursor_positioned_dml_surface{false};
  bool default_value_surface{false};
  bool generated_column_surface{false};
  bool trigger_interaction_descriptor_required{false};
};

std::string DatatypeDescriptorEvidenceJson(std::size_t datatype_reference_count);
std::string RenderDatatypeProfileEvidenceJson(
    std::string_view dialect_id,
    const DatatypeFamilySemanticDescriptor& descriptor);
std::string RenderIndexSemanticDefaultsEvidenceJson(
    std::string_view dialect_id,
    std::string_view release_profile,
    const IndexSemanticDefaultsDescriptor& descriptor);
std::string RenderConstraintSemanticDefaultsEvidenceJson(
    std::string_view dialect_id,
    std::string_view release_profile,
    const ConstraintSemanticDefaultsDescriptor& descriptor);
std::string RenderSequenceIdentitySemanticEvidenceJson(
    std::string_view dialect_id,
    std::string_view release_profile,
    const SequenceIdentitySemanticDescriptor& descriptor);
std::string RenderIdentifierNameResolutionSemanticEvidenceJson(
    std::string_view dialect_id,
    std::string_view release_profile,
    const IdentifierNameResolutionSemanticDescriptor& descriptor);
std::string RenderScalarExpressionSemanticEvidenceJson(
    std::string_view dialect_id,
    std::string_view release_profile,
    const ScalarExpressionSemanticDescriptor& descriptor);
std::string RenderDmlMutationSemanticEvidenceJson(
    std::string_view dialect_id,
    std::string_view release_profile,
    const DmlMutationSemanticDescriptor& descriptor);
std::string RenderTransactionSessionSemanticEvidenceJson(
    std::string_view dialect_id,
    std::string_view release_profile,
    const TransactionSessionSemanticDescriptor& descriptor);
std::string RenderTemporarySessionObjectSemanticEvidenceJson(
    std::string_view dialect_id,
    std::string_view release_profile,
    const TemporarySessionObjectSemanticDescriptor& descriptor);
std::string EnterpriseReadinessEvidenceJson();
std::string ProceduralBodySourceRetentionEvidenceJson(
    const ProceduralSourceRetentionMetadata& metadata);
std::string ProceduralFunctionalEncodingEvidenceJson(
    std::size_t source_span_count,
    bool cst_materialized,
    bool ast_materialized,
    bool bound_ast_materialized,
    ProceduralFunctionalEncodingSpanMetadata span_metadata);

} // namespace scratchbird::parser::firebird::evidence
