// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cluster_provider/cluster_provider.hpp"

#include <algorithm>
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

std::string LoadText(const std::string& path) {
  std::ifstream input(path);
  Require(input.good(), "could not open source file: " + path);
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
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

std::map<std::string, Row> IndexByOperationId(const std::vector<Row>& rows,
                                              const std::string& label) {
  std::map<std::string, Row> out;
  for (const auto& row : rows) {
    const auto& operation_id = Field(row, "operation_id");
    Require(out.emplace(operation_id, row).second,
            label + " duplicate operation_id " + operation_id);
  }
  return out;
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
  context.database_uuid.canonical = "fse-p5-cluster-external-policy-database";
  context.session_uuid.canonical = "fse-p5-cluster-external-policy-session";
  context.principal_uuid.canonical = "fse-p5-cluster-external-policy-principal";
  context.trace_tags.push_back("sblr_surface_fse_p5_cluster_external_authority_policy_gate");
  return context;
}

cluster_provider::ClusterProviderRequest ProviderRequest(std::string operation_id) {
  cluster_provider::ClusterProviderRequest request;
  request.context = Context();
  request.envelope.operation_id = std::move(operation_id);
  request.envelope.opcode = "SBLR_FSE_P5_CLUSTER_EXTERNAL_AUTHORITY_POLICY";
  request.envelope.trace_key = "fse-p5-cluster-external-authority-policy";
  request.envelope.requires_security_context = true;
  request.envelope.requires_cluster_authority = true;
  request.envelope.contains_sql_text = false;
  request.api_request.context = request.context;
  request.api_request.operation_id = request.envelope.operation_id;
  return request;
}

cluster_provider::ClusterProviderInfo ValidExternalProvider() {
  cluster_provider::ClusterProviderInfo info;
  info.provider_name = "scratchbird.cluster.external_provider.fse_p5_contract_test";
  info.provider_type = "external_cluster_provider";
  info.provider_version = "1.0.0-test";
  info.support_status = "enabled";
  info.provider_abi_version = cluster_provider::kClusterProviderAbiVersionCurrent;
  info.provider_contract_id = cluster_provider::kClusterProviderContractId;
  info.catalog_manifest_id = cluster_provider::kClusterProviderCatalogManifestId;
  info.catalog_manifest_version =
      cluster_provider::kClusterProviderCatalogManifestVersionCurrent;
  info.catalog_record_codec_version =
      cluster_provider::kClusterProviderCatalogRecordCodecVersionCurrent;
  info.catalog_compatibility_digest =
      cluster_provider::kClusterProviderCatalogCompatibilityDigest;
  info.operation_ids = cluster_provider::RequiredClusterProviderOperationSet();
  info.feature_flags = cluster_provider::RequiredClusterProviderFeatureFlags();
  info.authority_domains = cluster_provider::RequiredClusterProviderAuthorityDomains();
  info.external_provider = true;
  info.compile_link_only = false;
  info.supports_execution = true;
  info.supports_route_admission = true;
  info.local_runtime_execution_enabled = false;
  info.mutable_by_local_core = false;
  return info;
}

void VerifyMatrixCoverage(const std::vector<Row>& register_rows,
                          const std::vector<Row>& binary_rows,
                          const std::vector<Row>& route_rows,
                          const std::vector<Row>& policy_rows) {
  Require(register_rows.size() == 2760, "operation register row count drift");
  Require(binary_rows.size() == 2760, "binary matrix row count drift");
  Require(route_rows.size() == 2701, "P4 route matrix row count drift");
  Require(policy_rows.size() == 439, "P5 policy matrix row count drift");

  const auto binary_by_id = IndexBySourceImportId(binary_rows, "binary matrix");
  const auto route_by_id = IndexBySourceImportId(route_rows, "P4 route matrix");
  const auto policy_by_id = IndexBySourceImportId(policy_rows, "P5 policy matrix");

  int cluster_rows = 0;
  int external_rows = 0;
  for (const auto& row : register_rows) {
    const auto& source_import_id = Field(row, "source_import_id");
    Require(binary_by_id.contains(source_import_id),
            "binary matrix missing " + source_import_id);
    const bool cluster = Field(row, "source_type") == "cluster_normalization";
    const bool external =
        Field(row, "implementation_expectation") == "external_authority_fail_closed";
    if (cluster) {
      ++cluster_rows;
      Require(policy_by_id.contains(source_import_id),
              "P5 policy matrix missing cluster row " + source_import_id);
      Require(!route_by_id.contains(source_import_id),
              "cluster row leaked into P4 route matrix " + source_import_id);
      continue;
    }
    if (external) {
      ++external_rows;
      Require(policy_by_id.contains(source_import_id),
              "P5 policy matrix missing external row " + source_import_id);
      Require(route_by_id.contains(source_import_id),
              "external row missing P4 fail-closed route " + source_import_id);
      continue;
    }
    Require(!policy_by_id.contains(source_import_id),
            "non-P5 row leaked into policy matrix " + source_import_id);
  }
  Require(cluster_rows == 59, "P5 cluster row count drift");
  Require(external_rows == 380, "P5 external authority row count drift");

  for (const auto& row : policy_rows) {
    const auto& source_import_id = Field(row, "source_import_id");
    const auto& reg = register_rows.at(0);
    (void)reg;
    Require(binary_by_id.contains(source_import_id),
            "P5 row not represented in binary matrix " + source_import_id);
  }
}

void VerifyPolicyRows(const std::vector<Row>& policy_rows) {
  int cluster_rows = 0;
  int external_rows = 0;
  int query_rows = 0;
  int exact_refusal_rows = 0;
  for (const auto& row : policy_rows) {
    const auto& source_import_id = Field(row, "source_import_id");
    const auto& operation_id = Field(row, "operation_id");
    Require(Contains(Field(row, "parser_authority_policy"), "forbidden"),
            "parser authority not forbidden " + source_import_id);
    Require(Contains(Field(row, "reference_authority_policy"), "forbidden"),
            "reference authority not forbidden " + source_import_id);

    if (Field(row, "source_type") == "cluster_normalization") {
      ++cluster_rows;
      Require(Contains(Field(row, "private_provider_behavior"), "private_provider"),
              "private provider boundary missing " + source_import_id);
      Require(Contains(Field(row, "local_engine_execution_policy"), "no_local") ||
                  Contains(Field(row, "local_engine_execution_policy"), "refusal_only"),
              "local cluster execution policy drift " + source_import_id);
      Require(Contains(Field(row, "notes"), "MGA") &&
                  Contains(Field(row, "notes"), "engine-owned"),
              "MGA/finality ownership note missing " + source_import_id);
      if (StartsWith(operation_id, "cluster.query.")) {
        ++query_rows;
        Require(Field(row, "sblr_family") == "sblr.cluster.query.v1",
                "cluster query row used wrong SBLR family " + source_import_id);
        Require(Field(row, "cluster_query_authority_marker") ==
                    "cross_node_distributed_cluster_authority_not_local_query",
                "cluster query authority marker drift " + source_import_id);
      } else {
        Require(Field(row, "cluster_query_authority_marker") == "not_cluster_query",
                "non-query row marked as cluster query " + source_import_id);
      }
      if (Field(row, "implementation_expectation") == "exact_refusal") {
        ++exact_refusal_rows;
        Require(Contains(Field(row, "exact_refusal_policy"),
                         "exact_pre_provider_refusal_no_provider_call"),
                "exact refusal policy drift " + source_import_id);
        Require(Contains(Field(row, "provider_route"), "excluded"),
                "exact refusal leaked to provider route " + source_import_id);
        Require(Contains(Field(row, "compile_flag_policy"), "refuse_before_provider_admission"),
                "exact refusal compile policy drift " + source_import_id);
      } else {
        Require(Contains(Field(row, "compile_flag_policy"), "SB_ENABLE_CLUSTER_PROVIDER_OFF"),
                "cluster disabled compile policy missing " + source_import_id);
        Require(Contains(Field(row, "disabled_behavior"), "functionality_unsupported"),
                "cluster disabled behavior drift " + source_import_id);
        Require(Contains(Field(row, "public_stub_behavior"), "functionality_unlicensed"),
                "cluster public stub behavior drift " + source_import_id);
      }
      continue;
    }

    ++external_rows;
    Require(Field(row, "implementation_expectation") == "external_authority_fail_closed",
            "non-external row in external policy section " + source_import_id);
    Require(Contains(Field(row, "authority_boundary"), "external_authority"),
            "external authority boundary missing " + source_import_id);
    Require(Contains(Field(row, "disabled_behavior"), "fail_closed") &&
                Contains(Field(row, "disabled_behavior"), "no_local"),
            "external disabled behavior is not fail-closed " + source_import_id);
    Require(Contains(Field(row, "provider_route"), "redacted_provider_descriptor"),
            "external redacted provider descriptor missing " + source_import_id);
    Require(Contains(Field(row, "local_engine_execution_policy"), "no_local_direct"),
            "external local execution policy drift " + source_import_id);
    Require(Field(row, "cluster_query_authority_marker") == "not_cluster_query",
            "external row marked as cluster query " + source_import_id);
  }
  Require(cluster_rows == 59, "P5 cluster policy count drift");
  Require(external_rows == 380, "P5 external policy count drift");
  Require(query_rows == 9, "P5 cluster query policy count drift");
  Require(exact_refusal_rows == 3, "P5 exact cluster refusal count drift");
}

void VerifyClusterProviderBoundary(const std::vector<Row>& policy_rows) {
  Require(cluster_provider::RequiredClusterProviderCommandBoundarySet().size() == 59,
          "cluster provider command boundary size drift");
  Require(cluster_provider::RequiredClusterProviderPreAdmissionRefusalSet().size() == 3,
          "cluster provider exact refusal set size drift");

  std::set<std::string_view> normalized_commands;
  std::set<std::string_view> expected_operation_ids;
  int routed_count = 0;
  int refusal_count = 0;
  for (const auto& command :
       cluster_provider::RequiredClusterProviderCommandBoundarySet()) {
    Require(normalized_commands.insert(command.normalized_command).second,
            "duplicate normalized cluster command boundary entry");
    if (command.provider_routed) {
      ++routed_count;
      Require(command.provider_operation_id != "exact_refusal_no_provider_call",
              "provider-routed command used exact refusal provider id");
      expected_operation_ids.insert(command.normalized_command);
      expected_operation_ids.insert(command.provider_operation_id);
      Require(cluster_provider::ContainsProviderToken(
                  cluster_provider::RequiredClusterProviderOperationSet(),
                  command.normalized_command),
              "normalized command omitted from provider operation set");
      Require(cluster_provider::ContainsProviderToken(
                  cluster_provider::RequiredClusterProviderOperationSet(),
                  command.provider_operation_id),
              "provider alias omitted from provider operation set");
    } else {
      ++refusal_count;
      Require(command.provider_operation_id == "exact_refusal_no_provider_call",
              "exact refusal mapped to provider operation");
      Require(cluster_provider::ContainsProviderToken(
                  cluster_provider::RequiredClusterProviderPreAdmissionRefusalSet(),
                  command.normalized_command),
              "exact refusal omitted from pre-admission set");
      Require(!cluster_provider::ContainsProviderToken(
                  cluster_provider::RequiredClusterProviderOperationSet(),
                  command.normalized_command),
              "exact refusal leaked into provider operation set");
    }
  }
  Require(routed_count == 56, "provider routed command count drift");
  Require(refusal_count == 3, "provider exact refusal count drift");
  Require(cluster_provider::RequiredClusterProviderOperationSet().size() ==
              expected_operation_ids.size(),
          "operation set is not derived from normalized commands plus aliases");
  Require(!cluster_provider::ContainsProviderToken(
              cluster_provider::RequiredClusterProviderOperationSet(),
              "query.plan_operation"),
          "local query operation leaked into cluster provider route set");

  const auto valid = ValidExternalProvider();
  Require(cluster_provider::ValidateClusterProviderHandshake(valid).ok,
          "valid external provider handshake did not admit");
  for (const auto operation_id :
       cluster_provider::RequiredClusterProviderOperationSet()) {
    const auto admission =
        cluster_provider::EvaluateClusterProviderRouteAdmission(valid, operation_id);
    Require(admission.ok && admission.route_admitted,
            "valid external provider did not admit provider operation");
  }
  for (const auto refusal :
       cluster_provider::RequiredClusterProviderPreAdmissionRefusalSet()) {
    const auto admission =
        cluster_provider::EvaluateClusterProviderRouteAdmission(valid, refusal);
    Require(!admission.ok && !admission.route_admitted,
            "exact refusal was admitted to provider route");
    Require(admission.diagnostic_code ==
                cluster_provider::kClusterRouteAdmissionUnsupportedOperationCode,
            "exact refusal used wrong admission diagnostic");
  }
  const auto local_query =
      cluster_provider::EvaluateClusterProviderRouteAdmission(valid, "query.plan_operation");
  Require(!local_query.ok && !local_query.route_admitted,
          "local query operation admitted as cluster query authority");

  const auto info = cluster_provider::DescribeClusterProvider();
  const bool no_cluster = info.provider_type == "no_cluster";
  const bool compile_link_stub = info.provider_type == "compile_link_stub";
  Require(no_cluster || compile_link_stub,
          "P5 public gate expects a public no-cluster or compile-link-stub provider");
  Require(!info.supports_execution, "public provider unexpectedly supports execution");
  Require(!info.supports_route_admission,
          "public provider unexpectedly supports route admission");
  Require(!info.local_runtime_execution_enabled,
          "public provider enabled local runtime execution");
  Require(!info.mutable_by_local_core, "public provider enabled local mutation");

  for (const auto operation_id :
       cluster_provider::RequiredClusterProviderOperationSet()) {
    const auto result =
        cluster_provider::ExecuteClusterOperation(ProviderRequest(std::string(operation_id)));
    Require(!result.ok, "public provider executed cluster operation");
    Require(result.cluster_authority_required,
            "cluster authority flag missing from provider failure");
    Require(result.result_shape.rows.empty(),
            "public provider returned mutable result rows for cluster operation");
    Require(HasEvidence(result, "cluster_provider_type", info.provider_type),
            "provider type evidence missing");
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
  }

  const auto policy_by_op = IndexByOperationId(policy_rows, "P5 policy matrix");
  for (const auto& command :
       cluster_provider::RequiredClusterProviderCommandBoundarySet()) {
    Require(policy_by_op.contains(std::string(command.normalized_command)),
            "normalized cluster command missing from P5 policy matrix");
  }
}

void VerifyPublicSourceBoundaryText(const std::string& repo_root) {
  const auto header =
      LoadText(repo_root + "/project/src/cluster_provider/cluster_provider.hpp");
  const auto no_cluster =
      LoadText(repo_root + "/project/src/cluster_provider/no_cluster_provider.cpp");
  const auto stub =
      LoadText(repo_root + "/project/src/cluster_provider_stub/stub_cluster_provider.cpp");
  Require(Contains(header, "external provider target"),
          "cluster provider header lost external-provider boundary text");
  Require(Contains(stub, "contains no cluster implementation code"),
          "public stub lost no-implementation boundary text");
  for (const auto* source : {&no_cluster, &stub}) {
    Require(!Contains(*source, "supports_execution = true"),
            "public provider source claims execution support");
    Require(!Contains(*source, "supports_route_admission = true"),
            "public provider source claims route admission support");
    Require(!Contains(*source, "local_runtime_execution_enabled = true"),
            "public provider source enables local cluster runtime");
    Require(!Contains(*source, "mutable_by_local_core = true"),
            "public provider source enables local cluster mutation");
    Require(!Contains(*source, "cluster membership implementation"),
            "public provider source claims membership implementation");
    Require(!Contains(*source, "distributed query execution implementation"),
            "public provider source claims distributed query implementation");
  }
}

}  // namespace

