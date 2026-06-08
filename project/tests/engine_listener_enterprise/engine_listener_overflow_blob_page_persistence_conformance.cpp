// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "disk_device.hpp"
#include "overflow_persistence.hpp"
#include "page_manager.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

namespace disk = scratchbird::storage::disk;
namespace page = scratchbird::storage::page;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

constexpr platform::u32 kPageSize = 8192;
constexpr platform::u64 kFirstBlobPage = 128;
constexpr platform::u64 kPageGeneration = 5;
constexpr platform::u64 kLocalTransactionId = 44;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

platform::TypedUuid MakeUuid(platform::UuidKind kind, platform::u64 seed) {
  const auto generated =
      uuid::GenerateEngineIdentityV7(kind, 2000000000000ull + seed);
  if (!generated.ok()) {
    Fail("uuid generation failed");
  }
  return generated.value;
}

std::filesystem::path TempRoot() {
  std::string scope = std::filesystem::current_path().filename().string();
  if (scope.empty()) {
    scope = "default";
  }
#ifdef _WIN32
  const auto pid = static_cast<unsigned long long>(::GetCurrentProcessId());
#else
  const auto pid = static_cast<unsigned long long>(::getpid());
#endif
  auto root = std::filesystem::temp_directory_path() /
              ("scratchbird_overflow_blob_page_conformance_" + scope +
               "_" + std::to_string(pid));
  std::filesystem::create_directories(root);
  return root;
}

void RemoveDeviceArtifacts(const std::filesystem::path& path) {
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  std::filesystem::remove(path.string() + ".sb.owner.lock", ignored);
  std::filesystem::remove(path.string() + ".sb.route.owner.lock", ignored);
}

void RemoveTempRootIfEmpty() {
  std::error_code ignored;
  std::filesystem::remove(TempRoot(), ignored);
}

std::vector<platform::byte> Payload(std::size_t bytes) {
  std::vector<platform::byte> payload;
  payload.reserve(bytes);
  for (std::size_t index = 0; index < bytes; ++index) {
    payload.push_back(static_cast<platform::byte>((index * 29 + 11) & 0xffu));
  }
  return payload;
}

page::OverflowPersistRequest PersistRequest(const std::vector<platform::byte>& payload) {
  page::OverflowPersistRequest request;
  request.row_uuid = MakeUuid(platform::UuidKind::row, 1);
  request.object_uuid = MakeUuid(platform::UuidKind::object, 2);
  request.transaction_uuid = MakeUuid(platform::UuidKind::transaction, 3);
  request.chunk_policy_uuid = MakeUuid(platform::UuidKind::object, 4);
  request.local_transaction_id = kLocalTransactionId;
  request.generation = 3;
  request.value_descriptor = "engine-listener-enterprise.overflow.physical.blob";
  request.payload_bytes = payload;
  request.chunk_size = 1200;
  return request;
}

page::OverflowValueRecord CommittedRecord(
    const std::vector<platform::byte>& payload) {
  page::OverflowLedger ledger;
  const auto request = PersistRequest(payload);
  const auto persisted = page::PersistOverflowValue(&ledger, request);
  if (!persisted.ok()) {
    std::cerr << persisted.diagnostic.diagnostic_code << '\n';
  }
  Require(persisted.ok(), "overflow logical persist failed");

  page::OverflowCommitRequest commit;
  commit.overflow_value_uuid = persisted.overflow_value_uuid;
  commit.transaction_uuid = request.transaction_uuid;
  commit.local_transaction_id = request.local_transaction_id;
  commit.reason = "ELER-016 physical blob page proof";
  const auto committed = page::CommitOverflowValue(&ledger, commit);
  if (!committed.ok()) {
    std::cerr << committed.diagnostic.diagnostic_code << '\n';
  }
  Require(committed.ok(), "overflow logical commit failed");
  return committed.record;
}

