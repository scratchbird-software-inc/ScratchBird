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
using scratchbird::engine::sblr::SblrStatusCode;
using scratchbird::engine::sblr::SblrValue;
using scratchbird::engine::sblr::SblrValuePayloadKind;

SblrValue DocumentValue(std::string input) {
  SblrValue value;
  value.descriptor_id = "json_document";
  value.payload_kind = SblrValuePayloadKind::text;
  value.is_null = false;
  value.encoded_value = std::move(input);
  value.text_value = value.encoded_value;
  return value;
}

SblrValue TextValue(std::string input) {
  SblrValue value;
  value.descriptor_id = "character";
  value.payload_kind = SblrValuePayloadKind::text;
  value.is_null = false;
  value.encoded_value = std::move(input);
  value.text_value = value.encoded_value;
  return value;
}

SblrValue Int64Value(std::int64_t input) {
  SblrValue value;
  value.descriptor_id = "int64";
  value.payload_kind = SblrValuePayloadKind::signed_integer;
  value.is_null = false;
  value.has_int64_value = true;
  value.int64_value = input;
  value.encoded_value = std::to_string(input);
  value.text_value = value.encoded_value;
  return value;
}

SblrValue NullValue(std::string descriptor = {}) {
  SblrValue value;
  value.descriptor_id = std::move(descriptor);
  value.is_null = true;
  return value;
}

bool HasDiagnostic(const scratchbird::engine::sblr::SblrResult& result, std::string_view id) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.diagnostic_id == id) return true;
  }
  return false;
}

scratchbird::engine::sblr::SblrResult Run(const FunctionRegistry& registry,
                                          std::string function_id,
                                          std::vector<SblrValue> values = {}) {
  FunctionCallRequest request;
  request.context.function_id = std::move(function_id);
  request.context.security_allowed = true;
  request.context.policy_allowed = true;
  request.context.dependency_available = true;
  request.context.sblr_context.database_uuid = "SBSFC-013-document-runtime-db";
  request.context.sblr_context.transaction_uuid = "SBSFC-013-document-runtime-tx";
  request.context.sblr_context.transaction_context_present = true;
  for (std::size_t i = 0; i < values.size(); ++i) {
    request.arguments.push_back(FunctionArgument{"arg" + std::to_string(i), std::move(values[i])});
  }
  return DispatchFunctionCall(registry, std::move(request)).result;
}

bool ExpectOkScalar(const scratchbird::engine::sblr::SblrResult& result, std::string_view case_id) {
  if (!result.ok() || result.scalar_values.size() != 1) {
    std::cerr << case_id << ": expected one successful scalar result\n";
    return false;
  }
  return true;
}

bool ExpectText(std::string_view case_id,
                const scratchbird::engine::sblr::SblrResult& result,
                std::string_view expected,
                std::string_view descriptor) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (value.is_null || value.text_value != expected || value.descriptor_id != descriptor) {
    std::cerr << case_id << ": expected " << descriptor << " " << expected << ", got "
              << value.descriptor_id << " " << value.text_value << "\n";
    return false;
  }
  return true;
}

bool ExpectBoolean(std::string_view case_id,
                   const scratchbird::engine::sblr::SblrResult& result,
                   std::int64_t expected) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (value.is_null || !value.has_int64_value || value.int64_value != expected || value.descriptor_id != "boolean") {
    std::cerr << case_id << ": expected boolean " << expected << ", got "
              << value.descriptor_id << " " << value.encoded_value << "\n";
    return false;
  }
  return true;
}

bool ExpectUint64(std::string_view case_id,
                  const scratchbird::engine::sblr::SblrResult& result,
                  std::uint64_t expected) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (value.is_null || !value.has_uint64_value || value.uint64_value != expected || value.descriptor_id != "uint64") {
    std::cerr << case_id << ": expected uint64 " << expected << ", got "
              << value.descriptor_id << " " << value.encoded_value << "\n";
    return false;
  }
  return true;
}

