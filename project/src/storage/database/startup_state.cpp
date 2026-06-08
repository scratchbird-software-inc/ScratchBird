// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "startup_state.hpp"

#include "database_format.hpp"
#include "page_manager.hpp"
#include "page_header.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <sstream>
#include <utility>
#include <vector>

namespace scratchbird::storage::database {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::HostToLittle16;
using scratchbird::core::platform::HostToLittle32;
using scratchbird::core::platform::HostToLittle64;
using scratchbird::core::platform::LittleToHost16;
using scratchbird::core::platform::LittleToHost32;
using scratchbird::core::platform::LittleToHost64;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::UuidKind;
using scratchbird::storage::disk::FileDevice;
using scratchbird::storage::disk::FileOpenMode;
using scratchbird::storage::disk::ParseDatabaseHeader;
using scratchbird::storage::disk::SerializedDatabaseHeader;
using scratchbird::storage::disk::kPageHeaderSerializedBytes;
using scratchbird::storage::page::CheckedPageBodyOffset;

constexpr std::array<unsigned char, 8> kStartupMagic = {'S', 'B', 'S', 'T', 'V', '0', '0', '1'};
constexpr u32 kOffsetMagic = 0;
constexpr u32 kOffsetDatabaseUuid = 8;
constexpr u32 kOffsetFilespaceUuid = 24;
constexpr u32 kOffsetPageSize = 40;
constexpr u32 kOffsetFlags = 44;
constexpr u32 kOffsetStartupCounter = 48;
constexpr u32 kOffsetRestartGeneration = 56;
constexpr u32 kOffsetCheckpointGeneration = 64;
constexpr u32 kOffsetRuntimeActivationGeneration = 72;
constexpr u32 kOffsetRecoveryClassification = 80;
constexpr u32 kOffsetOwnerTokenSize = 84;
constexpr u32 kOffsetOwnerToken = 88;
constexpr u32 kMaxOwnerTokenBytes = 96;
constexpr u32 kOffsetFirstOpenActivationLocalTransactionId = kOffsetOwnerToken + kMaxOwnerTokenBytes;
constexpr u32 kOffsetCleanShutdownLocalTransactionId = kOffsetFirstOpenActivationLocalTransactionId + sizeof(u64);
constexpr u32 kOffsetBootstrapLocalTransactionId = kOffsetCleanShutdownLocalTransactionId + sizeof(u64);
constexpr u32 kOffsetLastLifecycleLocalTransactionId = kOffsetBootstrapLocalTransactionId + sizeof(u64);
constexpr u32 kOffsetLastLifecycleEventUnixEpochMillis = kOffsetLastLifecycleLocalTransactionId + sizeof(u64);
constexpr u32 kOffsetLifecycleGeneration = kOffsetLastLifecycleEventUnixEpochMillis + sizeof(u64);
constexpr u32 kOffsetDurableLifecyclePhase = kOffsetLifecycleGeneration + sizeof(u64);
constexpr u32 kOffsetDurableEvidenceFlags = kOffsetDurableLifecyclePhase + sizeof(u16);
constexpr u32 kRequiredStartupBodyBytes = kOffsetDurableEvidenceFlags + sizeof(u64);

constexpr u32 kFlagCleanShutdown = 1u << 0;
constexpr u32 kFlagStartupDirty = 1u << 1;
constexpr u32 kFlagWriteAdmissionFenced = 1u << 2;
constexpr u32 kFlagConfigAuthorityLoaded = 1u << 3;
constexpr u32 kFlagSecurityAuthorityLoaded = 1u << 4;
constexpr u32 kFlagI18nAuthorityLoaded = 1u << 5;
constexpr u32 kFlagRuntimeActivationComplete = 1u << 6;
constexpr u32 kFlagAgentRuntimeStarted = 1u << 7;
constexpr u32 kFlagCacheRuntimeStarted = 1u << 8;
constexpr u32 kFlagIpcRuntimeStarted = 1u << 9;
constexpr u32 kFlagServerRuntimeStarted = 1u << 10;

Status StartupOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::storage_disk};
}

