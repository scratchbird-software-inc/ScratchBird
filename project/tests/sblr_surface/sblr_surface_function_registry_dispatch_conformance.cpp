// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dispatch/function_dispatch.hpp"
#include "common/function_result_helpers.hpp"
#include "registry/function_seed_registry.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace functions = scratchbird::engine::functions;
namespace sblr = scratchbird::engine::sblr;

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) Fail(message);
}

bool Contains(std::string_view text, std::string_view needle) {
  return text.find(needle) != std::string_view::npos;
}

bool IsGenericMissingHandlerDiagnostic(const sblr::SblrRuntimeDiagnostic& diagnostic) {
  return diagnostic.diagnostic_id == "SB_DIAG_FUNCTION_FAMILY_HANDLER_MISSING" ||
         Contains(diagnostic.diagnostic_id, "_FUNCTION_UNHANDLED") ||
         Contains(diagnostic.detail, "function family does not have a dispatch handler") ||
         Contains(diagnostic.detail, "not handled by the activated") ||
         Contains(diagnostic.detail, "not implemented");
}

functions::FunctionCallRequest RequestFor(const functions::FunctionRegistryEntry& entry) {
  functions::FunctionCallRequest request;
  request.context.function_id = entry.function_id;
  request.context.security_allowed = true;
  request.context.policy_allowed = true;
  request.context.dependency_available = true;
  request.context.sblr_context.cluster_uuid = "ELER-074-function-registry-cluster";
  request.context.sblr_context.node_uuid = "ELER-074-function-registry-node";
  request.context.sblr_context.database_uuid = "ELER-074-function-registry-db";
  request.context.sblr_context.transaction_uuid = "ELER-074-function-registry-tx";
  request.context.sblr_context.statement_uuid = "ELER-074-function-registry-stmt";
  request.context.sblr_context.user_uuid = "ELER-074-function-registry-user";
  request.context.sblr_context.current_role_uuid = "ELER-074-function-registry-role";
  request.context.sblr_context.current_schema_uuid = "ELER-074-function-registry-schema";
  request.context.sblr_context.transaction_context_present = true;
  request.context.sblr_context.security_context_present = true;
  request.context.sblr_context.current_timestamp = "2026-06-03T12:00:00Z";
  request.context.sblr_context.statement_timestamp = "2026-06-03T12:00:00Z";
  request.context.sblr_context.transaction_timestamp = "2026-06-03T12:00:00Z";
  request.context.sblr_context.deterministic_random_u64 = 42;
  request.context.sblr_context.deterministic_random_u64_present = true;
  request.context.sblr_context.deterministic_random_bytes_hex = "00112233445566778899aabbccddeeff";
  request.context.sblr_context.deterministic_uuid_text = "019dffbb-f000-7000-8000-000000000001";
  return request;
}

functions::FunctionArgument TextArg(std::string value, std::string descriptor = "character") {
  return functions::FunctionArgument{"", functions::MakeTextValue(std::move(descriptor), std::move(value))};
}

functions::FunctionArgument IntArg(std::int64_t value, std::string descriptor = "int64") {
  return functions::FunctionArgument{"", functions::MakeInt64Value(std::move(descriptor), value)};
}

sblr::SblrResult DispatchById(const functions::FunctionRegistry& registry,
                              std::string_view function_id,
                              std::vector<functions::FunctionArgument> arguments = {}) {
  const auto* entry = registry.Lookup(std::string(function_id));
  Require(entry != nullptr, "missing function registry entry for " + std::string(function_id));
  auto request = RequestFor(*entry);
  request.arguments = std::move(arguments);
  return functions::DispatchFunctionCall(registry, std::move(request)).result;
}

const sblr::SblrValue& ScalarAt(const sblr::SblrResult& result, std::size_t index, std::string_view function_id) {
  Require(result.ok(), std::string(function_id) + " did not succeed");
  Require(result.scalar_values.size() > index, std::string(function_id) + " returned too few scalar values");
  return result.scalar_values[index];
}

