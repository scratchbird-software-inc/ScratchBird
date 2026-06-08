// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "donor_server_authority.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
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

bool HasEvidence(const server::DonorMigrationRouteResult& result,
                 std::string_view key,
                 std::string_view value) {
  return std::any_of(result.evidence.begin(),
                     result.evidence.end(),
                     [&](const auto& item) {
                       return item.first == key && item.second == value;
                     });
}

void VerifyMigrationRow(const std::map<std::string, std::string>& row) {
  const auto& decision_id = Field(row, "decision_id");
  const auto& engine_id = Field(row, "engine_id");
  const auto& surface_key = Field(row, "surface_key");
  const auto& vector = Field(row, "security_vector");

  const auto contract =
      server::ResolveDonorMigrationRouteContract(engine_id, surface_key);
  Require(contract.has_value(), decision_id + " missing migration route contract");
  Require(contract->route_diagnostic_code == vector,
          decision_id + " route diagnostic mismatch");
  Require(contract->mga_rule == Field(row, "mga_rule"),
          decision_id + " MGA rule mismatch");
  Require(contract->scratchbird_mga_authority_preserved,
          decision_id + " did not preserve ScratchBird MGA authority");
  Require(!contract->donor_storage_authority_accepted,
          decision_id + " accepted donor storage authority");
  Require(!contract->donor_finality_accepted,
          decision_id + " accepted donor finality");
  Require(!contract->sblr_execution_surface,
          decision_id + " became an SBLR execution surface");

  if (vector == "SB.MIGRATION.PHYSICAL_OPERATION_ROUTED") {
    Require(contract->route_kind == server::DonorMigrationRouteKind::kPhysicalOperation,
            decision_id + " did not resolve to physical migration");
    Require(contract->checkpoint_descriptor_kind ==
                std::string_view("scratchbird.migration.physical.checkpoint.v1"),
            decision_id + " physical checkpoint descriptor mismatch");
    Require(contract->resume_token_kind ==
                std::string_view("scratchbird.migration.physical.resume_token.v1"),
            decision_id + " physical resume token mismatch");
    Require(contract->unavailable_diagnostic_code ==
                std::string_view("SB.MIGRATION.PHYSICAL_SERVICE_UNAVAILABLE"),
            decision_id + " physical unavailable diagnostic mismatch");
  } else if (vector == "SB.MIGRATION.REPLICATION_ENDPOINT_ROUTED") {
    Require(contract->route_kind ==
                server::DonorMigrationRouteKind::kLiveReplicationEndpoint,
            decision_id + " did not resolve to live replication migration");
    Require(contract->checkpoint_descriptor_kind ==
                std::string_view("scratchbird.migration.live_replication.checkpoint.v1"),
            decision_id + " replication checkpoint descriptor mismatch");
    Require(contract->resume_token_kind ==
                std::string_view("scratchbird.migration.live_replication.resume_token.v1"),
            decision_id + " replication resume token mismatch");
    Require(contract->unavailable_diagnostic_code ==
                std::string_view("SB.MIGRATION.REPLICATION_SERVICE_UNAVAILABLE"),
            decision_id + " replication unavailable diagnostic mismatch");
  } else {
    Fail(decision_id + " has unexpected migration vector " + vector);
  }

  const auto routed = server::EvaluateDonorMigrationRoute(
      server::DonorMigrationRouteRequest{engine_id, surface_key, true});
  Require(routed.recognized && routed.routed && routed.accepted,
          decision_id + " did not route when migration service was available");
  Require(!routed.service_unavailable,
          decision_id + " reported unavailable service while available");
  Require(routed.diagnostic_code == vector,
          decision_id + " routed diagnostic mismatch");
  Require(routed.route_contract_id == contract->route_contract_id,
          decision_id + " route contract id mismatch");
  Require(routed.checkpoint_descriptor_kind == contract->checkpoint_descriptor_kind,
          decision_id + " routed checkpoint descriptor mismatch");
  Require(routed.resume_token_kind == contract->resume_token_kind,
          decision_id + " routed resume token mismatch");
  Require(!routed.sblr_execution_attempted,
          decision_id + " migration route attempted SBLR execution");
  Require(!routed.donor_storage_authority_accepted,
          decision_id + " migration route accepted donor storage authority");
  Require(!routed.donor_finality_accepted,
          decision_id + " migration route accepted donor finality");
  Require(routed.scratchbird_mga_authority_preserved,
          decision_id + " migration route did not preserve MGA authority");
  Require(HasEvidence(routed, "sblr_execution_attempted", "false"),
          decision_id + " missing no-SBLR route evidence");
  Require(HasEvidence(routed, "donor_storage_authority_accepted", "false"),
          decision_id + " missing donor-storage refusal evidence");
  Require(HasEvidence(routed, "donor_finality_accepted", "false"),
          decision_id + " missing donor-finality refusal evidence");

  const auto unavailable = server::EvaluateDonorMigrationRoute(
      server::DonorMigrationRouteRequest{engine_id, surface_key, false});
  Require(unavailable.recognized && !unavailable.routed && !unavailable.accepted,
          decision_id + " did not fail closed when migration service was unavailable");
  Require(unavailable.service_unavailable,
          decision_id + " unavailable route did not report service_unavailable");
  Require(unavailable.diagnostic_code == contract->unavailable_diagnostic_code,
          decision_id + " unavailable diagnostic mismatch");
  Require(!unavailable.sblr_execution_attempted,
          decision_id + " unavailable route attempted SBLR execution");
  Require(!unavailable.donor_storage_authority_accepted,
          decision_id + " unavailable route accepted donor storage authority");
  Require(!unavailable.donor_finality_accepted,
          decision_id + " unavailable route accepted donor finality");
}

}  // namespace

int main(int argc, char** argv) {
  Require(argc == 2, "usage: sblr_surface_migration_route_contract_conformance <matrix.csv>");
  const auto rows = LoadCsv(argv[1]);
  int migration_rows = 0;
  int physical_rows = 0;
  int replication_rows = 0;
  for (const auto& row : rows) {
    if (Field(row, "server_action") != "migration_service_route") continue;
    ++migration_rows;
    if (Field(row, "security_vector") == "SB.MIGRATION.PHYSICAL_OPERATION_ROUTED") {
      ++physical_rows;
    } else if (Field(row, "security_vector") ==
               "SB.MIGRATION.REPLICATION_ENDPOINT_ROUTED") {
      ++replication_rows;
    }
    VerifyMigrationRow(row);
  }
  Require(migration_rows == 55, "migration route row count mismatch");
  Require(physical_rows == 26, "physical migration route count mismatch");
  Require(replication_rows == 29, "replication migration route count mismatch");

  const auto non_migration = server::ResolveDonorMigrationRouteContract(
      "postgresql", "pg_copy_program");
  Require(!non_migration.has_value(),
          "non-migration authority row resolved as a migration route");
  const auto unknown = server::EvaluateDonorMigrationRoute(
      server::DonorMigrationRouteRequest{"postgresql", "pg_copy_program", true});
  Require(!unknown.recognized && !unknown.accepted && unknown.service_unavailable,
          "non-migration route did not fail closed");
  Require(unknown.diagnostic_code == "SB.MIGRATION.UNKNOWN_ROUTE",
          "non-migration unknown diagnostic mismatch");
  Require(!unknown.sblr_execution_attempted,
          "non-migration unknown route attempted SBLR execution");

  std::cout << "sblr_surface_migration_route_contract_conformance=passed\n";
  return EXIT_SUCCESS;
}
