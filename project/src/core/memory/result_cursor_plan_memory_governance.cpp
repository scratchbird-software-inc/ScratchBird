// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// CEIC-020: result cursor, plan-cache, and prepared-statement memory governance.
#include "result_cursor_plan_memory_governance.hpp"
#include "../uuid/uuid.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>

namespace scratchbird::core::memory {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

constexpr const char* kEvidenceAnchor =
    "CEIC-020_RESULT_CURSOR_PLAN_CACHE_PREPARED_MEMORY_GOVERNANCE";
constexpr const char* kAuthorityScope =
    "ceic_020.memory_evidence_only_not_transaction_finality_row_visibility_security_authorization_recovery_parser_reference_wal_benchmark_optimizer_plan_index_finality_or_agent_action_authority";

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::memory};
}

Status ErrorStatus(StatusCode code = StatusCode::memory_invalid_request) {
  return {code, Severity::error, Subsystem::memory};
}

ResultCursorPlanMemoryDecision RequestFailure(StatusCode code,
    ResultCursorPlanMemoryReleaseReason reason = ResultCursorPlanMemoryReleaseReason::explicit_release) {
  ResultCursorPlanMemoryDecision result;
  result.status = ErrorStatus(code);
  result.diagnostic.status = result.status;
  result.release_reason = reason;
  return result;
}

ResultCursorPlanMemoryDecision AllocationFailure(
    ResultCursorPlanMemoryReleaseReason reason = ResultCursorPlanMemoryReleaseReason::explicit_release) {
  return RequestFailure(StatusCode::memory_allocation_failed, reason);
}

class PendingReservation {
 public:
  PendingReservation(HierarchicalMemoryBudgetLedger& ledger, HierarchicalMemoryReservationToken token)
      : ledger_(ledger), token_(token) {}
  PendingReservation(const PendingReservation&) = delete;
  PendingReservation& operator=(const PendingReservation&) = delete;
  ~PendingReservation() { if (token_.valid()) (void)ledger_.ReleaseNoAlloc(token_); }
  void Publish() noexcept { token_ = {}; }
 private:
  HierarchicalMemoryBudgetLedger& ledger_;
  HierarchicalMemoryReservationToken token_;
};

std::string BoolText(bool value) {
  return value ? "true" : "false";
}

bool Blank(const std::string& value) {
  return value.find_first_not_of(" \t\r\n") == std::string::npos;
}

void AddEvidence(std::vector<std::string>* evidence, std::string value) {
  evidence->push_back(std::move(value));
}

void AddBoolEvidence(std::vector<std::string>* evidence,
                     const std::string& key,
                     bool value) {
  AddEvidence(evidence, key + "=" + BoolText(value));
}

void AppendScopeEvidence(std::vector<std::string>* evidence,
                         const ResultCursorPlanMemoryScope&) {
  AddEvidence(evidence, "ceic_020.scope_identity_representation=binary16");
  AddEvidence(evidence, "ceic_020.scope_identity_authority=typed_lease_record");
}

void AppendEpochEvidence(std::vector<std::string>* evidence,
                         const ResultCursorPlanMemoryEpochs& epochs) {
  AddEvidence(evidence, "ceic_020.catalog_epoch=" +
                            std::to_string(epochs.catalog_epoch));
  AddEvidence(evidence, "ceic_020.security_epoch=" +
                            std::to_string(epochs.security_epoch));
  AddEvidence(evidence, "ceic_020.redaction_epoch=" +
                            std::to_string(epochs.redaction_epoch));
  AddEvidence(evidence, "ceic_020.policy_epoch=" +
                            std::to_string(epochs.policy_epoch));
  AddEvidence(evidence, "ceic_020.resource_epoch=" +
                            std::to_string(epochs.resource_epoch));
  AddEvidence(evidence, "ceic_020.descriptor_epoch=" +
                            std::to_string(epochs.descriptor_epoch));
  AddEvidence(evidence, "ceic_020.memory_policy_epoch=" +
                            std::to_string(epochs.memory_policy_epoch));
}

DiagnosticRecord MakeGovernanceDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::vector<DiagnosticArgument> arguments = {}) {
  arguments.push_back({"authority_scope", kAuthorityScope});
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "core.memory.result_cursor_plan_governance",
                        "Route cursor, result-frame, plan-cache, prepared-statement, and descriptor memory through CEIC-011 reservations.");
}

ResultCursorPlanMemoryDecision BaseDecision(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    const ResultCursorPlanMemoryLeaseRequest& request,
    std::vector<DiagnosticArgument> arguments = {}) {
  ResultCursorPlanMemoryDecision decision;
  decision.status = status;
  decision.diagnostic = MakeGovernanceDiagnostic(
      status, std::move(diagnostic_code), std::move(message_key),
      std::move(arguments));
  AddEvidence(&decision.evidence, kEvidenceAnchor);
  AddEvidence(&decision.evidence, kAuthorityScope);
  AddEvidence(&decision.evidence,
              "ceic_020.surface=" +
                  std::string(ResultCursorPlanMemorySurfaceName(request.surface)));
  AddEvidence(&decision.evidence,
              "ceic_020.route_label=" + request.route_label);
  AddEvidence(&decision.evidence,
              "ceic_020.memory_class=" + request.memory_class);
  AddEvidence(&decision.evidence,
              "ceic_020.owner_identity=typed_binary_uuid");
  AddEvidence(&decision.evidence,
              "ceic_020.requested_bytes=" +
                  std::to_string(request.requested_bytes));
  AddBoolEvidence(&decision.evidence,
                  "ceic_020.cluster_route_requested",
                  request.cluster_route_requested);
  AppendScopeEvidence(&decision.evidence, request.scope);
  AppendEpochEvidence(&decision.evidence, request.epochs);
  AddBoolEvidence(&decision.evidence,
                  "ceic_020.engine_mga_authoritative",
                  request.authority.engine_mga_authoritative);
  AddBoolEvidence(&decision.evidence,
                  "ceic_020.transaction_inventory_authoritative",
                  request.authority.transaction_inventory_authoritative);
  AddBoolEvidence(&decision.evidence,
                  "ceic_020.security_or_policy_checked",
                  request.authority.security_or_policy_checked);
  AddBoolEvidence(&decision.evidence,
                  "ceic_020.memory_evidence_only",
                  request.authority.memory_evidence_only);
  return decision;
}

