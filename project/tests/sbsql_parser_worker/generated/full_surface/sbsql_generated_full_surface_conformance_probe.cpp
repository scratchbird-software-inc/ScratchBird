// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "registry/generated/sbsql_generated_registry.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sbsql = scratchbird::parser::sbsql;

namespace {

using CsvRow = std::unordered_map<std::string, std::string>;
using RowIndex = std::unordered_map<std::string, const CsvRow*>;

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

std::string LowerAscii(std::string_view value) {
  std::string lowered(value);
  std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return lowered;
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

RowIndex IndexUnique(const CsvTable& table,
                     std::string_view key_column,
                     std::string_view table_name,
                     Harness* harness) {
  RowIndex index;
  for (const auto& row : table.rows) {
    const auto key = std::string(Field(row, key_column));
    harness->Check(!key.empty(), std::string(table_name) + " row has empty " +
                                     std::string(key_column));
    if (key.empty()) continue;
    const auto inserted = index.emplace(key, &row);
    harness->Check(inserted.second, std::string(table_name) + " duplicate " +
                                        std::string(key_column) + " " + key);
  }
  return index;
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

void CheckEqual(Harness* harness,
                std::string_view context,
                std::string_view field,
                std::string_view expected,
                std::string_view actual) {
  harness->Check(expected == actual, std::string(context) + " " + std::string(field) +
                                       " mismatch: expected '" + std::string(expected) +
                                       "' got '" + std::string(actual) + "'");
}

void CheckRequiredField(Harness* harness,
                        std::string_view context,
                        std::string_view field,
                        std::string_view value) {
  harness->Check(!value.empty(), std::string(context) + " missing " + std::string(field));
}

void CheckGeneratedRowShape(Harness* harness,
                            const sbsql::GeneratedSurfaceRegistryRow& row) {
  const std::string context(row.surface_id);
  CheckRequiredField(harness, context, "surface_id", row.surface_id);
  CheckRequiredField(harness, context, "fixed_uuid_v7", row.fixed_uuid_v7);
  CheckRequiredField(harness, context, "canonical_name", row.canonical_name);
  CheckRequiredField(harness, context, "surface_kind", row.surface_kind);
  CheckRequiredField(harness, context, "family", row.family);
  CheckRequiredField(harness, context, "source_status", row.source_status);
  CheckRequiredField(harness, context, "cluster_scope", row.cluster_scope);
  CheckRequiredField(harness, context, "canonical_spec", row.canonical_spec);
  CheckRequiredField(harness, context, "sblr_operation_family", row.sblr_operation_family);
  CheckRequiredField(harness, context, "parser_packet", row.parser_packet);
  CheckRequiredField(harness, context, "engine_packet", row.engine_packet);
  CheckRequiredField(harness, context, "owner_lane", row.owner_lane);
  CheckRequiredField(harness, context, "batch_id", row.batch_id);
  CheckRequiredField(harness, context, "ctest_label", row.ctest_label);
  CheckRequiredField(harness, context, "parser_handler_key", row.parser_handler_key);
  CheckRequiredField(harness, context, "udr_handler_key", row.udr_handler_key);
  CheckRequiredField(harness, context, "lowering_handler_key", row.lowering_handler_key);
  CheckRequiredField(harness, context, "server_admission_key", row.server_admission_key);
  CheckRequiredField(harness, context, "engine_rule_key", row.engine_rule_key);
  CheckRequiredField(harness, context, "diagnostic_key", row.diagnostic_key);
  CheckRequiredField(harness, context, "oracle_key", row.oracle_key);
  CheckRequiredField(harness, context, "validation_fixture_id", row.validation_fixture_id);
  CheckRequiredField(harness, context, "final_acceptance_rule", row.final_acceptance_rule);
  CheckRequiredField(harness, context, "closure_action", row.closure_action);

  harness->Check(StartsWith(row.surface_id, "SBSQL-") && row.surface_id.size() == 18,
                 context + " has malformed surface_id");
  harness->Check(row.fixed_uuid_v7.size() == 36, context + " has malformed fixed_uuid_v7");
  harness->Check(StartsWith(row.validation_fixture_id, "SBSQL-SURFACE-"),
                 context + " has malformed validation_fixture_id");
  harness->Check(StartsWith(row.batch_id, "BATCH-"), context + " has malformed batch_id");
}

void ValidateSurfaceMatrices(std::span<const sbsql::GeneratedSurfaceRegistryRow> rows,
                             const CsvTable& canonical_surfaces,
                             const CsvTable& surface_status,
                             const CsvTable& operation_matrix,
                             const CsvTable& surface_backlog,
                             const CsvTable& batch_membership,
                             const CsvTable& oracle_map,
                             const CsvTable& batching_plan,
                             Harness* harness) {
  bool schema_ok = true;
  schema_ok &= RequireColumns(canonical_surfaces,
                              {"surface_id", "fixed_uuid_v7", "canonical_name",
                               "surface_kind", "family", "status", "cluster_scope",
                               "canonical_spec", "sblr_operation_family", "parser_packet",
                               "engine_packet"},
                              harness);
  schema_ok &= RequireColumns(surface_status,
                              {"surface_id", "canonical_name", "status", "allowed_lowering",
                               "diagnostic_if_not_allowed"},
                              harness);
  schema_ok &= RequireColumns(operation_matrix,
                              {"surface_id", "canonical_name", "sblr_operation_family",
                               "ingress_envelope", "required_context", "binding_steps",
                               "result_shape", "diagnostics"},
                              harness);
  schema_ok &= RequireColumns(surface_backlog,
                              {"surface_id", "fixed_uuid_v7", "canonical_name",
                               "surface_kind", "family", "source_status", "cluster_scope",
                               "canonical_spec", "sblr_operation_family", "parser_packet",
                               "engine_packet", "owner_lane", "target_file_group",
                               "parser_target_behavior", "udr_target_behavior",
                               "server_target_behavior", "engine_target_behavior",
                               "diagnostic_target", "validation_fixture_id",
                               "final_acceptance_rule", "closure_action", "status"},
                              harness);
  schema_ok &= RequireColumns(batch_membership,
                              {"batch_id", "surface_id", "fixed_uuid_v7", "canonical_name",
                               "family", "surface_kind", "source_status", "cluster_scope",
                               "owner_lane", "validation_fixture_id", "ctest_label",
                               "source_search_key", "status"},
                              harness);
  schema_ok &= RequireColumns(oracle_map,
                              {"fixture_id", "surface_id", "oracle_type", "oracle_source",
                               "source_search_key", "expected_result_summary", "status"},
                              harness);
  schema_ok &= RequireColumns(batching_plan,
                              {"batch_id", "source_matrix", "surface_filter", "row_count",
                               "owner_lane", "fixture_target", "ctest_label", "status"},
                              harness);
  if (!schema_ok) return;

  const auto canonical_by_id =
      IndexUnique(canonical_surfaces, "surface_id", "SBSQL_SURFACE_REGISTRY", harness);
  const auto status_by_id =
      IndexUnique(surface_status, "surface_id", "SBSQL_SURFACE_STATUS_MATRIX", harness);
  const auto operation_by_id =
      IndexUnique(operation_matrix, "surface_id", "SBSQL_TO_SBLR_OPERATION_MATRIX", harness);
  const auto backlog_by_id =
      IndexUnique(surface_backlog, "surface_id", "SURFACE_IMPLEMENTATION_BACKLOG", harness);
  const auto batch_by_id =
      IndexUnique(batch_membership, "surface_id", "BATCH_ROW_MEMBERSHIP", harness);
  const auto oracle_by_id =
      IndexUnique(oracle_map, "surface_id", "SEMANTIC_ORACLE_AUTHORITY_MAP", harness);
  const auto plan_by_batch =
      IndexUnique(batching_plan, "batch_id", "REGISTRY_FAMILY_BATCHING_PLAN", harness);

  harness->Check(canonical_surfaces.rows.size() == rows.size(),
                 "SBSQL_SURFACE_REGISTRY row count does not match generated registry");
  harness->Check(surface_status.rows.size() == rows.size(),
                 "SBSQL_SURFACE_STATUS_MATRIX row count does not match generated registry");
  harness->Check(operation_matrix.rows.size() == rows.size(),
                 "SBSQL_TO_SBLR_OPERATION_MATRIX row count does not match generated registry");
  harness->Check(surface_backlog.rows.size() == rows.size(),
                 "SURFACE_IMPLEMENTATION_BACKLOG row count does not match generated registry");
  harness->Check(batch_membership.rows.size() == rows.size(),
                 "BATCH_ROW_MEMBERSHIP row count does not match generated registry");
  harness->Check(oracle_map.rows.size() == rows.size(),
                 "SEMANTIC_ORACLE_AUTHORITY_MAP row count does not match generated registry");

  std::unordered_set<std::string> surface_ids;
  std::unordered_set<std::string> fixed_uuids;
  std::unordered_set<std::string> fixture_ids;
  std::map<std::string, std::size_t> observed_batch_counts;

  for (const auto& row : rows) {
    const std::string surface_id(row.surface_id);
    CheckGeneratedRowShape(harness, row);
    harness->Check(surface_ids.insert(surface_id).second,
                   surface_id + " duplicate generated surface_id");
    harness->Check(fixed_uuids.insert(std::string(row.fixed_uuid_v7)).second,
                   surface_id + " duplicate generated fixed_uuid_v7");
    harness->Check(fixture_ids.insert(std::string(row.validation_fixture_id)).second,
                   surface_id + " duplicate generated validation_fixture_id");
    ++observed_batch_counts[std::string(row.batch_id)];

    const auto canonical_it = canonical_by_id.find(surface_id);
    harness->Check(canonical_it != canonical_by_id.end(),
                   surface_id + " missing from SBSQL_SURFACE_REGISTRY");
    if (canonical_it != canonical_by_id.end()) {
      const auto& canonical = *canonical_it->second;
      CheckEqual(harness, surface_id, "fixed_uuid_v7", row.fixed_uuid_v7,
                 Field(canonical, "fixed_uuid_v7"));
      CheckEqual(harness, surface_id, "canonical_name", row.canonical_name,
                 Field(canonical, "canonical_name"));
      CheckEqual(harness, surface_id, "surface_kind", row.surface_kind,
                 Field(canonical, "surface_kind"));
      CheckEqual(harness, surface_id, "family", row.family, Field(canonical, "family"));
      CheckEqual(harness, surface_id, "source_status", row.source_status,
                 Field(canonical, "status"));
      CheckEqual(harness, surface_id, "cluster_scope", row.cluster_scope,
                 Field(canonical, "cluster_scope"));
      CheckEqual(harness, surface_id, "canonical_spec", row.canonical_spec,
                 Field(canonical, "canonical_spec"));
      CheckEqual(harness, surface_id, "sblr_operation_family", row.sblr_operation_family,
                 Field(canonical, "sblr_operation_family"));
      CheckEqual(harness, surface_id, "parser_packet", row.parser_packet,
                 Field(canonical, "parser_packet"));
      CheckEqual(harness, surface_id, "engine_packet", row.engine_packet,
                 Field(canonical, "engine_packet"));
    }

    const auto status_it = status_by_id.find(surface_id);
    harness->Check(status_it != status_by_id.end(),
                   surface_id + " missing from SBSQL_SURFACE_STATUS_MATRIX");
    if (status_it != status_by_id.end()) {
      const auto& status = *status_it->second;
      CheckEqual(harness, surface_id, "status canonical_name", row.canonical_name,
                 Field(status, "canonical_name"));
      CheckEqual(harness, surface_id, "status", row.source_status, Field(status, "status"));
      CheckRequiredField(harness, surface_id, "allowed_lowering",
                         Field(status, "allowed_lowering"));
    }

    const auto operation_it = operation_by_id.find(surface_id);
    harness->Check(operation_it != operation_by_id.end(),
                   surface_id + " missing from SBSQL_TO_SBLR_OPERATION_MATRIX");
    if (operation_it != operation_by_id.end()) {
      const auto& operation = *operation_it->second;
      CheckEqual(harness, surface_id, "operation canonical_name", row.canonical_name,
                 Field(operation, "canonical_name"));
      CheckEqual(harness, surface_id, "operation family", row.sblr_operation_family,
                 Field(operation, "sblr_operation_family"));
      CheckEqual(harness, surface_id, "ingress_envelope", "SBLRExecutionEnvelope.v3",
                 Field(operation, "ingress_envelope"));
      for (const auto column :
           {"required_context", "binding_steps", "result_shape", "diagnostics"}) {
        CheckRequiredField(harness, surface_id, column, Field(operation, column));
      }
    }

    const auto backlog_it = backlog_by_id.find(surface_id);
    harness->Check(backlog_it != backlog_by_id.end(),
                   surface_id + " missing from SURFACE_IMPLEMENTATION_BACKLOG");
    if (backlog_it != backlog_by_id.end()) {
      const auto& backlog = *backlog_it->second;
      CheckEqual(harness, surface_id, "backlog fixed_uuid_v7", row.fixed_uuid_v7,
                 Field(backlog, "fixed_uuid_v7"));
      CheckEqual(harness, surface_id, "backlog canonical_name", row.canonical_name,
                 Field(backlog, "canonical_name"));
      CheckEqual(harness, surface_id, "backlog surface_kind", row.surface_kind,
                 Field(backlog, "surface_kind"));
      CheckEqual(harness, surface_id, "backlog family", row.family,
                 Field(backlog, "family"));
      CheckEqual(harness, surface_id, "backlog source_status", row.source_status,
                 Field(backlog, "source_status"));
      CheckEqual(harness, surface_id, "backlog cluster_scope", row.cluster_scope,
                 Field(backlog, "cluster_scope"));
      CheckEqual(harness, surface_id, "backlog sblr_operation_family",
                 row.sblr_operation_family, Field(backlog, "sblr_operation_family"));
      CheckEqual(harness, surface_id, "backlog validation_fixture_id",
                 row.validation_fixture_id, Field(backlog, "validation_fixture_id"));
      for (const auto column :
           {"target_file_group", "parser_target_behavior", "udr_target_behavior",
            "server_target_behavior", "engine_target_behavior", "diagnostic_target",
            "final_acceptance_rule", "closure_action", "status"}) {
        CheckRequiredField(harness, surface_id, column, Field(backlog, column));
      }
    }

    const auto batch_it = batch_by_id.find(surface_id);
    harness->Check(batch_it != batch_by_id.end(),
                   surface_id + " missing from BATCH_ROW_MEMBERSHIP");
    if (batch_it != batch_by_id.end()) {
      const auto& batch = *batch_it->second;
      CheckEqual(harness, surface_id, "batch_id", row.batch_id, Field(batch, "batch_id"));
      CheckEqual(harness, surface_id, "batch fixed_uuid_v7", row.fixed_uuid_v7,
                 Field(batch, "fixed_uuid_v7"));
      CheckEqual(harness, surface_id, "batch canonical_name", row.canonical_name,
                 Field(batch, "canonical_name"));
      CheckEqual(harness, surface_id, "batch family", row.family, Field(batch, "family"));
      CheckEqual(harness, surface_id, "batch surface_kind", row.surface_kind,
                 Field(batch, "surface_kind"));
      CheckEqual(harness, surface_id, "batch source_status", row.source_status,
                 Field(batch, "source_status"));
      CheckEqual(harness, surface_id, "batch cluster_scope", row.cluster_scope,
                 Field(batch, "cluster_scope"));
      CheckEqual(harness, surface_id, "batch owner_lane", row.owner_lane,
                 Field(batch, "owner_lane"));
      CheckEqual(harness, surface_id, "batch validation_fixture_id",
                 row.validation_fixture_id, Field(batch, "validation_fixture_id"));
      CheckEqual(harness, surface_id, "batch ctest_label", row.ctest_label,
                 Field(batch, "ctest_label"));
      CheckEqual(harness, surface_id, "batch source_search_key", row.surface_id,
                 Field(batch, "source_search_key"));
      CheckRequiredField(harness, surface_id, "batch status", Field(batch, "status"));
    }

    const auto oracle_it = oracle_by_id.find(surface_id);
    harness->Check(oracle_it != oracle_by_id.end(),
                   surface_id + " missing from SEMANTIC_ORACLE_AUTHORITY_MAP");
    if (oracle_it != oracle_by_id.end()) {
      const auto& oracle = *oracle_it->second;
      CheckEqual(harness, surface_id, "oracle fixture_id", row.validation_fixture_id,
                 Field(oracle, "fixture_id"));
      CheckEqual(harness, surface_id, "oracle_type", row.oracle_key,
                 Field(oracle, "oracle_type"));
      CheckEqual(harness, surface_id, "oracle_source", row.canonical_spec,
                 Field(oracle, "oracle_source"));
      CheckEqual(harness, surface_id, "oracle source_search_key", row.surface_id,
                 Field(oracle, "source_search_key"));
      CheckRequiredField(harness, surface_id, "oracle expected_result_summary",
                         Field(oracle, "expected_result_summary"));
      CheckRequiredField(harness, surface_id, "oracle status", Field(oracle, "status"));
    }
  }

  for (const auto& [batch_id, observed_count] : observed_batch_counts) {
    const auto plan_it = plan_by_batch.find(batch_id);
    harness->Check(plan_it != plan_by_batch.end(),
                   batch_id + " missing from REGISTRY_FAMILY_BATCHING_PLAN");
    if (plan_it == plan_by_batch.end()) continue;
    const auto planned_count = ParseSize(Field(*plan_it->second, "row_count"));
    harness->Check(planned_count.has_value(), batch_id + " has invalid planned row_count");
    if (planned_count.has_value()) {
      harness->Check(planned_count.value() == observed_count,
                     batch_id + " planned row_count does not match membership");
    }
    CheckRequiredField(harness, batch_id, "ctest_label",
                       Field(*plan_it->second, "ctest_label"));
    CheckRequiredField(harness, batch_id, "status", Field(*plan_it->second, "status"));
  }
}

void ValidateEngineGapBacklog(const CsvTable& canonical_gaps,
                              const CsvTable& artifact_gaps,
                              Harness* harness) {
  bool schema_ok = true;
  schema_ok &= RequireColumns(canonical_gaps,
                              {"gap_id", "source_file", "source_anchor", "gap_type",
                               "required_behavior", "target_packet", "cluster_scope",
                               "current_status", "required_decision"},
                              harness);
  schema_ok &= RequireColumns(artifact_gaps,
                              {"gap_id", "source_file", "source_anchor", "source_gap_type",
                               "required_behavior", "target_packet", "cluster_scope",
                               "source_status", "required_decision", "owner_lane",
                               "target_file_group", "server_target_behavior",
                               "engine_target_behavior", "diagnostic_target",
                               "validation_fixture_id", "final_acceptance_rule",
                               "closure_action", "status"},
                              harness);
  if (!schema_ok) return;

  const auto canonical_by_id =
      IndexUnique(canonical_gaps, "gap_id", "SBSQL_ENGINE_GAP_MATRIX", harness);
  const auto artifact_by_id =
      IndexUnique(artifact_gaps, "gap_id", "ENGINE_GAP_IMPLEMENTATION_BACKLOG", harness);

  harness->Check(canonical_gaps.rows.size() == artifact_gaps.rows.size(),
                 "engine gap canonical/artifact row count mismatch");

  for (const auto& [gap_id, canonical] : canonical_by_id) {
    const auto artifact_it = artifact_by_id.find(gap_id);
    harness->Check(artifact_it != artifact_by_id.end(),
                   gap_id + " missing from ENGINE_GAP_IMPLEMENTATION_BACKLOG");
    if (artifact_it == artifact_by_id.end()) continue;
    const auto& artifact = *artifact_it->second;

    CheckEqual(harness, gap_id, "source_file", Field(*canonical, "source_file"),
               Field(artifact, "source_file"));
    CheckEqual(harness, gap_id, "source_anchor", Field(*canonical, "source_anchor"),
               Field(artifact, "source_anchor"));
    CheckEqual(harness, gap_id, "gap_type", Field(*canonical, "gap_type"),
               Field(artifact, "source_gap_type"));
    CheckEqual(harness, gap_id, "required_behavior", Field(*canonical, "required_behavior"),
               Field(artifact, "required_behavior"));
    CheckEqual(harness, gap_id, "target_packet", Field(*canonical, "target_packet"),
               Field(artifact, "target_packet"));
    CheckEqual(harness, gap_id, "cluster_scope", Field(*canonical, "cluster_scope"),
               Field(artifact, "cluster_scope"));
    CheckEqual(harness, gap_id, "status", Field(*canonical, "current_status"),
               Field(artifact, "status"));
    CheckEqual(harness, gap_id, "required_decision", Field(*canonical, "required_decision"),
               Field(artifact, "required_decision"));

    const auto cluster_scope = Field(*canonical, "cluster_scope");
    const auto status = Field(*canonical, "current_status");
    if (cluster_scope == "cluster_private") {
      CheckEqual(harness, gap_id, "cluster-private closure", "closed_by_cluster_fail_closed_gate",
                 status);
    } else {
      CheckEqual(harness, gap_id, "non-cluster closure",
                 "closed_by_engine_api_sblr_family_gate", status);
    }

    harness->Check(StartsWith(gap_id, "GAP-"), gap_id + " has malformed gap_id");
    harness->Check(StartsWith(Field(artifact, "validation_fixture_id"), "SBSQL-GAP-"),
                   gap_id + " has malformed validation_fixture_id");
    for (const auto column :
         {"owner_lane", "target_file_group", "server_target_behavior",
          "engine_target_behavior", "diagnostic_target", "final_acceptance_rule",
          "closure_action", "status"}) {
      CheckRequiredField(harness, gap_id, column, Field(artifact, column));
    }
  }
}

void ValidateReferenceAliasBacklog(const CsvTable& canonical_aliases,
                               const CsvTable& artifact_aliases,
                               Harness* harness) {
  bool schema_ok = true;
  schema_ok &= RequireColumns(canonical_aliases,
                              {"reference", "alias_kind", "reference_surface",
                               "native_sbsql_surface", "mapping_status",
                               "sblr_operation_family", "parser_owned_behavior",
                               "engine_owned_behavior", "notes"},
                              harness);
  schema_ok &= RequireColumns(artifact_aliases,
                              {"reference", "alias_kind", "reference_surface",
                               "native_sbsql_surface", "mapping_status",
                               "sblr_operation_family", "parser_owned_behavior",
                               "engine_owned_behavior", "owner_lane",
                               "target_file_group", "diagnostic_target",
                               "validation_fixture_id", "final_acceptance_rule",
                               "closure_action", "status", "notes"},
                              harness);
  if (!schema_ok) return;

  const auto canonical_by_surface = IndexUnique(
      canonical_aliases, "reference_surface", "REFERENCE_ALIAS_TO_SBSQL_SURFACE_MATRIX", harness);
  const auto artifact_by_surface =
      IndexUnique(artifact_aliases, "reference_surface", "REFERENCE_ALIAS_COVERAGE_BACKLOG", harness);

  harness->Check(canonical_aliases.rows.size() == artifact_aliases.rows.size(),
                 "reference alias canonical/artifact row count mismatch");

  for (const auto& [reference_surface, canonical] : canonical_by_surface) {
    const auto artifact_it = artifact_by_surface.find(reference_surface);
    harness->Check(artifact_it != artifact_by_surface.end(),
                   reference_surface + " missing from REFERENCE_ALIAS_COVERAGE_BACKLOG");
    if (artifact_it == artifact_by_surface.end()) continue;
    const auto& artifact = *artifact_it->second;

    for (const auto column :
         {"reference", "alias_kind", "native_sbsql_surface", "mapping_status",
          "sblr_operation_family", "parser_owned_behavior", "engine_owned_behavior",
          "notes"}) {
      CheckEqual(harness, reference_surface, column, Field(*canonical, column),
                 Field(artifact, column));
    }

    CheckEqual(harness, reference_surface, "mapping_status",
               "mapped_by_profile_or_refused_with_exact_diagnostic",
               Field(artifact, "mapping_status"));
    harness->Check(StartsWith(Field(artifact, "validation_fixture_id"), "SBSQL-REFERENCE-"),
                   reference_surface + " has malformed validation_fixture_id");
    for (const auto column :
         {"owner_lane", "target_file_group", "diagnostic_target", "final_acceptance_rule",
          "closure_action", "status"}) {
      CheckRequiredField(harness, reference_surface, column, Field(artifact, column));
    }
  }
}

void ValidateMessageVectorFixtures(const CsvTable& message_vectors, Harness* harness) {
  const bool schema_ok =
      RequireColumns(message_vectors,
                     {"backlog_id", "origin", "subsystem", "error_condition",
                      "diagnostic_code", "message_vector_fields",
                      "parser_rendering_template", "redaction_policy",
                      "conformance_fixture", "status"},
                     harness);
  if (!schema_ok) return;

  harness->Check(message_vectors.rows.size() >= 30,
                 "MESSAGE_VECTOR_COVERAGE_BACKLOG has fewer than 30 fixture rows");

  std::unordered_set<std::string> backlog_ids;
  std::unordered_set<std::string> diagnostic_codes;
  std::set<std::string> origins;
  std::set<std::string> diagnostic_prefixes;
  for (const auto& row : message_vectors.rows) {
    const std::string id(Field(row, "backlog_id"));
    harness->Check(StartsWith(id, "MV-"), id + " has malformed backlog_id");
    harness->Check(backlog_ids.insert(id).second, id + " duplicate message-vector backlog_id");
    harness->Check(diagnostic_codes.insert(std::string(Field(row, "diagnostic_code"))).second,
                   id + " duplicate diagnostic_code");
    CheckRequiredField(harness, id, "origin", Field(row, "origin"));
    CheckRequiredField(harness, id, "subsystem", Field(row, "subsystem"));
    CheckRequiredField(harness, id, "error_condition", Field(row, "error_condition"));
    CheckRequiredField(harness, id, "diagnostic_code", Field(row, "diagnostic_code"));
    CheckRequiredField(harness, id, "message_vector_fields",
                       Field(row, "message_vector_fields"));
    CheckRequiredField(harness, id, "parser_rendering_template",
                       Field(row, "parser_rendering_template"));
    CheckRequiredField(harness, id, "redaction_policy", Field(row, "redaction_policy"));
    CheckRequiredField(harness, id, "conformance_fixture",
                       Field(row, "conformance_fixture"));
    CheckRequiredField(harness, id, "status", Field(row, "status"));

    harness->Check(Contains(Field(row, "diagnostic_code"), "."),
                   id + " diagnostic_code lacks subsystem separator");
    harness->Check(Split(Field(row, "message_vector_fields"), ';').size() >= 3,
                   id + " message_vector_fields has fewer than 3 fields");
    harness->Check(StartsWith(Field(row, "conformance_fixture"), "MSGV-"),
                   id + " conformance_fixture must be MSGV-prefixed");

    const auto lowered = LowerAscii(std::string(Field(row, "diagnostic_code")) +
                                    std::string(Field(row, "message_vector_fields")) +
                                    std::string(Field(row, "parser_rendering_template")) +
                                    std::string(Field(row, "redaction_policy")) +
                                    std::string(Field(row, "conformance_fixture")));
    harness->Check(!Contains(lowered, "todo") && !Contains(lowered, "stub") &&
                       !Contains(lowered, "tbd"),
                   id + " message-vector fixture still contains placeholder text");

    origins.insert(std::string(Field(row, "origin")));
    const auto code = std::string(Field(row, "diagnostic_code"));
    const auto dot = code.find('.');
    if (dot != std::string::npos) diagnostic_prefixes.insert(code.substr(0, dot));
  }

  for (const auto required_origin :
       {"agent", "server", "engine", "parser", "udr", "listener", "manager"}) {
    harness->Check(origins.contains(required_origin),
                   std::string("MESSAGE_VECTOR_COVERAGE_BACKLOG missing origin ") +
                       required_origin);
  }
  for (const auto required_prefix :
       {"AGENT", "SERVER", "ENGINE", "SBSQL", "UDR", "LISTENER", "MANAGER"}) {
    harness->Check(diagnostic_prefixes.contains(required_prefix),
                   std::string("MESSAGE_VECTOR_COVERAGE_BACKLOG missing diagnostic prefix ") +
                       required_prefix);
  }
}

} // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: sbp_sbsql_generated_full_surface_conformance_probe "
                 "<artifact-root> <canonicalization-root>\n";
    return 1;
  }

