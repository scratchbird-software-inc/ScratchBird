// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "sblr_database_attach_journal.hpp"

#include "api_diagnostics.hpp"
#include "engine/sblr/sblr_database_attach_runtime.hpp"
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

constexpr std::size_t kHeaderBytes = 512;
constexpr std::size_t kResultBytes =
    scratchbird::engine::sblr::kSblrDatabaseAttachResultBytes;
constexpr std::string_view kRecordDomain =
    "ScratchBird.DatabaseAttachJournalRecord.V1";
constexpr std::string_view kAliasDomain =
    "ScratchBird.DatabaseAttachAliasName.V1";

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

SblrDatabaseAttachJournalHashV1 Hash(const std::uint8_t* bytes,
                                     std::size_t size) {
  std::vector<std::uint8_t> material;
  if (size != 0) material.assign(bytes, bytes + size);
  return scratchbird::core::hash::ComputeSha256Digest(material).digest;
}

SblrDatabaseAttachJournalHashV1 RecordHash(
    const std::vector<std::uint8_t>& bytes) {
  std::vector<std::uint8_t> material(kRecordDomain.begin(),
                                     kRecordDomain.end());
  material.insert(material.end(), bytes.begin(), bytes.end());
  return Hash(material.data(), material.size());
}

std::string UuidText(const SblrDatabaseAttachJournalUuidV1& value) {
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

std::string HashText(const SblrDatabaseAttachJournalHashV1& value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string text;
  text.reserve(64);
  for (const auto byte : value) {
    text.push_back(kHex[byte >> 4U]);
    text.push_back(kHex[byte & 0x0fU]);
  }
  return text;
}

bool KeyValid(const SblrDatabaseAttachJournalKeyV1& key) {
  return NonZero(key.database_uuid) && NonZero(key.session_uuid) &&
         NonZero(key.statement_receipt_uuid) && NonZero(key.attach_uuid) &&
         NonZero(key.storage_uuid) && NonZero(key.alias_uuid) &&
         NonZero(key.alias_name_sha256) && NonZero(key.descriptor_sha256) &&
         NonZero(key.storage_alias_binding_sha256) &&
         NonZero(key.catalog_snapshot_uuid) && key.catalog_generation != 0 &&
         NonZero(key.security_context_uuid) && key.security_epoch != 0 &&
         NonZero(key.policy_snapshot_uuid) && key.policy_generation != 0 &&
         NonZero(key.transaction_uuid) && key.transaction_generation != 0 &&
         (key.mode == 1 || key.mode == 2) && key.alias_scope == 1 &&
         NonZero(key.resource_admission_uuid) && key.resource_epoch != 0 &&
         key.executor_availability_generation != 0;
}

bool SameKey(const SblrDatabaseAttachJournalKeyV1& left,
             const SblrDatabaseAttachJournalKeyV1& right) {
  return left.database_uuid == right.database_uuid &&
         left.session_uuid == right.session_uuid &&
         left.statement_receipt_uuid == right.statement_receipt_uuid &&
         left.attach_uuid == right.attach_uuid &&
         left.storage_uuid == right.storage_uuid &&
         left.alias_uuid == right.alias_uuid &&
         left.alias_name_sha256 == right.alias_name_sha256 &&
         left.descriptor_sha256 == right.descriptor_sha256 &&
         left.storage_alias_binding_sha256 ==
             right.storage_alias_binding_sha256 &&
         left.catalog_snapshot_uuid == right.catalog_snapshot_uuid &&
         left.catalog_generation == right.catalog_generation &&
         left.security_context_uuid == right.security_context_uuid &&
         left.security_epoch == right.security_epoch &&
         left.policy_snapshot_uuid == right.policy_snapshot_uuid &&
         left.policy_generation == right.policy_generation &&
         left.transaction_uuid == right.transaction_uuid &&
         left.transaction_generation == right.transaction_generation &&
         left.mode == right.mode && left.alias_scope == right.alias_scope &&
         left.resource_admission_uuid == right.resource_admission_uuid &&
         left.resource_epoch == right.resource_epoch &&
         left.executor_availability_generation ==
             right.executor_availability_generation;
}

bool HasAuthority(const EngineRequestContext& context,
                  const SblrDatabaseAttachJournalKeyV1& key) {
  return context.security_context_present &&
         context.statement_metadata_snapshot_engine_owned &&
         !context.database_path.empty() &&
         context.database_uuid.canonical == UuidText(key.database_uuid) &&
         context.session_uuid.canonical == UuidText(key.session_uuid) &&
         context.statement_metadata_snapshot_uuid.canonical ==
             UuidText(key.catalog_snapshot_uuid) &&
         context.catalog_generation_id == key.catalog_generation &&
         context.authorization_context.authority_uuid.canonical ==
             UuidText(key.security_context_uuid) &&
         context.security_epoch == key.security_epoch &&
         context.transaction_policy_snapshot_uuid.canonical ==
             UuidText(key.policy_snapshot_uuid) &&
         context.transaction_policy_snapshot_generation ==
             key.policy_generation &&
         context.transaction_uuid.canonical == UuidText(key.transaction_uuid) &&
         context.local_transaction_id == key.transaction_generation &&
         context.resource_admission_uuid.canonical ==
             UuidText(key.resource_admission_uuid) &&
         context.resource_epoch == key.resource_epoch &&
         std::find(context.trace_tags.begin(), context.trace_tags.end(),
                   "private_database_attach_journal") !=
             context.trace_tags.end();
}

std::string Path(const EngineRequestContext& context,
                 const SblrDatabaseAttachJournalKeyV1& key) {
  return context.database_path + ".sb.sblr_database_attach_journal.v1." +
         UuidText(key.session_uuid) + "." + HashText(key.alias_name_sha256);
}

std::vector<std::uint8_t> Encode(
    const SblrDatabaseAttachJournalSnapshotV1& snapshot) {
  if (!KeyValid(snapshot.key) || snapshot.journal_generation == 0 ||
      (snapshot.state != SblrDatabaseAttachJournalStateV1::begun &&
       snapshot.state != SblrDatabaseAttachJournalStateV1::published)) {
    return {};
  }
  if ((snapshot.state == SblrDatabaseAttachJournalStateV1::begun &&
       (!snapshot.canonical_result_bytes.empty() ||
        NonZero(snapshot.canonical_result_sha256))) ||
      (snapshot.state == SblrDatabaseAttachJournalStateV1::published &&
       snapshot.canonical_result_bytes.size() != kResultBytes)) {
    return {};
  }
  const auto result_hash = snapshot.canonical_result_bytes.empty()
                               ? SblrDatabaseAttachJournalHashV1{}
                               : Hash(snapshot.canonical_result_bytes.data(),
                                      snapshot.canonical_result_bytes.size());
  if (NonZero(snapshot.canonical_result_sha256) &&
      snapshot.canonical_result_sha256 != result_hash) {
    return {};
  }

  std::vector<std::uint8_t> bytes{'S', 'B', 'A', 'J'};
  PutLe(&bytes, 1, 2);
  PutLe(&bytes, kHeaderBytes, 2);
  PutLe(&bytes, kHeaderBytes + snapshot.canonical_result_bytes.size(), 4);
  PutLe(&bytes, static_cast<std::uint32_t>(snapshot.state), 4);
  Put(&bytes, snapshot.key.database_uuid);
  Put(&bytes, snapshot.key.session_uuid);
  Put(&bytes, snapshot.key.statement_receipt_uuid);
  Put(&bytes, snapshot.key.attach_uuid);
  Put(&bytes, snapshot.key.storage_uuid);
  Put(&bytes, snapshot.key.alias_uuid);
  Put(&bytes, snapshot.key.alias_name_sha256);
  Put(&bytes, snapshot.key.descriptor_sha256);
  Put(&bytes, snapshot.key.storage_alias_binding_sha256);
  Put(&bytes, snapshot.key.catalog_snapshot_uuid);
  PutLe(&bytes, snapshot.key.catalog_generation, 8);
  Put(&bytes, snapshot.key.security_context_uuid);
  PutLe(&bytes, snapshot.key.security_epoch, 8);
  Put(&bytes, snapshot.key.policy_snapshot_uuid);
  PutLe(&bytes, snapshot.key.policy_generation, 8);
  Put(&bytes, snapshot.key.transaction_uuid);
  PutLe(&bytes, snapshot.key.transaction_generation, 8);
  bytes.push_back(snapshot.key.mode);
  bytes.push_back(snapshot.key.alias_scope);
  bytes.insert(bytes.end(), 6, 0);
  Put(&bytes, snapshot.key.resource_admission_uuid);
  PutLe(&bytes, snapshot.key.resource_epoch, 8);
  PutLe(&bytes, snapshot.key.executor_availability_generation, 8);
  PutLe(&bytes, snapshot.journal_generation, 8);
  PutLe(&bytes, snapshot.canonical_result_bytes.size(), 8);
  Put(&bytes, result_hash);
  bytes.insert(bytes.end(), 32, 0);
  bytes.insert(bytes.end(), 88, 0);
  bytes.insert(bytes.end(), snapshot.canonical_result_bytes.begin(),
               snapshot.canonical_result_bytes.end());
  const auto evidence = RecordHash(bytes);
  if (NonZero(snapshot.record_evidence_sha256) &&
      snapshot.record_evidence_sha256 != evidence) {
    return {};
  }
  std::copy(evidence.begin(), evidence.end(), bytes.begin() + 392);
  return bytes;
}

bool Decode(const std::vector<std::uint8_t>& bytes,
            SblrDatabaseAttachJournalSnapshotV1* snapshot) {
  if (snapshot == nullptr || bytes.size() < kHeaderBytes ||
      bytes.size() > kHeaderBytes + kResultBytes ||
      !std::equal(bytes.begin(), bytes.begin() + 4, "SBAJ") ||
      GetLe(bytes.data() + 4, 2) != 1 ||
      GetLe(bytes.data() + 6, 2) != kHeaderBytes ||
      GetLe(bytes.data() + 8, 4) != bytes.size() ||
      !Zero(bytes.data() + 306, bytes.data() + 312) ||
      !Zero(bytes.data() + 424, bytes.data() + 512)) {
    return false;
  }
  SblrDatabaseAttachJournalSnapshotV1 value;
  const auto state = GetLe(bytes.data() + 12, 4);
  if (state < 1 || state > 2) return false;
  value.state = static_cast<SblrDatabaseAttachJournalStateV1>(state);
  Get(bytes.data() + 16, &value.key.database_uuid);
  Get(bytes.data() + 32, &value.key.session_uuid);
  Get(bytes.data() + 48, &value.key.statement_receipt_uuid);
  Get(bytes.data() + 64, &value.key.attach_uuid);
  Get(bytes.data() + 80, &value.key.storage_uuid);
  Get(bytes.data() + 96, &value.key.alias_uuid);
  Get(bytes.data() + 112, &value.key.alias_name_sha256);
  Get(bytes.data() + 144, &value.key.descriptor_sha256);
  Get(bytes.data() + 176, &value.key.storage_alias_binding_sha256);
  Get(bytes.data() + 208, &value.key.catalog_snapshot_uuid);
  value.key.catalog_generation = GetLe(bytes.data() + 224, 8);
  Get(bytes.data() + 232, &value.key.security_context_uuid);
  value.key.security_epoch = GetLe(bytes.data() + 248, 8);
  Get(bytes.data() + 256, &value.key.policy_snapshot_uuid);
  value.key.policy_generation = GetLe(bytes.data() + 272, 8);
  Get(bytes.data() + 280, &value.key.transaction_uuid);
  value.key.transaction_generation = GetLe(bytes.data() + 296, 8);
  value.key.mode = bytes[304];
  value.key.alias_scope = bytes[305];
  Get(bytes.data() + 312, &value.key.resource_admission_uuid);
  value.key.resource_epoch = GetLe(bytes.data() + 328, 8);
  value.key.executor_availability_generation = GetLe(bytes.data() + 336, 8);
  value.journal_generation = GetLe(bytes.data() + 344, 8);
  const auto result_size = GetLe(bytes.data() + 352, 8);
  Get(bytes.data() + 360, &value.canonical_result_sha256);
  Get(bytes.data() + 392, &value.record_evidence_sha256);
  if (result_size != bytes.size() - kHeaderBytes) return false;
  value.canonical_result_bytes.assign(bytes.begin() + kHeaderBytes,
                                      bytes.end());
  auto evidence_material = bytes;
  std::fill(evidence_material.begin() + 392,
            evidence_material.begin() + 424, 0);
  if (!KeyValid(value.key) || value.journal_generation == 0 ||
      value.record_evidence_sha256 != RecordHash(evidence_material) ||
      Encode(value) != bytes) {
    return false;
  }
  if (value.state == SblrDatabaseAttachJournalStateV1::published) {
    scratchbird::engine::sblr::SblrDatabaseAttachResultV1 result;
    std::string detail;
    if (!scratchbird::engine::sblr::DecodeSblrDatabaseAttachResultV1(
            value.canonical_result_bytes.data(),
            value.canonical_result_bytes.size(), &result, &detail) ||
        result.attach_uuid != value.key.attach_uuid ||
        result.database_uuid != value.key.database_uuid ||
        result.alias_uuid != value.key.alias_uuid ||
        result.catalog_generation != value.key.catalog_generation) {
      return false;
    }
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

SblrDatabaseAttachJournalResultV1 Refused(std::string code,
                                          std::string key,
                                          std::string detail = {}) {
  SblrDatabaseAttachJournalResultV1 result;
  result.diagnostic =
      Diagnostic(std::move(code), std::move(key), std::move(detail));
  return result;
}

SblrDatabaseAttachJournalResultV1 Loaded(
    SblrDatabaseAttachJournalSnapshotV1 snapshot) {
  SblrDatabaseAttachJournalResultV1 result;
  result.ok = true;
  result.found = true;
  result.diagnostic = OkDiagnostic();
  result.snapshot = std::move(snapshot);
  return result;
}

SblrDatabaseAttachJournalResultV1 LoadExact(
    const EngineRequestContext& context,
    const SblrDatabaseAttachJournalKeyV1& key) {
  std::vector<std::uint8_t> bytes;
  const auto status = ReadFile(Path(context, key), &bytes);
  if (status == ReadStatus::absent) {
    SblrDatabaseAttachJournalResultV1 result;
    result.ok = true;
    result.diagnostic = OkDiagnostic();
    return result;
  }
  SblrDatabaseAttachJournalSnapshotV1 snapshot;
  if (status != ReadStatus::ok || !Decode(bytes, &snapshot)) {
    return Refused("MGA.AUTHORITY_MISMATCH",
                   "sblr.database_attach.journal_corrupt",
                   "durable database-attach journal is noncanonical");
  }
  if (!SameKey(snapshot.key, key)) {
    return Refused("DATABASE.ALIAS_CONFLICT",
                   "sblr.database_attach.alias_conflict",
                   "session alias is already bound to different authority");
  }
  return Loaded(std::move(snapshot));
}

bool InputsValid(const EngineRequestContext& context,
                 const SblrDatabaseAttachJournalKeyV1& key) {
  return KeyValid(key) && HasAuthority(context, key);
}

}  // namespace

SblrDatabaseAttachJournalHashV1 SblrDatabaseAttachAliasNameSha256V1(
    std::string_view raw_name, bool quoted) {
  if (raw_name.empty() || raw_name.size() > 256 ||
      raw_name.find('\0') != std::string_view::npos) {
    return {};
  }
  std::string canonical(raw_name);
  if (!quoted) {
    std::transform(canonical.begin(), canonical.end(), canonical.begin(),
                   [](unsigned char value) {
                     if (value >= 'A' && value <= 'Z') {
                       return static_cast<char>(value - 'A' + 'a');
                     }
                     return static_cast<char>(value);
                   });
  }
  std::vector<std::uint8_t> material(kAliasDomain.begin(),
                                     kAliasDomain.end());
  material.push_back(quoted ? 1U : 0U);
  PutLe(&material, canonical.size(), 2);
  material.insert(material.end(), canonical.begin(), canonical.end());
  return Hash(material.data(), material.size());
}

SblrDatabaseAttachJournalResultV1 LookupSblrDatabaseAttachJournalV1(
    const EngineRequestContext& context,
    const SblrDatabaseAttachJournalKeyV1& key) {
  std::lock_guard lock(g_journal_mutex);
  if (!InputsValid(context, key)) {
    return Refused("SECURITY.ACCESS_DENIED",
                   "sblr.database_attach.journal_hidden");
  }
  return LoadExact(context, key);
}

SblrDatabaseAttachJournalResultV1 EnsureSblrDatabaseAttachJournalV1(
    const EngineRequestContext& context,
    const SblrDatabaseAttachJournalKeyV1& key) {
  std::lock_guard lock(g_journal_mutex);
  if (!InputsValid(context, key)) {
    return Refused("SECURITY.ACCESS_DENIED",
                   "sblr.database_attach.journal_hidden");
  }
  auto existing = LoadExact(context, key);
  if (!existing.ok || existing.found) return existing;

  SblrDatabaseAttachJournalSnapshotV1 snapshot;
  snapshot.key = key;
  snapshot.state = SblrDatabaseAttachJournalStateV1::begun;
  snapshot.journal_generation = 1;
  const auto bytes = Encode(snapshot);
  if (bytes.empty()) {
    return Refused("SBLR.OPERAND_INVALID",
                   "sblr.database_attach.journal_key_invalid");
  }
  const auto created = CreateFile(Path(context, key), bytes);
  if (created == CreateStatus::exists) return LoadExact(context, key);
  if (created != CreateStatus::created || !Decode(bytes, &snapshot)) {
    return Refused("MGA.AUTHORITY_MISMATCH",
                   "sblr.database_attach.journal_publish_failed",
                   "durable attachment identity publication failed");
  }
  return Loaded(std::move(snapshot));
}

SblrDatabaseAttachJournalResultV1 PublishSblrDatabaseAttachJournalResultV1(
    const EngineRequestContext& context,
    const SblrDatabaseAttachJournalKeyV1& key,
    const std::vector<std::uint8_t>& canonical_result_bytes) {
  std::lock_guard lock(g_journal_mutex);
  if (!InputsValid(context, key)) {
    return Refused("SECURITY.ACCESS_DENIED",
                   "sblr.database_attach.journal_hidden");
  }
  auto existing = LoadExact(context, key);
  if (!existing.ok) return existing;
  if (!existing.found) {
    return Refused("MGA.AUTHORITY_MISMATCH",
                   "sblr.database_attach.journal_identity_missing");
  }
  if (existing.snapshot.state ==
      SblrDatabaseAttachJournalStateV1::published) {
    if (existing.snapshot.canonical_result_bytes == canonical_result_bytes) {
      return existing;
    }
    return Refused("MGA.AUTHORITY_MISMATCH",
                   "sblr.database_attach.journal_result_conflict");
  }
  scratchbird::engine::sblr::SblrDatabaseAttachResultV1 decoded;
  std::string detail;
  if (canonical_result_bytes.size() != kResultBytes ||
      !scratchbird::engine::sblr::DecodeSblrDatabaseAttachResultV1(
          canonical_result_bytes.data(), canonical_result_bytes.size(),
          &decoded, &detail) ||
      decoded.attach_uuid != key.attach_uuid ||
      decoded.database_uuid != key.database_uuid ||
      decoded.alias_uuid != key.alias_uuid ||
      decoded.catalog_generation != key.catalog_generation ||
      existing.snapshot.journal_generation ==
          std::numeric_limits<std::uint64_t>::max()) {
    return Refused("SBLR.OPERAND_INVALID",
                   "sblr.database_attach.journal_result_invalid", detail);
  }
  auto published = existing.snapshot;
  published.state = SblrDatabaseAttachJournalStateV1::published;
  ++published.journal_generation;
  published.canonical_result_bytes = canonical_result_bytes;
  published.canonical_result_sha256 =
      Hash(canonical_result_bytes.data(), canonical_result_bytes.size());
  published.record_evidence_sha256 = {};
  const auto bytes = Encode(published);
  if (bytes.empty() || !ReplaceFile(Path(context, key), bytes) ||
      !Decode(bytes, &published)) {
    return Refused("MGA.AUTHORITY_MISMATCH",
                   "sblr.database_attach.journal_result_publish_failed",
                   "durable database-attach result publication failed");
  }
  return Loaded(std::move(published));
}

}  // namespace scratchbird::engine::internal_api
