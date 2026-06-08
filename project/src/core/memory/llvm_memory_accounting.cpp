// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// CEIC-061: LLVM dynamic/static linkage memory accounting.
#include "llvm_memory_accounting.hpp"

#include <algorithm>
#include <utility>

namespace scratchbird::core::memory {
namespace {

namespace metrics = scratchbird::core::metrics;

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

constexpr const char* kEvidenceAnchor =
    "CEIC-061_LLVM_DYNAMIC_STATIC_MEMORY_ACCOUNTING";
constexpr const char* kAuthorityScope =
    "llvm_memory_accounting.authority_scope=memory_evidence_only_not_transaction_finality_visibility_authorization_security_recovery_parser_donor_wal_benchmark_optimizer_plan_index_finality_provider_finality_cluster_or_agent_action_authority";

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::memory};
}

Status ErrorStatus(StatusCode code = StatusCode::memory_invalid_request,
                   Severity severity = Severity::error) {
  return {code, severity, Subsystem::memory};
}

bool Blank(const std::string& value) {
  return value.find_first_not_of(" \t\r\n") == std::string::npos;
}

std::string BoolText(bool value) {
  return value ? "true" : "false";
}

bool IsRuntimeProvenance(HierarchicalMemoryBudgetProvenanceSource source) {
  return source == HierarchicalMemoryBudgetProvenanceSource::runtime_policy ||
         source == HierarchicalMemoryBudgetProvenanceSource::server_runtime_api ||
         source == HierarchicalMemoryBudgetProvenanceSource::agent_runtime;
}

DiagnosticRecord MakeLlvmMemoryDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::vector<DiagnosticArgument> arguments = {}) {
  arguments.push_back({"authority_scope", kAuthorityScope});
  return MakeDiagnostic(
      status.code,
      status.severity,
      status.subsystem,
      std::move(diagnostic_code),
      std::move(message_key),
      std::move(arguments),
      {},
      "core.memory.llvm_memory_accounting",
      "Reserve ScratchBird foreign memory for LLVM loader, code, data, JIT, AOT, and native allocations before calling LLVM or native code.");
}

void AddMetric(std::vector<metrics::MetricValue>* metrics_out,
               const std::string& family,
               metrics::MetricLabelSet labels,
               double value,
               bool gauge) {
  if (gauge) {
    (void)metrics::DefaultMetricRegistry().SetGauge(
        family, labels, value, "llvm_memory_accounting");
  } else {
    (void)metrics::DefaultMetricRegistry().IncrementCounter(
        family, labels, value, "llvm_memory_accounting");
  }
  for (const auto& metric : metrics::DefaultMetricRegistry().SnapshotCurrent()) {
    if (metric.family == family) {
      if (metrics_out != nullptr) {
        metrics_out->push_back(metric);
      }
    }
  }
}

void AddBaseEvidence(std::vector<std::string>* evidence,
                     const LlvmMemoryAccountingRequest& request) {
  evidence->push_back(kEvidenceAnchor);
  evidence->push_back(kAuthorityScope);
  evidence->push_back("llvm_memory.reserve_before_llvm_or_native_call=true");
  evidence->push_back("llvm_memory.linkage_mode=" +
                      std::string(ForeignMemoryLinkageModeName(
                          request.linkage_mode)));
  evidence->push_back("llvm_memory.production_like=" +
                      BoolText(request.production_like));
  evidence->push_back("llvm_memory.explicit_test_fixture=" +
                      BoolText(request.explicit_test_fixture));
  evidence->push_back("llvm_memory.provider_available=" +
                      BoolText(request.provider_available));
  evidence->push_back("llvm_memory.owner_id=" + request.owner_id);
  evidence->push_back("llvm_memory.owning_scope=" + request.owning_scope);
  evidence->push_back("llvm_memory.operation_id=" + request.operation_id);
  evidence->push_back("llvm_memory.native_callsite=" + request.native_callsite);
  evidence->push_back("llvm_memory.provider_label=" + request.provider_label);
  evidence->push_back("llvm_memory.provenance_source=" +
                      std::string(HierarchicalMemoryBudgetProvenanceSourceName(
                          request.provenance.source)));
  evidence->push_back("llvm_memory.authority_generation=" +
                      request.authority.authority_generation);
  evidence->push_back("llvm_memory.evidence_label=" +
                      request.authority.evidence_label);
  evidence->push_back("llvm_memory.no_authority.transaction_finality=true");
  evidence->push_back("llvm_memory.no_authority.visibility=true");
  evidence->push_back("llvm_memory.no_authority.authorization_security=true");
  evidence->push_back("llvm_memory.no_authority.recovery=true");
  evidence->push_back("llvm_memory.no_authority.parser_donor_wal=true");
  evidence->push_back(
      "llvm_memory.no_authority.benchmark_optimizer_plan_index_provider_cluster_agent=true");
  for (const auto& entry : request.evidence) {
    evidence->push_back("llvm_memory.evidence=" + entry);
  }
}

