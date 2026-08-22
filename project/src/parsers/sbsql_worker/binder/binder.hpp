// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "ast/ast.hpp"
#include "common/common.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace scratchbird::parser::sbsql {

enum class BoundNullability {
  kNonNull,
  kNullable,
  kUnknown,
};

enum class NativeCatalogRelationResolutionState : std::uint8_t {
  kUnresolved = 0,
  kBound = 1,
};

struct BoundWidthPrecisionScale {
  std::optional<std::uint32_t> width;
  std::optional<std::uint32_t> precision;
  std::optional<std::uint32_t> scale;
};

struct NativeDescriptorBindingInput {
  std::uint32_t descriptor_id{0};
  std::string descriptor_uuid;
  std::string type_uuid;
  BoundNullability nullability{BoundNullability::kUnknown};
  std::optional<std::string> collation_uuid;
  std::optional<std::string> timezone_profile_id;
  BoundWidthPrecisionScale width_precision_scale;
  // Engine-projected canonical type evidence used by closed model profiles.
  // Names never replace descriptor/type UUID authority.
  std::string canonical_type_name;
  std::string element_profile;
};

struct NativeExpressionBindingInput {
  std::uint32_t expression_id{0};
  std::uint32_t descriptor_id{0};
  std::optional<std::string> function_uuid;
  std::optional<std::string> bound_name_uuid;
  std::uint64_t structural_literal_occurrence_id{0};
  std::uint64_t structural_parameter_occurrence_id{0};
  std::uint64_t structural_variable_occurrence_id{0};
};

struct NativeOutputBindingInput {
  std::uint32_t output_id{0};
  std::uint32_t expression_id{0};
  std::string output_name_utf8;
  std::uint32_t descriptor_id{0};
  bool visible{true};
  std::uint32_t ordinal{0};
  std::uint32_t relation_id{0};
};

struct NativeRelationBindingInput {
  std::uint32_t relation_id{0};
  std::string semantic_variant_id;
};

struct NativeWindowFunctionBindingInput {
  std::uint32_t invocation_id{0};
  std::uint32_t function_expression_id{0};
  std::uint16_t abi_version{0};
  std::string builtin_id;
  std::string function_uuid;
  bool executable{false};
  std::uint32_t result_descriptor_id{0};
};

struct NativeCatalogColumnBindingInput {
  std::uint32_t ordinal{0};
  std::string column_uuid;
  std::uint32_t descriptor_id{0};
  std::string canonical_name_key;
};

struct NativeCatalogRelationBindingInput {
  std::uint32_t source_id{0};
  NativeCatalogRelationResolutionState resolution_state{
      NativeCatalogRelationResolutionState::kUnresolved};
  std::string object_uuid;
  std::string resolved_object_type;
  std::string resolved_schema_uuid;
  std::optional<std::string> parent_object_uuid;
  std::uint64_t catalog_generation_id{0};
  std::uint64_t security_epoch{0};
  std::uint64_t resource_epoch{0};
  std::vector<NativeCatalogColumnBindingInput> columns;
  // Spatial profile identity is supplied by the authenticated catalog
  // description. It is compared with every independently resolved operation
  // CRS and never inferred by the parser or provider.
  std::optional<std::string> spatial_crs_uuid;
  std::uint64_t spatial_crs_generation{0};
};

// Exact, read-only statement authority copied from EngineRequestContext at the
// engine-to-parser boundary.  The binder compares against these values; it
// never creates, selects, resolves, or repairs MGA authority.
struct NativeRelationalEngineStatementAuthority {
  std::string statement_uuid;
  std::string statement_timestamp;
  std::string transaction_uuid;
  std::string statement_snapshot_uuid;
  std::string statement_metadata_snapshot_uuid;
  std::string catalog_epoch_uuid;
  std::uint64_t local_transaction_id{0};
  std::uint64_t snapshot_visible_through_local_transaction_id{0};
};

struct NativeSpatialCrsBindingInput {
  std::string operation_id;
  std::string crs_uuid;
  std::uint64_t crs_generation{0};
};

