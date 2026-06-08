// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "page_reservation.hpp"

#include "database_format.hpp"
#include "metric_producer.hpp"
#include "page_header.hpp"
#include "page_registry.hpp"

#include <algorithm>
#include <utility>
#include <vector>

namespace scratchbird::storage::page {
namespace {

namespace filespace = scratchbird::storage::filespace;

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::uuid::GenerateEngineIdentityV7;
using scratchbird::core::uuid::IsEngineIdentityUuid;

Status ReservationOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::storage_page};
}

Status ReservationErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::storage_page};
}

bool IsTypedEngineIdentity(const TypedUuid& uuid, UuidKind kind) {
  return uuid.kind == kind && uuid.valid() && IsEngineIdentityUuid(uuid.value);
}

bool IsOptionalObjectIdentity(const TypedUuid& uuid) {
  return !uuid.valid() || IsTypedEngineIdentity(uuid, UuidKind::object);
}

bool IsKnownInsertPageFamily(const std::string& page_family) {
  return IsKnownPageFamilyName(page_family) || page_family == "overflow" || page_family == "toast";
}

bool StartupModeAllowsReservation(InsertReservationStartupMode mode) {
  return mode == InsertReservationStartupMode::normal;
}

u64 UsableBytesPerPage(u32 page_size) {
  if (page_size <= scratchbird::storage::disk::kPageHeaderSerializedBytes + 256) {
    return 0;
  }
  return static_cast<u64>(page_size - scratchbird::storage::disk::kPageHeaderSerializedBytes - 256);
}

u64 EstimateReservedPages(const InsertPageReservationRequest& request) {
  const u64 usable = UsableBytesPerPage(request.page_size);
  if (usable == 0) {
    return 0;
  }
  const u64 payload_pages = request.estimated_payload_bytes == 0
                                ? 1
                                : ((request.estimated_payload_bytes + usable - 1) / usable);
  const u64 row_pages = request.estimated_row_count == 0
                            ? 0
                            : ((request.estimated_row_count + 127) / 128);
  return std::max<u64>(1, std::max(payload_pages, row_pages));
}

TypedUuid ReservationIdentity(PageReservationLedger* ledger, const InsertPageReservationRequest& request) {
  if (IsTypedEngineIdentity(request.request_id, UuidKind::object)) {
    return request.request_id;
  }
  const u64 seed = request.current_time_authority_tick + (ledger == nullptr ? 1 : ledger->next_evidence_sequence);
  const auto generated = GenerateEngineIdentityV7(UuidKind::object, seed == 0 ? 1 : seed);
  return generated.ok() ? generated.value : TypedUuid{};
}

void EmitMetric(const std::string& family, const std::string& result, const std::string& reason, const std::string& page_family) {
  (void)scratchbird::core::metrics::IncrementCounter(
      family,
      scratchbird::core::metrics::Labels({{"component", "storage.page_allocation"},
                                          {"result", result},
                                          {"reason", reason},
                                          {"page_family", page_family}}),
      1.0,
      "engine_insert");
}

InsertPageReservationResult ErrorResult(PageReservationLedger* ledger,
                                        const InsertPageReservationRequest& request,
                                        std::string diagnostic_code,
                                        std::string message_key,
                                        std::string detail = {}) {
  EmitMetric("sb_page_reservation_refused_total", "refused", diagnostic_code, request.page_family);
  InsertPageReservationResult result;
  result.status = ReservationErrorStatus();
  result.diagnostic = MakePageReservationDiagnostic(result.status,
                                                    std::move(diagnostic_code),
                                                    std::move(message_key),
                                                    std::move(detail));
  if (ledger != nullptr) {
    PageReservationEvidenceRecord evidence;
    evidence.sequence = ledger->next_evidence_sequence++;
    evidence.action = "reserve_refuse";
    evidence.database_uuid = request.database_uuid;
    evidence.transaction_uuid = request.transaction_uuid;
    evidence.local_transaction_id = request.local_transaction_id;
    evidence.object_uuid = request.object_uuid;
    evidence.filespace_uuid = request.preferred_filespace_uuid;
    evidence.page_family = request.page_family;
    evidence.filespace_class = "forbidden";
    evidence.filespace_class_reason = result.diagnostic.diagnostic_code;
    evidence.previous_state = PageReservationState::absent;
    evidence.new_state = PageReservationState::absent;
    evidence.reason = result.diagnostic.diagnostic_code;
    evidence.diagnostic_code = result.diagnostic.diagnostic_code;
    evidence.durable_state_changed = false;
    ledger->evidence.push_back(evidence);
    result.evidence = evidence;
  }
  return result;
}

