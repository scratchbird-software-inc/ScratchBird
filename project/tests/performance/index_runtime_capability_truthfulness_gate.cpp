// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_access_method.hpp"
#include "index_family_registry.hpp"
#include "index_maintenance.hpp"
#include "index_optimizer_integration.hpp"
#include "index_route_capability.hpp"
#include "policy_blocked_index_admission.hpp"

#include <cstdlib>
#include <iostream>
#include <set>
#include <string>

namespace idx = scratchbird::core::index;

namespace {

void Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "index_runtime_capability_truthfulness_gate: " << message
              << '\n';
    std::exit(1);
  }
}

bool HasArgument(const scratchbird::core::platform::DiagnosticRecord& diagnostic,
                 const std::string& key,
                 const std::string& value) {
  for (const auto& argument : diagnostic.arguments) {
    if (argument.key == key && argument.value == value) {
      return true;
    }
  }
  return false;
}

scratchbird::core::platform::TypedUuid TestObjectUuid(unsigned char seed) {
  scratchbird::core::platform::TypedUuid uuid;
  uuid.kind = scratchbird::core::platform::UuidKind::object;
  for (std::size_t i = 0; i < uuid.value.bytes.size(); ++i) {
    uuid.value.bytes[i] = static_cast<scratchbird::core::platform::byte>(
        seed + static_cast<unsigned char>(i + 1));
  }
  uuid.value.bytes[6] =
      static_cast<scratchbird::core::platform::byte>((uuid.value.bytes[6] & 0x0f) | 0x70);
  uuid.value.bytes[8] =
      static_cast<scratchbird::core::platform::byte>((uuid.value.bytes[8] & 0x3f) | 0x80);
  return uuid;
}

