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
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace disk = scratchbird::storage::disk;
namespace page = scratchbird::storage::page;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

constexpr platform::u32 kPageSize = 8192;
std::size_t checks = 0;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  ++checks;
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
  body.page_size_bytes = kPageSize;
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
      ("scratchbird_allocation_map_" +
       uuid::UuidToString(MakeUuid(platform::UuidKind::object, 6000).value) +
       ".sbalm");
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
          "allocation map rebuild did not classify gaps");
  Require(rebuilt.body.extents[0].start_page == 1 &&
              rebuilt.body.extents[1].start_page == 3 &&
              rebuilt.body.extents[2].start_page == 10 &&
              rebuilt.body.extents[3].start_page == 12,
          "allocation map rebuild extents were not normalized");
  Require(rebuilt.validation.counts.free_pages == 0 &&
              rebuilt.validation.counts.quarantined_pages == 12 &&
              rebuilt.body.extents[1].state == page::PageAllocationLifecycleState::quarantined &&
              rebuilt.body.extents[3].state == page::PageAllocationLifecycleState::quarantined,
          "missing allocation evidence became free capacity");
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
  for (unsigned field = 0; field != 2; ++field) {
    auto substituted = built.body;
    if (field == 0) substituted.database_uuid = MakeUuid(platform::UuidKind::database, 9100);
    else substituted.filespace_uuid = MakeUuid(platform::UuidKind::filespace, 9101);
    page::PageBodyAgreementRequest request;
    request.header = HeaderFor(disk::PageType::allocation_map, substituted,
                               substituted.allocation_map_page_number);
    request.body = built.serialized;
    Require(!page::ValidatePageBodyAgreement(request).ok(),
            "allocation map was admitted under another physical owner");
  }
}

void RequireEmptyFailure(const page::AllocationMapPageBodyResult& result) {
  Require(!result.ok(), "malformed map was admitted");
  Require(result.body.extents.empty() && result.body.database_uuid.value.is_nil() &&
              result.body.filespace_uuid.value.is_nil() && result.body.total_pages == 0 &&
              result.serialized.empty() && result.validation.counts.total_counted_pages == 0 &&
              result.validation.counts.free_pages == 0,
          "failed map admission published partial allocation authority");
}

void StoreWire64(std::vector<platform::byte>* bytes, std::size_t offset,
                 platform::u64 value) {
  for (unsigned i = 0; i != 8; ++i) (*bytes)[offset + i] = value >> (8 * i);
}

void Rechecksum(std::vector<platform::byte>* bytes) {
  // Independent wire checksum: do not use the implementation's checksum helper.
  platform::u64 count = 0;
  for (unsigned i = 0; i != 4; ++i) count |= platform::u64((*bytes)[124 + i]) << (8 * i);
  platform::u64 hash = 14695981039346656037ull ^ 0x414c4c4f434d4150ull;
  for (std::size_t i = 192; i != 192 + count; ++i) {
    hash ^= (*bytes)[i];
    hash *= 1099511628211ull;
  }
  StoreWire64(bytes, 168, hash);
}

