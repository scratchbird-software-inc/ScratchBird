// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "allocation_map_page.hpp"
#include "database_lifecycle.hpp"
#include "index_btree_page.hpp"
#include "row_data_page.hpp"
#include "runtime_platform.hpp"
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

namespace db = scratchbird::storage::database;
namespace disk = scratchbird::storage::disk;
namespace page = scratchbird::storage::page;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

platform::u64 TimeSeed() {
  return static_cast<platform::u64>(
      std::chrono::steady_clock::now().time_since_epoch().count());
}

platform::TypedUuid NewUuid(platform::UuidKind kind, platform::u64 salt) {
  const auto generated =
      uuid::GenerateEngineIdentityV7(kind, 1900000000000ull + salt);
  Require(generated.ok(), "IPAR-P3-09 UUID generation failed");
  return generated.value;
}

void StoreLittle16(std::vector<platform::byte>* bytes,
                   std::size_t offset,
                   platform::u16 value) {
  Require(bytes != nullptr && bytes->size() >= offset + sizeof(value),
          "IPAR-P3-09 little-endian write out of range");
  platform::StoreLittle16(bytes->data() + offset, value);
}

void VerifyRowDataPageFormatSafety(platform::u64 salt) {
  page::RowDataPageBody body;
  body.relation_uuid = NewUuid(platform::UuidKind::object, salt + 1);
  body.segment_id = 1;
  body.segment_generation = 1;
  body.page_number = 1024;
  body.page_generation = 1;
  const auto built = page::BuildRowDataPageBody(body, 16384);
  Require(built.ok(), "IPAR-P3-09 current row page did not build");
  Require(page::ParseRowDataPageBody(built.serialized, body.page_number).ok(),
          "IPAR-P3-09 current row page did not parse");

  auto future = built.serialized;
  future[7] = '3';
  const auto refused = page::ParseRowDataPageBody(future, body.page_number);
  Require(!refused.ok(),
          "IPAR-P3-09 future row page format was accepted");
  Require(refused.diagnostic.diagnostic_code ==
              "SB-ROW-DATA-PAGE-FORMAT-UNSUPPORTED",
          "IPAR-P3-09 future row page did not return format refusal");
}

void VerifyIndexPageFormatSafety(platform::u64 salt) {
  page::IndexBtreePageBody body;
  body.index_uuid = NewUuid(platform::UuidKind::object, salt + 10);
  body.page_number = 4096;
  body.page_kind = page::IndexBtreePageKind::root;
  body.tree_level = 0;
  const auto built = page::BuildIndexBtreePageBody(body, 16384);
  Require(built.ok(), "IPAR-P3-09 current index page did not build");
  Require(page::ParseIndexBtreePageBody(built.serialized, body.page_number).ok(),
          "IPAR-P3-09 current index page did not parse");

  auto future = built.serialized;
  future[7] = '2';
  const auto refused = page::ParseIndexBtreePageBody(future, body.page_number);
  Require(!refused.ok(),
          "IPAR-P3-09 future index page format was accepted");
  Require(refused.diagnostic.diagnostic_code ==
              "SB-INDEX-BTREE-PAGE-FORMAT-UNSUPPORTED",
          "IPAR-P3-09 future index page did not return format refusal");
}

void VerifyAllocationMapFormatSafety(platform::u64 salt) {
  page::AllocationMapPageBody body;
  body.database_uuid = NewUuid(platform::UuidKind::database, salt + 20);
  body.filespace_uuid = NewUuid(platform::UuidKind::filespace, salt + 21);
  body.allocation_map_page_number = 64;
  body.map_generation = 1;
  body.capacity_generation = 1;
  body.filespace_start_page = 0;
  body.total_pages = 128;
  page::AllocationMapExtent extent;
  extent.start_page = 0;
  extent.page_count = body.total_pages;
  extent.state = page::PageAllocationLifecycleState::free;
  body.extents.push_back(extent);

  const auto built = page::BuildAllocationMapPageBody(body, 16384);
  Require(built.ok(), "IPAR-P3-09 current allocation map did not build");
  Require(page::ParseAllocationMapPageBody(built.serialized).ok(),
          "IPAR-P3-09 current allocation map did not parse");

  auto future = built.serialized;
  StoreLittle16(&future, 14, page::kAllocationMapPageBodyFormatMinor + 1);
  const auto refused = page::ParseAllocationMapPageBody(future);
  Require(!refused.ok(),
          "IPAR-P3-09 future allocation map format was accepted");
  Require(refused.diagnostic.diagnostic_code ==
              "SB-ALLOCATION-MAP-PAGE-FORMAT-UNSUPPORTED",
          "IPAR-P3-09 future allocation map did not return format refusal");
}

