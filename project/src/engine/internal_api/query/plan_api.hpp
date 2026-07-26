// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "catalog/sys_information_projection.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

enum class RelationalPackageRoot : std::uint8_t {
  kQueryExecute = 1,
};

enum class RelationalDagNodeKind : std::uint8_t {
  kScan = 1,
  kFilter,
  kProject,
  kJoin,
  kAggregate,
  kSort,
  kLimit,
  kWindow,
  kSetOperation,
  kSubquery,
  kCte,
  kRecursiveCte,
  kValues,
  kPivot,
  kUnpivot,
  kMatchRecognize,
  kTableFunctionInvoke,
};

enum class RelationalNullability : std::uint8_t {
  kNonNull = 1,
  kNullable,
  kUnknown,
};

enum class RelationalExpressionKind : std::uint8_t {
  kLiteral = 1,
  kParameter,
  kIdentifier,
  kFunctionCall,
  kUnary,
  kBinary,
  kParenthesized,
};

enum class RelationalLiteralKind : std::uint8_t {
  kNumeric = 1,
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

struct RelationalTypeDescriptor {
  std::uint32_t descriptor_id{0};
  std::string descriptor_uuid;
  std::string type_uuid;
  RelationalNullability nullability{RelationalNullability::kUnknown};
  std::optional<std::string> collation_uuid;
  std::optional<std::string> timezone_profile_id;
  std::optional<std::uint32_t> width;
  std::optional<std::uint32_t> precision;
  std::optional<std::uint32_t> scale;
};

struct RelationalExpressionRecord {
  std::uint32_t expression_id{0};
  RelationalExpressionKind expression_kind{RelationalExpressionKind::kLiteral};
  std::vector<std::uint32_t> child_expression_ids;
  std::uint32_t result_descriptor_id{0};
  std::optional<std::string> function_uuid;
  std::optional<std::string> bound_name_uuid;
  std::optional<RelationalLiteralKind> literal_kind;
  std::optional<std::string> operator_name;
  std::optional<std::string> literal_or_parameter_ref;
};

struct RelationalOutputRecord {
  std::uint32_t output_id{0};
  std::uint32_t relation_node_id{0};
  std::uint32_t expression_id{0};
  std::string output_name_utf8;
  std::uint32_t descriptor_id{0};
  bool visible{true};
  std::uint32_t ordinal{0};
};

struct RelationalValuesRowRecord {
  std::uint32_t row_id{0};
  std::vector<std::uint32_t> expression_ids;
};

struct RelationalDagNode {
  std::uint32_t node_id{0};
  RelationalDagNodeKind node_kind{RelationalDagNodeKind::kValues};
  std::vector<std::uint32_t> input_node_ids;
  std::vector<std::uint32_t> output_descriptor_ids;
  bool shareable{false};
  std::vector<std::uint32_t> values_row_ids;
};

struct TypedRelationalDag {
  std::uint16_t wire_version{1};
  RelationalPackageRoot package_root{RelationalPackageRoot::kQueryExecute};
  std::uint32_t root_node_id{0};
  std::vector<RelationalTypeDescriptor> descriptors;
  std::vector<RelationalExpressionRecord> expressions;
  std::vector<RelationalOutputRecord> outputs;
  std::vector<RelationalValuesRowRecord> values_rows;
  std::vector<RelationalDagNode> nodes;
};

struct RelationalDagLimits {
  std::size_t maximum_nodes{131072};
  std::size_t maximum_depth{256};
  std::size_t maximum_fanout{1024};
  std::size_t maximum_records{524288};
};

struct RelationalDagValidationIssue {
  std::string diagnostic_id;
  std::uint32_t node_id{0};
  std::string field_id;
};

struct RelationalDagValidationResult {
  bool accepted{false};
  std::size_t validated_node_count{0};
  std::size_t maximum_observed_depth{0};
  std::vector<RelationalDagValidationIssue> issues;
};

struct EngineTypedRelationalPlanRequest : EngineApiRequest {
  bool execute{false};
  TypedRelationalDag relational_dag;
};

// QOW-SOURCE-QRY-002-V1
RelationalDagValidationResult ValidateTypedRelationalDag(
    const TypedRelationalDag& dag,
    const RelationalDagLimits& limits = {});

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_QUERY_PLAN_API
struct EngineQueryRelation {
  std::string relation_name;
  std::string descriptor_digest;
  EngineObjectReference source_object;
  std::vector<EngineDescriptor> columns;
  std::vector<EngineRowValue> rows;
};

struct EnginePlanOperationRequest : EngineApiRequest {
  bool execute = false;
  std::string query_operation;
  std::string join_algorithm;
  std::string set_operation = "union_distinct";
  bool set_by_name = false;
  std::vector<EngineQueryRelation> relations;
  std::vector<std::size_t> projected_columns;
  std::size_t left_key_column = 0;
  std::size_t right_key_column = 0;
  std::string left_key_field;
  std::string right_key_field;
  std::size_t group_key_column = 0;
  std::size_t aggregate_value_column = 1;
  std::size_t aggregate_pair_value_column = 2;
  std::string group_key_field;
  std::string aggregate_value_field;
  std::string aggregate_pair_value_field;
  std::string aggregate_function = "sum";
  std::size_t order_column = 0;
  std::string order_field;
  std::string window_function = "row_number";
  std::size_t window_value_column = 0;
  std::string window_value_field;
  std::size_t partition_key_column = 0;
  std::string partition_key_field;
  EngineApiU64 window_n = 1;
  EngineApiU64 limit = 0;
  EngineApiU64 offset = 0;
  bool ascending = true;
  std::vector<SysInformationIparAgentLifecycleSource> ipar_agent_lifecycle;
  std::vector<SysInformationIparMetricCounterSource> ipar_metric_counters;
  std::vector<SysInformationIparTelemetryControlSource> ipar_telemetry_controls;
  std::vector<SysInformationIparSlowPathReasonSource> ipar_slow_path_reasons;
};
struct EnginePlanOperationResult : EngineApiResult {
  std::string plan_kind;
  EngineApiU64 output_row_count = 0;
};
EnginePlanOperationResult EnginePlanOperation(const EnginePlanOperationRequest& request);

}  // namespace scratchbird::engine::internal_api
