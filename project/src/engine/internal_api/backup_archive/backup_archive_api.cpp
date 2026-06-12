// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "backup_archive/backup_archive_api.hpp"

#include "api_diagnostics.hpp"
#include "behavior_support/api_behavior_store.hpp"
#include "crud_support/crud_store.hpp"
#include "filespace_lifecycle.hpp"
#include "metric_contracts.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "security/security_model.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api {
namespace {

constexpr const char* kLogicalBackupMagic = "SBLOGICALBACKUP1";
constexpr const char* kPhysicalBackupMagic = "SBPHYSICALBACKUP1";
constexpr const char* kDeltaPackageMagic = "SBDELTAPACKAGE1";
constexpr const char* kArchiveBeforeReclaimMagic = "SBARCHIVERECLAIM1";
constexpr std::uint64_t kDefaultArchiveMaxAgeMicroseconds = 604800000000ull;

namespace filespace = scratchbird::storage::filespace;
namespace catalog = scratchbird::core::catalog;
namespace mga = scratchbird::transaction::mga;

bool StartsWith(const std::string& value, const std::string& prefix) {
  return value.rfind(prefix, 0) == 0;
}

std::string OptionValue(const EngineApiRequest& request, const std::string& prefix) {
  for (const auto& option : request.option_envelopes) {
    if (StartsWith(option, prefix)) {
      return option.substr(prefix.size());
    }
  }
  return {};
}

std::string BackupPath(const EngineApiRequest& request) {
  for (const auto& key : {"target_uri:", "manifest_uri:", "backup_path:", "source_manifest_uri:"}) {
    const auto value = OptionValue(request, key);
    if (!value.empty()) {
      return value;
    }
  }
  return {};
}

std::uint64_t Fnv1a64(const std::string& value) {
  std::uint64_t hash = 1469598103934665603ull;
  for (unsigned char c : value) {
    hash ^= static_cast<std::uint64_t>(c);
    hash *= 1099511628211ull;
  }
  return hash;
}

std::vector<std::string> Split(const std::string& value, char delimiter) {
  std::vector<std::string> out;
  std::string current;
  std::istringstream input(value);
  while (std::getline(input, current, delimiter)) {
    out.push_back(current);
  }
  return out;
}

std::string EncodeList(const std::vector<std::string>& values) {
  std::vector<std::pair<std::string, std::string>> pairs;
  for (std::size_t i = 0; i < values.size(); ++i) {
    pairs.push_back({std::to_string(i), values[i]});
  }
  return EncodeCrudPairs(pairs);
}

std::vector<std::string> DecodeList(const std::string& encoded) {
  std::vector<std::string> out;
  for (const auto& pair : DecodeCrudPairs(encoded)) {
    out.push_back(pair.second);
  }
  return out;
}

std::string RecordLine(const std::string& kind, const std::vector<std::pair<std::string, std::string>>& fields) {
  return kind + "\t" + EncodeCrudPairs(fields) + "\n";
}

std::map<std::string, std::string> DecodeRecordFields(const std::string& line, std::string* kind) {
  const auto pos = line.find('\t');
  if (pos == std::string::npos) {
    if (kind != nullptr) { *kind = line; }
    return {};
  }
  if (kind != nullptr) { *kind = line.substr(0, pos); }
  std::map<std::string, std::string> fields;
  for (const auto& pair : DecodeCrudPairs(line.substr(pos + 1))) {
    fields[pair.first] = pair.second;
  }
  return fields;
}

std::uint64_t ParseU64(const std::string& value) {
  try {
    return static_cast<std::uint64_t>(std::stoull(value));
  } catch (...) {
    return 0;
  }
}

std::uint64_t ArchiveMaxAgeMicroseconds(const EngineApiRequest& request) {
  const auto value = OptionValue(request, "archive_max_age_microseconds:");
  const auto parsed = ParseU64(value);
  return parsed == 0 ? kDefaultArchiveMaxAgeMicroseconds : parsed;
}

void PublishArchiveSliceCreatedMetrics(const EngineApiRequest& request,
                                       const std::string& archive_class,
                                       const std::string& reason_class,
                                       std::uint64_t slice_bytes) {
  (void)scratchbird::core::metrics::PublishArchiveSliceCount(1.0, archive_class, reason_class);
  (void)scratchbird::core::metrics::PublishArchiveSliceBytes(static_cast<double>(slice_bytes), archive_class, reason_class);
  (void)scratchbird::core::metrics::PublishArchiveSliceAge(0.0, archive_class, reason_class);
  (void)scratchbird::core::metrics::PublishArchiveSliceMaxAge(static_cast<double>(ArchiveMaxAgeMicroseconds(request)), archive_class);
  (void)scratchbird::core::metrics::PublishArchiveHealthState(1.0, "healthy", archive_class);
  (void)scratchbird::core::metrics::PublishArchiveQueueDepth(0.0, archive_class, "idle");
}

void RecordArchiveRestoreRefusalMetric(const std::string& archive_class, const EngineApiDiagnostic& diagnostic) {
  (void)scratchbird::core::metrics::RecordArchiveRestoreRefusal(
      archive_class,
      diagnostic.detail.empty() ? diagnostic.code : diagnostic.detail);
  if (diagnostic.detail.find("CHECKSUM") != std::string::npos ||
      diagnostic.code.find("CHECKSUM") != std::string::npos) {
    (void)scratchbird::core::metrics::RecordArchiveChecksumFailure(archive_class, "checksum");
  }
}

std::uint64_t MaxCommittedTransaction(const CrudState& state) {
  std::uint64_t max_committed = 0;
  for (const auto& [tx, tx_state] : state.transactions) {
    if (tx_state == "committed" || tx_state == "archived") {
      max_committed = std::max(max_committed, tx);
    }
  }
  return max_committed;
}

EngineApiDiagnostic BackupInvalid(const std::string& operation_id, const std::string& detail) {
  return MakeInvalidRequestDiagnostic(operation_id, detail);
}

std::string RequiredManifestOption(const EngineApiRequest& request, const std::string& prefix) {
  return OptionValue(request, prefix);
}

std::string TimelineUuidFor(const EngineApiRequest& request) {
  const auto timeline = OptionValue(request, "timeline_uuid:");
  if (!timeline.empty()) { return timeline; }
  return request.context.database_uuid.canonical + ":timeline:local";
}

std::string ForkUuidFor(const EngineApiRequest& request) {
  const auto fork = OptionValue(request, "fork_uuid:");
  if (!fork.empty()) { return fork; }
  return request.context.database_uuid.canonical + ":fork:primary";
}

std::string KeyLineageFor(const EngineApiRequest& request) {
  const auto lineage = OptionValue(request, "key_lineage_id:");
  if (!lineage.empty()) { return lineage; }
  return request.context.database_uuid.canonical + ":key-lineage:local";
}

std::uint64_t CoverageStartFor(const EngineApiRequest& request, std::uint64_t fallback) {
  const auto parsed = ParseU64(OptionValue(request, "coverage_start_transaction_id:"));
  return parsed == 0 ? fallback : parsed;
}

std::uint64_t CoverageEndFor(const EngineApiRequest& request, std::uint64_t fallback) {
  const auto parsed = ParseU64(OptionValue(request, "coverage_end_transaction_id:"));
  return parsed == 0 ? fallback : parsed;
}

std::string ManifestField(const std::map<std::string, std::string>& fields, const std::string& key) {
  const auto found = fields.find(key);
  return found == fields.end() ? std::string{} : found->second;
}

EngineApiDiagnostic ValidateManifestProofFields(const std::string& operation_id,
                                                const std::map<std::string, std::string>& fields,
                                                bool require_source_backup_uuid) {
  const std::vector<std::string> required = {
      "manifest_version",
      "database_uuid",
      "filespace_uuid",
      "timeline_uuid",
      "fork_uuid",
      "key_lineage_id",
      "coverage_start_transaction_id",
      "coverage_end_transaction_id",
      "coverage_contiguous",
      "coverage_proof",
      "checksum_profile",
      "signature_profile",
      "finality_source",
  };
  for (const auto& key : required) {
    if (ManifestField(fields, key).empty()) {
      return BackupInvalid(operation_id, "RESTORE_MANIFEST_COVERAGE_FIELD_MISSING:" + key);
    }
  }
  if (require_source_backup_uuid && ManifestField(fields, "source_backup_uuid").empty()) {
    return BackupInvalid(operation_id, "BACKUP_DELTA_COVERAGE_FIELD_MISSING:source_backup_uuid");
  }
  if (ManifestField(fields, "coverage_contiguous") != "true") {
    return BackupInvalid(operation_id, "BACKUP_DELTA_COVERAGE_GAP:non_contiguous");
  }
  if (ManifestField(fields, "finality_source") != "local_mga_transaction_inventory") {
    return BackupInvalid(operation_id, "RESTORE_MANIFEST_FINALITY_SOURCE_INVALID");
  }
  const auto start = ParseU64(ManifestField(fields, "coverage_start_transaction_id"));
  const auto end = ParseU64(ManifestField(fields, "coverage_end_transaction_id"));
  if (end < start) {
    return BackupInvalid(operation_id, "RESTORE_MANIFEST_COVERAGE_RANGE_INVALID");
  }
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

std::string TypedUuidText(const scratchbird::core::platform::TypedUuid& uuid) {
  return scratchbird::core::uuid::UuidToString(uuid.value);
}

bool IsLocalArchiveHistoryFilespace(
    const filespace::FilespaceDescriptor& descriptor,
    const EngineRequestContext& context) {
  if (descriptor.database_uuid.kind != scratchbird::core::platform::UuidKind::database ||
      descriptor.filespace_uuid.kind != scratchbird::core::platform::UuidKind::filespace ||
      !descriptor.database_uuid.valid() || !descriptor.filespace_uuid.valid()) {
    return false;
  }
  if (TypedUuidText(descriptor.database_uuid) != context.database_uuid.canonical) {
    return false;
  }
  if (descriptor.path.empty() || !descriptor.archive_owner || !descriptor.read_only ||
      !descriptor.active) {
    return false;
  }
  if (descriptor.role != filespace::FilespaceRole::archive_history) {
    return false;
  }
  return descriptor.state == filespace::FilespaceState::attached ||
         descriptor.state == filespace::FilespaceState::read_only ||
         descriptor.state == filespace::FilespaceState::archived;
}

std::string ArchiveMovementMaterial(
    const EngineArchiveRetainedHistoryRecord& record,
    std::uint64_t cleanup_horizon) {
  const auto& metadata = record.metadata;
  return record.table_uuid + "|" +
         TypedUuidText(metadata.identity.row.row_uuid) + "|" +
         TypedUuidText(metadata.identity.creator_transaction.transaction_uuid) +
         "|" +
         std::to_string(metadata.identity.creator_transaction.local_id.value) +
         "|" + std::to_string(metadata.identity.version_sequence) + "|" +
         mga::RowVersionStateName(metadata.state) + "|" +
         mga::TransactionStateName(metadata.creator_transaction_state) + "|" +
         std::to_string(metadata.successor_transaction_local_id.value) + "|" +
         std::to_string(metadata.chain.previous_version_sequence) + "|" +
         std::to_string(metadata.chain.next_version_sequence) + "|" +
         record.payload_digest + "|" + record.retention_class + "|" +
         record.retention_policy_ref + "|" + record.key_lineage_id + "|" +
         std::to_string(cleanup_horizon);
}

EngineApiDiagnostic ValidateArchiveRetainedHistoryRecord(
    const std::string& operation_id,
    const EngineArchiveRetainedHistoryRecord& record,
    std::uint64_t cleanup_horizon) {
  if (record.table_uuid.empty()) {
    return BackupInvalid(operation_id, "ARCHIVE_ROW_TABLE_UUID_REQUIRED");
  }
  if (record.payload_digest.empty()) {
    return BackupInvalid(operation_id, "ARCHIVE_ROW_PAYLOAD_DIGEST_REQUIRED");
  }
  if (record.retention_class.empty() || record.retention_policy_ref.empty()) {
    return BackupInvalid(operation_id, "ARCHIVE_RETENTION_POLICY_REQUIRED");
  }
  if (record.key_lineage_id.empty()) {
    return BackupInvalid(operation_id, "ARCHIVE_KEY_LINEAGE_REQUIRED");
  }
  const auto metadata_status = mga::ValidateRowVersionMetadata(record.metadata);
  if (!metadata_status.ok()) {
    return BackupInvalid(operation_id,
                         "ARCHIVE_ROW_VERSION_METADATA_INVALID:" +
                             metadata_status.diagnostic.diagnostic_code);
  }
  if (record.metadata.state != mga::RowVersionState::committed ||
      record.metadata.creator_transaction_state != mga::TransactionState::committed ||
      !record.metadata.chain.has_next() || !record.metadata.payload_present ||
      !record.metadata.successor_transaction_local_id.valid()) {
    return BackupInvalid(operation_id,
                         "ARCHIVE_RETAINED_HISTORY_SHAPE_REQUIRED");
  }
  if (record.metadata.identity.creator_transaction.local_id.value >=
      cleanup_horizon) {
    return BackupInvalid(operation_id,
                         "ARCHIVE_ROW_NOT_BELOW_CLEANUP_HORIZON");
  }
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

std::string BuildArchiveBeforeReclaimManifestBody(
    const EngineArchiveRetainedHistoryBeforeReclaimRequest& request,
    const EngineUuid& archive_uuid,
    std::uint64_t* movement_record_count) {
  std::ostringstream body;
  const auto archive_filespace_uuid =
      TypedUuidText(request.archive_filespace.filespace_uuid);
  const auto cleanup_horizon =
      request.authoritative_cleanup_horizon_local_transaction_id;
  const auto& first = request.retained_history.front();
  body << kArchiveBeforeReclaimMagic << "\n";
  body << RecordLine(
      "META",
      {{"manifest_version", "1"},
       {"archive_uuid", archive_uuid.canonical},
       {"database_uuid", request.context.database_uuid.canonical},
       {"archive_filespace_uuid", archive_filespace_uuid},
       {"archive_filespace_role",
        filespace::FilespaceRoleName(request.archive_filespace.role)},
       {"archive_filespace_access_class", "local_archive_history_read_only"},
       {"archive_filespace_header_verified",
        request.local_archive_filespace_header_verified ? "true" : "false"},
       {"authoritative_cleanup_horizon_local_transaction_id",
        std::to_string(cleanup_horizon)},
       {"cleanup_horizon_authoritative",
        request.cleanup_horizon_authoritative ? "true" : "false"},
       {"finality_source", "local_mga_transaction_inventory"},
       {"archive_before_reclaim", "true"},
       {"manifest_reachability_required",
        request.manifest_reachability_required ? "true" : "false"},
       {"manifest_reachability_verified", "pending"},
       {"checksum_profile", "fnv1a64-manifest-body"},
       {"signature_profile", "unsigned-local-archive-movement-proof-v1"},
       {"key_lineage_id", first.key_lineage_id},
       {"retention_class", first.retention_class},
       {"retention_policy_ref", first.retention_policy_ref},
       {"legal_hold_active", request.legal_hold_active ? "true" : "false"},
       {"hot_reclaim_authority", "archive_manifest_precondition_only"},
       {"transaction_finality_authority", "false"},
       {"authoritative_wal", "false"}});

  std::uint64_t count = 0;
  for (const auto& record : request.retained_history) {
    const auto& metadata = record.metadata;
    const auto movement_checksum =
        Fnv1a64(ArchiveMovementMaterial(record, cleanup_horizon));
    body << RecordLine(
        "MOVE",
        {{"table_uuid", record.table_uuid},
         {"row_uuid", TypedUuidText(metadata.identity.row.row_uuid)},
         {"creator_transaction_uuid",
          TypedUuidText(metadata.identity.creator_transaction.transaction_uuid)},
         {"creator_local_transaction_id",
          std::to_string(metadata.identity.creator_transaction.local_id.value)},
         {"version_sequence",
          std::to_string(metadata.identity.version_sequence)},
         {"row_version_state", mga::RowVersionStateName(metadata.state)},
         {"creator_transaction_state",
          mga::TransactionStateName(metadata.creator_transaction_state)},
         {"successor_local_transaction_id",
          std::to_string(metadata.successor_transaction_local_id.value)},
         {"previous_version_sequence",
          std::to_string(metadata.chain.previous_version_sequence)},
         {"next_version_sequence",
          std::to_string(metadata.chain.next_version_sequence)},
         {"previous_version_uuid",
          TypedUuidText(metadata.chain.previous_version_uuid)},
         {"next_version_uuid", TypedUuidText(metadata.chain.next_version_uuid)},
         {"payload_digest", record.payload_digest},
         {"key_lineage_id", record.key_lineage_id},
         {"retention_class", record.retention_class},
         {"retention_policy_ref", record.retention_policy_ref},
         {"movement_record_checksum", std::to_string(movement_checksum)}});
    ++count;
  }
  if (movement_record_count != nullptr) {
    *movement_record_count = count;
  }
  return body.str();
}

std::string ReadBinaryFile(const std::string& path, bool* ok);

EngineApiDiagnostic ReadAndVerifyArchiveBeforeReclaimManifest(
    const std::string& manifest_path,
    const EngineUuid& archive_uuid,
    std::uint64_t expected_movement_count,
    std::uint64_t expected_checksum,
    std::uint64_t* manifest_bytes) {
  bool read_ok = false;
  const auto manifest = ReadBinaryFile(manifest_path, &read_ok);
  if (!read_ok) {
    return BackupInvalid("backup_archive.archive_retained_history_before_reclaim",
                         "ARCHIVE_MANIFEST_UNREACHABLE");
  }
  if (manifest_bytes != nullptr) {
    *manifest_bytes = static_cast<std::uint64_t>(manifest.size());
  }
  const auto checksum_pos = manifest.rfind("CHECKSUM\t");
  if (checksum_pos == std::string::npos) {
    return BackupInvalid("backup_archive.archive_retained_history_before_reclaim",
                         "ARCHIVE_MANIFEST_CHECKSUM_MISSING");
  }
  const auto body = manifest.substr(0, checksum_pos);
  const auto checksum_line = manifest.substr(checksum_pos);
  const auto checksum_parts = Split(checksum_line, '\t');
  if (checksum_parts.size() < 2 ||
      ParseU64(checksum_parts[1]) != expected_checksum ||
      ParseU64(checksum_parts[1]) != Fnv1a64(body)) {
    return BackupInvalid("backup_archive.archive_retained_history_before_reclaim",
                         "ARCHIVE_MANIFEST_CHECKSUM_INVALID");
  }
  if (!StartsWith(body, std::string(kArchiveBeforeReclaimMagic) + "\n")) {
    return BackupInvalid("backup_archive.archive_retained_history_before_reclaim",
                         "ARCHIVE_MANIFEST_MAGIC_INVALID");
  }
  bool meta_found = false;
  std::uint64_t movement_count = 0;
  for (const auto& line : Split(body, '\n')) {
    if (line.empty() || line == kArchiveBeforeReclaimMagic) {
      continue;
    }
    std::string kind;
    const auto fields = DecodeRecordFields(line, &kind);
    if (kind == "META") {
      meta_found = true;
      if (ManifestField(fields, "archive_uuid") != archive_uuid.canonical ||
          ManifestField(fields, "archive_before_reclaim") != "true" ||
          ManifestField(fields, "finality_source") !=
              "local_mga_transaction_inventory" ||
          ManifestField(fields, "transaction_finality_authority") != "false" ||
          ManifestField(fields, "authoritative_wal") != "false" ||  // no_wal manifest proof
          ManifestField(fields, "archive_filespace_access_class") !=
              "local_archive_history_read_only" ||
          ManifestField(fields, "key_lineage_id").empty() ||
          ManifestField(fields, "retention_policy_ref").empty()) {
        return BackupInvalid(
            "backup_archive.archive_retained_history_before_reclaim",
            "ARCHIVE_MANIFEST_META_INVALID");
      }
    } else if (kind == "MOVE") {
      if (ManifestField(fields, "row_uuid").empty() ||
          ManifestField(fields, "creator_transaction_uuid").empty() ||
          ManifestField(fields, "version_sequence").empty() ||
          ManifestField(fields, "payload_digest").empty() ||
          ManifestField(fields, "movement_record_checksum").empty()) {
        return BackupInvalid(
            "backup_archive.archive_retained_history_before_reclaim",
            "ARCHIVE_MOVEMENT_RECORD_INVALID");
      }
      ++movement_count;
    }
  }
  if (!meta_found) {
    return BackupInvalid("backup_archive.archive_retained_history_before_reclaim",
                         "ARCHIVE_MANIFEST_META_MISSING");
  }
  if (movement_count != expected_movement_count) {
    return BackupInvalid("backup_archive.archive_retained_history_before_reclaim",
                         "ARCHIVE_MOVEMENT_COUNT_MISMATCH");
  }
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

mga::LocalCleanupReclaimEvidenceRecord ArchiveReclaimEvidenceRecord(
    const EngineArchiveRetainedHistoryRecord& record,
    std::uint64_t cleanup_horizon,
    const EngineUuid& archive_uuid) {
  mga::LocalCleanupReclaimEvidenceRecord evidence;
  evidence.row_version_identity = record.metadata.identity;
  evidence.row_version_state = record.metadata.state;
  evidence.creator_transaction =
      record.metadata.identity.creator_transaction.local_id;
  evidence.successor_transaction = record.metadata.successor_transaction_local_id;
  evidence.authoritative_cleanup_horizon_local_transaction_id = cleanup_horizon;
  evidence.stable_evidence_id =
      "mga-archive-before-reclaim:" + archive_uuid.canonical + ":" +
      std::to_string(
          record.metadata.identity.creator_transaction.local_id.value) +
      ":" + std::to_string(record.metadata.identity.version_sequence) + ":" +
      std::to_string(Fnv1a64(
          ArchiveMovementMaterial(record, cleanup_horizon)));
  return evidence;
}

std::uint64_t CurrentUnixMicros() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

std::string BackupLifecycleLedgerPath(const EngineRequestContext& context) {
  return context.database_path + ".sb.backup_archive_lifecycle";
}

bool AppendBackupLifecycleLedger(const EngineApiRequest& request,
                                 BackupArchiveLifecycleOperation operation,
                                 const std::string& evidence_kind,
                                 const std::string& evidence_detail,
                                 std::string* evidence_id) {
  if (request.context.database_path.empty()) { return false; }
  const auto ledger_path = BackupLifecycleLedgerPath(request.context);
  const auto parent = std::filesystem::path(ledger_path).parent_path();
  if (!parent.empty()) { std::filesystem::create_directories(parent); }
  const std::string body =
      "SBBARE1\t" + std::to_string(CurrentUnixMicros()) + "\t" +
      BackupArchiveLifecycleOperationName(operation) + "\t" +
      request.context.database_uuid.canonical + "\t" +
      std::to_string(request.context.local_transaction_id) + "\t" +
      evidence_kind + "\t" + evidence_detail + "\tengine_owned";
  const auto checksum = Fnv1a64(body);
  std::ofstream out(ledger_path, std::ios::binary | std::ios::app);
  if (!out) { return false; }
  out << body << "\t" << checksum << "\n";
  out.close();
  if (!out) { return false; }
  if (evidence_id != nullptr) {
    *evidence_id = ledger_path + "#" + std::to_string(checksum);
  }
  return true;
}

std::string BackupForwardSessionLedgerPath(const EngineRequestContext& context) {
  return context.database_path + ".sb.backup_forward_sessions";
}

bool AppendBackupForwardSessionLedger(
    const EngineApiRequest& request,
    const std::string& event_kind,
    const std::vector<std::pair<std::string, std::string>>& fields,
    std::string* evidence_id) {
  if (request.context.database_path.empty()) {
    return false;
  }
  const auto ledger_path = BackupForwardSessionLedgerPath(request.context);
  const auto parent = std::filesystem::path(ledger_path).parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent);
  }
  std::vector<std::pair<std::string, std::string>> ledger_fields = fields;
  ledger_fields.push_back({"event_kind", event_kind});
  ledger_fields.push_back({"database_uuid", request.context.database_uuid.canonical});
  ledger_fields.push_back({"local_transaction_id",
                           std::to_string(request.context.local_transaction_id)});
  ledger_fields.push_back({"event_unix_micros",
                           std::to_string(CurrentUnixMicros())});
  ledger_fields.push_back({"write_after_recovery_authority", "false"});
  ledger_fields.push_back({"transaction_finality_authority", "false"});
  ledger_fields.push_back({"finality_source", "local_mga_transaction_inventory"});
  const std::string body =
      "SBBFWD1\t" + EncodeCrudPairs(ledger_fields);
  const auto checksum = Fnv1a64(body);
  std::ofstream out(ledger_path, std::ios::binary | std::ios::app);
  if (!out) {
    return false;
  }
  out << body << "\t" << checksum << "\n";
  out.close();
  if (!out) {
    return false;
  }
  if (evidence_id != nullptr) {
    *evidence_id = ledger_path + "#" + std::to_string(checksum);
  }
  return true;
}

struct MultiHorizonProofInput {
  const char* kind = "";
  bool authoritative = false;
  std::uint64_t horizon_transaction_id = 0;
  bool hold_active = false;
};

const char* MultiHorizonProofDisplayName(const char* kind) {
  return kind == nullptr || kind[0] == '\0' ? "unknown_horizon" : kind;
}

bool OptionBool(const EngineApiRequest& request, const std::string& prefix, bool fallback = false) {
  const auto value = OptionValue(request, prefix);
  if (value.empty()) { return fallback; }
  return value == "1" || value == "true" || value == "yes" || value == "on";
}

const char* OperationDiagnosticName(BackupArchiveLifecycleOperation operation) {
  switch (operation) {
    case BackupArchiveLifecycleOperation::logical_backup:
      return "backup_archive.start_logical_backup";
    case BackupArchiveLifecycleOperation::physical_backup:
      return "backup_archive.start_physical_backup";
    case BackupArchiveLifecycleOperation::logical_restore:
      return "backup_archive.restore_logical_backup";
    case BackupArchiveLifecycleOperation::physical_restore:
      return "backup_archive.restore_physical_backup";
    case BackupArchiveLifecycleOperation::delta_package:
      return "backup_archive.package_delta_stream";
    case BackupArchiveLifecycleOperation::delta_apply:
      return "backup_archive.apply_delta_stream";
    case BackupArchiveLifecycleOperation::shadow_snapshot:
      return "backup_archive.shadow_snapshot";
  }
  return "backup_archive.lifecycle";
}

bool RestoreOperation(BackupArchiveLifecycleOperation operation) {
  return operation == BackupArchiveLifecycleOperation::logical_restore ||
         operation == BackupArchiveLifecycleOperation::physical_restore ||
         operation == BackupArchiveLifecycleOperation::delta_apply;
}

BackupArchiveLifecycleAdmission LifecycleRefusal(const EngineApiRequest& request,
                                                 BackupArchiveLifecycleOperation operation,
                                                 std::string detail) {
  (void)request;
  BackupArchiveLifecycleAdmission admission;
  admission.operation = operation;
  admission.admitted = false;
  admission.diagnostic = BackupInvalid(OperationDiagnosticName(operation), std::move(detail));
  return admission;
}

void AddBackupLifecycleEvidence(EngineApiResult* result,
                                const BackupArchiveLifecycleAdmission& admission) {
  for (const auto& evidence : admission.evidence) {
    result->evidence.push_back(evidence);
  }
  AddApiBehaviorRow(result,
                    {{"backup_archive_lifecycle_operation",
                      BackupArchiveLifecycleOperationName(admission.operation)},
                     {"snapshot_hold_acquired", admission.snapshot_hold_acquired ? "true" : "false"},
                     {"filespace_hold_acquired", admission.filespace_hold_acquired ? "true" : "false"},
                     {"shutdown_blocker_registered",
                      admission.shutdown_blocker_registered ? "true" : "false"},
                     {"drop_blocker_registered", admission.drop_blocker_registered ? "true" : "false"},
                     {"legal_retention_satisfied",
                      admission.legal_retention_satisfied ? "true" : "false"},
                     {"restore_inspection_open",
                      admission.restore_inspection_open ? "true" : "false"},
                     {"live_file_shortcut_refused",
                      admission.live_file_shortcut_refused ? "true" : "false"},
                     {"lifecycle_policy_present",
                      admission.lifecycle_policy_present ? "true" : "false"},
                     {"lifecycle_policy_ref", admission.lifecycle_policy_ref},
                     {"engine_owned_path", admission.engine_owned_path ? "true" : "false"}});
}

bool HasBackupCreateRight(const EngineRequestContext& context) {
  return SecurityContextHasRight(context, "BACKUP_CREATE") ||
         SecurityContextHasRight(context, "BACKUP_CONTROL") ||
         SecurityContextHasRight(context, "SYS_BACKUP") ||
         SecurityContextHasTag(context, "security.bootstrap");
}

bool HasBackupRestoreRight(const EngineRequestContext& context) {
  return SecurityContextHasRight(context, "BACKUP_RESTORE") ||
         SecurityContextHasRight(context, "BACKUP_CONTROL") ||
         SecurityContextHasRight(context, "SYS_BACKUP") ||
         SecurityContextHasTag(context, "security.bootstrap");
}

struct LogicalBackupRecordSet {
  EngineUuid backup_uuid;
  EngineUuid snapshot_uuid;
  std::uint64_t snapshot_tx = 0;
  std::map<std::uint64_t, std::string> transaction_states;
  std::vector<CrudTableRecord> tables;
  std::vector<CrudIndexRecord> indexes;
  std::vector<CrudRowVersionRecord> rows;
};

std::string TransactionStateFor(const LogicalBackupRecordSet& records, std::uint64_t transaction_id) {
  const auto it = records.transaction_states.find(transaction_id);
  return it == records.transaction_states.end() ? std::string("unknown") : it->second;
}

bool IsUnsafeFinalityState(const std::string& state) {
  return state == "active" || state == "preparing" || state == "prepared" ||
         state == "committing" || state == "limbo" || state == "recovering";
}

struct TemporaryBackupExclusionStats {
  std::uint64_t table_count = 0;
  std::uint64_t row_count = 0;
  std::uint64_t index_count = 0;
};

std::vector<std::string> TemporaryTableUuidsVisibleThrough(
    const CrudState& state,
    std::uint64_t visible_through_tx) {
  std::vector<std::string> table_uuids;
  for (const auto& table : state.tables) {
    if (table.temporary &&
        CrudCreatorVisible(state,
                           table.creator_tx,
                           table.event_sequence,
                           visible_through_tx)) {
      table_uuids.push_back(table.table_uuid);
    }
  }
  return table_uuids;
}

bool ContainsUuid(const std::vector<std::string>& uuids,
                  const std::string& uuid) {
  return std::find(uuids.begin(), uuids.end(), uuid) != uuids.end();
}

TemporaryBackupExclusionStats CountTemporarySnapshotExclusions(
    const CrudState& state,
    std::uint64_t snapshot_tx) {
  TemporaryBackupExclusionStats stats;
  const auto temporary_table_uuids =
      TemporaryTableUuidsVisibleThrough(state, snapshot_tx);
  for (const auto& table : state.tables) {
    if (ContainsUuid(temporary_table_uuids, table.table_uuid)) {
      ++stats.table_count;
    }
  }
  for (const auto& index : state.indexes) {
    if (ContainsUuid(temporary_table_uuids, index.table_uuid) &&
        CrudCreatorVisible(state,
                           index.creator_tx,
                           index.event_sequence,
                           snapshot_tx)) {
      ++stats.index_count;
    }
  }
  for (const auto& row : state.row_versions) {
    if (ContainsUuid(temporary_table_uuids, row.table_uuid) &&
        CrudCreatorVisible(state,
                           row.creator_tx,
                           row.event_sequence,
                           snapshot_tx)) {
      ++stats.row_count;
    }
  }
  return stats;
}

TemporaryBackupExclusionStats CountTemporaryDeltaExclusions(
    const CrudState& state,
    std::uint64_t start_tx,
    std::uint64_t end_tx) {
  TemporaryBackupExclusionStats stats;
  const auto temporary_table_uuids =
      TemporaryTableUuidsVisibleThrough(state, end_tx);
  for (const auto& table : state.tables) {
    if (ContainsUuid(temporary_table_uuids, table.table_uuid) &&
        table.creator_tx >= start_tx && table.creator_tx <= end_tx &&
        CrudCreatorVisible(state,
                           table.creator_tx,
                           table.event_sequence,
                           end_tx)) {
      ++stats.table_count;
    }
  }
  for (const auto& index : state.indexes) {
    if (ContainsUuid(temporary_table_uuids, index.table_uuid) &&
        index.creator_tx >= start_tx && index.creator_tx <= end_tx &&
        CrudCreatorVisible(state,
                           index.creator_tx,
                           index.event_sequence,
                           end_tx)) {
      ++stats.index_count;
    }
  }
  for (const auto& row : state.row_versions) {
    if (ContainsUuid(temporary_table_uuids, row.table_uuid) &&
        row.creator_tx >= start_tx && row.creator_tx <= end_tx &&
        CrudCreatorVisible(state,
                           row.creator_tx,
                           row.event_sequence,
                           end_tx)) {
      ++stats.row_count;
    }
  }
  return stats;
}

void AddTemporaryBackupExclusionEvidence(
    EngineApiResult* result,
    const TemporaryBackupExclusionStats& stats) {
  AddApiBehaviorEvidence(result, "temporary_content_excluded", "true");
  AddApiBehaviorEvidence(result,
                         "temporary_tables_excluded",
                         std::to_string(stats.table_count));
  AddApiBehaviorEvidence(result,
                         "temporary_rows_excluded",
                         std::to_string(stats.row_count));
  AddApiBehaviorEvidence(result,
                         "temporary_indexes_excluded",
                         std::to_string(stats.index_count));
}

std::string UnsafeFinalityGap(const CrudState& state, std::uint64_t snapshot_tx) {
  for (const auto& [transaction_id, transaction_state] : state.transactions) {
    if (transaction_id <= snapshot_tx && IsUnsafeFinalityState(transaction_state)) {
      return std::to_string(transaction_id) + ":" + transaction_state;
    }
  }
  return {};
}

std::string BuildManifestBody(const EngineStartLogicalBackupRequest& request, const LogicalBackupRecordSet& records) {
  std::ostringstream body;
  body << kLogicalBackupMagic << "\n";
  const auto filespace_uuid = RequiredManifestOption(request, "filespace_uuid:");
  const auto coverage_start = CoverageStartFor(request, records.snapshot_tx == 0 ? 0 : 1);
  const auto coverage_end = CoverageEndFor(request, records.snapshot_tx);
  body << RecordLine("META", {{"backup_uuid", records.backup_uuid.canonical},
                               {"manifest_version", "1"},
                               {"snapshot_uuid", records.snapshot_uuid.canonical},
                               {"database_uuid", request.context.database_uuid.canonical},
                               {"filespace_uuid", filespace_uuid},
                               {"timeline_uuid", TimelineUuidFor(request)},
                               {"fork_uuid", ForkUuidFor(request)},
                               {"key_lineage_id", KeyLineageFor(request)},
                               {"coverage_start_transaction_id", std::to_string(coverage_start)},
                               {"coverage_end_transaction_id", std::to_string(coverage_end)},
                               {"coverage_contiguous", "true"},
                               {"coverage_proof", std::to_string(Fnv1a64(request.context.database_uuid.canonical + filespace_uuid + std::to_string(coverage_start) + ":" + std::to_string(coverage_end)))},
                               {"checksum_profile", "fnv1a64-manifest-body"},
                               {"signature_profile", "unsigned-local-manifest-proof-v1"},
                               {"snapshot_tx", std::to_string(records.snapshot_tx)},
                               {"finality_boundary_local_transaction_id", std::to_string(records.snapshot_tx)},
                               {"finality_source", "local_mga_transaction_inventory"},
                               {"archive_slice_kind", "mga_logical_snapshot"},
                               {"lineage_source", "mga_row_version_lineage"},
                               {"authoritative_wal", "false"},
                               {"format", "logical_snapshot_v1"}});
  for (const auto& table : records.tables) {
    body << RecordLine("TABLE", {{"creator_tx", std::to_string(table.creator_tx)},
                                  {"creator_transaction_state", TransactionStateFor(records, table.creator_tx)},
                                  {"event_sequence", std::to_string(table.event_sequence)},
                                  {"table_uuid", table.table_uuid},
                                  {"default_name", table.default_name},
                                  {"columns", EncodeCrudPairs(table.columns)}});
  }
  for (const auto& index : records.indexes) {
    body << RecordLine("INDEX", {{"creator_tx", std::to_string(index.creator_tx)},
                                  {"creator_transaction_state", TransactionStateFor(records, index.creator_tx)},
                                  {"event_sequence", std::to_string(index.event_sequence)},
                                  {"index_uuid", index.index_uuid},
                                  {"table_uuid", index.table_uuid},
                                  {"column_name", index.column_name},
                                  {"family", index.family},
                                  {"profile", index.profile},
                                  {"default_name", index.default_name},
                                  {"key_envelopes", EncodeList(index.key_envelopes)},
                                  {"include_columns", EncodeList(index.include_columns)},
                                  {"predicate_kind", index.predicate_kind},
                                  {"predicate_column", index.predicate_column},
                                  {"predicate_value", index.predicate_value},
                                  {"unique", index.unique ? "1" : "0"}});
  }
  for (const auto& row : records.rows) {
    const std::string lineage_material = row.table_uuid + "|" + row.row_uuid + "|" + row.version_uuid + "|" +
                                         row.previous_version_uuid + "|" + std::to_string(row.creator_tx) + "|" +
                                         std::to_string(row.sequence) + "|" + std::to_string(row.deleted ? 1 : 0);
    body << RecordLine("ROW", {{"creator_tx", std::to_string(row.creator_tx)},
                                {"creator_transaction_state", TransactionStateFor(records, row.creator_tx)},
                                {"event_sequence", std::to_string(row.event_sequence)},
                                {"sequence", std::to_string(row.sequence)},
                                {"table_uuid", row.table_uuid},
                                {"row_uuid", row.row_uuid},
                                {"version_uuid", row.version_uuid},
                                {"previous_version_uuid", row.previous_version_uuid},
                                {"previous_sequence", std::to_string(row.previous_sequence)},
                                {"lineage_checksum", std::to_string(Fnv1a64(lineage_material))},
                                {"deleted", row.deleted ? "1" : "0"},
                                {"values", EncodeCrudPairs(row.values)}});
  }
  return body.str();
}

EngineApiDiagnostic ReadAndVerifyManifest(const std::string& path, std::string* body) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return BackupInvalid("backup_archive.restore_logical_backup", "RESTORE_MANIFEST_UNAVAILABLE");
  }
  std::ostringstream data;
  data << in.rdbuf();
  const std::string manifest = data.str();
  const auto checksum_pos = manifest.rfind("CHECKSUM\t");
  if (checksum_pos == std::string::npos) {
    return BackupInvalid("backup_archive.restore_logical_backup", "RESTORE_MANIFEST_CHECKSUM_MISSING");
  }
  const std::string manifest_body = manifest.substr(0, checksum_pos);
  const std::string checksum_line = manifest.substr(checksum_pos);
  const auto fields = Split(checksum_line, '\t');
  if (fields.size() < 2 || ParseU64(fields[1]) != Fnv1a64(manifest_body)) {
    return BackupInvalid("backup_archive.restore_logical_backup", "RESTORE_MANIFEST_CHECKSUM_INVALID");
  }
  if (!StartsWith(manifest_body, std::string(kLogicalBackupMagic) + "\n")) {
    return BackupInvalid("backup_archive.restore_logical_backup", "RESTORE_MANIFEST_MAGIC_INVALID");
  }
  bool meta_found = false;
  for (const auto& line : Split(manifest_body, '\n')) {
    if (line.empty() || line == kLogicalBackupMagic) { continue; }
    std::string kind;
    const auto fields = DecodeRecordFields(line, &kind);
    if (kind != "META") { continue; }
    meta_found = true;
    const auto proof = ValidateManifestProofFields("backup_archive.restore_logical_backup",
                                                   fields,
                                                   false);
    if (proof.error) { return proof; }
  }
  if (!meta_found) {
    return BackupInvalid("backup_archive.restore_logical_backup", "RESTORE_MANIFEST_META_MISSING");
  }
  if (body != nullptr) { *body = manifest_body; }
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}


