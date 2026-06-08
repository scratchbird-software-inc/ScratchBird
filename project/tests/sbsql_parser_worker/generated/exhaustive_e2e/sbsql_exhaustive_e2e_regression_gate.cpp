// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_admission.hpp"
#include "sblr_dispatch_server.hpp"
#include "session_registry.hpp"
#include "sbu_sbsql_parser_support.hpp"
#include "lifecycle/engine_lifecycle_api.hpp"
#include "memory.hpp"
#include "transaction/transaction_api.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

using CsvRow = std::unordered_map<std::string, std::string>;
namespace api = scratchbird::engine::internal_api;
namespace memory = scratchbird::core::memory;

struct CsvTable {
  std::filesystem::path path;
  std::vector<std::string> headers;
  std::vector<CsvRow> rows;
};

struct Harness {
  bool ok{true};
  std::size_t failures{0};

  void Check(bool condition, std::string message) {
    if (condition) return;
    ok = false;
    if (failures < 120) std::cerr << message << '\n';
    ++failures;
  }
};

namespace sbps = scratchbird::server::sbps;

constexpr const char* kDynamicRouteDatabaseUuid = "019e05ef-f015-7000-8000-000000000001";
constexpr const char* kDynamicRouteSysVersionUuid = "b4a0fd27-e19b-7719-9105-5882443ee2bc";

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

memory::AllocationPolicy MemoryPolicy() {
  memory::AllocationPolicy policy;
  policy.policy_name = "sbsql_exhaustive_e2e_dynamic_route";
  policy.hard_limit_bytes = 64ull * 1024ull * 1024ull;
  policy.soft_limit_bytes = 48ull * 1024ull * 1024ull;
  policy.per_context_limit_bytes = 32ull * 1024ull * 1024ull;
  policy.page_buffer_pool_limit_bytes = 16ull * 1024ull * 1024ull;
  policy.track_allocations = true;
  policy.zero_memory_on_release = true;
  return policy;
}

void ConfigureMemoryFixture(Harness* harness) {
  const auto configured = memory::ConfigureDefaultMemoryManagerForFixture(
      MemoryPolicy(), "sbsql_exhaustive_e2e_regression_gate");
  harness->Check(configured.ok(),
                 "exhaustive E2E memory fixture configuration failed");
  harness->Check(configured.fixture_mode,
                 "exhaustive E2E memory fixture mode was not active");
}

std::filesystem::path MakeTempDir() {
  std::string tmpl = "/tmp/sbsql_exhaustive_e2e.XXXXXX";
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  return made == nullptr ? std::filesystem::path{} : std::filesystem::path(made);
}

void TrimLineEnding(std::string* line) {
  while (!line->empty() && (line->back() == '\r' || line->back() == '\n')) {
    line->pop_back();
  }
}

std::vector<std::string> SplitCsvLine(std::string_view line) {
  std::vector<std::string> fields;
  std::string current;
  bool in_quotes = false;

  for (std::size_t index = 0; index < line.size(); ++index) {
    const char ch = line[index];
    if (in_quotes) {
      if (ch == '"') {
        if (index + 1 < line.size() && line[index + 1] == '"') {
          current.push_back('"');
          ++index;
        } else {
          in_quotes = false;
        }
      } else {
        current.push_back(ch);
      }
      continue;
    }
    if (ch == '"') {
      in_quotes = true;
    } else if (ch == ',') {
      fields.push_back(current);
      current.clear();
    } else {
      current.push_back(ch);
    }
  }
  fields.push_back(current);
  return fields;
}

CsvTable ReadCsv(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("failed to open " + path.string());

  std::string line;
  if (!std::getline(input, line)) throw std::runtime_error("empty CSV " + path.string());
  TrimLineEnding(&line);

  CsvTable table;
  table.path = path;
  table.headers = SplitCsvLine(line);

  std::size_t line_number = 1;
  while (std::getline(input, line)) {
    ++line_number;
    TrimLineEnding(&line);
    if (line.empty()) continue;
    const auto fields = SplitCsvLine(line);
    if (fields.size() != table.headers.size()) {
      throw std::runtime_error(path.string() + ":" + std::to_string(line_number) +
                               " has " + std::to_string(fields.size()) +
                               " fields for " + std::to_string(table.headers.size()) +
                               " headers");
    }

    CsvRow row;
    for (std::size_t index = 0; index < table.headers.size(); ++index) {
      row.emplace(table.headers[index], fields[index]);
    }
    table.rows.push_back(std::move(row));
  }

  return table;
}

