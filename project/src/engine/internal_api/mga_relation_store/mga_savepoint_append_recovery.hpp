// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

// Private physical custody for an uncertain native-statement RELEASE append.
// This is not a statement result store or transaction/security admission API.
#include "api_diagnostics.hpp"
#include "core/hash/blake3_digest.hpp"
#include "core/memory/memory.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace scratchbird::engine::internal_api::savepoint_append_recovery {

inline EngineApiDiagnostic Ok() {
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}
inline EngineApiDiagnostic Refused(const char* detail) {
  return MakeInvalidRequestDiagnostic("transaction.release_savepoint", detail);
}

#if !defined(_WIN32)
enum class AppendPhase : std::uint8_t { none, write, file_sync, directory_sync, writer_close };
enum class CompensationPhase : std::uint8_t {
  not_attempted, identity, prefix, suffix, truncate, file_sync, directory_sync, verification, restored
};
constexpr std::uint64_t PhaseError(std::uint8_t phase, int error) noexcept {
  return (static_cast<std::uint64_t>(phase) << 32) | static_cast<std::uint32_t>(error);
}
constexpr int PhaseErrno(std::uint64_t value) noexcept { return static_cast<int>(static_cast<std::uint32_t>(value)); }
constexpr AppendPhase PrimaryPhase(std::uint64_t value) noexcept { return static_cast<AppendPhase>(value >> 32); }
constexpr CompensationPhase RestorePhase(std::uint64_t value) noexcept { return static_cast<CompensationPhase>(value >> 32); }
inline const char* PhaseName(AppendPhase phase) noexcept {
  switch (phase) {
    case AppendPhase::none: return "none";
    case AppendPhase::write: return "write";
    case AppendPhase::file_sync: return "file_sync";
    case AppendPhase::directory_sync: return "directory_sync";
    case AppendPhase::writer_close: return "writer_close";
  }
  return "none";
}
inline const char* PhaseName(CompensationPhase phase) noexcept {
  switch (phase) {
    case CompensationPhase::not_attempted: return "not_attempted";
    case CompensationPhase::identity: return "identity";
    case CompensationPhase::prefix: return "prefix";
    case CompensationPhase::suffix: return "suffix";
    case CompensationPhase::truncate: return "truncate";
    case CompensationPhase::file_sync: return "file_sync";
    case CompensationPhase::directory_sync: return "directory_sync";
    case CompensationPhase::verification: return "verification";
    case CompensationPhase::restored: return "restored";
  }
  return "not_attempted";
}
struct Owner final {
  Owner* next = nullptr;
  std::shared_ptr<core::memory::MemoryManager> manager;
  std::optional<core::memory::MemoryPolicyBinding> memory_policy;
  EngineUuid database, transaction, operation;
  std::uint64_t local_transaction = 0;
  pid_t process = 0;
  int recovery_fd = -1, directory_fd = -1;
  dev_t device = 0;
  ino_t inode = 0;
  off_t original_end = 0;
  std::size_t path_size = 0, database_path_size = 0, leaf_offset = 0, frame_size = 0;
  std::atomic<int> native_error{0};
  // Phase+errno pairs publish atomically, never a last-error-wins substitute.
  std::atomic<std::uint64_t> append_failure{0}, compensation{0};
  std::atomic<int> writer_close_error{0};
  bool in_progress = false;
  core::hash::Blake3Digest prefix_digest{};
  // Actual I/O/hash working state belongs to this charged, retained owner.
  core::hash::Blake3IncrementalStateV3 hash_state{};
  std::array<std::uint8_t, 4096> buffer{};
  char* path() noexcept { return reinterpret_cast<char*>(this + 1); }
  const char* path() const noexcept { return reinterpret_cast<const char*>(this + 1); }
  const char* leaf() const noexcept { return path() + leaf_offset; }
  std::uint8_t* frame() noexcept {
    return reinterpret_cast<std::uint8_t*>(path() + path_size + 1);
  }
  ~Owner() noexcept {
    // Linux close errors retire the descriptor; no retry of its integer value.
    if (recovery_fd >= 0) { const int fd = recovery_fd; recovery_fd = -1; (void)::close(fd); }
    if (directory_fd >= 0) { const int fd = directory_fd; directory_fd = -1; (void)::close(fd); }
    core::hash::ScrubBlake3IncrementalV3(&hash_state);
  }
};
struct Registry final {
  const pid_t process = ::getpid();
  std::mutex mutex;
  Owner* head = nullptr;
};
inline Registry& Entries() { static Registry registry; return registry; }