LlvmMemoryAccountingAcquireResult RefuseAcquire(
    const LlvmMemoryAccountingRequest& request,
    std::string diagnostic_code,
    std::string message_key,
    std::string reason,
    StatusCode code = StatusCode::memory_invalid_request,
    Severity severity = Severity::error) {
  LlvmMemoryAccountingAcquireResult result;
  result.status = ErrorStatus(code, severity);
  result.fail_closed = true;
  result.diagnostic = MakeLlvmMemoryDiagnostic(
      result.status,
      std::move(diagnostic_code),
      std::move(message_key),
      {{"reason", std::move(reason)},
       {"linkage_mode", ForeignMemoryLinkageModeName(request.linkage_mode)},
       {"owner_id", request.owner_id},
       {"operation_id", request.operation_id}});
  AddBaseEvidence(&result.evidence, request);
  result.evidence.push_back("llvm_memory.fail_closed=true");
  result.evidence.push_back("llvm_memory.reservation_created=false");
  AddMetric(&result.metrics,
            "sb_llvm_foreign_memory_refusals_total",
            {{"component", "llvm_memory"},
             {"operation", request.operation_id},
             {"result", "refused"},
             {"reason", result.diagnostic.diagnostic_code}},
            1.0,
            false);
  return result;
}

bool UnsafeAuthority(const ForeignMemoryAuthority& authority,
                     std::string* reason) {
  if (!authority.engine_mga_authoritative || !authority.memory_evidence_only) {
    *reason = "engine_mga_and_memory_evidence_only_authority_required";
    return true;
  }
  if (!authority.security_or_policy_checked) {
    *reason = "security_or_policy_check_required";
    return true;
  }
  if (!authority.evidence_fresh || Blank(authority.evidence_label) ||
      Blank(authority.authority_generation)) {
    *reason = "fresh_non_empty_authority_evidence_required";
    return true;
  }
  if (authority.transaction_finality_authority || authority.visibility_authority ||
      authority.recovery_authority || authority.parser_authority ||
      authority.donor_authority || authority.wal_authority ||
      authority.benchmark_authority || authority.support_bundle_authority ||
      authority.optimizer_plan_authority || authority.index_finality_authority ||
      authority.agent_action_authority || authority.authorization_authority ||
      authority.cluster_authority || authority.debug_or_relaxed_path) {
    *reason = "unsafe_authority_claim_refused";
    return true;
  }
  return false;
}

