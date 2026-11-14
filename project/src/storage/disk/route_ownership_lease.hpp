// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <chrono>
#include <memory>
#include <thread>
#include <optional>
#include <string>
#include <utility>

namespace scratchbird::server { class DatabaseOwnershipLock; }
namespace scratchbird::storage::database { class NativeOwnedCheckpointSource; class NativeOwnedCheckpointWriter; }

namespace scratchbird::storage::disk {

class RouteOwnershipLease;
class RouteSourceTransition;
class RouteSourceDrainWait;
class FileDevice;
// Initial OS capability selection only; neither mode grants engine mutation.
enum class RouteStorageAccess : std::uint8_t { read_only, read_write };
// SEARCH_KEY: SB_NATIVE_DATA_FILE_OWNERSHIP (slice step S1, owner 2026-11-13)
// Which authority makes a process the exclusive owner of a database file. The
// caller states it; nothing infers it from the file's contents.
//  - legacy_lock_files: the legacy create/open path, unchanged until the one
//    production cut (Q93): <db>.sb.route.owner.lock (with a pid/endpoint
//    descriptor) + <db>.sb.owner.lock, plus flock on the data file.
//  - native_data_file: the native single-file path. The ONLY authority is an
//    exclusive non-blocking flock(LOCK_EX|LOCK_NB) on the owner's retained
//    data-file descriptor, taken right after open and before any read or
//    write. No lock file and no descriptor text file is ever created (owner
//    2026-11-13: the lock files go the moment it is logical; S1 is that moment
//    for the native path). A refusal names no incumbent pid/endpoint (Q94).
// flock, not F_OFD_SETLK: legacy owners and legacy FileDevice::Open already
// flock the data inode (LockDataFile). On Linux flock and fcntl locks
// (including OFD locks) do not conflict, so an OFD-locking native owner and a
// flock-ing legacy owner could both "own" one file during coexistence. flock
// is also an open-file-description lock (dup'd served descriptors share it,
// FINDING-17), is released only when the last descriptor of that description
// closes, and exists on Linux and the BSDs. Classic F_SETLK is never used: it
// is per-process and dropped by closing ANY descriptor of the file.
enum class DatabaseFileOwnership : std::uint8_t { legacy_lock_files, native_data_file };
enum class RouteWriteCustodyState : std::uint8_t {
  not_requested, retained, primary_not_bound, withdrawn, wrong_process, lock_failure
};
enum class RouteSourceTransitionError {
  none, invalid_owner, wrong_process, withdrawn, resource_exhausted, lock_failure,
  invalid_deadline, not_drained, admission_busy, already_transferred,
  primary_not_bound, io_failure, writable_not_requested,
  // Source admission is attempted from a thread other than the one that began
  // the transition (owner ruling 2026-11-07: the route owner's thread adopts).
  wrong_thread
};
// Private factory selector only; constructing this enum grants no capability.
enum class RouteSourceAdmissionKind : std::uint8_t { read_only, initial_writer };
enum class RouteSourceDrainState { pending, drained, withdrawn, wrong_process, lock_failure };
// An observation only. Native source transfer must revalidate under the actual
// owning admission fence; a copied zero count never authorizes a transfer.
struct RouteSourceDrainObservation {
  RouteSourceDrainState state = RouteSourceDrainState::lock_failure;
  std::uint64_t legacy_borrowers = 0;
};
struct RouteSourceTransitionResult {
  RouteSourceTransitionError error = RouteSourceTransitionError::invalid_owner;
  std::shared_ptr<RouteSourceTransition> transition;
  bool ok() const noexcept { return error == RouteSourceTransitionError::none && transition != nullptr; }
};
enum class RouteSourceDrainWaitState {
  pending, drained, cancelled, deadline_expired, withdrawn, wrong_process,
  resource_exhausted, lock_failure
};
struct RouteSourceDrainWaitObservation {
  RouteSourceDrainWaitState state = RouteSourceDrainWaitState::lock_failure;
  std::uint64_t legacy_borrowers = 0;
  std::uint64_t parked_waiters = 0;
};
enum class RouteSourceDrainCancelResult {
  requested, already_completed, wrong_process, lock_failure
};
struct RouteSourceDrainWaitResult {
  RouteSourceTransitionError error = RouteSourceTransitionError::invalid_owner;
  std::shared_ptr<RouteSourceDrainWait> wait;
  bool ok() const noexcept { return error == RouteSourceTransitionError::none && wait != nullptr; }
};
// One retained, bounded wait attempt. Cancellation affects this attempt only;
// retries use the same transition and never reopen legacy admission. Retain a
// shared handle for every concurrent call. Completion is immutable and is NOT
// native transfer authority; transfer must revalidate the actual owner state.
class RouteSourceDrainWait final {
 public:
  RouteSourceDrainWait(const RouteSourceDrainWait&) = delete;
  RouteSourceDrainWait& operator=(const RouteSourceDrainWait&) = delete;
  RouteSourceDrainWaitObservation Wait() noexcept;
  RouteSourceDrainWaitObservation Observe() const noexcept;
  RouteSourceDrainCancelResult Cancel() noexcept;
 private:
  friend class RouteSourceTransition;
  RouteSourceDrainWait(std::shared_ptr<RouteOwnershipLease> owner,
      std::chrono::steady_clock::time_point deadline)
      : owner_(std::move(owner)), deadline_(deadline) {}
  std::shared_ptr<RouteOwnershipLease> owner_;
  const std::chrono::steady_clock::time_point deadline_;
  RouteSourceDrainWaitState state_ = RouteSourceDrainWaitState::pending;
  std::uint64_t completed_borrowers_ = 0;
  std::uint64_t parked_waiters_ = 0;
};
class RouteSourceTransition final {
 public:
  RouteSourceTransition(const RouteSourceTransition&) = delete;
  RouteSourceTransition& operator=(const RouteSourceTransition&) = delete;
  // Nonblocking observation, not a wait/cancel/transfer or shutdown receipt.
  RouteSourceDrainObservation ObserveDrain() const noexcept;
  // A finite monotonic deadline is mandatory; max() is not an unbounded wait.
  RouteSourceDrainWaitResult BeginDrainWait(
      std::chrono::steady_clock::time_point deadline) const noexcept;
 private:
  friend class RouteOwnershipLease;
  friend class scratchbird::storage::database::NativeOwnedCheckpointSource;
  friend class scratchbird::storage::database::NativeOwnedCheckpointWriter;
  // Only the validating source factory may reserve/consume admission. No
  // observation, pathname or ordinary borrower can construct this cohort.
  RouteSourceTransitionError BeginSourceAdmission(std::unique_ptr<FileDevice>& primary,
      RouteSourceAdmissionKind=RouteSourceAdmissionKind::read_only) noexcept;
  RouteSourceTransitionError CheckTransferredWriterAdmission() const noexcept;
  RouteSourceTransitionError CommitSourceAdmission() noexcept;
  void AbortSourceAdmission() noexcept;
  explicit RouteSourceTransition(std::shared_ptr<RouteOwnershipLease> owner)
      : owner_(std::move(owner)) {}
  std::shared_ptr<RouteOwnershipLease> owner_;
  // The thread that began the transition; only it may admit the source. A
  // one-file route has no caller-opened member whose own thread affinity would
  // otherwise refuse a foreign adopter.
  const std::thread::id opening_thread_ = std::this_thread::get_id();
};

// A pin on real route AND storage OS locks, never on discovery metadata.
// Only the native owner can publish/withdraw admission. Existing pins survive
// withdrawal, but no new FileDevice may borrow a withdrawn or inherited lease.
class RouteOwnershipLease final {
 public:
  ~RouteOwnershipLease();
  RouteOwnershipLease(const RouteOwnershipLease&) = delete;
  RouteOwnershipLease& operator=(const RouteOwnershipLease&) = delete;
  static std::shared_ptr<RouteOwnershipLease> Borrow(const std::string& route_path);
  // Includes admitted opens waiting for their path mutex. Copies share one
  // admission, which retires only after the last copy. Never an authority grant.
  std::optional<std::uint64_t> ObserveLegacyBorrowers() const noexcept;
  RouteWriteCustodyState ObserveWriteCustody() const noexcept;

