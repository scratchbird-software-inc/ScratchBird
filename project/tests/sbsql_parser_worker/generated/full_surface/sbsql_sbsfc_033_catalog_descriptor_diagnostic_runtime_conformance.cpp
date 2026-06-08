// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dispatch/function_dispatch.hpp"
#include "registry/function_seed_registry.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using scratchbird::engine::functions::BuildStandardFunctionSeedPackage;
using scratchbird::engine::functions::DispatchFunctionCall;
using scratchbird::engine::functions::FunctionArgument;
using scratchbird::engine::functions::FunctionCallRequest;
using scratchbird::engine::functions::FunctionRegistry;
using scratchbird::engine::sblr::SblrResult;
using scratchbird::engine::sblr::SblrValue;
using scratchbird::engine::sblr::SblrValuePayloadKind;

constexpr std::string_view kDatabaseUuid = "019f0000-0000-7000-8000-000000033301";
constexpr std::string_view kSchemaUuid = "019f0000-0000-7000-8000-000000033302";
constexpr std::string_view kUserUuid = "019f0000-0000-7000-8000-000000033303";
constexpr std::string_view kRoleUuid = "019f0000-0000-7000-8000-000000033304";
constexpr std::string_view kNodeUuid = "019f0000-0000-7000-8000-000000033305";
constexpr std::string_view kSessionUuid = "019f0000-0000-7000-8000-000000033306";
constexpr std::string_view kTransactionUuid = "019f0000-0000-7000-8000-000000033307";
constexpr std::string_view kStatementUuid = "019f0000-0000-7000-8000-000000033308";
constexpr std::string_view kSecuritySnapshotUuid = "019f0000-0000-7000-8000-000000033309";
constexpr std::string_view kDiagnosticUuid = "019f0000-0000-7000-8000-000000033310";
constexpr std::string_view kParserProfileUuid = "019f0000-0000-7000-8000-000000033311";
constexpr std::string_view kClientProtocolUuid = "019f0000-0000-7000-8000-000000033312";

SblrValue TextValue(std::string descriptor, std::string input) {
  SblrValue value;
  value.descriptor_id = std::move(descriptor);
  value.payload_kind = SblrValuePayloadKind::text;
  value.text_value = std::move(input);
  value.encoded_value = value.text_value;
  value.charset_name = "UTF-8";
  value.collation_name = "unicode_root";
  value.is_null = false;
  return value;
}

SblrResult Run(const FunctionRegistry& registry,
               std::string function_id,
               std::vector<SblrValue> values = {}) {
  FunctionCallRequest request;
  request.context.function_id = std::move(function_id);
  request.context.security_allowed = true;
  request.context.policy_allowed = true;
  request.context.dependency_available = true;
  request.context.sblr_context.database_uuid = std::string(kDatabaseUuid);
  request.context.sblr_context.current_schema_uuid = std::string(kSchemaUuid);
  request.context.sblr_context.user_uuid = std::string(kUserUuid);
  request.context.sblr_context.current_role_uuid = std::string(kRoleUuid);
  request.context.sblr_context.node_uuid = std::string(kNodeUuid);
  request.context.sblr_context.session_uuid = std::string(kSessionUuid);
  request.context.sblr_context.transaction_uuid = std::string(kTransactionUuid);
  request.context.sblr_context.statement_uuid = std::string(kStatementUuid);
  request.context.sblr_context.security_snapshot_uuid = std::string(kSecuritySnapshotUuid);
  request.context.sblr_context.current_diagnostic_uuid = std::string(kDiagnosticUuid);
  request.context.sblr_context.current_diagnostic_id = "SBSFC033_DIAGNOSTIC";
  request.context.sblr_context.current_sqlstate = "42000";
  request.context.sblr_context.local_transaction_id = 33033;
  request.context.sblr_context.transaction_isolation_level = "snapshot";
  request.context.sblr_context.transaction_context_present = true;
  request.context.sblr_context.security_context_present = true;
  request.context.sblr_context.parser_profile_uuid = std::string(kParserProfileUuid);
  request.context.sblr_context.client_protocol_uuid = std::string(kClientProtocolUuid);
  request.context.sblr_context.application_name = "sbsql_sbsfc_033_conformance";
  for (std::size_t i = 0; i < values.size(); ++i) {
    request.arguments.push_back(FunctionArgument{"arg" + std::to_string(i), std::move(values[i])});
  }
  return DispatchFunctionCall(registry, std::move(request)).result;
}