page::OverflowBlobPageResult WriteRecordToPath(
    const std::filesystem::path& path,
    const platform::TypedUuid& database_uuid,
    const platform::TypedUuid& filespace_uuid,
    const page::OverflowValueRecord& record) {
  RemoveDeviceArtifacts(path);
  disk::FileDevice device;
  const auto opened = device.Open(path.string(), disk::FileOpenMode::create_new);
  Require(opened.ok(), "blob page device create failed");

  page::OverflowBlobPageWriteRequest write;
  write.device = &device;
  write.database_uuid = database_uuid;
  write.filespace_uuid = filespace_uuid;
  write.record = record;
  write.page_size = kPageSize;
  write.first_page_number = kFirstBlobPage;
  write.page_generation = kPageGeneration;
  const auto written = page::WriteOverflowValueBlobPages(write);
  if (!written.ok()) {
    std::cerr << written.diagnostic.diagnostic_code << '\n';
  }
  Require(written.ok(), "overflow blob page write failed");
  Require(written.page_count == record.chunks.size(),
          "overflow blob page count mismatch");
  Require(device.Close().ok(), "blob page device close after write failed");
  return written;
}

page::OverflowBlobPageResult ReadRecordFromPath(
    const std::filesystem::path& path,
    const platform::TypedUuid& database_uuid,
    const platform::TypedUuid& filespace_uuid,
    const page::OverflowValueRecord& record) {
  disk::FileDevice device;
  const auto opened =
      device.Open(path.string(), disk::FileOpenMode::open_existing_read_only);
  Require(opened.ok(), "blob page device reopen read-only failed");

  page::OverflowBlobPageReadRequest read;
  read.device = &device;
  read.database_uuid = database_uuid;
  read.filespace_uuid = filespace_uuid;
  read.record = record;
  read.page_size = kPageSize;
  const auto result = page::ReadOverflowValueBlobPages(read);
  Require(device.Close().ok(), "blob page device close after read failed");
  return result;
}

void CorruptBlobPageByte(const std::filesystem::path& path,
                         platform::u64 page_number,
                         platform::u64 body_offset,
                         platform::byte value) {
  disk::FileDevice device;
  Require(device.Open(path.string(), disk::FileOpenMode::open_existing).ok(),
          "blob page device reopen write failed");
  const auto offset = page::CheckedPageBodyOffset(
      kPageSize,
      page_number,
      disk::kPageHeaderSerializedBytes + body_offset);
  Require(offset.ok(), "corruption offset calculation failed");
  const auto overwrite = device.WriteAt(offset.offset, &value, 1);
  Require(overwrite.ok(), "corruption write failed");
  Require(device.Sync().ok(), "corruption sync failed");
  Require(device.Close().ok(), "blob page device close after corruption failed");
}

void ProveWriteReadReopenAndLocations() {
  const auto payload = Payload(15000);
  const auto record = CommittedRecord(payload);
  const auto database_uuid = MakeUuid(platform::UuidKind::database, 10);
  const auto filespace_uuid = MakeUuid(platform::UuidKind::filespace, 11);
  const auto path = TempRoot() / "overflow_pages.sbdb";
  const auto written =
      WriteRecordToPath(path, database_uuid, filespace_uuid, record);

  Require(!written.record.chunks.empty(), "located record lost chunks");
  for (const auto& chunk : written.record.chunks) {
    Require(chunk.page_number == kFirstBlobPage + chunk.ordinal,
            "overflow chunk page number was not materialized");
    Require(chunk.page_generation == kPageGeneration,
            "overflow chunk page generation was not materialized");
  }

  const auto read =
      ReadRecordFromPath(path, database_uuid, filespace_uuid, written.record);
  if (!read.ok()) {
    std::cerr << read.diagnostic.diagnostic_code << '\n';
  }
  Require(read.ok(), "overflow blob pages did not read after reopen");
  Require(read.payload_bytes == payload,
          "overflow blob page read did not reconstruct payload");
  Require(read.locations.size() == written.locations.size(),
          "overflow blob page locations did not round trip");
  RemoveDeviceArtifacts(path);
}

