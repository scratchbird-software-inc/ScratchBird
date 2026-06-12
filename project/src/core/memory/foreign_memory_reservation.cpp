// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// CEIC-016: reservation-first governance for foreign/native memory.
#include "foreign_memory_reservation.hpp"

#include <algorithm>
#include <utility>

namespace scratchbird::core::memory {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

constexpr const char* kEvidenceAnchor =
    "CEIC-016_FOREIGN_MEMORY_RESERVATION_COVERAGE";
constexpr const char* kAuthorityScope =
    "foreign_memory.authority_scope=evidence_only_not_transaction_finality_visibility_recovery_parser_reference_wal_benchmark_optimizer_plan_index_finality_or_agent_action_authority";

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::memory};
}

Status ErrorStatus(StatusCode code = StatusCode::memory_invalid_request,
                   Severity severity = Severity::error) {
  return {code, severity, Subsystem::memory};
}

std::string BoolText(bool value) {
  return value ? "true" : "false";
}

bool Blank(const std::string& value) {
  return value.find_first_not_of(" \t\r\n") == std::string::npos;
}

void UpdatePeak(u64 current, u64* peak) {
  if (current > *peak) {
    *peak = current;
  }
}

void AddBaseEvidence(std::vector<std::string>* evidence,
                     const ForeignMemoryReservationRequest& request) {
  evidence->push_back(kEvidenceAnchor);
  evidence->push_back(kAuthorityScope);
  evidence->push_back("foreign_memory.source=" +
                      std::string(ForeignMemorySourceName(request.source)));
  evidence->push_back("foreign_memory.owner_id=" + request.owner_id);
  evidence->push_back("foreign_memory.owning_scope=" + request.owning_scope);
  evidence->push_back("foreign_memory.operation_id=" + request.operation_id);
  evidence->push_back("foreign_memory.native_callsite=" + request.native_callsite);
  evidence->push_back("foreign_memory.estimated_bytes=" +
                      std::to_string(request.estimated_bytes));
  evidence->push_back("foreign_memory.observed_bytes=" +
                      std::to_string(request.observed_bytes));
  evidence->push_back("foreign_memory.confidence=" +
                      std::string(ForeignMemoryConfidenceName(request.confidence)));
  evidence->push_back("foreign_memory.expected_release_event=" +
                      std::string(ForeignMemoryReleaseEventName(
                          request.expected_release_event)));
  evidence->push_back("foreign_memory.over_limit_action=" +
                      std::string(ForeignMemoryOverLimitActionName(
                          request.over_limit_action)));
  evidence->push_back("foreign_memory.linkage_mode=" +
                      std::string(ForeignMemoryLinkageModeName(
                          request.linkage_mode)));
  evidence->push_back(
      "foreign_memory.reservation_ledger=hierarchical_memory_budget_ledger");
  evidence->push_back("foreign_memory.reserve_before_native_call=true");
  evidence->push_back("foreign_memory.provider_available=" +
                      BoolText(request.authority.provider_available));
  evidence->push_back("foreign_memory.evidence_label=" +
                      request.authority.evidence_label);
  for (const auto& entry : request.evidence) {
    evidence->push_back("foreign_memory.evidence=" + entry);
  }
}

DiagnosticRecord MakeForeignMemoryDiagnostic(
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
                        "core.memory.foreign_memory_reservation",
                        "Reserve foreign/native memory with CEIC-011 before calling high-risk native or library code.");
}

bool UnsafeAuthority(const ForeignMemoryReservationRequest& request,
                     std::string* reason) {
  const auto& authority = request.authority;
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
      authority.reference_authority || authority.wal_authority ||
      authority.benchmark_authority || authority.support_bundle_authority ||
      authority.optimizer_plan_authority || authority.index_finality_authority ||
      authority.agent_action_authority || authority.authorization_authority ||
      authority.cluster_authority || authority.debug_or_relaxed_path) {
    *reason = "unsafe_authority_claim_refused";
    return true;
  }
  if (request.source == ForeignMemorySource::gpu_optional &&
      !authority.provider_available) {
    *reason = "gpu_provider_required_for_gpu_memory_claim";
    return true;
  }
  if (request.live_route_claim && !authority.provider_available) {
    *reason = "provider_required_for_live_foreign_memory_route_claim";
    return true;
  }
  return false;
}