 private:
  friend class scratchbird::server::DatabaseOwnershipLock;
  friend class FileDevice;
  friend class RouteSourceTransition;
  friend class RouteSourceDrainWait;
  struct DrainSignal;
  std::unique_ptr<DrainSignal> drain_signal_;
  struct BorrowPin;
  static std::shared_ptr<RouteOwnershipLease> PinBorrowLocked(
      const std::shared_ptr<RouteOwnershipLease>& owner);
  static RouteSourceTransitionResult BeginNativeSourceTransition(
      const std::shared_ptr<RouteOwnershipLease>& owner) noexcept;
  static std::shared_ptr<RouteOwnershipLease> BorrowAlias(const std::string& path);
#ifdef _WIN32
  std::uint32_t AcquireDataFile(const std::string& path);
  std::uint32_t BindDataFile(void* file);
  void* data_handle_ = nullptr;  // independently reopened physical owner handle
#else
  int AcquireDataFile(const std::string& path);
  int BindDataFile(int fd,int initial_writer_fd=-1);
  int data_fd_ = -1;  // actual primary inode; retained through the final reader
#endif
  RouteOwnershipLease(std::string route_path, std::uint64_t owner_pid,
      RouteStorageAccess access=RouteStorageAccess::read_only,
      DatabaseFileOwnership ownership=DatabaseFileOwnership::legacy_lock_files);
  bool Publish(const std::shared_ptr<RouteOwnershipLease>& self);
  void Withdraw();
  bool valid() const;
  // The owner's identity before the data file is bound: a native lease has no
  // lock-file handles, so binding is admitted on the owning process alone.
  bool may_bind() const;
  bool native_data_file() const noexcept {
    return ownership_ == DatabaseFileOwnership::native_data_file;
  }
  // SB_NATIVE_DATA_FILE_OWNERSHIP: the process-local registry key. Legacy keys
  // are the route lock path (unchanged). A native lease is keyed by the bound
  // data file's identity (st_dev/st_ino), set at binding: the flock that is the
  // authority is attached to that inode, the key survives rename and hard
  // links, and in-process lookups already match by inode (BorrowAlias). It is a
  // process-memory index only, never persisted and never a database identifier.
  const std::string& registry_key() const noexcept {
    return native_data_file() ? native_registry_key_ : route_path_;
  }
  using NativeHandle =
#ifdef _WIN32
      void*;
#else
      int;
#endif
  NativeHandle handle_{
#ifdef _WIN32
      nullptr
#else
      -1
#endif
  };
  NativeHandle storage_handle_{
#ifdef _WIN32
      nullptr
#else
      -1
#endif
  };
  // Legacy: the .sb.route.owner.lock path. Native: the database path, used
  // for diagnostics only (never a registry key, never a lock file).
  const std::string route_path_;
  const std::uint64_t owner_pid_;
  const RouteStorageAccess storage_access_;
  const DatabaseFileOwnership ownership_;
  std::string native_registry_key_;  // set once at native binding
#ifdef _WIN32
  void* write_data_handle_ = nullptr;
#else
  int write_data_fd_ = -1;
#endif
  bool accepting_ = false;  // guarded by the process-local registry mutex
  bool issuing_ = false;   // Withdraw revokes even a previously fenced issuer
  bool transition_started_ = false;
  bool source_admission_active_ = false;
  bool source_transferred_ = false;
  RouteSourceAdmissionKind source_admission_kind_=RouteSourceAdmissionKind::read_only;
  bool source_transferred_as_writer_=false;
  std::uint64_t legacy_borrowers_ = 0;
  std::weak_ptr<RouteSourceTransition> transition_;
};

}  // namespace scratchbird::storage::disk
