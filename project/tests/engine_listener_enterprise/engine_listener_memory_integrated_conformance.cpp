// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "hierarchical_memory_budget_ledger.hpp"
#include "memory.hpp"
#include "memory_pressure_response.hpp"
#include "memory_support_bundle.hpp"
#include "query_memory_arena.hpp"
#include "temp_workspace_lifecycle.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

namespace memory = scratchbird::core::memory;
namespace platform = scratchbird::core::platform;

using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::u64;

struct MatrixRow {
  std::string proof_id;
  std::string status;
  std::string evidence;
};

std::vector<MatrixRow> g_rows;

[[noreturn]] void Fail(std::string message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string message) {
  if (!condition) {
    Fail(std::move(message));
  }
}

void AddRow(std::string proof_id, std::string status, std::string evidence) {
  g_rows.push_back({std::move(proof_id), std::move(status), std::move(evidence)});
}

std::string CsvEscape(std::string value) {
  bool quote = false;
  for (const char ch : value) {
    quote = quote || ch == ',' || ch == '"' || ch == '\n' || ch == '\r';
  }
  if (!quote) {
    return value;
  }
  std::string escaped = "\"";
  for (const char ch : value) {
    if (ch == '"') {
      escaped.push_back('"');
    }
    escaped.push_back(ch);
  }
  escaped.push_back('"');
  return escaped;
}

void WriteMatrix(const std::filesystem::path& output) {
  std::filesystem::create_directories(output.parent_path());
  std::ofstream file(output);
  Require(file.good(), "ELER-050 matrix output could not be opened");
  file << "proof_id,status,evidence\n";
  for (const auto& row : g_rows) {
    file << CsvEscape(row.proof_id) << ',' << CsvEscape(row.status) << ','
         << CsvEscape(row.evidence) << '\n';
  }
  Require(file.good(), "ELER-050 matrix output write failed");
}

std::string Lower(std::string value) {
  std::transform(value.begin(),
                 value.end(),
                 value.begin(),
                 [](unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });
  return value;
}

bool ContainsSensitiveCanary(std::string_view value) {
  const std::string lower = Lower(std::string(value));
  return lower.find("eler050-secret") != std::string::npos ||
         lower.find("plain-password") != std::string::npos ||
         lower.find("kms_plaintext") != std::string::npos ||
         lower.find("bearer eler050") != std::string::npos;
}

memory::AllocationPolicy ProductionPolicy() {
  auto policy = memory::DefaultLocalEngineMemoryPolicy();
  policy.policy_name = "engine_listener_memory_integrated_production";
  policy.hard_limit_bytes = 8ull * 1024ull * 1024ull;
  policy.soft_limit_bytes = 6ull * 1024ull * 1024ull;
  policy.per_context_limit_bytes = 4ull * 1024ull * 1024ull;
  policy.page_buffer_pool_limit_bytes = 2ull * 1024ull * 1024ull;
  policy.track_allocations = true;
  policy.zero_memory_on_release = true;
  return policy;
}

memory::MemoryTag Tag(std::string purpose, std::string context_id) {
  memory::MemoryTag tag;
  tag.purpose = std::move(purpose);
  tag.category = memory::MemoryCategory::test_probe;
  tag.lifetime = memory::MemoryLifetime::temporary;
  tag.owner = "ELER-050";
  tag.context_id = std::move(context_id);
  tag.database_id = "eler050-db";
  tag.session_id = "eler050-session";
  tag.transaction_id = "eler050-transaction";
  tag.statement_id = "eler050-statement";
  tag.query_id = "eler050-query";
  return tag;
}

memory::QueryMemoryContext QueryContext(std::string suffix) {
  memory::QueryMemoryContext context;
  context.engine_id = "eler050-engine";
  context.database_id = "eler050-db";
  context.session_id = "eler050-session-" + suffix;
  context.transaction_id = "eler050-transaction-" + suffix;
  context.statement_id = "eler050-statement-" + suffix;
  context.query_id = "eler050-query-" + suffix;
  context.operation_id = "eler050-operation-" + suffix;
  context.engine_mga_authoritative = true;
  return context;
}

memory::QueryMemoryArenaLimits QueryLimits(bool allow_spill) {
  memory::QueryMemoryArenaLimits limits;
  limits.hard_limit_bytes = 8192;
  limits.soft_limit_bytes = allow_spill ? 256 : 8192;
  limits.family_limit_bytes = 8192;
  limits.query_limit_bytes = 8192;
  limits.spill_limit_bytes = 8192;
  limits.allow_spill = allow_spill;
  limits.require_hierarchical_reservation = true;
  return limits;
}

