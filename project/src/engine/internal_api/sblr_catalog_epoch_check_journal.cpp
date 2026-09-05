// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "sblr_catalog_epoch_check_journal.hpp"

#include "api_diagnostics.hpp"
#include "hash_digest.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace scratchbird::engine::internal_api {
namespace {

constexpr std::size_t kHeaderBytes = 288;
constexpr std::size_t kResultBytes = 192;
constexpr std::string_view kRecordDomain =
    "ScratchBird.CatalogEpochCheckJournalRecord.V1";

std::mutex g_journal_mutex;
std::atomic<std::uint64_t> g_temp_ordinal{1};

EngineApiDiagnostic Diagnostic(std::string code, std::string key,
                               std::string detail = {}) {
  return MakeEngineApiDiagnostic(std::move(code), std::move(key),
                                 std::move(detail));
}

EngineApiDiagnostic OkDiagnostic() {
  return MakeEngineApiDiagnostic("OK", "ok", {}, false);
}

template <std::size_t N>
bool NonZero(const std::array<std::uint8_t, N>& value) {
  return std::any_of(value.begin(), value.end(),
                     [](std::uint8_t byte) { return byte != 0; });
}

template <std::size_t N>
void Put(std::vector<std::uint8_t>* bytes,
         const std::array<std::uint8_t, N>& value) {
  bytes->insert(bytes->end(), value.begin(), value.end());
}

void PutLe(std::vector<std::uint8_t>* bytes, std::uint64_t value,
           std::size_t count) {
  for (std::size_t index = 0; index != count; ++index) {
    bytes->push_back(static_cast<std::uint8_t>(value >> (index * 8U)));
  }
}

std::uint64_t GetLe(const std::uint8_t* bytes, std::size_t count) {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index != count; ++index) {
    value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8U);
  }
  return value;
}

template <std::size_t N>
void Get(const std::uint8_t* bytes, std::array<std::uint8_t, N>* value) {
  std::copy_n(bytes, N, value->begin());
}

bool Zero(const std::uint8_t* begin, const std::uint8_t* end) {
  return std::all_of(begin, end,
                     [](std::uint8_t byte) { return byte == 0; });
}

SblrCatalogEpochCheckJournalHashV1 Hash(const std::uint8_t* bytes,
                                        std::size_t size) {
  std::vector<std::uint8_t> material;
  if (size != 0) material.assign(bytes, bytes + size);
  return scratchbird::core::hash::ComputeSha256Digest(material).digest;
}

SblrCatalogEpochCheckJournalHashV1 RecordHash(
    const std::vector<std::uint8_t>& bytes) {
  std::vector<std::uint8_t> material(kRecordDomain.begin(),
                                     kRecordDomain.end());
  material.insert(material.end(), bytes.begin(), bytes.end());
  return Hash(material.data(), material.size());
}

std::string UuidText(const SblrCatalogEpochCheckJournalUuidV1& value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string text;
  text.reserve(36);
  for (std::size_t index = 0; index != value.size(); ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10) {
      text.push_back('-');
    }
    text.push_back(kHex[value[index] >> 4U]);
    text.push_back(kHex[value[index] & 0x0fU]);
  }
  return text;
}

bool KeyValid(const SblrCatalogEpochCheckJournalKeyV1& key) {
  return NonZero(key.database_uuid) &&
         NonZero(key.statement_receipt_uuid) && NonZero(key.check_uuid) &&
         NonZero(key.descriptor_sha256) &&
         NonZero(key.visibility_scope_sha256) &&
         NonZero(key.requested_catalog_epoch_uuid) &&
         key.requested_catalog_generation != 0 &&
         key.schema_tree_generation != 0 && key.security_epoch != 0 &&
         key.resource_epoch != 0 &&
         key.executor_availability_generation != 0;
}

