// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// CEIC-037 focused validation for engine-owned exact recheck services.
#include "index_recheck.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

namespace index = scratchbird::core::index;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

TypedUuid TestUuid(UuidKind kind, unsigned seed) {
  TypedUuid value;
  value.kind = kind;
  for (std::size_t i = 0; i < value.value.bytes.size(); ++i) {
    value.value.bytes[i] =
        static_cast<scratchbird::core::platform::byte>(
            (seed * 37u + i * 19u + 0x61u) & 0xffu);
  }
  value.value.bytes[6] =
      static_cast<scratchbird::core::platform::byte>(
          (value.value.bytes[6] & 0x0fu) | 0x70u);
  value.value.bytes[8] =
      static_cast<scratchbird::core::platform::byte>(
          (value.value.bytes[8] & 0x3fu) | 0x80u);
  return value;
}

bool EvidenceHas(const index::EngineOwnedExactRecheckResult& result,
                 std::string_view token) {
  for (const auto& row : result.evidence) {
    if (row.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

index::EngineOwnedExactRecheckRequest ValidRequest() {
  index::EngineOwnedExactRecheckRequest request;
  request.candidate.key.encoded_key = "ceic037-exact-key";
  request.candidate.key.requires_recheck = true;
  request.candidate.locator.table_uuid = TestUuid(UuidKind::object, 1);
  request.candidate.locator.row_uuid = TestUuid(UuidKind::row, 2);
  request.candidate.locator.version_uuid = TestUuid(UuidKind::row, 3);
  request.candidate.locator.local_transaction_id = 42;
  request.candidate.mga_visible = true;
  request.candidate.predicate_exact = true;
  request.candidate.security_visible = true;
  request.route_evidence.family = index::IndexFamily::btree;
  request.route_evidence.route = index::IndexRouteKind::sql_select;
  request.route_evidence.candidate_route = true;
  request.proof.mga_visibility_proven = true;
  request.proof.mga_inventory_proof_present = true;
  request.proof.mga_snapshot_proof_present = true;
  request.proof.mga_snapshot_fresh = true;
  request.proof.security_context_present = true;
  request.proof.authorization_proven = true;
  request.proof.predicate_exactness_proven = true;
  request.proof.exact_source_row_verified = true;
  request.proof.exact_source_payload_verified = true;
  request.proof.mga_inventory_proof_id = "mga-inventory-proof-42";
  request.proof.mga_snapshot_proof_id = "mga-snapshot-proof-42";
  request.proof.security_context_id = "security-context-42";
  request.proof.authorization_proof_id = "authorization-proof-42";
  request.proof.predicate_proof_id = "predicate-proof-42";
  request.proof.exact_source_proof_id = "exact-source-proof-42";
  return request;
}

void RequireStatus(const index::EngineOwnedExactRecheckResult& result,
                   index::EngineOwnedExactRecheckStatus status,
                   std::string_view message) {
  Require(result.recheck_status == status, message);
  Require(!result.ok(), "refused CEIC-037 result unexpectedly ok");
  Require(result.fail_closed, "refused CEIC-037 result did not fail closed");
  Require(!result.row_admitted_to_executor,
          "refused CEIC-037 result admitted a row");
  Require(!result.diagnostic.diagnostic_code.empty(),
          "refused CEIC-037 result lacks diagnostic code");
  Require(result.engine_owned_evidence,
          "CEIC-037 evidence must remain engine owned");
}

void ExactCandidateAcceptedAfterAllProofs() {
  const auto result = index::ApplyEngineOwnedExactRecheck(ValidRequest());
  Require(result.ok(), "valid exact candidate was refused");
  Require(result.recheck_status ==
              index::EngineOwnedExactRecheckStatus::admitted_to_executor,
          "valid exact candidate returned wrong status");
  Require(result.row_admitted_to_executor,
          "valid exact candidate was not admitted to executor");
  Require(result.engine_owned_evidence,
          "CEIC-037 result must be engine-owned evidence");
  Require(!result.transaction_finality_authority,
          "CEIC-037 must not claim transaction finality authority");
  Require(!result.visibility_authority,
          "CEIC-037 must not claim visibility authority");
  Require(!result.authorization_authority,
          "CEIC-037 must not claim authorization authority");
  Require(!result.security_authority,
          "CEIC-037 must not claim security authority");
  Require(!result.recovery_authority,
          "CEIC-037 must not claim recovery authority");
  Require(!result.parser_authority,
          "CEIC-037 must not claim parser authority");
  Require(!result.reference_authority,
          "CEIC-037 must not claim reference authority");
  Require(!result.wal_authority, "CEIC-037 must not claim WAL authority");
  Require(!result.provider_authority,
          "CEIC-037 must not claim provider authority");
  Require(!result.benchmark_authority,
          "CEIC-037 must not claim benchmark authority");
  Require(!result.optimizer_plan_authority,
          "CEIC-037 must not claim optimizer-plan authority");
  Require(!result.index_finality_authority,
          "CEIC-037 must not claim index-finality authority");
  Require(!result.cluster_action_authority,
          "CEIC-037 must not claim cluster action authority");
  Require(!result.agent_action_authority,
          "CEIC-037 must not claim agent action authority");
  Require(!result.ceic_038_family_classification_claimed,
          "CEIC-037 must not claim CEIC-038");
  Require(!result.ceic_039_specialized_provider_closure_claimed,
          "CEIC-037 must not claim CEIC-039");
  Require(!result.ceic_040_runtime_metrics_claimed,
          "CEIC-037 must not claim CEIC-040");
  Require(!result.ceic_041_crash_matrix_claimed,
          "CEIC-037 must not claim CEIC-041");
  Require(!result.ceic_042_readiness_drift_claimed,
          "CEIC-037 must not claim CEIC-042");
  Require(!result.all_index_readiness_claimed,
          "CEIC-037 must not claim all-index readiness");
  Require(!result.enterprise_readiness_claimed,
          "CEIC-037 must not claim enterprise readiness");
  Require(EvidenceHas(result,
                      "ceic_search_key=CEIC_037_ENGINE_OWNED_EXACT_RECHECK_SERVICES"),
          "CEIC-037 evidence anchor missing");
  Require(EvidenceHas(result, "row_admitted_to_executor=true"),
          "row executor admission evidence missing");
  Require(EvidenceHas(result, "transaction_finality_authority=false"),
          "non-authority evidence missing");
}

void LossyCandidateRequiresExactFallback() {
  auto request = ValidRequest();
  request.candidate.key.lossy = true;
  request.route_evidence.lossy_candidate = true;
  request.route_evidence.exact_fallback_available = true;
  request.proof.exact_fallback_proof_id = "exact-fallback-proof-42";
  auto result = index::ApplyEngineOwnedExactRecheck(request);
  Require(result.ok(), "lossy candidate with exact fallback was refused");
  Require(EvidenceHas(result, "lossy_candidate=true"),
          "lossy candidate evidence missing");
  Require(EvidenceHas(result, "exact_fallback_available=true"),
          "exact fallback evidence missing");

  request.route_evidence.exact_fallback_available = false;
  request.proof.exact_fallback_proof_id.clear();
  RequireStatus(
      index::ApplyEngineOwnedExactRecheck(request),
      index::EngineOwnedExactRecheckStatus::
          lossy_or_approximate_without_exact_fallback,
      "lossy candidate without exact fallback did not fail closed");
}

void ApproximateVectorCandidateRequiresExactRerank() {
  auto request = ValidRequest();
  request.route_evidence.family = index::IndexFamily::vector_hnsw;
  request.route_evidence.route = index::IndexRouteKind::nosql_vector;
  request.route_evidence.approximate_candidate = true;
  request.route_evidence.exact_fallback_available = true;
  request.route_evidence.exact_rerank_required = true;
  request.route_evidence.exact_rerank_proven = true;
  request.route_evidence.vector_payload_required = true;
  request.proof.exact_vector_payload_verified = true;
  request.proof.exact_fallback_proof_id = "vector-exact-fallback-42";
  request.proof.exact_rerank_proof_id = "vector-exact-rerank-42";
  auto result = index::ApplyEngineOwnedExactRecheck(request);
  Require(result.ok(), "approximate vector candidate with rerank was refused");
  Require(EvidenceHas(result, "exact_rerank_proven=true"),
          "exact rerank evidence missing");

  request.route_evidence.exact_rerank_proven = false;
  request.proof.exact_rerank_proof_id.clear();
  RequireStatus(index::ApplyEngineOwnedExactRecheck(request),
                index::EngineOwnedExactRecheckStatus::
                    missing_required_exact_rerank,
                "missing exact rerank did not fail closed");
}

void DocumentTextAndGraphSourcesRequireExactPayloads() {
  auto document = ValidRequest();
  document.route_evidence.family = index::IndexFamily::document_path;
  document.route_evidence.route = index::IndexRouteKind::nosql_document;
  document.route_evidence.document_payload_required = true;
  RequireStatus(index::ApplyEngineOwnedExactRecheck(document),
                index::EngineOwnedExactRecheckStatus::
                    missing_exact_source_proof,
                "missing document source proof did not fail closed");
  document.proof.exact_document_payload_verified = true;
  Require(index::ApplyEngineOwnedExactRecheck(document).ok(),
          "document source proof was refused");

  auto text = ValidRequest();
  text.route_evidence.family = index::IndexFamily::full_text;
  text.route_evidence.route = index::IndexRouteKind::nosql_search;
  text.route_evidence.text_payload_required = true;
  text.proof.exact_text_payload_verified = true;
  Require(index::ApplyEngineOwnedExactRecheck(text).ok(),
          "text source proof was refused");

  auto graph = ValidRequest();
  graph.route_evidence.family = index::IndexFamily::graph;
  graph.route_evidence.route = index::IndexRouteKind::nosql_graph;
  graph.route_evidence.graph_payload_required = true;
  graph.proof.exact_graph_payload_verified = true;
  Require(index::ApplyEngineOwnedExactRecheck(graph).ok(),
          "graph source proof was refused");
}

void MissingCoreProofsFailClosed() {
  auto missing_route = ValidRequest();
  missing_route.route_evidence.route = index::IndexRouteKind::unknown;
  RequireStatus(index::ApplyEngineOwnedExactRecheck(missing_route),
                index::EngineOwnedExactRecheckStatus::missing_route_evidence,
                "missing route evidence did not fail closed");

  auto non_candidate_route = ValidRequest();
  non_candidate_route.route_evidence.candidate_route = false;
  RequireStatus(index::ApplyEngineOwnedExactRecheck(non_candidate_route),
                index::EngineOwnedExactRecheckStatus::missing_route_evidence,
                "non-candidate route evidence did not fail closed");

  auto missing_locator = ValidRequest();
  missing_locator.candidate.locator.table_uuid = {};
  RequireStatus(index::ApplyEngineOwnedExactRecheck(missing_locator),
                index::EngineOwnedExactRecheckStatus::missing_locator_uuid,
                "missing table UUID did not fail closed");

  auto missing_tx = ValidRequest();
  missing_tx.candidate.locator.local_transaction_id = 0;
  RequireStatus(index::ApplyEngineOwnedExactRecheck(missing_tx),
                index::EngineOwnedExactRecheckStatus::
                    missing_local_transaction_id,
                "missing local transaction id did not fail closed");

  auto missing_inventory = ValidRequest();
  missing_inventory.proof.mga_inventory_proof_present = false;
  RequireStatus(index::ApplyEngineOwnedExactRecheck(missing_inventory),
                index::EngineOwnedExactRecheckStatus::
                    missing_mga_inventory_proof,
                "missing MGA inventory proof did not fail closed");

  auto missing_snapshot = ValidRequest();
  missing_snapshot.proof.mga_snapshot_proof_present = false;
  RequireStatus(index::ApplyEngineOwnedExactRecheck(missing_snapshot),
                index::EngineOwnedExactRecheckStatus::
                    missing_mga_snapshot_proof,
                "missing MGA snapshot proof did not fail closed");

  auto stale = ValidRequest();
  stale.proof.mga_snapshot_fresh = false;
  RequireStatus(index::ApplyEngineOwnedExactRecheck(stale),
                index::EngineOwnedExactRecheckStatus::stale_mga_snapshot,
                "stale MGA snapshot did not fail closed");

  auto mga_failed = ValidRequest();
  mga_failed.candidate.mga_visible = false;
  RequireStatus(index::ApplyEngineOwnedExactRecheck(mga_failed),
                index::EngineOwnedExactRecheckStatus::
                    mga_visibility_recheck_failed,
                "failed MGA visibility recheck did not fail closed");

  auto missing_security = ValidRequest();
  missing_security.proof.security_context_present = false;
  RequireStatus(index::ApplyEngineOwnedExactRecheck(missing_security),
                index::EngineOwnedExactRecheckStatus::
                    missing_security_context,
                "missing security context did not fail closed");

  auto missing_authz = ValidRequest();
  missing_authz.proof.authorization_proven = false;
  RequireStatus(index::ApplyEngineOwnedExactRecheck(missing_authz),
                index::EngineOwnedExactRecheckStatus::
                    missing_authorization_proof,
                "missing authorization proof did not fail closed");

  auto security_failed = ValidRequest();
  security_failed.candidate.security_visible = false;
  RequireStatus(index::ApplyEngineOwnedExactRecheck(security_failed),
                index::EngineOwnedExactRecheckStatus::security_recheck_failed,
                "failed security recheck did not fail closed");

  auto missing_predicate = ValidRequest();
  missing_predicate.proof.predicate_exactness_proven = false;
  RequireStatus(index::ApplyEngineOwnedExactRecheck(missing_predicate),
                index::EngineOwnedExactRecheckStatus::missing_predicate_proof,
                "missing predicate proof did not fail closed");

  auto predicate_failed = ValidRequest();
  predicate_failed.candidate.predicate_exact = false;
  RequireStatus(index::ApplyEngineOwnedExactRecheck(predicate_failed),
                index::EngineOwnedExactRecheckStatus::predicate_recheck_failed,
                "failed predicate recheck did not fail closed");

  auto missing_source = ValidRequest();
  missing_source.proof.exact_source_payload_verified = false;
  RequireStatus(index::ApplyEngineOwnedExactRecheck(missing_source),
                index::EngineOwnedExactRecheckStatus::
                    missing_exact_source_proof,
                "missing exact source proof did not fail closed");
}

void AuthorityAndExternalRoutesFailClosed() {
  auto authorization = ValidRequest();
  authorization.authority_boundary.authorization_authority = true;
  RequireStatus(index::ApplyEngineOwnedExactRecheck(authorization),
                index::EngineOwnedExactRecheckStatus::
                    forbidden_authority_claim,
                "authorization authority claim did not fail closed");

  auto reference = ValidRequest();
  reference.authority_boundary.reference_authority = true;
  RequireStatus(index::ApplyEngineOwnedExactRecheck(reference),
                index::EngineOwnedExactRecheckStatus::
                    external_authority_refused,
                "reference authority did not fail closed");

  auto parser = ValidRequest();
  parser.authority_boundary.parser_authority = true;
  RequireStatus(index::ApplyEngineOwnedExactRecheck(parser),
                index::EngineOwnedExactRecheckStatus::
                    external_authority_refused,
                "parser authority did not fail closed");

  auto wal = ValidRequest();
  wal.authority_boundary.wal_authority = true;
  RequireStatus(index::ApplyEngineOwnedExactRecheck(wal),
                index::EngineOwnedExactRecheckStatus::
                    external_authority_refused,
                "WAL authority did not fail closed");

  auto provider = ValidRequest();
  provider.authority_boundary.provider_authority = true;
  RequireStatus(index::ApplyEngineOwnedExactRecheck(provider),
                index::EngineOwnedExactRecheckStatus::
                    external_authority_refused,
                "provider authority did not fail closed");

  auto local_cluster = ValidRequest();
  local_cluster.authority_boundary.local_cluster_authority = true;
  RequireStatus(index::ApplyEngineOwnedExactRecheck(local_cluster),
                index::EngineOwnedExactRecheckStatus::
                    external_authority_refused,
                "local cluster authority did not fail closed");
}

void SuccessorOverclaimsFailClosed() {
  auto ceic038 = ValidRequest();
  ceic038.successor_claims.ceic_038_family_classification_claimed = true;
  RequireStatus(index::ApplyEngineOwnedExactRecheck(ceic038),
                index::EngineOwnedExactRecheckStatus::
                    successor_or_enterprise_overclaim,
                "CEIC-038 overclaim did not fail closed");

  auto ceic039 = ValidRequest();
  ceic039.successor_claims.ceic_039_specialized_provider_closure_claimed =
      true;
  RequireStatus(index::ApplyEngineOwnedExactRecheck(ceic039),
                index::EngineOwnedExactRecheckStatus::
                    successor_or_enterprise_overclaim,
                "CEIC-039 overclaim did not fail closed");

  auto ceic040 = ValidRequest();
  ceic040.successor_claims.ceic_040_runtime_metrics_claimed = true;
  RequireStatus(index::ApplyEngineOwnedExactRecheck(ceic040),
                index::EngineOwnedExactRecheckStatus::
                    successor_or_enterprise_overclaim,
                "CEIC-040 overclaim did not fail closed");

  auto ceic041 = ValidRequest();
  ceic041.successor_claims.ceic_041_crash_matrix_claimed = true;
  RequireStatus(index::ApplyEngineOwnedExactRecheck(ceic041),
                index::EngineOwnedExactRecheckStatus::
                    successor_or_enterprise_overclaim,
                "CEIC-041 overclaim did not fail closed");

  auto ceic042 = ValidRequest();
  ceic042.successor_claims.ceic_042_readiness_drift_claimed = true;
  RequireStatus(index::ApplyEngineOwnedExactRecheck(ceic042),
                index::EngineOwnedExactRecheckStatus::
                    successor_or_enterprise_overclaim,
                "CEIC-042 overclaim did not fail closed");

  auto enterprise = ValidRequest();
  enterprise.successor_claims.enterprise_readiness_claimed = true;
  RequireStatus(index::ApplyEngineOwnedExactRecheck(enterprise),
                index::EngineOwnedExactRecheckStatus::
                    successor_or_enterprise_overclaim,
                "enterprise readiness overclaim did not fail closed");
}

void LightweightPolicyCompatibilityPreserved() {
  index::IndexCandidate candidate;
  candidate.locator.table_uuid = TestUuid(UuidKind::object, 11);
  candidate.locator.row_uuid = TestUuid(UuidKind::row, 12);
  candidate.locator.version_uuid = TestUuid(UuidKind::row, 13);
  candidate.locator.local_transaction_id = 99;
  candidate.mga_visible = true;
  candidate.predicate_exact = true;
  candidate.security_visible = true;
  auto result =
      index::ApplyIndexRecheckPolicy({candidate}, index::IndexRecheckPolicy{});
  Require(result.ok(), "legacy lightweight recheck policy failed");
  Require(result.accepted.size() == 1,
          "legacy lightweight recheck policy no longer accepts valid input");
  Require(result.metrics.rechecks == 1,
          "legacy lightweight recheck policy recheck count changed");
}

}  // namespace

int main() {
  ExactCandidateAcceptedAfterAllProofs();
  LossyCandidateRequiresExactFallback();
  ApproximateVectorCandidateRequiresExactRerank();
  DocumentTextAndGraphSourcesRequireExactPayloads();
  MissingCoreProofsFailClosed();
  AuthorityAndExternalRoutesFailClosed();
  SuccessorOverclaimsFailClosed();
  LightweightPolicyCompatibilityPreserved();
  std::cout << "ceic_037_engine_owned_exact_recheck_gate=pass\n";
  return EXIT_SUCCESS;
}