std::string PhysicalImagePath(const EngineApiRequest& request, const std::string& manifest_path) {
  const auto image = OptionValue(request, "image_uri:");
  if (!image.empty()) { return image; }
  return manifest_path + ".image";
}

std::string ReadBinaryFile(const std::string& path, bool* ok) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    if (ok != nullptr) { *ok = false; }
    return {};
  }
  std::ostringstream data;
  data << in.rdbuf();
  if (ok != nullptr) { *ok = true; }
  return data.str();
}

bool WriteBinaryFile(const std::string& path, const std::string& data) {
  const auto parent = std::filesystem::path(path).parent_path();
  if (!parent.empty()) { std::filesystem::create_directories(parent); }
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) { return false; }
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
  out.close();
  return static_cast<bool>(out);
}

std::string BuildPhysicalManifest(const EngineStartPhysicalBackupRequest& request,
                                  const EngineUuid& backup_uuid,
                                  const std::string& image_uri,
                                  std::uint64_t image_bytes,
                                  std::uint64_t image_checksum) {
  std::ostringstream body;
  body << kPhysicalBackupMagic << "\n";
  const auto filespace_uuid = RequiredManifestOption(request, "filespace_uuid:");
  const auto coverage_start = CoverageStartFor(request, 0);
  const auto coverage_end = CoverageEndFor(request, request.context.local_transaction_id);
  body << RecordLine("META", {{"backup_uuid", backup_uuid.canonical},
                               {"manifest_version", "1"},
                               {"database_uuid", request.context.database_uuid.canonical},
                               {"filespace_uuid", filespace_uuid},
                               {"timeline_uuid", TimelineUuidFor(request)},
                               {"fork_uuid", ForkUuidFor(request)},
                               {"key_lineage_id", KeyLineageFor(request)},
                               {"coverage_start_transaction_id", std::to_string(coverage_start)},
                               {"coverage_end_transaction_id", std::to_string(coverage_end)},
                               {"coverage_contiguous", "true"},
                               {"coverage_proof", std::to_string(Fnv1a64(request.context.database_uuid.canonical + filespace_uuid + std::to_string(image_checksum)))},
                               {"checksum_profile", "fnv1a64-manifest-body"},
                               {"signature_profile", "unsigned-local-manifest-proof-v1"},
                               {"finality_source", "local_mga_transaction_inventory"},
                               {"image_uri", image_uri},
                               {"image_bytes", std::to_string(image_bytes)},
                               {"image_checksum", std::to_string(image_checksum)},
                               {"authoritative_wal", "false"},
                               {"format", "physical_snapshot_v1"}});
  return body.str();
}