memory::TempWorkspacePolicy TempPolicy(const std::filesystem::path& root) {
  memory::TempWorkspacePolicy policy;
  policy.policy_name = "engine_listener_memory_integrated_temp";
  policy.root_path = root;
  policy.filespace_quota_bytes = 8192;
  policy.session_quota_bytes = 8192;
  policy.transaction_quota_bytes = 8192;
  policy.statement_quota_bytes = 8192;
  policy.operation_quota_bytes = 8192;
  return policy;
}

memory::QueryMemoryGrantRequest GrantRequest(memory::QueryMemoryFamily family,
                                             u64 bytes,
                                             bool spillable,
                                             std::string purpose) {
  memory::QueryMemoryGrantRequest request;
  request.family = family;
  request.bytes = bytes;
  request.spillable = spillable;
  request.purpose = std::move(purpose);
  return request;
}

void ConfigureProductionDefaultManager() {
  const auto before = memory::DefaultMemoryManagerState();
  Require(!before.initialized,
          "ELER-050 default manager was initialized before startup proof");
  Require(before.active_policy.refuse_all_allocations,
          "ELER-050 unconfigured default manager was not refusal-only");

  const auto configured = memory::ConfigureDefaultMemoryManager(
      ProductionPolicy(), "engine_listener_memory_integrated_startup");
  Require(configured.ok(), "ELER-050 production memory startup policy failed");
  Require(configured.applied && !configured.fixture_mode,
          "ELER-050 production memory startup was not applied as production");

  const auto state = memory::DefaultMemoryManagerState();
  Require(state.initialized && state.explicitly_configured &&
              !state.fixture_mode,
          "ELER-050 default manager state did not retain production startup");
  Require(state.provenance == "engine_listener_memory_integrated_startup",
          "ELER-050 default manager startup provenance drifted");
  Require(!state.active_policy.refuse_all_allocations,
          "ELER-050 production default manager remained refusal-only");

  auto allocation = memory::DefaultMemoryManager().Allocate(
      1024, alignof(std::max_align_t), Tag("startup_probe", "startup"));
  Require(allocation.ok(), "ELER-050 production default allocation failed");
  const auto release = memory::DefaultMemoryManager().Deallocate(
      allocation.pointer, Tag("startup_probe_release", "startup"));
  Require(release.ok(), "ELER-050 production default allocation release failed");
  Require(memory::DefaultMemoryManager().Snapshot().active_allocation_count == 0,
          "ELER-050 startup proof leaked memory");
  AddRow("production_default_manager_startup",
         "pass",
         "configured=true;fixture_mode=false;provenance=engine_listener_memory_integrated_startup;unconfigured_refusal=true");
}

