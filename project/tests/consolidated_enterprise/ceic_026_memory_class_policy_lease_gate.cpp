// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// CEIC-026 focused validation for memory classes policies and budget leases.
#include "memory_class_policy_lease.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace memory = scratchbird::core::memory;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

memory::HierarchicalMemoryBudgetProvenance Provenance(
    std::string label = "ceic-026-runtime-policy") {
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
    std::string id) {
  return {kind, std::move(id)};
}

std::vector<memory::HierarchicalMemoryScopeRef> ChainForClass(
    memory::MemoryClassKind kind,
    std::string owner) {
  std::vector<memory::HierarchicalMemoryScopeRef> chain = {
      Scope(memory::HierarchicalMemoryScopeKind::process, "process"),
      Scope(memory::HierarchicalMemoryScopeKind::database, "database")};
  switch (kind) {
    case memory::MemoryClassKind::clean_page_cache:
    case memory::MemoryClassKind::dirty_page_cache:
      chain.push_back(
          Scope(memory::HierarchicalMemoryScopeKind::page_cache, owner));
      break;
    case memory::MemoryClassKind::background_maintenance:
      chain.push_back(
          Scope(memory::HierarchicalMemoryScopeKind::background, owner));
      break;
    case memory::MemoryClassKind::plugin_udr:
      chain.push_back(Scope(memory::HierarchicalMemoryScopeKind::plugin, owner));
      break;
    default:
      chain.push_back(Scope(memory::HierarchicalMemoryScopeKind::tenant,
                            "tenant"));
      chain.push_back(
          Scope(memory::HierarchicalMemoryScopeKind::session, owner));
      chain.push_back(
          Scope(memory::HierarchicalMemoryScopeKind::statement, owner + "-stmt"));
      chain.push_back(
          Scope(memory::HierarchicalMemoryScopeKind::query, owner + "-query"));
      break;
  }
  return chain;
}

