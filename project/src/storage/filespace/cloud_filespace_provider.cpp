// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cloud_filespace_provider.hpp"

#include "metric_producer.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

namespace scratchbird::storage::filespace {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::uuid::IsEngineIdentityUuid;
using scratchbird::core::uuid::UuidToString;

Status CloudOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::storage_disk};
}

Status CloudErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::storage_disk};
}

bool IsTypedEngineIdentity(const TypedUuid& uuid, UuidKind kind) {
  return uuid.kind == kind && uuid.valid() && IsEngineIdentityUuid(uuid.value);
}

u64 Checksum(const std::vector<byte>& payload) {
  u64 hash = 1469598103934665603ull;
  for (const byte value : payload) {
    hash ^= static_cast<u64>(value);
    hash *= 1099511628211ull;
  }
  return hash;
}

void EmitCloudMetric(const char* operation,
                     const char* result,
                     const char* reason,
                     const CloudFilespaceBinding& binding) {
  (void)scratchbird::core::metrics::IncrementCounter(
      "sb_cloud_filespace_operation_total",
      scratchbird::core::metrics::Labels({
          {"component", "storage.filespace.cloud"},
          {"operation", operation},
          {"result", result},
          {"reason", reason},
          {"database_uuid", UuidToString(binding.database_uuid.value)},
          {"filespace_uuid", UuidToString(binding.filespace_uuid.value)},
          {"provider_family", CloudFilespaceProviderKindName(binding.kind)},
      }),
      1.0,
      "storage_filespace");
}

CloudFilespaceResult Error(std::string code,
                           std::string key,
                           std::string detail = {},
                           const CloudFilespaceBinding& binding = {}) {
  if (binding.database_uuid.valid() && binding.filespace_uuid.valid()) {
    EmitCloudMetric("cloud_filespace", "error", code.c_str(), binding);
  }
  CloudFilespaceResult result;
  result.status = CloudErrorStatus();
  result.diagnostic = MakeCloudFilespaceDiagnostic(result.status,
                                                   std::move(code),
                                                   std::move(key),
                                                   std::move(detail));
  return result;
}

CloudFilespaceResult Ok(CloudFilespaceBinding binding) {
  CloudFilespaceResult result;
  result.status = CloudOkStatus();
  result.binding = std::move(binding);
  result.metric_recorded = true;
  return result;
}

bool SafeObjectKey(const std::string& object_key) {
  if (object_key.empty() || object_key.front() == '/' || object_key.find('\\') != std::string::npos) {
    return false;
  }
  std::filesystem::path parsed(object_key);
  for (const auto& part : parsed) {
    if (part == ".." || part == ".") {
      return false;
    }
  }
  return true;
}

std::filesystem::path ObjectPath(const CloudFilespaceBinding& binding,
                                 const std::string& object_key) {
  return std::filesystem::path(binding.object_root_path) / std::filesystem::path(object_key);
}

bool CopyTree(const std::filesystem::path& source,
              const std::filesystem::path& target,
              std::string* error) {
  std::error_code ec;
  std::filesystem::create_directories(target, ec);
  if (ec) {
    if (error != nullptr) *error = ec.message();
    return false;
  }
  if (!std::filesystem::exists(source, ec)) {
    return true;
  }
  for (const auto& entry : std::filesystem::recursive_directory_iterator(source, ec)) {
    if (ec) {
      if (error != nullptr) *error = ec.message();
      return false;
    }
    const auto relative = std::filesystem::relative(entry.path(), source, ec);
    if (ec) {
      if (error != nullptr) *error = ec.message();
      return false;
    }
    const auto destination = target / relative;
    if (entry.is_directory()) {
      std::filesystem::create_directories(destination, ec);
    } else if (entry.is_regular_file()) {
      std::filesystem::create_directories(destination.parent_path(), ec);
      std::filesystem::copy_file(entry.path(),
                                 destination,
                                 std::filesystem::copy_options::overwrite_existing,
                                 ec);
    }
    if (ec) {
      if (error != nullptr) *error = ec.message();
      return false;
    }
  }
  return true;
}

}  // namespace

