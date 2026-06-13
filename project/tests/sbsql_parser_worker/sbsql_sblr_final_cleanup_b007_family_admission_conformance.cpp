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

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#ifndef SCRATCHBIRD_PROJECT_SOURCE_DIR
#define SCRATCHBIRD_PROJECT_SOURCE_DIR "."
#endif

namespace {

namespace api = scratchbird::engine::internal_api;
namespace cluster_provider = scratchbird::engine::cluster_provider;
namespace sblr = scratchbird::engine::sblr;

struct FamilyRow {
  std::string_view family;
  std::string_view operation_id;
  bool cluster_private;
};

constexpr std::array<FamilyRow, 47> kFamilies{{
    {"sblr.acceleration.gpu.v3", "acceleration.gpu.operation", false},
    {"sblr.acceleration.llvm.v3", "extensibility.compile_llvm_module", false},
    {"sblr.archive.operation.v3", "archive.operation", false},
    {"sblr.backup.operation.v3", "backup.operation", false},
    {"sblr.bulk.export.v3", "bulk.export", false},
    {"sblr.bulk.import.v3", "bulk.import", false},
    {"sblr.catalog.introspect.v3", "catalog.get_descriptor", false},
    {"sblr.cluster.control.v3", "cluster.control_cluster", true},
    {"sblr.cluster.report.v3", "cluster.inspect_state", true},
    {"sblr.cursor.operation.v3", "session.cursor_open", false},
    {"sblr.database.management.v3", "lifecycle.inspect_database", false},
    {"sblr.diagnostic.control.v3", "diagnostic.control", false},
    {"sblr.diagnostic.refusal.v3", "diagnostic.refusal", false},
    {"sblr.dml.delete.v3", "dml.delete_rows", false},
    {"sblr.dml.insert.v3", "dml.insert_rows", false},
    {"sblr.dml.merge.v3", "dml.merge_rows", false},
    {"sblr.dml.update.v3", "dml.update_rows", false},
    {"sblr.event.channel.v3", "event.channel.notify", false},
    {"sblr.event.delivery.v3", "event.delivery.poll", false},
    {"sblr.event.publication.v3", "event.publication.operation", false},
    {"sblr.event.subscription.v3", "event.subscription.list", false},
    {"sblr.filespace.management.v3", "storage.manage_operation", false},
    {"sblr.fulltext.execution.v3", "nosql.search_query", false},
    {"sblr.graph.execution.v3", "nosql.graph_query", false},
    {"sblr.index.maintenance.v3", "index.maintenance", false},
    {"sblr.management.control.v3", "management.control_runtime", false},
    {"sblr.management.report.v3", "management.inspect_runtime", false},
    {"sblr.metrics.read.v3", "observability.show_metrics", false},
    {"sblr.mga.control.v3", "transaction.set_characteristics", false},
    {"sblr.mga.report.v3", "observability.show_transactions", false},
    {"sblr.optimizer.plan.v3", "query.plan_operation", false},
    {"sblr.parser.operation.v3", "extensibility.register_parser_package", false},
    {"sblr.policy.operation.v3", "security.policy.show", false},
    {"sblr.query.document.v3", "nosql.document_find", false},
    {"sblr.query.graph.v3", "nosql.graph_query", false},
    {"sblr.query.kv.v3", "nosql.key_value_get", false},
    {"sblr.query.search.v3", "nosql.search_query", false},
    {"sblr.query.timeseries.v3", "nosql.time_series_append", false},
    {"sblr.query.vector.v3", "nosql.vector_search", false},
    {"sblr.replication.consumer.v3", "cluster.inspect_replication", true},
    {"sblr.replication.operation.v3", "replication.operation", false},
    {"sblr.routine.define.v3", "routine.define", false},
    {"sblr.routine.execute.v3", "extensibility.invoke_udr_package", false},
    {"sblr.security.mutation.v3", "security.grant_right", false},
    {"sblr.session.management.v3", "session.prepare_statement", false},
    {"sblr.statement.management.v3", "session.prepare_statement", false},
    {"sblr.vector.execution.v3", "nosql.vector_search", false},
}};

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool HasAdmissionDiagnostic(const scratchbird::server::ServerSblrAdmissionResult& result,
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

bool HasDispatchDiagnostic(const sblr::SblrDispatchResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

std::string EvidenceMessage(const FamilyRow& row,
                            std::string_view phase,
                            std::string_view message) {
  std::string out(row.family);
  out += ' ';
  out += phase;
  out += ": ";
  out += message;
  return out;
}

std::string ParserJsonEnvelope(const FamilyRow& row) {
  std::string json;
  json += "{\"envelope\":\"SBLRExecutionEnvelope.v3\",";
  json += "\"operation_family\":\"";
  json += row.family;
  json += "\",\"operation_id\":\"";
  json += row.operation_id;
  json += "\",\"result_shape\":\"result.shape.family_admission\",";
  json += "\"diagnostic_shape\":\"diagnostic.canonical_message_vector\",";
  json += "\"contains_sql_text\":false}";
  return json;
}

scratchbird::server::ServerSblrAdmissionResult Admit(std::string payload,
                                                     bool cluster_authority = false) {
  return scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{std::move(payload),
                                                      cluster_authority});
}

void RequireFamilyAdmission(const FamilyRow& row) {
  const auto json_admission = Admit(ParserJsonEnvelope(row));
  Require(json_admission.admitted,
          EvidenceMessage(row, "parser_json", "exact operation_family was rejected"));
  Require(json_admission.operation_family == row.family,
          EvidenceMessage(row, "parser_json", "operation family changed"));
  Require(json_admission.operation_id == row.operation_id,
          EvidenceMessage(row, "parser_json", "operation id changed"));

  const auto family_only_admission = Admit(std::string(row.family));
  Require(family_only_admission.admitted,
          EvidenceMessage(row, "family_only", "plain family payload was rejected"));
  Require(family_only_admission.operation_family == row.family,
          EvidenceMessage(row, "family_only", "plain family payload changed family"));

  if (row.cluster_private) {
    Require(family_only_admission.requires_public_abi_dispatch,
            EvidenceMessage(row, "cluster_boundary", "cluster family did not require dispatch boundary"));
    Require(StartsWith(family_only_admission.operation_id, "cluster."),
            EvidenceMessage(row, "cluster_boundary", "cluster family did not use a cluster operation"));
    Require(json_admission.requires_public_abi_dispatch,
            EvidenceMessage(row, "cluster_boundary", "cluster JSON route did not require dispatch boundary"));
  } else {
    Require(!StartsWith(family_only_admission.operation_family, "sblr.cluster."),
            EvidenceMessage(row, "noncluster_boundary", "non-cluster family became cluster-private"));
    Require(!StartsWith(family_only_admission.operation_id, "cluster."),
            EvidenceMessage(row, "noncluster_boundary", "non-cluster family used a cluster operation"));
  }
}

void RequireRejectionPathsPreserved() {
  const auto unknown = Admit(
      "{\"envelope\":\"SBLRExecutionEnvelope.v3\","
      "\"operation_family\":\"sblr.unknown.family.v3\","
      "\"operation_id\":\"query.plan_operation\","
      "\"result_shape\":\"result.shape.family_admission\","
      "\"diagnostic_shape\":\"diagnostic.canonical_message_vector\"}");
  Require(!unknown.admitted, "unknown SBLR family was admitted");
  Require(HasAdmissionDiagnostic(unknown, "PARSER_SERVER_IPC.SBLR_REVALIDATION_FAILED"),
          "unknown SBLR family did not return revalidation failure");

  const auto raw_sql = Admit("SELECT 1");
  Require(!raw_sql.admitted, "raw SQL payload was admitted");
  Require(HasAdmissionDiagnostic(raw_sql, "SBLR.SQL_TEXT_FORBIDDEN"),
          "raw SQL payload did not return SQL-text refusal");

  const auto sql_text = Admit(
      "{\"envelope\":\"SBLRExecutionEnvelope.v3\","
      "\"operation_family\":\"sblr.query.relational.v3\","
      "\"operation_id\":\"query.plan_operation\","
      "\"result_shape\":\"result.shape.family_admission\","
      "\"diagnostic_shape\":\"diagnostic.canonical_message_vector\","
      "\"sql_text\":\"SELECT 1\"}");
  Require(!sql_text.admitted, "JSON SQL-text marker was admitted");
  Require(HasAdmissionDiagnostic(sql_text, "SBLR.SQL_TEXT_FORBIDDEN"),
          "JSON SQL-text marker did not return SQL-text refusal");

  const auto duplicate_json = Admit(
      "{\"envelope\":\"SBLRExecutionEnvelope.v3\","
      "\"operation_family\":\"sblr.query.relational.v3\","
      "\"operation_family\":\"sblr.query.relational.v3\","
      "\"operation_id\":\"query.plan_operation\","
      "\"result_shape\":\"result.shape.family_admission\","
      "\"diagnostic_shape\":\"diagnostic.canonical_message_vector\"}");
  Require(!duplicate_json.admitted, "duplicate JSON family field was admitted");
  Require(HasAdmissionDiagnostic(duplicate_json, "PARSER_SERVER_IPC.SBLR_DUPLICATE_FIELD"),
          "duplicate JSON field did not return duplicate-field refusal");

  const auto duplicate_text = Admit(
      "operation_id=query.plan_operation\n"
      "operation_id=query.evaluate_projection\n"
      "sblr_operation_family=sblr.query.relational.v3\n"
      "result_shape=result.shape.family_admission\n"
      "diagnostic_shape=diagnostic.canonical_message_vector\n"
      "parser_resolved_names_to_uuids=true\n"
      "requires_cluster_authority=false\n");
  Require(!duplicate_text.admitted, "duplicate text operation field was admitted");
  Require(HasAdmissionDiagnostic(duplicate_text, "PARSER_SERVER_IPC.SBLR_DUPLICATE_FIELD"),
          "duplicate text field did not return duplicate-field refusal");

  const auto missing_result = Admit(
      "{\"envelope\":\"SBLRExecutionEnvelope.v3\","
      "\"operation_family\":\"sblr.query.relational.v3\","
      "\"operation_id\":\"query.plan_operation\","
      "\"diagnostic_shape\":\"diagnostic.canonical_message_vector\"}");
  Require(!missing_result.admitted, "missing result shape was admitted");
  Require(HasAdmissionDiagnostic(missing_result, "PARSER_SERVER_IPC.SBLR_REVALIDATION_FAILED"),
          "missing result shape did not return revalidation failure");

  const auto missing_diagnostic = Admit(
      "{\"envelope\":\"SBLRExecutionEnvelope.v3\","
      "\"operation_family\":\"sblr.query.relational.v3\","
      "\"operation_id\":\"query.plan_operation\","
      "\"result_shape\":\"result.shape.family_admission\"}");
  Require(!missing_diagnostic.admitted, "missing diagnostic shape was admitted");
  Require(HasAdmissionDiagnostic(missing_diagnostic, "PARSER_SERVER_IPC.SBLR_REVALIDATION_FAILED"),
          "missing diagnostic shape did not return revalidation failure");

  const auto unresolved_text = Admit(
      "operation_id=query.plan_operation\n"
      "sblr_operation_family=sblr.query.relational.v3\n"
      "result_shape=result.shape.family_admission\n"
      "diagnostic_shape=diagnostic.canonical_message_vector\n"
      "parser_resolved_names_to_uuids=false\n"
      "requires_cluster_authority=false\n");
  Require(!unresolved_text.admitted, "unresolved text envelope was admitted");
  Require(HasAdmissionDiagnostic(unresolved_text, "PARSER_SERVER_IPC.SBLR_REVALIDATION_FAILED"),
          "unresolved text envelope did not return revalidation failure");

  const auto unsupported_version = Admit(
      "{\"envelope\":\"SBLRExecutionEnvelope.v2\","
      "\"operation_family\":\"sblr.query.relational.v3\","
      "\"operation_id\":\"query.plan_operation\","
      "\"result_shape\":\"result.shape.family_admission\","
      "\"diagnostic_shape\":\"diagnostic.canonical_message_vector\"}");
  Require(!unsupported_version.admitted, "unsupported envelope version was admitted");
  Require(HasAdmissionDiagnostic(unsupported_version,
                                 "PARSER_SERVER_IPC.SBLR_ENVELOPE_VERSION_UNSUPPORTED"),
          "unsupported envelope version did not return version diagnostic");
}

api::EngineRequestContext EngineContext() {
  api::EngineRequestContext context;
  context.security_context_present = true;
  context.database_uuid.canonical = "family-admission-database";
  context.session_uuid.canonical = "family-admission-session";
  context.principal_uuid.canonical = "family-admission-principal";
  return context;
}

void RequireClusterProviderBoundary() {
  struct ClusterDispatchRow {
    std::string_view operation_id;
    std::string_view opcode;
  };
  constexpr std::array<ClusterDispatchRow, 3> rows{{
      {"cluster.control_cluster", "SBLR_CLUSTER_CONTROL_CLUSTER"},
      {"cluster.inspect_state", "SBLR_CLUSTER_INSPECT_STATE"},
      {"cluster.inspect_replication", "SBLR_CLUSTER_INSPECT_REPLICATION"},
  }};

  for (const auto& row : rows) {
    auto envelope = sblr::MakeSblrEnvelope(std::string(row.operation_id),
                                           std::string(row.opcode),
                                           "family-admission-cluster-boundary");
    envelope.requires_security_context = true;
    envelope.requires_cluster_authority = true;

    sblr::SblrDispatchRequest request;
    request.context = EngineContext();
    request.envelope = envelope;

    const auto result = sblr::DispatchSblrOperation(request);
    Require(result.envelope_validated, "cluster boundary envelope was not validated");
    Require(result.accepted, "cluster boundary dispatch was not accepted");
    Require(result.dispatched_to_api, "cluster boundary did not reach provider API");

    if (cluster_provider::ClusterProviderSupportsExecution()) {
      Require(result.api_result.ok, "cluster provider did not execute through configured provider");
    } else {
      Require(!result.api_result.ok, "no-cluster provider executed a private cluster operation");
      Require(result.api_result.cluster_authority_required,
              "no-cluster provider did not preserve cluster authority requirement");
      Require(HasApiDiagnostic(result.api_result,
                               cluster_provider::kClusterSupportNotEnabledCode),
              "no-cluster provider omitted cluster-disabled API diagnostic");
      Require(HasDispatchDiagnostic(result,
                                    cluster_provider::kClusterSupportNotEnabledCode),
              "no-cluster provider omitted cluster-disabled dispatch diagnostic");
    }
  }
}

void RequireProductionSourceIntegrity() {
  static constexpr std::array<std::string_view, 27> kForbidden = {
      "sbsql_sblr_final_cleanup",
      "final_cleanup",
      "B001Exact",
      "IsB001",
      "b001_",
      "_b001",
      "B002Exact",
      "IsB002",
      "b002_",
      "_b002",
      "B003Exact",
      "IsB003",
      "b003_",
      "_b003",
      "B007Exact",
      "IsB007",
      "AUDIT-0",
      "AUDIT-1",
      "AUDIT-2",
      "AUDIT-3",
      "AUDIT-4",
      "AUDIT-5",
      "AUDIT-6",
      "AUDIT-7",
      "AUDIT-8",
      "AUDIT-9",
      "SSFC-",
  };
  const std::filesystem::path source_root =
      std::filesystem::path(SCRATCHBIRD_PROJECT_SOURCE_DIR) / "src";
  for (const auto& entry : std::filesystem::recursive_directory_iterator(source_root)) {
    if (!entry.is_regular_file()) continue;
    std::ifstream in(entry.path(), std::ios::binary);
    if (!in) continue;
    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    for (const auto token : kForbidden) {
      Require(!Contains(text, token),
              std::string("production source contains forbidden batch token ") +
                  std::string(token) + " in " + entry.path().string());
    }
  }
}

}  // namespace

int main() {
  RequireProductionSourceIntegrity();
  for (const auto& row : kFamilies) {
    RequireFamilyAdmission(row);
  }
  RequireRejectionPathsPreserved();
  RequireClusterProviderBoundary();
  std::cout << "sbsql_sblr_final_cleanup_b007_family_admission_conformance=passed\n";
  return EXIT_SUCCESS;
}
