// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "repair_event_ledger.hpp"

#include "disk_device.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

namespace scratchbird::storage::database {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::UuidKind;

Status LedgerOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::storage_disk};
}

Status LedgerErrorStatus() {
  return {StatusCode::platform_required_feature_missing,
          Severity::error,
          Subsystem::storage_disk};
}

RepairEventLedgerResult LedgerError(std::string diagnostic_code,
                                    std::string message_key,
                                    std::string detail = {}) {
  RepairEventLedgerResult result;
  result.status = LedgerErrorStatus();
  result.diagnostic = MakeRepairEventLedgerDiagnostic(
      result.status,
      std::move(diagnostic_code),
      std::move(message_key),
      std::move(detail));
  return result;
}

std::string BoolField(bool value) { return value ? "1" : "0"; }

bool ParseBoolField(const std::string& value, bool* parsed) {
  if (value == "1") {
    *parsed = true;
    return true;
  }
  if (value == "0") {
    *parsed = false;
    return true;
  }
  return false;
}

u64 FnvUpdate(u64 hash, const std::string& text) {
  for (const unsigned char ch : text) {
    hash ^= static_cast<u64>(ch);
    hash *= 1099511628211ull;
  }
  hash ^= static_cast<u64>('|');
  hash *= 1099511628211ull;
  return hash;
}

bool ContainsDelimiter(const std::string& text) {
  return text.find('|') != std::string::npos ||
         text.find('\n') != std::string::npos ||
         text.find('\r') != std::string::npos;
}

std::string Lower(std::string value) {
  for (char& ch : value) {
    ch = static_cast<char>(
        std::tolower(static_cast<unsigned char>(ch)));
  }
  return value;
}

bool ContainsUnsafeAuthorityText(const std::string& text) {
  const std::string lower = Lower(text);
  return lower.find("raw_sql") != std::string::npos ||
         lower.find("parser_authority") != std::string::npos ||
         lower.find("donor_authority") != std::string::npos ||
         lower.find("name_authority") != std::string::npos;
}

std::string UuidField(const TypedUuid& value) {
  if (!value.valid()) {
    return "-";
  }
  return scratchbird::core::uuid::UuidToString(value.value);
}

bool SameTypedUuid(const TypedUuid& left, const TypedUuid& right) {
  return left.kind == right.kind && left.value == right.value;
}

RepairEventLedgerResult ParseUuidField(const std::string& text,
                                       UuidKind kind,
                                       bool required,
                                       TypedUuid* out) {
  if (text == "-") {
    if (required) {
      return LedgerError("SB-REPAIR-EVENT-UUID-REQUIRED",
                         "storage.repair_event_ledger.uuid_required");
    }
    *out = TypedUuid{};
    return RepairEventLedgerResult{LedgerOkStatus(), {}, {}, {}, {}};
  }
  const auto parsed = scratchbird::core::uuid::ParseTypedUuid(kind, text);
  if (!parsed.ok()) {
    RepairEventLedgerResult result;
    result.status = parsed.status;
    result.diagnostic = parsed.diagnostic;
    return result;
  }
  *out = parsed.value;
  return RepairEventLedgerResult{LedgerOkStatus(), {}, {}, {}, {}};
}

bool ParseU64(const std::string& text, u64* value) {
  try {
    std::size_t consumed = 0;
    const unsigned long long parsed = std::stoull(text, &consumed, 10);
    if (consumed != text.size()) {
      return false;
    }
    *value = static_cast<u64>(parsed);
    return true;
  } catch (...) {
    return false;
  }
}

RepairEventPhase ParsePhase(const std::string& text) {
  if (text == "finding_recorded") {
    return RepairEventPhase::finding_recorded;
  }
  if (text == "scan_admission") {
    return RepairEventPhase::scan_admission;
  }
  if (text == "mutation_admission") {
    return RepairEventPhase::mutation_admission;
  }
  if (text == "page_quarantined") {
    return RepairEventPhase::page_quarantined;
  }
  if (text == "page_review_blocked") {
    return RepairEventPhase::page_review_blocked;
  }
  if (text == "retention_hold_recorded") {
    return RepairEventPhase::retention_hold_recorded;
  }
  if (text == "retention_purge_blocked") {
    return RepairEventPhase::retention_purge_blocked;
  }
  if (text == "crash_resume_started") {
    return RepairEventPhase::crash_resume_started;
  }
  if (text == "crash_resume_replay_admitted") {
    return RepairEventPhase::crash_resume_replay_admitted;
  }
  if (text == "crash_resume_completed") {
    return RepairEventPhase::crash_resume_completed;
  }
  return RepairEventPhase::unknown;
}

