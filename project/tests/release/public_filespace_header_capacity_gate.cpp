// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "filespace_header.hpp"
#include "filespace_lifecycle.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <vector>
#if defined(__linux__)
#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

namespace filespace = scratchbird::storage::filespace;
namespace uuid = scratchbird::core::uuid;
#if defined(__linux__)
bool native_faults_active = false, fail_header_write = false, fail_header_sync = false;
bool fail_verification_read = false, mismatch_verification_size = false;
unsigned sync_count = 0;
int native_data_fd = -1;
bool replace_after_read = false;
std::filesystem::path replacement_source, replacement_target, retained_target;
#endif
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

TypedUuid MakeUuid(UuidKind kind, std::uint64_t salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, 1830000000000ull + salt);
  Require(generated.ok(), "uuid generation failed");
  return generated.value;
}

std::filesystem::path TempPath(std::string_view name) {
  const auto dir = std::filesystem::temp_directory_path() /
                   "sb_public_filespace_header_capacity_gate";
  std::filesystem::create_directories(dir);
  return dir / std::string(name);
}

void RemoveIfPresent(const std::filesystem::path& path) {
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  std::filesystem::remove(path.string() + ".sb.owner.lock", ignored);
}

filespace::PhysicalFilespaceHeader MakeHeader() {
  filespace::PhysicalFilespaceHeader header;
  header.database_uuid = MakeUuid(UuidKind::database, 1);
  header.filespace_uuid = MakeUuid(UuidKind::filespace, 2);
  header.role = filespace::FilespaceRole::secondary_data;
  header.state = filespace::FilespaceState::online;
  header.page_size = 8192;
  header.format_version = 1;
  header.checksum_profile = 1;
  header.encryption_profile = 0;
  header.physical_filespace_id = 2;
  header.total_pages = 3;
  header.free_pages = 1;
  header.preallocated_pages = 1;
  header.allocation_root_page = 1;
  header.header_generation = 7;
  header.writer_identity_uuid = MakeUuid(UuidKind::object, 3);
  header.creation_operation_uuid = "public-filespace-header-capacity";
  return header;
}

void HeaderWriteExtendsAndReadValidatesCapacity() {
  const auto path = TempPath("valid.sbfs");
  RemoveIfPresent(path);
  const auto header = MakeHeader();

  const auto written = filespace::WritePhysicalFilespaceHeader(path.string(), header, false);
  if (!written.ok()) {
    std::cerr << written.diagnostic.diagnostic_code << '\n';
  }
  Require(written.ok(), "valid filespace header write failed");
  Require(std::filesystem::file_size(path) == header.total_pages * header.page_size,
          "filespace file was not extended to declared capacity");

  const auto read = filespace::ReadPhysicalFilespaceHeader(path.string());
  if (!read.ok()) {
    std::cerr << read.diagnostic.diagnostic_code << '\n';
  }
  Require(read.ok(), "valid filespace header read failed");
  Require(read.file_size_bytes == header.total_pages * header.page_size,
          "read result did not report file size");
  Require(read.expected_capacity_bytes == header.total_pages * header.page_size,
          "read result did not report expected capacity");
  Require(read.file_size_matches_capacity,
          "read result did not compare file size to header capacity");
  Require(read.header.header_generation == header.header_generation,
          "header generation did not round trip");
  Require(read.header.writer_identity_uuid.value == header.writer_identity_uuid.value,
          "writer identity did not round trip");

  const auto validated = filespace::ValidatePhysicalFilespaceHeader(header,
                                                                    read.header,
                                                                    read.file_size_bytes);
  Require(validated.ok(), "explicit header validator rejected valid capacity");
}