EngineApiDiagnostic ReadAndVerifyPhysicalManifest(const std::string& manifest_path,
                                                  std::string* image_uri,
                                                  std::uint64_t* image_bytes,
                                                  std::uint64_t* image_checksum,
                                                  EngineUuid* backup_uuid) {
  bool manifest_ok = false;
  const auto manifest = ReadBinaryFile(manifest_path, &manifest_ok);
  if (!manifest_ok) {
    return BackupInvalid("backup_archive.restore_physical_backup", "RESTORE_MANIFEST_UNAVAILABLE");
  }
  const auto checksum_pos = manifest.rfind("CHECKSUM\t");
  if (checksum_pos == std::string::npos) {
    return BackupInvalid("backup_archive.restore_physical_backup", "RESTORE_MANIFEST_CHECKSUM_MISSING");
  }
  const auto body = manifest.substr(0, checksum_pos);
  const auto checksum_line = manifest.substr(checksum_pos);
  const auto checksum_parts = Split(checksum_line, '\t');
  if (checksum_parts.size() < 2 || ParseU64(checksum_parts[1]) != Fnv1a64(body)) {
    return BackupInvalid("backup_archive.restore_physical_backup", "RESTORE_MANIFEST_CHECKSUM_INVALID");
  }
  if (!StartsWith(body, std::string(kPhysicalBackupMagic) + "\n")) {
    return BackupInvalid("backup_archive.restore_physical_backup", "RESTORE_MANIFEST_MAGIC_INVALID");
  }
  for (const auto& line : Split(body, '\n')) {
    if (line.empty() || line == kPhysicalBackupMagic) { continue; }
    std::string kind;
    const auto fields = DecodeRecordFields(line, &kind);
    if (kind != "META") { continue; }
    const auto proof = ValidateManifestProofFields("backup_archive.restore_physical_backup",
                                                   fields,
                                                   false);
    if (proof.error) { return proof; }
    if (ManifestField(fields, "image_uri").empty() ||
        ManifestField(fields, "image_bytes").empty() ||
        ManifestField(fields, "image_checksum").empty() ||
        ManifestField(fields, "backup_uuid").empty()) {
      return BackupInvalid("backup_archive.restore_physical_backup", "RESTORE_MANIFEST_META_MISSING");
    }
    if (image_uri != nullptr) { *image_uri = ManifestField(fields, "image_uri"); }
    if (image_bytes != nullptr) { *image_bytes = ParseU64(ManifestField(fields, "image_bytes")); }
    if (image_checksum != nullptr) { *image_checksum = ParseU64(ManifestField(fields, "image_checksum")); }
    if (backup_uuid != nullptr) { backup_uuid->canonical = ManifestField(fields, "backup_uuid"); }
    return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  }
  return BackupInvalid("backup_archive.restore_physical_backup", "RESTORE_MANIFEST_META_MISSING");
}

}  // namespace

const char* BackupArchiveLifecycleOperationName(BackupArchiveLifecycleOperation operation) {
  switch (operation) {
    case BackupArchiveLifecycleOperation::logical_backup:
      return "logical_backup";
    case BackupArchiveLifecycleOperation::physical_backup:
      return "physical_backup";
    case BackupArchiveLifecycleOperation::logical_restore:
      return "logical_restore";
    case BackupArchiveLifecycleOperation::physical_restore:
      return "physical_restore";
    case BackupArchiveLifecycleOperation::delta_package:
      return "delta_package";
    case BackupArchiveLifecycleOperation::delta_apply:
      return "delta_apply";
    case BackupArchiveLifecycleOperation::shadow_snapshot:
      return "shadow_snapshot";
  }
  return "unknown";
}

BackupArchiveLifecycleAdmission EvaluateBackupArchiveLifecycleAdmission(
    const EngineApiRequest& request,
    BackupArchiveLifecycleOperation operation) {
  if (!request.context.security_context_present) {
    return LifecycleRefusal(request, operation, "BACKUP_SECURITY_CONTEXT_REQUIRED");
  }
  if (request.context.database_uuid.canonical.empty() || request.context.database_path.empty()) {
    return LifecycleRefusal(request, operation, "BACKUP_DATABASE_CONTEXT_REQUIRED");
  }
  if (OptionValue(request, "scope:") == "cluster" && !request.context.cluster_authority_available) {
    return LifecycleRefusal(request, operation, "BACKUP_CLUSTER_SCOPE_ABSENT");
  }
  if (OptionBool(request, "authoritative_wal:", false)) {
    return LifecycleRefusal(request, operation, "BACKUP_AUTHORITATIVE_WAL_FORBIDDEN");
  }
  if (OptionBool(request, "live_file_shortcut:", false) ||
      OptionValue(request, "source:") == "live_file" ||
      OptionBool(request, "external_file_copy:", false) ||
      OptionValue(request, "restore_mode:") == "overwrite_live") {
    return LifecycleRefusal(request, operation, "BACKUP_LIVE_FILE_SHORTCUT_FORBIDDEN");
  }

  BackupArchiveLifecycleAdmission admission;
  admission.operation = operation;
  admission.snapshot_hold_required = true;
  admission.filespace_hold_required = true;
  admission.legal_retention_satisfied =
      !OptionBool(request, "legal_hold_active:", false) &&
      !OptionBool(request, "retention_policy_blocked:", false);
  admission.engine_owned_path = !OptionBool(request, "engine_owned_path:", true) ? false : true;
  admission.live_file_shortcut_refused = true;
  const bool restore = RestoreOperation(operation);
  admission.lifecycle_policy_ref = OptionValue(request, restore ? "restore_policy_ref:"
                                                                : "backup_policy_ref:");
  if (admission.lifecycle_policy_ref.empty()) {
    admission.lifecycle_policy_ref = OptionValue(request, "lifecycle_policy_ref:");
  }
  if (admission.lifecycle_policy_ref.empty()) {
    admission.lifecycle_policy_ref = restore ? "restore.archive.default_policy.v1"
                                             : "backup.archive.default_policy.v1";
  }
  admission.lifecycle_policy_present =
      OptionBool(request, restore ? "restore_policy_installed:" : "backup_policy_installed:", true) &&
      OptionBool(request, "lifecycle_policy_installed:", true) &&
      admission.lifecycle_policy_ref != "none" &&
      admission.lifecycle_policy_ref != "disabled";

  if (!admission.engine_owned_path) {
    return LifecycleRefusal(request, operation, "BACKUP_ENGINE_OWNED_PATH_REQUIRED");
  }
  if (!admission.lifecycle_policy_present) {
    return LifecycleRefusal(request, operation, restore ? "RESTORE_POLICY_REQUIRED"
                                                       : "BACKUP_POLICY_REQUIRED");
  }
  if (!admission.legal_retention_satisfied) {
    return LifecycleRefusal(request, operation, "BACKUP_LEGAL_RETENTION_REQUIRED");
  }

  const std::vector<std::pair<std::string, std::string>> required_evidence = {
      {"snapshot_hold", "acquired"},
      {"filespace_hold", "acquired"},
      {"shutdown_blocker", "registered"},
      {"drop_blocker", "registered"},
      {"engine_checkpoint_filespace_evidence", "recorded"},
      {"legal_retention", "satisfied"},
      {"live_file_shortcut", "refused"},
      {"lifecycle_policy", admission.lifecycle_policy_ref},
  };
  for (const auto& [kind, detail] : required_evidence) {
    std::string evidence_id;
    if (!AppendBackupLifecycleLedger(request, operation, kind, detail, &evidence_id)) {
      return LifecycleRefusal(request, operation, "BACKUP_LIFECYCLE_EVIDENCE_WRITE_FAILED");
    }
    admission.evidence.push_back({kind, evidence_id});
  }
  admission.snapshot_hold_acquired = true;
  admission.filespace_hold_acquired = true;
  admission.shutdown_blocker_registered = true;
  admission.drop_blocker_registered = true;

  if (operation == BackupArchiveLifecycleOperation::shadow_snapshot &&
      !OptionBool(request, "shadow_target_registered:", false)) {
    return LifecycleRefusal(request, operation, "BACKUP_SHADOW_TARGET_REQUIRED");
  }

  admission.restore_inspection_open =
      OptionBool(request, "restore_inspection_open:", false) ||
      OptionValue(request, "open_class:") == "restore_inspection";
  admission.recovery_classification_verified =
      OptionValue(request, "recovery_classification:") == "restore_inspection" ||
      OptionBool(request, "recovery_classification_verified:", false);
  if (restore) {
    if (!admission.restore_inspection_open) {
      return LifecycleRefusal(request, operation, "RESTORE_INSPECTION_OPEN_REQUIRED");
    }
    if (!admission.recovery_classification_verified) {
      return LifecycleRefusal(request, operation, "RESTORE_RECOVERY_CLASSIFICATION_REQUIRED");
    }
    if (OptionBool(request, "target_database_open:", false) ||
        OptionBool(request, "restore_target_live:", false)) {
      return LifecycleRefusal(request, operation, "RESTORE_LIVE_TARGET_FORBIDDEN");
    }
  }

  admission.admitted = true;
  admission.diagnostic = MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  admission.evidence.push_back({"backup_archive_lifecycle",
                                BackupArchiveLifecycleOperationName(operation)});
  admission.evidence.push_back({"engine_owned_path", "true"});
  if (restore) {
    admission.evidence.push_back({"restore_inspection_open", "true"});
    admission.evidence.push_back({"restore_recovery_classification", "verified"});
  }
  if (operation == BackupArchiveLifecycleOperation::shadow_snapshot) {
    admission.evidence.push_back({"shadow_target", "registered"});
  }
  return admission;
}

EngineArchiveRetainedHistoryBeforeReclaimResult
EngineArchiveRetainedHistoryBeforeReclaim(
    const EngineArchiveRetainedHistoryBeforeReclaimRequest& request) {
  constexpr const char* kOperation =
      "backup_archive.archive_retained_history_before_reclaim";
  auto fail = [&](EngineApiDiagnostic diagnostic) {
    auto result =
        MakeApiBehaviorDiagnostic<EngineArchiveRetainedHistoryBeforeReclaimResult>(
            request.context, kOperation, std::move(diagnostic));
    result.archive_filespace_uuid.canonical =
        TypedUuidText(request.archive_filespace.filespace_uuid);
    return result;
  };

  if (!HasBackupCreateRight(request.context)) {
    return fail(MakeSecurityContextRequiredDiagnostic(kOperation));
  }
  if (request.context.database_uuid.canonical.empty() ||
      request.context.database_path.empty()) {
    return fail(BackupInvalid(kOperation, "ARCHIVE_DATABASE_CONTEXT_REQUIRED"));
  }
  const bool cluster_archive_requested =
      OptionValue(request, "scope:") == "cluster" ||
      OptionValue(request, "archive_route:") == "cluster" ||
      OptionBool(request, "cluster_archive_node:", false);
  if (cluster_archive_requested) {
    auto result = fail(BackupInvalid(kOperation,
                                     "ARCHIVE_CLUSTER_PROVIDER_REQUIRED"));
    result.cluster_route_refused = true;
    result.cluster_authority_required = true;
    return result;
  }
  if (OptionBool(request, "authoritative_wal:", false)) {
    return fail(BackupInvalid(kOperation,
                              "ARCHIVE_AUTHORITATIVE_WAL_FORBIDDEN"));  // no_wal refusal evidence
  }
  if (OptionBool(request, "live_file_shortcut:", false) ||
      OptionValue(request, "source:") == "live_file" ||
      OptionBool(request, "external_file_copy:", false)) {
    return fail(BackupInvalid(kOperation,
                              "ARCHIVE_LIVE_FILE_SHORTCUT_FORBIDDEN"));
  }
  if (!request.engine_mga_authoritative ||
      !request.cleanup_horizon_authoritative ||
      request.authoritative_cleanup_horizon_local_transaction_id == 0) {
    return fail(BackupInvalid(kOperation,
                              "ARCHIVE_MGA_CLEANUP_AUTHORITY_REQUIRED"));
  }
  if (request.legal_hold_active) {
    return fail(BackupInvalid(kOperation, "ARCHIVE_LEGAL_HOLD_ACTIVE"));
  }
  if (!request.retention_policy_installed) {
    return fail(BackupInvalid(kOperation,
                              "ARCHIVE_RETENTION_POLICY_REQUIRED"));
  }
  if (!request.local_archive_filespace_header_verified ||
      !IsLocalArchiveHistoryFilespace(request.archive_filespace,
                                      request.context)) {
    return fail(BackupInvalid(kOperation,
                              "ARCHIVE_FILESPACE_PROOF_REQUIRED"));
  }
  const auto manifest_path = BackupPath(request);
  if (manifest_path.empty()) {
    return fail(BackupInvalid(kOperation,
                              "ARCHIVE_MANIFEST_TARGET_REQUIRED"));
  }
  if (request.retained_history.empty()) {
    return fail(BackupInvalid(kOperation,
                              "ARCHIVE_RETAINED_HISTORY_REQUIRED"));
  }
  if (request.max_row_versions_to_archive == 0 ||
      request.retained_history.size() >
          request.max_row_versions_to_archive) {
    return fail(BackupInvalid(kOperation,
                              "ARCHIVE_BOUNDED_ROW_SET_REQUIRED"));
  }

  for (const auto& record : request.retained_history) {
    const auto row_valid = ValidateArchiveRetainedHistoryRecord(
        kOperation,
        record,
        request.authoritative_cleanup_horizon_local_transaction_id);
    if (row_valid.error) {
      return fail(row_valid);
    }
  }

  EngineUuid archive_uuid;
  archive_uuid.canonical = GenerateCrudEngineUuid("archive_before_reclaim");
  std::uint64_t movement_record_count = 0;
  const auto body = BuildArchiveBeforeReclaimManifestBody(
      request, archive_uuid, &movement_record_count);
  const auto checksum = Fnv1a64(body);
  const std::string manifest_payload =
      body + "CHECKSUM\t" + std::to_string(checksum) + "\n";
  if (!WriteBinaryFile(manifest_path, manifest_payload)) {
    return fail(BackupInvalid(kOperation, "ARCHIVE_MANIFEST_WRITE_FAILED"));
  }
  std::uint64_t manifest_bytes = 0;
  const auto manifest_verified = ReadAndVerifyArchiveBeforeReclaimManifest(
      manifest_path,
      archive_uuid,
      movement_record_count,
      checksum,
      &manifest_bytes);
  if (manifest_verified.error) {
    return fail(manifest_verified);
  }

  (void)scratchbird::core::metrics::PublishArchiveSliceCount(
      1.0, "local_history_archive", "archive_before_reclaim");
  (void)scratchbird::core::metrics::PublishArchiveSliceBytes(
      static_cast<double>(manifest_bytes),
      "local_history_archive",
      "archive_before_reclaim");
  (void)scratchbird::core::metrics::PublishArchiveHealthState(
      1.0, "healthy", "local_history_archive");

  auto result =
      MakeApiBehaviorSuccess<EngineArchiveRetainedHistoryBeforeReclaimResult>(
          request.context, kOperation);
  result.archive_uuid = archive_uuid;
  result.archive_filespace_uuid.canonical =
      TypedUuidText(request.archive_filespace.filespace_uuid);
  result.archived_row_version_count =
      static_cast<EngineApiU64>(request.retained_history.size());
  result.movement_record_count = movement_record_count;
  result.manifest_checksum = checksum;
  result.manifest_bytes = manifest_bytes;
  result.manifest_uri = manifest_path;
  result.local_archive_filespace_bound = true;
  result.manifest_written = true;
  result.manifest_verified = true;
  result.hot_reclaim_authorized = true;
  result.transaction_finality_authority = false;
  result.reclaim_evidence_records.reserve(request.retained_history.size());
  for (const auto& record : request.retained_history) {
    result.reclaim_evidence_records.push_back(ArchiveReclaimEvidenceRecord(
        record,
        request.authoritative_cleanup_horizon_local_transaction_id,
        archive_uuid));
  }

  AddApiBehaviorEvidence(&result,
                         "archive_before_reclaim_manifest",
                         manifest_path);
  AddApiBehaviorEvidence(&result,
                         "archive_before_reclaim_checksum",
                         std::to_string(checksum));
  AddApiBehaviorEvidence(&result,
                         "archive_filespace_uuid",
                         result.archive_filespace_uuid.canonical);
  AddApiBehaviorEvidence(&result,
                         "archive_filespace_access_class",
                         "local_archive_history_read_only");
  AddApiBehaviorEvidence(&result,
                         "archive_reclaim_authorization",
                         archive_uuid.canonical);
  AddApiBehaviorEvidence(&result,
                         "transaction_finality_authority",
                         "false");
  AddApiBehaviorEvidence(&result,
                         "finality_source",
                         "local_mga_transaction_inventory");
  AddApiBehaviorEvidence(&result, "authoritative_wal", "false");
  AddApiBehaviorRow(
      &result,
      {{"archive_uuid", archive_uuid.canonical},
       {"manifest_uri", manifest_path},
       {"archive_filespace_uuid", result.archive_filespace_uuid.canonical},
       {"archive_filespace_access_class", "local_archive_history_read_only"},
       {"archived_row_versions",
        std::to_string(result.archived_row_version_count)},
       {"movement_records", std::to_string(result.movement_record_count)},
       {"manifest_checksum", std::to_string(result.manifest_checksum)},
       {"manifest_verified", "true"},
       {"hot_reclaim_authorized", "true"},
       {"authoritative_cleanup_horizon_local_transaction_id",
        std::to_string(
            request.authoritative_cleanup_horizon_local_transaction_id)},
       {"finality_source", "local_mga_transaction_inventory"},
       {"transaction_finality_authority", "false"},
       {"authoritative_wal", "false"}});
  return result;
}