std::vector<std::string> SplitFields(const std::string& serialized) {
  std::vector<std::string> fields;
  std::string field;
  std::istringstream in(serialized);
  while (std::getline(in, field, '|')) {
    fields.push_back(field);
  }
  return fields;
}

std::string CanonicalRepairEventPayload(const RepairEventRecord& event,
                                        bool include_digest) {
  std::vector<std::string> fields = {
      "SB_REPAIR_EVENT_V1",
      std::to_string(event.sequence),
      std::to_string(event.ledger_epoch),
      RepairEventPhaseName(event.phase),
      UuidField(event.database_uuid),
      UuidField(event.operation_uuid),
      UuidField(event.finding_uuid),
      UuidField(event.page_uuid),
      UuidField(event.object_uuid),
      UuidField(event.row_uuid),
      UuidField(event.version_uuid),
      UuidField(event.transaction_uuid),
      std::to_string(event.local_transaction_id),
      std::to_string(event.page_number),
      std::to_string(event.page_generation),
      scratchbird::storage::disk::PageTypeName(event.page_type),
      std::to_string(event.observed_header_checksum),
      std::to_string(event.observed_body_checksum_low64),
      std::to_string(event.observed_body_checksum_high64),
      std::to_string(event.previous_event_digest),
      include_digest ? std::to_string(event.event_digest) : "0",
      BoolField(event.authority.durable_mga_inventory_authority),
      BoolField(event.authority.normal_mga_visibility_recheck_required),
      BoolField(event.authority
                    .repair_evidence_is_transaction_finality_authority),
      BoolField(event.authority.repair_evidence_is_visibility_authority),
      BoolField(event.authority.repair_evidence_is_recovery_authority),
      BoolField(event.authority.parser_or_donor_authority),
      BoolField(event.authority.names_are_authority),
      BoolField(event.authority.sblr_or_internal_operation),
      event.reason_code,
      event.stable_detail};

  std::ostringstream out;
  for (std::size_t i = 0; i < fields.size(); ++i) {
    if (i != 0) {
      out << '|';
    }
    out << fields[i];
  }
  return out.str();
}

RepairEventLedgerResult ValidateRepairEvent(const RepairEventRecord& event,
                                            bool require_digest) {
  if (event.format_version != kRepairEventLedgerFormatVersion) {
    return LedgerError("SB-REPAIR-EVENT-FORMAT-UNSUPPORTED",
                       "storage.repair_event_ledger.format_unsupported",
                       std::to_string(event.format_version));
  }
  if (event.sequence == 0 || event.ledger_epoch == 0) {
    return LedgerError("SB-REPAIR-EVENT-SEQUENCE-INVALID",
                       "storage.repair_event_ledger.sequence_invalid");
  }
  if (event.phase == RepairEventPhase::unknown) {
    return LedgerError("SB-REPAIR-EVENT-PHASE-INVALID",
                       "storage.repair_event_ledger.phase_invalid");
  }
  if (!event.database_uuid.valid() || !event.operation_uuid.valid() ||
      !event.finding_uuid.valid() || !event.page_uuid.valid()) {
    return LedgerError("SB-REPAIR-EVENT-UUID-REQUIRED",
                       "storage.repair_event_ledger.uuid_required");
  }
  if (event.database_uuid.kind != UuidKind::database ||
      event.operation_uuid.kind != UuidKind::object ||
      event.finding_uuid.kind != UuidKind::object ||
      event.page_uuid.kind != UuidKind::page) {
    return LedgerError("SB-REPAIR-EVENT-UUID-KIND-MISMATCH",
                       "storage.repair_event_ledger.uuid_kind_mismatch");
  }
  if (event.page_number == 0 || event.page_type == PageType::unknown) {
    return LedgerError("SB-REPAIR-EVENT-PAGE-IDENTITY-REQUIRED",
                       "storage.repair_event_ledger.page_identity_required");
  }
  if (event.reason_code.empty() || ContainsDelimiter(event.reason_code) ||
      ContainsDelimiter(event.stable_detail) ||
      ContainsUnsafeAuthorityText(event.reason_code) ||
      ContainsUnsafeAuthorityText(event.stable_detail)) {
    return LedgerError("SB-REPAIR-EVENT-REASON-INVALID",
                       "storage.repair_event_ledger.reason_invalid");
  }
  if (!event.authority.durable_mga_inventory_authority ||
      !event.authority.normal_mga_visibility_recheck_required ||
      event.authority.repair_evidence_is_transaction_finality_authority ||
      event.authority.repair_evidence_is_visibility_authority ||
      event.authority.repair_evidence_is_recovery_authority ||
      event.authority.parser_or_donor_authority ||
      event.authority.names_are_authority ||
      !event.authority.sblr_or_internal_operation) {
    return LedgerError("SB-REPAIR-EVENT-AUTHORITY-REFUSED",
                       "storage.repair_event_ledger.authority_refused");
  }
  if (require_digest && event.event_digest == 0) {
    return LedgerError("SB-REPAIR-EVENT-DIGEST-REQUIRED",
                       "storage.repair_event_ledger.digest_required");
  }
  RepairEventLedgerResult result;
  result.status = LedgerOkStatus();
  return result;
}