bool SameKey(const SblrCatalogEpochCheckJournalKeyV1& left,
             const SblrCatalogEpochCheckJournalKeyV1& right) {
  return left.database_uuid == right.database_uuid &&
         left.statement_receipt_uuid == right.statement_receipt_uuid &&
         left.check_uuid == right.check_uuid &&
         left.descriptor_sha256 == right.descriptor_sha256 &&
         left.visibility_scope_sha256 == right.visibility_scope_sha256 &&
         left.requested_catalog_epoch_uuid ==
             right.requested_catalog_epoch_uuid &&
         left.requested_catalog_generation ==
             right.requested_catalog_generation &&
         left.schema_tree_generation == right.schema_tree_generation &&
         left.security_epoch == right.security_epoch &&
         left.resource_epoch == right.resource_epoch &&
         left.executor_availability_generation ==
             right.executor_availability_generation;
}

bool HasAuthority(const EngineRequestContext& context,
                  const SblrCatalogEpochCheckJournalKeyV1& key) {
  return context.security_context_present &&
         context.statement_metadata_snapshot_engine_owned &&
         !context.database_path.empty() &&
         context.database_uuid.canonical == UuidText(key.database_uuid) &&
         std::find(context.trace_tags.begin(), context.trace_tags.end(),
                   "private_catalog_epoch_check_journal") !=
             context.trace_tags.end();
}

std::string Path(const EngineRequestContext& context,
                 const SblrCatalogEpochCheckJournalKeyV1& key) {
  return context.database_path +
         ".sb.sblr_catalog_epoch_check_journal.v1." +
         UuidText(key.check_uuid);
}

std::vector<std::uint8_t> Encode(
    const SblrCatalogEpochCheckJournalSnapshotV1& snapshot) {
  if (!KeyValid(snapshot.key) || snapshot.journal_generation == 0 ||
      (snapshot.state != SblrCatalogEpochCheckJournalStateV1::begun &&
       snapshot.state != SblrCatalogEpochCheckJournalStateV1::published)) {
    return {};
  }
  if ((snapshot.state == SblrCatalogEpochCheckJournalStateV1::begun &&
       (!snapshot.canonical_result_bytes.empty() ||
        NonZero(snapshot.canonical_result_sha256))) ||
      (snapshot.state == SblrCatalogEpochCheckJournalStateV1::published &&
       snapshot.canonical_result_bytes.size() != kResultBytes)) {
    return {};
  }
  const auto result_hash = snapshot.canonical_result_bytes.empty()
                               ? SblrCatalogEpochCheckJournalHashV1{}
                               : Hash(snapshot.canonical_result_bytes.data(),
                                      snapshot.canonical_result_bytes.size());
  if (NonZero(snapshot.canonical_result_sha256) &&
      snapshot.canonical_result_sha256 != result_hash) {
    return {};
  }

  std::vector<std::uint8_t> bytes{'S', 'B', 'C', 'J'};
  PutLe(&bytes, 1, 2);
  PutLe(&bytes, kHeaderBytes, 2);
  PutLe(&bytes, kHeaderBytes + snapshot.canonical_result_bytes.size(), 4);
  PutLe(&bytes, static_cast<std::uint32_t>(snapshot.state), 4);
  Put(&bytes, snapshot.key.database_uuid);
  Put(&bytes, snapshot.key.statement_receipt_uuid);
  Put(&bytes, snapshot.key.check_uuid);
  Put(&bytes, snapshot.key.descriptor_sha256);
  Put(&bytes, snapshot.key.visibility_scope_sha256);
  Put(&bytes, snapshot.key.requested_catalog_epoch_uuid);
  PutLe(&bytes, snapshot.key.requested_catalog_generation, 8);
  PutLe(&bytes, snapshot.key.schema_tree_generation, 8);
  PutLe(&bytes, snapshot.key.security_epoch, 8);
  PutLe(&bytes, snapshot.key.resource_epoch, 8);
  PutLe(&bytes, snapshot.key.executor_availability_generation, 8);
  PutLe(&bytes, snapshot.journal_generation, 8);
  PutLe(&bytes, snapshot.canonical_result_bytes.size(), 8);
  Put(&bytes, result_hash);
  bytes.insert(bytes.end(), 32, 0);
  bytes.insert(bytes.end(), 24, 0);
  bytes.insert(bytes.end(), snapshot.canonical_result_bytes.begin(),
               snapshot.canonical_result_bytes.end());
  const auto evidence = RecordHash(bytes);
  if (NonZero(snapshot.record_evidence_sha256) &&
      snapshot.record_evidence_sha256 != evidence) {
    return {};
  }
  std::copy(evidence.begin(), evidence.end(), bytes.begin() + 232);
  return bytes;
}

