// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cloud_filespace_provider.hpp"

#include "metric_producer.hpp"
#include "hash_digest.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string_view>
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

// Filesystem keys are one-way hashes of domain-separated native identities.
// Binary manifests retain the identities; paths are not recovery authority.
std::string CloudIdentityKey(const Uuid& uuid) {
  if (!IsEngineIdentityUuid(uuid)) return {};
  constexpr std::string_view domain = "SB_CLOUD_FILE_KEY_V2";
  std::vector<byte> material(domain.begin(), domain.end());
  material.insert(material.end(), uuid.bytes.begin(), uuid.bytes.end());
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(material);
  return digest.ok() ? scratchbird::core::hash::HexLower(digest.digest) : std::string{};
}
void PutU32(std::string& bytes, u32 value) {
  for (unsigned i = 0; i < 4; ++i) bytes.push_back(static_cast<char>(value >> (i * 8)));
}
void PutU64(std::string& bytes, u64 value) {
  for (unsigned i = 0; i < 8; ++i) bytes.push_back(static_cast<char>(value >> (i * 8)));
}
void PutUuid(std::string& bytes, const Uuid& uuid) {
  bytes.append(reinterpret_cast<const char*>(uuid.bytes.data()), uuid.bytes.size());
}
// Eight-byte versioned magic, LE32 body length, binary body, SHA256 checksum.
// These manifests describe copies and policy; MGA inventory owns finality.
std::string FrameManifest(std::string_view magic, const std::string& body) {
  if (magic.size() != 8 || body.size() > std::numeric_limits<u32>::max()) return {};
  std::string frame(magic);
  PutU32(frame, static_cast<u32>(body.size()));
  frame += body;
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(
      reinterpret_cast<const byte*>(frame.data()), frame.size());
  if (!digest.ok()) return {};
  frame.append(reinterpret_cast<const char*>(digest.digest.data()), digest.digest.size());
  return frame;
}

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
          {"database_uuid", binding.database_uuid.value},
          {"filespace_uuid", binding.filespace_uuid.value},
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

  const auto database_key = CloudIdentityKey(config.database_uuid.value);
  const auto filespace_key = CloudIdentityKey(config.filespace_uuid.value);
  if (database_key.empty() || filespace_key.empty() || binding.provider_name.size() > 65536) {
    return Error("SB-CLOUD-FILESPACE-MANIFEST-ENCODING-FAILED",
                 "storage.cloud_filespace.manifest_encoding_failed", {}, binding);
  }
  std::string body;
  PutUuid(body, config.database_uuid.value);
  PutUuid(body, config.filespace_uuid.value);
  PutU32(body, config.page_size);
  PutU32(body, 1);  // lifecycle checkpoint required; provider-native consistency false
  PutU32(body, static_cast<u32>(binding.provider_name.size()));
  body += binding.provider_name;
  const auto frame = FrameManifest("SBCFM002", body);
  if (frame.empty()) {
    return Error("SB-CLOUD-FILESPACE-MANIFEST-ENCODING-FAILED",
                 "storage.cloud_filespace.manifest_encoding_failed", {}, binding);
  }
  const std::filesystem::path root =
      std::filesystem::path(config.emulator_root) / "databases" /
      database_key / "filespaces" / filespace_key;
  binding.root_path = root.string();
  binding.object_root_path = (root / "objects").string();
  binding.snapshot_root_path = (root / "snapshots").string();
  binding.manifest_path = (root / "cloud_filespace_manifest.sbcf").string();
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
  manifest.write(frame.data(), static_cast<std::streamsize>(frame.size()));
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
  if (!IsEngineIdentityUuid(request.snapshot_uuid) ||
      !IsTypedEngineIdentity(binding.database_uuid, UuidKind::database) ||
      !IsTypedEngineIdentity(binding.filespace_uuid, UuidKind::filespace)) {
    return Error("SB-CLOUD-FILESPACE-SNAPSHOT-UUID-INVALID",
                 "storage.cloud_filespace.snapshot_uuid_invalid",
                 {},
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

  const auto snapshot_key = CloudIdentityKey(request.snapshot_uuid);
  std::string body;
  PutUuid(body, request.snapshot_uuid);
  PutUuid(body, binding.database_uuid.value);
  PutUuid(body, binding.filespace_uuid.value);
  PutU64(body, request.checkpoint_generation);
  PutU64(body, request.transaction_inventory_generation);
  PutU32(body, 31);  // lifecycle, attach/write fences, flush, coordinated consistency
  const auto frame = FrameManifest("SBCSM002", body);
  if (snapshot_key.empty() || frame.empty()) {
    return Error("SB-CLOUD-FILESPACE-SNAPSHOT-MANIFEST-ENCODING-FAILED",
                 "storage.cloud_filespace.snapshot_manifest_encoding_failed", {}, binding);
  }
  const auto snapshot_path = std::filesystem::path(binding.snapshot_root_path) / snapshot_key;
  std::string copy_error;
  if (!CopyTree(binding.object_root_path, snapshot_path / "objects", &copy_error)) {
    return Error("SB-CLOUD-FILESPACE-SNAPSHOT-COPY-FAILED",
                 "storage.cloud_filespace.snapshot_copy_failed",
                 copy_error,
                 binding);
  }

  const auto manifest_path = snapshot_path / "snapshot_manifest.sbcs";
  std::ofstream manifest(manifest_path, std::ios::binary | std::ios::trunc);
  if (!manifest.good()) {
    return Error("SB-CLOUD-FILESPACE-SNAPSHOT-MANIFEST-WRITE-FAILED",
                 "storage.cloud_filespace.snapshot_manifest_write_failed",
                 manifest_path.string(),
                 binding);
  }
  manifest.write(frame.data(), static_cast<std::streamsize>(frame.size()));
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
