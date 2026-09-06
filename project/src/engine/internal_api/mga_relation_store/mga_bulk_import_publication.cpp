// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "mga_relation_store/mga_relation_store.hpp"

#include "api_diagnostics.hpp"
#include "hash_digest.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace scratchbird::engine::internal_api {
namespace {

// SEARCH_KEY: SB_ENGINE_MGA_BULK_IMPORT_PUBLICATION_IMPLEMENTATION_AUTHORITY
// Owns durable bulk-import preparation, publication-state evidence, imported
// row evidence, and idempotent replay. These records classify companion stream
// state only: durable MGA transaction inventory remains the sole authority for
// transaction finality and row visibility.
constexpr const char* kRowStoreMagic = "SBMGA1";

EngineApiDiagnostic OkDiagnostic() {
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

std::vector<std::string> SplitTabs(const std::string& line) {
  std::vector<std::string> fields;
  std::size_t start = 0;
  while (start <= line.size()) {
    const auto tab = line.find('\t', start);
    if (tab == std::string::npos) {
      fields.push_back(line.substr(start));
      break;
    }
    fields.push_back(line.substr(start, tab - start));
    start = tab + 1;
  }
  return fields;
}

std::string JoinLine(const std::vector<std::string>& fields) {
  std::string line;
  for (std::size_t index = 0; index < fields.size(); ++index) {
    if (index != 0) line.push_back('\t');
    line += fields[index];
  }
  return line;
}

std::uint64_t ParseU64(const std::string& text,
                       std::uint64_t fallback = 0) {
  if (text.empty()) return fallback;
  try {
    return static_cast<std::uint64_t>(std::stoull(text));
  } catch (...) {
    return fallback;
  }
}

std::vector<std::string> ReadLines(const std::string& path) {
  std::vector<std::string> lines;
  std::ifstream input(path, std::ios::binary);
  if (!input) return lines;
  std::string line;
  while (std::getline(input, line)) lines.push_back(std::move(line));
  return lines;
}

int HexValue(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return 10 + value - 'a';
  if (value >= 'A' && value <= 'F') return 10 + value - 'A';
  return -1;
}

bool BulkImportParseUuid(
    std::string_view text, std::array<std::uint8_t, 16>* bytes = nullptr) {
  if (text.empty()) return false;
  const auto parsed = scratchbird::core::uuid::ParseUuid(std::string(text));
  if (!parsed.ok() || scratchbird::core::uuid::IsNilUuid(parsed.value) ||
      scratchbird::core::uuid::UuidToString(parsed.value) != text) {
    return false;
  }
  if (bytes != nullptr) {
    std::copy(parsed.value.bytes.begin(), parsed.value.bytes.end(),
              bytes->begin());
  }
  return true;
}

bool BulkImportShaNonzero(const MgaBulkImportSha256V1& value) {
  return std::any_of(value.begin(), value.end(),
                     [](std::uint8_t byte) { return byte != 0; });
}

std::string BulkImportShaHex(const MgaBulkImportSha256V1& value) {
  return scratchbird::core::hash::HexLower(value);
}

bool BulkImportParseSha(std::string_view text,
                                MgaBulkImportSha256V1* value) {
  if (value == nullptr || text.size() != value->size() * 2) return false;
  MgaBulkImportSha256V1 parsed{};
  for (std::size_t index = 0; index < parsed.size(); ++index) {
    const int high = HexValue(text[index * 2]);
    const int low = HexValue(text[index * 2 + 1]);
    if (high < 0 || low < 0) return false;
    parsed[index] = static_cast<std::uint8_t>((high << 4) | low);
  }
  *value = parsed;
  return true;
}

void BulkImportAppendU64(std::vector<std::uint8_t>* bytes,
                                 std::uint64_t value) {
  for (std::size_t offset = 0; offset < 8; ++offset) {
    bytes->push_back(
        static_cast<std::uint8_t>((value >> (offset * 8)) & 0xffu));
  }
}

bool BulkImportAppendUuid(std::vector<std::uint8_t>* bytes,
                                  std::string_view uuid) {
  std::array<std::uint8_t, 16> parsed{};
  if (!BulkImportParseUuid(uuid, &parsed)) return false;
  bytes->insert(bytes->end(), parsed.begin(), parsed.end());
  return true;
}

class BulkImportDurableFileLock final {
 public:
  explicit BulkImportDurableFileLock(const std::string& data_path) {
    const std::string path = data_path + ".lock";
#if defined(_WIN32)
    handle_ = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                          OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) return;
    OVERLAPPED overlapped{};
    if (LockFileEx(handle_, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD,
                   &overlapped) == 0) {
      CloseHandle(handle_);
      handle_ = INVALID_HANDLE_VALUE;
      return;
    }
    ok_ = true;
#else
    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd_ < 0) return;
    if (::flock(fd_, LOCK_EX) != 0) {
      ::close(fd_);
      fd_ = -1;
      return;
    }
    ok_ = true;
#endif
  }

  ~BulkImportDurableFileLock() {
#if defined(_WIN32)
    if (handle_ != INVALID_HANDLE_VALUE) {
      OVERLAPPED overlapped{};
      UnlockFileEx(handle_, 0, MAXDWORD, MAXDWORD, &overlapped);
      CloseHandle(handle_);
    }
#else
    if (fd_ >= 0) {
      (void)::flock(fd_, LOCK_UN);
      ::close(fd_);
    }
#endif
  }

  bool ok() const { return ok_; }

 private:
  bool ok_ = false;
#if defined(_WIN32)
  HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
  int fd_ = -1;
#endif
};

enum class BulkImportDurableAppendResultV1 : std::uint8_t {
  ok,
  write_failed,
  fsync_failed,
};