const char* CloudFilespaceProviderKindName(CloudFilespaceProviderKind kind) {
  switch (kind) {
    case CloudFilespaceProviderKind::local_emulator:
      return "local_emulator";
    case CloudFilespaceProviderKind::external_object_store:
      return "external_object_store";
  }
  return "unknown";
}

CloudFilespaceResult BindCloudFilespaceProvider(const CloudFilespaceProviderConfig& config) {
  if (!IsTypedEngineIdentity(config.database_uuid, UuidKind::database)) {
    return Error("SB-CLOUD-FILESPACE-DATABASE-UUID-MUST-BE-V7",
                 "storage.cloud_filespace.database_uuid_must_be_v7");
  }
  if (!IsTypedEngineIdentity(config.filespace_uuid, UuidKind::filespace)) {
    return Error("SB-CLOUD-FILESPACE-FILESPACE-UUID-MUST-BE-V7",
                 "storage.cloud_filespace.filespace_uuid_must_be_v7");
  }

  CloudFilespaceBinding binding;
  binding.kind = config.kind;
  binding.database_uuid = config.database_uuid;
  binding.filespace_uuid = config.filespace_uuid;
  binding.provider_name = config.provider_name.empty()
      ? CloudFilespaceProviderKindName(config.kind)
      : config.provider_name;

  if (config.kind == CloudFilespaceProviderKind::external_object_store) {
    binding.local_emulator = false;
    if (config.credential_reference.empty()) {
      return Error("SB-CLOUD-FILESPACE-CREDENTIAL-REQUIRED",
                   "storage.cloud_filespace.credential_required",
                   binding.provider_name,
                   binding);
    }
    return Error("SB-CLOUD-FILESPACE-ADAPTER-UNAVAILABLE",
                 "storage.cloud_filespace.adapter_unavailable",
                 binding.provider_name,
                 binding);
  }

  if (config.emulator_root.empty()) {
    return Error("SB-CLOUD-FILESPACE-EMULATOR-ROOT-REQUIRED",
                 "storage.cloud_filespace.emulator_root_required",
                 {},
                 binding);
  }

  const std::filesystem::path root =
      std::filesystem::path(config.emulator_root) / "databases" /
      UuidToString(config.database_uuid.value) / "filespaces" /
      UuidToString(config.filespace_uuid.value);
  binding.root_path = root.string();
  binding.object_root_path = (root / "objects").string();
  binding.snapshot_root_path = (root / "snapshots").string();
  binding.manifest_path = (root / "cloud_filespace_manifest.txt").string();
  binding.credential_verified = true;
  binding.local_emulator = true;

  std::error_code ec;
  std::filesystem::create_directories(binding.object_root_path, ec);
  if (ec) {
    return Error("SB-CLOUD-FILESPACE-EMULATOR-DIRECTORY-FAILED",
                 "storage.cloud_filespace.emulator_directory_failed",
                 ec.message(),
                 binding);
  }
  std::filesystem::create_directories(binding.snapshot_root_path, ec);
  if (ec) {
    return Error("SB-CLOUD-FILESPACE-SNAPSHOT-DIRECTORY-FAILED",
                 "storage.cloud_filespace.snapshot_directory_failed",
                 ec.message(),
                 binding);
  }

  std::ofstream manifest(binding.manifest_path, std::ios::binary | std::ios::trunc);
  if (!manifest.good()) {
    return Error("SB-CLOUD-FILESPACE-MANIFEST-WRITE-FAILED",
                 "storage.cloud_filespace.manifest_write_failed",
                 binding.manifest_path,
                 binding);
  }
  manifest << "scratchbird.cloud_filespace.local_emulator.v1\n"
           << "database_uuid=" << UuidToString(config.database_uuid.value) << "\n"
           << "filespace_uuid=" << UuidToString(config.filespace_uuid.value) << "\n"
           << "provider_name=" << binding.provider_name << "\n"
           << "page_size=" << config.page_size << "\n"
           << "snapshot_requires_lifecycle_checkpoint=1\n"
           << "provider_native_snapshot_database_consistent=0\n";
  manifest.flush();
  if (!manifest.good()) {
    return Error("SB-CLOUD-FILESPACE-MANIFEST-WRITE-FAILED",
                 "storage.cloud_filespace.manifest_write_failed",
                 binding.manifest_path,
                 binding);
  }

  EmitCloudMetric("bind", "ok", "ok", binding);
  return Ok(std::move(binding));
}