bool ValidateShape(const LlvmMemoryAccountingRequest& request,
                   std::string* reason) {
  if (request.reservation_ledger == nullptr || request.foreign_ledger == nullptr) {
    *reason = "reservation_and_foreign_ledgers_required";
    return false;
  }
  if (request.scope_chain.empty()) {
    *reason = "scope_chain_required";
    return false;
  }
  for (const auto& scope : request.scope_chain) {
    if (Blank(scope.scope_id)) {
      *reason = "scope_id_required";
      return false;
    }
  }
  if (Blank(request.owner_id) || Blank(request.owning_scope) ||
      Blank(request.operation_id) || Blank(request.native_callsite)) {
    *reason = "owner_scope_operation_and_callsite_required";
    return false;
  }
  if (request.linkage_mode != ForeignMemoryLinkageMode::dynamic_library &&
      request.linkage_mode != ForeignMemoryLinkageMode::static_library) {
    *reason = "llvm_dynamic_or_static_linkage_mode_required";
    return false;
  }
  if (!IsRuntimeProvenance(request.provenance.source) ||
      Blank(request.provenance.source_label) ||
      !request.provenance.engine_mga_authoritative ||
      !request.provenance.memory_evidence_only ||
      request.provenance.parser_authority || request.provenance.donor_authority ||
      request.provenance.transaction_finality_authority ||
      request.provenance.visibility_authority ||
      request.provenance.recovery_authority ||
      request.provenance.authorization_authority ||
      request.provenance.benchmark_authority ||
      request.provenance.support_bundle_authority ||
      request.provenance.cluster_authority ||
      request.provenance.debug_or_relaxed_path) {
    *reason = "safe_runtime_memory_provenance_required";
    return false;
  }
  if (request.explicit_test_fixture && request.production_like) {
    *reason = "test_fixture_must_not_be_marked_production_like";
    return false;
  }
  if (!request.provider_available && request.production_like &&
      !request.explicit_test_fixture) {
    *reason = "production_llvm_provider_required";
    return false;
  }
  if (!request.provider_available && !request.explicit_test_fixture) {
    *reason = "unavailable_llvm_provider_requires_explicit_test_fixture";
    return false;
  }
  if (request.evidence.empty()) {
    *reason = "llvm_memory_accounting_evidence_required";
    return false;
  }
  if (UnsafeAuthority(request.authority, reason)) {
    return false;
  }
  return true;
}

struct PhasePlan {
  LlvmMemoryReservationPhase phase = LlvmMemoryReservationPhase::data;
  MemoryCategory category = MemoryCategory::llvm_data_reserved;
  std::string memory_class;
  u64 bytes = 0;
};

std::vector<PhasePlan> BuildPhasePlan(
    const LlvmMemoryAccountingRequest& request) {
  std::vector<PhasePlan> phases;
  if (request.reserve_loader_or_link_metadata) {
    if (request.linkage_mode == ForeignMemoryLinkageMode::dynamic_library &&
        request.loader_bytes != 0) {
      phases.push_back({LlvmMemoryReservationPhase::dynamic_loader,
                        MemoryCategory::llvm_data_reserved,
                        "ceic_061.llvm.dynamic_loader",
                        request.loader_bytes});
    } else if (request.linkage_mode == ForeignMemoryLinkageMode::static_library &&
               request.static_link_metadata_bytes != 0) {
      phases.push_back({LlvmMemoryReservationPhase::static_linkage_metadata,
                        MemoryCategory::llvm_data_reserved,
                        "ceic_061.llvm.static_linkage_metadata",
                        request.static_link_metadata_bytes});
    }
  }
  if (request.reserve_code && request.code_bytes != 0) {
    phases.push_back({LlvmMemoryReservationPhase::code,
                      MemoryCategory::llvm_code_reserved,
                      "ceic_061.llvm.code",
                      request.code_bytes});
  }
  if (request.reserve_data && request.data_bytes != 0) {
    phases.push_back({LlvmMemoryReservationPhase::data,
                      MemoryCategory::llvm_data_reserved,
                      "ceic_061.llvm.data",
                      request.data_bytes});
  }
  if (request.reserve_native && request.native_bytes != 0) {
    phases.push_back({request.aot ? LlvmMemoryReservationPhase::aot_native
                                  : LlvmMemoryReservationPhase::jit_native,
                      MemoryCategory::llvm_code_reserved,
                      request.aot ? "ceic_061.llvm.aot_native"
                                  : "ceic_061.llvm.jit_native",
                      request.native_bytes});
  }
  return phases;
}