void ProveQueryReservationIntegration(const std::filesystem::path& temp_root) {
  auto* allocator = memory::DefaultMemoryManager().allocator();
  memory::HierarchicalMemoryBudgetLedger ledger;
  memory::UnifiedMemorySpillBudgetLedger unified("eler050-query", 8192);

  memory::QueryMemoryArena missing_ledger(QueryContext("missing-ledger"),
                                          QueryLimits(false),
                                          allocator,
                                          nullptr,
                                          &unified,
                                          nullptr);
  const auto refused = missing_ledger.Grant(GrantRequest(
      memory::QueryMemoryFamily::relational, 512, false, "missing_ledger"));
  Require(!refused.ok() && refused.fail_closed,
          "ELER-050 query arena admitted grant without hierarchical ledger");
  Require(refused.diagnostic.diagnostic_code ==
              "SB_QUERY_MEMORY_ARENA.HIERARCHICAL_RESERVATION_REQUIRED",
          "ELER-050 query arena missing-ledger diagnostic drifted");

  memory::QueryMemoryArena heap_arena(QueryContext("heap"),
                                      QueryLimits(false),
                                      allocator,
                                      nullptr,
                                      &unified,
                                      &ledger);
  const auto heap = heap_arena.Grant(GrantRequest(
      memory::QueryMemoryFamily::relational, 1024, false, "heap_grant"));
  Require(heap.ok() && heap.grant.has_value(),
          "ELER-050 reservation-backed heap grant failed");
  Require(ledger.Snapshot().current_bytes == 1024,
          "ELER-050 hierarchical ledger did not commit heap grant");
  const auto heap_release = heap_arena.Release(heap.grant->grant_id);
  Require(heap_release.ok(), "ELER-050 heap grant release failed");
  Require(ledger.Snapshot().current_bytes == 0 &&
              heap_arena.Snapshot().active_grant_count == 0,
          "ELER-050 heap grant release did not reconcile ledger and arena");

  const auto spill_root = temp_root / "query_spill";
  std::filesystem::remove_all(spill_root);
  memory::TempWorkspaceLifecycleManager temp_workspace(TempPolicy(spill_root));
  memory::QueryMemoryArena spill_arena(QueryContext("spill"),
                                       QueryLimits(true),
                                       allocator,
                                       &temp_workspace,
                                       &unified,
                                       &ledger);
  const auto spill = spill_arena.Grant(GrantRequest(
      memory::QueryMemoryFamily::document, 1024, true, "spill_grant"));
  Require(spill.ok() && spill.grant.has_value() && spill.grant->spilled,
          "ELER-050 spillable query grant was not routed to temp workspace");
  Require(ledger.Snapshot().current_bytes == 1024 &&
              unified.Snapshot().total_bytes == 1024,
          "ELER-050 spill grant did not reserve unified and hierarchical budgets");
  const auto spill_release = spill_arena.Release(spill.grant->grant_id);
  Require(spill_release.ok(), "ELER-050 spill grant release failed");
  Require(ledger.Snapshot().current_bytes == 0 &&
              unified.Snapshot().total_bytes == 0 &&
              spill_arena.Snapshot().active_grant_count == 0,
          "ELER-050 spill grant release did not reconcile all budgets");
  std::filesystem::remove_all(spill_root);

  auto unsafe_context = QueryContext("unsafe");
  unsafe_context.parser_or_reference_finality_or_visibility_authority = true;
  memory::QueryMemoryArena unsafe_arena(unsafe_context,
                                        QueryLimits(false),
                                        allocator,
                                        nullptr,
                                        &unified,
                                        &ledger);
  const auto unsafe = unsafe_arena.Grant(GrantRequest(
      memory::QueryMemoryFamily::graph, 512, false, "unsafe_authority"));
  Require(!unsafe.ok() && unsafe.fail_closed,
          "ELER-050 query arena admitted parser/reference authority");

  AddRow("query_arena_hierarchical_reservation",
         "pass",
         "missing_ledger_refused=true;heap_grant_reconciled=true;spill_grant_reconciled=true;unsafe_authority_refused=true");
}

memory::MemoryPressureDecision EmergencyPressureDecision(
    memory::EmergencyMemoryReserve* reserve) {
  memory::MemoryPressurePolicy policy;
  policy.max_emergency_top_contexts = 4;
  memory::MemoryPressureObservation observation;
  observation.route_label = "engine.listener.memory.integrated";
  observation.operation_id = "ELER-050";
  observation.current_bytes = 990;
  observation.soft_limit_bytes = 700;
  observation.hard_limit_bytes = 1000;
  observation.emergency_limit_bytes = 990;
  observation.unified_budget_bytes = 990;
  observation.unified_budget_limit_bytes = 1000;
  observation.page_cache_resident_bytes = 4096;
  observation.page_cache_target_bytes = 1024;
  observation.active_spill_bytes = 512;
  observation.reclaimable_background_bytes = 2048;
  observation.low_priority_query_count = 2;
  observation.pending_readmission_count = 8;
  observation.spill_supported = true;
  observation.forced_spill_supported = true;
  observation.page_cache_shrink_supported = true;
  observation.background_cleanup_supported = true;
  observation.cancellation_supported = true;
  observation.low_priority_cancellation_supported = true;
  observation.forced_cancel_supported = true;
  observation.noncritical_agent_suspend_supported = true;
  observation.emergency_diagnostics_supported = true;
  observation.adaptive_batch_reduction_supported = true;
  observation.engine_mga_authoritative = true;
  observation.mga_recheck_preserved = true;
  observation.security_recheck_preserved = true;
  observation.host_pressure.observed = true;
  observation.host_pressure.pressure = true;
  observation.host_pressure.current_bytes = 990;
  observation.host_pressure.total_bytes = 1000;
  observation.host_pressure.available_bytes = 10;
  observation.host_pressure.pressure_percent = 99;
  observation.container_pressure.observed = true;
  observation.container_pressure.pressure = true;
  observation.container_pressure.current_bytes = 990;
  observation.container_pressure.limit_bytes = 1000;
  observation.container_pressure.pressure_percent = 99;
  for (int i = 0; i < 6; ++i) {
    memory::MemoryPressureTopContext context;
    context.scope_kind = "query";
    context.scope_id = "eler050-top-context-" + std::to_string(i);
    context.current_bytes = static_cast<u64>(4096 - i * 256);
    context.peak_bytes = context.current_bytes + 128;
    context.low_priority = i % 2 == 0;
    context.cancelable = true;
    context.mga_recheck_required = true;
    context.security_recheck_required = true;
    observation.top_contexts.push_back(std::move(context));
  }
  observation.affected_scopes = {"listener", "engine", "query_arena"};
  return memory::PlanMemoryPressureResponse(policy, observation, reserve);
}

