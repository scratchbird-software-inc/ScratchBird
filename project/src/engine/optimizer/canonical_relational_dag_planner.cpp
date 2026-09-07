// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "relational_planner.hpp"

namespace scratchbird::engine::optimizer {

// SEARCH_KEY: SB_ENGINE_CANONICAL_RELATIONAL_DAG_PLANNER_AUTHORITY
// Owns the authority-preserving composition of validated planning context,
// profile enumeration, bounded search and immutable physical publication.
// It performs no data access and owns no transaction finality.

RelationalDagPlanningResult PlanCanonicalRelationalDag(
    const RelationalDagPlanningInput& input) {
  RelationalDagPlanningResult result;
  result.planning_context = ValidateCanonicalPlannerContexts(
      input.admission_request.logical_graph,
      input.admission_request.logical_properties, input.continuation_context,
      input.what_if_context);
  if (!result.planning_context.accepted) {
    result.diagnostics.push_back(
        result.planning_context.issues.empty()
            ? "QOW-DIAG-RELATIONAL-DAG-PLANNING-CONTEXT-V1"
            : result.planning_context.issues.front().diagnostic_id);
    return result;
  }
  result.factory = BuildCanonicalOptimizerAlternativeProfiles(
      input.admission_request, input.admission, input.executor_availability,
      input.identity_scope, input.calibration_profile_uuid);
  if (!result.factory.accepted ||
      !result.factory.optimizer_owned_enumeration ||
      !result.factory.snapshot_derived || !result.factory.deterministic ||
      result.factory.data_access_allowed || !result.factory.issues.empty()) {
    result.diagnostics.push_back(
        result.factory.issues.empty()
            ? "QOW-DIAG-RELATIONAL-DAG-PROFILE-FACTORY-V1"
            : result.factory.issues.front().diagnostic_id);
    return result;
  }
  result.complete_logical_dag_covered =
      result.factory.inventory.inventory_complete &&
      result.factory.inventory.receipts.size() >=
          input.admission_request.logical_graph.nodes.size();
  if (!result.complete_logical_dag_covered) {
    result.diagnostics.push_back(
        "QOW-DIAG-RELATIONAL-DAG-INCOMPLETE-COVERAGE-V1");
    return result;
  }

  result.search = SearchCanonicalRelationalMemo(
      input.admission_request, input.admission,
      result.factory.inventory.catalog, result.factory.candidates,
      input.search_policy);
  if (!result.search.accepted || !result.search.selected ||
      !result.search.issues.empty()) {
    result.diagnostics.push_back(
        result.search.issues.empty()
            ? "QOW-DIAG-RELATIONAL-DAG-SEARCH-V1"
            : result.search.issues.front().diagnostic_id);
    return result;
  }
  if (result.planning_context.what_if_planning) {
    result.accepted = true;
    result.optimizer_owned = true;
    result.physical_dag_published = false;
    result.cache_admission_allowed = false;
    result.execution_allowed = false;
    result.data_access_allowed = false;
    return result;
  }
  result.publication = PublishCanonicalPhysicalDag(
      input.admission_request, input.admission,
      result.factory.inventory.catalog, result.search,
      result.factory.capability_catalog, input.publication_identity);
  if (!result.publication.accepted || !result.publication.published ||
      !result.publication.issues.empty()) {
    result.diagnostics.push_back(
        result.publication.issues.empty()
            ? "QOW-DIAG-RELATIONAL-DAG-PUBLICATION-V1"
            : result.publication.issues.front().diagnostic_id);
    return result;
  }
  if (result.planning_context.continuation_planning) {
    auto& receipt = *result.planning_context.continuation_receipt;
    if (!ValidateCanonicalContinuationPhysicalRoot(
            &receipt, result.publication.physical_dag)) {
      result.diagnostics.push_back(
          "QOW-DIAG-RELATIONAL-DAG-CONTINUATION-PROPERTIES-V1");
      return result;
    }
  }
  result.accepted = true;
  result.optimizer_owned = true;
  result.physical_dag_published = true;
  result.cache_admission_allowed =
      result.planning_context.cache_admission_allowed;
  result.execution_allowed = result.planning_context.execution_allowed;
  result.data_access_allowed = false;
  return result;
}

}  // namespace scratchbird::engine::optimizer
