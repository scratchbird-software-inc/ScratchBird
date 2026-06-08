// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dispatch/function_dispatch.hpp"
#include "query/projection_api.hpp"
#include "registry/function_seed_registry.hpp"
#include "sblr/sblr_dispatch.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace functions = scratchbird::engine::functions;
namespace sblr = scratchbird::engine::sblr;
using sblr::SblrResult;
using sblr::SblrStatusCode;
using sblr::SblrValue;
using sblr::SblrValuePayloadKind;

constexpr const char* kSessionUuid = "019f5200-0000-7000-8000-000000000002";
constexpr const char* kPrincipalUuid = "019f5200-0000-7000-8000-000000000003";
constexpr std::uint64_t kLocalTransactionId = 52052;

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

SblrValue BoolValue(bool input) {
  SblrValue value;
  value.descriptor_id = "boolean";
  value.payload_kind = SblrValuePayloadKind::boolean;
  value.has_int64_value = true;
  value.int64_value = input ? 1 : 0;
  value.encoded_value = input ? "true" : "false";
  value.is_null = false;
  return value;
}

SblrValue IntValue(std::int64_t input) {
  SblrValue value;
  value.descriptor_id = "int64";
  value.payload_kind = SblrValuePayloadKind::signed_integer;
  value.has_int64_value = true;
  value.int64_value = input;
  value.encoded_value = std::to_string(input);
  value.is_null = false;
  return value;
}

SblrValue NullValue(std::string descriptor) {
  SblrValue value;
  value.descriptor_id = std::move(descriptor);
  value.payload_kind = SblrValuePayloadKind::none;
  value.is_null = true;
  return value;
}

functions::FunctionArgument Arg(std::string name, SblrValue value) {
  return functions::FunctionArgument{std::move(name), std::move(value)};
}

sblr::SblrExecutionContext BaseSblrContext() {
  sblr::SblrExecutionContext context;
  context.session_uuid = kSessionUuid;
  context.user_uuid = kPrincipalUuid;
  context.application_name = "sbsfc052-json-document-construction-table-aggregate";
  context.security_context_present = true;
  context.transaction_context_present = true;
  context.local_transaction_id = kLocalTransactionId;
  context.snapshot_visible_through_local_transaction_id = kLocalTransactionId;
  return context;
}

SblrResult Run(const functions::FunctionRegistry& registry,
               std::string function_id,
               std::vector<functions::FunctionArgument> arguments = {}) {
  functions::FunctionCallRequest request;
  request.context.function_id = std::move(function_id);
  request.context.security_allowed = true;
  request.context.policy_allowed = true;
  request.context.dependency_available = true;
  request.context.sblr_context = BaseSblrContext();
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    if (arguments[index].name.empty()) arguments[index].name = "arg" + std::to_string(index);
    request.arguments.push_back(std::move(arguments[index]));
  }
  return functions::DispatchFunctionCall(registry, std::move(request)).result;
}

