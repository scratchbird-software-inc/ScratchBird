// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "local_transaction_store.hpp"

#include "disk_device.hpp"
#include "hash_digest.hpp"
#include "page_header.hpp"
#include "page_manager.hpp"
#include "startup_state.hpp"
#include "transaction_inventory_page.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef UuidToString
#undef UuidToString
#endif
#endif

namespace scratchbird::storage::database {
namespace {

namespace core_hash = scratchbird::core::hash;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::u16;
using scratchbird::storage::disk::FileDevice;
using scratchbird::storage::disk::FileOpenMode;
using scratchbird::storage::disk::kPageHeaderSerializedBytes;
using scratchbird::storage::disk::PageType;
using scratchbird::storage::disk::ParsePageHeader;
using scratchbird::storage::disk::ParseDatabaseHeader;
using scratchbird::storage::disk::ReadDevicePageHeader;
using scratchbird::storage::disk::SerializedDatabaseHeader;
using scratchbird::storage::page::BuildManagedPageHeader;
using scratchbird::storage::page::BuildTransactionInventoryPageBody;
using scratchbird::storage::page::CheckedPageBodyOffset;
using scratchbird::storage::page::CheckedPageOffset;
using scratchbird::storage::page::ManagedPageHeaderRequest;
using scratchbird::storage::page::MaxTransactionInventoryEntriesPerPage;
using scratchbird::storage::page::ParseTransactionInventoryPageBody;
using scratchbird::storage::page::PageManagerContext;
using scratchbird::storage::page::TransactionInventoryPageBody;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::uuid::GenerateEngineIdentityV7;
using scratchbird::transaction::mga::ComputeLocalTransactionHorizons;
using scratchbird::transaction::mga::LocalTransactionInventory;
using scratchbird::transaction::mga::TransactionInventoryEntry;
using scratchbird::transaction::mga::TransactionScope;
using scratchbird::transaction::mga::TransactionState;

Status StoreOkStatus();

struct TransactionInventoryCacheSignature {
  std::uintmax_t file_size = 0;
  std::int64_t write_time_count = 0;
};

struct CachedTransactionInventory {
  TransactionInventoryCacheSignature signature;
  LocalTransactionInventory inventory;
  scratchbird::transaction::mga::LocalTransactionHorizons horizons;
};

std::mutex& TransactionInventoryCacheMutex() {
  static std::mutex mutex;
  return mutex;
}

std::map<std::string, CachedTransactionInventory>& TransactionInventoryCache() {
  static std::map<std::string, CachedTransactionInventory> cache;
  return cache;
}

std::optional<TransactionInventoryCacheSignature> ReadTransactionInventoryCacheSignature(
    const std::string& path) {
  if (path.empty()) {
    return std::nullopt;
  }
  std::error_code ignored;
  const auto file_size = std::filesystem::file_size(path, ignored);
  if (ignored) {
    return std::nullopt;
  }
  const auto write_time = std::filesystem::last_write_time(path, ignored);
  if (ignored) {
    return std::nullopt;
  }
  TransactionInventoryCacheSignature signature;
  signature.file_size = file_size;
  signature.write_time_count =
      static_cast<std::int64_t>(write_time.time_since_epoch().count());
  return signature;
}

bool SameTransactionInventoryCacheSignature(
    const TransactionInventoryCacheSignature& lhs,
    const TransactionInventoryCacheSignature& rhs) {
  return lhs.file_size == rhs.file_size &&
         lhs.write_time_count == rhs.write_time_count;
}

std::optional<LocalTransactionStoreResult> TryLoadCachedTransactionInventory(
    const std::string& path) {
  const auto signature = ReadTransactionInventoryCacheSignature(path);
  if (!signature.has_value()) {
    return std::nullopt;
  }
  std::lock_guard<std::mutex> guard(TransactionInventoryCacheMutex());
  const auto found = TransactionInventoryCache().find(path);
  if (found == TransactionInventoryCache().end() ||
      !SameTransactionInventoryCacheSignature(found->second.signature, *signature)) {
    return std::nullopt;
  }
  LocalTransactionStoreResult result;
  result.status = StoreOkStatus();
  result.inventory = found->second.inventory;
  result.horizons = found->second.horizons;
  return result;
}

void InvalidateTransactionInventoryCache(const std::string& path) {
  if (path.empty()) {
    return;
  }
  std::lock_guard<std::mutex> guard(TransactionInventoryCacheMutex());
  TransactionInventoryCache().erase(path);
}

void RefreshTransactionInventoryCache(
    const std::string& path,
    const LocalTransactionInventory& inventory,
    const scratchbird::transaction::mga::LocalTransactionHorizons& horizons) {
  const auto signature = ReadTransactionInventoryCacheSignature(path);
  if (!signature.has_value()) {
    InvalidateTransactionInventoryCache(path);
    return;
  }
  CachedTransactionInventory cached;
  cached.signature = *signature;
  cached.inventory = inventory;
  cached.horizons = horizons;
  std::lock_guard<std::mutex> guard(TransactionInventoryCacheMutex());
  TransactionInventoryCache()[path] = std::move(cached);
}

Status StoreOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::storage_disk};
}