CloudFilespaceResult PutCloudFilespaceObject(const CloudFilespaceBinding& binding,
                                             std::string object_key,
                                             const std::vector<byte>& payload) {
  if (!binding.local_emulator || binding.object_root_path.empty()) {
    return Error("SB-CLOUD-FILESPACE-LOCAL-EMULATOR-REQUIRED",
                 "storage.cloud_filespace.local_emulator_required",
                 {},
                 binding);
  }
  if (!SafeObjectKey(object_key)) {
    return Error("SB-CLOUD-FILESPACE-OBJECT-KEY-INVALID",
                 "storage.cloud_filespace.object_key_invalid",
                 object_key,
                 binding);
  }
  const auto path = ObjectPath(binding, object_key);
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (ec) {
    return Error("SB-CLOUD-FILESPACE-OBJECT-DIRECTORY-FAILED",
                 "storage.cloud_filespace.object_directory_failed",
                 ec.message(),
                 binding);
  }
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!payload.empty()) {
    out.write(reinterpret_cast<const char*>(payload.data()),
              static_cast<std::streamsize>(payload.size()));
  }
  out.flush();
  if (!out.good()) {
    return Error("SB-CLOUD-FILESPACE-OBJECT-WRITE-FAILED",
                 "storage.cloud_filespace.object_write_failed",
                 path.string(),
                 binding);
  }

  CloudFilespaceResult result = Ok(binding);
  result.object.object_key = std::move(object_key);
  result.object.local_path = path.string();
  result.object.bytes = static_cast<u64>(payload.size());
  result.object.content_checksum = Checksum(payload);
  result.object.generation = std::filesystem::file_size(path, ec);
  EmitCloudMetric("put_object", "ok", "ok", binding);
  return result;
}