struct NativeRelationalBindingContext {
  std::string bound_ast_uuid;
  std::string catalog_epoch_uuid;
  std::string security_context_uuid;
  std::string statement_uuid;
  std::string statement_timestamp;
  std::string owning_transaction_uuid;
  std::string statement_snapshot_uuid;
  std::string statement_metadata_snapshot_uuid;
  std::uint64_t local_transaction_id{0};
  std::uint64_t snapshot_visible_through_local_transaction_id{0};
  NativeRelationalEngineStatementAuthority engine_statement_authority;
  std::vector<NativeDescriptorBindingInput> descriptors;
  std::vector<NativeExpressionBindingInput> expressions;
  std::vector<NativeOutputBindingInput> outputs;
  std::vector<NativeRelationBindingInput> relations;
  std::vector<NativeWindowFunctionBindingInput> window_functions;
  std::vector<NativeCatalogRelationBindingInput> catalog_relations;
  std::string search_analyzer_uuid;
  std::uint64_t search_analyzer_generation{0};
  std::vector<NativeSpatialCrsBindingInput> spatial_crs_bindings;
};

struct BoundDescriptorAstRecord {
  std::uint32_t descriptor_id{0};
  std::string descriptor_uuid;
  std::string type_uuid;
  BoundNullability nullability{BoundNullability::kUnknown};
  std::optional<std::string> collation_uuid;
  std::optional<std::string> timezone_profile_id;
  BoundWidthPrecisionScale width_precision_scale;
  std::string canonical_type_name;
  std::string element_profile;
};

struct BoundExpressionAstRecord {
  std::uint32_t expression_id{0};
  NativeExpressionAstKind expression_kind{NativeExpressionAstKind::kLiteral};
  std::optional<NativeLiteralAstKind> literal_kind;
  std::vector<std::uint32_t> child_expression_ids;
  std::uint32_t result_descriptor_id{0};
  std::optional<std::string> bound_function_uuid;
  std::optional<std::string> bound_name_uuid;
  std::optional<std::string> canonical_operator_name;
  std::optional<std::string> literal_or_parameter_ref;
  std::uint64_t structural_literal_occurrence_id{0};
  std::uint64_t structural_parameter_occurrence_id{0};
  std::uint64_t structural_variable_occurrence_id{0};
};

struct BoundValuesRowAstRecord {
  std::uint32_t row_id{0};
  std::vector<std::uint32_t> expression_ids;
};

struct BoundOutputAstRecord {
  std::uint32_t output_id{0};
  std::uint32_t relation_id{0};
  std::uint32_t expression_id{0};
  std::string output_name_utf8;
  std::uint32_t descriptor_id{0};
  bool visible{true};
  std::uint32_t ordinal{0};
};

struct BoundGroupingSetAstRecord {
  std::uint32_t relation_id{0};
  std::uint32_t ordinal{0};
  std::vector<std::uint32_t> expression_ids;
};

struct BoundOrderingAstTerm {
  std::uint32_t expression_id{0};
  NativeSortDirection direction{NativeSortDirection::kAscending};
  NativeNullPlacement null_placement{NativeNullPlacement::kNullsLast};
};

struct BoundWindowFrameBoundAstRecord {
  NativeWindowFrameBoundKind bound_kind{
      NativeWindowFrameBoundKind::kCurrentRow};
  std::optional<std::uint32_t> offset_expression_id;
};

struct BoundWindowDefinitionAstRecord {
  std::uint32_t window_id{0};
  std::optional<std::string> canonical_name_key;
  std::optional<std::uint32_t> inherited_window_id;
  std::vector<std::uint32_t> partition_expression_ids;
  std::vector<BoundOrderingAstTerm> ordering_terms;
  std::optional<NativeWindowFrameUnit> frame_unit;
  std::optional<BoundWindowFrameBoundAstRecord> frame_start;
  std::optional<BoundWindowFrameBoundAstRecord> frame_end;
  NativeWindowFrameExclusion exclusion{
      NativeWindowFrameExclusion::kNoOthers};
};

struct BoundWindowInvocationAstRecord {
  std::uint32_t invocation_id{0};
  std::uint32_t function_expression_id{0};
  std::uint32_t window_definition_id{0};
  std::optional<std::string> output_name_utf8;
  std::uint16_t function_abi_version{0};
  std::string builtin_id;
  std::string bound_function_uuid;
  std::uint32_t result_descriptor_id{0};
  std::vector<std::uint32_t> argument_expression_ids;
};

