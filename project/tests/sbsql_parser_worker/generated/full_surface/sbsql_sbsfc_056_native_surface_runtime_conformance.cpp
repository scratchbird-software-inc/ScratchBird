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

constexpr const char* kSessionUuid = "019f5600-0000-7000-8000-000000000002";
constexpr const char* kPrincipalUuid = "019f5600-0000-7000-8000-000000000003";
constexpr std::uint64_t kLocalTransactionId = 56056;

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
  context.application_name = "sbsfc056-native-surface";
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

bool ExpectNull(std::string_view case_id,
                const SblrResult& result,
                std::string_view descriptor) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (!value.is_null || value.descriptor_id != descriptor) {
    std::cerr << case_id << ": expected NULL " << descriptor
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
                                         "SBSFC056-native-surface-projection");
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
  context.request_id = "sbsfc056-native-surface-projection";
  context.session_uuid.canonical = kSessionUuid;
  context.principal_uuid.canonical = kPrincipalUuid;
  context.local_transaction_id = kLocalTransactionId;
  context.snapshot_visible_through_local_transaction_id = kLocalTransactionId;
  context.application_name = "sbsfc056-native-surface";
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

  ok = ExpectText("SBSQL-DF502F8DF4FA SBSFC056-accept-marker",
                  Run(registry, "sb.scalar.accept"),
                  "character", "acceptance.surface") && ok;
  ok = ExpectText("SBSQL-8CBB8186C7CC SBSFC056-close-marker",
                  Run(registry, "sb.scalar.close"),
                  "character", "keyword.close") && ok;
  ok = ExpectText("SBSQL-755DD39EA853 SBSFC056-future-version-marker",
                  Run(registry, "sb.scalar.future_version"),
                  "character", "syntax.future_version") && ok;
  ok = ExpectText("SBSQL-B30BB888C751 SBSFC056-gap-marker",
                  Run(registry, "sb.scalar.gap"),
                  "character", "surface.gap") && ok;
  ok = ExpectText("SBSQL-CD2216F125FB SBSFC056-immutable-marker",
                  Run(registry, "sb.scalar.immutable"),
                  "character", "volatility.immutable") && ok;
  ok = ExpectText("SBSQL-14EDC2636B45 SBSFC056-match-recognize-marker",
                  Run(registry, "sb.scalar.match_recognize"),
                  "json_document", "{\"kind\":\"match_recognize\",\"version\":\"v1\",\"pattern\":\"\"}") && ok;
  ok = ExpectText("SBSQL-C4027F6E6C8A SBSFC056-open-marker",
                  Run(registry, "sb.scalar.open"),
                  "character", "keyword.open") && ok;
  ok = ExpectText("SBSQL-67B876B5339F SBSFC056-reserved-marker",
                  Run(registry, "sb.scalar.reserved"),
                  "character", "syntax.reserved") && ok;
  ok = ExpectText("SBSQL-4AF1FA4C5BBC SBSFC056-syntax-future-marker",
                  Run(registry, "sb.scalar.sbsql_syntax_future_version"),
                  "character", "sbsql.syntax.future_version") && ok;
  ok = ExpectText("SBSQL-4975481A1AB7 SBSFC056-syntax-reserved-marker",
                  Run(registry, "sb.scalar.sbsql_syntax_reserved"),
                  "character", "sbsql.syntax.reserved") && ok;
  ok = ExpectText("SBSQL-8893D25F387F SBSFC056-stable-marker",
                  Run(registry, "sb.scalar.stable"),
                  "character", "volatility.stable") && ok;
  ok = ExpectText("SBSQL-ABD89A468ECA SBSFC056-treat-marker",
                  Run(registry, "sb.scalar.treat"),
                  "character", "special_form.treat") && ok;
  ok = ExpectText("SBSQL-504CEBDC6FE1 SBSFC056-treat-typed",
                  Run(registry, "sb.scalar.treat_typed",
                      {Arg("expr", TextValue("character", "abc")),
                       Arg("subtype", TextValue("character", "varchar"))}),
                  "varchar", "abc") && ok;
  ok = ExpectText("SBSQL-2E11730BB92B SBSFC056-volatile-marker",
                  Run(registry, "sb.scalar.volatile"),
                  "character", "volatility.volatile") && ok;
  ok = ExpectBool("SBSQL-12CD234538AF SBSFC056-accept-sql2016-timeseries",
                  Run(registry, "sb.scalar.accept_sql2016_timeseries",
                      {Arg("feature", TextValue("character", "SQL:2016 fits time-series"))}),
                  true) && ok;
  ok = ExpectText("SBSQL-A23C7082573D SBSFC056-any-value-marker",
                  Run(registry, "sb.aggregate.any_value"),
                  "character", "aggregate.any_value") && ok;
  ok = ExpectText("SBSQL-76EC89319569 SBSFC056-any-value-expr",
                  Run(registry, "sb.aggregate.any_value_expr",
                      {Arg("expr", TextValue("character", "alpha"))}),
                  "character", "alpha") && ok;
  ok = ExpectText("SBSQL-6C877B4376DE SBSFC056-at-time-zone",
                  Run(registry, "sb.scalar.at_time_zone",
                      {Arg("timestamp", TextValue("timestamp", "2026-05-18T10:00:00")),
                       Arg("zone", TextValue("character", "America/Toronto"))}),
                  "timestamp_tz", "2026-05-18T10:00:00 America/Toronto") && ok;
  ok = ExpectText("SBSQL-E17CFDACCB8E SBSFC056-bit-string",
                  Run(registry, "sb.scalar.bit_string",
                      {Arg("bits", TextValue("character", "B'1010'"))}),
                  "bit_string", "1010") && ok;
  ok = ExpectText("SBSQL-4E24AE0D0EDE SBSFC056-bulk-exceptions",
                  Run(registry, "sb.scalar.bulk_exceptions"),
                  "json_document", "[]") && ok;
  ok = ExpectText("SBSQL-D03ED69E33B7 SBSFC056-collect-marker",
                  Run(registry, "sb.aggregate.collect"),
                  "json_document", "[]") && ok;
  ok = ExpectText("SBSQL-A1B94C83C5F1 SBSFC056-collect-expr",
                  Run(registry, "sb.aggregate.collect_expr",
                      {Arg("expr", TextValue("character", "alpha"))}),
                  "json_document", "[\"alpha\"]") && ok;
  ok = ExpectText("SBSQL-5550BDA0A76C SBSFC056-domain-stack-marker",
                  Run(registry, "sb.scalar.domain_stack"),
                  "json_document", "[]") && ok;
  ok = ExpectText("SBSQL-1906412209C9 SBSFC056-domain-stack-value",
                  Run(registry, "sb.scalar.domain_stack_value",
                      {Arg("value", TextValue("character", "alpha"))}),
                  "json_document", "[{\"value\":\"alpha\",\"domain\":\"character\"}]") && ok;
  ok = ExpectText("SBSQL-8F66D89149F5 SBSFC056-reference-only",
                  Run(registry, "sb.scalar.reference_only"),
                  "character", "surface.reference_only") && ok;
  ok = ExpectText("SBSQL-F785EAF383DE SBSFC056-reference-rewrite",
                  Run(registry, "sb.scalar.reference_rewrite"),
                  "character", "surface.reference_rewrite") && ok;
  ok = ExpectText("SBSQL-FD0DF4067008 SBSFC056-element",
                  Run(registry, "sb.multiset.element",
                      {Arg("multiset", TextValue("json_document", "[\"solo\"]"))}),
                  "json_document", "\"solo\"") && ok;
  ok = ExpectText("SBSQL-1A8470FC95E7 SBSFC056-expr-match-recognize",
                  Run(registry, "sb.expr.match_recognize.v1",
                      {Arg("pattern", TextValue("character", "PATTERN(A+)"))}),
                  "json_document", "{\"kind\":\"match_recognize\",\"version\":\"v1\",\"pattern\":\"PATTERN(A+)\"}") && ok;
  ok = ExpectText("SBSQL-9F6F909938A0 SBSFC056-fusion",
                  Run(registry, "sb.multiset.fusion",
                      {Arg("left", TextValue("json_document", "[1,2]")),
                       Arg("right", TextValue("json_document", "[2,3]"))}),
                  "json_document", "[1,2,2,3]") && ok;
  ok = ExpectText("SBSQL-DB32CA47B7B5 SBSFC056-integer",
                  Run(registry, "sb.type.integer"),
                  "type_descriptor", "integer") && ok;
  ok = ExpectText("SBSQL-9C90F3645C34 SBSFC056-intersection",
                  Run(registry, "sb.multiset.intersection",
                      {Arg("left", TextValue("json_document", "[1,2,2]")),
                       Arg("right", TextValue("json_document", "[2,2,3]"))}),
                  "json_document", "[2,2]") && ok;
  ok = ExpectText("SBSQL-AC8794BE30FE SBSFC056-native-future",
                  Run(registry, "sb.scalar.native_future"),
                  "character", "status.native_future") && ok;
  ok = ExpectText("SBSQL-3F50B9923297 SBSFC056-native-now",
                  Run(registry, "sb.scalar.native_now"),
                  "character", "status.native_now") && ok;
  ok = ExpectText("SBSQL-83969495B383 SBSFC056-nvl",
                  Run(registry, "sb.scalar.nvl",
                      {Arg("expr", NullValue("character")),
                       Arg("fallback", TextValue("character", "fallback"))}),
                  "character", "fallback") && ok;
  ok = ExpectText("SBSQL-75F997655797 SBSFC056-private-only",
                  Run(registry, "sb.scalar.private_only"),
                  "character", "surface.private_only") && ok;
  ok = ExpectText("SBSQL-F94025D79003 SBSFC056-tabular",
                  Run(registry, "sb.scalar.tabular"),
                  "json_document", "{\"kind\":\"tabular\",\"columns\":[],\"rows\":[],\"row_count\":0}") && ok;
  ok = ExpectNull("SBSQL-FB4A06130103 SBSFC056-void",
                  Run(registry, "sb.scalar.void"),
                  "void") && ok;

  ok = ExpectInvalidInput("SBSFC056-bit-string-invalid",
                          Run(registry, "sb.scalar.bit_string",
                              {Arg("bits", TextValue("character", "102"))})) && ok;
  ok = ExpectInvalidInput("SBSFC056-nvl-missing",
                          Run(registry, "sb.scalar.nvl",
                              {Arg("expr", NullValue("character"))})) && ok;
  ok = ExpectInvalidInput("SBSFC056-integer-invalid",
                          Run(registry, "sb.type.integer",
                              {Arg("value", TextValue("character", "not-an-int"))})) && ok;

  auto projection = ProjectionEnvelope(
      "sb.scalar.at_time_zone",
      {api::EngineProjectionFunctionArgument{"timestamp", "timestamp", "2026-05-18T10:00:00", false},
       api::EngineProjectionFunctionArgument{"zone", "character", "America/Toronto", false}});
  ok = ExpectProjectionText("SBSFC056-at-time-zone-projection-route",
                            sblr::DispatchSblrOperation({ProjectionContext(),
                                                         projection,
                                                         api::EngineApiRequest{}}),
                            "timestamp_tz",
                            "2026-05-18T10:00:00 America/Toronto") && ok;

  if (!ok) return EXIT_FAILURE;
  std::cout << "sbsql_sbsfc_056_native_surface_runtime_conformance=passed\n";
  return EXIT_SUCCESS;
}
