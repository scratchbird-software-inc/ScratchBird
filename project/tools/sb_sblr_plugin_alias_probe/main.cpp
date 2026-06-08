// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "common/function_result_helpers.hpp"
#include "dispatch/function_dispatch.hpp"
#include "metadata/function_parser_projection.hpp"
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

fn::FunctionCallRequest Request(std::string canonical_function_id, std::vector<sblr::SblrValue> values = {}) {
  fn::FunctionCallRequest request;
  request.context.function_id = std::move(canonical_function_id);
  request.context.security_allowed = true;
  request.context.policy_allowed = true;
  request.context.dependency_available = true;
  request.context.sblr_context.database_uuid = "018f0000-0000-7000-8000-00000000db03";
  request.context.sblr_context.transaction_uuid = "018f0000-0000-7000-8000-00000000bb03";
  request.context.sblr_context.user_uuid = "018f0000-0000-7000-8000-00000000aa03";
  for (std::size_t index = 0; index < values.size(); ++index) {
    request.arguments.push_back(fn::FunctionArgument{"arg" + std::to_string(index), std::move(values[index])});
  }
  return request;
}

const fn::FunctionParserProjectionRow* FindAlias(const std::vector<fn::FunctionParserProjectionRow>& rows,
                                                 std::string_view alias_name) {
  for (const auto& row : rows) {
    if (row.alias_name == alias_name) return &row;
  }
  return nullptr;
}

fn::FunctionCallResult CallAlias(const fn::FunctionRegistry& registry,
                                 const std::vector<fn::FunctionParserProjectionRow>& rows,
                                 std::string_view alias_name,
                                 std::vector<sblr::SblrValue> values,
                                 std::vector<std::string>* errors) {
  const auto* row = FindAlias(rows, alias_name);
  Expect(row != nullptr, std::string(alias_name) + " projection row is missing", errors);
  if (row == nullptr) return {};
  Expect(row->alias_source == fn::FunctionAliasSource::plugin_extension,
         std::string(alias_name) + " is not classified as plugin/extension alias", errors);
  Expect(row->projection_state == "available",
         std::string(alias_name) + " is not available through projection", errors);
  Expect(!row->function_uuid.empty(), std::string(alias_name) + " metadata-visible projection lacks UUID", errors);
  return fn::DispatchFunctionCall(registry, Request(row->canonical_function_id, std::move(values)));
}

const sblr::SblrValue* Scalar(const fn::FunctionCallResult& result) {
  if (!result.result.ok() || result.result.scalar_values.empty()) return nullptr;
  return &result.result.scalar_values.front();
}

void ExpectRealAlias(const fn::FunctionRegistry& registry,
                     const std::vector<fn::FunctionParserProjectionRow>& rows,
                     std::string alias_name,
                     std::vector<sblr::SblrValue> values,
                     double expected,
                     std::vector<std::string>* errors) {
  const auto result = CallAlias(registry, rows, alias_name, std::move(values), errors);
  Expect(result.result.ok(), alias_name + " canonical dispatch failed", errors);
  const auto* scalar = Scalar(result);
  Expect(scalar != nullptr, alias_name + " returned no scalar value", errors);
  if (scalar != nullptr) {
    Expect(scalar->has_real64_value, alias_name + " result is not real64", errors);
    Expect(std::fabs(scalar->real64_value - expected) < 0.000001, alias_name + " real result mismatch", errors);
  }
}

void ExpectIntAlias(const fn::FunctionRegistry& registry,
                    const std::vector<fn::FunctionParserProjectionRow>& rows,
                    std::string alias_name,
                    std::vector<sblr::SblrValue> values,
                    std::int64_t expected,
                    std::vector<std::string>* errors) {
  const auto result = CallAlias(registry, rows, alias_name, std::move(values), errors);
  Expect(result.result.ok(), alias_name + " canonical dispatch failed", errors);
  const auto* scalar = Scalar(result);
  Expect(scalar != nullptr, alias_name + " returned no scalar value", errors);
  if (scalar != nullptr) {
    Expect(scalar->has_int64_value, alias_name + " result is not int64-compatible", errors);
    Expect(scalar->int64_value == expected, alias_name + " int result mismatch", errors);
  }
}

}  // namespace

int main() {
  std::vector<std::string> errors;
  const auto package = fn::BuildStandardFunctionSeedPackage();
  const auto& registry = package.registry;
  fn::FunctionParserProjectionRequest projection_request;
  projection_request.parser_profile = "plugin-alias-conformance";
  projection_request.metadata_visible = true;
  projection_request.include_disabled = false;
  const auto rows = fn::BuildFunctionParserProjection(registry, projection_request);
  std::set<std::string> plugin_packages;
  for (const auto& row : rows) {
    if (row.alias_source == fn::FunctionAliasSource::plugin_extension) plugin_packages.insert(row.source_package);
  }

  ExpectRealAlias(registry,
                  rows,
                  "POSTGIS.ST_Distance",
                  {fn::MakeTextValue("geometry", "POINT(0 0)"), fn::MakeTextValue("geometry", "POINT(3 4)")},
                  5.0,
                  &errors);
  ExpectIntAlias(registry,
                 rows,
                 "POSTGIS.ST_Contains",
                 {fn::MakeTextValue("geometry", "POINT(1 1)"), fn::MakeTextValue("geometry", "POINT(1 1)")},
                 1,
                 &errors);
  ExpectRealAlias(registry,
                  rows,
                  "PGVECTOR.vector_l2_distance",
                  {fn::MakeTextValue("dense_vector", "[1,2]"), fn::MakeTextValue("dense_vector", "[4,6]")},
                  5.0,
                  &errors);
  ExpectRealAlias(registry,
                  rows,
                  "PG_TRGM.similarity",
                  {fn::MakeTextValue("character", "red red bird"), fn::MakeTextValue("character", "red")},
                  2.0,
                  &errors);
  ExpectIntAlias(registry,
                 rows,
                 "TIMESCALEDB.time_bucket",
                 {fn::MakeInt64Value("int64", 1000), fn::MakeInt64Value("timestamp_epoch_ms", 3500)},
                 3000,
                 &errors);

  for (const auto& package_name : {"postgis", "pgvector", "pg_trgm", "timescaledb"}) {
    Expect(plugin_packages.count(package_name) == 1,
           std::string("plugin package missing from projection: ") + package_name,
           &errors);
  }

  std::cout << "{\n";
  std::cout << "  \"ok\": " << (errors.empty() ? "true" : "false") << ",\n";
  std::cout << "  \"probe\": \"sb_sblr_plugin_alias_probe\",\n";
  std::cout << "  \"projection_rows\": " << rows.size() << ",\n";
  std::cout << "  \"plugin_packages\": " << plugin_packages.size() << ",\n";
  std::cout << "  \"errors\": [";
  for (std::size_t index = 0; index < errors.size(); ++index) {
    if (index != 0) std::cout << ", ";
    std::cout << '"' << JsonEscape(errors[index]) << '"';
  }
  std::cout << "]\n}\n";
  return errors.empty() ? 0 : 1;
}
