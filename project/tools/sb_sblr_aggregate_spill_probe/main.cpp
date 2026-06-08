// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "common/function_result_helpers.hpp"
#include "registry/function_seed_registry.hpp"
#include "sblr/sblr_aggregate_window_runtime.hpp"

#include <cstdint>
#include <iostream>
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

sblr::SblrExecutionContext Context() {
  sblr::SblrExecutionContext context;
  context.database_uuid = "018f0000-0000-7000-8000-00000000db01";
  context.transaction_uuid = "018f0000-0000-7000-8000-00000000bb01";
  context.statement_uuid = "018f0000-0000-7000-8000-00000000cc01";
  return context;
}

void UpdateOne(sblr::SblrAggregateWindowState* state,
               const sblr::SblrExecutionContext& context,
               sblr::SblrValue value,
               std::vector<std::string>* errors) {
  sblr::SblrAggregateUpdateRequest request;
  request.context = context;
  request.values.push_back(std::move(value));
  const auto result = sblr::UpdateSblrAggregateState(state, request);
  Expect(result.ok(), "aggregate update failed", errors);
}

}  // namespace

int main() {
  std::vector<std::string> errors;
  const auto context = Context();

  sblr::SblrAggregateWindowState left;
  sblr::SblrAggregateWindowState right;
  auto init_left = sblr::InitializeSblrAggregateState("data.aggregate.sum",
                                                     "018f0000-0000-7000-8000-000000000102",
                                                     "real64",
                                                     context,
                                                     &left);
  auto init_right = sblr::InitializeSblrAggregateState("data.aggregate.sum",
                                                      "018f0000-0000-7000-8000-000000000102",
                                                      "real64",
                                                      context,
                                                      &right);
  Expect(init_left.ok(), "left aggregate init failed", &errors);
  Expect(init_right.ok(), "right aggregate init failed", &errors);
  if (init_left.ok() && init_right.ok()) {
    UpdateOne(&left, context, fn::MakeInt64Value("int64", 10), &errors);
    UpdateOne(&left, context, fn::MakeInt64Value("int64", 20), &errors);
    UpdateOne(&right, context, fn::MakeInt64Value("int64", 5), &errors);
    const auto merge = sblr::MergeSblrAggregateState(&left, right, context);
    Expect(merge.ok(), "aggregate merge failed", &errors);
    sblr::SblrAggregateFinalizeRequest finalize;
    finalize.context = context;
    const auto final = sblr::FinalizeSblrAggregateState(left, finalize);
    Expect(final.ok(), "aggregate finalize failed", &errors);
    Expect(!final.scalar_values.empty(), "aggregate finalize did not return scalar", &errors);
    if (!final.scalar_values.empty()) {
      Expect(final.scalar_values.front().has_real64_value, "sum result is not real64", &errors);
      Expect(final.scalar_values.front().real64_value == 35.0, "sum merge/finalize result mismatch", &errors);
    }
  }

  sblr::SblrAggregateWindowState string_state;
  auto init_string = sblr::InitializeSblrAggregateState("data.aggregate.string_agg",
                                                       "018f0000-0000-7000-8000-000000000106",
                                                       "text",
                                                       context,
                                                       &string_state);
  Expect(init_string.ok(), "string aggregate init failed", &errors);
  if (init_string.ok()) {
    sblr::SblrAggregateUpdateRequest limited;
    limited.context = context;
    limited.options.max_state_bytes = 1;
    limited.values.push_back(fn::MakeTextValue("character", "this value must exceed the one-byte test budget"));
    const auto limited_result = sblr::UpdateSblrAggregateState(&string_state, limited);
    Expect(!limited_result.ok(), "aggregate memory budget did not refuse oversized state", &errors);
    Expect(!limited_result.diagnostics.empty(), "aggregate memory budget refusal lacks diagnostic", &errors);
    if (!limited_result.diagnostics.empty()) {
      Expect(limited_result.diagnostics.front().diagnostic_id == "SB_DIAG_AGGREGATE_STATE_MEMORY_LIMIT",
             "aggregate memory budget diagnostic mismatch", &errors);
    }
  }

  const auto package = fn::BuildStandardFunctionSeedPackage();
  const auto* count = package.registry.Lookup("data.aggregate.count");
  const auto* string_agg = package.registry.Lookup("data.aggregate.string_agg");
  Expect(count != nullptr, "count registry row missing", &errors);
  Expect(string_agg != nullptr, "string_agg registry row missing", &errors);
  if (count != nullptr) {
    Expect(count->optimizer_metadata.resource_budget_class == "aggregate_state_budget",
           "count aggregate must use aggregate_state_budget", &errors);
    Expect(count->resource_limits.max_memory_bytes != 0, "count aggregate must have memory limit", &errors);
  }
  if (string_agg != nullptr) {
    Expect(string_agg->optimizer_metadata.cost_class == fn::FunctionCostClass::aggregate_stateful,
           "string_agg must be aggregate_stateful", &errors);
  }

  std::cout << "{\n";
  std::cout << "  \"ok\": " << (errors.empty() ? "true" : "false") << ",\n";
  std::cout << "  \"probe\": \"sb_sblr_aggregate_spill_probe\",\n";
  std::cout << "  \"errors\": [";
  for (std::size_t index = 0; index < errors.size(); ++index) {
    if (index != 0) std::cout << ", ";
    std::cout << '"' << JsonEscape(errors[index]) << '"';
  }
  std::cout << "]\n}\n";
  return errors.empty() ? 0 : 1;
}
