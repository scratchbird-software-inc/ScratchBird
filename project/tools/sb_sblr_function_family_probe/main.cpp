// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "common/function_result_helpers.hpp"
#include "dispatch/function_dispatch.hpp"
#include "registry/function_seed_registry.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <set>
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
  request.context.sblr_context.transaction_uuid = "018f0000-0000-7000-8000-00000000bb01";
  request.context.sblr_context.user_uuid = "018f0000-0000-7000-8000-00000000aa01";
  request.context.sblr_context.current_timestamp = "2026-05-03T16:15:30Z";
  request.context.sblr_context.deterministic_random_u64 = 0x8000000000000000ull;
  request.context.sblr_context.deterministic_random_u64_present = true;
  for (std::size_t index = 0; index < values.size(); ++index) {
    request.arguments.push_back(fn::FunctionArgument{"arg" + std::to_string(index), std::move(values[index])});
  }
  return request;
}

fn::FunctionCallResult Call(const fn::FunctionRegistry& registry,
                            std::string function_id,
                            std::vector<sblr::SblrValue> values = {}) {
  return fn::DispatchFunctionCall(registry, Request(std::move(function_id), std::move(values)));
}

const sblr::SblrValue* Scalar(const fn::FunctionCallResult& result) {
  if (!result.result.ok() || result.result.scalar_values.empty()) return nullptr;
  return &result.result.scalar_values.front();
}

void ExpectOk(const fn::FunctionCallResult& result, std::string label, std::vector<std::string>* errors) {
  Expect(result.result.ok(), label + " did not return ok", errors);
  Expect(!result.result.scalar_values.empty(), label + " did not return a scalar result", errors);
}

void ExpectText(const fn::FunctionRegistry& registry,
                std::string function_id,
                std::vector<sblr::SblrValue> values,
                std::string expected,
                std::vector<std::string>* errors) {
  const auto result = Call(registry, function_id, std::move(values));
  ExpectOk(result, function_id, errors);
  const auto* scalar = Scalar(result);
  if (scalar != nullptr) Expect(fn::ValueAsText(*scalar) == expected, function_id + " text result mismatch", errors);
}

void ExpectInt(const fn::FunctionRegistry& registry,
               std::string function_id,
               std::vector<sblr::SblrValue> values,
               std::int64_t expected,
               std::vector<std::string>* errors) {
  const auto result = Call(registry, function_id, std::move(values));
  ExpectOk(result, function_id, errors);
  const auto* scalar = Scalar(result);
  if (scalar != nullptr) {
    Expect(scalar->has_int64_value, function_id + " result is not int64", errors);
    Expect(scalar->int64_value == expected, function_id + " int result mismatch", errors);
  }
}

void ExpectUint(const fn::FunctionRegistry& registry,
                std::string function_id,
                std::vector<sblr::SblrValue> values,
                std::uint64_t expected,
                std::vector<std::string>* errors) {
  const auto result = Call(registry, function_id, std::move(values));
  ExpectOk(result, function_id, errors);
  const auto* scalar = Scalar(result);
  if (scalar != nullptr) {
    Expect(scalar->has_uint64_value, function_id + " result is not uint64", errors);
    Expect(scalar->uint64_value == expected, function_id + " uint result mismatch", errors);
  }
}

void ExpectRealNear(const fn::FunctionRegistry& registry,
                    std::string function_id,
                    std::vector<sblr::SblrValue> values,
                    double expected,
                    std::vector<std::string>* errors) {
  const auto result = Call(registry, function_id, std::move(values));
  ExpectOk(result, function_id, errors);
  const auto* scalar = Scalar(result);
  if (scalar != nullptr) {
    Expect(scalar->has_real64_value, function_id + " result is not real64", errors);
    Expect(std::fabs(scalar->real64_value - expected) < 0.000001, function_id + " real result mismatch", errors);
  }
}