ForeignMemoryReservationRequest ForeignRequestForPhase(
    const LlvmMemoryAccountingRequest& request,
    const PhasePlan& phase) {
  ForeignMemoryReservationRequest foreign;
  foreign.source = ForeignMemorySource::llvm;
  foreign.reservation_ledger = request.reservation_ledger;
  foreign.scope_chain = request.scope_chain;
  foreign.category = phase.category;
  foreign.memory_class = phase.memory_class;
  foreign.estimated_bytes = phase.bytes;
  foreign.observed_bytes = 0;
  foreign.owner_id = request.owner_id;
  foreign.owning_scope = request.owning_scope;
  foreign.operation_id = request.operation_id;
  foreign.native_callsite =
      request.native_callsite + "." + LlvmMemoryReservationPhaseName(phase.phase);
  foreign.confidence = request.confidence;
  foreign.expected_release_event =
      ForeignMemoryReleaseEvent::adapter_shutdown;
  foreign.over_limit_action = request.over_limit_action;
  foreign.linkage_mode = request.linkage_mode;
  foreign.production_like = request.production_like;
  foreign.live_route_claim =
      request.production_like && request.provider_available &&
      !request.explicit_test_fixture;
  foreign.untracked_high_risk_native_call = false;
  foreign.conservative_reservation =
      !request.provider_available && request.explicit_test_fixture;
  foreign.provenance = request.provenance;
  foreign.authority = request.authority;
  foreign.authority.provider_available = request.provider_available;
  foreign.evidence = {
      kEvidenceAnchor,
      kAuthorityScope,
      "llvm_memory.phase=" +
          std::string(LlvmMemoryReservationPhaseName(phase.phase)),
      "llvm_memory.memory_class=" + phase.memory_class,
      "llvm_memory.reserve_before_llvm_or_native_call=true",
      "llvm_memory.production_test_separation=" +
          std::string(request.explicit_test_fixture ? "explicit_fixture"
                                                    : "production_runtime"),
      "llvm_memory.provider_available=" + BoolText(request.provider_available),
      "llvm_memory.live_route_claim=" + BoolText(foreign.live_route_claim),
      "llvm_memory.provider_label=" + request.provider_label};
  if (request.explicit_test_fixture) {
    foreign.evidence.push_back("llvm_memory.test_fixture_explicit=true");
  }
  for (const auto& entry : request.evidence) {
    foreign.evidence.push_back("llvm_memory.request_evidence=" + entry);
  }
  return foreign;
}

}  // namespace

const char* LlvmMemoryReservationPhaseName(
    LlvmMemoryReservationPhase phase) {
  switch (phase) {
    case LlvmMemoryReservationPhase::dynamic_loader:
      return "dynamic_loader";
    case LlvmMemoryReservationPhase::static_linkage_metadata:
      return "static_linkage_metadata";
    case LlvmMemoryReservationPhase::code:
      return "code";
    case LlvmMemoryReservationPhase::data:
      return "data";
    case LlvmMemoryReservationPhase::jit_native:
      return "jit_native";
    case LlvmMemoryReservationPhase::aot_native:
      return "aot_native";
  }
  return "unknown";
}

LlvmMemoryAccountingReservation::~LlvmMemoryAccountingReservation() {
  (void)Release(ForeignMemoryReleaseEvent::scope_exit);
}

LlvmMemoryAccountingReservation::LlvmMemoryAccountingReservation(
    LlvmMemoryAccountingRequest request,
    std::vector<PhaseReservation> reservations,
    std::shared_ptr<HandleState> state)
    : request_(std::move(request)),
      reservations_(std::move(reservations)),
      state_(std::move(state)) {}

bool LlvmMemoryAccountingReservation::active() const {
  return state_ != nullptr && !state_->released.load();
}

u64 LlvmMemoryAccountingReservation::reserved_bytes() const {
  u64 total = 0;
  for (const auto& phase : reservations_) {
    total += phase.bytes;
  }
  return total;
}

u64 LlvmMemoryAccountingReservation::reservation_count() const {
  return static_cast<u64>(reservations_.size());
}

LlvmMemoryAccountingSnapshot LlvmMemoryAccountingReservation::Snapshot() const {
  LlvmMemoryAccountingSnapshot snapshot;
  snapshot.active = active();
  snapshot.linkage_mode = request_.linkage_mode;
  snapshot.production_like = request_.production_like;
  snapshot.explicit_test_fixture = request_.explicit_test_fixture;
  snapshot.provider_available = request_.provider_available;
  snapshot.reservation_count = reservation_count();
  snapshot.reserved_bytes = reserved_bytes();
  AddBaseEvidence(&snapshot.evidence, request_);
  snapshot.evidence.push_back("llvm_memory.reservation_count=" +
                              std::to_string(snapshot.reservation_count));
  snapshot.evidence.push_back("llvm_memory.reserved_bytes=" +
                              std::to_string(snapshot.reserved_bytes));
  for (const auto& phase : reservations_) {
    snapshot.phases.push_back(phase.phase);
    if (phase.reservation != nullptr) {
      snapshot.tokens.push_back(phase.reservation->token());
    }
    snapshot.evidence.push_back("llvm_memory.phase=" +
                                std::string(LlvmMemoryReservationPhaseName(
                                    phase.phase)));
  }
  return snapshot;
}

