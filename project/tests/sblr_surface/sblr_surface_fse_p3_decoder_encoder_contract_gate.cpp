// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_admission.hpp"
#include "sblr_engine_envelope.hpp"

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

namespace sblr = scratchbird::engine::sblr;
namespace server = scratchbird::server;

using Row = std::map<std::string, std::string>;

std::string ResolveSblrExpansionRoot(const std::string& input) {
  if (input.find("final-sblr-sbsql-sblr-expansion-closure") != std::string::npos) {
    return input;
  }
  return input + "/public_execution_plan";
}

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) Fail(message);
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
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

std::vector<Row> LoadCsv(const std::string& path) {
  std::ifstream input(path);
  Require(input.good(), "could not open CSV: " + path);
  std::string line;
  Require(static_cast<bool>(std::getline(input, line)), "empty CSV: " + path);
  if (!line.empty() && line.back() == '\r') line.pop_back();
  const auto header = ParseCsvLine(line);
  std::vector<Row> rows;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    const auto values = ParseCsvLine(line);
    Require(values.size() == header.size(), "CSV row width mismatch: " + path);
    Row row;
    for (std::size_t i = 0; i < header.size(); ++i) row.emplace(header[i], values[i]);
    rows.push_back(std::move(row));
  }
  return rows;
}

const std::string& Field(const Row& row, const std::string& name) {
  const auto it = row.find(name);
  if (it == row.end()) Fail("missing CSV field: " + name);
  return it->second;
}

void RequireColumns(const std::vector<Row>& rows,
                    const std::vector<std::string>& columns,
                    const std::string& label) {
  Require(!rows.empty(), label + " has no rows");
  for (const auto& column : columns) {
    Require(rows.front().find(column) != rows.front().end(),
            label + " missing required column " + column);
  }
  for (const auto& row : rows) {
    for (const auto& column : columns) {
      Require(!Field(row, column).empty(), label + " has empty required field " + column);
    }
  }
}

std::map<std::string, Row> IndexBySourceImportId(const std::vector<Row>& rows,
                                                 const std::string& label) {
  std::map<std::string, Row> out;
  for (const auto& row : rows) {
    const auto& source_import_id = Field(row, "source_import_id");
    Require(out.emplace(source_import_id, row).second,
            label + " duplicate source_import_id " + source_import_id);
  }
  return out;
}

