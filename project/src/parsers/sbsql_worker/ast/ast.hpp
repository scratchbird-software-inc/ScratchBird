// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "cst/cst.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::parser::sbsql {

inline bool ExactBoundedGraphPatternV1(const std::string_view pattern) {
  if (pattern == "vertex(*)") return true;
  constexpr std::string_view kPrefix = "vertex(label=";
  if (!pattern.starts_with(kPrefix) || !pattern.ends_with(')') ||
      pattern.size() <= kPrefix.size() + 1) {
    return false;
  }
  const auto label =
      pattern.substr(kPrefix.size(), pattern.size() - kPrefix.size() - 1);
  const auto identifier_start = [](const unsigned char ch) {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
           ch == '_';
  };
  const auto identifier_continue = [&](const unsigned char ch) {
    return identifier_start(ch) || (ch >= '0' && ch <= '9');
  };
  return identifier_start(static_cast<unsigned char>(label.front())) &&
         std::ranges::all_of(label.substr(1), identifier_continue);
}

enum class StatementFamily {
  kUnknown,
  kQuery,
  kInsert,
  kUpdate,
  kDelete,
  kMerge,
  kUpsert,
  kCatalog,
  kShow,
  kSession,
  kTransaction,
  kExecute,
  kCall,
  kValues,
  kSecurity,
  kObservability,
  kRuntimeManagement,
  kStorageManagement,
  kJobsScheduler,
  kArchiveReplication,
  kAcceleration,
  kMultiModel,
  kMigration,
  kBridge,
  kClusterPrivate,
};

enum class NativeRelationalParseStatus {
  kNotRecognized,
  kAccepted,
  kRefused,
};

enum class NativeRelationAstKind {
  kValues,
  kAggregate,
  kFilter,
  kCatalogSource,
  kLimit,
  kProject,
  kSort,
  kJoin,
  kWindow,
  kQualify,
  kMatchRecognize,
  kTableFunctionInvoke,
};

enum class NativeJoinAstKind {
  kNone,
  kCross,
  kInner,
  kLeftOuter,
  kRightOuter,
  kFullOuter,
  kLeftSemi,
  kLeftAnti,
};

enum class NativeSortDirection {
  kAscending,
  kDescending,
};

enum class NativeNullPlacement {
  kNullsFirst,
  kNullsLast,
};

enum class NativeWindowFrameUnit {
  kRows,
  kRange,
  kGroups,
};

enum class NativeWindowFrameBoundKind {
  kUnboundedPreceding,
  kPreceding,
  kCurrentRow,
  kFollowing,
  kUnboundedFollowing,
};

enum class NativeWindowFrameExclusion {
  kNoOthers,
  kCurrentRow,
  kGroup,
  kTies,
};

enum class NativeRelationSourceAstKind {
  kCatalogRelation,
  kDocument,
  kGraph,
  kKeyValue,
  kTimeSeries,
  kVector,
  kSearch,
  kSpatial,
  kColumnar,
};

enum class NativeAggregateGroupingForm {
  kNone,
  kSimple,
  kGroupingSets,
  kRollup,
  kCube,
};

enum class NativeAggregateProjectionForm {
  kNone,
  kKeySumInt128,
  kKeyCountSum,
  kKeysCountSum,
  kKeysCountSumGrouping,
  kGlobalUnary,
};

enum class NativeExpressionAstKind {
  kLiteral,
  kParameter,
  kIdentifier,
  kFunctionCall,
  kUnary,
  kBinary,
  kParenthesized,
  kWildcard,
  kVariable,
};

enum class NativeLiteralAstKind {
  kNumeric,
  kString,
  kBinary,
  kTemporal,
  kUuid,
  kBoolean,
  kNull,
  kDefault,
  kDocument,
  kVector,
  kRegex,
  kRange,
};

enum class NativeTemporalTableAxis {
  kSystemTime,
  kValidTime,
};

enum class NativeTemporalTableForm {
  kUnspecified,
  kAsOf,
  kAll,
  kBetween,
  kFromTo,
};