Status StartupErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::storage_disk};
}

StartupWriteResult StartupWriteError(std::string diagnostic_code,
                                     std::string message_key,
                                     std::string detail = {}) {
  StartupWriteResult result;
  result.status = StartupErrorStatus();
  result.diagnostic = MakeStartupStateDiagnostic(result.status,
                                                 std::move(diagnostic_code),
                                                 std::move(message_key),
                                                 std::move(detail));
  return result;
}

StartupStateResult StartupReadError(std::string diagnostic_code,
                                    std::string message_key,
                                    std::string detail = {}) {
  StartupStateResult result;
  result.status = StartupErrorStatus();
  result.diagnostic = MakeStartupStateDiagnostic(result.status,
                                                 std::move(diagnostic_code),
                                                 std::move(message_key),
                                                 std::move(detail));
  return result;
}

StartupStateFormatCompatibilityResult StartupFormatOk() {
  StartupStateFormatCompatibilityResult result;
  result.status = StartupOkStatus();
  result.compatibility_class = StartupStateFormatCompatibilityClass::supported_current;
  return result;
}

StartupStateFormatCompatibilityResult StartupFormatError(
    StartupStateFormatCompatibilityClass compatibility_class,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail) {
  StartupStateFormatCompatibilityResult result;
  result.status = StartupErrorStatus();
  result.compatibility_class = compatibility_class;
  result.migration_required =
      compatibility_class == StartupStateFormatCompatibilityClass::missing_migration_plan_refused ||
      compatibility_class ==
          StartupStateFormatCompatibilityClass::migration_required_without_plan_refused;
  result.diagnostic = MakeStartupStateDiagnostic(result.status,
                                                 std::move(diagnostic_code),
                                                 std::move(message_key),
                                                 std::move(detail));
  return result;
}

void Store16(std::vector<unsigned char>* buffer, u32 offset, u16 value) {
  const u16 stored = HostToLittle16(value);
  std::memcpy(buffer->data() + offset, &stored, sizeof(stored));
}

void Store32(std::vector<unsigned char>* buffer, u32 offset, u32 value) {
  const u32 stored = HostToLittle32(value);
  std::memcpy(buffer->data() + offset, &stored, sizeof(stored));
}

void Store64(std::vector<unsigned char>* buffer, u32 offset, u64 value) {
  const u64 stored = HostToLittle64(value);
  std::memcpy(buffer->data() + offset, &stored, sizeof(stored));
}

u16 Load16(const std::vector<unsigned char>& buffer, u32 offset) {
  u16 value = 0;
  std::memcpy(&value, buffer.data() + offset, sizeof(value));
  return LittleToHost16(value);
}

u32 Load32(const std::vector<unsigned char>& buffer, u32 offset) {
  u32 value = 0;
  std::memcpy(&value, buffer.data() + offset, sizeof(value));
  return LittleToHost32(value);
}

u64 Load64(const std::vector<unsigned char>& buffer, u32 offset) {
  u64 value = 0;
  std::memcpy(&value, buffer.data() + offset, sizeof(value));
  return LittleToHost64(value);
}

