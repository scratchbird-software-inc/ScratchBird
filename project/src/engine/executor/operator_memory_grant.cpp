// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "operator_memory_grant.hpp"

#include "runtime_platform.hpp"

#include <utility>

namespace scratchbird::engine::executor {
namespace {

using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status OperatorMemoryOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::engine};
}

Status OperatorMemoryErrorStatus() {
  return {StatusCode::memory_invalid_request, Severity::error, Subsystem::engine};
}

void AppendCommonEvidence(const ExecutorOperatorMemoryRequest& request,
                          ExecutorOperatorMemoryResult* result) {
  if (result == nullptr) {
    return;
  }
  result->evidence.push_back("MMCH_LIVE_OPERATOR_MEMORY_GRANTS");
  result->evidence.push_back("executor.operator_memory.kind=" +
                             std::string(ExecutorMemoryOperatorKindName(request.operator_kind)));
  result->evidence.push_back("executor.operator_memory.route_label=" + request.route_label);
  result->evidence.push_back("executor.operator_memory.shape=" +
                             std::string(ExecutorQueryShapeName(
                                 ExecutorQueryShapeForOperator(request.operator_kind))));
  result->evidence.push_back("executor.operator_memory.primitive=query_memory_arena");
  result->evidence.push_back(
      "executor.operator_memory.authority_scope=evidence_only_not_transaction_finality_visibility_security_recovery_parser_reference_or_benchmark_authority");
  result->evidence.push_back(
      "executor.operator_memory.transaction_inventory_authoritative=" +
      std::string(request.authority.transaction_inventory_authoritative ? "true" : "false"));
  result->evidence.push_back(
      "executor.operator_memory.security_recheck_required=" +
      std::string(request.authority.security_recheck_required ? "true" : "false"));
}

ExecutorOperatorMemoryResult Refuse(ExecutorOperatorMemoryRequest request,
                                    std::string code,
                                    std::string message) {
  ExecutorOperatorMemoryResult result;
  result.status = OperatorMemoryErrorStatus();
  result.fail_closed = true;
  result.diagnostic = MakeDiagnostic(result.status.code,
                                     result.status.severity,
                                     result.status.subsystem,
                                     std::move(code),
                                     std::move(message),
                                     {},
                                     {},
                                     "engine.executor.operator_memory",
                                     "operator memory grants require route, arena, MGA, and security evidence");
  AppendCommonEvidence(request, &result);
  result.evidence.push_back("executor.operator_memory.fail_closed=true");
  result.evidence.push_back("executor.operator_memory.refused=" +
                            result.diagnostic.diagnostic_code);
  return result;
}

bool UnsafeAuthority(const ExecutorOperatorMemoryAuthority& authority) {
  return authority.parser_client_or_reference_memory_authority ||
         authority.memory_visibility_or_finality_authority ||
         authority.memory_recovery_authority ||
         authority.memory_authorization_authority;
}

}  // namespace

const char* ExecutorMemoryOperatorKindName(ExecutorMemoryOperatorKind kind) {
  switch (kind) {
    case ExecutorMemoryOperatorKind::scan: return "scan";
    case ExecutorMemoryOperatorKind::sort: return "sort";
    case ExecutorMemoryOperatorKind::hash_join: return "hash_join";
    case ExecutorMemoryOperatorKind::merge_join: return "merge_join";
    case ExecutorMemoryOperatorKind::aggregate: return "aggregate";
    case ExecutorMemoryOperatorKind::window: return "window";
    case ExecutorMemoryOperatorKind::candidate_set: return "candidate_set";
    case ExecutorMemoryOperatorKind::vector_search: return "vector_search";
    case ExecutorMemoryOperatorKind::full_text_search: return "full_text_search";
    case ExecutorMemoryOperatorKind::graph_traversal: return "graph_traversal";
    case ExecutorMemoryOperatorKind::document_path: return "document_path";
    case ExecutorMemoryOperatorKind::time_series_rollup: return "time_series_rollup";
    case ExecutorMemoryOperatorKind::dml_write: return "dml_write";
    case ExecutorMemoryOperatorKind::streaming_result: return "streaming_result";
  }
  return "unknown";
}

