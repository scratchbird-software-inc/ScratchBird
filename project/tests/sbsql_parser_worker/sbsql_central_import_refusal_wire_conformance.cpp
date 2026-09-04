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

constexpr std::array<RefusalCase, 50> kProceduralRefusalCases{{
    {"PSQL REPEAT LOOP;", "SBSQL-026A4D4C039B", "psql_repeat_stmt"},
    {"FORALL DML EXECUTE;", "SBSQL-02734A0F9F81", "forall_dml_or_execute"},
    {"PSQL FOR item IN range;", "SBSQL-036A5CE9F957", "psql_for_stmt"},
    {"DECLARE SUBROUTINE helper;", "SBSQL-07486BB23A2F", "declare_subroutine"},
    {"ROUTINE ATTRIBUTE DETERMINISTIC;", "SBSQL-0E3954A70810", "routine_attribute"},
    {"PSQL CALL helper();", "SBSQL-11A04416EEDE", "psql_call_stmt"},
    {"FORALL RANGE 1 TO 10;", "SBSQL-198EC86EF3E6", "forall_range"},
    {"PSQL FORALL item IN range;", "SBSQL-24F101012F9C", "psql_forall_stmt"},
    {"PSQL LEAVE loop_label;", "SBSQL-28A5C4933A91", "psql_leave_stmt"},
    {"SIGNAL INFO FIELD MESSAGE_TEXT;", "SBSQL-2B96962FC600", "signal_info_field"},
    {"COLON VARIABLE :v;", "SBSQL-2E73B3E7CB0A", "colon_variable"},
    {"DECLARE VARIABLE v INT;", "SBSQL-2E7EF3FB699A", "declare_variable"},
    {"EXCEPTION HANDLER WHEN ANY;", "SBSQL-375E2A7771C0", "exception_handler"},
    {"CALL TARGET LIST helper;", "SBSQL-3EDACF124EA2", "call_target_list"},
    {"PSQL OPEN CHANNEL events;", "SBSQL-3FE17C7E606A", "psql_open_channel_stmt"},
    {"RAISE SEVERITY WARNING;", "SBSQL-451E4A81B23D", "raise_severity"},
    {"LVALUE local_var;", "SBSQL-47E79B4B23EF", "lvalue"},
    {"DECLARE EXCEPTION ex;", "SBSQL-499A72248451", "declare_exception"},
    {"SIGNAL SQLSTATE '45000';", "SBSQL-4A737A655174", "signal"},
    {"VARIABLE DECL FORM v INT;", "SBSQL-4B4DAC62299D", "variable_decl_form"},
    {"SINGLE VAR FORM v;", "SBSQL-5AD1F33585EA", "single_var_form"},
    {"PSQL NULL;", "SBSQL-5AFD1BFCCEC8", "psql_null_stmt"},
    {"CALL ARG LIST a b;", "SBSQL-62256BEF9F1B", "call_arg_list"},
    {"ARG LIST a b;", "SBSQL-66B35A56EFF8", "arg_list"},
    {"PARAM MODE INOUT;", "SBSQL-6D4DE2A31C56", "param_mode"},
    {"EXCEPTION CONDITION LIST any;", "SBSQL-6EF52D5CB31E", "exception_condition_list"},
    {"ROUTINE BODY BEGIN END;", "SBSQL-6FABEBB2C400", "routine_body"},
    {"FOR RANGE FORM 1 TO 10;", "SBSQL-7177C130C2B7", "for_range_form"},
    {"PSQL RESIGNAL;", "SBSQL-7359F2775921", "psql_resignal_stmt"},
    {"PSQL DECLARE SECTION;", "SBSQL-74BE46D58008", "psql_declare_section"},
    {"PACKAGE BODY ITEM proc;", "SBSQL-769B003AF4F3", "package_body_item"},
    {"SIGNAL INFO ASSIGNMENT MESSAGE_TEXT;", "SBSQL-802635EDBB3A", "signal_info_assignment"},
    {"PACKAGE NAME pkg;", "SBSQL-81BCBF791042", "package_name"},
    {"PSQL WHILE cond LOOP;", "SBSQL-832C2821017E", "psql_while_stmt"},
    {"PSQL EMIT CHANNEL events;", "SBSQL-85A5F7E16A21", "psql_emit_channel_stmt"},
    {"PSQL ASSIGN var VALUE;", "SBSQL-8628143A198B", "psql_assignment"},
    {"PSQL GET DIAGNOSTICS;", "SBSQL-908F3A07EC23", "psql_get_diagnostics"},
    {"INTO TARGET LIST var;", "SBSQL-9164E0190F24", "into_target_list"},
    {"SIGNAL TARGET condition;", "SBSQL-91D6ECC8969F", "signal_target"},
    {"RAISE EXCEPTION;", "SBSQL-931C105F4478", "raise"},
    {"EXCEPTION DECLARATION ex;", "SBSQL-96CFEF2C7728", "exception_declaration"},
    {"RAISE OPTION MESSAGE_TEXT;", "SBSQL-A5437DC15591", "raise_option"},
    {"PSQL RETURN value;", "SBSQL-A5AA36E99CDB", "psql_return_stmt"},
    {"PSQL SIGNAL SQLSTATE;", "SBSQL-A61AE21E1DFC", "psql_signal_stmt"},
    {"DIAGNOSTIC FILTER condition;", "SBSQL-A61F84867DF2", "diagnostic_filter"},
    {"DIAGNOSTIC FAMILY exception;", "SBSQL-A67B68A9BB52", "diagnostic_family"},
    {"PSQL LOOP;", "SBSQL-AE02AD3F3CF7", "psql_loop_stmt"},
    {"PSQL RAISE WARNING;", "SBSQL-AFAE77165146", "psql_raise_stmt"},
    {"RETURN SHAPE scalar;", "SBSQL-AFF3B4857945", "return_shape"},
    {"EXCEPTION SECTION WHEN ANY;", "SBSQL-BA6B29FD2668", "exception_section"},
}};

