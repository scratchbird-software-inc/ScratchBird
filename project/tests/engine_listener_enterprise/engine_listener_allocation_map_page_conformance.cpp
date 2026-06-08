// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "allocation_map_page.hpp"
#include "page_body_integrity.hpp"
#include "page_header.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace disk = scratchbird::storage::disk;
namespace page = scratchbird::storage::page;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

constexpr platform::u32 kPageSize = 8192;

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
      uuid::GenerateEngineIdentityV7(kind, 1990000000000ull + seed);
  if (!generated.ok()) {
    Fail("uuid generation failed");
  }
  return generated.value;
}

disk::SerializedPageHeader HeaderFor(disk::PageType page_type,
                                     const page::AllocationMapPageBody& body,
                                     platform::u64 page_number) {
  disk::PageHeader header;
  header.page_size = kPageSize;
  header.page_type = page_type;
  header.database_uuid = body.database_uuid.value;
  header.filespace_uuid = body.filespace_uuid.value;
  header.page_uuid = MakeUuid(platform::UuidKind::page, 5000 + page_number).value;
  header.page_number = page_number;
  header.page_generation = body.map_generation;
  const auto serialized = disk::SerializePageHeader(header);
  if (!serialized.ok()) {
    std::cerr << serialized.diagnostic.diagnostic_code << '\n';
  }
  Require(serialized.ok(), "page header serialization failed");
  return serialized.serialized;
}

page::AllocationMapExtent Extent(platform::u64 start_page,
                                 platform::u64 page_count,
                                 page::PageAllocationLifecycleState state,
                                 disk::PageType page_type,
                                 page::PageFamily page_family,
                                 platform::u64 seed) {
  page::AllocationMapExtent extent;
  extent.start_page = start_page;
  extent.page_count = page_count;
  extent.state = state;
  extent.page_type = page_type;
  extent.page_family = page_family;
  extent.page_generation =
      state == page::PageAllocationLifecycleState::allocated ? seed + 1 : seed;
  if (state != page::PageAllocationLifecycleState::free &&
      state != page::PageAllocationLifecycleState::quarantined) {
    extent.allocation_uuid = MakeUuid(platform::UuidKind::object, 100 + seed);
    extent.owner_object_uuid = MakeUuid(platform::UuidKind::object, 200 + seed);
    extent.creator_transaction_uuid =
        MakeUuid(platform::UuidKind::transaction, 300 + seed);
  }
  if (state == page::PageAllocationLifecycleState::reusable_pending_mga) {
    extent.reusable_after_local_transaction_id = 400 + seed;
  }
  return extent;
}

page::AllocationMapPageBody FixtureBody() {
  page::AllocationMapPageBody body;
  body.database_uuid = MakeUuid(platform::UuidKind::database, 1);
  body.filespace_uuid = MakeUuid(platform::UuidKind::filespace, 2);
  body.file_member_uuid = MakeUuid(platform::UuidKind::object, 3);
  body.allocation_map_page_number = 42;
  body.map_generation = 7;
  body.capacity_generation = 9;
  body.filespace_start_page = 1;
  body.total_pages = 128;
  body.extents = {
      Extent(1,
             1,
             page::PageAllocationLifecycleState::allocated,
             disk::PageType::allocation_map,
             page::PageFamily::allocation,
             10),
      Extent(2,
             8,
             page::PageAllocationLifecycleState::allocated,
             disk::PageType::row_data,
             page::PageFamily::data,
             20),
      Extent(10,
             6,
             page::PageAllocationLifecycleState::free,
             disk::PageType::unknown,
             page::PageFamily::unknown,
             0),
      Extent(16,
             4,
             page::PageAllocationLifecycleState::preallocated,
             disk::PageType::row_data,
             page::PageFamily::data,
             30),
      Extent(20,
             4,
             page::PageAllocationLifecycleState::reusable_pending_mga,
             disk::PageType::row_data,
             page::PageFamily::data,
             40),
      Extent(24,
             1,
             page::PageAllocationLifecycleState::quarantined,
             disk::PageType::unknown,
             page::PageFamily::unknown,
             0),
      Extent(25,
             104,
             page::PageAllocationLifecycleState::free,
             disk::PageType::unknown,
             page::PageFamily::unknown,
             0),
  };
  return body;
}

