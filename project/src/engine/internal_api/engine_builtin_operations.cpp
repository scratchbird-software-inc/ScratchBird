// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "engine_builtin_operations.hpp"

#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::UuidKind;

Status EngineBuiltinOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::engine};
}

Status EngineBuiltinErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::engine};
}

EngineColumnDescriptor Column(std::string stable_name, CanonicalTypeId type_id, bool nullable = false) {
  EngineColumnDescriptor column;
  column.stable_name = std::move(stable_name);
  column.type_id = type_id;
  column.nullable = nullable;
  return column;
}

EngineResultValue StringValue(std::string value) {
  EngineResultValue result;
  result.kind = EngineResultValueKind::string_value;
  result.string_value = std::move(value);
  return result;
}

EngineResultValue UInt32Value(u32 value) {
  EngineResultValue result;
  result.kind = EngineResultValueKind::uint32_value;
  result.uint32_value = value;
  return result;
}

EngineResultValue BoolValue(bool value) {
  EngineResultValue result;
  result.kind = EngineResultValueKind::boolean_value;
  result.boolean_value = value;
  return result;
}

EngineResultValue UuidValue(TypedUuid value) {
  EngineResultValue result;
  result.kind = EngineResultValueKind::uuid_value;
  result.uuid_value = value;
  return result;
}

EngineResultCell Cell(std::string stable_column_name, CanonicalTypeId type_id, EngineResultValue value) {
  EngineResultCell cell;
  cell.stable_column_name = std::move(stable_column_name);
  cell.type_id = type_id;
  cell.value = std::move(value);
  return cell;
}

bool IsEngineIdentityTypedUuid(const TypedUuid& uuid, UuidKind expected_kind) {
  return uuid.kind == expected_kind && uuid.valid() && scratchbird::core::uuid::IsEngineIdentityUuid(uuid.value);
}

EngineOperationResult ErrorResult(EngineOperationCode operation_code,
                                  Status status,
                                  std::string diagnostic_code,
                                  std::string message_key,
                                  std::string detail = {}) {
  EngineOperationResult result;
  result.status = status;
  result.operation_code = operation_code;
  result.diagnostic = MakeEngineBuiltinOperationDiagnostic(status,
                                                           std::move(diagnostic_code),
                                                           std::move(message_key),
                                                           std::move(detail));
  result.diagnostics.push_back(result.diagnostic);
  return result;
}

}  // namespace

const char* EngineResultValueKindName(EngineResultValueKind kind) {
  switch (kind) {
    case EngineResultValueKind::null_value: return "null_value";
    case EngineResultValueKind::string_value: return "string_value";
    case EngineResultValueKind::uint32_value: return "uint32_value";
    case EngineResultValueKind::boolean_value: return "boolean_value";
    case EngineResultValueKind::uuid_value: return "uuid_value";
    case EngineResultValueKind::unknown: return "unknown";
  }
  return "unknown";
}

BoundEngineOperation MakeShowVersionOperationDescriptor() {
  BoundEngineOperation operation;
  operation.operation_code = EngineOperationCode::show_version;
  operation.authority_class = OperationAuthorityClass::baseline_inspect;
  operation.mutates_state = false;
  operation.requires_engine_security_check = true;
  operation.result_shape = MakeEngineResultShape(
                               EngineResultCardinality::single_row,
                               {Column("product_name", CanonicalTypeId::character),
                                Column("component_name", CanonicalTypeId::character),
                                Column("version_major", CanonicalTypeId::uint32),
                                Column("version_minor", CanonicalTypeId::uint32),
                                Column("version_patch", CanonicalTypeId::uint32),
                                Column("internal_api_major", CanonicalTypeId::uint32),
                                Column("internal_api_minor", CanonicalTypeId::uint32),
                                Column("build_label", CanonicalTypeId::character)})
                         .result_shape;
  return operation;
}

BoundEngineOperation MakeShowDatabaseOperationDescriptor() {
  BoundEngineOperation operation;
  operation.operation_code = EngineOperationCode::show_database;
  operation.authority_class = OperationAuthorityClass::baseline_inspect;
  operation.mutates_state = false;
  operation.requires_engine_security_check = true;
  operation.result_shape = MakeEngineResultShape(
                               EngineResultCardinality::single_row,
                               {Column("database_uuid", CanonicalTypeId::uuid),
                                Column("database_label", CanonicalTypeId::character),
                                Column("cluster_authority_active", CanonicalTypeId::boolean)})
                         .result_shape;
  return operation;
}

