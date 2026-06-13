// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cluster_provider/cluster_provider.hpp"
#include "sblr_admission.hpp"
#include "sblr_dispatch.hpp"
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
  context.transaction_uuid.canonical = "019f0000-0000-7000-8000-00000000f601";
  context.local_transaction_id = 6606;
  context.database_uuid.canonical = "fse-p6-result-vector-diagnostic-database";
  context.session_uuid.canonical = "fse-p6-result-vector-diagnostic-session";
  context.principal_uuid.canonical = "fse-p6-result-vector-diagnostic-principal";
  context.trace_tags.push_back("sblr_surface_fse_p6_result_vector_diagnostic_gate");
  return context;
}

sblr::SblrOperationEnvelope EnvelopeForRow(const Row& row) {
  auto envelope = sblr::MakeSblrEnvelope(Field(row, "operation_id"),
                                         "SBLR_FSE_P6_RESULT_VECTOR_DIAGNOSTIC",
                                         "FSE-P6-result-vector-diagnostic-proof");
  envelope.source_artifact_map.policy_status = "non_authoritative_render_metadata";
  envelope.source_artifact_map.source_identity =
      "fse-p6:" + Field(row, "source_import_id");
  envelope.source_artifact_map.source_hash = "sha256:fse-p6-result-vector-proof";
  envelope.source_artifact_map.render_metadata_only = true;
  envelope.source_artifact_map.contains_sql_text = false;
  envelope.source_artifact_map.raw_sql_text_authoritative = false;
  envelope.requires_cluster_authority = Field(row, "source_type") == "cluster_normalization";
  envelope.requires_transaction_context =
      Field(row, "authority_class") == "MGA_TRANSACTION_FINALITY";
  return envelope;
}

cluster_provider::ClusterProviderRequest ProviderRequest(std::string operation_id) {
  cluster_provider::ClusterProviderRequest request;
  request.context = Context();
  request.envelope.operation_id = std::move(operation_id);
  request.envelope.opcode = "SBLR_FSE_P6_CLUSTER_PROVIDER_DIAGNOSTIC";
  request.envelope.trace_key = "fse-p6-cluster-provider-diagnostic";
  request.envelope.requires_security_context = true;
  request.envelope.requires_cluster_authority = true;
  request.envelope.contains_sql_text = false;
  request.api_request.context = request.context;
  request.api_request.operation_id = request.envelope.operation_id;
  return request;
}

void VerifyCoverage(const std::vector<Row>& register_rows,
                    const std::vector<Row>& route_rows,
                    const std::vector<Row>& policy_rows,
                    const std::vector<Row>& diagnostic_rows) {
  Require(register_rows.size() == 2760, "operation register row count drift");
  Require(route_rows.size() == 2701, "P4 route matrix row count drift");
  Require(policy_rows.size() == 439, "P5 policy matrix row count drift");
  Require(diagnostic_rows.size() == 2760, "P6 diagnostic matrix row count drift");
  const auto route_by_id = IndexBySourceImportId(route_rows, "P4 route matrix");
  const auto policy_by_id = IndexBySourceImportId(policy_rows, "P5 policy matrix");
  const auto diagnostic_by_id =
      IndexBySourceImportId(diagnostic_rows, "P6 diagnostic matrix");

  int cluster_rows = 0;
  int external_rows = 0;
  for (const auto& row : register_rows) {
    const auto& source_import_id = Field(row, "source_import_id");
    Require(diagnostic_by_id.contains(source_import_id),
            "P6 diagnostic matrix missing " + source_import_id);
    const auto& diagnostic = diagnostic_by_id.at(source_import_id);
    Require(Field(diagnostic, "operation_id") == Field(row, "operation_id"),
            "P6 operation mismatch " + source_import_id);
    Require(Field(diagnostic, "source_search_key") == Field(row, "source_search_key"),
            "P6 source key mismatch " + source_import_id);
    const bool cluster = Field(row, "source_type") == "cluster_normalization";
    const bool external =
        Field(row, "implementation_expectation") == "external_authority_fail_closed";
    if (cluster) {
      ++cluster_rows;
      Require(policy_by_id.contains(source_import_id),
              "cluster row missing P5 policy " + source_import_id);
      Require(!route_by_id.contains(source_import_id),
              "cluster row leaked into P4 route " + source_import_id);
    } else {
      Require(route_by_id.contains(source_import_id),
              "non-cluster row missing P4 route " + source_import_id);
    }
    if (external) {
      ++external_rows;
      Require(policy_by_id.contains(source_import_id),
              "external row missing P5 policy " + source_import_id);
    }
  }
  Require(cluster_rows == 59, "cluster row count drift");
  Require(external_rows == 380, "external authority row count drift");
}

