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

#include <cstdint>
#include <iostream>
#include <map>
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

fn::FunctionCallRequest Request(std::string canonical_function_id,
                                std::vector<sblr::SblrValue> values = {},
                                bool last_identity_present = false) {
  fn::FunctionCallRequest request;
  request.context.function_id = std::move(canonical_function_id);
  request.context.security_allowed = true;
  request.context.policy_allowed = true;
  request.context.dependency_available = true;
  request.context.sblr_context.database_uuid = "018f0000-0000-7000-8000-00000000db02";
  request.context.sblr_context.transaction_uuid = "018f0000-0000-7000-8000-00000000bb02";
  request.context.sblr_context.user_uuid = "018f0000-0000-7000-8000-00000000aa02";
  request.context.sblr_context.last_identity_value = "77";
  request.context.sblr_context.last_identity_value_present = last_identity_present;
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
                                 std::vector<std::string>* errors,
                                 bool last_identity_present = false) {
  const auto* row = FindAlias(rows, alias_name);
  Expect(row != nullptr, std::string(alias_name) + " projection row is missing", errors);
  if (row == nullptr) return {};
  Expect(row->alias_source == fn::FunctionAliasSource::donor,
         std::string(alias_name) + " is not classified as donor alias", errors);
  Expect(row->projection_state == "available",
         std::string(alias_name) + " is not available through projection", errors);
  Expect(!row->function_uuid.empty(), std::string(alias_name) + " metadata-visible projection lacks UUID", errors);
  return fn::DispatchFunctionCall(registry,
                                  Request(row->canonical_function_id, std::move(values), last_identity_present));
}

const sblr::SblrValue* Scalar(const fn::FunctionCallResult& result) {
  if (!result.result.ok() || result.result.scalar_values.empty()) return nullptr;
  return &result.result.scalar_values.front();
}

void ExpectTextAlias(const fn::FunctionRegistry& registry,
                     const std::vector<fn::FunctionParserProjectionRow>& rows,
                     std::string alias_name,
                     std::vector<sblr::SblrValue> values,
                     std::string expected,
                     std::vector<std::string>* errors) {
  const auto result = CallAlias(registry, rows, alias_name, std::move(values), errors);
  Expect(result.result.ok(), alias_name + " canonical dispatch failed", errors);
  const auto* scalar = Scalar(result);
  Expect(scalar != nullptr, alias_name + " returned no scalar value", errors);
  if (scalar != nullptr) Expect(fn::ValueAsText(*scalar) == expected, alias_name + " result mismatch", errors);
}

void ExpectIntAlias(const fn::FunctionRegistry& registry,
                    const std::vector<fn::FunctionParserProjectionRow>& rows,
                    std::string alias_name,
                    std::vector<sblr::SblrValue> values,
                    std::int64_t expected,
                    std::vector<std::string>* errors,
                    bool last_identity_present = false) {
  const auto result = CallAlias(registry, rows, alias_name, std::move(values), errors, last_identity_present);
  Expect(result.result.ok(), alias_name + " canonical dispatch failed", errors);
  const auto* scalar = Scalar(result);
  Expect(scalar != nullptr, alias_name + " returned no scalar value", errors);
  if (scalar != nullptr) {
    Expect(scalar->has_int64_value || !fn::ValueAsText(*scalar).empty(), alias_name + " result lacks numeric payload/text", errors);
    const auto text = fn::ValueAsText(*scalar);
    Expect(text == std::to_string(expected), alias_name + " integer result mismatch", errors);
  }
}

}  // namespace