PageReservationMutationResult MutationError(std::string diagnostic_code,
                                            std::string message_key,
                                            std::string detail = {}) {
  PageReservationMutationResult result;
  result.status = ReservationErrorStatus();
  result.diagnostic = MakePageReservationDiagnostic(result.status,
                                                    std::move(diagnostic_code),
                                                    std::move(message_key),
                                                    std::move(detail));
  return result;
}

PageReservationEvidenceRecord MakeEvidence(PageReservationLedger* ledger,
                                           const PageReservationEntry& before,
                                           const PageReservationEntry& after,
                                           std::string action,
                                           std::string reason,
                                           std::string diagnostic_code) {
  PageReservationEvidenceRecord evidence;
  evidence.sequence = ledger == nullptr ? 0 : ledger->next_evidence_sequence++;
  evidence.action = std::move(action);
  evidence.reservation_id = after.reservation_id;
  evidence.database_uuid = after.database_uuid;
  evidence.transaction_uuid = after.transaction_uuid;
  evidence.local_transaction_id = after.local_transaction_id;
  evidence.object_uuid = after.object_uuid;
  evidence.filespace_uuid = after.filespace_uuid;
  evidence.page_family = after.page_family;
  evidence.filespace_class = after.filespace_class.empty() ? before.filespace_class : after.filespace_class;
  evidence.filespace_class_reason = after.filespace_class_reason.empty()
                                        ? before.filespace_class_reason
                                        : after.filespace_class_reason;
  evidence.previous_state = before.state;
  evidence.new_state = after.state;
  evidence.reserved_page_count = after.reserved_page_count;
  evidence.consumed_page_count = after.consumed_page_count;
  evidence.released_page_count = after.released_page_count;
  evidence.expires_at_time_authority_tick = after.expires_at_time_authority_tick;
  evidence.reason = std::move(reason);
  evidence.diagnostic_code = std::move(diagnostic_code);
  evidence.durable_state_changed = true;
  if (ledger != nullptr) {
    ledger->evidence.push_back(evidence);
  }
  return evidence;
}

filespace::FilespaceClassDecision ResolveReservationFilespaceClass(
    const InsertPageReservationRequest& request) {
  filespace::FilespaceClassRequest class_request;
  class_request.database_uuid = request.database_uuid;
  class_request.filespace_uuid = request.preferred_filespace_uuid;
  class_request.owner_object_uuid = request.object_uuid;
  class_request.object_class = request.object_class;
  class_request.page_family = request.page_family;
  class_request.reason = "insert_page_reservation";
  class_request.explicit_object_class =
      request.object_class != filespace::FilespaceObjectClass::unspecified;
  return filespace::ResolveFilespaceClass(class_request);
}

void ApplyFilespaceClassEvidence(PageReservationEvidenceRecord* evidence,
                                 const filespace::FilespaceClassDecision& decision) {
  if (evidence == nullptr || !decision.ok()) {
    return;
  }
  evidence->filespace_class = filespace::FilespaceClassName(decision.filespace_class);
  evidence->filespace_class_reason = decision.diagnostic.diagnostic_code;
  if (!evidence->reason.empty()) {
    evidence->reason += ";";
  }
  evidence->reason += "filespace_class=";
  evidence->reason += filespace::FilespaceClassName(decision.filespace_class);
  evidence->reason += ";object_class=";
  evidence->reason += filespace::FilespaceObjectClassName(decision.object_class);
  evidence->reason += ";visibility_authority=false";
  evidence->reason += ";mga_visibility_authority=durable_transaction_inventory";
}

PageReservationEntry* FindMutable(PageReservationLedger* ledger, const TypedUuid& reservation_id) {
  if (ledger == nullptr) {
    return nullptr;
  }
  for (auto& reservation : ledger->reservations) {
    if (reservation.reservation_id.value == reservation_id.value) {
      return &reservation;
    }
  }
  return nullptr;
}

}  // namespace

const char* InsertReservationStartupModeName(InsertReservationStartupMode mode) {
  switch (mode) {
    case InsertReservationStartupMode::normal: return "normal";
    case InsertReservationStartupMode::read_only: return "read_only";
    case InsertReservationStartupMode::restricted_open: return "restricted_open";
    case InsertReservationStartupMode::recovery_unsafe: return "recovery_unsafe";
    case InsertReservationStartupMode::maintenance: return "maintenance";
    case InsertReservationStartupMode::shutdown: return "shutdown";
  }
  return "unknown";
}

