// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "api_types.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct MatrixRow {
  std::string sblr_operation;
  std::string api_operation_id;
  std::string scope_status;
  std::string executor_readiness_status;
  bool required_transaction_context = false;
  bool required_security_context = true;
  bool requires_cluster_authority = false;
};

std::string Trim(std::string_view value) {
  std::size_t first = 0;
  while (first < value.size() && (value[first] == ' ' || value[first] == '\t' || value[first] == '"' || value[first] == '\'')) {
    ++first;
  }
  std::size_t last = value.size();
  while (last > first &&
         (value[last - 1] == ' ' || value[last - 1] == '\t' || value[last - 1] == '"' || value[last - 1] == '\'' ||
          value[last - 1] == '\r')) {
    --last;
  }
  return std::string(value.substr(first, last - first));
}

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool ParseBool(std::string_view value) {
  return value == "true" || value == "yes" || value == "1";
}

std::string FieldValue(std::string_view line) {
  const std::size_t colon = line.find(':');
  if (colon == std::string_view::npos) {
    return {};
  }
  return Trim(line.substr(colon + 1));
}

void FlushRow(std::vector<MatrixRow>* rows, MatrixRow* row) {
  if (!row->sblr_operation.empty() || !row->api_operation_id.empty()) {
    rows->push_back(*row);
  }
  *row = {};
}

std::vector<MatrixRow> LoadMatrix(const std::string& path) {
  std::ifstream input(path);
  std::vector<MatrixRow> rows;
  MatrixRow row;
  std::string line;
  while (std::getline(input, line)) {
    const std::string trimmed = Trim(line);
    if (StartsWith(trimmed, "- sblr_operation:")) {
      FlushRow(&rows, &row);
      row.sblr_operation = FieldValue(trimmed);
    } else if (StartsWith(trimmed, "api_operation_id:")) {
      row.api_operation_id = FieldValue(trimmed);
    } else if (StartsWith(trimmed, "scope_status:")) {
      row.scope_status = FieldValue(trimmed);
    } else if (StartsWith(trimmed, "executor_readiness_status:")) {
      row.executor_readiness_status = FieldValue(trimmed);
    } else if (StartsWith(trimmed, "required_transaction_context:")) {
      row.required_transaction_context = ParseBool(FieldValue(trimmed));
    } else if (StartsWith(trimmed, "required_security_context:")) {
      row.required_security_context = ParseBool(FieldValue(trimmed));
    } else if (StartsWith(trimmed, "requires_cluster_authority:")) {
      row.requires_cluster_authority = ParseBool(FieldValue(trimmed));
    }
  }
  FlushRow(&rows, &row);
  return rows;
}

scratchbird::engine::internal_api::EngineRequestContext ContextFor(const MatrixRow& row,
                                                                   const std::string& database_path) {
  scratchbird::engine::internal_api::EngineRequestContext context;
  context.trust_mode = scratchbird::engine::internal_api::EngineTrustMode::server_isolated;
  context.request_id = "fspe009-sbsql-behavior";
  context.database_path = database_path;
  context.database_uuid.canonical = "019e05b1-f009-7000-8000-000000000001";
  context.principal_uuid.canonical = "019e05b1-f009-7000-8000-000000000002";
  context.session_uuid.canonical = "019e05b1-f009-7000-8000-000000000003";
  context.transaction_uuid.canonical = "019e05b1-f009-7000-8000-000000000004";
  context.local_transaction_id = row.required_transaction_context ? 7009 : 7009;
  context.snapshot_visible_through_local_transaction_id = context.local_transaction_id;
  context.security_context_present = row.required_security_context;
  context.cluster_authority_available = false;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.trace_tags.push_back("FSPE-009");
  context.trace_tags.push_back("right:OBS_AGENT_STATE_READ");
  context.trace_tags.push_back("right:OBS_AGENT_CONTROL");
  context.trace_tags.push_back("right:OBS_CLUSTER_HEALTH_INSPECT");
  return context;
}