void VerifyDiagnosticPolicyRows(const std::vector<Row>& diagnostic_rows) {
  int executable_rows = 0;
  int exact_rows = 0;
  int external_rows = 0;
  int cluster_rows = 0;
  int cluster_query_rows = 0;
  int observability_rows = 0;
  int mga_rows = 0;
  for (const auto& row : diagnostic_rows) {
    const auto& source_import_id = Field(row, "source_import_id");
    const auto& impl = Field(row, "implementation_expectation");
    const auto& source_type = Field(row, "source_type");
    Require(Contains(Field(row, "redaction_policy"), "redact") ||
                Contains(Field(row, "redaction_policy"), "redaction"),
            "redaction policy missing " + source_import_id);
    const auto& leakage = Field(row, "secret_leakage_policy");
    Require(Contains(leakage, "forbid") && Contains(leakage, "secret") &&
                Contains(leakage, "local_path") && Contains(leakage, "raw_sql") &&
                Contains(leakage, "provider_private"),
            "secret leakage policy incomplete " + source_import_id);

    if (impl == "executable_engine_api") {
      ++executable_rows;
      Require(Contains(Field(row, "success_vector"), "engine") &&
                  Contains(Field(row, "failure_vector"), "stable"),
              "engine API result vector incomplete " + source_import_id);
    }
    if (impl == "architecture_refusal" || impl == "exact_refusal" ||
        Field(row, "authority_class") == "EXACT_POLICY_REFUSAL") {
      ++exact_rows;
      Require(Contains(Field(row, "success_vector"), "no_success_vector"),
              "exact/refusal success vector drift " + source_import_id);
      Require(Contains(Field(row, "failure_vector"), "empty_rows") &&
                  Contains(Field(row, "message_vector_code_policy"), "REFUSAL"),
              "exact/refusal failure vector drift " + source_import_id);
      Require(Contains(Field(row, "row_mutation_policy"), "no_mutable_rows"),
              "exact/refusal row mutation drift " + source_import_id);
    }
    if (impl == "external_authority_fail_closed") {
      ++external_rows;
      Require(Contains(Field(row, "failure_vector"), "fail_closed") &&
                  Contains(Field(row, "result_shape"), "empty_rows") &&
                  Contains(Field(row, "support_bundle_fields"),
                           "redacted_provider_descriptor"),
              "external authority diagnostic policy drift " + source_import_id);
    }
    if (source_type == "cluster_normalization") {
      ++cluster_rows;
      Require(Contains(Field(row, "failure_vector"), "unsupported") ||
                  Contains(Field(row, "failure_vector"), "exact_pre_provider_refusal"),
              "cluster failure vector drift " + source_import_id);
      Require(Contains(Field(row, "message_vector_code_policy"), "kCluster") ||
                  Contains(Field(row, "message_vector_code_policy"), "EXACT_PRE_PROVIDER"),
              "cluster message vector drift " + source_import_id);
      if (StartsWith(Field(row, "operation_id"), "cluster.query.")) {
        ++cluster_query_rows;
        Require(Field(row, "sblr_family") == "sblr.cluster.query.v1",
                "cluster query family drift " + source_import_id);
        Require(Contains(Field(row, "notes"),
                         "cross_node_distributed_cluster_authority_not_local_query"),
                "cluster query authority marker missing " + source_import_id);
      }
    }
    if (impl == "diagnostic_or_observability" ||
        Field(row, "authority_class") == "OBSERVABILITY_DIAGNOSTICS") {
      ++observability_rows;
      Require(Contains(Field(row, "support_bundle_fields"), "support") ||
                  Contains(Field(row, "support_bundle_fields"), "diagnostic"),
              "observability support fields missing " + source_import_id);
    }
    if (Field(row, "authority_class") == "MGA_TRANSACTION_FINALITY") {
      ++mga_rows;
      Require(Contains(Field(row, "message_vector_code_policy"), "ENGINE_AUTHORITY") ||
                  Contains(Field(row, "success_vector"), "engine_mga"),
              "MGA message policy missing engine authority " + source_import_id);
      Require(Contains(Field(row, "row_mutation_policy"), "parser_reference_provider") ||
                  Contains(Field(row, "notes"), "engine-owned"),
              "MGA finality ownership drift " + source_import_id);
    }
  }
  Require(executable_rows == 1438, "executable result row count drift");
  Require(exact_rows >= 396, "exact/refusal result rows missing");
  Require(external_rows == 380, "external authority result row count drift");
  Require(cluster_rows == 59, "cluster result row count drift");
  Require(cluster_query_rows == 9, "cluster query result row count drift");
  Require(observability_rows >= 495, "observability result rows missing");
  Require(mga_rows == 124, "MGA result row count drift");
}

