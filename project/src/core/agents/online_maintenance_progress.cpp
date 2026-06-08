// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "online_maintenance_progress.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>

namespace scratchbird::core::agents {
namespace {

bool IsTerminal(OnlineMaintenancePhase phase) {
  return phase == OnlineMaintenancePhase::published ||
         phase == OnlineMaintenancePhase::completed ||
         phase == OnlineMaintenancePhase::failed_closed;
}

bool IsPublishCandidate(OnlineMaintenanceOperationKind kind) {
  return kind == OnlineMaintenanceOperationKind::sorted_index_build ||
         kind == OnlineMaintenanceOperationKind::index_rebuild ||
         kind == OnlineMaintenanceOperationKind::optimized_backfill ||
         kind == OnlineMaintenanceOperationKind::generation_publish ||
         kind == OnlineMaintenanceOperationKind::vector_adaptive_tuning ||
         kind == OnlineMaintenanceOperationKind::vector_retrain ||
         kind == OnlineMaintenanceOperationKind::vector_rebuild;
}

std::string BoolText(bool value) { return value ? "true" : "false"; }

u64 PercentBasisPoints(u64 completed, u64 total) {
  if (total == 0) {
    return 0;
  }
  if (completed >= total) {
    return 10000;
  }
  return (completed * 10000) / total;
}

void RefreshDerivedFields(OnlineMaintenanceRecord* record) {
  if (record == nullptr) {
    return;
  }
  auto& snapshot = record->snapshot;
  snapshot.percent_basis_points =
      PercentBasisPoints(snapshot.completed_units, snapshot.total_units);
  snapshot.cancel_requested =
      snapshot.phase == OnlineMaintenancePhase::cancel_requested;
  snapshot.publish_ready =
      snapshot.phase == OnlineMaintenancePhase::publish_ready;
  snapshot.published_visible =
      snapshot.phase == OnlineMaintenancePhase::published &&
      !snapshot.partial_publish_visible &&
      record->publication_fence_persisted &&
      record->authoritative_generation_validated &&
      snapshot.engine_mga_authoritative;
  snapshot.resumable =
      (snapshot.phase == OnlineMaintenancePhase::cancelled ||
       snapshot.phase == OnlineMaintenancePhase::resumable) &&
      snapshot.durable_checkpoint_persisted &&
      !snapshot.partial_publish_visible;
}

void AddEvidence(OnlineMaintenanceProgressSnapshot* snapshot,
                 std::string key,
                 std::string value) {
  if (snapshot == nullptr) {
    return;
  }
  snapshot->evidence.push_back({std::move(key), std::move(value)});
}

void AddCommonEvidence(OnlineMaintenanceRecord* record,
                       std::string diagnostic_code) {
  if (record == nullptr) {
    return;
  }
  auto& snapshot = record->snapshot;
  snapshot.diagnostic_code = diagnostic_code;
  AddEvidence(&snapshot, "online_maintenance_control",
              "odf121_progress_cancel_resume_v1");
  AddEvidence(&snapshot, "operation_kind",
              OnlineMaintenanceOperationKindName(snapshot.kind));
  AddEvidence(&snapshot, "phase", OnlineMaintenancePhaseName(snapshot.phase));
  AddEvidence(&snapshot, "diagnostic_code", diagnostic_code);
  AddEvidence(&snapshot, "authority_source",
              "durable_mga_transaction_inventory");
  AddEvidence(&snapshot, "parser_finality_authority", "false");
  AddEvidence(&snapshot, "client_state_authority", "false");
  AddEvidence(&snapshot, "donor_finality_authority", "false");
  AddEvidence(&snapshot, "timestamp_ordering_authority", "false");
  AddEvidence(&snapshot, "uuid_ordering_authority", "false");
  AddEvidence(&snapshot, "write_ahead_recovery_authority", "false");
  AddEvidence(&snapshot, "support_bundle_event",
              "online_maintenance_lifecycle");
  AddEvidence(&snapshot, "observability_metric_family",
              "sys.metrics.online_maintenance");
}

void RecordStoreEvidence(OnlineMaintenanceStateStore* store,
                         const OnlineMaintenanceRecord& record,
                         std::string_view event) {
  if (store == nullptr) {
    return;
  }
  store->AddEvidence("event", std::string(event));
  store->AddEvidence("operation_uuid", record.snapshot.operation_uuid);
  store->AddEvidence("kind",
                     OnlineMaintenanceOperationKindName(record.snapshot.kind));
  store->AddEvidence("phase",
                     OnlineMaintenancePhaseName(record.snapshot.phase));
  store->AddEvidence("diagnostic_code", record.snapshot.diagnostic_code);
  store->AddEvidence("checkpoint_generation",
                     std::to_string(record.snapshot.checkpoint_generation));
}

OnlineMaintenanceResult MakeResult(OnlineMaintenanceRecord record,
                                   OnlineMaintenanceDecision decision,
                                   std::string diagnostic_code,
                                   bool fail_closed,
                                   std::string detail = {}) {
  record.snapshot.diagnostic_code = diagnostic_code;
  RefreshDerivedFields(&record);
  AddCommonEvidence(&record, diagnostic_code);
  if (!detail.empty()) {
    AddEvidence(&record.snapshot, "detail", std::move(detail));
  }

  OnlineMaintenanceResult result;
  result.status = fail_closed ? AgentError(diagnostic_code)
                              : AgentOk();
  result.decision = decision;
  result.record = record;
  result.snapshot = record.snapshot;
  result.fail_closed = fail_closed;
  return result;
}

OnlineMaintenanceResult RefuseMissingStore(std::string diagnostic_code) {
  OnlineMaintenanceRecord record;
  record.snapshot.phase = OnlineMaintenancePhase::failed_closed;
  return MakeResult(std::move(record),
                    OnlineMaintenanceDecision::refused,
                    std::move(diagnostic_code),
                    true);
}

OnlineMaintenanceResult RefuseExisting(OnlineMaintenanceStateStore* store,
                                       OnlineMaintenanceRecord* record,
                                       std::string diagnostic_code,
                                       std::string detail = {}) {
  if (record == nullptr) {
    return RefuseMissingStore(std::move(diagnostic_code));
  }
  record->snapshot.phase = OnlineMaintenancePhase::failed_closed;
  record->snapshot.published_visible = false;
  record->snapshot.partial_publish_visible = false;
  RefreshDerivedFields(record);
  OnlineMaintenanceRecord updated = *record;
  if (store != nullptr) {
    store->Upsert(updated);
    RecordStoreEvidence(store, updated, "fail_closed");
  }
  return MakeResult(std::move(updated),
                    OnlineMaintenanceDecision::refused,
                    std::move(diagnostic_code),
                    true,
                    std::move(detail));
}

std::string Escape(std::string_view input) {
  std::string out;
  for (char ch : input) {
    switch (ch) {
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '=': out += "\\e"; break;
      default: out.push_back(ch); break;
    }
  }
  return out;
}

std::string Unescape(std::string_view input) {
  std::string out;
  bool escaped = false;
  for (char ch : input) {
    if (!escaped) {
      if (ch == '\\') {
        escaped = true;
      } else {
        out.push_back(ch);
      }
      continue;
    }
    switch (ch) {
      case '\\': out.push_back('\\'); break;
      case 'n': out.push_back('\n'); break;
      case 'e': out.push_back('='); break;
      default: out.push_back(ch); break;
    }
    escaped = false;
  }
  if (escaped) {
    out.push_back('\\');
  }
  return out;
}

void WriteField(std::ostringstream* out,
                std::string_view key,
                std::string_view value) {
  *out << key << '=' << Escape(value) << '\n';
}

bool ParseBool(const std::string& value) { return value == "true" || value == "1"; }

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() &&
         value.substr(0, prefix.size()) == prefix;
}

u64 Fnv1a64(std::string_view value) {
  u64 hash = 1469598103934665603ull;
  for (unsigned char c : value) {
    hash ^= static_cast<u64>(c);
    hash *= 1099511628211ull;
  }
  return hash;
}

u64 ParseU64(const std::string& value) {
  if (value.empty()) {
    return 0;
  }
  try {
    return static_cast<u64>(std::stoull(value));
  } catch (...) {
    return 0;
  }
}

OnlineMaintenanceOperationKind ParseKind(const std::string& value) {
  if (value == "sorted_index_build") {
    return OnlineMaintenanceOperationKind::sorted_index_build;
  }
  if (value == "index_rebuild") {
    return OnlineMaintenanceOperationKind::index_rebuild;
  }
  if (value == "nosql_compaction") {
    return OnlineMaintenanceOperationKind::nosql_compaction;
  }
  if (value == "optimizer_stats_refresh") {
    return OnlineMaintenanceOperationKind::optimizer_stats_refresh;
  }
  if (value == "optimized_backfill") {
    return OnlineMaintenanceOperationKind::optimized_backfill;
  }
  if (value == "generation_publish") {
    return OnlineMaintenanceOperationKind::generation_publish;
  }
  if (value == "storage_cleanup") {
    return OnlineMaintenanceOperationKind::storage_cleanup;
  }
  if (value == "vector_adaptive_tuning") {
    return OnlineMaintenanceOperationKind::vector_adaptive_tuning;
  }
  if (value == "vector_retrain") {
    return OnlineMaintenanceOperationKind::vector_retrain;
  }
  if (value == "vector_rebuild") {
    return OnlineMaintenanceOperationKind::vector_rebuild;
  }
  return OnlineMaintenanceOperationKind::unknown;
}

OnlineMaintenancePhase ParsePhase(const std::string& value) {
  if (value == "requested") return OnlineMaintenancePhase::requested;
  if (value == "running") return OnlineMaintenancePhase::running;
  if (value == "cancel_requested") return OnlineMaintenancePhase::cancel_requested;
  if (value == "cancelled") return OnlineMaintenancePhase::cancelled;
  if (value == "resumable") return OnlineMaintenancePhase::resumable;
  if (value == "publish_ready") return OnlineMaintenancePhase::publish_ready;
  if (value == "published") return OnlineMaintenancePhase::published;
  if (value == "completed") return OnlineMaintenancePhase::completed;
  if (value == "failed_closed") return OnlineMaintenancePhase::failed_closed;
  return OnlineMaintenancePhase::failed_closed;
}

AgentRuntimeStatus AdmitResources(const OnlineMaintenanceStartRequest& request,
                                  OnlineMaintenanceRecord* record) {
  if (!request.require_resource_admission) {
    return AgentOk();
  }
  if (request.quota_controller == nullptr) {
    return AgentError(kOnlineMaintenanceResourceDenied,
                      "quota_controller_required");
  }
  WorkloadAdmissionRequest admission = request.resource_request;
  if (admission.request_uuid.empty()) {
    admission.request_uuid = request.operation_uuid + ":maintenance";
  }
  if (admission.principal_tag.empty()) {
    admission.principal_tag = "online_maintenance";
  }
  const auto admitted = request.quota_controller->Admit(admission);
  if (!admitted.status.ok || !admitted.reservation_created()) {
    return AgentError(kOnlineMaintenanceResourceDenied,
                      admitted.diagnostic.diagnostic_code);
  }
  record->snapshot.resource_reservation_token = admitted.reservation.token_id;
  AddEvidence(&record->snapshot, "resource_governance",
              admitted.diagnostic.diagnostic_code);
  AddEvidence(&record->snapshot, "resource_reservation_token",
              admitted.reservation.token_id);
  return AgentOk();
}

AgentRuntimeStatus AdmitResources(const OnlineMaintenanceResumeRequest& request,
                                  OnlineMaintenanceRecord* record) {
  if (!request.require_resource_admission) {
    return AgentOk();
  }
  if (request.quota_controller == nullptr) {
    return AgentError(kOnlineMaintenanceResourceDenied,
                      "quota_controller_required");
  }
  WorkloadAdmissionRequest admission = request.resource_request;
  if (admission.request_uuid.empty()) {
    admission.request_uuid = request.operation_uuid + ":resume";
  }
  if (admission.principal_tag.empty()) {
    admission.principal_tag = "online_maintenance_resume";
  }
  const auto admitted = request.quota_controller->Admit(admission);
  if (!admitted.status.ok || !admitted.reservation_created()) {
    return AgentError(kOnlineMaintenanceResourceDenied,
                      admitted.diagnostic.diagnostic_code);
  }
  record->snapshot.resource_reservation_token = admitted.reservation.token_id;
  AddEvidence(&record->snapshot, "resource_governance",
              admitted.diagnostic.diagnostic_code);
  AddEvidence(&record->snapshot, "resource_reservation_token",
              admitted.reservation.token_id);
  return AgentOk();
}

AgentRuntimeStatus ReleaseReservation(WorkloadResourceQuotaController* quota_controller,
                                      OnlineMaintenanceRecord* record,
                                      WorkloadReleaseReason reason) {
  if (record == nullptr || record->snapshot.resource_reservation_token.empty()) {
    return AgentOk();
  }
  if (quota_controller == nullptr) {
    return AgentError("ONLINE_MAINTENANCE.RESOURCE_RELEASE_CONTROLLER_REQUIRED",
                      record->snapshot.operation_uuid);
  }
  const auto released =
      quota_controller->Release(record->snapshot.resource_reservation_token,
                                reason);
  if (!released.ok) {
    return released;
  }
  AddEvidence(&record->snapshot, "resource_release",
              WorkloadReleaseReasonName(reason));
  record->snapshot.resource_reservation_token.clear();
  return AgentOk();
}

}  // namespace

