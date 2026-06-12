// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// CEIC-025 focused validation for multi-tenant memory fairness and scheduling.
#include "memory_fairness_scheduler.hpp"

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
    std::string label = "ceic-025-runtime-policy") {
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

std::vector<memory::HierarchicalMemoryScopeRef> QueryChain(
    std::string tenant,
    std::string user,
    std::string role,
    std::string session,
    std::string query,
    std::string operator_id = "op") {
  return {
      Scope(memory::HierarchicalMemoryScopeKind::process, "process"),
      Scope(memory::HierarchicalMemoryScopeKind::database, "database"),
      Scope(memory::HierarchicalMemoryScopeKind::tenant, std::move(tenant)),
      Scope(memory::HierarchicalMemoryScopeKind::user, std::move(user)),
      Scope(memory::HierarchicalMemoryScopeKind::role, std::move(role)),
      Scope(memory::HierarchicalMemoryScopeKind::session, std::move(session)),
      Scope(memory::HierarchicalMemoryScopeKind::transaction, "txn"),
      Scope(memory::HierarchicalMemoryScopeKind::statement, "stmt"),
      Scope(memory::HierarchicalMemoryScopeKind::query, std::move(query)),
      Scope(memory::HierarchicalMemoryScopeKind::operator_scope,
            std::move(operator_id))};
}

std::vector<memory::HierarchicalMemoryScopeRef> BackgroundChain(
    std::string tenant,
    std::string job) {
  return {
      Scope(memory::HierarchicalMemoryScopeKind::process, "process"),
      Scope(memory::HierarchicalMemoryScopeKind::database, "database"),
      Scope(memory::HierarchicalMemoryScopeKind::tenant, std::move(tenant)),
      Scope(memory::HierarchicalMemoryScopeKind::background, std::move(job))};
}

memory::MemoryFairnessScopePolicy Policy(
    memory::HierarchicalMemoryScopeRef scope,
    memory::u64 hard,
    memory::u64 soft = 0,
    memory::u64 guarantee = 0,
    memory::u64 burst = 0,
    memory::u64 burst_window = 0,
    memory::u64 priority_weight = 1,
    bool background = false) {
  memory::MemoryFairnessScopePolicy policy;
  policy.scope = std::move(scope);
  policy.hard_max_bytes = hard;
  policy.soft_max_bytes = soft;
  policy.guarantee_bytes = guarantee;
  policy.burst_bytes = burst;
  policy.burst_window_ms = burst_window;
  policy.priority_weight = priority_weight;
  policy.background_scope = background;
  policy.provenance = Provenance();
  return policy;
}

void AddPolicy(memory::MultiTenantMemoryFairnessScheduler* scheduler,
               memory::MemoryFairnessScopePolicy policy) {
  auto result = scheduler->SetScopePolicy(std::move(policy));
  Require(result.ok(), "CEIC-025 policy setup failed");
}

memory::MemoryFairnessRequest Request(
    std::vector<memory::HierarchicalMemoryScopeRef> chain,
    memory::u64 bytes,
    std::string owner,
    memory::MemoryFairnessWorkClass work_class =
        memory::MemoryFairnessWorkClass::foreground) {
  memory::MemoryFairnessRequest request;
  request.scope_chain = std::move(chain);
  request.requested_bytes = bytes;
  request.owner_id = std::move(owner);
  request.category = memory::MemoryCategory::executor_query_reserved;
  request.memory_class = "query_scratch";
  request.work_class = work_class;
  request.priority = work_class == memory::MemoryFairnessWorkClass::foreground
                         ? 10
                         : 1;
  request.weight = work_class == memory::MemoryFairnessWorkClass::foreground
                       ? 10
                       : 1;
  request.provenance = Provenance();
  return request;
}

bool EvidenceHas(const memory::MemoryFairnessDecision& decision,
                 std::string_view token) {
  for (const auto& row : decision.evidence) {
    if (row.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool BundleHas(const memory::MemoryFairnessDecision& decision,
               std::string_view key,
               std::string_view value) {
  for (const auto& row : decision.support_bundle_rows) {
    if (row.key.find(key) != std::string::npos &&
        row.value.find(value) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool DiagnosticArgumentHas(const memory::DiagnosticRecord& diagnostic,
                           std::string_view key,
                           std::string_view value) {
  for (const auto& arg : diagnostic.arguments) {
    if (arg.key.find(key) != std::string::npos &&
        arg.value.find(value) != std::string::npos) {
      return true;
    }
  }
  return false;
}

void RequireAuthorityEvidence(const memory::MemoryFairnessDecision& decision) {
  Require(EvidenceHas(decision,
                      "CEIC-025_MULTI_TENANT_MEMORY_FAIRNESS"),
          "CEIC-025 evidence anchor missing");
  Require(EvidenceHas(
              decision,
              "memory_fairness.authority_scope=evidence_only_not_transaction_finality_visibility_authorization_security_recovery_parser_reference_wal_benchmark_optimizer_plan_index_finality_cluster_or_agent_action_authority"),
          "CEIC-025 authority boundary evidence missing");
  Require(BundleHas(decision,
                    "memory_fairness.integrated_support_bundle_closure",
                    "not_claimed_ceic_091_pending"),
          "CEIC-025 support-bundle non-overclaim row missing");
}

void HighPriorityTenantProtection() {
  memory::HierarchicalMemoryBudgetLedger ledger;
  memory::MultiTenantMemoryFairnessScheduler scheduler(&ledger);
  AddPolicy(&scheduler,
            Policy(Scope(memory::HierarchicalMemoryScopeKind::process,
                         "process"),
                   1000));
  AddPolicy(&scheduler,
            Policy(Scope(memory::HierarchicalMemoryScopeKind::tenant,
                         "tenant-low"),
                   900, 900, 0, 0, 0, 2));
  AddPolicy(&scheduler,
            Policy(Scope(memory::HierarchicalMemoryScopeKind::tenant,
                         "tenant-high"),
                   800, 800, 400, 0, 0, 30));

  auto low = Request(QueryChain("tenant-low", "low-user", "low-role",
                                "low-session-a", "low-query-a"),
                     500, "low-a");
  auto low_grant = scheduler.Admit(low);
  Require(low_grant.ok(), "CEIC-025 low tenant setup grant failed");

  auto low_extra = Request(QueryChain("tenant-low", "low-user", "low-role",
                                      "low-session-b", "low-query-b"),
                           200, "low-b");
  low_extra.priority = 1;
  low_extra.weight = 1;
  low_extra.spillable = true;
  low_extra.cancelable = true;
  auto low_refused = scheduler.Admit(low_extra);
  Require(low_refused.action == memory::MemoryFairnessDecisionAction::spill,
          "CEIC-025 low-priority tenant did not spill before consuming high-priority guarantee");
  Require(low_refused.foreground_protection_applied,
          "CEIC-025 foreground guarantee headroom was not protected");
  RequireAuthorityEvidence(low_refused);

  auto high = Request(QueryChain("tenant-high", "high-user", "high-role",
                                 "high-session", "high-query"),
                      400, "high");
  high.priority = 30;
  high.weight = 10;
  auto high_grant = scheduler.Admit(high);
  Require(high_grant.ok(), "CEIC-025 high-priority tenant guarantee grant failed");

  Require(scheduler.Release(high_grant.grant).ok(),
          "CEIC-025 high grant release failed");
  Require(scheduler.Release(low_grant.grant).ok(),
          "CEIC-025 low grant release failed");
}

void LowPriorityReliefOrdering() {
  memory::HierarchicalMemoryBudgetLedger ledger;
  memory::MultiTenantMemoryFairnessScheduler scheduler(&ledger);
  AddPolicy(&scheduler,
            Policy(Scope(memory::HierarchicalMemoryScopeKind::process,
                         "process"),
                   1000));
  AddPolicy(&scheduler,
            Policy(Scope(memory::HierarchicalMemoryScopeKind::tenant,
                         "tenant-high"),
                   800, 800, 500, 0, 0, 40));
  AddPolicy(&scheduler,
            Policy(Scope(memory::HierarchicalMemoryScopeKind::tenant,
                         "tenant-low"),
                   900, 900, 0, 0, 0, 1));

  auto current = Request(QueryChain("tenant-low", "low-user", "low-role",
                                    "low-session", "low-query"),
                         400, "low-current");
  Require(scheduler.Admit(current).ok(), "CEIC-025 relief setup grant failed");

  auto spill = Request(QueryChain("tenant-low", "low-user", "low-role",
                                  "low-session-2", "low-spill"),
                       200, "low-spill");
  spill.priority = 1;
  spill.weight = 1;
  spill.spillable = true;
  spill.cancelable = true;
  Require(scheduler.Admit(spill).action ==
              memory::MemoryFairnessDecisionAction::spill,
          "CEIC-025 did not choose spill before cancel");

  auto throttle = Request(QueryChain("tenant-low", "low-user", "low-role",
                                     "low-session-3", "low-throttle"),
                          200, "low-throttle");
  throttle.priority = 1;
  throttle.weight = 1;
  throttle.spillable = false;
  throttle.throttleable = true;
  throttle.cancelable = true;
  Require(scheduler.Admit(throttle).action ==
              memory::MemoryFairnessDecisionAction::throttle,
          "CEIC-025 did not throttle before cancel when spilling was unavailable");

  auto cancel = Request(QueryChain("tenant-low", "low-user", "low-role",
                                   "low-session-4", "low-cancel"),
                        200, "low-cancel");
  cancel.priority = 1;
  cancel.weight = 1;
  cancel.spillable = false;
  cancel.throttleable = false;
  cancel.cancelable = true;
  Require(scheduler.Admit(cancel).action ==
              memory::MemoryFairnessDecisionAction::cancel,
          "CEIC-025 did not cancel after spill and throttle were unavailable");
}

void BackgroundCannotStarveForeground() {
  memory::HierarchicalMemoryBudgetLedger ledger;
  memory::MultiTenantMemoryFairnessScheduler scheduler(&ledger);
  auto process = Policy(Scope(memory::HierarchicalMemoryScopeKind::process,
                              "process"),
                        1000);
  process.foreground_protection_bytes = 500;
  AddPolicy(&scheduler, process);
  AddPolicy(&scheduler,
            Policy(Scope(memory::HierarchicalMemoryScopeKind::tenant,
                         "tenant-foreground"),
                   800, 800, 500, 0, 0, 20));
  AddPolicy(&scheduler,
            Policy(Scope(memory::HierarchicalMemoryScopeKind::background,
                         "stats-job"),
                   900, 900, 0, 0, 0, 1, true));

  auto background = Request(BackgroundChain("tenant-background", "stats-job"),
                            600, "stats-job",
                            memory::MemoryFairnessWorkClass::background);
  background.throttleable = true;
  auto refused = scheduler.Admit(background);
  Require(refused.action == memory::MemoryFairnessDecisionAction::throttle,
          "CEIC-025 background job did not throttle before starving foreground work");
  Require(refused.foreground_protection_applied,
          "CEIC-025 background refusal missing foreground protection evidence");

  auto foreground = Request(QueryChain("tenant-foreground", "fg-user",
                                       "fg-role", "fg-session", "fg-query"),
                            500, "foreground");
  auto granted = scheduler.Admit(foreground);
  Require(granted.ok(), "CEIC-025 foreground work was starved by background policy");
  Require(scheduler.Release(granted.grant).ok(),
          "CEIC-025 foreground grant release failed");
}

void TenantCannotBypassThroughSessions() {
  memory::HierarchicalMemoryBudgetLedger ledger;
  memory::MultiTenantMemoryFairnessScheduler scheduler(&ledger);
  AddPolicy(&scheduler,
            Policy(Scope(memory::HierarchicalMemoryScopeKind::process,
                         "process"),
                   2000));
  AddPolicy(&scheduler,
            Policy(Scope(memory::HierarchicalMemoryScopeKind::tenant,
                         "tenant-shared"),
                   700, 700));
  AddPolicy(&scheduler,
            Policy(Scope(memory::HierarchicalMemoryScopeKind::session,
                         "session-a"),
                   500, 500));
  AddPolicy(&scheduler,
            Policy(Scope(memory::HierarchicalMemoryScopeKind::session,
                         "session-b"),
                   500, 500));

  auto session_a = Request(QueryChain("tenant-shared", "user", "role",
                                      "session-a", "query-a"),
                           400, "session-a");
  auto granted = scheduler.Admit(session_a);
  Require(granted.ok(), "CEIC-025 first session grant failed");

  auto session_b = Request(QueryChain("tenant-shared", "user", "role",
                                      "session-b", "query-b"),
                           350, "session-b");
  session_b.throttleable = false;
  session_b.cancelable = true;
  auto refused = scheduler.Admit(session_b);
  Require(refused.action == memory::MemoryFairnessDecisionAction::cancel,
          "CEIC-025 tenant aggregate bypass did not cancel second session");
  Require(refused.dominant_scope_key == "tenant:tenant-shared",
          "CEIC-025 tenant aggregate was not the dominant refusal scope");

  Require(scheduler.Release(granted.grant).ok(),
          "CEIC-025 session grant release failed");
}

void UserRoleSessionLimitsCompose() {
  memory::HierarchicalMemoryBudgetLedger ledger;
  memory::MultiTenantMemoryFairnessScheduler scheduler(&ledger);
  AddPolicy(&scheduler,
            Policy(Scope(memory::HierarchicalMemoryScopeKind::process,
                         "process"),
                   3000));
  AddPolicy(&scheduler,
            Policy(Scope(memory::HierarchicalMemoryScopeKind::tenant,
                         "tenant-compose"),
                   2000));
  AddPolicy(&scheduler,
            Policy(Scope(memory::HierarchicalMemoryScopeKind::user,
                         "user-compose"),
                   600));
  AddPolicy(&scheduler,
            Policy(Scope(memory::HierarchicalMemoryScopeKind::role,
                         "role-compose"),
                   500));
  AddPolicy(&scheduler,
            Policy(Scope(memory::HierarchicalMemoryScopeKind::session,
                         "session-tight"),
                   450));
  AddPolicy(&scheduler,
            Policy(Scope(memory::HierarchicalMemoryScopeKind::session,
                         "session-wide"),
                   500));

  auto too_large_session = Request(QueryChain("tenant-compose", "user-compose",
                                              "role-compose", "session-tight",
                                              "query-too-large"),
                                   460, "session-tight");
  too_large_session.throttleable = false;
  too_large_session.cancelable = true;
  auto session_refused = scheduler.Admit(too_large_session);
  Require(session_refused.action == memory::MemoryFairnessDecisionAction::cancel,
          "CEIC-025 session hard max did not compose");
  Require(session_refused.dominant_scope_key == "session:session-tight",
          "CEIC-025 session limit was not dominant");

  auto first = Request(QueryChain("tenant-compose", "user-compose",
                                  "role-compose", "session-wide",
                                  "query-first"),
                       400, "compose-first");
  auto first_grant = scheduler.Admit(first);
  Require(first_grant.ok(), "CEIC-025 composed first grant failed");

  auto second = Request(QueryChain("tenant-compose", "user-compose",
                                   "role-compose", "session-tight",
                                   "query-second"),
                        150, "compose-second");
  second.throttleable = false;
  second.cancelable = true;
  auto role_refused = scheduler.Admit(second);
  Require(role_refused.action == memory::MemoryFairnessDecisionAction::cancel,
          "CEIC-025 role hard max did not compose across sessions");
  Require(role_refused.dominant_scope_key == "role:role-compose",
          "CEIC-025 role limit was not dominant across sessions");

  Require(scheduler.Release(first_grant.grant).ok(),
          "CEIC-025 composed first grant release failed");
}

void BurstWindowsExpireAndRefuse() {
  memory::HierarchicalMemoryBudgetLedger ledger;
  memory::MultiTenantMemoryFairnessScheduler scheduler(&ledger);
  AddPolicy(&scheduler,
            Policy(Scope(memory::HierarchicalMemoryScopeKind::process,
                         "process"),
                   1000));
  AddPolicy(&scheduler,
            Policy(Scope(memory::HierarchicalMemoryScopeKind::tenant,
                         "tenant-burst"),
                   200, 100, 0, 50, 100));

  auto first = Request(QueryChain("tenant-burst", "burst-user", "burst-role",
                                  "burst-session", "burst-query-a"),
                       140, "burst-a");
  first.now_ms = 0;
  auto first_grant = scheduler.Admit(first);
  Require(first_grant.ok(), "CEIC-025 burst grant failed");
  Require(first_grant.burst_used,
          "CEIC-025 first burst request did not record burst use");

  auto second = Request(QueryChain("tenant-burst", "burst-user", "burst-role",
                                   "burst-session", "burst-query-b"),
                        5, "burst-b");
  second.now_ms = 50;
  auto second_grant = scheduler.Admit(second);
  Require(second_grant.ok(), "CEIC-025 in-window burst grant failed");
  Require(second_grant.burst_used,
          "CEIC-025 in-window burst did not record burst use");

  auto expired = Request(QueryChain("tenant-burst", "burst-user", "burst-role",
                                    "burst-session", "burst-query-c"),
                         1, "burst-c");
  expired.now_ms = 150;
  expired.throttleable = true;
  auto refused = scheduler.Admit(expired);
  Require(refused.action == memory::MemoryFairnessDecisionAction::throttle,
          "CEIC-025 expired burst did not throttle");
  Require(refused.burst_window_expired,
          "CEIC-025 expired burst refusal missing expiry evidence");
  Require(EvidenceHas(refused, "memory_fairness.burst.window_expired=true"),
          "CEIC-025 expired burst evidence row missing");

  Require(scheduler.Release(second_grant.grant).ok(),
          "CEIC-025 second burst release failed");
  Require(scheduler.Release(first_grant.grant).ok(),
          "CEIC-025 first burst release failed");
}

void StarvationPreventionEvidence() {
  memory::HierarchicalMemoryBudgetLedger ledger;
  memory::MultiTenantMemoryFairnessScheduler scheduler(&ledger);
  AddPolicy(&scheduler,
            Policy(Scope(memory::HierarchicalMemoryScopeKind::process,
                         "process"),
                   1000));
  auto tenant = Policy(Scope(memory::HierarchicalMemoryScopeKind::tenant,
                             "tenant-starved"),
                       600, 100, 500, 0, 0, 30);
  tenant.starvation_prevention_ms = 1000;
  AddPolicy(&scheduler, tenant);

  auto request = Request(QueryChain("tenant-starved", "starved-user",
                                    "starved-role", "starved-session",
                                    "starved-query"),
                         200, "starved");
  request.now_ms = 2000;
  request.wait_started_at_ms = 500;
  auto granted = scheduler.Admit(request);
  Require(granted.ok(),
          "CEIC-025 starvation prevention did not grant under guarantee");
  Require(granted.starvation_prevention_applied,
          "CEIC-025 starvation prevention flag missing");
  Require(EvidenceHas(granted,
                      "memory_fairness.starvation_prevention.applied=true"),
          "CEIC-025 starvation prevention evidence missing");
  RequireAuthorityEvidence(granted);

  auto snapshot = scheduler.Snapshot();
  Require(snapshot.starvation_prevention_count == 1,
          "CEIC-025 starvation prevention metric count mismatch");
  Require(!snapshot.metrics.empty(),
          "CEIC-025 metrics rows missing from snapshot");
  Require(!snapshot.support_bundle_rows.empty(),
          "CEIC-025 support bundle rows missing from snapshot");

  Require(scheduler.Release(granted.grant).ok(),
          "CEIC-025 starvation grant release failed");
}

void AuthorityDriftIsRefused() {
  memory::HierarchicalMemoryBudgetLedger ledger;
  memory::MultiTenantMemoryFairnessScheduler scheduler(&ledger);
  AddPolicy(&scheduler,
            Policy(Scope(memory::HierarchicalMemoryScopeKind::process,
                         "process"),
                   1000));

  auto unsafe = Request(QueryChain("tenant-authority", "authority-user",
                                   "authority-role", "authority-session",
                                   "authority-query"),
                        10, "unsafe");
  unsafe.provenance.transaction_finality_authority = true;
  auto refused = scheduler.Admit(unsafe);
  Require(refused.action == memory::MemoryFairnessDecisionAction::deny,
          "CEIC-025 unsafe authority provenance was not denied");
  Require(DiagnosticArgumentHas(refused.diagnostic,
                                "authority_scope",
                                "not_transaction_finality"),
          "CEIC-025 unsafe authority diagnostic missing non-authority scope");

  auto cluster = Request(BackgroundChain("tenant-cluster", "cluster-job"),
                         10, "cluster-job",
                         memory::MemoryFairnessWorkClass::background);
  cluster.category = memory::MemoryCategory::cluster_control_reserved;
  auto cluster_refused = scheduler.Admit(cluster);
  Require(cluster_refused.action == memory::MemoryFairnessDecisionAction::deny,
          "CEIC-025 cluster memory route was not blocked");
  Require(EvidenceHas(cluster_refused,
                      "memory_fairness.cluster_production_behavior=blocked_not_implemented"),
          "CEIC-025 cluster blocked evidence missing");
}

void ReleaseApiFailsClosedWithoutLedger() {
  memory::MultiTenantMemoryFairnessScheduler scheduler(nullptr);
  memory::MemoryFairnessGrantToken token;
  token.grant_id = 1;
  token.bytes = 64;
  token.reservation.token_id = 1;
  token.reservation.bytes = 64;

  const auto release = scheduler.Release(token);
  Require(!release.ok(),
          "CEIC-025 release without CEIC-011 ledger unexpectedly succeeded");
  Require(DiagnosticArgumentHas(release.diagnostic,
                                "authority_scope",
                                "not_transaction_finality"),
          "CEIC-025 release missing non-authority diagnostic scope");
}

}  // namespace

int main() {
  HighPriorityTenantProtection();
  LowPriorityReliefOrdering();
  BackgroundCannotStarveForeground();
  TenantCannotBypassThroughSessions();
  UserRoleSessionLimitsCompose();
  BurstWindowsExpireAndRefuse();
  StarvationPreventionEvidence();
  AuthorityDriftIsRefused();
  ReleaseApiFailsClosedWithoutLedger();
  return EXIT_SUCCESS;
}
