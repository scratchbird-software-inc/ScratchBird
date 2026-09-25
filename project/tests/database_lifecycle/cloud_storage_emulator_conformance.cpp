// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cloud_filespace_provider.hpp"
#include "../support/binary_uuid_fixture.hpp"
#include <openssl/sha.h>
#include <algorithm>
#include <array>
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

std::uint64_t ReadLittle(std::string_view bytes, std::size_t offset, std::size_t width) {
  Require(width <= 8 && offset <= bytes.size() && width <= bytes.size() - offset,
          "binary cloud manifest integer bounds");
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < width; ++i)
    value |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[offset + i])) << (i * 8);
  return value;
}
void RequireUuid(std::string_view bytes, std::size_t offset,
                 const scratchbird::core::platform::Uuid& id) {
  Require(offset <= bytes.size() && 16 <= bytes.size() - offset,
          "binary cloud manifest UUID bounds");
  for (std::size_t i = 0; i < 16; ++i)
    Require(static_cast<unsigned char>(bytes[offset + i]) == id.bytes[i],
            "binary cloud manifest UUID bytes mismatch");
}
std::string ManifestBody(const std::filesystem::path& path, std::string_view magic) {
  const auto bytes = ReadFile(path);
  Require(bytes.size() >= 44 && bytes.substr(0, 8) == magic,
          "binary cloud manifest magic or framing missing");
  const auto length = ReadLittle(bytes, 8, 4);
  Require(length == bytes.size() - 44, "binary cloud manifest exact length mismatch");
  std::array<unsigned char, SHA256_DIGEST_LENGTH> checksum{};
  Require(SHA256(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size() - 32,
                 checksum.data()) != nullptr, "independent manifest checksum failed");
  for (std::size_t i = 0; i < 32; ++i)
    Require(static_cast<unsigned char>(bytes[bytes.size() - 32 + i]) == checksum[i],
            "binary cloud manifest checksum mismatch");
  return bytes.substr(12, length);
}
std::string ExpectedFileKey(const scratchbird::core::platform::Uuid& id) {
  std::string material = "SB_CLOUD_FILE_KEY_V2";
  material.append(reinterpret_cast<const char*>(id.bytes.data()), 16);
  std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
  Require(SHA256(reinterpret_cast<const unsigned char*>(material.data()), material.size(),
                 digest.data()) != nullptr, "independent cloud path digest failed");
  std::string key;
  for (auto value : digest) {
    key.push_back("0123456789abcdef"[value >> 4]);
    key.push_back("0123456789abcdef"[value & 15]);
  }
  return key;
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
  config.provider_name = std::string("p2\nprovider\0binary", 18);
  config.emulator_root = dir.string();
  const auto bound = filespace::BindCloudFilespaceProvider(config);
  Require(bound.ok(), "local cloud filespace emulator bind failed");
  Require(bound.binding.local_emulator, "local emulator binding did not identify emulator mode");
  Require(std::filesystem::exists(bound.binding.manifest_path),
          "local emulator manifest was not persisted");
  const auto provider_manifest = ManifestBody(bound.binding.manifest_path, "SBCFM002");
  RequireUuid(provider_manifest, 0, database_uuid.value);
  RequireUuid(provider_manifest, 16, filespace_uuid.value);
  Require(ReadLittle(provider_manifest, 32, 4) == config.page_size &&
              ReadLittle(provider_manifest, 36, 4) == 1,
          "binary provider manifest page size or lifecycle policy mismatch");
  Require(ReadLittle(provider_manifest, 40, 4) == config.provider_name.size() &&
              provider_manifest.substr(44) == config.provider_name,
          "binary provider manifest did not frame embedded name bytes");
  Require(std::filesystem::path(bound.binding.root_path) ==
              dir / "databases" / ExpectedFileKey(database_uuid.value) /
                  "filespaces" / ExpectedFileKey(filespace_uuid.value),
          "cloud provider path did not use native-identity digest keys");

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
  snapshot.snapshot_uuid = scratchbird::tests::FixtureUuid(6301, 1);
  auto invalid_snapshot = snapshot;
  invalid_snapshot.snapshot_uuid = {};
  Require(!filespace::CreateCloudFilespaceSnapshot(invalid_snapshot).ok(),
          "nil snapshot identity was admitted");
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
  const auto manifest = ManifestBody(coordinated.snapshot.manifest_path, "SBCSM002");
  Require(manifest.size() == 68, "binary snapshot manifest body length mismatch");
  RequireUuid(manifest, 0, snapshot.snapshot_uuid);
  RequireUuid(manifest, 16, database_uuid.value);
  RequireUuid(manifest, 32, filespace_uuid.value);
  Require(ReadLittle(manifest, 48, 8) == 42 && ReadLittle(manifest, 56, 8) == 43 &&
              ReadLittle(manifest, 64, 4) == 31,
          "binary snapshot manifest generation or coordination flags mismatch");
  Require(coordinated.snapshot.snapshot_uuid == snapshot.snapshot_uuid &&
              std::filesystem::path(coordinated.snapshot.snapshot_path).filename() ==
                  ExpectedFileKey(snapshot.snapshot_uuid),
          "snapshot native identity or digest key mismatch");

  bool cloud_metric = false;
  for (const auto& value : metrics::DefaultMetricRegistry().SnapshotCurrent()) {
    cloud_metric = cloud_metric || value.family == "sb_cloud_filespace_operation_total";
  }
  Require(cloud_metric, "cloud filespace operation metric was not published");
  return EXIT_SUCCESS;
}