BulkImportDurableAppendResultV1 BulkImportDurableAppend(
    const std::string& path, std::span<const std::uint8_t> encoded,
    bool successor_commit) {
  (void)successor_commit;
  if (encoded.empty()) return BulkImportDurableAppendResultV1::write_failed;
#if defined(_WIN32)
  HANDLE handle = CreateFileA(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return BulkImportDurableAppendResultV1::write_failed;
  }
  std::size_t offset = 0;
  bool write_ok = true;
  while (offset < encoded.size()) {
    DWORD written = 0;
    const DWORD request = static_cast<DWORD>(std::min<std::size_t>(
        encoded.size() - offset, std::numeric_limits<DWORD>::max()));
    if (WriteFile(handle, encoded.data() + offset, request, &written,
                  nullptr) == 0 ||
        written == 0) {
      write_ok = false;
      break;
    }
    offset += written;
  }
  const bool fsync_ok = write_ok && FlushFileBuffers(handle) != 0;
  CloseHandle(handle);
#else
  const int fd =
      ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
  if (fd < 0) return BulkImportDurableAppendResultV1::write_failed;
  std::size_t offset = 0;
  bool write_ok = true;
  while (offset < encoded.size()) {
    const ssize_t written =
        ::write(fd, encoded.data() + offset, encoded.size() - offset);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) {
      write_ok = false;
      break;
    }
    offset += static_cast<std::size_t>(written);
  }
  const bool fsync_ok = write_ok && ::fsync(fd) == 0;
  ::close(fd);
#endif
  if (!write_ok) return BulkImportDurableAppendResultV1::write_failed;
  return fsync_ok ? BulkImportDurableAppendResultV1::ok
                  : BulkImportDurableAppendResultV1::fsync_failed;
}

}  // namespace