std::vector<std::string> ReadLines(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("failed to open " + path.string());
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(input, line)) {
    TrimLineEnding(&line);
    if (!line.empty()) lines.push_back(line);
  }
  return lines;
}

std::string_view Field(const CsvRow& row, std::string_view column) {
  const auto found = row.find(std::string(column));
  if (found == row.end()) return {};
  return found->second;
}

bool HasColumn(const CsvTable& table, std::string_view column) {
  return std::find(table.headers.begin(), table.headers.end(), column) != table.headers.end();
}

void RequireColumns(const CsvTable& table,
                    std::initializer_list<std::string_view> columns,
                    Harness* harness) {
  for (const auto column : columns) {
    harness->Check(HasColumn(table, column),
                   table.path.filename().string() + " missing required column " +
                       std::string(column));
  }
}

std::unordered_map<std::string, const CsvRow*> IndexUnique(const CsvTable& table,
                                                           std::string_view column,
                                                           Harness* harness) {
  std::unordered_map<std::string, const CsvRow*> index;
  for (const auto& row : table.rows) {
    const std::string key(Field(row, column));
    harness->Check(!key.empty(), table.path.filename().string() +
                                     " row has empty " + std::string(column));
    if (key.empty()) continue;
    const auto inserted = index.emplace(key, &row);
    harness->Check(inserted.second, table.path.filename().string() +
                                        " duplicate " + std::string(column) + " " + key);
  }
  return index;
}

std::string ExtractJsonString(std::string_view line, std::string_view key) {
  const std::string needle = "\"" + std::string(key) + "\":\"";
  const auto start = line.find(needle);
  if (start == std::string_view::npos) return {};

  std::string value;
  bool escaped = false;
  for (std::size_t index = start + needle.size(); index < line.size(); ++index) {
    const char ch = line[index];
    if (escaped) {
      value.push_back(ch);
      escaped = false;
      continue;
    }
    if (ch == '\\') {
      escaped = true;
      continue;
    }
    if (ch == '"') break;
    value.push_back(ch);
  }
  return value;
}

void CheckRoute(Harness* harness,
                const CsvRow& fixture,
                std::string_view route,
                std::string_view surface_id) {
  harness->Check(Contains(Field(fixture, "route_set"), route),
                 std::string("fixture for ") + std::string(surface_id) +
                     " missing route " + std::string(route));
}

void CheckVariationManifest(const CsvTable& matrix, Harness* harness) {
  RequireColumns(matrix,
                 {"scope_id", "source_path", "expected_count", "coverage_rule", "status"},
                 harness);
  const auto by_scope = IndexUnique(matrix, "scope_id", harness);
  const std::map<std::string, std::string> required = {
      {"surface_registry", "2617"},
      {"replay_fixture_index", "2617"},
      {"full_route_executable_surfaces", "2560"},
      {"exact_refusal_surfaces", "57"},
      {"expression_runtime_catalog", "1534"},
      {"statement_surface_catalog", "1049"},
      {"sblr_operation_families", "16"},
      {"engine_gap_backlog", "932"},
      {"donor_alias_backlog", "312"},
      {"dynamic_stored_procedure_udr_sblr", "1"},
  };
  for (const auto& [scope, expected_count] : required) {
    const auto found = by_scope.find(scope);
    harness->Check(found != by_scope.end(), "variation matrix missing scope " + scope);
    if (found == by_scope.end()) continue;
    harness->Check(Field(*found->second, "expected_count") == expected_count,
                   "variation matrix count mismatch for " + scope);
    harness->Check(Field(*found->second, "status") == "ready",
                   "variation matrix scope is not ready: " + scope);
  }
}

std::unordered_map<std::string, std::string> ReadPayloadSurfaceIndex(
    const std::vector<std::string>& payload_lines,
    Harness* harness) {
  std::unordered_map<std::string, std::string> index;
  for (const auto& line : payload_lines) {
    const auto fixture_id = ExtractJsonString(line, "fixture_id");
    const auto surface_id = ExtractJsonString(line, "surface_id");
    harness->Check(!fixture_id.empty(), "expected payload line missing fixture_id");
    harness->Check(!surface_id.empty(), "expected payload line missing surface_id");
    if (fixture_id.empty() || surface_id.empty()) continue;
    const auto inserted = index.emplace(fixture_id, surface_id);
    harness->Check(inserted.second, "duplicate expected payload fixture_id " + fixture_id);
    harness->Check(Contains(line, "\"routes\""),
                   "expected payload missing route vector for " + fixture_id);
    harness->Check(Contains(line, "\"engine\""),
                   "expected payload missing engine oracle for " + fixture_id);
  }
  return index;
}

