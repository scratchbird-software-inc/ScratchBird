// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "cst/cst.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace scratchbird::parser::sbsql {

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
};

enum class NativeSortDirection {
  kAscending,
  kDescending,
};

enum class NativeNullPlacement {
  kNullsFirst,
  kNullsLast,
};

enum class NativeRelationSourceAstKind {
  kCatalogRelation,
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
  SourceRange qualified_name_range;
  SourceRange range;
};

struct NativeExpressionAstNode {
  std::uint32_t expression_id{0};
  NativeExpressionAstKind expression_kind{NativeExpressionAstKind::kLiteral};
  std::optional<NativeLiteralAstKind> literal_kind;
  std::vector<std::uint32_t> child_expression_ids;
  std::string spelling;
  std::string operator_name;
  SourceRange range;
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

struct NativeRelationAstNode {
  std::uint32_t relation_id{0};
  NativeRelationAstKind relation_kind{NativeRelationAstKind::kValues};
  NativeAggregateGroupingForm aggregate_grouping_form{
      NativeAggregateGroupingForm::kNone};
  NativeAggregateProjectionForm aggregate_projection_form{
      NativeAggregateProjectionForm::kNone};
  std::vector<std::uint32_t> input_relation_ids;
  std::vector<std::uint32_t> relation_source_ids;
  std::vector<std::uint32_t> values_row_ids;
  std::vector<std::uint32_t> output_expression_ids;
  std::vector<std::uint32_t> grouping_key_expression_ids;
  std::vector<std::uint32_t> aggregate_expression_ids;
  std::vector<std::uint32_t> predicate_expression_ids;
  std::vector<std::uint32_t> limit_expression_ids;
  std::vector<NativeOrderingAstTerm> ordering_terms;
  SourceRange range;
};

struct NativeRelationalAstDocument {
  NativeRelationalParseStatus status{NativeRelationalParseStatus::kNotRecognized};
  std::uint32_t root_relation_id{0};
  std::vector<NativeRelationAstNode> relations;
  std::vector<NativeCatalogRelationSourceAstNode> catalog_relation_sources;
  std::vector<NativeValuesRowAstNode> values_rows;
  std::vector<NativeGroupingSetAstNode> grouping_sets;
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
std::string NativeExpressionAstKindName(NativeExpressionAstKind kind);
std::string NativeLiteralAstKindName(NativeLiteralAstKind kind);

} // namespace scratchbird::parser::sbsql