EngineStartBackupForwardSessionResult EngineStartBackupForwardSession(
    const EngineStartBackupForwardSessionRequest& request) {
  constexpr const char* kOperation =
      "backup_archive.start_backup_forward_session";
  auto fail = [&](EngineApiDiagnostic diagnostic) {
    return MakeApiBehaviorDiagnostic<EngineStartBackupForwardSessionResult>(
        request.context, kOperation, std::move(diagnostic));
  };

  if (!HasBackupCreateRight(request.context)) {
    return fail(MakeSecurityContextRequiredDiagnostic(kOperation));
  }
  if (request.context.database_uuid.canonical.empty() ||
      request.context.database_path.empty()) {
    return fail(BackupInvalid(kOperation,
                              "BACKUP_FORWARD_DATABASE_CONTEXT_REQUIRED"));
  }
  if (OptionValue(request, "scope:") == "cluster" ||
      OptionBool(request, "cluster_recovery_authority:", false)) {
    auto result =
        fail(BackupInvalid(kOperation, "BACKUP_FORWARD_CLUSTER_PROVIDER_REQUIRED"));
    result.cluster_authority_required = true;
    return result;
  }
  if (OptionBool(request, "authoritative_wal:", false)) {
    return fail(BackupInvalid(kOperation,
                              "BACKUP_FORWARD_AUTHORITATIVE_WAL_FORBIDDEN"));  // no_wal refusal evidence
  }
  if (!request.write_after_requested) {
    return fail(BackupInvalid(kOperation,
                              "BACKUP_FORWARD_WRITE_AFTER_REQUIRED"));
  }
  if (request.base_backup_uuid.canonical.empty() ||
      request.base_snapshot_visible_through_local_transaction_id == 0 ||
      request.source_manifest_uri.empty() || request.filespace_uuid.empty()) {
    return fail(BackupInvalid(kOperation,
                              "BACKUP_FORWARD_BASE_SNAPSHOT_REQUIRED"));
  }

  EngineUuid session_uuid;
  session_uuid.canonical = GenerateCrudEngineUuid("backup_forward_session");
  const auto timeline = request.timeline_uuid.empty()
                            ? TimelineUuidFor(request)
                            : request.timeline_uuid;
  const auto fork = request.fork_uuid.empty() ? ForkUuidFor(request)
                                              : request.fork_uuid;
  const auto selected_start =
      request.base_snapshot_visible_through_local_transaction_id + 1;
  std::string ledger_id;
  if (!AppendBackupForwardSessionLedger(
          request,
          "start",
          {{"session_uuid", session_uuid.canonical},
           {"base_backup_uuid", request.base_backup_uuid.canonical},
           {"source_manifest_uri", request.source_manifest_uri},
           {"filespace_uuid", request.filespace_uuid},
           {"timeline_uuid", timeline},
           {"fork_uuid", fork},
           {"base_snapshot_visible_through_local_transaction_id",
            std::to_string(
                request.base_snapshot_visible_through_local_transaction_id)},
           {"selected_start_transaction_id", std::to_string(selected_start)},
           {"coverage_contiguous", "pending"},
           {"write_after_requested", "true"}},
          &ledger_id)) {
    return fail(BackupInvalid(kOperation,
                              "BACKUP_FORWARD_SESSION_LEDGER_WRITE_FAILED"));
  }

  auto result =
      MakeApiBehaviorSuccess<EngineStartBackupForwardSessionResult>(
          request.context, kOperation);
  result.session_uuid = session_uuid;
  result.base_backup_uuid = request.base_backup_uuid;
  result.selected_start_transaction_id = selected_start;
  result.source_manifest_uri = request.source_manifest_uri;
  result.filespace_uuid = request.filespace_uuid;
  result.timeline_uuid = timeline;
  result.fork_uuid = fork;
  result.ledger_uri = ledger_id;
  result.write_after_requested = true;
  result.write_after_recovery_authority = false;
  result.transaction_finality_authority = false;
  AddApiBehaviorEvidence(&result, "backup_forward_session", session_uuid.canonical);
  AddApiBehaviorEvidence(&result, "backup_forward_ledger", ledger_id);
  AddApiBehaviorEvidence(&result,
                         "base_backup_uuid",
                         request.base_backup_uuid.canonical);
  AddApiBehaviorEvidence(&result,
                         "selected_start_transaction_id",
                         std::to_string(selected_start));
  AddApiBehaviorEvidence(&result,
                         "finality_source",
                         "local_mga_transaction_inventory");
  AddApiBehaviorEvidence(&result,
                         "write_after_recovery_authority",
                         "false");
  AddApiBehaviorEvidence(&result,
                         "transaction_finality_authority",
                         "false");
  AddApiBehaviorEvidence(&result, "authoritative_wal", "false");
  AddApiBehaviorRow(
      &result,
      {{"session_uuid", session_uuid.canonical},
       {"base_backup_uuid", request.base_backup_uuid.canonical},
       {"source_manifest_uri", request.source_manifest_uri},
       {"filespace_uuid", request.filespace_uuid},
       {"timeline_uuid", timeline},
       {"fork_uuid", fork},
       {"selected_start_transaction_id", std::to_string(selected_start)},
       {"write_after_requested", "true"},
       {"write_after_recovery_authority", "false"},
       {"transaction_finality_authority", "false"},
       {"finality_source", "local_mga_transaction_inventory"}});
  return result;
}

EngineFinishBackupForwardSessionResult EngineFinishBackupForwardSession(
    const EngineFinishBackupForwardSessionRequest& request) {
  constexpr const char* kOperation =
      "backup_archive.finish_backup_forward_session";
  auto fail = [&](EngineApiDiagnostic diagnostic) {
    return MakeApiBehaviorDiagnostic<EngineFinishBackupForwardSessionResult>(
        request.context, kOperation, std::move(diagnostic));
  };

  if (!HasBackupCreateRight(request.context)) {
    return fail(MakeSecurityContextRequiredDiagnostic(kOperation));
  }
  if (request.context.database_uuid.canonical.empty() ||
      request.context.database_path.empty()) {
    return fail(BackupInvalid(kOperation,
                              "BACKUP_FORWARD_DATABASE_CONTEXT_REQUIRED"));
  }
  if (OptionValue(request, "scope:") == "cluster" ||
      OptionBool(request, "cluster_recovery_authority:", false)) {
    auto result =
        fail(BackupInvalid(kOperation, "BACKUP_FORWARD_CLUSTER_PROVIDER_REQUIRED"));
    result.cluster_authority_required = true;
    return result;
  }
  if (OptionBool(request, "authoritative_wal:", false)) {
    return fail(BackupInvalid(kOperation,
                              "BACKUP_FORWARD_AUTHORITATIVE_WAL_FORBIDDEN"));  // no_wal refusal evidence
  }
  if (!request.write_after_requested) {
    return fail(BackupInvalid(kOperation,
                              "BACKUP_FORWARD_WRITE_AFTER_REQUIRED"));
  }
  if (request.session_uuid.canonical.empty() ||
      request.base_backup_uuid.canonical.empty() ||
      request.source_manifest_uri.empty() || request.delta_manifest_uri.empty() ||
      request.filespace_uuid.empty() ||
      request.selected_start_transaction_id == 0 ||
      request.finish_transaction_id == 0) {
    return fail(BackupInvalid(kOperation,
                              "BACKUP_FORWARD_SESSION_PROOF_REQUIRED"));
  }
  if (request.finish_transaction_id < request.selected_start_transaction_id) {
    return fail(BackupInvalid(kOperation,
                              "BACKUP_FORWARD_COVERAGE_RANGE_INVALID"));
  }
  if (request.expected_previous_end_transaction_id != 0 &&
      request.selected_start_transaction_id !=
          request.expected_previous_end_transaction_id + 1) {
    return fail(BackupInvalid(kOperation,
                              "BACKUP_FORWARD_COVERAGE_GAP"));
  }

  EnginePackageDeltaStreamRequest delta;
  delta.context = request.context;
  delta.option_envelopes = {
      "target_uri:" + request.delta_manifest_uri,
      "source_backup_uuid:" + request.base_backup_uuid.canonical,
      "filespace_uuid:" + request.filespace_uuid,
      "start_transaction_id:" +
          std::to_string(request.selected_start_transaction_id),
      "end_transaction_id:" + std::to_string(request.finish_transaction_id),
      "coverage_start_transaction_id:" +
          std::to_string(request.selected_start_transaction_id),
      "coverage_end_transaction_id:" +
          std::to_string(request.finish_transaction_id),
      "timeline_uuid:" + (request.timeline_uuid.empty()
                              ? TimelineUuidFor(request)
                              : request.timeline_uuid),
      "fork_uuid:" + (request.fork_uuid.empty() ? ForkUuidFor(request)
                                                 : request.fork_uuid)};
  const auto packaged = EnginePackageDeltaStream(delta);
  if (!packaged.ok) {
    EngineApiDiagnostic diagnostic =
        packaged.diagnostics.empty()
            ? BackupInvalid(kOperation, "BACKUP_FORWARD_DELTA_PACKAGE_FAILED")
            : packaged.diagnostics.front();
    return fail(std::move(diagnostic));
  }

  const auto timeline = request.timeline_uuid.empty()
                            ? TimelineUuidFor(request)
                            : request.timeline_uuid;
  const auto fork = request.fork_uuid.empty() ? ForkUuidFor(request)
                                              : request.fork_uuid;
  std::string ledger_id;
  if (!AppendBackupForwardSessionLedger(
          request,
          "finish",
          {{"session_uuid", request.session_uuid.canonical},
           {"base_backup_uuid", request.base_backup_uuid.canonical},
           {"delta_uuid", packaged.delta_uuid.canonical},
           {"source_manifest_uri", request.source_manifest_uri},
           {"delta_manifest_uri", request.delta_manifest_uri},
           {"filespace_uuid", request.filespace_uuid},
           {"timeline_uuid", timeline},
           {"fork_uuid", fork},
           {"selected_start_transaction_id",
            std::to_string(request.selected_start_transaction_id)},
           {"finish_transaction_id",
            std::to_string(request.finish_transaction_id)},
           {"coverage_contiguous", "true"},
           {"write_after_segment_immutable", "true"}},
          &ledger_id)) {
    return fail(BackupInvalid(kOperation,
                              "BACKUP_FORWARD_SESSION_LEDGER_WRITE_FAILED"));
  }

  auto result =
      MakeApiBehaviorSuccess<EngineFinishBackupForwardSessionResult>(
          request.context, kOperation);
  result.session_uuid = request.session_uuid;
  result.base_backup_uuid = request.base_backup_uuid;
  result.delta_uuid = packaged.delta_uuid;
  result.selected_start_transaction_id = request.selected_start_transaction_id;
  result.finish_transaction_id = request.finish_transaction_id;
  result.packaged_row_count = packaged.row_count;
  result.packaged_table_count = packaged.table_count;
  result.source_manifest_uri = request.source_manifest_uri;
  result.delta_manifest_uri = request.delta_manifest_uri;
  result.filespace_uuid = request.filespace_uuid;
  result.timeline_uuid = timeline;
  result.fork_uuid = fork;
  result.ledger_uri = ledger_id;
  result.coverage_contiguous = true;
  result.write_after_segment_immutable = true;
  result.write_after_recovery_authority = false;
  result.cluster_recovery_authority = false;
  result.transaction_finality_authority = false;
  AddApiBehaviorEvidence(&result,
                         "backup_forward_session",
                         request.session_uuid.canonical);
  AddApiBehaviorEvidence(&result, "backup_forward_ledger", ledger_id);
  AddApiBehaviorEvidence(&result,
                         "delta_manifest",
                         request.delta_manifest_uri);
  AddApiBehaviorEvidence(&result, "coverage_contiguous", "true");
  AddApiBehaviorEvidence(&result,
                         "write_after_segment_immutable",
                         "true");
  AddApiBehaviorEvidence(&result,
                         "write_after_recovery_authority",
                         "false");
  AddApiBehaviorEvidence(&result,
                         "cluster_recovery_authority",
                         "false");
  AddApiBehaviorEvidence(&result,
                         "transaction_finality_authority",
                         "false");
  AddApiBehaviorEvidence(&result,
                         "finality_source",
                         "local_mga_transaction_inventory");
  AddApiBehaviorEvidence(&result, "authoritative_wal", "false");
  AddApiBehaviorRow(
      &result,
      {{"session_uuid", request.session_uuid.canonical},
       {"base_backup_uuid", request.base_backup_uuid.canonical},
       {"delta_uuid", packaged.delta_uuid.canonical},
       {"source_manifest_uri", request.source_manifest_uri},
       {"delta_manifest_uri", request.delta_manifest_uri},
       {"filespace_uuid", request.filespace_uuid},
       {"timeline_uuid", timeline},
       {"fork_uuid", fork},
       {"selected_start_transaction_id",
        std::to_string(request.selected_start_transaction_id)},
       {"finish_transaction_id", std::to_string(request.finish_transaction_id)},
       {"packaged_rows", std::to_string(result.packaged_row_count)},
       {"packaged_tables", std::to_string(result.packaged_table_count)},
       {"coverage_contiguous", "true"},
       {"write_after_segment_immutable", "true"},
       {"write_after_recovery_authority", "false"},
       {"cluster_recovery_authority", "false"},
       {"transaction_finality_authority", "false"},
       {"finality_source", "local_mga_transaction_inventory"}});
  return result;
}

EngineEvaluateHistoryDisposalMultiHorizonResult
EngineEvaluateHistoryDisposalMultiHorizon(
    const EngineEvaluateHistoryDisposalMultiHorizonRequest& request) {
  constexpr const char* kOperation =
      "backup_archive.evaluate_history_disposal_multi_horizon";

  auto populate_common =
      [&](EngineEvaluateHistoryDisposalMultiHorizonResult* result) {
        result->disposable_start_transaction_id =
            request.disposable_start_transaction_id;
        result->disposable_end_transaction_id =
            request.disposable_end_transaction_id;
        result->filespace_uuid = request.filespace_uuid;
        result->archive_manifest_uri = request.archive_manifest_uri;
        result->write_after_segment_uri = request.write_after_segment_uri;
        result->physical_reclaim_authorized = false;
        result->archive_deletion_authorized = false;
        result->write_after_truncation_authorized = false;
        result->disposal_authorized = false;
        result->fail_closed = true;
        result->mutation_performed = false;
        result->transaction_finality_authority = false;
        result->write_after_recovery_authority = false;
        result->cluster_recovery_authority = false;
      };

  auto add_common_evidence =
      [&](EngineEvaluateHistoryDisposalMultiHorizonResult* result) {
        AddApiBehaviorEvidence(result, "multi_horizon_disposal_guard",
                               result->decision_uuid.canonical);
        AddApiBehaviorEvidence(result, "filespace_uuid",
                               request.filespace_uuid);
        AddApiBehaviorEvidence(result, "disposable_range",
                               std::to_string(
                                   request.disposable_start_transaction_id) +
                                   ".." +
                                   std::to_string(
                                       request.disposable_end_transaction_id));
        AddApiBehaviorEvidence(result, "physical_reclaim_authorized",
                               result->physical_reclaim_authorized ? "true"
                                                                   : "false");
        AddApiBehaviorEvidence(result, "archive_deletion_authorized",
                               result->archive_deletion_authorized ? "true"
                                                                   : "false");
        AddApiBehaviorEvidence(result, "write_after_truncation_authorized",
                               result->write_after_truncation_authorized
                                   ? "true"
                                   : "false");
        AddApiBehaviorEvidence(result, "mutation_performed", "false");
        AddApiBehaviorEvidence(result, "finality_source",
                               "local_mga_transaction_inventory");
        AddApiBehaviorEvidence(result, "transaction_finality_authority",
                               "false");
        AddApiBehaviorEvidence(result, "write_after_recovery_authority",
                               "false");
        AddApiBehaviorEvidence(result, "cluster_recovery_authority", "false");
        AddApiBehaviorEvidence(result, "authoritative_wal", "false");
        AddApiBehaviorRow(
            result,
            {{"decision_uuid", result->decision_uuid.canonical},
             {"filespace_uuid", request.filespace_uuid},
             {"disposable_start_transaction_id",
              std::to_string(request.disposable_start_transaction_id)},
             {"disposable_end_transaction_id",
              std::to_string(request.disposable_end_transaction_id)},
             {"blocking_horizon_kind", result->blocking_horizon_kind},
             {"blocking_horizon_transaction_id",
              std::to_string(result->blocking_horizon_transaction_id)},
             {"disposal_authorized",
              result->disposal_authorized ? "true" : "false"},
             {"physical_reclaim_authorized",
              result->physical_reclaim_authorized ? "true" : "false"},
             {"archive_deletion_authorized",
              result->archive_deletion_authorized ? "true" : "false"},
             {"write_after_truncation_authorized",
              result->write_after_truncation_authorized ? "true" : "false"},
             {"fail_closed", result->fail_closed ? "true" : "false"},
             {"mutation_performed", "false"},
             {"finality_source", "local_mga_transaction_inventory"},
             {"transaction_finality_authority", "false"},
             {"write_after_recovery_authority", "false"},
             {"cluster_recovery_authority", "false"},
             {"authoritative_wal", "false"}});
      };

  auto fail = [&](EngineApiDiagnostic diagnostic,
                  const char* blocking_horizon_kind,
                  std::uint64_t blocking_horizon_transaction_id) {
    auto result =
        MakeApiBehaviorDiagnostic<EngineEvaluateHistoryDisposalMultiHorizonResult>(
            request.context, kOperation, std::move(diagnostic));
    populate_common(&result);
    result.decision_uuid.canonical = GenerateCrudEngineUuid("history_disposal_refusal");
    result.blocking_horizon_kind =
        MultiHorizonProofDisplayName(blocking_horizon_kind);
    result.blocking_horizon_transaction_id =
        blocking_horizon_transaction_id;
    add_common_evidence(&result);
    return result;
  };

  if (!HasBackupCreateRight(request.context)) {
    return fail(MakeSecurityContextRequiredDiagnostic(kOperation),
                "security_context",
                0);
  }
  if (request.context.database_uuid.canonical.empty() ||
      request.context.database_path.empty()) {
    return fail(BackupInvalid(kOperation,
                              "MULTI_HORIZON_DATABASE_CONTEXT_REQUIRED"),
                "database_context",
                0);
  }
  if (OptionValue(request, "scope:") == "cluster" ||
      OptionBool(request, "cluster_disposal:", false) ||
      OptionBool(request, "cluster_recovery_authority:", false)) {
    auto result = fail(BackupInvalid(kOperation,
                                     "MULTI_HORIZON_CLUSTER_PROVIDER_REQUIRED"),
                       "cluster_provider",
                       0);
    result.cluster_authority_required = true;
    return result;
  }
  if (OptionBool(request, "authoritative_wal:", false)) {
    return fail(BackupInvalid(kOperation,
                              "MULTI_HORIZON_AUTHORITATIVE_WAL_FORBIDDEN"),  // no_wal refusal evidence
                "authoritative_wal",  // no_wal result field
                0);
  }
  if (request.disposable_start_transaction_id == 0 ||
      request.disposable_end_transaction_id == 0 ||
      request.disposable_end_transaction_id <
          request.disposable_start_transaction_id) {
    return fail(BackupInvalid(kOperation,
                              "MULTI_HORIZON_RANGE_INVALID"),
                "range",
                0);
  }
  if (request.disposable_end_transaction_id ==
      std::numeric_limits<std::uint64_t>::max()) {
    return fail(BackupInvalid(kOperation,
                              "MULTI_HORIZON_RANGE_OVERFLOW"),
                "range",
                request.disposable_end_transaction_id);
  }
  if (request.filespace_uuid.empty()) {
    return fail(BackupInvalid(kOperation,
                              "MULTI_HORIZON_FILESPACE_UUID_REQUIRED"),
                "filespace_uuid",
                0);
  }
  if (!request.physical_reclaim_requested &&
      !request.archive_deletion_requested &&
      !request.write_after_truncation_requested) {
    return fail(BackupInvalid(kOperation,
                              "MULTI_HORIZON_DISPOSAL_ACTION_REQUIRED"),
                "disposal_action",
                0);
  }
  if (request.archive_deletion_requested &&
      request.archive_manifest_uri.empty()) {
    return fail(BackupInvalid(kOperation,
                              "MULTI_HORIZON_ARCHIVE_MANIFEST_REQUIRED"),
                "archive",
                0);
  }
  if (request.write_after_truncation_requested &&
      request.write_after_segment_uri.empty()) {
    return fail(BackupInvalid(kOperation,
                              "MULTI_HORIZON_WRITE_AFTER_SEGMENT_REQUIRED"),
                "backup_forward",
                0);
  }

  const std::vector<MultiHorizonProofInput> proofs = {
      {"reader",
       request.reader_horizon_authoritative,
       request.reader_horizon_transaction_id,
       false},
      {"writer",
       request.writer_horizon_authoritative,
       request.writer_horizon_transaction_id,
       false},
      {"parser_snapshot",
       request.parser_snapshot_horizon_authoritative,
       request.parser_snapshot_horizon_transaction_id,
       false},
      {"backup_forward",
       request.backup_forward_horizon_authoritative,
       request.backup_forward_horizon_transaction_id,
       false},
      {"archive",
       request.archive_horizon_authoritative,
       request.archive_horizon_transaction_id,
       false},
      {"legal_hold",
       request.legal_hold_horizon_authoritative,
       request.legal_hold_horizon_transaction_id,
       request.legal_hold_active},
      {"detached_filespace",
       request.detached_filespace_horizon_authoritative,
       request.detached_filespace_horizon_transaction_id,
       request.detached_filespace_hold_active},
      {"stable_checkpoint",
       request.stable_checkpoint_horizon_authoritative,
       request.stable_checkpoint_horizon_transaction_id,
       false},
      {"local_durable",
       request.local_durable_horizon_authoritative,
       request.local_durable_horizon_transaction_id,
       false},
      {"restore_reachability",
       request.restore_reachability_horizon_authoritative,
       request.restore_reachability_horizon_transaction_id,
       false},
  };
  const auto required_past_end =
      request.disposable_end_transaction_id + 1;
  for (const auto& proof : proofs) {
    if (proof.hold_active) {
      return fail(BackupInvalid(kOperation,
                                std::string("MULTI_HORIZON_HOLD_ACTIVE:") +
                                    proof.kind),
                  proof.kind,
                  proof.horizon_transaction_id);
    }
    if (!proof.authoritative) {
      return fail(BackupInvalid(
                      kOperation,
                      std::string("MULTI_HORIZON_PROOF_NOT_AUTHORITATIVE:") +
                          proof.kind),
                  proof.kind,
                  proof.horizon_transaction_id);
    }
    if (proof.horizon_transaction_id < required_past_end) {
      return fail(BackupInvalid(kOperation,
                                std::string("MULTI_HORIZON_RANGE_HELD:") +
                                    proof.kind),
                  proof.kind,
                  proof.horizon_transaction_id);
    }
  }

  auto result =
      MakeApiBehaviorSuccess<EngineEvaluateHistoryDisposalMultiHorizonResult>(
          request.context, kOperation);
  populate_common(&result);
  result.decision_uuid.canonical = GenerateCrudEngineUuid("history_disposal_guard");
  result.disposal_authorized = true;
  result.physical_reclaim_authorized = request.physical_reclaim_requested;
  result.archive_deletion_authorized = request.archive_deletion_requested;
  result.write_after_truncation_authorized =
      request.write_after_truncation_requested;
  result.fail_closed = false;
  add_common_evidence(&result);
  return result;
}