const char* PageReservationStateName(PageReservationState state) {
  switch (state) {
    case PageReservationState::absent: return "absent";
    case PageReservationState::durable_unconsumed: return "durable_unconsumed";
    case PageReservationState::partially_consumed: return "partially_consumed";
    case PageReservationState::consumed: return "consumed";
    case PageReservationState::released: return "released";
    case PageReservationState::expired: return "expired";
    case PageReservationState::quarantine: return "quarantine";
  }
  return "unknown";
}

const char* PageReservationRecoveryActionName(PageReservationRecoveryAction action) {
  switch (action) {
    case PageReservationRecoveryAction::no_action: return "no_action";
    case PageReservationRecoveryAction::release: return "release";
    case PageReservationRecoveryAction::retain: return "retain";
    case PageReservationRecoveryAction::quarantine: return "quarantine";
    case PageReservationRecoveryAction::fail_closed: return "fail_closed";
  }
  return "unknown";
}

InsertPageReservationResult ReserveInsertPagesDurable(PageReservationLedger* ledger,
                                                      const InsertPageReservationRequest& request) {
  if (ledger == nullptr) {
    return ErrorResult(nullptr,
                       request,
                       "insert_page_reservation_ledger_required",
                       "storage.page_reservation.ledger_required");
  }
  if (!IsTypedEngineIdentity(request.database_uuid, UuidKind::database)) {
    return ErrorResult(ledger,
                       request,
                       "insert_page_reservation_database_uuid_invalid",
                       "storage.page_reservation.database_uuid_invalid");
  }
  if (!IsTypedEngineIdentity(request.transaction_uuid, UuidKind::transaction)) {
    return ErrorResult(ledger,
                       request,
                       "insert_page_reservation_transaction_uuid_invalid",
                       "storage.page_reservation.transaction_uuid_invalid");
  }
  if (request.local_transaction_id == 0) {
    return ErrorResult(ledger,
                       request,
                       "insert_page_reservation_local_tx_required",
                       "storage.page_reservation.local_tx_required");
  }
  if (!IsTypedEngineIdentity(request.object_uuid, UuidKind::object)) {
    return ErrorResult(ledger,
                       request,
                       "insert_page_reservation_object_uuid_invalid",
                       "storage.page_reservation.object_uuid_invalid");
  }
  if (!IsTypedEngineIdentity(request.preferred_filespace_uuid, UuidKind::filespace)) {
    return ErrorResult(ledger,
                       request,
                       "insert_page_reservation_filespace_unavailable",
                       "storage.page_reservation.filespace_unavailable");
  }
  if (!IsOptionalObjectIdentity(request.policy_uuid)) {
    return ErrorResult(ledger,
                       request,
                       "insert_page_reservation_policy_uuid_invalid",
                       "storage.page_reservation.policy_uuid_invalid");
  }
  if (!IsKnownInsertPageFamily(request.page_family)) {
    return ErrorResult(ledger,
                       request,
                       "insert_page_reservation_unknown_page_family",
                       "storage.page_reservation.unknown_page_family",
                       request.page_family);
  }
  if (!scratchbird::storage::disk::IsSupportedDatabasePageSize(request.page_size)) {
    return ErrorResult(ledger,
                       request,
                       "insert_page_reservation_page_size_invalid",
                       "storage.page_reservation.page_size_invalid",
                       std::to_string(request.page_size));
  }
  if (!StartupModeAllowsReservation(request.startup_mode)) {
    return ErrorResult(ledger,
                       request,
                       "insert_page_reservation_unsafe_startup_mode",
                       "storage.page_reservation.unsafe_startup_mode",
                       InsertReservationStartupModeName(request.startup_mode));
  }
  if (request.estimated_row_count == 0 && request.estimated_payload_bytes == 0) {
    return ErrorResult(ledger,
                       request,
                       "insert_page_reservation_estimate_required",
                       "storage.page_reservation.estimate_required");
  }
  const u64 reserved_pages = EstimateReservedPages(request);
  const u64 usable = UsableBytesPerPage(request.page_size);
  if (reserved_pages == 0 || usable == 0) {
    return ErrorResult(ledger,
                       request,
                       "insert_page_reservation_page_size_invalid",
                       "storage.page_reservation.page_size_invalid");
  }
  const auto filespace_class = ResolveReservationFilespaceClass(request);
  if (!filespace_class.ok()) {
    return ErrorResult(ledger,
                       request,
                       filespace_class.diagnostic.diagnostic_code,
                       filespace_class.diagnostic.message_key);
  }

  PageReservationEntry entry;
  entry.reservation_id = ReservationIdentity(ledger, request);
  if (!IsTypedEngineIdentity(entry.reservation_id, UuidKind::object)) {
    return ErrorResult(ledger,
                       request,
                       "insert_page_reservation_id_generation_failed",
                       "storage.page_reservation.id_generation_failed");
  }
  entry.database_uuid = request.database_uuid;
  entry.transaction_uuid = request.transaction_uuid;
  entry.local_transaction_id = request.local_transaction_id;
  entry.object_uuid = request.object_uuid;
  entry.filespace_uuid = request.preferred_filespace_uuid;
  entry.policy_uuid = request.policy_uuid;
  entry.page_family = request.page_family;
  entry.filespace_class = filespace::FilespaceClassName(filespace_class.filespace_class);
  entry.filespace_class_reason = filespace_class.diagnostic.diagnostic_code;
  entry.state = PageReservationState::durable_unconsumed;
  entry.reserved_page_count = reserved_pages;
  entry.usable_free_bytes_estimate = reserved_pages * usable;
  entry.expires_at_time_authority_tick = request.current_time_authority_tick + request.lease_duration_ticks;

  PageReservationEntry before;
  before.state = PageReservationState::absent;
  ledger->reservations.push_back(entry);
  PageReservationEvidenceRecord evidence = MakeEvidence(ledger,
                                                        before,
                                                        entry,
                                                        "reserve_admit",
                                                        "admitted",
                                                        "insert_page_reservation_admitted");
  ApplyFilespaceClassEvidence(&evidence, filespace_class);
  if (!ledger->evidence.empty()) {
    ApplyFilespaceClassEvidence(&ledger->evidence.back(), filespace_class);
  }
  EmitMetric("sb_page_reservation_admitted_total", "ok", "admitted", request.page_family);

  InsertPageReservationResult result;
  result.status = ReservationOkStatus();
  result.admitted = true;
  result.reservation = entry;
  result.evidence = evidence;
  return result;
}

