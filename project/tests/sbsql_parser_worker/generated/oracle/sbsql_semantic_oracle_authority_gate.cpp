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
constexpr std::size_t kExpectedCanonicalSpecOracleCount = 1988;
constexpr std::size_t kExpectedPromotionOrRefusalOracleCount = 573;
constexpr std::size_t kExpectedClusterProfileOracleCount = 56;

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

void ValidateReport(const std::filesystem::path& artifact_root, Harness* harness) {
  const auto report = ReadText(artifact_root / "SEMANTIC_ORACLE_AUTHORITY_REPORT.md");
  for (const auto token :
       {"Status: complete", "2,617", "Oracle Types", "Authority Rules",
        "closed_by_semantic_oracle_authority_gate",
        "sbsql_semantic_oracle_authority_gate"}) {
    harness->Check(Contains(report, token),
                   std::string("SEMANTIC_ORACLE_AUTHORITY_REPORT.md missing token ") +
                       token);
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
  const auto found = by_name.find("sbsql_semantic_oracle_authority_gate");
  harness->Check(found != by_name.end(), "missing semantic oracle command row");
  if (found == by_name.end()) return;
  const auto& row = *found->second;
  harness->Check(Field(row, "owning_slice") == "FSPE-011C",
                 "semantic oracle command wrong owner");
  harness->Check(Field(row, "materialization_status") == "complete",
                 "semantic oracle command not complete");
  harness->Check(Field(row, "runnable_now") == "yes",
                 "semantic oracle command not runnable");
  harness->Check(Contains(Field(row, "executable_or_contract"),
                          "sbsql_semantic_oracle_authority_gate"),
                 "semantic oracle command does not select gate label");
  harness->Check(Contains(Field(row, "evidence_artifact"),
                          "SEMANTIC_ORACLE_AUTHORITY_REPORT.md") &&
                     Contains(Field(row, "evidence_artifact"),
                              "FSPE_011C_VALIDATION_RESULT.md"),
                 "semantic oracle command evidence incomplete");
}

void ValidateOracles(const CsvTable& oracles,
                     const CsvTable& membership,
                     const CsvTable& surface_backlog,
                     const CsvTable& surface_registry,
                     const CsvTable& operation_matrix,
                     const std::filesystem::path& repo_root,
                     Harness* harness) {
  bool schema_ok = true;
  schema_ok &= RequireColumns(oracles,
                              {"fixture_id", "surface_id", "oracle_type",
                               "oracle_source", "source_search_key",
                               "expected_result_summary", "status"},
                              harness);
  schema_ok &= RequireColumns(membership,
                              {"surface_id", "validation_fixture_id", "batch_id",
                               "ctest_label", "status"},
                              harness);
  schema_ok &= RequireColumns(surface_backlog,
                              {"surface_id", "canonical_name", "canonical_spec",
                               "sblr_operation_family", "diagnostic_target",
                               "validation_fixture_id", "final_acceptance_rule"},
                              harness);
  schema_ok &= RequireColumns(surface_registry,
                              {"surface_id", "canonical_name", "status",
                               "cluster_scope", "canonical_spec",
                               "sblr_operation_family"},
                              harness);
  schema_ok &= RequireColumns(operation_matrix,
                              {"surface_id", "canonical_name",
                               "sblr_operation_family", "required_context",
                               "binding_steps", "result_shape", "diagnostics"},
                              harness);
  if (!schema_ok) return;

  const auto membership_by_id = IndexUnique(membership, "surface_id", harness);
  const auto backlog_by_id = IndexUnique(surface_backlog, "surface_id", harness);
  const auto registry_by_id = IndexUnique(surface_registry, "surface_id", harness);
  const auto operation_by_id = IndexUnique(operation_matrix, "surface_id", harness);

  harness->Check(oracles.rows.size() == kExpectedSurfaceCount,
                 "oracle map must contain corrected authority surface count");
  harness->Check(membership.rows.size() == oracles.rows.size(),
                 "oracle/membership row count mismatch");
  harness->Check(surface_backlog.rows.size() == oracles.rows.size(),
                 "oracle/surface backlog row count mismatch");
  harness->Check(surface_registry.rows.size() == oracles.rows.size(),
                 "oracle/surface registry row count mismatch");
  harness->Check(operation_matrix.rows.size() == oracles.rows.size(),
                 "oracle/operation matrix row count mismatch");

  const std::set<std::string> allowed_types = {
      "canonical_spec_plus_sblr_matrix",
      "promotion_or_canonical_refusal_decision",
      "cluster_profile_and_standalone_refusal_policy",
      "reference_reference_behavior",
      "standards_reference",
      "explicit_scratchbird_design_decision",
  };

  std::map<std::string, std::size_t> type_counts;
  std::unordered_set<std::string> fixture_ids;
  std::unordered_set<std::string> surface_ids;

  for (const auto& row : oracles.rows) {
    const std::string surface_id(Field(row, "surface_id"));
    const std::string fixture_id(Field(row, "fixture_id"));
    const std::string oracle_type(Field(row, "oracle_type"));
    ++type_counts[oracle_type];

    harness->Check(surface_ids.insert(surface_id).second,
                   surface_id + " duplicate oracle surface_id");
    harness->Check(fixture_ids.insert(fixture_id).second,
                   fixture_id + " duplicate oracle fixture_id");
    harness->Check(StartsWith(surface_id, "SBSQL-"), surface_id + " malformed surface_id");
    harness->Check(StartsWith(fixture_id, "SBSQL-SURFACE-"),
                   surface_id + " malformed fixture_id");
    harness->Check(allowed_types.contains(oracle_type),
                   surface_id + " unsupported oracle_type " + oracle_type);
    harness->Check(Field(row, "source_search_key") == surface_id,
                   surface_id + " source_search_key must equal stable surface_id");
    harness->Check(StartsWith(Field(row, "oracle_source"), "public_release_evidence"),
                   surface_id + " oracle_source is not a canonical docs authority path");
    harness->Check(!Contains(Field(row, "oracle_source"), "project/src") &&
                       !Contains(Field(row, "oracle_source"), "project/tests") &&
                       !Contains(Field(row, "oracle_source"), "build/") &&
                       !Contains(Field(row, "oracle_source"), "/tmp"),
                   surface_id + " oracle_source points at implementation or temp output");
    harness->Check(!Field(row, "expected_result_summary").empty(),
                   surface_id + " missing expected_result_summary");
    harness->Check(Field(row, "status") == "closed_by_semantic_oracle_authority_gate",
                   surface_id + " status not closed by semantic oracle gate");

    const std::string oracle_source(Field(row, "oracle_source"));
    const auto fragment = oracle_source.find('#');
    const std::filesystem::path authority_path =
        repo_root / oracle_source.substr(0, fragment);
    harness->Check(std::filesystem::exists(authority_path),
                   surface_id + " oracle_source file does not exist: " +
                       authority_path.string());

    if (oracle_type == "cluster_profile_and_standalone_refusal_policy") {
      harness->Check(Contains(Field(row, "expected_result_summary"), "fail-closed") &&
                         Contains(Field(row, "expected_result_summary"), "profile"),
                     surface_id + " cluster/profile oracle summary is incomplete");
    } else if (oracle_type == "promotion_or_canonical_refusal_decision") {
      harness->Check(Contains(Field(row, "expected_result_summary"), "promotion") ||
                         Contains(Field(row, "expected_result_summary"), "refusal"),
                     surface_id + " promotion/refusal oracle summary is incomplete");
    } else {
      harness->Check(Contains(Field(row, "expected_result_summary"), "expected"),
                     surface_id + " canonical oracle summary lacks expected behavior");
    }

    const auto membership_it = membership_by_id.find(surface_id);
    harness->Check(membership_it != membership_by_id.end(),
                   surface_id + " missing batch membership");
    if (membership_it != membership_by_id.end()) {
      harness->Check(Field(*membership_it->second, "validation_fixture_id") == fixture_id,
                     surface_id + " fixture_id does not match batch membership");
      harness->Check(!Field(*membership_it->second, "batch_id").empty() &&
                         !Field(*membership_it->second, "ctest_label").empty(),
                     surface_id + " membership lacks batch or label");
    }

    const auto backlog_it = backlog_by_id.find(surface_id);
    harness->Check(backlog_it != backlog_by_id.end(),
                   surface_id + " missing surface backlog");
    if (backlog_it != backlog_by_id.end()) {
      harness->Check(Field(*backlog_it->second, "validation_fixture_id") == fixture_id,
                     surface_id + " fixture_id does not match surface backlog");
      harness->Check(Field(*backlog_it->second, "canonical_spec") ==
                         Field(row, "oracle_source"),
                     surface_id + " oracle source does not match canonical spec");
      harness->Check(!Field(*backlog_it->second, "diagnostic_target").empty() &&
                         !Field(*backlog_it->second, "final_acceptance_rule").empty(),
                     surface_id + " backlog lacks diagnostic/final acceptance authority");
    }

    const auto registry_it = registry_by_id.find(surface_id);
    harness->Check(registry_it != registry_by_id.end(),
                   surface_id + " missing canonical surface registry row");
    if (registry_it != registry_by_id.end()) {
      harness->Check(Field(*registry_it->second, "canonical_spec") ==
                         Field(row, "oracle_source"),
                     surface_id + " oracle source does not match canonical registry spec");
      harness->Check(!Field(*registry_it->second, "status").empty() &&
                         !Field(*registry_it->second, "cluster_scope").empty(),
                     surface_id + " registry row lacks status or cluster scope");
    }

    const auto operation_it = operation_by_id.find(surface_id);
    harness->Check(operation_it != operation_by_id.end(),
                   surface_id + " missing operation matrix row");
    if (operation_it != operation_by_id.end() && backlog_it != backlog_by_id.end()) {
      harness->Check(Field(*operation_it->second, "sblr_operation_family") ==
                         Field(*backlog_it->second, "sblr_operation_family"),
                     surface_id + " SBLR operation family mismatch");
      for (const auto column :
           {"required_context", "binding_steps", "result_shape", "diagnostics"}) {
        harness->Check(!Field(*operation_it->second, column).empty(),
                       surface_id + " operation matrix missing " + std::string(column));
      }
    }
    if (operation_it != operation_by_id.end() && registry_it != registry_by_id.end()) {
      harness->Check(Field(*operation_it->second, "sblr_operation_family") ==
                         Field(*registry_it->second, "sblr_operation_family"),
                     surface_id + " SBLR operation family mismatch with registry");
    }
  }

  harness->Check(type_counts["canonical_spec_plus_sblr_matrix"] ==
                     kExpectedCanonicalSpecOracleCount,
                 "canonical_spec_plus_sblr_matrix count mismatch");
  harness->Check(type_counts["promotion_or_canonical_refusal_decision"] ==
                     kExpectedPromotionOrRefusalOracleCount,
                 "promotion_or_canonical_refusal_decision count mismatch");
  harness->Check(type_counts["cluster_profile_and_standalone_refusal_policy"] ==
                     kExpectedClusterProfileOracleCount,
                 "cluster_profile_and_standalone_refusal_policy count mismatch");
}

} // namespace