struct NativeTemporalTableSourceRefusal {
  NativeTemporalTableAxis axis{NativeTemporalTableAxis::kSystemTime};
  NativeTemporalTableForm form{NativeTemporalTableForm::kUnspecified};
  SourceRange range;
};

struct NativeIdentifierAstNode {
  std::string spelling;
  bool quoted{false};
  SourceRange range;
};

struct NativeCatalogRelationSourceAstNode {
  std::uint32_t source_id{0};
  NativeRelationSourceAstKind source_kind{
      NativeRelationSourceAstKind::kCatalogRelation};
  std::vector<NativeIdentifierAstNode> qualified_name;
  std::optional<NativeIdentifierAstNode> alias;
  bool alias_is_explicit{false};
  std::string model_family_id;
  std::string model_operation_id;
  // Ordered semantic-stage operation identity and the parallel parser-owned
  // expression roots.  Multi-operation model legs must retain every entry;
  // model_operation_id is only a single-operation compatibility projection.
  std::vector<std::string> model_operation_ids;
  std::vector<std::uint32_t> model_operation_expression_ids;
  // For an attached model-source transform this is the alias of the
  // preceding base source. `alias` remains the output alias of the complete
  // model table primary.
  std::optional<NativeIdentifierAstNode> model_source_alias;
  std::optional<std::uint32_t> model_document_expression_id;
  std::optional<std::uint32_t> model_path_expression_id;
  std::optional<std::uint32_t> model_value_expression_id;
  std::optional<std::uint32_t> model_pattern_expression_id;
  std::optional<std::uint32_t> model_graph_alias_expression_id;
  // Ordered key/prefix expression identities for the key/value family.
  // Distinct nodes may evaluate to equal TEXT bytes; semantic normalization
  // occurs only after evaluation.
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
  std::optional<std::uint32_t> model_columnar_alias_expression_id;
  std::optional<std::uint32_t> model_columnar_operation_expression_id;
  std::optional<std::uint32_t> model_columnar_project_expression_id;
  std::optional<std::uint32_t> model_columnar_filter_expression_id;
  std::optional<std::uint32_t> model_columnar_predicate_expression_id;
  std::vector<std::uint32_t> model_columnar_project_expression_ids;
  std::vector<std::vector<NativeIdentifierAstNode>>
      model_columnar_project_names;
  std::string model_graph_direction;
  std::optional<std::uint64_t> model_graph_minimum_depth;
  std::optional<std::uint64_t> model_graph_maximum_depth;
  std::string model_graph_cycle_policy;
  std::string model_comparison_operator;
  bool model_wildcard_path{false};
  SourceRange qualified_name_range;
  SourceRange range;
};

// Exact parser-owned description of one public object-resolution request.
// It retains the accepted identifier components and quoting but carries no
// resolved identity; only the authenticated engine registry may supply one.
struct NativeModelObjectResolutionAstRequest {
  std::uint32_t source_id{0};
  std::string model_family_id;
  std::string object_class;
  std::vector<NativeIdentifierAstNode> qualified_name;
  SourceRange range;
};

struct NativeExpressionAstNode {
  std::uint32_t expression_id{0};
  NativeExpressionAstKind expression_kind{NativeExpressionAstKind::kLiteral};
  std::optional<NativeLiteralAstKind> literal_kind;
  std::vector<std::uint32_t> child_expression_ids;
  // Present only for identifier expressions. This prevents raw source
  // rendering (including whitespace around dots) from becoming name-binding
  // authority and preserves quoted-component semantics exactly.
  std::vector<NativeIdentifierAstNode> qualified_identifier;
  std::string spelling;
  std::string operator_name;
  SourceRange range;
  // Parser-assigned, source-order identity for literal descriptor negotiation.
  // It is independent of expression handles and remains stable if expression
  // records are reordered by later canonicalization.
  std::uint64_t structural_literal_occurrence_id{0};
  // Parser-assigned, source-order identity for parameter-set negotiation.
  // This is structural only; it does not choose a slot or descriptor.
  std::uint64_t structural_parameter_occurrence_id{0};
  // Parser-only source-order occurrence. Engine coordination maps it to the
  // authoritative scope/frame descriptor and dense variable ordinal.
  std::uint64_t structural_variable_occurrence_id{0};
};