PageReservationMutationResult ConsumeInsertPageReservation(PageReservationLedger* ledger,
                                                           const ConsumePageReservationRequest& request) {
  if (ledger == nullptr) {
    return MutationError("insert_page_reservation_ledger_required", "storage.page_reservation.ledger_required");
  }
  auto* reservation = FindMutable(ledger, request.reservation_id);
  if (reservation == nullptr) {
    return MutationError("insert_page_reservation_not_found", "storage.page_reservation.not_found");
  }
  if (request.pages_to_consume == 0) {
    return MutationError("insert_page_reservation_consume_count_required", "storage.page_reservation.consume_count_required");
  }
  if (reservation->state != PageReservationState::durable_unconsumed &&
      reservation->state != PageReservationState::partially_consumed) {
    return MutationError("insert_page_reservation_not_consumable", "storage.page_reservation.not_consumable");
  }
  if (reservation->consumed_page_count + request.pages_to_consume > reservation->reserved_page_count) {
    return MutationError("insert_page_reservation_consume_exceeds_reserved", "storage.page_reservation.consume_exceeds_reserved");
  }

  const PageReservationEntry before = *reservation;
  reservation->consumed_page_count += request.pages_to_consume;
  reservation->state = reservation->consumed_page_count == reservation->reserved_page_count
                           ? PageReservationState::consumed
                           : PageReservationState::partially_consumed;
  const auto evidence = MakeEvidence(ledger,
                                     before,
                                     *reservation,
                                     "reserve_consume",
                                     request.reason.empty() ? "consume" : request.reason,
                                     "insert_page_reservation_consumed");
  EmitMetric("sb_page_reservation_consumed_total", "ok", "consume", reservation->page_family);

  PageReservationMutationResult result;
  result.status = ReservationOkStatus();
  result.changed = true;
  result.reservation = *reservation;
  result.evidence = evidence;
  return result;
}