memory::MemoryPressureActionExecutorSet BoundExecutors(
    std::map<memory::MemoryPressureActionKind, int>* counters) {
  auto executor =
      [counters](const memory::MemoryPressureDecision& decision,
                 memory::MemoryPressureActionKind action) {
        Require(decision.new_state ==
                    memory::MemoryPressureState::emergency_pressure,
                "ELER-050 executor received non-emergency decision");
        ++(*counters)[action];
        return memory::MemoryPressureActionExecutorOk(
            action,
            std::string("eler050.") +
                memory::MemoryPressureActionKindName(action),
            {"eler050.executor.bound=true",
             std::string("eler050.executor.action=") +
                 memory::MemoryPressureActionKindName(action)});
      };
  memory::MemoryPressureActionExecutorSet executors;
  executors.admission_control = executor;
  executors.spill_preference = executor;
  executors.page_cache_shrink = executor;
  executors.background_cleanup = executor;
  executors.query_cancel = executor;
  executors.emergency_reserve_release = executor;
  executors.diagnostics = executor;
  executors.agent_suspend = executor;
  executors.adaptive_batch_reduction = executor;
  executors.forced_spill = executor;
  executors.forced_cancel = executor;
  return executors;
}

void ProvePressureDecisionAndExecutors() {
  memory::EmergencyMemoryReserve reserve(4096);
  const auto decision = EmergencyPressureDecision(&reserve);
  Require(decision.ok() && !decision.fail_closed,
          "ELER-050 emergency pressure decision failed");
  Require(decision.new_state == memory::MemoryPressureState::emergency_pressure,
          "ELER-050 pressure decision did not enter emergency state");
  Require(!decision.ordinary_admission_allowed,
          "ELER-050 emergency pressure allowed ordinary admission");
  Require(decision.emergency_reserve_released &&
              decision.emergency_reserve_released_bytes == 4096,
          "ELER-050 emergency reserve was not released for diagnostics");
  Require(decision.emergency_diagnostics.emitted &&
              decision.emergency_diagnostics.bounded &&
              decision.emergency_diagnostics.allocation_free_logger,
          "ELER-050 emergency diagnostics were not bounded/allocation-free");
  Require(decision.top_contexts.size() == 4,
          "ELER-050 pressure top contexts were not bounded");

  std::map<memory::MemoryPressureActionKind, int> counters;
  const auto executed =
      memory::ExecuteMemoryPressureDecision(decision, BoundExecutors(&counters));
  Require(executed.ok(), "ELER-050 bound pressure executors failed");
  Require(executed.executed_actions.size() == decision.actions.size(),
          "ELER-050 not every planned pressure action executed");
  for (const auto action : decision.actions) {
    if (action == memory::MemoryPressureActionKind::none) {
      continue;
    }
    Require(counters[action] == 1,
            std::string("ELER-050 executor count drift for ") +
                memory::MemoryPressureActionKindName(action));
  }

  auto missing_executors = BoundExecutors(&counters);
  missing_executors.forced_cancel = {};
  const auto missing =
      memory::ExecuteMemoryPressureDecision(decision, missing_executors);
  Require(!missing.ok() && missing.fail_closed &&
              missing.executed_actions.empty(),
          "ELER-050 missing pressure executor did not fail closed before side effects");

  memory::MemoryPressureObservation unsafe_observation;
  unsafe_observation.route_label = "engine.listener.memory.integrated";
  unsafe_observation.operation_id = "ELER-050-unsafe";
  unsafe_observation.current_bytes = 990;
  unsafe_observation.soft_limit_bytes = 700;
  unsafe_observation.hard_limit_bytes = 1000;
  unsafe_observation.parser_or_reference_authority = true;
  const auto unsafe = memory::PlanMemoryPressureResponse(
      memory::MemoryPressurePolicy{}, unsafe_observation);
  Require(!unsafe.ok() && unsafe.fail_closed,
          "ELER-050 unsafe memory pressure authority was admitted");

  AddRow("memory_pressure_executor_binding",
         "pass",
         "emergency_decision=true;ordinary_admission=false;all_actions_bound=true;missing_executor_preflight=true;unsafe_authority_refused=true");
}