CloudFilespaceResult GetCloudFilespaceObject(const CloudFilespaceBinding& binding,
                                             std::string object_key) {
  if (!binding.local_emulator || binding.object_root_path.empty()) {
    return Error("SB-CLOUD-FILESPACE-LOCAL-EMULATOR-REQUIRED",
                 "storage.cloud_filespace.local_emulator_required",
                 {},
                 binding);
  }
  if (!SafeObjectKey(object_key)) {
    return Error("SB-CLOUD-FILESPACE-OBJECT-KEY-INVALID",
                 "storage.cloud_filespace.object_key_invalid",
                 object_key,
                 binding);
  }
  const auto path = ObjectPath(binding, object_key);
  std::ifstream in(path, std::ios::binary);
  if (!in.good()) {
    return Error("SB-CLOUD-FILESPACE-OBJECT-NOT-FOUND",
                 "storage.cloud_filespace.object_not_found",
                 path.string(),
                 binding);
  }
  std::vector<byte> payload((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
  CloudFilespaceResult result = Ok(binding);
  result.object.object_key = std::move(object_key);
  result.object.local_path = path.string();
  result.object.bytes = static_cast<u64>(payload.size());
  result.object.content_checksum = Checksum(payload);
  result.payload = std::move(payload);
  EmitCloudMetric("get_object", "ok", "ok", binding);
  return result;
}

CloudFilespaceResult CreateCloudFilespaceSnapshot(const CloudFilespaceSnapshotRequest& request) {
  const CloudFilespaceBinding& binding = request.binding;
  if (!binding.local_emulator || binding.snapshot_root_path.empty()) {
    return Error("SB-CLOUD-FILESPACE-LOCAL-EMULATOR-REQUIRED",
                 "storage.cloud_filespace.local_emulator_required",
                 {},
                 binding);
  }
  if (request.snapshot_uuid.empty() || !SafeObjectKey(request.snapshot_uuid)) {
    return Error("SB-CLOUD-FILESPACE-SNAPSHOT-UUID-INVALID",
                 "storage.cloud_filespace.snapshot_uuid_invalid",
                 request.snapshot_uuid,
                 binding);
  }
  if (!request.lifecycle_coordinated ||
      !request.attach_admission_fenced ||
      !request.write_admission_fenced ||
      !request.dirty_pages_flushed ||
      request.checkpoint_generation == 0 ||
      request.transaction_inventory_generation == 0) {
    return Error("SB-CLOUD-FILESPACE-SNAPSHOT-UNCOORDINATED",
                 "storage.cloud_filespace.snapshot_uncoordinated",
                 "database lifecycle checkpoint and admission fences are required",
                 binding);
  }

  const auto snapshot_path = std::filesystem::path(binding.snapshot_root_path) / request.snapshot_uuid;
  std::string copy_error;
  if (!CopyTree(binding.object_root_path, snapshot_path / "objects", &copy_error)) {
    return Error("SB-CLOUD-FILESPACE-SNAPSHOT-COPY-FAILED",
                 "storage.cloud_filespace.snapshot_copy_failed",
                 copy_error,
                 binding);
  }

  const auto manifest_path = snapshot_path / "snapshot_manifest.txt";
  std::ofstream manifest(manifest_path, std::ios::binary | std::ios::trunc);
  if (!manifest.good()) {
    return Error("SB-CLOUD-FILESPACE-SNAPSHOT-MANIFEST-WRITE-FAILED",
                 "storage.cloud_filespace.snapshot_manifest_write_failed",
                 manifest_path.string(),
                 binding);
  }
  manifest << "scratchbird.cloud_filespace.snapshot.v1\n"
           << "snapshot_uuid=" << request.snapshot_uuid << "\n"
           << "database_uuid=" << UuidToString(binding.database_uuid.value) << "\n"
           << "filespace_uuid=" << UuidToString(binding.filespace_uuid.value) << "\n"
           << "lifecycle_coordinated=1\n"
           << "attach_admission_fenced=1\n"
           << "write_admission_fenced=1\n"
           << "dirty_pages_flushed=1\n"
           << "checkpoint_generation=" << request.checkpoint_generation << "\n"
           << "transaction_inventory_generation=" << request.transaction_inventory_generation << "\n"
           << "provider_native_snapshot_database_consistent=0\n"
           << "database_consistent=1\n";
  manifest.flush();
  if (!manifest.good()) {
    return Error("SB-CLOUD-FILESPACE-SNAPSHOT-MANIFEST-WRITE-FAILED",
                 "storage.cloud_filespace.snapshot_manifest_write_failed",
                 manifest_path.string(),
                 binding);
  }

  CloudFilespaceResult result = Ok(binding);
  result.snapshot.snapshot_uuid = request.snapshot_uuid;
  result.snapshot.snapshot_path = snapshot_path.string();
  result.snapshot.manifest_path = manifest_path.string();
  result.snapshot.database_consistent = true;
  result.snapshot.provider_native_snapshot_database_consistent = false;
  result.snapshot.checkpoint_generation = request.checkpoint_generation;
  result.snapshot.transaction_inventory_generation = request.transaction_inventory_generation;
  EmitCloudMetric("snapshot", "ok", "ok", binding);
  return result;
}

DiagnosticRecord MakeCloudFilespaceDiagnostic(Status status,
                                              std::string diagnostic_code,
                                              std::string message_key,
                                              std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "storage.filespace.cloud_provider");
}

}  // namespace scratchbird::storage::filespace
