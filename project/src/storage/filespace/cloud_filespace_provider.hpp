// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-CLOUD-FILESPACE-PROVIDER-ANCHOR
#include "runtime_platform.hpp"

#include <string>
#include <vector>

namespace scratchbird::storage::filespace {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

enum class CloudFilespaceProviderKind : u32 {
  local_emulator,
  external_object_store
};

struct CloudFilespaceProviderConfig {
  CloudFilespaceProviderKind kind = CloudFilespaceProviderKind::local_emulator;
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  std::string provider_name = "local_emulator";
  std::string emulator_root;
  std::string credential_reference;
  u32 page_size = 16384;
};

struct CloudFilespaceBinding {
  CloudFilespaceProviderKind kind = CloudFilespaceProviderKind::local_emulator;
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  std::string provider_name;
  std::string root_path;
  std::string object_root_path;
  std::string snapshot_root_path;
  std::string manifest_path;
  bool credential_verified = false;
  bool local_emulator = false;
  bool snapshot_requires_lifecycle_checkpoint = true;
};

struct CloudFilespaceObjectRef {
  std::string object_key;
  std::string local_path;
  u64 bytes = 0;
  u64 content_checksum = 0;
  u64 generation = 0;
};

struct CloudFilespaceSnapshotRequest {
  CloudFilespaceBinding binding;
  std::string snapshot_uuid;
  bool lifecycle_coordinated = false;
  bool attach_admission_fenced = false;
  bool write_admission_fenced = false;
  bool dirty_pages_flushed = false;
  u64 checkpoint_generation = 0;
  u64 transaction_inventory_generation = 0;
};

struct CloudFilespaceSnapshot {
  std::string snapshot_uuid;
  std::string snapshot_path;
  std::string manifest_path;
  bool database_consistent = false;
  bool provider_native_snapshot_database_consistent = false;
  u64 checkpoint_generation = 0;
  u64 transaction_inventory_generation = 0;
};

struct CloudFilespaceResult {
  Status status;
  DiagnosticRecord diagnostic;
  CloudFilespaceBinding binding;
  CloudFilespaceObjectRef object;
  CloudFilespaceSnapshot snapshot;
  std::vector<byte> payload;
  bool metric_recorded = false;

  bool ok() const { return status.ok(); }
};

const char* CloudFilespaceProviderKindName(CloudFilespaceProviderKind kind);
CloudFilespaceResult BindCloudFilespaceProvider(const CloudFilespaceProviderConfig& config);
CloudFilespaceResult PutCloudFilespaceObject(const CloudFilespaceBinding& binding,
                                             std::string object_key,
                                             const std::vector<byte>& payload);
CloudFilespaceResult GetCloudFilespaceObject(const CloudFilespaceBinding& binding,
                                             std::string object_key);
CloudFilespaceResult CreateCloudFilespaceSnapshot(const CloudFilespaceSnapshotRequest& request);
DiagnosticRecord MakeCloudFilespaceDiagnostic(Status status,
                                              std::string diagnostic_code,
                                              std::string message_key,
                                              std::string detail = {});

}  // namespace scratchbird::storage::filespace
