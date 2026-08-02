// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#define QOW_OPT_014_FIXTURE_ONLY
#include "qow_opt_014.cpp"

#include <cstdlib>
#include <string_view>

namespace {

namespace exec = scratchbird::engine::executor;

bool Require016(const bool condition, const std::string_view detail) {
  if (!condition) std::cerr << "QOW-TEST-OPT-016-V1: " << detail << '\n';
  return condition;
}

exec::PhysicalNodeKind PhysicalKind(
    const plan::CanonicalLogicalRelationalNodeKind kind) {
  return static_cast<exec::PhysicalNodeKind>(
      static_cast<std::uint8_t>(kind));
}

opt::CanonicalExecutorCapabilityCatalog Capabilities(
    const opt::CanonicalOptimizerAdmissionRequest& request,
    const plan::CanonicalPhysicalAlternativeCatalog& alternatives) {
  opt::CanonicalExecutorCapabilityCatalog catalog;
  catalog.capability_snapshot_uuid =
      request.policy_capability.capability_snapshot_uuid;
  catalog.policy_epoch = request.policy_capability.policy_epoch;
  catalog.engine_owned = true;
  for (const auto& alternative : alternatives.alternatives) {
    const auto node = std::ranges::find_if(
        request.logical_graph.nodes, [&](const auto& record) {
          return record.logical_node_id == alternative.logical_node_id;
        });
    opt::CanonicalExecutorCapabilityRecord capability;
    capability.capability_uuid = alternative.capability_uuid;
    capability.capability_abi_version = 1;
    capability.implementation_id = alternative.implementation_id;
    capability.logical_node_kind = node->node_kind;
    capability.physical_node_kind = PhysicalKind(node->node_kind);
    capability.minimum_input_count = node->input_logical_node_ids.size();
    capability.maximum_input_count = node->input_logical_node_ids.size();
    capability.maximum_memory_bytes = request.resource.memory_budget_bytes;
    capability.spill_supported = request.resource.spill_allowed;
    capability.storage_read_capable =
        node->node_kind ==
        plan::CanonicalLogicalRelationalNodeKind::kRelationSource;
    capability.mga_visibility_capable = capability.storage_read_capable;
    capability.available = true;
    capability.engine_owned = true;
    catalog.capabilities.push_back(std::move(capability));
  }
  return catalog;
}

opt::CanonicalOptimizerPhysicalPublicationIdentity PublicationIdentity() {
  opt::CanonicalOptimizerPhysicalPublicationIdentity identity;
  identity.selected_plan_uuid = Uuid(900);
  identity.first_causal_counter_id = 10'000;
  identity.engine_owned = true;
  return identity;
}

struct PublicationInputs {
  opt::CanonicalOptimizerAdmissionRequest request;
  opt::CanonicalOptimizerAdmissionResult admission;
  plan::CanonicalPhysicalAlternativeCatalog alternatives;
  opt::CanonicalOptimizerSearchResult search;
  opt::CanonicalExecutorCapabilityCatalog capabilities;
};

PublicationInputs Inputs() {
  PublicationInputs inputs;
  inputs.request = Request();
  inputs.admission =
      opt::AdmitCanonicalOptimizerPlanningRequest(inputs.request);
  inputs.alternatives = Alternatives();
  inputs.search = opt::SearchCanonicalRelationalMemo(
      inputs.request, inputs.admission, inputs.alternatives, Candidates(),
      Policy());
  inputs.capabilities = Capabilities(inputs.request, inputs.alternatives);
  return inputs;
}

const exec::PhysicalNodeRecord* PhysicalNode(
    const exec::TypedPhysicalNodeDag& dag, const std::uint64_t node_id) {
  const auto node = std::ranges::find_if(dag.nodes, [&](const auto& record) {
    return record.physical_node_id == node_id;
  });
  return node == dag.nodes.end() ? nullptr : &*node;
}

bool ValidatePublishedDag() {
  const auto inputs = Inputs();
  const auto result = opt::PublishCanonicalPhysicalDag(
      inputs.request, inputs.admission, inputs.alternatives, inputs.search,
      inputs.capabilities, PublicationIdentity());
  const auto validation =
      exec::ValidateTypedPhysicalNodeDag(result.physical_dag);
  bool passed = true;
  passed &= Require016(inputs.admission.admitted && inputs.search.accepted,
                       "physical publication fixture did not reach selection");
  passed &= Require016(
      result.accepted && result.published && result.issues.empty() &&
          result.immutable_node_identity_validated &&
          result.capability_validated_before_access &&
          !result.data_access_allowed && validation.accepted &&
          result.physical_dag.abi_version == 2 &&
          result.physical_dag.optimizer_published &&
          result.physical_dag.immutable_node_identity_validated &&
          result.physical_dag.capability_validated_before_access &&
          !result.physical_dag.data_access_observed &&
          result.physical_dag.nodes.size() == 4 &&
          result.physical_dag.root_physical_node_id == 4 &&
          exec::PhysicalMgaStatementContextEqual(
              result.physical_dag.nodes.front().mga_statement_context,
              result.physical_dag.mga_statement_context),
      "selected plan did not publish one validated immutable physical DAG");
  const auto* scan = PhysicalNode(result.physical_dag, 1);
  const auto* join = PhysicalNode(result.physical_dag, 3);
  const auto* project = PhysicalNode(result.physical_dag, 4);
  passed &= Require016(
      scan && scan->node_kind == exec::PhysicalNodeKind::kScan &&
          scan->implementation_id == "scan.heap.v1" &&
          scan->selected_alternative_uuid == Uuid(201) &&
          scan->executor_capability_uuid == Uuid(301) &&
          scan->cost_vector_uuid == Uuid(501) &&
          scan->causal_counter_id == 10'000 &&
          scan->engine_capability_validated,
      "published scan identity/capability/cost evidence drifted");
  passed &= Require016(
      join && join->implementation_id == "join.merge.v1" &&
          join->selected_alternative_uuid == Uuid(204) &&
          join->input_physical_node_ids ==
              std::vector<std::uint64_t>({1, 2}) &&
          join->causal_counter_id == 10'002,
      "published join did not match the selected memo alternative");
  passed &= Require016(
      project && project->implementation_id == "project.vector.v1" &&
          project->selected_alternative_uuid == Uuid(207) &&
          project->causal_counter_id == 10'003,
      "published root did not match the selected memo alternative");
  passed &= Require016(
      result.physical_dag.admission_evidence.size() == 8 &&
          result.physical_dag.admission_evidence[3].evidence_uuid ==
              inputs.admission.mga_statement_context.statement_snapshot_uuid,
      "published DAG lost ordered MGA/catalog admission evidence");

  auto mutated_capability = result.physical_dag;
  mutated_capability.nodes[0].executor_capability_uuid.clear();
  passed &= Require016(
      !exec::ValidateTypedPhysicalNodeDag(mutated_capability).accepted,
      "physical ABI accepted a post-publication capability mutation");
  auto late_publication = result.physical_dag;
  late_publication.data_access_observed = true;
  passed &= Require016(!exec::ValidateTypedPhysicalNodeDag(late_publication)
                            .accepted,
                       "physical ABI accepted publication after data access");
  auto changed_snapshot = result.physical_dag;
  changed_snapshot.mga_statement_context.current = false;
  passed &= Require016(
      !exec::ValidateTypedPhysicalNodeDag(changed_snapshot).accepted,
      "physical ABI accepted a non-current DAG snapshot");
  auto changed_node_snapshot = result.physical_dag;
  changed_node_snapshot.nodes.front().mga_statement_context.current = false;
  passed &= Require016(
      !exec::ValidateTypedPhysicalNodeDag(changed_node_snapshot).accepted,
      "physical ABI accepted a node-level snapshot mismatch");
  return passed;
}

bool ExpectRefusal(
    const PublicationInputs& inputs,
    const opt::CanonicalOptimizerPhysicalPublicationIdentity& identity,
    const std::string_view diagnostic_id, const std::string_view detail) {
  const auto result = opt::PublishCanonicalPhysicalDag(
      inputs.request, inputs.admission, inputs.alternatives, inputs.search,
      inputs.capabilities, identity);
  return Require016(!result.accepted && !result.published &&
                        !result.data_access_allowed &&
                        result.physical_dag.nodes.empty() &&
                        result.physical_dag.selected_plan_uuid.empty() &&
                        result.issues.size() == 1 &&
                        result.issues.front().diagnostic_id == diagnostic_id,
                    detail);
}

bool ValidateCapabilityRefusals() {
  bool passed = true;

  auto missing = Inputs();
  std::erase_if(missing.capabilities.capabilities, [](const auto& capability) {
    return capability.capability_uuid == Uuid(304);
  });
  passed &= ExpectRefusal(
      missing, PublicationIdentity(),
      "QOW-DIAG-OPTIMIZER-EXECUTOR-CAPABILITY-V1",
      "missing selected join capability published a DAG");

  auto disabled = Inputs();
  disabled.capabilities.capabilities[3].available = false;
  disabled.capabilities.capabilities[3].refusal_diagnostic_id =
      "QOW-DIAG-IMPLEMENTATION-DISABLED-V1";
  passed &= ExpectRefusal(
      disabled, PublicationIdentity(),
      "QOW-DIAG-OPTIMIZER-EXECUTOR-CAPABILITY-V1",
      "disabled selected implementation published a DAG");

  auto wrong_kind = Inputs();
  wrong_kind.capabilities.capabilities[3].physical_node_kind =
      exec::PhysicalNodeKind::kAggregate;
  passed &= ExpectRefusal(
      wrong_kind, PublicationIdentity(),
      "QOW-DIAG-OPTIMIZER-EXECUTOR-CAPABILITY-V1",
      "wrong executor node kind published a DAG");

  auto no_visibility = Inputs();
  no_visibility.capabilities.capabilities[0].mga_visibility_capable = false;
  passed &= ExpectRefusal(
      no_visibility, PublicationIdentity(),
      "QOW-DIAG-OPTIMIZER-EXECUTOR-CAPABILITY-V1",
      "storage capability without MGA visibility published a DAG");

  auto too_small = Inputs();
  too_small.capabilities.capabilities[3].maximum_memory_bytes = 1;
  passed &= ExpectRefusal(
      too_small, PublicationIdentity(),
      "QOW-DIAG-OPTIMIZER-EXECUTOR-CAPABILITY-V1",
      "operator exceeding executor memory capability published a DAG");

  auto stale = Inputs();
  stale.capabilities.capability_snapshot_uuid = Uuid(999);
  passed &= ExpectRefusal(
      stale, PublicationIdentity(),
      "QOW-DIAG-OPTIMIZER-EXECUTOR-CAPABILITY-V1",
      "stale capability snapshot published a DAG");

  auto stale_search = Inputs();
  stale_search.search.mga_statement_context.current = false;
  passed &= ExpectRefusal(
      stale_search, PublicationIdentity(),
      "QOW-DIAG-OPTIMIZER-PHYSICAL-SEARCH-V1",
      "changed search snapshot published a DAG");

  auto observed = Inputs();
  auto identity = PublicationIdentity();
  identity.data_access_observed = true;
  passed &= ExpectRefusal(
      observed, identity, "QOW-DIAG-OPTIMIZER-PHYSICAL-PUBLICATION-V1",
      "post-read publication identity published a DAG");
  return passed;
}

PublicationInputs PropertyInputs() {
  auto request = Request();
  request.logical_graph.nodes.back().bound_expression_ids = {77};
  plan::CanonicalLogicalPropertyCatalog properties;
  properties.bound_sblr_tree_uuid = Uuid(1);
  properties.catalog_epoch_uuid = Uuid(2);
  properties.security_context_uuid = Uuid(3);
  properties.local_transaction_id = 701;
  properties.statement_snapshot_id = 699;
  properties.mga_statement_context = MgaContext();
  plan::CanonicalLogicalPropertyRecord ordering;
  ordering.property_uuid = Uuid(10);
  ordering.property_kind = plan::CanonicalLogicalPropertyKind::kOrdering;
  ordering.origin_logical_node_id = 4;
  ordering.ordering_terms = {
      {77, plan::CanonicalLogicalPropertySortDirection::kAscending,
       plan::CanonicalLogicalPropertyNullPlacement::kNullsLast, Uuid(11)}};
  ordering.populated_from_bound_sblr = true;
  properties.properties = {ordering};
  const auto populated = plan::PopulateCanonicalLogicalPropertiesFromBoundSblr(
      request.logical_graph, properties,
      {{1, {}, {}}, {2, {}, {}}, {3, {}, {}}, {4, {}, {Uuid(10)}}});
  request.logical_graph = populated.logical_graph;
  request.logical_properties = populated.property_catalog;

  auto alternatives = Alternatives();
  alternatives.alternatives.back().delivered_property_uuids = {Uuid(10)};
  auto candidates = Candidates();
  std::erase_if(candidates, [](const auto& candidate) {
    return candidate.alternative_uuid == Uuid(206);
  });
  PublicationInputs inputs;
  inputs.request = std::move(request);
  inputs.admission =
      opt::AdmitCanonicalOptimizerPlanningRequest(inputs.request);
  inputs.alternatives = std::move(alternatives);
  inputs.search = opt::SearchCanonicalRelationalMemo(
      inputs.request, inputs.admission, inputs.alternatives, candidates,
      Policy());
  inputs.capabilities = Capabilities(inputs.request, inputs.alternatives);
  inputs.capabilities.capabilities.back().supported_property_kinds = {
      plan::CanonicalLogicalPropertyKind::kOrdering};
  return inputs;
}

bool ValidatePropertyCapability() {
  auto inputs = PropertyInputs();
  const auto accepted = opt::PublishCanonicalPhysicalDag(
      inputs.request, inputs.admission, inputs.alternatives, inputs.search,
      inputs.capabilities, PublicationIdentity());
  bool passed = true;
  passed &= Require016(accepted.accepted && accepted.published &&
                           PhysicalNode(accepted.physical_dag, 4) &&
                           PhysicalNode(accepted.physical_dag, 4)
                                   ->delivered_property_uuids ==
                               std::vector<std::string>{Uuid(10)},
                       "supported ordering property was not published");
  inputs.capabilities.capabilities.back().supported_property_kinds.clear();
  passed &= ExpectRefusal(
      inputs, PublicationIdentity(),
      "QOW-DIAG-OPTIMIZER-EXECUTOR-CAPABILITY-V1",
      "unsupported selected physical property published a DAG");
  return passed;
}

}  // namespace

#ifndef QOW_OPT_016_FIXTURE_ONLY
// QOW-TEST-OPT-016-V1
int main() {
  bool passed = true;
  passed &= ValidatePublishedDag();
  passed &= ValidateCapabilityRefusals();
  passed &= ValidatePropertyCapability();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
#endif