namespace {

constexpr std::string_view kBulkImportPublicationKindV1 =
    "BULK_IMPORT_PUBLICATION_V1";
constexpr std::string_view kBulkImportPublicationEvidenceDomainV1 =
    "ScratchBird.BulkImportStreamMgaPublicationRecord.V1";
constexpr std::size_t kBulkImportPublicationFieldCountV1 = 35;

std::mutex& BulkImportPublicationMutexV1() {
  static std::mutex mutex;
  return mutex;
}

std::string BulkImportPublicationPathV1(
    const EngineRequestContext& context) {
  return context.database_path + ".sb.mga_bulk_import_publication.v1";
}

bool BulkImportPublicationUuidV1(std::string_view value) {
  return BulkImportParseUuid(value);
}

bool BulkImportPublicationShaV1(const MgaBulkImportSha256V1& value) {
  return BulkImportShaNonzero(value);
}

void BulkImportPublicationAppendShaV1(
    std::vector<std::uint8_t>* material,
    const MgaBulkImportSha256V1& value) {
  material->insert(material->end(), value.begin(), value.end());
}

MgaBulkImportSha256V1 BulkImportPublicationEvidenceV1(
    const MgaBulkImportPublicationRecordV1& record) {
  std::vector<std::uint8_t> material;
  material.reserve(640);
  material.insert(material.end(), kBulkImportPublicationEvidenceDomainV1.begin(),
                  kBulkImportPublicationEvidenceDomainV1.end());
  if (!BulkImportAppendUuid(&material,
                                    record.durable_publication_uuid)) {
    return {};
  }
  BulkImportAppendU64(
      &material, record.durable_publication_generation);
  BulkImportPublicationAppendShaV1(
      &material, record.recovery_idempotency_key);
  if (!BulkImportAppendUuid(&material, record.stream_uuid)) return {};
  BulkImportAppendU64(&material, record.stream_generation);
  BulkImportPublicationAppendShaV1(&material, record.descriptor_evidence);
  if (!BulkImportAppendUuid(&material,
                                    record.target_relation_uuid)) {
    return {};
  }
  BulkImportAppendU64(&material, record.target_relation_generation);
  if (!BulkImportAppendUuid(&material,
                                    record.owning_transaction_uuid)) {
    return {};
  }
  BulkImportAppendU64(&material,
                              record.owning_local_transaction_id);
  if (!BulkImportAppendUuid(&material,
                                    record.authenticated_receipt_uuid) ||
      !BulkImportAppendUuid(&material, record.statement_uuid)) {
    return {};
  }
  BulkImportAppendU64(&material, record.savepoint_ordinal);
  if (
      !BulkImportAppendUuid(&material, record.mutation_uuid) ||
      !BulkImportAppendUuid(&material, record.bulk_batch_uuid)) {
    return {};
  }
  BulkImportPublicationAppendShaV1(&material, record.content_sha256);
  BulkImportAppendU64(&material, record.total_stream_bytes);
  BulkImportAppendU64(&material, record.chunk_count);
  BulkImportAppendU64(&material, record.input_row_count);
  BulkImportAppendU64(&material, record.affected_rows);
  BulkImportAppendU64(&material, record.rejected_rows);
  BulkImportAppendU64(
      &material, record.imported_row_postcondition_count);
  BulkImportPublicationAppendShaV1(
      &material, record.imported_row_postcondition_sha256);
  BulkImportPublicationAppendShaV1(
      &material, record.normalized_statement_effect_sha256);
  BulkImportPublicationAppendShaV1(
      &material, record.column_descriptor_set_sha256);
  BulkImportPublicationAppendShaV1(
      &material, record.import_policy_bundle_sha256);
  BulkImportPublicationAppendShaV1(
      &material, record.default_descriptor_set_sha256);
  BulkImportPublicationAppendShaV1(&material, record.constraint_set_sha256);
  BulkImportPublicationAppendShaV1(&material, record.trigger_set_sha256);
  BulkImportPublicationAppendShaV1(&material, record.index_set_sha256);
  BulkImportAppendU64(
      &material, record.executor_availability_generation);
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(material);
  return digest.ok() ? digest.digest : MgaBulkImportSha256V1{};
}

bool BulkImportPublicationRecordShapeV1(
    const EngineRequestContext& context,
    const MgaBulkImportPublicationRecordV1& record) {
  return !context.database_path.empty() &&
         BulkImportPublicationUuidV1(record.durable_publication_uuid) &&
         record.durable_publication_generation == 1 &&
         BulkImportPublicationShaV1(record.recovery_idempotency_key) &&
         BulkImportPublicationUuidV1(record.stream_uuid) &&
         record.stream_generation != 0 &&
         BulkImportPublicationShaV1(record.descriptor_evidence) &&
         BulkImportPublicationUuidV1(record.target_relation_uuid) &&
         record.target_relation_generation != 0 &&
         BulkImportPublicationUuidV1(record.owning_transaction_uuid) &&
         record.owning_transaction_uuid == context.transaction_uuid.canonical &&
         record.owning_local_transaction_id != 0 &&
         record.owning_local_transaction_id == context.local_transaction_id &&
         BulkImportPublicationUuidV1(record.authenticated_receipt_uuid) &&
         record.authenticated_receipt_uuid ==
             context.statement_receipt_uuid.canonical &&
         BulkImportPublicationUuidV1(record.statement_uuid) &&
         record.statement_uuid == context.statement_uuid.canonical &&
         record.savepoint_ordinal != 0 &&
         BulkImportPublicationUuidV1(record.mutation_uuid) &&
         BulkImportPublicationUuidV1(record.bulk_batch_uuid) &&
         BulkImportPublicationShaV1(record.content_sha256) &&
         record.total_stream_bytes != 0 && record.chunk_count != 0 &&
         record.input_row_count != 0 &&
         record.affected_rows == record.input_row_count &&
         record.rejected_rows == 0 &&
         record.imported_row_postcondition_count == record.input_row_count &&
         BulkImportPublicationShaV1(
             record.imported_row_postcondition_sha256) &&
         BulkImportPublicationShaV1(
             record.normalized_statement_effect_sha256) &&
         BulkImportPublicationShaV1(
             record.column_descriptor_set_sha256) &&
         BulkImportPublicationShaV1(record.import_policy_bundle_sha256) &&
         BulkImportPublicationShaV1(
             record.default_descriptor_set_sha256) &&
         BulkImportPublicationShaV1(record.constraint_set_sha256) &&
         BulkImportPublicationShaV1(record.trigger_set_sha256) &&
         BulkImportPublicationShaV1(record.index_set_sha256) &&
         record.executor_availability_generation != 0;
}

bool BulkImportPublicationAuthorityEqualV1(
    const MgaBulkImportPublicationRecordV1& left,
    const MgaBulkImportPublicationRecordV1& right) {
  auto normalized_left = left;
  auto normalized_right = right;
  normalized_left.lifecycle = MgaBulkImportPublicationLifecycleV1::prepared;
  normalized_right.lifecycle = MgaBulkImportPublicationLifecycleV1::prepared;
  normalized_left.record_evidence_sha256 = {};
  normalized_right.record_evidence_sha256 = {};
  return normalized_left == normalized_right;
}

std::string BulkImportPublicationLineV1(
    const MgaBulkImportPublicationRecordV1& record) {
  return JoinLine(
      {std::string(kRowStoreMagic),
       std::string(kBulkImportPublicationKindV1),
       std::to_string(static_cast<unsigned>(record.lifecycle)),
       record.durable_publication_uuid,
       std::to_string(record.durable_publication_generation),
       BulkImportShaHex(record.recovery_idempotency_key),
       record.stream_uuid,
       std::to_string(record.stream_generation),
       BulkImportShaHex(record.descriptor_evidence),
       record.target_relation_uuid,
       std::to_string(record.target_relation_generation),
       record.owning_transaction_uuid,
       std::to_string(record.owning_local_transaction_id),
       record.authenticated_receipt_uuid,
       record.statement_uuid,
       std::to_string(record.savepoint_ordinal),
       record.mutation_uuid,
       record.bulk_batch_uuid,
       BulkImportShaHex(record.content_sha256),
       std::to_string(record.total_stream_bytes),
       std::to_string(record.chunk_count),
       std::to_string(record.input_row_count),
       std::to_string(record.affected_rows),
       std::to_string(record.rejected_rows),
       std::to_string(record.imported_row_postcondition_count),
       BulkImportShaHex(record.imported_row_postcondition_sha256),
       BulkImportShaHex(record.normalized_statement_effect_sha256),
       BulkImportShaHex(record.column_descriptor_set_sha256),
       BulkImportShaHex(record.import_policy_bundle_sha256),
       BulkImportShaHex(record.default_descriptor_set_sha256),
       BulkImportShaHex(record.constraint_set_sha256),
       BulkImportShaHex(record.trigger_set_sha256),
       BulkImportShaHex(record.index_set_sha256),
       std::to_string(record.executor_availability_generation),
       BulkImportShaHex(record.record_evidence_sha256)});
}

bool BulkImportPublicationParseV1(
    const std::string& line, MgaBulkImportPublicationRecordV1* record) {
  if (record == nullptr) return false;
  const auto fields = SplitTabs(line);
  if (fields.size() != kBulkImportPublicationFieldCountV1 ||
      fields[0] != kRowStoreMagic ||
      fields[1] != kBulkImportPublicationKindV1) {
    return false;
  }
  const auto lifecycle = ParseU64(fields[2]);
  if (lifecycle < 1 || lifecycle > 3) return false;
  MgaBulkImportPublicationRecordV1 parsed;
  parsed.lifecycle =
      static_cast<MgaBulkImportPublicationLifecycleV1>(lifecycle);
  parsed.durable_publication_uuid = fields[3];
  parsed.durable_publication_generation = ParseU64(fields[4]);
  parsed.stream_uuid = fields[6];
  parsed.stream_generation = ParseU64(fields[7]);
  parsed.target_relation_uuid = fields[9];
  parsed.target_relation_generation = ParseU64(fields[10]);
  parsed.owning_transaction_uuid = fields[11];
  parsed.owning_local_transaction_id = ParseU64(fields[12]);
  parsed.authenticated_receipt_uuid = fields[13];
  parsed.statement_uuid = fields[14];
  parsed.savepoint_ordinal = ParseU64(fields[15]);
  parsed.mutation_uuid = fields[16];
  parsed.bulk_batch_uuid = fields[17];
  parsed.total_stream_bytes = ParseU64(fields[19]);
  parsed.chunk_count = ParseU64(fields[20]);
  parsed.input_row_count = ParseU64(fields[21]);
  parsed.affected_rows = ParseU64(fields[22]);
  parsed.rejected_rows = ParseU64(fields[23]);
  parsed.imported_row_postcondition_count = ParseU64(fields[24]);
  parsed.executor_availability_generation = ParseU64(fields[33]);
  if (!BulkImportParseSha(fields[5],
                                  &parsed.recovery_idempotency_key) ||
      !BulkImportParseSha(fields[8],
                                  &parsed.descriptor_evidence) ||
      !BulkImportParseSha(fields[18], &parsed.content_sha256) ||
      !BulkImportParseSha(
          fields[25], &parsed.imported_row_postcondition_sha256) ||
      !BulkImportParseSha(
          fields[26], &parsed.normalized_statement_effect_sha256) ||
      !BulkImportParseSha(
          fields[27], &parsed.column_descriptor_set_sha256) ||
      !BulkImportParseSha(
          fields[28], &parsed.import_policy_bundle_sha256) ||
      !BulkImportParseSha(
          fields[29], &parsed.default_descriptor_set_sha256) ||
      !BulkImportParseSha(fields[30],
                                  &parsed.constraint_set_sha256) ||
      !BulkImportParseSha(fields[31],
                                  &parsed.trigger_set_sha256) ||
      !BulkImportParseSha(fields[32], &parsed.index_set_sha256) ||
      !BulkImportParseSha(fields[34],
                                  &parsed.record_evidence_sha256) ||
      BulkImportPublicationEvidenceV1(parsed) !=
          parsed.record_evidence_sha256) {
    return false;
  }
  *record = std::move(parsed);
  return true;
}

MgaBulkImportPublicationResultV1 BulkImportPublicationFailureV1(
    std::string code, std::string key, std::string detail) {
  MgaBulkImportPublicationResultV1 result;
  result.diagnostic = MakeEngineApiDiagnostic(
      std::move(code), std::move(key), std::move(detail), true);
  return result;
}

bool BulkImportPublicationAppendV1(
    const EngineRequestContext& context,
    const MgaBulkImportPublicationRecordV1& record) {
  std::string line = BulkImportPublicationLineV1(record);
  if (line.empty()) return false;
  line.push_back('\n');
  const auto* begin = reinterpret_cast<const std::uint8_t*>(line.data());
  return BulkImportDurableAppend(
             BulkImportPublicationPathV1(context),
             std::span<const std::uint8_t>(begin, line.size()), false) ==
         BulkImportDurableAppendResultV1::ok;
}

MgaBulkImportPublicationResultV1 BulkImportPublicationLoadV1(
    const EngineRequestContext& context,
    const MgaBulkImportSha256V1& recovery_idempotency_key) {
  MgaBulkImportPublicationResultV1 result;
  if (context.database_path.empty() ||
      !BulkImportPublicationShaV1(recovery_idempotency_key)) {
    return BulkImportPublicationFailureV1(
        "SBLR.OPERAND_INVALID",
        "mga.bulk_import.publication_lookup_invalid",
        "database path and recovery key are required");
  }
  std::optional<MgaBulkImportPublicationRecordV1> found;
  for (const auto& line : ReadLines(BulkImportPublicationPathV1(context))) {
    MgaBulkImportPublicationRecordV1 parsed;
    if (!BulkImportPublicationParseV1(line, &parsed)) {
      return BulkImportPublicationFailureV1(
          "BULK.IMPORT.RECOVERY_CONFLICT",
          "mga.bulk_import.publication_record_corrupt",
          "one durable publication record failed canonical validation");
    }
    if (parsed.recovery_idempotency_key != recovery_idempotency_key) continue;
    if (found.has_value() &&
        !BulkImportPublicationAuthorityEqualV1(*found, parsed)) {
      return BulkImportPublicationFailureV1(
          "BULK.IMPORT.RECOVERY_CONFLICT",
          "mga.bulk_import.publication_record_forked",
          "the recovery key has incompatible durable publication records");
    }
    if (!found.has_value() ||
        static_cast<unsigned>(parsed.lifecycle) >=
            static_cast<unsigned>(found->lifecycle)) {
      found = std::move(parsed);
    }
  }
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  if (found.has_value()) {
    result.found = true;
    result.record = std::move(*found);
  }
  return result;
}

constexpr std::string_view kBulkImportImportedRowKindV1 =
    "BULK_IMPORT_IMPORTED_ROW_V1";
constexpr std::string_view kBulkImportImportedRowEvidenceDomainV1 =
    "ScratchBird.BulkImportStreamImportedRowEvent.V1";
constexpr std::size_t kBulkImportImportedRowFieldCountV1 = 23;

std::string BulkImportImportedRowPathV1(
    const EngineRequestContext& context) {
  return context.database_path + ".sb.mga_bulk_import_imported_rows.v1";
}

MgaBulkImportSha256V1 BulkImportImportedRowEvidenceV1(
    const MgaBulkImportImportedRowEventV1& event) {
  std::vector<std::uint8_t> material;
  material.reserve(560);
  material.insert(material.end(), kBulkImportImportedRowEvidenceDomainV1.begin(),
                  kBulkImportImportedRowEvidenceDomainV1.end());
  material.insert(material.end(), {1, 0, 0, 0});
  if (!BulkImportAppendUuid(&material,
                                    event.durable_publication_uuid)) {
    return {};
  }
  BulkImportAppendU64(&material,
                              event.durable_publication_generation);
  BulkImportPublicationAppendShaV1(
      &material, event.recovery_idempotency_key);
  if (!BulkImportAppendUuid(&material, event.mutation_uuid) ||
      !BulkImportAppendUuid(&material, event.bulk_batch_uuid) ||
      !BulkImportAppendUuid(&material,
                                    event.owning_transaction_uuid)) {
    return {};
  }
  BulkImportAppendU64(&material,
                              event.owning_local_transaction_id);
  if (!BulkImportAppendUuid(&material, event.statement_uuid)) {
    return {};
  }
  BulkImportAppendU64(&material, event.savepoint_ordinal);
  if (!BulkImportAppendUuid(&material,
                                    event.target_relation_uuid)) {
    return {};
  }
  BulkImportAppendU64(&material, event.target_relation_generation);
  BulkImportAppendU64(&material, event.import_ordinal);
  if (!BulkImportAppendUuid(&material, event.row_uuid) ||
      !BulkImportAppendUuid(&material, event.row_version_uuid) ||
      !BulkImportAppendUuid(&material, event.row_image_uuid)) {
    return {};
  }
  BulkImportAppendU64(
      &material, event.row_image_metadata_generation);
  BulkImportPublicationAppendShaV1(&material, event.row_image_domain_hash);
  BulkImportPublicationAppendShaV1(&material, event.row_image_value_hash);
  BulkImportPublicationAppendShaV1(
      &material, event.column_descriptor_set_sha256);
  BulkImportPublicationAppendShaV1(
      &material, event.canonical_typed_field_vector_sha256);
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(material);
  return digest.ok() ? digest.digest : MgaBulkImportSha256V1{};
}

bool BulkImportImportedRowShapeV1(
    const EngineRequestContext& context,
    const MgaBulkImportImportedRowEventV1& event) {
  return BulkImportPublicationUuidV1(event.durable_publication_uuid) &&
         event.durable_publication_generation == 1 &&
         BulkImportPublicationShaV1(event.recovery_idempotency_key) &&
         BulkImportPublicationUuidV1(event.mutation_uuid) &&
         BulkImportPublicationUuidV1(event.bulk_batch_uuid) &&
         BulkImportPublicationUuidV1(event.owning_transaction_uuid) &&
         event.owning_transaction_uuid == context.transaction_uuid.canonical &&
         event.owning_local_transaction_id == context.local_transaction_id &&
         event.owning_local_transaction_id != 0 &&
         BulkImportPublicationUuidV1(event.statement_uuid) &&
         event.statement_uuid == context.statement_uuid.canonical &&
         event.savepoint_ordinal != 0 &&
         BulkImportPublicationUuidV1(event.target_relation_uuid) &&
         event.target_relation_generation != 0 && event.import_ordinal != 0 &&
         BulkImportPublicationUuidV1(event.row_uuid) &&
         BulkImportPublicationUuidV1(event.row_version_uuid) &&
         BulkImportPublicationUuidV1(event.row_image_uuid) &&
         event.row_uuid != event.row_version_uuid &&
         event.row_uuid != event.row_image_uuid &&
         event.row_version_uuid != event.row_image_uuid &&
         event.row_image_metadata_generation != 0 &&
         BulkImportPublicationShaV1(event.row_image_domain_hash) &&
         BulkImportPublicationShaV1(event.row_image_value_hash) &&
         BulkImportPublicationShaV1(
             event.column_descriptor_set_sha256) &&
         BulkImportPublicationShaV1(
             event.canonical_typed_field_vector_sha256) &&
         event.row_image_value_hash ==
             event.canonical_typed_field_vector_sha256;
}

std::string BulkImportImportedRowLineV1(
    const MgaBulkImportImportedRowEventV1& event) {
  return JoinLine(
      {std::string(kRowStoreMagic), std::string(kBulkImportImportedRowKindV1),
       event.durable_publication_uuid,
       std::to_string(event.durable_publication_generation),
       BulkImportShaHex(event.recovery_idempotency_key),
       event.mutation_uuid, event.bulk_batch_uuid,
       event.owning_transaction_uuid,
       std::to_string(event.owning_local_transaction_id), event.statement_uuid,
       std::to_string(event.savepoint_ordinal), event.target_relation_uuid,
       std::to_string(event.target_relation_generation),
       std::to_string(event.import_ordinal), event.row_uuid,
       event.row_version_uuid, event.row_image_uuid,
       std::to_string(event.row_image_metadata_generation),
       BulkImportShaHex(event.row_image_domain_hash),
       BulkImportShaHex(event.row_image_value_hash),
       BulkImportShaHex(event.column_descriptor_set_sha256),
       BulkImportShaHex(
           event.canonical_typed_field_vector_sha256),
       BulkImportShaHex(event.event_evidence_sha256)});
}

bool BulkImportImportedRowParseV1(
    const std::string& line, MgaBulkImportImportedRowEventV1* event) {
  if (event == nullptr) return false;
  const auto fields = SplitTabs(line);
  if (fields.size() != kBulkImportImportedRowFieldCountV1 ||
      fields[0] != kRowStoreMagic ||
      fields[1] != kBulkImportImportedRowKindV1) {
    return false;
  }
  MgaBulkImportImportedRowEventV1 parsed;
  parsed.durable_publication_uuid = fields[2];
  parsed.durable_publication_generation = ParseU64(fields[3]);
  parsed.mutation_uuid = fields[5];
  parsed.bulk_batch_uuid = fields[6];
  parsed.owning_transaction_uuid = fields[7];
  parsed.owning_local_transaction_id = ParseU64(fields[8]);
  parsed.statement_uuid = fields[9];
  parsed.savepoint_ordinal = ParseU64(fields[10]);
  parsed.target_relation_uuid = fields[11];
  parsed.target_relation_generation = ParseU64(fields[12]);
  parsed.import_ordinal = ParseU64(fields[13]);
  parsed.row_uuid = fields[14];
  parsed.row_version_uuid = fields[15];
  parsed.row_image_uuid = fields[16];
  parsed.row_image_metadata_generation = ParseU64(fields[17]);
  if (!BulkImportParseSha(fields[4],
                                  &parsed.recovery_idempotency_key) ||
      !BulkImportParseSha(fields[18],
                                  &parsed.row_image_domain_hash) ||
      !BulkImportParseSha(fields[19],
                                  &parsed.row_image_value_hash) ||
      !BulkImportParseSha(
          fields[20], &parsed.column_descriptor_set_sha256) ||
      !BulkImportParseSha(
          fields[21], &parsed.canonical_typed_field_vector_sha256) ||
      !BulkImportParseSha(fields[22],
                                  &parsed.event_evidence_sha256) ||
      BulkImportImportedRowEvidenceV1(parsed) !=
          parsed.event_evidence_sha256) {
    return false;
  }
  *event = std::move(parsed);
  return true;
}

MgaBulkImportImportedRowEventResultV1 BulkImportImportedRowFailureV1(
    std::string code, std::string key, std::string detail) {
  MgaBulkImportImportedRowEventResultV1 result;
  result.diagnostic = MakeEngineApiDiagnostic(
      std::move(code), std::move(key), std::move(detail), true);
  return result;
}

MgaBulkImportImportedRowEventResultV1 BulkImportImportedRowLoadV1(
    const EngineRequestContext& context,
    const MgaBulkImportSha256V1& recovery_idempotency_key) {
  MgaBulkImportImportedRowEventResultV1 result;
  if (context.database_path.empty() ||
      !BulkImportPublicationShaV1(recovery_idempotency_key)) {
    return BulkImportImportedRowFailureV1(
        "SBLR.OPERAND_INVALID", "mga.bulk_import.imported_row_lookup_invalid",
        "database path and recovery key are required");
  }
  for (const auto& line : ReadLines(BulkImportImportedRowPathV1(context))) {
    MgaBulkImportImportedRowEventV1 parsed;
    if (!BulkImportImportedRowParseV1(line, &parsed)) {
      return BulkImportImportedRowFailureV1(
          "BULK.IMPORT.RECOVERY_CONFLICT",
          "mga.bulk_import.imported_row_event_corrupt",
          "one imported-row event failed canonical validation");
    }
    if (parsed.recovery_idempotency_key == recovery_idempotency_key) {
      result.events.push_back(std::move(parsed));
    }
  }
  std::sort(result.events.begin(), result.events.end(),
            [](const auto& left, const auto& right) {
              return left.import_ordinal < right.import_ordinal;
            });
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  return result;
}

}  // namespace