void RequireDiagnostic(const sblr::SblrResult& result,
                       sblr::SblrStatusCode status,
                       std::string_view diagnostic_id,
                       std::string_view function_id) {
  Require(!result.ok(), std::string(function_id) + " unexpectedly succeeded");
  Require(result.status == status, std::string(function_id) + " returned the wrong refusal status");
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.diagnostic_id == diagnostic_id) return;
  }
  Fail(std::string(function_id) + " did not return diagnostic " + std::string(diagnostic_id));
}

void RequireConcreteFamilyProbeCoverage(const functions::FunctionRegistry& registry) {
  {
    const auto result = DispatchById(registry,
                                     "sb.fn.security.sb_crypto_hmac",
                                     {TextArg("key"), TextArg("The quick brown fox jumps over the lazy dog")});
    Require(ScalarAt(result, 0, "sb.fn.security.sb_crypto_hmac").text_value ==
                "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8",
            "sb_crypto_hmac did not produce the HMAC-SHA256 known-answer value");
  }
  RequireDiagnostic(DispatchById(registry,
                                 "sb.fn.security.sb_connector_external_query",
                                 {TextArg("select 1")}),
                    sblr::SblrStatusCode::policy_refused,
                    "SB_DIAG_SECURITY_CONNECTOR_EXTERNAL_QUERY_POLICY_REQUIRED",
                    "sb.fn.security.sb_connector_external_query");

  {
    const auto result = DispatchById(registry,
                                     "sb.fn.nosql.document.sb_json_each",
                                     {TextArg("{\"a\":1}")});
    Require(Contains(ScalarAt(result, 0, "sb.fn.nosql.document.sb_json_each").text_value, "\"key\":\"a\""),
            "sb_json_each alias did not produce JSON key rows");
  }
  {
    const auto result = DispatchById(registry,
                                     "sb.fn.nosql.graph.graph_algorithm.path_shortest",
                                     {TextArg("A"), TextArg("B")});
    Require(ScalarAt(result, 0, "sb.fn.nosql.graph.graph_algorithm.path_shortest").text_value == "PATH(A->B)",
            "graph path_shortest did not produce deterministic path output");
  }
  {
    const auto result = DispatchById(registry,
                                     "sb.fn.search.search_query.query_match",
                                     {TextArg("alpha beta alpha"), TextArg("beta")});
    Require(ScalarAt(result, 0, "sb.fn.search.search_query.query_match").int64_value == 1,
            "search query_match did not detect the term");
  }
  {
    const auto result = DispatchById(registry,
                                     "sb.fn.search.search_rank.rank_bm25",
                                     {TextArg("alpha beta alpha"), TextArg("alpha")});
    Require(ScalarAt(result, 0, "sb.fn.search.search_rank.rank_bm25").real64_value >= 2.0,
            "search rank_bm25 did not score repeated terms");
  }
  {
    const auto result = DispatchById(registry,
                                     "sb.fn.spatial.spatial_construct.construct_point",
                                     {IntArg(1), IntArg(2)});
    Require(ScalarAt(result, 0, "sb.fn.spatial.spatial_construct.construct_point").text_value == "POINT(1 2)",
            "spatial construct_point did not produce POINT WKT");
  }
  {
    const auto result = DispatchById(registry,
                                     "sb.fn.spatial.spatial_predicate.predicate_intersects",
                                     {TextArg("POINT(1 1)", "geometry"), TextArg("POINT(1 1)", "geometry")});
    Require(ScalarAt(result, 0, "sb.fn.spatial.spatial_predicate.predicate_intersects").int64_value == 1,
            "spatial predicate_intersects did not recognize coincident points");
  }
  {
    const auto result = DispatchById(registry,
                                     "sb.fn.timeseries.time_bucket.bucket_fixed_interval",
                                     {IntArg(1000), IntArg(2501)});
    Require(ScalarAt(result, 0, "sb.fn.timeseries.time_bucket.bucket_fixed_interval").int64_value == 2000,
            "timeseries bucket_fixed_interval did not truncate epoch milliseconds");
  }
  {
    const auto result = DispatchById(registry,
                                     "sb.fn.vector.vector_distance.distance_l1",
                                     {TextArg("[1,2,3]", "dense_vector"), TextArg("[2,4,6]", "dense_vector")});
    Require(ScalarAt(result, 0, "sb.fn.vector.vector_distance.distance_l1").real64_value == 6.0,
            "vector distance_l1 did not compute Manhattan distance");
  }
  {
    const auto result = DispatchById(registry,
                                     "sb.fn.vector.vector_distance.distance_jaccard",
                                     {TextArg("[1,0,1]", "dense_vector"), TextArg("[1,1,0]", "dense_vector")});
    const double distance = ScalarAt(result, 0, "sb.fn.vector.vector_distance.distance_jaccard").real64_value;
    Require(distance > 0.66 && distance < 0.67, "vector distance_jaccard did not compute set distance");
  }
  {
    const auto result = DispatchById(registry,
                                     "sb.fn.data.aggregate.sketch_bloom.bloom_create",
                                     {IntArg(128), IntArg(3)});
    Require(Contains(ScalarAt(result, 0, "sb.fn.data.aggregate.sketch_bloom.bloom_create").text_value, "bloom.create"),
            "bloom_create did not produce a sketch descriptor");
  }
  {
    const auto result = DispatchById(registry,
                                     "sb.fn.data.aggregate.sketch_bloom.bloom_contains",
                                     {TextArg("bloom.add[alpha]"), TextArg("alpha")});
    Require(ScalarAt(result, 0, "sb.fn.data.aggregate.sketch_bloom.bloom_contains").int64_value == 1,
            "bloom_contains did not recognize descriptor membership");
  }
}

}  // namespace

