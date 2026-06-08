// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "memory.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

namespace db = scratchbird::storage::database;
namespace memory = scratchbird::core::memory;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

constexpr scratchbird::core::platform::u64 kExampleCreationMillis = 1767225600000ull;
constexpr scratchbird::core::platform::u32 kExamplePageSize = 16384;

struct Args {
  std::filesystem::path output;
  std::filesystem::path manifest;
  std::filesystem::path resource_seed_pack_root;
  bool overwrite = false;
};

void Usage() {
  std::cerr << "usage: public_example_database_seed --output PATH --manifest PATH "
               "--resource-seed-pack-root PATH [--overwrite]\n";
}

bool ParseArgs(int argc, char** argv, Args* args) {
  for (int i = 1; i < argc; ++i) {
    const std::string key = argv[i];
    if (key == "--overwrite") {
      args->overwrite = true;
      continue;
    }
    if (i + 1 >= argc) {
      return false;
    }
    const std::string value = argv[++i];
    if (key == "--output") {
      args->output = value;
    } else if (key == "--manifest") {
      args->manifest = value;
    } else if (key == "--resource-seed-pack-root") {
      args->resource_seed_pack_root = value;
    } else {
      return false;
    }
  }
  return !args->output.empty() && !args->manifest.empty() &&
         !args->resource_seed_pack_root.empty();
}

memory::AllocationPolicy ProductionMemoryPolicy() {
  memory::AllocationPolicy policy;
  policy.policy_name = "public_example_database_seed";
  policy.hard_limit_bytes = 128ull * 1024ull * 1024ull;
  policy.soft_limit_bytes = 96ull * 1024ull * 1024ull;
  policy.per_context_limit_bytes = 64ull * 1024ull * 1024ull;
  policy.page_buffer_pool_limit_bytes = 32ull * 1024ull * 1024ull;
  policy.track_allocations = true;
  policy.zero_memory_on_release = true;
  return policy;
}

bool ConfigureMemory() {
  const auto configured =
      memory::ConfigureDefaultMemoryManager(ProductionMemoryPolicy(),
                                            "public_example_database_seed_startup_policy");
  if (!configured.ok() || configured.fixture_mode) {
    std::cerr << "public example database seed memory configuration failed\n";
    return false;
  }
  return true;
}

scratchbird::core::platform::TypedUuid MakeIdentity(UuidKind kind,
                                                    scratchbird::core::platform::u64 millis) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, millis);
  if (!generated.ok()) {
    std::cerr << generated.diagnostic.diagnostic_code << ':'
              << generated.diagnostic.message_key << '\n';
    std::exit(EXIT_FAILURE);
  }
  return generated.value;
}

bool WriteManifest(const Args& args,
                   const scratchbird::core::platform::TypedUuid& database_uuid,
                   const scratchbird::core::platform::TypedUuid& filespace_uuid,
                   bool resource_seed_catalog_present) {
  std::filesystem::create_directories(args.manifest.parent_path());
  std::ofstream out(args.manifest, std::ios::trunc);
  if (!out) {
    std::cerr << "failed to open example database manifest: " << args.manifest << '\n';
    return false;
  }

  out << "{\n";
  out << "  \"example_database\": \"" << args.output.filename().string() << "\",\n";
  out << "  \"database_uuid\": \"" << uuid::UuidToString(database_uuid.value) << "\",\n";
  out << "  \"filespace_uuid\": \"" << uuid::UuidToString(filespace_uuid.value) << "\",\n";
  out << "  \"page_size\": " << kExamplePageSize << ",\n";
  out << "  \"creation_unix_epoch_millis\": " << kExampleCreationMillis << ",\n";
  out << "  \"resource_seed_catalog_present\": "
      << (resource_seed_catalog_present ? "true" : "false") << ",\n";
  out << "  \"cluster_execution\": \"no_cluster_fail_closed\",\n";
  out << "  \"authority\": {\n";
  out << "    \"database_identity\": \"engine_uuid\",\n";
  out << "    \"execution_surface\": \"storage_database_lifecycle_api\",\n";
  out << "    \"sql_text_authority\": false,\n";
  out << "    \"transaction_authority\": \"mga_inventory\"\n";
  out << "  }\n";
  out << "}\n";
  return static_cast<bool>(out);
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!ParseArgs(argc, argv, &args)) {
    Usage();
    return EXIT_FAILURE;
  }

  if (!std::filesystem::exists(args.resource_seed_pack_root)) {
    std::cerr << "resource seed pack root is missing: " << args.resource_seed_pack_root << '\n';
    return EXIT_FAILURE;
  }
  if (std::filesystem::exists(args.output)) {
    if (!args.overwrite) {
      std::cerr << "example database already exists: " << args.output << '\n';
      return EXIT_FAILURE;
    }
    std::filesystem::remove(args.output);
  }
  std::filesystem::create_directories(args.output.parent_path());

  if (!ConfigureMemory()) {
    return EXIT_FAILURE;
  }

  const auto database_uuid = MakeIdentity(UuidKind::database, kExampleCreationMillis);
  const auto filespace_uuid = MakeIdentity(UuidKind::filespace, kExampleCreationMillis + 1);

  db::DatabaseCreateConfig create;
  create.path = args.output.string();
  create.database_uuid = database_uuid;
  create.filespace_uuid = filespace_uuid;
  create.page_size = kExamplePageSize;
  create.creation_unix_epoch_millis = kExampleCreationMillis;
  create.resource_seed_pack_root = args.resource_seed_pack_root.string();
  create.require_resource_seed_pack = true;
  create.allow_minimal_resource_bootstrap = false;
  create.allow_overwrite = false;

  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
    return EXIT_FAILURE;
  }

  db::DatabaseOpenConfig open;
  open.path = args.output.string();
  open.suppress_background_agents = true;
  const auto opened = db::OpenDatabaseFile(open);
  if (!opened.ok()) {
    std::cerr << opened.diagnostic.diagnostic_code << ':'
              << opened.diagnostic.message_key << '\n';
    return EXIT_FAILURE;
  }

  if (!WriteManifest(args,
                     database_uuid,
                     filespace_uuid,
                     opened.state.resource_seed_catalog_present)) {
    return EXIT_FAILURE;
  }

  std::cout << "public_example_database_seed=passed database="
            << args.output.filename().string() << '\n';
  return EXIT_SUCCESS;
}
