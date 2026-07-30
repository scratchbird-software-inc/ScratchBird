// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: QOW_CT004_ASSERTION_ROUTE_REMOVAL_DIRECT_GATE_V1

#include "query/plan_api.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifndef QOW_CT004_SOURCE_ROOT
#error "QOW_CT004_SOURCE_ROOT must identify the ScratchBird project source root"
#endif

namespace api = scratchbird::engine::internal_api;

namespace {

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "QOW-CT-004 failure: " << message << '\n';
    std::exit(1);
  }
}

std::string ReadSource(std::string_view relative_path) {
  const auto path = std::filesystem::path(QOW_CT004_SOURCE_ROOT) /
                    std::filesystem::path(relative_path);
  std::ifstream input(path, std::ios::binary);
  Require(input.good(), "required production source is readable");
  std::ostringstream contents;
  contents << input.rdbuf();
  Require(input.good() || input.eof(), "required production source was read");
  return contents.str();
}

std::string LowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

std::size_t Count(std::string_view haystack, std::string_view needle) {
  Require(!needle.empty(), "count needle is nonempty");
  std::size_t count = 0;
  for (std::size_t offset = 0;
       (offset = haystack.find(needle, offset)) != std::string_view::npos;
       offset += needle.size()) {
    ++count;
  }
  return count;
}

void RemoveOnce(std::string* source, std::string_view allowed) {
  Require(source != nullptr, "source removal target exists");
  Require(Count(*source, allowed) == 1,
          "approved refusal marker occurs exactly once");
  source->erase(source->find(allowed), allowed.size());
}

void RequireAbsent(std::string_view source,
                   const std::vector<std::string_view>& retired_markers) {
  for (const auto marker : retired_markers) {
    Require(source.find(marker) == std::string_view::npos,
            "retired production route marker is absent");
  }
}

}  // namespace