RepairEventLedgerResult AppendChainError(std::string detail) {
  return LedgerError("SB-REPAIR-EVENT-LEDGER-CHAIN-INVALID",
                     "storage.repair_event_ledger.chain_invalid",
                     std::move(detail));
}

bool SameRepairIdentity(const RepairEventRecord& event,
                        const RepairAccessRequest& request) {
  return SameTypedUuid(event.operation_uuid, request.operation_uuid) &&
         SameTypedUuid(event.finding_uuid, request.finding_uuid) &&
         SameTypedUuid(event.page_uuid, request.page_uuid) &&
         event.page_number == request.page_number;
}

RepairEventPhase RequiredPhaseForIntent(RepairAccessIntent intent) {
  switch (intent) {
    case RepairAccessIntent::repair_scan:
      return RepairEventPhase::scan_admission;
    case RepairAccessIntent::repair_mutation:
      return RepairEventPhase::mutation_admission;
    case RepairAccessIntent::normal_access:
      return RepairEventPhase::unknown;
  }
  return RepairEventPhase::unknown;
}

RepairAccessDecision AccessDecisionError(std::string diagnostic_code,
                                         std::string message_key,
                                         std::string detail = {}) {
  RepairAccessDecision decision;
  decision.status = LedgerErrorStatus();
  decision.diagnostic = MakeRepairEventLedgerDiagnostic(
      decision.status,
      std::move(diagnostic_code),
      std::move(message_key),
      std::move(detail));
  decision.repair_evidence_is_transaction_authority = false;
  decision.evidence.push_back("durable_mga_inventory_authority_required=true");
  decision.evidence.push_back("repair_evidence_transaction_authority=false");
  return decision;
}

RepairEventRetentionDecision RetentionDecisionError(
    std::string diagnostic_code,
    std::string message_key,
    std::string detail = {}) {
  RepairEventRetentionDecision decision;
  decision.status = LedgerErrorStatus();
  decision.diagnostic = MakeRepairEventLedgerDiagnostic(
      decision.status,
      std::move(diagnostic_code),
      std::move(message_key),
      std::move(detail));
  decision.repair_evidence_is_transaction_authority = false;
  decision.evidence.push_back("repair_evidence_transaction_authority=false");
  decision.evidence.push_back("retention_decision_fail_closed=true");
  return decision;
}

RepairCrashResumeDecision CrashResumeDecisionError(
    std::string diagnostic_code,
    std::string message_key,
    std::string detail = {}) {
  RepairCrashResumeDecision decision;
  decision.status = LedgerErrorStatus();
  decision.diagnostic = MakeRepairEventLedgerDiagnostic(
      decision.status,
      std::move(diagnostic_code),
      std::move(message_key),
      std::move(detail));
  decision.repair_evidence_is_recovery_authority = false;
  decision.evidence.push_back("repair_evidence_recovery_authority=false");
  decision.evidence.push_back("crash_resume_decision_fail_closed=true");
  return decision;
}

