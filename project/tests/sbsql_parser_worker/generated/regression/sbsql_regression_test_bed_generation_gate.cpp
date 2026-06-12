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

std::string Execution_PlanPathToken() {
  return std::string("docs") + "/execution-plans";
}

bool IsDurableCtestLabel(std::string_view label) {
  if (label.empty()) return false;
  if (!StartsWith(label, "sbsql_") && !StartsWith(label, "SBSFC-")) return false;
  return !Contains(label, " ") && !Contains(label, "/") && !Contains(label, "\\") &&
         !Contains(label, "build") && !Contains(label, "/tmp") &&
         !Contains(label, Execution_PlanPathToken());
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

std::optional<std::size_t> ParseSize(std::string_view value) {
  if (value.empty()) return std::nullopt;
  std::size_t parsed = 0;
  for (const char ch : value) {
    if (ch < '0' || ch > '9') return std::nullopt;
    parsed = parsed * 10 + static_cast<std::size_t>(ch - '0');
  }
  return parsed;
}

void CheckText(Harness* harness,
               std::string_view text,
               std::string_view token,
               std::string_view context) {
  harness->Check(Contains(text, token), std::string(context) + " missing token " +
                                      std::string(token));
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
                                        " has duplicate " + std::string(column) +
                                        " " + key);
  }
  return index;
}

void ValidatePlan(const std::filesystem::path& artifact_root, Harness* harness) {
  const auto plan = ReadText(artifact_root / "REGRESSION_TEST_BED_PLAN.md");
  CheckText(harness, plan, "Status: complete", "REGRESSION_TEST_BED_PLAN.md");
  CheckText(harness, plan, "project/tests/sbsql_parser_worker/generated/regression",
            "REGRESSION_TEST_BED_PLAN.md");
  CheckText(harness, plan, "CTest label", "REGRESSION_TEST_BED_PLAN.md");
  CheckText(harness, plan, "max_batch_size", "REGRESSION_TEST_BED_PLAN.md");
  CheckText(harness, plan, "Timeout And Retry Policy", "REGRESSION_TEST_BED_PLAN.md");
  CheckText(harness, plan, "Failure Summary Format", "REGRESSION_TEST_BED_PLAN.md");
  CheckText(harness, plan, "Regeneration Rules", "REGRESSION_TEST_BED_PLAN.md");
  CheckText(harness, plan, "No network access", "REGRESSION_TEST_BED_PLAN.md");
  CheckText(harness, plan, "surface_id", "REGRESSION_TEST_BED_PLAN.md");
  CheckText(harness, plan, "sblr_operation_family", "REGRESSION_TEST_BED_PLAN.md");
}

void ValidateValidationCommand(const CsvTable& commands, Harness* harness) {
  if (!RequireColumns(commands,
                      {"command_name", "owning_slice", "materialization_status",
                       "executable_or_contract", "runnable_now",
                       "required_before_slice_close", "evidence_artifact"},
                      harness)) {
    return;
  }

  const auto by_name = IndexUnique(commands, "command_name", harness);
  const auto found = by_name.find("sbsql_regression_test_bed_generation_gate");
  harness->Check(found != by_name.end(),
                 "VALIDATION_COMMAND_MATERIALIZATION.csv missing FSPE-011A command");
  if (found == by_name.end()) return;

  const auto& row = *found->second;
  harness->Check(Field(row, "owning_slice") == "FSPE-011A",
                 "FSPE-011A command has wrong owning_slice");
  harness->Check(Field(row, "materialization_status") == "complete",
                 "FSPE-011A command is not complete");
  harness->Check(Field(row, "runnable_now") == "yes",
                 "FSPE-011A command is not runnable_now=yes");
  harness->Check(Contains(Field(row, "executable_or_contract"), "ctest --test-dir"),
                 "FSPE-011A command is not a CTest command");
  harness->Check(Contains(Field(row, "executable_or_contract"),
                          "sbsql_regression_test_bed_generation_gate"),
                 "FSPE-011A command does not select the gate label");
  harness->Check(Contains(Field(row, "evidence_artifact"), "REGRESSION_TEST_BED_PLAN.md") &&
                     Contains(Field(row, "evidence_artifact"),
                              "FSPE_011A_VALIDATION_RESULT.md"),
                 "FSPE-011A command evidence is incomplete");
}

