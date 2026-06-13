// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cluster_provider/cluster_provider.hpp"
#include "sblr_admission.hpp"
#include "sblr_engine_envelope.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace cluster_provider = scratchbird::engine::cluster_provider;
namespace sblr = scratchbird::engine::sblr;
namespace server = scratchbird::server;

using Row = std::map<std::string, std::string>;
constexpr int kSkipReturnCode = 77;

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

bool FileExists(const std::string& path) {
  std::ifstream input(path);
  return input.good();
}

int SkipMissingExecutionPlan(const std::string& path) {
  std::cout << "SKIP: public execution-plan CSV is not present in this checkout: "
            << path << '\n';
  return kSkipReturnCode;
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

std::map<std::string, Row> IndexBy(const std::vector<Row>& rows,
                                   const std::string& column,
                                   const std::string& label) {
  std::map<std::string, Row> out;
  for (const auto& row : rows) {
    const auto& key = Field(row, column);
    Require(out.emplace(key, row).second, label + " duplicate " + column + " " + key);
  }
  return out;
}

bool HasAdmissionDiagnostic(const server::ServerSblrAdmissionResult& result,
                            std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

bool HasApiDiagnostic(const api::EngineApiResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

bool HasUnsupportedFeature(const api::EngineApiResult& result,
                           std::string_view feature) {
  for (const auto& unsupported : result.unsupported_features) {
    if (unsupported.feature == feature) return true;
  }
  return false;
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && evidence.evidence_id == id) return true;
  }
  return false;
}

api::EngineRequestContext Context() {
  api::EngineRequestContext context;
  context.security_context_present = true;
  context.cluster_authority_available = true;
  context.transaction_uuid.canonical = "019f0000-0000-7000-8000-00000000f701";
  context.local_transaction_id = 7707;
  context.database_uuid.canonical = "fse-p7-execution-proof-database";
  context.session_uuid.canonical = "fse-p7-execution-proof-session";
  context.principal_uuid.canonical = "fse-p7-execution-proof-principal";
  context.trace_tags.push_back("sblr_surface_fse_p7_execution_proof_gate");
  return context;
}

cluster_provider::ClusterProviderRequest ProviderRequest(std::string operation_id) {
  cluster_provider::ClusterProviderRequest request;
  request.context = Context();
  request.envelope.operation_id = std::move(operation_id);
  request.envelope.opcode = "SBLR_FSE_P7_CLUSTER_PROVIDER_PROOF";
  request.envelope.trace_key = "fse-p7-execution-proof";
  request.envelope.requires_security_context = true;
  request.envelope.requires_cluster_authority = true;
  request.envelope.contains_sql_text = false;
  request.api_request.context = request.context;
  request.api_request.operation_id = request.envelope.operation_id;
  return request;
}

void VerifyCounts(const std::vector<Row>& p0,
                  const std::vector<Row>& p1,
                  const std::vector<Row>& binary,
                  const std::vector<Row>& compatibility,
                  const std::vector<Row>& decoder,
                  const std::vector<Row>& p4,
                  const std::vector<Row>& p5,
                  const std::vector<Row>& p6,
                  const std::vector<Row>& p7) {
  Require(p0.size() == 2760, "P0 import row count drift");
  Require(p1.size() == 2760, "P1 operation row count drift");
  Require(binary.size() == 2760, "P2 binary row count drift");
  Require(compatibility.size() == 2760, "P2 compatibility row count drift");
  Require(decoder.size() == 2760, "P3 decoder row count drift");
  Require(p4.size() == 2701, "P4 route row count drift");
  Require(p5.size() == 439, "P5 policy row count drift");
  Require(p6.size() == 2760, "P6 diagnostic row count drift");
  Require(p7.size() == 2760, "P7 proof row count drift");
}

void VerifyCoverage(const std::vector<Row>& p0,
                    const std::vector<Row>& p1,
                    const std::vector<Row>& binary,
                    const std::vector<Row>& compatibility,
                    const std::vector<Row>& decoder,
                    const std::vector<Row>& p4,
                    const std::vector<Row>& p5,
                    const std::vector<Row>& p6,
                    const std::vector<Row>& p7) {
  const auto p0_by_id = IndexBy(p0, "import_id", "P0 import matrix");
  const auto bin_by_id = IndexBy(binary, "source_import_id", "P2 binary matrix");
  const auto compat_by_id =
      IndexBy(compatibility, "source_import_id", "P2 compatibility matrix");
  const auto decoder_by_id = IndexBy(decoder, "source_import_id", "P3 decoder matrix");
  const auto p4_by_id = IndexBy(p4, "source_import_id", "P4 route matrix");
  const auto p5_by_id = IndexBy(p5, "source_import_id", "P5 policy matrix");
  const auto p6_by_id = IndexBy(p6, "source_import_id", "P6 diagnostic matrix");
  const auto p7_by_id = IndexBy(p7, "source_import_id", "P7 proof matrix");

  int cluster_rows = 0;
  int external_rows = 0;
  int query_rows = 0;
  int mga_rows = 0;
  for (const auto& row : p1) {
    const auto& source_import_id = Field(row, "source_import_id");
    const auto& operation_id = Field(row, "operation_id");
    Require(p0_by_id.contains(source_import_id), "P0 missing " + source_import_id);
    Require(bin_by_id.contains(source_import_id), "P2 binary missing " + source_import_id);
    Require(compat_by_id.contains(source_import_id),
            "P2 compatibility missing " + source_import_id);
    Require(decoder_by_id.contains(source_import_id), "P3 missing " + source_import_id);
    Require(p6_by_id.contains(source_import_id), "P6 missing " + source_import_id);
    Require(p7_by_id.contains(source_import_id), "P7 missing " + source_import_id);

    const bool cluster = Field(row, "source_type") == "cluster_normalization";
    const bool external =
        Field(row, "implementation_expectation") == "external_authority_fail_closed";
    const bool cluster_query = StartsWith(operation_id, "cluster.query.");
    if (cluster) {
      ++cluster_rows;
      Require(!p4_by_id.contains(source_import_id),
              "cluster row leaked into P4 route matrix " + source_import_id);
      Require(p5_by_id.contains(source_import_id),
              "cluster row missing P5 policy " + source_import_id);
      Require(Field(p7_by_id.at(source_import_id), "p4_route_id") ==
                  "cluster_rows_excluded_from_p4_by_design",
              "P7 did not record cluster P4 exclusion " + source_import_id);
    } else {
      Require(p4_by_id.contains(source_import_id),
              "non-cluster row missing P4 route " + source_import_id);
    }
    if (external) {
      ++external_rows;
      Require(p5_by_id.contains(source_import_id),
              "external authority row missing P5 policy " + source_import_id);
      Require(Contains(Field(p7_by_id.at(source_import_id), "external_authority_policy"),
                       "fail_closed"),
              "P7 external policy is not fail-closed " + source_import_id);
    }
    if (cluster_query) {
      ++query_rows;
      Require(Field(row, "sblr_family") == "sblr.cluster.query.v1",
              "cluster query row is not in sblr.cluster.query.v1 " + source_import_id);
      Require(Field(p7_by_id.at(source_import_id), "cluster_query_authority_marker") ==
                  "cross_node_distributed_cluster_authority_not_local_query",
              "P7 cluster query marker drift " + source_import_id);
      Require(Contains(Field(p7_by_id.at(source_import_id), "final_proof_assertion"),
                       "cluster_provider_boundary"),
              "cluster query proof did not remain provider-boundary " + source_import_id);
    }
    if (Field(row, "authority_class") == "MGA_TRANSACTION_FINALITY") {
      ++mga_rows;
      Require(Contains(Field(p7_by_id.at(source_import_id), "mga_finality_policy"),
                       "engine_owned_mga_finality"),
              "P7 MGA/finality policy missing engine ownership " + source_import_id);
      Require(Contains(Field(p7_by_id.at(source_import_id), "mga_finality_policy"),
                       "parser_reference_provider_finality_forbidden"),
              "P7 MGA/finality policy permits non-engine finality " + source_import_id);
    }

    const auto& proof = p7_by_id.at(source_import_id);
    Require(Field(proof, "operation_id") == operation_id,
            "P7 operation mismatch " + source_import_id);
    Require(Field(proof, "source_search_key") == Field(row, "source_search_key"),
            "P7 source key mismatch " + source_import_id);
    Require(Field(proof, "p0_import_id") == source_import_id,
            "P7 P0 import mismatch " + source_import_id);
    Require(Field(proof, "p2_binary_format_id") == Field(bin_by_id.at(source_import_id),
                                                          "binary_format_id"),
            "P7 binary id mismatch " + source_import_id);
    Require(Field(proof, "p2_compatibility_id") ==
                Field(compat_by_id.at(source_import_id), "compatibility_id"),
            "P7 compatibility id mismatch " + source_import_id);
    Require(Field(proof, "p6_diagnostic_id") == Field(p6_by_id.at(source_import_id),
                                                       "diagnostic_id"),
            "P7 diagnostic id mismatch " + source_import_id);
    Require(Contains(Field(proof, "parser_authority_policy"), "forbidden") &&
                Contains(Field(proof, "reference_authority_policy"), "forbidden"),
            "P7 parser/reference authority policy drift " + source_import_id);
    Require(Contains(Field(proof, "raw_sql_authority_policy"), "not_authoritative"),
            "P7 raw SQL authority policy drift " + source_import_id);
    Require(Contains(Field(proof, "executable_gate_set"),
                     "sblr_surface_fse_p7_execution_proof_gate"),
            "P7 gate set missing final proof gate " + source_import_id);
  }
  Require(cluster_rows == 59, "cluster row count drift");
  Require(external_rows == 380, "external authority row count drift");
  Require(query_rows == 9, "cluster query row count drift");
  Require(mga_rows == 124, "MGA/finality row count drift");
}

void VerifyDiagnosticsAndAuthority(const std::vector<Row>& p6,
                                   const std::vector<Row>& p7) {
  for (const auto& row : p6) {
    const auto& source_import_id = Field(row, "source_import_id");
    Require(Contains(Field(row, "secret_leakage_policy"), "forbid") &&
                Contains(Field(row, "secret_leakage_policy"), "raw_sql") &&
                Contains(Field(row, "secret_leakage_policy"), "local_path") &&
                Contains(Field(row, "secret_leakage_policy"), "provider_private"),
            "P6 redaction leakage policy incomplete " + source_import_id);
    if (Field(row, "implementation_expectation") == "external_authority_fail_closed") {
      Require(Contains(Field(row, "support_bundle_fields"),
                       "redacted_provider_descriptor"),
              "external row lacks redacted provider descriptor " + source_import_id);
      Require(Contains(Field(row, "failure_vector"), "fail_closed"),
              "external row is not fail-closed " + source_import_id);
    }
    if (Field(row, "authority_class") == "MGA_TRANSACTION_FINALITY") {
      Require(Contains(Field(row, "message_vector_code_policy"), "ENGINE_AUTHORITY"),
              "MGA diagnostic does not assert engine authority " + source_import_id);
    }
  }
  for (const auto& row : p7) {
    Require(Contains(Field(row, "notes"), "cross-checks"),
            "P7 proof row lacks cross-check statement " + Field(row, "source_import_id"));
  }
}

void VerifyCompletionMetadata(const std::vector<Row>& tracker,
                              const std::vector<Row>& gates,
                              const std::vector<Row>& outputs,
                              const std::vector<Row>& audit) {
  int non_pending = 0;
  bool tracker_p7 = false;
  for (const auto& row : tracker) {
    if (Field(row, "status") != "pending") ++non_pending;
    if (Field(row, "slice_id") == "FSE-P7") {
      tracker_p7 = Contains(Field(row, "status"), "p7") &&
                   Contains(Field(row, "outputs"), "SBLR_EXECUTION_PROOF_MATRIX.csv") &&
                   Contains(Field(row, "outputs"),
                            "sblr_surface_fse_p7_execution_proof_gate.cpp");
    }
  }
  Require(non_pending == 8, "tracker did not close exactly P0-P7");
  Require(tracker_p7, "FSE-P7 tracker row did not cite proof matrix and gate");

  bool gate_p7 = false;
  for (const auto& row : gates) {
    if (Field(row, "gate_id") == "FSE-GATE-008") {
      gate_p7 = Contains(Field(row, "status"), "p7") &&
                 Contains(Field(row, "evidence"), "SBLR_EXECUTION_PROOF_MATRIX.csv") &&
                 Contains(Field(row, "evidence"), "sblr_surface_fse_p7_execution_proof_gate.cpp");
    }
  }
  Require(gate_p7, "FSE-GATE-008 did not cite executable P7 proof evidence");

  bool output_p7 = false;
  for (const auto& row : outputs) {
    if (Field(row, "artifact") == "SBLR_EXECUTION_PROOF_MATRIX.csv") {
      output_p7 = Contains(Field(row, "status"), "p7") &&
                  Contains(Field(row, "required_columns"), "proof_id") &&
                  Contains(Field(row, "required_columns"), "executable_gate_set");
    }
  }
  Require(output_p7, "output contract missing completed P7 proof matrix");

  bool audit_matrix = false;
  bool audit_gate = false;
  for (const auto& row : audit) {
    if (Field(row, "source_search_key") == "FSE_P7_EXECUTION_PROOF_MATRIX") {
      audit_matrix = Contains(Field(row, "status"), "p7");
    }
    if (Field(row, "source_search_key") == "FSE_P7_EXECUTION_PROOF_CTEST") {
      audit_gate = Contains(Field(row, "status"), "p7");
    }
  }
  Require(audit_matrix && audit_gate, "implementation audit missing P7 matrix/gate evidence");
}

void VerifyCompiledProviderBoundary() {
  Require(cluster_provider::RequiredClusterProviderCommandBoundarySet().size() == 59,
          "cluster provider command boundary size drift");
  Require(cluster_provider::RequiredClusterProviderPreAdmissionRefusalSet().size() == 3,
          "cluster exact refusal set size drift");
  for (const auto& command :
       cluster_provider::RequiredClusterProviderCommandBoundarySet()) {
    if (command.provider_routed) {
      Require(cluster_provider::ContainsProviderToken(
                  cluster_provider::RequiredClusterProviderOperationSet(),
                  command.normalized_command),
              "routed normalized cluster command missing from operation set");
      Require(cluster_provider::ContainsProviderToken(
                  cluster_provider::RequiredClusterProviderOperationSet(),
                  command.provider_operation_id),
              "routed provider alias missing from operation set");
    } else {
      Require(!cluster_provider::ContainsProviderToken(
                  cluster_provider::RequiredClusterProviderOperationSet(),
                  command.normalized_command),
              "exact pre-provider refusal leaked into provider operation set");
      Require(cluster_provider::ContainsProviderToken(
                  cluster_provider::RequiredClusterProviderPreAdmissionRefusalSet(),
                  command.normalized_command),
              "exact pre-provider refusal missing from refusal set");
    }
  }
  Require(!cluster_provider::ContainsProviderToken(
              cluster_provider::RequiredClusterProviderOperationSet(),
              "query.plan_operation"),
          "local query operation leaked into cluster provider operation set");

  const auto info = cluster_provider::DescribeClusterProvider();
  Require(!info.supports_execution && !info.supports_route_admission,
          "public cluster provider is not fail-closed");
  const bool no_cluster = info.provider_type == "no_cluster";
  const bool compile_stub = info.provider_type == "compile_link_stub";
  Require(no_cluster || compile_stub, "unexpected public cluster provider type");
  const auto result =
      cluster_provider::ExecuteClusterOperation(ProviderRequest("cluster.query.plan_distributed"));
  Require(!result.ok && result.cluster_authority_required,
          "public cluster provider executed distributed query");
  Require(result.result_shape.rows.empty(),
          "public cluster provider returned mutable rows for fail-closed query");
  if (no_cluster) {
    Require(HasApiDiagnostic(result, cluster_provider::kClusterSupportNotEnabledCode),
            "no-cluster diagnostic missing");
    Require(HasUnsupportedFeature(result, "cluster.provider"),
            "no-cluster unsupported feature missing");
  } else {
    Require(HasApiDiagnostic(result,
                             cluster_provider::kClusterHandshakeStubCompileLinkOnlyCode),
            "compile-link stub diagnostic missing");
    Require(HasUnsupportedFeature(result, "cluster.provider.stub"),
            "compile-link stub unsupported feature missing");
  }
  Require(HasEvidence(result, "cluster_provider_route_admission", "false"),
          "fail-closed provider route evidence missing");

  auto local_query = sblr::MakeSblrEnvelope("query.plan_operation",
                                            "SBLR_QUERY_PLAN_OPERATION",
                                            "FSE-P7-local-query-refusal");
  local_query.requires_cluster_authority = true;
  local_query.source_artifact_map.policy_status = "non_authoritative_render_metadata";
  local_query.source_artifact_map.source_identity = "fse-p7:local-query-refusal";
  local_query.source_artifact_map.source_hash = "sha256:fse-p7-local-query-refusal";
  local_query.source_artifact_map.contains_sql_text = false;
  local_query.source_artifact_map.raw_sql_text_authoritative = false;
  const auto local_query_admission = server::AdmitServerSblrEnvelope(
      server::ServerSblrAdmissionRequest{sblr::EncodeSblrEnvelope(local_query), true});
  Require(!local_query_admission.admitted &&
              HasAdmissionDiagnostic(local_query_admission,
                                     "SBLR.CLUSTER_MAPPING.UNAVAILABLE"),
          "local query.plan_operation was usable as cluster query authority");

  const auto raw_sql = server::AdmitServerSblrEnvelope(
      server::ServerSblrAdmissionRequest{"select * from fse_p7_forbidden", false});
  Require(!raw_sql.admitted && HasAdmissionDiagnostic(raw_sql, "SBLR.SQL_TEXT_FORBIDDEN"),
          "raw SQL text was admitted as authoritative SBLR");
}

}  // namespace