struct PendingObservation final {
  bool present = false, wrong_process = false;
  EngineUuid database, transaction, operation;
  std::uint64_t local_transaction = 0, original_end = 0;
  int native_error = 0;
  std::uint64_t append_failure = 0, compensation = 0;
  int writer_close_error = 0;
};
inline PendingObservation ObserveOwner(const Owner& owner) noexcept {
  PendingObservation observation;
  observation.present = true;
  observation.database = owner.database;
  observation.transaction = owner.transaction;
  observation.operation = owner.operation;
  observation.local_transaction = owner.local_transaction;
  observation.original_end = static_cast<std::uint64_t>(owner.original_end);
  observation.native_error = owner.native_error.load();
  observation.append_failure = owner.append_failure.load();
  observation.compensation = owner.compensation.load();
  observation.writer_close_error = owner.writer_close_error.load();
  return observation;
}
inline bool Matches(const Owner& owner, const EngineRequestContext& context) noexcept {
  return owner.database == context.database_uuid ||
      (owner.database_path_size == context.database_path.size() &&
       std::memcmp(owner.path(), context.database_path.data(), owner.database_path_size) == 0);
}
inline PendingObservation Observe(const EngineRequestContext& context) {
  auto& entries = Entries();
  PendingObservation observation;
  if (entries.process != ::getpid()) { observation.wrong_process = true; return observation; }
  std::lock_guard lock(entries.mutex);
  for (const auto* owner = entries.head; owner; owner = owner->next) {
    if (!Matches(*owner, context)) continue;
    observation = ObserveOwner(*owner);
    break;
  }
  return observation;
}
inline void AddPhysicalCause(EngineApiDiagnostic& result, const PendingObservation& observation) {
  result.identity_fields = {{"database_uuid", observation.database},
      {"transaction_uuid", observation.transaction}, {"savepoint_uuid", observation.operation}};
  result.fields = {{"local_transaction_id", std::to_string(observation.local_transaction)},
      {"original_end", std::to_string(observation.original_end)},
      {"native_error", std::to_string(observation.native_error)},
      {"append_phase", PhaseName(PrimaryPhase(observation.append_failure))},
      {"append_native_error", std::to_string(PhaseErrno(observation.append_failure))},
      {"writer_close_native_error", std::to_string(observation.writer_close_error)},
      {"compensation_phase", PhaseName(RestorePhase(observation.compensation))},
      {"compensation_native_error", std::to_string(PhaseErrno(observation.compensation))}};
}
inline EngineApiDiagnostic Pending(const PendingObservation& observation) {
  if (observation.wrong_process) return Refused("native_savepoint_recovery_wrong_process");
  if (!observation.present) { EngineApiDiagnostic result; result.error = false; return result; }
  auto result = MakeEngineApiDiagnostic("MGA.STATEMENT.RECONCILIATION_REQUIRED",
      "mga.statement.reconciliation_required", "pending_native_savepoint_release_append", true);
  AddPhysicalCause(result, observation);
  return result;
}
inline EngineApiDiagnostic Check(const EngineRequestContext& context) { return Pending(Observe(context)); }

