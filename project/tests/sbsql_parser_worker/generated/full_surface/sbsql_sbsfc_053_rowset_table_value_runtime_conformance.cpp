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

constexpr const char* kSessionUuid = "019f5300-0000-7000-8000-000000000002";
constexpr const char* kPrincipalUuid = "019f5300-0000-7000-8000-000000000003";
constexpr std::uint64_t kLocalTransactionId = 53053;

constexpr const char* kEmptyRowset =
    "{\"kind\":\"rowset\",\"row_shape\":[],\"rows\":[],\"row_count\":0}";
constexpr const char* kTwoRowRowset =
    "{\"kind\":\"rowset\",\"row_shape\":[],\"rows\":[[1],[2]],\"row_count\":2}";
constexpr const char* kEmptyTableValue =
    "{\"kind\":\"table_value\",\"row_shape\":[],\"rows\":[],\"row_count\":0}";

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

functions::FunctionArgument Arg(std::string name, SblrValue value) {
  return functions::FunctionArgument{std::move(name), std::move(value)};
}

sblr::SblrExecutionContext BaseSblrContext() {
  sblr::SblrExecutionContext context;
  context.session_uuid = kSessionUuid;
  context.user_uuid = kPrincipalUuid;
  context.application_name = "sbsfc053-rowset-table-value";
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

bool ExpectInt(std::string_view case_id,
               const SblrResult& result,
               std::int64_t expected) {
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
                                         "SBSFC053-rowset-table-value-projection");
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
  context.request_id = "sbsfc053-rowset-table-value-projection";
  context.session_uuid.canonical = kSessionUuid;
  context.principal_uuid.canonical = kPrincipalUuid;
  context.local_transaction_id = kLocalTransactionId;
  context.snapshot_visible_through_local_transaction_id = kLocalTransactionId;
  context.application_name = "sbsfc053-rowset-table-value";
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

  ok = ExpectText("SBSQL-957EA3F617A2 SBSFC053-rowset-marker",
                  Run(registry, "sb.rowset.rowset"),
                  "json_document", kEmptyRowset) && ok;
  ok = ExpectText("SBSQL-441883FA4E87 SBSFC053-rowset-new-empty",
                  Run(registry, "sb.rowset.new"),
                  "json_document", kEmptyRowset) && ok;
  ok = ExpectText("SBSQL-A3DADE5255A6 SBSFC053-rowset-new-shape",
                  Run(registry, "sb.rowset.new",
                      {Arg("row_shape", TextValue("json_document",
                                                   "[{\"name\":\"id\",\"type\":\"int64\"}]"))}),
                  "json_document",
                  "{\"kind\":\"rowset\",\"row_shape\":[{\"name\":\"id\",\"type\":\"int64\"}],"
                  "\"rows\":[],\"row_count\":0}") && ok;
  ok = ExpectText("SBSQL-4C3F8279098E SBSFC053-rowset-append-marker",
                  Run(registry, "sb.rowset.append",
                      {Arg("rowset", TextValue("json_document", kEmptyRowset)),
                       Arg("expr", TextValue("character", "alpha"))}),
                  "json_document",
                  "{\"kind\":\"rowset\",\"row_shape\":[],\"rows\":[[\"alpha\"]],\"row_count\":1}") && ok;
  ok = ExpectText("SBSQL-1AFC18FA8618 SBSFC053-rowset-append-exprs",
                  Run(registry, "sb.rowset.append",
                      {Arg("rowset", TextValue("json_document", kEmptyRowset)),
                       Arg("id", IntValue(1)),
                       Arg("name", TextValue("character", "alpha"))}),
                  "json_document",
                  "{\"kind\":\"rowset\",\"row_shape\":[],\"rows\":[[1,\"alpha\"]],\"row_count\":1}") && ok;
  ok = ExpectInt("SBSQL-098E28A1F45B SBSFC053-rowset-size-marker",
                 Run(registry, "sb.rowset.size",
                     {Arg("rowset", TextValue("json_document", kTwoRowRowset))}),
                 2) && ok;
  ok = ExpectInt("SBSQL-50C1BBB6018E SBSFC053-rowset-size-rowset",
                 Run(registry, "sb.rowset.size",
                     {Arg("rowset", TextValue("json_document", kTwoRowRowset))}),
                 2) && ok;
  ok = ExpectText("SBSQL-054E4DC54266 SBSFC053-rowset-to-array-marker",
                  Run(registry, "sb.rowset.to_array",
                      {Arg("rowset", TextValue("json_document", kTwoRowRowset))}),
                  "json_document", "[[1],[2]]") && ok;
  ok = ExpectText("SBSQL-94F61E4D245C SBSFC053-rowset-to-array-rowset",
                  Run(registry, "sb.rowset.to_array",
                      {Arg("rowset", TextValue("json_document", kTwoRowRowset))}),
                  "json_document", "[[1],[2]]") && ok;

  ok = ExpectText("SBSQL-415E89D3266D SBSFC053-table-value-marker",
                  Run(registry, "sb.table_value.value"),
                  "json_document", kEmptyTableValue) && ok;
  ok = ExpectText("SBSQL-425230445B2C SBSFC053-table-value-new-empty",
                  Run(registry, "sb.table_value.new"),
                  "json_document", kEmptyTableValue) && ok;
  ok = ExpectText("SBSQL-8467B84B58DF SBSFC053-table-value-new-shape",
                  Run(registry, "sb.table_value.new",
                      {Arg("row_shape", TextValue("json_document",
                                                   "[{\"name\":\"name\",\"type\":\"character\"}]"))}),
                  "json_document",
                  "{\"kind\":\"table_value\",\"row_shape\":[{\"name\":\"name\",\"type\":\"character\"}],"
                  "\"rows\":[],\"row_count\":0}") && ok;
  ok = ExpectText("SBSQL-BB65E97117E9 SBSFC053-table-value-append-marker",
                  Run(registry, "sb.table_value.append",
                      {Arg("tv", TextValue("json_document", kEmptyTableValue)),
                       Arg("row", TextValue("json_document", "[1,\"alpha\"]"))}),
                  "json_document",
                  "{\"kind\":\"table_value\",\"row_shape\":[],\"rows\":[[1,\"alpha\"]],\"row_count\":1}") && ok;
  ok = ExpectText("SBSQL-24E967F07B8A SBSFC053-table-value-append-row",
                  Run(registry, "sb.table_value.append",
                      {Arg("tv", TextValue("json_document", kEmptyTableValue)),
                       Arg("row", TextValue("json_document", "[1,\"alpha\"]"))}),
                  "json_document",
                  "{\"kind\":\"table_value\",\"row_shape\":[],\"rows\":[[1,\"alpha\"]],\"row_count\":1}") && ok;

  ok = ExpectText("SBSQL-3278282AF7A1 SBSFC053-setof-generic",
                  Run(registry, "sb.setof.generic",
                      {Arg("value", TextValue("character", "alpha")),
                       Arg("ordinality", IntValue(1))}),
                  "json_document",
                  "{\"kind\":\"setof\",\"columns\":[\"value\",\"ordinality\"],"
                  "\"rows\":[[\"alpha\",1]],\"row_count\":1}") && ok;
  ok = ExpectText("SBSQL-618842668D61 SBSFC053-setof-key-text-value-text",
                  Run(registry, "sb.setof.key_text_value_text",
                      {Arg("key", TextValue("character", "a")),
                       Arg("value", TextValue("character", "one"))}),
                  "json_document",
                  "{\"kind\":\"setof\",\"columns\":[\"key\",\"value\"],"
                  "\"rows\":[[\"a\",\"one\"]],\"row_count\":1}") && ok;
  ok = ExpectText("SBSQL-DC6373538835 SBSFC053-setof-key-text-value-document",
                  Run(registry, "sb.setof.key_text_value_document",
                      {Arg("key", TextValue("character", "a")),
                       Arg("value", TextValue("json_document", "{\"n\":1}"))}),
                  "json_document",
                  "{\"kind\":\"setof\",\"columns\":[\"key\",\"value\"],"
                  "\"rows\":[[\"a\",{\"n\":1}]],\"row_count\":1}") && ok;

  ok = ExpectText("SBSQL-0D038FF22DA8 SBSFC053-unnest-marker",
                  Run(registry, "sb.rowset.unnest",
                      {Arg("array", TextValue("json_document", "[1,2,3]"))}),
                  "json_document", "[1,2,3]") && ok;
  ok = ExpectText("SBSQL-E11E27B45C94 SBSFC053-unnest-array",
                  Run(registry, "sb.rowset.unnest",
                      {Arg("array", TextValue("json_document", "[1,2,3]"))}),
                  "json_document", "[1,2,3]") && ok;
  ok = ExpectText("SBSQL-8A1E3E863769 SBSFC053-generate-series-marker",
                  Run(registry, "sb.rowset.generate_series",
                      {Arg("start", IntValue(1)), Arg("stop", IntValue(5)), Arg("step", IntValue(2))}),
                  "json_document", "[1,3,5]") && ok;
  ok = ExpectText("SBSQL-38EE3D5E1400 SBSFC053-generate-series-start-stop-step",
                  Run(registry, "sb.rowset.generate_series",
                      {Arg("start", IntValue(1)), Arg("stop", IntValue(5)), Arg("step", IntValue(2))}),
                  "json_document", "[1,3,5]") && ok;

  ok = ExpectText("SBSQL-01B057BBC0EA SBSFC053-multiset-element",
                  Run(registry, "sb.multiset.element",
                      {Arg("multiset", TextValue("json_document", "[7]"))}),
                  "json_document", "7") && ok;
  ok = ExpectText("SBSQL-4B19CB6607C3 SBSFC053-multiset-fusion",
                  Run(registry, "sb.multiset.fusion",
                      {Arg("left", TextValue("json_document", "[1,2]")),
                       Arg("right", TextValue("json_document", "[2,3]"))}),
                  "json_document", "[1,2,2,3]") && ok;
  ok = ExpectText("SBSQL-6B9810626A72 SBSFC053-multiset-intersection",
                  Run(registry, "sb.multiset.intersection",
                      {Arg("left", TextValue("json_document", "[1,2,2,3]")),
                       Arg("right", TextValue("json_document", "[2,2,4]"))}),
                  "json_document", "[2,2]") && ok;

  ok = ExpectInvalidInput("SBSFC053-rowset-append-missing-input",
                          Run(registry, "sb.rowset.append", {})) && ok;
  ok = ExpectInvalidInput("SBSFC053-generate-series-step-zero",
                          Run(registry, "sb.rowset.generate_series",
                              {Arg("start", IntValue(1)),
                               Arg("stop", IntValue(5)),
                               Arg("step", IntValue(0))})) && ok;
  ok = ExpectInvalidInput("SBSFC053-unnest-scalar-invalid",
                          Run(registry, "sb.rowset.unnest",
                              {Arg("array", TextValue("json_document", "1"))})) && ok;

  ok = ExpectProjectionJson(
           "SBSFC053-generate-series-projection",
           sblr::DispatchSblrOperation(
               {ProjectionContext(),
                ProjectionEnvelope(
                    "sb.rowset.generate_series",
                    {api::EngineProjectionFunctionArgument{"start", "int64", "1", false},
                     api::EngineProjectionFunctionArgument{"stop", "int64", "5", false},
                     api::EngineProjectionFunctionArgument{"step", "int64", "2", false}}),
                api::EngineApiRequest{}}),
           "[1,3,5]") && ok;

  if (!ok) return EXIT_FAILURE;
  std::cout << "sbsql_sbsfc_053_rowset_table_value_runtime_conformance=passed\n";
  return EXIT_SUCCESS;
}