std::vector<unsigned char> SerializeBody(const StartupStateRecord& state, u32 page_size) {
  std::vector<unsigned char> body(page_size - kPageHeaderSerializedBytes, 0);
  std::copy(kStartupMagic.begin(), kStartupMagic.end(), body.begin() + kOffsetMagic);
  std::copy(state.database_uuid.value.bytes.begin(), state.database_uuid.value.bytes.end(), body.begin() + kOffsetDatabaseUuid);
  std::copy(state.first_filespace_uuid.value.bytes.begin(), state.first_filespace_uuid.value.bytes.end(), body.begin() + kOffsetFilespaceUuid);
  Store32(&body, kOffsetPageSize, state.page_size);
  u32 flags = 0;
  if (state.clean_shutdown) { flags |= kFlagCleanShutdown; }
  if (state.startup_dirty) { flags |= kFlagStartupDirty; }
  if (state.write_admission_fenced) { flags |= kFlagWriteAdmissionFenced; }
  if (state.config_authority_loaded) { flags |= kFlagConfigAuthorityLoaded; }
  if (state.security_authority_loaded) { flags |= kFlagSecurityAuthorityLoaded; }
  if (state.i18n_authority_loaded) { flags |= kFlagI18nAuthorityLoaded; }
  if (state.runtime_activation_complete) { flags |= kFlagRuntimeActivationComplete; }
  if (state.agent_runtime_started) { flags |= kFlagAgentRuntimeStarted; }
  if (state.cache_runtime_started) { flags |= kFlagCacheRuntimeStarted; }
  if (state.ipc_runtime_started) { flags |= kFlagIpcRuntimeStarted; }
  if (state.server_runtime_started) { flags |= kFlagServerRuntimeStarted; }
  Store32(&body, kOffsetFlags, flags);
  Store64(&body, kOffsetStartupCounter, state.startup_counter);
  Store64(&body, kOffsetRestartGeneration, state.restart_generation);
  Store64(&body, kOffsetCheckpointGeneration, state.checkpoint_generation);
  Store64(&body, kOffsetRuntimeActivationGeneration, state.runtime_activation_generation);
  Store64(&body, kOffsetBootstrapLocalTransactionId, state.bootstrap_local_transaction_id);
  Store64(&body,
          kOffsetFirstOpenActivationLocalTransactionId,
          state.first_open_activation_local_transaction_id);
  Store64(&body, kOffsetCleanShutdownLocalTransactionId, state.clean_shutdown_local_transaction_id);
  Store64(&body, kOffsetLastLifecycleLocalTransactionId, state.last_lifecycle_local_transaction_id);
  Store64(&body, kOffsetLastLifecycleEventUnixEpochMillis, state.last_lifecycle_event_unix_epoch_millis);
  Store64(&body, kOffsetLifecycleGeneration, state.lifecycle_generation);
  Store16(&body, kOffsetRecoveryClassification, static_cast<u16>(state.recovery_classification));
  Store16(&body, kOffsetDurableLifecyclePhase, static_cast<u16>(state.durable_lifecycle_phase));
  Store64(&body, kOffsetDurableEvidenceFlags, state.durable_evidence_flags);
  const u32 token_size = static_cast<u32>(std::min<std::size_t>(state.owner_token.size(), kMaxOwnerTokenBytes));
  Store32(&body, kOffsetOwnerTokenSize, token_size);
  std::memcpy(body.data() + kOffsetOwnerToken, state.owner_token.data(), token_size);
  return body;
}