ResultCursorPlanMemoryDecision Refuse(
    const ResultCursorPlanMemoryLeaseRequest& request,
    std::string diagnostic_code,
    std::string message_key,
    std::string reason,
    StatusCode code = StatusCode::memory_invalid_request) {
  auto decision = BaseDecision(ErrorStatus(code),
                               std::move(diagnostic_code),
                               std::move(message_key),
                               request,
                               {{"reason", reason}});
  decision.fail_closed = true;
  decision.accepted = false;
  AddEvidence(&decision.evidence, "ceic_020.fail_closed=true");
  AddEvidence(&decision.evidence, "ceic_020.refusal_reason=" + reason);
  return decision;
}

ResultCursorPlanMemoryDecision ReleaseDecision(
    Status status,
    ResultCursorPlanMemoryReleaseReason reason,
    std::string diagnostic_code,
    std::string detail) {
  ResultCursorPlanMemoryDecision decision;
  decision.status = status;
  decision.fail_closed = !status.ok();
  decision.accepted = status.ok();
  decision.release_reason = reason;
  decision.diagnostic = MakeGovernanceDiagnostic(
      status,
      std::move(diagnostic_code),
      status.ok() ? "memory.ceic_020.release.ok"
                  : "memory.ceic_020.release.refused",
      {{"reason", ResultCursorPlanMemoryReleaseReasonName(reason)},
       {"detail", std::move(detail)}});
  AddEvidence(&decision.evidence, kEvidenceAnchor);
  AddEvidence(&decision.evidence, kAuthorityScope);
  AddEvidence(&decision.evidence,
              "ceic_020.release_reason=" +
                  std::string(ResultCursorPlanMemoryReleaseReasonName(reason)));
  AddBoolEvidence(&decision.evidence,
                  "ceic_020.release_fail_closed",
                  decision.fail_closed);
  return decision;
}

bool UnsafeAuthority(const ResultCursorPlanMemoryAuthority& authority,
                     std::string* reason) {
  if (!authority.engine_mga_authoritative ||
      !authority.transaction_inventory_authoritative ||
      !authority.security_or_policy_checked ||
      !authority.memory_evidence_only) {
    *reason = "engine_mga_transaction_inventory_security_policy_memory_evidence_required";
    return true;
  }
  if (authority.parser_authority || authority.reference_authority ||
      authority.transaction_finality_authority || authority.visibility_authority ||
      authority.recovery_authority || authority.authorization_authority ||
      authority.wal_authority || authority.benchmark_authority ||
      authority.optimizer_plan_authority || authority.index_finality_authority ||
      authority.agent_action_authority || authority.cluster_authority ||
      authority.debug_or_relaxed_path) {
    *reason = "unsafe_authority_claim_refused";
    return true;
  }
  return false;
}

bool EpochsPresent(const ResultCursorPlanMemoryEpochs& epochs) {
  return epochs.catalog_epoch != 0 && epochs.security_epoch != 0 &&
         epochs.redaction_epoch != 0 && epochs.policy_epoch != 0 &&
         epochs.resource_epoch != 0 && epochs.descriptor_epoch != 0 &&
         epochs.memory_policy_epoch != 0;
}

bool EpochRecordStale(const ResultCursorPlanMemoryEpochs& record,
                      const ResultCursorPlanMemoryEpochs& current) {
  return (current.catalog_epoch != 0 && record.catalog_epoch != current.catalog_epoch) ||
         (current.security_epoch != 0 && record.security_epoch != current.security_epoch) ||
         (current.redaction_epoch != 0 && record.redaction_epoch != current.redaction_epoch) ||
         (current.policy_epoch != 0 && record.policy_epoch != current.policy_epoch) ||
         (current.resource_epoch != 0 && record.resource_epoch != current.resource_epoch) ||
         (current.descriptor_epoch != 0 && record.descriptor_epoch != current.descriptor_epoch) ||
         (current.memory_policy_epoch != 0 &&
          record.memory_policy_epoch != current.memory_policy_epoch);
}

bool IsPlanPreparedOrDescriptor(ResultCursorPlanMemorySurface surface) {
  return surface == ResultCursorPlanMemorySurface::plan_cache_entry ||
         surface == ResultCursorPlanMemorySurface::prepared_statement ||
         surface == ResultCursorPlanMemorySurface::descriptor_snapshot;
}

bool IsCursorOrResult(ResultCursorPlanMemorySurface surface) {
  return surface == ResultCursorPlanMemorySurface::streaming_result ||
         surface == ResultCursorPlanMemorySurface::cursor ||
         surface == ResultCursorPlanMemorySurface::result_frame;
}

void AddScopeIfPresent(std::vector<HierarchicalMemoryScopeRef>* chain,
                       HierarchicalMemoryScopeKind kind,
                       const ResultCursorPlanMemoryUuid& value) {
  if (!value.is_nil()) {
    chain->push_back({kind, {}, value.bytes});
  }
}

u64 LimitForDimension(ResultCursorPlanMemorySurface surface,
                      const ResultCursorPlanMemoryPolicy& policy,
                      const std::string& dimension) {
  if (surface == ResultCursorPlanMemorySurface::plan_cache_entry) {
    if (dimension == "database") return policy.max_plan_cache_bytes_per_database;
    if (dimension == "tenant") return policy.max_plan_cache_bytes_per_tenant;
    if (dimension == "user") return policy.max_plan_cache_bytes_per_user;
    if (dimension == "session") return policy.max_plan_cache_bytes_per_session;
  }
  if (surface == ResultCursorPlanMemorySurface::prepared_statement) {
    if (dimension == "database") return policy.max_prepared_statement_bytes_per_database;
    if (dimension == "tenant") return policy.max_prepared_statement_bytes_per_tenant;
    if (dimension == "user") return policy.max_prepared_statement_bytes_per_user;
    if (dimension == "session") return policy.max_prepared_statement_bytes_per_session;
  }
  if (surface == ResultCursorPlanMemorySurface::descriptor_snapshot) {
    if (dimension == "database") return policy.max_descriptor_snapshot_bytes_per_database;
    if (dimension == "session") return policy.max_descriptor_snapshot_bytes_per_session;
  }
  return 0;
}

}  // namespace

const char* ResultCursorPlanMemorySurfaceName(
    ResultCursorPlanMemorySurface surface) {
  switch (surface) {
    case ResultCursorPlanMemorySurface::streaming_result:
      return "streaming_result";
    case ResultCursorPlanMemorySurface::cursor:
      return "cursor";
    case ResultCursorPlanMemorySurface::result_frame:
      return "result_frame";
    case ResultCursorPlanMemorySurface::plan_cache_entry:
      return "plan_cache_entry";
    case ResultCursorPlanMemorySurface::prepared_statement:
      return "prepared_statement";
    case ResultCursorPlanMemorySurface::descriptor_snapshot:
      return "descriptor_snapshot";
  }
  return "unknown";
}