struct BoundRelationAstRecord {
  std::uint32_t relation_id{0};
  NativeRelationAstKind relation_kind{NativeRelationAstKind::kValues};
  NativeAggregateGroupingForm aggregate_grouping_form{
      NativeAggregateGroupingForm::kNone};
  NativeAggregateProjectionForm aggregate_projection_form{
      NativeAggregateProjectionForm::kNone};
  std::vector<std::uint32_t> input_relation_ids;
  std::vector<std::uint32_t> values_row_ids;
  std::vector<std::uint32_t> output_expression_ids;
  std::vector<std::uint32_t> grouping_key_expression_ids;
  std::vector<std::uint32_t> aggregate_expression_ids;
  std::vector<std::uint32_t> predicate_expression_ids;
  std::vector<std::uint32_t> limit_expression_ids;
  std::vector<std::uint32_t> window_invocation_ids;
  std::vector<BoundOrderingAstTerm> ordering_terms;
  std::vector<std::uint32_t> bound_expression_ids;
  std::string semantic_variant_id;
  std::optional<std::string> bound_object_uuid;
  bool lateral{false};
};

struct BoundScopeAstRecord {
  std::uint32_t scope_id{0};
  std::optional<std::uint32_t> parent_scope_id;
  std::vector<std::uint32_t> visible_relation_ids;
  std::vector<std::uint32_t> visible_projection_ids;
  std::string catalog_epoch_uuid;
};

struct BoundCatalogColumnAstRecord {
  std::uint32_t ordinal{0};
  std::string column_uuid;
  std::uint32_t descriptor_id{0};
  std::string canonical_name_key;
};