LocalTransactionStoreResult StoreError(Status status, DiagnosticRecord diagnostic) {
  LocalTransactionStoreResult result;
  result.status = status;
  result.diagnostic = std::move(diagnostic);
  return result;
}

Status StorePageErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::storage_page};
}

LocalTransactionStoreResult StorePageError(std::string diagnostic_code,
                                           std::string message_key,
                                           std::string detail = {}) {
  const Status status = StorePageErrorStatus();
  return StoreError(status,
                    scratchbird::storage::page::MakeTransactionInventoryPageDiagnostic(status,
                                                                                      std::move(diagnostic_code),
                                                                                      std::move(message_key),
                                                                                      std::move(detail)));
}

scratchbird::storage::disk::DiskDevicePolicy InventoryHeaderPolicy(FileDevice* device, u32 page_size) {
  const bool read_only = device != nullptr && device->read_only();
  return scratchbird::storage::disk::DiskDevicePolicy{
      page_size,
      read_only ? scratchbird::storage::disk::DiskAccessMode::read_only
                : scratchbird::storage::disk::DiskAccessMode::read_write,
      read_only ? scratchbird::storage::disk::DiskFsyncPolicy::never
                : scratchbird::storage::disk::DiskFsyncPolicy::after_mutation,
      scratchbird::storage::disk::DiskChecksumPolicy::require_valid,
      scratchbird::storage::disk::UnknownPagePolicy::reject_all,
      true,
      false};
}

u64 CurrentUnixEpochMillisForInventoryPage() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
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

u64 ParseU64Field(const std::string& value) {
  try {
    return static_cast<u64>(std::stoull(value));
  } catch (...) {
    return 0;
  }
}

bool ParseBoolField(const std::string& value) {
  return value == "1" || value == "true";
}

TypedUuid MakeTyped(UuidKind kind, scratchbird::core::platform::Uuid value) {
  TypedUuid typed;
  typed.kind = kind;
  typed.value = value;
  return typed;
}

std::string Sha256Hex(std::string_view payload) {
  const auto digest = core_hash::ComputeSha256Digest(
      reinterpret_cast<const scratchbird::core::platform::byte*>(payload.data()),
      payload.size());
  return digest.ok() ? core_hash::HexLower(digest.digest) : std::string{};
}

std::filesystem::path PublishJournalPathForDevice(const FileDevice* device) {
  if (device == nullptr || device->path().empty()) {
    return {};
  }
  return std::filesystem::path(device->path() + ".sb.txn_publish");
}

std::filesystem::path PublishJournalTempPath(const std::filesystem::path& target) {
  return target.string() + ".tmp";
}