const char* ResultCursorPlanMemoryReleaseReasonName(
    ResultCursorPlanMemoryReleaseReason reason) {
  switch (reason) {
    case ResultCursorPlanMemoryReleaseReason::close:
      return "close";
    case ResultCursorPlanMemoryReleaseReason::cancel:
      return "cancel";
    case ResultCursorPlanMemoryReleaseReason::timeout:
      return "timeout";
    case ResultCursorPlanMemoryReleaseReason::disconnect:
      return "disconnect";
    case ResultCursorPlanMemoryReleaseReason::rollback:
      return "rollback";
    case ResultCursorPlanMemoryReleaseReason::epoch_invalidation:
      return "epoch_invalidation";
    case ResultCursorPlanMemoryReleaseReason::eviction:
      return "eviction";
    case ResultCursorPlanMemoryReleaseReason::shrink:
      return "shrink";
    case ResultCursorPlanMemoryReleaseReason::pressure_forced_close:
      return "pressure_forced_close";
    case ResultCursorPlanMemoryReleaseReason::expired_lease:
      return "expired_lease";
    case ResultCursorPlanMemoryReleaseReason::explicit_release:
      return "explicit_release";
  }
  return "unknown";
}

std::vector<HierarchicalMemoryScopeRef>
ResultCursorPlanMemoryGovernor::BuildScopeChain(
    const ResultCursorPlanMemoryLeaseRequest& request) const {
  std::vector<HierarchicalMemoryScopeRef> chain;
  AddScopeIfPresent(&chain, HierarchicalMemoryScopeKind::process, request.scope.process_id);
  AddScopeIfPresent(&chain, HierarchicalMemoryScopeKind::database,
                    request.scope.database_id);
  AddScopeIfPresent(&chain, HierarchicalMemoryScopeKind::tenant,
                    request.scope.tenant_id);
  AddScopeIfPresent(&chain, HierarchicalMemoryScopeKind::user,
                    request.scope.user_id);
  AddScopeIfPresent(&chain, HierarchicalMemoryScopeKind::role,
                    request.scope.role_id);
  AddScopeIfPresent(&chain, HierarchicalMemoryScopeKind::session,
                    request.scope.session_id);
  AddScopeIfPresent(&chain, HierarchicalMemoryScopeKind::transaction,
                    request.scope.transaction_id);
  AddScopeIfPresent(&chain, HierarchicalMemoryScopeKind::statement,
                    request.scope.statement_id);
  AddScopeIfPresent(&chain, HierarchicalMemoryScopeKind::query,
                    request.scope.query_id);
  AddScopeIfPresent(&chain, HierarchicalMemoryScopeKind::connection, request.scope.connection_id);
  AddScopeIfPresent(&chain, HierarchicalMemoryScopeKind::cursor, request.scope.cursor_id);
  AddScopeIfPresent(&chain, HierarchicalMemoryScopeKind::plan_cache_entry, request.scope.plan_cache_entry_id);
  AddScopeIfPresent(&chain, HierarchicalMemoryScopeKind::prepared_statement, request.scope.prepared_statement_id);
  AddScopeIfPresent(&chain, HierarchicalMemoryScopeKind::descriptor_snapshot, request.scope.descriptor_snapshot_id);
  return chain;
}

ResultCursorPlanMemoryGovernor::Counter
ResultCursorPlanMemoryGovernor::CounterForLocked(
    ResultCursorPlanMemorySurface surface,
    const std::string& dimension,
    const ResultCursorPlanMemoryUuid& value) const {
  const auto found = counters_.find({surface, dimension, value});
  return found == counters_.end() ? Counter{} : found->second;
}

ResultCursorPlanMemoryGovernor::~ResultCursorPlanMemoryGovernor() {
  // No concurrent callers may outlive the governor. The ledger lifetime is a
  // construction/owner obligation, just as for an allocator-backed container.
  for (const auto& [_, record] : leases_)
    (void)record.ledger->ReleaseNoAlloc(record.token);
}

bool ResultCursorPlanMemoryGovernor::PrepareCountersLocked(OwnedLease& record) {
  auto add_dim = [&](const std::string& dimension, const ResultCursorPlanMemoryUuid& value) {
    if (value.is_nil()) return;
    record.counter_keys.push_back({record.surface, dimension, value});
  };
  add_dim("database", record.scope.database_id);
  add_dim("tenant", record.scope.tenant_id);
  add_dim("user", record.scope.user_id);
  add_dim("session", record.scope.session_id);
  add_dim("connection", record.scope.connection_id);
  add_dim("transaction", record.scope.transaction_id);
  add_dim("statement", record.scope.statement_id);
  add_dim("query", record.scope.query_id);
  add_dim("cursor", record.scope.cursor_id);
  constexpr auto max = std::numeric_limits<u64>::max();
  if (record.reserved_bytes > max - active_bytes_) return false;
  for (const auto& key : record.counter_keys) {
    const auto& counter = counters_.try_emplace(key).first->second;
    if (record.reserved_bytes > max - counter.bytes ||
        counter.count == max || (record.frame_lease && counter.frames == max)) return false;
  }
  return true;
}

void ResultCursorPlanMemoryGovernor::AddCountersLocked(const OwnedLease& record) {
  for (const auto& key : record.counter_keys) {
    auto& counter = counters_.at(key);
    counter.bytes += record.reserved_bytes;
    ++counter.count;
    if (record.frame_lease) ++counter.frames;
  }
  active_bytes_ += record.reserved_bytes;
}

void ResultCursorPlanMemoryGovernor::RemoveCountersLocked(const OwnedLease& record) {
  for (const auto& key : record.counter_keys) {
    const auto found = counters_.find(key);
    auto& counter = found->second;
    counter.bytes -= record.reserved_bytes;
    --counter.count;
    if (record.frame_lease) --counter.frames;
    if (counter.count == 0) counters_.erase(found);
  }
  active_bytes_ -= record.reserved_bytes;
}

