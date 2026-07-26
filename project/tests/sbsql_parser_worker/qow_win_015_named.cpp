// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "lowering/lowering.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace sbsql = scratchbird::parser::sbsql;

namespace {

bool Require(const bool condition, const std::string_view detail) {
  if (!condition) std::cerr << "QOW-TEST-WIN-015-NAMED-V1: " << detail << '\n';
  return condition;
}

sbsql::CanonicalNamedWindowDefinition Definition(
    std::string name, std::optional<std::string> base,
    std::vector<std::uint32_t> partition,
    std::vector<std::uint32_t> order,
    std::optional<std::uint32_t> frame = std::nullopt) {
  sbsql::CanonicalNamedWindowDefinition definition;
  definition.name = std::move(name);
  definition.base_name = std::move(base);
  definition.partition_descriptor_ids = std::move(partition);
  definition.order_descriptor_ids = std::move(order);
  definition.frame_descriptor_id = frame;
  return definition;
}

bool ValidateNamedWindowInheritance() {
  const auto result = sbsql::ResolveCanonicalNamedWindows(
      {Definition("partitioned", std::nullopt, {101}, {}),
       Definition("ordered", "partitioned", {}, {201, 202}),
       Definition("framed", "ordered", {}, {}, 301)},
      {"framed", "ordered"});
  return Require(
      result.accepted && result.resolved_once_in_scope &&
          result.resolved_definitions.size() == 3 &&
          result.resolved_definitions[1].partition_descriptor_ids ==
              std::vector<std::uint32_t>({101}) &&
          result.resolved_definitions[1].order_descriptor_ids ==
              std::vector<std::uint32_t>({201, 202}) &&
          result.resolved_definitions[1].resolution_depth == 1 &&
          result.resolved_definitions[2].partition_descriptor_ids ==
              std::vector<std::uint32_t>({101}) &&
          result.resolved_definitions[2].order_descriptor_ids ==
              std::vector<std::uint32_t>({201, 202}) &&
          result.resolved_definitions[2].frame_descriptor_id ==
              std::optional<std::uint32_t>(301) &&
          result.resolved_definitions[2].resolution_depth == 2 &&
          result.reference_definition_indices ==
              std::vector<std::size_t>({2, 1}),
      "named-window inheritance did not resolve once in declaration scope");
}

bool ValidateNamedWindowRefusals() {
  bool passed = true;
  const auto refused = [&](const auto& definitions,
                           const auto& references,
                           const std::string_view detail) {
    const auto result =
        sbsql::ResolveCanonicalNamedWindows(definitions, references);
    return Require(!result.accepted &&
                       result.diagnostic_id == "QOW-DIAG-WINDOW-NAMED" &&
                       result.resolved_definitions.empty(),
                   detail);
  };
  passed &= refused(
      std::vector<sbsql::CanonicalNamedWindowDefinition>{
          Definition("derived", "later", {}, {201}),
          Definition("later", std::nullopt, {101}, {})},
      std::vector<std::string>{"derived"},
      "forward named-window base was admitted");
  passed &= refused(
      std::vector<sbsql::CanonicalNamedWindowDefinition>{
          Definition("cycle", "cycle", {}, {})},
      std::vector<std::string>{"cycle"},
      "cyclic named-window base was admitted");
  passed &= refused(
      std::vector<sbsql::CanonicalNamedWindowDefinition>{
          Definition("base", std::nullopt, {101}, {}),
          Definition("derived", "base", {102}, {})},
      std::vector<std::string>{"derived"},
      "derived PARTITION BY replaced its named-window base");
  passed &= refused(
      std::vector<sbsql::CanonicalNamedWindowDefinition>{
          Definition("base", std::nullopt, {}, {201}),
          Definition("derived", "base", {}, {202})},
      std::vector<std::string>{"derived"},
      "derived ORDER BY replaced its named-window base");
  passed &= refused(
      std::vector<sbsql::CanonicalNamedWindowDefinition>{
          Definition("base", std::nullopt, {}, {}, 301),
          Definition("derived", "base", {}, {}, 302)},
      std::vector<std::string>{"derived"},
      "derived frame replaced its named-window base frame");
  passed &= refused(
      std::vector<sbsql::CanonicalNamedWindowDefinition>{
          Definition("base", std::nullopt, {}, {})},
      std::vector<std::string>{"missing"},
      "unknown named-window reference was admitted");
  return passed;
}

}  // namespace

// QOW-TEST-WIN-015-NAMED-V1
int main() {
  return ValidateNamedWindowInheritance() && ValidateNamedWindowRefusals()
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