MgaBulkImportPublicationResultV1 PrepareMgaBulkImportPublicationV1(
    const EngineRequestContext& context,
    const MgaBulkImportPublicationRecordV1& requested) {
  std::lock_guard<std::mutex> guard(BulkImportPublicationMutexV1());
  auto record = requested;
  record.lifecycle = MgaBulkImportPublicationLifecycleV1::prepared;
  record.record_evidence_sha256 = BulkImportPublicationEvidenceV1(record);
  if (!BulkImportPublicationRecordShapeV1(context, record) ||
      !BulkImportPublicationShaV1(record.record_evidence_sha256)) {
    return BulkImportPublicationFailureV1(
        "SBLR.OPERAND_INVALID",
        "mga.bulk_import.publication_record_invalid",
        "the prepared publication authority is incomplete");
  }
  BulkImportDurableFileLock file_lock(BulkImportPublicationPathV1(context));
  if (!file_lock.ok()) {
    return BulkImportPublicationFailureV1(
        "BULK.IMPORT.RECOVERY_CONFLICT",
        "mga.bulk_import.publication_lock_failed",
        "the durable publication lock could not be acquired");
  }
  auto existing = BulkImportPublicationLoadV1(
      context, record.recovery_idempotency_key);
  if (!existing.ok) return existing;
  if (existing.found) {
    if (!BulkImportPublicationAuthorityEqualV1(existing.record, record)) {
      return BulkImportPublicationFailureV1(
          "BULK.IMPORT.RECOVERY_CONFLICT",
          "mga.bulk_import.publication_identity_conflict",
          "the recovery key is already bound to another publication");
    }
    existing.replayed = true;
    return existing;
  }
  if (!BulkImportPublicationAppendV1(context, record)) {
    return BulkImportPublicationFailureV1(
        "BULK.IMPORT.ABORTED",
        "mga.bulk_import.publication_prepare_failed",
        "the prepared publication record was not durable");
  }
  MgaBulkImportPublicationResultV1 result;
  result.ok = true;
  result.found = true;
  result.record = std::move(record);
  result.diagnostic = OkDiagnostic();
  return result;
}

