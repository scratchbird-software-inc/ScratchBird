// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cloud_filespace_provider.hpp"
#include "metric_registry.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace filespace = scratchbird::storage::filespace;
namespace metrics = scratchbird::core::metrics;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::byte;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

std::uint64_t CurrentUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

TypedUuid MakeUuid(UuidKind kind, std::uint64_t millis) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, millis);
  Require(generated.ok(), "uuid generation failed");
  return generated.value;
}

std::filesystem::path TempDir() {
  const auto path = std::filesystem::temp_directory_path() /
                    ("sb_p2_cloud_filespace_" + std::to_string(CurrentUnixMillis()));
  std::filesystem::create_directories(path);
  return path;
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

}  // namespace

int main() {
  const auto dir = TempDir();
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      std::error_code ignored;
      std::filesystem::remove_all(path, ignored);
    }
  } cleanup{dir};

  const auto now = CurrentUnixMillis();
  const auto database_uuid = MakeUuid(UuidKind::database, now);
  const auto filespace_uuid = MakeUuid(UuidKind::filespace, now + 1);

  filespace::CloudFilespaceProviderConfig external;
  external.kind = filespace::CloudFilespaceProviderKind::external_object_store;
  external.database_uuid = database_uuid;
  external.filespace_uuid = filespace_uuid;
  external.provider_name = "s3-compatible";
  const auto credential_refused = filespace::BindCloudFilespaceProvider(external);
  Require(!credential_refused.ok(), "external provider admitted missing credentials");
  Require(credential_refused.diagnostic.diagnostic_code ==
              "SB-CLOUD-FILESPACE-CREDENTIAL-REQUIRED",
          "external provider credential diagnostic mismatch");
  external.credential_reference = "secret://test/provider";
  const auto adapter_refused = filespace::BindCloudFilespaceProvider(external);
  Require(!adapter_refused.ok(), "external provider admitted without adapter");
  Require(adapter_refused.diagnostic.diagnostic_code ==
              "SB-CLOUD-FILESPACE-ADAPTER-UNAVAILABLE",
          "external provider adapter diagnostic mismatch");

  filespace::CloudFilespaceProviderConfig config;
  config.kind = filespace::CloudFilespaceProviderKind::local_emulator;
  config.database_uuid = database_uuid;
  config.filespace_uuid = filespace_uuid;
  config.provider_name = "p2-local-emulator";
  config.emulator_root = dir.string();
  const auto bound = filespace::BindCloudFilespaceProvider(config);
  Require(bound.ok(), "local cloud filespace emulator bind failed");
  Require(bound.binding.local_emulator, "local emulator binding did not identify emulator mode");
  Require(std::filesystem::exists(bound.binding.manifest_path),
          "local emulator manifest was not persisted");
  Require(Contains(ReadFile(bound.binding.manifest_path),
                   "snapshot_requires_lifecycle_checkpoint=1"),
          "local emulator manifest missing snapshot lifecycle policy");

  const std::vector<byte> page_payload = {'S', 'B', 'P', '2', 1, 2, 3, 4};
  const auto put =
      filespace::PutCloudFilespaceObject(bound.binding, "pages/00000001.sbp", page_payload);
  Require(put.ok(), "local emulator object put failed");
  Require(put.object.bytes == page_payload.size(), "local emulator put byte count mismatch");

  const auto get =
      filespace::GetCloudFilespaceObject(bound.binding, "pages/00000001.sbp");
  Require(get.ok(), "local emulator object get failed");
  Require(get.payload == page_payload, "local emulator object round trip mismatch");
  Require(get.object.content_checksum == put.object.content_checksum,
          "local emulator object checksum mismatch");

  filespace::CloudFilespaceSnapshotRequest snapshot;
  snapshot.binding = bound.binding;
  snapshot.snapshot_uuid = "019e2000-0000-7000-8000-000000000301";
  const auto uncoordinated = filespace::CreateCloudFilespaceSnapshot(snapshot);
  Require(!uncoordinated.ok(), "uncoordinated provider snapshot was admitted");
  Require(uncoordinated.diagnostic.diagnostic_code ==
              "SB-CLOUD-FILESPACE-SNAPSHOT-UNCOORDINATED",
          "uncoordinated snapshot diagnostic mismatch");

  snapshot.lifecycle_coordinated = true;
  snapshot.attach_admission_fenced = true;
  snapshot.write_admission_fenced = true;
  snapshot.dirty_pages_flushed = true;
  snapshot.checkpoint_generation = 42;
  snapshot.transaction_inventory_generation = 43;
  const auto coordinated = filespace::CreateCloudFilespaceSnapshot(snapshot);
  Require(coordinated.ok(), "coordinated local emulator snapshot failed");
  Require(coordinated.snapshot.database_consistent,
          "coordinated local emulator snapshot did not mark database consistency");
  Require(!coordinated.snapshot.provider_native_snapshot_database_consistent,
          "provider-native snapshot was incorrectly marked database consistent");
  Require(std::filesystem::exists(coordinated.snapshot.manifest_path),
          "coordinated snapshot manifest was not persisted");
  const auto manifest = ReadFile(coordinated.snapshot.manifest_path);
  Require(Contains(manifest, "database_consistent=1"),
          "coordinated snapshot manifest missing database consistency evidence");
  Require(Contains(manifest, "provider_native_snapshot_database_consistent=0"),
          "coordinated snapshot manifest missing provider-native warning");

  bool cloud_metric = false;
  for (const auto& value : metrics::DefaultMetricRegistry().SnapshotCurrent()) {
    cloud_metric = cloud_metric || value.family == "sb_cloud_filespace_operation_total";
  }
  Require(cloud_metric, "cloud filespace operation metric was not published");
  return EXIT_SUCCESS;
}