void VerifyCompiledSblrSamples(const std::vector<Row>& diagnostic_rows) {
  std::set<std::string> sampled_impls;
  std::set<std::string> sampled_families;
  for (const auto& row : diagnostic_rows) {
    const bool sample_impl = sampled_impls.insert(Field(row, "implementation_expectation")).second;
    const bool sample_family = sampled_families.insert(Field(row, "sblr_family")).second;
    const bool sample_query = StartsWith(Field(row, "operation_id"), "cluster.query.");
    if (!sample_impl && !sample_family && !sample_query) continue;

    const auto envelope = EnvelopeForRow(row);
    const auto encoded = sblr::EncodeSblrEnvelope(envelope);
    const auto decoded = sblr::DecodeSblrEnvelope(encoded);
    Require(decoded.ok, "SBLR decode failed " + Field(row, "source_import_id"));
    const auto validation = sblr::ValidateSblrEnvelope(decoded.envelope);
    Require(validation.ok, "SBLR validation failed " + Field(row, "source_import_id"));
    const auto admission = server::AdmitServerSblrEnvelope(
        server::ServerSblrAdmissionRequest{encoded, envelope.requires_cluster_authority});
    if (Field(row, "implementation_expectation") == "external_authority_fail_closed" ||
        Field(row, "implementation_expectation") == "architecture_refusal" ||
        Field(row, "implementation_expectation") == "exact_refusal") {
      Require(!admission.admitted,
              "fail-closed/refusal row admitted " + Field(row, "source_import_id"));
    }
    if (Field(row, "authority_class") == "MGA_TRANSACTION_FINALITY") {
      Require(envelope.requires_transaction_context,
              "MGA/finality sample omitted transaction context requirement");
      if (Field(row, "source_type") != "cluster_normalization") {
        Require(!envelope.requires_cluster_authority,
                "non-cluster MGA/finality sample requested cluster authority");
      }
    }
  }

  const auto raw_sql = server::AdmitServerSblrEnvelope(
      server::ServerSblrAdmissionRequest{"select * from fse_p6_forbidden", false});
  Require(!raw_sql.admitted && HasAdmissionDiagnostic(raw_sql, "SBLR.SQL_TEXT_FORBIDDEN"),
          "raw SQL was accepted as SBLR");

  auto local_query = sblr::MakeSblrEnvelope("query.plan_operation",
                                            "SBLR_QUERY_PLAN_OPERATION",
                                            "FSE-P6-local-query-cluster-refusal");
  local_query.requires_cluster_authority = true;
  local_query.source_artifact_map.policy_status = "non_authoritative_render_metadata";
  local_query.source_artifact_map.source_identity = "fse-p6:local-query-refusal";
  local_query.source_artifact_map.source_hash = "sha256:fse-p6-local-query-refusal";
  local_query.source_artifact_map.contains_sql_text = false;
  local_query.source_artifact_map.raw_sql_text_authoritative = false;
  const auto local_query_admission = server::AdmitServerSblrEnvelope(
      server::ServerSblrAdmissionRequest{sblr::EncodeSblrEnvelope(local_query), true});
  Require(!local_query_admission.admitted &&
              HasAdmissionDiagnostic(local_query_admission,
                                     "SBLR.CLUSTER_MAPPING.UNAVAILABLE"),
          "local query operation admitted as cluster query authority");
}