bool Decode(const std::vector<std::uint8_t>& bytes,
            SblrCatalogEpochCheckJournalSnapshotV1* snapshot) {
  if (snapshot == nullptr || bytes.size() < kHeaderBytes ||
      bytes.size() > kHeaderBytes + kResultBytes ||
      !std::equal(bytes.begin(), bytes.begin() + 4, "SBCJ") ||
      GetLe(bytes.data() + 4, 2) != 1 ||
      GetLe(bytes.data() + 6, 2) != kHeaderBytes ||
      GetLe(bytes.data() + 8, 4) != bytes.size() ||
      !Zero(bytes.data() + 264, bytes.data() + 288)) {
    return false;
  }
  SblrCatalogEpochCheckJournalSnapshotV1 value;
  const auto state = GetLe(bytes.data() + 12, 4);
  if (state < 1 || state > 2) return false;
  value.state = static_cast<SblrCatalogEpochCheckJournalStateV1>(state);
  Get(bytes.data() + 16, &value.key.database_uuid);
  Get(bytes.data() + 32, &value.key.statement_receipt_uuid);
  Get(bytes.data() + 48, &value.key.check_uuid);
  Get(bytes.data() + 64, &value.key.descriptor_sha256);
  Get(bytes.data() + 96, &value.key.visibility_scope_sha256);
  Get(bytes.data() + 128, &value.key.requested_catalog_epoch_uuid);
  value.key.requested_catalog_generation = GetLe(bytes.data() + 144, 8);
  value.key.schema_tree_generation = GetLe(bytes.data() + 152, 8);
  value.key.security_epoch = GetLe(bytes.data() + 160, 8);
  value.key.resource_epoch = GetLe(bytes.data() + 168, 8);
  value.key.executor_availability_generation = GetLe(bytes.data() + 176, 8);
  value.journal_generation = GetLe(bytes.data() + 184, 8);
  const auto result_size = GetLe(bytes.data() + 192, 8);
  Get(bytes.data() + 200, &value.canonical_result_sha256);
  Get(bytes.data() + 232, &value.record_evidence_sha256);
  if (result_size != bytes.size() - kHeaderBytes) return false;
  value.canonical_result_bytes.assign(bytes.begin() + kHeaderBytes,
                                      bytes.end());
  auto evidence_material = bytes;
  std::fill(evidence_material.begin() + 232,
            evidence_material.begin() + 264, 0);
  if (!KeyValid(value.key) || value.journal_generation == 0 ||
      value.record_evidence_sha256 != RecordHash(evidence_material) ||
      Encode(value) != bytes) {
    return false;
  }
  *snapshot = std::move(value);
  return true;
}

enum class ReadStatus { absent, ok, invalid, io_error };

ReadStatus ReadFile(const std::string& path,
                    std::vector<std::uint8_t>* bytes) {
  bytes->clear();
#if defined(_WIN32)
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) {
    return std::filesystem::exists(path) ? ReadStatus::io_error
                                         : ReadStatus::absent;
  }
  const auto end = input.tellg();
  if (end < 0 || static_cast<std::uint64_t>(end) >
                     kHeaderBytes + kResultBytes) {
    return ReadStatus::invalid;
  }
  bytes->resize(static_cast<std::size_t>(end));
  input.seekg(0);
  input.read(reinterpret_cast<char*>(bytes->data()), end);
  return input ? ReadStatus::ok : ReadStatus::io_error;
#else
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) return errno == ENOENT ? ReadStatus::absent
                                     : ReadStatus::io_error;
  struct stat metadata {};
  if (::fstat(fd, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
      metadata.st_size < 0 ||
      static_cast<std::uint64_t>(metadata.st_size) >
          kHeaderBytes + kResultBytes) {
    ::close(fd);
    return ReadStatus::invalid;
  }
  bytes->resize(static_cast<std::size_t>(metadata.st_size));
  std::size_t offset = 0;
  while (offset != bytes->size()) {
    const auto count =
        ::read(fd, bytes->data() + offset, bytes->size() - offset);
    if (count <= 0) {
      ::close(fd);
      return ReadStatus::io_error;
    }
    offset += static_cast<std::size_t>(count);
  }
  if (::close(fd) != 0) return ReadStatus::io_error;
  return ReadStatus::ok;
#endif
}