struct BoundCatalogRelationSourceAstRecord {
  std::uint32_t source_id{0};
  NativeRelationSourceAstKind source_kind{
      NativeRelationSourceAstKind::kCatalogRelation};
  NativeCatalogRelationResolutionState resolution_state{
      NativeCatalogRelationResolutionState::kUnresolved};
  std::vector<NativeIdentifierAstNode> qualified_name;
  std::optional<NativeIdentifierAstNode> alias;
  bool alias_is_explicit{false};
  std::string model_family_id;
  std::string model_operation_id;
  std::vector<std::string> model_operation_ids;
  std::vector<std::uint32_t> model_operation_expression_ids;
  std::optional<NativeIdentifierAstNode> model_source_alias;
  std::optional<std::uint32_t> model_document_expression_id;
  std::optional<std::uint32_t> model_path_expression_id;
  std::optional<std::uint32_t> model_value_expression_id;
  std::optional<std::uint32_t> model_pattern_expression_id;
  std::optional<std::uint32_t> model_graph_alias_expression_id;
  std::vector<std::uint32_t> model_key_expression_ids;
  std::optional<std::uint32_t> model_time_series_alias_expression_id;
  std::optional<std::uint32_t> model_range_expression_id;
  std::optional<std::uint32_t> model_range_start_expression_id;
  std::optional<std::uint32_t> model_range_end_expression_id;
  std::optional<std::uint32_t> model_interval_expression_id;
  std::optional<std::uint32_t> model_time_input_expression_id;
  std::optional<std::uint32_t> model_bucket_expression_id;
  std::optional<std::uint32_t> model_bucket_interval_expression_id;
  std::optional<std::uint32_t> model_bucket_time_input_expression_id;
  std::optional<std::uint32_t> model_downsample_expression_id;
  std::string model_time_series_aggregate_id;
  std::optional<std::uint32_t> model_vector_alias_expression_id;
  std::optional<std::uint32_t> model_vector_nearest_expression_id;
  std::optional<std::uint32_t> model_vector_query_expression_id;
  std::optional<std::uint32_t> model_vector_metric_expression_id;
  std::optional<std::uint32_t> model_vector_top_k_expression_id;
  std::optional<std::uint32_t> model_vector_filter_expression_id;
  std::optional<std::uint32_t> model_vector_metadata_predicate_expression_id;
  std::optional<std::uint32_t> model_vector_metadata_column_expression_id;
  std::optional<std::uint32_t> model_vector_metadata_value_expression_id;
  std::optional<NativeIdentifierAstNode> model_vector_result_alias;
  std::string model_vector_metric_id;
  std::optional<std::uint64_t> model_vector_top_k;
  std::optional<std::uint32_t> model_search_alias_expression_id;
  std::optional<std::uint32_t> model_search_match_expression_id;
  std::optional<std::uint32_t> model_search_query_expression_id;
  std::optional<std::uint32_t> model_search_text_expression_id;
  std::optional<std::uint32_t> model_search_edit_expression_id;
  std::optional<std::uint32_t> model_search_analyzer_expression_id;
  std::optional<std::uint32_t> model_search_top_k_expression_id;
  std::optional<std::uint32_t> model_search_filter_expression_id;
  std::optional<std::uint32_t> model_search_category_predicate_expression_id;
  std::optional<std::uint32_t> model_search_category_column_expression_id;
  std::optional<std::uint32_t> model_search_category_value_expression_id;
  std::optional<NativeIdentifierAstNode> model_search_result_alias;
  std::vector<NativeIdentifierAstNode> model_search_analyzer_name;
  std::string model_search_query_kind;
  std::optional<std::uint64_t> model_search_top_k;
  std::string model_search_analyzer_uuid;
  std::uint64_t model_search_analyzer_generation{0};
  std::optional<std::uint32_t> model_spatial_alias_expression_id;
  std::optional<std::uint32_t> model_spatial_operation_expression_id;
  std::optional<std::uint32_t> model_spatial_match_expression_id;
  std::optional<std::uint32_t> model_spatial_nearest_expression_id;
  std::optional<std::uint32_t> model_spatial_query_expression_id;
  std::vector<std::uint32_t> model_spatial_query_expression_ids;
  std::optional<std::uint32_t> model_spatial_predicate_expression_id;
  std::optional<std::uint32_t> model_spatial_crs_expression_id;
  std::vector<std::uint32_t> model_spatial_crs_expression_ids;
  std::vector<NativeIdentifierAstNode> model_spatial_crs_name;
  std::vector<std::vector<NativeIdentifierAstNode>> model_spatial_crs_names;
  std::string model_spatial_predicate_id;
  std::optional<std::uint32_t> model_spatial_top_k_expression_id;
  std::optional<std::uint64_t> model_spatial_top_k;
  std::vector<std::string> model_spatial_crs_uuids;
  std::vector<std::uint64_t> model_spatial_crs_generations;
  std::optional<std::uint32_t> model_columnar_alias_expression_id;
  std::optional<std::uint32_t> model_columnar_operation_expression_id;
  std::optional<std::uint32_t> model_columnar_project_expression_id;
  std::optional<std::uint32_t> model_columnar_filter_expression_id;
  std::optional<std::uint32_t> model_columnar_predicate_expression_id;
  std::vector<std::uint32_t> model_columnar_project_expression_ids;
  std::vector<std::string> model_columnar_project_column_uuids;
  std::string model_graph_direction;
  std::optional<std::uint64_t> model_graph_minimum_depth;
  std::optional<std::uint64_t> model_graph_maximum_depth;
  std::string model_graph_cycle_policy;
  std::string model_comparison_operator;
  bool model_wildcard_path{false};
  SourceRange qualified_name_range;
  SourceRange range;
  std::string object_uuid;
  std::string resolved_object_type;
  std::string resolved_schema_uuid;
  std::optional<std::string> parent_object_uuid;
  std::uint64_t catalog_generation_id{0};
  std::uint64_t security_epoch{0};
  std::uint64_t resource_epoch{0};
  std::vector<BoundCatalogColumnAstRecord> columns;
};