struct SurfaceCoverageSummary {
  std::size_t surfaces{0};
  std::size_t executable{0};
  std::size_t exact_refusals{0};
  std::size_t expression_rows{0};
  std::size_t statement_rows{0};
  std::size_t functions{0};
  std::size_t operators{0};
  std::size_t variables{0};
  std::set<std::string> operation_families;
};

void CheckFamilyCount(Harness* harness,
                      const std::map<std::string, std::size_t>& counts,
                      std::string family,
                      std::size_t expected) {
  const auto found = counts.find(family);
  const std::size_t actual = found == counts.end() ? 0 : found->second;
  harness->Check(actual == expected,
                 "surface family count mismatch for " + family + ": expected " +
                     std::to_string(expected) + " actual " + std::to_string(actual));
}

SurfaceCoverageSummary CheckSurfaceReplayCoverage(
    const CsvTable& surfaces,
    const CsvTable& surface_backlog,
    const CsvTable& batch_membership,
    const CsvTable& semantic_oracle,
    const CsvTable& fixture_index,
    const std::vector<std::string>& payload_lines,
    Harness* harness) {
  RequireColumns(surfaces,
                 {"surface_id", "canonical_name", "surface_kind", "family", "status",
                  "cluster_scope", "sblr_operation_family"},
                 harness);
  RequireColumns(surface_backlog,
                 {"surface_id", "validation_fixture_id", "final_acceptance_rule", "status"},
                 harness);
  RequireColumns(batch_membership,
                 {"surface_id", "batch_id", "validation_fixture_id", "status"},
                 harness);
  RequireColumns(semantic_oracle,
                 {"fixture_id", "surface_id", "oracle_type", "status"},
                 harness);
  RequireColumns(fixture_index,
                 {"fixture_id", "surface_id", "canonical_name", "family", "surface_kind",
                  "source_status", "cluster_scope", "operation_family", "route_set",
                  "input_text", "expected_engine_effect", "expected_payload_json", "status"},
                 harness);

  harness->Check(surfaces.rows.size() == 2617,
                 "surface registry row count changed from corrected authority baseline");
  harness->Check(surface_backlog.rows.size() == 2617,
                 "surface backlog row count changed from corrected authority baseline");
  harness->Check(batch_membership.rows.size() == 2617,
                 "batch membership row count changed from corrected authority baseline");
  harness->Check(semantic_oracle.rows.size() == 2617,
                 "semantic oracle row count changed from corrected authority baseline");
  harness->Check(fixture_index.rows.size() == 2617,
                 "differential replay fixture count changed from corrected authority baseline");
  harness->Check(payload_lines.size() == 2617,
                 "expected payload JSONL count changed from corrected authority baseline");

  const auto backlog_by_surface = IndexUnique(surface_backlog, "surface_id", harness);
  const auto batch_by_surface = IndexUnique(batch_membership, "surface_id", harness);
  const auto oracle_by_surface = IndexUnique(semantic_oracle, "surface_id", harness);
  const auto fixture_by_surface = IndexUnique(fixture_index, "surface_id", harness);
  const auto payload_surface_by_fixture = ReadPayloadSurfaceIndex(payload_lines, harness);

  SurfaceCoverageSummary summary;
  summary.surfaces = surfaces.rows.size();
  std::map<std::string, std::size_t> family_counts;

  for (const auto& surface : surfaces.rows) {
    const std::string surface_id(Field(surface, "surface_id"));
    harness->Check(backlog_by_surface.contains(surface_id),
                   "surface missing implementation backlog row " + surface_id);
    harness->Check(batch_by_surface.contains(surface_id),
                   "surface missing batch membership row " + surface_id);
    harness->Check(oracle_by_surface.contains(surface_id),
                   "surface missing semantic oracle row " + surface_id);
    const auto fixture_it = fixture_by_surface.find(surface_id);
    harness->Check(fixture_it != fixture_by_surface.end(),
                   "surface missing replay fixture row " + surface_id);
    if (fixture_it == fixture_by_surface.end()) continue;

    const auto& fixture = *fixture_it->second;
    const std::string fixture_id(Field(fixture, "fixture_id"));
    harness->Check(Field(fixture, "status") == "replay_ready",
                   "fixture is not replay_ready " + fixture_id);
    harness->Check(Field(fixture, "canonical_name") == Field(surface, "canonical_name"),
                   "fixture canonical_name drift for " + surface_id);
    harness->Check(Field(fixture, "family") == Field(surface, "family"),
                   "fixture family drift for " + surface_id);
    harness->Check(Field(fixture, "surface_kind") == Field(surface, "surface_kind"),
                   "fixture surface_kind drift for " + surface_id);
    harness->Check(Field(fixture, "operation_family") == Field(surface, "sblr_operation_family"),
                   "fixture operation_family drift for " + surface_id);
    harness->Check(!Field(fixture, "input_text").empty(),
                   "fixture input_text missing for " + surface_id);
    harness->Check(Contains(Field(fixture, "expected_payload_json"), fixture_id),
                   "fixture expected_payload_json does not point at fixture " + fixture_id);

    const auto payload_it = payload_surface_by_fixture.find(fixture_id);
    harness->Check(payload_it != payload_surface_by_fixture.end(),
                   "fixture missing expected payload line " + fixture_id);
    if (payload_it != payload_surface_by_fixture.end()) {
      harness->Check(payload_it->second == surface_id,
                     "expected payload surface_id drift for " + fixture_id);
    }

    CheckRoute(harness, fixture, "parser_parse_only", surface_id);
    CheckRoute(harness, fixture, "parser_bind_lower", surface_id);
    CheckRoute(harness, fixture, "diagnostic", surface_id);
    CheckRoute(harness, fixture, "server_admission", surface_id);

    const auto engine_effect = Field(fixture, "expected_engine_effect");
    if (engine_effect == "execute-sblr-internal-procedure-only-no-sql-text") {
      ++summary.executable;
      CheckRoute(harness, fixture, "udr_sql_to_sblr", surface_id);
      CheckRoute(harness, fixture, "engine_behavior", surface_id);
      CheckRoute(harness, fixture, "full_route", surface_id);
    } else if (engine_effect == "no-engine-mutation-exact-refusal" ||
               engine_effect == "no-engine-mutation;exact-refusal-or-profile-gate") {
      ++summary.exact_refusals;
      harness->Check(!Contains(Field(fixture, "route_set"), "engine_behavior"),
                     "exact-refusal fixture has engine_behavior route " + fixture_id);
      harness->Check(!Contains(Field(fixture, "route_set"), "full_route"),
                     "exact-refusal fixture has full_route route " + fixture_id);
    } else {
      harness->Check(false, "unknown expected_engine_effect for " + fixture_id);
    }

    const std::string family(Field(surface, "family"));
    ++family_counts[family];
    summary.operation_families.insert(std::string(Field(surface, "sblr_operation_family")));
    if (family == "expression_runtime") {
      ++summary.expression_rows;
      if (Field(surface, "surface_kind") == "function") ++summary.functions;
      if (Field(surface, "surface_kind") == "operator") ++summary.operators;
      if (Field(surface, "surface_kind") == "variable") ++summary.variables;
    } else {
      ++summary.statement_rows;
    }
  }

  harness->Check(summary.executable == 2560,
                 "executable full-route surface count changed from corrected authority baseline");
  harness->Check(summary.exact_refusals == 57,
                 "exact-refusal surface count changed from corrected authority baseline");
  harness->Check(summary.expression_rows == 1534,
                 "expression runtime surface count changed from corrected authority baseline");
  harness->Check(summary.statement_rows == 1083,
                 "statement/command surface count changed from 1083");
  harness->Check(summary.functions == 1515,
                 "function surface count changed from corrected authority baseline");
  harness->Check(summary.operators == 18,
                 "operator surface count changed from 18");
  harness->Check(summary.variables == 1,
                 "variable surface count changed from 1");

  CheckFamilyCount(harness, family_counts, "acceleration", 4);
  CheckFamilyCount(harness, family_counts, "archive_replication", 10);
  CheckFamilyCount(harness, family_counts, "bridge", 34);
  CheckFamilyCount(harness, family_counts, "cluster_private", 24);
  CheckFamilyCount(harness, family_counts, "ddl_catalog", 180);
  CheckFamilyCount(harness, family_counts, "dml", 36);
  CheckFamilyCount(harness, family_counts, "expression_runtime", 1534);
  CheckFamilyCount(harness, family_counts, "general", 554);
  CheckFamilyCount(harness, family_counts, "jobs_scheduler", 8);
  CheckFamilyCount(harness, family_counts, "multi_model", 70);
  CheckFamilyCount(harness, family_counts, "observability", 41);
  CheckFamilyCount(harness, family_counts, "query", 44);
  CheckFamilyCount(harness, family_counts, "runtime_management", 14);
  CheckFamilyCount(harness, family_counts, "security", 23);
  CheckFamilyCount(harness, family_counts, "storage_management", 11);
  CheckFamilyCount(harness, family_counts, "transaction", 26);

  const std::set<std::string> expected_families = {
      "sblr.acceleration.operation.v3",
      "sblr.acceleration.llvm.v3",
      "sblr.archive_replication.operation.v3",
      "sblr.bridge.operation.v3",
      "sblr.catalog.mutation.v3",
      "sblr.cluster.private_operation.v3",
      "sblr.dml.operation.v3",
      "sblr.expression.runtime.v3",
      "sblr.general.operation.v3",
      "sblr.jobs.operation.v3",
      "sblr.management.runtime_operation.v3",
      "sblr.migration.operation.v3",
      "sblr.observability.inspect.v3",
      "sblr.query.multimodel_or_ddl.v3",
      "sblr.query.relational.v3",
      "sblr.query.values.v3",
      "sblr.security.mutation_or_inspect.v3",
      "sblr.storage.management_operation.v3",
      "sblr.transaction.control.v3",
  };
  harness->Check(summary.operation_families == expected_families,
                 "SBLR operation family coverage set drifted");

  return summary;
}

