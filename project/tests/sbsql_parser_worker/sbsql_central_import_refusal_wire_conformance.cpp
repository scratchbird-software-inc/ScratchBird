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

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace database = scratchbird::storage::database;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) Fail(message);
}

std::filesystem::path MakeFixtureDatabase() {
  static std::atomic<std::uint64_t> identity_time{1788300000000ULL};
  std::string template_path = "/tmp/sbsql_central_import_refusal.XXXXXX";
  std::vector<char> writable(template_path.begin(), template_path.end());
  writable.push_back('\0');
  char* directory = ::mkdtemp(writable.data());
  Require(directory != nullptr,
          "central-import fixture directory was not created");

  const auto database_uuid = uuid::GenerateEngineIdentityV7(
      UuidKind::database, identity_time.fetch_add(2));
  const auto filespace_uuid = uuid::GenerateEngineIdentityV7(
      UuidKind::filespace, identity_time.fetch_add(2));
  Require(database_uuid.ok() && filespace_uuid.ok(),
          "central-import fixture identities were not issued");

  const std::filesystem::path path =
      std::filesystem::path(directory) / "central_import_refusal.sbdb";
  database::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid = database_uuid.value;
  create.filespace_uuid = filespace_uuid.value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = identity_time.fetch_add(2);
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  const auto created = database::CreateDatabaseFile(create);
  Require(created.ok() &&
              created.create_finality ==
                  database::DatabaseCreateFinalityClass::committed,
          "central-import fixture database was not durably published");
  return path;
}

using FileSnapshot = std::map<std::string, std::vector<std::uint8_t>>;

FileSnapshot CaptureFixtureBytes(const std::filesystem::path& root) {
  FileSnapshot snapshot;
  std::error_code error;
  for (std::filesystem::recursive_directory_iterator iterator(root, error), end;
       iterator != end && !error; iterator.increment(error)) {
    if (!iterator->is_regular_file(error)) {
      Require(!error, "central-import fixture file type lookup failed");
      continue;
    }
    std::ifstream input(iterator->path(), std::ios::binary);
    Require(input.good(), "central-import fixture file could not be opened");
    std::vector<std::uint8_t> bytes(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    snapshot.emplace(
        std::filesystem::relative(iterator->path(), root).generic_string(),
        std::move(bytes));
  }
  Require(!error, "central-import fixture traversal failed");
  return snapshot;
}

std::string DiagnosticField(
    const scratchbird::parser::ipc::Diagnostic& diagnostic,
    std::string_view name) {
  for (const auto& field : diagnostic.fields) {
    if (field.name == name) return field.value;
  }
  return {};
}

struct RefusalCase {
  std::string_view sql;
  std::string_view surface_id;
  std::string_view canonical_name;
};

constexpr std::array<RefusalCase, 14> kRefusalCases{{
    {"COPY customer TO STDOUT", "SBSQL-BDC2B64DA2A9", "copy_endpoint"},
    {"COPY customer FROM STDIN JSONL", "SBSQL-2DDA6BFD9B65", "copy_format"},
    {"COPY customer FROM STDIN WITH HEADER", "SBSQL-4369855D2FC4", "copy_options"},
    {"COPY customer FROM LOCATION source", "SBSQL-D19FE1151601", "copy_source"},
    {"CYPHER LOAD CSV FROM source INTO social", "SBSQL-B7DCE9CB07B6", "cypher_load_csv"},
    {"GPU WORKLOAD APPLY batches", "SBSQL-7254347122CB", "gpu_workload_action"},
    {"LOAD DATA INTO customer FROM source CSV", "SBSQL-DB993AE8EDBB", "load_data_clause"},
    {"UUID TO NAME target", "SBSQL-2B7126C58E41", "uuid_to_name"},
    {"USE qa_lifecycle", "SBSQL-5B1C5630A433", "use_database_alias"},
    {"RESOLVE NAME PUBLIC target", "SBSQL-5E6DC360F377", "resolve_name_public"},
    {"DISCONNECT SESSION 1", "SBSQL-71D1C5165313", "disconnect_session"},
    {"CREATE OBJECT target", "SBSQL-A8E627E27375", "create_object"},
    {"CONNECT SESSION user", "SBSQL-DC0192B217F7", "connect_session"},
    {"SET SESSION work_mem", "SBSQL-F6C4E9705A12", "set_session"},
}};

}  // namespace

int main() {
  using namespace scratchbird::parser::sbsql;

  const auto fixture_database = MakeFixtureDatabase();
  ParserConfig config;
  config.probe_mode = true;
  config.embedded_engine_direct = true;
  config.allow_uncredentialed_fixture_database = true;
  config.embedded_auth_bypass_sysarch = true;
  config.embedded_database_path = fixture_database.string();
  ParserMetrics metrics;
  SblrTemplateCache cache;

  {
    SbsqlTestWireSession session(config, &metrics, &cache);
    const auto authenticated = session.HandleLine("AUTH");
    Require(authenticated.text.find("OK AUTHENTICATED") != std::string::npos,
            "central-import wire fixture did not authenticate");
    const auto baseline = CaptureFixtureBytes(fixture_database.parent_path());

    for (const auto& row : kRefusalCases) {
      const auto result = session.RunPipeline(row.sql, true);
      Require(!result.accepted && result.messages.diagnostics.size() == 1,
              "central-import wire route did not produce one exact refusal");
      const auto& diagnostic = result.messages.diagnostics.front();
      Require(diagnostic.code == "SBSQL.IMPL.NOT_AVAILABLE" &&
                  diagnostic.severity == "ERROR" &&
                  DiagnosticField(diagnostic, "surface_id") == row.surface_id &&
                  DiagnosticField(diagnostic, "canonical_name") ==
                      row.canonical_name &&
                  DiagnosticField(diagnostic, "executable_sblr_emitted") ==
                      "false",
              "central-import wire diagnostic identity or fields drifted");
      Require(result.sblr_payload.empty() &&
                  result.server_operation_id.empty() &&
                  result.server_result_payload.empty() &&
                  result.server_row_count == 0 &&
                  result.server_affected_rows == 0 &&
                  !result.server_affected_rows_present &&
                  !result.parser_executes_sql &&
                  !result.cached_storage_authority &&
                  !result.cached_authorization_authority &&
                  !result.cached_finality_authority,
              "central-import refusal crossed an execution or authority boundary");
      Require(CaptureFixtureBytes(fixture_database.parent_path()) == baseline,
              "central-import refusal changed durable fixture bytes");
    }
  }

  std::error_code cleanup_error;
  std::filesystem::remove_all(fixture_database.parent_path(), cleanup_error);
  Require(!cleanup_error, "central-import fixture cleanup failed");
  std::cout << "sbsql_central_import_refusal_wire_conformance=passed\n";
  return EXIT_SUCCESS;
}