StartupStateResult ParseBody(const std::vector<unsigned char>& body, u32 expected_page_size) {
  if (body.size() < kRequiredStartupBodyBytes) {
    return StartupReadError("SB-STARTUP-STATE-BODY-TOO-SMALL", "storage.startup_state.body_too_small");
  }
  if (!std::equal(kStartupMagic.begin(), kStartupMagic.end(), body.begin() + kOffsetMagic)) {
    return StartupReadError("SB-STARTUP-STATE-MAGIC-INVALID", "storage.startup_state.magic_invalid");
  }

  StartupStateResult result;
  result.status = StartupOkStatus();
  result.state.format_major = kStartupStateFormatMajorCurrent;
  result.state.format_minor = kStartupStateFormatMinorCurrent;
  result.state.database_uuid.kind = UuidKind::database;
  result.state.first_filespace_uuid.kind = UuidKind::filespace;
  std::copy(body.begin() + kOffsetDatabaseUuid,
            body.begin() + kOffsetDatabaseUuid + result.state.database_uuid.value.bytes.size(),
            result.state.database_uuid.value.bytes.begin());
  std::copy(body.begin() + kOffsetFilespaceUuid,
            body.begin() + kOffsetFilespaceUuid + result.state.first_filespace_uuid.value.bytes.size(),
            result.state.first_filespace_uuid.value.bytes.begin());
  result.state.page_size = Load32(body, kOffsetPageSize);
  if (result.state.page_size != expected_page_size) {
    return StartupReadError("SB-STARTUP-STATE-PAGE-SIZE-MISMATCH",
                            "storage.startup_state.page_size_mismatch",
                            std::to_string(result.state.page_size));
  }
  const u32 flags = Load32(body, kOffsetFlags);
  result.state.clean_shutdown = (flags & kFlagCleanShutdown) != 0;
  result.state.startup_dirty = (flags & kFlagStartupDirty) != 0;
  result.state.write_admission_fenced = (flags & kFlagWriteAdmissionFenced) != 0;
  result.state.config_authority_loaded = (flags & kFlagConfigAuthorityLoaded) != 0;
  result.state.security_authority_loaded = (flags & kFlagSecurityAuthorityLoaded) != 0;
  result.state.i18n_authority_loaded = (flags & kFlagI18nAuthorityLoaded) != 0;
  result.state.runtime_activation_complete = (flags & kFlagRuntimeActivationComplete) != 0;
  result.state.agent_runtime_started = (flags & kFlagAgentRuntimeStarted) != 0;
  result.state.cache_runtime_started = (flags & kFlagCacheRuntimeStarted) != 0;
  result.state.ipc_runtime_started = (flags & kFlagIpcRuntimeStarted) != 0;
  result.state.server_runtime_started = (flags & kFlagServerRuntimeStarted) != 0;
  result.state.startup_counter = Load64(body, kOffsetStartupCounter);
  result.state.restart_generation = Load64(body, kOffsetRestartGeneration);
  result.state.checkpoint_generation = Load64(body, kOffsetCheckpointGeneration);
  result.state.runtime_activation_generation = Load64(body, kOffsetRuntimeActivationGeneration);
  result.state.bootstrap_local_transaction_id = Load64(body, kOffsetBootstrapLocalTransactionId);
  result.state.first_open_activation_local_transaction_id =
      Load64(body, kOffsetFirstOpenActivationLocalTransactionId);
  result.state.clean_shutdown_local_transaction_id = Load64(body, kOffsetCleanShutdownLocalTransactionId);
  result.state.last_lifecycle_local_transaction_id = Load64(body, kOffsetLastLifecycleLocalTransactionId);
  result.state.last_lifecycle_event_unix_epoch_millis = Load64(body, kOffsetLastLifecycleEventUnixEpochMillis);
  result.state.lifecycle_generation = Load64(body, kOffsetLifecycleGeneration);
  const u16 classification_value = Load16(body, kOffsetRecoveryClassification);
  if (classification_value > static_cast<u16>(StartupRecoveryClassification::operator_review_required)) {
    return StartupReadError("SB-STARTUP-STATE-RECOVERY-CLASSIFICATION-INVALID",
                            "storage.startup_state.recovery_classification_invalid",
                            std::to_string(classification_value));
  }
  result.state.recovery_classification = static_cast<StartupRecoveryClassification>(classification_value);
  const u16 durable_phase_value = Load16(body, kOffsetDurableLifecyclePhase);
  if (durable_phase_value > static_cast<u16>(StartupLifecycleDurablePhase::drop_evidence_recorded)) {
    return StartupReadError("SB-STARTUP-STATE-DURABLE-LIFECYCLE-PHASE-INVALID",
                            "storage.startup_state.durable_lifecycle_phase_invalid",
                            std::to_string(durable_phase_value));
  }
  result.state.durable_lifecycle_phase = static_cast<StartupLifecycleDurablePhase>(durable_phase_value);
  result.state.durable_evidence_flags = Load64(body, kOffsetDurableEvidenceFlags);
  const u32 token_size = std::min(Load32(body, kOffsetOwnerTokenSize), kMaxOwnerTokenBytes);
  result.state.owner_token.assign(reinterpret_cast<const char*>(body.data() + kOffsetOwnerToken), token_size);
  return result;
}

}  // namespace