bool RepairPhaseRequiresCrashResume(RepairEventPhase phase) {
  switch (phase) {
    case RepairEventPhase::finding_recorded:
    case RepairEventPhase::scan_admission:
    case RepairEventPhase::mutation_admission:
    case RepairEventPhase::page_quarantined:
    case RepairEventPhase::page_review_blocked:
    case RepairEventPhase::retention_hold_recorded:
    case RepairEventPhase::retention_purge_blocked:
    case RepairEventPhase::crash_resume_started:
    case RepairEventPhase::crash_resume_replay_admitted:
      return true;
    case RepairEventPhase::crash_resume_completed:
    case RepairEventPhase::unknown:
      return false;
  }
  return false;
}

bool RepairLedgerStateIsTrusted(const RepairEventLedger& ledger) {
  if (!ledger.verified_append_only) {
    return false;
  }

  u64 expected_sequence = 1;
  u64 previous_digest = 0;
  for (const RepairEventRecord& event : ledger.events) {
    if (event.sequence != expected_sequence ||
        event.previous_event_digest != previous_digest) {
      return false;
    }
    const auto validated = ValidateRepairEvent(event, true);
    if (!validated.ok()) {
      return false;
    }
    if (ComputeRepairEventDigest(event) != event.event_digest) {
      return false;
    }
    previous_digest = event.event_digest;
    ++expected_sequence;
  }

  const u64 expected_last_sequence =
      ledger.events.empty() ? 0 : ledger.events.back().sequence;
  return ledger.last_sequence == expected_last_sequence &&
         ledger.last_event_digest == previous_digest;
}

}  // namespace

const char* RepairEventPhaseName(RepairEventPhase phase) {
  switch (phase) {
    case RepairEventPhase::finding_recorded: return "finding_recorded";
    case RepairEventPhase::scan_admission: return "scan_admission";
    case RepairEventPhase::mutation_admission: return "mutation_admission";
    case RepairEventPhase::page_quarantined: return "page_quarantined";
    case RepairEventPhase::page_review_blocked: return "page_review_blocked";
    case RepairEventPhase::retention_hold_recorded: return "retention_hold_recorded";
    case RepairEventPhase::retention_purge_blocked: return "retention_purge_blocked";
    case RepairEventPhase::crash_resume_started: return "crash_resume_started";
    case RepairEventPhase::crash_resume_replay_admitted: return "crash_resume_replay_admitted";
    case RepairEventPhase::crash_resume_completed: return "crash_resume_completed";
    case RepairEventPhase::unknown: return "unknown";
  }
  return "unknown";
}

const char* RepairAccessIntentName(RepairAccessIntent intent) {
  switch (intent) {
    case RepairAccessIntent::normal_access: return "normal_access";
    case RepairAccessIntent::repair_scan: return "repair_scan";
    case RepairAccessIntent::repair_mutation: return "repair_mutation";
  }
  return "normal_access";
}

u64 ComputeRepairEventDigest(const RepairEventRecord& event) {
  const std::string canonical = CanonicalRepairEventPayload(event, false);
  u64 hash = 1469598103934665603ull;
  hash = FnvUpdate(hash, canonical);
  return hash == 0 ? 1 : hash;
}

RepairEventLedgerResult BuildRepairEventRecord(RepairEventRecord event) {
  const auto validated = ValidateRepairEvent(event, false);
  if (!validated.ok()) {
    return validated;
  }
  event.event_digest = ComputeRepairEventDigest(event);
  RepairEventLedgerResult result;
  result.status = LedgerOkStatus();
  result.event = event;
  result.serialized = CanonicalRepairEventPayload(event, true);
  return result;
}

RepairEventLedgerResult SerializeRepairEventRecord(
    const RepairEventRecord& event) {
  const auto validated = ValidateRepairEvent(event, true);
  if (!validated.ok()) {
    return validated;
  }
  const u64 expected = ComputeRepairEventDigest(event);
  if (expected != event.event_digest) {
    return LedgerError("SB-REPAIR-EVENT-DIGEST-MISMATCH",
                       "storage.repair_event_ledger.digest_mismatch");
  }
  RepairEventLedgerResult result;
  result.status = LedgerOkStatus();
  result.event = event;
  result.serialized = CanonicalRepairEventPayload(event, true);
  return result;
}