// An ordinary catalog source must not carry parser- or donor-authored model
// identity.  Keep this shared across the AST, binding, and lowering boundaries
// so a source cannot change profile by retaining an otherwise ignored carrier.
template <typename SourceRecord>
[[nodiscard]] inline bool IsExactOrdinaryCatalogSourceProfile(
    const SourceRecord& source) {
  const bool exact_ordinary_alias =
      (!source.alias.has_value() && !source.alias_is_explicit) ||
      (source.alias.has_value() && source.alias_is_explicit &&
       !source.alias->spelling.empty());
  const bool common_model_carriers_are_empty =
      source.source_kind == NativeRelationSourceAstKind::kCatalogRelation &&
      exact_ordinary_alias &&
      source.model_family_id.empty() && source.model_operation_id.empty() &&
      source.model_operation_ids.empty() &&
      source.model_operation_expression_ids.empty() &&
      !source.model_source_alias.has_value() &&
      !source.model_document_expression_id.has_value() &&
      !source.model_path_expression_id.has_value() &&
      !source.model_value_expression_id.has_value() &&
      !source.model_pattern_expression_id.has_value() &&
      !source.model_graph_alias_expression_id.has_value() &&
      source.model_key_expression_ids.empty() &&
      !source.model_time_series_alias_expression_id.has_value() &&
      !source.model_range_expression_id.has_value() &&
      !source.model_range_start_expression_id.has_value() &&
      !source.model_range_end_expression_id.has_value() &&
      !source.model_interval_expression_id.has_value() &&
      !source.model_time_input_expression_id.has_value() &&
      !source.model_bucket_expression_id.has_value() &&
      !source.model_bucket_interval_expression_id.has_value() &&
      !source.model_bucket_time_input_expression_id.has_value() &&
      !source.model_downsample_expression_id.has_value() &&
      source.model_time_series_aggregate_id.empty() &&
      !source.model_vector_alias_expression_id.has_value() &&
      !source.model_vector_nearest_expression_id.has_value() &&
      !source.model_vector_query_expression_id.has_value() &&
      !source.model_vector_metric_expression_id.has_value() &&
      !source.model_vector_top_k_expression_id.has_value() &&
      !source.model_vector_filter_expression_id.has_value() &&
      !source.model_vector_metadata_predicate_expression_id.has_value() &&
      !source.model_vector_metadata_column_expression_id.has_value() &&
      !source.model_vector_metadata_value_expression_id.has_value() &&
      !source.model_vector_result_alias.has_value() &&
      source.model_vector_metric_id.empty() &&
      !source.model_vector_top_k.has_value() &&
      !source.model_search_alias_expression_id.has_value() &&
      !source.model_search_match_expression_id.has_value() &&
      !source.model_search_query_expression_id.has_value() &&
      !source.model_search_text_expression_id.has_value() &&
      !source.model_search_edit_expression_id.has_value() &&
      !source.model_search_analyzer_expression_id.has_value() &&
      !source.model_search_top_k_expression_id.has_value() &&
      !source.model_search_filter_expression_id.has_value() &&
      !source.model_search_category_predicate_expression_id.has_value() &&
      !source.model_search_category_column_expression_id.has_value() &&
      !source.model_search_category_value_expression_id.has_value() &&
      !source.model_search_result_alias.has_value() &&
      source.model_search_analyzer_name.empty() &&
      source.model_search_query_kind.empty() &&
      !source.model_search_top_k.has_value() &&
      !source.model_spatial_alias_expression_id.has_value() &&
      !source.model_spatial_operation_expression_id.has_value() &&
      !source.model_spatial_match_expression_id.has_value() &&
      !source.model_spatial_nearest_expression_id.has_value() &&
      !source.model_spatial_query_expression_id.has_value() &&
      source.model_spatial_query_expression_ids.empty() &&
      !source.model_spatial_predicate_expression_id.has_value() &&
      !source.model_spatial_crs_expression_id.has_value() &&
      source.model_spatial_crs_expression_ids.empty() &&
      source.model_spatial_crs_name.empty() &&
      source.model_spatial_crs_names.empty() &&
      source.model_spatial_predicate_id.empty() &&
      !source.model_spatial_top_k_expression_id.has_value() &&
      !source.model_spatial_top_k.has_value() &&
      !source.model_columnar_alias_expression_id.has_value() &&
      !source.model_columnar_operation_expression_id.has_value() &&
      !source.model_columnar_project_expression_id.has_value() &&
      !source.model_columnar_filter_expression_id.has_value() &&
      !source.model_columnar_predicate_expression_id.has_value() &&
      source.model_columnar_project_expression_ids.empty() &&
      source.model_graph_direction.empty() &&
      !source.model_graph_minimum_depth.has_value() &&
      !source.model_graph_maximum_depth.has_value() &&
      source.model_graph_cycle_policy.empty() &&
      source.model_comparison_operator.empty() && !source.model_wildcard_path;
  if (!common_model_carriers_are_empty) return false;
  if constexpr (requires { source.model_columnar_project_names; }) {
    if (!source.model_columnar_project_names.empty()) return false;
  }
  if constexpr (requires { source.model_columnar_project_column_uuids; }) {
    if (!source.model_columnar_project_column_uuids.empty()) return false;
  }
  if constexpr (requires { source.model_search_analyzer_uuid; }) {
    if (!source.model_search_analyzer_uuid.empty() ||
        source.model_search_analyzer_generation != 0) {
      return false;
    }
  }
  if constexpr (requires { source.model_spatial_crs_uuids; }) {
    if (!source.model_spatial_crs_uuids.empty() ||
        !source.model_spatial_crs_generations.empty()) {
      return false;
    }
  }
  return true;
}