const char* StartupRecoveryClassificationName(StartupRecoveryClassification classification) {
  switch (classification) {
    case StartupRecoveryClassification::clean_checkpoint_path: return "clean_checkpoint_path";
    case StartupRecoveryClassification::checkpoint_rebuild_required: return "checkpoint_rebuild_required";
    case StartupRecoveryClassification::repaired_recovery: return "repaired_recovery";
    case StartupRecoveryClassification::fence_writes_until_safe: return "fence_writes_until_safe";
    case StartupRecoveryClassification::corruption_stop: return "corruption_stop";
    case StartupRecoveryClassification::restricted_open_required: return "restricted_open_required";
    case StartupRecoveryClassification::operator_review_required: return "operator_review_required";
  }
  return "operator_review_required";
}

const char* StartupLifecycleDurablePhaseName(StartupLifecycleDurablePhase phase) {
  switch (phase) {
    case StartupLifecycleDurablePhase::none: return "none";
    case StartupLifecycleDurablePhase::create_tx1_committed: return "create_tx1_committed";
    case StartupLifecycleDurablePhase::open_dirty_marked: return "open_dirty_marked";
    case StartupLifecycleDurablePhase::open_tx2_committed: return "open_tx2_committed";
    case StartupLifecycleDurablePhase::open_ready: return "open_ready";
    case StartupLifecycleDurablePhase::clean_shutdown: return "clean_shutdown";
    case StartupLifecycleDurablePhase::maintenance_entered: return "maintenance_entered";
    case StartupLifecycleDurablePhase::maintenance_exited: return "maintenance_exited";
    case StartupLifecycleDurablePhase::restricted_open_entered: return "restricted_open_entered";
    case StartupLifecycleDurablePhase::restricted_open_exited: return "restricted_open_exited";
    case StartupLifecycleDurablePhase::verify_completed: return "verify_completed";
    case StartupLifecycleDurablePhase::repair_completed: return "repair_completed";
    case StartupLifecycleDurablePhase::repair_refused: return "repair_refused";
    case StartupLifecycleDurablePhase::drop_evidence_recorded: return "drop_evidence_recorded";
  }
  return "none";
}

const char* StartupStateFormatCompatibilityClassName(
    StartupStateFormatCompatibilityClass compatibility_class) {
  switch (compatibility_class) {
    case StartupStateFormatCompatibilityClass::supported_current:
      return "supported-current";
    case StartupStateFormatCompatibilityClass::unsupported_old:
      return "unsupported-old";
    case StartupStateFormatCompatibilityClass::unsupported_new:
      return "unsupported-new";
    case StartupStateFormatCompatibilityClass::downgrade_refused:
      return "downgrade-refused";
    case StartupStateFormatCompatibilityClass::newer_than_supported_refused:
      return "newer-than-supported-refused";
    case StartupStateFormatCompatibilityClass::missing_migration_plan_refused:
      return "missing-migration-plan-refused";
    case StartupStateFormatCompatibilityClass::migration_required_without_plan_refused:
      return "migration-required-without-plan-refused";
  }
  return "unsupported-new";
}

