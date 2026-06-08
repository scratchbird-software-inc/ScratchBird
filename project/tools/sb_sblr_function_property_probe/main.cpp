// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "common/function_result_helpers.hpp"
#include "dispatch/function_dispatch.hpp"
#include "metadata/function_hardening.hpp"
#include "registry/function_seed_registry.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace fn = scratchbird::engine::functions;
namespace sblr = scratchbird::engine::sblr;

namespace {

void Expect(bool condition, std::string message, std::vector<std::string>* errors) {
  if (!condition) errors->push_back(std::move(message));
}

std::string JsonEscape(std::string_view text) {
  std::string out;
  for (char ch : text) {
    switch (ch) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      default: out += ch; break;
    }
  }
  return out;
}

fn::FunctionCallRequest Request(std::string function_id, std::vector<sblr::SblrValue> values = {}) {
  fn::FunctionCallRequest request;
  request.context.function_id = std::move(function_id);
  request.context.security_allowed = true;
  request.context.policy_allowed = true;
  request.context.dependency_available = true;
  request.context.sblr_context.database_uuid = "018f0000-0000-7000-8000-00000000db01";
  request.context.sblr_context.user_uuid = "018f0000-0000-7000-8000-00000000aa01";
  request.context.sblr_context.transaction_uuid = "018f0000-0000-7000-8000-00000000bb01";
  request.context.sblr_context.current_timestamp = "2026-05-03T16:15:30Z";
  request.context.sblr_context.statement_timestamp = "2026-05-03T16:15:30Z";
  request.context.sblr_context.deterministic_random_u64 = 0x8000000000000000ull;
  request.context.sblr_context.deterministic_random_u64_present = true;
  for (std::size_t index = 0; index < values.size(); ++index) {
    request.arguments.push_back(fn::FunctionArgument{"arg" + std::to_string(index), std::move(values[index])});
  }
  return request;
}

const sblr::SblrValue* Scalar(const fn::FunctionCallResult& result) {
  if (!result.result.ok() || result.result.scalar_values.empty()) return nullptr;
  return &result.result.scalar_values.front();
}

void ExpectText(const fn::FunctionRegistry& registry,
                std::string function_id,
                std::vector<sblr::SblrValue> values,
                std::string expected,
                std::vector<std::string>* errors) {
  auto result = fn::DispatchFunctionCall(registry, Request(std::move(function_id), std::move(values)));
  const auto* scalar = Scalar(result);
  Expect(scalar != nullptr, "expected scalar result", errors);
  if (scalar != nullptr) Expect(fn::ValueAsText(*scalar) == expected, "unexpected scalar text result", errors);
}

void ExpectInt(const fn::FunctionRegistry& registry,
               std::string function_id,
               std::vector<sblr::SblrValue> values,
               std::int64_t expected,
               std::vector<std::string>* errors) {
  auto result = fn::DispatchFunctionCall(registry, Request(std::move(function_id), std::move(values)));
  const auto* scalar = Scalar(result);
  Expect(scalar != nullptr, "expected scalar integer result", errors);
  if (scalar != nullptr) {
    Expect(scalar->has_int64_value, "scalar result does not carry int64 payload", errors);
    Expect(scalar->int64_value == expected, "unexpected scalar integer result", errors);
  }
}

void ExpectFailureDiagnostic(const fn::FunctionRegistry& registry,
                             std::string function_id,
                             std::vector<sblr::SblrValue> values,
                             std::string expected_diagnostic,
                             std::vector<std::string>* errors) {
  auto result = fn::DispatchFunctionCall(registry, Request(std::move(function_id), std::move(values)));
  Expect(!result.result.ok(), "expected function failure", errors);
  Expect(!result.result.diagnostics.empty(), "expected diagnostic row", errors);
  if (!result.result.diagnostics.empty()) {
    Expect(result.result.diagnostics.front().diagnostic_id == expected_diagnostic,
           "unexpected failure diagnostic", errors);
  }
}

}  // namespace

