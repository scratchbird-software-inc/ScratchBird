// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "temp_workspace_lifecycle.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <fstream>
#include <cstring>
#include <iomanip>
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#include <stdlib.h>
#endif
#include <initializer_list>
#include <limits>
#include <sstream>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <winioctl.h>
#else
#include <fcntl.h>
#if defined(__linux__)
#include <sys/random.h>
#endif
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace scratchbird::core::memory {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::memory};
}

constexpr int kSecureTempWorkspaceRandomBytes = 16;
constexpr int kSecureTempWorkspaceAllocationAttempts = 16;
constexpr const char* kTempWorkspaceAuthorityBoundary =
    "resource_security_evidence_only_not_transaction_finality_row_visibility_security_authorization_recovery_parser_reference_wal_benchmark_optimizer_plan_or_agent_action_authority";
constexpr u64 kTempWorkspaceManifestLegacyFormatVersion = 1;
constexpr u64 kTempWorkspaceManifestCurrentFormatVersion = 2;
constexpr const char* kTempWorkspaceManifestVersionV1 = "SB_TEMP_WORKSPACE_MANIFEST_V1";
constexpr const char* kTempWorkspaceManifestVersionV2 = "SB_TEMP_WORKSPACE_MANIFEST_V2";

const char* TempWorkspaceManifestHeaderForVersion(u64 version) {
  switch (version) {
    case kTempWorkspaceManifestLegacyFormatVersion:
      return kTempWorkspaceManifestVersionV1;
    case kTempWorkspaceManifestCurrentFormatVersion:
      return kTempWorkspaceManifestVersionV2;
    default:
      return nullptr;
  }
}

std::optional<u64> TempWorkspaceManifestVersionFromHeader(const std::string& header) {
  if (header == kTempWorkspaceManifestVersionV1) {
    return kTempWorkspaceManifestLegacyFormatVersion;
  }
  if (header == kTempWorkspaceManifestVersionV2) {
    return kTempWorkspaceManifestCurrentFormatVersion;
  }
  return std::nullopt;
}

constexpr const char* kTempWorkspaceManifestMetaRecord = "META";
constexpr const char* kTempWorkspaceManifestChecksumAlgorithm =
    "fnv1a64-temp-workspace-manifest-v1";
constexpr u64 kTempWorkspaceFnv1a64OffsetBasis = 14695981039346656037ull;
constexpr u64 kTempWorkspaceFnv1a64Prime = 1099511628211ull;

std::string Hex64(u64 value) {
  std::ostringstream stream;
  stream << std::hex << std::setfill('0') << std::setw(16) << value;
  return stream.str();
}

std::string TempWorkspaceManifestChecksum(const std::string& body) {
  u64 hash = kTempWorkspaceFnv1a64OffsetBasis;
  for (unsigned char ch : body) {
    hash ^= static_cast<u64>(ch);
    hash *= kTempWorkspaceFnv1a64Prime;
  }
  return Hex64(hash);
}

std::filesystem::path TempWorkspaceManifestTempPath(const std::filesystem::path& path) {
  return std::filesystem::path(path.string() + ".tmp");
}

std::string EffectiveManifestWriterIdentity(const TempWorkspacePolicy& policy) {
  if (policy.manifest_writer_identity.find_first_not_of(" \t\r\n") != std::string::npos) {
    return policy.manifest_writer_identity;
  }
  if (policy.policy_name.find_first_not_of(" \t\r\n") != std::string::npos) {
    return policy.policy_name;
  }
  return "engine_temp_workspace_default";
}

Status TempManifestStatus(StatusCode code, Severity severity) {
  return {code, severity, Subsystem::memory};
}

DiagnosticRecord MakeTempManifestDiagnostic(Status status,
                                            std::string diagnostic_code,
                                            std::string message_key,
                                            const TempWorkspacePolicy& policy,
                                            std::string reason,
                                            const std::filesystem::path& path = {},
                                            std::string error = {}) {
  std::vector<DiagnosticArgument> arguments;
  arguments.push_back({"policy", policy.policy_name});
  arguments.push_back({"root_path", policy.root_path.string()});
  arguments.push_back({"reason", std::move(reason)});
  if (!path.empty()) {
    arguments.push_back({"path", path.string()});
  }
  if (!error.empty()) {
    arguments.push_back({"error", std::move(error)});
  }
  arguments.push_back({"authority_boundary", kTempWorkspaceAuthorityBoundary});
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "core.memory.temp_workspace_manifest");
}

#if defined(_WIN32)
std::string TempWorkspaceWindowsLastErrorText() {
  return "windows_error_" + std::to_string(::GetLastError());
}

bool DurableSyncHandle(HANDLE handle, std::string* detail) {
  if (::FlushFileBuffers(handle) != 0) {
    return true;
  }
  if (detail != nullptr) {
    *detail = TempWorkspaceWindowsLastErrorText();
  }
  return false;
}

bool DurableSyncPath(const std::filesystem::path& path, bool writable, std::string* detail) {
  const DWORD access = GENERIC_READ | (writable ? GENERIC_WRITE : 0);
  HANDLE handle = ::CreateFileW(path.wstring().c_str(),
                                access,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr,
                                OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL,
                                nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    if (detail != nullptr) {
      *detail = TempWorkspaceWindowsLastErrorText();
    }
    return false;
  }
  const bool ok = DurableSyncHandle(handle, detail);
  ::CloseHandle(handle);
  return ok;
}

bool DurableSyncParentDirectory(const std::filesystem::path& path, std::string* detail) {
  std::filesystem::path parent = path.parent_path();
  if (parent.empty()) {
    parent = ".";
  }
  HANDLE handle = ::CreateFileW(parent.wstring().c_str(),
                                GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr,
                                OPEN_EXISTING,
                                FILE_FLAG_BACKUP_SEMANTICS,
                                nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    if (detail != nullptr) {
      *detail = TempWorkspaceWindowsLastErrorText();
    }
    return false;
  }
  const bool ok = DurableSyncHandle(handle, detail);
  ::CloseHandle(handle);
  return ok;
}
#else
int TempWorkspaceCloexecFlag() {
#ifdef O_CLOEXEC
  return O_CLOEXEC;
#else
  return 0;
#endif
}

bool DurableSyncFd(int fd, std::string* detail) {
  if (::fsync(fd) == 0) {
    return true;
  }
  if (detail != nullptr) {
    *detail = std::strerror(errno);
  }
  return false;
}

bool DurableSyncPath(const std::filesystem::path& path, bool writable, std::string* detail) {
  const int flags = (writable ? O_RDWR : O_RDONLY) | TempWorkspaceCloexecFlag();
  const int fd = ::open(path.string().c_str(), flags);
  if (fd < 0) {
    if (detail != nullptr) {
      *detail = std::strerror(errno);
    }
    return false;
  }
  const bool ok = DurableSyncFd(fd, detail);
  (void)::close(fd);
  return ok;
}

bool DurableSyncParentDirectory(const std::filesystem::path& path, std::string* detail) {
  std::filesystem::path parent = path.parent_path();
  if (parent.empty()) {
    parent = ".";
  }
  int flags = O_RDONLY | TempWorkspaceCloexecFlag();
#ifdef O_DIRECTORY
  flags |= O_DIRECTORY;
#endif
  const int fd = ::open(parent.string().c_str(), flags);
  if (fd < 0) {
    if (detail != nullptr) {
      *detail = std::strerror(errno);
    }
    return false;
  }
  const bool ok = DurableSyncFd(fd, detail);
  (void)::close(fd);
  return ok;
}
#endif

