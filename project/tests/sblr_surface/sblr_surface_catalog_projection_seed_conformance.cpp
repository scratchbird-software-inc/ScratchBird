// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "metadata/donor_catalog_projection_policy.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
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
  Require(input.good(), "could not open non-direct function matrix");
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

bool HasEvidence(const functions::DonorCatalogProjectionResult& result,
                 std::string_view key,
                 std::string_view value) {
  return std::any_of(result.evidence.begin(),
                     result.evidence.end(),
                     [&](const auto& item) {
                       return item.first == key && item.second == value;
                     });
}

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.rfind(prefix, 0) == 0;
}

std::string ExpectedManifestResource(std::string_view engine_id) {
  std::string resource = "donor-emulation/";
  resource.append(engine_id);
  resource.append("/catalog_seed_manifest_full.json");
  return resource;
}

void VerifyResultAuthority(const std::string& row_id,
                           const functions::DonorCatalogProjectionResult& result) {
  Require(!result.parser_execution_authority,
          row_id + " granted parser execution authority");
  Require(!result.donor_execution_authority,
          row_id + " accepted donor execution authority");
  Require(!result.sblr_execution_authority,
          row_id + " granted SBLR execution authority");
  Require(HasEvidence(result, "parser_execution_authority", "false"),
          row_id + " missing parser-authority evidence");
  Require(HasEvidence(result, "donor_execution_authority", "false"),
          row_id + " missing donor-authority evidence");
  Require(HasEvidence(result, "sblr_execution_authority", "false"),
          row_id + " missing SBLR-authority evidence");
}

void VerifyCatalogRow(const std::map<std::string, std::string>& row) {
  const auto& row_id = Field(row, "inventory_id");
  const auto manifest =
      functions::ResolveDonorCatalogSeedManifest(Field(row, "engine_id"));
  Require(manifest.has_value(), row_id + " did not resolve donor seed manifest");
  Require(manifest->project_owned_packet,
          row_id + " seed manifest was not project-owned");
  Require(manifest->clean_database_seedable,
          row_id + " seed manifest was not clean-database seedable");
  Require(!manifest->mutable_by_parser && !manifest->mutable_by_donor,
          row_id + " seed manifest was mutable outside SB catalog authority");
  Require(manifest->manifest_resource == ExpectedManifestResource(Field(row, "engine_id")),
          row_id + " seed manifest resource mismatch");

  const auto contract =
      functions::ResolveDonorCatalogProjectionContract(Field(row, "sb_normalized_target"));
  Require(contract.has_value(), row_id + " did not resolve catalog projection contract");
  Require(contract->sb_normalized_target == Field(row, "sb_normalized_target"),
          row_id + " normalized target mismatch");
  Require(contract->engine_owned,
          row_id + " catalog projection was not engine-owned");
  Require(contract->generated_from_system_catalog,
          row_id + " catalog projection was not generated from system catalogs");
  Require(contract->requires_materialized_authorization,
          row_id + " catalog projection lacked materialized authorization");
  Require(!contract->parser_execution_authority &&
              !contract->donor_execution_authority &&
              !contract->sblr_execution_authority,
          row_id + " catalog projection contract granted unsafe authority");
  Require(StartsWith(contract->sb_system_catalog_source, "sys.catalog."),
          row_id + " catalog projection did not source sys.catalog");
  Require(StartsWith(contract->seed_rowset_id, "sys.catalog."),
          row_id + " seed rowset id did not source sys.catalog");

  const auto redacted = functions::EvaluateDonorCatalogProjection(
      functions::DonorCatalogProjectionRequest{
          Field(row, "engine_id"),
          row_id,
          Field(row, "item_name"),
          Field(row, "implementation_decision"),
          Field(row, "capability_family"),
          Field(row, "sb_normalized_target"),
          Field(row, "sb_catalog_projection"),
          Field(row, "catalog_exposure"),
          false,
      });
  Require(redacted.recognized && redacted.accepted && !redacted.denied,
          row_id + " redacted catalog projection was not accepted");
  Require(redacted.clean_database_projection,
          row_id + " redacted catalog projection did not build clean database rowset");
  Require(redacted.metadata_redacted,
          row_id + " public catalog projection did not redact metadata");
  Require(redacted.rows.size() == 1,
          row_id + " catalog projection did not generate exactly one seed row");
  VerifyResultAuthority(row_id, redacted);
  Require(redacted.diagnostic_code == "SB.DONOR_CATALOG_PROJECTION.READY",
          row_id + " ready diagnostic mismatch");
  Require(redacted.route_contract_id == contract->route_contract_id,
          row_id + " route contract mismatch");
  Require(redacted.seed_manifest.has_value() && redacted.contract.has_value(),
          row_id + " result omitted manifest or contract");
  Require(HasEvidence(redacted, "implementation_search_key", "SB_SYSTEM_CATALOG_PROJECTION"),
          row_id + " missing implementation search key evidence");
  Require(HasEvidence(redacted, "seed_manifest_resource", manifest->manifest_resource),
          row_id + " missing seed manifest resource evidence");
  Require(HasEvidence(redacted, "sb_system_catalog_source",
                      std::string(contract->sb_system_catalog_source)),
          row_id + " missing system catalog source evidence");

  const auto& public_row = redacted.rows.front();
  Require(public_row.visible, row_id + " redacted row was not visible");
  Require(public_row.metadata_redacted, row_id + " redacted row flag mismatch");
  Require(public_row.donor_item_name == "[redacted]",
          row_id + " redacted row leaked donor item name");
  Require(public_row.catalog_projection_label == "[redacted]",
          row_id + " redacted row leaked projection label");
  Require(public_row.catalog_exposure == "[redacted]",
          row_id + " redacted row leaked catalog exposure");
  Require(public_row.engine_owned && !public_row.parser_mutable && !public_row.donor_mutable,
          row_id + " redacted row authority flags drifted");
  Require(public_row.sb_system_catalog_source == contract->sb_system_catalog_source,
          row_id + " redacted row source mismatch");
  Require(public_row.seed_rowset_id == contract->seed_rowset_id,
          row_id + " redacted row seed rowset mismatch");
  Require(public_row.source_manifest_id == manifest->manifest_id,
          row_id + " redacted row source manifest mismatch");
  Require(public_row.clean_database_state == "present_from_donor_catalog_seed_manifest",
          row_id + " redacted row clean database state mismatch");

  const auto visible = functions::EvaluateDonorCatalogProjection(
      functions::DonorCatalogProjectionRequest{
          Field(row, "engine_id"),
          row_id,
          Field(row, "item_name"),
          Field(row, "implementation_decision"),
          Field(row, "capability_family"),
          Field(row, "sb_normalized_target"),
          Field(row, "sb_catalog_projection"),
          Field(row, "catalog_exposure"),
          true,
      });
  Require(visible.recognized && visible.accepted && !visible.denied,
          row_id + " visible catalog projection was not accepted");
  Require(!visible.metadata_redacted,
          row_id + " visible catalog projection remained redacted");
  Require(visible.rows.size() == 1,
          row_id + " visible catalog projection did not generate one seed row");
  VerifyResultAuthority(row_id, visible);
  const auto& visible_row = visible.rows.front();
  Require(!visible_row.metadata_redacted,
          row_id + " visible row redaction flag mismatch");
  Require(visible_row.donor_item_name == Field(row, "item_name"),
          row_id + " visible row item name mismatch");
  Require(visible_row.catalog_projection_label == Field(row, "sb_catalog_projection"),
          row_id + " visible row projection label mismatch");
  Require(visible_row.catalog_exposure == Field(row, "catalog_exposure"),
          row_id + " visible row catalog exposure mismatch");
}