bool ExpectOkScalar(const SblrResult& result, std::string_view case_id) {
  if (!result.ok() || result.scalar_values.size() != 1 ||
      result.mutation_attempted || result.mutation_committed) {
    std::cerr << case_id << ": expected one successful non-mutating scalar result"
              << "; status=" << static_cast<int>(result.status)
              << "; scalar_count=" << result.scalar_values.size() << "\n";
    for (const auto& diagnostic : result.diagnostics) {
      std::cerr << "  diagnostic=" << diagnostic.diagnostic_id << "\n";
    }
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

bool ExpectNull(std::string_view case_id,
                const SblrResult& result,
                std::string_view descriptor) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (!value.is_null || value.descriptor_id != descriptor) {
    std::cerr << case_id << ": expected null " << descriptor << ", got "
              << value.descriptor_id << " " << value.encoded_value << "\n";
    return false;
  }
  return true;
}

bool ExpectInvalidInput(std::string_view case_id, const SblrResult& result) {
  if (result.ok() || result.status != SblrStatusCode::execution_failed ||
      result.diagnostics.empty() ||
      result.diagnostics.front().diagnostic_id != "SB_DIAG_FUNCTION_INVALID_INPUT" ||
      result.mutation_attempted || result.mutation_committed) {
    std::cerr << case_id << ": expected SB_DIAG_FUNCTION_INVALID_INPUT refusal\n";
    return false;
  }
  return true;
}

sblr::SblrOperationEnvelope ProjectionEnvelope(
    std::string function_id,
    std::vector<api::EngineProjectionFunctionArgument> arguments) {
  auto envelope = sblr::MakeSblrEnvelope("query.evaluate_projection",
                                         "SBLR_QUERY_EVALUATE_PROJECTION",
                                         "SBSFC052-json-document-projection");
  envelope.operands.push_back({"text", "projection_count", "1"});
  envelope.operands.push_back({"text", "projection_0_name", "value"});
  envelope.operands.push_back({"text", "projection_0_expr_kind", "function"});
  envelope.operands.push_back({"text", "projection_0_function_id", std::move(function_id)});
  envelope.operands.push_back({"text", "projection_0_function_arg_count", std::to_string(arguments.size())});
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    const auto prefix = "projection_0_arg_" + std::to_string(index) + "_";
    envelope.operands.push_back({"text", prefix + "name", arguments[index].name});
    envelope.operands.push_back({"text", prefix + "type", arguments[index].type_name});
    envelope.operands.push_back({"text", prefix + "value", arguments[index].encoded_value});
    envelope.operands.push_back({"text", prefix + "is_null", arguments[index].is_null ? "true" : "false"});
  }
  return envelope;
}

api::EngineRequestContext ProjectionContext() {
  api::EngineRequestContext context;
  context.request_id = "sbsfc052-json-document-projection";
  context.session_uuid.canonical = kSessionUuid;
  context.principal_uuid.canonical = kPrincipalUuid;
  context.local_transaction_id = kLocalTransactionId;
  context.snapshot_visible_through_local_transaction_id = kLocalTransactionId;
  context.application_name = "sbsfc052-json-document-construction-table-aggregate";
  context.security_context_present = true;
  return context;
}

bool ExpectProjectionJson(std::string_view case_id,
                          const sblr::SblrDispatchResult& result,
                          std::string_view expected) {
  if (!result.envelope_validated || !result.accepted || !result.dispatched_to_api ||
      !result.api_result.ok || result.api_result.result_shape.rows.size() != 1 ||
      result.api_result.result_shape.rows.front().fields.size() != 1) {
    std::cerr << case_id << ": expected one projected scalar field\n";
    return false;
  }
  const auto& value = result.api_result.result_shape.rows.front().fields.front().second;
  if (value.is_null || value.descriptor.canonical_type_name != "json_document" ||
      value.encoded_value != expected) {
    std::cerr << case_id << ": expected projected json_document " << expected
              << ", got " << value.descriptor.canonical_type_name << " "
              << value.encoded_value << "\n";
    return false;
  }
  return true;
}

}  // namespace