  const std::filesystem::path artifact_root(argv[1]);
  const std::filesystem::path canonicalization_root(argv[2]);
  Harness harness;

  try {
    const auto rows = sbsql::GeneratedSurfaceRegistryRows();
    harness.Check(rows.size() == sbsql::kGeneratedSurfaceRegistryRowCount,
                  "generated registry row count does not match generated constant");

    const auto canonical_surfaces =
        ReadCsv(canonicalization_root / "SBSQL_SURFACE_REGISTRY.csv");
    const auto surface_status =
        ReadCsv(canonicalization_root / "SBSQL_SURFACE_STATUS_MATRIX.csv");
    const auto operation_matrix =
        ReadCsv(canonicalization_root / "SBSQL_TO_SBLR_OPERATION_MATRIX.csv");
    const auto canonical_gaps =
        ReadCsv(canonicalization_root / "SBSQL_ENGINE_GAP_MATRIX.csv");
    const auto canonical_aliases =
        ReadCsv(canonicalization_root / "REFERENCE_ALIAS_TO_SBSQL_SURFACE_MATRIX.csv");

    const auto surface_backlog = ReadCsv(artifact_root / "SURFACE_IMPLEMENTATION_BACKLOG.csv");
    const auto batch_membership = ReadCsv(artifact_root / "BATCH_ROW_MEMBERSHIP.csv");
    const auto oracle_map = ReadCsv(artifact_root / "SEMANTIC_ORACLE_AUTHORITY_MAP.csv");
    const auto batching_plan = ReadCsv(artifact_root / "REGISTRY_FAMILY_BATCHING_PLAN.csv");
    const auto artifact_gaps = ReadCsv(artifact_root / "ENGINE_GAP_IMPLEMENTATION_BACKLOG.csv");
    const auto artifact_aliases = ReadCsv(artifact_root / "REFERENCE_ALIAS_COVERAGE_BACKLOG.csv");
    const auto message_vectors = ReadCsv(artifact_root / "MESSAGE_VECTOR_COVERAGE_BACKLOG.csv");

    ValidateSurfaceMatrices(rows, canonical_surfaces, surface_status, operation_matrix,
                            surface_backlog, batch_membership, oracle_map, batching_plan,
                            &harness);
    ValidateEngineGapBacklog(canonical_gaps, artifact_gaps, &harness);
    ValidateReferenceAliasBacklog(canonical_aliases, artifact_aliases, &harness);
    ValidateMessageVectorFixtures(message_vectors, &harness);

    if (!harness.ok) {
      std::cerr << "FSPE-011 generated full-surface conformance failed with "
                << harness.failures << " failure(s)\n";
      return 1;
    }

    std::set<std::string> operation_families;
    for (const auto& row : operation_matrix.rows) {
      operation_families.insert(std::string(Field(row, "sblr_operation_family")));
    }

    std::cout << "FSPE-011 generated full-surface conformance passed: surfaces="
              << rows.size() << " operation_rows=" << operation_matrix.rows.size()
              << " operation_families=" << operation_families.size()
              << " engine_gaps=" << artifact_gaps.rows.size()
              << " reference_aliases=" << artifact_aliases.rows.size()
              << " message_vectors=" << message_vectors.rows.size()
              << " batches=" << batching_plan.rows.size() << '\n';
  } catch (const std::exception& ex) {
    std::cerr << "FSPE-011 generated full-surface conformance failed: " << ex.what()
              << '\n';
    return 1;
  }

  return 0;
}