bool ValidateRequestShape(const ForeignMemoryReservationRequest& request,
                          std::string* reason) {
  if (request.source == ForeignMemorySource::unknown) {
    *reason = "known_foreign_memory_source_required";
    return false;
  }
  if (request.reservation_ledger == nullptr) {
    *reason = "hierarchical_memory_budget_ledger_required";
    return false;
  }
  if (request.scope_chain.empty()) {
    *reason = "scope_chain_required";
    return false;
  }
  if (Blank(request.owner_id) || Blank(request.owning_scope) ||
      Blank(request.operation_id) || Blank(request.native_callsite)) {
    *reason = "owner_scope_operation_and_callsite_required";
    return false;
  }
  if (request.estimated_bytes == 0 && request.observed_bytes == 0) {
    *reason = "estimated_or_observed_bytes_required";
    return false;
  }
  if (request.confidence == ForeignMemoryConfidence::unknown) {
    *reason = "non_unknown_confidence_required";
    return false;
  }
  if (request.evidence.empty()) {
    *reason = "foreign_memory_evidence_required";
    return false;
  }
  if (request.untracked_high_risk_native_call &&
      !request.conservative_reservation) {
    *reason = "untracked_high_risk_native_call_requires_conservative_reservation";
    return false;
  }
  if (request.source != ForeignMemorySource::llvm &&
      request.linkage_mode != ForeignMemoryLinkageMode::not_applicable) {
    *reason = "linkage_mode_only_valid_for_llvm_source";
    return false;
  }
  if (request.source == ForeignMemorySource::llvm &&
      request.linkage_mode == ForeignMemoryLinkageMode::not_applicable) {
    *reason = "llvm_linkage_mode_required";
    return false;
  }
  return true;
}

HierarchicalMemoryReservationRecommendation RecommendationFor(
    ForeignMemoryOverLimitAction action) {
  switch (action) {
    case ForeignMemoryOverLimitAction::spill:
      return HierarchicalMemoryReservationRecommendation::spill;
    case ForeignMemoryOverLimitAction::cancel:
      return HierarchicalMemoryReservationRecommendation::cancel;
    case ForeignMemoryOverLimitAction::degrade:
      return HierarchicalMemoryReservationRecommendation::degrade;
    case ForeignMemoryOverLimitAction::deny:
      return HierarchicalMemoryReservationRecommendation::deny;
  }
  return HierarchicalMemoryReservationRecommendation::deny;
}

bool Spillable(ForeignMemoryOverLimitAction action) {
  return action == ForeignMemoryOverLimitAction::spill;
}

bool Cancelable(ForeignMemoryOverLimitAction action) {
  return action == ForeignMemoryOverLimitAction::cancel;
}

std::string DefaultMemoryClassFor(ForeignMemorySource source) {
  return std::string("ceic_016.") + ForeignMemorySourceName(source);
}

ForeignMemoryReservationAcquireResult RefuseAcquire(
    const ForeignMemoryReservationRequest& request,
    std::string diagnostic_code,
    std::string message_key,
    std::string reason,
    StatusCode code = StatusCode::memory_invalid_request,
    Severity severity = Severity::error) {
  ForeignMemoryReservationAcquireResult result;
  result.status = ErrorStatus(code, severity);
  result.fail_closed = true;
  result.diagnostic = MakeForeignMemoryDiagnostic(
      result.status,
      std::move(diagnostic_code),
      std::move(message_key),
      {{"reason", std::move(reason)},
       {"source", ForeignMemorySourceName(request.source)},
       {"owner_id", request.owner_id},
       {"owning_scope", request.owning_scope},
       {"operation_id", request.operation_id}});
  AddBaseEvidence(&result.evidence, request);
  result.evidence.push_back("foreign_memory.fail_closed=true");
  result.evidence.push_back("foreign_memory.reservation_created=false");
  result.evidence.push_back("foreign_memory.refused=" +
                            result.diagnostic.diagnostic_code);
  return result;
}

ForeignMemoryReservationReleaseResult RefuseRelease(
    ForeignMemoryReservationToken token,
    std::string diagnostic_code,
    std::string message_key,
    std::string reason,
    StatusCode code = StatusCode::memory_unknown_pointer) {
  ForeignMemoryReservationReleaseResult result;
  result.status = ErrorStatus(code);
  result.fail_closed = true;
  result.diagnostic = MakeForeignMemoryDiagnostic(
      result.status,
      std::move(diagnostic_code),
      std::move(message_key),
      {{"reason", std::move(reason)},
       {"reservation_id", std::to_string(token.reservation_id)},
       {"ledger_token_id", std::to_string(token.ledger_token.token_id)},
       {"ledger_token_bytes", std::to_string(token.ledger_token.bytes)}});
  result.evidence.push_back(kEvidenceAnchor);
  result.evidence.push_back(kAuthorityScope);
  result.evidence.push_back("foreign_memory.release.fail_closed=true");
  return result;
}

ForeignMemoryReservationObservationResult RefuseObservation(
    ForeignMemoryReservationToken token,
    std::string diagnostic_code,
    std::string message_key,
    std::string reason,
    StatusCode code = StatusCode::memory_invalid_request) {
  ForeignMemoryReservationObservationResult result;
  result.status = ErrorStatus(code);
  result.fail_closed = true;
  result.diagnostic = MakeForeignMemoryDiagnostic(
      result.status,
      std::move(diagnostic_code),
      std::move(message_key),
      {{"reason", std::move(reason)},
       {"reservation_id", std::to_string(token.reservation_id)}});
  result.evidence.push_back(kEvidenceAnchor);
  result.evidence.push_back(kAuthorityScope);
  result.evidence.push_back("foreign_memory.observation.fail_closed=true");
  return result;
}

}  // namespace