MgaBulkImportPublicationResultV1 PublishMgaBulkImportPublicationV1(
    const EngineRequestContext& context,
    const MgaBulkImportPublicationRecordV1& prepared_record) {
  std::lock_guard<std::mutex> guard(BulkImportPublicationMutexV1());
  BulkImportDurableFileLock file_lock(BulkImportPublicationPathV1(context));
  if (!file_lock.ok()) {
    return BulkImportPublicationFailureV1(
        "BULK.IMPORT.RECOVERY_CONFLICT",
        "mga.bulk_import.publication_lock_failed",
        "the durable publication lock could not be acquired");
  }
  auto existing = BulkImportPublicationLoadV1(
      context, prepared_record.recovery_idempotency_key);
  if (!existing.ok || !existing.found) {
    return existing.ok
               ? BulkImportPublicationFailureV1(
                     "BULK.IMPORT.RECOVERY_CONFLICT",
                     "mga.bulk_import.publication_prepare_missing",
                     "publication requires the exact prepared record")
               : existing;
  }
  if (!BulkImportPublicationAuthorityEqualV1(existing.record,
                                              prepared_record)) {
    return BulkImportPublicationFailureV1(
        "BULK.IMPORT.RECOVERY_CONFLICT",
        "mga.bulk_import.publication_identity_conflict",
        "the prepared publication authority changed");
  }
  if (existing.record.lifecycle ==
      MgaBulkImportPublicationLifecycleV1::published_uncommitted) {
    existing.replayed = true;
    return existing;
  }
  if (existing.record.lifecycle !=
      MgaBulkImportPublicationLifecycleV1::prepared) {
    return BulkImportPublicationFailureV1(
        "BULK.IMPORT.RECOVERY_CONFLICT",
        "mga.bulk_import.publication_lifecycle_invalid",
        "an aborted publication cannot be promoted");
  }
  auto published = existing.record;
  published.lifecycle =
      MgaBulkImportPublicationLifecycleV1::published_uncommitted;
  if (!BulkImportPublicationAppendV1(context, published)) {
    return BulkImportPublicationFailureV1(
        "BULK.IMPORT.RECOVERY_CONFLICT",
        "mga.bulk_import.publication_publish_failed",
        "row mutation crossed its barrier but the publication successor was not durable");
  }
  MgaBulkImportPublicationResultV1 result;
  result.ok = true;
  result.found = true;
  result.record = std::move(published);
  result.diagnostic = OkDiagnostic();
  return result;
}