AgentRuntimeStatus OnlineMaintenanceStateStore::Upsert(
    OnlineMaintenanceRecord record) {
  if (record.snapshot.operation_uuid.empty()) {
    return AgentError("ONLINE_MAINTENANCE.OPERATION_UUID_REQUIRED");
  }
  auto existing = std::find_if(records_.begin(),
                               records_.end(),
                               [&](const OnlineMaintenanceRecord& candidate) {
                                 return candidate.snapshot.operation_uuid ==
                                        record.snapshot.operation_uuid;
                               });
  if (existing == records_.end()) {
    records_.push_back(std::move(record));
  } else {
    *existing = std::move(record);
  }
  return AgentOk();
}

std::optional<OnlineMaintenanceRecord> OnlineMaintenanceStateStore::Find(
    const std::string& operation_uuid) const {
  const auto found = std::find_if(records_.begin(),
                                  records_.end(),
                                  [&](const OnlineMaintenanceRecord& record) {
                                    return record.snapshot.operation_uuid ==
                                           operation_uuid;
                                  });
  if (found == records_.end()) {
    return std::nullopt;
  }
  return *found;
}

OnlineMaintenanceRecord* OnlineMaintenanceStateStore::MutableFind(
    const std::string& operation_uuid) {
  auto found = std::find_if(records_.begin(),
                            records_.end(),
                            [&](const OnlineMaintenanceRecord& record) {
                              return record.snapshot.operation_uuid ==
                                     operation_uuid;
                            });
  return found == records_.end() ? nullptr : &*found;
}