const char* ClusterCatalogTransferOperationName(
    ClusterCatalogTransferOperation operation) {
  switch (operation) {
    case ClusterCatalogTransferOperation::backup: return "backup";
    case ClusterCatalogTransferOperation::restore: return "restore";
    case ClusterCatalogTransferOperation::attach: return "attach";
    case ClusterCatalogTransferOperation::import: return "import";
    case ClusterCatalogTransferOperation::public_export: return "public_export";
  }
  return "unknown";
}

const char* ClusterCatalogIdentityDispositionName(
    ClusterCatalogIdentityDisposition disposition) {
  switch (disposition) {
    case ClusterCatalogIdentityDisposition::preserve: return "preserve";
    case ClusterCatalogIdentityDisposition::remap: return "remap";
    case ClusterCatalogIdentityDisposition::reject: return "reject";
    case ClusterCatalogIdentityDisposition::quarantine: return "quarantine";
  }
  return "unknown";
}

EngineEvaluateClusterCatalogBackupRestoreIdentityResult
EngineEvaluateClusterCatalogBackupRestoreIdentityPolicy(
    const EngineEvaluateClusterCatalogBackupRestoreIdentityRequest& request) {
  constexpr const char* kOperation =
      "backup_archive.cluster_catalog_backup_restore_identity";

  auto populate_common =
      [&](EngineEvaluateClusterCatalogBackupRestoreIdentityResult* result) {
        result->operation_id = request.operation_id.empty()
                                   ? kOperation
                                   : request.operation_id;
        result->transfer_operation = request.transfer_operation;
        result->embedded_trust_mode_observed =
            request.context.trust_mode == EngineTrustMode::embedded_in_process;
        result->cluster_authority_required = true;
        result->fail_closed = true;
        result->mutation_performed = false;
        result->local_runtime_execution_enabled = false;
        result->cluster_recovery_authority = false;
        AddApiBehaviorEvidence(result,
                               "cluster_catalog_backup_restore",
                               ClusterCatalogTransferOperationName(
                                   request.transfer_operation));
        AddApiBehaviorEvidence(result, "mutation_performed", "false");
        AddApiBehaviorEvidence(result,
                               "local_runtime_execution_enabled",
                               "false");
        AddApiBehaviorEvidence(result,
                               "cluster_recovery_authority",
                               "false");
      };

  auto fail =
      [&](EngineApiDiagnostic diagnostic)
          -> EngineEvaluateClusterCatalogBackupRestoreIdentityResult {
    EngineEvaluateClusterCatalogBackupRestoreIdentityResult result;
    populate_common(&result);
    result.ok = false;
    result.diagnostics.push_back(std::move(diagnostic));
    return result;
  };

  const bool cluster_catalog_present =
      request.cluster_catalog_present || !request.record_set.records.empty() ||
      !request.identity_rules.empty();

  if (!cluster_catalog_present) {
    auto result =
        MakeApiBehaviorSuccess<
            EngineEvaluateClusterCatalogBackupRestoreIdentityResult>(
            request.context,
            kOperation);
    populate_common(&result);
    result.cluster_authority_required = false;
    result.fail_closed = false;
    result.resolver_comment_security_projection_proven = true;
    AddApiBehaviorEvidence(&result, "cluster_catalog_present", "false");
    AddApiBehaviorRow(
        &result,
        {{"transfer_operation",
          ClusterCatalogTransferOperationName(request.transfer_operation)},
         {"cluster_catalog_present", "false"},
         {"identity_policy", "not_required"},
         {"mutation_performed", "false"},
         {"cluster_recovery_authority", "false"}});
    return result;
  }

  if (!request.context.security_context_present) {
    return fail(MakeSecurityContextRequiredDiagnostic(kOperation));
  }
  if (!request.context.cluster_authority_available ||
      !request.joined_cluster_catalog_state) {
    return fail(BackupInvalid(
        kOperation,
        "CLUSTER_CATALOG_JOINED_AUTHORITY_REQUIRED"));
  }
  if (!request.external_provider_available) {
    return fail(BackupInvalid(
        kOperation,
        "CLUSTER_CATALOG_EXTERNAL_PROVIDER_REQUIRED"));
  }
  if (request.transfer_operation ==
          ClusterCatalogTransferOperation::public_export &&
      !request.public_export_manifest_clean) {
    return fail(BackupInvalid(
        kOperation,
        "CLUSTER_CATALOG_PUBLIC_EXPORT_MANIFEST_UNCLEAN"));
  }

  const auto validated =
      catalog::ValidateClusterCatalogRecordSet(request.record_set);
  if (!validated.ok()) {
    return fail(MakeEngineApiDiagnostic(
        validated.diagnostic.diagnostic_code,
        validated.diagnostic.message_key,
        validated.diagnostic.diagnostic_code,
        true));
  }
  if (request.identity_rules.size() != request.record_set.records.size()) {
    return fail(BackupInvalid(
        kOperation,
        "CLUSTER_CATALOG_IDENTITY_RULE_COVERAGE_REQUIRED"));
  }

  std::map<std::string, std::string> table_by_source_uuid;
  for (const auto& record : request.record_set.records) {
    const std::string primary_uuid =
        catalog::ClusterCatalogRecordPrimaryUuidValue(record);
    table_by_source_uuid[primary_uuid] = record.table_path;
  }

  std::map<std::string, std::string> target_to_source;
  auto result =
      MakeApiBehaviorSuccess<
          EngineEvaluateClusterCatalogBackupRestoreIdentityResult>(
          request.context,
          kOperation);
  populate_common(&result);
  result.fail_closed = false;

  auto reject_rule =
      [&](const ClusterCatalogIdentityTransferRule& rule,
          const std::string& diagnostic_code,
          const std::string& detail)
          -> EngineEvaluateClusterCatalogBackupRestoreIdentityResult {
    auto failed = fail(BackupInvalid(kOperation, diagnostic_code + ":" + detail));
    ClusterCatalogIdentityTransferDecision decision;
    decision.table_path = rule.table_path;
    decision.source_record_uuid = rule.source_record_uuid;
    decision.target_record_uuid = rule.target_record_uuid;
    decision.disposition = rule.disposition;
    decision.accepted = false;
    decision.diagnostic_code = diagnostic_code;
    failed.decisions.push_back(std::move(decision));
    return failed;
  };

  for (const auto& rule : request.identity_rules) {
    const auto source_it = table_by_source_uuid.find(rule.source_record_uuid);
    if (source_it == table_by_source_uuid.end()) {
      return reject_rule(rule,
                         "CLUSTER_CATALOG_SOURCE_IDENTITY_UNKNOWN",
                         rule.source_record_uuid);
    }
    if (rule.table_path != source_it->second) {
      return reject_rule(rule,
                         "CLUSTER_CATALOG_SOURCE_TABLE_MISMATCH",
                         rule.table_path);
    }

    ClusterCatalogIdentityTransferDecision decision;
    decision.table_path = rule.table_path;
    decision.source_record_uuid = rule.source_record_uuid;
    decision.target_record_uuid = rule.target_record_uuid.empty()
                                      ? rule.source_record_uuid
                                      : rule.target_record_uuid;
    decision.disposition = rule.disposition;
    decision.accepted = true;
    decision.mutation_performed = false;
    decision.diagnostic_code = "SB_ENGINE_API_OK";

    const bool common_proof =
        rule.resolver_evidence_proven && rule.comment_evidence_proven &&
        rule.security_binding_proven && rule.projection_integrity_proven &&
        rule.provider_digest_verified;
    const bool terminal_disposition =
        rule.disposition == ClusterCatalogIdentityDisposition::reject ||
        rule.disposition == ClusterCatalogIdentityDisposition::quarantine;
    if (!common_proof && !terminal_disposition) {
      return reject_rule(rule,
                         "CLUSTER_CATALOG_IDENTITY_EVIDENCE_INCOMPLETE",
                         rule.source_record_uuid);
    }

    switch (rule.disposition) {
      case ClusterCatalogIdentityDisposition::preserve:
        if (decision.target_record_uuid != rule.source_record_uuid) {
          return reject_rule(rule,
                             "CLUSTER_CATALOG_PRESERVE_TARGET_CHANGED",
                             rule.source_record_uuid);
        }
        if (!rule.no_uuid_reuse_proven) {
          return reject_rule(rule,
                             "CLUSTER_CATALOG_UUID_REUSE_PROOF_REQUIRED",
                             rule.source_record_uuid);
        }
        decision.identity_preserved = true;
        ++result.preserved_count;
        break;
      case ClusterCatalogIdentityDisposition::remap:
        if (decision.target_record_uuid.empty() ||
            decision.target_record_uuid == rule.source_record_uuid) {
          return reject_rule(rule,
                             "CLUSTER_CATALOG_REMAP_TARGET_REQUIRED",
                             rule.source_record_uuid);
        }
        if (!rule.explicit_remap_authorized || !rule.no_uuid_reuse_proven) {
          return reject_rule(rule,
                             "CLUSTER_CATALOG_REMAP_PROOF_REQUIRED",
                             rule.source_record_uuid);
        }
        decision.identity_remapped = true;
        ++result.remapped_count;
        break;
      case ClusterCatalogIdentityDisposition::reject:
        if (rule.disposition_reason.empty()) {
          return reject_rule(rule,
                             "CLUSTER_CATALOG_REJECT_REASON_REQUIRED",
                             rule.source_record_uuid);
        }
        decision.identity_rejected = true;
        ++result.rejected_count;
        break;
      case ClusterCatalogIdentityDisposition::quarantine:
        if (rule.disposition_reason.empty()) {
          return reject_rule(rule,
                             "CLUSTER_CATALOG_QUARANTINE_REASON_REQUIRED",
                             rule.source_record_uuid);
        }
        decision.identity_quarantined = true;
        ++result.quarantined_count;
        break;
    }

    if (!terminal_disposition) {
      const auto [target_it, inserted] =
          target_to_source.emplace(decision.target_record_uuid,
                                   rule.source_record_uuid);
      if (!inserted && target_it->second != rule.source_record_uuid) {
        return reject_rule(rule,
                           "CLUSTER_CATALOG_TARGET_UUID_REUSED",
                           decision.target_record_uuid);
      }
    }

    AddApiBehaviorRow(
        &result,
        {{"transfer_operation",
          ClusterCatalogTransferOperationName(request.transfer_operation)},
         {"table_path", decision.table_path},
         {"source_record_uuid", decision.source_record_uuid},
         {"target_record_uuid", decision.target_record_uuid},
         {"disposition",
          ClusterCatalogIdentityDispositionName(decision.disposition)},
         {"accepted", decision.accepted ? "true" : "false"},
         {"mutation_performed", "false"},
         {"resolver_evidence_proven",
          rule.resolver_evidence_proven ? "true" : "false"},
         {"comment_evidence_proven",
          rule.comment_evidence_proven ? "true" : "false"},
         {"security_binding_proven",
          rule.security_binding_proven ? "true" : "false"},
         {"projection_integrity_proven",
          rule.projection_integrity_proven ? "true" : "false"},
         {"provider_digest_verified",
          rule.provider_digest_verified ? "true" : "false"}});
    result.decisions.push_back(std::move(decision));
  }

  result.resolver_comment_security_projection_proven =
      result.decisions.size() == request.identity_rules.size();
  AddApiBehaviorEvidence(&result, "cluster_catalog_present", "true");
  AddApiBehaviorEvidence(&result,
                         "cluster_catalog_identity_policy",
                         "preserve_remap_reject_quarantine_explicit");
  AddApiBehaviorEvidence(&result,
                         "resolver_comment_security_projection_proven",
                         result.resolver_comment_security_projection_proven
                             ? "true"
                             : "false");
  AddApiBehaviorEvidence(&result, "preserved_count",
                         std::to_string(result.preserved_count));
  AddApiBehaviorEvidence(&result, "remapped_count",
                         std::to_string(result.remapped_count));
  AddApiBehaviorEvidence(&result, "rejected_count",
                         std::to_string(result.rejected_count));
  AddApiBehaviorEvidence(&result, "quarantined_count",
                         std::to_string(result.quarantined_count));
  return result;
}