void HeaderRejectsOverflowAndInvalidWindows() {
  const auto path = TempPath("invalid.sbfs");
  RemoveIfPresent(path);

  auto overflow = MakeHeader();
  overflow.total_pages = std::numeric_limits<std::uint64_t>::max() / overflow.page_size + 1;
  const auto overflow_write = filespace::WritePhysicalFilespaceHeader(path.string(), overflow, false);
  Require(!overflow_write.ok(), "capacity overflow header was accepted");
  Require(overflow_write.diagnostic.diagnostic_code == "SB-FILESPACE-HEADER-CAPACITY-OVERFLOW",
          "capacity overflow diagnostic mismatch");

  auto invalid_window = MakeHeader();
  invalid_window.free_pages = invalid_window.total_pages;
  invalid_window.preallocated_pages = 1;
  const auto invalid_write = filespace::WritePhysicalFilespaceHeader(path.string(), invalid_window, false);
  Require(!invalid_write.ok(), "invalid capacity window was accepted");
  Require(invalid_write.diagnostic.diagnostic_code == "SB-FILESPACE-HEADER-CAPACITY-WINDOW-INVALID",
          "capacity window diagnostic mismatch");

  auto invalid_writer = MakeHeader();
  invalid_writer.writer_identity_uuid = {};
  const auto writer_write = filespace::WritePhysicalFilespaceHeader(path.string(), invalid_writer, false);
  Require(!writer_write.ok(), "missing writer identity was accepted");
  Require(writer_write.diagnostic.diagnostic_code == "SB-FILESPACE-HEADER-WRITER-UUID-INVALID",
          "writer identity diagnostic mismatch");
}

void HeaderReadRejectsFileSizeMismatch() {
  const auto path = TempPath("mismatch.sbfs");
  RemoveIfPresent(path);
  const auto header = MakeHeader();
  const auto written = filespace::WritePhysicalFilespaceHeader(path.string(), header, false);
  Require(written.ok(), "header write for mismatch test failed");

  std::filesystem::resize_file(path, header.page_size);
  const auto read = filespace::ReadPhysicalFilespaceHeader(path.string());
  Require(!read.ok(), "file-size/header capacity mismatch was accepted");
  Require(read.diagnostic.diagnostic_code == "SB-FILESPACE-HEADER-FILE-SIZE-CAPACITY-MISMATCH",
          "file-size mismatch diagnostic mismatch");
}

std::vector<unsigned char> FileBytes(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  Require(file.is_open(), "independent bytes open");
  return {std::istreambuf_iterator<char>(file), {}};
}

void HeaderMaintenancePreservesBytes() {
  for (const auto size : {8192u, 16384u, 32768u, 65536u, 131072u}) {
    const auto path = TempPath("maintenance-" + std::to_string(size) + ".sbfs");
    RemoveIfPresent(path);
    auto header = MakeHeader();
    header.page_size = size;
    Require(filespace::WritePhysicalFilespaceHeader(path.string(), header, false).ok(),
            "maintenance fixture create");
    // The prototype's defined metadata region is 256 bytes, not the whole
    // first page. Fill every other byte, including the last one, independently.
    {
      std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
      Require(file.is_open(), "maintenance fixture open");
      file.seekp(256);
      for (std::uint64_t n = 256; n < header.total_pages * size; ++n)
        file.put(static_cast<char>((n % 251) + 1));
      Require(file.good(), "maintenance fixture fill");
    }
    const auto before = FileBytes(path);
    header.state = filespace::FilespaceState::read_only;
    ++header.header_generation;
    for (int retry = 0; retry != 2; ++retry) {
      const auto updated = filespace::WritePhysicalFilespaceHeader(path.string(), header, true);
      Require(updated.ok(), "header-only maintenance failed");
      const auto after = FileBytes(path);
      Require(after.size() == before.size(), "header-only maintenance changed capacity");
      Require(std::equal(before.begin() + 256, before.end(), after.begin() + 256),
              "header-only maintenance destroyed page contents");
      const auto read = filespace::ReadPhysicalFilespaceHeader(path.string());
      Require(read.ok() && read.header.state == header.state &&
              read.header.header_generation == header.header_generation,
              "maintenance metadata did not survive reopen");
    }
    const auto stable = FileBytes(path);
    const auto replay = filespace::CreatePhysicalFilespaceFile(path.string(), header, true);
    Require(!replay.ok() && FileBytes(path) == stable,
            "filespace create replay overwrote an existing file");
    for (const auto pages : {header.total_pages - 1, header.total_pages + 1}) {
      auto mismatch = header;
      mismatch.total_pages = pages;
      mismatch.free_pages = mismatch.preallocated_pages = 0;
      const auto refused = filespace::WritePhysicalFilespaceHeader(path.string(), mismatch, true);
      Require(!refused.ok() && refused.diagnostic.diagnostic_code ==
              "SB-FILESPACE-HEADER-FILE-SIZE-CAPACITY-MISMATCH",
              "header-only maintenance accepted a capacity transition");
      Require(FileBytes(path) == stable, "capacity refusal changed bytes");
    }
    const auto grown = filespace::ExtendPhysicalFilespaceCapacity(
        path.string(), header.database_uuid, header.filespace_uuid, header.page_size,
        header.total_pages, header.preallocated_pages, 1, true);
    Require(grown.ok() && grown.header_updated && grown.physical_extension_synced,
            "real filespace capacity growth failed");
    const auto extended = FileBytes(path);
    Require(extended.size() == stable.size() + size &&
            std::equal(stable.begin() + 256, stable.end(), extended.begin() + 256),
            "capacity growth destroyed existing page contents");
    Require(std::all_of(extended.begin() + stable.size(), extended.end(),
                        [](unsigned char value) { return value == 0; }),
            "capacity growth exposed nonzero uninitialized bytes");
    RemoveIfPresent(path);
    const auto missing = filespace::WritePhysicalFilespaceHeader(path.string(), header, true);
    Require(!missing.ok() && !std::filesystem::exists(path),
            "maintenance recreated a missing filespace");
    RemoveIfPresent(path);
  }
}