StartupStateFormatCompatibilityResult ClassifyStartupStateFormatCompatibility(
    u32 format_major,
    u32 format_minor,
    const std::string& migration_plan_id,
    bool downgrade_requested,
    bool migration_plan_required) {
  const std::string detail = "startup_state_format=" + std::to_string(format_major) +
                             "." + std::to_string(format_minor);
  if (downgrade_requested) {
    return StartupFormatError(StartupStateFormatCompatibilityClass::downgrade_refused,
                              "SB-STARTUP-STATE-FORMAT-DOWNGRADE-REFUSED",
                              "storage.startup_state.format_downgrade_refused",
                              detail);
  }
  if (migration_plan_required && migration_plan_id.empty()) {
    return StartupFormatError(StartupStateFormatCompatibilityClass::missing_migration_plan_refused,
                              "SB-STARTUP-STATE-MIGRATION-PLAN-MISSING",
                              "storage.startup_state.migration_plan_missing",
                              detail);
  }
  if (format_major < kStartupStateFormatMajorMinSupported) {
    return StartupFormatError(StartupStateFormatCompatibilityClass::unsupported_old,
                              "SB-STARTUP-STATE-FORMAT-TOO-OLD",
                              "storage.startup_state.format_too_old",
                              detail);
  }
  if (format_major > kStartupStateFormatMajorMaxSupported ||
      (format_major == kStartupStateFormatMajorCurrent &&
       format_minor > kStartupStateFormatMinorMaxSupported)) {
    return StartupFormatError(StartupStateFormatCompatibilityClass::newer_than_supported_refused,
                              "SB-STARTUP-STATE-FORMAT-FUTURE",
                              "storage.startup_state.format_future",
                              detail);
  }
  if (format_minor < kStartupStateFormatMinorMinSupported) {
    return StartupFormatError(StartupStateFormatCompatibilityClass::downgrade_refused,
                              "SB-STARTUP-STATE-FORMAT-DOWNGRADE-REFUSED",
                              "storage.startup_state.format_downgrade_refused",
                              detail);
  }
  if (format_major == kStartupStateFormatMajorCurrent &&
      format_minor == kStartupStateFormatMinorCurrent) {
    return StartupFormatOk();
  }
  if (migration_plan_id.empty()) {
    return StartupFormatError(
        StartupStateFormatCompatibilityClass::migration_required_without_plan_refused,
        "SB-STARTUP-STATE-MIGRATION-REQUIRED-WITHOUT-PLAN",
        "storage.startup_state.migration_required_without_plan",
        detail);
  }
  return StartupFormatError(StartupStateFormatCompatibilityClass::missing_migration_plan_refused,
                            "SB-STARTUP-STATE-MIGRATION-PLAN-MISSING",
                            "storage.startup_state.migration_plan_missing",
                            detail + " plan=" + migration_plan_id);
}

StartupStateRecord MakeInitialStartupState(TypedUuid database_uuid,
                                           TypedUuid first_filespace_uuid,
                                           u32 page_size) {
  StartupStateRecord state;
  state.format_major = kStartupStateFormatMajorCurrent;
  state.format_minor = kStartupStateFormatMinorCurrent;
  state.database_uuid = database_uuid;
  state.first_filespace_uuid = first_filespace_uuid;
  state.page_size = page_size;
  state.clean_shutdown = true;
  state.startup_dirty = false;
  state.write_admission_fenced = false;
  state.config_authority_loaded = false;
  state.security_authority_loaded = false;
  state.i18n_authority_loaded = false;
  state.runtime_activation_complete = false;
  state.agent_runtime_started = false;
  state.cache_runtime_started = false;
  state.ipc_runtime_started = false;
  state.server_runtime_started = false;
  state.startup_counter = 1;
  state.restart_generation = 1;
  state.checkpoint_generation = 1;
  state.runtime_activation_generation = 0;
  state.bootstrap_local_transaction_id = 0;
  state.first_open_activation_local_transaction_id = 0;
  state.clean_shutdown_local_transaction_id = 0;
  state.last_lifecycle_local_transaction_id = 0;
  state.last_lifecycle_event_unix_epoch_millis = 0;
  state.lifecycle_generation = 0;
  state.durable_lifecycle_phase = StartupLifecycleDurablePhase::none;
  state.durable_evidence_flags = 0;
  state.recovery_classification = StartupRecoveryClassification::clean_checkpoint_path;
  state.completed_phases = {"create.database_header", "create.fixed_startup_map", "create.catalog_seed"};
  return state;
}

StartupStateRecord RecordStartupLifecycleEvidence(StartupStateRecord state,
                                                  StartupLifecycleDurablePhase phase,
                                                  u64 local_transaction_id,
                                                  u64 event_unix_epoch_millis,
                                                  u64 evidence_flags) {
  state.durable_lifecycle_phase = phase;
  if (local_transaction_id != 0) {
    state.last_lifecycle_local_transaction_id = local_transaction_id;
  }
  if (event_unix_epoch_millis != 0) {
    state.last_lifecycle_event_unix_epoch_millis = event_unix_epoch_millis;
  }
  state.durable_evidence_flags |= evidence_flags;
  state.lifecycle_generation += 1;
  return state;
}

