// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "compatibility_server_authority.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace server = scratchbird::server;

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) Fail(message);
}

std::vector<std::string> ParseCsvLine(const std::string& line) {
  std::vector<std::string> fields;
  std::string field;
  bool in_quotes = false;
  for (std::size_t i = 0; i < line.size(); ++i) {
    const char ch = line[i];
    if (ch == '"') {
      if (in_quotes && i + 1 < line.size() && line[i + 1] == '"') {
        field.push_back('"');
        ++i;
      } else {
        in_quotes = !in_quotes;
      }
      continue;
    }
    if (ch == ',' && !in_quotes) {
      fields.push_back(field);
      field.clear();
      continue;
    }
    field.push_back(ch);
  }
  Require(!in_quotes, "unterminated CSV quote");
  fields.push_back(field);
  return fields;
}

std::vector<std::map<std::string, std::string>> LoadCsv(const std::string& path) {
  std::ifstream input(path);
  Require(input.good(), "could not open SERVER_AUTHORITY_SURFACE_MATRIX.csv");
  std::string line;
  Require(static_cast<bool>(std::getline(input, line)), "empty CSV file");
  if (!line.empty() && line.back() == '\r') line.pop_back();
  const auto header = ParseCsvLine(line);
  std::vector<std::map<std::string, std::string>> rows;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    const auto values = ParseCsvLine(line);
    Require(values.size() == header.size(), "CSV row width mismatch");
    std::map<std::string, std::string> row;
    for (std::size_t i = 0; i < header.size(); ++i) {
      row.emplace(header[i], values[i]);
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

const std::string& Field(const std::map<std::string, std::string>& row,
                         const std::string& name) {
  const auto it = row.find(name);
  if (it == row.end()) Fail("missing CSV field: " + name);
  return it->second;
}

bool HasEvidence(const server::CompatibilityServerAuthorityRouteResult& result,
                 std::string_view key,
                 std::string_view value) {
  return std::any_of(result.evidence.begin(),
                     result.evidence.end(),
                     [&](const auto& item) {
                       return item.first == key && item.second == value;
                     });
}

void VerifyKnownRow(const std::map<std::string, std::string>& row) {
  const auto& decision_id = Field(row, "decision_id");
  const auto& engine_id = Field(row, "engine_id");
  const auto& surface_key = Field(row, "surface_key");
  const auto& expected_action = Field(row, "server_action");
  const auto& expected_vector = Field(row, "security_vector");

  Require(server::IsKnownCompatibilityEngineForServerAuthority(engine_id),
          decision_id + " engine is not recognized by server authority");
  const auto decision =
      server::ResolveCompatibilityServerAuthoritySurface(engine_id, surface_key);
  Require(decision.has_value(), decision_id + " did not resolve");
  Require(decision->server_action == expected_action,
          decision_id + " server action mismatch");
  Require(decision->diagnostic_code == expected_vector,
          decision_id + " diagnostic vector mismatch");
  Require(decision->sblr_action == Field(row, "sblr_action"),
          decision_id + " SBLR action mismatch");
  Require(decision->mga_rule == Field(row, "mga_rule"),
          decision_id + " MGA rule mismatch");
  Require(decision->required_execution_plan_lane == Field(row, "required_execution_plan_lane"),
          decision_id + " execution_plan lane mismatch");
  Require(decision->parser_classify_only,
          decision_id + " parser route was not classify-only");
  Require(!decision->sblr_execution_surface,
          decision_id + " was marked as an SBLR execution surface");
  Require(decision->preserves_scratchbird_mga_authority,
          decision_id + " did not preserve ScratchBird MGA authority");
  Require(!decision->accepts_external_finality,
          decision_id + " accepted external finality");

  const auto route = server::EvaluateCompatibilityServerAuthorityRoute(
      server::CompatibilityServerAuthorityRequest{
          engine_id,
          surface_key,
          Field(row, "reference_visible_surface"),
          true,
          true,
      });
  Require(route.recognized, decision_id + " route was not recognized");
  Require(route.routed, decision_id + " route did not execute route contract");
  Require(route.diagnostic_code == expected_vector,
          decision_id + " route diagnostic vector mismatch");
  Require(route.route_contract_id == decision->route_contract_id,
          decision_id + " route contract id mismatch");
  Require(!route.sblr_execution_attempted,
          decision_id + " attempted SBLR execution");
  Require(route.sblr_execution_blocked,
          decision_id + " did not block SBLR execution authority");
  Require(route.scratchbird_mga_authority_preserved,
          decision_id + " route did not preserve ScratchBird MGA authority");
  Require(!route.external_finality_accepted,
          decision_id + " route accepted external finality");
  Require(HasEvidence(route, "sblr_execution_attempted", "false"),
          decision_id + " missing no-SBLR evidence");
  Require(HasEvidence(route, "scratchbird_mga_authority_preserved", "true"),
          decision_id + " missing MGA preservation evidence");
  Require(HasEvidence(route, "external_finality_accepted", "false"),
          decision_id + " missing external-finality refusal evidence");

  if (expected_action == "security_denial" ||
      expected_action == "server_policy_gate") {
    Require(route.denied && !route.accepted,
            decision_id + " did not fail closed for security/policy authority");
  } else if (expected_action == "migration_service_route") {
    Require(route.migration_route && route.accepted,
            decision_id + " did not route to the migration service contract");
  } else {
    Fail(decision_id + " has unknown server_action " + expected_action);
  }
}

void VerifyUnknownRowsFailClosed() {
  auto unknown_engine = server::EvaluateCompatibilityServerAuthorityRoute(
      server::CompatibilityServerAuthorityRequest{
          "unknown_engine",
          "database_create",
          "CREATE DATABASE external",
          true,
          true,
      });
  Require(!unknown_engine.recognized && !unknown_engine.accepted && unknown_engine.denied,
          "unknown engine did not fail closed");
  Require(unknown_engine.diagnostic_code == "SB.SERVER_AUTHORITY.UNKNOWN_SURFACE",
          "unknown engine diagnostic mismatch");
  Require(!unknown_engine.sblr_execution_attempted,
          "unknown engine attempted SBLR execution");

  auto wrong_pair = server::EvaluateCompatibilityServerAuthorityRoute(
      server::CompatibilityServerAuthorityRequest{
          "mysql",
          "pg_copy_program",
          "COPY PROGRAM",
          true,
          true,
      });
  Require(!wrong_pair.recognized && !wrong_pair.accepted && wrong_pair.denied,
          "engine-specific surface accepted the wrong engine");
  Require(wrong_pair.diagnostic_code == "SB.SERVER_AUTHORITY.UNKNOWN_SURFACE",
          "wrong engine/surface diagnostic mismatch");
  Require(!wrong_pair.sblr_execution_attempted,
          "wrong engine/surface attempted SBLR execution");
}

}  // namespace

int main(int argc, char** argv) {
  Require(argc == 2, "usage: sblr_surface_server_authority_route_conformance <matrix.csv>");
  const auto rows = LoadCsv(argv[1]);
  Require(rows.size() == 318, "server authority matrix row count mismatch");

  std::map<std::string, int> action_counts;
  std::set<std::string> decision_ids;
  for (const auto& row : rows) {
    Require(decision_ids.insert(Field(row, "decision_id")).second,
            Field(row, "decision_id") + " duplicate decision id");
    Require(Field(row, "parser_action") == "classify_only",
            Field(row, "decision_id") + " parser action was not classify_only");
    Require(Field(row, "status") == "complete",
            Field(row, "decision_id") + " row is not complete");
    Require(Field(row, "is_sblr_execution_surface") == "no",
            Field(row, "decision_id") + " row claims SBLR execution authority");
    ++action_counts[Field(row, "server_action")];
    VerifyKnownRow(row);
  }

  Require(action_counts["security_denial"] == 181,
          "security denial route count mismatch");
  Require(action_counts["server_policy_gate"] == 82,
          "server policy gate route count mismatch");
  Require(action_counts["migration_service_route"] == 55,
          "migration service route count mismatch");
  VerifyUnknownRowsFailClosed();
  std::cout << "sblr_surface_server_authority_route_conformance=passed\n";
  return EXIT_SUCCESS;
}