idx::IndexReadinessPlanAdmissionEvidence ReadinessEvidence(
    idx::IndexFamily family,
    idx::IndexRouteKind route = idx::IndexRouteKind::sql_select) {
  const auto* state = idx::FindBuiltinIndexRouteCapabilityState(route, family);
  Require(state != nullptr && state->route_complete(),
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
      "sha256:ceic060-runtime-truthfulness-readiness";
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
  evidence.supports_read = state->supports_read;
  evidence.supports_equality_lookup = state->supports_equality_lookup;
  evidence.supports_ordered_range = state->supports_ordered_range;
  evidence.supports_negative_prune = state->supports_negative_prune;
  evidence.supports_summary_segment_prune =
      state->supports_summary_segment_prune;
  evidence.produces_candidate_set = state->produces_candidate_set;
  evidence.approximate_candidate_source =
      state->approximate_candidate_source;
  evidence.requires_exact_recheck = state->requires_exact_recheck;
  evidence.requires_mga_recheck = state->requires_mga_recheck;
  evidence.requires_security_recheck = state->requires_security_recheck;
  evidence.requires_exact_rerank = state->requires_exact_rerank;
  evidence.exact_recheck_proven = true;
  evidence.mga_recheck_proven = true;
  evidence.security_recheck_proven = true;
  evidence.exact_rerank_proven = state->requires_exact_rerank;
  evidence.operation_metrics_producer_proven = true;
  evidence.support_bundle_producer_proven = true;
  evidence.crash_reopen_proven = true;
  evidence.corruption_cleanup_proven = true;
  evidence.cleanup_horizon_proven = true;
  evidence.storage_integration_proven = true;
  evidence.external_cluster_provider_only = true;
  return evidence;
}

const idx::IndexFamilyPhysicalCapabilityState& RequireState(
    idx::IndexFamily family) {
  const auto* state = idx::FindBuiltinIndexFamilyPhysicalCapabilityState(family);
  Require(state != nullptr,
          std::string("missing physical capability state for ") +
              idx::IndexFamilyName(family));
  return *state;
}

void RequireBlocker(idx::IndexFamily family,
                    idx::IndexFamilyPhysicalCapabilityBlocker blocker,
                    const std::string& diagnostic_code) {
  const auto& state = RequireState(family);
  Require(state.blocker == blocker,
          std::string("blocker mismatch for ") + idx::IndexFamilyName(family));
  Require(state.blocker_diagnostic_code == diagnostic_code,
          std::string("diagnostic code mismatch for ") +
              idx::IndexFamilyName(family));
  Require(!state.blocker_detail.empty(),
          std::string("blocker detail missing for ") +
              idx::IndexFamilyName(family));
  const auto diagnostic = idx::MakeIndexFamilyCapabilityBlockerDiagnostic(
      state.runtime_available ? scratchbird::core::platform::Status{}
                              : scratchbird::core::platform::Status{
                                    scratchbird::core::platform::StatusCode::
                                        platform_required_feature_missing,
                                    scratchbird::core::platform::Severity::error,
                                    scratchbird::core::platform::Subsystem::
                                        engine},
      state);
  Require(diagnostic.diagnostic_code == diagnostic_code,
          std::string("blocker diagnostic emission mismatch for ") +
              idx::IndexFamilyName(family));
  Require(HasArgument(diagnostic, "family", idx::IndexFamilyName(family)),
          std::string("diagnostic family argument missing for ") +
              idx::IndexFamilyName(family));
  Require(HasArgument(diagnostic,
                      "blocker",
                      idx::IndexFamilyPhysicalCapabilityBlockerName(blocker)),
          std::string("diagnostic blocker argument missing for ") +
              idx::IndexFamilyName(family));
}

void RequireComplete(idx::IndexFamily family) {
  const auto& state = RequireState(family);
  Require(state.blocker == idx::IndexFamilyPhysicalCapabilityBlocker::none,
          std::string("completed family kept a blocker for ") +
              idx::IndexFamilyName(family));
  Require(state.runtime_available && state.benchmark_clean,
          std::string("completed family did not advertise runtime benchmark-clean closure for ") +
              idx::IndexFamilyName(family));
  Require(state.physically_complete(),
          std::string("completed family is not physically complete for ") +
              idx::IndexFamilyName(family));
  Require(state.blocker_diagnostic_code.empty() &&
              state.blocker_message_key.empty(),
          std::string("completed family retained stale blocker diagnostics for ") +
              idx::IndexFamilyName(family));
}

bool CompletedFamily(idx::IndexFamily family) {
  return family != idx::IndexFamily::reference_emulated &&
         family != idx::IndexFamily::policy_blocked &&
         family != idx::IndexFamily::unknown;
}

bool GenericAccessMethodFamily(idx::IndexFamily family) {
  return family != idx::IndexFamily::bitmap;
}

void RequireDescriptorStateParity() {
  const auto& descriptors = idx::BuiltinIndexFamilyDescriptors();
  const auto& states = idx::BuiltinIndexFamilyPhysicalCapabilityStates();
  Require(descriptors.size() == states.size(),
          "descriptor/state table size mismatch");

  std::set<idx::IndexFamily> seen_states;
  for (const auto& state : states) {
    Require(seen_states.insert(state.family).second,
            std::string("duplicate capability state for ") +
                idx::IndexFamilyName(state.family));
    Require(state.declared_capability,
            std::string("declared capability missing for ") +
                idx::IndexFamilyName(state.family));
    if (CompletedFamily(state.family)) {
      RequireComplete(state.family);
    } else {
      Require(state.blocker != idx::IndexFamilyPhysicalCapabilityBlocker::none,
              std::string("incomplete state missing exact blocker for ") +
                  idx::IndexFamilyName(state.family));
      Require(!state.runtime_available,
              std::string("runtime advertised before physical closure for ") +
                  idx::IndexFamilyName(state.family));
      Require(!state.benchmark_clean,
              std::string("benchmark-clean advertised before physical closure for ") +
                  idx::IndexFamilyName(state.family));
      Require(!state.physically_complete(),
              std::string("test baseline unexpectedly complete for ") +
                  idx::IndexFamilyName(state.family));
      Require(!state.blocker_diagnostic_code.empty() &&
                  !state.blocker_message_key.empty(),
              std::string("exact blocker diagnostic missing for ") +
                  idx::IndexFamilyName(state.family));
    }

    const auto caps = idx::CapabilitiesForFamily(state.family);
    const bool generic_access = GenericAccessMethodFamily(state.family);
    Require(caps.supports_scan ==
                (generic_access && state.runtime_available &&
                 state.physical_reader),
            std::string("scan capability drift for ") +
                idx::IndexFamilyName(state.family));
    Require(caps.supports_insert ==
                (generic_access && state.runtime_available &&
                 state.physical_writer),
            std::string("insert capability drift for ") +
                idx::IndexFamilyName(state.family));
    Require(caps.supports_rebuild ==
                (generic_access && state.runtime_available && state.rebuild),
            std::string("rebuild capability drift for ") +
                idx::IndexFamilyName(state.family));
  }

  for (const auto& descriptor : descriptors) {
    Require(seen_states.count(descriptor.family) == 1,
            std::string("descriptor lacks physical capability state for ") +
                descriptor.id);
  }
}

void RequireRuntimeRefusalsUseCapabilityDiagnostics() {
  for (const auto family : {idx::IndexFamily::reference_emulated}) {
    const auto& state = RequireState(family);
    idx::IndexOptimizerRequest optimizer;
    optimizer.index_uuid = TestObjectUuid(0x71);
    optimizer.family = family;
    const auto optimized = idx::PlanIndexOptimizerPath(optimizer);
    Require(!optimized.ok() && optimized.fallback_full_scan,
            std::string("optimizer route did not refuse incomplete family ") +
                idx::IndexFamilyName(family));
    Require(optimized.diagnostic.diagnostic_code ==
                state.blocker_diagnostic_code,
            std::string("optimizer route did not emit exact blocker for ") +
                idx::IndexFamilyName(family));
  }

  for (const auto family : {idx::IndexFamily::btree,
                            idx::IndexFamily::hash,
                            idx::IndexFamily::bitmap,
                            idx::IndexFamily::document_path}) {
    idx::IndexOptimizerRequest optimizer;
    optimizer.index_uuid = TestObjectUuid(0x70);
    optimizer.family = family;
    if (family == idx::IndexFamily::bitmap) {
      optimizer.category = idx::IndexPlanCategory::bitmap_combine;
      optimizer.requires_candidate_set = true;
    }
    if (family == idx::IndexFamily::document_path) {
      optimizer.route = idx::IndexRouteKind::nosql_document;
      optimizer.category = idx::IndexPlanCategory::inverted_search;
      optimizer.requires_candidate_set = true;
    }
    auto readiness = ReadinessEvidence(optimizer.family, optimizer.route);
    optimizer.readiness_evidence = &readiness;
    const auto optimized = idx::PlanIndexOptimizerPath(optimizer);
    Require(optimized.ok() && !optimized.fallback_full_scan,
            std::string("optimizer route did not admit completed physical family ") +
                idx::IndexFamilyName(family));
  }

  idx::IndexOptimizerRequest vector_optimizer;
  vector_optimizer.index_uuid = TestObjectUuid(0x70);
  vector_optimizer.family = idx::IndexFamily::vector_hnsw;
  vector_optimizer.route = idx::IndexRouteKind::nosql_vector;
  vector_optimizer.category = idx::IndexPlanCategory::vector_search;
  vector_optimizer.approximate = true;
  auto vector_readiness =
      ReadinessEvidence(vector_optimizer.family, vector_optimizer.route);
  vector_optimizer.readiness_evidence = &vector_readiness;
  const auto vector_refused = idx::PlanIndexOptimizerPath(vector_optimizer);
  Require(!vector_refused.ok() && vector_refused.fallback_full_scan,
          "approximate vector route was admitted without exact rerank");
  Require(vector_refused.diagnostic.diagnostic_code ==
              "INDEX.OPTIMIZER_READINESS_EVIDENCE.EXACT_RERANK_REQUIRED",
          "approximate vector missing exact rerank diagnostic");
  vector_optimizer.exact_rerank_available = true;
  const auto vector_admitted = idx::PlanIndexOptimizerPath(vector_optimizer);
  Require(vector_admitted.ok() && vector_admitted.rerank,
          "approximate vector route did not require exact rerank");

  idx::IndexOptimizerRequest policy_blocked_optimizer;
  policy_blocked_optimizer.index_uuid = TestObjectUuid(0x73);
  policy_blocked_optimizer.family = idx::IndexFamily::policy_blocked;
  const auto policy_blocked = idx::PlanIndexOptimizerPath(policy_blocked_optimizer);
  Require(!policy_blocked.ok() && policy_blocked.fallback_full_scan,
          "policy-blocked family was admitted to optimizer route");

  idx::IndexMaintenanceRequest maintenance;
  maintenance.index_uuid = TestObjectUuid(0x72);
  maintenance.family = idx::IndexFamily::btree;
  maintenance.operation = idx::IndexMaintenanceOperation::verify;
  maintenance.page_budget = 1;
  const auto maintained = idx::PlanIndexMaintenance(maintenance);
  Require(maintained.ok(),
          "maintenance route did not admit completed physical B-tree validation");
}

void RequireRouteCapabilityMatrixTruthfulness() {
  const auto& descriptors = idx::BuiltinIndexFamilyDescriptors();
  const auto& routes = idx::BuiltinIndexRouteCapabilityStates();
  Require(routes.size() == descriptors.size() * 11,
          "route capability matrix size mismatch");

  const auto* btree_dml =
      idx::FindBuiltinIndexRouteCapabilityState(idx::IndexRouteKind::dml_update,
                                                idx::IndexFamily::btree);
  Require(btree_dml != nullptr && btree_dml->route_complete() &&
              btree_dml->supports_write && btree_dml->supports_mutation,
          "B-tree DML update route was not admitted");

  const auto* hash_dml =
      idx::FindBuiltinIndexRouteCapabilityState(idx::IndexRouteKind::dml_update,
                                                idx::IndexFamily::hash);
  Require(hash_dml != nullptr && hash_dml->route_complete() &&
              hash_dml->family_physical_complete &&
              hash_dml->supports_write && hash_dml->supports_mutation &&
              hash_dml->requires_exact_recheck,
          "hash DML route was not admitted with exact recheck");

  const auto* hash_sql =
      idx::FindBuiltinIndexRouteCapabilityState(idx::IndexRouteKind::sql_select,
                                                idx::IndexFamily::hash);
  Require(hash_sql != nullptr && hash_sql->route_complete() &&
              hash_sql->supports_read && hash_sql->supports_equality_lookup,
          "hash SQL equality route was not admitted");

  const auto* bloom_sql =
      idx::FindBuiltinIndexRouteCapabilityState(idx::IndexRouteKind::sql_select,
                                                idx::IndexFamily::bloom);
  Require(bloom_sql != nullptr && bloom_sql->route_complete() &&
              bloom_sql->supports_negative_prune &&
              bloom_sql->requires_exact_recheck,
          "Bloom route did not advertise negative prune plus exact recheck");

  const auto* document =
      idx::FindBuiltinIndexRouteCapabilityState(idx::IndexRouteKind::nosql_document,
                                                idx::IndexFamily::document_path);
  Require(document != nullptr && document->route_complete() &&
              document->supports_read && document->produces_candidate_set,
          "document path NoSQL route was not admitted");

  const auto* vector =
      idx::FindBuiltinIndexRouteCapabilityState(idx::IndexRouteKind::nosql_vector,
                                                idx::IndexFamily::vector_hnsw);
  Require(vector != nullptr && vector->route_complete() &&
              vector->approximate_candidate_source &&
              vector->requires_exact_rerank &&
              vector->requires_mga_recheck &&
              vector->requires_security_recheck,
          "HNSW vector route did not preserve exact-rerank/MGA/security requirements");

  const auto* reference =
      idx::FindBuiltinIndexRouteCapabilityState(idx::IndexRouteKind::sql_select,
                                                idx::IndexFamily::reference_emulated);
  Require(reference != nullptr && !reference->route_complete() &&
              !reference->family_physical_complete,
          "reference-emulated route was promoted to physical capability");
}

void RequirePolicyBlockedTruthfulness() {
  const auto* descriptor =
      idx::FindBuiltinIndexFamily(idx::IndexFamily::policy_blocked);
  Require(descriptor != nullptr, "policy_blocked descriptor missing");
  Require(descriptor->completion ==
              idx::IndexCompletionStatus::policy_blocked_alpha,
          "policy_blocked completion status promoted");
  Require(descriptor->persistence == idx::IndexPersistenceClass::policy_blocked,
          "policy_blocked persistence promoted");
  Require(!descriptor->persistent,
          "policy_blocked became persistent physical storage");

  const auto& state = RequireState(idx::IndexFamily::policy_blocked);
  Require(!state.runtime_available && !state.benchmark_clean,
          "policy_blocked advertised runtime or benchmark-clean capability");
  Require(!state.physically_complete(),
          "policy_blocked became a complete physical provider");

  idx::IndexOptimizerRequest optimizer;
  optimizer.index_uuid = TestObjectUuid(0x73);
  optimizer.family = idx::IndexFamily::policy_blocked;
  const auto optimized = idx::PlanIndexOptimizerPath(optimizer);
  Require(!optimized.ok() && optimized.fallback_full_scan,
          "policy_blocked optimizer route did not fail closed");
  Require(optimized.diagnostic.diagnostic_code ==
              "INDEX.POLICY_BLOCKED.POLICY_NOT_ACCEPTED",
          "policy_blocked optimizer route did not emit exact policy diagnostic");
  Require(HasArgument(optimized.diagnostic,
                      "required_policy",
                      "SB_POLICY_INDEX_ADVANCED_VECTOR_NOT_ACCEPTED_ALPHA"),
          "policy_blocked diagnostic missing required policy");
  Require(HasArgument(optimized.diagnostic, "fallback_path", "full_scan"),
          "policy_blocked diagnostic missing fallback path");
}

void RequireReferenceEmulatedNonPhysicalMappingSurface() {
  const auto* descriptor =
      idx::FindBuiltinIndexFamily(idx::IndexFamily::reference_emulated);
  Require(descriptor != nullptr, "reference_emulated descriptor missing");
  Require(descriptor->persistence == idx::IndexPersistenceClass::reference_emulated,
          "reference_emulated persistence class drifted");
  Require(descriptor->key_model == idx::IndexKeyModel::reference_defined,
          "reference_emulated key model drifted");
  Require(!descriptor->persistent,
          "reference_emulated descriptor became persistent physical storage");

  const auto& state = RequireState(idx::IndexFamily::reference_emulated);
  Require(!state.runtime_available,
          "reference_emulated advertised runtime availability directly");
  Require(!state.benchmark_clean,
          "reference_emulated advertised benchmark-clean physical completion");
  Require(!state.physically_complete(),
          "reference_emulated became complete as its own physical family");
  Require(state.blocker ==
              idx::IndexFamilyPhysicalCapabilityBlocker::contract_only,
          "reference_emulated blocker is not contract-only");
  Require(state.blocker_detail.find("semantic mapping only") !=
              std::string::npos,
          "reference_emulated blocker does not name mapping-only semantics");
  Require(state.blocker_detail.find("cannot own visibility") !=
              std::string::npos,
          "reference_emulated blocker does not reject authority ownership");
}

}  // namespace

