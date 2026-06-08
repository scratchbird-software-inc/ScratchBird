// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "nosql/graph_api.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) { Fail(message); }
}

api::EngineRequestContext Context(api::EngineApiU64 tx = 75) {
  api::EngineRequestContext context;
  context.database_path = "/tmp/sb_odf_075_gate_api.sbdb";
  context.database_uuid.canonical = "019df075-0000-7000-8000-000000000001";
  context.transaction_uuid.canonical = "019df075-0000-7000-8000-000000000075";
  context.local_transaction_id = tx;
  context.security_context_present = true;
  return context;
}

api::EngineGraphPhysicalProof GraphProof() {
  api::EngineGraphPhysicalProof proof;
  proof.proof_supplied = true;
  proof.vertex_index_proof = true;
  proof.edge_index_proof = true;
  proof.adjacency_store_proof = true;
  proof.adjacency_page_proof = true;
  proof.frontier_batching_proof = true;
  proof.visited_cycle_policy_proof = true;
  proof.bidirectional_search_proof = true;
  proof.fusion_seed_proof = true;
  proof.provider_contract.family = api::EngineNoSqlProviderFamily::kGraph;
  proof.provider_contract.scope = api::EngineNoSqlProviderScope::kLocal;
  proof.provider_contract.provider_id = "odf075.local.graph.provider";
  proof.provider_contract.fallback_provider_id = "none";
  proof.provider_contract.local_provider_available = true;
  proof.provider_contract.descriptor_visibility.proof_present = true;
  proof.provider_contract.descriptor_visibility.visible_to_snapshot = true;
  proof.provider_contract.descriptor_visibility.descriptor_shape_compatible = true;
  proof.provider_contract.security_redaction.proof_present = true;
  proof.provider_contract.security_redaction.redaction_policy_bound = true;
  proof.provider_contract.security_redaction.security_snapshot_bound = true;
  proof.provider_contract.index_generation.proof_present = true;
  proof.provider_contract.index_generation.visible_to_snapshot = true;
  proof.provider_contract.index_generation.covers_predicate = true;
  proof.provider_contract.index_generation.required_generation = 75;
  proof.provider_contract.index_generation.available_generation = 75;
  proof.provider_contract.index_generation.index_uuid = "odf075-graph-adjacency";
  proof.provider_contract.policy.proof_present = true;
  proof.provider_contract.policy.allowed = true;
  proof.provider_contract.mga_recheck.proof_present = true;
  proof.provider_contract.mga_recheck.row_mga_recheck_required = true;
  proof.provider_contract.mga_recheck.row_security_recheck_required = true;
  proof.provider_contract.mga_recheck.authority_source =
      "engine_transaction_inventory";
  return proof;
}

std::vector<api::EngineGraphVertexInput> Vertices() {
  return {
      {"A",
       {"person", "seed"},
       {{"tenant", "blue"}, {"name", "alpha"}}},
      {"B",
       {"person"},
       {{"tenant", "green"}, {"name", "beta"}}},
      {"C",
       {"account"},
       {{"tenant", "blue"}, {"name", "connector"}}},
      {"D",
       {"account"},
       {{"tenant", "red"}, {"name", "detour"}}},
      {"E",
       {"person"},
       {{"tenant", "blue"}, {"name", "cycle"}}},
  };
}

std::vector<api::EngineGraphEdgeInput> Edges() {
  return {
      {"e-ac", "A", "C", "knows", {{"since", "2024"}}, 1.0},
      {"e-ad", "A", "D", "knows", {{"since", "2025"}}, 2.0},
      {"e-cb", "C", "B", "knows", {{"since", "2026"}}, 1.5},
      {"e-db", "D", "B", "knows", {{"since", "2026"}}, 2.5},
      {"e-ba", "B", "A", "blocks", {{"since", "2023"}}, 3.0},
      {"e-ae", "A", "E", "blocks", {{"since", "2022"}}, 4.0},
      {"e-ea", "E", "A", "knows", {{"since", "2022"}}, 5.0},
  };
}

api::EngineGraphQueryRequest BaseRequest() {
  api::EngineGraphQueryRequest request;
  request.context = Context();
  request.physical_query = true;
  request.vertices = Vertices();
  request.edges = Edges();
  request.edge_type_filter = "knows";
  request.physical_proof = GraphProof();
  return request;
}