const char* ForeignMemorySourceName(ForeignMemorySource source) {
  switch (source) {
    case ForeignMemorySource::llvm:
      return "llvm";
    case ForeignMemorySource::icu:
      return "icu";
    case ForeignMemorySource::crypto:
      return "crypto";
    case ForeignMemorySource::compression:
      return "compression";
    case ForeignMemorySource::regex:
      return "regex";
    case ForeignMemorySource::json:
      return "json";
    case ForeignMemorySource::mmap:
      return "mmap";
    case ForeignMemorySource::thread_stack:
      return "thread_stack";
    case ForeignMemorySource::os_buffer:
      return "os_buffer";
    case ForeignMemorySource::plugin_udr:
      return "plugin_udr";
    case ForeignMemorySource::driver_native:
      return "driver_native";
    case ForeignMemorySource::gpu_optional:
      return "gpu_optional";
    case ForeignMemorySource::unknown:
      break;
  }
  return "unknown";
}

const char* ForeignMemoryConfidenceName(ForeignMemoryConfidence confidence) {
  switch (confidence) {
    case ForeignMemoryConfidence::exact:
      return "exact";
    case ForeignMemoryConfidence::observed:
      return "observed";
    case ForeignMemoryConfidence::estimated:
      return "estimated";
    case ForeignMemoryConfidence::conservative:
      return "conservative";
    case ForeignMemoryConfidence::unknown:
      break;
  }
  return "unknown";
}

const char* ForeignMemoryReleaseEventName(ForeignMemoryReleaseEvent event) {
  switch (event) {
    case ForeignMemoryReleaseEvent::explicit_release:
      return "explicit_release";
    case ForeignMemoryReleaseEvent::scope_exit:
      return "scope_exit";
    case ForeignMemoryReleaseEvent::cancel_cleanup:
      return "cancel_cleanup";
    case ForeignMemoryReleaseEvent::owner_cleanup:
      return "owner_cleanup";
    case ForeignMemoryReleaseEvent::adapter_shutdown:
      return "adapter_shutdown";
  }
  return "unknown";
}

const char* ForeignMemoryOverLimitActionName(ForeignMemoryOverLimitAction action) {
  switch (action) {
    case ForeignMemoryOverLimitAction::deny:
      return "deny";
    case ForeignMemoryOverLimitAction::spill:
      return "spill";
    case ForeignMemoryOverLimitAction::cancel:
      return "cancel";
    case ForeignMemoryOverLimitAction::degrade:
      return "degrade";
  }
  return "unknown";
}

const char* ForeignMemoryLinkageModeName(ForeignMemoryLinkageMode mode) {
  switch (mode) {
    case ForeignMemoryLinkageMode::not_applicable:
      return "not_applicable";
    case ForeignMemoryLinkageMode::dynamic_library:
      return "dynamic_library";
    case ForeignMemoryLinkageMode::static_library:
      return "static_library";
  }
  return "unknown";
}

MemoryCategory DefaultForeignMemoryCategory(ForeignMemorySource source) {
  switch (source) {
    case ForeignMemorySource::llvm:
      return MemoryCategory::llvm_data_reserved;
    case ForeignMemorySource::plugin_udr:
      return MemoryCategory::udr_reserved;
    case ForeignMemorySource::gpu_optional:
      return MemoryCategory::gpu_device_reserved;
    case ForeignMemorySource::mmap:
    case ForeignMemorySource::os_buffer:
      return MemoryCategory::page_buffer;
    case ForeignMemorySource::icu:
    case ForeignMemorySource::crypto:
    case ForeignMemorySource::compression:
    case ForeignMemorySource::regex:
    case ForeignMemorySource::json:
    case ForeignMemorySource::thread_stack:
    case ForeignMemorySource::driver_native:
      return MemoryCategory::core_runtime;
    case ForeignMemorySource::unknown:
      break;
  }
  return MemoryCategory::unknown;
}

ForeignMemoryReservation::ForeignMemoryReservation(
    ForeignMemoryReservationLedger* owner,
    ForeignMemoryReservationRequest request,
    ForeignMemoryReservationToken token,
    std::shared_ptr<HandleState> state)
    : owner_(owner),
      request_(std::move(request)),
      token_(token),
      state_(std::move(state)) {}

ForeignMemoryReservation::~ForeignMemoryReservation() {
  (void)Release(ForeignMemoryReleaseEvent::scope_exit);
}

bool ForeignMemoryReservation::active() const {
  return owner_ != nullptr && state_ != nullptr && !state_->released.load() &&
         token_.valid();
}

const ForeignMemoryReservationToken& ForeignMemoryReservation::token() const {
  return token_;
}

const ForeignMemoryReservationRequest& ForeignMemoryReservation::request() const {
  return request_;
}

ForeignMemoryReservationReleaseResult ForeignMemoryReservation::Release(
    ForeignMemoryReleaseEvent event) {
  ForeignMemoryReservationReleaseResult result;
  result.status = OkStatus();
  if (state_ == nullptr || owner_ == nullptr || !token_.valid()) {
    result.status = ErrorStatus(StatusCode::memory_unknown_pointer);
    result.fail_closed = true;
    result.diagnostic = MakeForeignMemoryDiagnostic(
        result.status,
        "SB_CEIC_016_FOREIGN_MEMORY_RELEASE_INVALID",
        "memory.ceic_016.foreign.release_invalid",
        {{"reason", "reservation_handle_invalid"}});
    result.evidence.push_back(kEvidenceAnchor);
    result.evidence.push_back(kAuthorityScope);
    return result;
  }
  bool was_released = state_->released.exchange(true);
  if (was_released) {
    result.released = true;
    result.evidence.push_back(kEvidenceAnchor);
    result.evidence.push_back(kAuthorityScope);
    result.evidence.push_back("foreign_memory.release.already_released=true");
    return result;
  }
  result = owner_->ReleaseReservation(token_, event);
  if (!result.ok()) {
    state_->released.store(false);
  }
  return result;
}

