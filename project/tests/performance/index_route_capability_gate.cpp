// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_optimizer_integration.hpp"
#include "index_route_capability.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <algorithm>
#include <iostream>
#include <string>
#include <string_view>

namespace idx = scratchbird::core::index;
namespace platform = scratchbird::core::platform;

namespace {

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << "index_route_capability_gate: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) {
    Fail(message);
  }
}

platform::TypedUuid TestObjectUuid(unsigned char seed) {
  platform::TypedUuid uuid;
  uuid.kind = platform::UuidKind::object;
  for (std::size_t i = 0; i < uuid.value.bytes.size(); ++i) {
    uuid.value.bytes[i] =
        static_cast<platform::byte>(seed + static_cast<unsigned char>(i + 1));
  }
  uuid.value.bytes[6] =
      static_cast<platform::byte>((uuid.value.bytes[6] & 0x0f) | 0x70);
  uuid.value.bytes[8] =
      static_cast<platform::byte>((uuid.value.bytes[8] & 0x3f) | 0x80);
  return uuid;
}

const idx::IndexRouteCapabilityState& Route(idx::IndexRouteKind route,
                                            idx::IndexFamily family) {
  const auto* state = idx::FindBuiltinIndexRouteCapabilityState(route, family);
  Require(state != nullptr,
          std::string("missing route state ") + idx::IndexRouteKindName(route) +
              ":" + idx::IndexFamilyName(family));
  return *state;
}

idx::IndexReadinessPlanAdmissionEvidence ReadinessEvidence(
    idx::IndexFamily family,
    idx::IndexRouteKind route = idx::IndexRouteKind::sql_select) {
  const auto& state = Route(route, family);
  Require(state.route_complete(),
          std::string("readiness fixture needs complete route ") +
              idx::IndexRouteKindName(route) + ":" +
              idx::IndexFamilyName(family));
  idx::IndexReadinessPlanAdmissionEvidence evidence;
  evidence.family = family;
  evidence.route = route;
  evidence.manifest_epoch = 42;
  evidence.registry_epoch = 42;
  evidence.route_proof_epoch = 42;
  evidence.source_evidence_digest =
      "sha256:ceic060-route-capability-readiness";
  evidence.generated_by =
      "project/tools/ceic_index_readiness_manifest.py#CEIC_030_INDEX_READINESS_MANIFEST_TOOL";
  evidence.generated_manifest_present = true;
  evidence.generated_manifest_current = true;
  evidence.generated_manifest_validated = true;
  evidence.source_digest_matches = true;
  evidence.runtime_registry_family_matches = true;
  evidence.runtime_registry_route_matches = true;
  evidence.runtime_family_available = true;
  evidence.runtime_route_complete = true;
  evidence.supports_read = state.supports_read;
  evidence.supports_equality_lookup = state.supports_equality_lookup;
  evidence.supports_ordered_range = state.supports_ordered_range;
  evidence.supports_negative_prune = state.supports_negative_prune;
  evidence.supports_summary_segment_prune =
      state.supports_summary_segment_prune;
  evidence.produces_candidate_set = state.produces_candidate_set;
  evidence.approximate_candidate_source = state.approximate_candidate_source;
  evidence.requires_exact_recheck = state.requires_exact_recheck;
  evidence.requires_mga_recheck = state.requires_mga_recheck;
  evidence.requires_security_recheck = state.requires_security_recheck;
  evidence.requires_exact_rerank = state.requires_exact_rerank;
  evidence.exact_recheck_proven = true;
  evidence.mga_recheck_proven = true;
  evidence.security_recheck_proven = true;
  evidence.exact_rerank_proven = state.requires_exact_rerank;
  evidence.operation_metrics_producer_proven = true;
  evidence.support_bundle_producer_proven = true;
  evidence.crash_reopen_proven = true;
  evidence.corruption_cleanup_proven = true;
  evidence.cleanup_horizon_proven = true;
  evidence.storage_integration_proven = true;
  evidence.external_cluster_provider_only = true;
  return evidence;
}

bool HasEvidence(const std::vector<std::string>& evidence, std::string_view needle) {
  return std::any_of(evidence.begin(),
                     evidence.end(),
                     [&](const std::string& value) {
                       return value == needle ||
                              value.find(needle) != std::string::npos;
                     });
}

void RequireDmlWriteMatrix() {
  for (const auto route : {idx::IndexRouteKind::dml_insert,
                           idx::IndexRouteKind::dml_update,
                           idx::IndexRouteKind::dml_delete}) {
    for (const auto& descriptor : idx::BuiltinIndexFamilyDescriptors()) {
      const auto& state = Route(route, descriptor.family);
      const bool accepted =
          descriptor.completion ==
              idx::IndexCompletionStatus::accepted_requires_full_implementation &&
          descriptor.persistence != idx::IndexPersistenceClass::reference_emulated &&
          descriptor.persistence != idx::IndexPersistenceClass::policy_blocked;
      if (accepted) {
        Require(state.route_complete() && state.supports_write &&
                    state.supports_mutation && state.benchmark_clean,
                "accepted DML route was not benchmark-clean");
      } else {
        Require(!state.route_complete(),
                "non-runtime family was admitted to DML write route");
      }
    }
  }
}