bool ReplacePublishJournalAtomically(const std::filesystem::path& temp_path,
                                     const std::filesystem::path& target_path,
                                     std::string* detail) {
#if defined(_WIN32)
  if (::MoveFileExW(temp_path.wstring().c_str(),
                    target_path.wstring().c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
    return true;
  }
  if (detail != nullptr) {
    *detail = "win32_error=" + std::to_string(::GetLastError());
  }
  return false;
#else
  std::error_code ec;
  std::filesystem::rename(temp_path, target_path, ec);
  if (!ec) {
    return true;
  }
  if (detail != nullptr) {
    *detail = ec.message();
  }
  return false;
#endif
}

std::string SerializeInventorySnapshot(std::string_view label,
                                       const LocalTransactionInventory& inventory) {
  std::ostringstream out;
  out << "snapshot\t" << label << '\t'
      << inventory.next_local_transaction_id << '\t'
      << inventory.entries.size() << '\n';
  for (const TransactionInventoryEntry& entry : inventory.entries) {
    out << "entry\t"
        << entry.identity.local_id.value << '\t'
        << scratchbird::core::uuid::UuidToString(entry.identity.transaction_uuid.value) << '\t'
        << static_cast<u16>(entry.identity.scope) << '\t'
        << static_cast<u16>(entry.state) << '\t'
        << entry.begin_unix_epoch_millis << '\t'
        << entry.final_unix_epoch_millis << '\t'
        << entry.begin_visible_through_local_transaction_id << '\t'
        << (entry.evidence_record_required ? "1" : "0") << '\t'
        << (entry.evidence_record_written ? "1" : "0") << '\t'
        << (entry.rollback_only ? "1" : "0") << '\n';
  }
  out << "endsnapshot\t" << label << '\n';
  return out.str();
}

std::string BuildPublishJournalBody(std::string_view phase,
                                    u64 generation,
                                    const LocalTransactionInventory& old_inventory,
                                    const LocalTransactionInventory& new_inventory) {
  std::ostringstream out;
  out << "SBTXPUB002\n"
      << "phase\t" << phase << '\n'
      << "generation\t" << generation << '\n'
      << "authority\tdurable_transaction_inventory\n"
      << "checksum_algorithm\tsha256\n";
  out << SerializeInventorySnapshot("old", old_inventory);
  out << SerializeInventorySnapshot("new", new_inventory);
  out << "end\n";
  return out.str();
}

std::string BuildPublishJournal(std::string_view phase,
                                u64 generation,
                                const LocalTransactionInventory& old_inventory,
                                const LocalTransactionInventory& new_inventory) {
  const std::string body =
      BuildPublishJournalBody(phase, generation, old_inventory, new_inventory);
  const std::string checksum = Sha256Hex(body);
  if (checksum.empty()) {
    return {};
  }
  return body + "checksum_sha256\t" + checksum + '\n';
}

LocalTransactionStoreResult PersistPublishJournal(FileDevice* device,
                                                  std::string_view phase,
                                                  u64 generation,
                                                  const LocalTransactionInventory& old_inventory,
                                                  const LocalTransactionInventory& new_inventory) {
  const auto target_path = PublishJournalPathForDevice(device);
  if (target_path.empty()) {
    return StorePageError("SB-TXN-INVENTORY-PUBLISH-JOURNAL-PATH-MISSING",
                          "transaction_inventory_publish_journal.path_missing");
  }
  const std::string serialized =
      BuildPublishJournal(phase, generation, old_inventory, new_inventory);
  if (serialized.empty()) {
    return StorePageError("SB-TXN-INVENTORY-PUBLISH-JOURNAL-CHECKSUM-FAILED",
                          "transaction_inventory_publish_journal.checksum_failed");
  }

  std::error_code ec;
  const auto temp_path = PublishJournalTempPath(target_path);
  if (std::filesystem::exists(temp_path, ec)) {
    std::filesystem::remove(temp_path, ec);
    if (ec) {
      return StorePageError("SB-TXN-INVENTORY-PUBLISH-JOURNAL-STALE-TEMP-REMOVE-FAILED",
                            "transaction_inventory_publish_journal.stale_temp_remove_failed",
                            ec.message());
    }
    const auto parent_sync = scratchbird::storage::disk::SyncParentDirectoryPath(temp_path.string());
    if (!parent_sync.ok()) { return StoreError(parent_sync.status, parent_sync.diagnostic); }
  }

  {
    std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
    if (!out) {
      return StorePageError("SB-TXN-INVENTORY-PUBLISH-JOURNAL-WRITE-FAILED",
                            "transaction_inventory_publish_journal.open_failed",
                            temp_path.string());
    }
    out << serialized;
    out.close();
    if (!out) {
      return StorePageError("SB-TXN-INVENTORY-PUBLISH-JOURNAL-WRITE-FAILED",
                            "transaction_inventory_publish_journal.write_failed",
                            temp_path.string());
    }
  }

  const auto file_sync = scratchbird::storage::disk::SyncFilesystemPath(temp_path.string(), true);
  if (!file_sync.ok()) { return StoreError(file_sync.status, file_sync.diagnostic); }

  std::string replace_error;
  if (!ReplacePublishJournalAtomically(temp_path, target_path, &replace_error)) {
    return StorePageError("SB-TXN-INVENTORY-PUBLISH-JOURNAL-RENAME-FAILED",
                          "transaction_inventory_publish_journal.rename_failed",
                          replace_error);
  }

  const auto parent_sync = scratchbird::storage::disk::SyncParentDirectoryPath(target_path.string());
  if (!parent_sync.ok()) { return StoreError(parent_sync.status, parent_sync.diagnostic); }
  return LocalTransactionStoreResult{StoreOkStatus(), {}, {}, {}};
}

struct PublishJournal {
  std::string phase;
  u64 generation = 0;
  LocalTransactionInventory old_inventory;
  LocalTransactionInventory new_inventory;
};

struct PublishJournalLoadResult {
  LocalTransactionStoreResult store;
  bool present = false;
  PublishJournal journal;

  bool ok() const {
    return store.ok();
  }
};

PublishJournalLoadResult PublishJournalAbsent() {
  PublishJournalLoadResult result;
  result.store.status = StoreOkStatus();
  result.present = false;
  return result;
}

PublishJournalLoadResult PublishJournalError(std::string diagnostic_code,
                                             std::string message_key,
                                             std::string detail = {}) {
  PublishJournalLoadResult result;
  result.store = StorePageError(std::move(diagnostic_code),
                                std::move(message_key),
                                std::move(detail));
  result.present = true;
  return result;
}

bool ParseSnapshot(const std::vector<std::string>& lines,
                   std::size_t* index,
                   std::string_view expected_label,
                   LocalTransactionInventory* inventory) {
  if (index == nullptr || inventory == nullptr || *index >= lines.size()) {
    return false;
  }
  const auto header = SplitTabs(lines[*index]);
  if (header.size() != 4 || header[0] != "snapshot" ||
      header[1] != expected_label) {
    return false;
  }
  inventory->next_local_transaction_id = ParseU64Field(header[2]);
  const u64 expected_entries = ParseU64Field(header[3]);
  inventory->entries.clear();
  ++(*index);
  while (*index < lines.size()) {
    const auto parts = SplitTabs(lines[*index]);
    if (parts.size() == 2 && parts[0] == "endsnapshot" &&
        parts[1] == expected_label) {
      ++(*index);
      return inventory->entries.size() == expected_entries &&
             inventory->next_local_transaction_id != 0;
    }
    if (parts.size() != 11 || parts[0] != "entry") {
      return false;
    }
    TransactionInventoryEntry entry;
    entry.identity.local_id = scratchbird::transaction::mga::MakeLocalTransactionId(
        ParseU64Field(parts[1]));
    const auto parsed_uuid =
        scratchbird::core::uuid::ParseTypedUuid(UuidKind::transaction, parts[2]);
    if (!parsed_uuid.ok()) {
      return false;
    }
    entry.identity.transaction_uuid = parsed_uuid.value;
    entry.identity.scope = static_cast<TransactionScope>(ParseU64Field(parts[3]));
    entry.state = static_cast<TransactionState>(ParseU64Field(parts[4]));
    entry.begin_unix_epoch_millis = ParseU64Field(parts[5]);
    entry.final_unix_epoch_millis = ParseU64Field(parts[6]);
    entry.begin_visible_through_local_transaction_id = ParseU64Field(parts[7]);
    entry.evidence_record_required = ParseBoolField(parts[8]);
    entry.evidence_record_written = ParseBoolField(parts[9]);
    entry.rollback_only = ParseBoolField(parts[10]);
    if (!entry.identity.valid()) {
      return false;
    }
    inventory->entries.push_back(entry);
    ++(*index);
  }
  return false;
}

PublishJournalLoadResult ParsePublishJournal(std::string content) {
  const std::string checksum_marker = "\nchecksum_sha256\t";
  const std::size_t checksum_marker_pos = content.find(checksum_marker);
  if (checksum_marker_pos == std::string::npos) {
    return PublishJournalError("SB-TXN-INVENTORY-PUBLISH-RECOVERY-REQUIRED",
                               "transaction_inventory_publish_journal.recovery_required",
                               "partial_journal_write");
  }
  const std::size_t checksum_begin = checksum_marker_pos + checksum_marker.size();
  const std::size_t checksum_end = content.find('\n', checksum_begin);
  if (checksum_end == std::string::npos) {
    return PublishJournalError("SB-TXN-INVENTORY-PUBLISH-RECOVERY-REQUIRED",
                               "transaction_inventory_publish_journal.recovery_required",
                               "partial_journal_checksum");
  }
  const std::string body = content.substr(0, checksum_marker_pos + 1);
  const std::string expected_checksum = content.substr(checksum_begin,
                                                       checksum_end - checksum_begin);
  const std::string actual_checksum = Sha256Hex(body);
  if (actual_checksum.empty() ||
      expected_checksum.size() != 64 ||
      !core_hash::ConstantTimeEqual(expected_checksum, actual_checksum)) {
    return PublishJournalError("SB-TXN-INVENTORY-PUBLISH-JOURNAL-CHECKSUM-MISMATCH",
                               "transaction_inventory_publish_journal.checksum_mismatch");
  }

  std::vector<std::string> lines;
  std::stringstream stream(body);
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty()) {
      lines.push_back(line);
    }
  }
  if (lines.size() < 8 || lines[0] != "SBTXPUB002") {
    return PublishJournalError("SB-TXN-INVENTORY-PUBLISH-JOURNAL-INVALID",
                               "transaction_inventory_publish_journal.invalid_header");
  }

  PublishJournal journal;
  std::size_t index = 1;
  for (; index < lines.size(); ++index) {
    const auto parts = SplitTabs(lines[index]);
    if (parts.empty()) {
      continue;
    }
    if (parts[0] == "snapshot") {
      break;
    }
    if (parts.size() != 2) {
      return PublishJournalError("SB-TXN-INVENTORY-PUBLISH-JOURNAL-INVALID",
                                 "transaction_inventory_publish_journal.invalid_field");
    }
    if (parts[0] == "phase") {
      journal.phase = parts[1];
    } else if (parts[0] == "generation") {
      journal.generation = ParseU64Field(parts[1]);
    } else if (parts[0] == "authority" &&
               parts[1] != "durable_transaction_inventory") {
      return PublishJournalError("SB-TXN-INVENTORY-PUBLISH-JOURNAL-INVALID",
                                 "transaction_inventory_publish_journal.authority_invalid");
    } else if (parts[0] == "checksum_algorithm" && parts[1] != "sha256") {
      return PublishJournalError("SB-TXN-INVENTORY-PUBLISH-JOURNAL-INVALID",
                                 "transaction_inventory_publish_journal.checksum_algorithm_invalid");
    }
  }
  if ((journal.phase != "publishing" && journal.phase != "committed") ||
      journal.generation == 0) {
    return PublishJournalError("SB-TXN-INVENTORY-PUBLISH-JOURNAL-INVALID",
                               "transaction_inventory_publish_journal.phase_or_generation_invalid");
  }
  if (!ParseSnapshot(lines, &index, "old", &journal.old_inventory) ||
      !ParseSnapshot(lines, &index, "new", &journal.new_inventory) ||
      index >= lines.size() || lines[index] != "end") {
    return PublishJournalError("SB-TXN-INVENTORY-PUBLISH-JOURNAL-INVALID",
                               "transaction_inventory_publish_journal.snapshot_invalid");
  }
  PublishJournalLoadResult result;
  result.store.status = StoreOkStatus();
  result.present = true;
  result.journal = std::move(journal);
  return result;
}