ResultCursorPlanMemoryDecision ResultCursorPlanMemoryGovernor::Acquire(
    ResultCursorPlanMemoryLeaseRequest request) try {
  if (static_cast<int>(request.surface) < static_cast<int>(ResultCursorPlanMemorySurface::streaming_result) ||
      static_cast<int>(request.surface) > static_cast<int>(ResultCursorPlanMemorySurface::descriptor_snapshot)) {
    return Refuse(request, "SB_CEIC_020_MEMORY_GOVERNANCE.RESERVATION_REFUSED",
                  "memory.ceic_020.reservation_refused", "memory_surface_invalid");
  }
  if (request.ledger == nullptr || !request.policy.require_ledger_reservation) {
    return Refuse(request,
                  "SB_CEIC_020_MEMORY_GOVERNANCE.LEDGER_REQUIRED",
                  "memory.ceic_020.ledger_required",
                  "hierarchical_memory_budget_ledger_required");
  }
  if (request.policy.cluster_surfaces_external_only &&
      request.cluster_route_requested) {
    return Refuse(request,
                  "SB_CEIC_020_MEMORY_GOVERNANCE.CLUSTER_EXTERNAL_ONLY",
                  "memory.ceic_020.cluster_external_only",
                  "cluster_result_cursor_plan_cache_memory_external_provider_required");
  }
  if (request.requested_bytes == 0) {
    return Refuse(request,
                  "SB_CEIC_020_MEMORY_GOVERNANCE.ZERO_BYTES",
                  "memory.ceic_020.zero_bytes",
                  "requested_bytes_required");
  }
  if (!uuid::IsEngineIdentityUuid(request.owner_id) ||
      !uuid::IsEngineIdentityUuid(request.scope.process_id)) {
    return Refuse(request, "SB_CEIC_020_MEMORY_GOVERNANCE.SCOPE_REQUIRED",
                  "memory.ceic_020.scope_required", "binary_process_and_owner_identity_required");
  }
  for (const auto& identity : {request.scope.database_id, request.scope.tenant_id,
       request.scope.user_id, request.scope.role_id, request.scope.session_id,
       request.scope.connection_id, request.scope.transaction_id,
       request.scope.statement_id, request.scope.query_id, request.scope.cursor_id,
       request.scope.plan_cache_entry_id, request.scope.prepared_statement_id,
       request.scope.descriptor_snapshot_id}) {
    if (!identity.is_nil() && !uuid::IsEngineIdentityUuid(identity)) {
      return Refuse(request, "SB_CEIC_020_MEMORY_GOVERNANCE.SCOPE_REQUIRED",
                    "memory.ceic_020.scope_required", "invalid_binary_scope_identity");
    }
  }
  if (Blank(request.route_label)) {
    request.route_label =
        std::string("ceic020.") + ResultCursorPlanMemorySurfaceName(request.surface);
  }
  if (Blank(request.memory_class)) {
    request.memory_class =
        std::string("ceic_020.") +
        ResultCursorPlanMemorySurfaceName(request.surface);
  }
  std::string unsafe_reason;
  if (UnsafeAuthority(request.authority, &unsafe_reason)) {
    return Refuse(request,
                  "SB_CEIC_020_MEMORY_GOVERNANCE.UNSAFE_AUTHORITY",
                  "memory.ceic_020.unsafe_authority",
                  unsafe_reason);
  }
  if (!request.policy.require_epoch_evidence || !EpochsPresent(request.epochs)) {
    return Refuse(request,
                  "SB_CEIC_020_MEMORY_GOVERNANCE.EPOCH_EVIDENCE_REQUIRED",
                  "memory.ceic_020.epoch_evidence_required",
                  "catalog_security_redaction_policy_resource_descriptor_memory_epochs_required");
  }
  if (request.scope.database_id.is_nil() ||
      request.scope.session_id.is_nil()) {
    return Refuse(request,
                  "SB_CEIC_020_MEMORY_GOVERNANCE.SCOPE_REQUIRED",
                  "memory.ceic_020.scope_required",
                  "database_and_session_scope_required");
  }
  if (IsCursorOrResult(request.surface) &&
      (request.scope.connection_id.is_nil() || request.scope.query_id.is_nil())) {
    return Refuse(request,
                  "SB_CEIC_020_MEMORY_GOVERNANCE.CURSOR_SCOPE_REQUIRED",
                  "memory.ceic_020.cursor_scope_required",
                  "connection_and_query_scope_required_for_cursor_result_memory");
  }
  if (request.surface == ResultCursorPlanMemorySurface::cursor &&
      request.scope.cursor_id.is_nil()) {
    return Refuse(request,
                  "SB_CEIC_020_MEMORY_GOVERNANCE.CURSOR_ID_REQUIRED",
                  "memory.ceic_020.cursor_id_required",
                  "cursor_id_required");
  }
  if (request.surface == ResultCursorPlanMemorySurface::result_frame &&
      request.scope.cursor_id.is_nil()) {
    return Refuse(request,
                  "SB_CEIC_020_MEMORY_GOVERNANCE.FRAME_CURSOR_REQUIRED",
                  "memory.ceic_020.frame_cursor_required",
                  "result_frame_cursor_id_required");
  }
  if (request.surface == ResultCursorPlanMemorySurface::plan_cache_entry &&
      request.scope.plan_cache_entry_id.is_nil()) {
    return Refuse(request,
                  "SB_CEIC_020_MEMORY_GOVERNANCE.PLAN_CACHE_KEY_REQUIRED",
                  "memory.ceic_020.plan_cache_key_required",
                  "binary_plan_cache_entry_identity_required");
  }
  if (request.surface == ResultCursorPlanMemorySurface::prepared_statement &&
      request.scope.prepared_statement_id.is_nil()) {
    return Refuse(request,
                  "SB_CEIC_020_MEMORY_GOVERNANCE.PREPARED_ID_REQUIRED",
                  "memory.ceic_020.prepared_id_required",
                  "prepared_statement_id_required");
  }
  if (request.surface == ResultCursorPlanMemorySurface::descriptor_snapshot &&
      request.scope.descriptor_snapshot_id.is_nil()) {
    return Refuse(request,
                  "SB_CEIC_020_MEMORY_GOVERNANCE.DESCRIPTOR_ID_REQUIRED",
                  "memory.ceic_020.descriptor_id_required",
                  "descriptor_snapshot_id_required");
  }

  std::lock_guard<std::mutex> lock(mutex_);
  {
    if (request.surface == ResultCursorPlanMemorySurface::result_frame) {
      if (request.requested_bytes > request.policy.max_result_frame_bytes) {
        ++backpressure_count_;
        auto decision = Refuse(
            request,
            "SB_CEIC_020_MEMORY_GOVERNANCE.RESULT_FRAME_TOO_LARGE",
            "memory.ceic_020.result_frame_too_large",
            "max_result_frame_bytes_exceeded",
            StatusCode::memory_limit_exceeded);
        decision.backpressure_required = true;
        return decision;
      }
      auto check_frames = [&](const std::string& dimension,
                              const ResultCursorPlanMemoryUuid& value,
                              u64 limit) -> std::optional<ResultCursorPlanMemoryDecision> {
        if (value.is_nil() || limit == 0) return std::nullopt;
        const auto counter = CounterForLocked(request.surface, dimension, value);
        if (counter.frames >= limit) {
          ++backpressure_count_;
          auto decision = Refuse(
              request,
              "SB_CEIC_020_MEMORY_GOVERNANCE.RESULT_FRAME_BACKPRESSURE",
              "memory.ceic_020.result_frame_backpressure",
              "max_outstanding_frames_" + dimension + "_exceeded",
              StatusCode::memory_limit_exceeded);
          decision.backpressure_required = true;
          return decision;
        }
        return std::nullopt;
      };
      if (auto decision = check_frames("connection",
                                       request.scope.connection_id,
                                       request.policy.max_outstanding_frames_per_connection)) {
        return *decision;
      }
      if (auto decision = check_frames("session",
                                       request.scope.session_id,
                                       request.policy.max_outstanding_frames_per_session)) {
        return *decision;
      }
      if (auto decision = check_frames("query",
                                       request.scope.query_id,
                                       request.policy.max_outstanding_frames_per_query)) {
        return *decision;
      }
      if (auto decision = check_frames("cursor",
                                       request.scope.cursor_id,
                                       request.policy.max_outstanding_frames_per_cursor)) {
        return *decision;
      }
    }

    if (request.surface == ResultCursorPlanMemorySurface::cursor ||
        request.surface == ResultCursorPlanMemorySurface::streaming_result) {
      auto check_bytes = [&](const std::string& dimension,
                             const ResultCursorPlanMemoryUuid& value,
                             u64 limit) -> std::optional<ResultCursorPlanMemoryDecision> {
        if (value.is_nil() || limit == 0) return std::nullopt;
        const auto counter = CounterForLocked(request.surface, dimension, value);
        if (counter.bytes > limit || request.requested_bytes > limit - counter.bytes) {
          auto decision = Refuse(
              request,
              "SB_CEIC_020_MEMORY_GOVERNANCE.CURSOR_BYTES_LIMIT",
              "memory.ceic_020.cursor_bytes_limit",
              "max_cursor_bytes_" + dimension + "_exceeded",
              StatusCode::memory_limit_exceeded);
          decision.backpressure_required = true;
          return decision;
        }
        return std::nullopt;
      };
      if (auto decision = check_bytes("connection",
                                      request.scope.connection_id,
                                      request.policy.max_cursor_bytes_per_connection)) {
        return *decision;
      }
      if (auto decision = check_bytes("session",
                                      request.scope.session_id,
                                      request.policy.max_cursor_bytes_per_session)) {
        return *decision;
      }
      if (auto decision = check_bytes("query",
                                      request.scope.query_id,
                                      request.policy.max_cursor_bytes_per_query)) {
        return *decision;
      }
    }

    if (IsPlanPreparedOrDescriptor(request.surface)) {
      auto check_bytes = [&](const std::string& dimension,
                             const ResultCursorPlanMemoryUuid& value) -> std::optional<ResultCursorPlanMemoryDecision> {
        if (value.is_nil()) return std::nullopt;
        const u64 limit = LimitForDimension(request.surface, request.policy, dimension);
        if (limit == 0) return std::nullopt;
        const auto counter = CounterForLocked(request.surface, dimension, value);
        if (counter.bytes > limit || request.requested_bytes > limit - counter.bytes) {
          auto decision = Refuse(
              request,
              "SB_CEIC_020_MEMORY_GOVERNANCE.CACHE_BYTES_LIMIT",
              "memory.ceic_020.cache_bytes_limit",
              "max_" +
                  std::string(ResultCursorPlanMemorySurfaceName(request.surface)) +
                  "_bytes_" + dimension + "_exceeded",
              StatusCode::memory_limit_exceeded);
          decision.backpressure_required = true;
          return decision;
        }
        return std::nullopt;
      };
      if (auto decision = check_bytes("database", request.scope.database_id)) return *decision;
      if (auto decision = check_bytes("tenant", request.scope.tenant_id)) return *decision;
      if (auto decision = check_bytes("user", request.scope.user_id)) return *decision;
      if (auto decision = check_bytes("session", request.scope.session_id)) return *decision;
    }
  }

  HierarchicalMemoryReservationRequest reservation;
  reservation.scope_chain = BuildScopeChain(request);
  reservation.category = request.category;
  reservation.memory_class = request.memory_class;
  reservation.requested_bytes = request.requested_bytes;
  reservation.binary_owner_uuid = request.owner_id.bytes;
  reservation.spillable = request.spillable;
  reservation.cancelable = request.cancelable;
  reservation.priority = request.priority;
  reservation.weight = request.weight == 0 ? 1 : request.weight;
  reservation.lease_expires_at_ms = request.lease_expires_at_ms;
  reservation.provenance = request.provenance;

  auto reserved = request.ledger->Reserve(std::move(reservation));
  if (!reserved.ok()) {
    auto decision = Refuse(request,
                           "SB_CEIC_020_MEMORY_GOVERNANCE.RESERVATION_REFUSED",
                           "memory.ceic_020.reservation_refused",
                           reserved.diagnostic.diagnostic_code.empty()
                               ? "reservation_refused"
                               : reserved.diagnostic.diagnostic_code,
                           reserved.status.code);
    decision.diagnostic = reserved.diagnostic;
    return decision;
  }
  PendingReservation pending(*request.ledger, reserved.token);
  if (next_sequence_ == 0) {
    return Refuse(request, "SB_CEIC_020_MEMORY_GOVERNANCE.RESERVATION_REFUSED",
                  "memory.ceic_020.reservation_refused", "lease_identity_exhausted",
                  StatusCode::memory_limit_exceeded);
  }
  const u64 sequence = next_sequence_;
  next_sequence_ = sequence == std::numeric_limits<u64>::max() ? 0 : sequence + 1;
  OwnedLease record;
  const auto lease_identity = uuid::IssueRuntimeIdentityV7();
  if (!lease_identity) {
    return Refuse(request, "SB_CEIC_020_MEMORY_GOVERNANCE.RESERVATION_REFUSED",
                  "memory.ceic_020.reservation_refused", "lease_identity_issuance_failed",
                  StatusCode::memory_allocation_failed);
  }
  record.lease_id = *lease_identity;
  record.surface = request.surface;
  record.scope = request.scope;
  record.epochs = request.epochs;
  record.ledger = request.ledger;
  record.token = reserved.token;
  record.reserved_bytes = request.requested_bytes;
  record.acquired_sequence = sequence;
  record.memory_class = request.memory_class;
  record.owner_id = request.owner_id;
  record.route_label = request.route_label;
  record.lease_expires_at_ms = request.lease_expires_at_ms;
  record.active = true;
  record.frame_lease = request.surface == ResultCursorPlanMemorySurface::result_frame;
  // Failed admission must not accumulate zero-count map nodes for new UUIDs.
  // This guard also covers a partially completed PrepareCountersLocked.
  static_assert(std::is_nothrow_move_constructible_v<OwnedLease>);
  struct CounterPreparationRollback {
    decltype(counters_)& counters;
    const std::vector<CounterKey>& keys;
    bool published = false;
    ~CounterPreparationRollback() {
      if (published) return;
      for (const auto& key : keys) {
        const auto found = counters.find(key);
        if (found != counters.end() && found->second.count == 0) counters.erase(found);
      }
    }
  } counter_rollback{counters_, record.counter_keys};
  if (!PrepareCountersLocked(record)) {
    return Refuse(request, "SB_CEIC_020_MEMORY_GOVERNANCE.RESERVATION_REFUSED",
                  "memory.ceic_020.reservation_refused", "governor_counter_capacity_exceeded",
                  StatusCode::memory_limit_exceeded);
  }

  auto decision = BaseDecision(OkStatus(),
                               "SB_CEIC_020_MEMORY_GOVERNANCE.OK",
                               "memory.ceic_020.ok",
                               request,
                               {{"lease_identity", "typed_binary_uuid"}});
  decision.fail_closed = false;
  decision.accepted = true;
  decision.lease_id = record.lease_id;
  AddEvidence(&decision.evidence, "ceic_020.reservation_created=true");
  AddEvidence(&decision.evidence, "ceic_020.reservation_committed=true");
  AddEvidence(&decision.evidence,
              "ceic_020.reservation_token_id=" +
                  std::to_string(reserved.token.token_id));
  AddEvidence(&decision.evidence, "ceic_020.lease_identity=typed_binary_uuid");
  AddEvidence(&decision.evidence,
              "ceic_020.lease_expires_at_ms=" +
                  std::to_string(record.lease_expires_at_ms));
  AddEvidence(&decision.evidence,
              "ceic_020.slow_client_bounded_by_frame_credit=true");
  AddEvidence(&decision.evidence,
              "ceic_020.plan_prepared_descriptor_cache_memory_not_authority=true");
  auto committed = request.ledger->Commit(reserved.token);
  if (!committed.ok()) {
    auto failure = Refuse(request,
        "SB_CEIC_020_MEMORY_GOVERNANCE.RESERVATION_COMMIT_REFUSED",
        "memory.ceic_020.reservation_commit_refused", "reservation_commit_refused",
        committed.status.code);
    failure.diagnostic = std::move(committed.diagnostic);
    return failure;
  }
  // The pending owner releases even a committed reservation if map allocation
  // fails. No result is published until its actual lease is registered.
  const auto lease_key = record.lease_id;
  const auto inserted = leases_.try_emplace(lease_key, std::move(record));
  if (!inserted.second) {
    return Refuse(request, "SB_CEIC_020_MEMORY_GOVERNANCE.RESERVATION_REFUSED",
                  "memory.ceic_020.reservation_refused", "lease_identity_collision");
  }
  AddCountersLocked(inserted.first->second);
  counter_rollback.published = true;
  pending.Publish();
  return decision;
} catch (const std::bad_alloc&) {
  return AllocationFailure();
}

