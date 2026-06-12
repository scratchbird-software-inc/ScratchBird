// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/serializable_mutation_guard.hpp"

#include "api_diagnostics.hpp"
#include "disk_device.hpp"
#include "local_transaction_store.hpp"
#include "transaction_inventory.hpp"
#include "transaction_state.hpp"
#include "isolation.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace scratchbird::engine::internal_api::dml {
namespace {

namespace mga = scratchbird::transaction::mga;
namespace platform = scratchbird::core::platform;
namespace storage_db = scratchbird::storage::database;
namespace storage_disk = scratchbird::storage::disk;
namespace uuid = scratchbird::core::uuid;

using platform::TypedUuid;
using platform::UuidKind;
using platform::u64;

struct LedgerAccess {
  u64 sequence = 0;
  mga::SerializableAccessRecord record;
};

std::string LowerAscii(std::string value) {
  for (char& ch : value) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return value;
}

bool IsSerializableContext(const EngineRequestContext& context) {
  const std::string isolation = LowerAscii(context.transaction_isolation_level);
  return isolation == "serializable" || isolation == "serializable_snapshot";
}

bool HasTruthyOption(const std::vector<std::string>& options,
                     std::string_view key) {
  const std::string equals_prefix = std::string(key) + "=";
  const std::string colon_prefix = std::string(key) + ":";
  for (const std::string& option : options) {
    if (option == key || option == equals_prefix + "true" ||
        option == equals_prefix + "1" || option == colon_prefix + "true" ||
        option == colon_prefix + "1") {
      return true;
    }
  }
  return false;
}

bool ParserOrReferenceAuthorityRequested(const EngineRequestContext& context,
                                     const EnginePredicateEnvelope& predicate,
                                     std::span<const std::string> option_envelopes) {
  const std::vector<std::string> options(option_envelopes.begin(),
                                         option_envelopes.end());
  return predicate.predicate_kind == "reference_bulk" ||
         HasTruthyOption(context.trace_tags, "serializable.parser_or_reference_authority") ||
         HasTruthyOption(context.trace_tags, "odf031.parser_or_reference_authority") ||
         HasTruthyOption(options, "serializable.parser_or_reference_authority") ||
         HasTruthyOption(options, "odf031.parser_or_reference_authority");
}

std::filesystem::path LedgerPath(const EngineRequestContext& context) {
  if (context.database_path.empty()) {
    return {};
  }
  return std::filesystem::path(context.database_path + ".sb.serializable_access");
}

SerializableDmlAdmissionResult InactiveResult() {
  SerializableDmlAdmissionResult result;
  result.ok = true;
  result.active = false;
  return result;
}

SerializableDmlAdmissionResult Refuse(std::string operation_id,
                                      std::string code,
                                      std::string message_key,
                                      std::string detail) {
  SerializableDmlAdmissionResult result;
  result.ok = false;
  result.active = true;
  result.diagnostic = MakeEngineApiDiagnostic(std::move(code),
                                              std::move(message_key),
                                              std::move(operation_id) + ":" + detail,
                                              true);
  result.evidence.push_back({"serializable.admission", "refused"});
  result.evidence.push_back({"serializable.inventory_authority", "durable_transaction_inventory"});
  result.evidence.push_back({"serializable.failure", std::move(detail)});
  return result;
}

std::string DiagnosticDetail(const platform::DiagnosticRecord& diagnostic) {
  std::string detail = diagnostic.remediation_hint;
  for (const auto& argument : diagnostic.arguments) {
    if (!detail.empty()) {
      detail += ";";
    }
    detail += argument.key + "=" + argument.value;
  }
  return detail;
}

SerializableDmlAdmissionResult RefuseFromSerializable(
    std::string operation_id,
    const mga::SerializableConflictResult& conflict) {
  SerializableDmlAdmissionResult result;
  result.ok = false;
  result.active = true;
  const std::string detail = DiagnosticDetail(conflict.diagnostic);
  result.diagnostic = MakeEngineApiDiagnostic(
      conflict.diagnostic.diagnostic_code.empty()
          ? "SB-SNTXN-SERIALIZABLE-ADMISSION-REFUSED"
          : conflict.diagnostic.diagnostic_code,
      conflict.diagnostic.message_key.empty()
          ? "transaction.serializable.admission_refused"
          : conflict.diagnostic.message_key,
      std::move(operation_id) + ":" + detail,
      true);
  result.evidence.push_back({"serializable.admission", "refused"});
  result.evidence.push_back({"serializable.conflict",
                             mga::SerializableConflictKindName(conflict.conflict)});
  result.evidence.push_back({"serializable.retry_class",
                             mga::SerializableRetryClassName(conflict.retry_class)});
  for (const std::string& evidence : conflict.evidence) {
    result.evidence.push_back({"serializable.evidence", evidence});
  }
  return result;
}

SerializableDmlAdmissionResult Admit(std::string action,
                                     std::vector<EngineEvidenceReference> evidence) {
  SerializableDmlAdmissionResult result;
  result.ok = true;
  result.active = true;
  result.evidence = std::move(evidence);
  result.evidence.push_back({"serializable.admission", std::move(action)});
  result.evidence.push_back({"serializable.inventory_authority", "durable_transaction_inventory"});
  result.evidence.push_back({"serializable.parser_or_reference_authority", "false"});
  result.evidence.push_back({"serializable.ledger", "durable_access_file"});
  return result;
}

std::string HexEncode(std::string_view value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string encoded;
  encoded.reserve(value.size() * 2);
  for (unsigned char ch : value) {
    encoded.push_back(kHex[ch >> 4]);
    encoded.push_back(kHex[ch & 0x0f]);
  }
  return encoded;
}

int HexDigit(char ch) {
  if (ch >= '0' && ch <= '9') {
    return ch - '0';
  }
  if (ch >= 'a' && ch <= 'f') {
    return ch - 'a' + 10;
  }
  if (ch >= 'A' && ch <= 'F') {
    return ch - 'A' + 10;
  }
  return -1;
}

std::string HexDecode(std::string_view value) {
  if (value.size() % 2 != 0) {
    return {};
  }
  std::string decoded;
  decoded.reserve(value.size() / 2);
  for (std::size_t index = 0; index < value.size(); index += 2) {
    const int hi = HexDigit(value[index]);
    const int lo = HexDigit(value[index + 1]);
    if (hi < 0 || lo < 0) {
      return {};
    }
    decoded.push_back(static_cast<char>((hi << 4) | lo));
  }
  return decoded;
}

std::vector<std::string> SplitTabs(const std::string& line) {
  std::vector<std::string> parts;
  std::size_t start = 0;
  while (start <= line.size()) {
    const std::size_t tab = line.find('\t', start);
    if (tab == std::string::npos) {
      parts.push_back(line.substr(start));
      break;
    }
    parts.push_back(line.substr(start, tab - start));
    start = tab + 1;
  }
  return parts;
}

TypedUuid ParseRelationUuid(const std::string& relation_uuid) {
  const auto parsed = uuid::ParseTypedUuid(UuidKind::object, relation_uuid);
  return parsed.ok() ? parsed.value : TypedUuid{};
}

std::string RelationUuidText(const TypedUuid& relation_uuid) {
  return relation_uuid.valid() ? uuid::UuidToString(relation_uuid.value) : std::string{};
}

mga::SerializableAccessKind AccessKindFromText(const std::string& text) {
  if (text == "point_read") return mga::SerializableAccessKind::point_read;
  if (text == "range_read") return mga::SerializableAccessKind::range_read;
  if (text == "predicate_read") return mga::SerializableAccessKind::predicate_read;
  if (text == "insert") return mga::SerializableAccessKind::insert;
  if (text == "update") return mga::SerializableAccessKind::update;
  if (text == "delete_row") return mga::SerializableAccessKind::delete_row;
  return mga::SerializableAccessKind::unknown;
}

bool ParseBool(const std::string& value) {
  return value == "1" || value == "true";
}

u64 ParseU64(const std::string& value) {
  try {
    return static_cast<u64>(std::stoull(value));
  } catch (...) {
    return 0;
  }
}

std::string RowFieldKey(std::string_view column, std::string_view encoded_value) {
  return "column:" + std::string(column) + ":" + std::string(encoded_value);
}

std::string RowUuidKey(std::string_view row_uuid) {
  return "row_uuid:" + std::string(row_uuid);
}

std::string PredicateDigest(const EnginePredicateEnvelope& predicate) {
  std::string digest = predicate.predicate_kind + ":" +
                       predicate.canonical_predicate_envelope + ":" +
                       std::to_string(predicate.bound_values.size());
  for (const auto& value : predicate.bound_values) {
    digest += ":" + value.encoded_value;
  }
  return digest;
}

mga::SerializableKeyRange FullRelationRange(TypedUuid relation_uuid) {
  mga::SerializableKeyRange range;
  range.relation_uuid = relation_uuid;
  range.lower_unbounded = true;
  range.upper_unbounded = true;
  range.full_relation = true;
  return range;
}

std::vector<mga::SerializableKeyRange> RangesForPredicate(
    TypedUuid relation_uuid,
    const EnginePredicateEnvelope& predicate) {
  std::vector<mga::SerializableKeyRange> ranges;
  if (predicate.predicate_kind == "row_uuid_match" &&
      !predicate.canonical_predicate_envelope.empty()) {
    ranges.push_back(mga::MakeSerializablePointRange(
        relation_uuid,
        RowUuidKey(predicate.canonical_predicate_envelope)));
    return ranges;
  }
  if (predicate.predicate_kind == "row_uuid_in_list") {
    for (const auto& value : predicate.bound_values) {
      if (!value.encoded_value.empty()) {
        ranges.push_back(mga::MakeSerializablePointRange(
            relation_uuid,
            RowUuidKey(value.encoded_value)));
      }
    }
    if (!ranges.empty()) {
      return ranges;
    }
  }
  if (predicate.predicate_kind == "column_equals" &&
      !predicate.canonical_predicate_envelope.empty() &&
      !predicate.bound_values.empty()) {
    ranges.push_back(mga::MakeSerializablePointRange(
        relation_uuid,
        RowFieldKey(predicate.canonical_predicate_envelope,
                    predicate.bound_values.front().encoded_value)));
    return ranges;
  }
  if (predicate.predicate_kind == "column_in_list" &&
      !predicate.canonical_predicate_envelope.empty()) {
    for (const auto& value : predicate.bound_values) {
      ranges.push_back(mga::MakeSerializablePointRange(
          relation_uuid,
          RowFieldKey(predicate.canonical_predicate_envelope,
                      value.encoded_value)));
    }
    if (!ranges.empty()) {
      return ranges;
    }
  }
  if (predicate.predicate_kind == "column_range" &&
      !predicate.canonical_predicate_envelope.empty() &&
      predicate.bound_values.size() >= 2) {
    ranges.push_back(mga::MakeSerializableBoundedRange(
        relation_uuid,
        RowFieldKey(predicate.canonical_predicate_envelope,
                    predicate.bound_values[0].encoded_value),
        RowFieldKey(predicate.canonical_predicate_envelope,
                    predicate.bound_values[1].encoded_value)));
    return ranges;
  }
  auto range = FullRelationRange(relation_uuid);
  range.predicate_digest = PredicateDigest(predicate);
  ranges.push_back(std::move(range));
  return ranges;
}

std::vector<mga::SerializableKeyRange> RangesForRows(
    TypedUuid relation_uuid,
    std::span<const EngineRowValue> rows) {
  std::vector<mga::SerializableKeyRange> ranges;
  for (const auto& row : rows) {
    if (!row.requested_row_uuid.canonical.empty()) {
      ranges.push_back(mga::MakeSerializablePointRange(
          relation_uuid,
          RowUuidKey(row.requested_row_uuid.canonical)));
    }
    for (const auto& [column, value] : row.fields) {
      if (column.empty()) {
        continue;
      }
      ranges.push_back(mga::MakeSerializablePointRange(
          relation_uuid,
          RowFieldKey(column, value.is_null ? "<NULL>" : value.encoded_value)));
    }
  }
  if (ranges.empty()) {
    ranges.push_back(FullRelationRange(relation_uuid));
  }
  return ranges;
}

EngineEvidenceReference Evidence(std::string kind, std::string value) {
  return {std::move(kind), std::move(value)};
}

SerializableDmlAdmissionResult LoadInventory(
    const EngineRequestContext& context,
    std::string operation_id,
    storage_db::LocalTransactionStoreResult* loaded) {
  if (!loaded) {
    return Refuse(std::move(operation_id),
                  "SB-SNTXN-SERIALIZABLE-INTERNAL-INVALID",
                  "transaction.serializable.internal_invalid",
                  "missing_inventory_result");
  }
  if (context.database_path.empty()) {
    return Refuse(std::move(operation_id),
                  "SB-SNTXN-SERIALIZABLE-INVENTORY-AUTHORITY-REQUIRED",
                  "transaction.serializable.inventory_authority_required",
                  "database_path_required_for_durable_inventory");
  }
  *loaded = storage_db::LoadLocalTransactionInventoryFromDatabase(
      context.database_path);
  if (!loaded->ok()) {
    return Refuse(std::move(operation_id),
                  loaded->diagnostic.diagnostic_code.empty()
                      ? "SB-SNTXN-SERIALIZABLE-INVENTORY-LOAD-FAILED"
                      : loaded->diagnostic.diagnostic_code,
                  loaded->diagnostic.message_key.empty()
                      ? "transaction.serializable.inventory_load_failed"
                      : loaded->diagnostic.message_key,
                  DiagnosticDetail(loaded->diagnostic));
  }
  return Admit("inventory_loaded", {});
}

SerializableDmlAdmissionResult LookupCurrentTransaction(
    const EngineRequestContext& context,
    std::string operation_id,
    const mga::LocalTransactionInventory& inventory,
    mga::TransactionInventoryEntry* entry) {
  if (!entry) {
    return Refuse(std::move(operation_id),
                  "SB-SNTXN-SERIALIZABLE-INTERNAL-INVALID",
                  "transaction.serializable.internal_invalid",
                  "missing_transaction_entry");
  }
  const auto lookup = mga::LookupLocalTransaction(
      inventory,
      mga::MakeLocalTransactionId(context.local_transaction_id));
  if (!lookup.ok()) {
    return Refuse(std::move(operation_id),
                  lookup.diagnostic.diagnostic_code.empty()
                      ? "SB-SNTXN-SERIALIZABLE-INVENTORY-AUTHORITY-REQUIRED"
                      : lookup.diagnostic.diagnostic_code,
                  lookup.diagnostic.message_key.empty()
                      ? "transaction.serializable.inventory_authority_required"
                      : lookup.diagnostic.message_key,
                  DiagnosticDetail(lookup.diagnostic));
  }
  *entry = lookup.entry;
  return Admit("transaction_loaded", {});
}

bool CurrentTransactionStateAllowed(mga::TransactionState state, bool read_access) {
  if (state == mga::TransactionState::active) {
    return true;
  }
  return read_access && state == mga::TransactionState::read_only_active;
}

bool DecodeLedgerLine(const std::string& line,
                      const mga::LocalTransactionInventory& inventory,
                      LedgerAccess* access) {
  if (!access) {
    return false;
  }
  const auto parts = SplitTabs(line);
  if (parts.size() != 13 || parts[0] != "SBSER001") {
    return false;
  }
  const u64 local_id = ParseU64(parts[2]);
  const auto lookup = mga::LookupLocalTransaction(inventory,
                                                  mga::MakeLocalTransactionId(local_id));
  access->sequence = ParseU64(parts[1]);
  access->record.local_id = mga::MakeLocalTransactionId(local_id);
  access->record.transaction_state =
      lookup.ok() ? lookup.entry.state : mga::TransactionState::recovering;
  access->record.kind = AccessKindFromText(parts[3]);
  access->record.range.relation_uuid = ParseRelationUuid(parts[4]);
  access->record.range.lower_bound = HexDecode(parts[5]);
  access->record.range.upper_bound = HexDecode(parts[6]);
  access->record.range.lower_unbounded = ParseBool(parts[7]);
  access->record.range.upper_unbounded = ParseBool(parts[8]);
  access->record.range.lower_inclusive = ParseBool(parts[9]);
  access->record.range.upper_inclusive = ParseBool(parts[10]);
  access->record.range.full_relation = ParseBool(parts[11]);
  access->record.range.predicate_digest = HexDecode(parts[12]);
  access->record.durable_inventory_authoritative = true;
  access->record.sequence = access->sequence;
  return access->sequence != 0 && access->record.local_id.valid();
}

std::vector<LedgerAccess> LoadLedger(const EngineRequestContext& context,
                                     const mga::LocalTransactionInventory& inventory,
                                     u64* next_sequence) {
  std::vector<LedgerAccess> accesses;
  u64 max_sequence = 0;
  const auto path = LedgerPath(context);
  if (path.empty() || !std::filesystem::exists(path)) {
    if (next_sequence) {
      *next_sequence = 1;
    }
    return accesses;
  }
  std::ifstream input(path);
  std::string line;
  while (std::getline(input, line)) {
    LedgerAccess access;
    if (!DecodeLedgerLine(line, inventory, &access)) {
      continue;
    }
    max_sequence = std::max(max_sequence, access.sequence);
    accesses.push_back(std::move(access));
  }
  if (next_sequence) {
    *next_sequence = max_sequence + 1;
  }
  return accesses;
}

std::string EncodeLedgerLine(const mga::SerializableAccessRecord& access) {
  std::ostringstream out;
  out << "SBSER001\t"
      << access.sequence << '\t'
      << access.local_id.value << '\t'
      << mga::SerializableAccessKindName(access.kind) << '\t'
      << RelationUuidText(access.range.relation_uuid) << '\t'
      << HexEncode(access.range.lower_bound) << '\t'
      << HexEncode(access.range.upper_bound) << '\t'
      << (access.range.lower_unbounded ? "1" : "0") << '\t'
      << (access.range.upper_unbounded ? "1" : "0") << '\t'
      << (access.range.lower_inclusive ? "1" : "0") << '\t'
      << (access.range.upper_inclusive ? "1" : "0") << '\t'
      << (access.range.full_relation ? "1" : "0") << '\t'
      << HexEncode(access.range.predicate_digest) << '\n';
  return out.str();
}

SerializableDmlAdmissionResult AppendLedgerRecords(
    const EngineRequestContext& context,
    std::string operation_id,
    const std::vector<mga::SerializableAccessRecord>& records) {
  if (records.empty()) {
    return Admit("recorded", {});
  }
  const auto path = LedgerPath(context);
  if (path.empty()) {
    return Refuse(std::move(operation_id),
                  "SB-SNTXN-SERIALIZABLE-LEDGER-REQUIRED",
                  "transaction.serializable.ledger_required",
                  "database_path_required");
  }
  const bool existed_before_append = std::filesystem::exists(path);
  std::ofstream output(path, std::ios::binary | std::ios::app);
  if (!output) {
    return Refuse(std::move(operation_id),
                  "SB-SNTXN-SERIALIZABLE-LEDGER-WRITE-FAILED",
                  "transaction.serializable.ledger_write_failed",
                  "open_failed");
  }
  for (const auto& record : records) {
    output << EncodeLedgerLine(record);
  }
  output.flush();
  if (!output) {
    return Refuse(std::move(operation_id),
                  "SB-SNTXN-SERIALIZABLE-LEDGER-WRITE-FAILED",
                  "transaction.serializable.ledger_write_failed",
                  "flush_failed");
  }
  output.close();
  if (!output) {
    return Refuse(std::move(operation_id),
                  "SB-SNTXN-SERIALIZABLE-LEDGER-WRITE-FAILED",
                  "transaction.serializable.ledger_write_failed",
                  "close_failed");
  }
  const auto file_sync = storage_disk::SyncFilesystemPath(path.string(), true);
  if (!file_sync.ok()) {
    return Refuse(std::move(operation_id),
                  file_sync.diagnostic.diagnostic_code.empty()
                      ? "SB-SNTXN-SERIALIZABLE-LEDGER-SYNC-FAILED"
                      : file_sync.diagnostic.diagnostic_code,
                  file_sync.diagnostic.message_key.empty()
                      ? "transaction.serializable.ledger_sync_failed"
                      : file_sync.diagnostic.message_key,
                  DiagnosticDetail(file_sync.diagnostic));
  }
  if (!existed_before_append) {
    const auto parent_sync = storage_disk::SyncParentDirectoryPath(path.string());
    if (!parent_sync.ok()) {
      return Refuse(std::move(operation_id),
                    parent_sync.diagnostic.diagnostic_code.empty()
                        ? "SB-SNTXN-SERIALIZABLE-LEDGER-PARENT-SYNC-FAILED"
                        : parent_sync.diagnostic.diagnostic_code,
                    parent_sync.diagnostic.message_key.empty()
                        ? "transaction.serializable.ledger_parent_sync_failed"
                        : parent_sync.diagnostic.message_key,
                    DiagnosticDetail(parent_sync.diagnostic));
    }
  }
  std::vector<EngineEvidenceReference> evidence;
  evidence.push_back(Evidence("serializable.access_records",
                              std::to_string(records.size())));
  evidence.push_back(Evidence("serializable.ledger_persisted", "true"));
  evidence.push_back(Evidence("serializable.ledger_sync_fence", "fsync"));
  evidence.push_back(Evidence("serializable.ledger_parent_sync",
                              existed_before_append ? "not_required" : "fsync"));
  return Admit("recorded", std::move(evidence));
}

std::vector<mga::SerializableAccessRecord> ExistingRecords(
    const std::vector<LedgerAccess>& ledger) {
  std::vector<mga::SerializableAccessRecord> records;
  records.reserve(ledger.size());
  for (const LedgerAccess& access : ledger) {
    records.push_back(access.record);
  }
  return records;
}

mga::SerializableAccessRecord AccessRecord(
    const mga::TransactionInventoryEntry& txn,
    mga::SerializableAccessKind kind,
    mga::SerializableKeyRange range,
    u64 sequence,
    bool parser_or_reference_authority) {
  mga::SerializableAccessRecord access;
  access.local_id = txn.identity.local_id;
  access.transaction_state = txn.state;
  access.kind = kind;
  access.range = std::move(range);
  access.sequence = sequence;
  access.durable_inventory_authoritative = true;
  access.parser_or_reference_authority = parser_or_reference_authority;
  return access;
}

SerializableDmlAdmissionResult BuildRecords(
    const EngineRequestContext& context,
    std::string operation_id,
    std::string relation_uuid,
    mga::SerializableAccessKind kind,
    const std::vector<mga::SerializableKeyRange>& ranges,
    bool read_access,
    bool parser_or_reference_authority,
    storage_db::LocalTransactionStoreResult* loaded,
    mga::TransactionInventoryEntry* txn,
    u64* next_sequence,
    std::vector<mga::SerializableAccessRecord>* records,
    std::vector<mga::SerializableAccessRecord>* existing) {
  const auto inactive =
      IsSerializableContext(context) ? SerializableDmlAdmissionResult{} : InactiveResult();
  if (!IsSerializableContext(context)) {
    return inactive;
  }
  const TypedUuid relation = ParseRelationUuid(relation_uuid);
  if (!relation.valid()) {
    return Refuse(std::move(operation_id),
                  "SB-SNTXN-SERIALIZABLE-INVALID-RELATION",
                  "transaction.serializable.invalid_relation",
                  "relation_uuid_invalid");
  }
  auto inventory_result = LoadInventory(context, operation_id, loaded);
  if (!inventory_result.ok) {
    return inventory_result;
  }
  auto lookup = LookupCurrentTransaction(context,
                                         operation_id,
                                         loaded->inventory,
                                         txn);
  if (!lookup.ok) {
    return lookup;
  }
  if (!CurrentTransactionStateAllowed(txn->state, read_access)) {
    return Refuse(std::move(operation_id),
                  "SB-SNTXN-SERIALIZABLE-TRANSACTION-NOT-ACTIVE",
                  "transaction.serializable.transaction_not_active",
                  mga::TransactionStateName(txn->state));
  }
  const auto ledger = LoadLedger(context, loaded->inventory, next_sequence);
  if (existing) {
    *existing = ExistingRecords(ledger);
  }
  if (records) {
    records->clear();
    records->reserve(ranges.size());
    u64 sequence = *next_sequence;
    for (auto range : ranges) {
      range.relation_uuid = relation;
      records->push_back(AccessRecord(*txn,
                                      kind,
                                      std::move(range),
                                      sequence++,
                                      parser_or_reference_authority));
    }
    *next_sequence = sequence;
  }
  return Admit("prepared", {});
}

SerializableDmlAdmissionResult CheckRecords(
    std::string operation_id,
    const std::vector<mga::SerializableAccessRecord>& existing,
    const std::vector<mga::SerializableAccessRecord>& records) {
  for (const auto& record : records) {
    const auto conflict = mga::EvaluateSerializableWriteConflict(existing, record);
    if (!conflict.ok()) {
      return RefuseFromSerializable(operation_id, conflict);
    }
  }
  std::vector<EngineEvidenceReference> evidence;
  evidence.push_back(Evidence("serializable.checked_records",
                              std::to_string(records.size())));
  return Admit("checked", std::move(evidence));
}

SerializableDmlAdmissionResult RecordReadOrWrite(
    const EngineRequestContext& context,
    std::string operation_id,
    std::string relation_uuid,
    mga::SerializableAccessKind kind,
    const std::vector<mga::SerializableKeyRange>& ranges,
    bool read_access,
    bool parser_or_reference_authority,
    bool check_first) {
  storage_db::LocalTransactionStoreResult loaded;
  mga::TransactionInventoryEntry txn;
  u64 next_sequence = 0;
  std::vector<mga::SerializableAccessRecord> records;
  std::vector<mga::SerializableAccessRecord> existing;
  auto built = BuildRecords(context,
                            operation_id,
                            std::move(relation_uuid),
                            kind,
                            ranges,
                            read_access,
                            parser_or_reference_authority,
                            &loaded,
                            &txn,
                            &next_sequence,
                            &records,
                            &existing);
  if (!built.ok || !built.active) {
    return built;
  }
  if (check_first) {
    auto checked = CheckRecords(operation_id, existing, records);
    if (!checked.ok) {
      return checked;
    }
  }
  auto appended = AppendLedgerRecords(context, operation_id, records);
  if (!appended.ok) {
    return appended;
  }
  appended.evidence.push_back(Evidence("serializable.access_kind",
                                       mga::SerializableAccessKindName(kind)));
  return appended;
}

SerializableDmlAdmissionResult CheckWriteOnly(
    const EngineRequestContext& context,
    std::string operation_id,
    std::string relation_uuid,
    mga::SerializableAccessKind kind,
    const std::vector<mga::SerializableKeyRange>& ranges,
    bool parser_or_reference_authority) {
  storage_db::LocalTransactionStoreResult loaded;
  mga::TransactionInventoryEntry txn;
  u64 next_sequence = 0;
  std::vector<mga::SerializableAccessRecord> records;
  std::vector<mga::SerializableAccessRecord> existing;
  auto built = BuildRecords(context,
                            operation_id,
                            std::move(relation_uuid),
                            kind,
                            ranges,
                            false,
                            parser_or_reference_authority,
                            &loaded,
                            &txn,
                            &next_sequence,
                            &records,
                            &existing);
  if (!built.ok || !built.active) {
    return built;
  }
  return CheckRecords(operation_id, existing, records);
}

void AddPredicateEvidence(const EnginePredicateEnvelope& predicate,
                          SerializableDmlAdmissionResult* result) {
  if (!result || !result->active) {
    return;
  }
  result->evidence.push_back({"serializable.predicate_kind",
                              predicate.predicate_kind.empty()
                                  ? "full_relation"
                                  : predicate.predicate_kind});
  if (!predicate.canonical_predicate_envelope.empty()) {
    result->evidence.push_back({"serializable.predicate_descriptor",
                                predicate.canonical_predicate_envelope});
  }
}

}  // namespace

