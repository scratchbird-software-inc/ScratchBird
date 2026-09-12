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
#include <string>

namespace scratchbird::server { class DatabaseOwnershipLock; }

namespace scratchbird::storage::disk {

// A pin on real route AND storage OS locks, never on discovery metadata.
// Only the native owner can publish/withdraw admission. Existing pins survive
// withdrawal, but no new FileDevice may borrow a withdrawn or inherited lease.
class RouteOwnershipLease final {
 public:
  ~RouteOwnershipLease();
  RouteOwnershipLease(const RouteOwnershipLease&) = delete;
  RouteOwnershipLease& operator=(const RouteOwnershipLease&) = delete;
  static std::shared_ptr<RouteOwnershipLease> Borrow(const std::string& route_path);

 private:
  friend class scratchbird::server::DatabaseOwnershipLock;
  friend class FileDevice;
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
};

}  // namespace scratchbird::storage::disk
