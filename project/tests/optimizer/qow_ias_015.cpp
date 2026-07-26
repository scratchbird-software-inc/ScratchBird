// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "logical_plan.hpp"
#include "rule_planner.hpp"

#include <fstream>
#include <iterator>
#include <string>

namespace {

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
  logical.local_transaction_id = 41;
  logical.statement_snapshot_id = 42;
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
  alternatives.alternatives = {
      {"00000000-0000-7000-8000-000000000004", 1, "values.materialize.v1",
       "00000000-0000-7000-8000-000000000005", {101}, true, ""}};

  const auto validation =
      ValidateCanonicalLogicalPhysicalBoundary(logical, alternatives);
  if (!validation.accepted || validation.data_access_allowed ||
      !validation.issues.empty()) {
    return Fail(__LINE__);
  }
  return 0;
}
