// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "query/canonical_relational_bridge.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <type_traits>

namespace api = scratchbird::engine::internal_api;
namespace plan = scratchbird::engine::planner;
using Uuid = api::EngineUuid;

namespace {
unsigned checks = 0;
void Check(bool condition, const char* message) {
  ++checks;
  if (!condition) throw std::runtime_error(message);
}

Uuid Identity(std::uint8_t suffix) {
  return Uuid{{1, 0x9f, 0, 0, 0, 0, 0x70, 0, 0x80, 0, 0, 0, 0, 0, 0, suffix}};
}

api::CanonicalAccessCandidatePlanningRequestV1 Request() {
  api::CanonicalAccessCandidatePlanningRequestV1 r;
  auto& graph = r.logical_graph;
  graph.bound_sblr_tree_uuid = Identity(1);
  graph.catalog_epoch_uuid = Identity(2);
  graph.security_context_uuid = Identity(3);
  graph.local_transaction_id = 10;
  graph.statement_snapshot_id = 5;
  auto& mga = graph.mga_statement_context;
  mga.statement_uuid = Identity(4);
  mga.owning_transaction_uuid = Identity(5);
  mga.statement_snapshot_uuid = Identity(6);
  mga.statement_metadata_snapshot_uuid = Identity(7);
  mga.owning_local_transaction_id = 10;
  mga.visible_committed_high_watermark = 5;
  mga.oldest_active_transaction_id = 10;
  mga.oldest_interesting_transaction_id = 6;
  mga.oldest_snapshot_transaction_id = 6;
  mga.retention_horizon_transaction_id = 6;
  mga.active_excluded_local_transaction_ids = {10};
  mga.publication_inventory_next_local_transaction_id = 11;
  mga.snapshot_kind = "statement_stable";
  mga.inventory_authoritative = mga.complete = mga.current = true;
  graph.root_logical_node_id = 1;
  graph.result_descriptor_ids = {101};
  plan::CanonicalLogicalRelationalNode source;
  source.logical_node_id = 1;
  source.node_kind = plan::CanonicalLogicalRelationalNodeKind::kRelationSource;
  source.output_descriptor_ids = {101};
  source.bound_expression_ids = {501};
  source.origin_relational_node_ids = {1};
  source.required_object_uuids = {Identity(10)};
  source.semantic_variant_id = "relation.source.v1";
  graph.nodes.push_back(source);
  r.logical_node_id = 1;
  r.relation_uuid = Identity(10);
  r.predicate_expression_ids = {501};
  r.predicate_kind = "exact";
  r.heap_alternative_uuid = Identity(11);
  r.heap_capability_uuid = Identity(12);
  r.statistics_snapshot_uuid = Identity(13);
  r.current_catalog_generation = 2;
  r.current_relation_descriptor_generation = 3;
  r.current_statistics_generation = 4;
  r.maximum_candidate_count = 32;
  r.metadata_snapshot_engine_owned = true;
  r.storage_descriptor_engine_owned = true;
  r.statistics_snapshot_engine_owned = true;
  api::CanonicalAccessIndexMetadataV1 index;
  index.index_uuid = Identity(20);
  index.relation_uuid = r.relation_uuid;
  index.alternative_uuid = Identity(21);
  index.capability_uuid = Identity(22);
  index.implementation_id = "scan.index.btree.v1";
  index.key_expression_ids = {501, 502};
  index.catalog_generation = index.statistics_catalog_generation = 2;
  index.relation_descriptor_generation = 3;
  index.statistics_generation = 4;
  index.index_generation = index.statistics_index_generation = 5;
  index.visible_generation = 6;
  index.catalog_record_current = index.lifecycle_ready = true;
  index.build_validation_complete = index.profile_authoritative = true;
  index.profile_supports_mga_visibility = true;
  index.profile_supports_generation_visibility = true;
  index.supports_exact_lookup = index.supports_range_scan = true;
  index.statistics_present = index.statistics_current = true;
  index.statistics_profile_coupled = index.statistics_mga_visible = true;
  index.visibility_evidence_engine_owned = index.visible_to_statement_snapshot = true;
  r.indexes.push_back(index);
  return r;
}

void Refused(const api::CanonicalAccessCandidatePlanningRequestV1& r) {
  const auto result = api::QowGenerateCanonicalAccessCandidatesV1(r);
  Check(!result.accepted && !result.planning_complete_before_access &&
        !result.data_access_allowed && result.catalog.alternatives.empty() &&
        result.receipts.empty() && !result.diagnostic_id.empty(),
        "invalid candidate authority published partial alternatives");
}

void Test() {
  static_assert(std::is_same_v<decltype(api::CanonicalAccessIndexMetadataV1::index_uuid), Uuid>);
  static_assert(std::is_same_v<decltype(api::CanonicalAccessCandidateReceiptV1::alternative_uuid), Uuid>);
  static_assert(std::is_same_v<decltype(api::CanonicalAccessCandidatePlanningRequestV1::statistics_snapshot_uuid), Uuid>);
  auto r = Request();
  Check(plan::ValidateCanonicalLogicalRelationalGraph(r.logical_graph).accepted,
        "independent source graph fixture is invalid");
  auto result = api::QowGenerateCanonicalAccessCandidatesV1(r);
  Check(result.accepted && result.planning_complete_before_access &&
        !result.data_access_allowed && result.receipts.size() == 2 &&
        result.catalog.alternatives.size() == 2, "valid planning did not preserve heap and index");
  Check(result.receipts[0].heap_fallback && result.receipts[0].available &&
        result.receipts[1].index_uuid == Identity(20) && result.receipts[1].available &&
        result.catalog.alternatives[1].alternative_uuid == Identity(21),
        "binary index identities changed during planning");

  // Distinct indexes can use the same executor and capability. They are
  // distinguished by object and alternative UUIDs, never implementation text.
  for (unsigned byte = 0; byte < 16; ++byte) {
    r = Request();
    auto second = r.indexes.front();
    second.index_uuid.bytes[byte] ^= 1;
    second.alternative_uuid.bytes[byte] ^= 1;
    r.indexes.push_back(second);
    result = api::QowGenerateCanonicalAccessCandidatesV1(r);
    Check(result.accepted && result.receipts.size() == 3 &&
          result.receipts[1].available && result.receipts[2].available &&
          result.receipts[2].index_uuid == second.index_uuid,
          "same executor or shared UUID prefix collapsed separate indexes");
  }
  for (auto member : {&api::CanonicalAccessIndexMetadataV1::index_uuid,
                      &api::CanonicalAccessIndexMetadataV1::relation_uuid,
                      &api::CanonicalAccessIndexMetadataV1::alternative_uuid,
                      &api::CanonicalAccessIndexMetadataV1::capability_uuid}) {
    for (unsigned version = 0; version < 16; ++version) {
      for (unsigned variant = 0; variant < 4; ++variant) {
        if (version == 7 && variant == 2) continue;
        r = Request();
        auto& id = r.indexes.front().*member;
        id.bytes[6] = static_cast<std::uint8_t>(version << 4);
        id.bytes[8] = static_cast<std::uint8_t>(variant << 6);
        Refused(r);
      }
    }
    r = Request(); r.indexes.front().*member = {}; Refused(r);
  }
  r = Request(); r.indexes.push_back(r.indexes.front()); Refused(r);
  r = Request(); r.indexes.front().alternative_uuid = r.heap_alternative_uuid; Refused(r);
  r = Request(); r.maximum_candidate_count = 1; Refused(r);
  r = Request(); r.maximum_candidate_count = 0; Refused(r);
  r = Request(); r.data_access_observed = true; Refused(r);
  r = Request(); r.metadata_snapshot_engine_owned = false; Refused(r);
  r = Request(); r.storage_descriptor_engine_owned = false; Refused(r);
  r = Request(); r.statistics_snapshot_engine_owned = false; Refused(r);
  r = Request(); r.parser_planning_authority_claimed = true; Refused(r);
  r = Request(); r.transaction_finality_authority_claimed = true; Refused(r);

  const auto unavailable = [](auto mutate, const char* diagnostic) {
    auto request = Request(); mutate(request.indexes.front());
    const auto result = api::QowGenerateCanonicalAccessCandidatesV1(request);
    Check(result.accepted && !result.data_access_allowed && result.receipts.size() == 2 &&
          result.receipts[0].available && !result.receipts[1].available &&
          !result.catalog.alternatives[1].available &&
          result.receipts[1].refusal_diagnostic_id == diagnostic &&
          result.catalog.alternatives[1].refusal_diagnostic_id == diagnostic,
          "unavailable index lost its refusal or was admitted for access");
  };
  unavailable([](auto& i) { i.statistics_stale = true; }, "QOW-DIAG-OPT-004-STATISTICS-V1");
  unavailable([](auto& i) { i.visible_to_statement_snapshot = false; }, "QOW-DIAG-OPT-004-VISIBILITY-V1");
  unavailable([](auto& i) { ++i.catalog_generation; }, "QOW-DIAG-OPT-004-GENERATION-V1");
  unavailable([](auto& i) { i.lifecycle_ready = false; }, "QOW-DIAG-OPT-004-LIFECYCLE-V1");
  unavailable([](auto& i) { i.supports_exact_lookup = false; }, "QOW-DIAG-OPT-004-CAPABILITY-V1");
  unavailable([](auto& i) { i.key_expression_ids = {502}; }, "QOW-DIAG-OPT-004-PREDICATE-V1");
  unavailable([](auto& i) { i.relation_uuid = Identity(99); }, "QOW-DIAG-OPT-004-RELATION-V1");
  unavailable([](auto& i) { i.data_access_observed = true; }, "QOW-DIAG-OPT-004-PHASE-V1");
}
}  // namespace

int main() {
  try { Test(); std::cout << "binary access candidate checks=" << checks << '\n'; }
  catch (const std::exception& error) { std::cerr << error.what() << '\n'; return EXIT_FAILURE; }
}
