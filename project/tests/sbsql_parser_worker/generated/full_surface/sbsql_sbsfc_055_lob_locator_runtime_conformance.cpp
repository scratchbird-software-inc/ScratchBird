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

constexpr const char* kSessionUuid = "019f5500-0000-7000-8000-000000000002";
constexpr const char* kPrincipalUuid = "019f5500-0000-7000-8000-000000000003";
constexpr std::uint64_t kLocalTransactionId = 55055;

constexpr const char* kTextLob =
    "{\"kind\":\"lob_locator\",\"locator_id\":\"lob:test\",\"state\":\"open\","
    "\"mode\":\"read_write\",\"class\":\"text\",\"media_type\":\"text/plain\","
    "\"data_hex\":\"68656c6c6f\",\"size\":5,\"valid\":true}";
constexpr const char* kBinaryLob =
    "{\"kind\":\"lob_locator\",\"locator_id\":\"lob:test\",\"state\":\"open\","
    "\"mode\":\"read_write\",\"class\":\"binary\",\"media_type\":\"application/octet-stream\","
    "\"data_hex\":\"000102\",\"size\":3,\"valid\":true}";
constexpr const char* kDefaultOpen =
    "{\"kind\":\"lob_locator\",\"locator_id\":\"lob:inline\",\"state\":\"open\","
    "\"mode\":\"read_write\",\"class\":\"binary\",\"media_type\":\"application/octet-stream\","
    "\"data_hex\":\"\",\"size\":0,\"valid\":true}";
constexpr const char* kDefaultClosed =
    "{\"kind\":\"lob_locator\",\"locator_id\":\"lob:inline\",\"state\":\"closed\","
    "\"mode\":\"read_write\",\"class\":\"binary\",\"media_type\":\"application/octet-stream\","
    "\"data_hex\":\"\",\"size\":0,\"valid\":true}";
constexpr const char* kCreateBinary =
    "{\"kind\":\"lob_locator\",\"locator_id\":\"lob:create\",\"state\":\"open\","
    "\"mode\":\"read_write\",\"class\":\"binary\",\"media_type\":\"application/octet-stream\","
    "\"data_hex\":\"\",\"size\":0,\"valid\":true}";
constexpr const char* kCreateText =
    "{\"kind\":\"lob_locator\",\"locator_id\":\"lob:create\",\"state\":\"open\","
    "\"mode\":\"read_write\",\"class\":\"text\",\"media_type\":\"text/custom\","
    "\"data_hex\":\"\",\"size\":0,\"valid\":true}";
constexpr const char* kTextLobOpenRead =
    "{\"kind\":\"lob_locator\",\"locator_id\":\"lob:test\",\"state\":\"open\","
    "\"mode\":\"read\",\"class\":\"text\",\"media_type\":\"text/plain\","
    "\"data_hex\":\"68656c6c6f\",\"size\":5,\"valid\":true}";
constexpr const char* kTextLobClosed =
    "{\"kind\":\"lob_locator\",\"locator_id\":\"lob:test\",\"state\":\"closed\","
    "\"mode\":\"read_write\",\"class\":\"text\",\"media_type\":\"text/plain\","
    "\"data_hex\":\"68656c6c6f\",\"size\":5,\"valid\":true}";
constexpr const char* kTextLobWritten =
    "{\"kind\":\"lob_locator\",\"locator_id\":\"lob:test\",\"state\":\"open\","
    "\"mode\":\"read_write\",\"class\":\"text\",\"media_type\":\"text/plain\","
    "\"data_hex\":\"68595a6c6f\",\"size\":5,\"valid\":true}";
constexpr const char* kDefaultAppended =
    "{\"kind\":\"lob_locator\",\"locator_id\":\"lob:inline\",\"state\":\"open\","
    "\"mode\":\"read_write\",\"class\":\"binary\",\"media_type\":\"application/octet-stream\","
    "\"data_hex\":\"6869\",\"size\":2,\"valid\":true}";
constexpr const char* kTextLobAppended =
    "{\"kind\":\"lob_locator\",\"locator_id\":\"lob:test\",\"state\":\"open\","
    "\"mode\":\"read_write\",\"class\":\"text\",\"media_type\":\"text/plain\","
    "\"data_hex\":\"68656c6c6f21\",\"size\":6,\"valid\":true}";
constexpr const char* kTextLobTruncated =
    "{\"kind\":\"lob_locator\",\"locator_id\":\"lob:test\",\"state\":\"open\","
    "\"mode\":\"read_write\",\"class\":\"text\",\"media_type\":\"text/plain\","
    "\"data_hex\":\"68656c\",\"size\":3,\"valid\":true}";
constexpr const char* kLocator =
    "{\"kind\":\"locator\",\"locator_id\":\"locator:inline\",\"source_handle_kind\":\"generic\","
    "\"position\":0,\"valid\":true}";
