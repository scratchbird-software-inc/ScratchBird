// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "registry/generated/sbsql_generated_registry.hpp"

#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sbsql = scratchbird::parser::sbsql;

namespace {

bool RequireField(std::string_view surface_id,
                  std::string_view field,
                  std::string_view value) {
  if (!value.empty()) return true;
  std::cerr << "SBSQL generated registry row " << surface_id
            << " has empty required field " << field << "\n";
  return false;
}

bool HasPrefix(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
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

using CsvRow = std::unordered_map<std::string, std::string>;

std::vector<CsvRow> ReadCsv(const std::string& path) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("failed to open " + path);
  }

  std::string line;
  if (!std::getline(input, line)) {
    throw std::runtime_error("empty CSV " + path);
  }
  if (!line.empty() && line.back() == '\r') line.pop_back();
  const auto headers = SplitCsvLine(line);

  std::vector<CsvRow> rows;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    const auto fields = SplitCsvLine(line);
    if (fields.size() != headers.size()) {
      throw std::runtime_error("malformed CSV row in " + path);
    }
    CsvRow row;
    for (std::size_t index = 0; index < headers.size(); ++index) {
      row.emplace(headers[index], fields[index]);
    }
    rows.push_back(std::move(row));
  }

  return rows;
}

std::unordered_set<std::string_view> GeneratedSurfaceIdSet(
    std::span<const sbsql::GeneratedSurfaceRegistryRow> rows) {
  std::unordered_set<std::string_view> ids;
  for (const auto& row : rows) ids.insert(row.surface_id);
  return ids;
}

bool ValidateArtifactSurfaceCoverage(
    const std::string& artifact_root,
    std::span<const sbsql::GeneratedSurfaceRegistryRow> generated_rows) {
  bool ok = true;
  const auto generated_ids = GeneratedSurfaceIdSet(generated_rows);
  const auto backlog = ReadCsv(artifact_root + "/SURFACE_IMPLEMENTATION_BACKLOG.csv");
  const auto batches = ReadCsv(artifact_root + "/BATCH_ROW_MEMBERSHIP.csv");
  const auto oracles = ReadCsv(artifact_root + "/SEMANTIC_ORACLE_AUTHORITY_MAP.csv");

  if (backlog.size() != generated_rows.size()) {
    std::cerr << "artifact/generated surface count mismatch: artifact=" << backlog.size()
              << " generated=" << generated_rows.size() << "\n";
    ok = false;
  }

  std::unordered_set<std::string> artifact_ids;
  for (const auto& row : backlog) {
    const auto found = row.find("surface_id");
    if (found == row.end() || found->second.empty()) {
      std::cerr << "artifact row missing surface_id\n";
      ok = false;
      continue;
    }
    artifact_ids.insert(found->second);
    if (!generated_ids.contains(std::string_view(found->second))) {
      std::cerr << "generated registry missing artifact surface " << found->second
                << "\n";
      ok = false;
    }
  }

  for (const auto& row : generated_rows) {
    if (!artifact_ids.contains(std::string(row.surface_id))) {
      std::cerr << "generated registry has row not present in artifact "
                << row.surface_id << "\n";
      ok = false;
    }
  }

  std::unordered_set<std::string> batch_ids;
  for (const auto& row : batches) {
    const auto found = row.find("surface_id");
    if (found != row.end()) batch_ids.insert(found->second);
  }
  std::unordered_set<std::string> oracle_ids;
  for (const auto& row : oracles) {
    const auto found = row.find("surface_id");
    if (found != row.end()) oracle_ids.insert(found->second);
  }

  for (const auto& row : generated_rows) {
    const std::string surface_id(row.surface_id);
    if (!batch_ids.contains(surface_id)) {
      std::cerr << row.surface_id << " missing batch membership artifact row\n";
      ok = false;
    }
    if (!oracle_ids.contains(surface_id)) {
      std::cerr << row.surface_id << " missing semantic oracle artifact row\n";
      ok = false;
    }
  }

  return ok;
}

bool ValidParserHandler(std::string_view value) {
  return value == "parser.cluster_profile_gate" || value == "parser.grammar_ast" ||
         HasPrefix(value, "parser.statement_family.") ||
         HasPrefix(value, "parser.expression_runtime.");
}

bool ValidUdrHandler(std::string_view value) {
  return value == "udr.sbsql_parser_support.cluster_profile_gate" ||
         value == "udr.sbsql_parser_support.native_future_decision" ||
         value == "udr.sbsql_parser_support.parse_describe_normalize";
}

bool ValidLoweringHandler(std::string_view value) {
  return value == "lowering.cluster_profile_gate" ||
         HasPrefix(value, "lowering.expression_runtime.") ||
         HasPrefix(value, "lowering.sblr_family.");
}

bool ValidServerAdmission(std::string_view value) {
  return value == "server.admission.cluster_profile_gate" ||
         HasPrefix(value, "server.admission.sblr_");
}