#if defined(__linux__)
void GrowthKeepsRetainedFileAndAppliedEffects() {
  const auto path = TempPath("retained-growth.sbfs");
  const auto other = TempPath("replacement-growth.sbfs");
  const auto retained = TempPath("retained-original.sbfs");
  for (const auto& file : {path, other, retained}) RemoveIfPresent(file);
  const auto header = MakeHeader();
  auto replacement = header;
  replacement.filespace_uuid = MakeUuid(UuidKind::filespace, 991);
  Require(filespace::WritePhysicalFilespaceHeader(path.string(), header, false).ok() &&
          filespace::WritePhysicalFilespaceHeader(other.string(), replacement, false).ok(),
          "retained-growth fixtures");
  const auto replacement_bytes = FileBytes(other);
  replacement_source = other;
  replacement_target = path;
  retained_target = retained;
  replace_after_read = true;
  native_data_fd = -1;
  native_faults_active = true;
  const auto grown = filespace::ExtendPhysicalFilespaceCapacity(
      path.string(), header.database_uuid, header.filespace_uuid, header.page_size,
      header.total_pages, header.preallocated_pages, 1, true);
  native_faults_active = false;
  Require(!replace_after_read, "replacement boundary was not exercised");
  Require(grown.ok() && grown.header_before.filespace_uuid.value == header.filespace_uuid.value,
          "growth lost original retained file");
  Require(FileBytes(path) == replacement_bytes, "growth mutated replacement pathname target");
  const auto actual = filespace::ReadPhysicalFilespaceHeader(retained.string());
  Require(actual.ok() && actual.header.filespace_uuid.value == header.filespace_uuid.value &&
          actual.header.total_pages == header.total_pages + 1,
          "original retained file not grown/verified");
  for (const auto& file : {path, other, retained}) RemoveIfPresent(file);

  for (bool mismatch_size : {true, false}) {
    Require(filespace::WritePhysicalFilespaceHeader(path.string(), header, false).ok(),
            "post-effect failure fixture");
    sync_count = 0;
    native_data_fd = -1;
    mismatch_verification_size = mismatch_size;
    fail_verification_read = !mismatch_size;
    native_faults_active = true;
    const auto failed = filespace::ExtendPhysicalFilespaceCapacity(
        path.string(), header.database_uuid, header.filespace_uuid, header.page_size,
        header.total_pages, header.preallocated_pages, 1, true);
    native_faults_active = false;
    mismatch_verification_size = fail_verification_read = false;
    Require(!failed.ok() && failed.physical_extension_completed &&
            failed.physical_extension_synced && failed.header_updated,
            "post-effect verification failure erased applied-effect evidence");
    Require(failed.diagnostic.diagnostic_code == (mismatch_size ?
            "SB-FILESPACE-HEADER-GROWTH-SIZE-MISMATCH" : "SB-STORAGE-DISK-READ-SHORT"),
            "post-effect verification failure diagnostic changed");
    const auto applied = filespace::ReadPhysicalFilespaceHeader(path.string());
    Require(applied.ok() && applied.header.total_pages == header.total_pages + 1,
            "negative observation did not retain actual applied growth");
    RemoveIfPresent(path);
  }
}