ForeignMemoryReservationReleaseResult ForeignMemoryReservation::Cancel() {
  return Release(ForeignMemoryReleaseEvent::cancel_cleanup);
}

ForeignMemoryReservationObservationResult
ForeignMemoryReservation::UpdateObservedBytes(
    u64 observed_bytes,
    ForeignMemoryConfidence confidence) {
  if (owner_ == nullptr || state_ == nullptr || !token_.valid()) {
    return RefuseObservation(token_,
                             "SB_CEIC_016_FOREIGN_MEMORY_OBSERVATION_INVALID",
                             "memory.ceic_016.foreign.observation_invalid",
                             "reservation_handle_invalid");
  }
  if (state_->released.load()) {
    return RefuseObservation(token_,
                             "SB_CEIC_016_FOREIGN_MEMORY_OBSERVATION_RELEASED",
                             "memory.ceic_016.foreign.observation_released",
                             "reservation_already_released");
  }
  return owner_->UpdateObservedBytes(token_, observed_bytes, confidence);
}

ForeignMemoryActiveReservationSnapshot ForeignMemoryReservation::Snapshot() const {
  ForeignMemoryActiveReservationSnapshot snapshot;
  snapshot.token = token_;
  snapshot.source = request_.source;
  snapshot.category = request_.category;
  snapshot.owner_id = request_.owner_id;
  snapshot.owning_scope = request_.owning_scope;
  snapshot.operation_id = request_.operation_id;
  snapshot.memory_class = request_.memory_class;
  snapshot.estimated_bytes = request_.estimated_bytes;
  snapshot.observed_bytes = request_.observed_bytes;
  snapshot.confidence = request_.confidence;
  snapshot.expected_release_event = request_.expected_release_event;
  snapshot.over_limit_action = request_.over_limit_action;
  snapshot.linkage_mode = request_.linkage_mode;
  AddBaseEvidence(&snapshot.evidence, request_);
  snapshot.evidence.push_back("foreign_memory.handle_active=" + BoolText(active()));
  return snapshot;
}