RepairEventLedgerResult ParseRepairEventRecord(const std::string& serialized) {
  const auto fields = SplitFields(serialized);
  if (fields.size() != 31 || fields[0] != "SB_REPAIR_EVENT_V1") {
    return LedgerError("SB-REPAIR-EVENT-CODEC-INVALID",
                       "storage.repair_event_ledger.codec_invalid");
  }

  RepairEventRecord event;
  event.format_version = kRepairEventLedgerFormatVersion;
  u64 page_type_value = 0;
  if (!ParseU64(fields[1], &event.sequence) ||
      !ParseU64(fields[2], &event.ledger_epoch) ||
      !ParseU64(fields[12], &event.local_transaction_id) ||
      !ParseU64(fields[13], &event.page_number) ||
      !ParseU64(fields[14], &event.page_generation) ||
      !ParseU64(fields[16], &event.observed_header_checksum) ||
      !ParseU64(fields[17], &event.observed_body_checksum_low64) ||
      !ParseU64(fields[18], &event.observed_body_checksum_high64) ||
      !ParseU64(fields[19], &event.previous_event_digest) ||
      !ParseU64(fields[20], &event.event_digest)) {
    return LedgerError("SB-REPAIR-EVENT-CODEC-NUMBER-INVALID",
                       "storage.repair_event_ledger.codec_number_invalid");
  }
  event.phase = ParsePhase(fields[3]);

  const auto parse_database =
      ParseUuidField(fields[4], UuidKind::database, true, &event.database_uuid);
  if (!parse_database.ok()) { return parse_database; }
  const auto parse_operation =
      ParseUuidField(fields[5], UuidKind::object, true, &event.operation_uuid);
  if (!parse_operation.ok()) { return parse_operation; }
  const auto parse_finding =
      ParseUuidField(fields[6], UuidKind::object, true, &event.finding_uuid);
  if (!parse_finding.ok()) { return parse_finding; }
  const auto parse_page =
      ParseUuidField(fields[7], UuidKind::page, true, &event.page_uuid);
  if (!parse_page.ok()) { return parse_page; }
  const auto parse_object =
      ParseUuidField(fields[8], UuidKind::object, false, &event.object_uuid);
  if (!parse_object.ok()) { return parse_object; }
  const auto parse_row =
      ParseUuidField(fields[9], UuidKind::row, false, &event.row_uuid);
  if (!parse_row.ok()) { return parse_row; }
  const auto parse_version =
      ParseUuidField(fields[10], UuidKind::row, false, &event.version_uuid);
  if (!parse_version.ok()) { return parse_version; }
  const auto parse_transaction = ParseUuidField(
      fields[11], UuidKind::transaction, false, &event.transaction_uuid);
  if (!parse_transaction.ok()) { return parse_transaction; }

  for (const PageType declared : scratchbird::storage::disk::kDeclaredPageTypes) {
    if (fields[15] == scratchbird::storage::disk::PageTypeName(declared)) {
      event.page_type = declared;
      break;
    }
  }
  if (event.page_type == PageType::unknown && ParseU64(fields[15], &page_type_value)) {
    event.page_type = static_cast<PageType>(page_type_value);
  }

  if (!ParseBoolField(fields[21],
                      &event.authority.durable_mga_inventory_authority) ||
      !ParseBoolField(fields[22],
                      &event.authority.normal_mga_visibility_recheck_required) ||
      !ParseBoolField(
          fields[23],
          &event.authority
               .repair_evidence_is_transaction_finality_authority) ||
      !ParseBoolField(
          fields[24],
          &event.authority.repair_evidence_is_visibility_authority) ||
      !ParseBoolField(fields[25],
                      &event.authority.repair_evidence_is_recovery_authority) ||
      !ParseBoolField(fields[26],
                      &event.authority.parser_or_donor_authority) ||
      !ParseBoolField(fields[27], &event.authority.names_are_authority) ||
      !ParseBoolField(fields[28],
                      &event.authority.sblr_or_internal_operation)) {
    return LedgerError("SB-REPAIR-EVENT-CODEC-BOOL-INVALID",
                       "storage.repair_event_ledger.codec_bool_invalid");
  }
  event.reason_code = fields[29];
  event.stable_detail = fields[30];

  const auto validated = ValidateRepairEvent(event, true);
  if (!validated.ok()) {
    return validated;
  }
  const u64 expected = ComputeRepairEventDigest(event);
  if (expected != event.event_digest) {
    return LedgerError("SB-REPAIR-EVENT-DIGEST-MISMATCH",
                       "storage.repair_event_ledger.digest_mismatch");
  }

  RepairEventLedgerResult result;
  result.status = LedgerOkStatus();
  result.event = event;
  result.serialized = CanonicalRepairEventPayload(event, true);
  return result;
}