scratchbird::engine::internal_api::EngineApiRequest ApiRequestFor(
    const MatrixRow& row,
    const scratchbird::engine::internal_api::EngineRequestContext& context) {
  scratchbird::engine::internal_api::EngineApiRequest request;
  request.context = context;
  request.operation_id = row.api_operation_id;
  request.target_database.uuid.canonical = context.database_uuid.canonical;
  request.target_database.object_kind = "database";
  request.target_schema.uuid.canonical = "019e05b1-f009-7000-8000-000000000010";
  request.target_schema.object_kind = "schema";
  request.target_object.uuid.canonical = "019e05b1-f009-7000-8000-000000000011";
  request.target_object.object_kind = "object";
  request.localized_names.push_back({"en", "default", "schema", "fspe009_object", true});
  request.option_envelopes.push_back("name:fspe009_object");
  request.option_envelopes.push_back("policy_authorized:true");
  request.option_envelopes.push_back("evidence_sink_available:true");
  request.option_envelopes.push_back("metrics_fresh:true");
  request.option_envelopes.push_back("requested_pages:1");
  request.option_envelopes.push_back("requested_bytes:4096");
  request.option_envelopes.push_back("agent_type:conformance_agent");
  request.option_envelopes.push_back("action_class:conformance_action");
  request.predicate.predicate_kind = "constant_true";
  request.predicate.canonical_predicate_envelope = "sblr.predicate.true";
  request.projection.canonical_projection_envelopes.push_back("sblr.projection.identity");
  scratchbird::engine::internal_api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = "019e05b1-f009-7000-8000-000000000012";
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "text";
  descriptor.encoded_descriptor = "type=text";
  request.descriptors.push_back(descriptor);
  return request;
}

scratchbird::engine::sblr::SblrOperationEnvelope EnvelopeFor(const MatrixRow& row) {
  auto envelope = scratchbird::engine::sblr::MakeSblrEnvelope(row.api_operation_id, row.sblr_operation, "FSPE-009");
  envelope.parser_package_uuid = "019e05b1-f009-7000-8000-000000000020";
  envelope.registry_snapshot_uuid = "019e05b1-f009-7000-8000-000000000021";
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.requires_security_context = row.required_security_context;
  envelope.requires_transaction_context = row.required_transaction_context;
  envelope.requires_cluster_authority = row.requires_cluster_authority || StartsWith(row.scope_status, "cluster_only");
  envelope.operands.push_back({"option", "name", "fspe009_object"});
  envelope.operands.push_back({"option", "policy_authorized", "true"});
  envelope.operands.push_back({"option", "evidence_sink_available", "true"});
  envelope.operands.push_back({"option", "metrics_fresh", "true"});
  envelope.operands.push_back({"uuid", "target_object", "019e05b1-f009-7000-8000-000000000011"});
  return envelope;
}

bool HasApiDiagnostic(const scratchbird::engine::internal_api::EngineApiResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) {
      return true;
    }
  }
  return false;
}

bool HasDispatchDiagnostic(const scratchbird::engine::sblr::SblrDispatchResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) {
      return true;
    }
  }
  return false;
}

bool IsCallableStatus(const MatrixRow& row) {
  return row.executor_readiness_status == "mapped_ready" || row.executor_readiness_status == "sblr_callable";
}

bool IsClusterFailClosedStatus(const MatrixRow& row) {
  return row.executor_readiness_status == "cluster_deferred" ||
         row.executor_readiness_status == "cluster_fail_closed_mapped" ||
         row.executor_readiness_status == "cluster_provider_boundary_mapped";
}