MgaBulkImportPublicationResultV1 AbortMgaBulkImportPublicationV1(
    const EngineRequestContext& context,
    const MgaBulkImportPublicationRecordV1& prepared_record) {
  std::lock_guard<std::mutex> guard(BulkImportPublicationMutexV1());
  BulkImportDurableFileLock file_lock(BulkImportPublicationPathV1(context));
  if (!file_lock.ok()) {
    return BulkImportPublicationFailureV1(
        "BULK.IMPORT.RECOVERY_CONFLICT",
        "mga.bulk_import.publication_lock_failed",
        "the durable publication lock could not be acquired");
  }
  auto existing = BulkImportPublicationLoadV1(
      context, prepared_record.recovery_idempotency_key);
  if (!existing.ok || !existing.found) return existing;
  if (!BulkImportPublicationAuthorityEqualV1(existing.record,
                                              prepared_record)) {
    return BulkImportPublicationFailureV1(
        "BULK.IMPORT.RECOVERY_CONFLICT",
        "mga.bulk_import.publication_identity_conflict",
        "the prepared publication authority changed");
  }
  if (existing.record.lifecycle == MgaBulkImportPublicationLifecycleV1::aborted) {
    existing.replayed = true;
    return existing;
  }
  if (existing.record.lifecycle ==
      MgaBulkImportPublicationLifecycleV1::published_uncommitted) {
    return BulkImportPublicationFailureV1(
        "BULK.IMPORT.RECOVERY_CONFLICT",
        "mga.bulk_import.publication_already_published",
        "a published mutation cannot be aborted");
  }
  auto aborted = existing.record;
  aborted.lifecycle = MgaBulkImportPublicationLifecycleV1::aborted;
  if (!BulkImportPublicationAppendV1(context, aborted)) {
    return BulkImportPublicationFailureV1(
        "BULK.IMPORT.RECOVERY_CONFLICT",
        "mga.bulk_import.publication_abort_failed",
        "the abort successor was not durable");
  }
  MgaBulkImportPublicationResultV1 result;
  result.ok = true;
  result.found = true;
  result.record = std::move(aborted);
  result.diagnostic = OkDiagnostic();
  return result;
}

