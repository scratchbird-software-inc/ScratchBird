// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "../support/binary_uuid_fixture.hpp"
#include "../../src/wire/public_result_packet.hpp"
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
#include <vector>

namespace {

namespace database = scratchbird::storage::database;
namespace memory = scratchbird::core::memory;
namespace sbsql = scratchbird::parser::sbsql;
namespace uuid = scratchbird::core::uuid;
namespace packet = scratchbird::wire::public_result;
using scratchbird::core::platform::UuidKind;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) Fail(message);
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
  scratchbird::core::platform::Uuid database_uuid;

  FixtureDatabase() = default;
  FixtureDatabase(const FixtureDatabase&) = delete;
  FixtureDatabase& operator=(const FixtureDatabase&) = delete;
  FixtureDatabase(FixtureDatabase&& other) noexcept
      : directory(std::move(other.directory)), path(std::move(other.path)),
        database_uuid(other.database_uuid) {
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
  fixture.database_uuid = database_uuid.value.value;

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
  std::vector<packet::Field> fields;
};

bool MatchesResult(std::string_view bytes, const EngineBackedFixture& expected) {
  const auto operation = packet::Find(bytes, "operation_id");
  const auto count = packet::Find(bytes, "row_count");
  const auto row = packet::Find(bytes, "row[0]");
  if (!operation || operation->kind != packet::Kind::text ||
      operation->value != expected.operation_id || !count ||
      count->kind != packet::Kind::text || count->value != "1" ||
      !row || row->kind != packet::Kind::row) return false;
  std::vector<packet::Field> outer, fields;
  if (!packet::Decode(bytes, &outer) || !packet::Decode(row->value, &fields) ||
      fields.size() != expected.fields.size()) return false;
  for (const auto& field : outer)
    if (field.kind == packet::Kind::row && field.name != "row[0]") return false;
  for (const auto& field : expected.fields) {
    const auto actual = packet::Find(row->value, field.name);
    if (!actual || actual->kind != field.kind || actual->value != field.value)
      return false;
  }
  return true;
}

void CheckNativeResultOracle() {
  // Independent length framing: neither server serializer nor packet encoder.
  const auto frame = [](const std::vector<packet::Field>& fields) {
    std::string bytes = "SBRES002";
    const auto number = [&](std::uint64_t value, unsigned width) {
      for (unsigned i = 0; i < width; ++i)
        bytes.push_back(static_cast<char>(value >> (8 * i)));
    };
    number(fields.size(), 4);
    for (const auto& field : fields) {
      number(field.name.size(), 4); bytes += field.name;
      number(static_cast<unsigned>(field.kind), 1);
      number(field.value.size(), 8); bytes += field.value;
    }
    return bytes;
  };
  const auto identity = scratchbird::tests::FixtureUuidLiteral(
      "019f08a0-5200-7000-8000-000000000002");
  const std::string raw(reinterpret_cast<const char*>(identity.bytes.data()), 16);
  const EngineBackedFixture expected{"", "observability.show_database",
      {{"database_uuid", packet::Kind::uuid, raw}}};
  const auto wrap = [&](const std::vector<packet::Field>& fields) {
    return frame({{"operation_id", packet::Kind::text, std::string(expected.operation_id)},
                  {"row_count", packet::Kind::text, "1"},
                  {"row[0]", packet::Kind::row, frame(fields)}});
  };
  const auto valid = wrap(expected.fields);
  Require(MatchesResult(valid, expected), "independent binary result rejected");
  for (std::size_t n = 0; n < valid.size(); ++n)
    Require(!MatchesResult(std::string_view(valid).substr(0, n), expected),
            "truncated binary result accepted");
  Require(!MatchesResult(valid + 'x', expected), "trailing binary result accepted");
  Require(!MatchesResult(wrap({}), expected), "missing UUID field accepted");
  auto fields = expected.fields;
  fields.push_back(fields.front());
  Require(!MatchesResult(wrap(fields), expected), "duplicate UUID field accepted");
  fields = expected.fields; fields[0].kind = packet::Kind::text;
  Require(!MatchesResult(wrap(fields), expected), "UUID bytes tagged as text accepted");
  fields[0].value = "019f08a0-5200-7000-8000-000000000002";
  Require(!MatchesResult(wrap(fields), expected), "text UUID fallback accepted");
  fields = expected.fields; fields[0].value[15] ^= 1;
  Require(!MatchesResult(wrap(fields), expected), "foreign database UUID accepted");
  fields = expected.fields; fields[0].value.pop_back();
  Require(!MatchesResult(wrap(fields), expected), "short UUID accepted");
}

void VerifyEngineBackedResult(sbsql::SbsqlTestWireSession* parser,
                              const EngineBackedFixture& fixture) {
  auto execute = parser->RunPipeline(fixture.sql, true, false);
  if (!execute.accepted) PrintMessages(execute.messages);
  Require(execute.accepted && execute.server_operation_id == fixture.operation_id,
          "engine-backed canonical result execute was rejected");
  Require(execute.server_cursor_uuid.is_nil(),
          "non-streaming engine result unexpectedly returned a cursor UUID");
  Require(execute.server_row_count == 1 &&
              MatchesResult(execute.server_result_payload, fixture),
          "engine-backed result did not expose the exact native typed row");
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
  Require(!execute.server_cursor_uuid.is_nil(),
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
  CheckNativeResultOracle();
  auto memory_policy = memory::DefaultLocalEngineMemoryPolicy();
  memory_policy.policy_name = "sb_engine_backed_streaming_conformance";
  const auto configured = memory::ConfigureDefaultMemoryManagerForFixture(
      memory_policy, "sb_engine_backed_streaming_conformance");
  Require(configured.ok(), "engine-backed memory manager configuration failed");

  auto fixture = CreateFixtureDatabase();
  sbsql::ParserConfig config;
  config.parser_uuid = scratchbird::tests::FixtureUuidLiteral("019f08a0-5200-7000-8000-000000000001");
  config.probe_mode = true;
  config.embedded_engine_direct = true;
  config.allow_uncredentialed_fixture_database = true;
  config.embedded_auth_bypass_sysarch = true;
  config.embedded_database_path = fixture.path.string();

  sbsql::ParserMetrics metrics;
  sbsql::SblrTemplateCache cache;
  sbsql::SbsqlTestWireSession parser(config, &metrics, &cache);
  Authenticate(&parser, fixture.path);

  const std::string database_bytes(
      reinterpret_cast<const char*>(fixture.database_uuid.bytes.data()), 16);
  const EngineBackedFixture kFixtures[] = {
      {"SHOW VERSION;", "observability.show_version",
       {{"product", packet::Kind::text, "ScratchBird"},
        {"component", packet::Kind::text, "sb_engine"},
        {"api", packet::Kind::text, "1.0"}}},
      {"SHOW DATABASE;", "observability.show_database",
       {{"database_path", packet::Kind::text, fixture.path.string()},
        {"database_uuid", packet::Kind::uuid, database_bytes},
        {"page_size_bytes", packet::Kind::text, "16384"},
        {"cluster_authority_active", packet::Kind::text, "false"}}},
  };
  for (const auto& fixture_case : kFixtures) {
    VerifyEngineBackedResult(&parser, fixture_case);
  }
  VerifyEngineBackedCursor(&parser);

  std::cout << "sb_engine_backed_streaming_conformance=passed\n";
  return EXIT_SUCCESS;
}