bool ExpectOkScalar(const SblrResult& result, std::string_view case_id) {
  if (!result.ok() || result.scalar_values.size() != 1 ||
      result.mutation_attempted || result.mutation_committed) {
    std::cerr << case_id << ": expected one successful non-mutating scalar result\n";
    return false;
  }
  return true;
}

bool ExpectText(std::string_view case_id,
                const SblrResult& result,
                std::string_view descriptor,
                std::string_view expected) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (value.is_null || value.descriptor_id != descriptor || value.encoded_value != expected) {
    std::cerr << case_id << ": expected " << descriptor << " " << expected
              << ", got " << value.descriptor_id << " " << value.encoded_value << "\n";
    return false;
  }
  return true;
}

bool ExpectUint64(std::string_view case_id, const SblrResult& result, std::uint64_t expected) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (value.is_null || value.descriptor_id != "uint64" || !value.has_uint64_value ||
      value.uint64_value != expected) {
    std::cerr << case_id << ": expected uint64 " << expected << ", got "
              << value.descriptor_id << " " << value.encoded_value << "\n";
    return false;
  }
  return true;
}

bool ExpectInt64(std::string_view case_id, const SblrResult& result, std::int64_t expected) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (value.is_null || value.descriptor_id != "int64" || !value.has_int64_value ||
      value.int64_value != expected) {
    std::cerr << case_id << ": expected int64 " << expected << ", got "
              << value.descriptor_id << " " << value.encoded_value << "\n";
    return false;
  }
  return true;
}

bool ExpectNull(std::string_view case_id, const SblrResult& result, std::string_view descriptor) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (!value.is_null || value.descriptor_id != descriptor) {
    std::cerr << case_id << ": expected NULL descriptor " << descriptor << ", got "
              << value.descriptor_id << "\n";
    return false;
  }
  return true;
}

}  // namespace