int main() {
  const auto package = functions::BuildStandardFunctionSeedPackage();
  const auto closure_errors = functions::ValidateFunctionRegistryForClosure(package.registry);
  if (!closure_errors.empty()) {
    std::cerr << "function registry closure validation failed:\n";
    for (const auto& error : closure_errors) std::cerr << "  " << error << '\n';
    return EXIT_FAILURE;
  }

  auto entries = package.registry.Entries();
  std::sort(entries.begin(), entries.end(), [](const auto& lhs, const auto& rhs) {
    return lhs.function_id < rhs.function_id;
  });

  std::size_t final_rows = 0;
  std::size_t successful_rows = 0;
  std::size_t invalid_input_rows = 0;
  std::size_t exact_refusal_rows = 0;
  std::vector<std::string> failures;

  for (const auto& entry : entries) {
    if (!functions::IsFinalFunctionImplementationState(entry.implementation_state)) continue;
    ++final_rows;

    const auto result = functions::DispatchFunctionCall(package.registry, RequestFor(entry)).result;
    if (result.ok()) {
      ++successful_rows;
      continue;
    }

    if (result.diagnostics.empty()) {
      failures.push_back(entry.function_id + ": failure had no diagnostic");
      continue;
    }

    bool generic_missing_handler = false;
    bool invalid_input = false;
    for (const auto& diagnostic : result.diagnostics) {
      generic_missing_handler = generic_missing_handler || IsGenericMissingHandlerDiagnostic(diagnostic);
      invalid_input = invalid_input || diagnostic.diagnostic_id == "SB_DIAG_FUNCTION_INVALID_INPUT";
    }
    if (generic_missing_handler) {
      failures.push_back(entry.function_id + ": final registry row reached missing or unhandled function dispatch");
      continue;
    }
    if (invalid_input) {
      ++invalid_input_rows;
    } else {
      ++exact_refusal_rows;
    }
  }

  Require(entries.size() >= 300, "function registry row count unexpectedly small");
  Require(final_rows >= 300, "final function registry row count unexpectedly small");
  RequireConcreteFamilyProbeCoverage(package.registry);
  if (!failures.empty()) {
    std::cerr << "function registry dispatch closure failures:\n";
    for (const auto& failure : failures) std::cerr << "  " << failure << '\n';
    return EXIT_FAILURE;
  }

  std::cout << "sblr_surface_function_registry_dispatch_conformance=passed\n"
            << "registry_rows=" << entries.size() << '\n'
            << "final_rows=" << final_rows << '\n'
            << "successful_rows=" << successful_rows << '\n'
            << "invalid_input_rows=" << invalid_input_rows << '\n'
            << "exact_refusal_rows=" << exact_refusal_rows << '\n';
  return EXIT_SUCCESS;
}