PageReservationMutationResult ReleaseInsertPageReservation(PageReservationLedger* ledger,
                                                           const ReleasePageReservationRequest& request) {
  if (ledger == nullptr) {
    return MutationError("insert_page_reservation_ledger_required", "storage.page_reservation.ledger_required");
  }
  auto* reservation = FindMutable(ledger, request.reservation_id);
  if (reservation == nullptr) {
    return MutationError("insert_page_reservation_not_found", "storage.page_reservation.not_found");
  }
  if (reservation->state == PageReservationState::released ||
      reservation->state == PageReservationState::consumed) {
    PageReservationMutationResult result;
    result.status = ReservationOkStatus();
    result.changed = false;
    result.reservation = *reservation;
    return result;
  }
  if (reservation->state == PageReservationState::quarantine) {
    return MutationError("insert_page_reservation_quarantined", "storage.page_reservation.quarantined");
  }

  const PageReservationEntry before = *reservation;
  reservation->released_page_count = reservation->reserved_page_count - reservation->consumed_page_count;
  reservation->state = PageReservationState::released;
  const auto evidence = MakeEvidence(ledger,
                                     before,
                                     *reservation,
                                     "reserve_release",
                                     request.reason.empty() ? "release" : request.reason,
                                     "insert_page_reservation_released");
  EmitMetric("sb_page_reservation_released_total", "ok", "release", reservation->page_family);

  PageReservationMutationResult result;
  result.status = ReservationOkStatus();
  result.changed = true;
  result.reservation = *reservation;
  result.evidence = evidence;
  return result;
}

std::vector<PageReservationMutationResult> ExpireInsertPageReservations(PageReservationLedger* ledger,
                                                                        const ExpirePageReservationsRequest& request) {
  std::vector<PageReservationMutationResult> results;
  if (ledger == nullptr) {
    results.push_back(MutationError("insert_page_reservation_ledger_required", "storage.page_reservation.ledger_required"));
    return results;
  }
  for (auto& reservation : ledger->reservations) {
    if ((reservation.state == PageReservationState::durable_unconsumed ||
         reservation.state == PageReservationState::partially_consumed) &&
        request.current_time_authority_tick >= reservation.expires_at_time_authority_tick) {
      const PageReservationEntry before = reservation;
      reservation.released_page_count = reservation.reserved_page_count - reservation.consumed_page_count;
      reservation.state = PageReservationState::expired;
      const auto evidence = MakeEvidence(ledger,
                                         before,
                                         reservation,
                                         "reserve_expire",
                                         request.reason.empty() ? "expired" : request.reason,
                                         "insert_page_reservation_expired");
      EmitMetric("sb_page_reservation_expired_total", "ok", "expired", reservation.page_family);
      PageReservationMutationResult result;
      result.status = ReservationOkStatus();
      result.changed = true;
      result.reservation = reservation;
      result.evidence = evidence;
      results.push_back(result);
    }
  }
  return results;
}

PageReservationRecoveryClassification ClassifyPageReservationForRecovery(const PageReservationEntry& reservation) {
  PageReservationRecoveryClassification classification;
  classification.reservation_id = reservation.reservation_id;
  classification.observed_state = reservation.state;
  switch (reservation.state) {
    case PageReservationState::absent:
    case PageReservationState::released:
    case PageReservationState::consumed:
    case PageReservationState::expired:
      classification.action = PageReservationRecoveryAction::no_action;
      classification.stable_reason = "terminal_or_no_reservation";
      return classification;
    case PageReservationState::durable_unconsumed:
      classification.action = PageReservationRecoveryAction::release;
      classification.stable_reason = "durable_unconsumed_release_required";
      return classification;
    case PageReservationState::partially_consumed:
      classification.action = PageReservationRecoveryAction::retain;
      classification.stable_reason = "partially_consumed_follows_transaction_horizon";
      return classification;
    case PageReservationState::quarantine:
      classification.action = PageReservationRecoveryAction::quarantine;
      classification.fail_closed = true;
      classification.stable_reason = "quarantined_reservation";
      return classification;
  }
  classification.action = PageReservationRecoveryAction::fail_closed;
  classification.fail_closed = true;
  classification.stable_reason = "unknown_reservation_state";
  return classification;
}

PageReservationRecoveryResult ClassifyPageReservationLedgerForRecovery(const PageReservationLedger& ledger) {
  PageReservationRecoveryResult result;
  result.status = ReservationOkStatus();
  for (const auto& reservation : ledger.reservations) {
    result.classifications.push_back(ClassifyPageReservationForRecovery(reservation));
  }
  return result;
}

const PageReservationEntry* FindPageReservation(const PageReservationLedger& ledger,
                                                const TypedUuid& reservation_id) {
  for (const auto& reservation : ledger.reservations) {
    if (reservation.reservation_id.value == reservation_id.value) {
      return &reservation;
    }
  }
  return nullptr;
}

DiagnosticRecord MakePageReservationDiagnostic(Status status,
                                               std::string diagnostic_code,
                                               std::string message_key,
                                               std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", detail});
  }
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "storage.page.reservation");
}

}  // namespace scratchbird::storage::page