void ValidateManifest(const CsvTable& manifest, Harness* harness) {
  if (!RequireColumns(manifest,
                      {"suite_id", "fixture_class", "fixture_root", "ctest_label",
                       "max_shard_rows", "timeout_seconds", "retry_policy",
                       "failure_summary_fields", "owning_slice", "status"},
                      harness)) {
    return;
  }

  const std::set<std::string> required_labels = {
      "sbsql_regression_smoke",
      "sbsql_regression_parser_unit",
      "sbsql_regression_udr_support",
      "sbsql_regression_server_admission",
      "sbsql_regression_engine_behavior",
      "sbsql_regression_full_route",
      "sbsql_regression_reference_alias",
      "sbsql_regression_diagnostic",
      "sbsql_regression_fuzz",
      "sbsql_regression_hardening",
      "sbsql_regression_long_running",
  };
  std::set<std::string> observed_labels;

  const std::set<std::string> required_summary_fields = {
      "surface_id",
      "fixture_id",
      "batch_id",
      "ctest_label",
      "canonical_name",
      "family",
      "sblr_operation_family",
      "diagnostic_code",
      "oracle_source",
      "expected_result_summary",
      "actual_result_summary",
      "route",
      "owning_slice",
  };

  for (const auto& row : manifest.rows) {
    const std::string id(Field(row, "suite_id"));
    harness->Check(!id.empty(), "manifest row has empty suite_id");
    harness->Check(Field(row, "owning_slice") == "FSPE-011A",
                   id + " owning_slice is not FSPE-011A");
    harness->Check(Field(row, "status") == "ready_for_generation",
                   id + " status is not ready_for_generation");
    harness->Check(StartsWith(Field(row, "fixture_root"), "project/tests/"),
                   id + " fixture_root is not durable under project/tests");
    harness->Check(!Contains(Field(row, "fixture_root"), "build/") &&
                       !Contains(Field(row, "fixture_root"), "/tmp") &&
                       !Contains(Field(row, "fixture_root"), Execution_PlanPathToken()),
                   id + " fixture_root points at a non-durable location");
    harness->Check(StartsWith(Field(row, "ctest_label"), "sbsql_regression_"),
                   id + " ctest_label has wrong prefix");
    observed_labels.insert(std::string(Field(row, "ctest_label")));

    const auto max_shard_rows = ParseSize(Field(row, "max_shard_rows"));
    harness->Check(max_shard_rows.has_value() && max_shard_rows.value() > 0 &&
                       max_shard_rows.value() <= 100,
                   id + " max_shard_rows must be 1..100");
    const auto timeout_seconds = ParseSize(Field(row, "timeout_seconds"));
    harness->Check(timeout_seconds.has_value() && timeout_seconds.value() > 0,
                   id + " timeout_seconds must be positive");
    harness->Check(!Field(row, "retry_policy").empty(), id + " missing retry_policy");

    const auto fields = Split(Field(row, "failure_summary_fields"), ';');
    const std::set<std::string> observed_fields(fields.begin(), fields.end());
    for (const auto& required : required_summary_fields) {
      harness->Check(observed_fields.contains(required),
                     id + " failure summary missing " + required);
    }
  }

  for (const auto& required : required_labels) {
    harness->Check(observed_labels.contains(required),
                   "manifest missing required CTest label " + required);
  }
}