struct NativeValuesRowAstNode {
  std::uint32_t row_id{0};
  std::vector<std::uint32_t> expression_ids;
  SourceRange range;
};

struct NativeGroupingSetAstNode {
  std::uint32_t relation_id{0};
  std::uint32_t ordinal{0};
  std::vector<std::uint32_t> expression_ids;
  SourceRange range;
};

struct NativeOrderingAstTerm {
  std::uint32_t expression_id{0};
  NativeSortDirection direction{NativeSortDirection::kAscending};
  NativeNullPlacement null_placement{NativeNullPlacement::kNullsLast};
  SourceRange range;
};

struct NativeWindowFrameBoundAstNode {
  NativeWindowFrameBoundKind bound_kind{
      NativeWindowFrameBoundKind::kCurrentRow};
  std::optional<std::uint32_t> offset_expression_id;
  SourceRange range;
};

struct NativeWindowDefinitionAstNode {
  std::uint32_t window_id{0};
  std::optional<NativeIdentifierAstNode> name;
  std::optional<NativeIdentifierAstNode> base_name;
  std::vector<std::uint32_t> partition_expression_ids;
  std::vector<NativeOrderingAstTerm> ordering_terms;
  std::optional<NativeWindowFrameUnit> frame_unit;
  std::optional<NativeWindowFrameBoundAstNode> frame_start;
  std::optional<NativeWindowFrameBoundAstNode> frame_end;
  NativeWindowFrameExclusion exclusion{
      NativeWindowFrameExclusion::kNoOthers};
  SourceRange range;
};

struct NativeWindowInvocationAstNode {
  std::uint32_t invocation_id{0};
  std::uint32_t function_expression_id{0};
  std::uint32_t window_definition_id{0};
  std::optional<NativeIdentifierAstNode> output_alias;
  SourceRange range;
};

enum class NativeRowPatternRowsPerMatch : std::uint8_t {
  kOne = 1,
  kAll,
};

enum class NativeRowPatternAfterMatchSkip : std::uint8_t {
  kPastLastRow = 1,
  kToNextRow,
  kToFirstVariable,
  kToLastVariable,
};

struct NativeRowPatternVariableAstNode {
  NativeIdentifierAstNode name;
  std::uint32_t minimum_occurrences{1};
  std::optional<std::uint32_t> maximum_occurrences;
  bool reluctant{false};
  std::optional<std::uint32_t> define_expression_id;
  bool define_always_true{false};
  SourceRange range;
};

struct NativeRowPatternAstNode {
  std::uint32_t pattern_id{0};
  std::uint32_t relation_id{0};
  std::vector<std::uint32_t> partition_expression_ids;
  std::vector<NativeOrderingAstTerm> ordering_terms;
  std::vector<NativeRowPatternVariableAstNode> variables;
  std::vector<std::uint32_t> measure_expression_ids;
  NativeRowPatternRowsPerMatch rows_per_match{
      NativeRowPatternRowsPerMatch::kOne};
  NativeRowPatternAfterMatchSkip after_match_skip{
      NativeRowPatternAfterMatchSkip::kToNextRow};
  std::optional<NativeIdentifierAstNode> skip_target;
  std::uint32_t maximum_partition_rows{0};
  std::uint32_t maximum_active_states{0};
  std::uint32_t maximum_output_rows{0};
  bool stable_row_identity_tie_break_allowed{false};
  SourceRange range;
};

