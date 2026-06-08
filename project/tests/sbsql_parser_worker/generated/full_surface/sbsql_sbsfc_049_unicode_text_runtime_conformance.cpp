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
using sblr::SblrValue;
using sblr::SblrValuePayloadKind;

constexpr const char* kSessionUuid = "019f4900-0000-7000-8000-000000000002";
constexpr const char* kPrincipalUuid = "019f4900-0000-7000-8000-000000000003";
constexpr std::uint64_t kLocalTransactionId = 49049;

std::string ComposedEAcute() {
  return std::string("\xC3") + std::string("\xA9");
}

std::string DecomposedEAcute() {
  return std::string("e") + std::string("\xCC") + std::string("\x81");
}

std::string AlphaUnicodeText() {
  return std::string("A") + std::string("\xCE") + std::string("\xB2") +
         std::string("\xC3") + std::string("\x89");
}

SblrValue TextValue(std::string input) {
  SblrValue value;
  value.descriptor_id = "character";
  value.payload_kind = SblrValuePayloadKind::text;
  value.is_null = false;
  value.text_value = std::move(input);
  value.encoded_value = value.text_value;
  return value;
}

SblrValue NullValue(std::string descriptor) {
  SblrValue value;
  value.descriptor_id = std::move(descriptor);
  value.payload_kind = SblrValuePayloadKind::none;
  value.is_null = true;
  return value;
}

sblr::SblrExecutionContext BaseSblrContext() {
  sblr::SblrExecutionContext context;
  context.session_uuid = kSessionUuid;
  context.user_uuid = kPrincipalUuid;
  context.application_name = "sbsfc049-unicode-text";
  context.security_context_present = true;
  context.transaction_context_present = true;
  context.local_transaction_id = kLocalTransactionId;
  context.snapshot_visible_through_local_transaction_id = kLocalTransactionId;
  return context;
}

sblr::SblrResult RunFunction(const functions::FunctionRegistry& registry,
                             const sblr::SblrExecutionContext& context,
                             std::string function_id,
                             std::vector<SblrValue> values) {
  functions::FunctionCallRequest request;
  request.context.function_id = std::move(function_id);
  request.context.security_allowed = true;
  request.context.policy_allowed = true;
  request.context.dependency_available = true;
  request.context.sblr_context = context;
  for (std::size_t i = 0; i < values.size(); ++i) {
    request.arguments.push_back(
        functions::FunctionArgument{"arg" + std::to_string(i), std::move(values[i])});
  }
  return functions::DispatchFunctionCall(registry, std::move(request)).result;
}

bool ExpectText(std::string_view case_id,
                const sblr::SblrResult& result,
                std::string_view expected) {
  if (!result.ok() || result.scalar_values.size() != 1) {
    std::cerr << case_id << ": expected successful scalar result\n";
    return false;
  }
  const auto& value = result.scalar_values.front();
  if (value.is_null || value.descriptor_id != "character" ||
      value.text_value != expected) {
    std::cerr << case_id << ": expected character result, got "
              << value.descriptor_id << " " << value.text_value << '\n';
    return false;
  }
  return true;
}

bool ExpectBoolean(std::string_view case_id,
                   const sblr::SblrResult& result,
                   bool expected) {
  if (!result.ok() || result.scalar_values.size() != 1) {
    std::cerr << case_id << ": expected successful scalar result\n";
    return false;
  }
  const auto& value = result.scalar_values.front();
  const auto expected_int = expected ? 1 : 0;
  if (value.is_null || value.descriptor_id != "boolean" || !value.has_int64_value ||
      value.int64_value != expected_int) {
    std::cerr << case_id << ": expected boolean " << expected_int << ", got "
              << value.descriptor_id << " " << value.encoded_value << '\n';
    return false;
  }
  return true;
}

bool ExpectNull(std::string_view case_id,
                const sblr::SblrResult& result,
                std::string_view descriptor) {
  if (!result.ok() || result.scalar_values.size() != 1) {
    std::cerr << case_id << ": expected successful scalar result\n";
    return false;
  }
  const auto& value = result.scalar_values.front();
  if (!value.is_null || value.descriptor_id != descriptor) {
    std::cerr << case_id << ": expected null " << descriptor << ", got "
              << value.descriptor_id << " " << value.encoded_value << '\n';
    return false;
  }
  return true;
}

bool ExpectDiagnostic(std::string_view case_id,
                      const sblr::SblrResult& result,
                      std::string_view diagnostic_id) {
  if (result.ok() || result.diagnostics.empty()) {
    std::cerr << case_id << ": expected refusal diagnostic " << diagnostic_id << '\n';
    return false;
  }
  if (result.diagnostics.front().diagnostic_id != diagnostic_id) {
    std::cerr << case_id << ": expected diagnostic " << diagnostic_id << ", got "
              << result.diagnostics.front().diagnostic_id << '\n';
    return false;
  }
  return true;
}

