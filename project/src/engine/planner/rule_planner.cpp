// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "rule_planner.hpp"

#include <string>
#include <string_view>

namespace scratchbird::engine::planner {
namespace {

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.substr(0, prefix.size()) == prefix;
}

PhysicalAccessKind AccessForPredicate(const scratchbird::engine::internal_api::EngineApiRequest& request) {
  const std::string& predicate = request.predicate.predicate_kind;
  if (predicate == "row_uuid_eq") return PhysicalAccessKind::kRowUuidLookup;
  if (predicate == "scalar_eq" || predicate == "unique_eq") return PhysicalAccessKind::kScalarBtreeLookup;
  if (predicate == "scalar_range") return PhysicalAccessKind::kScalarBtreeRange;
  if (predicate == "full_text") return PhysicalAccessKind::kFullTextProbe;
  if (predicate == "vector_exact") return PhysicalAccessKind::kVectorExactSearch;
  if (predicate == "vector_approx") return PhysicalAccessKind::kVectorApproximateWithFallback;
  if (predicate == "document_path") return PhysicalAccessKind::kDocumentPathProbe;
  if (predicate == "graph_seed") return PhysicalAccessKind::kGraphTraversalSeed;
  return PhysicalAccessKind::kTableScan;
}

}  // namespace

LogicalPlan BuildDeterministicLogicalPlan(const RulePlannerInput& input) {
  // SEARCH_KEY: SB_RULE_PLANNER_OPTIMIZER_INTEGRATION
  LogicalPlan plan;
  plan.plan_id = "rule-plan:" + input.envelope.operation_id;

  const std::string& op = input.envelope.operation_id;
  if (op.empty()) {
    plan.diagnostics.push_back("operation_id_required");
    return plan;
  }

  if (StartsWith(op, "observability.") || StartsWith(op, "query.bind_")) {
    plan.ok = true;
    plan.nodes.push_back(MakeLogicalPlanNode(LogicalPlanNodeKind::kCommand, PhysicalAccessKind::kNone, op, "command_dispatch"));
    return plan;
  }
  if (StartsWith(op, "catalog.")) {
    plan.ok = true;
    plan.nodes.push_back(MakeLogicalPlanNode(LogicalPlanNodeKind::kCatalogLookup, PhysicalAccessKind::kCatalogUuidLookup, op, "catalog_lookup"));
    return plan;
  }
  if (StartsWith(op, "transaction.")) {
    plan.ok = true;
    plan.nodes.push_back(MakeLogicalPlanNode(LogicalPlanNodeKind::kTransactionControl, PhysicalAccessKind::kNone, op, "transaction_control"));
    return plan;
  }
  if (StartsWith(op, "ddl.")) {
    plan.ok = true;
    plan.nodes.push_back(MakeLogicalPlanNode(LogicalPlanNodeKind::kDdlMutation, PhysicalAccessKind::kCatalogUuidLookup, op, "ddl_catalog_mutation"));
    return plan;
  }
  if (op == "dml.select_rows") {
    plan.ok = true;
    plan.nodes.push_back(MakeLogicalPlanNode(LogicalPlanNodeKind::kDmlRead, AccessForPredicate(input.api_request), op, "dml_select"));
    return plan;
  }
  if (StartsWith(op, "dml.")) {
    plan.ok = true;
    plan.nodes.push_back(MakeLogicalPlanNode(LogicalPlanNodeKind::kDmlMutation, AccessForPredicate(input.api_request), op, "dml_mutation"));
    return plan;
  }
  if (StartsWith(op, "nosql.")) {
    plan.ok = true;
    plan.nodes.push_back(MakeLogicalPlanNode(LogicalPlanNodeKind::kNoSqlOperation, AccessForPredicate(input.api_request), op, "nosql_dispatch"));
    return plan;
  }
  if (StartsWith(op, "management.")) {
    plan.ok = true;
    plan.nodes.push_back(MakeLogicalPlanNode(LogicalPlanNodeKind::kManagementOperation, PhysicalAccessKind::kNone, op, "management_dispatch"));
    return plan;
  }
  if (StartsWith(op, "extensibility.")) {
    plan.ok = true;
    plan.nodes.push_back(MakeLogicalPlanNode(LogicalPlanNodeKind::kExtensibilityOperation, PhysicalAccessKind::kNone, op, "extensibility_dispatch"));
    return plan;
  }

  plan.diagnostics.push_back("operation_not_plannable");
  plan.nodes.push_back(MakeLogicalPlanNode(LogicalPlanNodeKind::kUnsupported, PhysicalAccessKind::kNone, op, "unsupported"));
  return plan;
}

}  // namespace scratchbird::engine::planner