LlvmMemoryAccountingReleaseResult LlvmMemoryAccountingReservation::Release(
    ForeignMemoryReleaseEvent event) {
  LlvmMemoryAccountingReleaseResult result;
  result.status = OkStatus();
  if (state_ == nullptr) {
    result.status = ErrorStatus(StatusCode::memory_unknown_pointer);
    result.fail_closed = true;
    result.diagnostic = MakeLlvmMemoryDiagnostic(
        result.status,
        "SB_CEIC_061_LLVM_MEMORY_RELEASE_INVALID",
        "memory.ceic_061.llvm.release_invalid",
        {{"reason", "reservation_handle_invalid"}});
    result.evidence.push_back(kEvidenceAnchor);
    result.evidence.push_back(kAuthorityScope);
    return result;
  }
  if (state_->released.exchange(true)) {
    result.released = true;
    result.evidence.push_back(kEvidenceAnchor);
    result.evidence.push_back(kAuthorityScope);
    result.evidence.push_back("llvm_memory.release.already_released=true");
    return result;
  }
  AddBaseEvidence(&result.evidence, request_);
  result.evidence.push_back("llvm_memory.release_event=" +
                            std::string(ForeignMemoryReleaseEventName(event)));
  for (auto it = reservations_.rbegin(); it != reservations_.rend(); ++it) {
    if (it->reservation == nullptr) {
      continue;
    }
    auto released = it->reservation->Release(event);
    if (!released.ok()) {
      result.status = released.status;
      result.fail_closed = true;
      result.diagnostic = released.diagnostic;
      result.evidence.push_back(
          "llvm_memory.release.fail_closed_phase=" +
          std::string(LlvmMemoryReservationPhaseName(it->phase)));
      return result;
    }
    result.released_bytes += it->bytes;
    ++result.released_reservation_count;
    result.evidence.push_back(
        "llvm_memory.release.phase=" +
        std::string(LlvmMemoryReservationPhaseName(it->phase)));
  }
  result.released = true;
  result.evidence.push_back("llvm_memory.reservation_released=true");
  result.evidence.push_back("llvm_memory.release_count=" +
                            std::to_string(result.released_reservation_count));
  result.diagnostic = MakeLlvmMemoryDiagnostic(
      result.status,
      "SB_CEIC_061_LLVM_MEMORY_RELEASED",
      "memory.ceic_061.llvm.released",
      {{"reservation_count",
        std::to_string(result.released_reservation_count)},
       {"released_bytes", std::to_string(result.released_bytes)}});
  (void)metrics::DefaultMetricRegistry().SetGauge(
      "sb_llvm_foreign_memory_reserved_bytes",
      {{"component", "llvm_memory"},
       {"operation", request_.operation_id},
       {"result", "current"},
       {"reason", ForeignMemoryLinkageModeName(request_.linkage_mode)}},
      0.0,
      "llvm_memory_accounting");
  return result;
}