int main() {
  std::vector<std::string> errors;
  auto package = fn::BuildStandardFunctionSeedPackage();
  const auto& registry = package.registry;

  ExpectText(registry, "data.scalar.lower", {fn::MakeTextValue("character", "AbC")}, "abc", &errors);
  ExpectText(registry, "data.scalar.upper", {fn::MakeTextValue("character", "AbC")}, "ABC", &errors);
  ExpectText(registry,
             "data.scalar.substring",
             {fn::MakeTextValue("character", "abcdef"), fn::MakeInt64Value("int64", 2), fn::MakeInt64Value("int64", 3)},
             "bcd",
             &errors);
  ExpectInt(registry,
            "data.scalar.cast",
            {fn::MakeTextValue("character", "42"), fn::MakeTextValue("descriptor", "int64")},
            42,
            &errors);
  ExpectText(registry,
             "data.scalar.cast",
             {fn::MakeInt64Value("int64", 42), fn::MakeTextValue("descriptor", "character")},
             "42",
             &errors);
  ExpectInt(registry,
            "data.scalar.extract",
            {fn::MakeTextValue("character", "year"), fn::MakeTextValue("timestamp_tz", "2026-05-03T16:15:30Z")},
            2026,
            &errors);
  ExpectInt(registry,
            "data.scalar.extract",
            {fn::MakeTextValue("character", "second"), fn::MakeTextValue("timestamp_tz", "2026-05-03T16:15:30Z")},
            30,
            &errors);
  ExpectFailureDiagnostic(registry,
                          "data.scalar.abs",
                          {fn::MakeInt64Value("int64", std::numeric_limits<std::int64_t>::min())},
                          "SB_DIAG_FUNCTION_NUMERIC_OVERFLOW",
                          &errors);
  ExpectText(registry,
             "nosql.document.get",
             {fn::MakeTextValue("json_document", "{\"a\":1,\"b\":\"x\"}"), fn::MakeTextValue("json_path", "$.a")},
             "1",
             &errors);

  const auto* lower_entry = registry.Lookup("data.scalar.lower");
  const auto* now_entry = registry.Lookup("data.scalar.now");
  const auto* json_entry = registry.Lookup("nosql.document.get");
  Expect(lower_entry != nullptr, "lower registry row missing", &errors);
  Expect(now_entry != nullptr, "now registry row missing", &errors);
  Expect(json_entry != nullptr, "document get registry row missing", &errors);
  if (lower_entry != nullptr) {
    Expect(!lower_entry->optimizer_metadata.collation_charset_timezone_rule.empty(),
           "lower must carry charset/collation rule metadata", &errors);
  }
  if (now_entry != nullptr) {
    Expect(now_entry->optimizer_metadata.determinism == fn::FunctionDeterminism::volatile_value,
           "now must be volatile and not foldable", &errors);
  }
  if (json_entry != nullptr) {
    Expect(json_entry->optimizer_metadata.descriptor_rule.find("json") != std::string::npos,
           "document get must carry JSON descriptor metadata", &errors);
  }

  const auto hardening = fn::ValidateFunctionCrossPlatformGate(registry);
  Expect(hardening.ok, "cross-platform metadata gate reported issues", &errors);

  std::cout << "{\n";
  std::cout << "  \"ok\": " << (errors.empty() ? "true" : "false") << ",\n";
  std::cout << "  \"probe\": \"sb_sblr_function_property_probe\",\n";
  std::cout << "  \"seed_functions\": " << registry.Entries().size() << ",\n";
  std::cout << "  \"errors\": [";
  for (std::size_t index = 0; index < errors.size(); ++index) {
    if (index != 0) std::cout << ", ";
    std::cout << '"' << JsonEscape(errors[index]) << '"';
  }
  std::cout << "]\n}\n";
  return errors.empty() ? 0 : 1;
}