bool IsClusterProviderExecutionResult(const scratchbird::engine::sblr::SblrDispatchResult& result) {
  return result.envelope_validated &&
         result.accepted &&
         result.dispatched_to_api &&
         result.api_result.ok &&
         StartsWith(result.api_result.result_shape.result_kind, "cluster.") &&
         !HasApiDiagnostic(result.api_result, "SBLR.CLUSTER.SUPPORT_NOT_ENABLED") &&
         !HasApiDiagnostic(result.api_result, "SB_SBLR_DISPATCH_CLUSTER_AUTHORITY_UNAVAILABLE") &&
         !HasApiDiagnostic(result.api_result, "SB_ENGINE_API_NOT_IMPLEMENTED");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: sbsql_behavior_conformance_fixture <SBLR_API_OPERATION_MATRIX.yaml>\n";
    return 1;
  }

  const std::vector<MatrixRow> rows = LoadMatrix(argv[1]);
  if (rows.empty()) {
    std::cerr << "matrix has no rows\n";
    return 2;
  }

  const std::string database_path = "/tmp/sb_fspe009_sbsql_behavior_conformance.db";
  std::remove(database_path.c_str());
  std::remove((database_path + ".sb.api_events").c_str());

  std::size_t callable_rows = 0;
  std::size_t cluster_boundary_rows = 0;
  std::size_t skipped_rows = 0;

  for (const auto& row : rows) {
    if (row.sblr_operation.empty() || row.api_operation_id.empty()) {
      std::cerr << "matrix row missing SBLR/API operation id\n";
      return 3;
    }

    const auto context = ContextFor(row, database_path);
    const auto request = ApiRequestFor(row, context);
    const auto encoded = scratchbird::engine::sblr::EncodeSblrEnvelope(EnvelopeFor(row));
    const auto result = scratchbird::engine::sblr::DecodeAndDispatchSblrOperation(encoded, context, request);

    if (IsCallableStatus(row)) {
      ++callable_rows;
      if (!result.envelope_validated || !result.accepted || !result.dispatched_to_api) {
        std::cerr << "callable row did not dispatch: " << row.api_operation_id << "\n"
                  << scratchbird::engine::sblr::SerializeSblrDispatchResultToJson(result);
        return 4;
      }
      if (HasDispatchDiagnostic(result, "SB_SBLR_DISPATCH_UNKNOWN_OPERATION") ||
          HasApiDiagnostic(result.api_result, "SB_ENGINE_API_NOT_IMPLEMENTED")) {
        std::cerr << "callable row used unknown/not-implemented path: " << row.api_operation_id << "\n"
                  << scratchbird::engine::sblr::SerializeSblrDispatchResultToJson(result);
        return 5;
      }
      continue;
    }

    if (IsClusterFailClosedStatus(row)) {
      ++cluster_boundary_rows;
      const bool has_old_dispatch_refusal =
          result.api_result.cluster_authority_required &&
          HasApiDiagnostic(result.api_result, "SB_SBLR_DISPATCH_CLUSTER_AUTHORITY_UNAVAILABLE");
      const bool has_provider_refusal =
          !result.api_result.ok &&
          HasApiDiagnostic(result.api_result, "SBLR.CLUSTER.SUPPORT_NOT_ENABLED");
      const bool has_provider_execution = IsClusterProviderExecutionResult(result);
      if (!result.envelope_validated || !result.dispatched_to_api ||
          (!has_old_dispatch_refusal && !has_provider_refusal && !has_provider_execution)) {
        std::cerr << "cluster row did not reach a valid cluster boundary result: " << row.api_operation_id << "\n"
                  << scratchbird::engine::sblr::SerializeSblrDispatchResultToJson(result);
        return 6;
      }
      continue;
    }

    ++skipped_rows;
  }

  if (callable_rows < 80 || cluster_boundary_rows < 5 || skipped_rows == 0) {
    std::cerr << "unexpected matrix coverage callable=" << callable_rows
              << " cluster_boundary=" << cluster_boundary_rows
              << " skipped=" << skipped_rows << "\n";
    return 7;
  }

  std::remove(database_path.c_str());
  std::remove((database_path + ".sb.api_events").c_str());
  return 0;
}