memory::MemoryBudgetLeaseRequest Request(
    memory::MemoryClassKind kind,
    memory::u64 bytes,
    std::string owner,
    memory::MemoryPressureState pressure = memory::MemoryPressureState::normal,
    memory::u64 now_ms = 1000,
    memory::u64 deadline_ms = 5000) {
  memory::MemoryBudgetLeaseRequest request;
  request.scope_chain = ChainForClass(kind, owner);
  request.class_kind = kind;
  request.owner_id = std::move(owner);
  request.requested_bytes = bytes;
  request.now_ms = now_ms;
  request.deadline_ms = deadline_ms;
  request.pressure_state = pressure;
  request.priority = 5;
  request.weight = 5;
  request.provenance = Provenance();
  return request;
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

bool RowsHave(const std::vector<memory::MemoryBudgetLeaseSupportBundleRow>& rows,
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

bool MetricsHave(const std::vector<memory::MemoryBudgetLeaseMetricRow>& rows,
                 std::string_view name) {
  for (const auto& row : rows) {
    if (row.metric_name.find(name) != std::string::npos) {
      return true;
    }
  }
  return false;
}

const memory::MemoryClassPolicySnapshot* FindClass(
    const memory::MemoryBudgetLeaseSnapshot& snapshot,
    memory::MemoryClassKind kind) {
  for (const auto& cls : snapshot.classes) {
    if (cls.kind == kind) {
      return &cls;
    }
  }
  return nullptr;
}

void RequireAuthorityEvidence(
    const std::vector<std::string>& evidence,
    std::string_view label) {
  (void)label;
  Require(EvidenceHas(evidence, "CEIC-026_MEMORY_CLASS_POLICY_LEASES"),
          "CEIC-026 anchor evidence missing");
  Require(EvidenceHas(evidence,
                      "not_transaction_finality_visibility_authorization_security_recovery_parser_reference_wal_benchmark_optimizer_plan_index_finality_cluster_or_agent_action_authority"),
          "CEIC-026 authority boundary evidence missing");
  Require(EvidenceHas(evidence,
                      "memory_class_policy_lease.no_authority.transaction_finality=true") ||
              EvidenceHas(evidence,
                          "memory_class_policy_lease.recovery.not_transaction_recovery_authority=true") ||
              EvidenceHas(evidence,
                          "memory_class_policy_lease.cleanup.no_transaction_finality_or_recovery_authority=true") ||
              EvidenceHas(evidence,
                          "memory_class_policy_lease.renew.no_authority.transaction_visibility_recovery=true"),
          "CEIC-026 no-authority evidence missing");
}

void DefaultPolicyMatrixCoversRequiredClasses() {
  using Kind = memory::MemoryClassKind;
  using Action = memory::MemoryClassPressureAction;
  const auto critical = memory::DefaultMemoryClassPolicy(Kind::critical_engine);
  Require(critical.critical_engine_class &&
              critical.high_pressure_action == Action::protect_critical,
          "CEIC-026 critical engine policy did not protect critical memory");

  const auto query = memory::DefaultMemoryClassPolicy(Kind::query_scratch);
  Require(query.category == memory::MemoryCategory::executor_query_reserved &&
              query.high_pressure_action == Action::prefer_spill,
          "CEIC-026 query scratch policy did not prefer spill");

  Require(memory::DefaultMemoryClassPolicy(Kind::clean_page_cache)
              .high_pressure_action == Action::shrink_clean_page_cache,
          "CEIC-026 clean page-cache policy did not shrink clean pages");
  Require(memory::DefaultMemoryClassPolicy(Kind::dirty_page_cache)
              .high_pressure_action == Action::flush_dirty_page_cache,
          "CEIC-026 dirty page-cache policy did not flush dirty pages");

  const auto protected_policy =
      memory::DefaultMemoryClassPolicy(Kind::protected_material);
  Require(protected_policy.requires_protected_material_route &&
              protected_policy.requires_zero_on_release &&
              protected_policy.high_pressure_action ==
                  Action::refuse_protected_material,
          "CEIC-026 protected material policy did not require protected routing");

  Require(memory::DefaultMemoryClassPolicy(Kind::result_driver_buffer)
              .emergency_pressure_action == Action::cancel,
          "CEIC-026 result/driver buffer policy did not cancel under emergency");
  Require(memory::DefaultMemoryClassPolicy(Kind::background_maintenance)
              .high_pressure_action == Action::suspend_background,
          "CEIC-026 background maintenance policy did not suspend");
  Require(memory::DefaultMemoryClassPolicy(Kind::plugin_udr).plugin_udr_class,
          "CEIC-026 plugin/UDR policy flag missing");
  Require(memory::DefaultMemoryClassPolicy(Kind::parser_handoff)
              .parser_handoff_class,
          "CEIC-026 parser handoff policy flag missing");
  Require(memory::DefaultMemoryClassPolicy(Kind::diagnostics_support)
              .diagnostics_support_class,
          "CEIC-026 diagnostics/support policy flag missing");
}

void ClassSpecificPressureActionsProduceLeaseEvidence() {
  memory::HierarchicalMemoryBudgetLedger ledger;
  memory::MemoryClassPolicyLeaseManager manager(&ledger);

  struct Case {
    memory::MemoryClassKind kind;
    memory::MemoryPressureState pressure;
    memory::MemoryClassPressureAction action;
  };
  const Case cases[] = {
      {memory::MemoryClassKind::critical_engine,
       memory::MemoryPressureState::high_pressure,
       memory::MemoryClassPressureAction::protect_critical},
      {memory::MemoryClassKind::query_scratch,
       memory::MemoryPressureState::high_pressure,
       memory::MemoryClassPressureAction::prefer_spill},
      {memory::MemoryClassKind::clean_page_cache,
       memory::MemoryPressureState::high_pressure,
       memory::MemoryClassPressureAction::shrink_clean_page_cache},
      {memory::MemoryClassKind::dirty_page_cache,
       memory::MemoryPressureState::high_pressure,
       memory::MemoryClassPressureAction::flush_dirty_page_cache},
      {memory::MemoryClassKind::result_driver_buffer,
       memory::MemoryPressureState::emergency_pressure,
       memory::MemoryClassPressureAction::cancel},
      {memory::MemoryClassKind::background_maintenance,
       memory::MemoryPressureState::high_pressure,
       memory::MemoryClassPressureAction::suspend_background},
      {memory::MemoryClassKind::parser_handoff,
       memory::MemoryPressureState::high_pressure,
       memory::MemoryClassPressureAction::throttle_parser_handoff},
      {memory::MemoryClassKind::diagnostics_support,
       memory::MemoryPressureState::high_pressure,
       memory::MemoryClassPressureAction::degrade_diagnostics},
  };

  std::vector<memory::MemoryBudgetLeaseToken> leases;
  for (const auto& test_case : cases) {
    auto request = Request(test_case.kind,
                           16,
                           std::string(memory::MemoryClassKindName(test_case.kind)),
                           test_case.pressure);
    auto decision = manager.AcquireLease(request);
    Require(decision.ok(), "CEIC-026 class pressure lease was refused");
    Require(decision.pressure_action == test_case.action,
            "CEIC-026 class-specific pressure action mismatch");
    RequireAuthorityEvidence(decision.evidence,
                             "CEIC-026 class pressure decision");
    Require(MetricsHave(decision.metrics,
                        "sb_memory_class_pressure_action_total"),
            "CEIC-026 class pressure metric row missing");
    Require(RowsHave(decision.support_bundle_rows,
                     "memory_class_policy_lease.integrated_support_bundle_closure",
                     "not_claimed_ceic_091_pending"),
            "CEIC-026 support-bundle non-overclaim row missing");
    leases.push_back(decision.lease);
  }
  for (const auto& lease : leases) {
    Require(manager.CancelLease(lease).ok(),
            "CEIC-026 pressure lease cancel cleanup failed");
  }
}

void LeaseRenewalExpiryAndMaxRenewal() {
  memory::HierarchicalMemoryBudgetLedger ledger;
  memory::MemoryClassPolicyLeaseManager manager(&ledger);
  auto policy = memory::DefaultMemoryClassPolicy(
      memory::MemoryClassKind::query_scratch);
  policy.max_lease_ms = 10'000;
  policy.max_renewals = 2;
  policy.provenance = Provenance("ceic-026-renewal-policy");
  Require(manager.SetClassPolicy(policy).ok(),
          "CEIC-026 class policy override failed");
  auto unbounded = policy;
  unbounded.max_lease_ms = 0;
  unbounded.provenance = Provenance("ceic-026-unbounded-policy");
  Require(!manager.SetClassPolicy(unbounded).ok(),
          "CEIC-026 unbounded class lease policy was not refused");

  auto lease = manager.AcquireLease(
      Request(memory::MemoryClassKind::query_scratch, 128, "renew-owner",
              memory::MemoryPressureState::normal, 1000, 2000));
  Require(lease.ok(), "CEIC-026 renewal setup lease failed");

  memory::MemoryBudgetLeaseRenewalRequest renew;
  renew.lease = lease.lease;
  renew.now_ms = 1500;
  renew.extend_by_ms = 1000;
  renew.provenance = Provenance("ceic-026-renewal");
  auto first = manager.RenewLease(renew);
  Require(first.ok() && first.deadline_ms == 3000 && first.renewal_count == 1,
          "CEIC-026 first renewal failed");
  RequireAuthorityEvidence(first.evidence, "CEIC-026 first renewal");

  renew.lease = first.lease;
  renew.now_ms = 2500;
  auto second = manager.RenewLease(renew);
  Require(second.ok() && second.deadline_ms == 4000 &&
              second.renewal_count == 2,
          "CEIC-026 second renewal failed");

  renew.lease = second.lease;
  renew.now_ms = 3500;
  auto refused = manager.RenewLease(renew);
  Require(!refused.ok() && refused.fail_closed,
          "CEIC-026 max-renewal refusal did not fail closed");

  auto cleanup = manager.CleanupExpiredLeases(5000);
  Require(cleanup.ok() && cleanup.cleaned_lease_count == 1 &&
              cleanup.cleaned_bytes == 128,
          "CEIC-026 expired lease cleanup failed");
  RequireAuthorityEvidence(cleanup.evidence, "CEIC-026 expiry cleanup");

  auto snapshot = manager.Snapshot();
  Require(snapshot.active_lease_count == 0 && snapshot.active_bytes == 0,
          "CEIC-026 expired lease remained active");
  Require(snapshot.renewal_count == 2 && snapshot.renewal_refusal_count == 1 &&
              snapshot.expiry_cleanup_count == 1,
          "CEIC-026 renewal/expiry counters incorrect");
}

void DisconnectAndCancelCleanup() {
  memory::HierarchicalMemoryBudgetLedger ledger;
  memory::MemoryClassPolicyLeaseManager manager(&ledger);

  auto first = manager.AcquireLease(Request(
      memory::MemoryClassKind::result_driver_buffer, 64, "disconnect-owner"));
  auto second = manager.AcquireLease(Request(
      memory::MemoryClassKind::result_driver_buffer, 32, "disconnect-owner"));
  auto survivor = manager.AcquireLease(Request(
      memory::MemoryClassKind::result_driver_buffer, 16, "survivor-owner"));
  Require(first.ok() && second.ok() && survivor.ok(),
          "CEIC-026 disconnect cleanup setup failed");

  auto cleanup = manager.CleanupOwner("disconnect-owner");
  Require(cleanup.ok() && cleanup.cleaned_lease_count == 2 &&
              cleanup.cleaned_bytes == 96,
          "CEIC-026 disconnect owner cleanup failed");
  RequireAuthorityEvidence(cleanup.evidence, "CEIC-026 disconnect cleanup");

  auto snapshot = manager.Snapshot();
  Require(snapshot.active_lease_count == 1 && snapshot.owner_cleanup_count == 2,
          "CEIC-026 owner cleanup counters incorrect");

  auto cancel = manager.CancelLease(survivor.lease);
  Require(cancel.ok() && cancel.cleaned_lease_count == 1,
          "CEIC-026 cancel cleanup failed");
  snapshot = manager.Snapshot();
  Require(snapshot.cancel_cleanup_count == 1 && snapshot.active_lease_count == 0,
          "CEIC-026 cancel cleanup counters incorrect");
}

void ProtectedMemoryAndPluginClassHandling() {
  memory::HierarchicalMemoryBudgetLedger ledger;
  memory::MemoryClassPolicyLeaseManager manager(&ledger);

  auto unsafe_protected = Request(
      memory::MemoryClassKind::protected_material, 24, "protected-owner");
  unsafe_protected.protected_material_routed_through_protected_buffer = false;
  auto refused = manager.AcquireLease(unsafe_protected);
  Require(!refused.ok() && refused.fail_closed &&
              refused.pressure_action ==
                  memory::MemoryClassPressureAction::refuse_protected_material,
          "CEIC-026 unsafe protected material was not refused");

  auto safe_protected = Request(
      memory::MemoryClassKind::protected_material, 24, "protected-owner");
  safe_protected.protected_material_routed_through_protected_buffer = true;
  safe_protected.protected_material_redacted = true;
  safe_protected.protected_zero_on_release = true;
  safe_protected.plaintext_material_observed = false;
  auto protected_grant = manager.AcquireLease(safe_protected);
  Require(protected_grant.ok(),
          "CEIC-026 protected material route was not granted");
  Require(RowsHave(protected_grant.support_bundle_rows,
                   "memory_class_policy_lease.protected_material",
                   "protected_reference_only"),
          "CEIC-026 protected support row missing redacted reference");
  Require(EvidenceHas(protected_grant.evidence,
                      "protected_material.zero_on_release=true"),
          "CEIC-026 protected zero-on-release evidence missing");

  auto plugin = Request(memory::MemoryClassKind::plugin_udr, 48, "plugin-owner",
                        memory::MemoryPressureState::high_pressure);
  plugin.plugin_udr_sandboxed = true;
  auto plugin_grant = manager.AcquireLease(plugin);
  Require(plugin_grant.ok() &&
              plugin_grant.pressure_action ==
                  memory::MemoryClassPressureAction::sandbox_plugin_udr,
          "CEIC-026 plugin/UDR class handling failed");
  Require(EvidenceHas(plugin_grant.evidence,
                      "plugin_udr.native_sandbox_closure=implemented_CEIC_027"),
          "CEIC-026 plugin/UDR CEIC-027 boundary evidence missing");

  auto snapshot = manager.Snapshot();
  const auto* protected_class =
      FindClass(snapshot, memory::MemoryClassKind::protected_material);
  const auto* plugin_class =
      FindClass(snapshot, memory::MemoryClassKind::plugin_udr);
  Require(protected_class != nullptr &&
              protected_class->requires_protected_material_route,
          "CEIC-026 protected class snapshot missing");
  Require(plugin_class != nullptr && plugin_class->plugin_udr_class,
          "CEIC-026 plugin class snapshot missing");
  Require(snapshot.protected_material_refusal_count == 1,
          "CEIC-026 protected refusal counter incorrect");

  Require(manager.CancelLease(protected_grant.lease).ok(),
          "CEIC-026 protected lease cancel failed");
  Require(manager.CancelLease(plugin_grant.lease).ok(),
          "CEIC-026 plugin lease cancel failed");
}

void RecoveryClassificationEvidenceOnly() {
  memory::HierarchicalMemoryBudgetLedger ledger;
  memory::MemoryClassPolicyLeaseManager manager(&ledger);

  memory::MemoryBudgetLeaseRecoveryInput expired;
  expired.class_kind = memory::MemoryClassKind::query_scratch;
  expired.owner_id = "recovery-owner";
  expired.scope_chain =
      ChainForClass(memory::MemoryClassKind::query_scratch, "recovery-owner");
  expired.lease_id = 10;
  expired.bytes = 64;
  expired.creation_sequence = 1;
  expired.deadline_ms = 2000;
  expired.now_ms = 3000;
  expired.provenance = Provenance("ceic-026-recovery");
  auto expired_result = manager.ClassifyRecovery(expired);
  Require(expired_result.ok() &&
              expired_result.classification ==
                  memory::MemoryBudgetLeaseRecoveryClassification::
                      expired_cleanup_required,
          "CEIC-026 expired recovery classification failed");
  RequireAuthorityEvidence(expired_result.evidence,
                           "CEIC-026 expired recovery classification");

  memory::MemoryBudgetLeaseRecoveryInput protected_input = expired;
  protected_input.class_kind = memory::MemoryClassKind::protected_material;
  protected_input.deadline_ms = 4000;
  protected_input.protected_material_routed_through_protected_buffer = false;
  auto protected_result = manager.ClassifyRecovery(protected_input);
  Require(protected_result.ok() &&
              protected_result.classification ==
                  memory::MemoryBudgetLeaseRecoveryClassification::
                      protected_material_quarantine,
          "CEIC-026 protected recovery quarantine classification failed");

  memory::MemoryBudgetLeaseRecoveryInput rebuilt = expired;
  rebuilt.deadline_ms = 5000;
  rebuilt.now_ms = 3000;
  auto rebuilt_result = manager.ClassifyRecovery(rebuilt);
  Require(rebuilt_result.ok() &&
              rebuilt_result.classification ==
                  memory::MemoryBudgetLeaseRecoveryClassification::
                      evidence_only_rebuilt,
          "CEIC-026 evidence-only recovery rebuild classification failed");

  auto unsafe = expired;
  unsafe.provenance.parser_authority = true;
  auto unsafe_result = manager.ClassifyRecovery(unsafe);
  Require(!unsafe_result.ok() && unsafe_result.fail_closed &&
              unsafe_result.classification ==
                  memory::MemoryBudgetLeaseRecoveryClassification::
                      unsafe_provenance_refused,
          "CEIC-026 unsafe recovery provenance was not refused");

  auto snapshot = manager.Snapshot();
  Require(snapshot.recovery_classification_count == 4,
          "CEIC-026 recovery classification counter incorrect");
}

void UnsafeProvenanceAndClusterRefusal() {
  memory::HierarchicalMemoryBudgetLedger ledger;
  memory::MemoryClassPolicyLeaseManager manager(&ledger);

  auto unsafe = Request(memory::MemoryClassKind::query_scratch, 16,
                        "unsafe-owner");
  unsafe.provenance.source =
      memory::HierarchicalMemoryBudgetProvenanceSource::test_fixture;
  unsafe.provenance.source_label = "fixture-only-proof";
  auto unsafe_decision = manager.AcquireLease(unsafe);
  Require(!unsafe_decision.ok() && unsafe_decision.fail_closed,
          "CEIC-026 unsafe provenance was not refused");
  RequireAuthorityEvidence(unsafe_decision.evidence,
                           "CEIC-026 unsafe provenance refusal");

  auto cluster = Request(memory::MemoryClassKind::query_scratch, 16,
                         "cluster-owner");
  cluster.route_kind = memory::MemoryBudgetLeaseRouteKind::cluster;
  auto cluster_decision = manager.AcquireLease(cluster);
  Require(!cluster_decision.ok() && cluster_decision.fail_closed &&
              cluster_decision.cluster_external_provider_required &&
              cluster_decision.pressure_action ==
                  memory::MemoryClassPressureAction::external_provider_required,
          "CEIC-026 cluster memory request did not fail closed");
  Require(EvidenceHas(cluster_decision.evidence,
                      "cluster_boundary=external_provider_only_fail_closed"),
          "CEIC-026 cluster external-provider evidence missing");

  auto snapshot = manager.Snapshot();
  Require(snapshot.unsafe_provenance_refusal_count == 1 &&
              snapshot.cluster_refusal_count == 1,
          "CEIC-026 refusal counters incorrect");
  Require(MetricsHave(snapshot.metrics, "sb_memory_budget_lease_active_bytes") &&
              RowsHave(snapshot.support_bundle_rows,
                       "memory_class_policy_lease.cluster_boundary",
                       "external_provider_only_fail_closed"),
          "CEIC-026 snapshot metric/support rows missing");
}

}  // namespace

int main() {
  DefaultPolicyMatrixCoversRequiredClasses();
  ClassSpecificPressureActionsProduceLeaseEvidence();
  LeaseRenewalExpiryAndMaxRenewal();
  DisconnectAndCancelCleanup();
  ProtectedMemoryAndPluginClassHandling();
  RecoveryClassificationEvidenceOnly();
  UnsafeProvenanceAndClusterRefusal();
  std::cout << "ceic_026_memory_class_policy_lease_gate=pass\n";
  return EXIT_SUCCESS;
}