void ProveCorruptionRefusal() {
  const auto payload = Payload(6000);
  const auto database_uuid = MakeUuid(platform::UuidKind::database, 20);
  const auto filespace_uuid = MakeUuid(platform::UuidKind::filespace, 21);

  const auto payload_path = TempRoot() / "overflow_corrupt_payload.sbdb";
  const auto payload_record = CommittedRecord(payload);
  const auto payload_written =
      WriteRecordToPath(payload_path, database_uuid, filespace_uuid, payload_record);
  CorruptBlobPageByte(payload_path,
                      payload_written.record.chunks.front().page_number,
                      176 + payload_written.record.content_hash.size(),
                      0x7f);

  const auto payload_read =
      ReadRecordFromPath(payload_path,
                         database_uuid,
                         filespace_uuid,
                         payload_written.record);
  Require(!payload_read.ok(), "corrupted overflow blob payload was admitted");
  RemoveDeviceArtifacts(payload_path);

  const auto metadata_path = TempRoot() / "overflow_corrupt_metadata.sbdb";
  const auto metadata_record = CommittedRecord(payload);
  const auto metadata_written =
      WriteRecordToPath(metadata_path, database_uuid, filespace_uuid, metadata_record);
  CorruptBlobPageByte(metadata_path,
                      metadata_written.record.chunks.front().page_number,
                      16,
                      0xff);

  const auto metadata_read =
      ReadRecordFromPath(metadata_path,
                         database_uuid,
                         filespace_uuid,
                         metadata_written.record);
  Require(!metadata_read.ok(), "corrupted overflow blob metadata was admitted");
  RemoveDeviceArtifacts(metadata_path);
}

void ProveAuthoritativeReclaim() {
  const auto payload = Payload(9000);
  auto record = CommittedRecord(payload);
  const auto database_uuid = MakeUuid(platform::UuidKind::database, 30);
  const auto filespace_uuid = MakeUuid(platform::UuidKind::filespace, 31);
  const auto path = TempRoot() / "overflow_reclaim.sbdb";
  const auto written =
      WriteRecordToPath(path, database_uuid, filespace_uuid, record);
  record = written.record;

  disk::FileDevice refused_device;
  Require(refused_device.Open(path.string(), disk::FileOpenMode::open_existing).ok(),
          "blob page device reopen for refused reclaim failed");
  page::OverflowBlobPageReclaimRequest refused;
  refused.device = &refused_device;
  refused.database_uuid = database_uuid;
  refused.filespace_uuid = filespace_uuid;
  refused.record = record;
  refused.page_size = kPageSize;
  refused.cleanup_horizon_authoritative = false;
  refused.authoritative_cleanup_horizon_local_transaction_id =
      kLocalTransactionId + 100;
  Require(!page::ReclaimOverflowValueBlobPages(refused).ok(),
          "non-authoritative overflow reclaim was admitted");
  Require(refused_device.Close().ok(),
          "blob page device close after refused reclaim failed");

  disk::FileDevice device;
  Require(device.Open(path.string(), disk::FileOpenMode::open_existing).ok(),
          "blob page device reopen for reclaim failed");
  page::OverflowBlobPageReclaimRequest reclaim = refused;
  reclaim.device = &device;
  reclaim.cleanup_horizon_authoritative = true;
  const auto reclaimed = page::ReclaimOverflowValueBlobPages(reclaim);
  if (!reclaimed.ok()) {
    std::cerr << reclaimed.diagnostic.diagnostic_code << '\n';
  }
  Require(reclaimed.ok(), "authoritative overflow reclaim failed");
  Require(reclaimed.page_count == record.chunks.size(),
          "authoritative overflow reclaim page count mismatch");
  Require(reclaimed.record.state == page::OverflowValueState::cleanup_reclaimed,
          "authoritative overflow reclaim did not return reclaimed state");
  Require(reclaimed.record.chunks.empty(),
          "authoritative overflow reclaim did not clear chunk locations");
  Require(device.Close().ok(), "blob page device close after reclaim failed");

  const auto read =
      ReadRecordFromPath(path, database_uuid, filespace_uuid, record);
  Require(!read.ok(), "reclaimed overflow blob pages were still readable");
  RemoveDeviceArtifacts(path);
}

}  // namespace

int main() {
  ProveWriteReadReopenAndLocations();
  ProveCorruptionRefusal();
  ProveAuthoritativeReclaim();
  RemoveTempRootIfEmpty();
  return EXIT_SUCCESS;
}