EngineStartLogicalBackupResult EngineStartLogicalBackup(const EngineStartLogicalBackupRequest& request) {
  constexpr const char* kOperation = "backup_archive.start_logical_backup";
  if (!HasBackupCreateRight(request.context)) {
    return MakeApiBehaviorDiagnostic<EngineStartLogicalBackupResult>(request.context,
                                                                     kOperation,
                                                                     MakeSecurityContextRequiredDiagnostic(kOperation));
  }
  if (OptionValue(request, "scope:") == "cluster" && !request.context.cluster_authority_available) {
    return MakeApiBehaviorDiagnostic<EngineStartLogicalBackupResult>(request.context,
                                                                     kOperation,
                                                                     BackupInvalid(kOperation, "BACKUP_CLUSTER_SCOPE_ABSENT"));
  }
  const auto lifecycle =
      EvaluateBackupArchiveLifecycleAdmission(request, BackupArchiveLifecycleOperation::logical_backup);
  if (!lifecycle.admitted) {
    return MakeApiBehaviorDiagnostic<EngineStartLogicalBackupResult>(
        request.context, kOperation, lifecycle.diagnostic);
  }
  const auto path = BackupPath(request);
  if (path.empty()) {
    return MakeApiBehaviorDiagnostic<EngineStartLogicalBackupResult>(request.context,
                                                                     kOperation,
                                                                     BackupInvalid(kOperation, "BACKUP_TARGET_UNAVAILABLE"));
  }
  if (RequiredManifestOption(request, "filespace_uuid:").empty()) {
    return MakeApiBehaviorDiagnostic<EngineStartLogicalBackupResult>(request.context,
                                                                     kOperation,
                                                                     BackupInvalid(kOperation, "BACKUP_FILESPACE_UUID_REQUIRED"));
  }
  const auto loaded_mga = LoadMgaRelationStoreState(request.context);
  if (!loaded_mga.ok) {
    return MakeApiBehaviorDiagnostic<EngineStartLogicalBackupResult>(request.context, kOperation, loaded_mga.diagnostic);
  }
  const CrudState loaded_state = BuildCrudCompatibilityStateFromMga(loaded_mga.state);
  LogicalBackupRecordSet records;
  records.backup_uuid.canonical = GenerateCrudEngineUuid("backup");
  records.snapshot_uuid.canonical = GenerateCrudEngineUuid("snapshot");
  records.snapshot_tx = MaxCommittedTransaction(loaded_state);
  records.transaction_states = loaded_state.transactions;
  const auto unsafe_gap = UnsafeFinalityGap(loaded_state, records.snapshot_tx);
  if (!unsafe_gap.empty() && OptionValue(request, "allow_inspect_only_archive:") != "true") {
    return MakeApiBehaviorDiagnostic<EngineStartLogicalBackupResult>(
        request.context,
        kOperation,
        BackupInvalid(kOperation, "BACKUP_FINALITY_GAP:" + unsafe_gap));
  }
  for (const auto& table : loaded_state.tables) {
    if (!table.temporary && CrudCreatorVisible(loaded_state, table.creator_tx, table.event_sequence, records.snapshot_tx)) {
      records.tables.push_back(table);
      auto rows = VisibleCrudRows(loaded_state, table.table_uuid, records.snapshot_tx);
      records.rows.insert(records.rows.end(), rows.begin(), rows.end());
      auto indexes = VisibleCrudIndexesForTable(loaded_state, table.table_uuid, records.snapshot_tx);
      records.indexes.insert(records.indexes.end(), indexes.begin(), indexes.end());
    }
  }
  const auto temporary_exclusions =
      CountTemporarySnapshotExclusions(loaded_state, records.snapshot_tx);
  const auto body = BuildManifestBody(request, records);
  const auto checksum = Fnv1a64(body);
  const std::string manifest_payload = body + "CHECKSUM\t" + std::to_string(checksum) + "\n";
  std::filesystem::create_directories(std::filesystem::path(path).parent_path());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    return MakeApiBehaviorDiagnostic<EngineStartLogicalBackupResult>(request.context,
                                                                     kOperation,
                                                                     BackupInvalid(kOperation, "BACKUP_TARGET_UNAVAILABLE"));
  }
  out << manifest_payload;
  out.close();
  if (!out) {
    return MakeApiBehaviorDiagnostic<EngineStartLogicalBackupResult>(request.context,
                                                                     kOperation,
                                                                     BackupInvalid(kOperation, "BACKUP_TARGET_WRITE_FAILED"));
  }
  (void)scratchbird::core::metrics::PublishBackupInProgress(0.0, "logical_backup");
  (void)scratchbird::core::metrics::PublishBackupProgressPercent(100.0, "logical_backup");
  PublishArchiveSliceCreatedMetrics(request, "logical_snapshot", "created", manifest_payload.size());
  auto result = MakeApiBehaviorSuccess<EngineStartLogicalBackupResult>(request.context, kOperation);
  AddBackupLifecycleEvidence(&result, lifecycle);
  result.backup_uuid = records.backup_uuid;
  result.snapshot_uuid = records.snapshot_uuid;
  result.snapshot_visible_through_local_transaction_id = records.snapshot_tx;
  result.table_count = records.tables.size();
  result.row_count = records.rows.size();
  result.index_count = records.indexes.size();
  result.manifest_uri = path;
  AddApiBehaviorEvidence(&result, "backup_manifest", path);
  AddApiBehaviorEvidence(&result, "backup_checksum", std::to_string(checksum));
  AddApiBehaviorEvidence(&result, "archive_slice_bytes", std::to_string(manifest_payload.size()));
  AddApiBehaviorEvidence(&result, "archive_retention_max_age_microseconds", std::to_string(ArchiveMaxAgeMicroseconds(request)));
  AddApiBehaviorEvidence(&result, "finality_boundary_local_transaction_id", std::to_string(records.snapshot_tx));
  AddApiBehaviorEvidence(&result, "lineage_source", "mga_row_version_lineage");
  AddApiBehaviorEvidence(&result, "authoritative_wal", "false");
  AddTemporaryBackupExclusionEvidence(&result, temporary_exclusions);
  AddApiBehaviorRow(&result, {{"backup_uuid", result.backup_uuid.canonical},
                              {"snapshot_uuid", result.snapshot_uuid.canonical},
                              {"manifest_uri", path},
                              {"snapshot_tx", std::to_string(records.snapshot_tx)},
                              {"finality_boundary_local_transaction_id", std::to_string(records.snapshot_tx)},
                              {"lineage_source", "mga_row_version_lineage"},
                              {"tables", std::to_string(result.table_count)},
                              {"rows", std::to_string(result.row_count)},
                              {"indexes", std::to_string(result.index_count)},
                              {"temporary_content_excluded", "true"},
                              {"temporary_tables_excluded", std::to_string(temporary_exclusions.table_count)},
                              {"temporary_rows_excluded", std::to_string(temporary_exclusions.row_count)},
                              {"temporary_indexes_excluded", std::to_string(temporary_exclusions.index_count)},
                              {"authoritative_wal", "false"}});
  return result;
}

EngineRestoreLogicalBackupResult EngineRestoreLogicalBackup(const EngineRestoreLogicalBackupRequest& request) {
  constexpr const char* kOperation = "backup_archive.restore_logical_backup";
  if (!HasBackupRestoreRight(request.context)) {
    return MakeApiBehaviorDiagnostic<EngineRestoreLogicalBackupResult>(request.context,
                                                                       kOperation,
                                                                       MakeSecurityContextRequiredDiagnostic(kOperation));
  }
  if (request.context.local_transaction_id == 0) {
    return MakeApiBehaviorDiagnostic<EngineRestoreLogicalBackupResult>(request.context,
                                                                       kOperation,
                                                                       BackupInvalid(kOperation, "local_transaction_id_required"));
  }
  const auto lifecycle =
      EvaluateBackupArchiveLifecycleAdmission(request, BackupArchiveLifecycleOperation::logical_restore);
  if (!lifecycle.admitted) {
    return MakeApiBehaviorDiagnostic<EngineRestoreLogicalBackupResult>(
        request.context, kOperation, lifecycle.diagnostic);
  }
  const auto path = BackupPath(request);
  std::string body;
  const auto read_status = ReadAndVerifyManifest(path, &body);
  if (read_status.error) {
    RecordArchiveRestoreRefusalMetric("logical_snapshot", read_status);
    return MakeApiBehaviorDiagnostic<EngineRestoreLogicalBackupResult>(request.context, kOperation, read_status);
  }

  std::vector<CrudTableRecord> tables;
  std::vector<CrudIndexRecord> indexes;
  std::vector<CrudRowVersionRecord> rows;
  EngineUuid backup_uuid;
  for (const auto& line : Split(body, '\n')) {
    if (line.empty() || line == kLogicalBackupMagic) { continue; }
    std::string kind;
    const auto fields = DecodeRecordFields(line, &kind);
    if (kind == "META") {
      auto it = fields.find("backup_uuid");
      if (it != fields.end()) { backup_uuid.canonical = it->second; }
    } else if (kind == "TABLE") {
      CrudTableRecord table;
      table.creator_tx = request.context.local_transaction_id;
      table.table_uuid = fields.at("table_uuid");
      table.default_name = fields.at("default_name");
      table.columns = DecodeCrudPairs(fields.at("columns"));
      tables.push_back(std::move(table));
    } else if (kind == "INDEX") {
      CrudIndexRecord index;
      index.creator_tx = request.context.local_transaction_id;
      index.index_uuid = fields.at("index_uuid");
      index.table_uuid = fields.at("table_uuid");
      index.column_name = fields.at("column_name");
      index.family = fields.at("family");
      index.profile = NormalizeCrudIndexProfile(fields.at("profile"));
      index.default_name = fields.at("default_name");
      index.key_envelopes = DecodeList(fields.at("key_envelopes"));
      index.include_columns = DecodeList(fields.at("include_columns"));
      index.predicate_kind = fields.at("predicate_kind");
      index.predicate_column = fields.at("predicate_column");
      index.predicate_value = fields.at("predicate_value");
      index.unique = fields.at("unique") == "1";
      indexes.push_back(std::move(index));
    } else if (kind == "ROW") {
      CrudRowVersionRecord row;
      row.creator_tx = request.context.local_transaction_id;
      row.table_uuid = fields.at("table_uuid");
      row.row_uuid = fields.at("row_uuid");
      row.version_uuid = fields.at("version_uuid");
      row.deleted = fields.at("deleted") == "1";
      row.values = DecodeCrudPairs(fields.at("values"));
      rows.push_back(std::move(row));
    }
  }

  for (const auto& table : tables) {
    const auto status = AppendMgaTableMetadata(request.context, table);
    if (status.error) {
      return MakeApiBehaviorDiagnostic<EngineRestoreLogicalBackupResult>(request.context, kOperation, status);
    }
  }
  for (const auto& index : indexes) {
    const auto status = AppendMgaIndexMetadata(request.context, index);
    if (status.error) {
      return MakeApiBehaviorDiagnostic<EngineRestoreLogicalBackupResult>(request.context, kOperation, status);
    }
  }
  for (const auto& row : rows) {
    CrudRowVersionRecord restored_row = row;
    restored_row.creator_tx = request.context.local_transaction_id;
    const auto row_status = AppendMgaRowVersion(request.context, restored_row, nullptr);
    if (row_status.error) {
      return MakeApiBehaviorDiagnostic<EngineRestoreLogicalBackupResult>(request.context, kOperation, row_status);
    }
    CrudState restore_state;
    restore_state.transactions[request.context.local_transaction_id] = "active";
    restore_state.tables = tables;
    restore_state.indexes = indexes;
    const auto index_status = AppendMgaIndexEntriesForRow(request.context,
                                                          restore_state,
                                                          row.table_uuid,
                                                          row.row_uuid,
                                                          row.version_uuid,
                                                          row.values);
    if (index_status.error) {
      return MakeApiBehaviorDiagnostic<EngineRestoreLogicalBackupResult>(request.context, kOperation, index_status);
    }
  }

  (void)scratchbird::core::metrics::PublishBackupInProgress(0.0, "logical_restore");
  (void)scratchbird::core::metrics::PublishBackupProgressPercent(100.0, "logical_restore");
  auto result = MakeApiBehaviorSuccess<EngineRestoreLogicalBackupResult>(request.context, kOperation);
  AddBackupLifecycleEvidence(&result, lifecycle);
  result.restore_uuid.canonical = GenerateCrudEngineUuid("restore");
  result.source_backup_uuid = backup_uuid;
  result.restored_table_count = tables.size();
  result.restored_row_count = rows.size();
  result.restored_index_count = indexes.size();
  result.source_manifest_uri = path;
  AddApiBehaviorEvidence(&result, "restore_manifest_validated", path);
  AddApiBehaviorEvidence(&result, "evidence_before_success", "logical_restore_applied");
  AddApiBehaviorEvidence(&result, "authoritative_wal", "false");
  AddApiBehaviorRow(&result, {{"restore_uuid", result.restore_uuid.canonical},
                              {"source_backup_uuid", result.source_backup_uuid.canonical},
                              {"source_manifest_uri", path},
                              {"tables", std::to_string(result.restored_table_count)},
                              {"rows", std::to_string(result.restored_row_count)},
                              {"indexes", std::to_string(result.restored_index_count)},
                              {"authoritative_wal", "false"}});
  return result;
}

std::string BuildDeltaManifestBody(const EnginePackageDeltaStreamRequest& request,
                                   const EngineUuid& delta_uuid,
                                   std::uint64_t start_tx,
                                   std::uint64_t end_tx,
                                   const std::vector<CrudTableRecord>& tables,
                                   const std::vector<CrudIndexRecord>& indexes,
                                   const std::vector<CrudRowVersionRecord>& rows,
                                   const CrudState& state) {
  std::ostringstream body;
  body << kDeltaPackageMagic << "\n";
  const auto filespace_uuid = RequiredManifestOption(request, "filespace_uuid:");
  const auto source_backup_uuid = RequiredManifestOption(request, "source_backup_uuid:");
  body << RecordLine("META", {{"delta_uuid", delta_uuid.canonical},
                               {"manifest_version", "1"},
                               {"source_backup_uuid", source_backup_uuid},
                               {"database_uuid", request.context.database_uuid.canonical},
                               {"filespace_uuid", filespace_uuid},
                               {"timeline_uuid", TimelineUuidFor(request)},
                               {"fork_uuid", ForkUuidFor(request)},
                               {"key_lineage_id", KeyLineageFor(request)},
                               {"start_transaction_id", std::to_string(start_tx)},
                               {"end_transaction_id", std::to_string(end_tx)},
                               {"coverage_start_transaction_id", std::to_string(start_tx)},
                               {"coverage_end_transaction_id", std::to_string(end_tx)},
                               {"coverage_contiguous", "true"},
                               {"coverage_gap_classification", "none"},
                               {"coverage_proof", std::to_string(Fnv1a64(delta_uuid.canonical + filespace_uuid + std::to_string(start_tx) + ":" + std::to_string(end_tx)))},
                               {"idempotency_key", OptionValue(request, "idempotency_key:")},
                               {"restore_point_name", OptionValue(request, "restore_point_name:")},
                               {"coverage_start_unix_micros", OptionValue(request, "coverage_start_unix_micros:")},
                               {"coverage_end_unix_micros", OptionValue(request, "coverage_end_unix_micros:")},
                               {"checksum_profile", "fnv1a64-manifest-body"},
                               {"signature_profile", "unsigned-local-manifest-proof-v1"},
                               {"replay_profile", "deterministic_mga_operation_envelopes"},
                               {"finality_source", "local_mga_transaction_inventory"},
                               {"delta_source", "mga_row_version_lineage"},
                               {"contains_committed_final_states_only", "true"},
                               {"authoritative_wal", "false"},
                               {"format", "write_after_delta_v1"}});
  for (const auto& table : tables) {
    body << RecordLine("TABLE", {{"creator_tx", std::to_string(table.creator_tx)},
                                  {"creator_transaction_state", state.transactions.count(table.creator_tx) == 0 ? "unknown" : state.transactions.at(table.creator_tx)},
                                  {"table_uuid", table.table_uuid},
                                  {"default_name", table.default_name},
                                  {"columns", EncodeCrudPairs(table.columns)}});
  }
  for (const auto& index : indexes) {
    body << RecordLine("INDEX", {{"creator_tx", std::to_string(index.creator_tx)},
                                  {"creator_transaction_state", state.transactions.count(index.creator_tx) == 0 ? "unknown" : state.transactions.at(index.creator_tx)},
                                  {"index_uuid", index.index_uuid},
                                  {"table_uuid", index.table_uuid},
                                  {"column_name", index.column_name},
                                  {"family", index.family},
                                  {"profile", index.profile},
                                  {"default_name", index.default_name},
                                  {"key_envelopes", EncodeList(index.key_envelopes)},
                                  {"include_columns", EncodeList(index.include_columns)},
                                  {"predicate_kind", index.predicate_kind},
                                  {"predicate_column", index.predicate_column},
                                  {"predicate_value", index.predicate_value},
                                  {"unique", index.unique ? "1" : "0"}});
  }
  for (const auto& row : rows) {
    const std::string lineage_material = row.table_uuid + "|" + row.row_uuid + "|" + row.version_uuid + "|" +
                                         row.previous_version_uuid + "|" + std::to_string(row.creator_tx) + "|" +
                                         std::to_string(row.sequence) + "|" + std::to_string(row.deleted ? 1 : 0);
    body << RecordLine("ROW", {{"creator_tx", std::to_string(row.creator_tx)},
                                {"creator_transaction_state", state.transactions.count(row.creator_tx) == 0 ? "unknown" : state.transactions.at(row.creator_tx)},
                                {"event_sequence", std::to_string(row.event_sequence)},
                                {"sequence", std::to_string(row.sequence)},
                                {"table_uuid", row.table_uuid},
                                {"row_uuid", row.row_uuid},
                                {"version_uuid", row.version_uuid},
                                {"previous_version_uuid", row.previous_version_uuid},
                                {"previous_sequence", std::to_string(row.previous_sequence)},
                                {"lineage_checksum", std::to_string(Fnv1a64(lineage_material))},
                                {"deleted", row.deleted ? "1" : "0"},
                                {"values", EncodeCrudPairs(row.values)}});
  }
  return body.str();
}