void VerifyFailClosedCases() {
  const auto unknown_engine = functions::EvaluateDonorCatalogProjection(
      functions::DonorCatalogProjectionRequest{
          "unknown_engine",
          "unknown",
          "unknown",
          "catalog_projection_only",
          "admin_observability",
          "SB.ADMIN.PLUGIN_STATUS",
          "unknown",
          "unknown",
          true,
      });
  Require(!unknown_engine.recognized && !unknown_engine.accepted && unknown_engine.denied,
          "unknown engine catalog projection did not fail closed");
  Require(unknown_engine.diagnostic_code ==
              "SB.DONOR_CATALOG_PROJECTION.UNKNOWN_DONOR_MANIFEST",
          "unknown engine diagnostic mismatch");
  VerifyResultAuthority("unknown_engine", unknown_engine);

  const auto unknown_target = functions::EvaluateDonorCatalogProjection(
      functions::DonorCatalogProjectionRequest{
          "mysql",
          "unknown",
          "unknown",
          "catalog_projection_only",
          "admin_observability",
          "SB.UNKNOWN.TARGET",
          "unknown",
          "unknown",
          true,
      });
  Require(!unknown_target.recognized && !unknown_target.accepted && unknown_target.denied,
          "unknown target catalog projection did not fail closed");
  Require(unknown_target.diagnostic_code == "SB.DONOR_CATALOG_PROJECTION.UNKNOWN_TARGET",
          "unknown target diagnostic mismatch");
  VerifyResultAuthority("unknown_target", unknown_target);

  const auto non_catalog = functions::EvaluateDonorCatalogProjection(
      functions::DonorCatalogProjectionRequest{
          "mysql",
          "unknown",
          "unknown",
          "connector_operation",
          "admin_observability",
          "SB.ADMIN.PLUGIN_STATUS",
          "unknown",
          "unknown",
          true,
      });
  Require(!non_catalog.recognized && !non_catalog.accepted && non_catalog.denied,
          "non-catalog function row did not fail closed");
  Require(non_catalog.diagnostic_code ==
              "SB.DONOR_CATALOG_PROJECTION.NOT_CATALOG_PROJECTION",
          "non-catalog diagnostic mismatch");
  VerifyResultAuthority("non_catalog", non_catalog);
}

}  // namespace

int main(int argc, char** argv) {
  Require(argc == 2,
          "usage: sblr_surface_catalog_projection_seed_conformance "
          "<non_direct_function_matrix.csv>");
  const auto rows = LoadCsv(argv[1]);
  Require(rows.size() == 258, "non-direct function row count mismatch");

  int catalog_rows = 0;
  std::map<std::string, int> target_counts;
  std::set<std::string> engines;
  for (const auto& row : rows) {
    if (Field(row, "implementation_decision") != "catalog_projection_only") continue;
    ++catalog_rows;
    ++target_counts[Field(row, "sb_normalized_target")];
    engines.insert(Field(row, "engine_id"));
    VerifyCatalogRow(row);
  }

  Require(catalog_rows == 112, "catalog projection row count mismatch");
  Require(target_counts["SB.ADMIN.PLUGIN_STATUS"] == 72,
          "plugin-status target count mismatch");
  Require(target_counts["SB.EXTENSION.ITEMS"] == 28,
          "extension-items target count mismatch");
  Require(target_counts["SB.EXTENSION.LIST"] == 12,
          "extension-list target count mismatch");
  Require(engines.size() == 19, "catalog projection donor engine count mismatch");

  VerifyFailClosedCases();

  std::cout << "sblr_surface_catalog_projection_seed_conformance=passed\n";
  return EXIT_SUCCESS;
}