SerializableDmlAdmissionResult RecordSerializableSelectRead(
    const EngineRequestContext& context,
    std::string operation_id,
    std::string relation_uuid,
    const EnginePredicateEnvelope& predicate,
    std::span<const std::string> option_envelopes) {
  const TypedUuid relation = ParseRelationUuid(relation_uuid);
  auto result = RecordReadOrWrite(
      context,
      operation_id,
      std::move(relation_uuid),
      predicate.predicate_kind == "row_uuid_match" ||
              predicate.predicate_kind == "column_equals" ||
              predicate.predicate_kind == "row_uuid_in_list" ||
              predicate.predicate_kind == "column_in_list"
          ? mga::SerializableAccessKind::point_read
          : predicate.predicate_kind == "column_range"
                ? mga::SerializableAccessKind::range_read
                : mga::SerializableAccessKind::predicate_read,
      relation.valid() ? RangesForPredicate(relation, predicate)
                       : std::vector<mga::SerializableKeyRange>{},
      true,
      ParserOrReferenceAuthorityRequested(context, predicate, option_envelopes),
      false);
  AddPredicateEvidence(predicate, &result);
  return result;
}

SerializableDmlAdmissionResult CheckSerializableInsertMutation(
    const EngineRequestContext& context,
    std::string operation_id,
    std::string relation_uuid,
    std::span<const EngineRowValue> rows,
    std::span<const std::string> option_envelopes) {
  const TypedUuid relation = ParseRelationUuid(relation_uuid);
  return CheckWriteOnly(
      context,
      std::move(operation_id),
      std::move(relation_uuid),
      mga::SerializableAccessKind::insert,
      relation.valid() ? RangesForRows(relation, rows)
                       : std::vector<mga::SerializableKeyRange>{},
      ParserOrReferenceAuthorityRequested(context, {}, option_envelopes));
}