MgaBulkImportPublicationResultV1 RecoverMgaBulkImportPublicationV1(
    const EngineRequestContext& context,
    const MgaBulkImportSha256V1& recovery_idempotency_key) {
  std::lock_guard<std::mutex> guard(BulkImportPublicationMutexV1());
  BulkImportDurableFileLock file_lock(BulkImportPublicationPathV1(context));
  if (!file_lock.ok()) {
    return BulkImportPublicationFailureV1(
        "BULK.IMPORT.RECOVERY_CONFLICT",
        "mga.bulk_import.publication_lock_failed",
        "the durable publication lock could not be acquired");
  }
  return BulkImportPublicationLoadV1(context, recovery_idempotency_key);
}

MgaBulkImportImportedRowEventResultV1 StoreMgaBulkImportImportedRowEventsV1(
    const EngineRequestContext& context,
    const std::vector<MgaBulkImportImportedRowEventV1>& requested) {
  std::lock_guard<std::mutex> guard(BulkImportPublicationMutexV1());
  if (requested.empty()) {
    return BulkImportImportedRowFailureV1(
        "SBLR.OPERAND_INVALID", "mga.bulk_import.imported_rows_empty",
        "at least one imported-row event is required");
  }
  auto events = requested;
  for (std::size_t index = 0; index < events.size(); ++index) {
    auto& event = events[index];
    event.event_evidence_sha256 = BulkImportImportedRowEvidenceV1(event);
    if (event.import_ordinal != index + 1 ||
        !BulkImportImportedRowShapeV1(context, event) ||
        !BulkImportPublicationShaV1(event.event_evidence_sha256) ||
        (index != 0 &&
         (event.recovery_idempotency_key !=
              events.front().recovery_idempotency_key ||
          event.durable_publication_uuid !=
              events.front().durable_publication_uuid ||
          event.mutation_uuid != events.front().mutation_uuid ||
          event.bulk_batch_uuid != events.front().bulk_batch_uuid ||
          event.target_relation_uuid !=
              events.front().target_relation_uuid))) {
      return BulkImportImportedRowFailureV1(
          "SBLR.OPERAND_INVALID",
          "mga.bulk_import.imported_row_event_invalid",
          "the ordered imported-row event set is incomplete or inconsistent");
    }
  }
  std::set<std::string> identities;
  for (const auto& event : events) {
    if (!identities.insert(event.row_uuid).second ||
        !identities.insert(event.row_version_uuid).second ||
        !identities.insert(event.row_image_uuid).second) {
      return BulkImportImportedRowFailureV1(
          "BULK.IMPORT.RECOVERY_CONFLICT",
          "mga.bulk_import.imported_row_identity_collision",
          "row, version, and image identities must be globally distinct in the event set");
    }
  }
  BulkImportDurableFileLock file_lock(BulkImportImportedRowPathV1(context));
  if (!file_lock.ok()) {
    return BulkImportImportedRowFailureV1(
        "BULK.IMPORT.RECOVERY_CONFLICT",
        "mga.bulk_import.imported_row_lock_failed",
        "the durable imported-row event lock could not be acquired");
  }
  auto existing = BulkImportImportedRowLoadV1(
      context, events.front().recovery_idempotency_key);
  if (!existing.ok) return existing;
  if (!existing.events.empty()) {
    if (existing.events != events) {
      return BulkImportImportedRowFailureV1(
          "BULK.IMPORT.RECOVERY_CONFLICT",
          "mga.bulk_import.imported_row_event_conflict",
          "the recovery key already owns another imported-row event set");
    }
    existing.replayed = true;
    return existing;
  }
  std::string payload;
  for (const auto& event : events) {
    std::string line = BulkImportImportedRowLineV1(event);
    if (line.empty()) {
      return BulkImportImportedRowFailureV1(
          "BULK.IMPORT.ABORTED",
          "mga.bulk_import.imported_row_encode_failed",
          "the imported-row event set could not be encoded");
    }
    payload.append(line);
    payload.push_back('\n');
  }
  const auto* begin =
      reinterpret_cast<const std::uint8_t*>(payload.data());
  if (BulkImportDurableAppend(
          BulkImportImportedRowPathV1(context),
          std::span<const std::uint8_t>(begin, payload.size()), false) !=
      BulkImportDurableAppendResultV1::ok) {
    return BulkImportImportedRowFailureV1(
        "BULK.IMPORT.ABORTED",
        "mga.bulk_import.imported_row_persist_failed",
        "the imported-row event set was not durable");
  }
  MgaBulkImportImportedRowEventResultV1 result;
  result.ok = true;
  result.events = std::move(events);
  result.diagnostic = OkDiagnostic();
  return result;
}