void OnlineMaintenanceStateStore::AddEvidence(std::string key,
                                              std::string value) {
  evidence_log_.push_back({std::move(key), std::move(value)});
}

const char* OnlineMaintenanceOperationKindName(
    OnlineMaintenanceOperationKind kind) {
  switch (kind) {
    case OnlineMaintenanceOperationKind::sorted_index_build:
      return "sorted_index_build";
    case OnlineMaintenanceOperationKind::index_rebuild:
      return "index_rebuild";
    case OnlineMaintenanceOperationKind::nosql_compaction:
      return "nosql_compaction";
    case OnlineMaintenanceOperationKind::optimizer_stats_refresh:
      return "optimizer_stats_refresh";
    case OnlineMaintenanceOperationKind::optimized_backfill:
      return "optimized_backfill";
    case OnlineMaintenanceOperationKind::generation_publish:
      return "generation_publish";
    case OnlineMaintenanceOperationKind::storage_cleanup:
      return "storage_cleanup";
    case OnlineMaintenanceOperationKind::vector_adaptive_tuning:
      return "vector_adaptive_tuning";
    case OnlineMaintenanceOperationKind::vector_retrain:
      return "vector_retrain";
    case OnlineMaintenanceOperationKind::vector_rebuild:
      return "vector_rebuild";
    case OnlineMaintenanceOperationKind::unknown:
      return "unknown";
  }
  return "unknown";
}

const char* OnlineMaintenancePhaseName(OnlineMaintenancePhase phase) {
  switch (phase) {
    case OnlineMaintenancePhase::requested: return "requested";
    case OnlineMaintenancePhase::running: return "running";
    case OnlineMaintenancePhase::cancel_requested: return "cancel_requested";
    case OnlineMaintenancePhase::cancelled: return "cancelled";
    case OnlineMaintenancePhase::resumable: return "resumable";
    case OnlineMaintenancePhase::publish_ready: return "publish_ready";
    case OnlineMaintenancePhase::published: return "published";
    case OnlineMaintenancePhase::completed: return "completed";
    case OnlineMaintenancePhase::failed_closed: return "failed_closed";
  }
  return "failed_closed";
}

const char* OnlineMaintenanceDecisionName(OnlineMaintenanceDecision decision) {
  switch (decision) {
    case OnlineMaintenanceDecision::admitted: return "admitted";
    case OnlineMaintenanceDecision::progress_recorded:
      return "progress_recorded";
    case OnlineMaintenanceDecision::cancel_accepted: return "cancel_accepted";
    case OnlineMaintenanceDecision::resumed: return "resumed";
    case OnlineMaintenanceDecision::recovered_resumable:
      return "recovered_resumable";
    case OnlineMaintenanceDecision::completed: return "completed";
    case OnlineMaintenanceDecision::published: return "published";
    case OnlineMaintenanceDecision::refused: return "refused";
  }
  return "refused";
}