bool HasDiagnostic(const server::ServerSblrAdmissionResult& result,
                   std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

bool HasEnvelopeDiagnostic(const sblr::SblrEnvelopeValidationResult& result,
                           std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

bool HasDecodeDiagnostic(const sblr::SblrDecodeResult& result,
                         std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

sblr::SblrOperationEnvelope EnvelopeForRow(const Row& register_row,
                                           const Row& binary_row) {
  auto envelope = sblr::MakeSblrEnvelope(Field(register_row, "operation_id"),
                                         Field(binary_row, "opcode_or_symbol"),
                                         "FSE-P3-decoder-encoder-proof");
  envelope.source_artifact_map.policy_status = "non_authoritative_render_metadata";
  envelope.source_artifact_map.source_identity =
      "fse-p3:" + Field(register_row, "source_import_id");
  envelope.source_artifact_map.source_hash = "sha256:fse-p3-matrix-proof";
  envelope.source_artifact_map.render_metadata_only = true;
  envelope.source_artifact_map.contains_sql_text = false;
  envelope.source_artifact_map.raw_sql_text_authoritative = false;
  envelope.requires_cluster_authority = StartsWith(Field(register_row, "operation_id"), "cluster.");
  envelope.requires_transaction_context =
      Field(register_row, "authority_class") == "MGA_TRANSACTION_FINALITY";
  return envelope;
}

void VerifyCoverage(const std::vector<Row>& register_rows,
                    const std::vector<Row>& binary_rows,
                    const std::vector<Row>& compatibility_rows,
                    const std::vector<Row>& test_rows) {
  Require(register_rows.size() == 2760, "operation register row count drift");
  Require(binary_rows.size() == 2760, "binary matrix row count drift");
  Require(compatibility_rows.size() == 2760, "compatibility matrix row count drift");
  Require(test_rows.size() == 2760, "decoder encoder matrix row count drift");

  const auto binary_by_id = IndexBySourceImportId(binary_rows, "binary matrix");
  const auto compatibility_by_id =
      IndexBySourceImportId(compatibility_rows, "compatibility matrix");
  const auto test_by_id = IndexBySourceImportId(test_rows, "decoder encoder matrix");

  std::set<std::string> families;
  int cluster_rows = 0;
  int cluster_query_rows = 0;
  int exact_refusal_rows = 0;
  int external_authority_rows = 0;
  int mga_rows = 0;
  for (const auto& row : register_rows) {
    const auto& source_import_id = Field(row, "source_import_id");
    Require(binary_by_id.contains(source_import_id),
            "binary matrix missing " + source_import_id);
    Require(compatibility_by_id.contains(source_import_id),
            "compatibility matrix missing " + source_import_id);
    Require(test_by_id.contains(source_import_id),
            "decoder encoder matrix missing " + source_import_id);
    families.insert(Field(row, "sblr_family"));

    const auto& binary = binary_by_id.at(source_import_id);
    const auto& compatibility = compatibility_by_id.at(source_import_id);
    const auto& test = test_by_id.at(source_import_id);
    Require(Field(binary, "operation_id") == Field(row, "operation_id"),
            "binary operation_id mismatch " + source_import_id);
    Require(Field(compatibility, "operation_id") == Field(row, "operation_id"),
            "compatibility operation_id mismatch " + source_import_id);
    Require(Field(test, "operation_id") == Field(row, "operation_id"),
            "test matrix operation_id mismatch " + source_import_id);

    const bool is_cluster = Field(row, "source_type") == "cluster_normalization";
    const bool is_query = StartsWith(Field(row, "operation_id"), "cluster.query.");
    const std::string expected_marker =
        is_query ? "cross_node_distributed_cluster_authority_not_local_query"
                 : "not_cluster_query";
    Require(Field(binary, "cluster_query_authority_marker") == expected_marker,
            "binary cluster query marker mismatch " + source_import_id);
    Require(Field(compatibility, "cluster_query_authority_marker") == expected_marker,
            "compatibility cluster query marker mismatch " + source_import_id);
    if (is_cluster) ++cluster_rows;
    if (is_query) {
      ++cluster_query_rows;
      Require(Field(binary, "sblr_family") == "sblr.cluster.query.v1",
              "cluster query binary family drift " + source_import_id);
      Require(Field(compatibility, "sblr_family") == "sblr.cluster.query.v1",
              "cluster query compatibility family drift " + source_import_id);
      Require(Contains(Field(test, "authority_boundary_test"),
                       "cross_node_distributed_cluster_authority_not_local_query"),
              "cluster query test matrix authority marker missing " + source_import_id);
    }

    const bool exact_refusal =
        Field(row, "authority_class") == "EXACT_POLICY_REFUSAL" ||
        Field(row, "implementation_expectation") == "architecture_refusal" ||
        Field(row, "implementation_expectation") == "exact_refusal";
    if (exact_refusal) {
      ++exact_refusal_rows;
      const std::string refusal_binary_contract =
          Field(binary, "payload_schema") + ";" +
          Field(binary, "required_fields") + ";" +
          Field(binary, "canonical_encoding") + ";" +
          Field(binary, "authority_token_encoding") + ";" +
          Field(binary, "notes");
      Require(Contains(refusal_binary_contract, "no_execution_payload"),
              "exact refusal row has execution payload " + source_import_id);
      Require(Contains(refusal_binary_contract, "provider_call_forbidden") ||
                  Contains(refusal_binary_contract, "no provider call"),
              "exact refusal row allows provider route " + source_import_id);
    }

    if (Field(row, "implementation_expectation") == "external_authority_fail_closed") {
      ++external_authority_rows;
      Require(Contains(Field(binary, "payload_schema"), "redacted_provider_descriptor"),
              "external authority row lacks redacted descriptor " + source_import_id);
      Require(Contains(Field(binary, "malformed_input_refusal"), "before_provider_admission"),
              "external authority row does not fail closed before provider " + source_import_id);
    }

    if (Field(row, "authority_class") == "MGA_TRANSACTION_FINALITY") {
      ++mga_rows;
      const std::string evidence =
          Field(binary, "payload_schema") + ";" + Field(binary, "authority_token_encoding");
      Require(Contains(evidence, "engine_mga") ||
                  Contains(evidence, "engine_finality_evidence"),
              "MGA row lacks engine-owned finality evidence " + source_import_id);
      Require(Contains(evidence, "provider_finality_authority_forbidden"),
              "MGA row allows provider finality authority " + source_import_id);
    }

    const bool existing_family =
        Field(compatibility, "existing_encoding_preserved") ==
        "yes_existing_encoding_preserved";
    if (existing_family) {
      Require(Contains(Field(compatibility, "compatibility_guard"),
                       "existing_encoding_preserved_guard"),
              "existing family compatibility guard missing " + source_import_id);
    } else {
      const std::string fail_closed =
          Field(compatibility, "unsupported_format_behavior") + ";" +
          Field(compatibility, "unsupported_version_refusal") + ";" +
          Field(compatibility, "compatibility_guard");
      Require(Contains(fail_closed, "unsupported") ||
                  Contains(fail_closed, "refusal") ||
                  Contains(fail_closed, "reject"),
              "non-existing family lacks fail-closed behavior " + source_import_id);
    }
  }

  Require(families.size() >= 12, "sblr family coverage unexpectedly small");
  Require(cluster_rows == 59, "cluster row coverage drift");
  Require(cluster_query_rows == 9, "cluster query row coverage drift");
  Require(exact_refusal_rows == 396, "exact refusal row coverage drift");
  Require(external_authority_rows == 380, "external authority row coverage drift");
  Require(mga_rows == 124, "MGA/finality row coverage drift");
}

std::vector<std::string> SampleSourceImportIds(const std::vector<Row>& register_rows) {
  std::set<std::string> selected;
  std::set<std::string> seen_families;
  for (const auto& row : register_rows) {
    if (seen_families.insert(Field(row, "sblr_family")).second) {
      selected.insert(Field(row, "source_import_id"));
    }
    if (StartsWith(Field(row, "operation_id"), "cluster.query.") ||
        Field(row, "authority_class") == "EXACT_POLICY_REFUSAL" ||
        Field(row, "implementation_expectation") == "architecture_refusal" ||
        Field(row, "implementation_expectation") == "exact_refusal") {
      selected.insert(Field(row, "source_import_id"));
    }
  }
  return {selected.begin(), selected.end()};
}

void VerifyCompiledEnvelopeApis(const std::vector<Row>& register_rows,
                                const std::map<std::string, Row>& binary_by_id) {
  for (const auto& source_import_id : SampleSourceImportIds(register_rows)) {
    const auto it = std::find_if(register_rows.begin(),
                                 register_rows.end(),
                                 [&](const Row& row) {
                                   return Field(row, "source_import_id") == source_import_id;
                                 });
    Require(it != register_rows.end(), "sample source_import_id not found");
    const auto envelope = EnvelopeForRow(*it, binary_by_id.at(source_import_id));
    const auto encoded = sblr::EncodeSblrEnvelope(envelope);
    const auto decoded = sblr::DecodeSblrEnvelope(encoded);
    Require(decoded.ok, "DecodeSblrEnvelope rejected sample " + source_import_id);
    Require(decoded.envelope.operation_id == envelope.operation_id,
            "decoded operation_id mismatch " + source_import_id);
    Require(decoded.envelope.opcode == envelope.opcode,
            "decoded opcode mismatch " + source_import_id);
    const auto validation = sblr::ValidateSblrEnvelope(decoded.envelope);
    Require(validation.ok, "ValidateSblrEnvelope rejected sample " + source_import_id);

    auto sql_text = decoded.envelope;
    sql_text.contains_sql_text = true;
    const auto sql_validation = sblr::ValidateSblrEnvelope(sql_text);
    Require(!sql_validation.ok &&
                HasEnvelopeDiagnostic(sql_validation, "SB_SBLR_SQL_TEXT_FORBIDDEN"),
            "SQL text envelope was not rejected " + source_import_id);

    auto unsupported_major = decoded.envelope;
    unsupported_major.envelope_major = 999;
    const auto version_validation = sblr::ValidateSblrEnvelope(unsupported_major);
    Require(!version_validation.ok &&
                HasEnvelopeDiagnostic(version_validation,
                                      "SB_SBLR_ENVELOPE_MAJOR_UNSUPPORTED"),
            "unsupported envelope major was not rejected " + source_import_id);

    auto missing_opcode = decoded.envelope;
    missing_opcode.opcode.clear();
    const auto missing_opcode_encoded = sblr::EncodeSblrEnvelope(missing_opcode);
    const auto missing_opcode_decoded = sblr::DecodeSblrEnvelope(missing_opcode_encoded);
    Require(!missing_opcode_decoded.ok &&
                HasDecodeDiagnostic(missing_opcode_decoded, "SB_SBLR_OPCODE_REQUIRED"),
            "missing opcode was not rejected " + source_import_id);

    const auto admission = server::AdmitServerSblrEnvelope(
        server::ServerSblrAdmissionRequest{encoded, false});
    if (StartsWith(Field(*it, "operation_id"), "cluster.query.")) {
      Require(!admission.admitted ||
                  Field(*it, "sblr_family") == "sblr.cluster.query.v1",
              "cluster query sample drifted from cluster query family");
    }
    if (Field(*it, "authority_class") == "EXACT_POLICY_REFUSAL") {
      Require(!admission.admitted,
              "exact refusal sample was admitted for execution " + source_import_id);
    }
  }

  const auto raw_sql = server::AdmitServerSblrEnvelope(
      server::ServerSblrAdmissionRequest{"select * from forbidden_source", false});
  Require(!raw_sql.admitted && HasDiagnostic(raw_sql, "SBLR.SQL_TEXT_FORBIDDEN"),
          "raw SQL was accepted as SBLR");

  auto local_query = sblr::MakeSblrEnvelope("query.plan_operation",
                                            "SBLR_QUERY_PLAN_OPERATION",
                                            "FSE-P3-local-query-cluster-refusal");
  local_query.requires_cluster_authority = true;
  local_query.source_artifact_map.policy_status = "non_authoritative_render_metadata";
  local_query.source_artifact_map.source_identity = "fse-p3:local-query-refusal";
  local_query.source_artifact_map.source_hash = "sha256:fse-p3-local-query-refusal";
  const auto local_query_admission = server::AdmitServerSblrEnvelope(
      server::ServerSblrAdmissionRequest{sblr::EncodeSblrEnvelope(local_query), true});
  Require(!local_query_admission.admitted &&
              HasDiagnostic(local_query_admission, "SBLR.CLUSTER_MAPPING.UNAVAILABLE"),
          "local query operation was admitted as cluster query authority");
}

}  // namespace

int main(int argc, char** argv) {
  Require(argc == 2,
          "usage: sblr_surface_fse_p3_decoder_encoder_contract_gate "
          "<repo-root-or-sblr-execution_plan-dir>");
  const std::string execution_plan_dir = ResolveSblrExpansionRoot(argv[1]);
  const auto register_rows =
      LoadCsv(execution_plan_dir + "/SBLR_OPERATION_EXPANSION_REGISTER.csv");
  const auto binary_rows =
      LoadCsv(execution_plan_dir + "/SBLR_BINARY_FORMAT_EXTENSION_MATRIX.csv");
  const auto compatibility_rows =
      LoadCsv(execution_plan_dir + "/SBLR_BACKWARD_COMPATIBILITY_MATRIX.csv");
  const auto test_rows =
      LoadCsv(execution_plan_dir + "/SBLR_DECODER_ENCODER_TEST_MATRIX.csv");

  RequireColumns(test_rows,
                 {"operation_id",
                  "source_import_id",
                  "sblr_family",
                  "opcode_or_symbol",
                  "encoder_contract",
                  "decoder_contract",
                  "roundtrip_test",
                  "malformed_test",
                  "unsupported_version_test",
                  "parser_admission_test",
                  "authority_boundary_test",
                  "proof_test_name",
                  "proof_file",
                  "status"},
                 "decoder encoder matrix");
  VerifyCoverage(register_rows, binary_rows, compatibility_rows, test_rows);
  VerifyCompiledEnvelopeApis(register_rows,
                             IndexBySourceImportId(binary_rows, "binary matrix"));
  std::cout << "sblr_surface_fse_p3_decoder_encoder_contract_gate=passed\n";
  return EXIT_SUCCESS;
}