constexpr std::array<RefusalCase, 11> kProcedureIrRefusalCases{{
    {"POST EVENT CHANNEL audit_channel PAYLOAD 'post-001'",
     "SBSQL-33A1149AB350", "post_event_stmt"},
    {"OPEN route_cur FOR SELECT 7 AS value;",
     "SBSQL-4A41A00C4F5C", "psql_open_cursor_stmt"},
    {"PSQL EXECUTE STATEMENT stmt;",
     "SBSQL-65DE8F82E1EB", "psql_execute_statement"},
    {"FETCH NEXT FROM route_cur;", "SBSQL-930016752278", "psql_fetch_stmt"},
    {"CLOSE route_cur;", "SBSQL-A4F34F00C071", "psql_close_cursor_stmt"},
    {"PSQL IF STMT branch;", "SBSQL-CD6D9CB540EC", "psql_if_stmt"},
    {"RESIGNAL condition;", "SBSQL-D22F75D62CC7", "resignal"},
    {"PSQL SUSPEND STMT yield;",
     "SBSQL-EBFDBD3C1F98", "psql_suspend_stmt"},
    {"PSQL PIPE ROW STMT result;",
     "SBSQL-F178404D32D6", "psql_pipe_row_stmt"},
    {"PSQL EXIT STMT loop;", "SBSQL-F5E78906D903", "psql_exit_stmt"},
    {"PSQL CONTINUE STMT loop;",
     "SBSQL-FEE85792235D", "psql_continue_stmt"},
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

    const auto require_refusal = [&](const RefusalCase& row) {
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
              std::string("central-import wire diagnostic identity or fields drifted for ") +
                  std::string(row.surface_id) + " observed=" + diagnostic.code +
                  " surface=" + DiagnosticField(diagnostic, "surface_id") +
                  " name=" + DiagnosticField(diagnostic, "canonical_name"));
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
    };
    for (const auto& row : kRefusalCases) {
      require_refusal(row);
    }
    for (const auto& row : kProceduralRefusalCases) {
      require_refusal(row);
    }
    for (const auto& row : kProcedureIrRefusalCases) {
      require_refusal(row);
    }
  }

  std::error_code cleanup_error;
  std::filesystem::remove_all(fixture_database.parent_path(), cleanup_error);
  Require(!cleanup_error, "central-import fixture cleanup failed");
  std::cout << "sbsql_central_import_refusal_wire_conformance=passed\n";
  return EXIT_SUCCESS;
}