struct BoundNativeRelationalDocument {
  bool bound{false};
  std::string bound_ast_uuid;
  std::string security_context_uuid;
  std::string statement_uuid;
  std::string statement_timestamp;
  std::string owning_transaction_uuid;
  std::string statement_snapshot_uuid;
  std::string statement_metadata_snapshot_uuid;
  std::uint64_t local_transaction_id{0};
  std::uint64_t snapshot_visible_through_local_transaction_id{0};
  std::uint32_t root_relation_id{0};
  std::uint32_t root_scope_id{0};
  std::vector<BoundDescriptorAstRecord> descriptors;
  std::vector<BoundExpressionAstRecord> expressions;
  std::vector<BoundValuesRowAstRecord> values_rows;
  std::vector<BoundGroupingSetAstRecord> grouping_sets;
  std::vector<BoundWindowDefinitionAstRecord> window_definitions;
  std::vector<BoundWindowInvocationAstRecord> window_invocations;
  std::vector<BoundOutputAstRecord> outputs;
  std::vector<BoundRelationAstRecord> relations;
  std::vector<BoundCatalogRelationSourceAstRecord> catalog_relation_sources;
  std::vector<BoundScopeAstRecord> scopes;
  MessageVectorSet messages;
};

struct BoundStatement {
  bool bound{false};
  bool native_relational_recognized{false};
  std::uint32_t bound_ast_format_version{1};
  std::uint32_t parser_api_major{0};
  std::uint32_t protocol_version{0};
  std::uint64_t catalog_epoch{0};
  std::uint64_t security_policy_epoch{0};
  std::uint64_t descriptor_epoch{0};
  std::string parser_package_uuid;
  std::string parser_package_version;
  std::string parser_build_id;
  std::string command_registry_snapshot_uuid;
  std::string session_uuid;
  std::string connection_uuid;
  std::string database_uuid;
  std::string dialect_profile_uuid;
  std::string registry_family;
  std::string operation_family;
  std::string command_family;
  std::string surface_key;
  std::string sblr_operation_key;
  std::string statement_surface_id;
  std::string statement_surface_name;
  std::string statement_parser_category;
  std::string parser_handler_key;
  std::string binding_contract_key;
  std::string admission_contract_key;
  std::string behavior_descriptor_key;
  std::string diagnostic_key;
  std::string name_resolution_authority_key;
  std::string descriptor_authority_key;
  std::string security_authority_key;
  std::string transaction_authority_key;
  std::string transaction_context;
  std::string result_shape_key;
  std::string diagnostic_shape_key;
  std::string resource_contract_key;
  std::string conformance_case_key;
  std::string trace_key;
  std::string edition_gate_result;
  std::string profile_gate_result;
  std::string granted_scope;
  std::uint64_t statement_hash{0};
  bool requires_name_resolution{false};
  bool requires_descriptor_authority{false};
  bool requires_security_authority{false};
  bool requires_transaction_authority{false};
  bool requires_cluster_profile{false};
  bool exact_refusal_required{false};
  std::vector<std::string> resolved_object_uuids;
  std::vector<std::string> descriptor_refs;
  std::vector<std::string> policy_refs;
  std::vector<std::string> required_rights;
  std::vector<std::string> required_authority_steps;
  BoundNativeRelationalDocument native_relational;
  MessageVectorSet messages;
};

BoundNativeRelationalDocument BindNativeRelationalAst(
    const NativeRelationalAstDocument& ast,
    const NativeRelationalBindingContext& context);

BoundStatement BindAst(const AstDocument& ast,
                       const CstDocument& cst,
                       const ParserConfig& config,
                       const SessionContext& session,
                       const std::vector<std::string>& resolved_object_uuids = {},
                       const NativeRelationalBindingContext* native_binding_context = nullptr);

} // namespace scratchbird::parser::sbsql