void VerifyDatabaseArtifactMigrationAdmission() {
  db::DatabaseArtifactVersionCompatibilityRequest current;
  current.artifact_kind = "row_page";
  current.format_major = 2;
  current.current_major = 2;
  current.max_supported_major = 2;
  const auto current_result =
      db::ClassifyDatabaseArtifactVersionCompatibility(current);
  Require(current_result.ok() &&
              current_result.compatibility_class ==
                  db::DatabaseOpenCompatibilityClass::current,
          "IPAR-P3-09 current artifact classification failed");

  db::DatabaseArtifactVersionCompatibilityRequest old = current;
  old.format_major = 1;
  const auto old_without_plan =
      db::ClassifyDatabaseArtifactVersionCompatibility(old);
  Require(!old_without_plan.ok() &&
              old_without_plan.compatibility_class ==
                  db::DatabaseOpenCompatibilityClass::
                      migration_required_without_plan_refused,
          "IPAR-P3-09 migration without explicit plan was not refused");

  old.migration_plan_id = "row_page_v1_0_to_v2_0_explicit_plan_v1";
  const auto old_with_plan =
      db::ClassifyDatabaseArtifactVersionCompatibility(old);
  Require(old_with_plan.ok() &&
              old_with_plan.compatibility_class ==
                  db::DatabaseOpenCompatibilityClass::supported_migration &&
              old_with_plan.migration_required,
          "IPAR-P3-09 explicit migration plan was not accepted");

  db::DatabaseArtifactVersionCompatibilityRequest future = current;
  future.format_major = 3;
  const auto future_result =
      db::ClassifyDatabaseArtifactVersionCompatibility(future);
  Require(!future_result.ok() &&
              future_result.compatibility_class ==
                  db::DatabaseOpenCompatibilityClass::
                      newer_than_supported_refused,
          "IPAR-P3-09 future artifact format was not refused");
}

struct DatabaseFileFixture {
  std::filesystem::path dir;
  std::filesystem::path database_path;

  ~DatabaseFileFixture() {
    if (!dir.empty()) {
      std::error_code ignored;
      std::filesystem::remove_all(dir, ignored);
    }
  }
};

DatabaseFileFixture CreateDatabaseFileFixture(platform::u64 salt,
                                              std::string_view suffix) {
  DatabaseFileFixture fixture;
  fixture.dir = std::filesystem::temp_directory_path() /
                ("scratchbird_ipar_storage_format_" + std::string(suffix) +
                 "_" + std::to_string(salt));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "format_refusal.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = NewUuid(platform::UuidKind::database, salt + 1);
  create.filespace_uuid = NewUuid(platform::UuidKind::filespace, salt + 2);
  create.creation_unix_epoch_millis = 1900000000000ull + salt + 3;
  create.page_size = 16384;
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "IPAR-P7-11 database create failed");
  return fixture;
}

void WriteLittle32At(const std::filesystem::path& path,
                     std::streamoff offset,
                     platform::u32 value) {
  std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
  Require(file.good(), "IPAR-P7-11 database file open for mutation failed");
  const platform::u32 stored = platform::HostToLittle32(value);
  file.seekp(offset);
  file.write(reinterpret_cast<const char*>(&stored), sizeof(stored));
  file.flush();
  Require(file.good(), "IPAR-P7-11 database file mutation failed");
}

void VerifyDatabaseOpenFormatRefusal(platform::u64 salt) {
  {
    auto current = CreateDatabaseFileFixture(salt + 10000, "current");
    const auto opened =
        db::OpenDatabaseFile({current.database_path.string(), false, false, false});
    if (!opened.ok()) {
      std::cerr << opened.diagnostic.diagnostic_code << ':'
                << opened.diagnostic.message_key << '\n';
    }
    Require(opened.ok(), "IPAR-P7-11 current database open failed");
  }

  {
    auto old = CreateDatabaseFileFixture(salt + 20000, "old");
    WriteLittle32At(old.database_path, 8, disk::kScratchBirdDatabaseFormatMajor - 1);
    const auto refused =
        db::OpenDatabaseFile({old.database_path.string(), false, false, false});
    Require(!refused.ok(),
            "IPAR-P7-11 unsupported old database format was opened");
    Require(refused.diagnostic.diagnostic_code ==
                "FORMAT.VERSION_TOO_OLD",
            "IPAR-P7-11 old database format did not return old-format refusal");
  }

  {
    auto future = CreateDatabaseFileFixture(salt + 30000, "future");
    WriteLittle32At(future.database_path,
                    8,
                    disk::kScratchBirdDatabaseFormatMajor + 1);
    const auto refused =
        db::OpenDatabaseFile({future.database_path.string(), false, false, false});
    Require(!refused.ok(),
            "IPAR-P7-11 future database format was opened");
    Require(refused.diagnostic.diagnostic_code ==
                "FORMAT.VERSION_UNSUPPORTED",
            "IPAR-P7-11 future database format did not return new-format refusal");
  }
}

}  // namespace

int main() {
  const platform::u64 seed = TimeSeed();
  VerifyRowDataPageFormatSafety(seed + 1000);
  VerifyIndexPageFormatSafety(seed + 2000);
  VerifyAllocationMapFormatSafety(seed + 3000);
  VerifyDatabaseArtifactMigrationAdmission();
  VerifyDatabaseOpenFormatRefusal(seed + 4000);
  return EXIT_SUCCESS;
}
