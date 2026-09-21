// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "sblr_parameter_set_registry.hpp"

#include "api_diagnostics.hpp"
#include "hash_digest.hpp"
#include "uuid.hpp"
#include "engine/sblr/sblr_parameter_runtime.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <limits>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_set>
#include <string_view>
#include <unordered_map>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#endif

namespace scratchbird::engine::internal_api {
namespace {
constexpr std::string_view kMagic{"SBPSR2\0\0", 8};
constexpr std::size_t kJournalLimit = 1024U * 1024U;
constexpr std::string_view kDomain = "ScratchBird.SblrParameterSetRegistry.V2";
constexpr std::string_view kBindMagic{"SBPBR2\0\0", 8};
constexpr std::string_view kBindDomain =
    "ScratchBird.SblrParameterBindPublication.V2";
constexpr std::size_t kMaximumBindValueBytes = 32U * 1024U * 1024U;
std::mutex& RegistryMutex() { static std::mutex value; return value; }
std::unordered_map<EngineUuid, SblrParameterSetSnapshot, EngineUuidHash>& LiveSets() {
  static std::unordered_map<EngineUuid, SblrParameterSetSnapshot, EngineUuidHash> value;
  return value;
}

EngineApiDiagnostic Diagnostic(std::string code, std::string key,
                               std::string detail) {
  return MakeEngineApiDiagnostic(std::move(code), std::move(key),
                                 std::move(detail));
}
bool HasAdmin(const EngineRequestContext& context) {
  return context.security_context_present &&
      std::find(context.trace_tags.begin(), context.trace_tags.end(),
                "right:SBLR_PARAMETER_SET_ADMIN") != context.trace_tags.end();
}
bool HasPrivateReceiptAuthority(const EngineRequestContext& context) {
  return context.security_context_present &&
      context.statement_metadata_snapshot_engine_owned &&
      std::find(context.trace_tags.begin(), context.trace_tags.end(),
                "private_statement_context_receipt") != context.trace_tags.end();
}
bool SafeReason(std::string_view value) {
  if (value.empty() || value.size() > 128) return false;
  return std::all_of(value.begin(), value.end(), [](unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '.' || c == '_' || c == ':' ||
           c == '-';
  });
}
bool ValidUuid(const EngineUuid& id, scratchbird::core::platform::UuidKind kind) {
  return scratchbird::core::uuid::MakeTypedUuid(kind, id).ok() &&
         scratchbird::core::uuid::IsEngineIdentityUuid(id);
}
bool ValidOptionalPair(const EngineUuid& id, std::uint64_t generation) {
  return (id.is_nil() && generation == 0) ||
      (generation != 0 &&
       ValidUuid(id, scratchbird::core::platform::UuidKind::object));
}
EngineUuid GenerateUuid() {
  return scratchbird::core::uuid::IssueRuntimeIdentityV7().value_or(EngineUuid{});
}
std::string Sha256(std::string_view bytes) {
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(
      reinterpret_cast<const scratchbird::core::platform::byte*>(bytes.data()),
      bytes.size());
  return digest.ok()
      ? "sha256:" + scratchbird::core::hash::HexLower(digest.digest)
      : std::string{};
}
std::string Sha256(const std::vector<std::uint8_t>& bytes) {
  return Sha256(std::string_view(
      reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}
bool ValidSha256(std::string_view value) {
  if (value.size() != 71 || value.substr(0, 7) != "sha256:") return false;
  return std::all_of(value.begin() + 7, value.end(), [](unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}
void SetLe(std::string& bytes, std::size_t at, std::uint64_t value,
           std::size_t width) {
  for (std::size_t i = 0; i < width; ++i)
    bytes[at + i] = static_cast<char>(value >> (8 * i));
}
std::uint64_t GetLe(std::string_view bytes, std::size_t at, std::size_t width) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < width; ++i)
    value |= std::uint64_t(static_cast<unsigned char>(bytes[at + i])) << (8 * i);
  return value;
}
void PutUuid(std::string& bytes, std::size_t at, const EngineUuid& id) {
  std::copy(id.bytes.begin(), id.bytes.end(), bytes.begin() + at);
}
EngineUuid GetUuid(std::string_view bytes, std::size_t at) {
  EngineUuid id;
  std::copy_n(bytes.begin() + at, 16, id.bytes.begin());
  return id;
}
bool PutHash(std::string& bytes, std::size_t at, std::string_view hash) {
  if (!ValidSha256(hash)) return false;
  const auto nibble = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
  for (std::size_t i = 0; i < 32; ++i)
    bytes[at + i] = static_cast<char>(
        (nibble(hash[7 + i * 2]) << 4) | nibble(hash[8 + i * 2]));
  return true;
}
std::string GetHash(std::string_view bytes, std::size_t at) {
  std::array<std::uint8_t, 32> digest{};
  std::copy_n(bytes.begin() + at, 32, digest.begin());
  return "sha256:" + scratchbird::core::hash::HexLower(digest);
}
bool ValidateSlots(const std::vector<SblrParameterSlotDescriptor>& slots) {
  if (slots.empty() || slots.size() > 4096) return false;
  std::unordered_set<EngineUuid, EngineUuidHash> identities;
  for (std::size_t i = 0; i < slots.size(); ++i) {
    const auto& slot = slots[i];
    const auto direction = static_cast<unsigned>(slot.direction);
    if (slot.slot_ordinal != i ||
        !ValidUuid(slot.slot_uuid, scratchbird::core::platform::UuidKind::object) ||
        !identities.insert(slot.slot_uuid).second ||
        !ValidUuid(slot.datatype_descriptor_uuid,
                   scratchbird::core::platform::UuidKind::object) ||
        slot.datatype_descriptor_generation == 0 || direction < 1 || direction > 3)
      return false;
  }
  return true;
}
std::string SlotBytes(const std::vector<SblrParameterSlotDescriptor>& slots) {
  std::string bytes(slots.size() * 48, '\0');
  for (std::size_t i = 0; i < slots.size(); ++i) {
    const auto& slot = slots[i];
    const auto at = i * 48;
    SetLe(bytes, at, slot.slot_ordinal, 4);
    PutUuid(bytes, at + 4, slot.slot_uuid);
    PutUuid(bytes, at + 20, slot.datatype_descriptor_uuid);
    SetLe(bytes, at + 36, slot.datatype_descriptor_generation, 8);
    bytes[at + 44] = static_cast<char>(slot.direction);
    bytes[at + 45] = slot.nullable ? 1 : 0;
  }
  return bytes;
}
std::string SlotsHash(const std::vector<SblrParameterSlotDescriptor>& slots) {
  std::string material("ScratchBird.SblrParameterSlots.V2");
  const auto at = material.size();
  material.resize(at + 4);
  SetLe(material, at, slots.size(), 4);
  material += SlotBytes(slots);
  return Sha256(material);
}
bool ParseSlots(std::string_view bytes,
                std::vector<SblrParameterSlotDescriptor>* slots) {
  if (bytes.empty() || bytes.size() % 48 || bytes.size() / 48 > 4096) return false;
  std::vector<SblrParameterSlotDescriptor> decoded;
  decoded.reserve(bytes.size() / 48);
  for (std::size_t at = 0; at < bytes.size(); at += 48) {
    if (GetLe(bytes, at + 45, 1) > 1 || GetLe(bytes, at + 46, 2) != 0)
      return false;
    decoded.push_back({static_cast<std::uint32_t>(GetLe(bytes, at, 4)),
        GetUuid(bytes, at + 4), GetUuid(bytes, at + 20),
        GetLe(bytes, at + 36, 8),
        static_cast<SblrParameterDirection>(GetLe(bytes, at + 44, 1)),
        bytes[at + 45] != 0});
  }
  if (!ValidateSlots(decoded)) return false;
  *slots = std::move(decoded);
  return true;
}
bool SameSlots(const std::vector<SblrParameterSlotDescriptor>& left,
               const std::vector<SblrParameterSlotDescriptor>& right) {
  if (left.size()!=right.size()) return false;
  for (std::size_t i=0;i<left.size();++i) {
    if (left[i].slot_ordinal!=right[i].slot_ordinal ||
        left[i].slot_uuid!=right[i].slot_uuid ||
        left[i].datatype_descriptor_uuid!=right[i].datatype_descriptor_uuid ||
        left[i].datatype_descriptor_generation!=right[i].datatype_descriptor_generation ||
        left[i].direction!=right[i].direction || left[i].nullable!=right[i].nullable)
      return false;
  }
  return true;
}
bool SameImmutableBinding(const SblrParameterSetSnapshot& left,
                          const SblrParameterSetSnapshot& right) {
  return left.database_uuid==right.database_uuid &&
      left.session_uuid==right.session_uuid &&
      left.statement_receipt_uuid==right.statement_receipt_uuid &&
      left.execution_uuid==right.execution_uuid &&
      left.parameter_set_descriptor_uuid==right.parameter_set_descriptor_uuid &&
      left.prepared_statement_uuid==right.prepared_statement_uuid &&
      left.prepared_generation==right.prepared_generation &&
      left.batch_uuid==right.batch_uuid && left.batch_generation==right.batch_generation &&
      left.dynamic_package_uuid==right.dynamic_package_uuid &&
      left.dynamic_generation==right.dynamic_generation &&
      left.catalog_generation==right.catalog_generation &&
      left.security_epoch==right.security_epoch && left.resource_epoch==right.resource_epoch &&
      left.slots_sha256==right.slots_sha256 && SameSlots(left.slots,right.slots);
}
std::string Record(std::uint8_t phase, const SblrParameterSetSnapshot& value,
                   const EngineUuid& prior_uuid, std::uint64_t prior_generation,
                   std::string_view reason) {
  if (phase > 2 || !SafeReason(reason) || !ValidateSlots(value.slots) ||
      !ValidSha256(value.slots_sha256)) return {};
  std::string bytes(320 + value.slots.size() * 48 + reason.size(), '\0');
  std::copy(kMagic.begin(), kMagic.end(), bytes.begin());
  SetLe(bytes, 8, bytes.size(), 4);
  SetLe(bytes, 12, 2, 2);
  bytes[14] = static_cast<char>(phase);
  bytes[15] = static_cast<char>(value.state);
  const std::array<const EngineUuid*, 10> ids{
      &value.snapshot_uuid, &value.database_uuid, &value.session_uuid,
      &value.statement_receipt_uuid, &value.execution_uuid,
      &value.parameter_set_descriptor_uuid, &value.prepared_statement_uuid,
      &value.batch_uuid, &value.dynamic_package_uuid, &prior_uuid};
  for (std::size_t i = 0; i < ids.size(); ++i) PutUuid(bytes, 16 + i * 16, *ids[i]);
  const std::array<std::uint64_t, 9> numbers{
      value.snapshot_generation, value.descriptor_generation,
      value.prepared_generation, value.batch_generation, value.dynamic_generation,
      value.catalog_generation, value.security_epoch, value.resource_epoch,
      prior_generation};
  for (std::size_t i = 0; i < numbers.size(); ++i) SetLe(bytes, 176 + i * 8, numbers[i], 8);
  SetLe(bytes, 248, value.slots.size(), 4);
  SetLe(bytes, 252, reason.size(), 4);
  if (!PutHash(bytes, 256, value.slots_sha256) ||
      (phase != 0 && !PutHash(bytes, 288, value.decision_evidence_sha256))) return {};
  const auto slots = SlotBytes(value.slots);
  std::copy(slots.begin(), slots.end(), bytes.begin() + 320);
  std::copy(reason.begin(), reason.end(), bytes.begin() + 320 + slots.size());
  return bytes;
}
std::string SnapshotMaterial(const SblrParameterSetSnapshot& value,
                             const EngineUuid& prior_uuid,
                             std::uint64_t prior_generation,
                             std::string_view reason) {
  auto bytes = Record(0, value, prior_uuid, prior_generation, reason);
  if (bytes.empty()) return {};
  return std::string(kDomain) + bytes;
}
std::string StorePath(const EngineRequestContext& context, const EngineUuid& id) {
  return context.database_path + ".sb.sblr_parameter_set." +
      scratchbird::core::uuid::UuidToString(id) + ".v1";
}
std::string BindStorePath(const EngineRequestContext& context, const EngineUuid& id) {
  return context.database_path + ".sb.sblr_parameter_bind." +
      scratchbird::core::uuid::UuidToString(id) + ".v1";
}
enum class ReadStatus { ok, absent, invalid, io_error };
#if !defined(_WIN32)
struct Descriptor {
  int fd;
  explicit Descriptor(int value) : fd(value) {}
  ~Descriptor() { if (fd >= 0) ::close(fd); }
  Descriptor(const Descriptor&) = delete;
  Descriptor& operator=(const Descriptor&) = delete;
};
bool SyncDirectory(const std::string& path) {
  const auto parent = std::filesystem::path(path).parent_path();
  Descriptor directory(::open(parent.empty() ? "." : parent.c_str(),
                              O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  return directory.fd >= 0 && ::fsync(directory.fd) == 0;
}
bool WriteAll(int fd, std::string_view bytes) {
  while (!bytes.empty()) {
    const auto count = ::write(fd, bytes.data(), bytes.size());
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return false;
    bytes.remove_prefix(static_cast<std::size_t>(count));
  }
  return true;
}
#endif
bool ConfirmBindingBarrier(const std::string& path) {
  // A previous publisher may have linked the complete record but failed its
  // directory barrier. A read/replay must establish durability, not infer it
  // from the presence of valid bytes.
#if defined(_WIN32)
  HANDLE file = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE,
      FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  struct Guard { HANDLE file; ~Guard() { CloseHandle(file); } } guard{file};
  BY_HANDLE_FILE_INFORMATION info{};
  return GetFileInformationByHandle(file, &info) &&
      !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
      GetFileType(file) == FILE_TYPE_DISK && FlushFileBuffers(file);
#else
  Descriptor file(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
  if (file.fd < 0) return false;
  struct stat info{};
  return ::fstat(file.fd, &info) == 0 && S_ISREG(info.st_mode) &&
      ::fsync(file.fd) == 0 && SyncDirectory(path);
#endif
}
ReadStatus ReadBounded(const std::string& path, std::size_t limit, std::string* bytes) {
#if defined(_WIN32)
  HANDLE file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
      OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return GetLastError() == ERROR_FILE_NOT_FOUND ? ReadStatus::absent : ReadStatus::io_error;
  struct Guard { HANDLE file; ~Guard() { CloseHandle(file); } } guard{file};
  BY_HANDLE_FILE_INFORMATION info{};
  if (!GetFileInformationByHandle(file, &info)) return ReadStatus::io_error;
  const auto size = (std::uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
  if ((info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
      GetFileType(file) != FILE_TYPE_DISK || size == 0 || size > limit) return ReadStatus::invalid;
  std::string loaded(static_cast<std::size_t>(size), '\0');
  DWORD count = 0;
  if (!ReadFile(file, loaded.data(), static_cast<DWORD>(size), &count, nullptr) ||
      count != size) return ReadStatus::io_error;
  char extra;
  if (!ReadFile(file, &extra, 1, &count, nullptr) || count != 0) return ReadStatus::io_error;
#else
  Descriptor file(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
  if (file.fd < 0) {
    if (errno == ENOENT) return ReadStatus::absent;
    return errno == ELOOP ? ReadStatus::invalid : ReadStatus::io_error;
  }
  struct stat info{};
  if (::fstat(file.fd, &info) != 0) return ReadStatus::io_error;
  if (!S_ISREG(info.st_mode) || info.st_size <= 0 ||
      static_cast<std::uint64_t>(info.st_size) > limit) return ReadStatus::invalid;
  std::string loaded(static_cast<std::size_t>(info.st_size), '\0');
  std::size_t at = 0;
  while (at < loaded.size()) {
    const auto count = ::read(file.fd, loaded.data() + at, loaded.size() - at);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return ReadStatus::io_error;
    at += static_cast<std::size_t>(count);
  }
  char extra;
  ssize_t count;
  do { count = ::read(file.fd, &extra, 1); } while (count < 0 && errno == EINTR);
  if (count != 0) return ReadStatus::io_error;
#endif
  *bytes = std::move(loaded);
  return ReadStatus::ok;
}
bool DurableAppend(const std::string& path, std::string_view bytes, bool create,
                   std::size_t limit) {
  if (bytes.empty() || bytes.size() > limit) return false;
#if defined(_WIN32)
  HANDLE file = CreateFileA(path.c_str(), GENERIC_WRITE | GENERIC_READ, FILE_SHARE_READ,
      nullptr, create ? CREATE_NEW : OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  struct Guard { HANDLE file; ~Guard() { CloseHandle(file); } } guard{file};
  BY_HANDLE_FILE_INFORMATION info{};
  if (!GetFileInformationByHandle(file, &info) ||
      (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
      GetFileType(file) != FILE_TYPE_DISK) return false;
  const auto size = (std::uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
  if (size > limit - bytes.size()) return false;
  LARGE_INTEGER zero{};
  if (!SetFilePointerEx(file, zero, nullptr, FILE_END)) return false;
  DWORD count = 0;
  return WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) &&
      count == bytes.size() && FlushFileBuffers(file);
#else
  Descriptor file(::open(path.c_str(), O_WRONLY | O_CLOEXEC | O_NOFOLLOW |
      O_NONBLOCK | O_APPEND | (create ? O_CREAT | O_EXCL : 0), 0600));
  if (file.fd < 0) return false;
  struct stat info{};
  if (::fstat(file.fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0 ||
      static_cast<std::uint64_t>(info.st_size) > limit - bytes.size()) return false;
  return WriteAll(file.fd, bytes) && ::fsync(file.fd) == 0 && SyncDirectory(path);
#endif
}
std::string BindRecord(const SblrParameterBindPublicationSnapshot& value,
                       bool material = false) {
  if (value.canonical_value_vector.empty() ||
      value.canonical_value_vector.size() > kMaximumBindValueBytes) return {};
  std::string bytes(360 + value.canonical_value_vector.size(), '\0');
  std::copy(kBindMagic.begin(), kBindMagic.end(), bytes.begin());
  SetLe(bytes, 8, bytes.size(), 4);
  SetLe(bytes, 12, 2, 2);
  const std::array<const EngineUuid*, 11> ids{
      &value.database_uuid, &value.session_uuid, &value.statement_receipt_uuid,
      &value.execution_uuid, &value.prepared_statement_uuid,
      &value.parameter_set_descriptor_uuid, &value.batch_uuid,
      &value.dynamic_package_uuid, &value.catalog_snapshot_uuid,
      &value.mga_snapshot_uuid, &value.bind_evidence_uuid};
  for (std::size_t i = 0; i < ids.size(); ++i) PutUuid(bytes, 16 + 16 * i, *ids[i]);
  const std::array<std::uint64_t, 8> numbers{
      value.prepared_generation, value.parameter_set_generation,
      value.batch_generation, value.dynamic_generation, value.catalog_generation,
      value.security_epoch, value.resource_epoch, value.executor_availability_generation};
  for (std::size_t i = 0; i < numbers.size(); ++i) SetLe(bytes, 192 + 8 * i, numbers[i], 8);
  if (!PutHash(bytes, 256, value.ordered_slot_table_sha256) ||
      !PutHash(bytes, 288, value.value_vector_sha256) ||
      (!material && !PutHash(bytes, 320, value.publication_evidence_sha256))) return {};
  SetLe(bytes, 352, value.canonical_value_vector.size(), 4);
  std::copy(value.canonical_value_vector.begin(), value.canonical_value_vector.end(),
            bytes.begin() + 360);
  return bytes;
}
std::string BindMaterial(const SblrParameterBindPublicationSnapshot& value) {
  auto bytes = BindRecord(value, true);
  return bytes.empty() ? std::string{} : std::string(kBindDomain) + bytes;
}
bool DecodeBindRecord(std::string_view record,
                      SblrParameterBindPublicationSnapshot* value) {
  if (value == nullptr) return false;
  if (record.size() < 360 || record.size() > 360 + kMaximumBindValueBytes ||
      record.substr(0, 8) != kBindMagic || GetLe(record, 8, 4) != record.size() ||
      GetLe(record, 12, 2) != 2 || GetLe(record, 14, 2) != 0 ||
      GetLe(record, 356, 4) != 0 || GetLe(record, 352, 4) != record.size() - 360 ||
      record.size() == 360) return false;
  SblrParameterBindPublicationSnapshot decoded;
  std::array<EngineUuid*, 11> ids{
      &decoded.database_uuid, &decoded.session_uuid, &decoded.statement_receipt_uuid,
      &decoded.execution_uuid, &decoded.prepared_statement_uuid,
      &decoded.parameter_set_descriptor_uuid, &decoded.batch_uuid,
      &decoded.dynamic_package_uuid, &decoded.catalog_snapshot_uuid,
      &decoded.mga_snapshot_uuid, &decoded.bind_evidence_uuid};
  for (std::size_t i = 0; i < ids.size(); ++i) *ids[i] = GetUuid(record, 16 + 16 * i);
  std::array<std::uint64_t*, 8> numbers{
      &decoded.prepared_generation, &decoded.parameter_set_generation,
      &decoded.batch_generation, &decoded.dynamic_generation, &decoded.catalog_generation,
      &decoded.security_epoch, &decoded.resource_epoch, &decoded.executor_availability_generation};
  for (std::size_t i = 0; i < numbers.size(); ++i) *numbers[i] = GetLe(record, 192 + 8 * i, 8);
  decoded.ordered_slot_table_sha256 = GetHash(record, 256);
  decoded.value_vector_sha256 = GetHash(record, 288);
  decoded.publication_evidence_sha256 = GetHash(record, 320);
  decoded.canonical_value_vector.assign(record.begin() + 360, record.end());
  if (
      !ValidUuid(decoded.database_uuid,
                 scratchbird::core::platform::UuidKind::database) ||
      !ValidUuid(decoded.session_uuid,
                 scratchbird::core::platform::UuidKind::session) ||
      !ValidUuid(decoded.statement_receipt_uuid,
                 scratchbird::core::platform::UuidKind::object) ||
      !ValidUuid(decoded.execution_uuid,
                 scratchbird::core::platform::UuidKind::object) ||
      !ValidUuid(decoded.prepared_statement_uuid,
                 scratchbird::core::platform::UuidKind::object) ||
      decoded.prepared_generation == 0 ||
      !ValidUuid(decoded.parameter_set_descriptor_uuid,
                 scratchbird::core::platform::UuidKind::object) ||
      decoded.parameter_set_generation == 0 ||
      !ValidSha256(decoded.ordered_slot_table_sha256) ||
      !ValidOptionalPair(decoded.batch_uuid, decoded.batch_generation) ||
      !ValidOptionalPair(decoded.dynamic_package_uuid,
                         decoded.dynamic_generation) ||
      !ValidUuid(decoded.catalog_snapshot_uuid,
                 scratchbird::core::platform::UuidKind::object) ||
      decoded.catalog_generation == 0 || decoded.security_epoch == 0 ||
      decoded.resource_epoch == 0 ||
      !ValidUuid(decoded.mga_snapshot_uuid,
                 scratchbird::core::platform::UuidKind::object) ||
      decoded.executor_availability_generation == 0 ||
      !ValidSha256(decoded.value_vector_sha256) ||
      !ValidUuid(decoded.bind_evidence_uuid,
                 scratchbird::core::platform::UuidKind::object) ||
      !ValidSha256(decoded.publication_evidence_sha256) ||
      decoded.value_vector_sha256 != Sha256(decoded.canonical_value_vector) ||
      decoded.publication_evidence_sha256 != Sha256(BindMaterial(decoded))) {
    return false;
  }
  *value = std::move(decoded);
  return true;
}

bool SameBindPublication(
    const SblrParameterBindPublicationSnapshot& left,
    const SblrParameterBindPublicationSnapshot& right) {
  return left.database_uuid == right.database_uuid &&
      left.session_uuid == right.session_uuid &&
      left.statement_receipt_uuid == right.statement_receipt_uuid &&
      left.execution_uuid == right.execution_uuid &&
      left.prepared_statement_uuid == right.prepared_statement_uuid &&
      left.prepared_generation == right.prepared_generation &&
      left.parameter_set_descriptor_uuid ==
          right.parameter_set_descriptor_uuid &&
      left.parameter_set_generation == right.parameter_set_generation &&
      left.ordered_slot_table_sha256 == right.ordered_slot_table_sha256 &&
      left.batch_uuid == right.batch_uuid &&
      left.batch_generation == right.batch_generation &&
      left.dynamic_package_uuid == right.dynamic_package_uuid &&
      left.dynamic_generation == right.dynamic_generation &&
      left.catalog_snapshot_uuid == right.catalog_snapshot_uuid &&
      left.catalog_generation == right.catalog_generation &&
      left.security_epoch == right.security_epoch &&
      left.resource_epoch == right.resource_epoch &&
      left.mga_snapshot_uuid == right.mga_snapshot_uuid &&
      left.executor_availability_generation ==
          right.executor_availability_generation &&
      left.value_vector_sha256 == right.value_vector_sha256 &&
      left.canonical_value_vector == right.canonical_value_vector;
}

bool DurablePublishBind(const EngineRequestContext& context,
                        const SblrParameterBindPublicationSnapshot& value) {
  const auto path = BindStorePath(context, value.parameter_set_descriptor_uuid);
  const auto id = GenerateUuid();
  if (id.is_nil()) return false;
  const auto temporary = path + ".tmp." + scratchbird::core::uuid::UuidToString(id);
  const auto record = BindRecord(value);
  if (record.empty()) return false;
  // CREATE_NEW / O_EXCL ensures this invocation alone owns the provisional file.
#if defined(_WIN32)
  HANDLE file = CreateFileA(temporary.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
      nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
#else
  Descriptor file(::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL |
                         O_CLOEXEC | O_NOFOLLOW, 0600));
  if (file.fd < 0) return false;
#endif
  struct Temporary {
    const std::string& path;
    ~Temporary() {
      // Cleanup runs during allocation-failure unwinding too: constructing a
      // filesystem::path here could itself throw and terminate the process.
#if defined(_WIN32)
      DeleteFileA(path.c_str());
#else
      ::unlink(path.c_str());
#endif
    }
  } cleanup{temporary};
#if defined(_WIN32)
  DWORD count = 0;
  const bool written = WriteFile(file, record.data(), static_cast<DWORD>(record.size()),
                                  &count, nullptr) &&
      count == record.size() && FlushFileBuffers(file);
  CloseHandle(file);
  if (!written) return false;
#else
  if (!WriteAll(file.fd, record) || ::fsync(file.fd) != 0) return false;
#endif
  std::error_code error;
  // Hard-link creation is atomic and never replaces an existing publication.
  std::filesystem::create_hard_link(temporary, path, error);
  if (error) return false;
#if !defined(_WIN32)
  if (!SyncDirectory(path)) return false;
#endif
  return true;
}
bool DecodeRecord(std::string_view bytes, std::uint8_t phase,
                  SblrParameterSetSnapshot* value, EngineUuid* prior_uuid,
                  std::uint64_t* prior_generation, std::string* reason) {
  if (bytes.size() < 320 || bytes.size() > kJournalLimit ||
      bytes.substr(0, 8) != kMagic || GetLe(bytes, 8, 4) != bytes.size() ||
      GetLe(bytes, 12, 2) != 2 || GetLe(bytes, 14, 1) != phase ||
      GetLe(bytes, 15, 1) < 1 || GetLe(bytes, 15, 1) > 2) return false;
  const auto count = GetLe(bytes, 248, 4);
  const auto reason_bytes = GetLe(bytes, 252, 4);
  if (count == 0 || count > 4096 || reason_bytes == 0 || reason_bytes > 128 ||
      320 + count * 48 + reason_bytes != bytes.size()) return false;
  SblrParameterSetSnapshot decoded;
  std::array<EngineUuid*, 10> ids{
      &decoded.snapshot_uuid, &decoded.database_uuid, &decoded.session_uuid,
      &decoded.statement_receipt_uuid, &decoded.execution_uuid,
      &decoded.parameter_set_descriptor_uuid, &decoded.prepared_statement_uuid,
      &decoded.batch_uuid, &decoded.dynamic_package_uuid, prior_uuid};
  for (std::size_t i = 0; i < ids.size(); ++i) *ids[i] = GetUuid(bytes, 16 + 16 * i);
  std::array<std::uint64_t*, 9> numbers{
      &decoded.snapshot_generation, &decoded.descriptor_generation,
      &decoded.prepared_generation, &decoded.batch_generation, &decoded.dynamic_generation,
      &decoded.catalog_generation, &decoded.security_epoch, &decoded.resource_epoch,
      prior_generation};
  for (std::size_t i = 0; i < numbers.size(); ++i) *numbers[i] = GetLe(bytes, 176 + 8 * i, 8);
  decoded.state = static_cast<SblrParameterSetState>(GetLe(bytes, 15, 1));
  decoded.slots_sha256 = GetHash(bytes, 256);
  decoded.decision_evidence_sha256 = GetHash(bytes, 288);
  *reason = bytes.substr(320 + count * 48, reason_bytes);
  using scratchbird::core::platform::UuidKind;
  if (!ParseSlots(bytes.substr(320, count * 48), &decoded.slots) ||
      !ValidUuid(decoded.snapshot_uuid, UuidKind::object) ||
      !ValidUuid(decoded.database_uuid, UuidKind::database) ||
      !ValidUuid(decoded.session_uuid, UuidKind::session) ||
      !ValidUuid(decoded.statement_receipt_uuid, UuidKind::object) ||
      !ValidUuid(decoded.execution_uuid, UuidKind::object) ||
      !ValidUuid(decoded.parameter_set_descriptor_uuid, UuidKind::object) ||
      decoded.snapshot_generation == 0 || decoded.descriptor_generation == 0 ||
      decoded.catalog_generation == 0 || decoded.security_epoch == 0 ||
      decoded.resource_epoch == 0 ||
      !ValidOptionalPair(decoded.prepared_statement_uuid, decoded.prepared_generation) ||
      !ValidOptionalPair(decoded.batch_uuid, decoded.batch_generation) ||
      !ValidOptionalPair(decoded.dynamic_package_uuid, decoded.dynamic_generation) ||
      (!decoded.prepared_statement_uuid.is_nil() && !decoded.dynamic_package_uuid.is_nil()) ||
      !ValidOptionalPair(*prior_uuid, *prior_generation) ||
      !SafeReason(*reason) || decoded.slots_sha256 != SlotsHash(decoded.slots) ||
      decoded.decision_evidence_sha256 != Sha256(SnapshotMaterial(
          decoded, *prior_uuid, *prior_generation, *reason))) return false;
  *value = std::move(decoded);
  return true;
}
bool Publish(const EngineRequestContext& context, const SblrParameterSetSnapshot& value,
             const EngineUuid& prior_uuid, std::uint64_t prior_generation,
             std::string_view reason) {
  const auto path = StorePath(context, value.parameter_set_descriptor_uuid);
  // Allocate both frames before the first durable effect.
  const auto evidence = Record(1, value, prior_uuid, prior_generation, reason);
  const auto snapshot = Record(2, value, prior_uuid, prior_generation, reason);
  return !evidence.empty() && !snapshot.empty() &&
      DurableAppend(path, evidence, prior_generation == 0, kJournalLimit) &&
      DurableAppend(path, snapshot, false, kJournalLimit);
}
SblrParameterSetLoadResult LoadLocked(const EngineRequestContext& context,
                                     const EngineUuid& descriptor_uuid) {
  SblrParameterSetLoadResult result;
  using scratchbird::core::platform::UuidKind;
  if (context.database_path.empty() ||
      !ValidUuid(context.database_uuid, UuidKind::database) ||
      !ValidUuid(context.session_uuid, UuidKind::session) ||
      !ValidUuid(descriptor_uuid, UuidKind::object)) {
    result.diagnostic = Diagnostic("SBLR.OPERAND_INVALID",
        "sblr.parameter_set.identity_invalid", "exact binary owner identities required");
    return result;
  }
  std::string bytes;
  const auto status = ReadBounded(StorePath(context, descriptor_uuid), kJournalLimit, &bytes);
  if (status != ReadStatus::ok) {
    result.diagnostic = Diagnostic(
        status == ReadStatus::io_error ? "SBLR.EXECUTION_FAILED" : "SBLR.PARAMETER.STALE",
        "sblr.parameter_set.read_failed", "parameter registry is absent or unreadable");
    return result;
  }
  SblrParameterSetSnapshot prior;
  for (std::size_t at = 0; at < bytes.size();) {
    const auto remaining = std::string_view(bytes).substr(at);
    if (remaining.size() < 320) {
      result.diagnostic = Diagnostic("SBLR.PARAMETER.STALE",
          "sblr.parameter_set.torn", "torn registry publication"); return result;
    }
    const auto size = GetLe(remaining, 8, 4);
    if (size < 320 || size > remaining.size() / 2) {
      result.diagnostic = Diagnostic("SBLR.PARAMETER.STALE",
          "sblr.parameter_set.torn", "torn registry publication"); return result;
    }
    const auto first = remaining.substr(0, size);
    const auto second = remaining.substr(size, size);
    SblrParameterSetSnapshot evidence, snapshot;
    EngineUuid ep, sp;
    std::uint64_t eg = 0, sg = 0;
    std::string er, sr;
    if (!DecodeRecord(first, 1, &evidence, &ep, &eg, &er) ||
        !DecodeRecord(second, 2, &snapshot, &sp, &sg, &sr) ||
        first.substr(0, 14) != second.substr(0, 14) || first.substr(15) != second.substr(15) ||
        snapshot.database_uuid != context.database_uuid ||
        snapshot.session_uuid != context.session_uuid ||
        snapshot.parameter_set_descriptor_uuid != descriptor_uuid ||
        (at == 0 ? (!ep.is_nil() || eg != 0 || snapshot.snapshot_generation != 1 ||
                    snapshot.descriptor_generation != 1 ||
                    snapshot.state != SblrParameterSetState::active)
                 : (ep != prior.snapshot_uuid || eg != prior.snapshot_generation ||
                    prior.snapshot_generation != 1 || prior.descriptor_generation != 1 ||
                    snapshot.snapshot_generation != 2 || snapshot.descriptor_generation != 2 ||
                    snapshot.snapshot_uuid == prior.snapshot_uuid ||
                    prior.state != SblrParameterSetState::active ||
                    snapshot.state != SblrParameterSetState::revoked ||
                    !SameImmutableBinding(prior, snapshot)))) {
      result.diagnostic = Diagnostic("SBLR.PARAMETER.STALE",
          "sblr.parameter_set.corrupt", "contradictory registry evidence"); return result;
    }
    prior = std::move(snapshot);
    at += size * 2;
  }
  result.snapshot = std::move(prior);
  result.diagnostic = MakeEngineApiDiagnostic("OK", "ok", {}, false);
  result.ok = true;
  return result;
}
bool SameSnapshot(const SblrParameterSetSnapshot& a, const SblrParameterSetSnapshot& b) {
  return a.snapshot_uuid == b.snapshot_uuid &&
      a.snapshot_generation == b.snapshot_generation &&
      a.descriptor_generation == b.descriptor_generation && a.state == b.state &&
      a.decision_evidence_sha256 == b.decision_evidence_sha256 && SameImmutableBinding(a, b);
}
bool LiveAuthority(const EngineRequestContext& context, const SblrParameterSetSnapshot& value) {
  const auto live = LiveSets().find(value.parameter_set_descriptor_uuid);
  return HasPrivateReceiptAuthority(context) &&
      value.state == SblrParameterSetState::active &&
      value.database_uuid == context.database_uuid && value.session_uuid == context.session_uuid &&
      value.catalog_generation == context.catalog_generation_id &&
      value.security_epoch == context.security_epoch && value.resource_epoch == context.resource_epoch &&
      live != LiveSets().end() && SameSnapshot(live->second, value);
}
bool ValidValueVector(const SblrParameterBindPublicationSnapshot& value,
                      const SblrParameterSetSnapshot& set) {
  const auto decoded = scratchbird::engine::sblr::DecodeSblrParameterValueSetV1(
      value.canonical_value_vector.data(), value.canonical_value_vector.size());
  if (!decoded.ok || decoded.value.parameter_set_descriptor_uuid != set.parameter_set_descriptor_uuid.bytes ||
      decoded.value.descriptor_generation != set.descriptor_generation ||
      decoded.value.execution_uuid != value.execution_uuid.bytes ||
      decoded.value.statement_receipt_uuid != value.statement_receipt_uuid.bytes ||
      decoded.value.records.size() != set.slots.size()) return false;
  for (std::size_t i = 0; i < set.slots.size(); ++i) {
    const auto& record = decoded.value.records[i];
    const auto& slot = set.slots[i];
    using State = scratchbird::engine::sblr::SblrParameterValueStateV1;
    if (record.slot_ordinal != slot.slot_ordinal || record.slot_uuid != slot.slot_uuid.bytes ||
        record.datatype_descriptor_uuid != slot.datatype_descriptor_uuid.bytes ||
        record.datatype_descriptor_generation != slot.datatype_descriptor_generation ||
        static_cast<unsigned>(record.direction) != static_cast<unsigned>(slot.direction) ||
        (record.state == State::null_value && !slot.nullable) ||
        (slot.direction != SblrParameterDirection::out && record.state == State::unbound) ||
        (slot.direction == SblrParameterDirection::out && record.state != State::unbound))
      return false;
  }
  return true;
}
bool BindMatchesSet(const SblrParameterBindPublicationSnapshot& binding,
                    const SblrParameterSetSnapshot& set) {
  return binding.database_uuid == set.database_uuid && binding.session_uuid == set.session_uuid &&
      binding.parameter_set_descriptor_uuid == set.parameter_set_descriptor_uuid &&
      binding.parameter_set_generation == set.descriptor_generation &&
      binding.prepared_statement_uuid == set.prepared_statement_uuid &&
      binding.prepared_generation == set.prepared_generation &&
      binding.batch_uuid == set.batch_uuid && binding.batch_generation == set.batch_generation &&
      binding.dynamic_package_uuid == set.dynamic_package_uuid &&
      binding.dynamic_generation == set.dynamic_generation &&
      binding.ordered_slot_table_sha256 == set.slots_sha256 &&
      binding.catalog_generation == set.catalog_generation &&
      binding.security_epoch == set.security_epoch && binding.resource_epoch == set.resource_epoch &&
      ValidValueVector(binding, set);
}
SblrParameterBindPublicationResult LoadBindLocked(
    const EngineRequestContext& context, const EngineUuid& descriptor_uuid) {
  SblrParameterBindPublicationResult result;
  const auto set = LoadLocked(context, descriptor_uuid);
  if (!set.ok || !LiveAuthority(context, set.snapshot)) {
    result.diagnostic = set.ok ? Diagnostic("SBLR.PARAMETER.STALE",
        "sblr.parameter_bind.execution_not_live", "execution authority does not recover") : set.diagnostic;
    return result;
  }
  std::string bytes;
  const auto status = ReadBounded(BindStorePath(context, descriptor_uuid),
                                  360 + kMaximumBindValueBytes, &bytes);
  if (status != ReadStatus::ok) {
    result.diagnostic = Diagnostic(
        status == ReadStatus::io_error ? "SBLR.EXECUTION_FAILED" : "SBLR.PARAMETER.STALE",
        "sblr.parameter_bind.read_failed", "binding is absent or unreadable");
    return result;
  }
  SblrParameterBindPublicationSnapshot decoded;
  if (!DecodeBindRecord(bytes, &decoded) || !BindMatchesSet(decoded, set.snapshot)) {
    result.diagnostic = Diagnostic("SBLR.PARAMETER.STALE",
        "sblr.parameter_bind.corrupt", "binding does not match live parameter authority");
    return result;
  }
  if (!ConfirmBindingBarrier(BindStorePath(context, descriptor_uuid))) {
    result.diagnostic = Diagnostic("SBLR.EXECUTION_FAILED",
        "sblr.parameter_bind.barrier_failed", "binding durability could not be confirmed");
    return result;
  }
  result.snapshot = std::move(decoded);
  result.diagnostic = MakeEngineApiDiagnostic("OK", "ok", {}, false);
  result.ok = true;
  return result;
}

}  // namespace

SblrParameterSetMutationResult IssueSblrParameterSet(
    const EngineRequestContext& context,const SblrParameterSetIssueRequest& request) {
  std::lock_guard lock(RegistryMutex()); SblrParameterSetMutationResult result;
  if (!HasPrivateReceiptAuthority(context)) { result.diagnostic=Diagnostic("SECURITY.ACCESS_DENIED","sblr.parameter_set.issue_denied","engine-owned private statement receipt required"); return result; }
  if (context.database_path.empty() ||
      !ValidUuid(context.database_uuid,scratchbird::core::platform::UuidKind::database) ||
      !ValidUuid(context.session_uuid,scratchbird::core::platform::UuidKind::session) ||
      !ValidUuid(request.statement_receipt_uuid,scratchbird::core::platform::UuidKind::object) ||
      !ValidUuid(request.execution_uuid,scratchbird::core::platform::UuidKind::object) ||
      context.catalog_generation_id==0||context.security_epoch==0||context.resource_epoch==0||
      !ValidOptionalPair(request.prepared_statement_uuid,request.prepared_generation)||
      !ValidOptionalPair(request.batch_uuid,request.batch_generation)||
      !ValidOptionalPair(request.dynamic_package_uuid,request.dynamic_generation)||
      request.slots.empty()||request.slots.size()>4096||!SafeReason(request.reason_code)) {
    result.diagnostic=Diagnostic("SBLR.OPERAND_INVALID","sblr.parameter_set.issue_invalid","exact engine context and demands required"); return result;
  }
  if (!request.prepared_statement_uuid.is_nil() &&
      !request.dynamic_package_uuid.is_nil()) {
    result.diagnostic=Diagnostic("SBLR.OPERAND_INVALID","sblr.parameter_set.identity_matrix_invalid","prepared and dynamic identities are mutually exclusive"); return result;
  }
  SblrParameterSetSnapshot value; value.snapshot_uuid=GenerateUuid();
  value.snapshot_generation=1; value.database_uuid=context.database_uuid;
  value.session_uuid=context.session_uuid; value.statement_receipt_uuid=request.statement_receipt_uuid;
  value.execution_uuid=request.execution_uuid; value.parameter_set_descriptor_uuid=GenerateUuid();
  value.descriptor_generation=1; value.prepared_statement_uuid=request.prepared_statement_uuid;
  value.prepared_generation=request.prepared_generation; value.batch_uuid=request.batch_uuid;
  value.batch_generation=request.batch_generation; value.dynamic_package_uuid=request.dynamic_package_uuid;
  value.dynamic_generation=request.dynamic_generation; value.catalog_generation=context.catalog_generation_id;
  value.security_epoch=context.security_epoch; value.resource_epoch=context.resource_epoch;
  value.state=SblrParameterSetState::active;
  for (std::size_t i=0;i<request.slots.size();++i) {
    const auto& demand=request.slots[i]; const auto direction=static_cast<unsigned>(demand.direction);
    if (!ValidUuid(demand.datatype_descriptor_uuid,scratchbird::core::platform::UuidKind::object)||
        demand.datatype_descriptor_generation==0||direction<1||direction>3) {
      result.diagnostic=Diagnostic("DATATYPE.DESCRIPTOR.INVALID","sblr.parameter_set.slot_descriptor_invalid","exact datatype identity required"); return result;
    }
    value.slots.push_back({static_cast<std::uint32_t>(i),GenerateUuid(),demand.datatype_descriptor_uuid,demand.datatype_descriptor_generation,demand.direction,demand.nullable});
  }
  value.slots_sha256=SlotsHash(value.slots);
  value.decision_evidence_sha256=Sha256(SnapshotMaterial(value,{},0,request.reason_code));
  if (value.snapshot_uuid.is_nil() || value.parameter_set_descriptor_uuid.is_nil() ||
      !ValidateSlots(value.slots) || !ValidSha256(value.slots_sha256) ||
      !ValidSha256(value.decision_evidence_sha256)) {
    result.diagnostic = Diagnostic("SBLR.EXECUTION_FAILED",
        "sblr.parameter_set.issue_failed", "identity or hash authority unavailable");
    return result;
  }
  // Stage every allocating success result and live-map entry before durable effects.
  result.snapshot = value;
  result.diagnostic = MakeEngineApiDiagnostic("OK", "ok", {}, false);
  result.evidence.push_back({"sblr.parameter_set.issue", value.decision_evidence_sha256});
  const auto inserted = LiveSets().emplace(value.parameter_set_descriptor_uuid, value);
  if (!inserted.second) {
    result = {};
    result.diagnostic = Diagnostic("SBLR.EXECUTION_FAILED",
        "sblr.parameter_set.identity_collision", "identity is already issued");
    return result;
  }
  struct LiveGuard {
    const EngineUuid& id;
    bool committed = false;
    ~LiveGuard() { if (!committed) LiveSets().erase(id); }
  } guard{value.parameter_set_descriptor_uuid};
  if (!Publish(context, value, {}, 0, request.reason_code)) {
    result = {};
    result.diagnostic = Diagnostic("SBLR.EXECUTION_FAILED",
        "sblr.parameter_set.publish_failed", "durable issue failed");
    return result;
  }
  guard.committed = true;
  result.ok = true;
  return result;
}

SblrParameterSetLoadResult LoadSblrParameterSet(const EngineRequestContext& context,
                                                const EngineUuid& uuid) {
  std::lock_guard lock(RegistryMutex()); return LoadLocked(context,uuid);
}

SblrParameterBindPublicationResult PublishSblrParameterBinding(
    const EngineRequestContext& context,
    const SblrParameterSetSnapshot& admitted,
    const SblrParameterBindPublicationRequest& request) {
  std::lock_guard lock(RegistryMutex());
  SblrParameterBindPublicationResult result;
  const auto refuse = [&](std::string code, std::string key,
                          std::string detail) {
    result.diagnostic = Diagnostic(std::move(code), std::move(key),
                                   std::move(detail));
    return result;
  };
  if (!HasPrivateReceiptAuthority(context)) {
    return refuse("SECURITY.ACCESS_DENIED",
                  "sblr.parameter_bind.publish_denied",
                  "engine-owned private statement receipt required");
  }
  const auto durable = LoadLocked(context, admitted.parameter_set_descriptor_uuid);
  if (!durable.ok || !LiveAuthority(context, durable.snapshot) ||
      !SameSnapshot(admitted, durable.snapshot)) {
    return refuse(durable.ok ? "SBLR.PARAMETER.STALE" : durable.diagnostic.code,
                  "sblr.parameter_bind.execution_not_live",
                  "current durable and live parameter authority required");
  }
  const char* authority_mismatch = nullptr;
  if (admitted.state != SblrParameterSetState::active)
    authority_mismatch = "parameter_set_not_active";
  else if (admitted.database_uuid != context.database_uuid)
    authority_mismatch = "database_uuid";
  else if (admitted.session_uuid != context.session_uuid)
    authority_mismatch = "session_uuid";
  else if (request.parameter_set_descriptor_uuid !=
           admitted.parameter_set_descriptor_uuid)
    authority_mismatch = "parameter_set_descriptor_uuid";
  else if (request.parameter_set_generation != admitted.descriptor_generation)
    authority_mismatch = "parameter_set_generation";
  else if (request.prepared_statement_uuid != admitted.prepared_statement_uuid)
    authority_mismatch = "prepared_statement_uuid";
  else if (request.prepared_generation != admitted.prepared_generation)
    authority_mismatch = "prepared_generation";
  else if (request.batch_uuid != admitted.batch_uuid)
    authority_mismatch = "batch_uuid";
  else if (request.batch_generation != admitted.batch_generation)
    authority_mismatch = "batch_generation";
  else if (request.dynamic_package_uuid != admitted.dynamic_package_uuid)
    authority_mismatch = "dynamic_package_uuid";
  else if (request.dynamic_generation != admitted.dynamic_generation)
    authority_mismatch = "dynamic_generation";
  else if (request.ordered_slot_table_sha256 != admitted.slots_sha256)
    authority_mismatch = "ordered_slot_table_sha256";
  else if (request.catalog_generation != context.catalog_generation_id)
    authority_mismatch = "statement_catalog_generation";
  else if (request.catalog_generation != admitted.catalog_generation)
    authority_mismatch = "parameter_set_catalog_generation";
  else if (request.security_epoch != context.security_epoch)
    authority_mismatch = "statement_security_epoch";
  else if (request.security_epoch != admitted.security_epoch)
    authority_mismatch = "parameter_set_security_epoch";
  else if (request.resource_epoch != context.resource_epoch)
    authority_mismatch = "statement_resource_epoch";
  else if (request.resource_epoch != admitted.resource_epoch)
    authority_mismatch = "parameter_set_resource_epoch";
  if (authority_mismatch != nullptr) {
    return refuse("SBLR.PARAMETER.STALE",
                  "sblr.parameter_bind.authority_stale",
                  std::string("parameter-set or statement authority changed: ") +
                      authority_mismatch);
  }
  if (!ValidUuid(request.statement_receipt_uuid,
                 scratchbird::core::platform::UuidKind::object) ||
      !ValidUuid(request.execution_uuid,
                 scratchbird::core::platform::UuidKind::object) ||
      !ValidUuid(request.prepared_statement_uuid,
                 scratchbird::core::platform::UuidKind::object) ||
      request.prepared_generation == 0 ||
      !ValidUuid(request.catalog_snapshot_uuid,
                 scratchbird::core::platform::UuidKind::object) ||
      !ValidUuid(request.mga_snapshot_uuid,
                 scratchbird::core::platform::UuidKind::object) ||
      request.executor_availability_generation == 0 ||
      request.canonical_value_vector.empty() ||
      request.canonical_value_vector.size() > kMaximumBindValueBytes ||
      !ValidSha256(request.value_vector_sha256) ||
      request.value_vector_sha256 != Sha256(request.canonical_value_vector)) {
    return refuse("SBLR.OPERAND_INVALID",
                  "sblr.parameter_bind.publication_invalid",
                  "canonical bind identities and value evidence required");
  }
  SblrParameterBindPublicationSnapshot proposed;
  proposed.database_uuid = context.database_uuid;
  proposed.session_uuid = context.session_uuid;
  proposed.statement_receipt_uuid = request.statement_receipt_uuid;
  proposed.execution_uuid = request.execution_uuid;
  proposed.prepared_statement_uuid = request.prepared_statement_uuid;
  proposed.prepared_generation = request.prepared_generation;
  proposed.parameter_set_descriptor_uuid =
      request.parameter_set_descriptor_uuid;
  proposed.parameter_set_generation = request.parameter_set_generation;
  proposed.ordered_slot_table_sha256 = request.ordered_slot_table_sha256;
  proposed.batch_uuid = request.batch_uuid;
  proposed.batch_generation = request.batch_generation;
  proposed.dynamic_package_uuid = request.dynamic_package_uuid;
  proposed.dynamic_generation = request.dynamic_generation;
  proposed.catalog_snapshot_uuid = request.catalog_snapshot_uuid;
  proposed.catalog_generation = request.catalog_generation;
  proposed.security_epoch = request.security_epoch;
  proposed.resource_epoch = request.resource_epoch;
  proposed.mga_snapshot_uuid = request.mga_snapshot_uuid;
  proposed.executor_availability_generation =
      request.executor_availability_generation;
  proposed.value_vector_sha256 = request.value_vector_sha256;
  proposed.canonical_value_vector = request.canonical_value_vector;

  if (!ValidValueVector(proposed, durable.snapshot)) {
    return refuse("SBLR.OPERAND_INVALID", "sblr.parameter_bind.value_vector_invalid",
                  "canonical value slots do not match the issued descriptor");
  }
  std::error_code error;
  const auto path = BindStorePath(context,
                                  request.parameter_set_descriptor_uuid);
  if (std::filesystem::exists(path, error)) {
    if (error) {
      return refuse("SBLR.EXECUTION_FAILED",
                    "sblr.parameter_bind.lookup_failed",
                    "durable binding identity cannot be classified");
    }
    auto existing = LoadBindLocked(context,
                                   request.parameter_set_descriptor_uuid);
    if (!existing.ok || !SameBindPublication(existing.snapshot, proposed)) {
      return refuse("SBLR.PARAMETER.STALE",
                    "sblr.parameter_bind.replay_conflict",
                    "an existing binding differs from the exact request");
    }
    existing.replayed = true;
    existing.evidence.push_back({"sblr.parameter_bind.publication",
                                 existing.snapshot.
                                     publication_evidence_sha256});
    return existing;
  }
  if (error) {
    return refuse("SBLR.EXECUTION_FAILED",
                  "sblr.parameter_bind.lookup_failed",
                  "durable binding identity cannot be classified");
  }
  if (context.query_cancellation_requested &&
      context.query_cancellation_requested()) {
    return refuse("PROCESS.CANCELLED", "sblr.parameter_bind.cancelled",
                  "binding was cancelled before durable publication");
  }
  proposed.bind_evidence_uuid = GenerateUuid();
  proposed.publication_evidence_sha256 = Sha256(BindMaterial(proposed));
  if (proposed.bind_evidence_uuid.is_nil() ||
      !ValidSha256(proposed.publication_evidence_sha256)) {
    return refuse("SBLR.EXECUTION_FAILED", "sblr.parameter_bind.publish_failed",
                  "binding identity or hash authority unavailable");
  }
  result.snapshot = std::move(proposed);
  result.diagnostic = MakeEngineApiDiagnostic("OK", "ok", {}, false);
  result.evidence.push_back({"sblr.parameter_bind.publication",
                             result.snapshot.publication_evidence_sha256});
  if (!DurablePublishBind(context, result.snapshot)) {
    result = {};
    return refuse("SBLR.EXECUTION_FAILED", "sblr.parameter_bind.publish_failed",
                  "durable parameter binding publication failed");
  }
  result.ok = true;
  return result;
}

SblrParameterBindPublicationResult LoadSblrParameterBinding(
    const EngineRequestContext& context,
    const EngineUuid& parameter_set_descriptor_uuid) {
  std::lock_guard lock(RegistryMutex());
  if (!HasPrivateReceiptAuthority(context)) {
    SblrParameterBindPublicationResult result;
    result.diagnostic = Diagnostic("SECURITY.ACCESS_DENIED",
                                   "sblr.parameter_bind.load_denied",
                                   "engine-owned private statement receipt required");
    return result;
  }
  return LoadBindLocked(context, parameter_set_descriptor_uuid);
}

EngineApiDiagnostic BeginSblrParameterSetRegistryRecovery(
    const EngineRequestContext& context) {
  std::lock_guard lock(RegistryMutex());
  if (!HasAdmin(context)) {
    return Diagnostic("SECURITY.ACCESS_DENIED","sblr.parameter_set.recovery_denied","startup recovery authority required");
  }
  if (context.database_path.empty() ||
      !ValidUuid(context.database_uuid, scratchbird::core::platform::UuidKind::database)) {
    return Diagnostic("SBLR.OPERAND_INVALID","sblr.parameter_set.recovery_identity_invalid",
                      "exact database identity required for recovery");
  }
  for (auto it=LiveSets().begin();it!=LiveSets().end();) {
    if (it->second.database_uuid==context.database_uuid) it=LiveSets().erase(it);
    else ++it;
  }
  return MakeEngineApiDiagnostic("OK","ok",{},false);
}

SblrParameterSetMutationResult InvalidateSblrParameterSet(
    const EngineRequestContext& context,const EngineUuid& uuid,
    const EngineUuid& expected_uuid,std::uint64_t expected_generation,
    const std::string& reason) {
  std::lock_guard lock(RegistryMutex()); SblrParameterSetMutationResult result;
  if (!HasAdmin(context) && !HasPrivateReceiptAuthority(context)) { result.diagnostic=Diagnostic("SECURITY.ACCESS_DENIED","sblr.parameter_set.invalidate_denied","admin or engine-owned receipt authority required"); return result; }
  auto loaded=LoadLocked(context,uuid); if(!loaded.ok){result.diagnostic=loaded.diagnostic;return result;}
  if(loaded.snapshot.snapshot_uuid!=expected_uuid||loaded.snapshot.snapshot_generation!=expected_generation){result.diagnostic=Diagnostic("SBLR.PARAMETER.STALE","sblr.parameter_set.compare_stale","snapshot compare failed");return result;}
  if(!SafeReason(reason)){result.diagnostic=Diagnostic("SBLR.OPERAND_INVALID","sblr.parameter_set.reason_invalid","canonical reason required");return result;}
  if (loaded.snapshot.state != SblrParameterSetState::active ||
      loaded.snapshot.snapshot_generation != 1 || loaded.snapshot.descriptor_generation != 1) {
    result.diagnostic = Diagnostic("SBLR.PARAMETER.STALE",
        "sblr.parameter_set.already_revoked", "only an active initial generation can be revoked");
    return result;
  }
  auto next = loaded.snapshot;
  next.snapshot_uuid = GenerateUuid();
  next.snapshot_generation = 2;
  next.descriptor_generation = 2;
  next.state = SblrParameterSetState::revoked;
  next.decision_evidence_sha256 = Sha256(SnapshotMaterial(
      next, loaded.snapshot.snapshot_uuid, loaded.snapshot.snapshot_generation, reason));
  if (next.snapshot_uuid.is_nil() || !ValidSha256(next.decision_evidence_sha256)) {
    result.diagnostic = Diagnostic("SBLR.EXECUTION_FAILED",
        "sblr.parameter_set.invalidate_failed", "revocation identity or hash authority unavailable");
    return result;
  }
  result.snapshot = next;
  result.diagnostic = MakeEngineApiDiagnostic("OK", "ok", {}, false);
  result.evidence.push_back({"sblr.parameter_set.invalidate", next.decision_evidence_sha256});
  // Revoke live authority before attempting durable invalidation; a torn write
  // must never leave an executable descriptor in this process.
  LiveSets().erase(uuid);
  if (!Publish(context, next, loaded.snapshot.snapshot_uuid,
               loaded.snapshot.snapshot_generation, reason)) {
    result = {};
    result.diagnostic = Diagnostic("SBLR.EXECUTION_FAILED",
        "sblr.parameter_set.invalidate_failed", "durable invalidation failed");
    return result;
  }
  result.ok = true;
  return result;
}

EngineApiDiagnostic RevalidateSblrParameterSet(
    const EngineRequestContext& context,const SblrParameterSetSnapshot& admitted,
    const EngineUuid& receipt,const EngineUuid& execution,
    const EngineUuid& prepared,std::uint64_t prepared_generation,
    const EngineUuid& batch,std::uint64_t batch_generation,
    const EngineUuid& dynamic,std::uint64_t dynamic_generation,
    SblrParameterSetSnapshot* current) {
  std::lock_guard lock(RegistryMutex());
  const auto loaded = LoadLocked(context, admitted.parameter_set_descriptor_uuid);
  if (!loaded.ok) return loaded.diagnostic;
  if (!LiveAuthority(context, loaded.snapshot) || !SameSnapshot(admitted, loaded.snapshot))
    return Diagnostic("SBLR.PARAMETER.STALE", "sblr.parameter_set.execution_not_live",
                      "receipt execution authority does not recover");
  if(loaded.snapshot.state!=SblrParameterSetState::active)return Diagnostic("SBLR.PARAMETER.STALE","sblr.parameter_set.revoked","parameter set revoked");
  if(loaded.snapshot.snapshot_uuid!=admitted.snapshot_uuid||loaded.snapshot.snapshot_generation!=admitted.snapshot_generation||loaded.snapshot.descriptor_generation!=admitted.descriptor_generation||
     loaded.snapshot.session_uuid!=context.session_uuid||loaded.snapshot.statement_receipt_uuid!=receipt||loaded.snapshot.execution_uuid!=execution||
     loaded.snapshot.prepared_statement_uuid!=prepared||loaded.snapshot.prepared_generation!=prepared_generation||loaded.snapshot.batch_uuid!=batch||loaded.snapshot.batch_generation!=batch_generation||loaded.snapshot.dynamic_package_uuid!=dynamic||loaded.snapshot.dynamic_generation!=dynamic_generation||
     loaded.snapshot.catalog_generation!=context.catalog_generation_id||loaded.snapshot.security_epoch!=context.security_epoch||loaded.snapshot.resource_epoch!=context.resource_epoch)
    return Diagnostic("SBLR.PARAMETER.STALE","sblr.parameter_set.binding_stale","immutable binding changed");
  if (current) {
    auto staged = loaded.snapshot;
    *current = std::move(staged);
  }
  return MakeEngineApiDiagnostic("OK","ok",{},false);
}
}  // namespace scratchbird::engine::internal_api
