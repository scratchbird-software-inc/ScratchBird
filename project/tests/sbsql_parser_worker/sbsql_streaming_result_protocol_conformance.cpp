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

constexpr std::string_view kFiveRowQuery =
    "SELECT key_a, COUNT(*), SUM(amount) FROM "
    "(VALUES (0, 10), (1, 11), (2, 12), (3, 13), (4, 14)) "
    "AS input(key_a, amount) GROUP BY key_a;";

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) Fail(message);
}

void PrintMessages(const sbsql::MessageVectorSet& messages) {
  for (const auto& diagnostic : messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
}

bool HasDiagnostic(const sbsql::MessageVectorSet& messages,
                   std::string_view code) {
  for (const auto& diagnostic : messages.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
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
  FixtureDatabase& operator=(FixtureDatabase&&) = delete;

  ~FixtureDatabase() {
    std::error_code ignored;
    if (!directory.empty()) std::filesystem::remove_all(directory, ignored);
  }
};

FixtureDatabase CreateFixtureDatabase() {
  static std::atomic<std::uint64_t> identity_time{1784201000000ULL};
  FixtureDatabase fixture;
  const auto nonce = static_cast<std::uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  fixture.directory = std::filesystem::temp_directory_path() /
                      ("sb_streaming_result_protocol_" +
                       std::to_string(nonce));
  std::filesystem::create_directories(fixture.directory);
  fixture.path = fixture.directory / "streaming.sbdb";

  const auto database_uuid = uuid::GenerateEngineIdentityV7(
      UuidKind::database, identity_time.fetch_add(2));
  const auto filespace_uuid = uuid::GenerateEngineIdentityV7(
      UuidKind::filespace, identity_time.fetch_add(2));
  Require(database_uuid.ok() && filespace_uuid.ok(),
          "streaming fixture UUID generation failed");

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
          "streaming fixture database was not durably published");
  return fixture;
}

void Authenticate(sbsql::SbsqlTestWireSession* parser,
                  const std::filesystem::path& database_path) {
  sbsql::AuthCredentialEnvelope credentials;
  credentials.requested_database = database_path.string();
  sbsql::MessageVectorSet messages;
  const bool authenticated =
      parser->AuthenticateCredentials(credentials, &messages);
  if (!authenticated) PrintMessages(messages);
  Require(authenticated && parser->session().authenticated,
          "embedded canonical streaming authentication failed");
  Require(parser->session().local_transaction_id != 0 &&
              !parser->session().transaction_uuid.empty(),
          "embedded canonical streaming attach did not publish an active transaction");
  if (parser->session().admitted_parser_package_uuid.empty() ||
      parser->session().catalog_epoch == 0 ||
      parser->session().security_policy_epoch == 0 ||
      parser->session().descriptor_epoch == 0) {
    std::cerr << "streaming_session_scope="
              << parser->session().admitted_parser_package_uuid << ','
              << parser->session().catalog_epoch << ','
              << parser->session().security_policy_epoch << ','
              << parser->session().descriptor_epoch << '\n';
  }
}

sbsql::PipelineResult OpenCursor(sbsql::SbsqlTestWireSession* parser) {
  auto opened = parser->RunPipeline(kFiveRowQuery, true, true);
  if (!opened.accepted) PrintMessages(opened.messages);
  Require(opened.accepted && opened.server_operation_id == "query.execute",
          "canonical query.execute cursor request was rejected");
  Require(!opened.server_cursor_uuid.empty() && opened.server_row_count == 5,
          "canonical query.execute did not publish the five-row cursor");
  return opened;
}

}  // namespace