MgaBulkImportImportedRowEventResultV1 RecoverMgaBulkImportImportedRowEventsV1(
    const EngineRequestContext& context,
    const MgaBulkImportSha256V1& recovery_idempotency_key) {
  std::lock_guard<std::mutex> guard(BulkImportPublicationMutexV1());
  BulkImportDurableFileLock file_lock(BulkImportImportedRowPathV1(context));
  if (!file_lock.ok()) {
    return BulkImportImportedRowFailureV1(
        "BULK.IMPORT.RECOVERY_CONFLICT",
        "mga.bulk_import.imported_row_lock_failed",
        "the durable imported-row event lock could not be acquired");
  }
  auto result =
      BulkImportImportedRowLoadV1(context, recovery_idempotency_key);
  if (!result.ok) return result;
  for (std::size_t index = 0; index < result.events.size(); ++index) {
    if (result.events[index].import_ordinal != index + 1 ||
        !BulkImportImportedRowShapeV1(context, result.events[index])) {
      return BulkImportImportedRowFailureV1(
          "BULK.IMPORT.RECOVERY_CONFLICT",
          "mga.bulk_import.imported_row_event_invalid",
          "the durable imported-row event set failed authority validation");
    }
  }
  return result;
}

MgaBulkImportRowLineageResultV1 ProbeMgaBulkImportRowIdentityLineageV1(
    const EngineRequestContext& context, const std::string& table_uuid,
    const std::string& row_uuid) {
  MgaBulkImportRowLineageResultV1 result;
  if (context.database_path.empty() || !BulkImportPublicationUuidV1(table_uuid) ||
      !BulkImportPublicationUuidV1(row_uuid)) {
    result.diagnostic = MakeEngineApiDiagnostic(
        "SBLR.OPERAND_INVALID", "mga.bulk_import.row_lineage_lookup_invalid",
        "database, table, and row identities are required", true);
    return result;
  }
  auto loaded =
      LoadMgaRelationStoreRowsForPointLookup(context, table_uuid, row_uuid);
  if (!loaded.ok) {
    result.diagnostic = MakeEngineApiDiagnostic(
        "BULK.IMPORT.RECOVERY_CONFLICT",
        "mga.bulk_import.row_lineage_decode_failed",
        "the retained relation row lineage could not be decoded", true);
    return result;
  }
  auto rows = std::move(loaded.state.row_versions);
  rows.erase(std::remove_if(rows.begin(), rows.end(), [&](const auto& row) {
               return row.table_uuid != table_uuid || row.row_uuid != row_uuid;
             }),
             rows.end());
  std::sort(rows.begin(), rows.end(), [](const auto& left, const auto& right) {
    return std::tie(left.event_sequence, left.version_uuid) <
           std::tie(right.event_sequence, right.version_uuid);
  });
  rows.erase(std::unique(rows.begin(), rows.end(),
                         [](const auto& left, const auto& right) {
                           return left.event_sequence == right.event_sequence &&
                                  left.version_uuid == right.version_uuid;
                         }),
             rows.end());
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  result.versions = std::move(rows);
  return result;
}

}  // namespace scratchbird::engine::internal_api