BoundEngineOperation MakeShowDatabaseResourcesOperationDescriptor() {
  BoundEngineOperation operation;
  operation.operation_code = EngineOperationCode::show_database_resources;
  operation.authority_class = OperationAuthorityClass::catalog_inspect;
  operation.mutates_state = false;
  operation.requires_engine_security_check = true;
  operation.result_shape = MakeEngineResultShape(
                               EngineResultCardinality::single_row,
                               {Column("seed_pack_name", CanonicalTypeId::character),
                                Column("seed_pack_version", CanonicalTypeId::character),
                                Column("seed_pack_hash", CanonicalTypeId::character),
                                Column("active", CanonicalTypeId::boolean),
                                Column("minimal_bootstrap", CanonicalTypeId::boolean),
                                Column("resource_bundle_records", CanonicalTypeId::uint32),
                                Column("resource_artifact_records", CanonicalTypeId::uint32),
                                Column("charset_records", CanonicalTypeId::uint32),
                                Column("charset_alias_records", CanonicalTypeId::uint32),
                                Column("charset_mapping_artifacts", CanonicalTypeId::uint32),
                                Column("collation_records", CanonicalTypeId::uint32),
                                Column("collation_tailoring_records", CanonicalTypeId::uint32),
                                Column("timezone_records", CanonicalTypeId::uint32),
                                Column("timezone_transition_records", CanonicalTypeId::uint32),
                                Column("timezone_leap_second_records", CanonicalTypeId::uint32)})
                         .result_shape;
  return operation;
}

EngineOperationResult ExecuteShowVersionOperation(const EngineContext& context, const EngineVersionInfo& version) {
  EngineApiValidationResult context_result = ValidateEngineContext(context);
  if (!context_result.ok()) {
    EngineOperationResult result;
    result.status = context_result.status;
    result.operation_code = EngineOperationCode::show_version;
    result.diagnostic = context_result.diagnostic;
    result.diagnostics.push_back(context_result.diagnostic);
    return result;
  }

  BoundEngineOperation operation = MakeShowVersionOperationDescriptor();
  EngineApiValidationResult operation_result = ValidateBoundEngineOperation(operation);
  if (!operation_result.ok()) {
    EngineOperationResult result;
    result.status = operation_result.status;
    result.operation_code = EngineOperationCode::show_version;
    result.diagnostic = operation_result.diagnostic;
    result.diagnostics.push_back(operation_result.diagnostic);
    return result;
  }

  EngineResultRow row;
  row.cells.push_back(Cell("product_name", CanonicalTypeId::character, StringValue(version.product_name)));
  row.cells.push_back(Cell("component_name", CanonicalTypeId::character, StringValue(version.component_name)));
  row.cells.push_back(Cell("version_major", CanonicalTypeId::uint32, UInt32Value(version.version_major)));
  row.cells.push_back(Cell("version_minor", CanonicalTypeId::uint32, UInt32Value(version.version_minor)));
  row.cells.push_back(Cell("version_patch", CanonicalTypeId::uint32, UInt32Value(version.version_patch)));
  row.cells.push_back(Cell("internal_api_major", CanonicalTypeId::uint32, UInt32Value(version.internal_api_major)));
  row.cells.push_back(Cell("internal_api_minor", CanonicalTypeId::uint32, UInt32Value(version.internal_api_minor)));
  row.cells.push_back(Cell("build_label", CanonicalTypeId::character, StringValue(version.build_label)));

  EngineOperationResult result;
  result.status = EngineBuiltinOkStatus();
  result.operation_code = EngineOperationCode::show_version;
  result.result_shape = operation.result_shape;
  result.rows.push_back(std::move(row));
  return result;
}

EngineOperationResult ExecuteShowDatabaseOperation(const EngineContext& context, const EngineDatabaseInfo& database) {
  EngineApiValidationResult context_result = ValidateEngineContext(context);
  if (!context_result.ok()) {
    EngineOperationResult result;
    result.status = context_result.status;
    result.operation_code = EngineOperationCode::show_database;
    result.diagnostic = context_result.diagnostic;
    result.diagnostics.push_back(context_result.diagnostic);
    return result;
  }

  if (!IsEngineIdentityTypedUuid(database.database_uuid, UuidKind::database)) {
    return ErrorResult(EngineOperationCode::show_database,
                       EngineBuiltinErrorStatus(),
                       "SB-ENGINE-BUILTIN-DATABASE-UUID-MUST-BE-V7",
                       "engine.builtin.database_uuid_must_be_v7");
  }

  if (database.database_label.empty()) {
    return ErrorResult(EngineOperationCode::show_database,
                       EngineBuiltinErrorStatus(),
                       "SB-ENGINE-BUILTIN-DATABASE-LABEL-REQUIRED",
                       "engine.builtin.database_label_required");
  }

  BoundEngineOperation operation = MakeShowDatabaseOperationDescriptor();
  EngineApiValidationResult operation_result = ValidateBoundEngineOperation(operation);
  if (!operation_result.ok()) {
    EngineOperationResult result;
    result.status = operation_result.status;
    result.operation_code = EngineOperationCode::show_database;
    result.diagnostic = operation_result.diagnostic;
    result.diagnostics.push_back(operation_result.diagnostic);
    return result;
  }

  EngineResultRow row;
  row.cells.push_back(Cell("database_uuid", CanonicalTypeId::uuid, UuidValue(database.database_uuid)));
  row.cells.push_back(Cell("database_label", CanonicalTypeId::character, StringValue(database.database_label)));
  row.cells.push_back(Cell("cluster_authority_active",
                           CanonicalTypeId::boolean,
                           BoolValue(database.cluster_authority_active)));

  EngineOperationResult result;
  result.status = EngineBuiltinOkStatus();
  result.operation_code = EngineOperationCode::show_database;
  result.result_shape = operation.result_shape;
  result.rows.push_back(std::move(row));
  return result;
}