void VerifyClusterProviderDiagnostics() {
  const auto info = cluster_provider::DescribeClusterProvider();
  const bool no_cluster = info.provider_type == "no_cluster";
  const bool compile_link_stub = info.provider_type == "compile_link_stub";
  Require(no_cluster || compile_link_stub,
          "P6 public gate expects no-cluster or compile-link-stub provider");
  Require(!info.supports_execution, "public cluster provider unexpectedly executes");
  Require(!info.supports_route_admission,
          "public cluster provider unexpectedly admits routes");
  Require(!info.local_runtime_execution_enabled,
          "public cluster provider enables local runtime execution");
  Require(!info.mutable_by_local_core,
          "public cluster provider enables local mutation");

  for (const auto operation_id :
       cluster_provider::RequiredClusterProviderOperationSet()) {
    const auto result =
        cluster_provider::ExecuteClusterOperation(ProviderRequest(std::string(operation_id)));
    Require(!result.ok, "public provider executed cluster operation");
    Require(result.cluster_authority_required,
            "cluster authority flag missing from fail-closed result");
    Require(result.result_shape.rows.empty(),
            "public provider returned mutable result rows");
    Require(HasEvidence(result, "cluster_provider_type", info.provider_type),
            "provider type evidence missing");
    Require(HasEvidence(result,
                        "cluster_provider_route_admission",
                        "false"),
            "route admission fail-closed evidence missing");
    if (no_cluster) {
      Require(HasApiDiagnostic(result, cluster_provider::kClusterSupportNotEnabledCode),
              "no-cluster diagnostic code missing");
      Require(HasUnsupportedFeature(result, "cluster.provider"),
              "no-cluster unsupported feature missing");
    } else {
      Require(HasApiDiagnostic(result,
                               cluster_provider::kClusterHandshakeStubCompileLinkOnlyCode),
              "compile-link stub diagnostic code missing");
      Require(HasUnsupportedFeature(result, "cluster.provider.stub"),
              "compile-link stub unsupported feature missing");
    }
  }
  for (const auto refusal :
       cluster_provider::RequiredClusterProviderPreAdmissionRefusalSet()) {
    Require(!cluster_provider::ContainsProviderToken(
                cluster_provider::RequiredClusterProviderOperationSet(),
                refusal),
            "exact pre-provider refusal leaked into routed operation set");
  }
}

}  // namespace

int main(int argc, char** argv) {
  Require(argc == 2,
          "usage: sblr_surface_fse_p6_result_vector_diagnostic_gate "
          "<repo-root-or-sblr-execution_plan-dir>");
  const std::string execution_plan_dir = ResolveSblrExpansionRoot(argv[1]);
  const std::string register_path =
      execution_plan_dir + "/SBLR_OPERATION_EXPANSION_REGISTER.csv";
  if (!FileExists(register_path)) return SkipMissingExecutionPlan(register_path);
  const auto register_rows =
      LoadCsv(register_path);
  const auto route_rows =
      LoadCsv(execution_plan_dir + "/ENGINE_INTERNAL_API_ROUTE_MATRIX.csv");
  const auto policy_rows =
      LoadCsv(execution_plan_dir + "/SBLR_CLUSTER_AND_EXTERNAL_AUTHORITY_POLICY_MATRIX.csv");
  const auto diagnostic_rows =
      LoadCsv(execution_plan_dir + "/SBLR_RESULT_VECTOR_AND_DIAGNOSTIC_MATRIX.csv");
  RequireColumns(
      diagnostic_rows,
      {"diagnostic_id",
       "operation_id",
       "source_import_id",
       "source_search_key",
       "source_type",
       "authority_class",
       "implementation_expectation",
       "sblr_family",
       "success_vector",
       "failure_vector",
       "message_vector_code_policy",
       "redaction_policy",
       "support_bundle_fields",
       "result_shape",
       "row_mutation_policy",
       "secret_leakage_policy",
       "operator_action_policy",
       "proof_test_name",
       "proof_file",
       "status",
       "notes"},
      "P6 result vector diagnostic matrix");
  VerifyCoverage(register_rows, route_rows, policy_rows, diagnostic_rows);
  VerifyDiagnosticPolicyRows(diagnostic_rows);
  VerifyCompiledSblrSamples(diagnostic_rows);
  VerifyClusterProviderDiagnostics();
  std::cout << "sblr_surface_fse_p6_result_vector_diagnostic_gate=passed\n";
  return EXIT_SUCCESS;
}