platform::DiagnosticRecord CanaryDiagnostic() {
  platform::DiagnosticRecord diagnostic;
  diagnostic.status =
      {StatusCode::memory_limit_exceeded, Severity::error, Subsystem::memory};
  diagnostic.diagnostic_code = "ELER050_LOW_MEMORY_CANARY";
  diagnostic.message_key = "memory.low_memory_canary";
  diagnostic.source_component = "engine_listener_memory_integrated";
  diagnostic.remediation_hint = "collect redacted low-memory support bundle";
  diagnostic.arguments.push_back({"authorization_token",
                                  "ELER050-SECRET-token-value"});
  diagnostic.arguments.push_back({"password_material",
                                  "plain-password-ELER050"});
  return diagnostic;
}

memory::MemorySupportBundleRequest SupportBundleRequest(
    const memory::MemoryAccountingSnapshot& snapshot,
    const memory::ProtectedMemoryEvidence& protected_evidence,
    memory::MemorySupportBundleMode mode) {
  memory::MemorySupportBundleRequest request;
  request.bundle_profile = "engine_listener.memory.integrated.v1";
  request.redaction_profile = "engine_listener.memory.redacted.v1";
  request.mode = mode;
  request.limits.max_rows = 96;
  request.limits.max_output_bytes = 12ull * 1024ull;
  request.limits.max_key_bytes = 96;
  request.limits.max_value_bytes = 128;
  request.limits.max_diagnostic_arguments = 8;
  request.limits.max_metrics = 8;
  request.limits.max_pressure_transitions = 4;
  request.limits.max_allocation_classes = 4;
  request.snapshot = snapshot;
  request.diagnostics.push_back(CanaryDiagnostic());
  request.include_failure_reasons = true;
  request.include_metrics = false;
  request.allow_protected_material = false;
  request.redaction_before_buffering = true;
  request.exclude_protected_material = true;
  request.max_top_contexts = 8;
  request.max_top_categories = 8;
  for (int i = 0; i < 10; ++i) {
    memory::MemorySupportBundlePressureTransitionRow transition;
    transition.previous_state = i == 0 ? "normal" : "high_pressure";
    transition.new_state = "emergency_pressure";
    transition.trigger = "host_memory_pressure";
    transition.current_bytes = 900 + static_cast<u64>(i);
    transition.limit_bytes = 1000;
    transition.emergency = true;
    request.pressure_transitions.push_back(std::move(transition));
  }
  for (int i = 0; i < 8; ++i) {
    memory::MemorySupportBundleAllocationClassSnapshot allocation_class;
    allocation_class.memory_class = "eler050-class-" + std::to_string(i);
    allocation_class.current_bytes = static_cast<u64>(1024 - i * 64);
    allocation_class.peak_bytes = allocation_class.current_bytes + 128;
    allocation_class.allocation_count = 3;
    allocation_class.release_count = 2;
    allocation_class.failure_count = i == 0 ? 1 : 0;
    request.allocation_classes.push_back(std::move(allocation_class));
  }
  request.protected_memory_review.enabled = true;
  request.protected_memory_review.diagnostics_log_exception_scan_complete = true;
  request.protected_memory_review.support_bundle_scan_complete = true;
  request.protected_memory_review.zeroization_not_optimized_away = true;
  request.protected_memory_review.protected_buffer_zero_on_release = true;
  request.protected_memory_review.require_platform_protection_attempts = true;
  request.protected_memory_review.hsm_kms_plugin_routes_use_protected_buffers = true;
  request.protected_memory_review.require_protected_material_exclusion = true;
  request.protected_memory_review.protected_buffer_evidence.push_back(
      protected_evidence);
  memory::ProtectedSecretRouteEvidence route;
  route.source_kind = "auth_kms_route";
  route.route_id = "protected_reference:ELER050-KMS";
  route.routed_through_protected_buffer = true;
  route.protected_reference_only = true;
  route.plaintext_material_observed = false;
  request.protected_memory_review.secret_routes.push_back(std::move(route));
  return request;
}