void ExpectRuntimeRefusal(const fn::FunctionRegistry& registry,
                          std::string function_id,
                          std::vector<std::string>* errors) {
  const auto result = Call(registry, function_id);
  Expect(!result.result.ok(), function_id + " should refuse at runtime", errors);
  Expect(!result.result.diagnostics.empty(), function_id + " refusal lacks diagnostic", errors);
  if (!result.result.diagnostics.empty()) {
    Expect(result.result.diagnostics.front().diagnostic_id == "SB_DIAG_FUNCTION_RUNTIME_REFUSAL",
           function_id + " refusal diagnostic mismatch", errors);
  }
}

}  // namespace

int main() {
  std::vector<std::string> errors;
  const auto package = fn::BuildStandardFunctionSeedPackage();
  const auto& registry = package.registry;
  std::set<std::string> families_seen;
  for (const auto& entry : registry.Entries()) families_seen.insert(entry.family);

  ExpectText(registry, "data.scalar.substring",
             {fn::MakeTextValue("character", "abcdef"), fn::MakeInt64Value("int64", 2), fn::MakeInt64Value("int64", 3)},
             "bcd", &errors);
  ExpectInt(registry, "data.aggregate.count", {fn::MakeInt64Value("int64", 1)}, 1, &errors);
  ExpectText(registry, "nosql.document.query",
             {fn::MakeTextValue("json_document", "{\"a\":1}"), fn::MakeTextValue("json_path", "$.a")},
             "1", &errors);
  ExpectText(registry, "nosql.kv.put",
             {fn::MakeTextValue("character", "k"), fn::MakeTextValue("character", "v")},
             "k", &errors);
  ExpectText(registry, "nosql.kv.get", {fn::MakeTextValue("character", "k")}, "v", &errors);
  ExpectText(registry, "nosql.graph.path",
             {fn::MakeTextValue("graph_node", "A"), fn::MakeTextValue("graph_node", "B")},
             "PATH(A->B)", &errors);
  ExpectText(registry, "nosql.graph.match",
             {fn::MakeTextValue("graph_query", "MATCH (n)")},
             "GRAPH_QUERY(MATCH (n))", &errors);
  ExpectText(registry, "search.query", {fn::MakeTextValue("character", "Red Bird")}, "[\"red\",\"bird\"]", &errors);
  ExpectRealNear(registry, "spatial.distance",
                 {fn::MakeTextValue("geometry", "POINT(0 0)"), fn::MakeTextValue("geometry", "POINT(3 4)")},
                 5.0, &errors);
  ExpectInt(registry, "timeseries.bucket",
            {fn::MakeInt64Value("int64", 1000), fn::MakeInt64Value("timestamp_epoch_ms", 3500)},
            3000, &errors);
  ExpectRealNear(registry, "vector.distance",
                 {fn::MakeTextValue("dense_vector", "[1,2]"), fn::MakeTextValue("dense_vector", "[4,6]")},
                 5.0, &errors);
  ExpectRuntimeRefusal(registry, "extension.catalog.list", &errors);
  ExpectRuntimeRefusal(registry, "management.udr.status", &errors);
  ExpectRuntimeRefusal(registry, "metrics.current.read", &errors);
  ExpectRuntimeRefusal(registry, "schema.ddl.create_schema", &errors);

  for (const auto& family : {"data.scalar", "data.aggregate", "nosql.document", "nosql.kv",
                             "nosql.graph", "search", "spatial", "timeseries", "vector",
                             "extension", "management", "metrics", "schema.ddl"}) {
    Expect(families_seen.count(family) == 1, std::string("registry family missing: ") + family, &errors);
  }

  std::cout << "{\n";
  std::cout << "  \"ok\": " << (errors.empty() ? "true" : "false") << ",\n";
  std::cout << "  \"probe\": \"sb_sblr_function_family_probe\",\n";
  std::cout << "  \"families_seen\": " << families_seen.size() << ",\n";
  std::cout << "  \"errors\": [";
  for (std::size_t index = 0; index < errors.size(); ++index) {
    if (index != 0) std::cout << ", ";
    std::cout << '"' << JsonEscape(errors[index]) << '"';
  }
  std::cout << "]\n}\n";
  return errors.empty() ? 0 : 1;
}
