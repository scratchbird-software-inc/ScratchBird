// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "disk_device.hpp"
#include "page_cache.hpp"
#include "startup_state.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

namespace db = scratchbird::storage::database;
namespace disk = scratchbird::storage::disk;
namespace page = scratchbird::storage::page;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::storage::disk::PageType;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

std::uint64_t CurrentUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

TypedUuid MakeUuid(UuidKind kind, std::uint64_t offset) {
  auto generated = uuid::GenerateEngineIdentityV7(kind, CurrentUnixMillis() + offset);
  Require(generated.ok(), "DBLC-013I UUID generation failed");
  return generated.value;
}

page::PageCacheLifecycleInput ValidInput() {
  page::PageCacheLifecycleInput input;
  input.database_uuid = MakeUuid(UuidKind::database, 1);
  input.filespace_uuid = MakeUuid(UuidKind::filespace, 2);
  input.database_lifecycle_state = "opened";
  input.policy_generation = 3;
  input.checkpoint_generation = 4;
  input.tx2_activation_committed = true;
  input.cache_runtime_started = true;
  input.engine_agent_active = true;
  input.writeback_allowed = true;
  input.checkpoint_allowed = true;
  input.standalone_mode = true;
  input.cluster_authority_available = false;
  return input;
}

page::PageCacheEntry Entry(const page::PageCacheLifecycleInput& input,
                           std::uint64_t index,
                           PageType type = PageType::row_data) {
  page::PageCacheEntry entry;
  entry.database_uuid = input.database_uuid;
  entry.filespace_uuid = input.filespace_uuid;
  entry.page_uuid = MakeUuid(UuidKind::page, 100 + index);
  entry.page_type = type;
  entry.page_number = index;
  entry.page_generation = 1;
  entry.page_size = 16384;
  return entry;
}

std::filesystem::path TestDatabasePath() {
  return std::filesystem::temp_directory_path() /
         ("sb_dblc013i_cache_checkpoint_" + std::to_string(CurrentUnixMillis()) + ".sbdb");
}

db::StartupStateRecord ReadStartup(const std::filesystem::path& path, std::uint32_t page_size) {
  disk::FileDevice device;
  const auto opened = device.Open(path.string(), disk::FileOpenMode::open_existing_read_only);
  Require(opened.ok(), "DBLC-013I startup read open failed");
  const auto startup = db::ReadStartupStatePageBody(&device, page_size);
  Require(startup.ok(), "DBLC-013I startup state read failed");
  return startup.state;
}

void TestActivationRequiredAndAuthorityBoundary() {
  page::PageCacheLedger ledger;
  page::PageCachePolicy policy;
  auto input = ValidInput();
  input.tx2_activation_committed = false;
  const auto refused = page::StartPageCacheLifecycle(&ledger, policy, input, {});
  Require(!refused.ok(), "page cache lifecycle started before tx2");
  Require(refused.diagnostic.diagnostic_code == "CACHE.CHECKPOINT_INPUT_INVALID",
          "page cache tx2 refusal diagnostic mismatch");

  page::PageCacheAuthorityBoundary invalid;
  invalid.transaction_finality_authority = true;
  const auto authority = page::ValidatePageCacheAuthorityBoundary(invalid);
  Require(!authority.ok(), "page cache accepted transaction finality authority");
  Require(authority.diagnostic.diagnostic_code == "CACHE.CHECKPOINT_AUTHORITY_DENIED",
          "page cache authority diagnostic mismatch");
}