ForeignMemoryReservationAcquireResult ForeignMemoryReservationLedger::Reserve(
    ForeignMemoryReservationRequest request) {
  if (request.estimated_bytes == 0 && request.observed_bytes != 0) {
    request.estimated_bytes = request.observed_bytes;
  }
  if (request.observed_bytes > request.estimated_bytes) {
    request.estimated_bytes = request.observed_bytes;
  }
  if (request.category == MemoryCategory::unknown) {
    request.category = DefaultForeignMemoryCategory(request.source);
  }
  if (request.memory_class.empty()) {
    request.memory_class = DefaultMemoryClassFor(request.source);
  }
  std::string reason;
  if (!ValidateRequestShape(request, &reason)) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++fail_closed_refusal_count_;
    auto refused = RefuseAcquire(
        request,
        "SB_CEIC_016_FOREIGN_MEMORY_REQUEST_REFUSED",
        "memory.ceic_016.foreign.request_refused",
        std::move(reason));
    return refused;
  }
  if (UnsafeAuthority(request, &reason)) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++fail_closed_refusal_count_;
    auto refused = RefuseAcquire(
        request,
        "SB_CEIC_016_FOREIGN_MEMORY_AUTHORITY_REFUSED",
        "memory.ceic_016.foreign.authority_refused",
        std::move(reason));
    return refused;
  }

  HierarchicalMemoryReservationRequest reservation;
  reservation.scope_chain = request.scope_chain;
  reservation.category = request.category;
  reservation.memory_class = request.memory_class;
  reservation.requested_bytes = request.estimated_bytes;
  reservation.owner_id = request.owner_id;
  reservation.spillable = Spillable(request.over_limit_action);
  reservation.cancelable = Cancelable(request.over_limit_action);
  reservation.weight = 1;
  reservation.provenance = request.provenance;

  auto reserved = request.reservation_ledger->Reserve(std::move(reservation));
  if (!reserved.ok()) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (reserved.status.code == StatusCode::memory_limit_exceeded) {
      ++over_limit_refusal_count_;
      ++source_accounting_[request.source].over_limit_refusal_count;
      ++owning_scope_accounting_[request.owning_scope].over_limit_refusal_count;
    } else {
      ++fail_closed_refusal_count_;
    }
    auto result = RefuseAcquire(
        request,
        "SB_CEIC_016_FOREIGN_MEMORY_RESERVATION_REFUSED",
        "memory.ceic_016.foreign.reservation_refused",
        reserved.diagnostic.diagnostic_code.empty()
            ? "hierarchical_memory_budget_reservation_refused"
            : reserved.diagnostic.diagnostic_code,
        reserved.status.code,
        reserved.status.severity);
    result.diagnostic = reserved.diagnostic;
    result.evidence.push_back(
        "foreign_memory.ledger_recommendation=" +
        std::string(HierarchicalMemoryReservationRecommendationName(
            reserved.recommendation)));
    result.evidence.push_back(
        "foreign_memory.requested_over_limit_action=" +
        std::string(ForeignMemoryOverLimitActionName(request.over_limit_action)));
    result.evidence.push_back(
        "foreign_memory.expected_recommendation=" +
        std::string(HierarchicalMemoryReservationRecommendationName(
            RecommendationFor(request.over_limit_action))));
    return result;
  }

  auto committed = request.reservation_ledger->Commit(reserved.token);
  if (!committed.ok()) {
    (void)request.reservation_ledger->Release(reserved.token);
    std::lock_guard<std::mutex> lock(mutex_);
    ++fail_closed_refusal_count_;
    auto result = RefuseAcquire(
        request,
        "SB_CEIC_016_FOREIGN_MEMORY_COMMIT_REFUSED",
        "memory.ceic_016.foreign.commit_refused",
        committed.diagnostic.diagnostic_code.empty()
            ? "hierarchical_memory_budget_commit_refused"
            : committed.diagnostic.diagnostic_code,
        committed.status.code,
        committed.status.severity);
    result.diagnostic = committed.diagnostic;
    return result;
  }

  const u64 reservation_id =
      next_reservation_id_.fetch_add(1, std::memory_order_relaxed);
  auto state = std::make_shared<ForeignMemoryReservation::HandleState>();
  ForeignMemoryReservationToken token{reservation_id, reserved.token};
  ReservationRecord record;
  record.request = request;
  record.token = token;
  record.observed_bytes = request.observed_bytes;
  record.confidence = request.confidence;
  record.state = state;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    ApplyReservationLocked(record);
    records_[reservation_id] = record;
  }

  ForeignMemoryReservationAcquireResult result;
  result.status = OkStatus();
  result.reservation.reset(
      new ForeignMemoryReservation(this, std::move(request), token, state));
  AddBaseEvidence(&result.evidence, result.reservation->request());
  result.evidence.push_back("foreign_memory.reservation_created=true");
  result.evidence.push_back("foreign_memory.reservation_committed=true");
  result.evidence.push_back("foreign_memory.reservation_id=" +
                            std::to_string(reservation_id));
  result.evidence.push_back("foreign_memory.ledger_token_id=" +
                            std::to_string(reserved.token.token_id));
  result.diagnostic = MakeForeignMemoryDiagnostic(
      result.status,
      "SB_CEIC_016_FOREIGN_MEMORY_OK",
      "memory.ceic_016.foreign.ok",
      {{"source", ForeignMemorySourceName(result.reservation->request().source)},
       {"estimated_bytes",
        std::to_string(result.reservation->request().estimated_bytes)},
       {"observed_bytes",
        std::to_string(result.reservation->request().observed_bytes)}});
  return result;
}

ForeignMemoryReservationReleaseResult
ForeignMemoryReservationLedger::ReleaseReservation(
    ForeignMemoryReservationToken token,
    ForeignMemoryReleaseEvent event) {
  ReservationRecord record;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = records_.find(token.reservation_id);
    if (it == records_.end() ||
        it->second.token.ledger_token.token_id != token.ledger_token.token_id ||
        it->second.token.ledger_token.bytes != token.ledger_token.bytes) {
      ++failed_release_count_;
      return RefuseRelease(token,
                           "SB_CEIC_016_FOREIGN_MEMORY_RELEASE_UNKNOWN",
                           "memory.ceic_016.foreign.release_unknown",
                           it == records_.end() ? "reservation_not_found"
                                                : "reservation_token_mismatch");
    }
    record = it->second;
    records_.erase(it);
  }

  auto released = record.request.reservation_ledger->Release(token.ledger_token);
  if (!released.ok()) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++failed_release_count_;
    records_[token.reservation_id] = record;
    if (record.state != nullptr) {
      record.state->released.store(false);
    }
    auto refused = RefuseRelease(
        token,
        "SB_CEIC_016_FOREIGN_MEMORY_LEDGER_RELEASE_REFUSED",
        "memory.ceic_016.foreign.ledger_release_refused",
        released.diagnostic.diagnostic_code.empty()
            ? "hierarchical_memory_budget_release_refused"
            : released.diagnostic.diagnostic_code,
        released.status.code);
    refused.diagnostic = released.diagnostic;
    return refused;
  }

  ForeignMemoryReservationReleaseResult result;
  result.status = OkStatus();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ApplyReleaseLocked(record, event);
    result.snapshot = SnapshotForRecordLocked(record);
  }
  result.released = true;
  AddBaseEvidence(&result.evidence, record.request);
  result.evidence.push_back("foreign_memory.release_event=" +
                            std::string(ForeignMemoryReleaseEventName(event)));
  result.evidence.push_back("foreign_memory.reservation_released=true");
  result.diagnostic = MakeForeignMemoryDiagnostic(
      result.status,
      "SB_CEIC_016_FOREIGN_MEMORY_RELEASED",
      "memory.ceic_016.foreign.released",
      {{"source", ForeignMemorySourceName(record.request.source)},
       {"reservation_id", std::to_string(token.reservation_id)}});
  return result;
}