LlvmMemoryAccountingAcquireResult AcquireLlvmMemoryAccountingReservation(
    LlvmMemoryAccountingRequest request) {
  if (request.provenance.source ==
      HierarchicalMemoryBudgetProvenanceSource::unknown) {
    request.provenance.source =
        HierarchicalMemoryBudgetProvenanceSource::runtime_policy;
  }
  if (request.provenance.source_label.empty()) {
    request.provenance.source_label = "ceic_061_llvm_memory_accounting";
  }
  request.provenance.engine_mga_authoritative = true;
  request.provenance.memory_evidence_only = true;
  if (request.authority.evidence_label.empty()) {
    request.authority.evidence_label = "ceic_061_llvm_memory_accounting";
  }
  if (request.authority.authority_generation.empty()) {
    request.authority.authority_generation = "runtime";
  }
  std::string reason;
  if (!ValidateShape(request, &reason)) {
    return RefuseAcquire(
        request,
        "SB_CEIC_061_LLVM_MEMORY_REQUEST_REFUSED",
        "memory.ceic_061.llvm.request_refused",
        std::move(reason));
  }
  const auto phases = BuildPhasePlan(request);
  if (phases.empty()) {
    return RefuseAcquire(
        request,
        "SB_CEIC_061_LLVM_MEMORY_PHASES_REQUIRED",
        "memory.ceic_061.llvm.phases_required",
        "at_least_one_llvm_memory_phase_required");
  }

  std::vector<LlvmMemoryAccountingReservation::PhaseReservation> reservations;
  reservations.reserve(phases.size());
  u64 reserved_bytes = 0;
  std::vector<std::string> evidence;
  AddBaseEvidence(&evidence, request);
  for (const auto& phase : phases) {
    auto acquired = AcquireForeignMemoryReservation(
        request.foreign_ledger, ForeignRequestForPhase(request, phase));
    if (!acquired.ok()) {
      for (auto& prior : reservations) {
        if (prior.reservation != nullptr) {
          (void)prior.reservation->Release(
              ForeignMemoryReleaseEvent::cancel_cleanup);
        }
      }
      auto refused = RefuseAcquire(
          request,
          "SB_CEIC_061_LLVM_MEMORY_RESERVATION_REFUSED",
          "memory.ceic_061.llvm.reservation_refused",
          acquired.diagnostic.diagnostic_code.empty()
              ? "foreign_memory_reservation_refused"
              : acquired.diagnostic.diagnostic_code,
          acquired.status.code,
          acquired.status.severity);
      refused.diagnostic = acquired.diagnostic;
      return refused;
    }
    evidence.push_back("llvm_memory.reserved_phase=" +
                       std::string(LlvmMemoryReservationPhaseName(phase.phase)));
    evidence.insert(evidence.end(), acquired.evidence.begin(), acquired.evidence.end());
    reserved_bytes += phase.bytes;
    reservations.push_back({phase.phase, phase.bytes, std::move(acquired.reservation)});
  }

  auto state =
      std::make_shared<LlvmMemoryAccountingReservation::HandleState>();
  LlvmMemoryAccountingAcquireResult result;
  result.status = OkStatus();
  result.reservation.reset(new LlvmMemoryAccountingReservation(
      request, std::move(reservations), state));
  result.evidence = std::move(evidence);
  result.evidence.push_back("llvm_memory.reservation_created=true");
  result.evidence.push_back("llvm_memory.reservation_count=" +
                            std::to_string(result.reservation->reservation_count()));
  result.evidence.push_back("llvm_memory.reserved_bytes=" +
                            std::to_string(reserved_bytes));
  result.diagnostic = MakeLlvmMemoryDiagnostic(
      result.status,
      "SB_CEIC_061_LLVM_MEMORY_RESERVED",
      "memory.ceic_061.llvm.reserved",
      {{"reservation_count",
        std::to_string(result.reservation->reservation_count())},
       {"reserved_bytes", std::to_string(reserved_bytes)},
       {"linkage_mode", ForeignMemoryLinkageModeName(request.linkage_mode)}});
  AddMetric(&result.metrics,
            "sb_llvm_foreign_memory_reservations_total",
            {{"component", "llvm_memory"},
             {"operation", request.operation_id},
             {"result", "reserved"},
             {"reason", ForeignMemoryLinkageModeName(request.linkage_mode)}},
            static_cast<double>(result.reservation->reservation_count()),
            false);
  AddMetric(&result.metrics,
            "sb_llvm_foreign_memory_reserved_bytes",
            {{"component", "llvm_memory"},
             {"operation", request.operation_id},
             {"result", "current"},
             {"reason", ForeignMemoryLinkageModeName(request.linkage_mode)}},
            static_cast<double>(reserved_bytes),
            true);
  return result;
}

}  // namespace scratchbird::core::memory
