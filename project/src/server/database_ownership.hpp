// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_DATABASE_OWNERSHIP_LOCK

#pragma once

#include <cstdint>
#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include "../storage/disk/route_ownership_lease.hpp"

namespace scratchbird::storage::disk {
class RouteOwnershipLease;
struct RouteSourceTransitionResult;
}

namespace scratchbird::server {

struct DatabaseOwnershipRequest;
struct DatabaseOwnershipResult;
class ServerAgentRuntime;

struct DatabaseOwnershipDescriptor {
  std::string format;
  std::string owner_kind;
  std::string database_path;
  std::string sbps_endpoint;
  std::string pid;
  std::string created_unix_ms;
};

class DatabaseOwnershipLock {
 public:
  using NativeHandle =
#ifdef _WIN32
      void*;
#else
      int;
#endif


  ~DatabaseOwnershipLock();

  DatabaseOwnershipLock(const DatabaseOwnershipLock&) = delete;
  DatabaseOwnershipLock& operator=(const DatabaseOwnershipLock&) = delete;
  DatabaseOwnershipLock(DatabaseOwnershipLock&& other) noexcept;
  DatabaseOwnershipLock& operator=(DatabaseOwnershipLock&& other) noexcept;

  [[nodiscard]] bool valid() const;
  // Externally serialize issuance, move and release of this owner. Successful
  // issuance fences legacy borrowing only; it is not drain/transfer/readiness.
  // The result definition is in storage/disk/route_ownership_lease.hpp.
  [[nodiscard]] storage::disk::RouteSourceTransitionResult
  BeginNativeSourceTransition() noexcept;
  [[nodiscard]] storage::disk::RouteWriteCustodyState ObserveWriteCustody() const noexcept;
  [[nodiscard]] NativeHandle native_handle() const;
#ifndef _WIN32
  [[nodiscard]] int fd() const { return native_handle(); }
#endif
  [[nodiscard]] const std::filesystem::path& lock_path() const { return lock_path_; }
  // SEARCH_KEY: SB_ROUTE_OWNER_SERVED_DEVICE (register C9). The owner's
  // published storage lease, for FileDevice::OpenServed by this process's own
  // threads. Empty in any other process or once released.
  [[nodiscard]] std::shared_ptr<storage::disk::RouteOwnershipLease> ServedStorageLease() const;
  void release();

 private:
  friend class ServerAgentRuntime;
  // One coordinator per actual route owner. This is native lifetime exclusion,
  // never service authorization or an engine readiness/shutdown receipt.
  class CoordinatorLease {
   public:
    CoordinatorLease(const CoordinatorLease&) = delete;
    CoordinatorLease& operator=(const CoordinatorLease&) = delete;
    CoordinatorLease(CoordinatorLease&& other) noexcept;
    CoordinatorLease& operator=(CoordinatorLease&& other) noexcept;
    ~CoordinatorLease();
   private:
    friend class DatabaseOwnershipLock;
    explicit CoordinatorLease(std::shared_ptr<DatabaseOwnershipLock> owner) noexcept;
    void Reset() noexcept;
    std::shared_ptr<DatabaseOwnershipLock> owner_;
  };
  static std::optional<CoordinatorLease> AcquireCoordinatorLease(
      std::shared_ptr<DatabaseOwnershipLock> owner) noexcept;
  std::atomic<bool> coordinator_active_{false};
  friend DatabaseOwnershipResult AcquireDatabaseOwnership(const DatabaseOwnershipRequest&);
  DatabaseOwnershipLock(NativeHandle handle, NativeHandle storage_handle,
                        std::filesystem::path lock_path);
  bool PublishStorageLease();
#ifdef _WIN32
  std::uint32_t PrepareStorageLease(const std::string& database_path,storage::disk::RouteStorageAccess);
#else
  int PrepareStorageLease(const std::string& database_path,storage::disk::RouteStorageAccess);
#endif
  std::shared_ptr<storage::disk::RouteOwnershipLease> lease_;
  NativeHandle handle_{
#ifdef _WIN32
      nullptr
#else
      -1
#endif
  };
  // Server routing and direct storage must contend on the same storage lock.
  NativeHandle storage_handle_{
#ifdef _WIN32
      nullptr
#else
      -1
#endif
  };
  std::uint64_t owner_pid_ = 0;
  std::filesystem::path lock_path_;
};

struct DatabaseOwnershipRequest {
  std::filesystem::path database_path;
  std::string owner_kind = "server";
  std::filesystem::path sbps_endpoint;
  // Physical capability is retained privately at initial OS ownership only.
  // Read-source transfer still yields no writer or engine admission authority.
  storage::disk::RouteStorageAccess storage_access=storage::disk::RouteStorageAccess::read_only;
};

struct DatabaseOwnershipResult {
  bool acquired = false;
  std::shared_ptr<DatabaseOwnershipLock> lock;
  DatabaseOwnershipDescriptor incumbent;
  std::filesystem::path lock_path;
  std::string diagnostic_code;
  std::string diagnostic_detail;
};

std::filesystem::path DatabaseOwnershipLockPath(const std::filesystem::path& database_path);
DatabaseOwnershipDescriptor ReadDatabaseOwnershipDescriptor(
    const std::filesystem::path& lock_path);
DatabaseOwnershipResult AcquireDatabaseOwnership(const DatabaseOwnershipRequest& request);

}  // namespace scratchbird::server
