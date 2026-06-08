// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_DATABASE_OWNERSHIP_LOCK

#pragma once

#include <filesystem>
#include <memory>
#include <string>

namespace scratchbird::server {

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

  DatabaseOwnershipLock(NativeHandle handle, std::filesystem::path lock_path);
  ~DatabaseOwnershipLock();

  DatabaseOwnershipLock(const DatabaseOwnershipLock&) = delete;
  DatabaseOwnershipLock& operator=(const DatabaseOwnershipLock&) = delete;
  DatabaseOwnershipLock(DatabaseOwnershipLock&& other) noexcept;
  DatabaseOwnershipLock& operator=(DatabaseOwnershipLock&& other) noexcept;

  [[nodiscard]] bool valid() const;
  [[nodiscard]] NativeHandle native_handle() const { return handle_; }
#ifndef _WIN32
  [[nodiscard]] int fd() const { return handle_; }
#endif
  [[nodiscard]] const std::filesystem::path& lock_path() const { return lock_path_; }
  void release();

 private:
  NativeHandle handle_{
#ifdef _WIN32
      nullptr
#else
      -1
#endif
  };
  std::filesystem::path lock_path_;
};

struct DatabaseOwnershipRequest {
  std::filesystem::path database_path;
  std::string owner_kind = "server";
  std::filesystem::path sbps_endpoint;
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