OnlineMaintenanceResult StartOnlineMaintenanceOperation(
    OnlineMaintenanceStateStore* store,
    const OnlineMaintenanceStartRequest& request) {
  if (store == nullptr) {
    return RefuseMissingStore("ONLINE_MAINTENANCE.STORE_REQUIRED");
  }
  OnlineMaintenanceRecord record;
  record.snapshot.kind = request.kind;
  record.snapshot.operation_uuid = request.operation_uuid;
  record.snapshot.database_uuid = request.database_uuid;
  record.snapshot.target_uuid = request.target_uuid;
  record.snapshot.phase = OnlineMaintenancePhase::running;
  record.snapshot.stage = request.stage.empty() ? "running" : request.stage;
  record.snapshot.work_unit_label = request.work_unit_label.empty()
                                        ? "work_units"
                                        : request.work_unit_label;
  record.snapshot.total_units = request.total_units;
  record.snapshot.durable_checkpoint_persisted =
      request.durable_checkpoint_persisted;
  record.snapshot.engine_mga_authoritative = request.engine_mga_authoritative;
  record.snapshot.cancelable = request.cancelable;
  record.snapshot.support_bundle_evidence_present =
      request.support_bundle_sink_available;
  record.snapshot.observability_evidence_present =
      request.observability_sink_available;
  record.snapshot.restart_token = request.operation_uuid + ":restart:0";
  record.started_at_microseconds = request.now_microseconds;
  record.updated_at_microseconds = request.now_microseconds;

  if (request.kind == OnlineMaintenanceOperationKind::unknown ||
      request.operation_uuid.empty() ||
      request.database_uuid.empty() ||
      request.target_uuid.empty()) {
    record.snapshot.phase = OnlineMaintenancePhase::failed_closed;
    return MakeResult(std::move(record),
                      OnlineMaintenanceDecision::refused,
                      "ONLINE_MAINTENANCE.INVALID_IDENTITY",
                      true);
  }
  if (request.total_units == 0) {
    record.snapshot.phase = OnlineMaintenancePhase::failed_closed;
    return MakeResult(std::move(record),
                      OnlineMaintenanceDecision::refused,
                      "ONLINE_MAINTENANCE.TOTAL_UNITS_REQUIRED",
                      true);
  }
  if (!request.engine_mga_authoritative ||
      !request.durable_checkpoint_persisted ||
      !request.support_bundle_sink_available ||
      !request.observability_sink_available) {
    record.snapshot.phase = OnlineMaintenancePhase::failed_closed;
    return MakeResult(std::move(record),
                      OnlineMaintenanceDecision::refused,
                      "ONLINE_MAINTENANCE.AUTHORITY_EVIDENCE_REQUIRED",
                      true);
  }
  const auto duplicate = store->Find(request.operation_uuid);
  if (duplicate.has_value()) {
    record.snapshot.phase = OnlineMaintenancePhase::failed_closed;
    return MakeResult(std::move(record),
                      OnlineMaintenanceDecision::refused,
                      "ONLINE_MAINTENANCE.DUPLICATE_OPERATION",
                      true);
  }

  const auto admitted = AdmitResources(request, &record);
  if (!admitted.ok) {
    record.snapshot.phase = OnlineMaintenancePhase::failed_closed;
    return MakeResult(std::move(record),
                      OnlineMaintenanceDecision::refused,
                      admitted.diagnostic_code,
                      true,
                      admitted.detail);
  }

  RefreshDerivedFields(&record);
  store->Upsert(record);
  RecordStoreEvidence(store, record, "started");
  return MakeResult(std::move(record),
                    OnlineMaintenanceDecision::admitted,
                    kOnlineMaintenanceStarted,
                    false);
}

OnlineMaintenanceResult RecordOnlineMaintenanceProgress(
    OnlineMaintenanceStateStore* store,
    const OnlineMaintenanceProgressRequest& request) {
  if (store == nullptr) {
    return RefuseMissingStore("ONLINE_MAINTENANCE.STORE_REQUIRED");
  }
  auto* record = store->MutableFind(request.operation_uuid);
  if (record == nullptr) {
    return RefuseMissingStore("ONLINE_MAINTENANCE.OPERATION_NOT_FOUND");
  }
  if (IsTerminal(record->snapshot.phase)) {
    return RefuseExisting(store,
                          record,
                          "ONLINE_MAINTENANCE.TERMINAL_PROGRESS_REFUSED",
                          OnlineMaintenancePhaseName(record->snapshot.phase));
  }
  const u64 total =
      request.total_units == 0 ? record->snapshot.total_units
                               : request.total_units;
  if (!request.durable_checkpoint_persisted ||
      total == 0 ||
      total != record->snapshot.total_units ||
      request.completed_units < record->snapshot.completed_units ||
      request.completed_units > total) {
    return RefuseExisting(store,
                          record,
                          "ONLINE_MAINTENANCE.PROGRESS_CHECKPOINT_INVALID",
                          "monotonic durable progress checkpoint required");
  }
  record->snapshot.completed_units = request.completed_units;
  record->snapshot.stage = request.stage.empty() ? record->snapshot.stage
                                                 : request.stage;
  record->snapshot.durable_checkpoint_persisted = true;
  record->snapshot.checkpoint_generation += 1;
  record->snapshot.restart_token =
      request.operation_uuid + ":restart:" +
      std::to_string(record->snapshot.checkpoint_generation);
  record->checkpoint_payload = request.checkpoint_payload;
  record->updated_at_microseconds = request.now_microseconds;
  if (request.completed_units == total) {
    record->snapshot.phase = IsPublishCandidate(record->snapshot.kind)
                                 ? OnlineMaintenancePhase::publish_ready
                                 : OnlineMaintenancePhase::completed;
  } else {
    record->snapshot.phase = OnlineMaintenancePhase::running;
  }
  RefreshDerivedFields(record);
  OnlineMaintenanceRecord updated = *record;
  store->Upsert(updated);
  RecordStoreEvidence(store, updated, "progress");
  return MakeResult(std::move(updated),
                    OnlineMaintenanceDecision::progress_recorded,
                    kOnlineMaintenanceProgressRecorded,
                    false);
}

