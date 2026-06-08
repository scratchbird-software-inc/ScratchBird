// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_workload_resource_quota.hpp"
#include "compression_policy.hpp"
#include "index_optimizer_integration.hpp"
#include "index_route_capability.hpp"
#include "optimized_path_resource_governance.hpp"
#include "runtime_consumption_evidence.hpp"
#include "vector_training_recall_lifecycle.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace agents = scratchbird::core::agents;
namespace idx = scratchbird::core::index;
namespace opt = scratchbird::engine::optimizer;
namespace platform = scratchbird::core::platform;

// SEARCH_KEY: ORH_INDEX_CORRECTION_REBASE

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << "ORH-004 gate failure: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) {
    Fail(message);
  }
}

bool Has(const std::vector<std::string>& values, std::string_view expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

bool Contains(const std::vector<std::string>& values, std::string_view needle) {
  return std::any_of(values.begin(), values.end(), [&](const auto& value) {
    return value.find(needle) != std::string::npos;
  });
}

std::string Lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });
  return value;
}

void RequireNoRuntimeDocDependency(const std::vector<std::string>& values,
                                   std::string_view context) {
  for (const auto& value : values) {
    const auto lower = Lower(value);
    for (const auto* marker : {"docs/",
                               "execution-plans/",
                               "audit/",
                               "findings/",
                               "reference/",
                               "references/",
                               "contracts/"}) {
      Require(lower.find(marker) == std::string::npos,
              std::string(context) + " leaked runtime doc marker " + marker +
                  " in value: " + value);
    }
  }
}

void RequireNoBareGenericIndexRuntimeBlocker(
    const std::vector<std::string>& values,
    std::string_view context) {
  for (const auto& value : values) {
    Require(value != "INDEX_RUNTIME_UNPROVEN",
            std::string(context) + " used bare INDEX_RUNTIME_UNPROVEN");
    Require(value.find("=INDEX_RUNTIME_UNPROVEN") == std::string::npos,
            std::string(context) +
                " used bare key/value INDEX_RUNTIME_UNPROVEN: " + value);
    Require(value.find(":INDEX_RUNTIME_UNPROVEN") == std::string::npos,
            std::string(context) +
                " used bare diagnostic INDEX_RUNTIME_UNPROVEN: " + value);
  }
}

void AppendDiagnosticValues(const platform::DiagnosticRecord& diagnostic,
                            std::vector<std::string>* values) {
  values->push_back(diagnostic.diagnostic_code);
  values->push_back(diagnostic.message_key);
  values->push_back(diagnostic.source_component);
  values->push_back(diagnostic.remediation_hint);
  for (const auto& argument : diagnostic.arguments) {
    values->push_back(argument.key);
    values->push_back(argument.value);
  }
}