PublishJournalLoadResult LoadPublishJournal(FileDevice* device) {
  const auto path = PublishJournalPathForDevice(device);
  if (path.empty()) {
    return PublishJournalAbsent();
  }
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    return PublishJournalAbsent();
  }
  if (ec || !std::filesystem::is_regular_file(path, ec)) {
    return PublishJournalError("SB-TXN-INVENTORY-PUBLISH-JOURNAL-UNREADABLE",
                               "transaction_inventory_publish_journal.unreadable",
                               path.string());
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return PublishJournalError("SB-TXN-INVENTORY-PUBLISH-JOURNAL-UNREADABLE",
                               "transaction_inventory_publish_journal.open_failed",
                               path.string());
  }
  return ParsePublishJournal(
      std::string((std::istreambuf_iterator<char>(input)),
                  std::istreambuf_iterator<char>()));
}

LocalTransactionStoreResult ResultFromRecoveredInventory(
    LocalTransactionInventory inventory) {
  const auto horizons = ComputeLocalTransactionHorizons(inventory);
  if (!horizons.ok()) { return StoreError(horizons.status, horizons.diagnostic); }
  LocalTransactionStoreResult result;
  result.status = StoreOkStatus();
  result.inventory = std::move(inventory);
  result.horizons = horizons.horizons;
  return result;
}

