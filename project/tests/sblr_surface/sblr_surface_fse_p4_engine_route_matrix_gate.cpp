// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_admission.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"

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
namespace sblr = scratchbird::engine::sblr;
namespace server = scratchbird::server;

using Row = std::map<std::string, std::string>;
constexpr int kSkipReturnCode = 77;

std::string ResolveSblrExpansionRoot(const std::string& input) {
  if (input.find("sblr_sbsql_expansion_public_evidence") != std::string::npos) {
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

bool HasDiagnostic(const server::ServerSblrAdmissionResult& result,
                   std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

api::EngineRequestContext Context() {
  api::EngineRequestContext context;
  context.security_context_present = true;
  context.transaction_uuid.canonical = "019f0000-0000-7000-8000-00000000f401";
  context.local_transaction_id = 4404;
  context.database_uuid.canonical = "019f0000-0000-7000-8000-00000000f402";
  context.session_uuid.canonical = "019f0000-0000-7000-8000-00000000f403";
  return context;
}

sblr::SblrOperationEnvelope EnvelopeForRoute(const Row& route) {
  auto envelope = sblr::MakeSblrEnvelope(Field(route, "operation_id"),
                                         "SBLR_FSE_P4_ROUTE_PROOF",
                                         "FSE-P4-engine-route-proof");
  envelope.source_artifact_map.policy_status = "non_authoritative_render_metadata";
  envelope.source_artifact_map.source_identity =
      "fse-p4:" + Field(route, "source_import_id");
  envelope.source_artifact_map.source_hash = "sha256:fse-p4-route-proof";
  envelope.source_artifact_map.render_metadata_only = true;
  envelope.source_artifact_map.contains_sql_text = false;
  envelope.source_artifact_map.raw_sql_text_authoritative = false;
  envelope.requires_transaction_context =
      Field(route, "authority_class") == "MGA_TRANSACTION_FINALITY";
  envelope.requires_cluster_authority = false;
  return envelope;
}

void VerifyCoverage(const std::vector<Row>& register_rows,
                    const std::vector<Row>& route_rows) {
  Require(register_rows.size() == 2760, "operation register row count drift");
  Require(route_rows.size() == 2701, "P4 route row count drift");
  const auto route_by_id = IndexBySourceImportId(route_rows, "route matrix");
  int cluster_exclusions = 0;
  for (const auto& row : register_rows) {
    const auto& source_import_id = Field(row, "source_import_id");
    const bool cluster = Field(row, "source_type") == "cluster_normalization";
    if (cluster) {
      ++cluster_exclusions;
      Require(!route_by_id.contains(source_import_id),
              "cluster row was included in P4 route matrix " + source_import_id);
      continue;
    }
    Require(route_by_id.contains(source_import_id),
            "P4 route matrix missing " + source_import_id);
    const auto& route = route_by_id.at(source_import_id);
    Require(Field(route, "operation_id") == Field(row, "operation_id"),
            "P4 route operation mismatch " + source_import_id);
    Require(Field(route, "source_search_key") == Field(row, "source_search_key"),
            "P4 route source key mismatch " + source_import_id);
  }
  Require(cluster_exclusions == 59, "P4 cluster exclusion count drift");
}

void VerifyRoutePolicies(const std::vector<Row>& route_rows) {
  std::set<std::string> route_classes;
  int exact_rows = 0;
  int external_rows = 0;
  int mga_rows = 0;
  int binding_rows = 0;
  for (const auto& route : route_rows) {
    const auto& source_import_id = Field(route, "source_import_id");
    Require(Field(route, "source_type") != "cluster_normalization",
            "cluster row present in P4 route matrix " + source_import_id);
    Require(Contains(Field(route, "parser_authority_policy"), "forbidden"),
            "parser authority not forbidden " + source_import_id);
    Require(Contains(Field(route, "reference_authority_policy"), "forbidden"),
            "reference authority not forbidden " + source_import_id);
    Require(Contains(Field(route, "mga_finality_policy"), "finality"),
            "MGA/finality policy missing " + source_import_id);
    route_classes.insert(Field(route, "engine_route_class"));

    if (Field(route, "engine_route_class") == "exact_policy_refusal") {
      ++exact_rows;
      Require(Contains(Field(route, "dispatch_policy"), "refuse_before_dispatch"),
              "exact refusal dispatch policy drift " + source_import_id);
      Require(Contains(Field(route, "refusal_policy"), "no_provider_call"),
              "exact refusal provider policy drift " + source_import_id);
      Require(Contains(Field(route, "engine_api_target"), "no_engine_mutation"),
              "exact refusal engine mutation policy drift " + source_import_id);
    }
    if (Field(route, "engine_route_class") == "external_authority_fail_closed_guard") {
      ++external_rows;
      const std::string policy = Field(route, "dispatch_policy") + ";" +
                                 Field(route, "refusal_policy") + ";" +
                                 Field(route, "engine_api_target");
      Require(Contains(policy, "fail") &&
                  Contains(policy, "no_local") &&
                  Contains(policy, "connector"),
              "external authority fail-closed policy drift " + source_import_id);
    }
    if (Field(route, "authority_class") == "MGA_TRANSACTION_FINALITY") {
      ++mga_rows;
      Require(Contains(Field(route, "mga_finality_policy"), "engine_mga_finality"),
              "MGA finality not engine-owned " + source_import_id);
      Require(Contains(Field(route, "mga_finality_policy"), "parser_reference_provider_finality_forbidden"),
              "MGA reference/provider finality policy drift " + source_import_id);
    }
    if (Field(route, "engine_route_class") == "engine_binding_resolution_guard") {
      ++binding_rows;
      const std::string policy = Field(route, "dispatch_policy") + ";" +
                                 Field(route, "refusal_policy");
      Require(Contains(policy, "fail_closed") ||
                  Contains(policy, "unsupported_or_unresolved_binding_refusal"),
              "binding resolution guard is not fail-closed " + source_import_id);
    }
    if (Field(route, "implementation_expectation") == "diagnostic_or_observability") {
      Require(Contains(Field(route, "result_shape"), "redacted"),
              "observability row lacks redacted result shape " + source_import_id);
    }
  }
  Require(route_classes.size() >= 8, "route class coverage unexpectedly small");
  Require(exact_rows > 0, "exact refusal routes missing");
  Require(external_rows > 0, "external authority routes missing");
  Require(mga_rows > 0, "MGA/finality routes missing");
  Require(binding_rows > 0, "binding guard routes missing");
}

void VerifyCompiledSamples(const std::vector<Row>& route_rows) {
  std::set<std::string> sampled_classes;
  for (const auto& route : route_rows) {
    if (sampled_classes.insert(Field(route, "engine_route_class")).second) {
      const auto envelope = EnvelopeForRoute(route);
      const auto encoded = sblr::EncodeSblrEnvelope(envelope);
      const auto decoded = sblr::DecodeSblrEnvelope(encoded);
      Require(decoded.ok, "sample envelope decode failed " + Field(route, "source_import_id"));
      const auto admission = server::AdmitServerSblrEnvelope(
          server::ServerSblrAdmissionRequest{encoded, false});
      if (Field(route, "engine_route_class") == "exact_policy_refusal" ||
          Field(route, "engine_route_class") == "engine_binding_resolution_guard" ||
          Field(route, "engine_route_class") == "external_authority_fail_closed_guard") {
        Require(!admission.admitted,
                "guarded route admitted before engine binding/admission " +
                    Field(route, "source_import_id"));
      }
      const auto dispatch = sblr::DecodeAndDispatchSblrOperation(encoded, Context());
      if (Field(route, "engine_route_class") == "exact_policy_refusal") {
        Require(!dispatch.accepted && !dispatch.dispatched_to_api,
                "exact refusal route dispatched " + Field(route, "source_import_id"));
      }
    }
  }

  for (const auto& route : route_rows) {
    if (Field(route, "engine_route_class") != "exact_policy_refusal" &&
        Field(route, "authority_class") != "MGA_TRANSACTION_FINALITY") {
      continue;
    }
    const auto envelope = EnvelopeForRoute(route);
    const auto encoded = sblr::EncodeSblrEnvelope(envelope);
    const auto dispatch = sblr::DecodeAndDispatchSblrOperation(encoded, Context());
    if (Field(route, "engine_route_class") == "exact_policy_refusal") {
      Require(!dispatch.accepted && !dispatch.dispatched_to_api,
              "exact refusal row dispatched " + Field(route, "source_import_id"));
    }
    if (Field(route, "authority_class") == "MGA_TRANSACTION_FINALITY") {
      Require(!envelope.requires_cluster_authority,
              "MGA/finality route requested cluster authority " +
                  Field(route, "source_import_id"));
    }
  }

  const auto raw_sql = server::AdmitServerSblrEnvelope(
      server::ServerSblrAdmissionRequest{"select * from p4_forbidden", false});
  Require(!raw_sql.admitted && HasDiagnostic(raw_sql, "SBLR.SQL_TEXT_FORBIDDEN"),
          "raw SQL was accepted as SBLR");

  auto local_query = sblr::MakeSblrEnvelope("query.plan_operation",
                                            "SBLR_QUERY_PLAN_OPERATION",
                                            "FSE-P4-local-query-cluster-refusal");
  local_query.requires_cluster_authority = true;
  local_query.source_artifact_map.policy_status = "non_authoritative_render_metadata";
  local_query.source_artifact_map.source_identity = "fse-p4:local-query-refusal";
  local_query.source_artifact_map.source_hash = "sha256:fse-p4-local-query-refusal";
  const auto local_query_admission = server::AdmitServerSblrEnvelope(
      server::ServerSblrAdmissionRequest{sblr::EncodeSblrEnvelope(local_query), true});
  Require(!local_query_admission.admitted &&
              HasDiagnostic(local_query_admission, "SBLR.CLUSTER_MAPPING.UNAVAILABLE"),
          "local query operation was admitted as cluster query authority");
}

}  // namespace

int main(int argc, char** argv) {
  Require(argc == 2,
          "usage: sblr_surface_fse_p4_engine_route_matrix_gate "
          "<repo-root-or-sblr-execution_plan-dir>");
  const std::string execution_plan_dir = ResolveSblrExpansionRoot(argv[1]);
  const std::string register_path =
      execution_plan_dir + "/SBLR_OPERATION_EXPANSION_REGISTER.csv";
  if (!FileExists(register_path)) return SkipMissingExecutionPlan(register_path);
  const auto register_rows =
      LoadCsv(register_path);
  const auto binary_rows =
      LoadCsv(execution_plan_dir + "/SBLR_BINARY_FORMAT_EXTENSION_MATRIX.csv");
  const auto decoder_rows =
      LoadCsv(execution_plan_dir + "/SBLR_DECODER_ENCODER_TEST_MATRIX.csv");
  const auto route_rows =
      LoadCsv(execution_plan_dir + "/ENGINE_INTERNAL_API_ROUTE_MATRIX.csv");
  Require(binary_rows.size() == 2760, "binary matrix row count drift");
  Require(decoder_rows.size() == 2760, "decoder encoder matrix row count drift");
  RequireColumns(route_rows,
                 {"route_id",
                  "operation_id",
                  "source_import_id",
                  "source_search_key",
                  "source_type",
                  "authority_class",
                  "implementation_expectation",
                  "sblr_family",
                  "engine_route_class",
                  "engine_api_target",
                  "catalog_or_storage_authority",
                  "transaction_policy",
                  "result_shape",
                  "dispatch_policy",
                  "refusal_policy",
                  "parser_authority_policy",
                  "reference_authority_policy",
                  "mga_finality_policy",
                  "proof_test_name",
                  "proof_file",
                  "status",
                  "notes"},
                 "engine internal API route matrix");
  VerifyCoverage(register_rows, route_rows);
  VerifyRoutePolicies(route_rows);
  VerifyCompiledSamples(route_rows);
  std::cout << "sblr_surface_fse_p4_engine_route_matrix_gate=passed\n";
  return EXIT_SUCCESS;
}