void ProveLowMemorySupportBundle() {
  std::vector<void*> active;
  for (int i = 0; i < 6; ++i) {
    auto tag = Tag("support_bundle_top_context",
                   "bundle-context-" + std::to_string(i));
    if (i == 5) {
      tag.owner = "ELER050-SECRET-token-value";
      tag.context_id = "ELER050-SECRET-token-value";
      tag.database_id = "ELER050-SECRET-token-value";
      tag.session_id = "ELER050-SECRET-token-value";
      tag.transaction_id = "ELER050-SECRET-token-value";
      tag.statement_id = "ELER050-SECRET-token-value";
      tag.query_id = "ELER050-SECRET-token-value";
    }
    auto allocation = memory::DefaultMemoryManager().Allocate(
        256 + static_cast<memory::usize>(i * 64),
        alignof(std::max_align_t),
        std::move(tag));
    Require(allocation.ok(), "ELER-050 support-bundle allocation failed");
    active.push_back(allocation.pointer);
  }

  memory::ProtectedMemoryRequest protected_request;
  protected_request.bytes = 128;
  protected_request.alignment = alignof(std::max_align_t);
  protected_request.tag = Tag("protected_canary", "protected");
  protected_request.material_class = "kms_plaintext_ELER050";
  protected_request.platform_policy =
      memory::ProtectedMemoryPlatformPolicy::best_effort;
  auto protected_buffer =
      memory::DefaultMemoryManager().AllocateProtected(protected_request);
  Require(protected_buffer.ok(),
          "ELER-050 protected memory evidence allocation failed");

  const auto snapshot = memory::DefaultMemoryManager().Snapshot();
  auto request = SupportBundleRequest(snapshot,
                                      protected_buffer.evidence,
                                      memory::MemorySupportBundleMode::low_memory);
  const auto bundle = memory::BuildMemorySupportBundleEvidence(request);
  Require(bundle.ok(), "ELER-050 low-memory support bundle failed");
  Require(bundle.low_memory_mode && !bundle.emergency_summary_mode,
          "ELER-050 support bundle was not in low-memory mode");
  Require(bundle.rows.size() <= request.limits.max_rows &&
              bundle.output_bytes <= request.limits.max_output_bytes,
          "ELER-050 low-memory support bundle exceeded configured bounds");
  Require(bundle.top_context_count <= 4 &&
              bundle.pressure_transition_count <= 4 &&
              bundle.allocation_class_count <= 4,
          "ELER-050 low-memory support bundle did not bound row families");
  Require(bundle.redaction_before_buffering &&
              bundle.protected_material_excluded &&
              bundle.protected_memory_review_passed,
          "ELER-050 support bundle redaction/protected review failed");
  Require(bundle.redacted_row_count != 0,
          "ELER-050 support bundle did not redact canary rows");
  for (const auto& row : bundle.rows) {
    Require(!ContainsSensitiveCanary(row.value),
            "ELER-050 support bundle leaked a synthetic secret value");
  }

  request.mode = memory::MemorySupportBundleMode::emergency_summary;
  const auto emergency =
      memory::BuildMemorySupportBundleEvidence(std::move(request));
  Require(emergency.ok() && emergency.emergency_summary_mode &&
              emergency.emergency_summary.emitted &&
              emergency.emergency_summary.row_count <=
                  emergency.emergency_summary.max_rows,
          "ELER-050 emergency support summary was not bounded");
  for (u64 i = 0; i < emergency.emergency_summary.row_count; ++i) {
    const auto& row = emergency.emergency_summary.rows[static_cast<std::size_t>(i)];
    Require(!ContainsSensitiveCanary(row.value.data()),
            "ELER-050 emergency summary leaked a synthetic secret value");
  }

  const auto protected_release = protected_buffer.buffer.Reset();
  Require(protected_release.ok(),
          "ELER-050 protected buffer release failed");
  for (int i = 0; i < static_cast<int>(active.size()); ++i) {
    const auto release = memory::DefaultMemoryManager().Deallocate(
        active[static_cast<std::size_t>(i)],
        Tag("support_bundle_top_context_release",
            "bundle-context-" + std::to_string(i)));
    Require(release.ok(), "ELER-050 support-bundle allocation release failed");
  }
  Require(memory::DefaultMemoryManager().Snapshot().active_allocation_count == 0,
          "ELER-050 support-bundle proof leaked memory");

  AddRow("low_memory_support_bundle",
         "pass",
         "low_memory_bounded=true;emergency_summary_bounded=true;redaction_before_buffering=true;protected_canaries_redacted=true");
}