Status ResultCursorPlanMemoryGovernor::ReleaseNoAlloc(const ResultCursorPlanMemoryUuid& lease_id) {
  if (!uuid::IsEngineIdentityUuid(lease_id)) return ErrorStatus(StatusCode::memory_invalid_request);
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = leases_.find(lease_id);
  if (it == leases_.end()) return ErrorStatus(StatusCode::memory_unknown_pointer);
  const auto status = it->second.ledger->ReleaseNoAlloc(it->second.token);
  if (!status.ok()) return status;
  RemoveCountersLocked(it->second);
  leases_.erase(it);
  ++release_count_;
  return OkStatus();
}

ResultCursorPlanMemoryDecision ResultCursorPlanMemoryGovernor::DrainLocked(
    const std::vector<LeaseMap::iterator>& selected,
    ResultCursorPlanMemoryDecision aggregate) {
  if (static_cast<int>(aggregate.release_reason) < static_cast<int>(ResultCursorPlanMemoryReleaseReason::close) ||
      static_cast<int>(aggregate.release_reason) > static_cast<int>(ResultCursorPlanMemoryReleaseReason::explicit_release))
    return RequestFailure(StatusCode::memory_invalid_request, aggregate.release_reason);
  // Stage every fallible response allocation before releasing any selected
  // owner. Subsequent publication is just moves into reserved vector capacity.
  std::vector<std::array<std::string, 4>> evidence;
  evidence.reserve(selected.size());
  for (const auto it : selected) {
    evidence.push_back({"ceic_020.lease_identity=typed_binary_uuid",
      "ceic_020.released_bytes=" + std::to_string(it->second.reserved_bytes),
      "ceic_020.ledger_release_routed_by_token=true",
      "ceic_020.memory_evidence_authority=advisory_only"});
  }
  if (selected.size() > (aggregate.evidence.max_size() - aggregate.evidence.size()) / 4)
    throw std::bad_alloc();
  aggregate.evidence.reserve(aggregate.evidence.size() + selected.size() * 4);
  if (selected.size() > aggregate.released_lease_ids.max_size() - aggregate.released_lease_ids.size())
    throw std::bad_alloc();
  aggregate.released_lease_ids.reserve(aggregate.released_lease_ids.size() + selected.size());
  auto failure = ReleaseDecision(ErrorStatus(StatusCode::memory_unknown_pointer),
      aggregate.release_reason, "SB_CEIC_020_MEMORY_GOVERNANCE.LEDGER_RELEASE_FAILED",
      "ceic_011_ledger_release_failed");
  for (std::size_t index = 0; index < selected.size(); ++index) {
    const auto it = selected[index];
    const auto status = it->second.ledger->ReleaseNoAlloc(it->second.token);
    if (!status.ok()) {
      aggregate.status = status;
      aggregate.fail_closed = true;
      aggregate.accepted = false;
      failure.diagnostic.status = status;
      aggregate.diagnostic = std::move(failure.diagnostic);
      return aggregate;  // Remaining owners and counters stay intact for retry.
    }
    aggregate.released_bytes += it->second.reserved_bytes;
    ++aggregate.released_lease_count;
    aggregate.released_lease_ids.push_back(it->first);
    RemoveCountersLocked(it->second);
    leases_.erase(it);
    ++release_count_;
    for (auto& item : evidence[index]) aggregate.evidence.push_back(std::move(item));
  }
  return aggregate;
}

