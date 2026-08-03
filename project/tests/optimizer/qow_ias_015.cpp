// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "logical_plan.hpp"
#include "rule_planner.hpp"

#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>

namespace {

constexpr std::uint64_t kOwner = 0xffff'ffff'ffff'ff00ULL;
constexpr std::uint64_t kOldestActive = 0xffff'ffff'ffff'fee8ULL;
constexpr std::uint64_t kHorizon = 0xffff'ffff'ffff'fed0ULL;
constexpr std::uint64_t kInDoubt = 0xffff'ffff'ffff'fef0ULL;
constexpr std::uint64_t kInventoryNext = 0xffff'ffff'ffff'fff0ULL;

int Fail(const int line) { return line; }

std::string ReadFile(const char* path) {
  std::ifstream input(path);
  if (!input) return {};
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

}  // namespace

// QOW-TEST-IAS-015-V1
int main() {
  using namespace scratchbird::engine::planner;

  static_assert(kOperationPrefixRulePlannerRemoved);
  const std::string source = ReadFile(SB_QOW_RULE_PLANNER_SOURCE_FILE);
  const std::string header = ReadFile(SB_QOW_RULE_PLANNER_HEADER_FILE);
  if (source.empty() || header.empty()) return Fail(__LINE__);

  const std::string removed_function =
      std::string("Build") + "DeterministicLogicalPlan";
  const std::string removed_input = std::string("RulePlanner") + "Input";
  if (source.find(removed_function) != std::string::npos ||
      header.find(removed_function) != std::string::npos ||
      source.find(removed_input) != std::string::npos ||
      header.find(removed_input) != std::string::npos) {
    return Fail(__LINE__);
  }
  if (source.find("QOW-SOURCE-IAS-015-V1") == std::string::npos) {
    return Fail(__LINE__);
  }

  CanonicalLogicalRelationalGraph logical;
  logical.bound_sblr_tree_uuid = "00000000-0000-7000-8000-000000000001";
  logical.catalog_epoch_uuid = "00000000-0000-7000-8000-000000000002";
  logical.security_context_uuid = "00000000-0000-7000-8000-000000000003";
  logical.local_transaction_id = kOwner;
  logical.statement_snapshot_id = 0;
  logical.mga_statement_context = {
      "00000000-0000-7000-8000-000000000011",
      "00000000-0000-7000-8000-000000000012",
      "00000000-0000-7000-8000-000000000013",
      "00000000-0000-7000-8000-000000000014",
      kOwner,
      0,
      kOldestActive,
      kHorizon,
      kHorizon,
      kHorizon,
      {kOldestActive, kOwner},
      {kInDoubt},
      "statement_stable",
      kInventoryNext,
      true,
      true,
      true};
  logical.root_logical_node_id = 1;
  logical.result_descriptor_ids = {101};
  logical.nodes = {{1,
                    CanonicalLogicalRelationalNodeKind::kValues,
                    {},
                    {101},
                    {201},
                    {301},
                    {},
                    "values.literal_table.v1",
                    false}};

  CanonicalPhysicalAlternativeCatalog alternatives;
  alternatives.bound_sblr_tree_uuid = logical.bound_sblr_tree_uuid;
  alternatives.catalog_epoch_uuid = logical.catalog_epoch_uuid;
  alternatives.security_context_uuid = logical.security_context_uuid;
  alternatives.local_transaction_id = logical.local_transaction_id;
  alternatives.statement_snapshot_id = logical.statement_snapshot_id;
  alternatives.mga_statement_context = logical.mga_statement_context;
  alternatives.alternatives = {
      {"00000000-0000-7000-8000-000000000004", 1, "values.materialize.v1",
       "00000000-0000-7000-8000-000000000005", {101}, true, ""}};

  const auto validation =
      ValidateCanonicalLogicalPhysicalBoundary(logical, alternatives);
  if (!validation.accepted || validation.data_access_allowed ||
      !validation.issues.empty() ||
      logical.statement_snapshot_id != 0 ||
      logical.local_transaction_id !=
          logical.mga_statement_context.owning_local_transaction_id) {
    return Fail(__LINE__);
  }

  auto stale = alternatives;
  stale.mga_statement_context.current = false;
  const auto stale_result =
      ValidateCanonicalLogicalPhysicalBoundary(logical, stale);
  if (stale_result.accepted || stale_result.data_access_allowed ||
      stale_result.validated_alternative_count != 0 ||
      stale_result.executable_alternative_count != 0 ||
      stale_result.issues.size() != 1) {
    return Fail(__LINE__);
  }

  auto duplicate = alternatives;
  duplicate.mga_statement_context.active_excluded_local_transaction_ids =
      {kOldestActive, kOwner, kOwner};
  const auto duplicate_result =
      ValidateCanonicalLogicalPhysicalBoundary(logical, duplicate);
  if (duplicate_result.accepted || duplicate_result.data_access_allowed ||
      duplicate_result.validated_alternative_count != 0 ||
      duplicate_result.issues.size() != 1) {
    return Fail(__LINE__);
  }

  auto narrowed = alternatives;
  narrowed.local_transaction_id = static_cast<std::uint32_t>(kOwner);
  const auto narrowed_result =
      ValidateCanonicalLogicalPhysicalBoundary(logical, narrowed);
  if (narrowed_result.accepted || narrowed_result.data_access_allowed ||
      narrowed_result.validated_alternative_count != 0 ||
      narrowed_result.issues.size() != 1) {
    return Fail(__LINE__);
  }
  return 0;
}