void CheckEngineGapAndDonorClosure(const CsvTable& engine_gap,
                                   const CsvTable& donor_alias,
                                   const CsvTable& donor_fixtures,
                                   Harness* harness) {
  RequireColumns(engine_gap, {"gap_id", "validation_fixture_id", "status"}, harness);
  RequireColumns(donor_alias,
                 {"donor", "alias_kind", "native_sbsql_surface", "validation_fixture_id",
                  "status"},
                 harness);
  RequireColumns(donor_fixtures,
                 {"alias_kind", "fixture_root", "ctest_label", "status"},
                 harness);

  harness->Check(engine_gap.rows.size() == 932,
                 "engine gap backlog count changed from 932");
  std::size_t engine_family_closed = 0;
  std::size_t cluster_closed = 0;
  for (const auto& row : engine_gap.rows) {
    harness->Check(!Field(row, "validation_fixture_id").empty(),
                   "engine gap missing validation fixture");
    if (Field(row, "status") == "closed_by_engine_api_sblr_family_gate") {
      ++engine_family_closed;
    } else if (Field(row, "status") == "closed_by_cluster_fail_closed_gate") {
      ++cluster_closed;
    } else {
      harness->Check(false, "engine gap has unclosed status " + std::string(Field(row, "gap_id")));
    }
  }
  harness->Check(engine_family_closed == 816,
                 "engine API/SBLR family closure count changed from 816");
  harness->Check(cluster_closed == 116,
                 "cluster fail-closed gap count changed from 116");

  harness->Check(donor_alias.rows.size() == 312,
                 "donor alias backlog count changed from 312");
  std::set<std::string> donor_alias_kinds;
  for (const auto& row : donor_alias.rows) {
    donor_alias_kinds.insert(std::string(Field(row, "alias_kind")));
    harness->Check(Field(row, "status") == "closed_by_donor_alias_rendering_gate",
                   "donor alias is not closed " + std::string(Field(row, "donor")) + ":" +
                       std::string(Field(row, "alias_kind")));
    harness->Check(!Field(row, "validation_fixture_id").empty(),
                   "donor alias missing validation fixture");
  }
  harness->Check(donor_alias_kinds.size() >= 10,
                 "donor alias coverage lost command variation breadth");
  harness->Check(donor_fixtures.rows.size() >= 10,
                 "donor rendering fixture manifest lost representative rows");
  for (const auto& row : donor_fixtures.rows) {
    harness->Check(Field(row, "status") == "ready_for_generation",
                   "donor fixture manifest row is not ready");
  }
}

