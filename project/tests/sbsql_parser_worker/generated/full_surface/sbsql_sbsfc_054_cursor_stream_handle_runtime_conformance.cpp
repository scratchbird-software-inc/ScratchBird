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

constexpr const char* kSessionUuid = "019f5400-0000-7000-8000-000000000002";
constexpr const char* kPrincipalUuid = "019f5400-0000-7000-8000-000000000003";
constexpr std::uint64_t kLocalTransactionId = 54054;

constexpr const char* kCursor =
    "{\"kind\":\"cursor\",\"handle_id\":\"cursor:test\",\"state\":\"open\","
    "\"position\":2,\"lifetime_class\":\"statement\",\"holdability\":\"without_hold\","
    "\"scrollability\":\"forward_only\",\"row_shape\":[],\"rows\":[[1],[2]],\"row_count\":2}";
constexpr const char* kCursorFromOpen =
    "{\"kind\":\"cursor\",\"handle_id\":\"cursor:open\",\"state\":\"open\","
    "\"position\":0,\"lifetime_class\":\"statement\",\"holdability\":\"without_hold\","
    "\"scrollability\":\"forward_only\",\"row_shape\":[],\"rows\":[],\"row_count\":0}";
constexpr const char* kClosedCursor =
    "{\"kind\":\"cursor\",\"handle_id\":\"cursor:test\",\"state\":\"closed\","
    "\"position\":2,\"lifetime_class\":\"statement\",\"holdability\":\"without_hold\","
    "\"scrollability\":\"forward_only\",\"row_shape\":[],\"rows\":[[1],[2]],\"row_count\":2}";
constexpr const char* kClosedDefaultCursor =
    "{\"kind\":\"cursor\",\"handle_id\":\"cursor:inline\",\"state\":\"closed\","
    "\"position\":0,\"lifetime_class\":\"statement\",\"holdability\":\"without_hold\","
    "\"scrollability\":\"forward_only\",\"row_shape\":[],\"rows\":[],\"row_count\":0}";
constexpr const char* kRowset =
    "{\"kind\":\"rowset\",\"row_shape\":[],\"rows\":[[1],[2]],\"row_count\":2}";
constexpr const char* kTableValue =
    "{\"kind\":\"table_value\",\"row_shape\":[],\"rows\":[[\"a\"]],\"row_count\":1}";
constexpr const char* kStream =
    "{\"kind\":\"stream\",\"handle_id\":\"stream:test\",\"state\":\"open\","
    "\"rows\":[[3],[4]],\"row_count\":2}";
constexpr const char* kCursorFromRowset =
    "{\"kind\":\"cursor\",\"handle_id\":\"cursor:rowset\",\"state\":\"open\","
    "\"position\":0,\"lifetime_class\":\"statement\",\"holdability\":\"without_hold\","
    "\"scrollability\":\"forward_only\",\"row_shape\":[],\"rows\":[[1],[2]],\"row_count\":2}";
constexpr const char* kCursorFromTableValue =
    "{\"kind\":\"cursor\",\"handle_id\":\"cursor:table_value\",\"state\":\"open\","
    "\"position\":0,\"lifetime_class\":\"statement\",\"holdability\":\"without_hold\","
    "\"scrollability\":\"forward_only\",\"row_shape\":[],\"rows\":[[\"a\"]],\"row_count\":1}";
constexpr const char* kRowsetFromStream =
    "{\"kind\":\"rowset\",\"row_shape\":[],\"rows\":[[3],[4]],\"row_count\":2}";
constexpr const char* kClosedEmptyStream =
    "{\"kind\":\"stream\",\"handle_id\":\"stream:inline\",\"state\":\"closed\",\"rows\":[],\"row_count\":0}";
constexpr const char* kClosedStream =
    "{\"kind\":\"stream\",\"handle_id\":\"stream:inline\",\"state\":\"closed\",\"rows\":[[3],[4]],\"row_count\":2}";
constexpr const char* kLocator =
    "{\"kind\":\"locator\",\"locator_id\":\"cursor:test:2\",\"source_handle_kind\":\"cursor\","
    "\"position\":2,\"valid\":true}";

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
  context.application_name = "sbsfc054-cursor-stream-handle";
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

bool ExpectBool(std::string_view case_id,
                const SblrResult& result,
                bool expected) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  const std::int64_t expected_int = expected ? 1 : 0;
  if (value.is_null || value.descriptor_id != "boolean" || !value.has_int64_value ||
      value.int64_value != expected_int) {
    std::cerr << case_id << ": expected boolean " << expected
              << ", got " << value.descriptor_id << " " << value.encoded_value << "\n";
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
                                         "SBSFC054-cursor-stream-handle-projection");
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
  context.request_id = "sbsfc054-cursor-stream-handle-projection";
  context.session_uuid.canonical = kSessionUuid;
  context.principal_uuid.canonical = kPrincipalUuid;
  context.local_transaction_id = kLocalTransactionId;
  context.snapshot_visible_through_local_transaction_id = kLocalTransactionId;
  context.application_name = "sbsfc054-cursor-stream-handle";
  context.security_context_present = true;
  return context;
}