void TestPreloadDirtyWritebackCheckpoint() {
  page::PageCacheLedger ledger;
  page::PageCachePolicy policy;
  policy.max_resident_pages = 4;
  policy.max_resident_bytes = 4ull * 16384ull;
  const auto input = ValidInput();
  std::vector<page::PageCacheEntry> entries = {Entry(input, 1), Entry(input, 2)};

  const auto started = page::StartPageCacheLifecycle(&ledger, policy, input, entries);
  Require(started.ok(), "page cache preload failed");
  Require(started.publication.preload_complete, "page cache preload was not published");
  Require(started.publication.cluster_paths_failed_closed,
          "page cache did not fail cluster paths closed in standalone mode");
  Require(page::PageCacheAuthorityBoundaryValid(started.publication.authority_boundary),
          "page cache authority boundary is invalid");
  const auto json = page::SerializePageCacheCheckpointJson(started.publication, false);
  Require(Contains(json, "\"authority_boundary_valid\":true"),
          "page cache JSON omitted authority boundary");

  const auto dirty = page::MarkPageCacheEntryDirty(&ledger, entries.front().page_uuid, true);
  Require(dirty.ok(), "page cache dirty tracking failed");
  Require(dirty.snapshot.dirty_pages == 1, "page cache dirty count mismatch");

  const auto try_checkpoint =
      page::CheckpointPageCacheLifecycle(&ledger, input, page::PageCacheCheckpointMode::try_checkpoint);
  Require(!try_checkpoint.ok(), "try checkpoint succeeded with dirty pages");
  Require(try_checkpoint.diagnostic.diagnostic_code == "CACHE.CHECKPOINT_DIRTY_PAGES_REMAIN",
          "try checkpoint dirty diagnostic mismatch");

  const auto writeback = page::WritebackDirtyPageCacheEntries(&ledger, input);
  Require(writeback.ok(), "page cache writeback failed");
  Require(writeback.flushed_pages == 1, "page cache writeback flushed count mismatch");
  Require(writeback.publication.writeback_complete,
          "page cache writeback did not publish completion");

  const auto checkpoint =
      page::CheckpointPageCacheLifecycle(&ledger, input, page::PageCacheCheckpointMode::force_checkpoint);
  Require(checkpoint.ok(), "force checkpoint failed");
  Require(checkpoint.publication.checkpoint_complete,
          "force checkpoint did not publish completion");
  Require(!checkpoint.publication.clean_close_evidence,
          "ordinary checkpoint incorrectly published clean-close evidence");
}

void TestMemoryPressureAndPinnedRefusal() {
  auto input = ValidInput();
  page::PageCacheLedger ledger;
  page::PageCachePolicy policy;
  policy.max_resident_pages = 3;
  policy.max_resident_bytes = 3ull * 16384ull;
  input.target_resident_pages = 1;
  std::vector<page::PageCacheEntry> entries = {Entry(input, 1), Entry(input, 2), Entry(input, 3)};
  Require(page::StartPageCacheLifecycle(&ledger, policy, input, entries).ok(),
          "memory-pressure preload failed");
  for (const auto& entry : entries) {
    Require(page::MarkPageCacheEntryDirty(&ledger, entry.page_uuid, true).ok(),
            "memory-pressure dirty tracking failed");
  }
  const auto pressure = page::ApplyPageCacheMemoryPressure(&ledger, policy, input);
  Require(pressure.ok(), "memory pressure handling failed");
  Require(pressure.flushed_pages == 3, "memory pressure did not flush dirty pages first");
  Require(pressure.evicted_pages == 2, "memory pressure eviction count mismatch");
  Require(pressure.snapshot.resident_pages == 1, "memory pressure target not reached");
  Require(pressure.publication.memory_pressure_handled,
          "memory pressure handling was not published");

  page::PageCacheLedger pinned_ledger;
  std::vector<page::PageCacheEntry> pinned_entries = {Entry(input, 11), Entry(input, 12)};
  Require(page::StartPageCacheLifecycle(&pinned_ledger, policy, input, pinned_entries).ok(),
          "pinned preload failed");
  Require(page::PinPageCacheEntry(&pinned_ledger, pinned_entries[0].page_uuid).ok(),
          "pin first page failed");
  Require(page::PinPageCacheEntry(&pinned_ledger, pinned_entries[1].page_uuid).ok(),
          "pin second page failed");
  input.target_resident_pages = 1;
  const auto refused = page::ApplyPageCacheMemoryPressure(&pinned_ledger, policy, input);
  Require(!refused.ok(), "memory pressure evicted pinned pages");
  Require(refused.diagnostic.diagnostic_code == "CACHE.CHECKPOINT_MEMORY_PRESSURE_PINNED",
          "pinned pressure diagnostic mismatch");
}