platform::Status RefuseStatus() {
  return {platform::StatusCode::platform_required_feature_missing,
          platform::Severity::warning,
          platform::Subsystem::engine};
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
  evidence.source_evidence_digest = "sha256:orh004-index-readiness";
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

std::vector<std::string> RouteValues(
    const idx::IndexRouteCapabilityState& state) {
  std::vector<std::string> values = state.evidence;
  values.push_back(state.route_diagnostic_code);
  values.push_back(state.route_message_key);
  values.push_back(state.route_detail);
  return values;
}

void RequireDiagnosticArgument(const platform::DiagnosticRecord& diagnostic,
                               std::string_view key,
                               std::string_view value) {
  const auto found = std::any_of(
      diagnostic.arguments.begin(), diagnostic.arguments.end(),
      [&](const auto& argument) {
        return argument.key == key && argument.value == value;
      });
  Require(found,
          "diagnostic missing argument " + std::string(key) + "=" +
              std::string(value));
}

idx::IndexOptimizerPlan RequireDispatchPlan(std::string_view row,
                                            idx::IndexFamily family,
                                            idx::IndexRouteKind route) {
  const auto& route_state = Route(route, family);
  Require(route_state.route_complete(),
          std::string(row) + " route was not benchmark-clean");
  Require(route_state.requires_mga_recheck &&
              route_state.requires_security_recheck,
          std::string(row) + " route dropped MGA/security recheck authority");

  idx::IndexOptimizerRequest request;
  request.index_uuid =
      TestObjectUuid(static_cast<unsigned char>(0x40 + row.size()));
  request.family = family;
  request.route = route;
  request.approximate = route_state.approximate_candidate_source;
  request.exact_rerank_available = route_state.requires_exact_rerank;
  request.hash_high_assurance_active = true;
  if (route == idx::IndexRouteKind::nosql_vector) {
    request.category = idx::IndexPlanCategory::vector_search;
    request.requires_candidate_set = true;
  } else if (route == idx::IndexRouteKind::nosql_graph) {
    request.category = idx::IndexPlanCategory::graph_search;
    request.requires_candidate_set = true;
  } else if (route == idx::IndexRouteKind::nosql_document ||
             route == idx::IndexRouteKind::nosql_search) {
    request.category = idx::IndexPlanCategory::inverted_search;
    request.requires_candidate_set = true;
  } else if (family == idx::IndexFamily::bitmap) {
    request.category = idx::IndexPlanCategory::bitmap_combine;
    request.requires_candidate_set = true;
  }
  auto readiness = ReadinessEvidence(family, route);
  request.readiness_evidence = &readiness;

  auto plan = idx::PlanIndexExecutorDispatch(request);
  Require(plan.ok(), std::string(row) + " corrected route was not admitted");
  Require(plan.route_benchmark_clean &&
              plan.route_capability == "benchmark_clean",
          std::string(row) + " did not consume route-specific capability");
  Require(Has(plan.steps,
              std::string("route_kind=") + idx::IndexRouteKindName(route)),
          std::string(row) + " route evidence missing route kind");
  Require(Has(plan.steps, std::string("index_family=") +
                              idx::IndexFamilyName(family)),
          std::string(row) + " route evidence missing family");
  Require(Has(plan.steps, "requires_mga_recheck=true") &&
              Has(plan.steps, "requires_security_recheck=true"),
          std::string(row) + " route evidence lost MGA/security recheck flags");
  Require(Has(plan.steps, "dispatch_to_family_access_method"),
          std::string(row) + " did not dispatch through runtime index path");
  RequireNoBareGenericIndexRuntimeBlocker(plan.steps, row);
  RequireNoRuntimeDocDependency(plan.steps, row);
  return plan;
}

opt::RuntimeOptimizedPathEvidence RuntimeConsumedEvidence(
    std::string selected_path,
    std::string result_contract_hash) {
  auto evidence = opt::MakeSelectionOnlyRuntimeEvidence(
      std::move(selected_path),
      "embedded",
      "INDEX.ROUTE_CAPABILITY.OK",
      "route_capability_revalidated");
  evidence.transaction_snapshot_class = "engine.mga.snapshot";
  evidence.result_contract_hash = std::move(result_contract_hash);
  evidence.catalog_epoch = 4;
  evidence.security_epoch = 4;
  evidence.redaction_epoch = 4;
  evidence.provider_generation = 4;
  return opt::MarkRuntimeEvidenceConsumed(
      std::move(evidence), "sb_core_index.index_optimizer_integration");
}

void RequireConsumedRuntimeEvidence(std::string_view row,
                                    const opt::RuntimeOptimizedPathEvidence& evidence) {
  const auto validation = opt::ValidateRuntimeOptimizedPathEvidence(evidence);
  Require(validation.ok &&
              validation.state ==
                  opt::RuntimeConsumptionState::kRuntimeConsumed,
          std::string(row) + " runtime evidence was not consumed");

  const opt::RouteCompletionClaim claim{
      .route_kind = evidence.route_kind,
      .benchmark_clean = true,
      .live_route = true,
      .mark_complete = true,
  };
  const auto guard = opt::EvaluateRouteCompletionClaim(claim, {evidence});
  Require(guard.can_mark_complete,
          std::string(row) +
              " could not pass route guard with consumed route evidence");
  std::vector<std::string> values = {
      evidence.selected_path,
      evidence.consumed_module,
      evidence.route_kind,
      evidence.transaction_snapshot_class,
      evidence.result_contract_hash,
      evidence.fallback_reason,
      evidence.diagnostic_code,
  };
  RequireNoBareGenericIndexRuntimeBlocker(values, row);
  RequireNoRuntimeDocDependency(values, row);
}

void RequireUnsupportedRouteDiagnostic(idx::IndexRouteKind route,
                                       idx::IndexFamily family,
                                       std::string_view context) {
  const auto& state = Route(route, family);
  Require(!state.route_complete() && state.family_physical_complete &&
              !state.route_supported,
          std::string(context) + " unsupported route was not fail-closed");
  Require(state.route_diagnostic_code ==
              "INDEX.ROUTE_CAPABILITY.UNSUPPORTED_ROUTE_FAMILY",
          std::string(context) + " diagnostic did not name unsupported route");
  Require(!state.supports_read && !state.supports_write &&
              !state.supports_mutation,
          std::string(context) + " unsupported route exposed capabilities");
  auto diagnostic = idx::MakeIndexRouteCapabilityDiagnostic(RefuseStatus(),
                                                            state);
  Require(diagnostic.diagnostic_code ==
              "INDEX.ROUTE_CAPABILITY.UNSUPPORTED_ROUTE_FAMILY",
          std::string(context) + " emitted diagnostic drifted");
  RequireDiagnosticArgument(diagnostic,
                            "route",
                            idx::IndexRouteKindName(route));
  RequireDiagnosticArgument(diagnostic, "route_supported", "false");
  RequireDiagnosticArgument(diagnostic, "route_benchmark_clean", "false");
  std::vector<std::string> values = RouteValues(state);
  AppendDiagnosticValues(diagnostic, &values);
  RequireNoBareGenericIndexRuntimeBlocker(values, context);
  RequireNoRuntimeDocDependency(values, context);
}

void ProveCorrectedRouteCapabilityModel() {
  const auto& hash_sql = Route(idx::IndexRouteKind::sql_select,
                               idx::IndexFamily::hash);
  Require(hash_sql.route_complete() && hash_sql.supports_read &&
              hash_sql.supports_equality_lookup &&
              !hash_sql.supports_ordered_range && !hash_sql.supports_write &&
              !hash_sql.supports_mutation && hash_sql.requires_exact_recheck,
          "hash SQL route did not remain equality-only candidate evidence");
  Require(Has(hash_sql.evidence, "hash_equality_lookup_supported=true") &&
              Has(hash_sql.evidence, "hash_ordered_range_supported=false") &&
              Has(hash_sql.evidence, "hash_negative_prune_supported=false") &&
              Has(hash_sql.evidence,
                  "hash_dml_write_requires_explicit_hash_route=true"),
          "hash route evidence did not expose exact route limits");

  idx::IndexOptimizerRequest hash_lookup;
  hash_lookup.index_uuid = TestObjectUuid(0x51);
  hash_lookup.family = idx::IndexFamily::hash;
  hash_lookup.route = idx::IndexRouteKind::sql_select;
  auto hash_readiness =
      ReadinessEvidence(hash_lookup.family, hash_lookup.route);
  hash_lookup.readiness_evidence = &hash_readiness;
  const auto hash_plan = idx::PlanIndexExecutorDispatch(hash_lookup);
  Require(hash_plan.ok() && hash_plan.route_benchmark_clean &&
              Has(hash_plan.steps, "hash_ordered_range_supported=false") &&
              Has(hash_plan.steps, "hash_negative_prune_supported=false"),
          "optimizer did not consume hash equality-only route limits");

  idx::IndexOptimizerRequest legacy_hash = hash_lookup;
  legacy_hash.hash_keyed_algorithm_active = false;
  const auto refused_legacy = idx::PlanIndexOptimizerPath(legacy_hash);
  Require(!refused_legacy.ok() &&
              refused_legacy.diagnostic.diagnostic_code ==
                  "INDEX.ROUTE_CAPABILITY.KEYED_HASH_REQUIRED",
          "legacy hash route did not fail closed with keyed diagnostic");

  RequireUnsupportedRouteDiagnostic(idx::IndexRouteKind::dml_update,
                                    idx::IndexFamily::hash,
                                    "hash DML update");
  RequireUnsupportedRouteDiagnostic(idx::IndexRouteKind::dml_delete,
                                    idx::IndexFamily::bloom,
                                    "bloom DML delete");
  RequireUnsupportedRouteDiagnostic(idx::IndexRouteKind::nosql_document,
                                    idx::IndexFamily::btree,
                                    "btree NoSQL document");
  RequireUnsupportedRouteDiagnostic(idx::IndexRouteKind::nosql_vector,
                                    idx::IndexFamily::document_path,
                                    "document path vector");

  const auto& btree_dml = Route(idx::IndexRouteKind::dml_update,
                                idx::IndexFamily::btree);
  Require(btree_dml.route_complete() && btree_dml.supports_write &&
              btree_dml.supports_mutation &&
              !btree_dml.supports_negative_prune,
          "B-tree DML update route did not expose route-local write support");

  const auto& document = Route(idx::IndexRouteKind::nosql_document,
                               idx::IndexFamily::document_path);
  Require(document.route_complete() && document.supports_read &&
              document.produces_candidate_set &&
              document.requires_exact_recheck,
          "document_path route was not route-specific candidate evidence");

  const auto& vector = Route(idx::IndexRouteKind::nosql_vector,
                             idx::IndexFamily::vector_hnsw);
  Require(vector.route_complete() && vector.approximate_candidate_source &&
              vector.requires_exact_rerank && vector.requires_exact_recheck,
          "HNSW route did not preserve exact rerank/recheck limits");
}

void ProveDonorAndPolicyBlockedRemainNonRuntime() {
  for (const auto family :
       {idx::IndexFamily::donor_emulated, idx::IndexFamily::policy_blocked}) {
    const auto* state = idx::FindBuiltinIndexFamilyPhysicalCapabilityState(family);
    Require(state != nullptr, "blocked family capability state missing");
    Require(!state->runtime_available && !state->benchmark_clean &&
                !state->physically_complete(),
            std::string(idx::IndexFamilyName(family)) +
                " became runtime or benchmark-clean");
    Require(state->blocker != idx::IndexFamilyPhysicalCapabilityBlocker::none,
            std::string(idx::IndexFamilyName(family)) +
                " lost exact capability blocker");

    for (const auto route : {idx::IndexRouteKind::dml_insert,
                             idx::IndexRouteKind::dml_update,
                             idx::IndexRouteKind::dml_delete,
                             idx::IndexRouteKind::sql_select,
                             idx::IndexRouteKind::bulk_build,
                             idx::IndexRouteKind::nosql_document,
                             idx::IndexRouteKind::nosql_graph,
                             idx::IndexRouteKind::nosql_vector,
                             idx::IndexRouteKind::nosql_search,
                             idx::IndexRouteKind::maintenance,
                             idx::IndexRouteKind::validate_repair}) {
      const auto& route_state = Route(route, family);
      Require(!route_state.route_complete() &&
                  !route_state.family_physical_complete &&
                  !route_state.benchmark_clean && !route_state.supports_read &&
                  !route_state.supports_write,
              std::string(idx::IndexFamilyName(family)) +
                  " exposed runtime route capability");
      RequireNoBareGenericIndexRuntimeBlocker(RouteValues(route_state),
                                              idx::IndexFamilyName(family));
    }
  }

  idx::IndexOptimizerRequest donor;
  donor.index_uuid = TestObjectUuid(0x61);
  donor.family = idx::IndexFamily::donor_emulated;
  const auto donor_plan = idx::PlanIndexOptimizerPath(donor);
  Require(!donor_plan.ok() && donor_plan.fallback_full_scan &&
              donor_plan.diagnostic.diagnostic_code ==
                  "INDEX.CAPABILITY.DONOR_EMULATED."
                  "CONTRACT_ONLY_NON_AUTHORITY_MAPPING",
          "donor_emulated optimizer route did not fail closed exactly");

  idx::IndexOptimizerRequest policy;
  policy.index_uuid = TestObjectUuid(0x62);
  policy.family = idx::IndexFamily::policy_blocked;
  const auto policy_plan = idx::PlanIndexOptimizerPath(policy);
  Require(!policy_plan.ok() && policy_plan.fallback_full_scan &&
              policy_plan.diagnostic.diagnostic_code ==
                  "INDEX.POLICY_BLOCKED.POLICY_NOT_ACCEPTED",
          "policy_blocked optimizer route did not fail closed exactly");

  std::vector<std::string> values;
  AppendDiagnosticValues(donor_plan.diagnostic, &values);
  AppendDiagnosticValues(policy_plan.diagnostic, &values);
  RequireNoBareGenericIndexRuntimeBlocker(values, "blocked family plans");
  RequireNoRuntimeDocDependency(values, "blocked family plans");
}

void ProveOrh071ConsumesRouteStateWithoutClaimingOrh242() {
  const auto& route = Route(idx::IndexRouteKind::maintenance,
                            idx::IndexFamily::btree);
  Require(route.route_complete(), "ORH-071 index maintenance route unavailable");

  auto request =
      idx::DefaultCompressionPolicyRequest(idx::CompressionFamily::kExactIndexPage);
  request.runtime_index_compression_requested = false;
  request.index_runtime_correctness_proven = route.route_complete();
  request.uncompressed_bytes = 16 * 1024;
  request.estimated_compressed_bytes = 4 * 1024;
  request.measured_feedback.present = true;
  request.measured_feedback.compress_ns_per_byte = 1.0;
  request.measured_feedback.decompress_ns_per_byte = 1.2;
  request.measured_feedback.observed_compression_ratio = 0.28;
  request.measured_feedback.cache_hit_improvement = 0.20;
  request.measured_feedback.write_amplification_change = -0.02;
  request.measured_feedback.dictionary_miss_rate = 0.0;
  request.measured_feedback.fallback_rate = 0.0;
  request.measured_feedback.sample_count = 512;
  request.measured_feedback.age_ms = 1000;

  const auto decision = idx::EvaluateCompressionPolicy(request);
  Require(decision.accepted,
          "ORH-071 measured policy did not consume corrected index route state");
  Require(!decision.runtime_index_compression_requested &&
              !decision.index_runtime_closure_claimed,
          "ORH-071 claimed compact index compression before ORH-242");
  Require(Has(decision.evidence,
              "compression_index_runtime_correctness_proven=true"),
          "ORH-071 did not record corrected index runtime proof");
  Require(!Has(decision.diagnostics,
               "SB_ORH_COMPRESSION_FAMILY_THRESHOLD.INDEX_RUNTIME_UNPROVEN"),
          "ORH-071 retained stale index runtime blocker after route proof");
  Require(Has(decision.evidence, "parser_or_donor_authority=false") &&
              Has(decision.evidence, "wal_or_finality_authority=false"),
          "ORH-071 drifted into parser/donor/finality authority");
  RequireNoBareGenericIndexRuntimeBlocker(decision.evidence, "ORH-071");
  RequireNoRuntimeDocDependency(decision.evidence, "ORH-071");
}

void ProveOrh080081ConsumeVectorRouteState() {
  const auto vector_plan = RequireDispatchPlan("ORH-080/081",
                                               idx::IndexFamily::vector_hnsw,
                                               idx::IndexRouteKind::nosql_vector);
  Require(vector_plan.rerank && vector_plan.exact_recheck,
          "ORH-080/081 vector route did not require rerank and exact recheck");

  auto profile = idx::DefaultVectorTrainingRecallLifecycleProfile(
      idx::IndexVectorAlgorithm::hnsw);
  profile.drift.p95_latency_microseconds = 4000;
  profile.drift.policy_p95_latency_microseconds = 2000;
  profile.drift.hnsw_degree_imbalance_ratio = 1.50;
  profile.drift.adaptive_tuning_expected_sufficient = true;
  profile.drift.current_ef_search = 80;
  profile.drift.tuned_ef_search = 120;
  profile.drift.max_ef_search = 256;
  const auto decision = idx::EvaluateVectorTrainingRecallLifecycle(profile);
  Require(decision.accepted &&
              decision.action ==
                  idx::VectorTrainingRecallLifecycleAction::
                      kScheduleAdaptiveTuning,
          "ORH-080/081 vector lifecycle did not use corrected route proof");
  Require(Has(decision.evidence, "exact_rerank_required=true") &&
              Has(decision.evidence,
                  "exact_rerank_final_scoring_authority=true") &&
              Has(decision.evidence, "base_row_mga_recheck_required=true") &&
              Has(decision.evidence,
                  "base_row_security_recheck_required=true"),
          "ORH-080/081 lost exact rerank or MGA/security recheck evidence");
  RequireNoBareGenericIndexRuntimeBlocker(decision.evidence, "ORH-080/081");
  RequireNoRuntimeDocDependency(decision.evidence, "ORH-080/081");
}

void ProveOrh120122ConsumeCorrectedRuntimeEvidence() {
  (void)RequireDispatchPlan("ORH-120",
                            idx::IndexFamily::document_path,
                            idx::IndexRouteKind::nosql_document);
  RequireConsumedRuntimeEvidence(
      "ORH-120",
      RuntimeConsumedEvidence("orh120.document_path.corrected_route",
                              "result_contract:orh120:document_path"));

  (void)RequireDispatchPlan("ORH-122",
                            idx::IndexFamily::covering,
                            idx::IndexRouteKind::sql_select);
  RequireConsumedRuntimeEvidence(
      "ORH-122",
      RuntimeConsumedEvidence("orh122.covering.route_churn_revalidated",
                              "result_contract:orh122:covering"));
}

agents::ResourceGovernanceQuotaVector Quotas(std::int64_t value) {
  agents::ResourceGovernanceQuotaVector quotas;
  quotas.memory_bytes = value;
  quotas.device_memory_bytes = value;
  quotas.pinned_memory_bytes = value;
  quotas.io_bytes = value;
  quotas.io_ops = value;
  quotas.worker_threads = value;
  quotas.backlog_items = value;
  quotas.candidate_rows = value;
  quotas.cache_entries = value;
  quotas.batch_rows = value;
  quotas.fragments = value;
  quotas.lanes = value;
  quotas.time_budget_microseconds = value;
  return quotas;
}

agents::ResourceGovernanceAdmissionRequest ResourceRequest(
    std::string operation_id,
    agents::ResourceGovernanceFamily family) {
  agents::ResourceGovernanceAdmissionRequest request;
  request.operation_id = operation_id;
  request.expected_family = family;
  request.descriptor.descriptor_id = operation_id + ".runtime_policy";
  request.descriptor.family = family;
  request.descriptor.source =
      agents::ResourceGovernanceDescriptorSource::kRuntimePolicy;
  request.descriptor.source_path_or_label = "runtime_policy:orh004";
  request.descriptor.descriptor_generation = 4;
  request.descriptor.expected_generation = 4;
  request.descriptor.limits = Quotas(1024);
  request.descriptor.over_limit_action = agents::ResourceGovernanceAction::kFailClosed;
  request.descriptor.benchmark_clean = true;
  request.descriptor.runtime_dependency_present = true;
  request.requested = Quotas(1);
  return request;
}

void ProveOrh124UsesCorrectedIndexRuntimeState() {
  const auto& document = Route(idx::IndexRouteKind::nosql_document,
                               idx::IndexFamily::document_path);
  agents::OptimizedPathResourceGovernanceRequest request;
  request.operation_id = "orh124.document_path.corrected_route";
  request.surface = agents::OptimizedPathResourceSurface::nosql_provider;
  request.resource_admission =
      ResourceRequest(request.operation_id,
                      agents::ResourceGovernanceFamily::kOptimizedNoSqlProvider);
  request.workload_quota_required = false;
  request.foreground_protection_required = true;
  request.foreground_capacity_reserved = true;
  request.index_runtime_dependent = true;
  request.index_runtime_correctness_proven = document.route_complete();

  const auto governed = agents::GovernOptimizedPathResources(request);
  Require(governed.admitted && !governed.fail_closed,
          "ORH-124 resource governance did not admit corrected route proof");
  Require(Has(governed.evidence,
              "optimized_resource.index_runtime_correctness_proven=true"),
          "ORH-124 did not consume corrected route proof");
  Require(!Contains(governed.evidence, "optimized_resource.index_runtime_blocker="),
          "ORH-124 retained an index runtime blocker after route proof");
  Require(Has(governed.evidence,
              "optimized_resource.mga_authority=engine_transaction_inventory") &&
              Has(governed.evidence,
                  "optimized_resource.parser_or_donor_authority=false"),
          "ORH-124 resource governance drifted from MGA authority");
  RequireNoBareGenericIndexRuntimeBlocker(governed.evidence, "ORH-124");
  RequireNoRuntimeDocDependency(governed.evidence, "ORH-124");
}

void ProveOrh221StaysNonIndexAndHasNoStaleBlocker() {
  agents::OptimizedPathResourceGovernanceRequest request;
  request.operation_id = "orh221.parallel_pipeline.non_index";
  request.surface = agents::OptimizedPathResourceSurface::background_job;
  request.resource_admission =
      ResourceRequest(request.operation_id,
                      agents::ResourceGovernanceFamily::kParallelPhysicalPipeline);
  request.workload_quota_required = false;
  request.foreground_protection_required = true;
  request.foreground_capacity_reserved = true;
  request.index_runtime_dependent = false;
  request.index_runtime_correctness_proven = false;

  const auto governed = agents::GovernOptimizedPathResources(request);
  Require(governed.admitted && !governed.fail_closed,
          "ORH-221 non-index parallel pipeline governance failed");
  Require(Has(governed.evidence,
              "optimized_resource.index_runtime_dependent=false"),
          "ORH-221 accidentally became index-dependent");
  Require(!Contains(governed.evidence, "index_runtime_blocker="),
          "ORH-221 retained stale index runtime blocker");
  RequireNoBareGenericIndexRuntimeBlocker(governed.evidence, "ORH-221");
  RequireNoRuntimeDocDependency(governed.evidence, "ORH-221");

  RequireConsumedRuntimeEvidence(
      "ORH-221",
      RuntimeConsumedEvidence("orh221.parallel_pipeline.non_index",
                              "result_contract:orh221:parallel_pipeline"));
}

}  // namespace

int main() {
  ProveCorrectedRouteCapabilityModel();
  ProveDonorAndPolicyBlockedRemainNonRuntime();
  ProveOrh071ConsumesRouteStateWithoutClaimingOrh242();
  ProveOrh080081ConsumeVectorRouteState();
  ProveOrh120122ConsumeCorrectedRuntimeEvidence();
  ProveOrh124UsesCorrectedIndexRuntimeState();
  ProveOrh221StaysNonIndexAndHasNoStaleBlocker();
  std::cout << "optimizer_runtime_hot_path_orh_004_gate=passed\n";
  return EXIT_SUCCESS;
}
