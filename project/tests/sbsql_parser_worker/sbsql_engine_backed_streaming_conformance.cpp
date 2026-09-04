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
#include "wire/sbsql_test_wire.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace database = scratchbird::storage::database;
namespace memory = scratchbird::core::memory;
namespace sbsql = scratchbird::parser::sbsql;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) Fail(message);
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

void PrintMessages(const sbsql::MessageVectorSet& messages) {
  for (const auto& diagnostic : messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message;
    for (const auto& field : diagnostic.fields) {
      std::cerr << ' ' << field.name << '=' << field.value;
    }
    std::cerr << '\n';
  }
}

struct FixtureDatabase {
  std::filesystem::path directory;
  std::filesystem::path path;

  FixtureDatabase() = default;
  FixtureDatabase(const FixtureDatabase&) = delete;
  FixtureDatabase& operator=(const FixtureDatabase&) = delete;
  FixtureDatabase(FixtureDatabase&& other) noexcept
      : directory(std::move(other.directory)), path(std::move(other.path)) {
    other.directory.clear();
  }

  ~FixtureDatabase() {
    std::error_code ignored;
    if (!directory.empty()) std::filesystem::remove_all(directory, ignored);
  }
};

FixtureDatabase CreateFixtureDatabase() {
  static std::atomic<std::uint64_t> identity_time{1784202000000ULL};
  FixtureDatabase fixture;
  const auto nonce = static_cast<std::uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  fixture.directory = std::filesystem::temp_directory_path() /
                      ("sb_engine_backed_streaming_" + std::to_string(nonce));
  std::filesystem::create_directories(fixture.directory);
  fixture.path = fixture.directory / "streaming.sbdb";

  const auto database_uuid = uuid::GenerateEngineIdentityV7(
      UuidKind::database, identity_time.fetch_add(2));
  const auto filespace_uuid = uuid::GenerateEngineIdentityV7(
      UuidKind::filespace, identity_time.fetch_add(2));
  Require(database_uuid.ok() && filespace_uuid.ok(),
          "engine-backed fixture UUID generation failed");

  database::DatabaseCreateConfig create;
  create.path = fixture.path.string();
  create.database_uuid = database_uuid.value;
  create.filespace_uuid = filespace_uuid.value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = identity_time.fetch_add(2);
  create.resource_seed_pack_root = SB_BOOTSTRAP_SEED_PACK_ROOT;
  create.allow_minimal_resource_bootstrap = false;
  create.require_resource_seed_pack = true;
  const auto created = database::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok() &&
              created.create_finality ==
                  database::DatabaseCreateFinalityClass::committed,
          "engine-backed fixture database was not durably published");
  return fixture;
}

void Authenticate(sbsql::SbsqlTestWireSession* parser,
                  const std::filesystem::path& database_path) {
  sbsql::AuthCredentialEnvelope credentials;
  credentials.requested_database = database_path.string();
  sbsql::MessageVectorSet messages;
  const bool authenticated = parser->AuthenticateCredentials(credentials, &messages);
  if (!authenticated) PrintMessages(messages);
  Require(authenticated && parser->session().authenticated,
          "embedded engine-backed streaming authentication failed");
}

struct EngineBackedFixture {
  std::string_view sql;
  std::string_view operation_id;
  std::string_view expected_field;
};

void VerifyEngineBackedResult(sbsql::SbsqlTestWireSession* parser,
                              const EngineBackedFixture& fixture) {
  auto execute = parser->RunPipeline(fixture.sql, true, false);
  if (!execute.accepted) PrintMessages(execute.messages);
  Require(execute.accepted && execute.server_operation_id == fixture.operation_id,
          "engine-backed canonical result execute was rejected");
  Require(execute.server_cursor_uuid.empty(),
          "non-streaming engine result unexpectedly returned a cursor UUID");
  Require(Contains(execute.server_result_payload, fixture.expected_field),
          "engine-backed result did not expose the engine payload");
}

void VerifyEngineBackedCursor(sbsql::SbsqlTestWireSession* parser) {
  constexpr std::string_view kCanonicalSourceFreeCursorQuery =
      "SELECT key_a,COUNT(*),SUM(amount) FROM (VALUES (1,5), (1,7)) "
      "AS input(key_a,amount) GROUP BY key_a;";
  auto execute =
      parser->RunPipeline(kCanonicalSourceFreeCursorQuery, true, true);
  if (!execute.accepted) PrintMessages(execute.messages);
  Require(execute.accepted && execute.server_operation_id == "query.execute",
          "engine-backed canonical query cursor execute was rejected");
  Require(!execute.server_cursor_uuid.empty(),
          "engine-backed query did not return a cursor UUID");

  const auto fetch = parser->FetchCursorOnRoute(execute.server_cursor_uuid, 1);
  if (!fetch.accepted) PrintMessages(fetch.messages);
  Require(fetch.accepted && fetch.row_count == 1 && fetch.end_of_cursor &&
              !fetch.row_packet.empty(),
          "engine-backed fetch did not return its terminal row batch");
  Require(!parser->CloseCursorOnRoute(execute.server_cursor_uuid).accepted,
          "end-of-stream engine cursor retained live close authority");
}

}  // namespace

int main() {
  auto memory_policy = memory::DefaultLocalEngineMemoryPolicy();
  memory_policy.policy_name = "sb_engine_backed_streaming_conformance";
  const auto configured = memory::ConfigureDefaultMemoryManagerForFixture(
      memory_policy, "sb_engine_backed_streaming_conformance");
  Require(configured.ok(), "engine-backed memory manager configuration failed");

  auto fixture = CreateFixtureDatabase();
  sbsql::ParserConfig config;
  config.parser_uuid = "019f08a0-5200-7000-8000-000000000001";
  config.probe_mode = true;
  config.embedded_engine_direct = true;
  config.allow_uncredentialed_fixture_database = true;
  config.embedded_auth_bypass_sysarch = true;
  config.embedded_database_path = fixture.path.string();

  sbsql::ParserMetrics metrics;
  sbsql::SblrTemplateCache cache;
  sbsql::SbsqlTestWireSession parser(config, &metrics, &cache);
  Authenticate(&parser, fixture.path);

  constexpr EngineBackedFixture kFixtures[] = {
      {"SHOW VERSION;", "observability.show_version", "product=ScratchBird"},
      {"SHOW DATABASE;", "observability.show_database", "database_uuid="},
  };
  for (const auto& fixture_case : kFixtures) {
    VerifyEngineBackedResult(&parser, fixture_case);
  }
  VerifyEngineBackedCursor(&parser);

  std::cout << "sb_engine_backed_streaming_conformance=passed\n";
  return EXIT_SUCCESS;
}