constexpr const char* kCurrentRowLocator =
    "{\"kind\":\"locator\",\"locator_id\":\"row:current\",\"source_handle_kind\":\"row\","
    "\"position\":0,\"valid\":true}";

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

SblrValue BinaryValue(std::vector<std::uint8_t> input) {
  SblrValue value;
  value.descriptor_id = "binary";
  value.payload_kind = SblrValuePayloadKind::binary;
  value.binary_value = std::move(input);
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
  context.application_name = "sbsfc055-lob-locator";
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

bool ExpectBinary(std::string_view case_id,
                  const SblrResult& result,
                  std::vector<std::uint8_t> expected) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (value.is_null || value.descriptor_id != "binary" ||
      value.payload_kind != SblrValuePayloadKind::binary ||
      value.binary_value != expected) {
    std::cerr << case_id << ": expected binary length " << expected.size()
              << ", got " << value.descriptor_id << " length " << value.binary_value.size() << "\n";
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
                                         "SBSFC055-lob-locator-projection");
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
  context.request_id = "sbsfc055-lob-locator-projection";
  context.session_uuid.canonical = kSessionUuid;
  context.principal_uuid.canonical = kPrincipalUuid;
  context.local_transaction_id = kLocalTransactionId;
  context.snapshot_visible_through_local_transaction_id = kLocalTransactionId;
  context.application_name = "sbsfc055-lob-locator";
  context.security_context_present = true;
  return context;
}