int main() {
  auto memory_policy = memory::DefaultLocalEngineMemoryPolicy();
  memory_policy.policy_name = "sb_streaming_result_protocol_conformance";
  const auto memory_configured =
      memory::ConfigureDefaultMemoryManagerForFixture(
          memory_policy, "sb_streaming_result_protocol_conformance");
  Require(memory_configured.ok(),
          "streaming fixture memory manager configuration failed");

  auto fixture = CreateFixtureDatabase();

  sbsql::ParserConfig config;
  config.parser_uuid = "019f08a0-5100-7000-8000-000000000001";
  config.probe_mode = true;
  config.embedded_engine_direct = true;
  config.allow_uncredentialed_fixture_database = true;
  config.embedded_auth_bypass_sysarch = true;
  config.embedded_database_path = fixture.path.string();

  sbsql::ParserMetrics metrics;
  sbsql::SblrTemplateCache cache;
  sbsql::SbsqlTestWireSession parser(config, &metrics, &cache);
  Authenticate(&parser, fixture.path);

  const auto cursor = OpenCursor(&parser);
  const auto first = parser.FetchCursorOnRoute(cursor.server_cursor_uuid, 2);
  if (!first.accepted) PrintMessages(first.messages);
  Require(first.accepted && first.row_count == 2 && !first.end_of_cursor &&
              !first.row_packet.empty(),
          "first canonical descriptor-bound fetch did not return two rows");

  const auto second = parser.FetchCursorOnRoute(cursor.server_cursor_uuid, 2);
  if (!second.accepted) PrintMessages(second.messages);
  Require(second.accepted && second.row_count == 2 && !second.end_of_cursor &&
              !second.row_packet.empty(),
          "second canonical descriptor-bound fetch did not return two rows");

  const auto third = parser.FetchCursorOnRoute(cursor.server_cursor_uuid, 2);
  if (!third.accepted) PrintMessages(third.messages);
  Require(third.accepted && third.row_count == 1 && third.end_of_cursor &&
              !third.row_packet.empty(),
          "final canonical descriptor-bound fetch did not return one row at EOS");

  const auto stale_after_eos =
      parser.FetchCursorOnRoute(cursor.server_cursor_uuid, 1);
  Require(!stale_after_eos.accepted &&
              HasDiagnostic(stale_after_eos.messages,
                            "SERVER.STREAM.DESCRIPTOR_STALE"),
          "post-EOS descriptor replay did not fail closed as stale");

  const auto bounded = OpenCursor(&parser);
  const auto clamped =
      parser.FetchCursorOnRoute(bounded.server_cursor_uuid, 1024);
  if (!clamped.accepted) PrintMessages(clamped.messages);
  Require(clamped.accepted && clamped.row_count == 4 &&
              !clamped.end_of_cursor,
          "parser did not enforce the server-issued four-row cursor bound");
  const auto bounded_final =
      parser.FetchCursorOnRoute(bounded.server_cursor_uuid, 1024);
  Require(bounded_final.accepted && bounded_final.row_count == 1 &&
              bounded_final.end_of_cursor,
          "descriptor-bounded cursor did not preserve its final row");

  const auto owned = OpenCursor(&parser);
  sbsql::ParserMetrics other_metrics;
  sbsql::SblrTemplateCache other_cache;
  sbsql::SbsqlTestWireSession other_parser(config, &other_metrics, &other_cache);
  const auto cross_session =
      other_parser.FetchCursorOnRoute(owned.server_cursor_uuid, 1);
  Require(!cross_session.accepted &&
              HasDiagnostic(cross_session.messages,
                            "SERVER.STREAM.DESCRIPTOR_REQUIRED"),
          "a parser session without the issued descriptor did not fail closed");
  Require(parser.CloseCursorOnRoute(owned.server_cursor_uuid).accepted,
          "descriptor-owning session could not close its cursor");
  const auto fetch_closed =
      parser.FetchCursorOnRoute(owned.server_cursor_uuid, 1);
  Require(!fetch_closed.accepted &&
              HasDiagnostic(fetch_closed.messages,
                            "SERVER.STREAM.DESCRIPTOR_REQUIRED"),
          "closed cursor retained parser-side descriptor authority");

  std::cout << "sb_streaming_result_protocol_conformance=passed\n";
  return EXIT_SUCCESS;
}