EngineApiDiagnostic ReadAndVerifyDeltaManifest(const std::string& path,
                                               std::string* body,
                                               std::map<std::string, std::string>* meta_fields) {
  bool ok = false;
  const auto manifest = ReadBinaryFile(path, &ok);
  if (!ok) { return BackupInvalid("backup_archive.apply_delta_stream", "BACKUP_DELTA_REQUEST_INVALID"); }
  const auto checksum_pos = manifest.rfind("CHECKSUM\t");
  if (checksum_pos == std::string::npos) {
    return BackupInvalid("backup_archive.apply_delta_stream", "RESTORE_MANIFEST_CHECKSUM_MISSING");
  }
  const auto manifest_body = manifest.substr(0, checksum_pos);
  const auto checksum_parts = Split(manifest.substr(checksum_pos), '\t');
  if (checksum_parts.size() < 2 || ParseU64(checksum_parts[1]) != Fnv1a64(manifest_body)) {
    return BackupInvalid("backup_archive.apply_delta_stream", "RESTORE_MANIFEST_CHECKSUM_INVALID");
  }
  if (!StartsWith(manifest_body, std::string(kDeltaPackageMagic) + "\n")) {
    return BackupInvalid("backup_archive.apply_delta_stream", "RESTORE_MANIFEST_MAGIC_INVALID");
  }
  bool meta_found = false;
  for (const auto& line : Split(manifest_body, '\n')) {
    if (line.empty() || line == kDeltaPackageMagic) { continue; }
    std::string kind;
    const auto fields = DecodeRecordFields(line, &kind);
    if (kind != "META") { continue; }
    meta_found = true;
    const auto proof = ValidateManifestProofFields("backup_archive.apply_delta_stream",
                                                   fields,
                                                   true);
    if (proof.error) { return proof; }
    if (meta_fields != nullptr) { *meta_fields = fields; }
  }
  if (!meta_found) {
    return BackupInvalid("backup_archive.apply_delta_stream", "RESTORE_MANIFEST_META_MISSING");
  }
  if (body != nullptr) { *body = manifest_body; }
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

std::map<std::string, std::string> ExtractManifestMetaFields(
    const std::string& body,
    const std::string& magic) {
  for (const auto& line : Split(body, '\n')) {
    if (line.empty() || line == magic) { continue; }
    std::string kind;
    const auto fields = DecodeRecordFields(line, &kind);
    if (kind == "META") {
      return fields;
    }
  }
  return {};
}

struct VerifiedCoverageSegment {
  std::string manifest_uri;
  std::uint64_t coverage_start = 0;
  std::uint64_t coverage_end = 0;
  std::string source_backup_uuid;
  std::string filespace_uuid;
  std::string idempotency_key;
};

EngineUpdateBackupFromVerifiedCoverageResult
EngineUpdateBackupFromVerifiedCoverage(
    const EngineUpdateBackupFromVerifiedCoverageRequest& request) {
  constexpr const char* kOperation =
      "backup_archive.update_backup_from_verified_coverage";

  auto fail = [&](EngineApiDiagnostic diagnostic) {
    auto result =
        MakeApiBehaviorDiagnostic<EngineUpdateBackupFromVerifiedCoverageResult>(
            request.context, kOperation, std::move(diagnostic));
    result.backup_uuid = request.backup_uuid;
    result.base_manifest_uri = request.base_manifest_uri;
    result.update_manifest_uri = request.update_manifest_uri;
    result.target_end_transaction_id = request.target_end_transaction_id;
    result.transaction_finality_authority = false;
    result.write_after_recovery_authority = false;
    result.cluster_recovery_authority = false;
    AddApiBehaviorEvidence(&result, "backup_update_authorized", "false");
    AddApiBehaviorEvidence(&result, "finality_source",
                           "local_mga_transaction_inventory");
    AddApiBehaviorEvidence(&result, "transaction_finality_authority", "false");
    AddApiBehaviorEvidence(&result, "write_after_recovery_authority", "false");
    AddApiBehaviorEvidence(&result, "cluster_recovery_authority", "false");
    AddApiBehaviorEvidence(&result, "authoritative_wal", "false");
    return result;
  };

  if (!HasBackupCreateRight(request.context)) {
    return fail(MakeSecurityContextRequiredDiagnostic(kOperation));
  }
  if (request.context.database_uuid.canonical.empty() ||
      request.context.database_path.empty()) {
    return fail(BackupInvalid(kOperation,
                              "BACKUP_UPDATE_DATABASE_CONTEXT_REQUIRED"));
  }
  if (OptionValue(request, "scope:") == "cluster" ||
      OptionBool(request, "cluster_recovery_authority:", false)) {
    auto result = fail(BackupInvalid(kOperation,
                                     "BACKUP_UPDATE_CLUSTER_PROVIDER_REQUIRED"));
    result.cluster_authority_required = true;
    return result;
  }
  if (OptionBool(request, "authoritative_wal:", false)) {
    return fail(BackupInvalid(kOperation,
                              "BACKUP_UPDATE_AUTHORITATIVE_WAL_FORBIDDEN"));  // no_wal refusal evidence
  }
  if (request.backup_uuid.canonical.empty() ||
      request.base_manifest_uri.empty() ||
      request.filespace_uuid.empty() ||
      request.target_end_transaction_id == 0) {
    return fail(BackupInvalid(kOperation,
                              "BACKUP_UPDATE_REQUEST_PROOF_REQUIRED"));
  }

  std::string base_body;
  const auto base_status =
      ReadAndVerifyManifest(request.base_manifest_uri, &base_body);
  if (base_status.error) {
    return fail(base_status);
  }
  const auto base_meta =
      ExtractManifestMetaFields(base_body, kLogicalBackupMagic);
  const auto base_backup_uuid = ManifestField(base_meta, "backup_uuid");
  if (base_backup_uuid != request.backup_uuid.canonical) {
    return fail(BackupInvalid(kOperation,
                              "BACKUP_UPDATE_BASE_BACKUP_UUID_MISMATCH"));
  }
  if (ManifestField(base_meta, "filespace_uuid") != request.filespace_uuid) {
    return fail(BackupInvalid(kOperation,
                              "BACKUP_UPDATE_FILESPACE_MISMATCH"));
  }

  std::uint64_t current_coverage_end =
      ParseU64(ManifestField(base_meta, "coverage_end_transaction_id"));
  const std::uint64_t base_coverage_end = current_coverage_end;
  std::vector<VerifiedCoverageSegment> segments;
  for (const auto& manifest_uri : request.verified_segment_manifest_uris) {
    std::map<std::string, std::string> meta;
    const auto segment_status =
        ReadAndVerifyDeltaManifest(manifest_uri, nullptr, &meta);
    if (segment_status.error) {
      return fail(segment_status);
    }
    VerifiedCoverageSegment segment;
    segment.manifest_uri = manifest_uri;
    segment.coverage_start =
        ParseU64(ManifestField(meta, "coverage_start_transaction_id"));
    segment.coverage_end =
        ParseU64(ManifestField(meta, "coverage_end_transaction_id"));
    segment.source_backup_uuid = ManifestField(meta, "source_backup_uuid");
    segment.filespace_uuid = ManifestField(meta, "filespace_uuid");
    segment.idempotency_key = ManifestField(meta, "idempotency_key");
    if (segment.source_backup_uuid != request.backup_uuid.canonical) {
      return fail(BackupInvalid(kOperation,
                                "BACKUP_UPDATE_SEGMENT_BACKUP_UUID_MISMATCH"));
    }
    if (segment.filespace_uuid != request.filespace_uuid) {
      return fail(BackupInvalid(kOperation,
                                "BACKUP_UPDATE_SEGMENT_FILESPACE_MISMATCH"));
    }
    if (!request.idempotency_key.empty() &&
        !segment.idempotency_key.empty() &&
        segment.idempotency_key != request.idempotency_key) {
      return fail(BackupInvalid(kOperation,
                                "BACKUP_UPDATE_IDEMPOTENCY_KEY_MISMATCH"));
    }
    segments.push_back(std::move(segment));
  }

  std::sort(segments.begin(), segments.end(), [](const auto& left,
                                                 const auto& right) {
    if (left.coverage_start != right.coverage_start) {
      return left.coverage_start < right.coverage_start;
    }
    return left.coverage_end < right.coverage_end;
  });

  for (const auto& segment : segments) {
    if (segment.coverage_start <= current_coverage_end) {
      auto result = fail(BackupInvalid(kOperation,
                                       "BACKUP_UPDATE_COVERAGE_OVERLAP"));
      result.overlap_detected = true;
      return result;
    }
    if (segment.coverage_start != current_coverage_end + 1) {
      auto result = fail(BackupInvalid(kOperation,
                                       "BACKUP_UPDATE_COVERAGE_GAP"));
      result.gap_detected = true;
      return result;
    }
    current_coverage_end = segment.coverage_end;
  }

  if (request.last_covered_transaction_id != current_coverage_end) {
    return fail(BackupInvalid(kOperation,
                              "BACKUP_UPDATE_LAST_COVERED_MISMATCH"));
  }

  auto result =
      MakeApiBehaviorSuccess<EngineUpdateBackupFromVerifiedCoverageResult>(
          request.context, kOperation);
  result.update_uuid.canonical = GenerateCrudEngineUuid("backup_update");
  result.backup_uuid = request.backup_uuid;
  result.base_coverage_end_transaction_id = base_coverage_end;
  result.reused_coverage_end_transaction_id = current_coverage_end;
  result.target_end_transaction_id = request.target_end_transaction_id;
  result.reused_segment_count =
      static_cast<EngineApiU64>(segments.size());
  result.base_manifest_uri = request.base_manifest_uri;
  result.historical_coverage_reused = true;
  result.coverage_contiguous = true;
  result.transaction_finality_authority = false;
  result.write_after_recovery_authority = false;
  result.cluster_recovery_authority = false;

  if (request.target_end_transaction_id <= current_coverage_end) {
    result.idempotent_noop = true;
    AddApiBehaviorEvidence(&result, "backup_update_idempotent_noop", "true");
  } else {
    if (request.update_manifest_uri.empty()) {
      return fail(BackupInvalid(kOperation,
                                "BACKUP_UPDATE_TARGET_MANIFEST_REQUIRED"));
    }
    EnginePackageDeltaStreamRequest package;
    package.context = request.context;
    package.option_envelopes = {
        "target_uri:" + request.update_manifest_uri,
        "source_backup_uuid:" + request.backup_uuid.canonical,
        "filespace_uuid:" + request.filespace_uuid,
        "start_transaction_id:" + std::to_string(current_coverage_end + 1),
        "end_transaction_id:" +
            std::to_string(request.target_end_transaction_id),
        "coverage_start_transaction_id:" +
            std::to_string(current_coverage_end + 1),
        "coverage_end_transaction_id:" +
            std::to_string(request.target_end_transaction_id),
        "idempotency_key:" + request.idempotency_key};
    const auto packaged = EnginePackageDeltaStream(package);
    if (!packaged.ok) {
      EngineApiDiagnostic diagnostic =
          packaged.diagnostics.empty()
              ? BackupInvalid(kOperation, "BACKUP_UPDATE_PACKAGE_FAILED")
              : packaged.diagnostics.front();
      return fail(std::move(diagnostic));
    }
    result.update_manifest_uri = request.update_manifest_uri;
    result.packaged_start_transaction_id = packaged.start_transaction_id;
    result.packaged_end_transaction_id = packaged.end_transaction_id;
    result.packaged_row_count = packaged.row_count;
    result.packaged_table_count = packaged.table_count;
    result.reused_coverage_end_transaction_id =
        request.target_end_transaction_id;
  }

  AddApiBehaviorEvidence(&result, "backup_update_uuid",
                         result.update_uuid.canonical);
  AddApiBehaviorEvidence(&result, "base_backup_uuid",
                         request.backup_uuid.canonical);
  AddApiBehaviorEvidence(&result, "base_manifest_uri",
                         request.base_manifest_uri);
  AddApiBehaviorEvidence(&result, "historical_coverage_reused", "true");
  AddApiBehaviorEvidence(&result, "coverage_contiguous", "true");
  AddApiBehaviorEvidence(&result, "idempotent_noop",
                         result.idempotent_noop ? "true" : "false");
  AddApiBehaviorEvidence(&result, "reused_segment_count",
                         std::to_string(result.reused_segment_count));
  AddApiBehaviorEvidence(&result, "finality_source",
                         "local_mga_transaction_inventory");
  AddApiBehaviorEvidence(&result, "transaction_finality_authority", "false");
  AddApiBehaviorEvidence(&result, "write_after_recovery_authority", "false");
  AddApiBehaviorEvidence(&result, "cluster_recovery_authority", "false");
  AddApiBehaviorEvidence(&result, "authoritative_wal", "false");
  AddApiBehaviorRow(
      &result,
      {{"update_uuid", result.update_uuid.canonical},
       {"backup_uuid", request.backup_uuid.canonical},
       {"base_coverage_end_transaction_id",
        std::to_string(base_coverage_end)},
       {"reused_coverage_end_transaction_id",
        std::to_string(result.reused_coverage_end_transaction_id)},
       {"target_end_transaction_id",
        std::to_string(request.target_end_transaction_id)},
       {"reused_segment_count", std::to_string(result.reused_segment_count)},
       {"packaged_start_transaction_id",
        std::to_string(result.packaged_start_transaction_id)},
       {"packaged_end_transaction_id",
        std::to_string(result.packaged_end_transaction_id)},
       {"historical_coverage_reused", "true"},
       {"idempotent_noop", result.idempotent_noop ? "true" : "false"},
       {"coverage_contiguous", "true"},
       {"finality_source", "local_mga_transaction_inventory"},
       {"transaction_finality_authority", "false"},
       {"write_after_recovery_authority", "false"},
       {"cluster_recovery_authority", "false"},
       {"authoritative_wal", "false"}});
  return result;
}

EnginePackageDeltaStreamResult EnginePackageDeltaStream(const EnginePackageDeltaStreamRequest& request) {
  constexpr const char* kOperation = "backup_archive.package_delta_stream";
  if (!HasBackupCreateRight(request.context)) {
    return MakeApiBehaviorDiagnostic<EnginePackageDeltaStreamResult>(request.context,
                                                                     kOperation,
                                                                     MakeSecurityContextRequiredDiagnostic(kOperation));
  }
  if (OptionValue(request, "authoritative_wal:") == "true") {
    return MakeApiBehaviorDiagnostic<EnginePackageDeltaStreamResult>(request.context,
                                                                     kOperation,
                                                                     BackupInvalid(kOperation, "BACKUP_AUTHORITATIVE_WAL_FORBIDDEN"));
  }
  const auto lifecycle =
      EvaluateBackupArchiveLifecycleAdmission(request, BackupArchiveLifecycleOperation::delta_package);
  if (!lifecycle.admitted) {
    return MakeApiBehaviorDiagnostic<EnginePackageDeltaStreamResult>(
        request.context, kOperation, lifecycle.diagnostic);
  }
  const auto path = BackupPath(request);
  if (path.empty()) {
    return MakeApiBehaviorDiagnostic<EnginePackageDeltaStreamResult>(request.context,
                                                                     kOperation,
                                                                     BackupInvalid(kOperation, "BACKUP_TARGET_UNAVAILABLE"));
  }
  if (RequiredManifestOption(request, "filespace_uuid:").empty() ||
      RequiredManifestOption(request, "source_backup_uuid:").empty()) {
    return MakeApiBehaviorDiagnostic<EnginePackageDeltaStreamResult>(
        request.context,
        kOperation,
        BackupInvalid(kOperation, "BACKUP_DELTA_COVERAGE_PROOF_REQUIRED"));
  }
  const auto loaded_mga = LoadMgaRelationStoreState(request.context);
  if (!loaded_mga.ok) { return MakeApiBehaviorDiagnostic<EnginePackageDeltaStreamResult>(request.context, kOperation, loaded_mga.diagnostic); }
  const CrudState loaded_state = BuildCrudCompatibilityStateFromMga(loaded_mga.state);
  const auto start_tx = ParseU64(OptionValue(request, "start_transaction_id:"));
  const auto requested_end = ParseU64(OptionValue(request, "end_transaction_id:"));
  const auto end_tx = requested_end == 0 ? MaxCommittedTransaction(loaded_state) : requested_end;
  if (end_tx < start_tx) {
    return MakeApiBehaviorDiagnostic<EnginePackageDeltaStreamResult>(
        request.context,
        kOperation,
        BackupInvalid(kOperation, "BACKUP_DELTA_FINALITY_RANGE_INVALID"));
  }
  const auto unsafe_gap = UnsafeFinalityGap(loaded_state, end_tx);
  if (!unsafe_gap.empty()) {
    return MakeApiBehaviorDiagnostic<EnginePackageDeltaStreamResult>(
        request.context,
        kOperation,
        BackupInvalid(kOperation, "BACKUP_DELTA_FINALITY_GAP:" + unsafe_gap));
  }
  std::vector<CrudTableRecord> tables;
  std::vector<CrudIndexRecord> indexes;
  std::vector<CrudRowVersionRecord> rows;
  const auto temporary_table_uuids =
      TemporaryTableUuidsVisibleThrough(loaded_state, end_tx);
  const auto temporary_exclusions =
      CountTemporaryDeltaExclusions(loaded_state, start_tx, end_tx);
  for (const auto& table : loaded_state.tables) {
    if (table.temporary) { continue; }
    if (table.creator_tx >= start_tx && table.creator_tx <= end_tx &&
        CrudCreatorVisible(loaded_state, table.creator_tx, table.event_sequence, end_tx)) {
      tables.push_back(table);
    }
  }
  for (const auto& index : loaded_state.indexes) {
    if (ContainsUuid(temporary_table_uuids, index.table_uuid)) { continue; }
    if (index.creator_tx >= start_tx && index.creator_tx <= end_tx &&
        CrudCreatorVisible(loaded_state, index.creator_tx, index.event_sequence, end_tx)) {
      indexes.push_back(index);
    }
  }
  for (const auto& row : loaded_state.row_versions) {
    if (ContainsUuid(temporary_table_uuids, row.table_uuid)) { continue; }
    if (row.creator_tx >= start_tx && row.creator_tx <= end_tx &&
        CrudCreatorVisible(loaded_state, row.creator_tx, row.event_sequence, end_tx)) {
      rows.push_back(row);
    }
  }
  EngineUuid delta_uuid;
  delta_uuid.canonical = GenerateCrudEngineUuid("delta");
  const auto body = BuildDeltaManifestBody(request, delta_uuid, start_tx, end_tx, tables, indexes, rows, loaded_state);
  const auto checksum = Fnv1a64(body);
  if (!WriteBinaryFile(path, body + "CHECKSUM\t" + std::to_string(checksum) + "\n")) {
    return MakeApiBehaviorDiagnostic<EnginePackageDeltaStreamResult>(request.context,
                                                                     kOperation,
                                                                     BackupInvalid(kOperation, "BACKUP_TARGET_UNAVAILABLE"));
  }
  (void)scratchbird::core::metrics::PublishPitrWindowAvailableSeconds(static_cast<double>(end_tx >= start_tx ? end_tx - start_tx : 0), "local_delta");
  const auto latest_committed = MaxCommittedTransaction(loaded_state);
  const auto lag_transactions = latest_committed > end_tx ? latest_committed - end_tx : 0;
  const std::string delta_payload = body + "CHECKSUM\t" + std::to_string(checksum) + "\n";
  (void)scratchbird::core::metrics::PublishArchiveLagBytes(0.0, "local_delta", "current");
  (void)scratchbird::core::metrics::PublishArchiveDeltaLagTransactions(static_cast<double>(lag_transactions), "local_delta", "package");
  PublishArchiveSliceCreatedMetrics(request, "local_delta", "created", delta_payload.size());
  auto result = MakeApiBehaviorSuccess<EnginePackageDeltaStreamResult>(request.context, kOperation);
  AddBackupLifecycleEvidence(&result, lifecycle);
  result.delta_uuid = delta_uuid;
  result.start_transaction_id = start_tx;
  result.end_transaction_id = end_tx;
  result.row_count = rows.size();
  result.table_count = tables.size();
  result.delta_manifest_uri = path;
  AddApiBehaviorEvidence(&result, "delta_manifest", path);
  AddApiBehaviorEvidence(&result, "authoritative_wal", "false");
  AddApiBehaviorEvidence(&result, "contains_committed_final_states_only", "true");
  AddApiBehaviorEvidence(&result, "finality_interval", std::to_string(start_tx) + ".." + std::to_string(end_tx));
  AddApiBehaviorEvidence(&result, "delta_source", "mga_row_version_lineage");
  AddApiBehaviorEvidence(&result, "archive_slice_bytes", std::to_string(delta_payload.size()));
  AddApiBehaviorEvidence(&result, "archive_retention_max_age_microseconds", std::to_string(ArchiveMaxAgeMicroseconds(request)));
  AddTemporaryBackupExclusionEvidence(&result, temporary_exclusions);
  AddApiBehaviorRow(&result, {{"delta_uuid", delta_uuid.canonical},
                              {"delta_manifest_uri", path},
                              {"start_transaction_id", std::to_string(start_tx)},
                              {"end_transaction_id", std::to_string(end_tx)},
                              {"delta_source", "mga_row_version_lineage"},
                              {"rows", std::to_string(result.row_count)},
                              {"tables", std::to_string(result.table_count)},
                              {"temporary_content_excluded", "true"},
                              {"temporary_tables_excluded", std::to_string(temporary_exclusions.table_count)},
                              {"temporary_rows_excluded", std::to_string(temporary_exclusions.row_count)},
                              {"temporary_indexes_excluded", std::to_string(temporary_exclusions.index_count)},
                              {"authoritative_wal", "false"}});
  return result;
}

EngineApplyDeltaStreamResult EngineApplyDeltaStream(const EngineApplyDeltaStreamRequest& request) {
  constexpr const char* kOperation = "backup_archive.apply_delta_stream";
  if (!HasBackupRestoreRight(request.context)) {
    return MakeApiBehaviorDiagnostic<EngineApplyDeltaStreamResult>(request.context,
                                                                   kOperation,
                                                                   MakeSecurityContextRequiredDiagnostic(kOperation));
  }
  if (request.context.local_transaction_id == 0) {
    return MakeApiBehaviorDiagnostic<EngineApplyDeltaStreamResult>(request.context,
                                                                   kOperation,
                                                                   BackupInvalid(kOperation, "local_transaction_id_required"));
  }
  const auto lifecycle =
      EvaluateBackupArchiveLifecycleAdmission(request, BackupArchiveLifecycleOperation::delta_apply);
  if (!lifecycle.admitted) {
    return MakeApiBehaviorDiagnostic<EngineApplyDeltaStreamResult>(
        request.context, kOperation, lifecycle.diagnostic);
  }
  const auto path = BackupPath(request);
  std::string body;
  std::map<std::string, std::string> delta_meta;
  const auto status = ReadAndVerifyDeltaManifest(path, &body, &delta_meta);
  if (status.error) {
    RecordArchiveRestoreRefusalMetric("local_delta", status);
    return MakeApiBehaviorDiagnostic<EngineApplyDeltaStreamResult>(request.context, kOperation, status);
  }
  const auto expected_previous_end = ParseU64(OptionValue(request, "expected_previous_end_transaction_id:"));
  const auto coverage_start = ParseU64(ManifestField(delta_meta, "coverage_start_transaction_id"));
  const auto coverage_end = ParseU64(ManifestField(delta_meta, "coverage_end_transaction_id"));
  if (expected_previous_end != 0 && coverage_start != expected_previous_end + 1) {
    const auto gap = BackupInvalid(kOperation, "BACKUP_DELTA_COVERAGE_GAP:expected_start_" +
                                                 std::to_string(expected_previous_end + 1) +
                                                 "_actual_" + std::to_string(coverage_start));
    RecordArchiveRestoreRefusalMetric("local_delta", gap);
    return MakeApiBehaviorDiagnostic<EngineApplyDeltaStreamResult>(request.context, kOperation, gap);
  }
  std::uint64_t pitr_target_transaction_id = 0;
  std::string pitr_restore_point_name = OptionValue(request, "restore_point_name:");
  const auto pitr_selector = OptionValue(request, "pitr_target_selector:");
  if (pitr_selector == "latest-valid") {
    pitr_target_transaction_id = coverage_end;
  }
  const auto requested_pitr_tx = ParseU64(OptionValue(request, "pitr_target_transaction_id:"));
  if (requested_pitr_tx != 0) {
    pitr_target_transaction_id = requested_pitr_tx;
  }
  const auto requested_pitr_timestamp = ParseU64(OptionValue(request, "pitr_target_unix_micros:"));
  if (requested_pitr_timestamp != 0) {
    const auto coverage_start_micros = ParseU64(ManifestField(delta_meta, "coverage_start_unix_micros"));
    const auto coverage_end_micros = ParseU64(ManifestField(delta_meta, "coverage_end_unix_micros"));
    if (coverage_start_micros == 0 || coverage_end_micros == 0 ||
        requested_pitr_timestamp < coverage_start_micros ||
        requested_pitr_timestamp > coverage_end_micros) {
      const auto pitr_gap = BackupInvalid(kOperation, "PITR_TARGET_OUTSIDE_COVERAGE:timestamp");
      RecordArchiveRestoreRefusalMetric("local_delta", pitr_gap);
      return MakeApiBehaviorDiagnostic<EngineApplyDeltaStreamResult>(request.context, kOperation, pitr_gap);
    }
    pitr_target_transaction_id = coverage_end;
  }
  if (!pitr_restore_point_name.empty() &&
      pitr_restore_point_name != ManifestField(delta_meta, "restore_point_name")) {
    const auto pitr_gap = BackupInvalid(kOperation, "PITR_TARGET_OUTSIDE_COVERAGE:restore_point_name");
    RecordArchiveRestoreRefusalMetric("local_delta", pitr_gap);
    return MakeApiBehaviorDiagnostic<EngineApplyDeltaStreamResult>(request.context, kOperation, pitr_gap);
  }
  if (pitr_target_transaction_id != 0 &&
      (pitr_target_transaction_id < coverage_start || pitr_target_transaction_id > coverage_end)) {
    const auto pitr_gap = BackupInvalid(kOperation, "PITR_TARGET_OUTSIDE_COVERAGE:transaction");
    RecordArchiveRestoreRefusalMetric("local_delta", pitr_gap);
    return MakeApiBehaviorDiagnostic<EngineApplyDeltaStreamResult>(request.context, kOperation, pitr_gap);
  }
  EngineUuid delta_uuid;
  std::uint64_t start_tx = 0;
  std::uint64_t end_tx = 0;
  std::vector<CrudTableRecord> tables;
  std::vector<CrudIndexRecord> indexes;
  std::vector<CrudRowVersionRecord> rows;
  for (const auto& line : Split(body, '\n')) {
    if (line.empty() || line == kDeltaPackageMagic) { continue; }
    std::string kind;
    const auto fields = DecodeRecordFields(line, &kind);
    if (kind == "META") {
      delta_uuid.canonical = fields.at("delta_uuid");
      start_tx = ParseU64(fields.at("start_transaction_id"));
      end_tx = ParseU64(fields.at("end_transaction_id"));
    } else if (kind == "TABLE") {
      CrudTableRecord table;
      table.creator_tx = request.context.local_transaction_id;
      table.table_uuid = fields.at("table_uuid");
      table.default_name = fields.at("default_name");
      table.columns = DecodeCrudPairs(fields.at("columns"));
      tables.push_back(std::move(table));
    } else if (kind == "INDEX") {
      CrudIndexRecord index;
      index.creator_tx = request.context.local_transaction_id;
      index.index_uuid = fields.at("index_uuid");
      index.table_uuid = fields.at("table_uuid");
      index.column_name = fields.at("column_name");
      index.family = fields.at("family");
      index.profile = NormalizeCrudIndexProfile(fields.at("profile"));
      index.default_name = fields.at("default_name");
      index.key_envelopes = DecodeList(fields.at("key_envelopes"));
      index.include_columns = DecodeList(fields.at("include_columns"));
      index.predicate_kind = fields.at("predicate_kind");
      index.predicate_column = fields.at("predicate_column");
      index.predicate_value = fields.at("predicate_value");
      index.unique = fields.at("unique") == "1";
      indexes.push_back(std::move(index));
    } else if (kind == "ROW") {
      CrudRowVersionRecord row;
      row.creator_tx = request.context.local_transaction_id;
      row.table_uuid = fields.at("table_uuid");
      row.row_uuid = fields.at("row_uuid");
      row.version_uuid = fields.at("version_uuid");
      row.deleted = fields.at("deleted") == "1";
      row.values = DecodeCrudPairs(fields.at("values"));
      rows.push_back(std::move(row));
    }
  }
  if (pitr_target_transaction_id != 0 && tables.empty() && indexes.empty() && rows.empty()) {
    auto result = MakeApiBehaviorSuccess<EngineApplyDeltaStreamResult>(request.context, kOperation);
    AddBackupLifecycleEvidence(&result, lifecycle);
    result.apply_uuid.canonical = GenerateCrudEngineUuid("delta_apply");
    result.delta_uuid = delta_uuid;
    result.pitr_target_transaction_id = pitr_target_transaction_id;
    result.pitr_restore_point_name = pitr_restore_point_name;
    result.delta_manifest_uri = path;
    AddApiBehaviorEvidence(&result, "delta_manifest_validated", path);
    AddApiBehaviorEvidence(&result, "evidence_before_success", "pitr_rollforward_target_selected");
    AddApiBehaviorEvidence(&result, "coverage_proof", ManifestField(delta_meta, "coverage_proof"));
    AddApiBehaviorEvidence(&result, "pitr_rollforward_profile", ManifestField(delta_meta, "replay_profile"));
    AddApiBehaviorEvidence(&result, "pitr_target_transaction_id", std::to_string(pitr_target_transaction_id));
    AddApiBehaviorEvidence(&result, "authoritative_wal", "false");
    AddApiBehaviorRow(&result, {{"apply_uuid", result.apply_uuid.canonical},
                                {"delta_uuid", delta_uuid.canonical},
                                {"delta_manifest_uri", path},
                                {"start_transaction_id", std::to_string(start_tx)},
                                {"end_transaction_id", std::to_string(end_tx)},
                                {"pitr_target_transaction_id", std::to_string(pitr_target_transaction_id)},
                                {"rows", "0"},
                                {"tables", "0"},
                                {"authoritative_wal", "false"}});
    return result;
  }
  const auto loaded_target_mga = LoadMgaRelationStoreState(request.context);
  if (!loaded_target_mga.ok) {
    return MakeApiBehaviorDiagnostic<EngineApplyDeltaStreamResult>(request.context, kOperation, loaded_target_mga.diagnostic);
  }
  const CrudState target_state = BuildCrudCompatibilityStateFromMga(loaded_target_mga.state);
  auto table_exists = [&](const std::string& table_uuid) {
    return std::any_of(target_state.tables.begin(), target_state.tables.end(), [&](const auto& existing) {
      return existing.table_uuid == table_uuid;
    });
  };
  auto index_exists = [&](const std::string& index_uuid) {
    return std::any_of(target_state.indexes.begin(), target_state.indexes.end(), [&](const auto& existing) {
      return existing.index_uuid == index_uuid;
    });
  };
  auto row_version_exists = [&](const std::string& row_uuid, const std::string& version_uuid) {
    return std::any_of(target_state.row_versions.begin(), target_state.row_versions.end(), [&](const auto& existing) {
      return existing.row_uuid == row_uuid && existing.version_uuid == version_uuid;
    });
  };
  std::vector<CrudTableRecord> tables_to_apply;
  for (const auto& table : tables) {
    if (!table_exists(table.table_uuid)) { tables_to_apply.push_back(table); }
  }
  std::vector<CrudIndexRecord> indexes_to_apply;
  for (const auto& index : indexes) {
    if (!index_exists(index.index_uuid)) { indexes_to_apply.push_back(index); }
  }
  std::vector<CrudRowVersionRecord> rows_to_apply;
  for (const auto& row : rows) {
    if (!row_version_exists(row.row_uuid, row.version_uuid)) { rows_to_apply.push_back(row); }
  }
  for (const auto& table : tables_to_apply) {
    const auto table_status = AppendMgaTableMetadata(request.context, table);
    if (table_status.error) { return MakeApiBehaviorDiagnostic<EngineApplyDeltaStreamResult>(request.context, kOperation, table_status); }
  }
  for (const auto& index : indexes_to_apply) {
    const auto index_status = AppendMgaIndexMetadata(request.context, index);
    if (index_status.error) { return MakeApiBehaviorDiagnostic<EngineApplyDeltaStreamResult>(request.context, kOperation, index_status); }
  }
  for (const auto& row : rows_to_apply) {
    CrudRowVersionRecord delta_row = row;
    delta_row.creator_tx = request.context.local_transaction_id;
    const auto row_status = AppendMgaRowVersion(request.context, delta_row, nullptr);
    if (row_status.error) { return MakeApiBehaviorDiagnostic<EngineApplyDeltaStreamResult>(request.context, kOperation, row_status); }
    CrudState delta_state;
    delta_state.transactions[request.context.local_transaction_id] = "active";
    delta_state.tables = tables_to_apply;
    delta_state.indexes = indexes_to_apply;
    const auto index_status = AppendMgaIndexEntriesForRow(request.context,
                                                          delta_state,
                                                          row.table_uuid,
                                                          row.row_uuid,
                                                          row.version_uuid,
                                                          row.values);
    if (index_status.error) { return MakeApiBehaviorDiagnostic<EngineApplyDeltaStreamResult>(request.context, kOperation, index_status); }
  }
  auto result = MakeApiBehaviorSuccess<EngineApplyDeltaStreamResult>(request.context, kOperation);
  AddBackupLifecycleEvidence(&result, lifecycle);
  (void)scratchbird::core::metrics::PublishArchiveDeltaApplyLagTransactions(0.0, "local_delta", "applied");
  (void)scratchbird::core::metrics::PublishArchiveHealthState(1.0, "healthy", "local_delta");
  result.apply_uuid.canonical = GenerateCrudEngineUuid("delta_apply");
  result.delta_uuid = delta_uuid;
  result.pitr_target_transaction_id = pitr_target_transaction_id;
  result.pitr_restore_point_name = pitr_restore_point_name;
  result.applied_row_count = rows_to_apply.size();
  result.applied_table_count = tables_to_apply.size();
  result.delta_manifest_uri = path;
  AddApiBehaviorEvidence(&result, "delta_manifest_validated", path);
  AddApiBehaviorEvidence(&result, "evidence_before_success", "delta_applied");
  AddApiBehaviorEvidence(&result, "idempotency_key", delta_uuid.canonical + ":" + std::to_string(start_tx) + ".." + std::to_string(end_tx));
  AddApiBehaviorEvidence(&result, "already_applied_rows", std::to_string(rows.size() - rows_to_apply.size()));
  AddApiBehaviorEvidence(&result, "coverage_proof", ManifestField(delta_meta, "coverage_proof"));
  AddApiBehaviorEvidence(&result, "pitr_rollforward_profile", ManifestField(delta_meta, "replay_profile"));
  AddApiBehaviorEvidence(&result, "authoritative_wal", "false");
  if (pitr_target_transaction_id != 0) {
    AddApiBehaviorEvidence(&result, "pitr_target_transaction_id", std::to_string(pitr_target_transaction_id));
  }
  AddApiBehaviorRow(&result, {{"apply_uuid", result.apply_uuid.canonical},
                              {"delta_uuid", delta_uuid.canonical},
                              {"delta_manifest_uri", path},
                              {"start_transaction_id", std::to_string(start_tx)},
                              {"end_transaction_id", std::to_string(end_tx)},
                              {"pitr_target_transaction_id", std::to_string(pitr_target_transaction_id)},
                              {"rows", std::to_string(result.applied_row_count)},
                              {"tables", std::to_string(result.applied_table_count)},
                              {"authoritative_wal", "false"}});
  return result;
}


EngineStartPhysicalBackupResult EngineStartPhysicalBackup(const EngineStartPhysicalBackupRequest& request) {
  constexpr const char* kOperation = "backup_archive.start_physical_backup";
  if (!HasBackupCreateRight(request.context)) {
    return MakeApiBehaviorDiagnostic<EngineStartPhysicalBackupResult>(request.context,
                                                                      kOperation,
                                                                      MakeSecurityContextRequiredDiagnostic(kOperation));
  }
  if (OptionValue(request, "scope:") == "cluster" && !request.context.cluster_authority_available) {
    return MakeApiBehaviorDiagnostic<EngineStartPhysicalBackupResult>(request.context,
                                                                      kOperation,
                                                                      BackupInvalid(kOperation, "BACKUP_CLUSTER_SCOPE_ABSENT"));
  }
  const auto lifecycle =
      EvaluateBackupArchiveLifecycleAdmission(request, BackupArchiveLifecycleOperation::physical_backup);
  if (!lifecycle.admitted) {
    return MakeApiBehaviorDiagnostic<EngineStartPhysicalBackupResult>(
        request.context, kOperation, lifecycle.diagnostic);
  }
  const auto manifest_path = BackupPath(request);
  if (manifest_path.empty() || request.context.database_path.empty()) {
    return MakeApiBehaviorDiagnostic<EngineStartPhysicalBackupResult>(request.context,
                                                                      kOperation,
                                                                      BackupInvalid(kOperation, "BACKUP_TARGET_UNAVAILABLE"));
  }
  if (RequiredManifestOption(request, "filespace_uuid:").empty()) {
    return MakeApiBehaviorDiagnostic<EngineStartPhysicalBackupResult>(request.context,
                                                                      kOperation,
                                                                      BackupInvalid(kOperation, "BACKUP_FILESPACE_UUID_REQUIRED"));
  }
  bool source_ok = false;
  const auto image_bytes = ReadBinaryFile(request.context.database_path, &source_ok);
  if (!source_ok || image_bytes.empty()) {
    return MakeApiBehaviorDiagnostic<EngineStartPhysicalBackupResult>(request.context,
                                                                      kOperation,
                                                                      BackupInvalid(kOperation, "BACKUP_SOURCE_UNAVAILABLE"));
  }
  const auto image_uri = PhysicalImagePath(request, manifest_path);
  if (!WriteBinaryFile(image_uri, image_bytes)) {
    return MakeApiBehaviorDiagnostic<EngineStartPhysicalBackupResult>(request.context,
                                                                      kOperation,
                                                                      BackupInvalid(kOperation, "BACKUP_IMAGE_WRITE_FAILED"));
  }
  EngineUuid backup_uuid;
  backup_uuid.canonical = GenerateCrudEngineUuid("physical_backup");
  const auto image_checksum = Fnv1a64(image_bytes);
  const auto body = BuildPhysicalManifest(request,
                                          backup_uuid,
                                          image_uri,
                                          static_cast<std::uint64_t>(image_bytes.size()),
                                          image_checksum);
  const auto manifest_checksum = Fnv1a64(body);
  if (!WriteBinaryFile(manifest_path, body + "CHECKSUM\t" + std::to_string(manifest_checksum) + "\n")) {
    return MakeApiBehaviorDiagnostic<EngineStartPhysicalBackupResult>(request.context,
                                                                      kOperation,
                                                                      BackupInvalid(kOperation, "BACKUP_MANIFEST_WRITE_FAILED"));
  }
  (void)scratchbird::core::metrics::PublishBackupInProgress(0.0, "physical_backup");
  (void)scratchbird::core::metrics::PublishBackupProgressPercent(100.0, "physical_backup");
  auto result = MakeApiBehaviorSuccess<EngineStartPhysicalBackupResult>(request.context, kOperation);
  AddBackupLifecycleEvidence(&result, lifecycle);
  result.backup_uuid = backup_uuid;
  result.manifest_uri = manifest_path;
  result.image_uri = image_uri;
  result.image_bytes = static_cast<EngineApiU64>(image_bytes.size());
  AddApiBehaviorEvidence(&result, "physical_backup_manifest", manifest_path);
  AddApiBehaviorEvidence(&result, "physical_backup_image", image_uri);
  AddApiBehaviorEvidence(&result, "image_checksum", std::to_string(image_checksum));
  AddApiBehaviorEvidence(&result, "authoritative_wal", "false");
  AddApiBehaviorRow(&result, {{"backup_uuid", backup_uuid.canonical},
                              {"manifest_uri", manifest_path},
                              {"image_uri", image_uri},
                              {"image_bytes", std::to_string(result.image_bytes)},
                              {"authoritative_wal", "false"}});
  return result;
}

EngineRestorePhysicalBackupResult EngineRestorePhysicalBackup(const EngineRestorePhysicalBackupRequest& request) {
  constexpr const char* kOperation = "backup_archive.restore_physical_backup";
  if (!HasBackupRestoreRight(request.context)) {
    return MakeApiBehaviorDiagnostic<EngineRestorePhysicalBackupResult>(request.context,
                                                                        kOperation,
                                                                        MakeSecurityContextRequiredDiagnostic(kOperation));
  }
  const auto manifest_path = BackupPath(request);
  if (manifest_path.empty() || request.context.database_path.empty()) {
    return MakeApiBehaviorDiagnostic<EngineRestorePhysicalBackupResult>(request.context,
                                                                        kOperation,
                                                                        BackupInvalid(kOperation, "RESTORE_REQUEST_INVALID"));
  }
  const auto lifecycle =
      EvaluateBackupArchiveLifecycleAdmission(request, BackupArchiveLifecycleOperation::physical_restore);
  if (!lifecycle.admitted) {
    return MakeApiBehaviorDiagnostic<EngineRestorePhysicalBackupResult>(
        request.context, kOperation, lifecycle.diagnostic);
  }
  std::string image_uri;
  std::uint64_t expected_bytes = 0;
  std::uint64_t expected_checksum = 0;
  EngineUuid backup_uuid;
  const auto manifest_status = ReadAndVerifyPhysicalManifest(manifest_path,
                                                             &image_uri,
                                                             &expected_bytes,
                                                             &expected_checksum,
                                                             &backup_uuid);
  if (manifest_status.error) {
    return MakeApiBehaviorDiagnostic<EngineRestorePhysicalBackupResult>(request.context, kOperation, manifest_status);
  }
  bool image_ok = false;
  const auto image = ReadBinaryFile(image_uri, &image_ok);
  if (!image_ok || image.size() != expected_bytes || Fnv1a64(image) != expected_checksum) {
    return MakeApiBehaviorDiagnostic<EngineRestorePhysicalBackupResult>(request.context,
                                                                        kOperation,
                                                                        BackupInvalid(kOperation, "RESTORE_MANIFEST_CHECKSUM_INVALID"));
  }
  if (!WriteBinaryFile(request.context.database_path, image)) {
    return MakeApiBehaviorDiagnostic<EngineRestorePhysicalBackupResult>(request.context,
                                                                        kOperation,
                                                                        BackupInvalid(kOperation, "RESTORE_TARGET_WRITE_FAILED"));
  }
  (void)scratchbird::core::metrics::PublishBackupInProgress(0.0, "physical_restore");
  (void)scratchbird::core::metrics::PublishBackupProgressPercent(100.0, "physical_restore");
  auto result = MakeApiBehaviorSuccess<EngineRestorePhysicalBackupResult>(request.context, kOperation);
  AddBackupLifecycleEvidence(&result, lifecycle);
  result.restore_uuid.canonical = GenerateCrudEngineUuid("physical_restore");
  result.source_backup_uuid = backup_uuid;
  result.source_manifest_uri = manifest_path;
  result.restored_database_path = request.context.database_path;
  result.image_bytes = static_cast<EngineApiU64>(image.size());
  AddApiBehaviorEvidence(&result, "physical_restore_manifest_validated", manifest_path);
  AddApiBehaviorEvidence(&result, "physical_restore_image_checksum", std::to_string(expected_checksum));
  AddApiBehaviorEvidence(&result, "evidence_before_success", "physical_restore_installed");
  AddApiBehaviorRow(&result, {{"restore_uuid", result.restore_uuid.canonical},
                              {"source_backup_uuid", backup_uuid.canonical},
                              {"source_manifest_uri", manifest_path},
                              {"restored_database_path", request.context.database_path},
                              {"image_bytes", std::to_string(result.image_bytes)}});
  return result;
}


}  // namespace scratchbird::engine::internal_api