OnlineMaintenanceResult CancelOnlineMaintenanceOperation(
    OnlineMaintenanceStateStore* store,
    const OnlineMaintenanceCancelRequest& request) {
  if (store == nullptr) {
    return RefuseMissingStore("ONLINE_MAINTENANCE.STORE_REQUIRED");
  }
  auto* record = store->MutableFind(request.operation_uuid);
  if (record == nullptr) {
    return RefuseMissingStore("ONLINE_MAINTENANCE.OPERATION_NOT_FOUND");
  }
  if (!record->snapshot.cancelable || IsTerminal(record->snapshot.phase)) {
    return RefuseExisting(store,
                          record,
                          "ONLINE_MAINTENANCE.CANCEL_NOT_ALLOWED",
                          OnlineMaintenancePhaseName(record->snapshot.phase));
  }
  if (!request.durable_checkpoint_persisted) {
    return RefuseExisting(store,
                          record,
                          "ONLINE_MAINTENANCE.CANCEL_CHECKPOINT_REQUIRED",
                          "durable checkpoint is required before cancel");
  }
  if (request.release_resource_reservation) {
    const auto released = ReleaseReservation(request.quota_controller,
                                             record,
                                             WorkloadReleaseReason::cancellation);
    if (!released.ok) {
      return RefuseExisting(store,
                            record,
                            released.diagnostic_code,
                            released.detail);
    }
  }
  record->snapshot.phase = OnlineMaintenancePhase::cancelled;
  record->snapshot.stage = "cancelled";
  record->snapshot.durable_checkpoint_persisted = true;
  record->snapshot.checkpoint_generation += 1;
  record->checkpoint_payload = request.checkpoint_payload;
  record->updated_at_microseconds = request.now_microseconds;
  AddEvidence(&record->snapshot, "cancel_reason", request.reason);
  RefreshDerivedFields(record);
  OnlineMaintenanceRecord updated = *record;
  store->Upsert(updated);
  RecordStoreEvidence(store, updated, "cancelled");
  return MakeResult(std::move(updated),
                    OnlineMaintenanceDecision::cancel_accepted,
                    kOnlineMaintenanceCancelCheckpointed,
                    false);
}

OnlineMaintenanceResult ResumeOnlineMaintenanceOperation(
    OnlineMaintenanceStateStore* store,
    const OnlineMaintenanceResumeRequest& request) {
  if (store == nullptr) {
    return RefuseMissingStore("ONLINE_MAINTENANCE.STORE_REQUIRED");
  }
  auto* record = store->MutableFind(request.operation_uuid);
  if (record == nullptr) {
    return RefuseMissingStore("ONLINE_MAINTENANCE.OPERATION_NOT_FOUND");
  }
  if (record->snapshot.phase != OnlineMaintenancePhase::cancelled &&
      record->snapshot.phase != OnlineMaintenancePhase::resumable) {
    return RefuseExisting(store,
                          record,
                          kOnlineMaintenanceUnsafeResumeRefused,
                          OnlineMaintenancePhaseName(record->snapshot.phase));
  }
  if (!request.engine_mga_authoritative ||
      !request.durable_checkpoint_persisted ||
      !record->snapshot.durable_checkpoint_persisted ||
      record->snapshot.partial_publish_visible ||
      !request.support_bundle_sink_available ||
      !request.observability_sink_available) {
    return RefuseExisting(store,
                          record,
                          kOnlineMaintenanceUnsafeResumeRefused,
                          "engine MGA authority durable checkpoint support bundle and observability evidence required");
  }
  const auto admitted = AdmitResources(request, record);
  if (!admitted.ok) {
    return RefuseExisting(store,
                          record,
                          admitted.diagnostic_code,
                          admitted.detail);
  }
  record->snapshot.phase = OnlineMaintenancePhase::running;
  record->snapshot.stage = "resumed";
  record->snapshot.engine_mga_authoritative = true;
  record->snapshot.support_bundle_evidence_present = true;
  record->snapshot.observability_evidence_present = true;
  record->snapshot.checkpoint_generation += 1;
  record->updated_at_microseconds = request.now_microseconds;
  RefreshDerivedFields(record);
  OnlineMaintenanceRecord updated = *record;
  store->Upsert(updated);
  RecordStoreEvidence(store, updated, "resumed");
  return MakeResult(std::move(updated),
                    OnlineMaintenanceDecision::resumed,
                    kOnlineMaintenanceResumedFromCheckpoint,
                    false);
}

OnlineMaintenanceResult CompleteOnlineMaintenanceOperation(
    OnlineMaintenanceStateStore* store,
    const OnlineMaintenanceCompleteRequest& request) {
  if (store == nullptr) {
    return RefuseMissingStore("ONLINE_MAINTENANCE.STORE_REQUIRED");
  }
  auto* record = store->MutableFind(request.operation_uuid);
  if (record == nullptr) {
    return RefuseMissingStore("ONLINE_MAINTENANCE.OPERATION_NOT_FOUND");
  }
  if (record->snapshot.phase != OnlineMaintenancePhase::running &&
      record->snapshot.phase != OnlineMaintenancePhase::completed) {
    return RefuseExisting(store,
                          record,
                          "ONLINE_MAINTENANCE.COMPLETE_NOT_ALLOWED",
                          OnlineMaintenancePhaseName(record->snapshot.phase));
  }
  if (!request.durable_checkpoint_persisted ||
      !request.support_bundle_sink_available ||
      !request.observability_sink_available) {
    return RefuseExisting(store,
                          record,
                          "ONLINE_MAINTENANCE.COMPLETE_EVIDENCE_REQUIRED",
                          "durable checkpoint support bundle and observability evidence required");
  }
  if (request.release_resource_reservation) {
    const auto released = ReleaseReservation(request.quota_controller,
                                             record,
                                             WorkloadReleaseReason::success);
    if (!released.ok) {
      return RefuseExisting(store,
                            record,
                            released.diagnostic_code,
                            released.detail);
    }
  }
  record->snapshot.completed_units = record->snapshot.total_units;
  record->snapshot.phase = OnlineMaintenancePhase::completed;
  record->snapshot.stage = "completed";
  record->snapshot.durable_checkpoint_persisted = true;
  record->snapshot.support_bundle_evidence_present = true;
  record->snapshot.observability_evidence_present = true;
  record->snapshot.checkpoint_generation += 1;
  record->updated_at_microseconds = request.now_microseconds;
  RefreshDerivedFields(record);
  OnlineMaintenanceRecord updated = *record;
  store->Upsert(updated);
  RecordStoreEvidence(store, updated, "completed");
  return MakeResult(std::move(updated),
                    OnlineMaintenanceDecision::completed,
                    kOnlineMaintenanceCompletedUnpublished,
                    false);
}

