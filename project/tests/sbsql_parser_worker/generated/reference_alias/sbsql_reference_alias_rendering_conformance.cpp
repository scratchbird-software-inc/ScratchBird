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
#include <vector>

namespace {

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
    if (failures < 80) std::cerr << message << '\n';
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

void ValidateCommands(const CsvTable& commands, Harness* harness) {
  if (!RequireColumns(commands,
                      {"command_name", "owning_slice", "materialization_status",
                       "executable_or_contract", "runnable_now",
                       "evidence_artifact"},
                      harness)) {
    return;
  }
  const auto by_name = IndexUnique(commands, "command_name", harness);
  for (const auto command_name :
       {"sbsql_reference_alias_mapping_conformance",
        "sbsql_reference_alias_rendering_conformance"}) {
    const auto found = by_name.find(command_name);
    harness->Check(found != by_name.end(), std::string("missing command ") + command_name);
    if (found == by_name.end()) continue;
    const auto& row = *found->second;
    harness->Check(Field(row, "owning_slice") == "FSPE-011B",
                   std::string(command_name) + " wrong owning_slice");
    harness->Check(Field(row, "materialization_status") == "complete",
                   std::string(command_name) + " not complete");
    harness->Check(Field(row, "runnable_now") == "yes",
                   std::string(command_name) + " not runnable");
    harness->Check(Contains(Field(row, "executable_or_contract"),
                            "sbsql_reference_alias_rendering_conformance"),
                   std::string(command_name) + " command does not select reference alias gate");
    harness->Check(Contains(Field(row, "evidence_artifact"),
                            "REFERENCE_ALIAS_RENDERING_REPORT.md") &&
                       Contains(Field(row, "evidence_artifact"),
                                "FSPE_011B_VALIDATION_RESULT.md"),
                   std::string(command_name) + " evidence is incomplete");
  }
}

void ValidateReport(const std::filesystem::path& artifact_root, Harness* harness) {
  const auto report = ReadText(artifact_root / "REFERENCE_ALIAS_RENDERING_REPORT.md");
  for (const auto token :
       {"Status: complete", "312", "Reference Profiles", "Rendering Policy",
        "REFERENCE_ALIAS_RENDERING_FIXTURES.csv", "message vector",
        "engine authority", "sbsql_reference_alias_rendering_conformance"}) {
    harness->Check(Contains(report, token),
                   std::string("REFERENCE_ALIAS_RENDERING_REPORT.md missing token ") + token);
  }
}

void ValidateFixturePolicy(const CsvTable& fixture_policy, Harness* harness) {
  if (!RequireColumns(fixture_policy,
                      {"alias_kind", "fixture_root", "ctest_label",
                       "result_metadata_fields", "command_tag_policy",
                       "affected_rows_policy", "warning_error_policy",
                       "catalog_shape_policy", "status"},
                      harness)) {
    return;
  }

  const std::set<std::string> required_alias_kinds = {
      "query_select",      "dml_insert",     "dml_update",
      "dml_delete",       "dml_merge_upsert", "ddl_create",
      "ddl_alter",        "ddl_drop",       "transaction_control",
      "session_settings", "observability",  "function_call",
      "bulk_io",
  };

  std::set<std::string> observed;
  for (const auto& row : fixture_policy.rows) {
    const std::string alias_kind(Field(row, "alias_kind"));
    observed.insert(alias_kind);
    harness->Check(StartsWith(Field(row, "fixture_root"),
                              "project/tests/sbsql_parser_worker/generated/reference_alias/"),
                   alias_kind + " fixture root is not durable reference_alias project path");
    harness->Check(Field(row, "ctest_label") == "sbsql_reference_alias_rendering_conformance",
                   alias_kind + " ctest_label mismatch");
    harness->Check(Contains(Field(row, "result_metadata_fields"), "profile_label"),
                   alias_kind + " result metadata lacks profile label");
    harness->Check(StartsWith(Field(row, "command_tag_policy"), "render_reference_"),
                   alias_kind + " command tag policy is not reference-rendered");
    harness->Check(Contains(Field(row, "warning_error_policy"),
                            "reference_message_vector"),
                   alias_kind + " warning/error policy does not use reference message vectors");
    harness->Check(Contains(Field(row, "catalog_shape_policy"),
                            "reference_catalog_projection_or_exact_refusal"),
                   alias_kind + " catalog shape policy mismatch");
    harness->Check(Field(row, "status") == "ready_for_generation",
                   alias_kind + " status must be ready_for_generation");
  }

  for (const auto& required : required_alias_kinds) {
    harness->Check(observed.contains(required),
                   "fixture policy missing alias_kind " + required);
  }
}

void ValidateReferenceMatrices(const CsvTable& canonical_aliases,
                           const CsvTable& backlog_aliases,
                           const CsvTable& fixture_policy,
                           Harness* harness) {
  bool schema_ok = true;
  schema_ok &= RequireColumns(canonical_aliases,
                              {"reference", "alias_kind", "reference_surface",
                               "native_sbsql_surface", "mapping_status",
                               "sblr_operation_family", "parser_owned_behavior",
                               "engine_owned_behavior", "notes"},
                              harness);
  schema_ok &= RequireColumns(backlog_aliases,
                              {"reference", "alias_kind", "reference_surface",
                               "native_sbsql_surface", "mapping_status",
                               "sblr_operation_family", "parser_owned_behavior",
                               "engine_owned_behavior", "owner_lane",
                               "target_file_group", "diagnostic_target",
                               "validation_fixture_id", "final_acceptance_rule",
                               "closure_action", "status", "notes"},
                              harness);
  if (!schema_ok) return;

  const auto canonical_by_surface = IndexUnique(canonical_aliases, "reference_surface", harness);
  const auto backlog_by_surface = IndexUnique(backlog_aliases, "reference_surface", harness);
  const auto policy_by_alias_kind = IndexUnique(fixture_policy, "alias_kind", harness);

  harness->Check(canonical_aliases.rows.size() == 312,
                 "canonical reference alias matrix must contain 312 rows");
  harness->Check(backlog_aliases.rows.size() == canonical_aliases.rows.size(),
                 "reference alias backlog/canonical row count mismatch");

  std::map<std::string, std::size_t> reference_counts;
  std::map<std::string, std::size_t> alias_kind_counts;

  for (const auto& [reference_surface, canonical] : canonical_by_surface) {
    const auto backlog_it = backlog_by_surface.find(reference_surface);
    harness->Check(backlog_it != backlog_by_surface.end(),
                   reference_surface + " missing from reference alias backlog");
    if (backlog_it == backlog_by_surface.end()) continue;
    const auto& backlog = *backlog_it->second;

    for (const auto column :
         {"reference", "alias_kind", "native_sbsql_surface", "mapping_status",
          "sblr_operation_family", "parser_owned_behavior", "engine_owned_behavior",
          "notes"}) {
      harness->Check(Field(*canonical, column) == Field(backlog, column),
                     reference_surface + " mismatch for " + std::string(column));
    }

    const std::string reference(Field(*canonical, "reference"));
    const std::string alias_kind(Field(*canonical, "alias_kind"));
    ++reference_counts[reference];
    ++alias_kind_counts[alias_kind];

    harness->Check(StartsWith(reference_surface, reference + ":"),
                   reference_surface + " does not start with reference profile");
    harness->Check(policy_by_alias_kind.contains(alias_kind),
                   reference_surface + " has no rendering fixture policy");
    harness->Check(Field(backlog, "mapping_status") ==
                       "mapped_by_profile_or_refused_with_exact_diagnostic",
                   reference_surface + " mapping status is not exact mapped/refused policy");
    harness->Check(!Field(backlog, "native_sbsql_surface").empty(),
                   reference_surface + " missing native SBSQL surface");
    harness->Check(StartsWith(Field(backlog, "sblr_operation_family"), "sblr."),
                   reference_surface + " missing SBLR operation family");
    harness->Check(Contains(Field(backlog, "parser_owned_behavior"), "reference syntax") &&
                       Contains(Field(backlog, "parser_owned_behavior"),
                                "reference error rendering"),
                   reference_surface + " parser-owned behavior lacks reference rendering; lacks compatibility rendering");
    harness->Check(Contains(Field(backlog, "engine_owned_behavior"), "UUID authority") &&
                       Contains(Field(backlog, "engine_owned_behavior"),
                                "SBLR execution"),
                   reference_surface + " engine-owned behavior does not preserve authority");
    harness->Check(Field(backlog, "owner_lane") ==
                       "reference conformance/parser profile worker",
                   reference_surface + " owner lane mismatch");
    harness->Check(Contains(Field(backlog, "target_file_group"),
                            "project/tests/sbsql_parser_worker"),
                   reference_surface + " target file group lacks generated test path");
    harness->Check(Field(backlog, "diagnostic_target") ==
                       "reference_profile_message_vector_rendering",
                   reference_surface + " diagnostic target mismatch");
    harness->Check(StartsWith(Field(backlog, "validation_fixture_id"), "SBSQL-REFERENCE-"),
                   reference_surface + " fixture id is not reference-prefixed");
    harness->Check(Contains(Field(backlog, "final_acceptance_rule"),
                            "reference_rendering_test"),
                   reference_surface + " final acceptance lacks reference rendering test");
    harness->Check(Field(backlog, "closure_action") ==
                       "map_to_native_behavior_or_exact_canonical_refusal",
                   reference_surface + " closure action mismatch");
    harness->Check(!Field(backlog, "status").empty(),
                   reference_surface + " missing status");
  }

  harness->Check(reference_counts.size() == 24, "reference profile count must be 24");
  harness->Check(alias_kind_counts.size() == 13, "alias kind count must be 13");
  for (const auto& [reference, count] : reference_counts) {
    harness->Check(count == 13, reference + " must have 13 alias kinds");
  }
  for (const auto& [alias_kind, count] : alias_kind_counts) {
    harness->Check(count == 24, alias_kind + " must cover 24 reference profiles");
  }
}

} // namespace