int main() {
  const auto package = BuildStandardFunctionSeedPackage();
  const auto& registry = package.registry;
  bool ok = true;

  ok = ExpectText("SBSFC033-catalog-object-owner-bare",
                  Run(registry, "sb.scalar.catalog_object_owner", {}),
                  "uuid", kUserUuid) && ok;
  ok = ExpectText("SBSFC033-catalog-object-owner-uuid",
                  Run(registry, "sb.scalar.catalog_object_owner",
                      {TextValue("uuid", std::string(kSchemaUuid))}),
                  "uuid", kUserUuid) && ok;

  ok = ExpectText("SBSFC033-catalog-object-uuid-bare",
                  Run(registry, "sb.scalar.catalog_object_uuid", {}),
                  "uuid", kDatabaseUuid) && ok;
  ok = ExpectText("SBSFC033-catalog-object-uuid-name-class",
                  Run(registry, "sb.scalar.catalog_object_uuid",
                      {TextValue("character", "current_schema"), TextValue("character", "schema")}),
                  "uuid", kSchemaUuid) && ok;

  ok = ExpectText("SBSFC033-catalog-object-name-bare",
                  Run(registry, "sb.scalar.catalog_object_name", {}),
                  "character", "current_database") && ok;
  ok = ExpectText("SBSFC033-catalog-object-name-uuid",
                  Run(registry, "sb.scalar.catalog_object_name",
                      {TextValue("uuid", std::string(kSchemaUuid))}),
                  "character", "current_schema") && ok;

  ok = ExpectText("SBSFC033-catalog-object-class-bare",
                  Run(registry, "sb.scalar.catalog_object_class", {}),
                  "character", "database") && ok;
  ok = ExpectText("SBSFC033-catalog-object-class-uuid",
                  Run(registry, "sb.scalar.catalog_object_class",
                      {TextValue("uuid", std::string(kRoleUuid))}),
                  "character", "role") && ok;

  ok = ExpectText("SBSFC033-descriptor-snapshot-id",
                  Run(registry, "sb.scalar.descriptor_snapshot_id", {}),
                  "uuid", kSecuritySnapshotUuid) && ok;

  ok = ExpectText(
           "SBSFC033-execution-type-descriptor",
           Run(registry, "sb.scalar.execution_type_descriptor", {}),
           "json_document",
           "{\"descriptor_kind\":\"ExecutionTypeDescriptor\","
           "\"database_uuid\":\"019f0000-0000-7000-8000-000000033301\","
           "\"transaction_context_present\":true,"
           "\"transaction_uuid\":\"019f0000-0000-7000-8000-000000033307\","
           "\"transaction_isolation_level\":\"snapshot\","
           "\"statement_uuid\":\"019f0000-0000-7000-8000-000000033308\","
           "\"user_uuid\":\"019f0000-0000-7000-8000-000000033303\","
           "\"security_context_present\":true,"
           "\"parser_profile_uuid\":\"019f0000-0000-7000-8000-000000033311\","
           "\"client_protocol_uuid\":\"019f0000-0000-7000-8000-000000033312\","
           "\"read_only_mode\":false,\"restricted_open_mode\":false}") && ok;

  ok = ExpectText(
           "SBSFC033-column-descriptor-table-column",
           Run(registry, "sb.scalar.column_descriptor",
               {TextValue("uuid", std::string(kDatabaseUuid)), TextValue("character", "database_uuid")}),
           "json_document",
           "{\"descriptor_kind\":\"column_descriptor\","
           "\"table_uuid\":\"019f0000-0000-7000-8000-000000033301\","
           "\"column_name\":\"database_uuid\","
           "\"descriptor_id\":\"uuid\","
           "\"nullable\":false,"
           "\"source\":\"sblr_execution_context\"}") && ok;
  ok = ExpectText(
           "SBSFC033-column-descriptor-bare",
           Run(registry, "sb.scalar.column_descriptor", {}),
           "json_document",
           "{\"descriptor_kind\":\"column_descriptor_set\","
           "\"table_uuid\":\"019f0000-0000-7000-8000-000000033301\","
           "\"source\":\"sblr_execution_context\","
           "\"column_names\":[\"database_uuid\",\"current_schema_uuid\",\"user_uuid\","
           "\"current_role_uuid\",\"statement_uuid\",\"transaction_uuid\"]}") && ok;

  ok = ExpectText("SBSFC033-index-descriptor-bare",
                  Run(registry, "sb.scalar.index_descriptor", {}),
                  "json_document",
                  "{\"descriptor_kind\":\"index_descriptor\",\"available\":false,"
                  "\"source\":\"catalog_snapshot_unresolved\"}") && ok;
  ok = ExpectNull("SBSFC033-index-descriptor-unknown",
                  Run(registry, "sb.scalar.index_descriptor",
                      {TextValue("uuid", "019f0000-0000-7000-8000-000000033399")}),
                  "json_document") && ok;

  ok = ExpectText("SBSFC033-diagnostic-field-bare",
                  Run(registry, "sb.scalar.diagnostic_field", {}),
                  "character", "SBSFC033_DIAGNOSTIC") && ok;
  ok = ExpectText("SBSFC033-diagnostic-field-name",
                  Run(registry, "sb.scalar.diagnostic_field", {TextValue("character", "sqlstate")}),
                  "character", "42000") && ok;
  ok = ExpectUint64("SBSFC033-diagnostic-count",
                    Run(registry, "sb.scalar.diagnostic_count", {}),
                    1) && ok;
  ok = ExpectInt64("SBSFC033-gdscode",
                   Run(registry, "sb.scalar.gdscode", {}),
                   -1) && ok;
  ok = ExpectNull("SBSFC033-last-error-position",
                  Run(registry, "sb.scalar.last_error_position", {}),
                  "int64") && ok;
  ok = ExpectText("SBSFC033-error-class",
                  Run(registry, "sb.scalar.error_class", {}),
                  "character", "syntax_error_or_access_rule_violation") && ok;

  if (!ok) return EXIT_FAILURE;
  std::cout << "sbsql_sbsfc_033_catalog_descriptor_diagnostic_runtime_conformance=passed\n";
  return EXIT_SUCCESS;
}