bool ValidEngineRule(std::string_view value) {
  return value == "engine.rule.cluster_private_fail_closed_or_profile" ||
         value == "engine.rule.native_future_promotion_or_refusal" ||
         value == "engine.rule.packet_refusal_or_implementation" ||
         HasPrefix(value, "engine.rule.sblr_");
}

bool ValidDiagnostic(std::string_view value) {
  return value == "diagnostic.cluster_profile_fail_closed" ||
         value == "diagnostic.native_future_decision" ||
         value == "diagnostic.canonical_message_vector";
}

bool ValidateCanary(std::string_view surface_id,
                    std::string_view canonical_name,
                    std::string_view source_status,
                    std::string_view cluster_scope) {
  const auto* row = sbsql::FindGeneratedSurfaceRegistryRowById(surface_id);
  if (row == nullptr) {
    std::cerr << "missing canary surface " << surface_id << "\n";
    return false;
  }
  if (row->canonical_name != canonical_name || row->source_status != source_status ||
      row->cluster_scope != cluster_scope) {
    std::cerr << "canary surface " << surface_id << " has unexpected descriptor\n";
    return false;
  }
  return true;
}

bool ValidateRow(const sbsql::GeneratedSurfaceRegistryRow& row) {
  bool ok = true;
  ok &= RequireField(row.surface_id, "surface_id", row.surface_id);
  ok &= RequireField(row.surface_id, "fixed_uuid_v7", row.fixed_uuid_v7);
  ok &= RequireField(row.surface_id, "canonical_name", row.canonical_name);
  ok &= RequireField(row.surface_id, "surface_kind", row.surface_kind);
  ok &= RequireField(row.surface_id, "family", row.family);
  ok &= RequireField(row.surface_id, "source_status", row.source_status);
  ok &= RequireField(row.surface_id, "cluster_scope", row.cluster_scope);
  ok &= RequireField(row.surface_id, "canonical_spec", row.canonical_spec);
  ok &= RequireField(row.surface_id, "sblr_operation_family", row.sblr_operation_family);
  ok &= RequireField(row.surface_id, "parser_packet", row.parser_packet);
  ok &= RequireField(row.surface_id, "engine_packet", row.engine_packet);
  ok &= RequireField(row.surface_id, "owner_lane", row.owner_lane);
  ok &= RequireField(row.surface_id, "batch_id", row.batch_id);
  ok &= RequireField(row.surface_id, "ctest_label", row.ctest_label);
  ok &= RequireField(row.surface_id, "parser_handler_key", row.parser_handler_key);
  ok &= RequireField(row.surface_id, "udr_handler_key", row.udr_handler_key);
  ok &= RequireField(row.surface_id, "lowering_handler_key", row.lowering_handler_key);
  ok &= RequireField(row.surface_id, "server_admission_key", row.server_admission_key);
  ok &= RequireField(row.surface_id, "engine_rule_key", row.engine_rule_key);
  ok &= RequireField(row.surface_id, "diagnostic_key", row.diagnostic_key);
  ok &= RequireField(row.surface_id, "oracle_key", row.oracle_key);
  ok &= RequireField(row.surface_id, "validation_fixture_id", row.validation_fixture_id);
  ok &= RequireField(row.surface_id, "final_acceptance_rule", row.final_acceptance_rule);
  ok &= RequireField(row.surface_id, "closure_action", row.closure_action);

  if (!ValidParserHandler(row.parser_handler_key)) {
    std::cerr << row.surface_id << " has unknown parser handler key "
              << row.parser_handler_key << "\n";
    ok = false;
  }
  if (!ValidUdrHandler(row.udr_handler_key)) {
    std::cerr << row.surface_id << " has unknown UDR handler key "
              << row.udr_handler_key << "\n";
    ok = false;
  }
  if (!ValidLoweringHandler(row.lowering_handler_key)) {
    std::cerr << row.surface_id << " has unknown lowering handler key "
              << row.lowering_handler_key << "\n";
    ok = false;
  }
  if (!ValidServerAdmission(row.server_admission_key)) {
    std::cerr << row.surface_id << " has unknown server admission key "
              << row.server_admission_key << "\n";
    ok = false;
  }
  if (!ValidEngineRule(row.engine_rule_key)) {
    std::cerr << row.surface_id << " has unknown engine rule key " << row.engine_rule_key
              << "\n";
    ok = false;
  }
  if (!ValidDiagnostic(row.diagnostic_key)) {
    std::cerr << row.surface_id << " has unknown diagnostic key " << row.diagnostic_key
              << "\n";
    ok = false;
  }

  if (row.source_status == "cluster_private" && row.cluster_scope != "cluster_private") {
    std::cerr << row.surface_id << " is cluster_private without cluster_private scope\n";
    ok = false;
  }
  if (row.cluster_scope == "cluster_private" &&
      (row.parser_handler_key != "parser.cluster_profile_gate" ||
       row.server_admission_key != "server.admission.cluster_profile_gate" ||
       row.engine_rule_key != "engine.rule.cluster_private_fail_closed_or_profile" ||
       row.diagnostic_key != "diagnostic.cluster_profile_fail_closed")) {
    std::cerr << row.surface_id << " does not fail closed through cluster profile keys\n";
    ok = false;
  }
  if (row.source_status == "native_future" && row.cluster_scope != "cluster_private" &&
      row.engine_rule_key != "engine.rule.native_future_promotion_or_refusal") {
    std::cerr << row.surface_id << " native_future row lacks explicit promotion/refusal rule\n";
    ok = false;
  }
  if (!HasPrefix(row.batch_id, "BATCH-")) {
    std::cerr << row.surface_id << " has invalid batch id " << row.batch_id << "\n";
    ok = false;
  }
  if (!HasPrefix(row.validation_fixture_id, "SBSQL-SURFACE-")) {
    std::cerr << row.surface_id << " has invalid fixture id "
              << row.validation_fixture_id << "\n";
    ok = false;
  }

  return ok;
}

} // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: sbp_sbsql_registry_generation_probe <artifact-root>\n";
    return 1;
  }

  const auto rows = sbsql::GeneratedSurfaceRegistryRows();
  if (rows.size() != sbsql::kGeneratedSurfaceRegistryRowCount) {
    std::cerr << "generated registry row count mismatch: " << rows.size() << "\n";
    return 1;
  }

  std::unordered_set<std::string_view> surface_ids;
  std::unordered_set<std::string_view> fixed_uuids;
  int native_now = 0;
  int native_future = 0;
  int cluster_private_status = 0;
  int cluster_private_scope = 0;
  bool ok = true;

  for (const auto& row : rows) {
    ok &= ValidateRow(row);
    if (!surface_ids.insert(row.surface_id).second) {
      std::cerr << "duplicate generated surface id " << row.surface_id << "\n";
      ok = false;
    }
    if (!fixed_uuids.insert(row.fixed_uuid_v7).second) {
      std::cerr << "duplicate generated fixed UUID " << row.fixed_uuid_v7 << "\n";
      ok = false;
    }

    if (row.source_status == "native_now") ++native_now;
    if (row.source_status == "native_future") ++native_future;
    if (row.source_status == "cluster_private") ++cluster_private_status;
    if (row.cluster_scope == "cluster_private") ++cluster_private_scope;
  }

  ok &= ValidateCanary("SBSQL-E4E0E6EB328C", "create_table_stmt", "native_now",
                       "noncluster_or_profile_scoped");
  ok &= ValidateCanary("SBSQL-971C709406A0", "@", "native_now",
                       "noncluster_or_profile_scoped");
  ok &= ValidateCanary("SBSQL-39C545BEBF5A", "cluster_publish_options",
                       "cluster_private", "cluster_private");
  ok &= ValidateCanary("SBSQL-DF502F8DF4FA", "Accept", "native_now",
                       "noncluster_or_profile_scoped");

  const auto backlog = ReadCsv(std::string(argv[1]) + "/SURFACE_IMPLEMENTATION_BACKLOG.csv");
  int expected_native_now = 0;
  int expected_native_future = 0;
  int expected_cluster_private_status = 0;
  int expected_cluster_private_scope = 0;
  for (const auto& row : backlog) {
    const auto status = row.find("source_status");
    const auto scope = row.find("cluster_scope");
    if (status != row.end()) {
      if (status->second == "native_now") ++expected_native_now;
      else if (status->second == "native_future") ++expected_native_future;
      else if (status->second == "cluster_private") ++expected_cluster_private_status;
    }
    if (scope != row.end() && scope->second == "cluster_private") {
      ++expected_cluster_private_scope;
    }
  }
  if (native_now != expected_native_now || native_future != expected_native_future ||
      cluster_private_status != expected_cluster_private_status ||
      cluster_private_scope != expected_cluster_private_scope) {
    std::cerr << "unexpected generated registry status/scope counts: native_now="
              << native_now << " native_future=" << native_future
              << " cluster_private_status=" << cluster_private_status
              << " cluster_private_scope=" << cluster_private_scope
              << " expected(native_now=" << expected_native_now
              << ", native_future=" << expected_native_future
              << ", cluster_private_status=" << expected_cluster_private_status
              << ", cluster_private_scope=" << expected_cluster_private_scope << ")\n";
    ok = false;
  }

  const auto* create_table =
      sbsql::FindGeneratedSurfaceRegistryRowByCanonicalName("create_table_stmt");
  if (create_table == nullptr || create_table->surface_id != "SBSQL-E4E0E6EB328C") {
    std::cerr << "canonical-name lookup failed for create_table_stmt\n";
    ok = false;
  }

  try {
    ok &= ValidateArtifactSurfaceCoverage(argv[1], rows);
  } catch (const std::exception& ex) {
    std::cerr << "artifact coverage validation failed: " << ex.what() << "\n";
    ok = false;
  }

  if (!ok) return 1;

  std::cout << "SBSQL generated registry lint passed for " << rows.size()
            << " rows; native_now=" << native_now
            << " native_future=" << native_future
            << " cluster_private_status=" << cluster_private_status
            << " cluster_private_scope=" << cluster_private_scope << "\n";
  return 0;
}