RepairEventLedgerResult LoadRepairEventLedger(const std::string& ledger_path) {
  if (ledger_path.empty()) {
    return LedgerError("SB-REPAIR-EVENT-LEDGER-PATH-REQUIRED",
                       "storage.repair_event_ledger.path_required");
  }

  RepairEventLedger ledger;
  ledger.verified_append_only = true;
  if (!std::filesystem::exists(ledger_path)) {
    RepairEventLedgerResult result;
    result.status = LedgerOkStatus();
    result.ledger = std::move(ledger);
    return result;
  }

  std::ifstream in(ledger_path, std::ios::binary);
  if (!in.is_open()) {
    return LedgerError("SB-REPAIR-EVENT-LEDGER-OPEN-FAILED",
                       "storage.repair_event_ledger.open_failed",
                       ledger_path);
  }

  std::string line;
  u64 expected_sequence = 1;
  u64 previous_digest = 0;
  while (std::getline(in, line)) {
    if (line.empty()) {
      continue;
    }
    const auto parsed = ParseRepairEventRecord(line);
    if (!parsed.ok()) {
      return parsed;
    }
    const RepairEventRecord& event = parsed.event;
    if (event.sequence != expected_sequence ||
        event.previous_event_digest != previous_digest) {
      return AppendChainError(std::to_string(event.sequence));
    }
    ledger.last_sequence = event.sequence;
    ledger.last_event_digest = event.event_digest;
    previous_digest = event.event_digest;
    ++expected_sequence;
    ledger.events.push_back(event);
  }

  RepairEventLedgerResult result;
  result.status = LedgerOkStatus();
  result.ledger = std::move(ledger);
  return result;
}

RepairEventLedgerResult AppendRepairEventToLedger(
    const std::string& ledger_path,
    RepairEventRecord event) {
  const auto loaded = LoadRepairEventLedger(ledger_path);
  if (!loaded.ok()) {
    return loaded;
  }
  const u64 next_sequence = loaded.ledger.last_sequence + 1;
  if (event.sequence != next_sequence ||
      event.previous_event_digest != loaded.ledger.last_event_digest) {
    return AppendChainError(std::to_string(event.sequence));
  }

  const auto built = BuildRepairEventRecord(event);
  if (!built.ok()) {
    return built;
  }
  const std::string line = built.serialized + "\n";

  scratchbird::storage::disk::FileDevice device;
  const bool exists = std::filesystem::exists(ledger_path);
  const auto open = device.Open(ledger_path,
                                exists ? scratchbird::storage::disk::
                                             FileOpenMode::open_existing
                                       : scratchbird::storage::disk::
                                             FileOpenMode::create_new);
  if (!open.ok()) {
    RepairEventLedgerResult result;
    result.status = open.status;
    result.diagnostic = open.diagnostic;
    return result;
  }
  const auto size = device.Size();
  if (!size.ok()) {
    RepairEventLedgerResult result;
    result.status = size.status;
    result.diagnostic = size.diagnostic;
    return result;
  }
  const auto write =
      device.WriteAt(size.size_bytes, line.data(), line.size());
  if (!write.ok()) {
    RepairEventLedgerResult result;
    result.status = write.status;
    result.diagnostic = write.diagnostic;
    return result;
  }
  const auto sync = device.Sync();
  if (!sync.ok()) {
    RepairEventLedgerResult result;
    result.status = sync.status;
    result.diagnostic = sync.diagnostic;
    return result;
  }
  const auto close = device.Close();
  if (!close.ok()) {
    RepairEventLedgerResult result;
    result.status = close.status;
    result.diagnostic = close.diagnostic;
    return result;
  }

  auto verified = LoadRepairEventLedger(ledger_path);
  if (!verified.ok()) {
    return verified;
  }
  if (verified.ledger.last_event_digest != built.event.event_digest) {
    return AppendChainError("post_append_digest");
  }
  verified.event = built.event;
  verified.serialized = built.serialized;
  return verified;
}

