// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// CEIC-027 focused validation for plugin/UDR native memory sandboxing.
#include "plugin_native_memory_sandbox.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace memory = scratchbird::core::memory;
using scratchbird::core::platform::StatusCode;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool EvidenceHas(const std::vector<std::string>& evidence,
                 std::string_view token) {
  for (const auto& row : evidence) {
    if (row.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool RowsHave(
    const std::vector<memory::PluginNativeMemorySandboxSupportBundleRow>& rows,
    std::string_view key,
    std::string_view value) {
  for (const auto& row : rows) {
    if (row.key.find(key) != std::string::npos &&
        row.value.find(value) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool MetricsHave(
    const std::vector<memory::PluginNativeMemorySandboxMetricRow>& rows,
    std::string_view metric_name) {
  for (const auto& row : rows) {
    if (row.metric_name.find(metric_name) != std::string::npos) {
      return true;
    }
  }
  return false;
}

memory::HierarchicalMemoryBudgetProvenance Provenance(
    std::string label = "ceic-027-runtime-policy") {
  memory::HierarchicalMemoryBudgetProvenance provenance;
  provenance.source =
      memory::HierarchicalMemoryBudgetProvenanceSource::runtime_policy;
  provenance.source_label = std::move(label);
  provenance.engine_mga_authoritative = true;
  provenance.memory_evidence_only = true;
  return provenance;
}

memory::HierarchicalMemoryScopeRef Scope(
    memory::HierarchicalMemoryScopeKind kind,
    std::string scope_id) {
  return {kind, std::move(scope_id)};
}

std::vector<memory::HierarchicalMemoryScopeRef> ScopeChain(
    std::string suffix) {
  return {Scope(memory::HierarchicalMemoryScopeKind::process,
                "ceic-027-process"),
          Scope(memory::HierarchicalMemoryScopeKind::database,
                "ceic-027-database"),
          Scope(memory::HierarchicalMemoryScopeKind::session,
                "ceic-027-session-" + suffix),
          Scope(memory::HierarchicalMemoryScopeKind::statement,
                "ceic-027-statement-" + suffix),
          Scope(memory::HierarchicalMemoryScopeKind::plugin,
                "ceic-027-plugin-scope-" + suffix)};
}

memory::PluginNativeMemorySandboxRequest Request(
    memory::ForeignMemorySource source,
    std::string suffix,
    memory::MemoryPressureState pressure =
        memory::MemoryPressureState::normal) {
  memory::PluginNativeMemorySandboxRequest request;
  request.source = source;
  request.scope_chain = ScopeChain(suffix);
  request.owner_id = "ceic-027-owner-" + suffix;
  request.sandbox_id = "ceic-027-sandbox-" + suffix;
  request.plugin_id = "ceic-027-plugin-" + suffix;
  request.plugin_allocator_abi = "scratchbird.plugin_allocator.v1";
  request.plugin_memory_context_id = "ceic-027-plugin-context-" + suffix;
  request.udr_entrypoint = "ceic_027_udr_entrypoint";
  request.library_path = "/non-live/ceic_027_plugin.so";
  request.operation_id = "ceic-027-operation-" + suffix;
  request.native_callsite =
      std::string("ceic_027.native.") +
      memory::ForeignMemorySourceName(source);
  request.estimated_bytes = 512;
  request.observed_bytes = 64;
  request.conservative_estimated_bytes = 1024;
  request.invocation_budget_bytes = 2048;
  request.result_buffer_bytes = 128;
  request.confidence = memory::ForeignMemoryConfidence::estimated;
  request.expected_release_event =
      memory::ForeignMemoryReleaseEvent::explicit_release;
  request.over_limit_action = memory::ForeignMemoryOverLimitAction::deny;
  request.pressure_state = pressure;
  request.now_ms = 1000;
  request.deadline_ms = 5000;
  request.provider_available = true;
  request.live_provider_proof = false;
  request.live_route_claim = false;
  request.allow_conservative_estimate = false;
  request.production_raw_external_allocation_gate = true;
  request.raw_external_allocation = false;
  request.raw_plugin_allocation_explicitly_allowed = false;
  request.untracked_native_allocation = false;
  request.plugin_udr_sandboxed = true;
  request.result_buffer_owned_by_engine = true;
  request.plugin_cancellation_on_pressure = true;
  request.plugin_unload_cleanup_supported = true;
  request.support_bundle_view_enabled = true;
  request.require_plugin_udr_class_proof = true;
  request.require_release_evidence = true;
  request.provenance = Provenance("ceic-027-request-" + suffix);
  request.authority.engine_mga_authoritative = true;
  request.authority.memory_evidence_only = true;
  request.authority.security_or_policy_checked = true;
  request.authority.evidence_fresh = true;
  request.authority.provider_available = true;
  request.authority.authority_generation = "ceic-027-runtime";
  request.authority.evidence_label = "ceic_027_plugin_native_sandbox_gate";
  request.evidence = {
      "plugin_allocator_abi=required",
      "plugin_memory_context=required",
      "invocation_budget=required",
      "result_buffer_owner=engine",
      "plugin_support_bundle_view=enabled",
      "production_raw_plugin_allocation_gate=enabled"};
  if (source == memory::ForeignMemorySource::llvm) {
    request.linkage_mode = memory::ForeignMemoryLinkageMode::dynamic_library;
    request.evidence.push_back("llvm_linkage=dynamic_library");
  }
  return request;
}

struct Harness {
  memory::HierarchicalMemoryBudgetLedger budget_ledger;
  memory::ForeignMemoryReservationLedger foreign_ledger;
  memory::MemoryClassPolicyLeaseManager class_manager;
  memory::PluginNativeMemorySandboxManager sandbox;

  Harness()
      : class_manager(&budget_ledger),
        sandbox(&budget_ledger, &foreign_ledger, &class_manager) {}
};

void RequireNoAuthorityEvidence(const std::vector<std::string>& evidence) {
  Require(EvidenceHas(evidence, "CEIC-027_PLUGIN_NATIVE_MEMORY_SANDBOX"),
          "CEIC-027 anchor evidence missing");
  Require(EvidenceHas(
              evidence,
              "memory_evidence_only_not_transaction_finality_visibility_authorization_security_recovery_parser_reference_wal_benchmark_optimizer_plan_index_finality_cluster_or_agent_action_authority"),
          "CEIC-027 authority scope evidence missing");
  Require(EvidenceHas(evidence,
                      "no_authority.transaction_finality=true") &&
              EvidenceHas(evidence, "no_authority.visibility=true") &&
              EvidenceHas(evidence, "no_authority.authorization_security=true") &&
              EvidenceHas(evidence, "no_authority.recovery=true") &&
              EvidenceHas(evidence, "no_authority.parser_reference_wal=true") &&
              EvidenceHas(evidence,
                          "no_authority.benchmark_optimizer_index_agent=true"),
          "CEIC-027 no-authority evidence missing");
}

void ModeledSourceCoverageAndIntegration() {
  Harness harness;
  Require(memory::PluginNativeMemorySandboxModeledSources().size() == 11,
          "CEIC-027 modeled source count mismatch");

  int index = 0;
  for (auto source : memory::PluginNativeMemorySandboxModeledSources()) {
    auto request =
        Request(source,
                std::string(memory::ForeignMemorySourceName(source)) + "-" +
                    std::to_string(index++));
    if (source == memory::ForeignMemorySource::gpu_optional) {
      request.evidence.push_back("gpu_provider_fixture_available_for_model_only=true");
    }
    auto acquired = harness.sandbox.Acquire(request);
    Require(acquired.ok(), "CEIC-027 modeled source acquisition failed");
    Require(acquired.reservation->token().class_lease.valid(),
            "CEIC-027 CEIC-026 plugin_udr class lease missing");
    Require(acquired.reservation->token().foreign_reservation.valid(),
            "CEIC-027 CEIC-016 foreign reservation handle missing");
    Require(EvidenceHas(acquired.evidence,
                        "ceic_026_plugin_udr_lease=true") &&
                EvidenceHas(acquired.evidence, "ceic_016_handle=true") &&
                EvidenceHas(acquired.evidence, "ceic_011_reserved=true"),
            "CEIC-027 CEIC-011/016/026 integration evidence missing");
    Require(EvidenceHas(acquired.evidence,
                        "plugin_allocator_abi=scratchbird.plugin_allocator.v1") &&
                EvidenceHas(acquired.evidence,
                            "plugin_memory_context_id=ceic-027-plugin-context"),
            "CEIC-027 plugin allocator ABI/context evidence missing");
    Require(EvidenceHas(acquired.evidence,
                        "invocation_budget_bytes=2048") &&
                EvidenceHas(acquired.evidence, "result_buffer_owner=engine"),
            "CEIC-027 invocation/result buffer evidence missing");
    RequireNoAuthorityEvidence(acquired.evidence);

    auto budget_snapshot = harness.budget_ledger.Snapshot();
    Require(budget_snapshot.current_bytes >= 1024,
            "CEIC-027 CEIC-011 bytes were not reserved through lease and foreign handle");
    Require(harness.foreign_ledger.Snapshot().active_reservation_count == 1,
            "CEIC-027 CEIC-016 active handle missing");
    Require(harness.class_manager.Snapshot().active_lease_count == 1,
            "CEIC-027 CEIC-026 plugin lease missing");
    Require(acquired.reservation
                ->Release({"ceic_027_explicit_release_evidence=true"})
                .ok(),
            "CEIC-027 modeled source release failed");
  }

  Require(harness.sandbox.Snapshot().current_estimated_bytes == 0,
          "CEIC-027 sandbox estimated bytes leaked");
  Require(harness.foreign_ledger.Snapshot().current_estimated_bytes == 0,
          "CEIC-027 CEIC-016 foreign bytes leaked");
  Require(harness.class_manager.Snapshot().active_lease_count == 0,
          "CEIC-027 CEIC-026 plugin leases leaked");
  Require(harness.budget_ledger.Snapshot().current_bytes == 0,
          "CEIC-027 CEIC-011 bytes leaked");
}

void ConservativeFallbackAndPressureBehavior() {
  Harness harness;
  auto conservative =
      Request(memory::ForeignMemorySource::icu, "icu-conservative");
  conservative.provider_available = false;
  conservative.authority.provider_available = false;
  conservative.allow_conservative_estimate = true;
  conservative.conservative_estimated_bytes = 4096;
  conservative.invocation_budget_bytes = 8192;
  auto fallback = harness.sandbox.Acquire(conservative);
  Require(fallback.ok(), "CEIC-027 conservative absent-provider fallback failed");
  Require(EvidenceHas(fallback.evidence, "provider_available=false") &&
              EvidenceHas(fallback.evidence, "conservative_estimate=true"),
          "CEIC-027 conservative fallback evidence missing");
  Require(fallback.reservation->Snapshot().confidence ==
              memory::ForeignMemoryConfidence::conservative,
          "CEIC-027 conservative fallback confidence missing");
  Require(fallback.reservation
              ->Release({"ceic_027_conservative_release_evidence=true"})
              .ok(),
          "CEIC-027 conservative fallback release failed");

  auto high_pressure =
      Request(memory::ForeignMemorySource::plugin_udr,
              "pressure",
              memory::MemoryPressureState::high_pressure);
  auto pressured = harness.sandbox.Acquire(high_pressure);
  Require(pressured.ok(), "CEIC-027 high-pressure plugin sandbox failed");
  Require(pressured.reservation->Snapshot().pressure_action ==
              memory::MemoryClassPressureAction::sandbox_plugin_udr,
          "CEIC-027 plugin pressure action did not sandbox plugin/UDR");
  Require(EvidenceHas(pressured.evidence,
                      "plugin_cancellation_on_pressure=true"),
          "CEIC-027 pressure cancellation evidence missing");
  Require(pressured.reservation
              ->Release({"ceic_027_pressure_release_evidence=true"})
              .ok(),
          "CEIC-027 pressure release failed");

  auto no_cancel =
      Request(memory::ForeignMemorySource::plugin_udr,
              "pressure-no-cancel",
              memory::MemoryPressureState::high_pressure);
  no_cancel.plugin_cancellation_on_pressure = false;
  auto refused = harness.sandbox.Acquire(no_cancel);
  Require(!refused.ok() && refused.fail_closed,
          "CEIC-027 pressure without plugin cancellation was accepted");
}

void ReleaseOwnerAndUnloadCleanup() {
  Harness harness;
  auto missing_release =
      harness.sandbox.Acquire(Request(memory::ForeignMemorySource::regex,
                                      "missing-release"));
  Require(missing_release.ok(), "CEIC-027 missing-release setup failed");
  auto refused = missing_release.reservation->Release({});
  Require(!refused.ok() && refused.fail_closed,
          "CEIC-027 missing release evidence was accepted");
  Require(missing_release.reservation->active(),
          "CEIC-027 missing release evidence incorrectly released handle");
  Require(missing_release.reservation
              ->Release({"ceic_027_release_evidence_present=true"})
              .ok(),
          "CEIC-027 release after missing evidence failed");
  Require(harness.sandbox.Snapshot().missing_release_evidence_refusal_count == 1,
          "CEIC-027 missing release evidence counter mismatch");

  auto owner_a =
      Request(memory::ForeignMemorySource::json, "owner-cleanup-a");
  owner_a.owner_id = "ceic-027-owner-cleanup";
  auto owner_b =
      Request(memory::ForeignMemorySource::compression, "owner-cleanup-b");
  owner_b.owner_id = "ceic-027-owner-cleanup";
  auto owner_acquired_a = harness.sandbox.Acquire(owner_a);
  auto owner_acquired_b = harness.sandbox.Acquire(owner_b);
  Require(owner_acquired_a.ok() && owner_acquired_b.ok(),
          "CEIC-027 owner cleanup setup failed");
  auto owner_cleanup = harness.sandbox.CleanupOwner("ceic-027-owner-cleanup");
  Require(owner_cleanup.ok() &&
              owner_cleanup.cleaned_reservation_count == 2 &&
              owner_cleanup.cleaned_estimated_bytes == 1024,
          "CEIC-027 owner cleanup failed");

  auto unload_a =
      Request(memory::ForeignMemorySource::mmap, "unload-a");
  unload_a.plugin_id = "ceic-027-plugin-unload";
  auto unload_b =
      Request(memory::ForeignMemorySource::thread_stack, "unload-b");
  unload_b.plugin_id = "ceic-027-plugin-unload";
  auto unload_acquired_a = harness.sandbox.Acquire(unload_a);
  auto unload_acquired_b = harness.sandbox.Acquire(unload_b);
  Require(unload_acquired_a.ok() && unload_acquired_b.ok(),
          "CEIC-027 plugin unload setup failed");
  auto unload = harness.sandbox.CleanupPluginUnload("ceic-027-plugin-unload");
  Require(unload.ok() && unload.cleaned_reservation_count == 2,
          "CEIC-027 plugin unload cleanup failed");
  Require(harness.sandbox.Snapshot().plugin_unload_cleanup_count == 2,
          "CEIC-027 plugin unload cleanup counter mismatch");
}

void RefusalMatrixAndProductionRawPluginGate() {
  Harness harness;

  auto unsafe =
      Request(memory::ForeignMemorySource::crypto, "unsafe-provenance");
  unsafe.provenance.parser_authority = true;
  Require(!harness.sandbox.Acquire(unsafe).ok(),
          "CEIC-027 unsafe provenance was accepted");

  auto untracked =
      Request(memory::ForeignMemorySource::driver_native, "untracked");
  untracked.untracked_native_allocation = true;
  Require(!harness.sandbox.Acquire(untracked).ok(),
          "CEIC-027 untracked native allocation was accepted");

  auto cluster =
      Request(memory::ForeignMemorySource::plugin_udr, "cluster-route");
  cluster.route_kind = memory::MemoryBudgetLeaseRouteKind::cluster;
  auto cluster_refused = harness.sandbox.Acquire(cluster);
  Require(!cluster_refused.ok() && cluster_refused.fail_closed,
          "CEIC-027 cluster route was accepted");
  Require(EvidenceHas(cluster_refused.evidence,
                      "cluster_boundary=external_provider_only_fail_closed"),
          "CEIC-027 cluster refusal evidence missing");

  auto missing_identity =
      Request(memory::ForeignMemorySource::plugin_udr, "missing-context");
  missing_identity.plugin_memory_context_id.clear();
  Require(!harness.sandbox.Acquire(missing_identity).ok(),
          "CEIC-027 missing plugin memory context was accepted");

  auto budget =
      Request(memory::ForeignMemorySource::plugin_udr, "budget");
  budget.invocation_budget_bytes = 128;
  Require(!harness.sandbox.Acquire(budget).ok(),
          "CEIC-027 under-budget invocation was accepted");

  auto result_owner =
      Request(memory::ForeignMemorySource::plugin_udr, "result-owner");
  result_owner.result_buffer_owned_by_engine = false;
  Require(!harness.sandbox.Acquire(result_owner).ok(),
          "CEIC-027 plugin-owned result buffer was accepted");

  auto support_view =
      Request(memory::ForeignMemorySource::plugin_udr, "support-view");
  support_view.support_bundle_view_enabled = false;
  Require(!harness.sandbox.Acquire(support_view).ok(),
          "CEIC-027 missing plugin support-bundle view was accepted");

  auto raw_plugin =
      Request(memory::ForeignMemorySource::plugin_udr, "raw-plugin");
  raw_plugin.raw_external_allocation = true;
  auto raw_refused = harness.sandbox.Acquire(raw_plugin);
  Require(!raw_refused.ok() && raw_refused.fail_closed,
          "CEIC-027 raw plugin allocation without allowance was accepted");
  Require(EvidenceHas(raw_refused.evidence,
                      "raw_plugin_allocation_explicitly_allowed=false"),
          "CEIC-027 raw plugin refusal evidence missing");

  auto allowed_raw =
      Request(memory::ForeignMemorySource::plugin_udr, "raw-plugin-allowed");
  allowed_raw.raw_external_allocation = true;
  allowed_raw.raw_plugin_allocation_explicitly_allowed = true;
  allowed_raw.evidence.push_back("raw_plugin_allocation_allowance=operator_policy");
  auto allowed = harness.sandbox.Acquire(allowed_raw);
  Require(allowed.ok(), "CEIC-027 explicitly allowed raw plugin allocation failed");
  Require(EvidenceHas(allowed.evidence,
                      "raw_plugin_allocation_explicitly_allowed=true"),
          "CEIC-027 explicit raw plugin allowance evidence missing");
  Require(allowed.reservation
              ->Release({"ceic_027_allowed_raw_release_evidence=true"})
              .ok(),
          "CEIC-027 allowed raw plugin release failed");

  auto gpu_absent =
      Request(memory::ForeignMemorySource::gpu_optional, "gpu-absent");
  gpu_absent.provider_available = false;
  gpu_absent.authority.provider_available = false;
  gpu_absent.allow_conservative_estimate = true;
  Require(!harness.sandbox.Acquire(gpu_absent).ok(),
          "CEIC-027 absent optional GPU provider was not fail-closed");

  memory::HierarchicalMemoryBudgetLedger stale_budget;
  memory::ForeignMemoryReservationLedger stale_foreign;
  memory::MemoryClassPolicyLeaseManager stale_class(&stale_budget);
  auto stale_policy =
      memory::DefaultMemoryClassPolicy(memory::MemoryClassKind::plugin_udr);
  stale_policy.plugin_udr_class = false;
  stale_policy.provenance = Provenance("ceic-027-stale-class-policy");
  Require(stale_class.SetClassPolicy(stale_policy).ok(),
          "CEIC-027 stale class policy setup failed");
  memory::PluginNativeMemorySandboxManager stale_sandbox(
      &stale_budget, &stale_foreign, &stale_class);
  Require(!stale_sandbox
               .Acquire(Request(memory::ForeignMemorySource::plugin_udr,
                                "stale-class"))
               .ok(),
          "CEIC-027 stale plugin_udr class proof was accepted");

  auto missing_class =
      Request(memory::ForeignMemorySource::plugin_udr, "missing-class-proof");
  missing_class.require_plugin_udr_class_proof = false;
  Require(!harness.sandbox.Acquire(missing_class).ok(),
          "CEIC-027 missing plugin_udr class proof was accepted");

  auto snapshot = harness.sandbox.Snapshot();
  Require(snapshot.unsafe_provenance_refusal_count >= 1 &&
              snapshot.untracked_native_allocation_refusal_count >= 1 &&
              snapshot.cluster_refusal_count >= 1 &&
              snapshot.raw_external_allocation_refusal_count >= 1 &&
              snapshot.raw_plugin_allocation_allowed_count == 1,
          "CEIC-027 refusal counters mismatch");
}

void SupportBundleRowsAndNoAuthority() {
  Harness harness;
  auto acquired = harness.sandbox.Acquire(
      Request(memory::ForeignMemorySource::plugin_udr, "support"));
  Require(acquired.ok(), "CEIC-027 support setup failed");
  Require(MetricsHave(acquired.metrics,
                      "sb_plugin_native_memory_sandbox_decision_total"),
          "CEIC-027 decision metric missing");
  Require(RowsHave(acquired.support_bundle_rows,
                   "plugin_native_memory_sandbox.authority_scope",
                   "memory_evidence_only"),
          "CEIC-027 support authority row missing");
  Require(RowsHave(acquired.support_bundle_rows,
                   "plugin_native_memory_sandbox.integrated_support_bundle_closure",
                   "not_claimed_ceic_091_pending"),
          "CEIC-027 non-overclaim support row missing");
  RequireNoAuthorityEvidence(acquired.evidence);
  Require(acquired.reservation
              ->Release({"ceic_027_support_release_evidence=true"})
              .ok(),
          "CEIC-027 support release failed");

  auto snapshot = harness.sandbox.Snapshot();
  Require(MetricsHave(snapshot.metrics,
                      "sb_plugin_native_memory_sandbox_active_bytes") &&
              RowsHave(snapshot.support_bundle_rows,
                       "plugin_native_memory_sandbox.plugin_support_bundle_view",
                       "allocator_abi_memory_context_invocation_budget_result_buffer_pressure_unload"),
          "CEIC-027 snapshot metric/support-bundle rows missing");
  Require(snapshot.active_reservation_count == 0 &&
              harness.budget_ledger.Snapshot().current_bytes == 0,
          "CEIC-027 support test leaked reservations");
}

}  // namespace

int main() {
  ModeledSourceCoverageAndIntegration();
  ConservativeFallbackAndPressureBehavior();
  ReleaseOwnerAndUnloadCleanup();
  RefusalMatrixAndProductionRawPluginGate();
  SupportBundleRowsAndNoAuthority();
  std::cout << "ceic_027_plugin_native_memory_sandbox_gate=pass\n";
  return EXIT_SUCCESS;
}
