// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

constexpr std::size_t kExpectedSurfaceCount = 2617;

using CsvRow = std::unordered_map<std::string, std::string>;

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
    if (failures < 100) std::cerr << message << '\n';
    ++failures;
  }
};

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool Contains(std::string_view value, std::string_view needle) {
  return value.find(needle) != std::string_view::npos;
}

void TrimLineEnding(std::string* line) {
  while (!line->empty() && (line->back() == '\r' || line->back() == '\n')) {
    line->pop_back();
  }
}

std::vector<std::string> Split(std::string_view value, char delimiter) {
  std::vector<std::string> parts;
  std::string current;
  for (const char ch : value) {
    if (ch == delimiter) {
      parts.push_back(current);
      current.clear();
    } else {
      current.push_back(ch);
    }
  }
  parts.push_back(current);
  return parts;
}

std::vector<std::string> SplitCsvLine(std::string_view line) {
  std::vector<std::string> fields;
  std::string current;
  bool in_quotes = false;

  for (std::size_t i = 0; i < line.size(); ++i) {
    const char ch = line[i];
    if (in_quotes) {
      if (ch == '"') {
        if (i + 1 < line.size() && line[i + 1] == '"') {
          current.push_back('"');
          ++i;
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

std::string ReadText(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("failed to open " + path.string());
  return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::string_view Field(const CsvRow& row, std::string_view column) {
  const auto found = row.find(std::string(column));
  if (found == row.end()) return {};
  return found->second;
}

bool HasColumn(const CsvTable& table, std::string_view column) {
  return std::find(table.headers.begin(), table.headers.end(), column) != table.headers.end();
}

bool RequireColumns(const CsvTable& table,
                    std::initializer_list<std::string_view> columns,
                    Harness* harness) {
  bool ok = true;
  for (const auto column : columns) {
    const bool present = HasColumn(table, column);
    harness->Check(present, table.path.filename().string() +
                                " missing required column " + std::string(column));
    ok &= present;
  }
  return ok;
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
                                        " duplicate " + std::string(column) +
                                        " " + key);
  }
  return index;
}

void CheckText(Harness* harness,
               std::string_view text,
               std::string_view token,
               std::string_view context) {
  harness->Check(Contains(text, token),
                 std::string(context) + " missing token " + std::string(token));
}

std::string ExtractJsonString(std::string_view line, std::string_view key) {
  const std::string needle = "\"" + std::string(key) + "\":\"";
  const auto start = line.find(needle);
  if (start == std::string_view::npos) return {};
  std::string value;
  bool escaped = false;
  for (std::size_t i = start + needle.size(); i < line.size(); ++i) {
    const char ch = line[i];
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

void ValidatePlanAndReport(const std::filesystem::path& artifact_root,
                           Harness* harness) {
  const auto plan = ReadText(artifact_root / "DIFFERENTIAL_REPLAY_HARNESS_PLAN.md");
  for (const auto token :
       {"Status: complete", "DIFFERENTIAL_REPLAY_FIXTURE_INDEX.csv",
        "DIFFERENTIAL_REPLAY_EXPECTED_PAYLOADS.jsonl", "parser_parse_only",
        "udr_sql_to_sblr", "server_admission", "engine_behavior", "full_route",
        "reference_alias", "diagnostic", "fixture_id", "surface_id", "expected_message_vector",
        "expected_rendered_output", "no SQL text reaches engine"}) {
    CheckText(harness, plan, token, "DIFFERENTIAL_REPLAY_HARNESS_PLAN.md");
  }

  const auto report = ReadText(artifact_root / "DIFFERENTIAL_REPLAY_HARNESS_REPORT.md");
  for (const auto token :
       {"Status: complete", "Total fixtures | 2,617",
        "Unexpected failures | 0", "sbsql_differential_replay_harness_gate",
        "parser_parse_only", "parser_bind_lower", "udr_sql_to_sblr",
        "server_admission", "engine_behavior", "full_route", "reference_alias",
        "diagnostic", "Failure Row Schema"}) {
    CheckText(harness, report, token, "DIFFERENTIAL_REPLAY_HARNESS_REPORT.md");
  }
}

void ValidateCommand(const CsvTable& commands, Harness* harness) {
  if (!RequireColumns(commands,
                      {"command_name", "owning_slice", "materialization_status",
                       "executable_or_contract", "runnable_now",
                       "evidence_artifact"},
                      harness)) {
    return;
  }

  const auto by_name = IndexUnique(commands, "command_name", harness);
  const auto found = by_name.find("sbsql_differential_replay_harness_gate");
  harness->Check(found != by_name.end(), "missing differential replay command row");
  if (found == by_name.end()) return;

  const auto& row = *found->second;
  harness->Check(Field(row, "owning_slice") == "FSPE-011F",
                 "differential replay command wrong owner");
  harness->Check(Field(row, "materialization_status") == "complete",
                 "differential replay command not complete");
  harness->Check(Field(row, "runnable_now") == "yes",
                 "differential replay command not runnable");
  harness->Check(Contains(Field(row, "executable_or_contract"),
                          "sbsql_differential_replay_harness_gate"),
                 "differential replay command does not select gate label");
  harness->Check(Contains(Field(row, "evidence_artifact"),
                          "DIFFERENTIAL_REPLAY_HARNESS_REPORT.md") &&
                     Contains(Field(row, "evidence_artifact"),
                              "FSPE_011F_VALIDATION_RESULT.md"),
                 "differential replay command evidence incomplete");
}

std::set<std::string> ReadReferenceNativeSurfaceNames(const CsvTable& reference_matrix,
                                                  Harness* harness) {
  std::set<std::string> names;
  if (!RequireColumns(reference_matrix,
                      {"reference", "alias_kind", "reference_surface",
                       "native_sbsql_surface", "mapping_status",
                       "sblr_operation_family"},
                      harness)) {
    return names;
  }
  for (const auto& row : reference_matrix.rows) {
    harness->Check(!Field(row, "native_sbsql_surface").empty(),
                   "reference matrix row has empty native_sbsql_surface");
    names.insert(std::string(Field(row, "native_sbsql_surface")));
  }
  return names;
}

void ValidateRouteManifest(const CsvTable& route_manifest,
                           const CsvTable& reference_fixtures,
                           Harness* harness) {
  if (!RequireColumns(route_manifest,
                      {"route_id", "ctest_label", "route_class", "replay_scope",
                       "expected_source", "status"},
                      harness)) {
    return;
  }
  if (!RequireColumns(reference_fixtures,
                      {"alias_kind", "fixture_root", "ctest_label",
                       "result_metadata_fields", "command_tag_policy",
                       "affected_rows_policy", "warning_error_policy",
                       "catalog_shape_policy", "status"},
                      harness)) {
    return;
  }

  const auto by_route = IndexUnique(route_manifest, "route_id", harness);
  const std::set<std::string> required_routes = {
      "parser_parse_only", "parser_bind_lower", "udr_sql_to_sblr",
      "server_admission", "engine_behavior", "full_route", "reference_alias",
      "diagnostic"};
  for (const auto& route : required_routes) {
    const auto found = by_route.find(route);
    harness->Check(found != by_route.end(), "route manifest missing " + route);
    if (found == by_route.end()) continue;
    const auto& row = *found->second;
    harness->Check(StartsWith(Field(row, "ctest_label"), "sbsql_replay_"),
                   route + " ctest_label must use sbsql_replay_ prefix");
    harness->Check(!Field(row, "replay_scope").empty() &&
                       !Field(row, "expected_source").empty(),
                   route + " missing replay scope or expected source");
    harness->Check(Field(row, "status") == "complete",
                   route + " status is not complete");
  }

  std::set<std::string> reference_alias_kinds;
  for (const auto& row : reference_fixtures.rows) {
    reference_alias_kinds.insert(std::string(Field(row, "alias_kind")));
    harness->Check(StartsWith(Field(row, "fixture_root"),
                              "project/tests/sbsql_parser_worker/generated/reference_alias"),
                   std::string(Field(row, "alias_kind")) +
                       " reference fixture root is not durable");
    harness->Check(Field(row, "ctest_label") ==
                       "sbsql_reference_alias_rendering_conformance",
                   std::string(Field(row, "alias_kind")) +
                       " reference fixture ctest label mismatch");
    harness->Check(Field(row, "status") == "ready_for_generation",
                   std::string(Field(row, "alias_kind")) +
                       " reference fixture status mismatch");
  }
  harness->Check(reference_alias_kinds.size() == 13,
                 "reference alias fixture manifest must cover 13 alias kinds");
}

void ValidatePayloads(const std::filesystem::path& payload_path,
                      const std::set<std::string>& index_fixture_ids,
                      Harness* harness) {
  std::ifstream input(payload_path);
  if (!input) throw std::runtime_error("failed to open " + payload_path.string());

  std::set<std::string> payload_fixture_ids;
  std::string line;
  std::size_t line_count = 0;
  while (std::getline(input, line)) {
    ++line_count;
    TrimLineEnding(&line);
    if (line.empty()) continue;

    const auto fixture_id = ExtractJsonString(line, "fixture_id");
    harness->Check(!fixture_id.empty(),
                   payload_path.filename().string() + " line " +
                       std::to_string(line_count) + " missing fixture_id");
    if (!fixture_id.empty()) {
      harness->Check(payload_fixture_ids.insert(fixture_id).second,
                     fixture_id + " duplicate expected payload");
    }

    for (const auto token :
         {"\"input\":", "\"parser\":", "\"binding\":", "\"server\":",
          "\"engine\":", "\"diagnostics\":", "\"oracle\":", "\"routes\":",
          "\"cleanup_policy\":", "not-derived-from-current-output"}) {
      harness->Check(Contains(line, token),
                     payload_path.filename().string() + " " + fixture_id +
                         " missing payload token " + token);
    }
  }

  harness->Check(line_count == kExpectedSurfaceCount,
                 "expected payload JSONL must contain corrected authority surface count");
  harness->Check(payload_fixture_ids == index_fixture_ids,
                 "expected payload fixture ids do not match replay index");
}

void ValidateReplayIndex(const CsvTable& replay_index,
                         const CsvTable& route_manifest,
                         const CsvTable& oracles,
                         const CsvTable& membership,
                         const CsvTable& surface_backlog,
                         const CsvTable& surface_registry,
                         const CsvTable& operation_matrix,
                         const std::set<std::string>& reference_native_names,
                         const std::filesystem::path& payload_path,
                         Harness* harness) {
  bool schema_ok = true;
  schema_ok &= RequireColumns(replay_index,
                              {"fixture_id", "surface_id", "batch_id",
                               "canonical_name", "family", "surface_kind",
                               "source_status", "cluster_scope",
                               "operation_family", "primary_route", "route_set",
                               "parser_profile", "session_context", "input_text",
                               "expected_parse", "expected_bound_shape",
                               "expected_sblr_digest_policy",
                               "expected_server_result", "expected_engine_effect",
                               "expected_message_vector", "expected_rendered_output",
                               "oracle_type", "oracle_source", "source_search_key",
                               "expected_result_summary", "expected_payload_json",
                               "status"},
                              harness);
  schema_ok &= RequireColumns(oracles,
                              {"fixture_id", "surface_id", "oracle_type",
                               "oracle_source", "source_search_key",
                               "expected_result_summary", "status"},
                              harness);
  schema_ok &= RequireColumns(membership,
                              {"surface_id", "validation_fixture_id", "batch_id",
                               "canonical_name", "family", "surface_kind",
                               "source_status", "cluster_scope", "ctest_label"},
                              harness);
  schema_ok &= RequireColumns(surface_backlog,
                              {"surface_id", "canonical_name", "surface_kind",
                               "family", "source_status", "cluster_scope",
                               "sblr_operation_family", "server_target_behavior",
                               "engine_target_behavior", "diagnostic_target",
                               "validation_fixture_id"},
                              harness);
  schema_ok &= RequireColumns(surface_registry,
                              {"surface_id", "canonical_name", "status",
                               "cluster_scope", "sblr_operation_family"},
                              harness);
  schema_ok &= RequireColumns(operation_matrix,
                              {"surface_id", "canonical_name",
                               "sblr_operation_family", "required_context",
                               "result_shape", "diagnostics"},
                              harness);
  if (!schema_ok) return;

  const auto routes_by_id = IndexUnique(route_manifest, "route_id", harness);
  const auto replay_by_surface = IndexUnique(replay_index, "surface_id", harness);
  const auto replay_by_fixture = IndexUnique(replay_index, "fixture_id", harness);
  const auto oracle_by_surface = IndexUnique(oracles, "surface_id", harness);
  const auto membership_by_surface = IndexUnique(membership, "surface_id", harness);
  const auto backlog_by_surface = IndexUnique(surface_backlog, "surface_id", harness);
  const auto registry_by_surface = IndexUnique(surface_registry, "surface_id", harness);
  const auto operation_by_surface = IndexUnique(operation_matrix, "surface_id", harness);

  harness->Check(replay_index.rows.size() == kExpectedSurfaceCount,
                 "replay index must contain corrected authority surface count");
  harness->Check(replay_index.rows.size() == oracles.rows.size(),
                 "replay index/oracle row count mismatch");
  harness->Check(replay_index.rows.size() == membership.rows.size(),
                 "replay index/membership row count mismatch");
  harness->Check(replay_index.rows.size() == surface_backlog.rows.size(),
                 "replay index/surface backlog row count mismatch");
  harness->Check(replay_index.rows.size() == surface_registry.rows.size(),
                 "replay index/surface registry row count mismatch");
  harness->Check(replay_index.rows.size() == operation_matrix.rows.size(),
                 "replay index/operation matrix row count mismatch");

  std::map<std::string, std::size_t> route_counts;
  std::set<std::string> index_fixture_ids;

  for (const auto& row : replay_index.rows) {
    const std::string surface_id(Field(row, "surface_id"));
    const std::string fixture_id(Field(row, "fixture_id"));
    const std::string context = surface_id + "/" + fixture_id;
    index_fixture_ids.insert(fixture_id);

    harness->Check(Field(row, "status") == "replay_ready",
                   context + " status is not replay_ready");
    harness->Check(StartsWith(surface_id, "SBSQL-"),
                   context + " malformed surface_id");
    harness->Check(StartsWith(fixture_id, "SBSQL-SURFACE-"),
                   context + " malformed fixture_id");
    harness->Check(!Field(row, "input_text").empty(),
                   context + " missing durable replay input text");
    harness->Check(Field(row, "input_text") != Field(row, "canonical_name"),
                   context + " input_text must not be raw canonical_name only");
    harness->Check(Contains(Field(row, "expected_sblr_digest_policy"),
                            "not-derived-from-current-output"),
                   context + " SBLR digest policy is not independent");
    harness->Check(Contains(Field(row, "expected_message_vector"),
                            "message_vector") ||
                       Contains(Field(row, "expected_message_vector"),
                                "SBSQL.") ||
                       Contains(Field(row, "expected_message_vector"),
                                "SBLR."),
                   context + " expected message vector is incomplete");
    harness->Check(Contains(Field(row, "expected_rendered_output"),
                            "ExecutionResultEnvelope.v3"),
                   context + " expected rendered output lacks result envelope");
    harness->Check(StartsWith(Field(row, "expected_payload_json"),
                              "project/tests/sbsql_parser_worker/generated/replay/"
                              "DIFFERENTIAL_REPLAY_EXPECTED_PAYLOADS.jsonl#"),
                   context + " payload reference is not durable");
    harness->Check(Contains(Field(row, "expected_payload_json"), "#" + fixture_id),
                   context + " payload reference does not anchor fixture id");

    const auto route_names = Split(Field(row, "route_set"), ';');
    const std::set<std::string> route_set(route_names.begin(), route_names.end());
    harness->Check(route_set.contains(std::string(Field(row, "primary_route"))),
                   context + " primary_route not present in route_set");
    for (const auto& route : {"parser_parse_only", "parser_bind_lower",
                              "server_admission", "diagnostic"}) {
      harness->Check(route_set.contains(route),
                     context + " missing required replay route " + route);
    }
    for (const auto& route : route_set) {
      ++route_counts[route];
      harness->Check(routes_by_id.contains(route),
                     context + " references missing route " + route);
    }

    const bool active_native =
        Field(row, "source_status") == "native_now" &&
        Field(row, "cluster_scope") != "cluster_private";
    if (active_native) {
      for (const auto& route :
           {"udr_sql_to_sblr", "engine_behavior", "full_route"}) {
        harness->Check(route_set.contains(route),
                       context + " active native fixture missing route " + route);
      }
      harness->Check(Contains(Field(row, "expected_engine_effect"), "execute-sblr") &&
                         Contains(Field(row, "expected_engine_effect"), "no-sql-text"),
                     context + " active native engine effect is not SBLR-only");
    } else {
      harness->Check(Contains(Field(row, "expected_engine_effect"), "no-engine-mutation"),
                     context + " refusal/profile fixture must not mutate engine");
    }
    if (reference_native_names.contains(std::string(Field(row, "canonical_name")))) {
      harness->Check(route_set.contains("reference_alias"),
                     context + " reference native surface missing reference_alias route");
    }

    const auto oracle_it = oracle_by_surface.find(surface_id);
    const auto membership_it = membership_by_surface.find(surface_id);
    const auto backlog_it = backlog_by_surface.find(surface_id);
    const auto registry_it = registry_by_surface.find(surface_id);
    const auto operation_it = operation_by_surface.find(surface_id);
    harness->Check(oracle_it != oracle_by_surface.end(), context + " missing oracle row");
    harness->Check(membership_it != membership_by_surface.end(),
                   context + " missing membership row");
    harness->Check(backlog_it != backlog_by_surface.end(), context + " missing backlog row");
    harness->Check(registry_it != registry_by_surface.end(), context + " missing registry row");
    harness->Check(operation_it != operation_by_surface.end(),
                   context + " missing operation matrix row");
    if (oracle_it == oracle_by_surface.end() ||
        membership_it == membership_by_surface.end() ||
        backlog_it == backlog_by_surface.end() ||
        registry_it == registry_by_surface.end() ||
        operation_it == operation_by_surface.end()) {
      continue;
    }

    const auto& oracle = *oracle_it->second;
    const auto& member = *membership_it->second;
    const auto& backlog = *backlog_it->second;
    const auto& registry = *registry_it->second;
    const auto& operation = *operation_it->second;
    harness->Check(Field(row, "fixture_id") == Field(oracle, "fixture_id"),
                   context + " fixture_id mismatch with oracle");
    harness->Check(Field(row, "fixture_id") == Field(member, "validation_fixture_id"),
                   context + " fixture_id mismatch with membership");
    harness->Check(Field(row, "batch_id") == Field(member, "batch_id"),
                   context + " batch_id mismatch with membership");
    for (const auto column :
         {"canonical_name", "family", "surface_kind", "source_status",
          "cluster_scope"}) {
      harness->Check(Field(row, column) == Field(backlog, column),
                     context + " " + std::string(column) +
                         " mismatch with surface backlog");
      if (std::string(column) != "family" && std::string(column) != "surface_kind" &&
          std::string(column) != "source_status") {
        harness->Check(Field(row, column) == Field(registry, column),
                       context + " " + std::string(column) +
                           " mismatch with surface registry");
      }
    }
    harness->Check(Field(row, "operation_family") == Field(backlog, "sblr_operation_family"),
                   context + " operation family mismatch with backlog");
    harness->Check(Field(row, "operation_family") == Field(registry, "sblr_operation_family"),
                   context + " operation family mismatch with registry");
    harness->Check(Field(row, "operation_family") == Field(operation, "sblr_operation_family"),
                   context + " operation family mismatch with operation matrix");
    harness->Check(Field(row, "session_context") == Field(operation, "required_context"),
                   context + " session context mismatch");
    harness->Check(Contains(Field(row, "expected_bound_shape"),
                            Field(operation, "result_shape")),
                   context + " bound shape lacks operation result shape");
    harness->Check(Contains(Field(row, "expected_message_vector"),
                            Field(operation, "diagnostics")),
                   context + " message vector lacks operation diagnostics");
    harness->Check(Field(row, "oracle_type") == Field(oracle, "oracle_type"),
                   context + " oracle type mismatch");
    harness->Check(Field(row, "oracle_source") == Field(oracle, "oracle_source"),
                   context + " oracle source mismatch");
    harness->Check(Field(row, "source_search_key") == surface_id &&
                       Field(row, "source_search_key") ==
                           Field(oracle, "source_search_key"),
                   context + " source search key mismatch");
    harness->Check(Field(row, "expected_result_summary") ==
                       Field(oracle, "expected_result_summary"),
                   context + " expected result summary mismatch");
  }

  for (const auto& required :
       {"parser_parse_only", "parser_bind_lower", "udr_sql_to_sblr",
        "server_admission", "engine_behavior", "full_route", "reference_alias",
        "diagnostic"}) {
    harness->Check(route_counts[required] > 0,
                   std::string("replay route has no fixtures: ") + required);
  }
  harness->Check(route_counts["parser_parse_only"] == kExpectedSurfaceCount,
                 "parser_parse_only route must cover every fixture");
  harness->Check(route_counts["parser_bind_lower"] == kExpectedSurfaceCount,
                 "parser_bind_lower route must cover every fixture");
  harness->Check(route_counts["server_admission"] == kExpectedSurfaceCount,
                 "server_admission route must cover every fixture");
  harness->Check(route_counts["diagnostic"] == kExpectedSurfaceCount,
                 "diagnostic route must cover every fixture");

  ValidatePayloads(payload_path, index_fixture_ids, harness);
  harness->Check(replay_by_surface.size() == replay_index.rows.size() &&
                     replay_by_fixture.size() == replay_index.rows.size(),
                 "replay index uniqueness failed");
}

} // namespace

int main(int argc, char** argv) {
  if (argc != 5) {
    std::cerr << "usage: sbp_sbsql_differential_replay_harness_gate "
                 "<artifact-root> <canonicalization-root> <replay-root> "
                 "<reference-fixtures>\n";
    return 1;
  }

  const std::filesystem::path artifact_root(argv[1]);
  const std::filesystem::path canonicalization_root(argv[2]);
  const std::filesystem::path replay_root(argv[3]);
  const std::filesystem::path reference_fixtures_path(argv[4]);
  Harness harness;

  try {
    const auto route_manifest =
        ReadCsv(replay_root / "DIFFERENTIAL_REPLAY_ROUTE_MANIFEST.csv");
    const auto replay_index =
        ReadCsv(replay_root / "DIFFERENTIAL_REPLAY_FIXTURE_INDEX.csv");
    const auto reference_matrix =
        ReadCsv(canonicalization_root / "REFERENCE_ALIAS_TO_SBSQL_SURFACE_MATRIX.csv");
    const auto reference_fixtures = ReadCsv(reference_fixtures_path);
    const auto reference_native_names =
        ReadReferenceNativeSurfaceNames(reference_matrix, &harness);

    ValidatePlanAndReport(artifact_root, &harness);
    ValidateCommand(ReadCsv(artifact_root / "VALIDATION_COMMAND_MATERIALIZATION.csv"),
                    &harness);
    ValidateRouteManifest(route_manifest, reference_fixtures, &harness);
    ValidateReplayIndex(replay_index, route_manifest,
                        ReadCsv(artifact_root / "SEMANTIC_ORACLE_AUTHORITY_MAP.csv"),
                        ReadCsv(artifact_root / "BATCH_ROW_MEMBERSHIP.csv"),
                        ReadCsv(artifact_root / "SURFACE_IMPLEMENTATION_BACKLOG.csv"),
                        ReadCsv(canonicalization_root / "SBSQL_SURFACE_REGISTRY.csv"),
                        ReadCsv(canonicalization_root /
                                "SBSQL_TO_SBLR_OPERATION_MATRIX.csv"),
                        reference_native_names,
                        replay_root / "DIFFERENTIAL_REPLAY_EXPECTED_PAYLOADS.jsonl",
                        &harness);

    if (!harness.ok) {
      std::cerr << "FSPE-011F differential replay harness gate failed with "
                << harness.failures << " failure(s)\n";
      return 1;
    }

    std::cout << "FSPE-011F differential replay harness gate passed: "
              << "fixtures=" << kExpectedSurfaceCount
              << " routes=8 payloads=" << kExpectedSurfaceCount
              << " unexpected_failures=0\n";
  } catch (const std::exception& ex) {
    std::cerr << "FSPE-011F differential replay harness gate failed: "
              << ex.what() << '\n';
    return 1;
  }

  return 0;
}