RepairAccessDecision AdmitRepairAccessFromLedger(
    const RepairEventLedger& ledger,
    const RepairAccessRequest& request) {
  if (!RepairLedgerStateIsTrusted(ledger)) {
    return AccessDecisionError("SB-REPAIR-ACCESS-LEDGER-UNVERIFIED",
                               "storage.repair_event_ledger.unverified");
  }
  if (!request.durable_mga_inventory_authority ||
      request.repair_evidence_is_transaction_authority ||
      request.parser_or_donor_authority ||
      request.names_are_authority) {
    return AccessDecisionError("SB-REPAIR-ACCESS-AUTHORITY-REFUSED",
                               "storage.repair_event_ledger.access_authority_refused");
  }

  RepairAccessDecision decision;
  decision.status = LedgerOkStatus();
  decision.evidence.push_back("durable_mga_inventory_authority=true");
  decision.evidence.push_back("repair_evidence_transaction_authority=false");
  decision.evidence.push_back(std::string("intent=") +
                              RepairAccessIntentName(request.intent));

  if (request.intent == RepairAccessIntent::normal_access) {
    for (const RepairEventRecord& event : ledger.events) {
      if (SameTypedUuid(event.page_uuid, request.page_uuid) &&
          event.page_number == request.page_number &&
          (event.phase == RepairEventPhase::page_quarantined ||
           event.phase == RepairEventPhase::page_review_blocked)) {
        return AccessDecisionError("SB-REPAIR-ACCESS-PAGE-QUARANTINED",
                                   "storage.repair_event_ledger.page_quarantined",
                                   std::to_string(event.sequence));
      }
    }
    decision.admitted = true;
    decision.normal_access_allowed = true;
    return decision;
  }

  const RepairEventPhase required_phase =
      RequiredPhaseForIntent(request.intent);
  for (const RepairEventRecord& event : ledger.events) {
    if (event.phase == required_phase &&
        SameRepairIdentity(event, request)) {
      decision.admitted = true;
      decision.prior_event_persisted = true;
      decision.prior_event_digest = event.event_digest;
      decision.scan_allowed =
          request.intent == RepairAccessIntent::repair_scan;
      decision.mutation_allowed =
          request.intent == RepairAccessIntent::repair_mutation;
      decision.evidence.push_back(std::string("prior_event_phase=") +
                                  RepairEventPhaseName(event.phase));
      decision.evidence.push_back("prior_event_digest=" +
                                  std::to_string(event.event_digest));
      return decision;
    }
  }

  return AccessDecisionError("SB-REPAIR-ACCESS-PRIOR-EVENT-REQUIRED",
                             "storage.repair_event_ledger.prior_event_required",
                             RepairAccessIntentName(request.intent));
}