bool SyncParent(const std::string& path) {
#if defined(_WIN32)
  (void)path;
  return true;
#else
  auto parent = std::filesystem::path(path).parent_path();
  if (parent.empty()) parent = ".";
  const int fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) return false;
  const bool ok = ::fsync(fd) == 0;
  ::close(fd);
  return ok;
#endif
}

enum class CreateStatus { created, exists, failed };

CreateStatus CreateFile(const std::string& path,
                        const std::vector<std::uint8_t>& bytes) {
#if defined(_WIN32)
  if (std::filesystem::exists(path)) return CreateStatus::exists;
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) return CreateStatus::failed;
  output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  output.flush();
  return output ? CreateStatus::created : CreateStatus::failed;
#else
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL |
                                          O_CLOEXEC | O_NOFOLLOW,
                        0600);
  if (fd < 0) return errno == EEXIST ? CreateStatus::exists
                                     : CreateStatus::failed;
  std::size_t offset = 0;
  bool ok = true;
  while (offset != bytes.size()) {
    const auto count =
        ::write(fd, bytes.data() + offset, bytes.size() - offset);
    if (count <= 0) {
      ok = false;
      break;
    }
    offset += static_cast<std::size_t>(count);
  }
  ok = ok && ::fsync(fd) == 0;
  if (::close(fd) != 0) ok = false;
  if (!ok || !SyncParent(path)) {
    (void)::unlink(path.c_str());
    return CreateStatus::failed;
  }
  return CreateStatus::created;
#endif
}

bool ReplaceFile(const std::string& path,
                 const std::vector<std::uint8_t>& bytes) {
  const auto ordinal = g_temp_ordinal.fetch_add(1, std::memory_order_relaxed);
  const auto temp = path + ".tmp." + std::to_string(ordinal);
  if (CreateFile(temp, bytes) != CreateStatus::created) return false;
  std::error_code error;
  std::filesystem::rename(temp, path, error);
  if (error) {
    std::filesystem::remove(temp, error);
    return false;
  }
  return SyncParent(path);
}

SblrCatalogEpochCheckJournalResultV1 Refused(std::string code,
                                             std::string key,
                                             std::string detail = {}) {
  SblrCatalogEpochCheckJournalResultV1 result;
  result.diagnostic =
      Diagnostic(std::move(code), std::move(key), std::move(detail));
  return result;
}

SblrCatalogEpochCheckJournalResultV1 Loaded(
    SblrCatalogEpochCheckJournalSnapshotV1 snapshot) {
  SblrCatalogEpochCheckJournalResultV1 result;
  result.ok = true;
  result.found = true;
  result.diagnostic = OkDiagnostic();
  result.snapshot = std::move(snapshot);
  return result;
}

SblrCatalogEpochCheckJournalResultV1 LoadExact(
    const EngineRequestContext& context,
    const SblrCatalogEpochCheckJournalKeyV1& key) {
  std::vector<std::uint8_t> bytes;
  const auto status = ReadFile(Path(context, key), &bytes);
  if (status == ReadStatus::absent) {
    SblrCatalogEpochCheckJournalResultV1 result;
    result.ok = true;
    result.diagnostic = OkDiagnostic();
    return result;
  }
  SblrCatalogEpochCheckJournalSnapshotV1 snapshot;
  if (status != ReadStatus::ok || !Decode(bytes, &snapshot)) {
    return Refused("MGA.AUTHORITY_MISMATCH",
                   "sblr.catalog_epoch_check.journal_corrupt",
                   "durable catalog-epoch-check journal is noncanonical");
  }
  if (!SameKey(snapshot.key, key)) {
    return Refused("MGA.AUTHORITY_MISMATCH",
                   "sblr.catalog_epoch_check.journal_identity_conflict",
                   "check identity is already bound to different authority");
  }
  return Loaded(std::move(snapshot));
}

bool InputsValid(const EngineRequestContext& context,
                 const SblrCatalogEpochCheckJournalKeyV1& key) {
  return KeyValid(key) && HasAuthority(context, key);
}

}  // namespace