inline void Destroy(Owner* owner) noexcept {
  if (!owner) return;
  auto manager = std::move(owner->manager);
  owner->~Owner();
  (void)manager->allocator()->DeallocateNoAlloc(owner);
}
struct PreparedOwner final {
  Owner* value = nullptr;
  ~PreparedOwner() noexcept { Destroy(value); }
};
inline bool Identity(const Owner& owner, int fd, struct stat* state) noexcept {
  if (::fstat(fd, state) != 0) return false;
  return S_ISREG(state->st_mode) && state->st_dev == owner.device && state->st_ino == owner.inode;
}
inline bool PathIdentity(const Owner& owner) noexcept {
  struct stat state{};
  return ::fstatat(owner.directory_fd, owner.leaf(), &state, AT_SYMLINK_NOFOLLOW) == 0 &&
      S_ISREG(state.st_mode) && state.st_dev == owner.device && state.st_ino == owner.inode;
}
inline bool ReadAt(Owner& owner, off_t offset, std::size_t bytes) noexcept {
  std::size_t read = 0;
  while (read < bytes) {
    const auto count = ::pread(owner.recovery_fd, owner.buffer.data() + read, bytes - read,
        offset + static_cast<off_t>(read));
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) { owner.native_error = count < 0 ? errno : EIO; return false; }
    read += static_cast<std::size_t>(count);
  }
  return true;
}
inline bool PrefixDigest(Owner& owner, core::hash::Blake3Digest* output) noexcept {
  namespace hash = core::hash;
  // Each source reread is an independent hash operation. The native hasher
  // deliberately rejects reinitializing a finalized/failed state. Retain its
  // charged storage, not its terminal lifecycle, between recovery attempts.
  hash::ScrubBlake3IncrementalV3(&owner.hash_state);
  struct Scrub {
    hash::Blake3IncrementalStateV3& state;
    ~Scrub() { hash::ScrubBlake3IncrementalV3(&state); }
  } scrub{owner.hash_state};
  if (hash::InitializeBlake3IncrementalV3(&owner.hash_state) != hash::Blake3IncrementalStatusV3::kOk)
    return false;
  off_t offset = 0;
  while (offset < owner.original_end) {
    const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(owner.buffer.size(),
        static_cast<std::uint64_t>(owner.original_end - offset)));
    if (!ReadAt(owner, offset, count) || hash::UpdateBlake3IncrementalV3(&owner.hash_state,
        owner.buffer.data(), count) != hash::Blake3IncrementalStatusV3::kOk) {
      return false;
    }
    offset += static_cast<off_t>(count);
  }
  return hash::FinalizeBlake3IncrementalV3(&owner.hash_state, output) == hash::Blake3IncrementalStatusV3::kOk;
}
inline bool Sync(int fd, Owner& owner) noexcept {
  int result;
  do { result = ::fsync(fd); } while (result != 0 && errno == EINTR);
  if (result != 0) owner.native_error = errno;
  return result == 0;
}
inline void RecordAppendFailure(Owner& owner, AppendPhase phase, int error) noexcept {
  std::uint64_t expected = 0;
  (void)owner.append_failure.compare_exchange_strong(expected, PhaseError(static_cast<std::uint8_t>(phase), error));
  owner.native_error = error;
}
inline bool CompensationFailed(Owner& owner, CompensationPhase phase, int error) noexcept {
  if (!error) error = EIO;
  owner.native_error = error;
  owner.compensation = PhaseError(static_cast<std::uint8_t>(phase), error);
  return false;
}
// Only the exact original inode and an unchanged original prefix plus a prefix
// of THIS owner's release frame may be truncated. Unexpected appends, inode
// replacement, prefix edits and extent loss remain retained recovery failures.
inline bool Restore(Owner& owner) noexcept {
  owner.compensation = 0;
  owner.native_error = 0;
  struct stat state{};
  if (!Identity(owner, owner.recovery_fd, &state) || !PathIdentity(owner) ||
      state.st_size < owner.original_end ||
      static_cast<std::uint64_t>(state.st_size - owner.original_end) > owner.frame_size) {
    return CompensationFailed(owner, CompensationPhase::identity, ESTALE);
  }
  core::hash::Blake3Digest digest{};
  if (!PrefixDigest(owner, &digest) || digest != owner.prefix_digest) {
    return CompensationFailed(owner, CompensationPhase::prefix, owner.native_error.load());
  }
  const auto appended = static_cast<std::size_t>(state.st_size - owner.original_end);
  for (std::size_t offset = 0; offset < appended;) {
    const auto count = std::min(owner.buffer.size(), appended - offset);
    if (!ReadAt(owner, owner.original_end + static_cast<off_t>(offset), count) ||
        std::memcmp(owner.buffer.data(), owner.frame() + offset, count) != 0) {
      return CompensationFailed(owner, CompensationPhase::suffix, owner.native_error.load());
    }
    offset += count;
  }
  if (::ftruncate(owner.recovery_fd, owner.original_end) != 0)
    return CompensationFailed(owner, CompensationPhase::truncate, errno);
  if (!Sync(owner.recovery_fd, owner))
    return CompensationFailed(owner, CompensationPhase::file_sync, owner.native_error.load());
  if (!Sync(owner.directory_fd, owner))
    return CompensationFailed(owner, CompensationPhase::directory_sync, owner.native_error.load());
  if (!Identity(owner, owner.recovery_fd, &state) || state.st_size != owner.original_end ||
      !PathIdentity(owner) || !PrefixDigest(owner, &digest) || digest != owner.prefix_digest) {
    return CompensationFailed(owner, CompensationPhase::verification, owner.native_error.load());
  }
  owner.compensation = PhaseError(static_cast<std::uint8_t>(CompensationPhase::restored), 0);
  owner.native_error = owner.writer_close_error.load() ? owner.writer_close_error.load() : PhaseErrno(owner.append_failure.load());
  return true;
}
inline void Unlink(Owner* owner) noexcept {
  auto& entries = Entries();
  std::lock_guard lock(entries.mutex);
  auto** link = &entries.head;
  while (*link && *link != owner) link = &(*link)->next;
  if (*link) *link = owner->next;
}

