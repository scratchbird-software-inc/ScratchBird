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
};

struct NativeExpressionBindingInput {
  std::uint32_t expression_id{0};
  std::uint32_t descriptor_id{0};
  std::optional<std::string> function_uuid;
  std::optional<std::string> bound_name_uuid;
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

struct NativeCatalogColumnBindingInput {
  std::uint32_t ordinal{0};
  std::string column_uuid;
  std::uint32_t descriptor_id{0};
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
};

struct NativeRelationalBindingContext {
  std::string bound_ast_uuid;
  std::string catalog_epoch_uuid;
  std::string security_context_uuid;
  std::vector<NativeDescriptorBindingInput> descriptors;
  std::vector<NativeExpressionBindingInput> expressions;
  std::vector<NativeOutputBindingInput> outputs;
  std::vector<NativeRelationBindingInput> relations;
  std::vector<NativeCatalogRelationBindingInput> catalog_relations;
};

struct BoundDescriptorAstRecord {
  std::uint32_t descriptor_id{0};
  std::string descriptor_uuid;
  std::string type_uuid;
  BoundNullability nullability{BoundNullability::kUnknown};
  std::optional<std::string> collation_uuid;
  std::optional<std::string> timezone_profile_id;
  BoundWidthPrecisionScale width_precision_scale;
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

struct BoundNativeRelationalDocument {
  bool bound{false};
  std::string bound_ast_uuid;
  std::string security_context_uuid;
  std::uint32_t root_relation_id{0};
  std::uint32_t root_scope_id{0};
  std::vector<BoundDescriptorAstRecord> descriptors;
  std::vector<BoundExpressionAstRecord> expressions;
  std::vector<BoundValuesRowAstRecord> values_rows;
  std::vector<BoundGroupingSetAstRecord> grouping_sets;
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
