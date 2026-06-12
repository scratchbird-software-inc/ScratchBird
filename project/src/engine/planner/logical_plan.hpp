// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::planner {

enum class LogicalPlanNodeKind {
  kCommand,
  kCatalogLookup,
  kTransactionControl,
  kDdlMutation,
  kDmlRead,
  kDmlMutation,
  kNoSqlOperation,
  kManagementOperation,
  kExtensibilityOperation,
  kUnsupported,
};

enum class PhysicalAccessKind {
  kNone,
  kCatalogUuidLookup,
  kTableScan,
  kRowUuidLookup,
  kScalarBtreeLookup,
  kScalarHashLookup,
  kScalarBtreeRange,
  kCoveringIndexScan,
  kBitmapSummaryScan,
  kFullTextProbe,
  kVectorExactSearch,
  kVectorApproximateWithFallback,
  kDocumentPathProbe,
  kGraphTraversalSeed,
  kTimeSeriesAppendPath,
  kJoinNestedLoop,
  kJoinHash,
  kJoinMerge,
  kAggregateGeneric,
  kAggregateHash,
  kSort,
  kTopN,
  kSortThenWindow,
  kCteInline,
  kCteMaterialize,
  kSetOperation,
  kClusterFragmentScan,
  kRemoteNodePushdown,
};


enum class QueryShapeKind {
  kPointLookup,
  kRangeQuery,
  kJoinQuery,
  kAggregateQuery,
  kWindowQuery,
  kCteSubquery,
  kSetOperation,
  kSpecializedWorkload,
};

enum class LogicalPlanSortDirection {
  kUnspecified,
  kAscending,
  kDescending,
};

enum class LogicalPlanNullOrdering {
  kUnspecified,
  kNullsFirst,
  kNullsLast,
};

struct LogicalPlanExpressionEquivalenceFact {
  std::string equivalence_class_id;
  std::vector<std::string> expression_ids;
  bool normalized_sblr_metadata = true;
};

struct LogicalPlanOrderingTerm {
  std::string expression_id;
  LogicalPlanSortDirection direction = LogicalPlanSortDirection::kUnspecified;
  LogicalPlanNullOrdering null_ordering = LogicalPlanNullOrdering::kUnspecified;
  std::vector<std::string> equivalent_expression_ids;
};

struct LogicalPlanOrderingFact {
  std::string fact_id;
  std::vector<LogicalPlanOrderingTerm> terms;
  bool normalized_sblr_metadata = true;
};

struct LogicalPlanGroupingFact {
  std::string fact_id;
  std::vector<std::string> group_expression_ids;
  std::vector<std::string> equivalent_group_expression_ids;
  bool normalized_sblr_metadata = true;
};

struct LogicalPlanWindowFact {
  std::string fact_id;
  std::vector<std::string> partition_expression_ids;
  std::vector<LogicalPlanOrderingTerm> ordering_terms;
  bool normalized_sblr_metadata = true;
};

// SEARCH_KEY: OPCH_LOGICAL_PROPERTY_METADATA
// SEARCH_KEY: OPCH_ORDERING_GROUPING_WINDOW_METADATA
// Logical-plan property facts are normalized SBLR/logical-plan metadata only.
// They may guide optimizer ordering, grouping, window, and equivalence choices,
// but they cannot carry SQL text, execute parser work, or become transaction,
// visibility, security, authorization, or recovery authority.
struct LogicalPlanPropertyMetadata {
  bool metadata_present = false;
  std::vector<LogicalPlanOrderingFact> ordering_facts;
  std::vector<LogicalPlanGroupingFact> grouping_facts;
  std::vector<LogicalPlanWindowFact> window_facts;
  std::vector<LogicalPlanExpressionEquivalenceFact> expression_equivalence_facts;
  bool raw_sql_text_present = false;
  bool parser_execution_authority_claimed = false;
  bool parser_visibility_or_finality_authority_claimed = false;
};

struct QueryShapeEvidence {
  QueryShapeKind shape = QueryShapeKind::kPointLookup;
  bool has_usable_index = false;
  bool has_ordered_access = false;
  bool has_cardinality_estimate = false;
  bool grouping_present = false;
  bool cte_reused = false;
  bool volatile_or_side_effecting = false;
  std::string specialized_kind;
};

struct NormalizedOptimizerPolicyControls {
  std::string plan_profile_id = "plan_profile:default";
  std::string join_search_policy_id = "join_search:default";
  std::string memory_policy_id = "memory_policy:default";
  std::string spill_policy_id = "spill_policy:default";
  std::string parallelism_policy_id = "parallelism:default";
  std::string what_if_policy_id = "what_if:disabled";
  std::vector<std::string> safe_control_ids;
};

// SEARCH_KEY: OPCH_ENGINE_BOUNDARY_PARSER_SAFE_CONTROLS
// Optimizer policy reaches the engine only as normalized SBLR/API/logical-plan
// metadata. SQL text, parser execution authority, and reference/legacy authority
// claims are rejected by the optimizer request boundary.
struct OptimizerPolicyMetadata {
  bool optimizer_policy_metadata_present = false;
  std::string policy_source_kind;
  std::uint64_t policy_epoch = 0;
  NormalizedOptimizerPolicyControls normalized_controls;
  std::vector<std::string> safe_control_ids;
  bool raw_sql_text_present = false;
  bool parser_execution_authority_claimed = false;
  bool parser_session_directives_unbound = false;
  bool reference_or_legacy_policy_authority_claimed = false;
};

struct LogicalPlanNode {
  LogicalPlanNodeKind kind = LogicalPlanNodeKind::kUnsupported;
  PhysicalAccessKind access_kind = PhysicalAccessKind::kNone;
  std::string operation_id;
  std::string stable_name;
  std::vector<std::string> required_descriptors;
  std::vector<std::string> required_object_uuids;
  std::vector<std::string> diagnostics;
};

struct LogicalPlan {
  bool ok = false;
  std::string plan_id;
  OptimizerPolicyMetadata optimizer_policy;
  LogicalPlanPropertyMetadata property_metadata;
  std::vector<LogicalPlanNode> nodes;
  std::vector<std::string> diagnostics;
};

const char* LogicalPlanNodeKindName(LogicalPlanNodeKind kind);
const char* PhysicalAccessKindName(PhysicalAccessKind kind);
const char* QueryShapeKindName(QueryShapeKind kind);
const char* LogicalPlanSortDirectionName(LogicalPlanSortDirection direction);
const char* LogicalPlanNullOrderingName(LogicalPlanNullOrdering null_ordering);
bool LogicalPlanPropertyMetadataSafe(const LogicalPlanPropertyMetadata& metadata);
LogicalPlanNode MakeLogicalPlanNode(LogicalPlanNodeKind kind,
                                    PhysicalAccessKind access_kind,
                                    std::string operation_id,
                                    std::string stable_name);
std::string SerializeLogicalPlanToJson(const LogicalPlan& plan);
LogicalPlan BuildQueryShapePlan(const QueryShapeEvidence& evidence);

}  // namespace scratchbird::engine::planner