int main() {
  const auto package = functions::BuildStandardFunctionSeedPackage();
  const auto& registry = package.registry;
  bool ok = true;

  ok = ExpectText(
           "SBSQL-E4C08DADB61A SBSFC052-json-table",
           Run(registry, "sb.json.table",
               {Arg("doc", TextValue("json_document", "{\"items\":[1,2]}")),
                Arg("path", TextValue("character", "$.items")),
                Arg("column_id", TextValue("character", "id column")),
                Arg("passing_limit", TextValue("character", "limit"))}),
           "json_document",
           "{\"function\":\"JSON_TABLE\",\"document_descriptor\":\"json_document\","
           "\"path\":\"$.items\",\"document_bytes\":15,\"document_nonempty\":true,"
           "\"passing_argument_count\":1,\"column_count\":1,\"result\":\"descriptor\"}") && ok;
  ok = ExpectNull("SBSFC052-json-table-null",
                  Run(registry, "sb.json.table",
                      {Arg("doc", NullValue("json_document")),
                       Arg("path", TextValue("character", "$"))}),
                  "json_document") && ok;

  ok = ExpectText("SBSQL-2866302407B6 SBSFC052-array-to-json",
                  Run(registry, "sb.json.array_to_json",
                      {Arg("array", TextValue("json_document", "[1,2]"))}),
                  "json_document", "[1,2]") && ok;
  ok = ExpectText("SBSFC052-array-to-json-scalar",
                  Run(registry, "sb.json.array_to_json",
                      {Arg("value", TextValue("character", "x"))}),
                  "json_document", "[\"x\"]") && ok;
  ok = ExpectText("SBSQL-579AE2ED91B2 SBSFC052-array-to-json-pretty",
                  Run(registry, "sb.json.array_to_json",
                      {Arg("array", TextValue("json_document", "[1,2]")),
                       Arg("pretty", BoolValue(true))}),
                  "json_document", "[\n  1,\n  2\n]") && ok;
  ok = ExpectNull("SBSFC052-array-to-json-null",
                  Run(registry, "sb.json.array_to_json",
                      {Arg("array", NullValue("json_document"))}),
                  "json_document") && ok;

  ok = ExpectText("SBSQL-4DBBCD45F15C SBSFC052-json-object-text-arrays",
                  Run(registry, "sb.json.object_text_array",
                      {Arg("keys", TextValue("json_document", "[\"a\",\"b\"]")),
                       Arg("values", TextValue("json_document", "[\"1\",\"2\"]"))}),
                  "json_document", "{\"a\":\"1\",\"b\":\"2\"}") && ok;
  ok = ExpectText("SBSFC052-json-object-single-array-pairs",
                  Run(registry, "sb.json.object_text_array",
                      {Arg("pairs", TextValue("json_document", "[\"a\",\"1\",\"b\",\"2\"]"))}),
                  "json_document", "{\"a\":\"1\",\"b\":\"2\"}") && ok;
  ok = ExpectInvalidInput("SBSFC052-json-object-odd-array-invalid",
                          Run(registry, "sb.json.object_text_array",
                              {Arg("pairs", TextValue("json_document", "[\"a\",\"1\",\"b\"]"))})) && ok;

  ok = ExpectText("SBSQL-5F35CBE51FA4 SBSFC052-jsonb-agg",
                  Run(registry, "sb.json.jsonb_agg",
                      {Arg("a", IntValue(1)), Arg("b", NullValue("character")),
                       Arg("c", TextValue("character", "x"))}),
                  "json_document", "[1,null,\"x\"]") && ok;
  ok = ExpectText("SBSQL-F9F64D586108 SBSFC052-jsonb-agg-expr",
                  Run(registry, "sb.json.jsonb_agg",
                      {Arg("expr", TextValue("character", "x"))}),
                  "json_document", "[\"x\"]") && ok;
  ok = ExpectText("SBSFC052-jsonb-agg-zero",
                  Run(registry, "sb.json.jsonb_agg", {}),
                  "json_document", "[]") && ok;

  ok = ExpectText("SBSQL-EA3286F7FED5 SBSFC052-row-to-json",
                  Run(registry, "sb.json.row_to_json",
                      {Arg("row", TextValue("json_document", "{\"a\":1,\"b\":2}"))}),
                  "json_document", "{\"a\":1,\"b\":2}") && ok;
  ok = ExpectText("SBSFC052-row-to-json-named-args",
                  Run(registry, "sb.json.row_to_json",
                      {Arg("a", IntValue(1)), Arg("b", TextValue("character", "x"))}),
                  "json_document", "{\"a\":1,\"b\":\"x\"}") && ok;
  ok = ExpectText("SBSQL-1E99FF5633C4 SBSFC052-row-to-json-pretty",
                  Run(registry, "sb.json.row_to_json",
                      {Arg("row", TextValue("json_document", "{\"a\":1,\"b\":2}")),
                       Arg("pretty", BoolValue(true))}),
                  "json_document", "{\n  \"a\": 1,\n  \"b\": 2\n}") && ok;

  ok = ExpectProjectionJson(
           "SBSFC052-array-to-json-projection",
           sblr::DispatchSblrOperation(
               {ProjectionContext(),
                ProjectionEnvelope(
                    "sb.json.array_to_json",
                    {api::EngineProjectionFunctionArgument{
                        "array", "json_document", "[1,2]", false}}),
                api::EngineApiRequest{}}),
           "[1,2]") && ok;

  if (!ok) return EXIT_FAILURE;
  std::cout << "sbsql_sbsfc_052_json_document_construction_table_aggregate_runtime_conformance=passed\n";
  return EXIT_SUCCESS;
}