void ProveBoundedConcurrentPressureLoop() {
  constexpr int kThreadCount = 4;
  constexpr int kIterations = 24;
  std::atomic<int> failures{0};
  std::mutex failure_mutex;
  std::vector<std::string> failure_messages;

  auto record_failure = [&](std::string message) {
    failures.fetch_add(1);
    std::lock_guard<std::mutex> lock(failure_mutex);
    failure_messages.push_back(std::move(message));
  };

  auto worker = [&](int worker_id) {
    for (int iteration = 0; iteration < kIterations; ++iteration) {
      const std::string suffix = std::to_string(worker_id) + "-" +
                                 std::to_string(iteration);
      auto allocation = memory::DefaultMemoryManager().Allocate(
          128,
          alignof(std::max_align_t),
          Tag("concurrent_pressure_loop", "concurrent-" + suffix));
      if (!allocation.ok()) {
        record_failure("allocation_failed:" + suffix);
        continue;
      }
      memory::MemoryPressureObservation observation;
      observation.route_label = "engine.listener.memory.concurrent";
      observation.operation_id = "ELER-050-concurrent-" + suffix;
      observation.current_bytes = 760;
      observation.soft_limit_bytes = 700;
      observation.hard_limit_bytes = 1000;
      observation.spill_supported = true;
      observation.engine_mga_authoritative = true;
      observation.mga_recheck_preserved = true;
      observation.security_recheck_preserved = true;
      const auto decision = memory::PlanMemoryPressureResponse(
          memory::MemoryPressurePolicy{}, observation);
      if (!decision.ok() ||
          decision.new_state != memory::MemoryPressureState::soft_pressure ||
          !decision.HasAction(memory::MemoryPressureActionKind::throttle) ||
          !decision.HasAction(memory::MemoryPressureActionKind::prefer_spill)) {
        record_failure("pressure_decision_failed:" + suffix);
      }
      const auto release = memory::DefaultMemoryManager().Deallocate(
          allocation.pointer,
          Tag("concurrent_pressure_loop_release", "concurrent-" + suffix));
      if (!release.ok()) {
        record_failure("release_failed:" + suffix);
      }
    }
  };

  std::vector<std::thread> threads;
  for (int i = 0; i < kThreadCount; ++i) {
    threads.emplace_back(worker, i);
  }
  for (auto& thread : threads) {
    thread.join();
  }

  if (failures.load() != 0) {
    std::string combined;
    for (const auto& message : failure_messages) {
      combined += message + ";";
    }
    Fail("ELER-050 concurrent pressure loop failed: " + combined);
  }
  const auto snapshot = memory::DefaultMemoryManager().Snapshot();
  Require(snapshot.active_allocation_count == 0 &&
              snapshot.leak_candidate_count == 0,
          "ELER-050 concurrent pressure loop leaked allocations");
  Require(snapshot.peak_bytes >= 128,
          "ELER-050 concurrent pressure loop did not exercise allocator");
  AddRow("bounded_concurrent_pressure_loop",
         "pass",
         "threads=4;iterations_per_thread=24;soft_pressure_planned=true;allocator_leaks=0");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr
        << "usage: engine_listener_memory_integrated_conformance <matrix-csv>\n";
    return EXIT_FAILURE;
  }

  const std::filesystem::path matrix_path = argv[1];
  const std::filesystem::path temp_root =
      matrix_path.parent_path() / "ELER_050_MEMORY_INTEGRATED_TMP";
  std::filesystem::remove_all(temp_root);
  std::filesystem::create_directories(temp_root);

  ConfigureProductionDefaultManager();
  ProveQueryReservationIntegration(temp_root);
  ProvePressureDecisionAndExecutors();
  ProveLowMemorySupportBundle();
  ProveBoundedConcurrentPressureLoop();
  WriteMatrix(matrix_path);

  std::filesystem::remove_all(temp_root);
  return EXIT_SUCCESS;
}