inline EngineApiDiagnostic Append(const EngineRequestContext& context, const EngineUuid& operation,
    const std::string& path, const std::string& frame) {
  auto& entries = Entries();
  if (entries.process != ::getpid()) return Refused("native_savepoint_recovery_wrong_process");
  const auto pending = Check(context);
  if (pending.error) return pending;
  if (!core::uuid::IsEngineIdentityUuid(context.database_uuid) ||
      !core::uuid::IsEngineIdentityUuid(context.transaction_uuid) ||
      !core::uuid::IsEngineIdentityUuid(operation) || !context.local_transaction_id ||
      context.database_path.empty() || context.database_path.find('\0') != std::string::npos ||
      path.find('\0') != std::string::npos || frame.empty() ||
      context.database_path.size() > path.size() ||
      std::memcmp(path.data(), context.database_path.data(), context.database_path.size()) != 0 ||
      path.size() > std::numeric_limits<std::size_t>::max() - sizeof(Owner) - 1 ||
      frame.size() > std::numeric_limits<std::size_t>::max() - sizeof(Owner) - 1 - path.size())
    return Refused("native_savepoint_recovery_binding_invalid");
  auto manager = core::memory::RetainDefaultMemoryManagerV1();
  if (!manager) return Refused("native_savepoint_recovery_memory_unavailable");
  core::memory::MemoryTag tag;
  tag.category = core::memory::MemoryCategory::transaction_local;
  tag.lifetime = core::memory::MemoryLifetime::transaction;
  tag.purpose = "savepoint_append_recovery";
  using Scope = core::memory::MemoryBinaryScopeKind;
  tag.binary_ownership[Scope::context] = context.transaction_uuid.bytes;
  tag.binary_ownership[Scope::owner] = operation.bytes;
  tag.binary_ownership[Scope::database] = context.database_uuid.bytes;
  tag.binary_ownership[Scope::transaction] = context.transaction_uuid.bytes;
  auto allocation = manager->Allocate(sizeof(Owner) + path.size() + 1 + frame.size(),
      alignof(Owner), std::move(tag));
  if (!allocation.ok()) return MakeEngineApiDiagnosticFromNative(allocation.diagnostic,
      "RESOURCE.BUDGET_EXCEEDED", "engine.nosql.key_value.refused", "native_savepoint_recovery_memory_unavailable");
  PreparedOwner prepared{new (allocation.pointer) Owner};
  auto& owner = *prepared.value;
  owner.manager = std::move(manager);
  owner.memory_policy = allocation.policy_binding;
  owner.process = ::getpid();
  owner.database = context.database_uuid; owner.transaction = context.transaction_uuid;
  owner.operation = operation; owner.local_transaction = context.local_transaction_id;
  owner.path_size = path.size(); owner.database_path_size = context.database_path.size();
  const auto slash = path.find_last_of('/');
  owner.leaf_offset = slash == std::string::npos ? 0 : slash + 1;
  owner.frame_size = frame.size();
  std::memcpy(owner.path(), path.c_str(), path.size() + 1);
  std::memcpy(owner.frame(), frame.data(), frame.size());
  const auto directory = slash == std::string::npos ? std::string(".") :
      slash == 0 ? std::string("/") : path.substr(0, slash);
  owner.directory_fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (owner.directory_fd < 0) return Refused("native_savepoint_recovery_directory_unavailable");
  owner.recovery_fd = ::openat(owner.directory_fd, owner.leaf(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
  if (owner.recovery_fd < 0) return Refused("native_savepoint_recovery_handle_unavailable");
  struct stat original{};
  if (::fstat(owner.recovery_fd, &original) != 0 || !S_ISREG(original.st_mode) || original.st_size < 0 ||
      frame.size() > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) ||
      static_cast<std::uint64_t>(original.st_size) >
          static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) - frame.size())
    return Refused("native_savepoint_recovery_extent_invalid");
  owner.device = original.st_dev; owner.inode = original.st_ino; owner.original_end = original.st_size;
  if (!PathIdentity(owner) || !PrefixDigest(owner, &owner.prefix_digest))
    return Refused("native_savepoint_recovery_prefix_unavailable");
  // All fallible diagnostic preparation precedes the first write as well.
  auto success = Ok();
  auto failure = Refused("savepoint_release_append_failed");
  const int writer = ::openat(owner.directory_fd, owner.leaf(), O_WRONLY | O_APPEND | O_CLOEXEC | O_NOFOLLOW);
  if (writer < 0) return failure;
  struct stat writer_state{};
  if (!Identity(owner, writer, &writer_state) || writer_state.st_size != owner.original_end || !PathIdentity(owner)) {
    (void)::close(writer); return Refused("native_savepoint_recovery_identity_changed");
  }
  // Intrusive insertion needs no map node or uncharged retained allocation.
  // The existing transaction-inventory guard spans this entire operation.
  bool conflict = false;
  {
    std::lock_guard lock(entries.mutex);
    for (const auto* candidate = entries.head; candidate; candidate = candidate->next)
      if (Matches(*candidate, context)) { conflict = true; break; }
    if (!conflict) {
      owner.in_progress = true;
      owner.next = entries.head; entries.head = &owner;
    }
  }
  if (conflict) {
    (void)::close(writer);
    const auto observed = Check(context);
    return observed.error ? observed : failure;
  }
  prepared.value = nullptr; // registry now owns the actual memory and handles
  std::size_t written = 0;
  while (written < owner.frame_size) {
    const auto count = ::write(writer, owner.frame() + written, owner.frame_size - written);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) { RecordAppendFailure(owner, AppendPhase::write, count < 0 ? errno : EIO); break; }
    written += static_cast<std::size_t>(count);
  }
  bool durable = written == owner.frame_size;
  if (durable && !Sync(writer, owner)) {
    RecordAppendFailure(owner, AppendPhase::file_sync, owner.native_error.load()); durable = false;
  }
  if (durable && !Sync(owner.directory_fd, owner)) {
    RecordAppendFailure(owner, AppendPhase::directory_sync, owner.native_error.load()); durable = false;
  }
  // Exactly once: a Linux late close failure may already have retired writer.
  if (::close(writer) != 0) {
    owner.writer_close_error = errno;
    RecordAppendFailure(owner, AppendPhase::writer_close, owner.writer_close_error.load()); durable = false;
  }
  if (durable) { Unlink(&owner); Destroy(&owner); return success; }
  if (Restore(owner)) {
    // Rendering is outside every registry lock and after I/O. If rendering
    // fails, keep genuine custody; exact reconciliation can safely retry the
    // already-restored prefix rather than abandon its retained allocation.
    try { AddPhysicalCause(failure, ObserveOwner(owner)); }
    catch (...) {
      std::lock_guard lock(entries.mutex); owner.in_progress = false; throw;
    }
    Unlink(&owner); Destroy(&owner); return failure;
  }
  {
    std::lock_guard lock(entries.mutex);
    owner.in_progress = false;
  }
  return Check(context); // failed compensation retains the exact physical owner
}

