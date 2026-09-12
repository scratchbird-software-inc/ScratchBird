// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog/catalog_object_lifecycle.hpp"
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

int main() {
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
        ("scratchbird_catalog_read_" + uuid::UuidToString(database.value.value));
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
    const fs::path journal = create.path + ".sb.catalog_object_events";
    Setup(!fs::exists(journal), "unexpected preexisting catalog fixture journal");
    const std::string identity = uuid::UuidToString(object.value.value);
    // Actual current-format bootstrap catalog event. This is a storage/API
    // component fixture, not a parser, public command or visibility oracle.
    const std::string event = "SBCATOBJ1\tOBJECT\t0\t" + identity +
        "\tschema\t\t\tactive\t1\t41\t\t0\n";
    const auto write = [&](const std::string& bytes) {
      std::ofstream out(journal, std::ios::binary | std::ios::trunc);
      out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
      out.close(); Setup(static_cast<bool>(out), "fixture journal write failed");
    };
    const auto absent = [&] {
      const auto state = api::LoadCatalogObjectLifecycleState(context);
      const auto epoch = api::LoadCatalogObjectLifecycleEpochState(context);
      Check(state.ok && state.state.objects.empty(), "absent/empty catalog state");
      Check(epoch.ok && epoch.state.metadata_epoch == 0, "absent/empty catalog epoch");
    };
    const auto present = [&] {
      const auto state = api::LoadCatalogObjectLifecycleState(context);
      const auto epoch = api::LoadCatalogObjectLifecycleEpochState(context);
      Check(state.ok && state.state.objects.size() == 1 &&
            state.state.objects[0].object_uuid == identity &&
            state.state.metadata_epoch == 41, "actual object identity and epoch must survive");
      Check(epoch.ok && epoch.state.metadata_epoch == 41, "actual metadata epoch must survive");
    };
    const auto refused = [&] {
      const auto state = api::LoadCatalogObjectLifecycleState(context);
      const auto epoch = api::LoadCatalogObjectLifecycleEpochState(context);
      Check(!state.ok && state.diagnostic.code == api::kCatalogObjectDiagnosticMgaVisibilityRefused,
            "catalog read failure must not return successful authority");
      Check(!epoch.ok && epoch.diagnostic.code == api::kCatalogObjectDiagnosticMgaVisibilityRefused,
            "epoch read failure must not return successful authority");
      Check(state.state.objects.empty() && state.state.metadata_epoch == 0,
            "catalog failure must not expose cached or partial state");
      Check(epoch.state.metadata_epoch == 0, "epoch failure must not expose partial state");
    };
    absent();
    write(""); absent();
    write(event); present(); present();
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
    Setup(::mkfifo(journal.c_str(), 0600) == 0, "FIFO fault setup failed");
    refused(); // Must reject the non-file before an open can block.
    Setup(fs::remove(journal), "FIFO cleanup failed");
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
    std::cout << "PASS real catalog reads, access errors, cache recovery and epoch agreement\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << "; fixture retained at " << root << '\n';
    return 1;
  }
}