ForeignMemoryReservationObservationResult
ForeignMemoryReservationLedger::UpdateObservedBytes(
    ForeignMemoryReservationToken token,
    u64 observed_bytes,
    ForeignMemoryConfidence confidence) {
  if (confidence == ForeignMemoryConfidence::unknown) {
    return RefuseObservation(token,
                             "SB_CEIC_016_FOREIGN_MEMORY_OBSERVATION_CONFIDENCE_REFUSED",
                             "memory.ceic_016.foreign.observation_confidence_refused",
                             "non_unknown_confidence_required");
  }
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = records_.find(token.reservation_id);
  if (it == records_.end()) {
    ++failed_release_count_;
    return RefuseObservation(token,
                             "SB_CEIC_016_FOREIGN_MEMORY_OBSERVATION_UNKNOWN",
                             "memory.ceic_016.foreign.observation_unknown",
                             "reservation_not_found",
                             StatusCode::memory_unknown_pointer);
  }
  if (it->second.token.ledger_token.token_id != token.ledger_token.token_id ||
      it->second.token.ledger_token.bytes != token.ledger_token.bytes) {
    ++failed_release_count_;
    return RefuseObservation(token,
                             "SB_CEIC_016_FOREIGN_MEMORY_OBSERVATION_TOKEN_MISMATCH",
                             "memory.ceic_016.foreign.observation_token_mismatch",
                             "reservation_token_mismatch",
                             StatusCode::memory_unknown_pointer);
  }
  if (observed_bytes > it->second.request.estimated_bytes) {
    ++over_limit_refusal_count_;
    ++source_accounting_[it->second.request.source].over_limit_refusal_count;
    ++owning_scope_accounting_[it->second.request.owning_scope].over_limit_refusal_count;
    return RefuseObservation(
        token,
        "SB_CEIC_016_FOREIGN_MEMORY_OBSERVED_BYTES_EXCEED_RESERVATION",
        "memory.ceic_016.foreign.observed_bytes_exceed_reservation",
        "observed_bytes_exceed_estimated_reservation",
        StatusCode::memory_limit_exceeded);
  }
  ApplyObservedDeltaLocked(&it->second, observed_bytes, confidence);

  ForeignMemoryReservationObservationResult result;
  result.status = OkStatus();
  result.snapshot = SnapshotForRecordLocked(it->second);
  AddBaseEvidence(&result.evidence, it->second.request);
  result.evidence.push_back("foreign_memory.observed_bytes_updated=true");
  result.evidence.push_back("foreign_memory.observed_bytes=" +
                            std::to_string(observed_bytes));
  result.diagnostic = MakeForeignMemoryDiagnostic(
      result.status,
      "SB_CEIC_016_FOREIGN_MEMORY_OBSERVED",
      "memory.ceic_016.foreign.observed",
      {{"source", ForeignMemorySourceName(it->second.request.source)},
       {"observed_bytes", std::to_string(observed_bytes)}});
  return result;
}

ForeignMemoryReservationCleanupResult
ForeignMemoryReservationLedger::CleanupOwner(std::string owner_id) {
  ForeignMemoryReservationCleanupResult cleanup;
  cleanup.status = OkStatus();
  cleanup.evidence.push_back(kEvidenceAnchor);
  cleanup.evidence.push_back(kAuthorityScope);
  cleanup.evidence.push_back("foreign_memory.owner_cleanup.owner_id=" + owner_id);
  if (Blank(owner_id)) {
    cleanup.status = ErrorStatus();
    cleanup.diagnostic = MakeForeignMemoryDiagnostic(
        cleanup.status,
        "SB_CEIC_016_FOREIGN_MEMORY_OWNER_CLEANUP_INVALID",
        "memory.ceic_016.foreign.owner_cleanup_invalid",
        {{"reason", "owner_id_required"}});
    return cleanup;
  }

  std::vector<ForeignMemoryReservationToken> tokens;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : records_) {
      if (entry.second.request.owner_id == owner_id) {
        tokens.push_back(entry.second.token);
      }
    }
  }
  std::sort(tokens.begin(),
            tokens.end(),
            [](const ForeignMemoryReservationToken& lhs,
               const ForeignMemoryReservationToken& rhs) {
              return lhs.reservation_id < rhs.reservation_id;
            });

  for (const auto& token : tokens) {
    u64 estimated = 0;
    u64 observed = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = records_.find(token.reservation_id);
      if (it != records_.end()) {
        estimated = it->second.request.estimated_bytes;
        observed = it->second.observed_bytes;
        if (it->second.state != nullptr) {
          it->second.state->released.store(true);
        }
      }
    }
    auto released = ReleaseReservation(token, ForeignMemoryReleaseEvent::owner_cleanup);
    if (released.ok()) {
      ++cleanup.cleaned_reservation_count;
      cleanup.cleaned_estimated_bytes += estimated;
      cleanup.cleaned_observed_bytes += observed;
    } else {
      cleanup.status = released.status;
      cleanup.diagnostic = released.diagnostic;
      break;
    }
  }
  cleanup.evidence.push_back("foreign_memory.owner_cleanup.cleaned_count=" +
                             std::to_string(cleanup.cleaned_reservation_count));
  cleanup.evidence.push_back(
      "foreign_memory.owner_cleanup.cleaned_estimated_bytes=" +
      std::to_string(cleanup.cleaned_estimated_bytes));
  return cleanup;
}