LocalTransactionStoreResult RecoverInventoryFromPublishJournal(
    FileDevice* device,
    const LocalTransactionStoreResult& primary_failure) {
  const auto journal = LoadPublishJournal(device);
  if (!journal.present) {
    return primary_failure;
  }
  if (!journal.ok()) {
    return journal.store;
  }
  if (journal.journal.phase == "committed") {
    return ResultFromRecoveredInventory(journal.journal.new_inventory);
  }
  if (journal.journal.phase == "publishing") {
    return ResultFromRecoveredInventory(journal.journal.old_inventory);
  }
  return StorePageError("SB-TXN-INVENTORY-PUBLISH-RECOVERY-REQUIRED",
                        "transaction_inventory_publish_journal.recovery_required");
}

LocalTransactionStoreResult MakeRootPageContext(FileDevice* device,
                                                u32 page_size,
                                                PageManagerContext* context) {
  if (device == nullptr || context == nullptr) {
    return StorePageError("SB-TXN-INVENTORY-PAGE-CHAIN-INVALID",
                          "transaction_inventory_page.null_device_or_context");
  }
  const auto header = ReadDevicePageHeader(device,
                                           page_size,
                                           kTransactionInventoryPageNumber,
                                           InventoryHeaderPolicy(device, page_size));
  if (!header.ok()) { return StoreError(header.status, header.diagnostic); }
  if (header.classification.page_type != PageType::transaction_inventory) {
    return StorePageError("SB-TXN-INVENTORY-PAGE-TYPE-MISMATCH",
                          "transaction_inventory_page.root_type_mismatch",
                          std::to_string(kTransactionInventoryPageNumber));
  }
  const auto parsed = ParsePageHeader(header.serialized);
  if (!parsed.ok()) { return StoreError(parsed.status, parsed.diagnostic); }
  context->page_size = page_size;
  context->database_uuid = MakeTyped(UuidKind::database, parsed.header.database_uuid);
  context->filespace_uuid = MakeTyped(UuidKind::filespace, parsed.header.filespace_uuid);
  context->cluster_authority_active = false;
  return LocalTransactionStoreResult{StoreOkStatus(), {}, {}, {}};
}