EngineOperationResult ExecuteShowDatabaseResourcesOperation(
    const EngineContext& context,
    const scratchbird::core::resources::ResourceSeedCatalogImage& resources) {
  EngineApiValidationResult context_result = ValidateEngineContext(context);
  if (!context_result.ok()) {
    EngineOperationResult result;
    result.status = context_result.status;
    result.operation_code = EngineOperationCode::show_database_resources;
    result.diagnostic = context_result.diagnostic;
    result.diagnostics.push_back(context_result.diagnostic);
    return result;
  }

  const auto validated = scratchbird::core::resources::ValidateResourceSeedCatalogImage(resources,
                                                                                       resources.minimal_bootstrap);
  if (!validated.ok()) {
    EngineOperationResult result;
    result.status = validated.status;
    result.operation_code = EngineOperationCode::show_database_resources;
    result.diagnostic = validated.diagnostic;
    result.diagnostics.push_back(validated.diagnostic);
    return result;
  }

  BoundEngineOperation operation = MakeShowDatabaseResourcesOperationDescriptor();
  EngineApiValidationResult operation_result = ValidateBoundEngineOperation(operation);
  if (!operation_result.ok()) {
    EngineOperationResult result;
    result.status = operation_result.status;
    result.operation_code = EngineOperationCode::show_database_resources;
    result.diagnostic = operation_result.diagnostic;
    result.diagnostics.push_back(operation_result.diagnostic);
    return result;
  }

  EngineResultRow row;
  row.cells.push_back(Cell("seed_pack_name", CanonicalTypeId::character, StringValue(resources.seed_pack_name)));
  row.cells.push_back(Cell("seed_pack_version", CanonicalTypeId::character, StringValue(resources.seed_pack_version)));
  row.cells.push_back(Cell("seed_pack_hash", CanonicalTypeId::character, StringValue(resources.content_hash)));
  row.cells.push_back(Cell("active", CanonicalTypeId::boolean, BoolValue(resources.active)));
  row.cells.push_back(Cell("minimal_bootstrap", CanonicalTypeId::boolean, BoolValue(resources.minimal_bootstrap)));
  row.cells.push_back(Cell("resource_bundle_records", CanonicalTypeId::uint32, UInt32Value(resources.resource_bundle_records)));
  row.cells.push_back(Cell("resource_artifact_records", CanonicalTypeId::uint32, UInt32Value(resources.resource_artifact_records)));
  row.cells.push_back(Cell("charset_records", CanonicalTypeId::uint32, UInt32Value(resources.charset_records)));
  row.cells.push_back(Cell("charset_alias_records", CanonicalTypeId::uint32, UInt32Value(resources.charset_alias_records)));
  row.cells.push_back(Cell("charset_mapping_artifacts", CanonicalTypeId::uint32, UInt32Value(resources.charset_mapping_artifacts)));
  row.cells.push_back(Cell("collation_records", CanonicalTypeId::uint32, UInt32Value(resources.collation_records)));
  row.cells.push_back(Cell("collation_tailoring_records", CanonicalTypeId::uint32, UInt32Value(resources.collation_tailoring_records)));
  row.cells.push_back(Cell("timezone_records", CanonicalTypeId::uint32, UInt32Value(resources.timezone_records)));
  row.cells.push_back(Cell("timezone_transition_records", CanonicalTypeId::uint32, UInt32Value(resources.timezone_transition_records)));
  row.cells.push_back(Cell("timezone_leap_second_records", CanonicalTypeId::uint32, UInt32Value(resources.timezone_leap_second_records)));

  EngineOperationResult result;
  result.status = EngineBuiltinOkStatus();
  result.operation_code = EngineOperationCode::show_database_resources;
  result.result_shape = operation.result_shape;
  result.rows.push_back(std::move(row));
  return result;
}

DiagnosticRecord MakeEngineBuiltinOperationDiagnostic(Status status,
                                                      std::string diagnostic_code,
                                                      std::string message_key,
                                                      std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", detail});
  }

  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "engine.builtin_operations");
}

}  // namespace scratchbird::engine::internal_api