struct DynamicRouteContext {
  std::filesystem::path database_path;
  std::uint64_t local_transaction_id = 0;
  std::uint64_t snapshot_visible_through_local_transaction_id = 0;
  std::string transaction_uuid;
};

api::EngineRequestContext MakeEngineContext(const std::filesystem::path& database_path) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = "sbsql-exhaustive-e2e-dynamic-route";
  context.database_path = database_path.string();
  context.database_uuid.canonical = kDynamicRouteDatabaseUuid;
  context.principal_uuid.canonical = "019e05ef-f015-7000-8000-000000000002";
  context.session_uuid.canonical = "019e05ef-f015-7000-8000-000000000003";
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.trace_tags.push_back("SBSFC-exhaustive-e2e");
  return context;
}

DynamicRouteContext CreateDynamicRouteContext(Harness* harness) {
  DynamicRouteContext route;
  const auto work = MakeTempDir();
  harness->Check(!work.empty(), "failed to create dynamic route temp directory");
  if (work.empty()) return route;
  route.database_path = work / "sbsql_dynamic_route.sbdb";

  api::EngineCreateLifecycleRequest create;
  create.context = MakeEngineContext(route.database_path);
  create.option_envelopes.push_back("allow_minimal_resource_bootstrap:true");
  const auto created = api::EngineCreateLifecycle(create);
  harness->Check(created.ok, "dynamic route database create failed");
  harness->Check(std::filesystem::exists(route.database_path),
                 "dynamic route database file was not created");
  if (!created.ok) return route;

  api::EngineOpenLifecycleRequest open;
  open.context = MakeEngineContext(route.database_path);
  const auto opened = api::EngineOpenLifecycle(open);
  harness->Check(opened.ok, "dynamic route database open failed");
  if (!opened.ok) return route;

  api::EngineBeginTransactionRequest begin;
  begin.context = MakeEngineContext(route.database_path);
  const auto begun = api::EngineBeginTransaction(begin);
  harness->Check(begun.ok && begun.local_transaction_id != 0,
                 "dynamic route engine-owned transaction begin failed");
  route.local_transaction_id = begun.local_transaction_id;
  route.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  route.transaction_uuid = begun.transaction_uuid.canonical;
  return route;
}

