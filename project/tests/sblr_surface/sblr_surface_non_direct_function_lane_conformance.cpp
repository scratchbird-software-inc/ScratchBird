// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "metadata/donor_function_surface_policy.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace functions = scratchbird::engine::functions;

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

std::vector<std::string> ParseCsvRecords(const std::string& content) {
  std::vector<std::string> records;
  std::string record;
  bool in_quotes = false;
  for (std::size_t i = 0; i < content.size(); ++i) {
    const char ch = content[i];
    if (ch == '"') {
      record.push_back(ch);
      if (in_quotes && i + 1 < content.size() && content[i + 1] == '"') {
        record.push_back(content[++i]);
      } else {
        in_quotes = !in_quotes;
      }
      continue;
    }
    if ((ch == '\n' || ch == '\r') && !in_quotes) {
      if (!record.empty()) records.push_back(record);
      record.clear();
      if (ch == '\r' && i + 1 < content.size() && content[i + 1] == '\n') ++i;
      continue;
    }
    record.push_back(ch);
  }
  Require(!in_quotes, "unterminated CSV quote");
  if (!record.empty()) records.push_back(record);
  return records;
}

std::vector<std::map<std::string, std::string>> LoadCsv(const std::string& path) {
  std::ifstream input(path);
  Require(input.good(), "could not open function surface matrix");
  const std::string content((std::istreambuf_iterator<char>(input)),
                            std::istreambuf_iterator<char>());
  const auto records = ParseCsvRecords(content);
  Require(!records.empty(), "empty CSV file");
  const auto header = ParseCsvLine(records.front());
  std::vector<std::map<std::string, std::string>> rows;
  for (std::size_t record_index = 1; record_index < records.size(); ++record_index) {
    const auto values = ParseCsvLine(records[record_index]);
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

bool HasEvidence(const functions::DonorFunctionSurfaceResult& result,
                 std::string_view key,
                 std::string_view value) {
  return std::any_of(result.evidence.begin(),
                     result.evidence.end(),
                     [&](const auto& item) {
                       return item.first == key && item.second == value;
                     });
}

void VerifyFunctionRow(const std::map<std::string, std::string>& row,
                       bool explicit_unsupported_matrix) {
  const auto& row_id = Field(row, "inventory_id");
  const auto& decision_name = Field(row, "implementation_decision");
  const auto policy = functions::ResolveDonorFunctionSurfacePolicy(decision_name);
  Require(policy.has_value(), row_id + " did not resolve function surface policy");
  Require(policy->implementation_decision == decision_name,
          row_id + " implementation decision mismatch");
  Require(policy->required_execution_plan_lane == Field(row, "required_execution_plan_lane"),
          row_id + " execution_plan lane mismatch");
  Require(!policy->parser_shortcut_allowed,
          row_id + " allowed parser shortcut authority");
  Require(!policy->donor_execution_authority_accepted,
          row_id + " accepted donor execution authority");
  Require(!policy->sblr_execution_authority,
          row_id + " granted SBLR execution authority");

  const bool connector_authorized = decision_name == "connector_operation";
  const bool trusted_udr = decision_name == "trusted_udr_registration";
  const auto result = functions::EvaluateDonorFunctionSurface(
      functions::DonorFunctionSurfaceRequest{
          Field(row, "engine_id"),
          row_id,
          Field(row, "item_name"),
          decision_name,
          Field(row, "capability_family"),
          Field(row, "sb_normalized_target"),
          connector_authorized,
          trusted_udr,
      });
  Require(result.recognized, row_id + " route result was not recognized");
  Require(result.route_contract_id == policy->route_contract_id,
          row_id + " route contract mismatch");
  Require(result.diagnostic_code == policy->diagnostic_code,
          row_id + " diagnostic mismatch");
  Require(result.result_shape == policy->result_shape,
          row_id + " result shape mismatch");
  Require(!result.parser_shortcut_used,
          row_id + " used parser shortcut authority");
  Require(!result.donor_execution_authority_accepted,
          row_id + " accepted donor authority");
  Require(!result.sblr_execution_authority,
          row_id + " granted SBLR authority");
  Require(HasEvidence(result, "parser_shortcut_used", "false"),
          row_id + " missing parser shortcut refusal evidence");
  Require(HasEvidence(result, "donor_execution_authority_accepted", "false"),
          row_id + " missing donor authority refusal evidence");
  Require(HasEvidence(result, "sblr_execution_authority", "false"),
          row_id + " missing SBLR authority refusal evidence");

  if (decision_name == "catalog_projection_only") {
    Require(result.accepted && result.catalog_projection,
            row_id + " did not route catalog projection");
  } else if (decision_name == "connector_operation") {
    Require(result.accepted && result.connector_route,
            row_id + " did not route authorized connector operation");
    const auto denied = functions::EvaluateDonorFunctionSurface(
        functions::DonorFunctionSurfaceRequest{
            Field(row, "engine_id"),
            row_id,
            Field(row, "item_name"),
            decision_name,
            Field(row, "capability_family"),
            Field(row, "sb_normalized_target"),
            false,
            false,
        });
    Require(denied.denied && !denied.accepted,
            row_id + " connector route did not fail closed without authorization");
  } else if (decision_name == "trusted_udr_registration") {
    Require(result.accepted && result.trusted_udr_registration_route,
            row_id + " did not route trusted UDR registration");
    const auto denied = functions::EvaluateDonorFunctionSurface(
        functions::DonorFunctionSurfaceRequest{
            Field(row, "engine_id"),
            row_id,
            Field(row, "item_name"),
            decision_name,
            Field(row, "capability_family"),
            Field(row, "sb_normalized_target"),
            false,
            false,
        });
    Require(denied.denied && !denied.accepted,
            row_id + " trusted UDR route did not fail closed without policy");
  } else if (decision_name == "policy_blocked") {
    Require(result.denied && !result.accepted,
            row_id + " policy-blocked route did not deny");
  } else if (decision_name == "unsupported") {
    Require(result.denied && !result.accepted && result.unsupported_refusal,
            row_id + " unsupported route did not return exact refusal");
    Require(result.diagnostic_code == "SB.DONOR_FUNCTION.UNSUPPORTED",
            row_id + " unsupported diagnostic mismatch");
    if (explicit_unsupported_matrix) {
      Require(Field(row, "required_execution_plan_lane") == "exact_unsupported_refusal",
              row_id + " explicit unsupported row has wrong lane");
    }
  } else {
    Fail(row_id + " unknown implementation decision " + decision_name);
  }
}

}  // namespace

int main(int argc, char** argv) {
  Require(argc == 3,
          "usage: sblr_surface_non_direct_function_lane_conformance "
          "<non_direct.csv> <unsupported.csv>");
  const auto non_direct = LoadCsv(argv[1]);
  const auto unsupported = LoadCsv(argv[2]);
  Require(non_direct.size() == 258, "non-direct function row count mismatch");
  Require(unsupported.size() == 5, "explicit unsupported row count mismatch");

  std::map<std::string, int> decision_counts;
  for (const auto& row : non_direct) {
    ++decision_counts[Field(row, "implementation_decision")];
    VerifyFunctionRow(row, false);
  }
  Require(decision_counts["catalog_projection_only"] == 112,
          "catalog projection count mismatch");
  Require(decision_counts["connector_operation"] == 80,
          "connector operation count mismatch");
  Require(decision_counts["policy_blocked"] == 46,
          "policy-blocked count mismatch");
  Require(decision_counts["trusted_udr_registration"] == 15,
          "trusted UDR registration count mismatch");
  Require(decision_counts["unsupported"] == 5,
          "non-direct unsupported count mismatch");

  for (const auto& row : unsupported) {
    VerifyFunctionRow(row, true);
  }

  const auto unknown = functions::EvaluateDonorFunctionSurface(
      functions::DonorFunctionSurfaceRequest{
          "unknown",
          "unknown",
          "unknown",
          "file_presence_only",
          "unknown",
          "unknown",
          true,
          true,
      });
  Require(!unknown.recognized && !unknown.accepted && unknown.denied,
          "unknown function surface did not fail closed");
  Require(unknown.diagnostic_code == "SB.DONOR_FUNCTION.UNKNOWN_SURFACE",
          "unknown function surface diagnostic mismatch");
  Require(!unknown.parser_shortcut_used &&
              !unknown.donor_execution_authority_accepted &&
              !unknown.sblr_execution_authority,
          "unknown function surface gained authority");

  std::cout << "sblr_surface_non_direct_function_lane_conformance=passed\n";
  return EXIT_SUCCESS;
}
