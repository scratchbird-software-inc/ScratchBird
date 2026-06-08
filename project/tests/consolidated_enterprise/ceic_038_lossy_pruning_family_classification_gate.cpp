// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// CEIC-038 focused validation for lossy candidate and pruning family classification.
#include "index_family_route_classification.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

namespace index = scratchbird::core::index;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool EvidenceHas(const index::IndexRouteFamilyClassificationResult& result,
                 std::string_view token) {
  for (const auto& row : result.evidence) {
    if (row.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

index::IndexRouteClassificationRequest Request(
    index::IndexFamily family,
    index::IndexRouteKind route = index::IndexRouteKind::sql_select) {
  index::IndexRouteClassificationRequest request;
  request.family = family;
  request.route = route;
  request.external_cluster_provider_only = true;
  return request;
}

void RequireStatus(
    const index::IndexRouteFamilyClassificationResult& result,
    index::IndexRouteClassificationStatus status,
    std::string_view message) {
  Require(result.classification_status == status, message);
  Require(!result.ok(), "refused CEIC-038 classification unexpectedly ok");
  Require(result.fail_closed,
          "refused CEIC-038 classification did not fail closed");
  Require(!result.runtime_admissible,
          "refused CEIC-038 classification was runtime-admissible");
  Require(!result.diagnostic.diagnostic_code.empty(),
          "refused CEIC-038 classification lacks diagnostic code");
}

void RequireNonAuthority(
    const index::IndexRouteFamilyClassificationResult& result) {
  Require(!result.row_truth_authority,
          "classification must not claim row-truth authority");
  Require(!result.final_row_authority,
          "classification must not claim final row authority");
  Require(!result.transaction_finality_authority,
          "classification must not claim transaction finality authority");
  Require(!result.visibility_authority,
          "classification must not claim visibility authority");
  Require(!result.authorization_authority,
          "classification must not claim authorization authority");
  Require(!result.authorization_security_authority,
          "classification must not claim authorization/security authority");
  Require(!result.security_authority,
          "classification must not claim security authority");
  Require(!result.recovery_authority,
          "classification must not claim recovery authority");
  Require(!result.parser_authority,
          "classification must not claim parser authority");
  Require(!result.donor_authority,
          "classification must not claim donor authority");
  Require(!result.wal_authority,
          "classification must not claim WAL authority");
  Require(!result.benchmark_authority,
          "classification must not claim benchmark authority");
  Require(!result.optimizer_plan_authority,
          "classification must not claim optimizer plan authority");
  Require(!result.optimizer_plan_finality_authority,
          "classification must not claim optimizer-plan finality");
  Require(!result.index_finality_authority,
          "classification must not claim index-finality authority");
  Require(!result.result_finality_authority,
          "classification must not claim result-finality authority");
  Require(!result.local_cluster_authority,
          "classification must not claim local cluster authority");
  Require(!result.cluster_action_authority,
          "classification must not claim cluster action authority");
  Require(!result.agent_action_authority,
          "classification must not claim agent action authority");
}

void RequireNoSuccessorOverclaim(
    const index::IndexRouteFamilyClassificationResult& result) {
  Require(result.ceic_038_family_classification_evidence,
          "CEIC-038 must identify its own classification evidence");
  Require(!result.ceic_039_specialized_provider_closure_claimed,
          "CEIC-038 must not claim CEIC-039");
  Require(!result.ceic_040_runtime_metrics_claimed,
          "CEIC-038 must not claim CEIC-040");
  Require(!result.ceic_041_crash_matrix_claimed,
          "CEIC-038 must not claim CEIC-041");
  Require(!result.ceic_042_readiness_drift_claimed,
          "CEIC-038 must not claim CEIC-042");
  Require(!result.all_index_readiness_claimed,
          "CEIC-038 must not claim all-index readiness");
  Require(!result.donor_dominance_claimed,
          "CEIC-038 must not claim donor dominance");
  Require(!result.enterprise_readiness_claimed,
          "CEIC-038 must not claim enterprise readiness");
}

void ExactFamiliesRemainCandidateOnlyForRows() {
  const auto btree =
      index::ClassifyIndexFamilyRoute(Request(index::IndexFamily::btree));
  Require(btree.ok(), "B-tree classification failed");
  Require(btree.semantic == index::IndexFamilyRouteSemantic::exact_candidate,
          "B-tree semantic was not exact_candidate");
  Require(btree.produces_exact_candidates,
          "B-tree should produce exact candidates");
  Require(btree.supports_ordered_range,
          "B-tree should support ordered range");
  Require(btree.requirements.ceic_037_exact_recheck_handoff_required,
          "B-tree row admission still requires CEIC-037 exact recheck handoff");
  Require(btree.requirements.mga_visibility_recheck_required,
          "B-tree classification must require MGA visibility recheck");
  Require(btree.requirements.security_recheck_required,
          "B-tree classification must require security recheck");
  RequireNonAuthority(btree);
  RequireNoSuccessorOverclaim(btree);
  Require(EvidenceHas(
              btree,
              "ceic_search_key=CEIC_038_LOSSY_PRUNING_FAMILY_CLASSIFICATION"),
          "CEIC-038 evidence anchor missing");

  const auto hash =
      index::ClassifyIndexFamilyRoute(Request(index::IndexFamily::hash));
  Require(hash.ok(), "hash classification failed");
  Require(hash.semantic ==
              index::IndexFamilyRouteSemantic::hash_equality_candidate,
          "hash semantic was not hash_equality_candidate");
  Require(hash.produces_exact_candidates,
          "hash should produce exact equality candidates");
  Require(hash.hash_equality_only, "hash must remain equality-only");
  Require(!hash.supports_ordered_range,
          "hash must not support ordered ranges");
  Require(hash.requirements.ceic_037_exact_recheck_handoff_required,
          "hash row admission requires CEIC-037 recheck handoff");
  RequireNonAuthority(hash);

  RequireStatus(
      index::ClassifyIndexFamilyRoute(
          Request(index::IndexFamily::hash,
                  index::IndexRouteKind::dml_insert)),
      index::IndexRouteClassificationStatus::route_not_supported,
      "hash DML insert route was not refused");
}

void BloomAndSummaryFamiliesArePruneOnly() {
  const auto bloom =
      index::ClassifyIndexFamilyRoute(Request(index::IndexFamily::bloom));
  Require(bloom.ok(), "Bloom classification failed");
  Require(bloom.semantic ==
              index::IndexFamilyRouteSemantic::bloom_negative_prune,
          "Bloom semantic was not bloom_negative_prune");
  Require(bloom.supports_negative_prune,
          "Bloom should support negative prune");
  Require(bloom.negative_prune_only,
          "Bloom must be negative-prune only");
  Require(!bloom.produces_candidate_set,
          "Bloom must not produce row candidates");
  Require(!bloom.row_truth_authority,
          "Bloom must never become row truth");
  Require(bloom.requirements.exact_fallback_required,
          "Bloom positives require exact fallback proof");
  Require(bloom.requirements.false_positive_accounting_required,
          "Bloom requires false-positive accounting");
  Require(bloom.requirements.ceic_037_exact_recheck_handoff_required,
          "Bloom handoff to rows requires CEIC-037 proof");
  RequireNonAuthority(bloom);

  for (const auto family :
       {index::IndexFamily::brin_zone,
        index::IndexFamily::columnar_zone}) {
    const auto result = index::ClassifyIndexFamilyRoute(Request(family));
    Require(result.ok(), "summary family classification failed");
    Require(result.semantic ==
                index::IndexFamilyRouteSemantic::summary_segment_prune,
            "summary family semantic was not summary_segment_prune");
    Require(result.supports_summary_segment_prune,
            "summary family should support segment prune");
    Require(result.summary_segment_prune_only,
            "summary family must be summary/segment-prune only");
    Require(!result.produces_candidate_set,
            "summary family must not produce row candidates directly");
    Require(!result.supports_ordered_range,
            "summary family must not claim ordered range authority");
    Require(result.requirements.exact_fallback_required,
            "summary family requires exact fallback/handoff proof");
    Require(result.requirements.ceic_037_exact_recheck_handoff_required,
            "summary family row handoff requires CEIC-037 proof");
    RequireNonAuthority(result);
  }
}

void CandidateRankingSeedFamiliesRequireExactProofs() {
  const auto bitmap =
      index::ClassifyIndexFamilyRoute(Request(index::IndexFamily::bitmap));
  Require(bitmap.ok(), "bitmap classification failed");
  Require(bitmap.bitmap_candidate_source,
          "bitmap should be a candidate source");
  Require(bitmap.produces_candidate_set,
          "bitmap should produce a candidate set");
  Require(bitmap.requirements.exact_fallback_required,
          "bitmap candidates require exact fallback/recheck proof");
  RequireNonAuthority(bitmap);

  const auto full_text = index::ClassifyIndexFamilyRoute(
      Request(index::IndexFamily::full_text,
              index::IndexRouteKind::nosql_search));
  Require(full_text.ok(), "full-text classification failed");
  Require(full_text.token_or_inverted_candidate_source,
          "full-text should be token/inverted candidate source");
  Require(full_text.produces_candidate_set,
          "full-text should produce candidates");
  Require(full_text.requirements.exact_fallback_required,
          "full-text candidates require exact fallback/recheck proof");
  RequireNonAuthority(full_text);

  const auto sparse_wand = index::ClassifyIndexFamilyRoute(
      Request(index::IndexFamily::sparse_wand,
              index::IndexRouteKind::nosql_search));
  Require(sparse_wand.ok(), "sparse WAND classification failed");
  Require(sparse_wand.semantic ==
              index::IndexFamilyRouteSemantic::token_ranking_candidate,
          "sparse WAND should be token ranking candidate");
  Require(sparse_wand.produces_ranking,
          "sparse WAND should produce ranking");
  Require(sparse_wand.requirements.exact_rerank_required,
          "sparse WAND requires exact rerank proof");
  Require(sparse_wand.requirements.exact_fallback_required,
          "sparse WAND requires exact fallback proof");
  RequireNonAuthority(sparse_wand);

  const auto hnsw = index::ClassifyIndexFamilyRoute(
      Request(index::IndexFamily::vector_hnsw,
              index::IndexRouteKind::nosql_vector));
  Require(hnsw.ok(), "HNSW classification failed");
  Require(hnsw.semantic ==
              index::IndexFamilyRouteSemantic::vector_approximate_candidate,
          "HNSW should be approximate vector candidate");
  Require(hnsw.approximate_candidate_source,
          "HNSW should be approximate candidate source");
  Require(hnsw.produces_ranking,
          "HNSW should produce ranking only before rerank");
  Require(hnsw.requirements.exact_rerank_required,
          "HNSW requires exact rerank proof");
  Require(hnsw.requirements.exact_fallback_required,
          "HNSW requires exact fallback proof");
  RequireNonAuthority(hnsw);

  const auto ivf = index::ClassifyIndexFamilyRoute(
      Request(index::IndexFamily::vector_ivf,
              index::IndexRouteKind::nosql_vector));
  Require(ivf.ok(), "IVF classification failed");
  Require(ivf.requirements.exact_rerank_required,
          "IVF requires exact rerank proof");
  Require(ivf.requirements.exact_fallback_required,
          "IVF requires exact fallback proof");
  RequireNonAuthority(ivf);

  const auto vector_exact = index::ClassifyIndexFamilyRoute(
      Request(index::IndexFamily::vector_exact,
              index::IndexRouteKind::nosql_vector));
  Require(vector_exact.ok(), "vector exact classification failed");
  Require(vector_exact.semantic ==
              index::IndexFamilyRouteSemantic::vector_exact_candidate,
          "vector exact semantic drifted");
  Require(vector_exact.vector_candidate_source,
          "vector exact should remain vector candidate/ranking source");
  Require(!vector_exact.approximate_candidate_source,
          "vector exact must not be approximate");
  Require(!vector_exact.requirements.exact_rerank_required,
          "vector exact does not require ANN rerank proof");
  RequireNonAuthority(vector_exact);

  const auto document = index::ClassifyIndexFamilyRoute(
      Request(index::IndexFamily::document_path,
              index::IndexRouteKind::nosql_document));
  Require(document.ok(), "document path classification failed");
  Require(document.document_candidate_source,
          "document path should be candidate source");
  Require(document.produces_seed_set,
          "document path should be a seed producer");
  Require(document.requirements.exact_fallback_required,
          "document path requires exact fallback proof");
  RequireNonAuthority(document);

  const auto graph = index::ClassifyIndexFamilyRoute(
      Request(index::IndexFamily::graph,
              index::IndexRouteKind::nosql_graph));
  Require(graph.ok(), "graph classification failed");
  Require(graph.graph_candidate_source,
          "graph should be candidate source");
  Require(graph.produces_seed_set,
          "graph should be a seed producer");
  Require(graph.requirements.exact_fallback_required,
          "graph requires exact fallback proof");
  RequireNonAuthority(graph);

  const auto spatial =
      index::ClassifyIndexFamilyRoute(Request(index::IndexFamily::spatial));
  Require(spatial.ok(), "spatial classification failed");
  Require(spatial.spatial_candidate_source,
          "spatial should be candidate source");
  Require(spatial.produces_seed_set,
          "spatial should produce seeds/candidates");
  Require(spatial.requirements.exact_fallback_required,
          "spatial candidates require exact fallback proof");
  RequireNonAuthority(spatial);
}

void DonorPolicyAndUnsafeClaimsFailClosed() {
  RequireStatus(
      index::ClassifyIndexFamilyRoute(
          Request(index::IndexFamily::donor_emulated)),
      index::IndexRouteClassificationStatus::donor_emulated_non_runtime,
      "donor-emulated classification did not fail closed");

  RequireStatus(
      index::ClassifyIndexFamilyRoute(
          Request(index::IndexFamily::policy_blocked)),
      index::IndexRouteClassificationStatus::policy_blocked_non_runtime,
      "policy-blocked classification did not fail closed");

  auto row_truth = Request(index::IndexFamily::btree);
  row_truth.authority_claims.row_truth_authority = true;
  RequireStatus(index::ClassifyIndexFamilyRoute(row_truth),
                index::IndexRouteClassificationStatus::
                    forbidden_authority_claim,
                "row-truth authority claim did not fail closed");

  auto final_row = Request(index::IndexFamily::btree);
  final_row.authority_claims.final_row_authority = true;
  RequireStatus(index::ClassifyIndexFamilyRoute(final_row),
                index::IndexRouteClassificationStatus::
                    forbidden_authority_claim,
                "final-row authority claim did not fail closed");

  auto txn = Request(index::IndexFamily::btree);
  txn.authority_claims.transaction_finality_authority = true;
  RequireStatus(index::ClassifyIndexFamilyRoute(txn),
                index::IndexRouteClassificationStatus::
                    forbidden_authority_claim,
                "transaction finality authority claim did not fail closed");

  auto visibility = Request(index::IndexFamily::btree);
  visibility.authority_claims.visibility_authority = true;
  RequireStatus(index::ClassifyIndexFamilyRoute(visibility),
                index::IndexRouteClassificationStatus::
                    forbidden_authority_claim,
                "visibility authority claim did not fail closed");

  auto authz = Request(index::IndexFamily::btree);
  authz.authority_claims.authorization_authority = true;
  RequireStatus(index::ClassifyIndexFamilyRoute(authz),
                index::IndexRouteClassificationStatus::
                    forbidden_authority_claim,
                "authorization authority claim did not fail closed");

  auto security = Request(index::IndexFamily::btree);
  security.authority_claims.security_authority = true;
  RequireStatus(index::ClassifyIndexFamilyRoute(security),
                index::IndexRouteClassificationStatus::
                    forbidden_authority_claim,
                "security authority claim did not fail closed");

  auto recovery = Request(index::IndexFamily::btree);
  recovery.authority_claims.recovery_authority = true;
  RequireStatus(index::ClassifyIndexFamilyRoute(recovery),
                index::IndexRouteClassificationStatus::
                    forbidden_authority_claim,
                "recovery authority claim did not fail closed");

  auto parser = Request(index::IndexFamily::btree);
  parser.authority_claims.parser_authority = true;
  RequireStatus(index::ClassifyIndexFamilyRoute(parser),
                index::IndexRouteClassificationStatus::
                    forbidden_authority_claim,
                "parser authority claim did not fail closed");

  auto donor = Request(index::IndexFamily::btree);
  donor.authority_claims.donor_authority = true;
  RequireStatus(index::ClassifyIndexFamilyRoute(donor),
                index::IndexRouteClassificationStatus::
                    forbidden_authority_claim,
                "donor authority claim did not fail closed");

  auto wal = Request(index::IndexFamily::btree);
  wal.authority_claims.wal_authority = true;
  RequireStatus(index::ClassifyIndexFamilyRoute(wal),
                index::IndexRouteClassificationStatus::
                    forbidden_authority_claim,
                "WAL authority claim did not fail closed");

  auto benchmark = Request(index::IndexFamily::btree);
  benchmark.authority_claims.benchmark_authority = true;
  RequireStatus(index::ClassifyIndexFamilyRoute(benchmark),
                index::IndexRouteClassificationStatus::
                    forbidden_authority_claim,
                "benchmark authority claim did not fail closed");

  auto optimizer = Request(index::IndexFamily::btree);
  optimizer.authority_claims.optimizer_plan_finality_authority = true;
  RequireStatus(index::ClassifyIndexFamilyRoute(optimizer),
                index::IndexRouteClassificationStatus::
                    forbidden_authority_claim,
                "optimizer-plan finality claim did not fail closed");

  auto index_finality = Request(index::IndexFamily::btree);
  index_finality.authority_claims.index_finality_authority = true;
  RequireStatus(index::ClassifyIndexFamilyRoute(index_finality),
                index::IndexRouteClassificationStatus::
                    forbidden_authority_claim,
                "index-finality authority claim did not fail closed");

  auto result_finality = Request(index::IndexFamily::btree);
  result_finality.authority_claims.result_finality_authority = true;
  RequireStatus(index::ClassifyIndexFamilyRoute(result_finality),
                index::IndexRouteClassificationStatus::
                    forbidden_authority_claim,
                "result-finality authority claim did not fail closed");

  auto agent = Request(index::IndexFamily::btree);
  agent.authority_claims.agent_action_authority = true;
  RequireStatus(index::ClassifyIndexFamilyRoute(agent),
                index::IndexRouteClassificationStatus::
                    forbidden_authority_claim,
                "agent action authority claim did not fail closed");
}

void ClusterAndSuccessorOverclaimsFailClosed() {
  auto cluster = Request(index::IndexFamily::btree);
  cluster.cluster_path_requested = true;
  RequireStatus(index::ClassifyIndexFamilyRoute(cluster),
                index::IndexRouteClassificationStatus::
                    cluster_external_provider_only,
                "local cluster path did not fail external-provider-only");

  auto local_cluster = Request(index::IndexFamily::btree);
  local_cluster.authority_claims.local_cluster_authority = true;
  RequireStatus(index::ClassifyIndexFamilyRoute(local_cluster),
                index::IndexRouteClassificationStatus::
                    cluster_external_provider_only,
                "local cluster authority did not fail external-provider-only");

  auto cluster_action = Request(index::IndexFamily::btree);
  cluster_action.authority_claims.cluster_action_authority = true;
  RequireStatus(index::ClassifyIndexFamilyRoute(cluster_action),
                index::IndexRouteClassificationStatus::
                    cluster_external_provider_only,
                "cluster action authority did not fail external-provider-only");

  auto ceic039 = Request(index::IndexFamily::btree);
  ceic039.successor_claims.ceic_039_specialized_provider_closure_claimed =
      true;
  RequireStatus(index::ClassifyIndexFamilyRoute(ceic039),
                index::IndexRouteClassificationStatus::
                    successor_or_enterprise_overclaim,
                "CEIC-039 overclaim did not fail closed");

  auto ceic040 = Request(index::IndexFamily::btree);
  ceic040.successor_claims.ceic_040_runtime_metrics_claimed = true;
  RequireStatus(index::ClassifyIndexFamilyRoute(ceic040),
                index::IndexRouteClassificationStatus::
                    successor_or_enterprise_overclaim,
                "CEIC-040 overclaim did not fail closed");

  auto ceic041 = Request(index::IndexFamily::btree);
  ceic041.successor_claims.ceic_041_crash_matrix_claimed = true;
  RequireStatus(index::ClassifyIndexFamilyRoute(ceic041),
                index::IndexRouteClassificationStatus::
                    successor_or_enterprise_overclaim,
                "CEIC-041 overclaim did not fail closed");

  auto ceic042 = Request(index::IndexFamily::btree);
  ceic042.successor_claims.ceic_042_readiness_drift_claimed = true;
  RequireStatus(index::ClassifyIndexFamilyRoute(ceic042),
                index::IndexRouteClassificationStatus::
                    successor_or_enterprise_overclaim,
                "CEIC-042 overclaim did not fail closed");

  auto all_index = Request(index::IndexFamily::btree);
  all_index.successor_claims.all_index_readiness_claimed = true;
  RequireStatus(index::ClassifyIndexFamilyRoute(all_index),
                index::IndexRouteClassificationStatus::
                    successor_or_enterprise_overclaim,
                "all-index readiness overclaim did not fail closed");

  auto donor_dominance = Request(index::IndexFamily::btree);
  donor_dominance.successor_claims.donor_dominance_claimed = true;
  RequireStatus(index::ClassifyIndexFamilyRoute(donor_dominance),
                index::IndexRouteClassificationStatus::
                    successor_or_enterprise_overclaim,
                "donor dominance overclaim did not fail closed");

  auto enterprise = Request(index::IndexFamily::btree);
  enterprise.successor_claims.enterprise_readiness_claimed = true;
  RequireStatus(index::ClassifyIndexFamilyRoute(enterprise),
                index::IndexRouteClassificationStatus::
                    successor_or_enterprise_overclaim,
                "enterprise readiness overclaim did not fail closed");
}

}  // namespace

int main() {
  ExactFamiliesRemainCandidateOnlyForRows();
  BloomAndSummaryFamiliesArePruneOnly();
  CandidateRankingSeedFamiliesRequireExactProofs();
  DonorPolicyAndUnsafeClaimsFailClosed();
  ClusterAndSuccessorOverclaimsFailClosed();
  std::cout << "ceic_038_lossy_pruning_family_classification_gate=pass\n";
  return EXIT_SUCCESS;
}