std::vector<platform::byte> ReadFileBytes(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  Require(in.good(), "failed to open persisted allocation map body");
  std::vector<char> chars((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char>());
  return std::vector<platform::byte>(chars.begin(), chars.end());
}

void ProveBuildParseAgreementAndReopen() {
  const page::AllocationMapPageBody body = FixtureBody();
  const auto built = page::BuildAllocationMapPageBody(body, kPageSize);
  if (!built.ok()) {
    std::cerr << built.diagnostic.diagnostic_code << '\n';
  }
  Require(built.ok(), "allocation map page body build failed");
  Require(built.validation.counts.free_pages == 110,
          "free page count was not materialized");
  Require(built.validation.counts.reserved_pages == 4,
          "reserved page count was not materialized");
  Require(built.validation.counts.allocated_pages == 9,
          "allocated page count was not materialized");
  Require(built.validation.counts.reusable_pending_mga_pages == 4,
          "pending MGA page count was not materialized");
  Require(built.validation.counts.quarantined_pages == 1,
          "quarantined page count was not materialized");

  const auto parsed = page::ParseAllocationMapPageBody(built.serialized);
  Require(parsed.ok(), "allocation map page body parse failed");
  Require(parsed.body.capacity_generation == body.capacity_generation,
          "capacity generation did not round trip");
  Require(parsed.body.extents.size() == 7,
          "extent inventory did not round trip");

  page::PageBodyAgreementRequest agreement;
  agreement.header =
      HeaderFor(disk::PageType::allocation_map,
                parsed.body,
                parsed.body.allocation_map_page_number);
  agreement.body = built.serialized;
  agreement.checksum_profile = page::PageBodyChecksumProfile::strong;
  const auto agreed = page::ValidatePageBodyAgreement(agreement);
  if (!agreed.ok()) {
    std::cerr << agreed.diagnostic.diagnostic_code << '\n';
  }
  Require(agreed.ok(), "allocation map page body agreement failed");
  Require(agreed.body_kind == page::PageBodyKind::allocation_map,
          "allocation map body kind was not detected");
  Require(agreed.production_admitted && agreed.production_mutating,
          "allocation map page was not admitted as local mutating body");

  const std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      "scratchbird_allocation_map_page_conformance.sbalm";
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    Require(out.good(), "failed to create persisted allocation map body");
    out.write(reinterpret_cast<const char*>(built.serialized.data()),
              static_cast<std::streamsize>(built.serialized.size()));
    Require(out.good(), "failed to write persisted allocation map body");
  }
  const auto reopened = page::ParseAllocationMapPageBody(ReadFileBytes(path));
  std::filesystem::remove(path);
  Require(reopened.ok(), "persisted allocation map body did not reopen");
  Require(reopened.body.map_generation == parsed.body.map_generation,
          "reopened map generation drifted");
}

void ProveMutation() {
  const auto built = page::BuildAllocationMapPageBody(FixtureBody(), kPageSize);
  Require(built.ok(), "fixture build failed before mutation");

  page::AllocationMapPageBodyMutation mutation;
  mutation.extent = Extent(12,
                           2,
                           page::PageAllocationLifecycleState::allocated,
                           disk::PageType::row_data,
                           page::PageFamily::data,
                           70);
  const auto mutated =
      page::ApplyAllocationMapPageBodyMutation(built.body, mutation, kPageSize);
  if (!mutated.ok()) {
    std::cerr << mutated.diagnostic.diagnostic_code << '\n';
  }
  Require(mutated.ok(), "allocation map mutation failed");
  Require(mutated.body.map_generation == built.body.map_generation + 1,
          "allocation map mutation did not advance generation");
  Require(mutated.validation.counts.free_pages == 108,
          "allocation map mutation did not reduce free count");
  Require(mutated.validation.counts.allocated_pages == 11,
          "allocation map mutation did not increase allocated count");
  Require(page::ParseAllocationMapPageBody(mutated.serialized).ok(),
          "mutated allocation map did not parse");

  mutation.extent.start_page = 200;
  mutation.extent.page_count = 1;
  Require(!page::ApplyAllocationMapPageBodyMutation(built.body,
                                                    mutation,
                                                    kPageSize)
               .ok(),
          "out-of-range allocation map mutation was admitted");
}