bool ExpectProjectionInt(std::string_view case_id,
                         const sblr::SblrDispatchResult& result,
                         std::int64_t expected) {
  if (!result.envelope_validated || !result.accepted || !result.dispatched_to_api ||
      !result.api_result.ok || result.api_result.result_shape.rows.size() != 1 ||
      result.api_result.result_shape.rows.front().fields.size() != 1) {
    std::cerr << case_id << ": expected one projected scalar field\n";
    return false;
  }
  const auto& value = result.api_result.result_shape.rows.front().fields.front().second;
  if (value.is_null || value.descriptor.canonical_type_name != "int64" ||
      value.encoded_value != std::to_string(expected)) {
    std::cerr << case_id << ": expected projected int64 " << expected
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

  ok = ExpectBinary("SBSQL-15EB156297E9 SBSFC055-lob-locator-to-binary-marker",
                    Run(registry, "sb.lob.locator_to_binary"),
                    {}) && ok;
  ok = ExpectBool("SBSQL-176685E96193 SBSFC055-locator-validity-arg",
                  Run(registry, "sb.locator.validity",
                      {Arg("locator", TextValue("json_document", kTextLob))}),
                  true) && ok;
  ok = ExpectText("SBSQL-1EE6C3D7F2EE SBSFC055-lob-write-marker",
                  Run(registry, "sb.lob.write",
                      {Arg("locator", TextValue("json_document", kTextLob)),
                       Arg("offset", IntValue(2)),
                       Arg("data", TextValue("character", "YZ"))}),
                  "json_document", kTextLobWritten) && ok;
  ok = ExpectText("SBSQL-2E2F4913A42F SBSFC055-lob-close-arg",
                  Run(registry, "sb.lob.close",
                      {Arg("locator", TextValue("json_document", kTextLob))}),
                  "json_document", kTextLobClosed) && ok;
  ok = ExpectInt("SBSQL-317A464A74B3 SBSFC055-lob-size-marker",
                 Run(registry, "sb.lob.size"),
                 0) && ok;
  ok = ExpectText("SBSQL-41A19A07C09B SBSFC055-lob-open-arg",
                  Run(registry, "sb.lob.open",
                      {Arg("locator", TextValue("json_document", kTextLob)),
                       Arg("mode", TextValue("character", "read"))}),
                  "json_document", kTextLobOpenRead) && ok;
  ok = ExpectText("SBSQL-4B3B3D4FB26A SBSFC055-lob-append-marker",
                  Run(registry, "sb.lob.append",
                      {Arg("data", TextValue("character", "hi"))}),
                  "json_document", kDefaultAppended) && ok;
  ok = ExpectText("SBSQL-531A760C8C66 SBSFC055-current-row-locator-marker",
                  Run(registry, "sb.locator.current_row"),
                  "json_document", kCurrentRowLocator) && ok;
  ok = ExpectBool("SBSQL-7B6B59743B35 SBSFC055-locator-validity-marker",
                  Run(registry, "sb.locator.validity"),
                  false) && ok;
  ok = ExpectText("SBSQL-7CF4F4150D85 SBSFC055-lob-truncate-marker",
                  Run(registry, "sb.lob.truncate",
                      {Arg("locator", TextValue("json_document", kTextLob)),
                       Arg("length", IntValue(3))}),
                  "json_document", kTextLobTruncated) && ok;
  ok = ExpectText("SBSQL-891992A5F310 SBSFC055-lob-open-marker",
                  Run(registry, "sb.lob.open"),
                  "json_document", kDefaultOpen) && ok;
  ok = ExpectBinary("SBSQL-96DE0F2265B6 SBSFC055-lob-locator-to-binary-arg",
                    Run(registry, "sb.lob.locator_to_binary",
                        {Arg("locator", TextValue("json_document", kBinaryLob))}),
                    {0x00, 0x01, 0x02}) && ok;
  ok = ExpectText("SBSQL-9C16F6BF7072 SBSFC055-lob-truncate-arg",
                  Run(registry, "sb.lob.truncate",
                      {Arg("locator", TextValue("json_document", kTextLob)),
                       Arg("length", IntValue(3))}),
                  "json_document", kTextLobTruncated) && ok;
  ok = ExpectText("SBSQL-A9177A9A947C SBSFC055-lob-write-arg",
                  Run(registry, "sb.lob.write",
                      {Arg("locator", TextValue("json_document", kTextLob)),
                       Arg("offset", IntValue(2)),
                       Arg("data", TextValue("character", "YZ"))}),
                  "json_document", kTextLobWritten) && ok;
  ok = ExpectText("SBSQL-C2F659F4DFC0 SBSFC055-lob-read-marker",
                  Run(registry, "sb.lob.read",
                      {Arg("locator", TextValue("json_document", kTextLob)),
                       Arg("offset", IntValue(2)),
                       Arg("length", IntValue(3))}),
                  "character", "ell") && ok;
  ok = ExpectInt("SBSQL-C62D69D167C8 SBSFC055-lob-size-arg",
                 Run(registry, "sb.lob.size",
                     {Arg("locator", TextValue("json_document", kTextLob))}),
                 5) && ok;
  ok = ExpectText("SBSQL-D2E48C4160ED SBSFC055-lob-create-arg",
                  Run(registry, "sb.lob.create",
                      {Arg("class", TextValue("character", "text")),
                       Arg("media", TextValue("character", "text/custom"))}),
                  "json_document", kCreateText) && ok;
  ok = ExpectText("SBSQL-D5E167A4984B SBSFC055-lob-append-arg",
                  Run(registry, "sb.lob.append",
                      {Arg("locator", TextValue("json_document", kTextLob)),
                       Arg("data", TextValue("character", "!"))}),
                  "json_document", kTextLobAppended) && ok;
  ok = ExpectText("SBSQL-D89EF3B31969 SBSFC055-lob-create-marker",
                  Run(registry, "sb.lob.create"),
                  "json_document", kCreateBinary) && ok;
  ok = ExpectText("SBSQL-DA9087A02218 SBSFC055-lob-locator-to-text-arg",
                  Run(registry, "sb.lob.locator_to_text",
                      {Arg("locator", TextValue("json_document", kTextLob))}),
                  "character", "hello") && ok;
  ok = ExpectText("SBSQL-DF89DE098501 SBSFC055-lob-close-marker",
                  Run(registry, "sb.lob.close"),
                  "json_document", kDefaultClosed) && ok;
  ok = ExpectText("SBSQL-E7CBEEE4AAC6 SBSFC055-lob-locator-to-text-marker",
                  Run(registry, "sb.lob.locator_to_text"),
                  "character", "") && ok;
  ok = ExpectText("SBSQL-F2A363288372 SBSFC055-locator-marker",
                  Run(registry, "sb.locator.locator"),
                  "json_document", kLocator) && ok;
  ok = ExpectText("SBSQL-FC06D12DAC16 SBSFC055-lob-read-arg",
                  Run(registry, "sb.lob.read",
                      {Arg("locator", TextValue("json_document", kTextLob)),
                       Arg("offset", IntValue(2)),
                       Arg("length", IntValue(3))}),
                  "character", "ell") && ok;

  ok = ExpectInvalidInput("SBSFC055-lob-read-missing",
                          Run(registry, "sb.lob.read")) && ok;
  ok = ExpectInvalidInput("SBSFC055-lob-open-malformed",
                          Run(registry, "sb.lob.open",
                              {Arg("locator", TextValue("json_document", "{\"kind\":\"locator\"}"))})) && ok;
  ok = ExpectInvalidInput("SBSFC055-lob-write-bad-offset",
                          Run(registry, "sb.lob.write",
                              {Arg("locator", TextValue("json_document", kTextLob)),
                               Arg("offset", IntValue(0)),
                               Arg("data", TextValue("character", "x"))})) && ok;

  auto projection = ProjectionEnvelope(
      "sb.lob.size",
      {api::EngineProjectionFunctionArgument{"locator", "json_document", kTextLob, false}});
  ok = ExpectProjectionInt("SBSFC055-lob-size-projection-route",
                           sblr::DispatchSblrOperation({ProjectionContext(),
                                                        projection,
                                                        api::EngineApiRequest{}}),
                           5) && ok;

  if (!ok) return EXIT_FAILURE;
  std::cout << "sbsql_sbsfc_055_lob_locator_runtime_conformance=passed\n";
  return EXIT_SUCCESS;
}