sblr::SblrOperationEnvelope ProjectionEnvelope(
    std::string function_id,
    std::vector<api::EngineProjectionFunctionArgument> arguments) {
  auto envelope = sblr::MakeSblrEnvelope("query.evaluate_projection",
                                         "SBLR_QUERY_EVALUATE_PROJECTION",
                                         "SBSFC049-unicode-text-projection");
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
  context.request_id = "sbsfc049-unicode-text-projection";
  context.session_uuid.canonical = kSessionUuid;
  context.principal_uuid.canonical = kPrincipalUuid;
  context.local_transaction_id = kLocalTransactionId;
  context.snapshot_visible_through_local_transaction_id = kLocalTransactionId;
  context.application_name = "sbsfc049-unicode-text";
  context.security_context_present = true;
  return context;
}

bool ExpectProjectionText(std::string_view case_id,
                          const sblr::SblrDispatchResult& result,
                          std::string_view expected) {
  if (!result.envelope_validated || !result.accepted || !result.dispatched_to_api ||
      !result.api_result.ok || result.api_result.result_shape.rows.size() != 1 ||
      result.api_result.result_shape.rows.front().fields.size() != 1) {
    std::cerr << case_id << ": expected one projected scalar field\n";
    return false;
  }
  const auto& value = result.api_result.result_shape.rows.front().fields.front().second;
  if (value.is_null || value.descriptor.canonical_type_name != "character" ||
      value.encoded_value != expected) {
    std::cerr << case_id << ": expected projected character result\n";
    return false;
  }
  return true;
}

}  // namespace

int main() {
  const auto package = functions::BuildStandardFunctionSeedPackage();
  const auto& registry = package.registry;
  const auto context = BaseSblrContext();
  bool ok = true;

  ok = ExpectText("SBSFC049-unicode-normalize-marker",
                  RunFunction(registry, context, "sb.scalar.unicode_normalize", {}),
                  "unicode_normalization.nfc.route") && ok;
  ok = ExpectText("SBSFC049-normalize-marker",
                  RunFunction(registry, context, "sb.scalar.normalize", {}),
                  "unicode_normalization.nfc.route") && ok;

  ok = ExpectText("SBSFC049-normalize-nfc-compose",
                  RunFunction(registry, context, "sb.scalar.normalize_text_form",
                              {TextValue(DecomposedEAcute()), TextValue("NFC")}),
                  ComposedEAcute()) && ok;
  ok = ExpectText("SBSFC049-normalize-nfd-decompose",
                  RunFunction(registry, context, "sb.scalar.normalize_text_form",
                              {TextValue(ComposedEAcute()), TextValue("NFD")}),
                  DecomposedEAcute()) && ok;
  ok = ExpectText("SBSFC049-unicode-normalize-default",
                  RunFunction(registry, context, "sb.scalar.unicode_normalize",
                              {TextValue(DecomposedEAcute())}),
                  ComposedEAcute()) && ok;
  ok = ExpectNull("SBSFC049-normalize-null-text",
                  RunFunction(registry, context, "sb.scalar.normalize_text_form",
                              {NullValue("character")}),
                  "character") && ok;
  ok = ExpectDiagnostic(
           "SBSFC049-normalize-invalid-form",
           RunFunction(registry, context, "sb.scalar.normalize_text_form",
                       {TextValue("abc"), TextValue("BAD")}),
           "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;

  ok = ExpectBoolean("SBSFC049-is-alpha-marker",
                     RunFunction(registry, context, "sb.scalar.is_alpha", {}),
                     false) && ok;
  ok = ExpectBoolean("SBSFC049-is-alpha-true",
                     RunFunction(registry, context, "sb.scalar.is_alpha",
                                 {TextValue(AlphaUnicodeText())}),
                     true) && ok;
  ok = ExpectBoolean("SBSFC049-is-alpha-false",
                     RunFunction(registry, context, "sb.scalar.is_alpha",
                                 {TextValue("A1")}),
                     false) && ok;
  ok = ExpectNull("SBSFC049-is-alpha-null",
                  RunFunction(registry, context, "sb.scalar.is_alpha",
                              {NullValue("character")}),
                  "boolean") && ok;

  ok = ExpectProjectionText(
           "SBSFC049-normalize-projection",
           sblr::DispatchSblrOperation(
               {ProjectionContext(),
                ProjectionEnvelope(
                    "sb.scalar.normalize_text_form",
                    {api::EngineProjectionFunctionArgument{
                         "text", "character", DecomposedEAcute(), false},
                     api::EngineProjectionFunctionArgument{
                         "form", "character", "NFC", false}}),
                api::EngineApiRequest{}}),
           ComposedEAcute()) && ok;

  if (!ok) return 1;
  std::cout << "sbsql_sbsfc_049_unicode_text_runtime_conformance=passed\n";
  return 0;
}