void ProveRebuild() {
  page::AllocationMapPageBody sparse = FixtureBody();
  sparse.filespace_start_page = 1;
  sparse.total_pages = 16;
  sparse.map_generation = 11;
  sparse.capacity_generation = 12;
  sparse.extents = {
      Extent(10,
             2,
             page::PageAllocationLifecycleState::allocated,
             disk::PageType::row_data,
             page::PageFamily::data,
             80),
      Extent(1,
             2,
             page::PageAllocationLifecycleState::preallocated,
             disk::PageType::row_data,
             page::PageFamily::data,
             81),
  };
  const auto rebuilt = page::RebuildAllocationMapPageBody(sparse, kPageSize);
  if (!rebuilt.ok()) {
    std::cerr << rebuilt.diagnostic.diagnostic_code << '\n';
  }
  Require(rebuilt.ok(), "allocation map rebuild failed");
  Require(rebuilt.body.extents.size() == 4,
          "allocation map rebuild did not fill free gaps");
  Require(rebuilt.body.extents[0].start_page == 1 &&
              rebuilt.body.extents[1].start_page == 3 &&
              rebuilt.body.extents[2].start_page == 10 &&
              rebuilt.body.extents[3].start_page == 12,
          "allocation map rebuild extents were not normalized");
  Require(rebuilt.validation.counts.free_pages == 12,
          "allocation map rebuild free count wrong");
  Require(rebuilt.validation.counts.reserved_pages == 2,
          "allocation map rebuild reserved count wrong");
  Require(rebuilt.validation.counts.allocated_pages == 2,
          "allocation map rebuild allocated count wrong");
}

void ProveFailClosedRefusals() {
  auto body = FixtureBody();
  body.capacity_generation = 0;
  Require(!page::BuildAllocationMapPageBody(body, kPageSize).ok(),
          "zero capacity generation allocation map was admitted");

  body = FixtureBody();
  body.extents[1].start_page = 1;
  Require(!page::BuildAllocationMapPageBody(body, kPageSize).ok(),
          "overlapping allocation map extent was admitted");

  const auto built = page::BuildAllocationMapPageBody(FixtureBody(), kPageSize);
  Require(built.ok(), "fixture build failed before corruption test");
  auto corrupted = built.serialized;
  corrupted[page::kAllocationMapPageBodyHeaderBytes + 17] ^= 0x55;
  Require(!page::ParseAllocationMapPageBody(corrupted).ok(),
          "corrupted allocation map extent checksum was admitted");

  page::PageBodyAgreementRequest wrong_header;
  wrong_header.header = HeaderFor(disk::PageType::row_data,
                                  built.body,
                                  built.body.allocation_map_page_number);
  wrong_header.body = built.serialized;
  const auto refused = page::ValidatePageBodyAgreement(wrong_header);
  Require(!refused.ok(), "allocation map body matched non-allocation header");
  Require(refused.kind == page::PageBodyAgreementKind::body_family_mismatch,
          "allocation map header mismatch used wrong refusal kind");
}

}  // namespace

int main() {
  ProveBuildParseAgreementAndReopen();
  ProveMutation();
  ProveRebuild();
  ProveFailClosedRefusals();
  return EXIT_SUCCESS;
}