int main() {
  RequireDescriptorStateParity();
  RequireReferenceEmulatedNonPhysicalMappingSurface();
  RequireBlocker(
      idx::IndexFamily::reference_emulated,
      idx::IndexFamilyPhysicalCapabilityBlocker::contract_only,
      "INDEX.CAPABILITY.REFERENCE_EMULATED.CONTRACT_ONLY_NON_AUTHORITY_MAPPING");
  RequireComplete(idx::IndexFamily::btree);
  RequireComplete(idx::IndexFamily::unique_btree);
  RequireComplete(idx::IndexFamily::expression);
  RequireComplete(idx::IndexFamily::partial);
  RequireComplete(idx::IndexFamily::covering);
  RequireComplete(idx::IndexFamily::hash);
  RequireComplete(idx::IndexFamily::bitmap);
  RequireComplete(idx::IndexFamily::brin_zone);
  RequireComplete(idx::IndexFamily::bloom);
  RequireComplete(idx::IndexFamily::full_text);
  RequireComplete(idx::IndexFamily::gin);
  RequireComplete(idx::IndexFamily::inverted);
  RequireComplete(idx::IndexFamily::ngram);
  RequireComplete(idx::IndexFamily::sparse_wand);
  RequireComplete(idx::IndexFamily::spatial);
  RequireComplete(idx::IndexFamily::rtree);
  RequireComplete(idx::IndexFamily::gist);
  RequireComplete(idx::IndexFamily::spgist);
  RequireComplete(idx::IndexFamily::vector_exact);
  RequireComplete(idx::IndexFamily::vector_hnsw);
  RequireComplete(idx::IndexFamily::vector_ivf);
  RequireComplete(idx::IndexFamily::columnar_zone);
  RequireComplete(idx::IndexFamily::document_path);
  RequireComplete(idx::IndexFamily::graph);
  RequireComplete(idx::IndexFamily::temporary_work);
  RequireComplete(idx::IndexFamily::in_memory);
  RequireBlocker(idx::IndexFamily::policy_blocked,
                 idx::IndexFamilyPhysicalCapabilityBlocker::policy_blocked,
                 "INDEX.CAPABILITY.POLICY_BLOCKED.NOT_ACCEPTED_ALPHA");
  RequireRuntimeRefusalsUseCapabilityDiagnostics();
  RequireRouteCapabilityMatrixTruthfulness();
  RequirePolicyBlockedTruthfulness();
  std::cout << "index_runtime_capability_truthfulness_gate=passed\n";
  return 0;
}