void ProveEvidenceAdmission() {
  using Kind = platform::UuidKind;
  using State = page::PageAllocationLifecycleState;
  auto body = FixtureBody();
  body.file_member_uuid = {};
  body.extents[1].owner_object_uuid = {};
  body.extents[1].creator_transaction_uuid = {};
  auto built = page::BuildAllocationMapPageBody(body, kPageSize);
  Require(built.ok(), "optional identity absence was refused");
  auto parsed = page::ParseAllocationMapPageBody(built.serialized);
  Require(parsed.ok() && parsed.body.file_member_uuid.kind == Kind::unknown &&
              parsed.body.extents[1].owner_object_uuid.kind == Kind::unknown &&
              parsed.body.extents[1].creator_transaction_uuid.kind == Kind::unknown,
          "nil disk identities were not decoded as absence");
  const std::vector<Kind> kinds = {Kind::unknown, Kind::object, Kind::transaction,
                                  Kind::filespace, static_cast<Kind>(65535)};
  // Exercise all eight states, including free and quarantined, not only allocated.
  for (unsigned state = 0; state != 8; ++state) {
    for (unsigned field = 0; field != 4; ++field) {
      for (Kind kind : kinds) {
        for (unsigned malformed = 0; malformed != 3; ++malformed) {
          body = FixtureBody();
          auto& extent = body.extents[1];
          extent = Extent(2, 8, static_cast<State>(state), disk::PageType::row_data,
                          page::PageFamily::data, 1);
          if (state == 0) extent = {2, 8, State::free};
          auto& id = field == 0 ? body.file_member_uuid :
                     field == 1 ? extent.allocation_uuid :
                     field == 2 ? extent.owner_object_uuid : extent.creator_transaction_uuid;
          id = MakeUuid(field == 3 ? Kind::transaction : Kind::object, 7000);
          id.kind = kind;
          if (malformed == 0) id.value = {};
          if (malformed == 2) id.value.bytes[6] = (id.value.bytes[6] & 15) | 0x40;
          const bool absent = kind == Kind::unknown && malformed == 0;
          const bool required = field == 1 && state != 0 && state != 6;
          const bool valid = malformed == 1 &&
              kind == (field == 3 ? Kind::transaction : Kind::object);
          const bool allowed = (absent && !required) ||
              (valid && (field == 0 || state != 0));
          const auto validation = page::ValidateAllocationMapPageBody(body);
          Require(validation.ok() == allowed, "typed UUID/state validation disagrees with oracle");
          const auto result = page::BuildAllocationMapPageBody(body, kPageSize);
          Require(result.ok() == allowed, "build normalized invalid identity/state");
          if (!allowed) {
            RequireEmptyFailure(result);
            RequireEmptyFailure(page::RebuildAllocationMapPageBody(body, kPageSize));
            if (field != 0) {
              page::AllocationMapPageBodyMutation mutation;
              mutation.extent = extent;
              RequireEmptyFailure(page::ApplyAllocationMapPageBodyMutation(
                  FixtureBody(), mutation, kPageSize));
            }
          } else {
            Require(page::ParseAllocationMapPageBody(result.serialized).ok(),
                    "admitted identity did not round trip");
          }
        }
      }
    }
  }
  built = page::BuildAllocationMapPageBody(FixtureBody(), kPageSize);
  Require(built.ok(), "wire fixture build failed");
  for (unsigned field = 0; field != 5; ++field) {
    body = FixtureBody();
    auto& extent = body.extents[2];
    if (field == 0) extent.page_type = disk::PageType::row_data;
    if (field == 1) extent.page_family = page::PageFamily::data;
    if (field == 2) extent.extent_flags = 1;
    if (field == 3) extent.page_generation = 1;
    if (field == 4) extent.reusable_after_local_transaction_id = 1;
    Require(!page::ValidateAllocationMapPageBody(body).ok(),
            "free metadata was silently cleared by validation");
    RequireEmptyFailure(page::BuildAllocationMapPageBody(body, kPageSize));
    RequireEmptyFailure(page::RebuildAllocationMapPageBody(body, kPageSize));
    page::AllocationMapPageBodyMutation mutation;
    mutation.extent = extent;
    RequireEmptyFailure(page::ApplyAllocationMapPageBodyMutation(FixtureBody(), mutation, kPageSize));
  }
  // Every metadata byte in a free extent must remain canonical, including UUIDs.
  for (std::size_t offset = 20; offset != 96; ++offset) {
    auto bytes = built.serialized;
    bytes[192 + 2 * 96 + offset] ^= 1;
    Rechecksum(&bytes);
    RequireEmptyFailure(page::ParseAllocationMapPageBody(bytes));
  }
  for (std::size_t offset : {std::size_t(176), std::size_t(191), built.serialized.size() - 1}) {
    auto bytes = built.serialized;
    bytes[offset] = 1;
    RequireEmptyFailure(page::ParseAllocationMapPageBody(bytes));
  }
  auto bytes = built.serialized;
  bytes.pop_back();
  RequireEmptyFailure(page::ParseAllocationMapPageBody(bytes));
  bytes = built.serialized;
  bytes.push_back(0);
  RequireEmptyFailure(page::ParseAllocationMapPageBody(bytes));
  bytes = built.serialized;
  bytes[24 + 6] = (bytes[24 + 6] & 15) | 0x40;
  RequireEmptyFailure(page::ParseAllocationMapPageBody(bytes));
  bytes = built.serialized;
  bytes[192 + 5 * 96 + 48] = 1; // Malformed quarantined allocation identity.
  Rechecksum(&bytes);
  RequireEmptyFailure(page::ParseAllocationMapPageBody(bytes));
  bytes = built.serialized;
  bytes[192 + 5 * 96 + 24 + 3] = 0xff; // Unknown family on quarantine.
  Rechecksum(&bytes);
  RequireEmptyFailure(page::ParseAllocationMapPageBody(bytes));

  body = FixtureBody();
  page::AllocationMapPageBodyMutation mutation;
  mutation.extent = {10, 6, State::free};
  body.map_generation = std::numeric_limits<platform::u64>::max();
  RequireEmptyFailure(page::ApplyAllocationMapPageBodyMutation(body, mutation, kPageSize));
  body = FixtureBody();
  body.extents.insert(body.extents.begin() + 2, {10, 0, State::free});
  RequireEmptyFailure(page::BuildAllocationMapPageBody(body, kPageSize));

  body = FixtureBody();
  body.extents = {{10, 6, State::free}};
  auto rebuilt = page::RebuildAllocationMapPageBody(body, kPageSize);
  Require(rebuilt.ok() && rebuilt.validation.counts.free_pages == 6 &&
              rebuilt.validation.counts.quarantined_pages == 122 &&
              rebuilt.body.extents.size() == 3 &&
              rebuilt.body.extents[0].start_page == 1 &&
              rebuilt.body.extents[0].page_count == 9 &&
              rebuilt.body.extents[2].start_page == 16 &&
              rebuilt.body.extents[2].page_count == 113,
          "explicit free evidence or leading/trailing gaps were lost");
  Require(page::ParseAllocationMapPageBody(rebuilt.serialized).ok(),
          "reconstructed map could not be decoded");
  body.extents.clear();
  rebuilt = page::RebuildAllocationMapPageBody(body, kPageSize);
  Require(rebuilt.ok() && rebuilt.validation.counts.free_pages == 0 &&
              rebuilt.validation.counts.quarantined_pages == 128,
          "empty allocation evidence fabricated free capacity");
  body.extents = {{10, 6, State::free}, {12, 3, State::free}};
  RequireEmptyFailure(page::RebuildAllocationMapPageBody(body, kPageSize));
  body.extents = {{128, 2, State::free}};
  RequireEmptyFailure(page::RebuildAllocationMapPageBody(body, kPageSize));
}

}  // namespace

int main() {
  ProveBuildParseAgreementAndReopen();
  ProveMutation();
  ProveRebuild();
  ProveFailClosedRefusals();
  ProveEvidenceAdmission();
  std::cout << "allocation evidence checks=" << checks << " failures=0\n";
  return EXIT_SUCCESS;
}