void TestShutdownFlush() {
  auto input = ValidInput();
  page::PageCacheLedger ledger;
  page::PageCachePolicy policy;
  std::vector<page::PageCacheEntry> entries = {Entry(input, 1), Entry(input, 2)};
  Require(page::StartPageCacheLifecycle(&ledger, policy, input, entries).ok(),
          "shutdown preload failed");
  for (const auto& entry : entries) {
    Require(page::MarkPageCacheEntryDirty(&ledger, entry.page_uuid, true).ok(),
            "shutdown dirty tracking failed");
  }
  input.shutdown_requested = true;
  input.clean_close_requested = true;
  input.database_lifecycle_state = "closed";
  const auto shutdown = page::ShutdownFlushPageCacheLifecycle(&ledger, input);
  Require(shutdown.ok(), "shutdown flush failed");
  Require(shutdown.state == page::PageCacheLifecycleState::stopped,
          "shutdown flush did not stop page cache lifecycle");
  Require(shutdown.flushed_pages == 2, "shutdown flush count mismatch");
  Require(shutdown.snapshot.resident_pages == 0, "shutdown flush left resident pages");
  Require(shutdown.publication.clean_close_evidence,
          "shutdown flush did not publish clean-close evidence");
  Require(!shutdown.publication.ordinary_admission_allowed,
          "shutdown page cache admitted ordinary work");
}

void TestDatabaseLifecycleIntegration() {
  const auto path = TestDatabasePath();
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
    }
  } cleanup{path};

  const auto now = CurrentUnixMillis();
  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid = uuid::GenerateEngineIdentityV7(UuidKind::database, now).value;
  create.filespace_uuid = uuid::GenerateEngineIdentityV7(UuidKind::filespace, now + 1).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = now + 2;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "DBLC-013I database create failed");

  const auto opened = db::OpenDatabaseFile({path.string(), false, false, false});
  if (!opened.ok()) {
    std::cerr << opened.diagnostic.diagnostic_code << ":"
              << opened.diagnostic.message_key << '\n';
  }
  Require(opened.ok(), "DBLC-013I database open failed");
  Require(opened.state.cache_checkpoint_present,
          "database open did not publish cache checkpoint state");
  Require(opened.state.cache_checkpoint.preload_complete,
          "database open did not record cache preload evidence");
  Require(Contains(opened.state.cache_checkpoint_json, "\"page_cache_checkpoint\""),
          "database open cache checkpoint JSON missing");
  Require(db::StartupLifecycleEvidencePresent(
              opened.state.startup_state,
              db::StartupLifecycleEvidenceFlag::cache_preload_completed),
          "database open did not persist cache preload evidence flag");

  const auto clean = db::MarkDatabaseCleanShutdown(path.string());
  Require(clean.ok(), "DBLC-013I clean shutdown failed");
  const auto durable = ReadStartup(path, create.page_size);
  Require(durable.clean_shutdown, "clean shutdown did not persist clean state");
  Require(durable.checkpoint_generation >= 2,
          "clean shutdown did not advance cache checkpoint generation");
  Require(db::StartupLifecycleEvidencePresent(
              durable,
              db::StartupLifecycleEvidenceFlag::cache_checkpoint_completed),
          "clean shutdown did not persist cache checkpoint evidence");
  Require(db::StartupLifecycleEvidencePresent(
              durable,
              db::StartupLifecycleEvidenceFlag::cache_shutdown_flush_completed),
          "clean shutdown did not persist cache shutdown flush evidence");
}

}  // namespace

int main() {
  TestActivationRequiredAndAuthorityBoundary();
  TestPreloadDirtyWritebackCheckpoint();
  TestMemoryPressureAndPinnedRefusal();
  TestShutdownFlush();
  TestDatabaseLifecycleIntegration();
  return EXIT_SUCCESS;
}