struct NativeRelationAstNode {
  std::uint32_t relation_id{0};
  NativeRelationAstKind relation_kind{NativeRelationAstKind::kValues};
  NativeAggregateGroupingForm aggregate_grouping_form{
      NativeAggregateGroupingForm::kNone};
  NativeAggregateProjectionForm aggregate_projection_form{
      NativeAggregateProjectionForm::kNone};
  NativeJoinAstKind join_kind{NativeJoinAstKind::kNone};
  std::vector<std::uint32_t> input_relation_ids;
  std::vector<std::uint32_t> relation_source_ids;
  std::vector<std::uint32_t> values_row_ids;
  std::vector<std::uint32_t> output_expression_ids;
  std::vector<std::uint32_t> grouping_key_expression_ids;
  std::vector<std::uint32_t> aggregate_expression_ids;
  std::vector<std::uint32_t> predicate_expression_ids;
  std::vector<std::uint32_t> limit_expression_ids;
  std::vector<NativeIdentifierAstNode> table_function_name;
  std::vector<std::uint32_t> table_function_argument_expression_ids;
  std::vector<std::uint32_t> window_invocation_ids;
  std::vector<NativeOrderingAstTerm> ordering_terms;
  SourceRange range;
};

struct NativeRelationalAstDocument {
  NativeRelationalParseStatus status{NativeRelationalParseStatus::kNotRecognized};
  std::uint32_t root_relation_id{0};
  std::vector<NativeRelationAstNode> relations;
  std::vector<NativeCatalogRelationSourceAstNode> catalog_relation_sources;
  std::vector<NativeModelObjectResolutionAstRequest>
      model_object_resolution_requests;
  std::vector<NativeValuesRowAstNode> values_rows;
  std::vector<NativeGroupingSetAstNode> grouping_sets;
  std::vector<NativeWindowDefinitionAstNode> window_definitions;
  std::vector<NativeWindowInvocationAstNode> window_invocations;
  std::vector<NativeRowPatternAstNode> row_patterns;
  std::vector<NativeExpressionAstNode> expressions;
  std::optional<NativeTemporalTableSourceRefusal> temporal_table_source_refusal;
  MessageVectorSet messages;

  [[nodiscard]] bool recognized() const {
    return status != NativeRelationalParseStatus::kNotRecognized;
  }

  [[nodiscard]] bool accepted() const {
    return status == NativeRelationalParseStatus::kAccepted && !messages.has_errors();
  }
};

struct AstDocument {
  StatementFamily family{StatementFamily::kUnknown};
  std::string registry_family;
  std::string operation_family;
  std::string statement_kind;
  std::string statement_surface_id;
  std::string statement_surface_name;
  std::string statement_parser_category;
  std::string parser_handler_key;
  std::string statement_binding_contract_key;
  std::string statement_admission_contract_key;
  std::string statement_behavior_descriptor_key;
  std::string diagnostic_key;
  std::string source_text;
  std::string canonical_render;
  std::uint64_t source_hash{0};
  std::size_t root_node_index{0};
  std::size_t statement_token_begin{0};
  std::size_t statement_token_end{0};
  bool requires_name_resolution{false};
  bool produces_sblr{false};
  bool exact_refusal_required{false};
  bool requires_cluster_profile{false};
  struct Node {
    std::string kind;
    std::string text;
    SourceRange range;
    std::size_t token_begin{0};
    std::size_t token_end{0};
    bool source_artifact{false};
    std::vector<std::size_t> children;
  };
  std::vector<Node> nodes;
  NativeRelationalAstDocument native_relational;
  MessageVectorSet messages;
};

NativeRelationalAstDocument ParseNativeRelationalAst(const CstDocument& cst);
AstDocument BuildAst(const CstDocument& cst);
std::string StatementFamilyName(StatementFamily family);
std::string NativeRelationAstKindName(NativeRelationAstKind kind);
std::string NativeRelationSourceAstKindName(NativeRelationSourceAstKind kind);
std::string NativeAggregateGroupingFormName(NativeAggregateGroupingForm form);
std::string NativeAggregateProjectionFormName(
    NativeAggregateProjectionForm form);
std::string NativeWindowFrameUnitName(NativeWindowFrameUnit unit);
std::string NativeWindowFrameBoundKindName(NativeWindowFrameBoundKind kind);
std::string NativeWindowFrameExclusionName(NativeWindowFrameExclusion exclusion);
std::string NativeExpressionAstKindName(NativeExpressionAstKind kind);
std::string NativeLiteralAstKindName(NativeLiteralAstKind kind);

} // namespace scratchbird::parser::sbsql