bool EvidenceContains(const api::EngineApiResult& result,
                      std::string_view kind,
                      std::string_view id) {
  for (const auto& item : result.evidence) {
    if (item.evidence_kind.find(kind) != std::string::npos &&
        item.evidence_id.find(id) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool DiagnosticContains(const api::EngineApiResult& result,
                        std::string_view token) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code.find(token) != std::string::npos ||
        diagnostic.detail.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

std::string RowField(const api::EngineApiResult& result,
                     std::size_t row_index,
                     std::string_view field) {
  if (row_index >= result.result_shape.rows.size()) { return {}; }
  for (const auto& [name, value] : result.result_shape.rows[row_index].fields) {
    if (name == field) { return value.encoded_value; }
  }
  return {};
}

bool AnyRowFieldEquals(const api::EngineApiResult& result,
                       std::string_view field,
                       std::string_view expected) {
  for (std::size_t i = 0; i < result.result_shape.rows.size(); ++i) {
    if (RowField(result, i, field) == expected) {
      return true;
    }
  }
  return false;
}

void RequireEvidenceHygiene(const api::EngineApiResult& result) {
  for (const auto& item : result.evidence) {
    for (const auto forbidden :
         {"docs/", "execution-plans", "findings", "contracts",
          "behavior_store_scan_selected=true", "descriptor_scan_selected=true",
          "local_descriptor_scan", "specialized_descriptor_fallback",
          "parser_executes_sql=true", "wal_recovery_authority=true",
          "provider_transaction_finality_authority=true",
          "provider_visibility_authority=true",
          "parser_transaction_finality_authority=true",
          "client_autocommit_authority=true"}) {
      Require(item.evidence_kind.find(forbidden) == std::string::npos &&
                  item.evidence_id.find(forbidden) == std::string::npos,
              "ODF-075 evidence leaked forbidden authority or fallback token");
    }
  }
}

void PropertyIndexSeedTraversalAndFrontierBatching() {
  auto request = BaseRequest();
  request.seed_label = "person";
  request.seed_property_key = "tenant";
  request.seed_property_value = "blue";
  request.max_depth = 2;
  request.option_envelopes.push_back("graph.frontier_batch_size=1");

  const auto result = api::EngineGraphQuery(request);
  Require(result.ok, "ODF-075 property-index graph traversal failed");
  Require(RowField(result, 0, "vertex_id") == "A",
          "ODF-075 property index seed did not select vertex A first");
  Require(AnyRowFieldEquals(result, "path", "A->C->B"),
          "ODF-075 depth-2 frontier traversal missed A->C->B");
  Require(AnyRowFieldEquals(result, "depth", "2"),
          "ODF-075 depth-2 traversal did not emit depth 2 rows");
  Require(EvidenceContains(result, "graph_seed_index", "vertex_property_index"),
          "ODF-075 graph seed/index evidence missing");
  Require(EvidenceContains(result, "graph_adjacency_store",
                           "compressed_adjacency_pages"),
          "ODF-075 compressed adjacency-page evidence missing");
  Require(EvidenceContains(result, "graph_frontier_batching", "batches="),
          "ODF-075 frontier batching evidence missing");
  Require(!EvidenceContains(result, "graph_frontier_batching", "batches=0"),
          "ODF-075 frontier batching did not batch any frontier");
  Require(EvidenceContains(result, "graph_cycle_policy", "visited_set"),
          "ODF-075 visited/cycle policy evidence missing");
  Require(EvidenceContains(result, "row_mga_recheck_evidence", "required"),
          "ODF-075 MGA recheck evidence missing");
  Require(EvidenceContains(result, "row_security_recheck_evidence", "required"),
          "ODF-075 security recheck evidence missing");
  Require(RowField(result, 0, "row_mga_recheck_required") == "true",
          "ODF-075 rows did not carry MGA recheck requirements");
  Require(RowField(result, 0, "row_security_recheck_required") == "true",
          "ODF-075 rows did not carry security recheck requirements");
  Require(result.dml_summary.visible_rows_scanned == 0,
          "ODF-075 physical graph traversal reported descriptor row scans");
  RequireEvidenceHygiene(result);
}

void BidirectionalAToBPath() {
  auto request = BaseRequest();
  request.bidirectional_start_vertex_id = "A";
  request.bidirectional_end_vertex_id = "B";
  request.max_depth = 4;

  const auto result = api::EngineGraphQuery(request);
  Require(result.ok, "ODF-075 bidirectional graph path query failed");
  Require(RowField(result, 0, "vertex_id") == "A",
          "ODF-075 bidirectional path did not start at A");
  Require(RowField(result, 2, "vertex_id") == "B",
          "ODF-075 bidirectional path did not end at B");
  Require(RowField(result, 2, "path") == "A->C->B",
          "ODF-075 bidirectional path was not deterministic");
  Require(EvidenceContains(result, "graph_bidirectional_search", "applied=true"),
          "ODF-075 bidirectional search evidence missing");
  Require(EvidenceContains(result, "graph_bidirectional_search",
                           "two_sided_meet"),
          "ODF-075 bidirectional search did not prove two-sided expansion");
  RequireEvidenceHygiene(result);
}

void VectorSearchFusionSeedTraversal() {
  auto request = BaseRequest();
  request.fusion_source_kind = api::EngineGraphFusionSourceKind::kVector;
  request.fused_candidate_seed_vertex_ids = {"C"};
  request.max_depth = 1;

  const auto result = api::EngineGraphQuery(request);
  Require(result.ok, "ODF-075 graph+vector fusion seed traversal failed");
  Require(RowField(result, 0, "vertex_id") == "C",
          "ODF-075 fusion seed did not start at candidate C");
  Require(AnyRowFieldEquals(result, "path", "C->B"),
          "ODF-075 fusion seed traversal missed C->B");
  Require(EvidenceContains(result, "graph_fusion_seed_source", "vector"),
          "ODF-075 graph fusion source evidence missing");
  Require(EvidenceContains(result, "graph_vector_search_fusion",
                           "candidate_seed_intersection_applied"),
          "ODF-075 graph+vector/search fusion evidence missing");
  RequireEvidenceHygiene(result);
}

void SearchFusionSeedTraversal() {
  auto request = BaseRequest();
  request.fusion_source_kind = api::EngineGraphFusionSourceKind::kSearch;
  request.fused_candidate_seed_vertex_ids = {"A"};
  request.max_depth = 1;

  const auto result = api::EngineGraphQuery(request);
  Require(result.ok, "ODF-075 graph+search fusion seed traversal failed");
  Require(RowField(result, 0, "vertex_id") == "A",
          "ODF-075 search fusion seed did not start at candidate A");
  Require(AnyRowFieldEquals(result, "path", "A->C"),
          "ODF-075 search fusion seed traversal missed A->C");
  Require(EvidenceContains(result, "graph_fusion_seed_source", "search"),
          "ODF-075 graph search fusion source evidence missing");
  Require(EvidenceContains(result, "graph_search_fusion",
                           "candidate_seed_intersection_applied"),
          "ODF-075 graph+search fusion evidence missing");
  RequireEvidenceHygiene(result);
}

void MissingProofsFailClosedWithExactDiagnostics() {
  auto request = BaseRequest();
  request.physical_proof = {};
  auto result = api::EngineGraphQuery(request);
  Require(!result.ok, "ODF-075 missing physical proof did not fail closed");
  Require(DiagnosticContains(result, api::kGraphPhysicalProofMissing),
          "ODF-075 missing physical proof diagnostic changed");

  const struct {
    const char* diagnostic;
    void (*mutate)(api::EngineGraphPhysicalProof*);
  } cases[] = {
      {api::kGraphVertexIndexProofMissing,
       [](api::EngineGraphPhysicalProof* proof) {
         proof->vertex_index_proof = false;
       }},
      {api::kGraphEdgeIndexProofMissing,
       [](api::EngineGraphPhysicalProof* proof) {
         proof->edge_index_proof = false;
       }},
      {api::kGraphAdjacencyStoreProofMissing,
       [](api::EngineGraphPhysicalProof* proof) {
         proof->adjacency_store_proof = false;
       }},
      {api::kGraphAdjacencyPageProofMissing,
       [](api::EngineGraphPhysicalProof* proof) {
         proof->adjacency_page_proof = false;
       }},
      {api::kGraphFrontierBatchingProofMissing,
       [](api::EngineGraphPhysicalProof* proof) {
         proof->frontier_batching_proof = false;
       }},
      {api::kGraphVisitedCyclePolicyProofMissing,
       [](api::EngineGraphPhysicalProof* proof) {
         proof->visited_cycle_policy_proof = false;
       }},
      {api::kGraphBidirectionalSearchProofMissing,
       [](api::EngineGraphPhysicalProof* proof) {
         proof->bidirectional_search_proof = false;
       }},
      {api::kGraphFusionSeedProofMissing,
       [](api::EngineGraphPhysicalProof* proof) {
         proof->fusion_seed_proof = false;
       }},
  };
  for (const auto& item : cases) {
    request = BaseRequest();
    item.mutate(&request.physical_proof);
    result = api::EngineGraphQuery(request);
    Require(!result.ok, "ODF-075 missing graph proof did not fail closed");
    Require(DiagnosticContains(result, item.diagnostic),
            "ODF-075 missing graph proof diagnostic changed");
  }
}

void ProviderContractRefusalsFailClosed() {
  auto request = BaseRequest();
  request.physical_proof.provider_contract.descriptor_visibility
      .descriptor_scan_selected = true;
  auto result = api::EngineGraphQuery(request);
  Require(!result.ok, "ODF-075 descriptor scan was accepted as graph access");
  Require(DiagnosticContains(result,
                             api::kNoSqlProviderDescriptorScanNotPhysicalProvider),
          "ODF-075 descriptor scan refusal diagnostic changed");

  request = BaseRequest();
  request.physical_proof.provider_contract.descriptor_visibility
      .behavior_store_scan_selected = true;
  result = api::EngineGraphQuery(request);
  Require(!result.ok, "ODF-075 behavior-store scan was accepted as graph access");
  Require(DiagnosticContains(result,
                             api::kNoSqlProviderBehaviorScanNotPhysicalProvider),
          "ODF-075 behavior-store scan refusal diagnostic changed");

  request = BaseRequest();
  request.physical_proof.provider_contract.security_redaction.proof_present = false;
  result = api::EngineGraphQuery(request);
  Require(!result.ok, "ODF-075 missing security proof did not fail closed");
  Require(DiagnosticContains(result, api::kNoSqlProviderSecurityProofMissing),
          "ODF-075 missing security proof diagnostic changed");

  request = BaseRequest();
  request.physical_proof.provider_contract.mga_recheck.proof_present = false;
  result = api::EngineGraphQuery(request);
  Require(!result.ok, "ODF-075 missing MGA proof did not fail closed");
  Require(DiagnosticContains(result, api::kNoSqlProviderMgaRecheckProofMissing),
          "ODF-075 missing MGA proof diagnostic changed");

  request = BaseRequest();
  request.physical_proof.provider_contract.mga_recheck
      .parser_claims_transaction_finality_authority = true;
  result = api::EngineGraphQuery(request);
  Require(!result.ok, "ODF-075 parser finality authority was accepted");
  Require(DiagnosticContains(result,
                             api::kNoSqlProviderParserFinalityAuthorityRefused),
          "ODF-075 parser authority refusal diagnostic changed");

  request = BaseRequest();
  request.physical_proof.provider_contract.mga_recheck
      .provider_claims_transaction_finality_authority = true;
  result = api::EngineGraphQuery(request);
  Require(!result.ok, "ODF-075 provider finality authority was accepted");
  Require(DiagnosticContains(result, api::kNoSqlProviderFinalityAuthorityRefused),
          "ODF-075 provider finality authority diagnostic changed");

  request = BaseRequest();
  request.physical_proof.provider_contract.mga_recheck
      .provider_claims_visibility_authority = true;
  result = api::EngineGraphQuery(request);
  Require(!result.ok, "ODF-075 provider visibility authority was accepted");
  Require(DiagnosticContains(result, api::kNoSqlProviderVisibilityAuthorityRefused),
          "ODF-075 provider visibility authority diagnostic changed");

  request = BaseRequest();
  request.physical_proof.provider_contract.family =
      api::EngineNoSqlProviderFamily::kVector;
  result = api::EngineGraphQuery(request);
  Require(!result.ok, "ODF-075 non-graph provider family was accepted");
  Require(DiagnosticContains(result, api::kNoSqlProviderFamilyUnsupported),
          "ODF-075 provider family refusal diagnostic changed");
}

void LegacyEmptyRequestFallbackCompatibility() {
  api::EngineGraphQueryRequest request;
  request.context = Context();
  request.target_object.uuid.canonical = "legacy-graph-collection";
  request.target_object.object_kind = "graph_collection";
  const auto result = api::EngineGraphQuery(request);
  Require(result.ok, "ODF-075 legacy graph fallback failed");
  Require(EvidenceContains(result, "graph_query", "local_descriptor_scan"),
          "ODF-075 legacy graph fallback evidence changed");
  Require(EvidenceContains(result, "nosql_behavior", "local_descriptor_scan"),
          "ODF-075 legacy graph behavior fallback evidence missing");
}

}  // namespace

int main() {
  PropertyIndexSeedTraversalAndFrontierBatching();
  BidirectionalAToBPath();
  VectorSearchFusionSeedTraversal();
  SearchFusionSeedTraversal();
  MissingProofsFailClosedWithExactDiagnostics();
  ProviderContractRefusalsFailClosed();
  LegacyEmptyRequestFallbackCompatibility();
  return EXIT_SUCCESS;
}