LocalTransactionStoreResult ReadInventoryPageBody(FileDevice* device,
                                                  u32 page_size,
                                                  u64 page_number,
                                                  TransactionInventoryPageBody* body) {
  if (device == nullptr || body == nullptr) {
    return StorePageError("SB-TXN-INVENTORY-PAGE-CHAIN-INVALID",
                          "transaction_inventory_page.null_device_or_body");
  }
  const auto body_offset = CheckedPageBodyOffset(page_size,
                                                 page_number,
                                                 kPageHeaderSerializedBytes);
  if (!body_offset.ok()) { return StoreError(body_offset.status, body_offset.diagnostic); }
  std::vector<scratchbird::core::platform::byte> serialized(page_size - kPageHeaderSerializedBytes, 0);
  const auto read_body = device->ReadAt(body_offset.offset,
                                        serialized.data(),
                                        serialized.size());
  if (!read_body.ok()) { return StoreError(read_body.status, read_body.diagnostic); }
  const auto parsed_body = ParseTransactionInventoryPageBody(serialized, page_number);
  if (!parsed_body.ok()) { return StoreError(parsed_body.status, parsed_body.diagnostic); }
  *body = parsed_body.body;
  return LocalTransactionStoreResult{StoreOkStatus(), {}, {}, {}};
}

LocalTransactionStoreResult ValidateInventoryPageHeader(FileDevice* device,
                                                        u32 page_size,
                                                        u64 page_number) {
  const auto header = ReadDevicePageHeader(device,
                                           page_size,
                                           page_number,
                                           InventoryHeaderPolicy(device, page_size));
  if (!header.ok()) { return StoreError(header.status, header.diagnostic); }
  if (header.classification.page_type != PageType::transaction_inventory) {
    return StorePageError("SB-TXN-INVENTORY-PAGE-TYPE-MISMATCH",
                          "transaction_inventory_page.chain_type_mismatch",
                          std::to_string(page_number));
  }
  return LocalTransactionStoreResult{StoreOkStatus(), {}, {}, {}};
}

LocalTransactionStoreResult LoadInventoryChain(FileDevice* device,
                                               u32 page_size,
                                               LocalTransactionInventory* inventory,
                                               std::vector<u64>* page_chain) {
  if (inventory == nullptr || page_chain == nullptr) {
    return StorePageError("SB-TXN-INVENTORY-PAGE-CHAIN-INVALID",
                          "transaction_inventory_page.null_inventory_or_chain");
  }
  inventory->entries.clear();
  page_chain->clear();
  std::set<u64> visited;
  u64 page_number = kTransactionInventoryPageNumber;
  u64 previous_page_number = 0;
  u64 chain_generation = 0;
  u32 page_count = 0;
  u64 next_local_id = 1;
  while (page_number != 0) {
    if (++page_count > 4096 || visited.count(page_number) != 0) {
      return StorePageError("SB-TXN-INVENTORY-PAGE-CHAIN-INVALID",
                            "transaction_inventory_page.chain_cycle_or_too_long",
                            std::to_string(page_number));
    }
    const auto header = ValidateInventoryPageHeader(device, page_size, page_number);
    if (!header.ok()) { return header; }
    TransactionInventoryPageBody page_body;
    const auto read = ReadInventoryPageBody(device, page_size, page_number, &page_body);
    if (!read.ok()) { return read; }
    visited.insert(page_number);
    if (page_body.previous_page_number != previous_page_number) {
      return StorePageError("SB-TXN-INVENTORY-PAGE-CHAIN-INVALID",
                            "transaction_inventory_page.previous_link_mismatch",
                            std::to_string(page_number));
    }
    if (chain_generation == 0) {
      chain_generation = page_body.inventory_generation;
    } else if (page_body.inventory_generation != chain_generation) {
      return StorePageError("SB-TXN-INVENTORY-PAGE-GENERATION-INVALID",
                            "transaction_inventory_page.chain_generation_mismatch",
                            std::to_string(page_number));
    }
    page_chain->push_back(page_number);
    if (page_number == kTransactionInventoryPageNumber) {
      next_local_id = page_body.inventory.next_local_transaction_id;
    }
    inventory->entries.insert(inventory->entries.end(),
                              page_body.inventory.entries.begin(),
                              page_body.inventory.entries.end());
    previous_page_number = page_number;
    page_number = page_body.next_page_number;
  }
  inventory->next_local_transaction_id = next_local_id;
  return LocalTransactionStoreResult{StoreOkStatus(), {}, {}, {}};
}

LocalTransactionStoreResult WriteInventoryPageHeader(FileDevice* device,
                                                     const PageManagerContext& context,
                                                     u64 page_number,
                                                     u64 generation_seed) {
  const auto page_uuid = GenerateEngineIdentityV7(UuidKind::page,
                                                  CurrentUnixEpochMillisForInventoryPage() + generation_seed);
  if (!page_uuid.ok()) { return StoreError(page_uuid.status, page_uuid.diagnostic); }
  ManagedPageHeaderRequest request;
  request.context = context;
  request.page_type = PageType::transaction_inventory;
  request.page_uuid = page_uuid.value;
  request.page_number = page_number;
  request.page_generation = 1;
  const auto built = BuildManagedPageHeader(request);
  if (!built.ok()) { return StoreError(built.status, built.diagnostic); }
  const auto page_offset = CheckedPageOffset(context.page_size, page_number);
  if (!page_offset.ok()) { return StoreError(page_offset.status, page_offset.diagnostic); }
  const auto write = device->WriteAt(page_offset.offset,
                                     built.serialized.data(),
                                     built.serialized.size());
  if (!write.ok()) { return StoreError(write.status, write.diagnostic); }
  return LocalTransactionStoreResult{StoreOkStatus(), {}, {}, {}};
}