void HeaderMaintenanceNativeFailuresPreservePayload() {
  const auto path = TempPath("maintenance-fault.sbfs");
  RemoveIfPresent(path);
  auto header = MakeHeader();
  Require(filespace::WritePhysicalFilespaceHeader(path.string(), header, false).ok(), "fault fixture");
  const auto before = FileBytes(path);
  header.state = filespace::FilespaceState::read_only;
  ++header.header_generation;
  for (bool writing : {true, false}) {
    fail_header_write = writing;
    fail_header_sync = !writing;
    native_data_fd = -1;
    native_faults_active = true;
    const auto failed = filespace::WritePhysicalFilespaceHeader(path.string(), header, true);
    native_faults_active = false;
    fail_header_write = fail_header_sync = false;
    if (failed.ok() || failed.diagnostic.diagnostic_code != (writing ?
        "SB-STORAGE-DISK-WRITE-FAILED" : "SB-STORAGE-DISK-SYNC-FAILED"))
      std::cerr << "maintenance native diagnostic: " << failed.diagnostic.diagnostic_code << '\n';
    Require(!failed.ok() && failed.diagnostic.diagnostic_code == (writing ?
            "SB-STORAGE-DISK-WRITE-FAILED" : "SB-STORAGE-DISK-SYNC-FAILED"),
            "maintenance native failure did not propagate");
    const auto actual = FileBytes(path);
    Require(actual.size() == before.size() &&
            std::equal(before.begin() + 256, before.end(), actual.begin() + 256),
            "maintenance failure destroyed existing payload");
    if (writing) Require(actual == before, "failed first write changed bytes");
  }
  RemoveIfPresent(path);
}
#endif

void RegistryRoundTripsCapacityAndWriterIdentity() {
  filespace::FilespaceRegistry registry;
  filespace::FilespaceDescriptor descriptor;
  const auto header = MakeHeader();
  descriptor.database_uuid = header.database_uuid;
  descriptor.filespace_uuid = header.filespace_uuid;
  descriptor.path = "relative/public-filespace.sbfs";
  descriptor.role = header.role;
  descriptor.state = filespace::FilespaceState::attached;
  descriptor.page_size = header.page_size;
  descriptor.generation = 11;
  descriptor.active = true;
  descriptor.physical_filespace_id = header.physical_filespace_id;
  descriptor.total_pages = header.total_pages;
  descriptor.free_pages = header.free_pages;
  descriptor.preallocated_pages = header.preallocated_pages;
  descriptor.allocation_root_page = header.allocation_root_page;
  descriptor.header_generation = header.header_generation;
  descriptor.writer_identity_uuid = header.writer_identity_uuid;
  registry.filespaces.push_back(descriptor);

  const auto serialized = filespace::SerializeFilespaceRegistry(registry);
  Require(serialized.ok(), "filespace registry serialization failed");
  Require(serialized.payload.find("scratchbird.filespace.registry.v2") != std::string::npos,
          "registry did not use durable capacity format");

  const auto parsed = filespace::ParseFilespaceRegistry(serialized.payload);
  if (!parsed.ok()) {
    std::cerr << parsed.diagnostic.diagnostic_code << '\n';
  }
  Require(parsed.ok(), "filespace registry parse failed");
  Require(parsed.registry.filespaces.size() == 1, "registry parse row count mismatch");
  const auto& round_trip = parsed.registry.filespaces.front();
  Require(round_trip.physical_filespace_id == descriptor.physical_filespace_id,
          "registry physical filespace id did not round trip");
  Require(round_trip.total_pages == descriptor.total_pages,
          "registry total pages did not round trip");
  Require(round_trip.free_pages == descriptor.free_pages,
          "registry free pages did not round trip");
  Require(round_trip.preallocated_pages == descriptor.preallocated_pages,
          "registry preallocated pages did not round trip");
  Require(round_trip.allocation_root_page == descriptor.allocation_root_page,
          "registry allocation root did not round trip");
  Require(round_trip.header_generation == descriptor.header_generation,
          "registry header generation did not round trip");
  Require(round_trip.writer_identity_uuid.value == descriptor.writer_identity_uuid.value,
          "registry writer identity did not round trip");
}