// QOW-TEST-ASSERTION-ROUTE-REMOVAL-V1
// QOW-CTEST-ASSERTION-ROUTE-REMOVAL-V1
int main() {
  const auto lowering = ReadSource("src/parsers/sbsql_worker/lowering/lowering.cpp");
  const auto plan_header = ReadSource("src/engine/internal_api/query/plan_api.hpp");
  const auto plan = ReadSource("src/engine/internal_api/query/plan_api.cpp");
  const auto sblr_dispatch = ReadSource("src/engine/sblr/sblr_dispatch.cpp");
  const auto select = ReadSource("src/engine/internal_api/dml/select_api.cpp");
  const auto show = ReadSource("src/engine/internal_api/observability/show_api.cpp");
  const auto server = ReadSource("src/server/sblr_dispatch_server.cpp");

  for (const auto* strict_source :
       {&lowering, &plan_header, &sblr_dispatch, &select, &show}) {
    Require(LowerAscii(*strict_source).find("assertion") == std::string::npos,
            "assertion-only terminology is absent from strict production paths");
  }

  RequireAbsent(lowering, {
      "ConsumeSelectCountAssertionProjection",
      "ConsumeSelectFieldAssertionProjection",
      "ConsumeSelectAggregateAssertionProjection",
      "ConsumeSelectJoinCountAssertionProjection",
      "ConsumeSelectFieldCountAssertionProjection",
      "ConsumeCteAggregateAssertionProjection",
      "IsSupportedRecursiveCteAggregateAssertion",
      "AnalyzeJoinGroupAggregateAssertionRoute",
      "AnalyzeJoinWindowMaxAssertionRoute",
      "ConsumeGeneratedWindowLiteral",
      "ConsumeGeneratedWindowScalarSubquery",
      "ConsumeGeneratedWindowFinalSelect",
      "ConsumeGeneratedFrameInputCte",
      "ConsumeGeneratedWindowFunctionCall",
      "ConsumeGeneratedWindowedCte",
      "ConsumeGeneratedWindowCteSequence"});

  RequireAbsent(plan, {
      "CountAssertionResultShape",
      "NumericAssertionResultShape",
      "ValueAssertionResultShape",
      "EvaluateRecursiveCteAggregateAssertion",
      "RelationWithConstantGroupKey",
      "EmptyAggregateAssertionValue",
      "FirstAggregatePayloadValue",
      "OrderedSetHypotheticalAssertionShape",
      "MaterializedAggregateAssertionShape",
      "MaterializedWindowAssertionShape",
      "join_group_sum_assertion",
      "join_window_max_assertion",
      "grouping_sets_grand_total_assertion"});

  constexpr std::string_view expectation_fields[] = {
      "assertion_id", "actual_source_column", "actual_column_name",
      "expected_column_name", "expected_count", "expected_value",
      "expected_value_is_null", "count_compare_op", "count_compare_value"};
  for (const auto field : expectation_fields) {
    Require(Count(plan, std::string(field) + ":") == 1,
            "engine expectation field exists only in the refusal guard");
    Require(Count(server, std::string("\"") + std::string(field) + "\"") == 1,
            "server expectation field exists only in the refusal guard");
  }

  Require(Count(plan, "RetiredResultExpectationRequested(request)") == 2,
          "both public plan entry paths apply the retirement guard");
  Require(Count(plan, "query_plan_retired_result_expectation_refused") == 2,
          "both public plan entry paths fail closed");

  auto approved_plan_text = LowerAscii(plan);
  RemoveOnce(&approved_plan_text, "assertion branches");
  RemoveOnce(&approved_plan_text, "window_assertion");
  RemoveOnce(&approved_plan_text, "_assertion");
  RemoveOnce(&approved_plan_text, "assertion_id:");
  RemoveOnce(&approved_plan_text, "assertion-only");
  Require(approved_plan_text.find("assertion") == std::string::npos,
          "plan assertion references are limited to explicit refusal logic");

  auto approved_server_text = LowerAscii(server);
  RemoveOnce(&approved_server_text, "assertion_id");
  for (const auto result_shape : {"count_assertion", "field_assertion",
                                  "aggregate_assertion", "window_assertion"}) {
    RemoveOnce(&approved_server_text, result_shape);
  }
  Require(approved_server_text.find("assertion") == std::string::npos,
          "server assertion references are limited to explicit refusal logic");

  Require(lowering.find(R"(\"result_projection\":\"count\")") !=
              std::string::npos,
          "ordinary count projection remains serialized");
  Require(lowering.find(R"(\"result_projection\":\"rowset\")") !=
              std::string::npos,
          "ordinary rowset projection remains serialized");
  Require(lowering.find(R"(\"aggregate_function\":\"sb.aggregate.count\")") !=
              std::string::npos,
          "ordinary aggregate count remains serialized");
  Require(plan.find("{\"query_join_result_projection\", \"count\"}") !=
              std::string::npos,
          "ordinary engine join count remains executable");
  Require(plan.find("{\"query_join_result_projection\", \"rowset\"}") !=
              std::string::npos,
          "ordinary engine join rowset remains executable");
  Require(select.find("retired_result_shape_not_supported") !=
              std::string::npos,
          "DML result-shape boundary fails closed");
  Require(show.find("SBSQL.CATALOG.RETIRED_RESULT_SHAPE") !=
              std::string::npos,
          "catalog result-shape boundary fails closed");
  Require(server.find("PARSER_SERVER_IPC.RETIRED_RESULT_SHAPE") !=
              std::string::npos,
          "server result-shape boundary fails closed");
  Require(server.find("lowered_inspection_text") != std::string::npos,
          "server refusal inspection is case insensitive");

  const auto ordinary_rowset =
      api::RefuseRetiredResultExpectationRoute("inner_join", "rowset", {});
  const auto ordinary_count =
      api::RefuseRetiredResultExpectationRoute("materialized_cte", "count", {});
  Require(!ordinary_rowset.applies && !ordinary_rowset.accepted &&
              ordinary_rowset.diagnostic_code.empty(),
          "ordinary rowset remains outside the retired-result refusal");
  Require(!ordinary_count.applies && !ordinary_count.accepted &&
              ordinary_count.diagnostic_code.empty(),
          "ordinary count remains outside the retired-result refusal");

  const std::vector<std::vector<std::string>> forged_options = {
      {"assertion_id:forged"},
      {"actual_source_column:value"},
      {"actual_column_name:actual_value"},
      {"expected_column_name:expected_value"},
      {"expected_count:1"},
      {"expected_value:1"},
      {"expected_value_is_null:true"},
      {"count_compare_op:="},
      {"count_compare_value:1"},
      {"EXPECTED_COUNT:1"}};
  for (const auto& options : forged_options) {
    const auto refused =
        api::RefuseRetiredResultExpectationRoute("inner_join", "rowset", options);
    Require(refused.applies && !refused.accepted &&
                refused.diagnostic_code ==
                    "QOW-DIAG-IAS-003-RETIRED-RESULT-EXPECTATION-V1" &&
                !refused.detail.empty(),
            "forged expected-result field is refused by the engine contract");
  }
  for (const auto operation : {"join_group_sum_assertion",
                               "JOIN_WINDOW_MAX_ASSERTION"}) {
    const auto refused =
        api::RefuseRetiredResultExpectationRoute(operation, "rowset", {});
    Require(refused.applies && !refused.accepted,
            "forged assertion operation is refused by the engine contract");
  }
  for (const auto projection : {"count_assertion", "field_assertion",
                                "aggregate_assertion", "window_assertion"}) {
    const auto refused =
        api::RefuseRetiredResultExpectationRoute("inner_join", projection, {});
    Require(refused.applies && !refused.accepted,
            "forged assertion projection is refused by the engine contract");
  }

  std::cout << "QOW-CT-004 assertion-route-removal gate passed\n";
  return 0;
}