ForeignMemoryReservationSnapshot ForeignMemoryReservationLedger::Snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  ForeignMemoryReservationSnapshot snapshot;
  snapshot.current_estimated_bytes = current_estimated_bytes_;
  snapshot.peak_estimated_bytes = peak_estimated_bytes_;
  snapshot.current_observed_bytes = current_observed_bytes_;
  snapshot.peak_observed_bytes = peak_observed_bytes_;
  snapshot.reservation_count = reservation_count_;
  snapshot.release_count = release_count_;
  snapshot.cancel_cleanup_count = cancel_cleanup_count_;
  snapshot.owner_cleanup_count = owner_cleanup_count_;
  snapshot.over_limit_refusal_count = over_limit_refusal_count_;
  snapshot.fail_closed_refusal_count = fail_closed_refusal_count_;
  snapshot.failed_release_count = failed_release_count_;
  snapshot.active_reservation_count = static_cast<u64>(records_.size());

  snapshot.sources.reserve(source_accounting_.size());
  for (const auto& entry : source_accounting_) {
    ForeignMemorySourceSnapshot source;
    source.source = entry.first;
    source.active_reservation_count = entry.second.active_reservation_count;
    source.current_estimated_bytes = entry.second.current_estimated_bytes;
    source.peak_estimated_bytes = entry.second.peak_estimated_bytes;
    source.current_observed_bytes = entry.second.current_observed_bytes;
    source.peak_observed_bytes = entry.second.peak_observed_bytes;
    source.reservation_count = entry.second.reservation_count;
    source.release_count = entry.second.release_count;
    source.cancel_cleanup_count = entry.second.cancel_cleanup_count;
    source.owner_cleanup_count = entry.second.owner_cleanup_count;
    source.over_limit_refusal_count = entry.second.over_limit_refusal_count;
    snapshot.sources.push_back(std::move(source));
  }

  snapshot.owning_scopes.reserve(owning_scope_accounting_.size());
  for (const auto& entry : owning_scope_accounting_) {
    ForeignMemoryOwningScopeSnapshot scope;
    scope.owning_scope = entry.first;
    scope.active_reservation_count = entry.second.active_reservation_count;
    scope.current_estimated_bytes = entry.second.current_estimated_bytes;
    scope.peak_estimated_bytes = entry.second.peak_estimated_bytes;
    scope.current_observed_bytes = entry.second.current_observed_bytes;
    scope.peak_observed_bytes = entry.second.peak_observed_bytes;
    scope.reservation_count = entry.second.reservation_count;
    scope.release_count = entry.second.release_count;
    scope.cancel_cleanup_count = entry.second.cancel_cleanup_count;
    scope.owner_cleanup_count = entry.second.owner_cleanup_count;
    scope.over_limit_refusal_count = entry.second.over_limit_refusal_count;
    snapshot.owning_scopes.push_back(std::move(scope));
  }

  snapshot.active_reservations.reserve(records_.size());
  for (const auto& entry : records_) {
    snapshot.active_reservations.push_back(SnapshotForRecordLocked(entry.second));
  }
  return snapshot;
}

void ForeignMemoryReservationLedger::ApplyReservationLocked(
    const ReservationRecord& record) {
  const u64 estimated = record.request.estimated_bytes;
  const u64 observed = record.observed_bytes;
  current_estimated_bytes_ += estimated;
  current_observed_bytes_ += observed;
  UpdatePeak(current_estimated_bytes_, &peak_estimated_bytes_);
  UpdatePeak(current_observed_bytes_, &peak_observed_bytes_);
  ++reservation_count_;

  auto& source = source_accounting_[record.request.source];
  ++source.active_reservation_count;
  source.current_estimated_bytes += estimated;
  source.current_observed_bytes += observed;
  UpdatePeak(source.current_estimated_bytes, &source.peak_estimated_bytes);
  UpdatePeak(source.current_observed_bytes, &source.peak_observed_bytes);
  ++source.reservation_count;

  auto& scope = owning_scope_accounting_[record.request.owning_scope];
  ++scope.active_reservation_count;
  scope.current_estimated_bytes += estimated;
  scope.current_observed_bytes += observed;
  UpdatePeak(scope.current_estimated_bytes, &scope.peak_estimated_bytes);
  UpdatePeak(scope.current_observed_bytes, &scope.peak_observed_bytes);
  ++scope.reservation_count;
}