int main(int argc, char** argv) {
  if (argc != 4) {
    std::cerr << "usage: sbp_sbsql_reference_alias_rendering_conformance "
                 "<artifact-root> <canonicalization-root> <fixture-policy-csv>\n";
    return 1;
  }

  const std::filesystem::path artifact_root(argv[1]);
  const std::filesystem::path canonicalization_root(argv[2]);
  const std::filesystem::path fixture_policy_csv(argv[3]);
  Harness harness;

  try {
    const auto fixture_policy = ReadCsv(fixture_policy_csv);
    ValidateReport(artifact_root, &harness);
    ValidateCommands(ReadCsv(artifact_root / "VALIDATION_COMMAND_MATERIALIZATION.csv"),
                     &harness);
    ValidateFixturePolicy(fixture_policy, &harness);
    ValidateReferenceMatrices(
        ReadCsv(canonicalization_root / "REFERENCE_ALIAS_TO_SBSQL_SURFACE_MATRIX.csv"),
        ReadCsv(artifact_root / "REFERENCE_ALIAS_COVERAGE_BACKLOG.csv"),
        fixture_policy,
        &harness);

    if (!harness.ok) {
      std::cerr << "FSPE-011B reference alias rendering conformance failed with "
                << harness.failures << " failure(s)\n";
      return 1;
    }

    std::cout << "FSPE-011B reference alias rendering conformance passed: reference_aliases=312 "
                 "reference_profiles=24 alias_kinds=13 fixture_policies="
              << fixture_policy.rows.size() << '\n';
  } catch (const std::exception& ex) {
    std::cerr << "FSPE-011B reference alias rendering conformance failed: "
              << ex.what() << '\n';
    return 1;
  }

  return 0;
}