OnlineMaintenanceResult PublishOnlineMaintenanceOperation(
    OnlineMaintenanceStateStore* store,
    const OnlineMaintenancePublishRequest& request) {
  if (store == nullptr) {
    return RefuseMissingStore("ONLINE_MAINTENANCE.STORE_REQUIRED");
  }
  auto* record = store->MutableFind(request.operation_uuid);
  if (record == nullptr) {
    return RefuseMissingStore("ONLINE_MAINTENANCE.OPERATION_NOT_FOUND");
  }
  if (record->snapshot.phase != OnlineMaintenancePhase::publish_ready &&
      record->snapshot.phase != OnlineMaintenancePhase::published) {
    return RefuseExisting(store,
                          record,
                          kOnlineMaintenanceUnsafePublishRefused,
                          OnlineMaintenancePhaseName(record->snapshot.phase));
  }
  if (!request.engine_mga_authoritative ||
      !record->snapshot.durable_checkpoint_persisted ||
      !request.durable_publication_fence_persisted ||
      !request.authoritative_generation_validated ||
      !request.no_partial_visibility ||
      !request.support_bundle_sink_available ||
      !request.observability_sink_available ||
      record->snapshot.partial_publish_visible) {
    return RefuseExisting(store,
                          record,
                          kOnlineMaintenanceUnsafePublishRefused,
                          "publish requires durable MGA checkpoint publication fence validated generation no-partial-visibility support bundle and observability evidence");
  }
  if (request.release_resource_reservation) {
    const auto released = ReleaseReservation(request.quota_controller,
                                             record,
                                             WorkloadReleaseReason::success);
    if (!released.ok) {
      return RefuseExisting(store,
                            record,
                            released.diagnostic_code,
                            released.detail);
    }
  }
  record->publication_fence_persisted = true;
  record->authoritative_generation_validated = true;
  record->snapshot.phase = OnlineMaintenancePhase::published;
  record->snapshot.stage = "published";
  record->snapshot.engine_mga_authoritative = true;
  record->snapshot.support_bundle_evidence_present = true;
  record->snapshot.observability_evidence_present = true;
  record->snapshot.checkpoint_generation += 1;
  record->updated_at_microseconds = request.now_microseconds;
  RefreshDerivedFields(record);
  OnlineMaintenanceRecord updated = *record;
  store->Upsert(updated);
  RecordStoreEvidence(store, updated, "published");
  return MakeResult(std::move(updated),
                    OnlineMaintenanceDecision::published,
                    kOnlineMaintenancePublishSuccess,
                    false);
}

OnlineMaintenanceResult RecoverOnlineMaintenanceOperation(
    OnlineMaintenanceStateStore* store,
    const OnlineMaintenanceRecoveryRequest& request) {
  if (store == nullptr) {
    return RefuseMissingStore("ONLINE_MAINTENANCE.STORE_REQUIRED");
  }
  OnlineMaintenanceRecord record = request.checkpoint;
  if (record.snapshot.operation_uuid.empty() ||
      !request.engine_mga_authoritative ||
      !record.snapshot.durable_checkpoint_persisted ||
      record.snapshot.partial_publish_visible ||
      !request.support_bundle_sink_available ||
      !request.observability_sink_available) {
    record.snapshot.phase = OnlineMaintenancePhase::failed_closed;
    record.snapshot.published_visible = false;
    record.snapshot.partial_publish_visible = false;
    store->Upsert(record);
    RecordStoreEvidence(store, record, "recovery_refused");
    return MakeResult(std::move(record),
                      OnlineMaintenanceDecision::refused,
                      kOnlineMaintenanceUnsafeRestartRefused,
                      true,
                      "restart requires durable checkpoint with no partial visibility and engine MGA authority");
  }
  if (record.snapshot.phase == OnlineMaintenancePhase::published) {
    if (!record.publication_fence_persisted ||
        !record.authoritative_generation_validated) {
      record.snapshot.phase = OnlineMaintenancePhase::failed_closed;
      record.snapshot.published_visible = false;
      store->Upsert(record);
      RecordStoreEvidence(store, record, "published_recovery_refused");
      return MakeResult(std::move(record),
                        OnlineMaintenanceDecision::refused,
                        kOnlineMaintenanceUnsafeRestartRefused,
                        true,
                        "published checkpoint lacks publication fence");
    }
    record.snapshot.engine_mga_authoritative = true;
    record.snapshot.support_bundle_evidence_present = true;
    record.snapshot.observability_evidence_present = true;
    RefreshDerivedFields(&record);
    store->Upsert(record);
    RecordStoreEvidence(store, record, "published_recovered");
    return MakeResult(std::move(record),
                      OnlineMaintenanceDecision::published,
                      kOnlineMaintenancePublishSuccess,
                      false);
  }

  if (record.snapshot.phase == OnlineMaintenancePhase::completed) {
    RefreshDerivedFields(&record);
    store->Upsert(record);
    RecordStoreEvidence(store, record, "completed_recovered");
    return MakeResult(std::move(record),
                      OnlineMaintenanceDecision::completed,
                      kOnlineMaintenanceCompletedUnpublished,
                      false);
  }

  record.snapshot.phase = OnlineMaintenancePhase::resumable;
  record.snapshot.stage = "crash_recovered_resumable";
  record.snapshot.engine_mga_authoritative = true;
  record.snapshot.support_bundle_evidence_present = true;
  record.snapshot.observability_evidence_present = true;
  record.updated_at_microseconds = request.now_microseconds;
  RefreshDerivedFields(&record);
  store->Upsert(record);
  RecordStoreEvidence(store, record, "recovered_resumable");
  return MakeResult(std::move(record),
                    OnlineMaintenanceDecision::recovered_resumable,
                    kOnlineMaintenanceRecoveredResumable,
                    false);
}

