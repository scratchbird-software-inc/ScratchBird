// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// CEIC-039 focused validation for specialized persistent provider closure.
#include "specialized_persistent_provider_closure.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace index = scratchbird::core::index;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::byte;

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
        static_cast<byte>((seed * 43u + i * 29u + 0x67u) & 0xffu);
  }
  value.value.bytes[6] =
      static_cast<byte>((value.value.bytes[6] & 0x0fu) | 0x70u);
  value.value.bytes[8] =
      static_cast<byte>((value.value.bytes[8] & 0x3fu) | 0x80u);
  return value;
}

bool EvidenceHas(const index::SpecializedPersistentProviderClosureResult& result,
                 std::string_view token) {
  for (const auto& row : result.evidence) {
    if (row.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

index::IndexRouteKind RouteFor(index::IndexFamily family) {
  switch (family) {
    case index::IndexFamily::full_text:
    case index::IndexFamily::gin:
    case index::IndexFamily::inverted:
    case index::IndexFamily::ngram:
    case index::IndexFamily::sparse_wand:
      return index::IndexRouteKind::nosql_search;
    case index::IndexFamily::vector_exact:
    case index::IndexFamily::vector_hnsw:
    case index::IndexFamily::vector_ivf:
      return index::IndexRouteKind::nosql_vector;
    case index::IndexFamily::document_path:
      return index::IndexRouteKind::nosql_document;
    case index::IndexFamily::graph:
      return index::IndexRouteKind::nosql_graph;
    default:
      return index::IndexRouteKind::sql_select;
  }
}

unsigned SeedFor(index::IndexFamily family) {
  return static_cast<unsigned>(family) + 100u;
}

bool TokenFamily(index::IndexFamily family) {
  return family == index::IndexFamily::full_text ||
         family == index::IndexFamily::gin ||
         family == index::IndexFamily::inverted ||
         family == index::IndexFamily::ngram ||
         family == index::IndexFamily::sparse_wand;
}

index::IndexProviderAccessMethodContract ProviderContract(
    index::IndexFamily family,
    index::IndexRouteKind route) {
  const auto seed = SeedFor(family);
  index::IndexProviderAccessMethodContract contract;
  contract.family = family;
  contract.route = route;
  contract.provider.provider_id =
      std::string("ceic039-") + index::IndexFamilyName(family) + "-provider";
  contract.provider.provider_name =
      std::string("CEIC-039 ") + index::IndexFamilyName(family) + " Provider";
  contract.provider.provider_contract_version = "ceic039.provider.v1";
  contract.provider.persistent_access_method = true;
  contract.provider.provider_backed = true;
  contract.route_boundary.route_capability_present = true;
  contract.route_boundary.provider_route_supported = true;
  contract.route_boundary.static_registry_complete_capability_seen = false;
  contract.route_boundary.external_cluster_provider_only = true;
  contract.route_boundary.route_specific_boundary_declared = true;
  contract.generation.generation_uuid = TestUuid(UuidKind::object, seed + 1);
  contract.generation.generation_number = seed + 1000;
  contract.generation.provider_generation_id =
      std::string("ceic039-provider-generation-") +
      index::IndexFamilyName(family);
  contract.generation.root_identity_bound = true;
  contract.generation.cow_generation = true;
  contract.cleanup.oldest_active_transaction_id = seed + 7;
  contract.cleanup.cleanup_generation_floor = seed + 1000;
  contract.cleanup.engine_mga_horizon_bound = true;
  contract.cleanup.provider_cleanup_supported = true;
  contract.provider_evidence = {
      "ceic031_admitted_provider=true",
      std::string("ceic039_provider_family=") + index::IndexFamilyName(family),
      "ceic039_consumes_ceic031=true"};
  return contract;
}

index::IndexMGARecoveryContract MGAContract(index::IndexFamily family,
                                            index::IndexRouteKind route) {
  const auto seed = SeedFor(family);
  index::IndexMGARecoveryContract contract;
  contract.identity.family = family;
  contract.identity.route = route;
  contract.identity.provider_id =
      std::string("ceic039-") + index::IndexFamilyName(family) + "-provider";
  contract.identity.provider_contract_version = "ceic039.provider.v1";
  contract.identity.persistent_provider = true;
  contract.identity.external_cluster_provider_only = true;
  contract.mga_authority.inventory_present = true;
  contract.mga_authority.inventory_authoritative = true;
  contract.mga_authority.inventory_durable = true;
  contract.mga_authority.snapshot_present = true;
  contract.mga_authority.snapshot_authoritative = true;
  contract.mga_authority.cleanup_horizon_present = true;
  contract.mga_authority.cleanup_horizon_authoritative = true;
  contract.mga_authority.cleanup_horizon_engine_bound = true;
  contract.mga_authority.inventory_epoch = seed + 1000;
  contract.mga_authority.snapshot_epoch = seed + 1000;
  contract.mga_authority.cleanup_horizon_epoch = seed + 1000;
  contract.mga_authority.required_engine_evidence_epoch = seed + 1000;
  contract.mga_authority.inventory_evidence_id =
      std::string("mga-inventory-") + index::IndexFamilyName(family);
  contract.mga_authority.snapshot_evidence_id =
      std::string("mga-snapshot-") + index::IndexFamilyName(family);
  contract.mga_authority.cleanup_horizon_evidence_id =
      std::string("mga-horizon-") + index::IndexFamilyName(family);
  contract.generation.index_uuid = TestUuid(UuidKind::object, seed + 2);
  contract.generation.generation_uuid = TestUuid(UuidKind::object, seed + 3);
  contract.generation.generation_number = seed + 1000;
  contract.generation.cow_generation_number = seed + 1000;
  contract.generation.provider_generation_id =
      std::string("ceic039-provider-generation-") +
      index::IndexFamilyName(family);
  contract.generation.root_identity_bound = true;
  contract.generation.cow_generation_identity_bound = true;
  contract.generation.publish_state =
      index::IndexGenerationPublishState::published;
  contract.recovery.crash_classification =
      index::IndexCrashRecoveryClassification::crash_after_generation_publish;
  contract.recovery.corruption_classification =
      index::IndexCorruptionClassification::checksum_mismatch;
  contract.recovery.recovery_evidence_id =
      std::string("ceic039-recovery-") + index::IndexFamilyName(family);
  contract.recovery.durable_recovery_evidence = true;
  contract.recovery.replay_idempotent = true;
  contract.recovery.provider_evidence_only = true;
  contract.provider_evidence = {
      "ceic032_admitted_mga_contract=true",
      std::string("ceic039_mga_family=") + index::IndexFamilyName(family),
      "ceic039_consumes_ceic032=true"};
  return contract;
}

index::EngineOwnedExactRecheckRequest ExactRecheckRequest(
    index::IndexFamily family,
    index::IndexRouteKind route,
    const index::IndexRouteFamilyClassificationResult& classification) {
  const auto seed = SeedFor(family);
  index::EngineOwnedExactRecheckRequest request;
  request.candidate.key.encoded_key =
      std::string("ceic039-key-") + index::IndexFamilyName(family);
  request.candidate.key.requires_recheck = true;
  request.candidate.key.lossy =
      classification.requirements.exact_fallback_required;
  request.candidate.locator.table_uuid = TestUuid(UuidKind::object, seed + 4);
  request.candidate.locator.row_uuid = TestUuid(UuidKind::row, seed + 5);
  request.candidate.locator.version_uuid = TestUuid(UuidKind::row, seed + 6);
  request.candidate.locator.local_transaction_id = seed + 17;
  request.candidate.mga_visible = true;
  request.candidate.predicate_exact = true;
  request.candidate.security_visible = true;
  request.route_evidence.family = family;
  request.route_evidence.route = route;
  request.route_evidence.candidate_route = true;
  request.route_evidence.lossy_candidate =
      classification.produces_lossy_candidates ||
      classification.supports_negative_prune ||
      classification.supports_summary_segment_prune;
  request.route_evidence.approximate_candidate =
      classification.approximate_candidate_source;
  request.route_evidence.exact_fallback_available =
      classification.requirements.exact_fallback_required;
  request.route_evidence.exact_rerank_required =
      classification.requirements.exact_rerank_required;
  request.route_evidence.exact_rerank_proven =
      classification.requirements.exact_rerank_required;
  request.route_evidence.vector_payload_required =
      classification.vector_candidate_source;
  request.route_evidence.document_payload_required =
      classification.document_candidate_source;
  request.route_evidence.text_payload_required = TokenFamily(family);
  request.route_evidence.graph_payload_required =
      classification.graph_candidate_source;
  request.proof.mga_visibility_proven = true;
  request.proof.mga_inventory_proof_present = true;
  request.proof.mga_snapshot_proof_present = true;
  request.proof.mga_snapshot_fresh = true;
  request.proof.security_context_present = true;
  request.proof.authorization_proven = true;
  request.proof.predicate_exactness_proven = true;
  request.proof.exact_source_row_verified = true;
  request.proof.exact_source_payload_verified = true;
  request.proof.exact_vector_payload_verified =
      request.route_evidence.vector_payload_required;
  request.proof.exact_document_payload_verified =
      request.route_evidence.document_payload_required;
  request.proof.exact_text_payload_verified =
      request.route_evidence.text_payload_required;
  request.proof.exact_graph_payload_verified =
      request.route_evidence.graph_payload_required;
  request.proof.mga_inventory_proof_id =
      std::string("mga-inventory-proof-") + index::IndexFamilyName(family);
  request.proof.mga_snapshot_proof_id =
      std::string("mga-snapshot-proof-") + index::IndexFamilyName(family);
  request.proof.security_context_id =
      std::string("security-context-") + index::IndexFamilyName(family);
  request.proof.authorization_proof_id =
      std::string("authorization-proof-") + index::IndexFamilyName(family);
  request.proof.predicate_proof_id =
      std::string("predicate-proof-") + index::IndexFamilyName(family);
  request.proof.exact_source_proof_id =
      std::string("exact-source-proof-") + index::IndexFamilyName(family);
  if (classification.requirements.exact_fallback_required) {
    request.proof.exact_fallback_proof_id =
        std::string("exact-fallback-proof-") + index::IndexFamilyName(family);
  }
  if (classification.requirements.exact_rerank_required) {
    request.proof.exact_rerank_proof_id =
        std::string("exact-rerank-proof-") + index::IndexFamilyName(family);
  }
  return request;
}

index::SpecializedPersistentProviderClosureRequest RequestFor(
    index::IndexFamily family) {
  const auto route = RouteFor(family);
  const auto seed = SeedFor(family);
  index::SpecializedPersistentProviderClosureRequest request;
  request.family = family;
  request.route = route;
  request.provider_id =
      std::string("ceic039-") + index::IndexFamilyName(family) + "-provider";
  request.provider_admission =
      index::AdmitIndexProviderAccessMethod(ProviderContract(family, route));
  request.mga_recovery_contract =
      index::AdmitIndexMGARecoveryContract(MGAContract(family, route));
  index::IndexRouteClassificationRequest classify;
  classify.family = family;
  classify.route = route;
  classify.external_cluster_provider_only = true;
  request.route_classification = index::ClassifyIndexFamilyRoute(classify);
  request.declaration =
      index::BuildSpecializedProviderFamilyDeclaration(family);
  request.durable_provider.durable_storage_integration_proven = true;
  request.durable_provider.family_specific_physical_payload_proven = true;
  request.durable_provider.provider_artifact_format_version_proven = true;
  request.durable_provider.provider_open_reopen_identity_proven = true;
  request.durable_provider.provider_payload_integrity_proven = true;
  request.durable_provider.provider_evidence_only_not_authority = true;
  request.durable_provider.durable_storage_evidence_id =
      std::string("durable-storage-") + index::IndexFamilyName(family);
  request.durable_provider.provider_payload_evidence_id =
      std::string("payload-integrity-") + index::IndexFamilyName(family);
  request.durable_provider.artifact_format_evidence_id =
      std::string("artifact-format-") + index::IndexFamilyName(family);
  request.durable_provider.durable_evidence_rows = {
      "provider_payload_integrity=true",
      "open_reopen_identity=true",
      "storage_integration=true"};
  request.generation_identity.index_uuid = TestUuid(UuidKind::object, seed + 2);
  request.generation_identity.generation_uuid =
      TestUuid(UuidKind::object, seed + 3);
  request.generation_identity.root_or_segment_uuid =
      TestUuid(UuidKind::object, seed + 8);
  request.generation_identity.generation_number = seed + 1000;
  request.generation_identity.cow_generation_number = seed + 1000;
  request.generation_identity.cleanup_generation_floor = seed + 1000;
  request.generation_identity.oldest_active_transaction_id = seed + 7;
  request.generation_identity.provider_generation_id =
      std::string("ceic039-provider-generation-") +
      index::IndexFamilyName(family);
  request.generation_identity.root_or_provider_identity_evidence_id =
      std::string("root-identity-") + index::IndexFamilyName(family);
  request.generation_identity.cow_generation_publish_proven = true;
  request.generation_identity.root_or_provider_identity_bound = true;
  request.generation_identity.provider_generation_matches_ceic031 = true;
  request.generation_identity.generation_matches_ceic032 = true;
  request.generation_identity.cleanup_identity_matches_ceic031_ceic032 = true;
  request.generation_identity.publish_after_durable_mga_evidence = true;
  request.cleanup.oldest_active_transaction_id =
      request.generation_identity.oldest_active_transaction_id;
  request.cleanup.cleanup_generation_floor =
      request.generation_identity.cleanup_generation_floor;
  request.cleanup.engine_mga_horizon_bound = true;
  request.cleanup.cleanup_uses_engine_horizon = true;
  request.cleanup.cleanup_identity_matches_ceic031_ceic032 = true;
  request.cleanup.provider_cleanup_evidence_only = true;
  request.cleanup.cleanup_evidence_id =
      std::string("cleanup-evidence-") + index::IndexFamilyName(family);
  request.validation_repair_rebuild.validation_proven = true;
  request.validation_repair_rebuild.repair_supported = true;
  request.validation_repair_rebuild.rebuild_supported = true;
  request.validation_repair_rebuild.deterministic_diagnostics = true;
  request.validation_repair_rebuild.recommendation_matches_ceic032 = true;
  request.validation_repair_rebuild.evidence_only_not_crash_matrix = true;
  request.validation_repair_rebuild.validation_evidence_id =
      std::string("validation-") + index::IndexFamilyName(family);
  request.validation_repair_rebuild.repair_rebuild_evidence_id =
      std::string("repair-rebuild-") + index::IndexFamilyName(family);
  request.candidate_discipline.negative_prune_only =
      request.route_classification.negative_prune_only;
  request.candidate_discipline.summary_segment_prune_only =
      request.route_classification.summary_segment_prune_only;
  request.candidate_discipline.candidate_set_only =
      request.route_classification.produces_candidate_set;
  request.candidate_discipline.ranking_producer =
      request.route_classification.produces_ranking;
  request.candidate_discipline.seed_producer =
      request.route_classification.produces_seed_set;
  request.candidate_discipline.exact_recheck_handoff_required = true;
  request.candidate_discipline.false_positive_accounting_declared = true;
  request.candidate_discipline.final_row_authority = false;
  request.candidate_discipline.row_truth_authority = false;
  request.candidate_discipline.result_finality_authority = false;
  auto exact_request =
      ExactRecheckRequest(family, route, request.route_classification);
  request.exact_fallback_recheck.exact_recheck_result =
      index::ApplyEngineOwnedExactRecheck(exact_request);
  request.exact_fallback_recheck.exact_recheck_result_consumed = true;
  request.exact_fallback_recheck.exact_recheck_required = true;
  request.exact_fallback_recheck.exact_fallback_required =
      request.route_classification.requirements.exact_fallback_required;
  request.exact_fallback_recheck.exact_fallback_proven =
      request.route_classification.requirements.exact_fallback_required;
  request.exact_fallback_recheck.exact_rerank_required =
      request.route_classification.requirements.exact_rerank_required;
  request.exact_fallback_recheck.exact_rerank_proven =
      request.route_classification.requirements.exact_rerank_required;
  request.exact_fallback_recheck.exact_source_payload_proven = true;
  if (request.exact_fallback_recheck.exact_fallback_required) {
    request.exact_fallback_recheck.fallback_evidence_id =
        std::string("fallback-") + index::IndexFamilyName(family);
  }
  if (request.exact_fallback_recheck.exact_rerank_required) {
    request.exact_fallback_recheck.rerank_evidence_id =
        std::string("rerank-") + index::IndexFamilyName(family);
  }
  request.durable_provider_evidence_claimed = true;
  request.evidence = {
      "ceic039_specialized_provider_payload=true",
      std::string("ceic039_family=") + index::IndexFamilyName(family)};
  return request;
}

void RequireStatus(
    const index::SpecializedPersistentProviderClosureResult& result,
    index::SpecializedPersistentProviderClosureStatus status,
    std::string_view message) {
  Require(result.closure_status == status, message);
  Require(!result.ok(), "refused CEIC-039 result unexpectedly ok");
  Require(result.fail_closed, "refused CEIC-039 result did not fail closed");
  Require(!result.specialized_provider_closure_claimed,
          "refused CEIC-039 result claimed specialized provider closure");
  Require(!result.durable_provider_evidence,
          "refused CEIC-039 result claimed durable provider evidence");
  Require(!result.diagnostic.diagnostic_code.empty(),
          "refused CEIC-039 result lacks diagnostic code");
}

void RequireNoOverclaims(
    const index::SpecializedPersistentProviderClosureResult& result) {
  Require(!result.ceic_040_runtime_metric_producer_claimed,
          "CEIC-039 must not claim CEIC-040");
  Require(!result.ceic_041_crash_corruption_matrix_claimed,
          "CEIC-039 must not claim CEIC-041");
  Require(!result.ceic_042_readiness_drift_claimed,
          "CEIC-039 must not claim CEIC-042");
  Require(!result.all_index_readiness_claimed,
          "CEIC-039 must not claim all-index readiness");
  Require(!result.reference_dominance_claimed,
          "CEIC-039 must not claim reference dominance");
  Require(!result.enterprise_readiness_claimed,
          "CEIC-039 must not claim enterprise readiness");
}

void ValidSpecializedFamiliesAreAdmitted() {
  const std::vector<index::IndexFamily> families = {
      index::IndexFamily::bitmap,
      index::IndexFamily::brin_zone,
      index::IndexFamily::bloom,
      index::IndexFamily::full_text,
      index::IndexFamily::gin,
      index::IndexFamily::inverted,
      index::IndexFamily::ngram,
      index::IndexFamily::sparse_wand,
      index::IndexFamily::spatial,
      index::IndexFamily::rtree,
      index::IndexFamily::gist,
      index::IndexFamily::spgist,
      index::IndexFamily::vector_exact,
      index::IndexFamily::vector_hnsw,
      index::IndexFamily::vector_ivf,
      index::IndexFamily::columnar_zone,
      index::IndexFamily::document_path,
      index::IndexFamily::graph};

  for (const auto family : families) {
    auto request = RequestFor(family);
    Require(request.provider_admission.ok(), "provider admission setup failed");
    Require(request.mga_recovery_contract.ok(), "MGA contract setup failed");
    Require(request.route_classification.ok(), "route classification setup failed");
    Require(request.exact_fallback_recheck.exact_recheck_result.ok(),
            "exact recheck setup failed");
    const auto result = index::AdmitSpecializedPersistentProviderClosure(request);
    if (!result.ok()) {
      std::cerr << "family " << index::IndexFamilyName(family)
                << " failed with "
                << index::SpecializedPersistentProviderClosureStatusName(
                       result.closure_status)
                << '\n';
      Fail("valid specialized family was refused");
    }
    Require(result.closure_status ==
                index::SpecializedPersistentProviderClosureStatus::
                    admitted_specialized_provider_evidence,
            "valid family returned wrong closure status");
    Require(result.specialized_provider_closure_claimed,
            "valid family did not claim CEIC-039 closure evidence");
    Require(result.durable_provider_evidence,
            "valid family did not claim durable provider evidence");
    RequireNoOverclaims(result);
    Require(EvidenceHas(
                result,
                "ceic_search_key=CEIC_039_SPECIALIZED_PERSISTENT_PROVIDER_CLOSURE"),
            "CEIC-039 evidence anchor missing");
    Require(EvidenceHas(result,
                        "provider_closure_boundary=specialized_provider_evidence_only_pending_CEIC_040_CEIC_041_CEIC_042"),
            "CEIC-039 boundary evidence missing");
  }
}

void PriorReferencePolicyAndClusterPathsFailClosed() {
  RequireStatus(
      index::AdmitSpecializedPersistentProviderClosure(
          RequestFor(index::IndexFamily::btree)),
      index::SpecializedPersistentProviderClosureStatus::
          already_closed_by_prior_slice,
      "B-tree prior slice closure was not refused");
  RequireStatus(
      index::AdmitSpecializedPersistentProviderClosure(
          RequestFor(index::IndexFamily::hash)),
      index::SpecializedPersistentProviderClosureStatus::
          already_closed_by_prior_slice,
      "hash prior slice closure was not refused");

  index::SpecializedPersistentProviderClosureRequest reference;
  reference.family = index::IndexFamily::reference_emulated;
  reference.route = index::IndexRouteKind::sql_select;
  reference.provider_id = "reference";
  RequireStatus(
      index::AdmitSpecializedPersistentProviderClosure(reference),
      index::SpecializedPersistentProviderClosureStatus::
          reference_emulated_non_runtime,
      "reference-emulated family was not refused");

  index::SpecializedPersistentProviderClosureRequest policy;
  policy.family = index::IndexFamily::policy_blocked;
  policy.route = index::IndexRouteKind::sql_select;
  policy.provider_id = "policy";
  RequireStatus(
      index::AdmitSpecializedPersistentProviderClosure(policy),
      index::SpecializedPersistentProviderClosureStatus::
          policy_blocked_non_runtime,
      "policy-blocked family was not refused");

  auto cluster = RequestFor(index::IndexFamily::full_text);
  cluster.cluster_local_participation = true;
  RequireStatus(
      index::AdmitSpecializedPersistentProviderClosure(cluster),
      index::SpecializedPersistentProviderClosureStatus::
          cluster_external_provider_only,
      "local cluster participation was not refused");
}

void DependencyFailuresFailClosed() {
  auto missing_provider = RequestFor(index::IndexFamily::gin);
  missing_provider.provider_admission.admitted = false;
  missing_provider.provider_admission.fail_closed = true;
  RequireStatus(
      index::AdmitSpecializedPersistentProviderClosure(missing_provider),
      index::SpecializedPersistentProviderClosureStatus::
          provider_admission_not_admitted,
      "missing CEIC-031 provider admission was not refused");

  auto missing_mga = RequestFor(index::IndexFamily::gin);
  missing_mga.mga_recovery_contract.admitted = false;
  missing_mga.mga_recovery_contract.fail_closed = true;
  RequireStatus(
      index::AdmitSpecializedPersistentProviderClosure(missing_mga),
      index::SpecializedPersistentProviderClosureStatus::
          mga_recovery_contract_not_admitted,
      "missing CEIC-032 MGA contract was not refused");

  auto missing_classification = RequestFor(index::IndexFamily::gin);
  missing_classification.route_classification.classified = false;
  missing_classification.route_classification.fail_closed = true;
  RequireStatus(
      index::AdmitSpecializedPersistentProviderClosure(missing_classification),
      index::SpecializedPersistentProviderClosureStatus::
          route_classification_not_admitted,
      "missing CEIC-038 classification was not refused");

  auto missing_recheck = RequestFor(index::IndexFamily::gin);
  missing_recheck.exact_fallback_recheck.exact_recheck_result.admitted = false;
  missing_recheck.exact_fallback_recheck.exact_recheck_result.fail_closed = true;
  RequireStatus(
      index::AdmitSpecializedPersistentProviderClosure(missing_recheck),
      index::SpecializedPersistentProviderClosureStatus::
          exact_recheck_not_admitted,
      "missing CEIC-037 exact recheck was not refused");
}

void IdentityAndProofFailuresFailClosed() {
  auto identity = RequestFor(index::IndexFamily::document_path);
  identity.provider_id = "wrong-provider";
  RequireStatus(
      index::AdmitSpecializedPersistentProviderClosure(identity),
      index::SpecializedPersistentProviderClosureStatus::
          provider_mga_identity_mismatch,
      "provider identity mismatch was not refused");

  auto declaration = RequestFor(index::IndexFamily::document_path);
  declaration.declaration.provider_class =
      index::SpecializedPersistentProviderClass::vector_hnsw;
  RequireStatus(
      index::AdmitSpecializedPersistentProviderClosure(declaration),
      index::SpecializedPersistentProviderClosureStatus::
          provider_class_mismatch,
      "provider class mismatch was not refused");

  auto durable = RequestFor(index::IndexFamily::document_path);
  durable.durable_provider.family_specific_physical_payload_proven = false;
  RequireStatus(
      index::AdmitSpecializedPersistentProviderClosure(durable),
      index::SpecializedPersistentProviderClosureStatus::
          durable_provider_evidence_missing,
      "missing durable provider evidence was not refused");

  auto generation = RequestFor(index::IndexFamily::document_path);
  generation.generation_identity.generation_uuid = {};
  RequireStatus(
      index::AdmitSpecializedPersistentProviderClosure(generation),
      index::SpecializedPersistentProviderClosureStatus::
          generation_identity_missing,
      "missing generation identity was not refused");

  auto cleanup = RequestFor(index::IndexFamily::document_path);
  cleanup.cleanup.engine_mga_horizon_bound = false;
  RequireStatus(
      index::AdmitSpecializedPersistentProviderClosure(cleanup),
      index::SpecializedPersistentProviderClosureStatus::
          cleanup_horizon_not_engine_bound,
      "unbound cleanup horizon was not refused");

  auto validation = RequestFor(index::IndexFamily::document_path);
  validation.validation_repair_rebuild.repair_supported = false;
  RequireStatus(
      index::AdmitSpecializedPersistentProviderClosure(validation),
      index::SpecializedPersistentProviderClosureStatus::
          validation_repair_rebuild_missing,
      "missing validation repair rebuild proof was not refused");
}

void CandidateAndExactFallbackFailuresFailClosed() {
  auto bloom = RequestFor(index::IndexFamily::bloom);
  bloom.candidate_discipline.negative_prune_only = false;
  bloom.candidate_discipline.candidate_set_only = true;
  RequireStatus(
      index::AdmitSpecializedPersistentProviderClosure(bloom),
      index::SpecializedPersistentProviderClosureStatus::
          candidate_role_mismatch,
      "Bloom row-candidate role overclaim was not refused");

  auto brin = RequestFor(index::IndexFamily::brin_zone);
  brin.candidate_discipline.summary_segment_prune_only = false;
  brin.candidate_discipline.candidate_set_only = true;
  RequireStatus(
      index::AdmitSpecializedPersistentProviderClosure(brin),
      index::SpecializedPersistentProviderClosureStatus::
          candidate_role_mismatch,
      "BRIN row-candidate role overclaim was not refused");

  auto hnsw = RequestFor(index::IndexFamily::vector_hnsw);
  hnsw.exact_fallback_recheck.exact_rerank_proven = false;
  hnsw.exact_fallback_recheck.rerank_evidence_id.clear();
  RequireStatus(
      index::AdmitSpecializedPersistentProviderClosure(hnsw),
      index::SpecializedPersistentProviderClosureStatus::
          exact_fallback_recheck_rerank_missing,
      "missing HNSW exact rerank proof was not refused");

  auto sparse = RequestFor(index::IndexFamily::sparse_wand);
  sparse.exact_fallback_recheck.exact_fallback_proven = false;
  sparse.exact_fallback_recheck.fallback_evidence_id.clear();
  RequireStatus(
      index::AdmitSpecializedPersistentProviderClosure(sparse),
      index::SpecializedPersistentProviderClosureStatus::
          exact_fallback_recheck_rerank_missing,
      "missing sparse-WAND exact fallback proof was not refused");
}

void AuthorityAndSuccessorOverclaimsFailClosed() {
  auto authority = RequestFor(index::IndexFamily::full_text);
  authority.authority_boundary.row_truth_authority = true;
  RequireStatus(
      index::AdmitSpecializedPersistentProviderClosure(authority),
      index::SpecializedPersistentProviderClosureStatus::
          forbidden_authority_claim,
      "row truth authority claim was not refused");

  auto ceic040 = RequestFor(index::IndexFamily::full_text);
  ceic040.successor_claims.ceic_040_runtime_metric_producer_claimed = true;
  RequireStatus(
      index::AdmitSpecializedPersistentProviderClosure(ceic040),
      index::SpecializedPersistentProviderClosureStatus::
          successor_scope_overclaim,
      "CEIC-040 overclaim was not refused");

  auto ceic041 = RequestFor(index::IndexFamily::full_text);
  ceic041.successor_claims.ceic_041_crash_corruption_matrix_claimed = true;
  RequireStatus(
      index::AdmitSpecializedPersistentProviderClosure(ceic041),
      index::SpecializedPersistentProviderClosureStatus::
          successor_scope_overclaim,
      "CEIC-041 overclaim was not refused");

  auto ceic042 = RequestFor(index::IndexFamily::full_text);
  ceic042.successor_claims.ceic_042_readiness_drift_claimed = true;
  RequireStatus(
      index::AdmitSpecializedPersistentProviderClosure(ceic042),
      index::SpecializedPersistentProviderClosureStatus::
          successor_scope_overclaim,
      "CEIC-042 overclaim was not refused");

  auto enterprise = RequestFor(index::IndexFamily::full_text);
  enterprise.successor_claims.enterprise_readiness_claimed = true;
  RequireStatus(
      index::AdmitSpecializedPersistentProviderClosure(enterprise),
      index::SpecializedPersistentProviderClosureStatus::
          enterprise_readiness_overclaim,
      "enterprise readiness overclaim was not refused");
}

}  // namespace

int main() {
  ValidSpecializedFamiliesAreAdmitted();
  PriorReferencePolicyAndClusterPathsFailClosed();
  DependencyFailuresFailClosed();
  IdentityAndProofFailuresFailClosed();
  CandidateAndExactFallbackFailuresFailClosed();
  AuthorityAndSuccessorOverclaimsFailClosed();
  std::cout << "ceic_039_specialized_persistent_provider_closure_gate=pass\n";
  return EXIT_SUCCESS;
}
