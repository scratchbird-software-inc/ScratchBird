// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dispatch/function_dispatch.hpp"
#include "registry/function_seed_registry.hpp"
#include "sblr_operator_runtime.hpp"
#include "sblr_resource_governance.hpp"
#include "sblr_test_vectors.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace fn = scratchbird::engine::functions;
namespace sblr = scratchbird::engine::sblr;

namespace {

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

void Expect(bool condition, std::string message, std::vector<std::string>* errors) {
  if (!condition) errors->push_back(std::move(message));
}

}  // namespace

int main() {
  std::vector<std::string> errors;
  const auto seeds = fn::BuildStandardFunctionSeedPackage();
  Expect(seeds.registry.Lookup("data.scalar.lower") != nullptr, "lower seed missing", &errors);
  Expect(!seeds.name_rows.empty(), "name seed rows missing", &errors);

  fn::FunctionCallRequest request;
  request.context.function_id = "data.scalar.lower";
  request.context.security_allowed = true;
  request.context.policy_allowed = true;
  request.context.dependency_available = true;
  request.arguments.push_back({"value", {.descriptor_id = "text", .text_value = "ABC", .is_null = false}});
  const auto lower = fn::DispatchFunctionCall(seeds.registry, request);
  Expect(lower.result.ok(), "lower function should succeed", &errors);
  if (lower.result.ok()) Expect(lower.result.scalar_values.front().text_value == "abc", "lower result mismatch", &errors);

  const auto truth = sblr::SblrAnd(sblr::SblrTruthValue::true_value, sblr::SblrTruthValue::unknown);
  Expect(truth == sblr::SblrTruthValue::unknown, "TRUE AND UNKNOWN should be UNKNOWN", &errors);

  const auto vectors = sblr::BuiltInSblrExecutorTestVectors();
  Expect(vectors.size() >= 7, "built-in test vector coverage missing", &errors);

  sblr::SblrExecutionContext context;
  const auto budget = sblr::CheckSblrResourceBudget({.max_frame_depth = 1}, {.frame_depth = 2}, context, "probe");
  Expect(!budget.ok(), "budget overrun should refuse", &errors);

  std::cout << "{\n  \"ok\": " << (errors.empty() ? "true" : "false") << ",\n  \"errors\": [";
  for (std::size_t i = 0; i < errors.size(); ++i) {
    if (i != 0) std::cout << ", ";
    std::cout << '\"' << JsonEscape(errors[i]) << '\"';
  }
  std::cout << "]\n}\n";
  return errors.empty() ? 0 : 1;
}