SerializableDmlAdmissionResult RecordSerializableInsertMutation(
    const EngineRequestContext& context,
    std::string operation_id,
    std::string relation_uuid,
    std::span<const EngineRowValue> rows,
    std::span<const std::string> option_envelopes) {
  const TypedUuid relation = ParseRelationUuid(relation_uuid);
  return RecordReadOrWrite(
      context,
      std::move(operation_id),
      std::move(relation_uuid),
      mga::SerializableAccessKind::insert,
      relation.valid() ? RangesForRows(relation, rows)
                       : std::vector<mga::SerializableKeyRange>{},
      false,
      ParserOrReferenceAuthorityRequested(context, {}, option_envelopes),
      true);
}

SerializableDmlAdmissionResult CheckSerializablePredicateMutation(
    const EngineRequestContext& context,
    std::string operation_id,
    std::string relation_uuid,
    const EnginePredicateEnvelope& predicate,
    bool delete_row,
    std::span<const std::string> option_envelopes) {
  const TypedUuid relation = ParseRelationUuid(relation_uuid);
  auto result = CheckWriteOnly(
      context,
      operation_id,
      std::move(relation_uuid),
      delete_row ? mga::SerializableAccessKind::delete_row
                 : mga::SerializableAccessKind::update,
      relation.valid() ? RangesForPredicate(relation, predicate)
                       : std::vector<mga::SerializableKeyRange>{},
      ParserOrReferenceAuthorityRequested(context, predicate, option_envelopes));
  AddPredicateEvidence(predicate, &result);
  return result;
}

SerializableDmlAdmissionResult RecordSerializablePredicateMutation(
    const EngineRequestContext& context,
    std::string operation_id,
    std::string relation_uuid,
    const EnginePredicateEnvelope& predicate,
    bool delete_row,
    std::span<const std::string> option_envelopes) {
  const TypedUuid relation = ParseRelationUuid(relation_uuid);
  auto result = RecordReadOrWrite(
      context,
      operation_id,
      std::move(relation_uuid),
      delete_row ? mga::SerializableAccessKind::delete_row
                 : mga::SerializableAccessKind::update,
      relation.valid() ? RangesForPredicate(relation, predicate)
                       : std::vector<mga::SerializableKeyRange>{},
      false,
      ParserOrReferenceAuthorityRequested(context, predicate, option_envelopes),
      true);
  AddPredicateEvidence(predicate, &result);
  return result;
}

}  // namespace scratchbird::engine::internal_api::dml