bool ExpectProjectionText(std::string_view case_id,
                          const sblr::SblrDispatchResult& result,
                          std::string_view descriptor,
                          std::string_view expected) {
  if (!result.envelope_validated || !result.accepted || !result.dispatched_to_api ||
      !result.api_result.ok || result.api_result.result_shape.rows.size() != 1 ||
      result.api_result.result_shape.rows.front().fields.size() != 1) {
    std::cerr << case_id << ": expected one projected scalar field\n";
    return false;
  }
  const auto& value = result.api_result.result_shape.rows.front().fields.front().second;
  if (value.is_null || value.descriptor.canonical_type_name != descriptor ||
      value.encoded_value != expected) {
    std::cerr << case_id << ": expected projected " << descriptor << " " << expected
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

  ok = ExpectText("SBSQL-14BCC57267D0 SBSFC054-cursor-lifetime-class-arg",
                  Run(registry, "sb.cursor.lifetime_class",
                      {Arg("cursor", TextValue("json_document", kCursor))}),
                  "character", "statement") && ok;
  ok = ExpectText("SBSQL-163833F6642E SBSFC054-cursor-open-select",
                  Run(registry, "sb.cursor.open",
                      {Arg("select", TextValue("json_document", "{\"kind\":\"select\",\"sql\":\"select 1\"}"))}),
                  "json_document", kCursorFromOpen) && ok;
  ok = ExpectText("SBSQL-21E6F6488A64 SBSFC054-cursor-to-rowset-marker",
                  Run(registry, "sb.cursor.to_rowset",
                      {Arg("cursor", TextValue("json_document", kCursor))}),
                  "json_document", kRowset) && ok;
  ok = ExpectText("SBSQL-38FDC3F10237 SBSFC054-cursor-close-arg",
                  Run(registry, "sb.cursor.close",
                      {Arg("cursor", TextValue("json_document", kCursor))}),
                  "json_document", kClosedCursor) && ok;
  ok = ExpectText("SBSQL-60054AA2660F SBSFC054-cursor-lifetime-class-marker",
                  Run(registry, "sb.cursor.lifetime_class"),
                  "character", "statement") && ok;
  ok = ExpectText("SBSQL-6C87F2E4972C SBSFC054-cursor-open-marker",
                  Run(registry, "sb.cursor.open"),
                  "json_document", kCursorFromOpen) && ok;
  ok = ExpectText("SBSQL-892AE352BD3A SBSFC054-rowset-to-cursor-arg",
                  Run(registry, "sb.cursor.rowset_to_cursor",
                      {Arg("rowset", TextValue("json_document", kRowset))}),
                  "json_document", kCursorFromRowset) && ok;
  ok = ExpectText("SBSQL-8BE380B5BA73 SBSFC054-stream-close-marker",
                  Run(registry, "sb.stream.close"),
                  "json_document", kClosedEmptyStream) && ok;
  ok = ExpectText("SBSQL-92B7E4FF1332 SBSFC054-cursor-state-marker",
                  Run(registry, "sb.cursor.state"),
                  "character", "open") && ok;
  ok = ExpectInt("SBSQL-9CD08935260C SBSFC054-cursor-position-marker",
                 Run(registry, "sb.cursor.position"),
                 0) && ok;
  ok = ExpectText("SBSQL-A1FC30F481BC SBSFC054-table-value-to-cursor-arg",
                  Run(registry, "sb.cursor.table_value_to_cursor",
                      {Arg("tv", TextValue("json_document", kTableValue))}),
                  "json_document", kCursorFromTableValue) && ok;
  ok = ExpectText("SBSQL-A2E0FE1E034D SBSFC054-cursor-close-marker",
                  Run(registry, "sb.cursor.close"),
                  "json_document", kClosedDefaultCursor) && ok;
  ok = ExpectText("SBSQL-A339D846AD19 SBSFC054-current-row-locator-cursor",
                  Run(registry, "sb.cursor.current_row_locator",
                      {Arg("cursor", TextValue("json_document", kCursor))}),
                  "json_document", kLocator) && ok;
  ok = ExpectText("SBSQL-A99EC7329DF0 SBSFC054-cursor-to-rowset-args",
                  Run(registry, "sb.cursor.to_rowset",
                      {Arg("cursor", TextValue("json_document", kCursor)),
                       Arg("max_rows", IntValue(2))}),
                  "json_document", kRowset) && ok;
  ok = ExpectText("SBSQL-AE071DCD88A8 SBSFC054-cursor-scrollability-arg",
                  Run(registry, "sb.cursor.scrollability",
                      {Arg("cursor", TextValue("json_document", kCursor))}),
                  "character", "forward_only") && ok;
  ok = ExpectText("SBSQL-B062E4E23477 SBSFC054-handle-kind-marker",
                  Run(registry, "sb.handle.kind"),
                  "character", "none") && ok;
  ok = ExpectText("SBSQL-B9BA63C166A2 SBSFC054-stream-to-rowset-args",
                  Run(registry, "sb.stream.to_rowset",
                      {Arg("stream", TextValue("json_document", kStream)),
                       Arg("max_rows", IntValue(2))}),
                  "json_document", kRowsetFromStream) && ok;
  ok = ExpectText("SBSQL-C3E53B267C4B SBSFC054-cursor-holdability-marker",
                  Run(registry, "sb.cursor.holdability"),
                  "character", "without_hold") && ok;
  ok = ExpectInt("SBSQL-C4EB99EF9F6F SBSFC054-cursor-position-arg",
                 Run(registry, "sb.cursor.position",
                     {Arg("cursor", TextValue("json_document", kCursor))}),
                 2) && ok;
  ok = ExpectText("SBSQL-C682B85033B8 SBSFC054-cursor-scrollability-marker",
                  Run(registry, "sb.cursor.scrollability"),
                  "character", "forward_only") && ok;
  ok = ExpectText("SBSQL-CD315B828601 SBSFC054-table-value-to-cursor-marker",
                  Run(registry, "sb.cursor.table_value_to_cursor",
                      {Arg("tv", TextValue("json_document", kTableValue))}),
                  "json_document", kCursorFromTableValue) && ok;
  ok = ExpectText("SBSQL-CF69DD85814A SBSFC054-rowset-to-cursor-marker",
                  Run(registry, "sb.cursor.rowset_to_cursor",
                      {Arg("rowset", TextValue("json_document", kRowset))}),
                  "json_document", kCursorFromRowset) && ok;
  ok = ExpectText("SBSQL-D059810BF5A0 SBSFC054-handle-kind-arg",
                  Run(registry, "sb.handle.kind",
                      {Arg("handle", TextValue("json_document", kCursor))}),
                  "character", "cursor") && ok;
  ok = ExpectBool("SBSQL-D7858961F2DA SBSFC054-cursor-active-marker",
                  Run(registry, "sb.cursor.active"),
                  false) && ok;
  ok = ExpectText("SBSQL-DCB17997FCA9 SBSFC054-stream-close-arg",
                  Run(registry, "sb.stream.close",
                      {Arg("stream", TextValue("json_document", kStream))}),
                  "json_document", kClosedStream) && ok;
  ok = ExpectBool("SBSQL-EFC58ACD7975 SBSFC054-cursor-active-name",
                  Run(registry, "sb.cursor.active",
                      {Arg("name", TextValue("character", "c1"))}),
                  false) && ok;
  ok = ExpectText("SBSQL-F0E216005A4E SBSFC054-cursor-holdability-arg",
                  Run(registry, "sb.cursor.holdability",
                      {Arg("cursor", TextValue("json_document", kCursor))}),
                  "character", "without_hold") && ok;
  ok = ExpectText("SBSQL-F15435ED32F1 SBSFC054-stream-to-rowset-marker",
                  Run(registry, "sb.stream.to_rowset",
                      {Arg("stream", TextValue("json_document", kStream))}),
                  "json_document", kRowsetFromStream) && ok;
  ok = ExpectText("SBSQL-FCD7942CBB69 SBSFC054-cursor-state-arg",
                  Run(registry, "sb.cursor.state",
                      {Arg("cursor", TextValue("json_document", kCursor))}),
                  "character", "open") && ok;

  ok = ExpectInvalidInput("SBSFC054-cursor-to-rowset-missing",
                          Run(registry, "sb.cursor.to_rowset")) && ok;
  ok = ExpectInvalidInput("SBSFC054-rowset-to-cursor-malformed",
                          Run(registry, "sb.cursor.rowset_to_cursor",
                              {Arg("rowset", TextValue("json_document", kTableValue))})) && ok;
  ok = ExpectInvalidInput("SBSFC054-stream-to-rowset-negative-max",
                          Run(registry, "sb.stream.to_rowset",
                              {Arg("stream", TextValue("json_document", kStream)),
                               Arg("max_rows", IntValue(-1))})) && ok;

  ok = ExpectProjectionText(
           "SBSFC054-cursor-state-projection",
           sblr::DispatchSblrOperation(
               {ProjectionContext(),
                ProjectionEnvelope(
                    "sb.cursor.state",
                    {api::EngineProjectionFunctionArgument{"cursor", "json_document", kCursor, false}}),
                api::EngineApiRequest{}}),
           "character",
           "open") && ok;

  if (!ok) return EXIT_FAILURE;
  std::cout << "sbsql_sbsfc_054_cursor_stream_handle_runtime_conformance=passed\n";
  return EXIT_SUCCESS;
}