bool StartupLifecycleEvidencePresent(const StartupStateRecord& state, u64 required_flags) {
  return (state.durable_evidence_flags & required_flags) == required_flags;
}

StartupWriteResult WriteStartupStatePageBody(FileDevice* device, const StartupStateRecord& state) {
  if (device == nullptr || !device->is_open()) {
    return StartupWriteError("SB-STARTUP-STATE-DEVICE-NOT-OPEN", "storage.startup_state.device_not_open");
  }
  const auto compatibility =
      ClassifyStartupStateFormatCompatibility(state.format_major, state.format_minor);
  if (!compatibility.ok()) {
    StartupWriteResult result;
    result.status = compatibility.status;
    result.diagnostic = compatibility.diagnostic;
    return result;
  }
  const auto body_offset = CheckedPageBodyOffset(state.page_size,
                                                 kSystemStatePageNumber,
                                                 kPageHeaderSerializedBytes);
  if (!body_offset.ok()) {
    StartupWriteResult result;
    result.status = body_offset.status;
    result.diagnostic = body_offset.diagnostic;
    return result;
  }
  const auto body = SerializeBody(state, state.page_size);
  const auto write = device->WriteAt(body_offset.offset,
                                     body.data(),
                                     body.size());
  if (!write.ok()) {
    StartupWriteResult result;
    result.status = write.status;
    result.diagnostic = write.diagnostic;
    return result;
  }
  StartupWriteResult result;
  result.status = StartupOkStatus();
  return result;
}

StartupStateResult ReadStartupStatePageBody(FileDevice* device, u32 page_size) {
  if (device == nullptr || !device->is_open()) {
    return StartupReadError("SB-STARTUP-STATE-DEVICE-NOT-OPEN", "storage.startup_state.device_not_open");
  }
  const auto body_offset = CheckedPageBodyOffset(page_size,
                                                 kSystemStatePageNumber,
                                                 kPageHeaderSerializedBytes);
  if (!body_offset.ok()) {
    StartupStateResult result;
    result.status = body_offset.status;
    result.diagnostic = body_offset.diagnostic;
    return result;
  }
  std::vector<unsigned char> body(page_size - kPageHeaderSerializedBytes, 0);
  const auto read = device->ReadAt(body_offset.offset,
                                  body.data(),
                                  body.size());
  if (!read.ok()) {
    StartupStateResult result;
    result.status = read.status;
    result.diagnostic = read.diagnostic;
    return result;
  }
  return ParseBody(body, page_size);
}

StartupStateRecord MarkStartupDirty(StartupStateRecord state,
                                    std::string owner_token,
                                    StartupRecoveryClassification prior_state_classification) {
  state.clean_shutdown = false;
  state.startup_dirty = true;
  state.write_admission_fenced = true;
  state.owner_token = std::move(owner_token);
  state.startup_counter += 1;
  if (prior_state_classification != StartupRecoveryClassification::clean_checkpoint_path) {
    state.restart_generation += 1;
  }
  state.recovery_classification = prior_state_classification;
  state.completed_phases.push_back("open.startup_dirty_marked");
  return state;
}

StartupStateRecord MarkStartupClean(StartupStateRecord state) {
  state.clean_shutdown = true;
  state.startup_dirty = false;
  state.write_admission_fenced = false;
  state.agent_runtime_started = false;
  state.cache_runtime_started = false;
  state.ipc_runtime_started = false;
  state.server_runtime_started = false;
  state.owner_token.clear();
  state.recovery_classification = StartupRecoveryClassification::clean_checkpoint_path;
  state.completed_phases.push_back("close.runtime_agents_stopped");
  state.completed_phases.push_back("close.cache_runtime_stopped");
  state.completed_phases.push_back("close.ipc_runtime_stopped");
  state.completed_phases.push_back("close.server_runtime_stopped");
  state.completed_phases.push_back("close.clean_shutdown_marked");
  return state;
}

DiagnosticRecord MakeStartupStateDiagnostic(Status status,
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
                        "storage.startup_state");
}

}  // namespace scratchbird::storage::database