LocalTransactionStoreResult CollectExistingChainOrInitial(FileDevice* device,
                                                          u32 page_size,
                                                          const LocalTransactionInventory& replacement,
                                                          std::vector<u64>* existing_chain) {
  LocalTransactionInventory current;
  const auto loaded = LoadInventoryChain(device, page_size, &current, existing_chain);
  if (loaded.ok()) { return loaded; }
  if (replacement.entries.empty() && replacement.next_local_transaction_id == 1) {
    existing_chain->clear();
    existing_chain->push_back(kTransactionInventoryPageNumber);
    return LocalTransactionStoreResult{StoreOkStatus(), {}, {}, {}};
  }
  return loaded;
}

u64 NextAppendPageNumber(FileDevice* device, u32 page_size) {
  const auto size = device->Size();
  if (!size.ok()) { return kCatalogOverflowFirstPageNumber; }
  const u64 pages = (size.size_bytes + page_size - 1) / page_size;
  return std::max<u64>(kCatalogOverflowFirstPageNumber, pages);
}

}  // namespace

LocalTransactionStoreResult LoadLocalTransactionInventoryFromDatabase(std::string path) {
  if (auto cached = TryLoadCachedTransactionInventory(path)) {
    return *cached;
  }
  FileDevice device;
  const auto open = device.Open(path, FileOpenMode::open_existing);
  if (!open.ok()) { return StoreError(open.status, open.diagnostic); }
  SerializedDatabaseHeader header_bytes{};
  const auto read_header = device.ReadAt(0, header_bytes.data(), header_bytes.size());
  if (!read_header.ok()) { return StoreError(read_header.status, read_header.diagnostic); }
  const auto parsed_header = ParseDatabaseHeader(header_bytes);
  if (!parsed_header.ok()) { return StoreError(parsed_header.status, parsed_header.diagnostic); }
  auto result = LoadLocalTransactionInventoryFromOpenDevice(&device, parsed_header.header.page_size);
  if (result.ok()) {
    RefreshTransactionInventoryCache(path, result.inventory, result.horizons);
  } else {
    InvalidateTransactionInventoryCache(path);
  }
  return result;
}

LocalTransactionStoreResult LoadLocalTransactionInventoryFromOpenDevice(FileDevice* device, u32 page_size) {
  LocalTransactionInventory inventory;
  std::vector<u64> page_chain;
  const auto loaded = LoadInventoryChain(device, page_size, &inventory, &page_chain);
  if (!loaded.ok()) {
    return RecoverInventoryFromPublishJournal(device, loaded);
  }
  const auto horizons = ComputeLocalTransactionHorizons(inventory);
  if (!horizons.ok()) { return StoreError(horizons.status, horizons.diagnostic); }
  LocalTransactionStoreResult result;
  result.status = StoreOkStatus();
  result.inventory = std::move(inventory);
  result.horizons = horizons.horizons;
  return result;
}

LocalTransactionStoreResult PersistLocalTransactionInventoryToDatabase(
    std::string path,
    scratchbird::transaction::mga::LocalTransactionInventory inventory) {
  InvalidateTransactionInventoryCache(path);
  const LocalTransactionInventory cache_inventory = inventory;
  FileDevice device;
  const auto open = device.Open(path, FileOpenMode::open_existing);
  if (!open.ok()) { return StoreError(open.status, open.diagnostic); }
  SerializedDatabaseHeader header_bytes{};
  const auto read_header = device.ReadAt(0, header_bytes.data(), header_bytes.size());
  if (!read_header.ok()) { return StoreError(read_header.status, read_header.diagnostic); }
  const auto parsed_header = ParseDatabaseHeader(header_bytes);
  if (!parsed_header.ok()) { return StoreError(parsed_header.status, parsed_header.diagnostic); }
  auto result = PersistLocalTransactionInventoryToOpenDevice(&device,
                                                            parsed_header.header.page_size,
                                                            std::move(inventory));
  if (result.ok()) {
    RefreshTransactionInventoryCache(path, cache_inventory, result.horizons);
  } else {
    InvalidateTransactionInventoryCache(path);
  }
  return result;
}