ResultCursorPlanMemoryDecision ResultCursorPlanMemoryGovernor::Release(
    const ResultCursorPlanMemoryUuid& lease_id, ResultCursorPlanMemoryReleaseReason reason) try {
  if (!uuid::IsEngineIdentityUuid(lease_id)) return RequestFailure(StatusCode::memory_invalid_request, reason);
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = leases_.find(lease_id);
  if (it == leases_.end())
    return ReleaseDecision(ErrorStatus(StatusCode::memory_unknown_pointer), reason,
        "SB_CEIC_020_MEMORY_GOVERNANCE.LEASE_NOT_FOUND", "lease_not_found");
  auto decision = ReleaseDecision(OkStatus(), reason,
      "SB_CEIC_020_MEMORY_GOVERNANCE.RELEASE_OK", "lease_released");
  decision.lease_id = lease_id;
  return DrainLocked({it}, std::move(decision));
} catch (const std::bad_alloc&) {
  return AllocationFailure(reason);
}

ResultCursorPlanMemoryDecision ResultCursorPlanMemoryGovernor::ReleaseByCursor(
    const ResultCursorPlanMemoryUuid& cursor_id, ResultCursorPlanMemoryReleaseReason reason) try {
  if (!uuid::IsEngineIdentityUuid(cursor_id)) return RequestFailure(StatusCode::memory_invalid_request, reason);
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<LeaseMap::iterator> selected;
  for (auto it = leases_.begin(); it != leases_.end(); ++it) {
    const auto& record = it->second;
    if (record.scope.cursor_id == cursor_id) selected.push_back(it);
  }
  return DrainLocked(selected, ReleaseDecision(OkStatus(), reason,
      "SB_CEIC_020_MEMORY_GOVERNANCE.RELEASE_CURSOR_OK", "cursor_release"));
} catch (const std::bad_alloc&) {
  return AllocationFailure(reason);
}