scratchbird::server::HostedEngineState MakeEngineState(const DynamicRouteContext& route) {
  scratchbird::server::HostedEngineState state;
  state.engine_context_active = true;
  scratchbird::server::HostedDatabaseSnapshot database;
  database.state = scratchbird::server::HostedDatabaseState::kOpen;
  database.database_created = true;
  database.database_open = true;
  database.database_path = route.database_path.string();
  database.database_uuid = kDynamicRouteDatabaseUuid;
  state.databases.push_back(database);
  return state;
}

scratchbird::server::ServerSessionRegistry MakeRegistry(
    const DynamicRouteContext& route,
    std::array<std::uint8_t, 16>* session_uuid) {
  scratchbird::server::ServerSessionRecord session;
  session.session_uuid = sbps::MakeUuidV7Bytes();
  session.auth_context_uuid = sbps::MakeUuidV7Bytes();
  session.principal_uuid = sbps::MakeUuidV7Bytes();
  session.effective_user_uuid = session.principal_uuid;
  session.database_path = route.database_path.string();
  session.database_uuid = kDynamicRouteDatabaseUuid;
  session.local_transaction_id = route.local_transaction_id;
  session.snapshot_visible_through_local_transaction_id =
      route.snapshot_visible_through_local_transaction_id;
  session.transaction_uuid = route.transaction_uuid;
  *session_uuid = session.session_uuid;

  scratchbird::server::ServerSessionRegistry registry;
  registry.sessions_by_uuid[scratchbird::server::UuidBytesToText(session.session_uuid)] =
      session;
  return registry;
}

sbps::Frame PrepareFrame(const std::array<std::uint8_t, 16>& session_uuid,
                         const std::string& encoded) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kPrepareSblr);
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.session_uuid = session_uuid;
  frame.payload = scratchbird::server::EncodePrepareSblrPayloadForTest(session_uuid, encoded);
  return frame;
}

sbps::Frame ExecuteFrame(const std::array<std::uint8_t, 16>& session_uuid,
                         const std::array<std::uint8_t, 16>& prepared_uuid,
                         const std::string& encoded,
                         bool cursor_requested = false) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kExecuteSblr);
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.session_uuid = session_uuid;
  frame.payload = scratchbird::server::EncodeExecuteSblrPayloadForTest(
      session_uuid, prepared_uuid, encoded, cursor_requested);
  return frame;
}

sbps::Frame FetchFrame(const std::array<std::uint8_t, 16>& session_uuid,
                       const std::array<std::uint8_t, 16>& cursor_uuid) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kFetch);
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.session_uuid = session_uuid;
  frame.payload = scratchbird::server::EncodeFetchPayloadForTest(session_uuid, cursor_uuid);
  return frame;
}