SblrCatalogEpochCheckJournalResultV1 LookupSblrCatalogEpochCheckJournalV1(
    const EngineRequestContext& context,
    const SblrCatalogEpochCheckJournalKeyV1& key) {
  std::lock_guard lock(g_journal_mutex);
  if (!InputsValid(context, key)) {
    return Refused("SECURITY.ACCESS_DENIED",
                   "sblr.catalog_epoch_check.journal_hidden");
  }
  return LoadExact(context, key);
}

SblrCatalogEpochCheckJournalResultV1 EnsureSblrCatalogEpochCheckJournalV1(
    const EngineRequestContext& context,
    const SblrCatalogEpochCheckJournalKeyV1& key) {
  std::lock_guard lock(g_journal_mutex);
  if (!InputsValid(context, key)) {
    return Refused("SECURITY.ACCESS_DENIED",
                   "sblr.catalog_epoch_check.journal_hidden");
  }
  auto existing = LoadExact(context, key);
  if (!existing.ok || existing.found) return existing;

  SblrCatalogEpochCheckJournalSnapshotV1 snapshot;
  snapshot.key = key;
  snapshot.state = SblrCatalogEpochCheckJournalStateV1::begun;
  snapshot.journal_generation = 1;
  const auto bytes = Encode(snapshot);
  if (bytes.empty()) {
    return Refused("SBLR.OPERAND_INVALID",
                   "sblr.catalog_epoch_check.journal_key_invalid");
  }
  const auto created = CreateFile(Path(context, key), bytes);
  if (created == CreateStatus::exists) return LoadExact(context, key);
  if (created != CreateStatus::created || !Decode(bytes, &snapshot)) {
    return Refused("MGA.AUTHORITY_MISMATCH",
                   "sblr.catalog_epoch_check.journal_publish_failed",
                   "durable check identity publication failed");
  }
  return Loaded(std::move(snapshot));
}

SblrCatalogEpochCheckJournalResultV1
PublishSblrCatalogEpochCheckJournalResultV1(
    const EngineRequestContext& context,
    const SblrCatalogEpochCheckJournalKeyV1& key,
    const std::vector<std::uint8_t>& canonical_result_bytes) {
  std::lock_guard lock(g_journal_mutex);
  if (!InputsValid(context, key)) {
    return Refused("SECURITY.ACCESS_DENIED",
                   "sblr.catalog_epoch_check.journal_hidden");
  }
  auto existing = LoadExact(context, key);
  if (!existing.ok) return existing;
  if (!existing.found) {
    return Refused("MGA.AUTHORITY_MISMATCH",
                   "sblr.catalog_epoch_check.journal_identity_missing");
  }
  if (existing.snapshot.state ==
      SblrCatalogEpochCheckJournalStateV1::published) {
    if (existing.snapshot.canonical_result_bytes == canonical_result_bytes) {
      return existing;
    }
    return Refused("MGA.AUTHORITY_MISMATCH",
                   "sblr.catalog_epoch_check.journal_result_conflict");
  }
  if (canonical_result_bytes.size() != kResultBytes ||
      existing.snapshot.journal_generation ==
          std::numeric_limits<std::uint64_t>::max()) {
    return Refused("SBLR.OPERAND_INVALID",
                   "sblr.catalog_epoch_check.journal_result_invalid");
  }
  auto published = existing.snapshot;
  published.state = SblrCatalogEpochCheckJournalStateV1::published;
  ++published.journal_generation;
  published.canonical_result_bytes = canonical_result_bytes;
  published.canonical_result_sha256 =
      Hash(canonical_result_bytes.data(), canonical_result_bytes.size());
  published.record_evidence_sha256 = {};
  const auto bytes = Encode(published);
  if (bytes.empty() || !ReplaceFile(Path(context, key), bytes) ||
      !Decode(bytes, &published)) {
    return Refused("MGA.AUTHORITY_MISMATCH",
                   "sblr.catalog_epoch_check.journal_result_publish_failed",
                   "durable catalog-epoch-check result publication failed");
  }
  return Loaded(std::move(published));
}

}  // namespace scratchbird::engine::internal_api
