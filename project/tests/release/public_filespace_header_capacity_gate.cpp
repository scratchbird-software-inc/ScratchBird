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

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>

namespace {

namespace filespace = scratchbird::storage::filespace;
namespace uuid = scratchbird::core::uuid;
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

int main() {
  HeaderWriteExtendsAndReadValidatesCapacity();
  HeaderRejectsOverflowAndInvalidWindows();
  HeaderReadRejectsFileSizeMismatch();
  RegistryRoundTripsCapacityAndWriterIdentity();
  AttachComparesHeaderRegistryCapacity();
  return EXIT_SUCCESS;
}