ExecutorQueryShape ExecutorQueryShapeForOperator(ExecutorMemoryOperatorKind kind) {
  switch (kind) {
    case ExecutorMemoryOperatorKind::scan:
    case ExecutorMemoryOperatorKind::sort:
    case ExecutorMemoryOperatorKind::hash_join:
    case ExecutorMemoryOperatorKind::merge_join:
    case ExecutorMemoryOperatorKind::aggregate:
    case ExecutorMemoryOperatorKind::window:
    case ExecutorMemoryOperatorKind::streaming_result:
      return ExecutorQueryShape::relational;
    case ExecutorMemoryOperatorKind::candidate_set:
      return ExecutorQueryShape::candidate_set;
    case ExecutorMemoryOperatorKind::vector_search:
      return ExecutorQueryShape::vector;
    case ExecutorMemoryOperatorKind::full_text_search:
      return ExecutorQueryShape::search;
    case ExecutorMemoryOperatorKind::graph_traversal:
      return ExecutorQueryShape::graph;
    case ExecutorMemoryOperatorKind::document_path:
      return ExecutorQueryShape::document;
    case ExecutorMemoryOperatorKind::time_series_rollup:
      return ExecutorQueryShape::time_series;
    case ExecutorMemoryOperatorKind::dml_write:
      return ExecutorQueryShape::dml;
  }
  return ExecutorQueryShape::relational;
}

ExecutorOperatorMemoryResult RequestExecutorOperatorMemory(
    ExecutorOperatorMemoryRequest request) {
  if (request.route_label.empty()) {
    return Refuse(std::move(request),
                  "SB_EXECUTOR_OPERATOR_MEMORY.ROUTE_LABEL_REQUIRED",
                  "executor.operator_memory.route_label_required");
  }
  if (request.arena == nullptr) {
    return Refuse(std::move(request),
                  "SB_EXECUTOR_OPERATOR_MEMORY.ARENA_REQUIRED",
                  "executor.operator_memory.arena_required");
  }
  if (!request.authority.engine_mga_snapshot_bound ||
      !request.authority.transaction_inventory_authoritative) {
    return Refuse(std::move(request),
                  "SB_EXECUTOR_OPERATOR_MEMORY.MGA_UNPROVEN",
                  "executor.operator_memory.mga_unproven");
  }
  if (!request.authority.security_recheck_required) {
    return Refuse(std::move(request),
                  "SB_EXECUTOR_OPERATOR_MEMORY.SECURITY_RECHECK_REQUIRED",
                  "executor.operator_memory.security_recheck_required");
  }
  if (UnsafeAuthority(request.authority)) {
    return Refuse(std::move(request),
                  "SB_EXECUTOR_OPERATOR_MEMORY.UNSAFE_AUTHORITY",
                  "executor.operator_memory.unsafe_authority");
  }

  ExecutorQueryMemoryRequest query_request;
  query_request.shape = ExecutorQueryShapeForOperator(request.operator_kind);
  query_request.bytes = request.bytes;
  query_request.spillable = request.spillable;
  query_request.purpose =
      request.purpose.empty()
          ? std::string("operator.") + ExecutorMemoryOperatorKindName(request.operator_kind)
          : request.purpose;
  query_request.resource_governance = request.resource_governance;

  ExecutorOperatorMemoryResult result;
  result.query_memory = RequestExecutorQueryMemory(request.arena, std::move(query_request));
  result.status = result.query_memory.status;
  result.fail_closed = result.query_memory.fail_closed;
  result.diagnostic = result.query_memory.diagnostic;
  result.evidence = result.query_memory.evidence;
  AppendCommonEvidence(request, &result);
  if (!result.query_memory.ok() || !result.query_memory.arena_result.grant.has_value()) {
    result.fail_closed = true;
    result.evidence.push_back("executor.operator_memory.fail_closed=true");
    return result;
  }
  result.status = OperatorMemoryOkStatus();
  result.grant_id = result.query_memory.arena_result.grant->grant_id;
  result.evidence.push_back("executor.operator_memory.live_operator_route=true");
  result.evidence.push_back("executor.operator_memory.grant_id=" + result.grant_id);
  return result;
}

scratchbird::core::memory::QueryMemoryArenaReleaseResult ReleaseExecutorOperatorMemory(
    scratchbird::core::memory::QueryMemoryArena* arena,
    ExecutorMemoryOperatorKind operator_kind,
    const std::string& grant_id) {
  auto result = ReleaseExecutorQueryMemory(arena, grant_id);
  result.evidence.push_back("MMCH_LIVE_OPERATOR_MEMORY_GRANTS");
  result.evidence.push_back("executor.operator_memory.release_kind=" +
                            std::string(ExecutorMemoryOperatorKindName(operator_kind)));
  result.evidence.push_back("executor.operator_memory.release_routed=true");
  return result;
}

}  // namespace scratchbird::engine::executor