LocalTransactionStoreResult PersistLocalTransactionInventoryToOpenDevice(
    FileDevice* device,
    u32 page_size,
    LocalTransactionInventory inventory) {
  PageManagerContext context;
  const auto context_result = MakeRootPageContext(device, page_size, &context);
  if (!context_result.ok()) { return context_result; }

  const auto horizons = ComputeLocalTransactionHorizons(inventory);
  if (!horizons.ok()) { return StoreError(horizons.status, horizons.diagnostic); }

  const u32 capacity = MaxTransactionInventoryEntriesPerPage(page_size);
  if (capacity == 0) {
    return StorePageError("SB-TXN-INVENTORY-PAGE-CAPACITY-INVALID",
                          "transaction_inventory_page.zero_entry_capacity",
                          std::to_string(page_size));
  }

  std::vector<u64> page_chain;
  const auto existing_chain = CollectExistingChainOrInitial(device, page_size, inventory, &page_chain);
  LocalTransactionInventory old_inventory;
  bool old_inventory_loaded_from_journal = false;
  if (!existing_chain.ok()) {
    const auto journal = LoadPublishJournal(device);
    if (!journal.present) {
      return existing_chain;
    }
    if (!journal.ok()) {
      return journal.store;
    }
    old_inventory = journal.journal.phase == "committed"
                        ? journal.journal.new_inventory
                        : journal.journal.old_inventory;
    page_chain.clear();
    page_chain.push_back(kTransactionInventoryPageNumber);
    old_inventory_loaded_from_journal = true;
  }
  if (!old_inventory_loaded_from_journal) {
    std::vector<u64> old_page_chain;
    const auto old_loaded = LoadInventoryChain(device, page_size, &old_inventory, &old_page_chain);
    if (!old_loaded.ok()) {
      if (inventory.entries.empty() && inventory.next_local_transaction_id == 1) {
        old_inventory = scratchbird::transaction::mga::MakeEmptyLocalTransactionInventory();
      } else {
        return old_loaded;
      }
    }
  }

  const u64 required_pages =
      std::max<u64>(1, (static_cast<u64>(inventory.entries.size()) + capacity - 1) / capacity);
  const u64 inventory_generation =
      std::max<u64>(1, inventory.next_local_transaction_id == 0
                           ? 1
                           : inventory.next_local_transaction_id - 1);
  u64 append_page = NextAppendPageNumber(device, page_size);
  while (page_chain.size() < required_pages) {
    while (std::find(page_chain.begin(), page_chain.end(), append_page) != page_chain.end()) {
      ++append_page;
    }
    page_chain.push_back(append_page++);
  }
  page_chain.resize(static_cast<std::size_t>(required_pages));

  const auto publish_begin = PersistPublishJournal(device,
                                                   "publishing",
                                                   inventory_generation,
                                                   old_inventory,
                                                   inventory);
  if (!publish_begin.ok()) { return publish_begin; }

  for (std::size_t page_index = 0; page_index < page_chain.size(); ++page_index) {
    if (page_index > 0) {
      const auto header = WriteInventoryPageHeader(device, context, page_chain[page_index], page_index);
      if (!header.ok()) { return header; }
    }

    LocalTransactionInventory page_inventory;
    page_inventory.next_local_transaction_id = inventory.next_local_transaction_id;
    const std::size_t begin = page_index * capacity;
    const std::size_t end = std::min<std::size_t>(inventory.entries.size(), begin + capacity);
    if (begin < end) {
      page_inventory.entries.insert(page_inventory.entries.end(),
                                    inventory.entries.begin() + begin,
                                    inventory.entries.begin() + end);
    }

    TransactionInventoryPageBody body;
    body.page_number = page_chain[page_index];
    body.previous_page_number = page_index == 0 ? 0 : page_chain[page_index - 1];
    body.next_page_number = (page_index + 1 < page_chain.size()) ? page_chain[page_index + 1] : 0;
    body.inventory_generation = inventory_generation;
    body.inventory = std::move(page_inventory);
    body.horizons = (page_index == 0) ? horizons.horizons : scratchbird::transaction::mga::LocalTransactionHorizons{};
    const auto built = BuildTransactionInventoryPageBody(body, page_size);
    if (!built.ok()) { return StoreError(built.status, built.diagnostic); }
    const auto body_offset = CheckedPageBodyOffset(page_size,
                                                   page_chain[page_index],
                                                   kPageHeaderSerializedBytes);
    if (!body_offset.ok()) { return StoreError(body_offset.status, body_offset.diagnostic); }
    const auto write_body = device->WriteAt(body_offset.offset,
                                            built.serialized.data(),
                                            built.serialized.size());
    if (!write_body.ok()) { return StoreError(write_body.status, write_body.diagnostic); }
  }

  const auto sync = device->Sync();
  if (!sync.ok()) { return StoreError(sync.status, sync.diagnostic); }
  const auto publish_commit = PersistPublishJournal(device,
                                                    "committed",
                                                    inventory_generation,
                                                    old_inventory,
                                                    inventory);
  if (!publish_commit.ok()) { return publish_commit; }
  LocalTransactionStoreResult result;
  result.status = StoreOkStatus();
  result.inventory = std::move(inventory);
  result.horizons = horizons.horizons;
  return result;
}

}  // namespace scratchbird::storage::database
