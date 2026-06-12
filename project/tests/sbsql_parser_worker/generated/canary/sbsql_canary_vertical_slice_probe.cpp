// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ast/ast.hpp"
#include "binder/binder.hpp"
#include "cst/cst.hpp"
#include "lowering/lowering.hpp"
#include "registry/generated/sbsql_generated_registry.hpp"

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace sbsql = scratchbird::parser::sbsql;

namespace {

using CsvRow = std::unordered_map<std::string, std::string>;

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

std::vector<CsvRow> ReadCsv(const std::string& path) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("failed to open " + path);

  std::string line;
  if (!std::getline(input, line)) throw std::runtime_error("empty CSV " + path);
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

const CsvRow* FindRow(const std::vector<CsvRow>& rows,
                      std::string_view column,
                      std::string_view value) {
  for (const auto& row : rows) {
    const auto found = row.find(std::string(column));
    if (found != row.end() && found->second == value) return &row;
  }
  return nullptr;
}

std::string Field(const CsvRow& row, std::string_view column) {
  const auto found = row.find(std::string(column));
  if (found == row.end()) return {};
  return found->second;
}

bool Require(bool condition, std::string_view message) {
  if (condition) return true;
  std::cerr << message << "\n";
  return false;
}

bool HasRequiredAssignments(const sbsql::GeneratedSurfaceRegistryRow& row) {
  bool ok = true;
  ok &= Require(!row.parser_handler_key.empty(), "missing parser handler");
  ok &= Require(!row.udr_handler_key.empty(), "missing UDR handler");
  ok &= Require(!row.lowering_handler_key.empty(), "missing lowering handler");
  ok &= Require(!row.server_admission_key.empty(), "missing server admission");
  ok &= Require(!row.engine_rule_key.empty(), "missing engine rule");
  ok &= Require(!row.diagnostic_key.empty(), "missing diagnostic key");
  ok &= Require(!row.validation_fixture_id.empty(), "missing fixture id");
  ok &= Require(!row.oracle_key.empty(), "missing oracle key");
  return ok;
}

bool ValidateSurface(const std::vector<CsvRow>& batches,
                     const std::vector<CsvRow>& oracles,
                     std::string_view surface_id,
                     std::string_view canonical_name) {
  const auto* row = sbsql::FindGeneratedSurfaceRegistryRowById(surface_id);
  bool ok = true;
  ok &= Require(row != nullptr, "missing canary generated registry row");
  if (row == nullptr) return false;
  ok &= Require(row->canonical_name == canonical_name, "unexpected canary canonical name");
  ok &= HasRequiredAssignments(*row);
  ok &= Require(FindRow(batches, "surface_id", surface_id) != nullptr,
                "missing canary batch membership");
  ok &= Require(FindRow(oracles, "surface_id", surface_id) != nullptr,
                "missing canary oracle assignment");
  return ok;
}

bool ValidateClusterProfileGateRow(std::string_view surface_id,
                                   std::string_view canonical_name,
                                   std::string_view sblr_operation_family) {
  const auto* row = sbsql::FindGeneratedSurfaceRegistryRowById(surface_id);
  if (row == nullptr) return Require(false, "missing cluster canary row");
  bool ok = true;
  ok &= Require(row->canonical_name == canonical_name,
                "cluster canary row canonical name mismatch");
  ok &= Require(row->source_status == "cluster_private",
                "cluster canary row lacks cluster_private source status");
  ok &= Require(row->cluster_scope == "cluster_private",
                "cluster canary row lacks cluster_private scope");
  ok &= Require(row->sblr_operation_family == sblr_operation_family,
                "cluster canary row SBLR operation family mismatch");
  ok &= Require(row->parser_handler_key == "parser.cluster_profile_gate",
                "cluster canary lacks parser profile gate");
  ok &= Require(row->server_admission_key == "server.admission.cluster_profile_gate",
                "cluster canary lacks server profile gate");
  ok &= Require(row->engine_rule_key == "engine.rule.cluster_private_fail_closed_or_profile",
                "cluster canary lacks fail-closed engine rule");
  ok &= Require(row->diagnostic_key == "diagnostic.cluster_profile_fail_closed",
                "cluster canary lacks fail-closed diagnostic key");
  return ok;
}

bool ValidateClusterFailClosed(const std::vector<CsvRow>& messages) {
  bool ok = true;
  ok &= ValidateClusterProfileGateRow("SBSQL-6689D8CFD6EA",
                                      "create_cluster_stmt",
                                      "sblr.catalog.mutation.v3");
  ok &= ValidateClusterProfileGateRow("SBSQL-5FEA0732FD1C",
                                      "alter_cluster_stmt",
                                      "sblr.catalog.mutation.v3");
  ok &= ValidateClusterProfileGateRow("SBSQL-4EF886377AB2",
                                      "drop_cluster_stmt",
                                      "sblr.catalog.mutation.v3");
  ok &= ValidateClusterProfileGateRow("SBSQL-39C545BEBF5A",
                                      "cluster_publish_options",
                                      "sblr.archive_replication.operation.v3");
  ok &= ValidateClusterProfileGateRow("SBSQL-3AA85DA2ED21",
                                      "cluster_commit_options",
                                      "sblr.transaction.control.v3");
  ok &= ValidateClusterProfileGateRow("SBSQL-3D3DCFA99D24",
                                      "cluster_rollback_options",
                                      "sblr.transaction.control.v3");

  const auto* message = FindRow(messages, "diagnostic_code", "SERVER.ADMISSION.REFUSED");
  ok &= Require(message != nullptr, "missing server admission refusal message vector");
  if (message != nullptr) {
    ok &= Require(!Field(*message, "parser_rendering_template").empty(),
                  "missing cluster refusal rendering template");
    ok &= Require(!Field(*message, "redaction_policy").empty(),
                  "missing cluster refusal redaction policy");
    ok &= Require(!Field(*message, "conformance_fixture").empty(),
                  "missing cluster refusal conformance fixture");
  }
  return ok;
}

bool ValidateNativeNowClosureDecision() {
  const auto* row = sbsql::FindGeneratedSurfaceRegistryRowById("SBSQL-DF502F8DF4FA");
  if (row == nullptr) return Require(false, "missing native surface canary row");

  bool ok = true;
  ok &= Require(row->source_status == "native_now",
                "native surface canary status mismatch");
  ok &= Require(row->parser_handler_key == "parser.expression_runtime.function",
                "native surface canary parser route mismatch");
  ok &= Require(row->lowering_handler_key == "lowering.expression_runtime.function",
                "native surface canary lowering route mismatch");
  ok &= Require(row->engine_rule_key == "engine.rule.sblr_expression_runtime_v3",
                "native surface canary engine rule mismatch");
  ok &= Require(row->final_acceptance_rule ==
                    "parse_bind_lower_server_engine_diagnostic_and_regression_evidence",
                "native surface canary final acceptance rule mismatch");
  ok &= Require(row->closure_action ==
                    "promote_to_implemented_behavior_or_reclassify_with_canonical_refusal",
                "native surface canary closure action mismatch");
  return ok;
}

bool ValidateReferenceAlias(const std::vector<CsvRow>& reference_aliases) {
  const auto* alias = FindRow(reference_aliases, "reference_surface", "apache_ignite:query_select");
  bool ok = true;
  ok &= Require(alias != nullptr, "missing reference alias canary row");
  if (alias != nullptr) {
    ok &= Require(Field(*alias, "native_sbsql_surface") == "select",
                  "reference alias does not map to native select");
    ok &= Require(Field(*alias, "mapping_status") ==
                      "mapped_by_profile_or_refused_with_exact_diagnostic",
                  "reference alias lacks exact mapping/refusal policy");
    ok &= Require(Field(*alias, "diagnostic_target") ==
                      "reference_profile_message_vector_rendering",
                  "reference alias lacks reference rendering diagnostic target");
  }
  return ok;
}

bool ValidateMinimalParserRoute() {
  using namespace scratchbird::parser::sbsql;
  ParserConfig config;
  config.probe_mode = true;
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "00000000-0000-7000-8000-000000000001";
  session.catalog_epoch = 1;
  session.security_policy_epoch = 1;
  session.descriptor_epoch = 1;

  auto cst = BuildCst("select 1");
  auto ast = BuildAst(cst);
  auto bound = BindAst(ast, cst, config, session);
  auto lowered = LowerToSblr(bound, cst, session);
  return Require(!lowered.payload.empty(), "minimal parser route produced empty SBLR") &&
         Require(!lowered.messages.has_errors(), "minimal parser route produced errors");
}

} // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: sbp_sbsql_canary_vertical_slice_probe <artifact-root>\n";
    return 1;
  }

  try {
    const std::string artifact_root = argv[1];
    const auto batches = ReadCsv(artifact_root + "/BATCH_ROW_MEMBERSHIP.csv");
    const auto oracles = ReadCsv(artifact_root + "/SEMANTIC_ORACLE_AUTHORITY_MAP.csv");
    const auto messages = ReadCsv(artifact_root + "/MESSAGE_VECTOR_COVERAGE_BACKLOG.csv");
    const auto reference_aliases = ReadCsv(artifact_root + "/REFERENCE_ALIAS_COVERAGE_BACKLOG.csv");

    bool ok = true;
    ok &= ValidateSurface(batches, oracles, "SBSQL-E4E0E6EB328C",
                          "create_table_stmt");
    ok &= ValidateSurface(batches, oracles, "SBSQL-971C709406A0", "@");
    ok &= ValidateSurface(batches, oracles, "SBSQL-39C545BEBF5A",
                          "cluster_publish_options");
    ok &= ValidateSurface(batches, oracles, "SBSQL-DF502F8DF4FA", "Accept");
    ok &= ValidateClusterFailClosed(messages);
    ok &= ValidateNativeNowClosureDecision();
    ok &= ValidateReferenceAlias(reference_aliases);
    ok &= ValidateMinimalParserRoute();

    if (!ok) return 1;

    std::cout << "SBSQL canary vertical slice metadata and parser route passed\n";
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "canary validation failed: " << ex.what() << "\n";
    return 1;
  }
}