std::string SerializeOnlineMaintenanceCheckpoint(
    const OnlineMaintenanceRecord& record) {
  std::ostringstream out;
  WriteField(&out, "operation_uuid", record.snapshot.operation_uuid);
  WriteField(&out, "database_uuid", record.snapshot.database_uuid);
  WriteField(&out, "target_uuid", record.snapshot.target_uuid);
  WriteField(&out, "kind",
             OnlineMaintenanceOperationKindName(record.snapshot.kind));
  WriteField(&out, "phase", OnlineMaintenancePhaseName(record.snapshot.phase));
  WriteField(&out, "stage", record.snapshot.stage);
  WriteField(&out, "work_unit_label", record.snapshot.work_unit_label);
  WriteField(&out, "completed_units",
             std::to_string(record.snapshot.completed_units));
  WriteField(&out, "total_units", std::to_string(record.snapshot.total_units));
  WriteField(&out, "checkpoint_generation",
             std::to_string(record.snapshot.checkpoint_generation));
  WriteField(&out, "durable_checkpoint_persisted",
             BoolText(record.snapshot.durable_checkpoint_persisted));
  WriteField(&out, "engine_mga_authoritative",
             BoolText(record.snapshot.engine_mga_authoritative));
  WriteField(&out, "cancelable", BoolText(record.snapshot.cancelable));
  WriteField(&out, "partial_publish_visible",
             BoolText(record.snapshot.partial_publish_visible));
  WriteField(&out, "support_bundle_evidence_present",
             BoolText(record.snapshot.support_bundle_evidence_present));
  WriteField(&out, "observability_evidence_present",
             BoolText(record.snapshot.observability_evidence_present));
  WriteField(&out, "restart_token", record.snapshot.restart_token);
  WriteField(&out, "resource_reservation_token",
             record.snapshot.resource_reservation_token);
  WriteField(&out, "diagnostic_code", record.snapshot.diagnostic_code);
  WriteField(&out, "checkpoint_payload", record.checkpoint_payload);
  WriteField(&out, "started_at_microseconds",
             std::to_string(record.started_at_microseconds));
  WriteField(&out, "updated_at_microseconds",
             std::to_string(record.updated_at_microseconds));
  WriteField(&out, "publication_fence_persisted",
             BoolText(record.publication_fence_persisted));
  WriteField(&out, "authoritative_generation_validated",
             BoolText(record.authoritative_generation_validated));
  return out.str();
}

std::optional<OnlineMaintenanceRecord> ParseOnlineMaintenanceCheckpoint(
    const std::string& text) {
  OnlineMaintenanceRecord record;
  std::istringstream input(text);
  std::string line;
  bool saw_operation = false;
  while (std::getline(input, line)) {
    const auto pos = line.find('=');
    if (pos == std::string::npos) {
      continue;
    }
    const std::string key = line.substr(0, pos);
    const std::string value = Unescape(std::string_view(line).substr(pos + 1));
    if (key == "operation_uuid") {
      record.snapshot.operation_uuid = value;
      saw_operation = !value.empty();
    } else if (key == "database_uuid") {
      record.snapshot.database_uuid = value;
    } else if (key == "target_uuid") {
      record.snapshot.target_uuid = value;
    } else if (key == "kind") {
      record.snapshot.kind = ParseKind(value);
    } else if (key == "phase") {
      record.snapshot.phase = ParsePhase(value);
    } else if (key == "stage") {
      record.snapshot.stage = value;
    } else if (key == "work_unit_label") {
      record.snapshot.work_unit_label = value;
    } else if (key == "completed_units") {
      record.snapshot.completed_units = ParseU64(value);
    } else if (key == "total_units") {
      record.snapshot.total_units = ParseU64(value);
    } else if (key == "checkpoint_generation") {
      record.snapshot.checkpoint_generation = ParseU64(value);
    } else if (key == "durable_checkpoint_persisted") {
      record.snapshot.durable_checkpoint_persisted = ParseBool(value);
    } else if (key == "engine_mga_authoritative") {
      record.snapshot.engine_mga_authoritative = ParseBool(value);
    } else if (key == "cancelable") {
      record.snapshot.cancelable = ParseBool(value);
    } else if (key == "partial_publish_visible") {
      record.snapshot.partial_publish_visible = ParseBool(value);
    } else if (key == "support_bundle_evidence_present") {
      record.snapshot.support_bundle_evidence_present = ParseBool(value);
    } else if (key == "observability_evidence_present") {
      record.snapshot.observability_evidence_present = ParseBool(value);
    } else if (key == "restart_token") {
      record.snapshot.restart_token = value;
    } else if (key == "resource_reservation_token") {
      record.snapshot.resource_reservation_token = value;
    } else if (key == "diagnostic_code") {
      record.snapshot.diagnostic_code = value;
    } else if (key == "checkpoint_payload") {
      record.checkpoint_payload = value;
    } else if (key == "started_at_microseconds") {
      record.started_at_microseconds = ParseU64(value);
    } else if (key == "updated_at_microseconds") {
      record.updated_at_microseconds = ParseU64(value);
    } else if (key == "publication_fence_persisted") {
      record.publication_fence_persisted = ParseBool(value);
    } else if (key == "authoritative_generation_validated") {
      record.authoritative_generation_validated = ParseBool(value);
    }
  }
  if (!saw_operation) {
    return std::nullopt;
  }
  RefreshDerivedFields(&record);
  return record;
}