bool HasDiagnostic(const scratchbird::server::SessionOperationResult& result,
                   std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

void CheckDynamicStoredProcedureRoute(Harness* harness) {
  using scratchbird::udr::sbsql_parser_support::sbu_sbsql_parse_to_sblr;

  const std::string part_select = "SELECT * ";
  const std::string part_from = "FROM ";
  const std::string target_object = "sys.version";
  const std::string dynamic_sql = part_select + part_from + target_object;

  const auto missing_resolution = sbu_sbsql_parse_to_sblr(
      dynamic_sql, "engine_context=trusted;resolver=public;authenticated=true");
  harness->Check(!missing_resolution.ok &&
                     Contains(missing_resolution.message_vector_json,
                              "SBSQL.NAME_RESOLUTION.PUBLIC_RESOLVER_REQUIRED"),
                 "dynamic SQL without engine-resolved UUID did not fail closed");

  const auto generated = sbu_sbsql_parse_to_sblr(
      dynamic_sql,
      "engine_context=trusted;resolver=public;authenticated=true;"
      "resolved_uuid=b4a0fd27-e19b-7719-9105-5882443ee2bc");
  harness->Check(generated.ok,
                 "dynamic SQL UDR conversion failed: " + generated.message_vector_json);
  harness->Check(Contains(generated.payload, "SBLRExecutionEnvelope.v3"),
                 "UDR payload missing SBLRExecutionEnvelope.v3");
  harness->Check(Contains(generated.payload, "\"operation_family\":\"sblr.query.relational.v3\""),
                 "UDR payload did not lower to relational query SBLR");
  harness->Check(Contains(generated.payload, "\"resolved_object_uuids\""),
                 "UDR payload missing resolved UUID list");
  harness->Check(Contains(generated.payload, kDynamicRouteSysVersionUuid),
                 "UDR payload did not preserve the engine-resolved UUID");
  harness->Check(Contains(generated.payload, "authority.server.resolve_name_registry_public"),
                 "UDR payload missing name-resolution authority contract");
  harness->Check(!Contains(generated.payload, dynamic_sql),
                 "UDR payload leaked original dynamic SQL text");
  harness->Check(Contains(generated.payload, "\"sql_text_included\":false"),
                 "UDR payload missing no-SQL-text authority marker");
  harness->Check(!Contains(generated.payload, "\"sql_text\":") &&
                     !Contains(generated.payload, "\"source_text\""),
                 "UDR payload exposed a raw SQL text field");

  const auto raw_direct = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{dynamic_sql, false});
  harness->Check(!raw_direct.admitted && !raw_direct.diagnostics.empty(),
                 "server admitted concatenated raw SQL without UDR/SBLR conversion");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{generated.payload, false});
  harness->Check(admission.admitted,
                 "server admission rejected UDR-generated SBLR payload");
  harness->Check(admission.operation_family == "sblr.query.relational.v3",
                 "server admission operation family mismatch for dynamic SBLR");

  std::array<std::uint8_t, 16> session_uuid{};
  const auto dynamic_route = CreateDynamicRouteContext(harness);
  auto registry = MakeRegistry(dynamic_route, &session_uuid);
  const auto engine_state = MakeEngineState(dynamic_route);

  const auto raw_prepare = scratchbird::server::HandlePrepareSblr(
      &registry, engine_state, PrepareFrame(session_uuid, dynamic_sql));
  harness->Check(!raw_prepare.accepted,
                 "server prepared concatenated raw SQL without UDR/SBLR conversion");

  const auto prepare = scratchbird::server::HandlePrepareSblr(
      &registry, engine_state, PrepareFrame(session_uuid, generated.payload));
  harness->Check(prepare.accepted,
                 "server did not prepare UDR-generated SBLR payload");
  const auto prepared_uuid =
      scratchbird::server::DecodePreparedStatementUuidForTest(prepare.payload);
  harness->Check(prepared_uuid.has_value(),
                 "prepare did not return a prepared statement UUID");
  if (prepared_uuid.has_value()) {
    const auto prepared_execute = scratchbird::server::HandleExecuteSblr(
        &registry, engine_state, ExecuteFrame(session_uuid, *prepared_uuid, ""));
    harness->Check(prepared_execute.accepted,
                   "server did not execute prepared dynamic SBLR payload");
  }

  const auto cursor_execute = scratchbird::server::HandleExecuteSblr(
      &registry, engine_state, ExecuteFrame(session_uuid, {}, generated.payload, true));
  harness->Check(cursor_execute.accepted,
                 "server did not execute dynamic SBLR as cursor route");
  const auto cursor_uuid = scratchbird::server::DecodeCursorUuidForTest(cursor_execute.payload);
  harness->Check(cursor_uuid.has_value(),
                 "dynamic SBLR cursor execution did not return a cursor UUID");
  if (cursor_uuid.has_value()) {
    const auto fetch =
        scratchbird::server::HandleFetch(&registry, FetchFrame(session_uuid, *cursor_uuid));
    harness->Check(fetch.accepted, "dynamic SBLR cursor fetch was rejected");
    const auto decoded_fetch = scratchbird::server::DecodeFetchResultForTest(fetch.payload);
    harness->Check(decoded_fetch.has_value(), "dynamic SBLR fetch payload did not decode");
    if (decoded_fetch.has_value()) {
      harness->Check(decoded_fetch->row_count == 1,
                     "dynamic SBLR fetch did not return expected row_count=1");
    }
  }

  const auto tampered_payload =
      generated.payload.substr(0, generated.payload.size() - 1) +
      ",\"sql_text\":\"SELECT * FROM sys.version\"}";
  const auto tampered = scratchbird::server::HandleExecuteSblr(
      &registry, engine_state, ExecuteFrame(session_uuid, {}, tampered_payload));
  harness->Check(!tampered.accepted && HasDiagnostic(tampered, "SBLR.SQL_TEXT_FORBIDDEN"),
                 "server did not reject tampered dynamic SBLR payload with sql_text marker");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 6) {
    std::cerr << "usage: " << argv[0]
              << " <artifact-root> <canonicalization-root> <replay-root>"
              << " <donor-fixtures> <variation-matrix>\n";
    return EXIT_FAILURE;
  }

  try {
    const std::filesystem::path artifact_root = argv[1];
    const std::filesystem::path canonicalization_root = argv[2];
    const std::filesystem::path replay_root = argv[3];
    const std::filesystem::path donor_fixtures_path = argv[4];
    const std::filesystem::path variation_matrix_path = argv[5];

    Harness harness;
    ConfigureMemoryFixture(&harness);
    CheckVariationManifest(ReadCsv(variation_matrix_path), &harness);

    const auto summary = CheckSurfaceReplayCoverage(
        ReadCsv(canonicalization_root / "SBSQL_SURFACE_REGISTRY.csv"),
        ReadCsv(artifact_root / "SURFACE_IMPLEMENTATION_BACKLOG.csv"),
        ReadCsv(artifact_root / "BATCH_ROW_MEMBERSHIP.csv"),
        ReadCsv(artifact_root / "SEMANTIC_ORACLE_AUTHORITY_MAP.csv"),
        ReadCsv(replay_root / "DIFFERENTIAL_REPLAY_FIXTURE_INDEX.csv"),
        ReadLines(replay_root / "DIFFERENTIAL_REPLAY_EXPECTED_PAYLOADS.jsonl"),
        &harness);

    CheckEngineGapAndDonorClosure(
        ReadCsv(artifact_root / "ENGINE_GAP_IMPLEMENTATION_BACKLOG.csv"),
        ReadCsv(artifact_root / "DONOR_ALIAS_COVERAGE_BACKLOG.csv"),
        ReadCsv(donor_fixtures_path),
        &harness);
    CheckDynamicStoredProcedureRoute(&harness);

    if (!harness.ok) {
      std::cerr << "sbsql_exhaustive_e2e_regression_gate failed with "
                << harness.failures << " failure(s)\n";
      return EXIT_FAILURE;
    }

    std::cout << "sbsql_exhaustive_e2e_regression_gate=passed"
              << " surfaces=" << summary.surfaces
              << " executable_full_route=" << summary.executable
              << " exact_refusals=" << summary.exact_refusals
              << " expressions=" << summary.expression_rows
              << " statements=" << summary.statement_rows
              << " functions=" << summary.functions
              << " operators=" << summary.operators
              << " variables=" << summary.variables
              << " operation_families=" << summary.operation_families.size()
              << '\n';
    return EXIT_SUCCESS;
  } catch (const std::exception& ex) {
    std::cerr << "sbsql_exhaustive_e2e_regression_gate exception: "
              << ex.what() << '\n';
    return EXIT_FAILURE;
  }
}