ResultCursorPlanMemoryDecision ResultCursorPlanMemoryGovernor::ReleaseResultFramesByCursor(
    const ResultCursorPlanMemoryUuid& cursor_id, ResultCursorPlanMemoryReleaseReason reason) try {
  if (!uuid::IsEngineIdentityUuid(cursor_id)) return RequestFailure(StatusCode::memory_invalid_request, reason);
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<LeaseMap::iterator> selected;
  for (auto it = leases_.begin(); it != leases_.end(); ++it) {
    const auto& record = it->second;
    if (record.scope.cursor_id == cursor_id && record.surface == ResultCursorPlanMemorySurface::result_frame) selected.push_back(it);
  }
  return DrainLocked(selected, ReleaseDecision(OkStatus(), reason,
      "SB_CEIC_020_MEMORY_GOVERNANCE.RELEASE_CURSOR_FRAMES_OK", "cursor_result_frame_release"));
} catch (const std::bad_alloc&) {
  return AllocationFailure(reason);
}

ResultCursorPlanMemoryDecision ResultCursorPlanMemoryGovernor::ReleaseBySession(
    const ResultCursorPlanMemoryUuid& session_id, ResultCursorPlanMemoryReleaseReason reason) try {
  if (!uuid::IsEngineIdentityUuid(session_id)) return RequestFailure(StatusCode::memory_invalid_request, reason);
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<LeaseMap::iterator> selected;
  for (auto it = leases_.begin(); it != leases_.end(); ++it) {
    const auto& record = it->second;
    if (record.scope.session_id == session_id) selected.push_back(it);
  }
  return DrainLocked(selected, ReleaseDecision(OkStatus(), reason,
      "SB_CEIC_020_MEMORY_GOVERNANCE.RELEASE_SESSION_OK", "session_release"));
} catch (const std::bad_alloc&) {
  return AllocationFailure(reason);
}

ResultCursorPlanMemoryDecision ResultCursorPlanMemoryGovernor::ReleaseByConnection(
    const ResultCursorPlanMemoryUuid& connection_id, ResultCursorPlanMemoryReleaseReason reason) try {
  if (!uuid::IsEngineIdentityUuid(connection_id)) return RequestFailure(StatusCode::memory_invalid_request, reason);
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<LeaseMap::iterator> selected;
  for (auto it = leases_.begin(); it != leases_.end(); ++it) {
    const auto& record = it->second;
    if (record.scope.connection_id == connection_id) selected.push_back(it);
  }
  return DrainLocked(selected, ReleaseDecision(OkStatus(), reason,
      "SB_CEIC_020_MEMORY_GOVERNANCE.RELEASE_CONNECTION_OK", "connection_release"));
} catch (const std::bad_alloc&) {
  return AllocationFailure(reason);
}

ResultCursorPlanMemoryDecision ResultCursorPlanMemoryGovernor::ReleaseByQuery(
    const ResultCursorPlanMemoryUuid& query_id, ResultCursorPlanMemoryReleaseReason reason) try {
  if (!uuid::IsEngineIdentityUuid(query_id)) return RequestFailure(StatusCode::memory_invalid_request, reason);
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<LeaseMap::iterator> selected;
  for (auto it = leases_.begin(); it != leases_.end(); ++it) {
    const auto& record = it->second;
    if (record.scope.query_id == query_id) selected.push_back(it);
  }
  return DrainLocked(selected, ReleaseDecision(OkStatus(), reason,
      "SB_CEIC_020_MEMORY_GOVERNANCE.RELEASE_QUERY_OK", "query_release"));
} catch (const std::bad_alloc&) {
  return AllocationFailure(reason);
}

ResultCursorPlanMemoryDecision ResultCursorPlanMemoryGovernor::ReleaseByTransaction(
    const ResultCursorPlanMemoryUuid& transaction_id, ResultCursorPlanMemoryReleaseReason reason) try {
  if (!uuid::IsEngineIdentityUuid(transaction_id)) return RequestFailure(StatusCode::memory_invalid_request, reason);
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<LeaseMap::iterator> selected;
  for (auto it = leases_.begin(); it != leases_.end(); ++it) {
    const auto& record = it->second;
    if (record.scope.transaction_id == transaction_id) selected.push_back(it);
  }
  return DrainLocked(selected, ReleaseDecision(OkStatus(), reason,
      "SB_CEIC_020_MEMORY_GOVERNANCE.RELEASE_TRANSACTION_OK", "transaction_release"));
} catch (const std::bad_alloc&) {
  return AllocationFailure(reason);
}

