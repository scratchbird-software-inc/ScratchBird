// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "uuid.hpp"
#include "wire/sbsql_test_wire.hpp"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

namespace {

namespace database = scratchbird::storage::database;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

std::filesystem::path MakeFixtureDatabase() {
  static std::atomic<std::uint64_t> identity_time{1788200000000ULL};
  std::string template_path = "/tmp/sbp_sbsql_pipeline_probe.XXXXXX";
  std::vector<char> writable(template_path.begin(), template_path.end());
  writable.push_back('\0');
  char* directory = ::mkdtemp(writable.data());
  if (directory == nullptr) return {};

  const auto database_uuid = uuid::GenerateEngineIdentityV7(
      UuidKind::database, identity_time.fetch_add(2));
  const auto filespace_uuid = uuid::GenerateEngineIdentityV7(
      UuidKind::filespace, identity_time.fetch_add(2));
  if (!database_uuid.ok() || !filespace_uuid.ok()) {
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    return {};
  }

  const std::filesystem::path path =
      std::filesystem::path(directory) / "pipeline_probe.sbdb";
  database::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid = database_uuid.value;
  create.filespace_uuid = filespace_uuid.value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = identity_time.fetch_add(2);
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  const auto created = database::CreateDatabaseFile(create);
  if (!created.ok() ||
      created.create_finality !=
          database::DatabaseCreateFinalityClass::committed) {
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    return {};
  }
  return path;
}

void PrintMessages(
    const scratchbird::parser::sbsql::MessageVectorSet& messages) {
  for (const auto& diagnostic : messages.diagnostics) {
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
}

}  // namespace

int main() {
  using namespace scratchbird::parser::sbsql;
  const auto fixture_database = MakeFixtureDatabase();
  if (fixture_database.empty()) {
    std::cerr << "parser pipeline fixture database creation failed\n";
    return EXIT_FAILURE;
  }

  ParserConfig config;
  config.probe_mode = true;
  config.embedded_engine_direct = true;
  config.allow_uncredentialed_fixture_database = true;
  config.embedded_auth_bypass_sysarch = true;
  config.embedded_database_path = fixture_database.string();
  ParserMetrics metrics;
  SblrTemplateCache cache;
  bool accepted = false;
  {
    SbsqlTestWireSession session(config, &metrics, &cache);
    const auto authenticated = session.HandleLine("AUTH");
    if (authenticated.text.find("OK AUTHENTICATED") == std::string::npos) {
      std::cerr << authenticated.text;
    } else {
      const auto result = session.RunPipeline("select 1", true);
      if (!result.accepted) PrintMessages(result.messages);
      accepted = result.accepted && !result.sblr_payload.empty() &&
                 !result.messages.has_errors() &&
                 result.server_operation_id == "query.execute";
    }
  }

  std::error_code cleanup_error;
  std::filesystem::remove_all(fixture_database.parent_path(), cleanup_error);
  if (!accepted) {
    std::cerr << "parser pipeline probe failed\n";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