void AttachComparesHeaderRegistryCapacity() {
  const auto path = TempPath("attach.sbfs");
  RemoveIfPresent(path);
  const auto header = MakeHeader();
  const auto written = filespace::WritePhysicalFilespaceHeader(path.string(), header, false);
  Require(written.ok(), "attach header write failed");

  filespace::FilespaceOperationRequest attach;
  attach.operation = filespace::FilespaceOperation::attach_filespace;
  attach.database_uuid = header.database_uuid;
  attach.filespace_uuid = header.filespace_uuid;
  attach.path = path.string();
  attach.role = header.role;
  attach.page_size = header.page_size;
  attach.physical_filespace_id = header.physical_filespace_id;
  attach.total_pages = header.total_pages;
  attach.free_pages = header.free_pages;
  attach.preallocated_pages = header.preallocated_pages;
  attach.allocation_root_page = header.allocation_root_page;
  attach.header_generation = header.header_generation;
  attach.writer_identity_uuid = header.writer_identity_uuid;
  attach.policy.require_physical_header_for_attach = true;

  filespace::FilespaceRegistry registry;
  const auto attached = filespace::ApplyFilespaceOperation(&registry, attach);
  if (!attached.ok()) {
    std::cerr << attached.diagnostic.diagnostic_code << '\n';
  }
  Require(attached.ok(), "matching header/registry capacity attach failed");
  Require(attached.descriptor.total_pages == header.total_pages,
          "attached descriptor did not adopt header capacity");

  filespace::FilespaceRegistry mismatch_registry;
  auto mismatch = attach;
  mismatch.total_pages = header.total_pages - 1;
  const auto refused = filespace::ApplyFilespaceOperation(&mismatch_registry, mismatch);
  Require(!refused.ok(), "mismatched registry/header capacity attach was accepted");
  Require(refused.diagnostic.diagnostic_code ==
              "SB-FILESPACE-LIFECYCLE-ATTACH-PHYSICAL-HEADER-MISMATCH",
          "attach mismatch diagnostic mismatch");
}

}  // namespace

#if defined(__linux__)
extern "C" ssize_t __real_pread(int, void*, size_t, off_t);
extern "C" ssize_t __wrap_pread(int fd, void* bytes, size_t size, off_t offset) {
  if (native_faults_active && offset == 0 && size == 256) native_data_fd = fd;
  if (native_faults_active && fail_verification_read && sync_count >= 2) {
    errno = EIO; return -1;
  }
  const auto result = __real_pread(fd, bytes, size, offset);
  if (native_faults_active && replace_after_read && offset == 0 && size == 256 && result == 256) {
    replace_after_read = false;
    std::error_code error;
    std::filesystem::rename(replacement_target, retained_target, error);
    if (!error) std::filesystem::rename(replacement_source, replacement_target, error);
    if (error) { errno = EIO; return -1; }
  }
  return result;
}
extern "C" int __real_fstat(int, struct stat*);
extern "C" int __wrap_fstat(int fd, struct stat* state) {
  const int result = __real_fstat(fd, state);
  if (!result && native_faults_active && mismatch_verification_size && sync_count >= 2)
    --state->st_size; // Negative consistency observation only; real bytes remain.
  return result;
}
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd) {
  if (native_faults_active && fd == native_data_fd) {
    if (fail_header_sync) { errno = EIO; return -1; }
    ++sync_count;
  }
  return __real_fsync(fd);
}
extern "C" ssize_t __real_pwrite(int, const void*, size_t, off_t);
extern "C" ssize_t __wrap_pwrite(int fd, const void* bytes, size_t size, off_t offset) {
  if (native_faults_active && offset == 0 && size == 256) native_data_fd = fd;
  if (native_faults_active && fail_header_write) { errno = EIO; return -1; }
  return __real_pwrite(fd, bytes, size, offset);
}
#endif

int main() {
  HeaderMaintenancePreservesBytes();
#if defined(__linux__)
  GrowthKeepsRetainedFileAndAppliedEffects();
  HeaderMaintenanceNativeFailuresPreservePayload();
#endif
  HeaderWriteExtendsAndReadValidatesCapacity();
  HeaderRejectsOverflowAndInvalidWindows();
  HeaderReadRejectsFileSizeMismatch();
  RegistryRoundTripsCapacityAndWriterIdentity();
  AttachComparesHeaderRegistryCapacity();
  return EXIT_SUCCESS;
}