ResultCursorPlanMemoryDecision ResultCursorPlanMemoryGovernor::InvalidateByEpoch(
    ResultCursorPlanMemoryEpochs current_epochs) try {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<LeaseMap::iterator> selected;
  for (auto it = leases_.begin(); it != leases_.end(); ++it)
    if (IsPlanPreparedOrDescriptor(it->second.surface) &&
        EpochRecordStale(it->second.epochs, current_epochs)) selected.push_back(it);
  auto decision = ReleaseDecision(OkStatus(), ResultCursorPlanMemoryReleaseReason::epoch_invalidation,
      "SB_CEIC_020_MEMORY_GOVERNANCE.EPOCH_INVALIDATION_OK", "epoch_invalidation");
  AppendEpochEvidence(&decision.evidence, current_epochs);
  auto result = DrainLocked(selected, std::move(decision));
  if (result.released_lease_count != 0) ++epoch_invalidation_count_;
  return result;
} catch (const std::bad_alloc&) {
  return AllocationFailure(ResultCursorPlanMemoryReleaseReason::epoch_invalidation);
}

ResultCursorPlanMemoryDecision ResultCursorPlanMemoryGovernor::ShrinkPlanCache(
    const ResultCursorPlanMemoryUuid& database_id, u64 target_bytes) try {
  if (!uuid::IsEngineIdentityUuid(database_id)) return RequestFailure(StatusCode::memory_invalid_request, ResultCursorPlanMemoryReleaseReason::shrink);
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<LeaseMap::iterator> selected;
  u64 current = 0;
  for (auto it = leases_.begin(); it != leases_.end(); ++it) {
    if (it->second.surface == ResultCursorPlanMemorySurface::plan_cache_entry &&
        it->second.scope.database_id == database_id) {
      current += it->second.reserved_bytes;
      selected.push_back(it);
    }
  }
  if (current <= target_bytes) {
    auto decision = ReleaseDecision(OkStatus(), ResultCursorPlanMemoryReleaseReason::shrink,
        "SB_CEIC_020_MEMORY_GOVERNANCE.SHRINK_NOT_NEEDED", "plan_cache_under_target");
    AddEvidence(&decision.evidence, "ceic_020.plan_cache_current_bytes=" + std::to_string(current));
    AddEvidence(&decision.evidence, "ceic_020.plan_cache_target_bytes=" + std::to_string(target_bytes));
    return decision;
  }
  std::sort(selected.begin(), selected.end(), [](const auto left, const auto right) {
    return left->second.acquired_sequence < right->second.acquired_sequence;
  });
  u64 remaining = current;
  std::size_t count = 0;
  while (remaining > target_bytes) remaining -= selected[count++]->second.reserved_bytes;
  selected.resize(count);
  return DrainLocked(selected, ReleaseDecision(OkStatus(), ResultCursorPlanMemoryReleaseReason::shrink,
      "SB_CEIC_020_MEMORY_GOVERNANCE.SHRINK_OK", "plan_cache_shrink"));
} catch (const std::bad_alloc&) {
  return AllocationFailure(ResultCursorPlanMemoryReleaseReason::shrink);
}

ResultCursorPlanMemoryDecision ResultCursorPlanMemoryGovernor::ForceCloseCursorUnderPressure(
    const ResultCursorPlanMemoryUuid& cursor_id) try {
  if (!uuid::IsEngineIdentityUuid(cursor_id)) return RequestFailure(StatusCode::memory_invalid_request, ResultCursorPlanMemoryReleaseReason::pressure_forced_close);
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<LeaseMap::iterator> selected;
  for (auto it = leases_.begin(); it != leases_.end(); ++it)
    if (it->second.scope.cursor_id == cursor_id) selected.push_back(it);
  auto decision = ReleaseDecision(OkStatus(), ResultCursorPlanMemoryReleaseReason::pressure_forced_close,
      "SB_CEIC_020_MEMORY_GOVERNANCE.RELEASE_CURSOR_OK", "cursor_release");
  decision.forced_close_required = true;
  AddEvidence(&decision.evidence, "ceic_020.pressure_forced_cursor_close=true");
  auto result = DrainLocked(selected, std::move(decision));
  if (result.released_lease_count != 0) ++forced_close_count_;
  return result;
} catch (const std::bad_alloc&) {
  return AllocationFailure(ResultCursorPlanMemoryReleaseReason::pressure_forced_close);
}

ResultCursorPlanMemoryDecision ResultCursorPlanMemoryGovernor::CleanupExpiredLeases(u64 now_ms) try {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<LeaseMap::iterator> selected;
  for (auto it = leases_.begin(); it != leases_.end(); ++it)
    if (it->second.lease_expires_at_ms != 0 && it->second.lease_expires_at_ms <= now_ms)
      selected.push_back(it);
  auto decision = ReleaseDecision(OkStatus(), ResultCursorPlanMemoryReleaseReason::expired_lease,
      "SB_CEIC_020_MEMORY_GOVERNANCE.EXPIRED_CLEANUP_OK", "expired_lease_cleanup");
  AddEvidence(&decision.evidence, "ceic_020.expired_cleanup_now_ms=" + std::to_string(now_ms));
  return DrainLocked(selected, std::move(decision));
} catch (const std::bad_alloc&) {
  return AllocationFailure(ResultCursorPlanMemoryReleaseReason::expired_lease);
}

ResultCursorPlanMemorySnapshot ResultCursorPlanMemoryGovernor::Snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  ResultCursorPlanMemorySnapshot snapshot;
  snapshot.quota_counter_count = static_cast<u64>(counters_.size());
  snapshot.active_lease_count = static_cast<u64>(leases_.size());
  snapshot.backpressure_count = backpressure_count_;
  snapshot.forced_close_count = forced_close_count_;
  snapshot.release_count = release_count_;
  snapshot.epoch_invalidation_count = epoch_invalidation_count_;
  for (const auto& [_, record] : leases_) {
    snapshot.active_bytes += record.reserved_bytes;
    snapshot.active_leases.push_back(record);
    switch (record.surface) {
      case ResultCursorPlanMemorySurface::streaming_result:
      case ResultCursorPlanMemorySurface::cursor:
        ++snapshot.cursor_count;
        snapshot.cursor_bytes += record.reserved_bytes;
        break;
      case ResultCursorPlanMemorySurface::result_frame:
        ++snapshot.result_frame_count;
        snapshot.result_frame_bytes += record.reserved_bytes;
        break;
      case ResultCursorPlanMemorySurface::plan_cache_entry:
        ++snapshot.plan_cache_entry_count;
        snapshot.plan_cache_bytes += record.reserved_bytes;
        break;
      case ResultCursorPlanMemorySurface::prepared_statement:
        ++snapshot.prepared_statement_count;
        snapshot.prepared_statement_bytes += record.reserved_bytes;
        break;
      case ResultCursorPlanMemorySurface::descriptor_snapshot:
        ++snapshot.descriptor_snapshot_count;
        snapshot.descriptor_snapshot_bytes += record.reserved_bytes;
        break;
    }
  }
  return snapshot;
}

}  // namespace scratchbird::core::memory