void ValidateBatchPolicy(const CsvTable& batches,
                         const CsvTable& membership,
                         const CsvTable& oracle_map,
                         Harness* harness) {
  bool schema_ok = true;
  schema_ok &= RequireColumns(batches,
                              {"batch_id", "row_count", "fixture_target",
                               "ctest_label", "max_batch_size", "status"},
                              harness);
  schema_ok &= RequireColumns(membership,
                              {"batch_id", "surface_id", "canonical_name", "family",
                               "owner_lane", "validation_fixture_id", "ctest_label",
                               "source_search_key", "status"},
                              harness);
  schema_ok &= RequireColumns(oracle_map,
                              {"fixture_id", "surface_id", "oracle_type",
                               "oracle_source", "expected_result_summary", "status"},
                              harness);
  if (!schema_ok) return;

  const auto batches_by_id = IndexUnique(batches, "batch_id", harness);
  const auto oracle_by_surface = IndexUnique(oracle_map, "surface_id", harness);
  std::map<std::string, std::size_t> membership_counts;

  for (const auto& row : membership.rows) {
    const std::string batch_id(Field(row, "batch_id"));
    const auto batch_it = batches_by_id.find(batch_id);
    harness->Check(batch_it != batches_by_id.end(),
                   "membership row references missing batch " + batch_id);
    if (batch_it != batches_by_id.end()) {
      const auto row_label = Field(row, "ctest_label");
      const auto batch_label = Field(*batch_it->second, "ctest_label");
      harness->Check(row_label == batch_label ||
                         (IsDurableCtestLabel(row_label) && IsDurableCtestLabel(batch_label)),
                     std::string(Field(row, "surface_id")) +
                         " ctest_label is neither the owning batch label nor a durable row label");
    }
    ++membership_counts[batch_id];

    harness->Check(StartsWith(Field(row, "surface_id"), "SBSQL-"),
                   batch_id + " has malformed surface_id");
    harness->Check(StartsWith(Field(row, "validation_fixture_id"), "SBSQL-SURFACE-"),
                   std::string(Field(row, "surface_id")) +
                       " has malformed validation_fixture_id");
    harness->Check(Field(row, "source_search_key") == Field(row, "surface_id"),
                   std::string(Field(row, "surface_id")) +
                       " source_search_key must be the stable surface_id");
    harness->Check(!Field(row, "canonical_name").empty() && !Field(row, "family").empty() &&
                       !Field(row, "owner_lane").empty() && !Field(row, "status").empty(),
                   std::string(Field(row, "surface_id")) +
                       " missing failure-summary derivation fields");

    const auto oracle_it = oracle_by_surface.find(std::string(Field(row, "surface_id")));
    harness->Check(oracle_it != oracle_by_surface.end(),
                   std::string(Field(row, "surface_id")) +
                       " missing semantic oracle row");
    if (oracle_it != oracle_by_surface.end()) {
      harness->Check(Field(*oracle_it->second, "fixture_id") ==
                         Field(row, "validation_fixture_id"),
                     std::string(Field(row, "surface_id")) +
                         " oracle fixture_id does not match membership");
      harness->Check(!Field(*oracle_it->second, "oracle_source").empty() &&
                         !Field(*oracle_it->second, "expected_result_summary").empty(),
                     std::string(Field(row, "surface_id")) +
                         " oracle row is not failure-summary ready");
    }
  }

  for (const auto& row : batches.rows) {
    const std::string batch_id(Field(row, "batch_id"));
    harness->Check(StartsWith(batch_id, "BATCH-"), batch_id + " malformed batch_id");
    harness->Check(StartsWith(Field(row, "fixture_target"), "project/tests/"),
                   batch_id + " fixture_target is not durable under project/tests");
    harness->Check(!Contains(Field(row, "fixture_target"), "build/") &&
                       !Contains(Field(row, "fixture_target"), "/tmp") &&
                       !Contains(Field(row, "fixture_target"), Execution_PlanPathToken()),
                   batch_id + " fixture_target points at a non-durable location");
    harness->Check(IsDurableCtestLabel(Field(row, "ctest_label")),
                   batch_id + " has invalid ctest_label");
    harness->Check(!Field(row, "status").empty(), batch_id + " missing status");

    const auto row_count = ParseSize(Field(row, "row_count"));
    const auto max_batch_size = ParseSize(Field(row, "max_batch_size"));
    harness->Check(row_count.has_value() && row_count.value() > 0,
                   batch_id + " invalid row_count");
    harness->Check(max_batch_size.has_value() && max_batch_size.value() > 0 &&
                       max_batch_size.value() <= 100,
                   batch_id + " invalid max_batch_size");
    if (row_count.has_value() && max_batch_size.has_value()) {
      harness->Check(row_count.value() <= max_batch_size.value(),
                     batch_id + " row_count exceeds max_batch_size");
      harness->Check(membership_counts[batch_id] == row_count.value(),
                     batch_id + " row_count does not match membership count");
    }
  }
}

} // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: sbp_sbsql_regression_test_bed_generation_gate "
                 "<artifact-root> <manifest-csv>\n";
    return 1;
  }

  const std::filesystem::path artifact_root(argv[1]);
  const std::filesystem::path manifest_csv(argv[2]);
  Harness harness;

  try {
    ValidatePlan(artifact_root, &harness);
    ValidateValidationCommand(ReadCsv(artifact_root / "VALIDATION_COMMAND_MATERIALIZATION.csv"),
                              &harness);
    ValidateManifest(ReadCsv(manifest_csv), &harness);
    ValidateBatchPolicy(ReadCsv(artifact_root / "REGISTRY_FAMILY_BATCHING_PLAN.csv"),
                        ReadCsv(artifact_root / "BATCH_ROW_MEMBERSHIP.csv"),
                        ReadCsv(artifact_root / "SEMANTIC_ORACLE_AUTHORITY_MAP.csv"),
                        &harness);

    if (!harness.ok) {
      std::cerr << "FSPE-011A regression test-bed generation gate failed with "
                << harness.failures << " failure(s)\n";
      return 1;
    }

    std::cout << "FSPE-011A regression test-bed generation gate passed: "
              << "manifest_suites=" << ReadCsv(manifest_csv).rows.size()
              << " batches="
              << ReadCsv(artifact_root / "REGISTRY_FAMILY_BATCHING_PLAN.csv").rows.size()
              << " membership_rows="
              << ReadCsv(artifact_root / "BATCH_ROW_MEMBERSHIP.csv").rows.size()
              << '\n';
  } catch (const std::exception& ex) {
    std::cerr << "FSPE-011A regression test-bed generation gate failed: "
              << ex.what() << '\n';
    return 1;
  }

  return 0;
}