void RequireReadRouteMatrix() {
  const auto& hash = Route(idx::IndexRouteKind::sql_select,
                           idx::IndexFamily::hash);
  Require(hash.route_complete() && hash.supports_read &&
              hash.supports_equality_lookup && !hash.supports_ordered_range,
          "hash SQL route did not advertise equality-only semantics");
  Require(HasEvidence(hash.evidence, "hash_route_requires_keyed_algorithm=true"),
          "hash route did not advertise keyed-hash requirement");
  Require(HasEvidence(hash.evidence, "hash_negative_prune_supported=false"),
          "hash route incorrectly advertised negative-prune support");

  const auto& bloom = Route(idx::IndexRouteKind::sql_select,
                            idx::IndexFamily::bloom);
  Require(bloom.route_complete() && bloom.supports_negative_prune &&
              !bloom.produces_candidate_set && bloom.requires_exact_recheck,
          "Bloom route did not remain negative-prune-only");

  const auto& document = Route(idx::IndexRouteKind::nosql_document,
                               idx::IndexFamily::document_path);
  Require(document.route_complete() && document.supports_read &&
              document.produces_candidate_set,
          "document path NoSQL route was not complete");

  const auto& graph = Route(idx::IndexRouteKind::nosql_graph,
                            idx::IndexFamily::graph);
  Require(graph.route_complete() && graph.supports_read &&
              graph.produces_candidate_set,
          "graph NoSQL route was not complete");

  const auto& hnsw = Route(idx::IndexRouteKind::nosql_vector,
                           idx::IndexFamily::vector_hnsw);
  Require(hnsw.route_complete() && hnsw.approximate_candidate_source &&
              hnsw.requires_exact_rerank && hnsw.requires_exact_recheck,
          "HNSW route did not preserve candidate-plus-rerank semantics");

  const auto& wand = Route(idx::IndexRouteKind::nosql_search,
                           idx::IndexFamily::sparse_wand);
  Require(wand.route_complete() && wand.requires_exact_rerank &&
              wand.produces_candidate_set,
          "sparse WAND route did not require exact rerank");
}

void RequireOptimizerConsumesRouteMatrix() {
  idx::IndexOptimizerRequest hash;
  hash.index_uuid = TestObjectUuid(0x30);
  hash.family = idx::IndexFamily::hash;
  hash.route = idx::IndexRouteKind::sql_select;
  auto hash_readiness = ReadinessEvidence(hash.family, hash.route);
  hash.readiness_evidence = &hash_readiness;
  const auto hash_plan = idx::PlanIndexOptimizerPath(hash);
  Require(hash_plan.ok() && hash_plan.route_benchmark_clean,
          "optimizer did not consume hash SQL route capability");

  idx::IndexOptimizerRequest legacy_hash = hash;
  legacy_hash.hash_keyed_algorithm_active = false;
  const auto refused_legacy = idx::PlanIndexOptimizerPath(legacy_hash);
  Require(!refused_legacy.ok() &&
              refused_legacy.diagnostic.diagnostic_code ==
                  "INDEX.ROUTE_CAPABILITY.KEYED_HASH_REQUIRED",
          "optimizer admitted legacy hash route without policy");

  legacy_hash.hash_legacy_algorithm_allowed_by_policy = true;
  const auto allowed_legacy = idx::PlanIndexOptimizerPath(legacy_hash);
  Require(allowed_legacy.ok(),
          "optimizer did not allow legacy hash when explicit policy allowed it");

  idx::IndexOptimizerRequest high_assurance_hash = hash;
  high_assurance_hash.hash_high_assurance_required = true;
  const auto refused_high = idx::PlanIndexOptimizerPath(high_assurance_hash);
  Require(!refused_high.ok() &&
              refused_high.diagnostic.diagnostic_code ==
                  "INDEX.ROUTE_CAPABILITY.HIGH_ASSURANCE_HASH_REQUIRED",
          "optimizer admitted hash route without required high assurance");
  high_assurance_hash.hash_high_assurance_active = true;
  const auto admitted_high = idx::PlanIndexOptimizerPath(high_assurance_hash);
  Require(admitted_high.ok(),
          "optimizer did not admit high-assurance hash route");

  idx::IndexOptimizerRequest hnsw;
  hnsw.index_uuid = TestObjectUuid(0x31);
  hnsw.family = idx::IndexFamily::vector_hnsw;
  hnsw.route = idx::IndexRouteKind::nosql_vector;
  hnsw.category = idx::IndexPlanCategory::vector_search;
  hnsw.approximate = true;
  auto hnsw_readiness = ReadinessEvidence(hnsw.family, hnsw.route);
  hnsw.readiness_evidence = &hnsw_readiness;
  const auto refused = idx::PlanIndexOptimizerPath(hnsw);
  Require(!refused.ok() &&
              refused.diagnostic.diagnostic_code ==
                  "INDEX.OPTIMIZER_READINESS_EVIDENCE.EXACT_RERANK_REQUIRED",
          "optimizer admitted ANN route without exact rerank");
  hnsw.exact_rerank_available = true;
  const auto admitted = idx::PlanIndexOptimizerPath(hnsw);
  Require(admitted.ok() && admitted.rerank &&
              admitted.route_capability == "benchmark_clean",
          "optimizer did not consume ANN route after exact rerank proof");
}

void RequireBlockedMappingsStayBlocked() {
  const auto& reference = Route(idx::IndexRouteKind::sql_select,
                            idx::IndexFamily::reference_emulated);
  Require(!reference.route_complete() && !reference.family_physical_complete,
          "reference-emulated route became physical authority");
  const auto diagnostic = idx::MakeIndexRouteCapabilityDiagnostic(
      platform::Status{platform::StatusCode::platform_required_feature_missing,
                       platform::Severity::warning,
                       platform::Subsystem::engine},
      reference);
  Require(diagnostic.source_component == "sb_core_index.route_capability",
          "route diagnostic source mismatch");
}

}  // namespace

int main() {
  RequireDmlWriteMatrix();
  RequireReadRouteMatrix();
  RequireOptimizerConsumesRouteMatrix();
  RequireBlockedMappingsStayBlocked();
  std::cout << "index_route_capability_gate=passed\n";
  return EXIT_SUCCESS;
}