AgentRuntimeStatus PersistOnlineMaintenanceCheckpointFile(
    const std::string& path,
    const OnlineMaintenanceRecord& record) {
  if (path.empty()) {
    return AgentError("ONLINE_MAINTENANCE.CHECKPOINT_PATH_REQUIRED");
  }
  const auto checkpoint_path = std::filesystem::path(path);
  const auto parent = checkpoint_path.parent_path();
  if (!parent.empty()) {
    std::error_code mkdir_error;
    std::filesystem::create_directories(parent, mkdir_error);
    if (mkdir_error) {
      return AgentError("ONLINE_MAINTENANCE.CHECKPOINT_WRITE_FAILED",
                        mkdir_error.message());
    }
  }
  const std::string body =
      std::string("SBOMCP1\n") + SerializeOnlineMaintenanceCheckpoint(record);
  const std::string payload =
      body + "checkpoint_checksum=" + std::to_string(Fnv1a64(body)) + "\n";
  const auto temp_path = checkpoint_path.string() + ".tmp";
  {
    std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
    if (!out) {
      return AgentError("ONLINE_MAINTENANCE.CHECKPOINT_WRITE_FAILED",
                        temp_path);
    }
    out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    out.close();
    if (!out) {
      return AgentError("ONLINE_MAINTENANCE.CHECKPOINT_WRITE_FAILED",
                        temp_path);
    }
  }
  std::error_code rename_error;
  std::filesystem::rename(temp_path, checkpoint_path, rename_error);
  if (rename_error) {
    std::error_code remove_error;
    std::filesystem::remove(checkpoint_path, remove_error);
    rename_error.clear();
    std::filesystem::rename(temp_path, checkpoint_path, rename_error);
  }
  if (rename_error) {
    return AgentError("ONLINE_MAINTENANCE.CHECKPOINT_WRITE_FAILED",
                      rename_error.message());
  }
  return AgentOk();
}

OnlineMaintenanceCheckpointFileResult LoadOnlineMaintenanceCheckpointFile(
    const std::string& path) {
  OnlineMaintenanceCheckpointFileResult result;
  if (path.empty()) {
    result.status =
        AgentError("ONLINE_MAINTENANCE.CHECKPOINT_PATH_REQUIRED");
    return result;
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    result.status =
        AgentError("ONLINE_MAINTENANCE.CHECKPOINT_READ_FAILED", path);
    return result;
  }
  std::ostringstream data;
  data << in.rdbuf();
  const std::string payload = data.str();
  if (!StartsWith(payload, "SBOMCP1\n")) {
    result.status =
        AgentError("ONLINE_MAINTENANCE.CHECKPOINT_MAGIC_INVALID", path);
    return result;
  }
  const auto checksum_pos = payload.rfind("checkpoint_checksum=");
  if (checksum_pos == std::string::npos) {
    result.status =
        AgentError("ONLINE_MAINTENANCE.CHECKPOINT_CHECKSUM_MISSING", path);
    return result;
  }
  const std::string body = payload.substr(0, checksum_pos);
  const std::string checksum_text =
      payload.substr(checksum_pos + std::string("checkpoint_checksum=").size());
  if (ParseU64(checksum_text) != Fnv1a64(body)) {
    result.status =
        AgentError("ONLINE_MAINTENANCE.CHECKPOINT_CHECKSUM_INVALID", path);
    return result;
  }
  auto record = ParseOnlineMaintenanceCheckpoint(body.substr(8));
  if (!record.has_value()) {
    result.status =
        AgentError("ONLINE_MAINTENANCE.CHECKPOINT_PARSE_FAILED", path);
    return result;
  }
  result.status = AgentOk();
  result.record = std::move(*record);
  result.record_present = true;
  return result;
}

std::string SerializeOnlineMaintenanceSnapshot(
    const OnlineMaintenanceProgressSnapshot& snapshot) {
  std::ostringstream out;
  out << "operation_uuid=" << snapshot.operation_uuid << '\n';
  out << "database_uuid=" << snapshot.database_uuid << '\n';
  out << "target_uuid=" << snapshot.target_uuid << '\n';
  out << "kind=" << OnlineMaintenanceOperationKindName(snapshot.kind) << '\n';
  out << "phase=" << OnlineMaintenancePhaseName(snapshot.phase) << '\n';
  out << "stage=" << snapshot.stage << '\n';
  out << "completed_units=" << snapshot.completed_units << '\n';
  out << "total_units=" << snapshot.total_units << '\n';
  out << "percent_basis_points=" << snapshot.percent_basis_points << '\n';
  out << "checkpoint_generation=" << snapshot.checkpoint_generation << '\n';
  out << "durable_checkpoint_persisted="
      << BoolText(snapshot.durable_checkpoint_persisted) << '\n';
  out << "engine_mga_authoritative="
      << BoolText(snapshot.engine_mga_authoritative) << '\n';
  out << "cancelable=" << BoolText(snapshot.cancelable) << '\n';
  out << "resumable=" << BoolText(snapshot.resumable) << '\n';
  out << "publish_ready=" << BoolText(snapshot.publish_ready) << '\n';
  out << "published_visible=" << BoolText(snapshot.published_visible) << '\n';
  out << "partial_publish_visible="
      << BoolText(snapshot.partial_publish_visible) << '\n';
  out << "support_bundle_evidence_present="
      << BoolText(snapshot.support_bundle_evidence_present) << '\n';
  out << "observability_evidence_present="
      << BoolText(snapshot.observability_evidence_present) << '\n';
  out << "diagnostic_code=" << snapshot.diagnostic_code << '\n';
  for (const auto& field : snapshot.evidence) {
    out << "evidence." << field.key << '=' << field.value << '\n';
  }
  return out.str();
}

}  // namespace scratchbird::core::agents