int main(int argc, char** argv) {
  Require(argc == 2,
          "usage: sblr_surface_fse_p7_execution_proof_gate "
          "<repo-root-or-sblr-execution_plan-dir>");
  const std::string dir = ResolveSblrExpansionRoot(argv[1]);
  const std::string p0_path = dir + "/SBLR_P0_INPUT_IMPORT_DEDUP_MATRIX.csv";
  if (!FileExists(p0_path)) return SkipMissingExecutionPlan(p0_path);
  const auto p0 = LoadCsv(p0_path);
  const auto p1 = LoadCsv(dir + "/SBLR_OPERATION_EXPANSION_REGISTER.csv");
  const auto binary = LoadCsv(dir + "/SBLR_BINARY_FORMAT_EXTENSION_MATRIX.csv");
  const auto compatibility = LoadCsv(dir + "/SBLR_BACKWARD_COMPATIBILITY_MATRIX.csv");
  const auto decoder = LoadCsv(dir + "/SBLR_DECODER_ENCODER_TEST_MATRIX.csv");
  const auto p4 = LoadCsv(dir + "/ENGINE_INTERNAL_API_ROUTE_MATRIX.csv");
  const auto p5 = LoadCsv(dir + "/SBLR_CLUSTER_AND_EXTERNAL_AUTHORITY_POLICY_MATRIX.csv");
  const auto p6 = LoadCsv(dir + "/SBLR_RESULT_VECTOR_AND_DIAGNOSTIC_MATRIX.csv");
  const auto p7 = LoadCsv(dir + "/SBLR_EXECUTION_PROOF_MATRIX.csv");
  const auto tracker = LoadCsv(dir + "/TRACKER.csv");
  const auto gates = LoadCsv(dir + "/ACCEPTANCE_GATES.csv");
  const auto outputs = LoadCsv(dir + "/OUTPUT_CONTRACT.csv");
  const auto audit = LoadCsv(dir + "/SPEC_IMPLEMENTATION_AUDIT_MATRIX.csv");

  RequireColumns(
      p7,
      {"proof_id",
       "operation_id",
       "source_import_id",
       "source_search_key",
       "source_type",
       "authority_class",
       "implementation_expectation",
       "sblr_family",
       "p0_import_id",
       "p1_operation_status",
       "p2_binary_format_id",
       "p2_compatibility_id",
       "p3_decoder_encoder_proof",
       "p4_route_id",
       "p5_policy_id",
       "p6_diagnostic_id",
       "cluster_query_authority_marker",
       "mga_finality_policy",
       "external_authority_policy",
       "parser_authority_policy",
       "reference_authority_policy",
       "raw_sql_authority_policy",
       "executable_gate_set",
       "final_proof_assertion",
       "proof_test_name",
       "proof_file",
       "status",
       "notes"},
      "P7 execution proof matrix");

  VerifyCounts(p0, p1, binary, compatibility, decoder, p4, p5, p6, p7);
  VerifyCoverage(p0, p1, binary, compatibility, decoder, p4, p5, p6, p7);
  VerifyDiagnosticsAndAuthority(p6, p7);
  VerifyCompletionMetadata(tracker, gates, outputs, audit);
  VerifyCompiledProviderBoundary();
  std::cout << "sblr_surface_fse_p7_execution_proof_gate=passed\n";
  return EXIT_SUCCESS;
}
