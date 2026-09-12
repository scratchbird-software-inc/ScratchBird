// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog/name_registry.hpp"
#include "database_lifecycle.hpp"
#include "uuid.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#if defined(__linux__)
#include <sys/stat.h>
#endif

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace uuid = scratchbird::core::uuid;
namespace fs = std::filesystem;
using scratchbird::core::platform::UuidKind;

int failures = 0;
void Check(bool value, const char* message) {
  if (!value) { ++failures; std::cerr << "FAIL " << message << '\n'; }
}
void Setup(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

int main(int argc, char** argv) {
  const bool baseline = argc == 2 && std::string(argv[1]) == "--baseline";
  fs::path root;
  try {
    const auto millis = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    const auto database = uuid::GenerateEngineIdentityV7(UuidKind::database, millis);
    const auto filespace = uuid::GenerateEngineIdentityV7(UuidKind::filespace, millis);
    const auto object = uuid::GenerateEngineIdentityV7(UuidKind::object, millis);
    Setup(database.ok() && filespace.ok() && object.ok(), "UUID generation failed");
    root = fs::temp_directory_path() /
        ("scratchbird_name_read_" + uuid::UuidToString(database.value.value));
    Setup(fs::create_directory(root), "unique fixture directory not created");
    std::cout << "fixture=" << root << '\n';
    db::DatabaseCreateConfig create;
    create.path = (root / "primary.sdb").string();
    create.database_uuid = database.value;
    create.filespace_uuid = filespace.value;
    create.page_size = 16384;
    create.creation_unix_epoch_millis = millis;
    create.allow_minimal_resource_bootstrap = true;
    create.require_resource_seed_pack = false;
    Setup(db::CreateDatabaseFile(create).ok(), "real database initialization failed");
    api::EngineRequestContext context;
    context.database_path = create.path;
    context.database_uuid.canonical = uuid::UuidToString(database.value.value);
    const fs::path journal = create.path + ".sb.api_events";
    Setup(!fs::exists(journal), "unexpected preexisting catalog fixture journal");
    const std::string identity = uuid::UuidToString(object.value.value);
    // Exercise the real existing writer and reader. The textual carrier and
    // profile label here are current-format regression data, not migration proof.
    api::NameRegistryEntry entry;
    entry.creator_tx = 1; // Real committed database bootstrap transaction.
    entry.name_entry_uuid = uuid::UuidToString(
        uuid::GenerateEngineIdentityV7(UuidKind::object, millis).value.value);
    entry.object_uuid = identity;
    entry.object_class = "table";
    entry.raw_name_text = entry.display_name = "READ_AUTHORITY_PROBE";
    entry.normalized_lookup_key = entry.exact_lookup_key = entry.raw_name_text;
    entry.catalog_generation_id = entry.resource_epoch = entry.name_resolution_epoch = 1;
    api::EngineApiRequest request;
    api::EngineLocalizedName wanted;
    wanted.name = entry.raw_name_text;
    request.localized_names.push_back(wanted);
    const auto resolve = [&] {
      request.context = context;
      return api::ResolveNameRegistryPrivate(request, "table");
    };
    const auto absent = [&] {
      const auto state = api::LoadNameRegistryState(context, 0);
      Check(state.ok, "absent journal retains valid bootstrap authority");
      for (const auto& item : state.state.entries)
        Check(item.object_uuid != identity, "absent journal cannot return cached probe");
      const auto resolved = resolve();
      Check(!resolved.ok && resolved.matches.empty(), "absent probe cannot resolve");
    };
    const auto present = [&] {
      const auto state = api::LoadNameRegistryState(context, 0);
      bool found = false;
      for (const auto& item : state.state.entries)
        if (item.object_uuid == identity && item.name_entry_uuid == entry.name_entry_uuid &&
            item.raw_name_text == entry.raw_name_text && !item.derived_from_legacy_name)
          found = true;
      Check(state.ok && found, "real persisted explicit entry identity survives");
      const auto resolved = resolve();
      Check(resolved.ok && resolved.matches.size() == 1 &&
            resolved.matches[0].object_uuid == identity, "actual name resolves to persisted identity");
    };
    const auto refused = [&] {
      // Resolver first: its snapshot cache must not bypass current authority.
      const auto resolved = resolve();
      Check(!resolved.ok && resolved.diagnostic.error && resolved.matches.empty(),
            "failed read cannot resolve cached or partial names");
      const auto state = api::LoadNameRegistryState(context, 0);
      Check(!state.ok && state.diagnostic.error && state.state.entries.empty(),
            "failed read cannot expose successful cached or partial authority");
    };
    absent();
    { std::ofstream empty(journal, std::ios::binary); Setup(empty.good(), "empty fixture open failed"); }
    absent();
    Setup(!api::AppendNameRegistryEntry(context, entry, "name_read_probe").error,
          "actual name journal append failed");
    present(); present();
    const auto permissions = fs::status(journal).permissions();
#if defined(__linux__)
    // Run as an ordinary user: require a real kernel access denial, no mock.
    fs::permissions(journal, fs::perms::none);
    Setup(!std::ifstream(journal).is_open(), "permission fault needs unprivileged Linux execution");
    refused(); // Warm cache must not bypass current read access.
    ++context.catalog_generation_id;
    refused(); // Cold failure must not cache a fabricated empty catalog.
    fs::permissions(journal, permissions);
    present();
#endif
    const fs::path saved = root / "saved.catalog";
    fs::rename(journal, saved);
    Setup(fs::create_directory(journal), "directory fault setup failed");
    refused();
    Setup(fs::remove(journal), "empty directory fault cleanup failed");
    fs::rename(saved, journal); present();
#if defined(__linux__)
    fs::rename(journal, saved);
    fs::create_symlink(root / "absent_target", journal); refused();
    Setup(fs::remove(journal), "dangling symlink cleanup failed");
    fs::create_symlink(journal, journal); refused();
    Setup(fs::remove(journal), "symlink loop cleanup failed");
    if (!baseline) {
      Setup(::mkfifo(journal.c_str(), 0600) == 0, "FIFO fault setup failed");
      refused(); // Must reject the non-file before an open can block.
      Setup(fs::remove(journal), "FIFO cleanup failed");
    }
    // Linux exposes this as a regular file whose offset-zero read fails with
    // EIO. Exercise a real stream read failure after a successful open.
    fs::create_symlink("/proc/self/mem", journal);
    {
      std::ifstream probe(journal, std::ios::binary);
      Setup(probe.is_open(), "kernel read fault did not open");
      std::string ignored;
      std::getline(probe, ignored);
      Setup(probe.bad(), "kernel read fault did not fail");
    }
    refused();
    Setup(fs::remove(journal), "kernel read fault cleanup failed");
    fs::create_symlink(saved, journal); present();
    Setup(fs::remove(journal), "regular symlink cleanup failed");
    fs::rename(saved, journal); present();
#endif
    // Removing a once-cached journal must not return the previous object.
    fs::rename(journal, saved); absent();
    fs::rename(saved, journal); present();
    if (failures != 0) {
      std::cerr << failures << " failed assertions; fixture retained at " << root << '\n';
      return 1;
    }
    // Exact uniquely created test root only; no user database is in scope.
    fs::remove_all(root);
    std::cout << "PASS real name writes, reads, resolution, access errors and cache recovery\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << "; fixture retained at " << root << '\n';
    return 1;
  }
}