void ForeignMemoryReservationLedger::ApplyObservedDeltaLocked(
    ReservationRecord* record,
    u64 observed_bytes,
    ForeignMemoryConfidence confidence) {
  const u64 previous = record->observed_bytes;
  if (observed_bytes >= previous) {
    const u64 delta = observed_bytes - previous;
    current_observed_bytes_ += delta;
    source_accounting_[record->request.source].current_observed_bytes += delta;
    owning_scope_accounting_[record->request.owning_scope].current_observed_bytes += delta;
  } else {
    const u64 delta = previous - observed_bytes;
    current_observed_bytes_ =
        current_observed_bytes_ >= delta ? current_observed_bytes_ - delta : 0;
    auto& source = source_accounting_[record->request.source];
    source.current_observed_bytes =
        source.current_observed_bytes >= delta
            ? source.current_observed_bytes - delta
            : 0;
    auto& scope = owning_scope_accounting_[record->request.owning_scope];
    scope.current_observed_bytes =
        scope.current_observed_bytes >= delta
            ? scope.current_observed_bytes - delta
            : 0;
  }
  UpdatePeak(current_observed_bytes_, &peak_observed_bytes_);
  auto& source = source_accounting_[record->request.source];
  UpdatePeak(source.current_observed_bytes, &source.peak_observed_bytes);
  auto& scope = owning_scope_accounting_[record->request.owning_scope];
  UpdatePeak(scope.current_observed_bytes, &scope.peak_observed_bytes);
  record->observed_bytes = observed_bytes;
  record->confidence = confidence;
  record->request.observed_bytes = observed_bytes;
  record->request.confidence = confidence;
}

void ForeignMemoryReservationLedger::ApplyReleaseLocked(
    const ReservationRecord& record,
    ForeignMemoryReleaseEvent event) {
  const u64 estimated = record.request.estimated_bytes;
  const u64 observed = record.observed_bytes;
  current_estimated_bytes_ =
      current_estimated_bytes_ >= estimated ? current_estimated_bytes_ - estimated : 0;
  current_observed_bytes_ =
      current_observed_bytes_ >= observed ? current_observed_bytes_ - observed : 0;
  ++release_count_;
  if (event == ForeignMemoryReleaseEvent::cancel_cleanup) {
    ++cancel_cleanup_count_;
  } else if (event == ForeignMemoryReleaseEvent::owner_cleanup) {
    ++owner_cleanup_count_;
  }

  auto& source = source_accounting_[record.request.source];
  if (source.active_reservation_count != 0) {
    --source.active_reservation_count;
  }
  source.current_estimated_bytes =
      source.current_estimated_bytes >= estimated
          ? source.current_estimated_bytes - estimated
          : 0;
  source.current_observed_bytes =
      source.current_observed_bytes >= observed
          ? source.current_observed_bytes - observed
          : 0;
  ++source.release_count;
  if (event == ForeignMemoryReleaseEvent::cancel_cleanup) {
    ++source.cancel_cleanup_count;
  } else if (event == ForeignMemoryReleaseEvent::owner_cleanup) {
    ++source.owner_cleanup_count;
  }

  auto& scope = owning_scope_accounting_[record.request.owning_scope];
  if (scope.active_reservation_count != 0) {
    --scope.active_reservation_count;
  }
  scope.current_estimated_bytes =
      scope.current_estimated_bytes >= estimated
          ? scope.current_estimated_bytes - estimated
          : 0;
  scope.current_observed_bytes =
      scope.current_observed_bytes >= observed
          ? scope.current_observed_bytes - observed
          : 0;
  ++scope.release_count;
  if (event == ForeignMemoryReleaseEvent::cancel_cleanup) {
    ++scope.cancel_cleanup_count;
  } else if (event == ForeignMemoryReleaseEvent::owner_cleanup) {
    ++scope.owner_cleanup_count;
  }
}

ForeignMemoryActiveReservationSnapshot
ForeignMemoryReservationLedger::SnapshotForRecordLocked(
    const ReservationRecord& record) const {
  ForeignMemoryActiveReservationSnapshot snapshot;
  snapshot.token = record.token;
  snapshot.source = record.request.source;
  snapshot.category = record.request.category;
  snapshot.owner_id = record.request.owner_id;
  snapshot.owning_scope = record.request.owning_scope;
  snapshot.operation_id = record.request.operation_id;
  snapshot.memory_class = record.request.memory_class;
  snapshot.estimated_bytes = record.request.estimated_bytes;
  snapshot.observed_bytes = record.observed_bytes;
  snapshot.confidence = record.confidence;
  snapshot.expected_release_event = record.request.expected_release_event;
  snapshot.over_limit_action = record.request.over_limit_action;
  snapshot.linkage_mode = record.request.linkage_mode;
  AddBaseEvidence(&snapshot.evidence, record.request);
  return snapshot;
}

ForeignMemoryReservationAcquireResult AcquireForeignMemoryReservation(
    ForeignMemoryReservationLedger* ledger,
    ForeignMemoryReservationRequest request) {
  if (ledger == nullptr) {
    return RefuseAcquire(
        request,
        "SB_CEIC_016_FOREIGN_MEMORY_LEDGER_REQUIRED",
        "memory.ceic_016.foreign.ledger_required",
        "foreign_memory_reservation_ledger_required");
  }
  return ledger->Reserve(std::move(request));
}

}  // namespace scratchbird::core::memory