int main(int argc, char** argv) {
  Require(argc == 2 || argc == 3,
          "usage: sblr_surface_fse_p5_cluster_external_authority_policy_gate "
          "<repo-root-or-sblr-execution_plan-dir> [repo-root]");
  const std::string first_arg = argv[1];
  const std::string execution_plan_dir = ResolveSblrExpansionRoot(first_arg);
  const std::string repo_root = argc == 3 ? argv[2] : first_arg;
  const std::string register_path =
      execution_plan_dir + "/SBLR_OPERATION_EXPANSION_REGISTER.csv";
  if (!FileExists(register_path)) return SkipMissingExecutionPlan(register_path);
  const auto register_rows =
      LoadCsv(register_path);
  const auto binary_rows =
      LoadCsv(execution_plan_dir + "/SBLR_BINARY_FORMAT_EXTENSION_MATRIX.csv");
  const auto route_rows =
      LoadCsv(execution_plan_dir + "/ENGINE_INTERNAL_API_ROUTE_MATRIX.csv");
  const auto policy_rows =
      LoadCsv(execution_plan_dir + "/SBLR_CLUSTER_AND_EXTERNAL_AUTHORITY_POLICY_MATRIX.csv");
  RequireColumns(
      policy_rows,
      {"policy_id",
       "operation_id",
       "source_import_id",
       "source_search_key",
       "source_type",
       "authority_class",
       "implementation_expectation",
       "sblr_family",
       "authority_boundary",
       "compile_flag_policy",
       "provider_route",
       "disabled_behavior",
       "public_stub_behavior",
       "private_provider_behavior",
       "exact_refusal_policy",
       "local_engine_execution_policy",
       "parser_authority_policy",
       "reference_authority_policy",
       "cluster_query_authority_marker",
       "message_vector_policy",
       "proof_test_name",
       "proof_file",
       "status",
       "notes"},
      "P5 policy matrix");

  VerifyMatrixCoverage(register_rows, binary_rows, route_rows, policy_rows);
  VerifyPolicyRows(policy_rows);
  VerifyClusterProviderBoundary(policy_rows);
  VerifyPublicSourceBoundaryText(repo_root);
  return EXIT_SUCCESS;
}