// Called only after the public adapter acquires inventory ordering and validates
// the actual native transaction owner. No guessed path reopen is permitted.
inline EngineApiDiagnostic Reconcile(const EngineRequestContext& context, const EngineUuid& operation) {
  auto& entries = Entries();
  if (entries.process != ::getpid()) return Refused("native_savepoint_recovery_wrong_process");
  auto success = Ok();
  Owner* owner = nullptr;
  bool mismatch = false;
  {
    std::lock_guard lock(entries.mutex);
    for (auto* candidate = entries.head; candidate; candidate = candidate->next) {
      if (!Matches(*candidate, context)) continue;
      if (candidate->database != context.database_uuid || candidate->transaction != context.transaction_uuid ||
          candidate->local_transaction != context.local_transaction_id || candidate->operation != operation ||
          candidate->process != ::getpid() || candidate->in_progress) { mismatch = true; break; }
      owner = candidate; owner->in_progress = true; break;
    }
  }
  if (mismatch) return Refused("native_savepoint_recovery_exact_owner_required");
  if (!owner) return Refused("native_savepoint_recovery_owner_not_found");
  if (Restore(*owner)) { Unlink(owner); Destroy(owner); return success; }
  {
    std::lock_guard lock(entries.mutex);
    owner->in_progress = false;
  }
  return Check(context);
}
#else
inline EngineApiDiagnostic Check(const EngineRequestContext&) { return Ok(); }
inline EngineApiDiagnostic Append(const EngineRequestContext&, const EngineUuid&,
    const std::string&, const std::string&) {
  return Refused("native_savepoint_recovery_platform_unavailable");
}
inline EngineApiDiagnostic Reconcile(const EngineRequestContext&, const EngineUuid&) {
  return Refused("native_savepoint_recovery_platform_unavailable");
}
#endif
} // namespace scratchbird::engine::internal_api::savepoint_append_recovery