bool ExpectNull(std::string_view case_id,
                const scratchbird::engine::sblr::SblrResult& result,
                std::string_view descriptor) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (!value.is_null || value.descriptor_id != descriptor) {
    std::cerr << case_id << ": expected NULL descriptor " << descriptor << ", got "
              << value.descriptor_id << "\n";
    return false;
  }
  return true;
}

bool ExpectFailure(std::string_view case_id,
                   const scratchbird::engine::sblr::SblrResult& result,
                   std::string_view diagnostic_id) {
  if (result.status == SblrStatusCode::ok || !HasDiagnostic(result, diagnostic_id)) {
    std::cerr << case_id << ": expected diagnostic " << diagnostic_id << "\n";
    return false;
  }
  return true;
}

}  // namespace

int main() {
  const auto package = BuildStandardFunctionSeedPackage();
  const auto& registry = package.registry;
  bool ok = true;

  ok = ExpectText("SBSQL-FE519C1C20F6 SBSFC013-json-typeof-generic",
                  Run(registry, "sb.json.typeof", {DocumentValue(R"({"a":1})")}),
                  "object",
                  "character") && ok;
  ok = ExpectText("SBSQL-4324775DBCA0 SBSFC013-json-typeof-document-form",
                  Run(registry, "sb.json.typeof", {DocumentValue("[1,2]")}),
                  "array",
                  "character") && ok;
  ok = ExpectText("SBSQL-36FF2B0254C0 SBSFC013-json-typeof-canonical",
                  Run(registry, "sb.json.typeof", {DocumentValue("true")}),
                  "boolean",
                  "character") && ok;
  ok = ExpectText("SBSQL-83995B2BC266 SBSFC013-json-extract-generic",
                  Run(registry, "sb.json.extract", {DocumentValue(R"({"a":42})"), TextValue("$.a")}),
                  "42",
                  "json_document") && ok;
  ok = ExpectNull("SBSQL-35F0E9FF7755 SBSFC013-json-extract-document-form",
                  Run(registry, "sb.json.extract", {DocumentValue(R"({"a":1})"), TextValue("$.missing")}),
                  "json_document") && ok;
  ok = ExpectText("SBSQL-7B0C5EB84734 SBSFC013-json-extract-canonical",
                  Run(registry, "sb.json.extract", {DocumentValue(R"({"a":42})"), TextValue("$.a")}),
                  "42",
                  "json_document") && ok;

  ok = ExpectBoolean("SBSQL-7EE8FAD14C5A SBSFC013-json-exists-generic",
                     Run(registry, "sb.json.exists", {DocumentValue(R"({"a":1})"), TextValue("$.a")}),
                     1) && ok;
  ok = ExpectBoolean("SBSQL-3E3BA120C541 SBSFC013-json-exists-sql-form",
                     Run(registry, "sb.json.exists", {DocumentValue(R"({"a":1})"), TextValue("$.missing")}),
                     0) && ok;
  ok = ExpectBoolean("SBSQL-1342A8B02022 SBSFC013-json-exists-document-form",
                     Run(registry, "sb.json.exists", {DocumentValue(R"({"a":1})"), TextValue("$.a")}),
                     1) && ok;

  ok = ExpectText("SBSQL-5E705E2E1462 SBSFC013-json-value-generic",
                  Run(registry, "sb.json.value", {DocumentValue(R"({"a":7})"), TextValue("$.a")}),
                  "7",
                  "json_document") && ok;
  ok = ExpectText("SBSQL-5B765753ADEC SBSFC013-json-value-sql-form",
                  Run(registry, "sb.json.value", {DocumentValue(R"({"a":"bird"})"), TextValue("$.a")}),
                  R"("bird")",
                  "json_document") && ok;
  ok = ExpectNull("SBSQL-68134CF09B70 SBSFC013-json-value-document-form",
                  Run(registry, "sb.json.value", {DocumentValue(R"({"a":1})"), TextValue("$.missing")}),
                  "json_document") && ok;

  ok = ExpectText("SBSQL-09BA0A3A71DB SBSFC013-json-query-generic",
                  Run(registry, "sb.json.query", {DocumentValue(R"({"a":{"b":2}})"), TextValue("$.a")}),
                  R"({"b":2})",
                  "json_document") && ok;
  ok = ExpectText("SBSQL-DC8507F9B9C5 SBSFC013-json-query-document-form",
                  Run(registry, "sb.json.query", {DocumentValue(R"({"a":[1,2]})"), TextValue("$.a")}),
                  "[1,2]",
                  "json_document") && ok;

  ok = ExpectText("SBSQL-03D2E8D0B9AE SBSFC013-json-set-generic",
                  Run(registry, "sb.json.set", {DocumentValue(R"({"a":1})"), TextValue("$.b"), Int64Value(2)}),
                  R"({"a":1,"b":2})",
                  "json_document") && ok;
  ok = ExpectText("SBSQL-7AA44FB9077C SBSFC013-json-set-document-form",
                  Run(registry, "sb.json.set", {DocumentValue(R"({"a":1})"), TextValue("$.a"), Int64Value(3)}),
                  R"({"a":3})",
                  "json_document") && ok;

  ok = ExpectText("SBSQL-E062D64D0C8F SBSFC013-json-remove-generic",
                  Run(registry, "sb.json.remove", {DocumentValue(R"({"a":1,"b":2})"), TextValue("$.a")}),
                  R"({"b":2})",
                  "json_document") && ok;
  ok = ExpectText("SBSQL-B4CF3C70B1D3 SBSFC013-json-remove-document-form",
                  Run(registry, "sb.json.remove", {DocumentValue(R"({"a":1,"b":2})"), TextValue("$.missing")}),
                  R"({"a":1,"b":2})",
                  "json_document") && ok;

  ok = ExpectText("SBSQL-14C23DAA9C77 SBSFC013-json-replace-generic",
                  Run(registry, "sb.json.replace", {DocumentValue(R"({"a":1})"), TextValue("$.a"), Int64Value(4)}),
                  R"({"a":4})",
                  "json_document") && ok;
  ok = ExpectText("SBSQL-23EEDCBB8140 SBSFC013-json-replace-document-form",
                  Run(registry, "sb.json.replace", {DocumentValue(R"({"a":1})"), TextValue("$.missing"), Int64Value(4)}),
                  R"({"a":1})",
                  "json_document") && ok;

  ok = ExpectText("SBSQL-AF045C026980 SBSFC013-json-insert-generic",
                  Run(registry, "sb.json.insert", {DocumentValue(R"({"a":1})"), TextValue("$.b"), Int64Value(2)}),
                  R"({"a":1,"b":2})",
                  "json_document") && ok;
  ok = ExpectText("SBSQL-2E230404921F SBSFC013-json-insert-document-form",
                  Run(registry, "sb.json.insert", {DocumentValue(R"({"a":1})"), TextValue("$.a"), Int64Value(9)}),
                  R"({"a":1})",
                  "json_document") && ok;

  ok = ExpectText("SBSQL-C55F7ADBD13B SBSFC013-jsonb-set-generic",
                  Run(registry, "sb.json.jsonb_set", {DocumentValue(R"({"a":1})"), TextValue("$.a"), Int64Value(5)}),
                  R"({"a":5})",
                  "json_document") && ok;
  ok = ExpectText("SBSQL-723E5CADA519 SBSFC013-jsonb-set-document-form",
                  Run(registry, "sb.json.jsonb_set", {DocumentValue(R"({"a":1})"), TextValue("$.missing"), Int64Value(5), Int64Value(0)}),
                  R"({"a":1})",
                  "json_document") && ok;

  ok = ExpectUint64("SBSQL-0A060E6427B3 SBSFC013-json-array-length-generic",
                    Run(registry, "sb.json.array_length", {DocumentValue(R"([1,2,{"a":3}])")}),
                    3) && ok;
  ok = ExpectUint64("SBSQL-DEDF07FAB7F3 SBSFC013-json-array-length-document-form",
                    Run(registry, "sb.json.array_length", {DocumentValue("[]")}),
                    0) && ok;
  ok = ExpectFailure("SBSQL-0A060E6427B3 SBSFC013-json-array-length-domain-error",
                     Run(registry, "sb.json.array_length", {DocumentValue(R"({"a":1})")}),
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;
  ok = ExpectUint64("SBSQL-5BCF9869AA4C SBSFC013-jsonb-array-length-generic",
                    Run(registry, "sb.json.jsonb_array_length", {DocumentValue("[1,2,3]")}),
                    3) && ok;
  ok = ExpectUint64("SBSQL-3CEB816A1165 SBSFC013-jsonb-array-length-document-form",
                    Run(registry, "sb.json.jsonb_array_length", {DocumentValue("[]")}),
                    0) && ok;

  ok = ExpectText("SBSQL-7B99FF977C66 SBSFC013-json-build-array-generic",
                  Run(registry, "sb.json.build_array", {Int64Value(1), TextValue("bird"), NullValue()}),
                  R"([1,"bird",null])",
                  "json_document") && ok;
  ok = ExpectText("SBSQL-4640811DBAC8 SBSFC013-json-build-array-variadic",
                  Run(registry, "sb.json.build_array"),
                  "[]",
                  "json_document") && ok;

  ok = ExpectText("SBSQL-E2DFF93CA59C SBSFC013-json-build-object-generic",
                  Run(registry, "sb.json.build_object", {TextValue("a"), Int64Value(1), TextValue("b"), TextValue("bird")}),
                  R"({"a":1,"b":"bird"})",
                  "json_document") && ok;
  ok = ExpectFailure("SBSQL-3217FFB2F3BD SBSFC013-json-build-object-variadic",
                     Run(registry, "sb.json.build_object", {TextValue("a"), Int64Value(1), TextValue("b")}),
                     "SB_DIAG_FUNCTION_INVALID_INPUT") && ok;
  ok = ExpectText("SBSQL-36FBFED38C80 SBSFC013-jsonb-build-array-generic",
                  Run(registry, "sb.json.jsonb_build_array", {Int64Value(1), TextValue("bird"), NullValue()}),
                  R"([1,"bird",null])",
                  "json_document") && ok;
  ok = ExpectText("SBSQL-34E68EB56EDC SBSFC013-jsonb-build-object-generic",
                  Run(registry, "sb.json.jsonb_build_object", {TextValue("a"), Int64Value(1)}),
                  R"({"a":1})",
                  "json_document") && ok;

  ok = ExpectText("SBSQL-CB837AAEBEAD SBSFC013-to-json-generic",
                  Run(registry, "sb.json.to_json", {TextValue("bird")}),
                  R"("bird")",
                  "json_document") && ok;
  ok = ExpectText("SBSQL-F0AB18F7417B SBSFC013-to-json-any-form",
                  Run(registry, "sb.json.to_json", {NullValue()}),
                  "null",
                  "json_document") && ok;
  ok = ExpectText("SBSQL-4119D041403C SBSFC013-to-jsonb-generic",
                  Run(registry, "sb.json.to_jsonb", {DocumentValue(R"({"a":1})")}),
                  R"({"a":1})",
                  "json_document") && ok;
  ok = ExpectText("SBSQL-88E66066EBC7 SBSFC013-to-jsonb-any-form",
                  Run(registry, "sb.json.to_jsonb", {Int64Value(7)}),
                  "7",
                  "json_document") && ok;

  ok = ExpectText("SBSQL-048498BB9A7F SBSFC013-jsonb-typeof-generic",
                  Run(registry, "sb.json.jsonb_typeof", {DocumentValue(R"({"a":1})")}),
                  "object",
                  "character") && ok;
  ok = ExpectText("SBSQL-2F78C18D9292 SBSFC013-jsonb-typeof-document-form",
                  Run(registry, "sb.json.jsonb_typeof", {DocumentValue("[1,2]")}),
                  "array",
                  "character") && ok;

  ok = ExpectText("SBSQL-0926D8F4ABD5 SBSFC013-json-object-generic",
                  Run(registry, "sb.json.object", {TextValue("a"), Int64Value(1)}),
                  R"({"a":1})",
                  "json_document") && ok;
  ok = ExpectText("SBSQL-551339ECEE75 SBSFC013-jsonb-object-generic",
                  Run(registry, "sb.json.jsonb_object", {TextValue("a"), Int64Value(1)}),
                  R"({"a":1})",
                  "json_document") && ok;
  ok = ExpectText("SBSQL-90E1BC86D62F SBSFC013-json-object-keys-generic",
                  Run(registry, "sb.json.object_keys", {DocumentValue(R"({"a":1,"b":2})")}),
                  R"(["a","b"])",
                  "json_document") && ok;
  ok = ExpectText("SBSQL-A05313B740CC SBSFC013-json-object-keys-document",
                  Run(registry, "sb.json.object_keys", {DocumentValue(R"({"a":1})")}),
                  R"(["a"])",
                  "json_document") && ok;
  ok = ExpectText("SBSQL-03447BA4EB25 SBSFC013-jsonb-object-keys-generic",
                  Run(registry, "sb.json.jsonb_object_keys", {DocumentValue(R"({"a":1,"b":2})")}),
                  R"(["a","b"])",
                  "json_document") && ok;
  ok = ExpectText("SBSQL-EB982B4F95B3 SBSFC013-jsonb-object-keys-document",
                  Run(registry, "sb.json.jsonb_object_keys", {DocumentValue(R"({"a":1})")}),
                  R"(["a"])",
                  "json_document") && ok;
  ok = ExpectText("SBSQL-0390232C7296 SBSFC013-json-array-elements-generic",
                  Run(registry, "sb.json.array_elements", {DocumentValue("[1,2]")}),
                  "[1,2]",
                  "json_document") && ok;
  ok = ExpectText("SBSQL-7819B29C7AB5 SBSFC013-json-array-elements-document",
                  Run(registry, "sb.json.array_elements", {DocumentValue("[1,2]")}),
                  "[1,2]",
                  "json_document") && ok;
  ok = ExpectText("SBSQL-4D97B9EA482B SBSFC013-json-array-elements-text",
                  Run(registry, "sb.json.array_elements_text", {DocumentValue("[1,2]")}),
                  R"(["1","2"])",
                  "json_document") && ok;
  ok = ExpectText("SBSQL-76883ECD3648 SBSFC013-json-each-generic",
                  Run(registry, "sb.json.each", {DocumentValue(R"({"a":1})")}),
                  R"([{"key":"a","value":1}])",
                  "json_document") && ok;
  ok = ExpectText("SBSQL-18521C5D03B8 SBSFC013-json-each-document",
                  Run(registry, "sb.json.each", {DocumentValue(R"({"a":1})")}),
                  R"([{"key":"a","value":1}])",
                  "json_document") && ok;
  ok = ExpectText("SBSQL-4B7CDEB23364 SBSFC013-json-each-text-generic",
                  Run(registry, "sb.json.each_text", {DocumentValue(R"({"a":1})")}),
                  R"([{"key":"a","value":"1"}])",
                  "json_document") && ok;
  ok = ExpectText("SBSQL-6AF2FB9EDEB9 SBSFC013-json-each-text-document",
                  Run(registry, "sb.json.each_text", {DocumentValue(R"({"a":1})")}),
                  R"([{"key":"a","value":"1"}])",
                  "json_document") && ok;
  ok = ExpectText("SBSQL-CE5BD771D075 SBSFC013-jsonb-insert-generic",
                  Run(registry, "sb.json.jsonb_insert", {DocumentValue(R"({"a":1})"), TextValue("$.b"), Int64Value(2)}),
                  R"({"a":1,"b":2})",
                  "json_document") && ok;
  ok = ExpectText("SBSQL-429DB32D5CC2 SBSFC013-jsonb-insert-document",
                  Run(registry, "sb.json.jsonb_insert", {DocumentValue(R"({"a":1})"), TextValue("$.a"), Int64Value(9), Int64Value(1)}),
                  R"({"a":1})",
                  "json_document") && ok;
  ok = ExpectBoolean("SBSQL-58F6D7F43DA6 SBSFC013-jsonb-path-exists-generic",
                     Run(registry, "sb.json.jsonb_path_exists", {DocumentValue(R"({"a":1})"), TextValue("$.a")}),
                     1) && ok;
  ok = ExpectBoolean("SBSQL-D4C29991D99B SBSFC013-jsonb-path-exists-document",
                     Run(registry, "sb.json.jsonb_path_exists", {DocumentValue(R"({"a":1})"), TextValue("$.missing")}),
                     0) && ok;
  ok = ExpectBoolean("SBSQL-9A4AB48B76FD SBSFC013-jsonb-path-match-generic",
                     Run(registry, "sb.json.jsonb_path_match", {DocumentValue(R"({"ok":true})"), TextValue("$.ok")}),
                     1) && ok;
  ok = ExpectBoolean("SBSQL-7C4821112F94 SBSFC013-jsonb-path-match-document",
                     Run(registry, "sb.json.jsonb_path_match", {DocumentValue(R"({"ok":false})"), TextValue("$.ok")}),
                     0) && ok;
  ok = ExpectText("SBSQL-436880E1F3F7 SBSFC013-jsonb-path-query-generic",
                  Run(registry, "sb.json.jsonb_path_query", {DocumentValue(R"({"a":{"b":2}})"), TextValue("$.a")}),
                  R"({"b":2})",
                  "json_document") && ok;
  ok = ExpectText("SBSQL-EA5E00825D4D SBSFC013-jsonb-path-query-document",
                  Run(registry, "sb.json.jsonb_path_query", {DocumentValue(R"({"a":7})"), TextValue("$.a")}),
                  "7",
                  "json_document") && ok;
  ok = ExpectText("SBSQL-A1C65D80CE68 SBSFC013-jsonb-path-query-array",
                  Run(registry, "sb.json.jsonb_path_query_array", {DocumentValue(R"({"a":7})"), TextValue("$.a")}),
                  "[7]",
                  "json_document") && ok;
  ok = ExpectText("SBSQL-B64295F1B742 SBSFC013-jsonb-path-query-first",
                  Run(registry, "sb.json.jsonb_path_query_first", {DocumentValue(R"({"a":"bird"})"), TextValue("$.a")}),
                  R"("bird")",
                  "json_document") && ok;
  ok = ExpectText("SBSQL-6910DED90537 SBSFC013-jsonb-pretty-generic",
                  Run(registry, "sb.json.jsonb_pretty", {DocumentValue(R"({"a":1})")}),
                  "{\n  \"a\": 1\n}",
                  "character") && ok;
  ok = ExpectText("SBSQL-4CFCAC326BFB SBSFC013-jsonb-pretty-document",
                  Run(registry, "sb.json.jsonb_pretty", {DocumentValue(R"({"a":1})")}),
                  "{\n  \"a\": 1\n}",
                  "character") && ok;
  ok = ExpectText("SBSQL-5157364BCB20 SBSFC013-jsonb-strip-nulls-generic",
                  Run(registry, "sb.json.jsonb_strip_nulls", {DocumentValue(R"({"a":null,"b":2})")}),
                  R"({"b":2})",
                  "json_document") && ok;
  ok = ExpectText("SBSQL-98D9B54A7630 SBSFC013-jsonb-strip-nulls-document",
                  Run(registry, "sb.json.jsonb_strip_nulls", {DocumentValue(R"({"a":1,"b":null})")}),
                  R"({"a":1})",
                  "json_document") && ok;

  return ok ? 0 : 1;
}