int main(int argc, char** argv) {
  if (argc != 4) {
    std::cerr << "usage: sbp_sbsql_semantic_oracle_authority_gate "
                 "<artifact-root> <canonicalization-root> <repo-root>\n";
    return 1;
  }

  const std::filesystem::path artifact_root(argv[1]);
  const std::filesystem::path canonicalization_root(argv[2]);
  const std::filesystem::path repo_root(argv[3]);
  Harness harness;

  try {
    ValidateReport(artifact_root, &harness);
    ValidateCommand(ReadCsv(artifact_root / "VALIDATION_COMMAND_MATERIALIZATION.csv"),
                    &harness);
    ValidateOracles(ReadCsv(artifact_root / "SEMANTIC_ORACLE_AUTHORITY_MAP.csv"),
                    ReadCsv(artifact_root / "BATCH_ROW_MEMBERSHIP.csv"),
                    ReadCsv(artifact_root / "SURFACE_IMPLEMENTATION_BACKLOG.csv"),
                    ReadCsv(canonicalization_root / "SBSQL_SURFACE_REGISTRY.csv"),
                    ReadCsv(canonicalization_root / "SBSQL_TO_SBLR_OPERATION_MATRIX.csv"),
                    repo_root,
                    &harness);

    if (!harness.ok) {
      std::cerr << "FSPE-011C semantic oracle authority gate failed with "
                << harness.failures << " failure(s)\n";
      return 1;
    }

    std::cout << "FSPE-011C semantic oracle authority gate passed: "
              << "oracles=" << kExpectedSurfaceCount
              << " canonical_spec=" << kExpectedCanonicalSpecOracleCount
              << " promotion_or_refusal="
              << kExpectedPromotionOrRefusalOracleCount
              << " cluster_profile=" << kExpectedClusterProfileOracleCount
              << "\n";
  } catch (const std::exception& ex) {
    std::cerr << "FSPE-011C semantic oracle authority gate failed: "
              << ex.what() << '\n';
    return 1;
  }

  return 0;
}
