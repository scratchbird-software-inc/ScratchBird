// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace scratchbird::server { class DatabaseOwnershipLock; }

namespace scratchbird::storage::disk {

class RouteOwnershipLease;
class RouteSourceTransition;
enum class RouteSourceTransitionError {
  none, invalid_owner, wrong_process, withdrawn, resource_exhausted, lock_failure
};
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
class RouteSourceTransition final {
 public:
  RouteSourceTransition(const RouteSourceTransition&) = delete;
  RouteSourceTransition& operator=(const RouteSourceTransition&) = delete;
  // Nonblocking observation, not a wait/cancel/transfer or shutdown receipt.
  RouteSourceDrainObservation ObserveDrain() const noexcept;
 private:
  friend class RouteOwnershipLease;
  explicit RouteSourceTransition(std::shared_ptr<RouteOwnershipLease> owner)
      : owner_(std::move(owner)) {}
  std::shared_ptr<RouteOwnershipLease> owner_;
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

 private:
  friend class scratchbird::server::DatabaseOwnershipLock;
  friend class FileDevice;
  friend class RouteSourceTransition;
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
  int BindDataFile(int fd);
  int data_fd_ = -1;  // actual primary inode; retained through the final reader
#endif
  RouteOwnershipLease(std::string route_path, std::uint64_t owner_pid);
  bool Publish(const std::shared_ptr<RouteOwnershipLease>& self);
  void Withdraw();
  bool valid() const;
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
  const std::string route_path_;
  const std::uint64_t owner_pid_;
  bool accepting_ = false;  // guarded by the process-local registry mutex
  bool issuing_ = false;   // Withdraw revokes even a previously fenced issuer
  bool transition_started_ = false;
  std::uint64_t legacy_borrowers_ = 0;
  std::weak_ptr<RouteSourceTransition> transition_;
};

}  // namespace scratchbird::storage::disk