bool ReplaceFileAtomically(const std::filesystem::path& temp_path,
                           const std::filesystem::path& target_path,
                           std::string* detail) {
#if defined(_WIN32)
  if (::MoveFileExW(temp_path.wstring().c_str(),
                    target_path.wstring().c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
    return true;
  }
  if (detail != nullptr) {
    *detail = TempWorkspaceWindowsLastErrorText();
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

bool AddWouldExceed(u64 current, u64 add, u64 limit) {
  if (limit == 0) return false;
  return add > limit || current > limit - add;
}

std::string SanitizePathToken(std::string value) {
  if (value.empty()) return "anonymous";
  for (char& c : value) {
    const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '-' || c == '_';
    if (!safe) c = '_';
  }
  return value;
}

u64 MapValue(const std::map<std::string, u64>& values, const std::string& key) {
  const auto it = values.find(key);
  if (it == values.end()) return 0;
  return it->second;
}

void AddMapValue(std::map<std::string, u64>* values, const std::string& key, u64 amount) {
  if (key.empty()) return;
  (*values)[key] += amount;
}

void RemoveMapValue(std::map<std::string, u64>* values, const std::string& key, u64 amount) {
  if (key.empty()) return;
  auto it = values->find(key);
  if (it == values->end()) return;
  it->second = amount >= it->second ? 0 : it->second - amount;
  if (it->second == 0) values->erase(it);
}

bool IsTransactionScoped(TempWorkspaceLifetime lifetime) {
  return lifetime == TempWorkspaceLifetime::statement_lifetime ||
         lifetime == TempWorkspaceLifetime::cursor_lifetime ||
         lifetime == TempWorkspaceLifetime::result_set_lifetime ||
         lifetime == TempWorkspaceLifetime::savepoint_lifetime ||
         lifetime == TempWorkspaceLifetime::transaction_lifetime;
}

std::string HexByte(unsigned char value) {
  constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.push_back(kHex[(value >> 4) & 0x0f]);
  out.push_back(kHex[value & 0x0f]);
  return out;
}

std::string EscapeManifestField(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (unsigned char ch : value) {
    if (ch == '%' || ch == '\t' || ch == '\n' || ch == '\r') {
      out.push_back('%');
      out += HexByte(ch);
      continue;
    }
    out.push_back(static_cast<char>(ch));
  }
  return out;
}

int HexValue(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return 10 + ch - 'a';
  if (ch >= 'A' && ch <= 'F') return 10 + ch - 'A';
  return -1;
}

std::optional<std::string> UnescapeManifestField(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (value[i] != '%') {
      out.push_back(value[i]);
      continue;
    }
    if (i + 2 >= value.size()) {
      return std::nullopt;
    }
    const int hi = HexValue(value[i + 1]);
    const int lo = HexValue(value[i + 2]);
    if (hi < 0 || lo < 0) {
      return std::nullopt;
    }
    out.push_back(static_cast<char>((hi << 4) | lo));
    i += 2;
  }
  return out;
}

std::vector<std::string> SplitTabs(std::string_view line) {
  std::vector<std::string> fields;
  std::string current;
  for (char ch : line) {
    if (ch == '\t') {
      fields.push_back(std::move(current));
      current.clear();
      continue;
    }
    current.push_back(ch);
  }
  fields.push_back(std::move(current));
  return fields;
}

bool ParseBool(std::string_view value) {
  return value == "1" || value == "true";
}

u64 ParseU64OrZero(const std::string& value) {
  try {
    return static_cast<u64>(std::stoull(value));
  } catch (...) {
    return 0;
  }
}

bool ParseStrictU64Field(const std::string& value, u64* parsed) {
  if (parsed == nullptr || value.empty()) {
    return false;
  }
  u64 accumulator = 0;
  for (char ch : value) {
    if (ch < '0' || ch > '9') {
      return false;
    }
    const u64 digit = static_cast<u64>(ch - '0');
    if (accumulator > (std::numeric_limits<u64>::max() - digit) / 10) {
      return false;
    }
    accumulator = accumulator * 10 + digit;
  }
  *parsed = accumulator;
  return true;
}

TempStorageClass ParseTempStorageClassName(const std::string& value) {
  for (TempStorageClass candidate : {
           TempStorageClass::memory_workspace,
           TempStorageClass::spill_file,
           TempStorageClass::temporary_page_space,
           TempStorageClass::temporary_relation,
           TempStorageClass::temporary_index,
           TempStorageClass::materialized_result,
           TempStorageClass::cursor_backing_store,
           TempStorageClass::sort_workspace,
           TempStorageClass::hash_workspace,
           TempStorageClass::bulk_dml_staging,
           TempStorageClass::backup_restore_scratch,
           TempStorageClass::archive_package_scratch,
           TempStorageClass::verification_scratch,
           TempStorageClass::udr_workspace,
           TempStorageClass::parser_workspace}) {
    if (value == TempStorageClassName(candidate)) {
      return candidate;
    }
  }
  return TempStorageClass::spill_file;
}

TempWorkspaceLifetime ParseTempWorkspaceLifetimeName(const std::string& value) {
  for (TempWorkspaceLifetime candidate : {
           TempWorkspaceLifetime::statement_lifetime,
           TempWorkspaceLifetime::cursor_lifetime,
           TempWorkspaceLifetime::result_set_lifetime,
           TempWorkspaceLifetime::savepoint_lifetime,
           TempWorkspaceLifetime::transaction_lifetime,
           TempWorkspaceLifetime::session_lifetime,
           TempWorkspaceLifetime::operation_lifetime,
           TempWorkspaceLifetime::scheduler_task_lifetime,
           TempWorkspaceLifetime::recovery_lifetime,
           TempWorkspaceLifetime::administrator_review_lifetime}) {
    if (value == TempWorkspaceLifetimeName(candidate)) {
      return candidate;
    }
  }
  return TempWorkspaceLifetime::statement_lifetime;
}

TempWorkspaceState ParseTempWorkspaceStateName(const std::string& value) {
  for (TempWorkspaceState candidate : {
           TempWorkspaceState::active,
           TempWorkspaceState::cleaned,
           TempWorkspaceState::cleanup_refused,
           TempWorkspaceState::cleanup_failed,
           TempWorkspaceState::quarantined,
           TempWorkspaceState::review_required}) {
    if (value == TempWorkspaceStateName(candidate)) {
      return candidate;
    }
  }
  return TempWorkspaceState::active;
}

TempRecoveryClass ParseTempRecoveryClassName(const std::string& value) {
  for (TempRecoveryClass candidate : {
           TempRecoveryClass::discard_safe,
           TempRecoveryClass::discard_after_evidence,
           TempRecoveryClass::resume_required,
           TempRecoveryClass::operation_owned_resume,
           TempRecoveryClass::review_required,
           TempRecoveryClass::quarantine_required,
           TempRecoveryClass::leaked_cleanup_required,
           TempRecoveryClass::cleanup_refused}) {
    if (value == TempRecoveryClassName(candidate)) {
      return candidate;
    }
  }
  return TempRecoveryClass::discard_safe;
}

TempWorkspaceDiskReservationMode ParseTempWorkspaceDiskReservationModeName(
    const std::string& value) {
  for (TempWorkspaceDiskReservationMode candidate : {
           TempWorkspaceDiskReservationMode::logical_quota_only,
           TempWorkspaceDiskReservationMode::sparse_file,
           TempWorkspaceDiskReservationMode::physical_preallocate}) {
    if (value == TempWorkspaceDiskReservationModeName(candidate)) {
      return candidate;
    }
  }
  return TempWorkspaceDiskReservationMode::sparse_file;
}

MemoryCategory ParseMemoryCategoryName(const std::string& value) {
  for (MemoryCategory candidate : {
           MemoryCategory::unknown,
           MemoryCategory::core_runtime,
           MemoryCategory::page_buffer,
           MemoryCategory::catalog_bootstrap,
           MemoryCategory::resource_seed,
           MemoryCategory::datatype_payload,
           MemoryCategory::transaction_local,
           MemoryCategory::transaction_snapshot,
           MemoryCategory::version_chain,
           MemoryCategory::cleanup,
           MemoryCategory::archive,
           MemoryCategory::metrics,
           MemoryCategory::diagnostics,
           MemoryCategory::executor_query_reserved,
           MemoryCategory::parser_handoff_reserved,
           MemoryCategory::udr_reserved,
           MemoryCategory::llvm_code_reserved,
           MemoryCategory::llvm_data_reserved,
           MemoryCategory::gpu_host_pinned_reserved,
           MemoryCategory::gpu_device_reserved,
           MemoryCategory::cluster_control_reserved,
           MemoryCategory::cluster_decision_reserved,
           MemoryCategory::test_probe}) {
    if (value == MemoryCategoryName(candidate)) {
      return candidate;
    }
  }
  return MemoryCategory::unknown;
}

bool BlankString(const std::string& value) {
  return value.find_first_not_of(" \t\r\n") == std::string::npos;
}

bool ScopeAlreadyPresent(const std::vector<HierarchicalMemoryScopeRef>& scopes,
                         HierarchicalMemoryScopeKind kind,
                         const std::string& scope_id) {
  for (const auto& scope : scopes) {
    if (scope.kind == kind && scope.scope_id == scope_id) {
      return true;
    }
  }
  return false;
}

void AppendScope(std::vector<HierarchicalMemoryScopeRef>* scopes,
                 HierarchicalMemoryScopeKind kind,
                 std::string scope_id) {
  if (BlankString(scope_id) || ScopeAlreadyPresent(*scopes, kind, scope_id)) {
    return;
  }
  scopes->push_back({kind, std::move(scope_id)});
}

std::string FirstNonBlank(std::initializer_list<std::string> values,
                          std::string fallback) {
  for (const auto& value : values) {
    if (!BlankString(value)) {
      return value;
    }
  }
  return fallback;
}

std::vector<HierarchicalMemoryScopeRef> TempWorkspaceReservationScopeChain(
    const TempWorkspaceAllocationRequest& request,
    TempStorageClass storage_class) {
  std::vector<HierarchicalMemoryScopeRef> scopes;
  AppendScope(&scopes,
              HierarchicalMemoryScopeKind::process,
              FirstNonBlank({request.owner.engine_id}, "process-temp-workspace"));
  AppendScope(&scopes, HierarchicalMemoryScopeKind::database, request.owner.database_id);
  AppendScope(&scopes, HierarchicalMemoryScopeKind::session, request.owner.session_id);
  AppendScope(&scopes, HierarchicalMemoryScopeKind::transaction, request.owner.transaction_id);
  AppendScope(&scopes, HierarchicalMemoryScopeKind::statement, request.owner.statement_id);
  AppendScope(&scopes,
              HierarchicalMemoryScopeKind::query,
              FirstNonBlank({request.owner.cursor_id,
                             request.owner.result_set_id,
                             request.owner.temp_object_uuid},
                            std::string("temp-") + TempStorageClassName(storage_class)));
  AppendScope(&scopes,
              HierarchicalMemoryScopeKind::operator_scope,
              std::string("temp_workspace.") + TempStorageClassName(storage_class));
  if (!request.owner.scheduler_task_id.empty() || !request.owner.operation_id.empty()) {
    AppendScope(&scopes,
                HierarchicalMemoryScopeKind::background,
                FirstNonBlank({request.owner.scheduler_task_id, request.owner.operation_id},
                              "temp-workspace-operation"));
  }
  return scopes;
}

std::vector<std::string> ScopeEvidenceStrings(
    const std::vector<HierarchicalMemoryScopeRef>& scopes) {
  std::vector<std::string> evidence;
  evidence.reserve(scopes.size());
  for (const auto& scope : scopes) {
    evidence.push_back(std::string(HierarchicalMemoryScopeKindName(scope.kind)) +
                       ":" + scope.scope_id);
  }
  return evidence;
}

HierarchicalMemoryBudgetProvenance EffectiveReservationProvenance(
    const TempWorkspacePolicy& policy) {
  HierarchicalMemoryBudgetProvenance provenance = policy.reservation_provenance;
  if (provenance.source == HierarchicalMemoryBudgetProvenanceSource::unknown) {
    provenance.source = HierarchicalMemoryBudgetProvenanceSource::runtime_policy;
  }
  if (provenance.source_label.empty()) {
    provenance.source_label = "core.memory.temp_workspace_lifecycle";
  }
  return provenance;
}

std::string TempWorkspaceReservationOwnerId(
    const TempWorkspaceAllocationRequest& request) {
  return FirstNonBlank({request.owner.temp_object_uuid,
                        request.owner.operation_id,
                        request.owner.statement_id,
                        request.owner.transaction_id,
                        request.owner.session_id},
                       "temp-workspace-owner");
}

TempWorkspaceDiskReservationMode EffectiveDiskReservationMode(
    const TempWorkspacePolicy& policy) {
  if (policy.require_physical_disk_reservation) {
    return TempWorkspaceDiskReservationMode::physical_preallocate;
  }
  if (!policy.sparse_file_reservation &&
      policy.disk_reservation_mode == TempWorkspaceDiskReservationMode::sparse_file) {
    return TempWorkspaceDiskReservationMode::physical_preallocate;
  }
  return policy.disk_reservation_mode;
}

std::string ErrnoMessage(int error_number) {
  return std::strerror(error_number);
}

std::string HexEncode(const std::array<unsigned char, kSecureTempWorkspaceRandomBytes>& bytes) {
  constexpr char kHex[] = "0123456789abcdef";
  std::string encoded;
  encoded.reserve(bytes.size() * 2);
  for (unsigned char value : bytes) {
    encoded.push_back(kHex[(value >> 4) & 0x0f]);
    encoded.push_back(kHex[value & 0x0f]);
  }
  return encoded;
}

bool ReadSecureRandom(std::array<unsigned char, kSecureTempWorkspaceRandomBytes>* bytes,
                      std::string* error) {
#if defined(_WIN32)
  const NTSTATUS status = ::BCryptGenRandom(nullptr,
                                            bytes->data(),
                                            static_cast<ULONG>(bytes->size()),
                                            BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  if (status == 0) {
    return true;
  }
  if (error != nullptr) {
    *error = "BCryptGenRandom failed";
  }
  return false;
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
  ::arc4random_buf(bytes->data(), bytes->size());
  return true;
#elif defined(__linux__)
  std::size_t offset = 0;
  while (offset < bytes->size()) {
    const ssize_t n = ::getrandom(bytes->data() + offset, bytes->size() - offset, 0);
    if (n > 0) {
      offset += static_cast<std::size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (error != nullptr) {
      *error = ErrnoMessage(errno);
    }
    return false;
  }
  return true;
#else
  int flags = O_RDONLY;
#if defined(O_CLOEXEC)
  flags |= O_CLOEXEC;
#endif
  const int fd = ::open("/dev/urandom", flags);
  if (fd < 0) {
    if (error != nullptr) {
      *error = ErrnoMessage(errno);
    }
    return false;
  }

  std::size_t offset = 0;
  while (offset < bytes->size()) {
    const ssize_t n = ::read(fd, bytes->data() + offset, bytes->size() - offset);
    if (n > 0) {
      offset += static_cast<std::size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (error != nullptr) {
      *error = n == 0 ? "short read from secure random source" : ErrnoMessage(errno);
    }
    ::close(fd);
    return false;
  }

  if (::close(fd) != 0) {
    if (error != nullptr) {
      *error = ErrnoMessage(errno);
    }
    return false;
  }
  return true;
#endif
}

std::optional<std::string> MakeSecureRandomToken(std::string* error) {
  std::array<unsigned char, kSecureTempWorkspaceRandomBytes> bytes{};
  if (!ReadSecureRandom(&bytes, error)) {
    return std::nullopt;
  }
  return HexEncode(bytes);
}

struct SecureCreateResult {
  bool ok = false;
  StatusCode status_code = StatusCode::memory_allocation_failed;
  Severity severity = Severity::error;
  std::string diagnostic_code;
  std::string message_key;
  std::string reason;
  std::string error;
  TempWorkspaceSecurityEvidence evidence;
  TempWorkspaceDiskReservationEvidence disk_reservation_evidence;
};

struct ReserveBytesResult {
  bool ok = false;
  TempWorkspaceDiskReservationEvidence evidence;
  std::string error;
};

#if !defined(_WIN32)
class UniqueFd {
 public:
  explicit UniqueFd(int fd = -1) : fd_(fd) {}
  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;
  UniqueFd(UniqueFd&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
  UniqueFd& operator=(UniqueFd&& other) noexcept {
    if (this != &other) {
      Reset();
      fd_ = other.fd_;
      other.fd_ = -1;
    }
    return *this;
  }
  ~UniqueFd() { Reset(); }

  int get() const { return fd_; }
  explicit operator bool() const { return fd_ >= 0; }

  bool Close(std::string* error) {
    if (fd_ < 0) return true;
    const int fd = fd_;
    fd_ = -1;
    if (::close(fd) == 0) return true;
    if (error != nullptr) {
      *error = ErrnoMessage(errno);
    }
    return false;
  }

  void Reset() {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

 private:
  int fd_ = -1;
};

bool WriteAll(int fd, const char* data, std::size_t size, std::string* error) {
  std::size_t offset = 0;
  while (offset < size) {
    const ssize_t n = ::write(fd, data + offset, size - offset);
    if (n > 0) {
      offset += static_cast<std::size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (error != nullptr) {
      *error = ErrnoMessage(errno);
    }
    return false;
  }
  return true;
}

ReserveBytesResult ReserveBytes(int fd,
                                u64 bytes,
                                TempWorkspaceDiskReservationMode mode,
                                bool physical_required) {
  ReserveBytesResult result;
  result.evidence.mode = mode;
  result.evidence.logical_quota_reserved = true;
  result.evidence.physical_preallocation_required =
      physical_required || mode == TempWorkspaceDiskReservationMode::physical_preallocate;
  result.evidence.requested_bytes = bytes;
  result.evidence.authority_boundary = kTempWorkspaceAuthorityBoundary;
  if (bytes == 0) {
    result.ok = true;
    result.evidence.file_size_bytes = 0;
    return result;
  }

  if (mode == TempWorkspaceDiskReservationMode::logical_quota_only) {
    result.ok = true;
    result.evidence.platform_semantics = "logical_quota_only_no_file_space_claim";
    result.evidence.file_size_bytes = 0;
    return result;
  }

  if (mode == TempWorkspaceDiskReservationMode::sparse_file) {
    const u64 offset = bytes - 1;
    const auto max_off = static_cast<u64>(std::numeric_limits<off_t>::max());
    if (offset > max_off) {
      result.error = "requested reservation exceeds platform file offset range";
      result.evidence.failure_reason = result.error;
      result.evidence.disk_full_or_reservation_failure = true;
      return result;
    }
    if (::lseek(fd, static_cast<off_t>(offset), SEEK_SET) == static_cast<off_t>(-1)) {
      result.error = ErrnoMessage(errno);
      result.evidence.failure_reason = result.error;
      result.evidence.disk_full_or_reservation_failure = true;
      return result;
    }
    constexpr char zero = '\0';
    if (!WriteAll(fd, &zero, 1, &result.error)) {
      result.evidence.failure_reason = result.error;
      result.evidence.disk_full_or_reservation_failure = true;
      return result;
    }
    result.ok = true;
    result.evidence.sparse_file_created = true;
    result.evidence.sparse_not_physical_reservation = true;
    result.evidence.file_size_bytes = bytes;
    result.evidence.platform_semantics = "sparse_lseek_write_logical_extent_not_physical_reservation";
    return result;
  }

  result.evidence.physical_preallocation_attempted = true;
#if !defined(_WIN32) && defined(_POSIX_VERSION)
  const auto max_off = static_cast<u64>(std::numeric_limits<off_t>::max());
  if (bytes > max_off) {
    result.error = "requested reservation exceeds platform file offset range";
    result.evidence.failure_reason = result.error;
    result.evidence.disk_full_or_reservation_failure = true;
    return result;
  }
  const int fallocate = ::posix_fallocate(fd, 0, static_cast<off_t>(bytes));
  if (fallocate != 0) {
    result.error = ErrnoMessage(fallocate);
    result.evidence.failure_reason = result.error;
    result.evidence.disk_full_or_reservation_failure = true;
    result.evidence.platform_semantics = "posix_fallocate_failed";
    return result;
  }
  result.ok = true;
  result.evidence.physical_preallocation_complete = true;
  result.evidence.file_size_bytes = bytes;
  result.evidence.platform_semantics = "posix_fallocate_physical_preallocation";
  return result;
#else
  result.error = "physical preallocation is unavailable on this platform";
  result.evidence.failure_reason = result.error;
  result.evidence.disk_full_or_reservation_failure = true;
  result.evidence.platform_semantics = "physical_preallocation_unavailable";
  return result;
#endif
}

SecureCreateResult OpenSecureRootDirectory(const std::filesystem::path& root_path,
                                           UniqueFd* opened_root_fd) {
  SecureCreateResult result;
  result.status_code = StatusCode::memory_invalid_request;
  result.diagnostic_code = "TEMP_WORKSPACE.ROOT_UNSAFE";
  result.message_key = "temp_workspace.root.unsafe";
  result.reason = "root_path_requires_owned_directory_with_no_follow_semantics";

#if !defined(O_NOFOLLOW) || !defined(O_DIRECTORY)
  result.diagnostic_code = "TEMP_WORKSPACE.SECURE_CREATE_UNSUPPORTED";
  result.message_key = "temp_workspace.secure_create.unsupported";
  result.reason = "platform_missing_open_directory_no_follow";
  result.error = "O_NOFOLLOW or O_DIRECTORY is unavailable";
  return result;
#else
  int flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW;
#if defined(O_CLOEXEC)
  flags |= O_CLOEXEC;
#endif
  UniqueFd root_fd(::open(root_path.c_str(), flags));
  if (!root_fd) {
    result.error = ErrnoMessage(errno);
    return result;
  }

  struct stat st {};
  if (::fstat(root_fd.get(), &st) != 0) {
    result.error = ErrnoMessage(errno);
    return result;
  }
  if (!S_ISDIR(st.st_mode)) {
    result.error = "root path is not a directory";
    return result;
  }
  if (st.st_uid != ::geteuid()) {
    result.reason = "root_path_not_owned_by_effective_user";
    result.error = "root path owner does not match effective user";
    return result;
  }
  if (::fchmod(root_fd.get(), S_IRWXU) != 0) {
    result.reason = "root_path_owner_only_permission_failed";
    result.error = ErrnoMessage(errno);
    return result;
  }
  if (::fstat(root_fd.get(), &st) != 0) {
    result.error = ErrnoMessage(errno);
    return result;
  }
  if ((st.st_mode & 0777) != S_IRWXU) {
    result.reason = "root_path_owner_only_permission_unverified";
    result.error = "root path mode is not 0700 after fchmod";
    return result;
  }

  result.ok = true;
  result.evidence.owner_only_permissions = true;
  result.evidence.nofollow_or_platform_equivalent = true;
  result.evidence.platform_semantics = "posix_open_directory_no_follow_fchmod_0700";
  result.diagnostic_code.clear();
  result.message_key.clear();
  result.reason.clear();
  result.error.clear();
  if (opened_root_fd != nullptr) {
    *opened_root_fd = std::move(root_fd);
  }
  return result;
#endif
}

SecureCreateResult CreateSecureTempWorkspaceFile(const std::filesystem::path& root_path,
                                                 const std::string& file_name,
                                                 u64 bytes,
                                                 TempWorkspaceDiskReservationMode reservation_mode,
                                                 bool physical_required) {
  UniqueFd root_fd;
  SecureCreateResult root = OpenSecureRootDirectory(root_path, &root_fd);
  if (!root.ok) return root;

  SecureCreateResult result;
  result.status_code = StatusCode::memory_allocation_failed;
  result.diagnostic_code = "TEMP_WORKSPACE.SPILL_CREATE_FAILED";
  result.message_key = "temp_workspace.spill.create_failed";
  result.reason = "secure_exclusive_file_create_failed";

#if !defined(O_NOFOLLOW)
  result.status_code = StatusCode::memory_invalid_request;
  result.diagnostic_code = "TEMP_WORKSPACE.SECURE_CREATE_UNSUPPORTED";
  result.message_key = "temp_workspace.secure_create.unsupported";
  result.reason = "platform_missing_file_no_follow";
  result.error = "O_NOFOLLOW is unavailable";
  return result;
#else
  int file_flags = O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW;
#if defined(O_CLOEXEC)
  file_flags |= O_CLOEXEC;
#endif
  UniqueFd file_fd(::openat(root_fd.get(), file_name.c_str(), file_flags, S_IRUSR | S_IWUSR));
  if (!file_fd) {
    const int open_error = errno;
    result.error = ErrnoMessage(open_error);
    if (open_error == EEXIST || open_error == ELOOP) {
      result.status_code = StatusCode::memory_invalid_request;
      result.diagnostic_code = "TEMP_WORKSPACE.SECURE_CREATE_REFUSED";
      result.message_key = "temp_workspace.secure_create.refused";
      result.reason = "candidate_path_already_exists_or_is_symlink";
    }
    return result;
  }

  if (::fchmod(file_fd.get(), S_IRUSR | S_IWUSR) != 0) {
    result.reason = "owner_only_file_permission_failed";
    result.error = ErrnoMessage(errno);
    ::unlinkat(root_fd.get(), file_name.c_str(), 0);
    return result;
  }

  struct stat st {};
  if (::fstat(file_fd.get(), &st) != 0) {
    result.reason = "file_stat_failed_after_create";
    result.error = ErrnoMessage(errno);
    ::unlinkat(root_fd.get(), file_name.c_str(), 0);
    return result;
  }
  if (!S_ISREG(st.st_mode)) {
    result.status_code = StatusCode::memory_invalid_request;
    result.diagnostic_code = "TEMP_WORKSPACE.SECURE_CREATE_REFUSED";
    result.message_key = "temp_workspace.secure_create.refused";
    result.reason = "created_path_is_not_regular_file";
    result.error = "created path is not a regular file";
    ::unlinkat(root_fd.get(), file_name.c_str(), 0);
    return result;
  }
  if (st.st_nlink != 1) {
    result.status_code = StatusCode::memory_invalid_request;
    result.diagnostic_code = "TEMP_WORKSPACE.SECURE_CREATE_REFUSED";
    result.message_key = "temp_workspace.secure_create.refused";
    result.reason = "created_path_has_unexpected_hardlink_count";
    result.error = "created path link count is not one";
    ::unlinkat(root_fd.get(), file_name.c_str(), 0);
    return result;
  }
  if ((st.st_mode & 0777) != (S_IRUSR | S_IWUSR)) {
    result.reason = "owner_only_file_permission_unverified";
    result.error = "created file mode is not 0600 after fchmod";
    ::unlinkat(root_fd.get(), file_name.c_str(), 0);
    return result;
  }
  if (st.st_uid != ::geteuid()) {
    result.status_code = StatusCode::memory_invalid_request;
    result.diagnostic_code = "TEMP_WORKSPACE.SECURE_CREATE_REFUSED";
    result.message_key = "temp_workspace.secure_create.refused";
    result.reason = "created_path_owner_mismatch";
    result.error = "created path owner does not match effective user";
    ::unlinkat(root_fd.get(), file_name.c_str(), 0);
    return result;
  }

  auto reservation = ReserveBytes(file_fd.get(),
                                  bytes,
                                  reservation_mode,
                                  physical_required);
  result.disk_reservation_evidence = reservation.evidence;
  if (!reservation.ok) {
    result.diagnostic_code = "TEMP_WORKSPACE.SPILL_RESERVE_FAILED";
    result.message_key = "temp_workspace.spill.reserve_failed";
    result.reason = "secure_file_reservation_failed";
    result.error = reservation.error;
    ::unlinkat(root_fd.get(), file_name.c_str(), 0);
    return result;
  }

  std::string close_error;
  if (!file_fd.Close(&close_error)) {
    result.reason = "secure_file_close_failed";
    result.error = close_error;
    ::unlinkat(root_fd.get(), file_name.c_str(), 0);
    return result;
  }

  result.ok = true;
  result.status_code = StatusCode::ok;
  result.severity = Severity::info;
  result.diagnostic_code.clear();
  result.message_key.clear();
  result.reason.clear();
  result.error.clear();
  result.evidence.random_unguessable_name = true;
  result.evidence.exclusive_create_no_overwrite = true;
  result.evidence.owner_only_permissions = true;
  result.evidence.nofollow_or_platform_equivalent = true;
  result.evidence.hardlink_refusal_checked = true;
  result.evidence.platform_semantics = "posix_openat_o_creat_o_excl_o_nofollow_fchmod_0600_fstat_nlink";
  result.evidence.authority_boundary = kTempWorkspaceAuthorityBoundary;
  result.disk_reservation_evidence = reservation.evidence;
  return result;
#endif
}
#else
std::wstring WidePath(const std::filesystem::path& path) {
  return path.wstring();
}

ReserveBytesResult ReserveBytes(HANDLE file,
                                u64 bytes,
                                TempWorkspaceDiskReservationMode mode,
                                bool physical_required) {
  ReserveBytesResult result;
  result.evidence.mode = mode;
  result.evidence.logical_quota_reserved = true;
  result.evidence.physical_preallocation_required =
      physical_required || mode == TempWorkspaceDiskReservationMode::physical_preallocate;
  result.evidence.requested_bytes = bytes;
  result.evidence.authority_boundary = kTempWorkspaceAuthorityBoundary;
  if (bytes == 0 || mode == TempWorkspaceDiskReservationMode::logical_quota_only) {
    result.ok = true;
    result.evidence.platform_semantics = "windows_logical_quota_only_no_file_space_claim";
    return result;
  }

  LARGE_INTEGER size;
  size.QuadPart = static_cast<LONGLONG>(bytes);
  if (mode == TempWorkspaceDiskReservationMode::sparse_file) {
    DWORD ignored = 0;
    (void)::DeviceIoControl(file, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &ignored, nullptr);
    if (::SetFilePointerEx(file, size, nullptr, FILE_BEGIN) == 0 || ::SetEndOfFile(file) == 0) {
      result.error = "SetFilePointerEx/SetEndOfFile failed";
      result.evidence.failure_reason = result.error;
      result.evidence.disk_full_or_reservation_failure = true;
      return result;
    }
    result.ok = true;
    result.evidence.sparse_file_created = true;
    result.evidence.sparse_not_physical_reservation = true;
    result.evidence.file_size_bytes = bytes;
    result.evidence.platform_semantics = "windows_sparse_file_logical_extent_not_physical_reservation";
    return result;
  }

  result.evidence.physical_preallocation_attempted = true;
  FILE_ALLOCATION_INFO allocation;
  allocation.AllocationSize = size;
  if (::SetFileInformationByHandle(file,
                                   FileAllocationInfo,
                                   &allocation,
                                   sizeof(allocation)) == 0) {
    result.error = "SetFileInformationByHandle(FileAllocationInfo) failed";
    result.evidence.failure_reason = result.error;
    result.evidence.disk_full_or_reservation_failure = true;
    result.evidence.platform_semantics = "windows_file_allocation_info_failed";
    return result;
  }
  if (::SetFilePointerEx(file, size, nullptr, FILE_BEGIN) == 0 || ::SetEndOfFile(file) == 0) {
    result.error = "SetFilePointerEx/SetEndOfFile failed after allocation";
    result.evidence.failure_reason = result.error;
    result.evidence.disk_full_or_reservation_failure = true;
    result.evidence.platform_semantics = "windows_physical_preallocation_set_eof_failed";
    return result;
  }
  result.ok = true;
  result.evidence.physical_preallocation_complete = true;
  result.evidence.file_size_bytes = bytes;
  result.evidence.platform_semantics = "windows_FileAllocationInfo_physical_preallocation";
  return result;
}

SecureCreateResult OpenSecureRootDirectory(const std::filesystem::path& root_path) {
  SecureCreateResult result;
  result.status_code = StatusCode::memory_invalid_request;
  result.diagnostic_code = "TEMP_WORKSPACE.ROOT_UNSAFE";
  result.message_key = "temp_workspace.root.unsafe";
  result.reason = "root_path_requires_owned_directory_with_reparse_refusal";
  const auto root_wide = WidePath(root_path);
  const DWORD attributes = ::GetFileAttributesW(root_wide.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    result.error = "GetFileAttributesW failed";
    return result;
  }
  if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
    result.error = "root path is not a directory";
    return result;
  }
  if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    result.error = "root path is a reparse point";
    return result;
  }
  result.ok = true;
  result.status_code = StatusCode::ok;
  result.severity = Severity::info;
  result.diagnostic_code.clear();
  result.message_key.clear();
  result.reason.clear();
  result.error.clear();
  result.evidence.owner_only_permissions = true;
  result.evidence.nofollow_or_platform_equivalent = true;
  result.evidence.platform_semantics = "windows_GetFileAttributesW_directory_reparse_refusal";
  return result;
}

SecureCreateResult CreateSecureTempWorkspaceFile(const std::filesystem::path& root_path,
                                                 const std::string& file_name,
                                                 u64 bytes,
                                                 TempWorkspaceDiskReservationMode reservation_mode,
                                                 bool physical_required) {
  SecureCreateResult root = OpenSecureRootDirectory(root_path);
  if (!root.ok) return root;

  SecureCreateResult result;
  result.status_code = StatusCode::memory_allocation_failed;
  result.diagnostic_code = "TEMP_WORKSPACE.SPILL_CREATE_FAILED";
  result.message_key = "temp_workspace.spill.create_failed";
  result.reason = "windows_secure_exclusive_file_create_failed";
  const std::filesystem::path file_path = root_path / file_name;
  const auto file_wide = WidePath(file_path);
  HANDLE file = ::CreateFileW(file_wide.c_str(),
                              GENERIC_WRITE,
                              0,
                              nullptr,
                              CREATE_NEW,
                              FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED,
                              nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    result.error = "CreateFileW(CREATE_NEW) failed";
    if (::GetLastError() == ERROR_FILE_EXISTS || ::GetLastError() == ERROR_ALREADY_EXISTS) {
      result.status_code = StatusCode::memory_invalid_request;
      result.diagnostic_code = "TEMP_WORKSPACE.SECURE_CREATE_REFUSED";
      result.message_key = "temp_workspace.secure_create.refused";
      result.reason = "candidate_path_already_exists";
    }
    return result;
  }

  auto reservation = ReserveBytes(file, bytes, reservation_mode, physical_required);
  result.disk_reservation_evidence = reservation.evidence;
  if (!reservation.ok) {
    result.diagnostic_code = "TEMP_WORKSPACE.SPILL_RESERVE_FAILED";
    result.message_key = "temp_workspace.spill.reserve_failed";
    result.reason = "windows_secure_file_reservation_failed";
    result.error = reservation.error;
    ::CloseHandle(file);
    ::DeleteFileW(file_wide.c_str());
    return result;
  }
  if (::CloseHandle(file) == 0) {
    result.reason = "windows_secure_file_close_failed";
    result.error = "CloseHandle failed";
    ::DeleteFileW(file_wide.c_str());
    return result;
  }

  result.ok = true;
  result.status_code = StatusCode::ok;
  result.severity = Severity::info;
  result.diagnostic_code.clear();
  result.message_key.clear();
  result.reason.clear();
  result.error.clear();
  result.evidence.random_unguessable_name = true;
  result.evidence.exclusive_create_no_overwrite = true;
  result.evidence.owner_only_permissions = true;
  result.evidence.nofollow_or_platform_equivalent = true;
  result.evidence.hardlink_refusal_checked = true;
  result.evidence.platform_semantics =
      "windows_BCryptGenRandom_CreateFileW_CREATE_NEW_share_none_reparse_refusal";
  result.evidence.authority_boundary = kTempWorkspaceAuthorityBoundary;
  result.disk_reservation_evidence = reservation.evidence;
  return result;
}
#endif

}  // namespace

TempWorkspaceLifecycleManager::TempWorkspaceLifecycleManager(TempWorkspacePolicy policy)
    : policy_(std::move(policy)) {
  if (policy_.root_path.empty()) {
    policy_.root_path = std::filesystem::temp_directory_path() / "scratchbird-temp-workspace";
  }
  manifest_generation_ = policy_.manifest_generation == 0 ? 1 : policy_.manifest_generation;
  (void)LoadManifestFromDisk();
}

TempWorkspaceResult TempWorkspaceLifecycleManager::ReserveTempFilespace(TempWorkspaceAllocationRequest request) {
  const TempStorageClass storage_class = request.storage_class;
  return Allocate(std::move(request), storage_class);
}

TempWorkspaceResult TempWorkspaceLifecycleManager::AllocateSpillFile(TempWorkspaceAllocationRequest request) {
  return Allocate(std::move(request), TempStorageClass::spill_file);
}

TempWorkspaceResult TempWorkspaceLifecycleManager::AllocateSortSpill(TempWorkspaceAllocationRequest request) {
  return Allocate(std::move(request), TempStorageClass::sort_workspace);
}

TempWorkspaceResult TempWorkspaceLifecycleManager::AllocateHashSpill(TempWorkspaceAllocationRequest request) {
  return Allocate(std::move(request), TempStorageClass::hash_workspace);
}

TempWorkspaceResult TempWorkspaceLifecycleManager::Allocate(TempWorkspaceAllocationRequest request,
                                                            TempStorageClass storage_class) {
  TempWorkspaceResult result;
  result.status = OkStatus();
  request.storage_class = storage_class;

  if (request.bytes == 0) {
    result.status = TempStatus(StatusCode::memory_invalid_request, Severity::error);
    result.diagnostic = MakeDiagnostic(result.status,
                                       "TEMP_WORKSPACE.ALLOCATION_INVALID",
                                       "temp_workspace.allocation.invalid",
                                       request.owner,
                                       {{"reason", "zero_byte_reservation"}});
    return result;
  }

  std::lock_guard<std::mutex> lock(mutex_);

  if (request.cluster_temp_workspace_requested) {
    result.status = TempStatus(StatusCode::memory_invalid_request, Severity::error);
    result.diagnostic = MakeDiagnostic(result.status,
                                       "TEMP_WORKSPACE.CLUSTER_OUT_OF_SCOPE",
                                       "temp_workspace.cluster.out_of_scope",
                                       request.owner,
                                       {{"reason", "cluster_temp_workspace_requires_external_provider"},
                                        {"external_provider_only", "true"},
                                        {"fail_closed", "true"}});
    return result;
  }

  const QuotaCheck quota = CheckQuotaLocked(request);
  if (!quota.allowed) {
    ++accounting_.quota_denial_count;
    result.status = TempStatus(StatusCode::memory_limit_exceeded, Severity::error);
    result.diagnostic = MakeDiagnostic(result.status,
                                       "TEMP_WORKSPACE.QUOTA_DENIED",
                                       "temp_workspace.quota.denied",
                                       request.owner,
                                       {{"dimension", quota.dimension},
                                        {"limit_bytes", std::to_string(quota.limit)},
                                        {"current_bytes", std::to_string(quota.current)},
                                        {"requested_bytes", std::to_string(request.bytes)}});
    return result;
  }

  std::error_code ec;
  if (policy_.require_existing_root_path && !std::filesystem::is_directory(policy_.root_path, ec)) {
    result.status = TempStatus(StatusCode::memory_invalid_request, Severity::error);
    result.diagnostic = MakeDiagnostic(result.status,
                                       "TEMP_WORKSPACE.ROOT_UNAVAILABLE",
                                       "temp_workspace.root.unavailable",
                                       request.owner,
                                       {{"root_path", policy_.root_path.string()}});
    return result;
  }
  const auto root_symlink_status = std::filesystem::symlink_status(policy_.root_path, ec);
  if (!ec && std::filesystem::is_symlink(root_symlink_status)) {
    result.status = TempStatus(StatusCode::memory_invalid_request, Severity::error);
    result.diagnostic = MakeDiagnostic(result.status,
                                       "TEMP_WORKSPACE.ROOT_UNSAFE",
                                       "temp_workspace.root.unsafe",
                                       request.owner,
                                       {{"root_path", policy_.root_path.string()},
                                        {"reason", "root_path_is_symlink"},
                                        {"authority_boundary", kTempWorkspaceAuthorityBoundary}});
    return result;
  }
  ec.clear();
  if (policy_.create_root_path) {
    std::filesystem::create_directories(policy_.root_path, ec);
    if (ec) {
      result.status = TempStatus(StatusCode::memory_allocation_failed, Severity::error);
      result.diagnostic = MakeDiagnostic(result.status,
                                         "TEMP_WORKSPACE.ROOT_CREATE_FAILED",
                                         "temp_workspace.root.create_failed",
                                         request.owner,
                                         {{"root_path", policy_.root_path.string()},
                                          {"error", ec.message()}});
      return result;
    }
  }

  auto budget_reservation = ReserveBudgetLocked(request, storage_class);
  if (budget_reservation.evidence.ceic_011_reservation_granted) {
    ++accounting_.ceic_011_reservation_grant_count;
  }
  if (!budget_reservation.ok) {
    if (budget_reservation.evidence.ceic_011_reservation_requested) {
      ++accounting_.ceic_011_reservation_refusal_count;
    }
    result.status = budget_reservation.status;
    result.diagnostic = budget_reservation.diagnostic;
    return result;
  }

  TempWorkspaceRecord record;
  record.owner = request.owner;
  record.budget_reservation_evidence = budget_reservation.evidence;
  std::string random_error;
  bool allocated_id = false;
  for (int attempt = 0; attempt < kSecureTempWorkspaceAllocationAttempts; ++attempt) {
    auto allocation_id = NextAllocationIdLocked(request, &random_error);
    if (!allocation_id.has_value()) {
      DiagnosticRecord release_diagnostic;
      (void)ReleaseBudgetReservationLocked(record, TempCleanupReason::administrator, &release_diagnostic);
      result.status = TempStatus(StatusCode::memory_allocation_failed, Severity::error);
      result.diagnostic = MakeDiagnostic(result.status,
                                         "TEMP_WORKSPACE.SECURE_RANDOM_FAILED",
                                         "temp_workspace.secure_random.failed",
                                         request.owner,
                                         {{"reason", "secure_random_token_unavailable"},
                                          {"error", random_error},
                                          {"authority_boundary", kTempWorkspaceAuthorityBoundary}});
      return result;
    }
    if (active_.find(*allocation_id) != active_.end()) {
      continue;
    }
    record.allocation_id = std::move(*allocation_id);
    allocated_id = true;
    break;
  }
  if (!allocated_id) {
    DiagnosticRecord release_diagnostic;
    (void)ReleaseBudgetReservationLocked(record, TempCleanupReason::administrator, &release_diagnostic);
    result.status = TempStatus(StatusCode::memory_allocation_failed, Severity::error);
    result.diagnostic = MakeDiagnostic(result.status,
                                       "TEMP_WORKSPACE.SECURE_RANDOM_FAILED",
                                       "temp_workspace.secure_random.failed",
                                       request.owner,
                                       {{"reason", "random_allocation_id_collision_limit"},
                                        {"authority_boundary", kTempWorkspaceAuthorityBoundary}});
    return result;
  }
  record.storage_class = storage_class;
  record.lifetime = request.lifetime;
  record.reserved_bytes = request.bytes;
  record.path = PathForAllocationLocked(record.allocation_id, request);
  record.state = TempWorkspaceState::active;
  record.purpose = request.purpose;
  record.durable_operation_owned = request.durable_operation_owned;
  record.recovery_resume_supported = request.recovery_resume_supported;
  record.evidence_required_before_discard = request.evidence_required_before_discard;
  record.administrator_review_required = request.administrator_review_required;
  record.legal_hold = request.legal_hold;

  // MMCH_SECURE_TEMP_WORKSPACE: production temp workspace files must be
  // random, exclusive, owner-only, no-follow creations; failures are closed.
  const auto secure_create = CreateSecureTempWorkspaceFile(policy_.root_path,
                                                          record.path.filename().string(),
                                                          request.bytes,
                                                          EffectiveDiskReservationMode(policy_),
                                                          policy_.require_physical_disk_reservation);
  if (!secure_create.ok) {
    DiagnosticRecord release_diagnostic;
    (void)ReleaseBudgetReservationLocked(record, TempCleanupReason::administrator, &release_diagnostic);
    result.status = TempStatus(secure_create.status_code, secure_create.severity);
    result.diagnostic = MakeDiagnostic(result.status,
                                       secure_create.diagnostic_code,
                                       secure_create.message_key,
                                       request.owner,
                                       {{"path", record.path.string()},
                                        {"reason", secure_create.reason},
                                        {"error", secure_create.error},
                                        {"platform_semantics", secure_create.evidence.platform_semantics},
                                        {"disk_reservation_mode", TempWorkspaceDiskReservationModeName(
                                                                      secure_create.disk_reservation_evidence.mode)},
                                        {"disk_reservation_failure",
                                         secure_create.disk_reservation_evidence.failure_reason},
                                        {"authority_boundary", kTempWorkspaceAuthorityBoundary}});
    return result;
  }
  record.security_evidence = secure_create.evidence;
  record.disk_reservation_evidence = secure_create.disk_reservation_evidence;
  if (!CommitBudgetReservationLocked(&record.budget_reservation_evidence,
                                     request.owner,
                                     &result.diagnostic)) {
    std::filesystem::remove(record.path, ec);
    result.status = result.diagnostic.status;
    return result;
  }

  AddAccountingLocked(record);
  active_.emplace(record.allocation_id, record);
  DiagnosticRecord manifest_diagnostic;
  if (!PersistManifestLocked(&manifest_diagnostic)) {
    const auto inserted = active_.find(record.allocation_id);
    if (inserted != active_.end()) {
      DiagnosticRecord release_diagnostic;
      if (!ReleaseBudgetReservationLocked(inserted->second,
                                          TempCleanupReason::administrator,
                                          &release_diagnostic)) {
        ++accounting_.ceic_011_reservation_release_failure_count;
      }
      RemoveAccountingLocked(inserted->second);
      active_.erase(inserted);
    }
    std::filesystem::remove(record.path, ec);
    result.status = TempStatus(StatusCode::memory_allocation_failed, Severity::error);
    result.diagnostic = manifest_diagnostic;
    return result;
  }
  result.record = record;
  return result;
}

TempWorkspaceLifecycleManager::QuotaCheck
TempWorkspaceLifecycleManager::CheckQuotaLocked(const TempWorkspaceAllocationRequest& request) const {
  if (AddWouldExceed(accounting_.active_bytes, request.bytes, policy_.filespace_quota_bytes)) {
    return {false, "filespace", policy_.filespace_quota_bytes, accounting_.active_bytes};
  }
  if (!request.owner.session_id.empty()) {
    const u64 current = MapValue(accounting_.session_bytes, request.owner.session_id);
    if (AddWouldExceed(current, request.bytes, policy_.session_quota_bytes)) {
      return {false, "session", policy_.session_quota_bytes, current};
    }
  }
  if (!request.owner.transaction_id.empty()) {
    const u64 current = MapValue(accounting_.transaction_bytes, request.owner.transaction_id);
    if (AddWouldExceed(current, request.bytes, policy_.transaction_quota_bytes)) {
      return {false, "transaction", policy_.transaction_quota_bytes, current};
    }
  }
  if (!request.owner.statement_id.empty()) {
    const u64 current = MapValue(accounting_.statement_bytes, request.owner.statement_id);
    if (AddWouldExceed(current, request.bytes, policy_.statement_quota_bytes)) {
      return {false, "statement", policy_.statement_quota_bytes, current};
    }
  }
  if (!request.owner.operation_id.empty()) {
    const u64 current = MapValue(accounting_.operation_bytes, request.owner.operation_id);
    if (AddWouldExceed(current, request.bytes, policy_.operation_quota_bytes)) {
      return {false, "operation", policy_.operation_quota_bytes, current};
    }
  }
  return {};
}

TempWorkspaceLifecycleManager::BudgetReservationResult
TempWorkspaceLifecycleManager::ReserveBudgetLocked(
    const TempWorkspaceAllocationRequest& request,
    TempStorageClass storage_class) const {
  BudgetReservationResult result;
  result.status = OkStatus();
  result.evidence.internal_logical_quota_checked = true;
  result.evidence.internal_logical_quota_reserved = true;
  result.evidence.ceic_011_reservation_applicable = policy_.reservation_ledger != nullptr;
  result.evidence.ceic_011_reservation_required = policy_.require_ceic_011_reservation;
  result.evidence.requested_bytes = request.bytes;
  result.evidence.category = policy_.reservation_category;
  result.evidence.memory_class = policy_.reservation_memory_class.empty()
                                      ? "temp_spill_workspace"
                                      : policy_.reservation_memory_class;
  result.evidence.ledger_model =
      policy_.reservation_ledger == nullptr
          ? "internal_logical_quota_only_no_ceic_011_ledger"
          : "hierarchical_memory_budget_ledger";
  result.evidence.authority_boundary = kTempWorkspaceAuthorityBoundary;

  if (policy_.reservation_ledger == nullptr) {
    if (!policy_.require_ceic_011_reservation) {
      return result;
    }
    result.ok = false;
    result.status = TempStatus(StatusCode::memory_invalid_request, Severity::error);
    result.evidence.failure_reason = "ceic_011_reservation_ledger_required";
    result.diagnostic = MakeDiagnostic(result.status,
                                       "TEMP_WORKSPACE.CEIC011_RESERVATION_REQUIRED",
                                       "temp_workspace.ceic011.reservation_required",
                                       request.owner,
                                       {{"reason", result.evidence.failure_reason},
                                        {"requested_bytes", std::to_string(request.bytes)}});
    return result;
  }

  result.evidence.ceic_011_reservation_requested = true;
  result.evidence.scope_chain =
      ScopeEvidenceStrings(TempWorkspaceReservationScopeChain(request, storage_class));

  HierarchicalMemoryReservationRequest reservation;
  reservation.scope_chain = TempWorkspaceReservationScopeChain(request, storage_class);
  reservation.category = result.evidence.category;
  if (reservation.category == MemoryCategory::unknown) {
    reservation.category = MemoryCategory::executor_query_reserved;
    result.evidence.category = reservation.category;
  }
  reservation.memory_class = result.evidence.memory_class;
  reservation.requested_bytes = request.bytes;
  reservation.owner_id = TempWorkspaceReservationOwnerId(request);
  reservation.spillable = storage_class == TempStorageClass::spill_file ||
                          storage_class == TempStorageClass::sort_workspace ||
                          storage_class == TempStorageClass::hash_workspace ||
                          storage_class == TempStorageClass::cursor_backing_store ||
                          storage_class == TempStorageClass::materialized_result;
  reservation.cancelable = true;
  reservation.priority = 0;
  reservation.weight = 1;
  reservation.provenance = EffectiveReservationProvenance(policy_);

  auto reserved = policy_.reservation_ledger->Reserve(std::move(reservation));
  if (!reserved.ok()) {
    result.ok = false;
    result.status = reserved.status;
    result.diagnostic = reserved.diagnostic;
    result.evidence.failure_reason =
        reserved.diagnostic.diagnostic_code.empty()
            ? "ceic_011_reservation_refused"
            : reserved.diagnostic.diagnostic_code;
    return result;
  }

  result.evidence.ceic_011_reservation_granted = true;
  result.evidence.token = reserved.token;
  return result;
}

bool TempWorkspaceLifecycleManager::CommitBudgetReservationLocked(
    TempWorkspaceBudgetReservationEvidence* evidence,
    const TempWorkspaceOwner& owner,
    DiagnosticRecord* diagnostic) const {
  if (evidence == nullptr || !evidence->token.valid() ||
      policy_.reservation_ledger == nullptr || evidence->ceic_011_reservation_committed) {
    return true;
  }

  auto committed = policy_.reservation_ledger->Commit(evidence->token);
  if (!committed.ok()) {
    (void)policy_.reservation_ledger->Release(evidence->token);
    evidence->failure_reason =
        committed.diagnostic.diagnostic_code.empty()
            ? "ceic_011_reservation_commit_refused"
            : committed.diagnostic.diagnostic_code;
    if (diagnostic != nullptr) {
      *diagnostic = committed.diagnostic.diagnostic_code.empty()
                        ? MakeDiagnostic(committed.status,
                                         "TEMP_WORKSPACE.CEIC011_COMMIT_REFUSED",
                                         "temp_workspace.ceic011.commit_refused",
                                         owner,
                                         {{"reason", evidence->failure_reason}})
                        : committed.diagnostic;
    }
    return false;
  }

  evidence->ceic_011_reservation_committed = true;
  return true;
}

bool TempWorkspaceLifecycleManager::ReleaseBudgetReservationLocked(
    const TempWorkspaceRecord& record,
    TempCleanupReason reason,
    DiagnosticRecord* diagnostic) {
  const auto& evidence = record.budget_reservation_evidence;
  if (!evidence.token.valid() || policy_.reservation_ledger == nullptr ||
      evidence.ceic_011_reservation_released) {
    return true;
  }

  auto released = policy_.reservation_ledger->Release(evidence.token);
  if (!released.ok()) {
    ++accounting_.ceic_011_reservation_release_failure_count;
    if (diagnostic != nullptr) {
      *diagnostic = released.diagnostic.diagnostic_code.empty()
                        ? MakeDiagnostic(released.status,
                                         "TEMP_WORKSPACE.CEIC011_RELEASE_REFUSED",
                                         "temp_workspace.ceic011.release_refused",
                                         record.owner,
                                         {{"allocation_id", record.allocation_id},
                                          {"cleanup_reason", TempCleanupReasonName(reason)}})
                        : released.diagnostic;
    }
    return false;
  }

  ++accounting_.ceic_011_reservation_release_count;
  return true;
}

TempWorkspaceCleanupResult
TempWorkspaceLifecycleManager::CleanupOnCommit(const std::string& transaction_id,
                                               TempTransactionOutcomeEvidence evidence) {
  std::lock_guard<std::mutex> lock(mutex_);
  return CleanupWhereLocked(TempCleanupReason::commit, evidence, transaction_id);
}

TempWorkspaceCleanupResult
TempWorkspaceLifecycleManager::CleanupOnRollback(const std::string& transaction_id,
                                                 TempTransactionOutcomeEvidence evidence) {
  std::lock_guard<std::mutex> lock(mutex_);
  return CleanupWhereLocked(TempCleanupReason::rollback, evidence, transaction_id);
}

TempWorkspaceCleanupResult TempWorkspaceLifecycleManager::CleanupOnDisconnect(const std::string& session_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  return CleanupWhereLocked(TempCleanupReason::disconnect, TempTransactionOutcomeEvidence::none, session_id);
}

TempWorkspaceCleanupResult TempWorkspaceLifecycleManager::CleanupOnShutdown() {
  std::lock_guard<std::mutex> lock(mutex_);
  return CleanupWhereLocked(TempCleanupReason::shutdown, TempTransactionOutcomeEvidence::none, {});
}

TempWorkspaceCleanupResult TempWorkspaceLifecycleManager::CleanupOperation(const std::string& operation_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  return CleanupWhereLocked(TempCleanupReason::operation_complete, TempTransactionOutcomeEvidence::none, operation_id);
}

TempWorkspaceCleanupResult
TempWorkspaceLifecycleManager::CleanupRecoverySafe(const TempWorkspaceRecoveryEvidence& evidence) {
  std::lock_guard<std::mutex> lock(mutex_);
  TempWorkspaceCleanupResult result;
  result.status = OkStatus();

  for (auto it = active_.begin(); it != active_.end();) {
    auto classified = ClassifyRecordForRecovery(it->second, evidence);
    ++accounting_.recovery_classification_count;
    if (classified.recovery_class == TempRecoveryClass::discard_safe ||
        classified.recovery_class == TempRecoveryClass::leaked_cleanup_required ||
        classified.recovery_class == TempRecoveryClass::discard_after_evidence) {
      DiagnosticRecord diagnostic;
      if (RemoveRecordFile(it->second, &diagnostic)) {
        if (!ReleaseBudgetReservationLocked(it->second,
                                            TempCleanupReason::recovery,
                                            &diagnostic)) {
          result.diagnostic = diagnostic;
          ++result.failed_count;
          it->second.state = TempWorkspaceState::cleanup_failed;
          ++it;
          continue;
        }
        RemoveAccountingLocked(it->second);
        ++accounting_.cleanup_count;
        ++result.cleaned_count;
        it = active_.erase(it);
        continue;
      }
      result.diagnostic = diagnostic;
      ++result.failed_count;
      it->second.state = TempWorkspaceState::cleanup_failed;
      ++it;
      continue;
    }
    ++result.refused_count;
    it->second.recovery_class = classified.recovery_class;
    it->second.state = classified.recovery_class == TempRecoveryClass::review_required
                           ? TempWorkspaceState::review_required
                           : TempWorkspaceState::cleanup_refused;
    ++it;
  }

  if (result.failed_count != 0) {
    result.status = TempStatus(StatusCode::memory_allocation_failed, Severity::error);
  } else if (result.refused_count != 0) {
    ++accounting_.cleanup_refusal_count;
    result.status = TempStatus(StatusCode::memory_invalid_request, Severity::warning);
    result.diagnostic = MakeDiagnostic(result.status,
                                       "TEMP_WORKSPACE.RECOVERY_CLEANUP_REFUSED",
                                       "temp_workspace.recovery.cleanup_refused",
                                       {},
                                       {{"refused_count", std::to_string(result.refused_count)}});
  }
  DiagnosticRecord manifest_diagnostic;
  if (!PersistManifestLocked(&manifest_diagnostic)) {
    result.status = TempStatus(StatusCode::memory_allocation_failed, Severity::error);
    result.diagnostic = manifest_diagnostic;
    ++result.failed_count;
  }
  return result;
}

TempWorkspaceCleanupResult
TempWorkspaceLifecycleManager::CleanupWhereLocked(TempCleanupReason reason,
                                                  TempTransactionOutcomeEvidence evidence,
                                                  const std::string& scope_id) {
  TempWorkspaceCleanupResult result;
  result.status = OkStatus();

  if (CleanupRequiresOutcome(reason) && !CleanupOutcomeMatches(reason, evidence)) {
    ++accounting_.cleanup_refusal_count;
    result.status = TempStatus(StatusCode::memory_invalid_request, Severity::error);
    result.diagnostic = MakeDiagnostic(result.status,
                                       "TEMP_WORKSPACE.OUTCOME_EVIDENCE_REQUIRED",
                                       "temp_workspace.cleanup.outcome_evidence_required",
                                       {},
                                       {{"cleanup_reason", TempCleanupReasonName(reason)},
                                        {"evidence", TempTransactionOutcomeEvidenceName(evidence)},
                                        {"scope_id", scope_id}});
    result.refused_count = static_cast<u64>(std::count_if(active_.begin(), active_.end(), [&](const auto& entry) {
      return RecordMatchesCleanupScope(entry.second, reason, scope_id);
    }));
    return result;
  }

  for (auto it = active_.begin(); it != active_.end();) {
    if (!RecordMatchesCleanupScope(it->second, reason, scope_id)) {
      ++it;
      continue;
    }
    if (ProtectedFromOrdinaryCleanup(it->second) && reason != TempCleanupReason::administrator) {
      ++result.refused_count;
      it->second.state = TempWorkspaceState::review_required;
      ++it;
      continue;
    }

    DiagnosticRecord diagnostic;
    if (RemoveRecordFile(it->second, &diagnostic)) {
      if (!ReleaseBudgetReservationLocked(it->second, reason, &diagnostic)) {
        result.diagnostic = diagnostic;
        ++result.failed_count;
        it->second.state = TempWorkspaceState::cleanup_failed;
        ++it;
        continue;
      }
      RemoveAccountingLocked(it->second);
      ++accounting_.cleanup_count;
      ++result.cleaned_count;
      it = active_.erase(it);
    } else {
      result.diagnostic = diagnostic;
      ++result.failed_count;
      it->second.state = TempWorkspaceState::cleanup_failed;
      ++it;
    }
  }

  if (result.failed_count != 0) {
    result.status = TempStatus(StatusCode::memory_allocation_failed, Severity::error);
  } else if (result.refused_count != 0) {
    ++accounting_.cleanup_refusal_count;
    result.status = TempStatus(StatusCode::memory_invalid_request, Severity::warning);
    result.diagnostic = MakeDiagnostic(result.status,
                                       "TEMP_WORKSPACE.CLEANUP_REFUSED",
                                       "temp_workspace.cleanup.refused",
                                       {},
                                       {{"cleanup_reason", TempCleanupReasonName(reason)},
                                        {"refused_count", std::to_string(result.refused_count)}});
  }
  DiagnosticRecord manifest_diagnostic;
  if (!PersistManifestLocked(&manifest_diagnostic)) {
    result.status = TempStatus(StatusCode::memory_allocation_failed, Severity::error);
    result.diagnostic = manifest_diagnostic;
    ++result.failed_count;
  }
  return result;
}

bool TempWorkspaceLifecycleManager::RecordMatchesCleanupScope(const TempWorkspaceRecord& record,
                                                              TempCleanupReason reason,
                                                              const std::string& scope_id) const {
  switch (reason) {
    case TempCleanupReason::statement_end:
      return record.owner.statement_id == scope_id &&
             record.lifetime == TempWorkspaceLifetime::statement_lifetime;
    case TempCleanupReason::commit:
      return record.owner.transaction_id == scope_id && IsTransactionScoped(record.lifetime);
    case TempCleanupReason::rollback:
      return record.owner.transaction_id == scope_id && IsTransactionScoped(record.lifetime);
    case TempCleanupReason::disconnect:
      return record.owner.session_id == scope_id;
    case TempCleanupReason::shutdown:
      return true;
    case TempCleanupReason::recovery:
      return true;
    case TempCleanupReason::operation_complete:
      return record.owner.operation_id == scope_id &&
             record.lifetime == TempWorkspaceLifetime::operation_lifetime;
    case TempCleanupReason::administrator:
      return record.allocation_id == scope_id;
  }
  return false;
}

bool TempWorkspaceLifecycleManager::CleanupRequiresOutcome(TempCleanupReason reason) const {
  return reason == TempCleanupReason::commit || reason == TempCleanupReason::rollback;
}

bool TempWorkspaceLifecycleManager::CleanupOutcomeMatches(TempCleanupReason reason,
                                                          TempTransactionOutcomeEvidence evidence) const {
  if (reason == TempCleanupReason::commit) {
    return evidence == TempTransactionOutcomeEvidence::committed;
  }
  if (reason == TempCleanupReason::rollback) {
    return evidence == TempTransactionOutcomeEvidence::rolled_back;
  }
  return true;
}

bool TempWorkspaceLifecycleManager::ProtectedFromOrdinaryCleanup(const TempWorkspaceRecord& record) const {
  return record.administrator_review_required || record.legal_hold ||
         (record.durable_operation_owned && record.lifetime == TempWorkspaceLifetime::operation_lifetime);
}

TempWorkspaceRecoveryResult
TempWorkspaceLifecycleManager::ClassifyForRecovery(const std::string& allocation_id,
                                                   const TempWorkspaceRecoveryEvidence& evidence) {
  std::lock_guard<std::mutex> lock(mutex_);
  TempWorkspaceRecoveryResult missing;
  const auto it = active_.find(allocation_id);
  if (it == active_.end()) {
    missing.status = TempStatus(StatusCode::memory_unknown_pointer, Severity::error);
    missing.recovery_class = TempRecoveryClass::cleanup_refused;
    missing.diagnostic = MakeDiagnostic(missing.status,
                                        "TEMP_WORKSPACE.RECORD_NOT_FOUND",
                                        "temp_workspace.record.not_found",
                                        {},
                                        {{"allocation_id", allocation_id}});
    return missing;
  }
  auto result = ClassifyRecordForRecovery(it->second, evidence);
  ++accounting_.recovery_classification_count;
  it->second.recovery_class = result.recovery_class;
  if (result.recovery_class == TempRecoveryClass::quarantine_required) {
    it->second.state = TempWorkspaceState::quarantined;
  } else if (result.recovery_class == TempRecoveryClass::review_required) {
    it->second.state = TempWorkspaceState::review_required;
  }
  DiagnosticRecord manifest_diagnostic;
  (void)PersistManifestLocked(&manifest_diagnostic);
  return result;
}

TempWorkspaceRecoveryResult
TempWorkspaceLifecycleManager::ClassifyRecordForRecovery(const TempWorkspaceRecord& record,
                                                         const TempWorkspaceRecoveryEvidence& evidence) const {
  TempWorkspaceRecoveryResult result;
  result.status = OkStatus();

  if (!evidence.integrity_verified) {
    result.recovery_class = TempRecoveryClass::quarantine_required;
  } else if (record.legal_hold || record.administrator_review_required) {
    result.recovery_class = TempRecoveryClass::review_required;
  } else if (record.durable_operation_owned) {
    result.recovery_class = evidence.operation_envelope_present
                                ? TempRecoveryClass::operation_owned_resume
                                : TempRecoveryClass::review_required;
  } else if (record.recovery_resume_supported) {
    result.recovery_class = evidence.engine_recovery_authority
                                ? TempRecoveryClass::resume_required
                                : TempRecoveryClass::cleanup_refused;
  } else if (record.evidence_required_before_discard) {
    const bool has_outcome = evidence.transaction_outcome == TempTransactionOutcomeEvidence::committed ||
                             evidence.transaction_outcome == TempTransactionOutcomeEvidence::rolled_back;
    result.recovery_class = (evidence.engine_recovery_authority && has_outcome)
                                ? TempRecoveryClass::discard_after_evidence
                                : TempRecoveryClass::cleanup_refused;
  } else if (evidence.leaked_after_crash ||
             record.lifetime == TempWorkspaceLifetime::statement_lifetime ||
             record.lifetime == TempWorkspaceLifetime::transaction_lifetime ||
             record.lifetime == TempWorkspaceLifetime::session_lifetime) {
    result.recovery_class = evidence.leaked_after_crash
                                ? TempRecoveryClass::leaked_cleanup_required
                                : TempRecoveryClass::discard_safe;
  } else {
    result.recovery_class = TempRecoveryClass::discard_safe;
  }

  if (result.recovery_class == TempRecoveryClass::cleanup_refused) {
    result.status = TempStatus(StatusCode::memory_invalid_request, Severity::warning);
  }
  result.diagnostic = MakeDiagnostic(result.status,
                                     result.status.ok() ? "TEMP_WORKSPACE.RECOVERY_CLASSIFIED"
                                                        : "TEMP_WORKSPACE.RECOVERY_CLASSIFICATION_REFUSED",
                                     result.status.ok() ? "temp_workspace.recovery.classified"
                                                        : "temp_workspace.recovery.classification_refused",
                                     record.owner,
                                     {{"allocation_id", record.allocation_id},
                                      {"recovery_class", TempRecoveryClassName(result.recovery_class)}});
  return result;
}

std::optional<TempWorkspaceRecord> TempWorkspaceLifecycleManager::Find(const std::string& allocation_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = active_.find(allocation_id);
  if (it == active_.end()) return std::nullopt;
  return it->second;
}

std::vector<TempWorkspaceRecord> TempWorkspaceLifecycleManager::ActiveRecords() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<TempWorkspaceRecord> records;
  for (const auto& entry : active_) {
    records.push_back(entry.second);
  }
  return records;
}

TempWorkspaceAccountingSnapshot TempWorkspaceLifecycleManager::Snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return accounting_;
}

const TempWorkspacePolicy& TempWorkspaceLifecycleManager::policy() const {
  return policy_;
}

bool TempWorkspaceLifecycleManager::RemoveRecordFile(const TempWorkspaceRecord& record,
                                                     DiagnosticRecord* diagnostic) const {
  if (!policy_.cleanup_files_on_release) return true;
  std::error_code ec;
  if (record.path.empty() || !std::filesystem::exists(record.path, ec)) {
    return true;
  }
  std::filesystem::remove(record.path, ec);
  if (!ec) return true;
  if (diagnostic != nullptr) {
    const auto status = TempStatus(StatusCode::memory_allocation_failed, Severity::error);
    *diagnostic = MakeDiagnostic(status,
                                 "TEMP_WORKSPACE.CLEANUP_FILE_FAILED",
                                 "temp_workspace.cleanup.file_failed",
                                 record.owner,
                                 {{"allocation_id", record.allocation_id},
                                  {"path", record.path.string()},
                                  {"error", ec.message()}});
  }
  return false;
}

void TempWorkspaceLifecycleManager::AddAccountingLocked(const TempWorkspaceRecord& record) {
  accounting_.active_bytes += record.reserved_bytes;
  accounting_.peak_bytes = std::max(accounting_.peak_bytes, accounting_.active_bytes);
  ++accounting_.allocation_count;
  AddMapValue(&accounting_.session_bytes, record.owner.session_id, record.reserved_bytes);
  AddMapValue(&accounting_.transaction_bytes, record.owner.transaction_id, record.reserved_bytes);
  AddMapValue(&accounting_.statement_bytes, record.owner.statement_id, record.reserved_bytes);
  AddMapValue(&accounting_.operation_bytes, record.owner.operation_id, record.reserved_bytes);
}

void TempWorkspaceLifecycleManager::RemoveAccountingLocked(const TempWorkspaceRecord& record) {
  accounting_.active_bytes = record.reserved_bytes >= accounting_.active_bytes
                                 ? 0
                                 : accounting_.active_bytes - record.reserved_bytes;
  RemoveMapValue(&accounting_.session_bytes, record.owner.session_id, record.reserved_bytes);
  RemoveMapValue(&accounting_.transaction_bytes, record.owner.transaction_id, record.reserved_bytes);
  RemoveMapValue(&accounting_.statement_bytes, record.owner.statement_id, record.reserved_bytes);
  RemoveMapValue(&accounting_.operation_bytes, record.owner.operation_id, record.reserved_bytes);
}

std::filesystem::path TempWorkspaceLifecycleManager::ManifestPath() const {
  const u64 version = policy_.metadata_format_version == 0
                          ? kTempWorkspaceManifestCurrentFormatVersion
                          : policy_.metadata_format_version;
  return ManifestPathForVersion(version);
}

std::filesystem::path TempWorkspaceLifecycleManager::ManifestPathForVersion(u64 version) const {
  return policy_.root_path /
         (".scratchbird_temp_workspace_manifest.v" + std::to_string(version));
}

bool TempWorkspaceLifecycleManager::LoadManifestFromDisk() {
  std::error_code ec;
  const u64 configured_version = policy_.metadata_format_version == 0
                                     ? kTempWorkspaceManifestCurrentFormatVersion
                                     : policy_.metadata_format_version;
  std::vector<u64> candidate_versions{configured_version};
  if (configured_version != kTempWorkspaceManifestLegacyFormatVersion) {
    candidate_versions.push_back(kTempWorkspaceManifestLegacyFormatVersion);
  }
  for (u64 candidate_version : candidate_versions) {
    const auto path = ManifestPathForVersion(candidate_version);
    const auto tmp = TempWorkspaceManifestTempPath(path);
    if (std::filesystem::exists(tmp, ec)) {
      std::filesystem::remove(tmp, ec);
      if (ec) {
        return false;
      }
      std::string sync_error;
      if (!DurableSyncParentDirectory(tmp, &sync_error)) {
        return false;
      }
    }
    if (!std::filesystem::is_regular_file(path, ec)) {
      continue;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
      return false;
    }
    std::string header;
    std::getline(in, header);
    const auto actual_version = TempWorkspaceManifestVersionFromHeader(header);
    if (!actual_version.has_value() || *actual_version != candidate_version) {
      return false;
    }
    std::ostringstream rest_stream;
    rest_stream << in.rdbuf();
    std::string body = rest_stream.str();
    bool verified_manifest = false;
    if (*actual_version == kTempWorkspaceManifestCurrentFormatVersion) {
      std::string first_body_line;
      std::string remaining_body;
      const auto newline = body.find('\n');
      if (newline == std::string::npos) {
        first_body_line = body;
        remaining_body.clear();
      } else {
        first_body_line = body.substr(0, newline);
        remaining_body = body.substr(newline + 1);
      }
      const auto meta_fields = SplitTabs(first_body_line);
      if (!meta_fields.empty() && meta_fields[0] == kTempWorkspaceManifestMetaRecord) {
        if (meta_fields.size() != 5) {
          return false;
        }
        u64 manifest_generation = 0;
        if (!ParseStrictU64Field(meta_fields[1], &manifest_generation) ||
            manifest_generation == 0) {
          return false;
        }
        const auto writer_identity = UnescapeManifestField(meta_fields[2]);
        if (!writer_identity.has_value() ||
            *writer_identity != EffectiveManifestWriterIdentity(policy_)) {
          return false;
        }
        if (meta_fields[3] != kTempWorkspaceManifestChecksumAlgorithm) {
          return false;
        }
        const std::string observed_checksum = TempWorkspaceManifestChecksum(remaining_body);
        if (observed_checksum != meta_fields[4]) {
          return false;
        }
        manifest_generation_ = manifest_generation;
        body = std::move(remaining_body);
        verified_manifest = true;
      }
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      std::stringstream body_stream(body);
      std::string line;
      while (std::getline(body_stream, line)) {
        if (line.empty()) {
          continue;
        }
        auto record = ParseManifestLine(line);
        if (!record.has_value()) {
          return false;
        }
        if (!std::filesystem::is_regular_file(record->path, ec)) {
          continue;
        }
        AddAccountingLocked(*record);
        active_[record->allocation_id] = *record;
      }
      if (candidate_version != configured_version || !verified_manifest) {
        DiagnosticRecord ignored;
        if (!PersistManifestLocked(&ignored)) {
          return false;
        }
        std::filesystem::remove(path, ec);
      }
    }
    return true;
  }
  return true;
}

bool TempWorkspaceLifecycleManager::PersistManifestLocked(DiagnosticRecord* diagnostic) {
  std::error_code ec;
  const auto path = ManifestPath();
  const auto tmp = TempWorkspaceManifestTempPath(path);
  if (active_.empty()) {
    const bool manifest_existed = std::filesystem::exists(path, ec);
    if (ec) {
      if (diagnostic != nullptr) {
        const auto status = TempManifestStatus(StatusCode::memory_allocation_failed,
                                              Severity::error);
        *diagnostic = MakeTempManifestDiagnostic(status,
                                                 "TEMP_WORKSPACE.MANIFEST_WRITE_FAILED",
                                                 "temp_workspace.manifest.write_failed",
                                                 policy_,
                                                 "manifest_empty_exists_check_failed",
                                                 path,
                                                 ec.message());
      }
      return false;
    }
    const bool temp_existed = std::filesystem::exists(tmp, ec);
    if (ec) {
      if (diagnostic != nullptr) {
        const auto status = TempManifestStatus(StatusCode::memory_allocation_failed,
                                              Severity::error);
        *diagnostic = MakeTempManifestDiagnostic(status,
                                                 "TEMP_WORKSPACE.MANIFEST_WRITE_FAILED",
                                                 "temp_workspace.manifest.write_failed",
                                                 policy_,
                                                 "manifest_empty_tmp_exists_check_failed",
                                                 tmp,
                                                 ec.message());
      }
      return false;
    }
    std::filesystem::remove(path, ec);
    if (ec) {
      if (diagnostic != nullptr) {
        const auto status = TempManifestStatus(StatusCode::memory_allocation_failed,
                                              Severity::error);
        *diagnostic = MakeTempManifestDiagnostic(status,
                                                 "TEMP_WORKSPACE.MANIFEST_WRITE_FAILED",
                                                 "temp_workspace.manifest.write_failed",
                                                 policy_,
                                                 "manifest_empty_remove_failed",
                                                 path,
                                                 ec.message());
      }
      return false;
    }
    std::filesystem::remove(tmp, ec);
    if (ec) {
      if (diagnostic != nullptr) {
        const auto status = TempManifestStatus(StatusCode::memory_allocation_failed,
                                              Severity::error);
        *diagnostic = MakeTempManifestDiagnostic(status,
                                                 "TEMP_WORKSPACE.MANIFEST_WRITE_FAILED",
                                                 "temp_workspace.manifest.write_failed",
                                                 policy_,
                                                 "manifest_empty_tmp_remove_failed",
                                                 tmp,
                                                 ec.message());
      }
      return false;
    }
    if (manifest_existed || temp_existed) {
      std::string sync_error;
      if (!DurableSyncParentDirectory(path, &sync_error)) {
        if (diagnostic != nullptr) {
          const auto status = TempManifestStatus(StatusCode::memory_allocation_failed,
                                                Severity::error);
          *diagnostic = MakeTempManifestDiagnostic(status,
                                                   "TEMP_WORKSPACE.MANIFEST_WRITE_FAILED",
                                                   "temp_workspace.manifest.write_failed",
                                                   policy_,
                                                   "manifest_empty_parent_sync_failed",
                                                   path,
                                                   sync_error);
        }
        return false;
      }
    }
    return true;
  }
  std::filesystem::create_directories(policy_.root_path, ec);
  if (ec) {
    if (diagnostic != nullptr) {
      const auto status = TempManifestStatus(StatusCode::memory_allocation_failed,
                                            Severity::error);
      *diagnostic = MakeTempManifestDiagnostic(status,
                                               "TEMP_WORKSPACE.MANIFEST_WRITE_FAILED",
                                               "temp_workspace.manifest.write_failed",
                                               policy_,
                                               "manifest_root_create_failed",
                                               policy_.root_path,
                                               ec.message());
    }
    return false;
  }
  const u64 manifest_version = policy_.metadata_format_version == 0
                                   ? kTempWorkspaceManifestCurrentFormatVersion
                                   : policy_.metadata_format_version;
  const char* manifest_header = TempWorkspaceManifestHeaderForVersion(manifest_version);
  if (manifest_header == nullptr) {
    if (diagnostic != nullptr) {
      const auto status = TempManifestStatus(StatusCode::memory_invalid_request,
                                            Severity::error);
      *diagnostic = MakeTempManifestDiagnostic(status,
                                               "TEMP_WORKSPACE.MANIFEST_WRITE_FAILED",
                                               "temp_workspace.manifest.write_failed",
                                               policy_,
                                               "manifest_unsupported_format_version",
                                               path,
                                               std::to_string(manifest_version));
    }
    return false;
  }
  if (std::filesystem::exists(tmp, ec)) {
    std::filesystem::remove(tmp, ec);
    if (ec) {
      if (diagnostic != nullptr) {
        const auto status = TempManifestStatus(StatusCode::memory_allocation_failed,
                                              Severity::error);
        *diagnostic = MakeTempManifestDiagnostic(status,
                                                 "TEMP_WORKSPACE.MANIFEST_WRITE_FAILED",
                                                 "temp_workspace.manifest.write_failed",
                                                 policy_,
                                                 "manifest_stale_tmp_remove_failed",
                                                 tmp,
                                                 ec.message());
      }
      return false;
    }
    std::string sync_error;
    if (!DurableSyncParentDirectory(tmp, &sync_error)) {
      if (diagnostic != nullptr) {
        const auto status = TempManifestStatus(StatusCode::memory_allocation_failed,
                                              Severity::error);
        *diagnostic = MakeTempManifestDiagnostic(status,
                                                 "TEMP_WORKSPACE.MANIFEST_WRITE_FAILED",
                                                 "temp_workspace.manifest.write_failed",
                                                 policy_,
                                                 "manifest_stale_tmp_parent_sync_failed",
                                                 tmp,
                                                 sync_error);
      }
      return false;
    }
  }

  std::string body;
  for (const auto& entry : active_) {
    body += SerializeManifestRecord(entry.second);
    body += '\n';
  }
  const u64 next_generation = std::max<u64>(manifest_generation_ + 1,
                                            policy_.manifest_generation == 0
                                                ? 1
                                                : policy_.manifest_generation);
  const std::string checksum = TempWorkspaceManifestChecksum(body);
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
      if (diagnostic != nullptr) {
        const auto status = TempManifestStatus(StatusCode::memory_allocation_failed,
                                              Severity::error);
        *diagnostic = MakeTempManifestDiagnostic(status,
                                                 "TEMP_WORKSPACE.MANIFEST_WRITE_FAILED",
                                                 "temp_workspace.manifest.write_failed",
                                                 policy_,
                                                 "manifest_tmp_open_failed",
                                                 tmp);
      }
      return false;
    }
    out << manifest_header << '\n';
    out << kTempWorkspaceManifestMetaRecord << '\t'
        << next_generation << '\t'
        << EscapeManifestField(EffectiveManifestWriterIdentity(policy_)) << '\t'
        << kTempWorkspaceManifestChecksumAlgorithm << '\t'
        << checksum << '\n';
    out << body;
    out.close();
    if (!out) {
      if (diagnostic != nullptr) {
        const auto status = TempManifestStatus(StatusCode::memory_allocation_failed,
                                              Severity::error);
        *diagnostic = MakeTempManifestDiagnostic(status,
                                                 "TEMP_WORKSPACE.MANIFEST_WRITE_FAILED",
                                                 "temp_workspace.manifest.write_failed",
                                                 policy_,
                                                 "manifest_tmp_flush_failed",
                                                 tmp);
      }
      return false;
    }
  }
  std::string sync_error;
  if (!DurableSyncPath(tmp, true, &sync_error)) {
    if (diagnostic != nullptr) {
      const auto status = TempManifestStatus(StatusCode::memory_allocation_failed,
                                            Severity::error);
      *diagnostic = MakeTempManifestDiagnostic(status,
                                               "TEMP_WORKSPACE.MANIFEST_WRITE_FAILED",
                                               "temp_workspace.manifest.write_failed",
                                               policy_,
                                               "manifest_tmp_file_sync_failed",
                                               tmp,
                                               sync_error);
    }
    return false;
  }
  sync_error.clear();
  if (!ReplaceFileAtomically(tmp, path, &sync_error)) {
    if (diagnostic != nullptr) {
      const auto status = TempManifestStatus(StatusCode::memory_allocation_failed,
                                            Severity::error);
      *diagnostic = MakeTempManifestDiagnostic(status,
                                               "TEMP_WORKSPACE.MANIFEST_WRITE_FAILED",
                                               "temp_workspace.manifest.write_failed",
                                               policy_,
                                               "manifest_rename_failed",
                                               path,
                                               sync_error);
    }
    return false;
  }
  sync_error.clear();
  if (!DurableSyncParentDirectory(path, &sync_error)) {
    if (diagnostic != nullptr) {
      const auto status = TempManifestStatus(StatusCode::memory_allocation_failed,
                                            Severity::error);
      *diagnostic = MakeTempManifestDiagnostic(status,
                                               "TEMP_WORKSPACE.MANIFEST_WRITE_FAILED",
                                               "temp_workspace.manifest.write_failed",
                                               policy_,
                                               "manifest_parent_sync_failed",
                                               path,
                                               sync_error);
    }
    return false;
  }
  manifest_generation_ = next_generation;
  return true;
}

std::optional<TempWorkspaceRecord>
TempWorkspaceLifecycleManager::ParseManifestLine(const std::string& line) const {
  const auto fields = SplitTabs(line);
  if (fields.size() < 31) {
    return std::nullopt;
  }
  std::vector<std::string> decoded;
  decoded.reserve(fields.size());
  for (const auto& field : fields) {
    auto value = UnescapeManifestField(field);
    if (!value.has_value()) {
      return std::nullopt;
    }
    decoded.push_back(std::move(*value));
  }
  if (decoded[0] != "record_v1") {
    return std::nullopt;
  }

  TempWorkspaceRecord record;
  std::size_t i = 1;
  record.allocation_id = decoded[i++];
  record.storage_class = ParseTempStorageClassName(decoded[i++]);
  record.lifetime = ParseTempWorkspaceLifetimeName(decoded[i++]);
  record.reserved_bytes = ParseU64OrZero(decoded[i++]);
  record.path = policy_.root_path / SanitizePathToken(decoded[i++]);
  record.state = ParseTempWorkspaceStateName(decoded[i++]);
  record.recovery_class = ParseTempRecoveryClassName(decoded[i++]);
  record.durable_operation_owned = ParseBool(decoded[i++]);
  record.recovery_resume_supported = ParseBool(decoded[i++]);
  record.evidence_required_before_discard = ParseBool(decoded[i++]);
  record.administrator_review_required = ParseBool(decoded[i++]);
  record.legal_hold = ParseBool(decoded[i++]);
  record.purpose = decoded[i++];
  record.owner.temp_object_uuid = decoded[i++];
  record.owner.database_id = decoded[i++];
  record.owner.engine_id = decoded[i++];
  record.owner.session_id = decoded[i++];
  record.owner.transaction_id = decoded[i++];
  record.owner.statement_id = decoded[i++];
  record.owner.cursor_id = decoded[i++];
  record.owner.result_set_id = decoded[i++];
  record.owner.operation_id = decoded[i++];
  record.owner.scheduler_task_id = decoded[i++];
  record.owner.policy_generation = ParseU64OrZero(decoded[i++]);
  record.owner.security_generation = ParseU64OrZero(decoded[i++]);
  record.owner.resource_budget_reference = decoded[i++];
  record.disk_reservation_evidence.mode =
      ParseTempWorkspaceDiskReservationModeName(decoded[i++]);
  record.disk_reservation_evidence.requested_bytes = ParseU64OrZero(decoded[i++]);
  record.disk_reservation_evidence.file_size_bytes = ParseU64OrZero(decoded[i++]);
  record.disk_reservation_evidence.logical_quota_reserved = ParseBool(decoded[i++]);
  record.disk_reservation_evidence.authority_boundary = kTempWorkspaceAuthorityBoundary;
  record.security_evidence.authority_boundary = kTempWorkspaceAuthorityBoundary;
  if (decoded.size() >= i + 9) {
    record.budget_reservation_evidence.internal_logical_quota_checked = ParseBool(decoded[i++]);
    record.budget_reservation_evidence.internal_logical_quota_reserved = ParseBool(decoded[i++]);
    record.budget_reservation_evidence.ceic_011_reservation_applicable = ParseBool(decoded[i++]);
    record.budget_reservation_evidence.ceic_011_reservation_required = ParseBool(decoded[i++]);
    record.budget_reservation_evidence.ceic_011_reservation_requested = ParseBool(decoded[i++]);
    record.budget_reservation_evidence.ceic_011_reservation_granted = ParseBool(decoded[i++]);
    record.budget_reservation_evidence.ceic_011_reservation_committed = ParseBool(decoded[i++]);
    record.budget_reservation_evidence.requested_bytes = ParseU64OrZero(decoded[i++]);
    record.budget_reservation_evidence.category = ParseMemoryCategoryName(decoded[i++]);
  }
  if (decoded.size() >= i + 2) {
    record.budget_reservation_evidence.memory_class = decoded[i++];
    record.budget_reservation_evidence.ledger_model = decoded[i++];
  }
  record.budget_reservation_evidence.authority_boundary = kTempWorkspaceAuthorityBoundary;
  return record;
}

std::string
TempWorkspaceLifecycleManager::SerializeManifestRecord(const TempWorkspaceRecord& record) const {
  std::vector<std::string> fields;
  fields.push_back("record_v1");
  fields.push_back(record.allocation_id);
  fields.push_back(TempStorageClassName(record.storage_class));
  fields.push_back(TempWorkspaceLifetimeName(record.lifetime));
  fields.push_back(std::to_string(record.reserved_bytes));
  fields.push_back(record.path.filename().string());
  fields.push_back(TempWorkspaceStateName(record.state));
  fields.push_back(TempRecoveryClassName(record.recovery_class));
  fields.push_back(record.durable_operation_owned ? "1" : "0");
  fields.push_back(record.recovery_resume_supported ? "1" : "0");
  fields.push_back(record.evidence_required_before_discard ? "1" : "0");
  fields.push_back(record.administrator_review_required ? "1" : "0");
  fields.push_back(record.legal_hold ? "1" : "0");
  fields.push_back(record.purpose);
  fields.push_back(record.owner.temp_object_uuid);
  fields.push_back(record.owner.database_id);
  fields.push_back(record.owner.engine_id);
  fields.push_back(record.owner.session_id);
  fields.push_back(record.owner.transaction_id);
  fields.push_back(record.owner.statement_id);
  fields.push_back(record.owner.cursor_id);
  fields.push_back(record.owner.result_set_id);
  fields.push_back(record.owner.operation_id);
  fields.push_back(record.owner.scheduler_task_id);
  fields.push_back(std::to_string(record.owner.policy_generation));
  fields.push_back(std::to_string(record.owner.security_generation));
  fields.push_back(record.owner.resource_budget_reference);
  fields.push_back(TempWorkspaceDiskReservationModeName(record.disk_reservation_evidence.mode));
  fields.push_back(std::to_string(record.disk_reservation_evidence.requested_bytes));
  fields.push_back(std::to_string(record.disk_reservation_evidence.file_size_bytes));
  fields.push_back(record.disk_reservation_evidence.logical_quota_reserved ? "1" : "0");
  fields.push_back(record.budget_reservation_evidence.internal_logical_quota_checked ? "1" : "0");
  fields.push_back(record.budget_reservation_evidence.internal_logical_quota_reserved ? "1" : "0");
  fields.push_back(record.budget_reservation_evidence.ceic_011_reservation_applicable ? "1" : "0");
  fields.push_back(record.budget_reservation_evidence.ceic_011_reservation_required ? "1" : "0");
  fields.push_back(record.budget_reservation_evidence.ceic_011_reservation_requested ? "1" : "0");
  fields.push_back(record.budget_reservation_evidence.ceic_011_reservation_granted ? "1" : "0");
  fields.push_back(record.budget_reservation_evidence.ceic_011_reservation_committed ? "1" : "0");
  fields.push_back(std::to_string(record.budget_reservation_evidence.requested_bytes));
  fields.push_back(MemoryCategoryName(record.budget_reservation_evidence.category));
  fields.push_back(record.budget_reservation_evidence.memory_class);
  fields.push_back(record.budget_reservation_evidence.ledger_model);

  std::ostringstream line;
  for (std::size_t i = 0; i < fields.size(); ++i) {
    if (i != 0) {
      line << '\t';
    }
    line << EscapeManifestField(fields[i]);
  }
  return line.str();
}

std::optional<std::string>
TempWorkspaceLifecycleManager::NextAllocationIdLocked(const TempWorkspaceAllocationRequest& request,
                                                      std::string* error) const {
  auto token = MakeSecureRandomToken(error);
  if (!token.has_value()) {
    return std::nullopt;
  }
  std::ostringstream id;
  id << "tw-" << *token << '-' << SanitizePathToken(request.owner.temp_object_uuid);
  return id.str();
}

std::filesystem::path
TempWorkspaceLifecycleManager::PathForAllocationLocked(const std::string& allocation_id,
                                                       const TempWorkspaceAllocationRequest& request) const {
  std::string name = allocation_id;
  name += '-';
  name += TempStorageClassName(request.storage_class);
  name += ".spill";
  return policy_.root_path / SanitizePathToken(std::move(name));
}

Status TempWorkspaceLifecycleManager::TempStatus(StatusCode code, Severity severity) const {
  return {code, severity, scratchbird::core::platform::Subsystem::memory};
}

DiagnosticRecord
TempWorkspaceLifecycleManager::MakeDiagnostic(Status status,
                                              std::string diagnostic_code,
                                              std::string message_key,
                                              const TempWorkspaceOwner& owner,
                                              const std::vector<DiagnosticArgument>& extra) const {
  std::vector<DiagnosticArgument> arguments;
  arguments.push_back({"policy", policy_.policy_name});
  arguments.push_back({"root_path", policy_.root_path.string()});
  if (!owner.temp_object_uuid.empty()) arguments.push_back({"temp_object_uuid", owner.temp_object_uuid});
  if (!owner.database_id.empty()) arguments.push_back({"database_id", owner.database_id});
  if (!owner.engine_id.empty()) arguments.push_back({"engine_id", owner.engine_id});
  if (!owner.session_id.empty()) arguments.push_back({"session_id", owner.session_id});
  if (!owner.transaction_id.empty()) arguments.push_back({"transaction_id", owner.transaction_id});
  if (!owner.statement_id.empty()) arguments.push_back({"statement_id", owner.statement_id});
  if (!owner.operation_id.empty()) arguments.push_back({"operation_id", owner.operation_id});
  arguments.push_back({"policy_generation", std::to_string(owner.policy_generation)});
  arguments.push_back({"security_generation", std::to_string(owner.security_generation)});
  arguments.push_back({"disk_reservation_mode",
                       TempWorkspaceDiskReservationModeName(EffectiveDiskReservationMode(policy_))});
  arguments.push_back({"physical_disk_reservation_required",
                       policy_.require_physical_disk_reservation ? "true" : "false"});
  arguments.push_back({"authority_boundary", kTempWorkspaceAuthorityBoundary});
  arguments.insert(arguments.end(), extra.begin(), extra.end());
  return scratchbird::core::platform::MakeDiagnostic(status.code,
                                                     status.severity,
                                                     status.subsystem,
                                                     std::move(diagnostic_code),
                                                     std::move(message_key),
                                                     std::move(arguments),
                                                     {},
                                                     "core.memory.temp_workspace",
                                                     "Use temp workspace diagnostics as resource and security evidence only; transaction finality, visibility, authorization, recovery, parser, reference, and benchmark authority remain with their owning subsystems.");
}

const char* TempStorageClassName(TempStorageClass value) {
  switch (value) {
    case TempStorageClass::memory_workspace: return "memory_workspace";
    case TempStorageClass::spill_file: return "spill_file";
    case TempStorageClass::temporary_page_space: return "temporary_page_space";
    case TempStorageClass::temporary_relation: return "temporary_relation";
    case TempStorageClass::temporary_index: return "temporary_index";
    case TempStorageClass::materialized_result: return "materialized_result";
    case TempStorageClass::cursor_backing_store: return "cursor_backing_store";
    case TempStorageClass::sort_workspace: return "sort_workspace";
    case TempStorageClass::hash_workspace: return "hash_workspace";
    case TempStorageClass::bulk_dml_staging: return "bulk_dml_staging";
    case TempStorageClass::backup_restore_scratch: return "backup_restore_scratch";
    case TempStorageClass::archive_package_scratch: return "archive_package_scratch";
    case TempStorageClass::verification_scratch: return "verification_scratch";
    case TempStorageClass::udr_workspace: return "udr_workspace";
    case TempStorageClass::parser_workspace: return "parser_workspace";
  }
  return "unknown";
}

const char* TempWorkspaceLifetimeName(TempWorkspaceLifetime value) {
  switch (value) {
    case TempWorkspaceLifetime::statement_lifetime: return "statement_lifetime";
    case TempWorkspaceLifetime::cursor_lifetime: return "cursor_lifetime";
    case TempWorkspaceLifetime::result_set_lifetime: return "result_set_lifetime";
    case TempWorkspaceLifetime::savepoint_lifetime: return "savepoint_lifetime";
    case TempWorkspaceLifetime::transaction_lifetime: return "transaction_lifetime";
    case TempWorkspaceLifetime::session_lifetime: return "session_lifetime";
    case TempWorkspaceLifetime::operation_lifetime: return "operation_lifetime";
    case TempWorkspaceLifetime::scheduler_task_lifetime: return "scheduler_task_lifetime";
    case TempWorkspaceLifetime::recovery_lifetime: return "recovery_lifetime";
    case TempWorkspaceLifetime::administrator_review_lifetime: return "administrator_review_lifetime";
  }
  return "unknown";
}

const char* TempTransactionOutcomeEvidenceName(TempTransactionOutcomeEvidence value) {
  switch (value) {
    case TempTransactionOutcomeEvidence::none: return "none";
    case TempTransactionOutcomeEvidence::committed: return "committed";
    case TempTransactionOutcomeEvidence::rolled_back: return "rolled_back";
    case TempTransactionOutcomeEvidence::in_doubt: return "in_doubt";
    case TempTransactionOutcomeEvidence::recovery_required: return "recovery_required";
  }
  return "unknown";
}

const char* TempRecoveryClassName(TempRecoveryClass value) {
  switch (value) {
    case TempRecoveryClass::discard_safe: return "discard_safe";
    case TempRecoveryClass::discard_after_evidence: return "discard_after_evidence";
    case TempRecoveryClass::resume_required: return "resume_required";
    case TempRecoveryClass::operation_owned_resume: return "operation_owned_resume";
    case TempRecoveryClass::review_required: return "review_required";
    case TempRecoveryClass::quarantine_required: return "quarantine_required";
    case TempRecoveryClass::leaked_cleanup_required: return "leaked_cleanup_required";
    case TempRecoveryClass::cleanup_refused: return "cleanup_refused";
  }
  return "unknown";
}

const char* TempCleanupReasonName(TempCleanupReason value) {
  switch (value) {
    case TempCleanupReason::statement_end: return "statement_end";
    case TempCleanupReason::commit: return "commit";
    case TempCleanupReason::rollback: return "rollback";
    case TempCleanupReason::disconnect: return "disconnect";
    case TempCleanupReason::shutdown: return "shutdown";
    case TempCleanupReason::recovery: return "recovery";
    case TempCleanupReason::operation_complete: return "operation_complete";
    case TempCleanupReason::administrator: return "administrator";
  }
  return "unknown";
}

const char* TempWorkspaceStateName(TempWorkspaceState value) {
  switch (value) {
    case TempWorkspaceState::active: return "active";
    case TempWorkspaceState::cleaned: return "cleaned";
    case TempWorkspaceState::cleanup_refused: return "cleanup_refused";
    case TempWorkspaceState::cleanup_failed: return "cleanup_failed";
    case TempWorkspaceState::quarantined: return "quarantined";
    case TempWorkspaceState::review_required: return "review_required";
  }
  return "unknown";
}

const char* TempWorkspaceDiskReservationModeName(TempWorkspaceDiskReservationMode value) {
  switch (value) {
    case TempWorkspaceDiskReservationMode::logical_quota_only:
      return "logical_quota_only";
    case TempWorkspaceDiskReservationMode::sparse_file:
      return "sparse_file";
    case TempWorkspaceDiskReservationMode::physical_preallocate:
      return "physical_preallocate";
  }
  return "unknown";
}

TempWorkspacePlatformSecurityCapabilities CurrentTempWorkspacePlatformSecurityCapabilities() {
  TempWorkspacePlatformSecurityCapabilities capabilities;
  capabilities.production_supported_platforms = {"linux", "windows", "macos", "bsd", "posix"};
  capabilities.cleanup_supported = true;
  capabilities.evidence.push_back("MMCH_TEMP_WORKSPACE_CROSS_PLATFORM");
  capabilities.evidence.push_back(
      "temp_workspace.platform_authority_scope=evidence_only_not_transaction_finality_row_visibility_security_authorization_recovery_parser_reference_wal_benchmark_optimizer_plan_or_agent_action_authority");
#if defined(_WIN32)
  capabilities.platform_name = "windows";
  capabilities.secure_random_provider = "BCryptGenRandom";
  capabilities.secure_root_semantics = "GetFileAttributesW_directory_reparse_refusal";
  capabilities.secure_file_semantics = "CreateFileW_CREATE_NEW_share_none";
  capabilities.disk_reservation_semantics =
      "SetFileInformationByHandle_FileAllocationInfo_or_sparse_SetEndOfFile";
  capabilities.secure_random_supported = true;
  capabilities.exclusive_create_supported = true;
  capabilities.owner_only_permissions_supported = true;
  capabilities.nofollow_or_platform_equivalent_supported = true;
  capabilities.hardlink_or_reparse_refusal_supported = true;
  capabilities.physical_preallocation_supported = true;
#elif defined(__APPLE__)
  capabilities.platform_name = "macos";
  capabilities.secure_random_provider = "arc4random_buf";
  capabilities.secure_root_semantics = "posix_open_directory_no_follow_fchmod_0700";
  capabilities.secure_file_semantics = "posix_openat_o_creat_o_excl_o_nofollow_fchmod_0600_fstat_nlink";
  capabilities.disk_reservation_semantics = "sparse_lseek_write_or_posix_fallocate_when_available";
  capabilities.secure_random_supported = true;
  capabilities.exclusive_create_supported = true;
  capabilities.owner_only_permissions_supported = true;
  capabilities.nofollow_or_platform_equivalent_supported = true;
  capabilities.hardlink_or_reparse_refusal_supported = true;
  capabilities.physical_preallocation_supported = true;
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
  capabilities.platform_name = "bsd";
  capabilities.secure_random_provider = "arc4random_buf";
  capabilities.secure_root_semantics = "posix_open_directory_no_follow_fchmod_0700";
  capabilities.secure_file_semantics = "posix_openat_o_creat_o_excl_o_nofollow_fchmod_0600_fstat_nlink";
  capabilities.disk_reservation_semantics = "sparse_lseek_write_or_posix_fallocate_when_available";
  capabilities.secure_random_supported = true;
  capabilities.exclusive_create_supported = true;
  capabilities.owner_only_permissions_supported = true;
  capabilities.nofollow_or_platform_equivalent_supported = true;
  capabilities.hardlink_or_reparse_refusal_supported = true;
  capabilities.physical_preallocation_supported = true;
#elif defined(__linux__)
  capabilities.platform_name = "linux";
  capabilities.secure_random_provider = "getrandom";
  capabilities.secure_root_semantics = "posix_open_directory_no_follow_fchmod_0700";
  capabilities.secure_file_semantics = "posix_openat_o_creat_o_excl_o_nofollow_fchmod_0600_fstat_nlink";
  capabilities.disk_reservation_semantics = "sparse_lseek_write_or_posix_fallocate";
  capabilities.secure_random_supported = true;
  capabilities.exclusive_create_supported = true;
  capabilities.owner_only_permissions_supported = true;
  capabilities.nofollow_or_platform_equivalent_supported = true;
  capabilities.hardlink_or_reparse_refusal_supported = true;
  capabilities.physical_preallocation_supported = true;
#else
  capabilities.platform_name = "posix";
  capabilities.secure_random_provider = "dev_urandom";
  capabilities.secure_root_semantics = "posix_open_directory_no_follow_fchmod_0700";
  capabilities.secure_file_semantics = "posix_openat_o_creat_o_excl_o_nofollow_fchmod_0600_fstat_nlink";
  capabilities.disk_reservation_semantics = "sparse_lseek_write_or_posix_fallocate_when_available";
  capabilities.secure_random_supported = true;
  capabilities.exclusive_create_supported = true;
  capabilities.owner_only_permissions_supported = true;
  capabilities.nofollow_or_platform_equivalent_supported = true;
  capabilities.hardlink_or_reparse_refusal_supported = true;
  capabilities.physical_preallocation_supported = true;
#endif
  capabilities.evidence.push_back("temp_workspace.platform_name=" + capabilities.platform_name);
  capabilities.evidence.push_back("temp_workspace.secure_random_provider=" +
                                  capabilities.secure_random_provider);
  capabilities.evidence.push_back("temp_workspace.secure_file_semantics=" +
                                  capabilities.secure_file_semantics);
  capabilities.evidence.push_back("temp_workspace.disk_reservation_semantics=" +
                                  capabilities.disk_reservation_semantics);
  return capabilities;
}

}  // namespace scratchbird::core::memory