int main() {
  std::vector<std::string> errors;
  const auto package = fn::BuildStandardFunctionSeedPackage();
  const auto& registry = package.registry;
  fn::FunctionParserProjectionRequest projection_request;
  projection_request.parser_profile = "donor-alias-conformance";
  projection_request.metadata_visible = true;
  projection_request.include_disabled = false;
  const auto rows = fn::BuildFunctionParserProjection(registry, projection_request);
  std::set<std::string> donor_packages;
  for (const auto& row : rows) {
    if (row.alias_source == fn::FunctionAliasSource::donor) donor_packages.insert(row.source_package);
  }

  ExpectTextAlias(registry, rows, "POSTGRES.substring",
                  {fn::MakeTextValue("character", "abcdef"), fn::MakeInt64Value("int64", 2), fn::MakeInt64Value("int64", 3)},
                  "bcd", &errors);
  ExpectTextAlias(registry, rows, "POSTGRES.lower", {fn::MakeTextValue("character", "AbC")}, "abc", &errors);
  ExpectTextAlias(registry, rows, "MYSQL.LCASE", {fn::MakeTextValue("character", "AbC")}, "abc", &errors);
  ExpectTextAlias(registry, rows, "MYSQL.JSON_EXTRACT",
                  {fn::MakeTextValue("json_document", "{\"a\":1}"), fn::MakeTextValue("json_path", "$.a")},
                  "1", &errors);
  ExpectTextAlias(registry, rows, "MYSQL.JSON_SET",
                  {fn::MakeTextValue("json_document", "{}"), fn::MakeTextValue("json_path", "$.a"), fn::MakeTextValue("json", "1")},
                  "{\"a\":1}", &errors);
  ExpectTextAlias(registry, rows, "MYSQL.JSON_REMOVE",
                  {fn::MakeTextValue("json_document", "{\"a\":1,\"b\":2}"), fn::MakeTextValue("json_path", "$.a")},
                  "{\"b\":2}", &errors);
  ExpectTextAlias(registry, rows, "MYSQL.GROUP_CONCAT", {fn::MakeTextValue("character", "x")}, "x", &errors);
  ExpectIntAlias(registry, rows, "FIREBIRD.GEN_ID",
                 {fn::MakeTextValue("uuid", "018f0000-0000-7000-8000-00000000f001"), fn::MakeInt64Value("int64", 5)},
                 1, &errors);
  ExpectIntAlias(registry, rows, "FIREBIRD.NEXT_VALUE_FOR",
                 {fn::MakeTextValue("uuid", "018f0000-0000-7000-8000-00000000f002")},
                 1, &errors);
  ExpectTextAlias(registry, rows, "SQLITE.json_extract",
                  {fn::MakeTextValue("json_document", "{\"a\":1}"), fn::MakeTextValue("json_path", "$.a")},
                  "1", &errors);
  ExpectIntAlias(registry, rows, "SQLITE.last_insert_rowid", {}, 77, &errors, true);
  ExpectTextAlias(registry, rows, "REDIS.SET",
                  {fn::MakeTextValue("character", "key"), fn::MakeTextValue("character", "value")},
                  "key", &errors);
  ExpectTextAlias(registry, rows, "REDIS.GET", {fn::MakeTextValue("character", "key")}, "value", &errors);
  ExpectIntAlias(registry, rows, "REDIS.DEL", {fn::MakeTextValue("character", "key")}, 1, &errors);
  ExpectTextAlias(registry, rows, "NEO4J.shortestPath",
                  {fn::MakeTextValue("graph_node", "A"), fn::MakeTextValue("graph_node", "B")},
                  "PATH(A->B)", &errors);
  ExpectTextAlias(registry, rows, "OPENSEARCH.match", {fn::MakeTextValue("character", "red bird")}, "[\"red\",\"bird\"]", &errors);

  for (const auto& package_name : {"postgresql", "mysql", "firebird", "sqlite", "redis", "neo4j", "opensearch"}) {
    Expect(donor_packages.count(package_name) == 1, std::string("donor package missing from projection: ") + package_name, &errors);
  }
  const auto authority = fn::ValidateFunctionParserProjectionAuthority(projection_request);
  Expect(authority.allowed, "metadata-only donor projection authority was refused", &errors);
  projection_request.parser_claims_execution_authority = true;
  const auto denied = fn::ValidateFunctionParserProjectionAuthority(projection_request);
  Expect(!denied.allowed && denied.diagnostic_id == "SB_DIAG_PARSER_EXECUTION_AUTHORITY_DENIED",
         "parser execution authority claim was not denied", &errors);

  std::cout << "{\n";
  std::cout << "  \"ok\": " << (errors.empty() ? "true" : "false") << ",\n";
  std::cout << "  \"probe\": \"sb_sblr_donor_alias_probe\",\n";
  std::cout << "  \"projection_rows\": " << rows.size() << ",\n";
  std::cout << "  \"donor_packages\": " << donor_packages.size() << ",\n";
  std::cout << "  \"errors\": [";
  for (std::size_t index = 0; index < errors.size(); ++index) {
    if (index != 0) std::cout << ", ";
    std::cout << '"' << JsonEscape(errors[index]) << '"';
  }
  std::cout << "]\n}\n";
  return errors.empty() ? 0 : 1;
}