RepairEventRetentionDecision EvaluateRepairEventRetention(
    const RepairEventRetentionRequest& request) {
  if (!RepairLedgerStateIsTrusted(request.ledger)) {
    return RetentionDecisionError("SB-REPAIR-RETENTION-LEDGER-UNVERIFIED",
                                  "storage.repair_event_ledger.retention_unverified");
  }
  if (request.repair_evidence_is_transaction_authority ||
      request.parser_or_donor_authority ||
      request.names_are_authority) {
    return RetentionDecisionError("SB-REPAIR-RETENTION-AUTHORITY-REFUSED",
                                  "storage.repair_event_ledger.retention_authority_refused");
  }
  if (!request.durable_retention_policy_loaded) {
    return RetentionDecisionError("SB-REPAIR-RETENTION-POLICY-REQUIRED",
                                  "storage.repair_event_ledger.retention_policy_required");
  }

  RepairEventRetentionDecision decision;
  decision.status = LedgerOkStatus();
  decision.evaluated = true;
  decision.tamper_chain_verified = true;
  decision.repair_evidence_is_transaction_authority = false;
  decision.legal_hold_blocker = request.legal_hold_active;
  decision.maintenance_hold_blocker = request.maintenance_hold_active;
  decision.retention_deadline_blocker =
      request.retention_deadline_epoch_millis != 0 &&
      request.now_epoch_millis < request.retention_deadline_epoch_millis;
  decision.purge_blocked = request.purge_requested &&
                           (decision.legal_hold_blocker ||
                            decision.maintenance_hold_blocker ||
                            decision.retention_deadline_blocker);
  decision.purge_allowed = request.purge_requested && !decision.purge_blocked;
  decision.evidence.push_back("repair_ledger_append_only_verified=true");
  decision.evidence.push_back("repair_evidence_transaction_authority=false");
  decision.evidence.push_back("retention_policy_loaded=true");
  decision.evidence.push_back(std::string("legal_hold_active=") +
                              BoolField(request.legal_hold_active));
  decision.evidence.push_back(std::string("maintenance_hold_active=") +
                              BoolField(request.maintenance_hold_active));
  decision.evidence.push_back(std::string("purge_requested=") +
                              BoolField(request.purge_requested));
  decision.evidence.push_back("retention_deadline_epoch_millis=" +
                              std::to_string(
                                  request.retention_deadline_epoch_millis));
  decision.evidence.push_back(std::string("purge_allowed=") +
                              BoolField(decision.purge_allowed));
  decision.evidence.push_back(std::string("purge_blocked=") +
                              BoolField(decision.purge_blocked));
  decision.evidence.push_back("last_repair_event_digest=" +
                              std::to_string(request.ledger.last_event_digest));
  if (decision.purge_blocked) {
    decision.diagnostic = MakeRepairEventLedgerDiagnostic(
        LedgerOkStatus(),
        "SB-REPAIR-RETENTION-PURGE-BLOCKED",
        "storage.repair_event_ledger.retention_purge_blocked",
        decision.legal_hold_blocker
            ? "legal_hold"
            : (decision.maintenance_hold_blocker ? "maintenance_hold"
                                                 : "retention_deadline"));
  }
  return decision;
}

RepairCrashResumeDecision EvaluateRepairCrashResumeFromLedger(
    const RepairCrashResumeRequest& request) {
  if (!RepairLedgerStateIsTrusted(request.ledger)) {
    return CrashResumeDecisionError("SB-REPAIR-CRASH-RESUME-LEDGER-UNVERIFIED",
                                    "storage.repair_event_ledger.crash_resume_unverified");
  }
  if (!request.durable_mga_inventory_authority ||
      request.repair_evidence_is_recovery_authority ||
      request.parser_or_donor_authority ||
      request.names_are_authority) {
    return CrashResumeDecisionError("SB-REPAIR-CRASH-RESUME-AUTHORITY-REFUSED",
                                    "storage.repair_event_ledger.crash_resume_authority_refused");
  }

  RepairCrashResumeDecision decision;
  decision.status = LedgerOkStatus();
  decision.evaluated = true;
  decision.tamper_chain_verified = true;
  decision.repair_evidence_is_recovery_authority = false;
  decision.last_event_digest = request.ledger.last_event_digest;
  if (!request.ledger.events.empty()) {
    decision.last_phase = request.ledger.events.back().phase;
  }
  decision.resume_required =
      request.crash_recovery_open &&
      RepairPhaseRequiresCrashResume(decision.last_phase);
  decision.replay_required =
      decision.resume_required &&
      (decision.last_phase != RepairEventPhase::crash_resume_completed);
  decision.completed =
      !decision.resume_required ||
      decision.last_phase == RepairEventPhase::crash_resume_completed;
  decision.evidence.push_back("repair_ledger_append_only_verified=true");
  decision.evidence.push_back("durable_mga_inventory_authority=true");
  decision.evidence.push_back("repair_evidence_recovery_authority=false");
  decision.evidence.push_back(std::string("last_phase=") +
                              RepairEventPhaseName(decision.last_phase));
  decision.evidence.push_back("last_repair_event_digest=" +
                              std::to_string(decision.last_event_digest));
  return decision;
}

DiagnosticRecord MakeRepairEventLedgerDiagnostic(Status status,
                                                 std::string diagnostic_code,
                                                 std::string message_key,
                                                 std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "storage.database.repair_event_ledger");
}

}  // namespace scratchbird::storage::database
