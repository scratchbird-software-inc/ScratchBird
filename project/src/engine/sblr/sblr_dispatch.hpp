// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "dml/import_api.hpp"
#include "query/contextual_text_literal_authority.hpp"
#include "sblr_engine_envelope.hpp"
#include "sblr_parameter_runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::sblr {

// Receipt-owned, process-local authority for one query.execute-1.1 dispatch.
// Prepare populates this object while the statement receipt mutex is held;
// the admitted direct route performs the sole joint token/profile transition.
// The move-only prepared set and lease are never copied into a plan, cursor,
// or provider-local closure.
struct ContextualTextDispatchActivationV2 {
  using JointConsumeLocked = scratchbird::engine::internal_api::
      EngineContextualTextLiteralJointConsumeResultV2 (*)(
          void*, scratchbird::engine::internal_api::
                     PreparedContextualTextLiteralSetV2*);

  scratchbird::engine::internal_api::PreparedContextualTextLiteralSetV2
      prepared;
  scratchbird::engine::internal_api::ContextualTextExecutionAuthorityLeaseV2
      lease;
  JointConsumeLocked joint_consume_locked = nullptr;
  void* joint_consume_context = nullptr;
  // Receipt-lock bridge state which remains live beside this activation for
  // the synchronous direct dispatch. Provider-owned Prepared/lease bytes are
  // reported by the contextual authority resource estimator and are never
  // mirrored here.
  std::uint64_t bridge_retained_logical_bytes = 0;
  bool joint_consumed = false;
};

struct SblrDispatchRequest {
  scratchbird::engine::internal_api::EngineRequestContext context;
  SblrOperationEnvelope envelope;
  scratchbird::engine::internal_api::EngineApiRequest api_request;
  std::optional<SblrParameterValueSetV1> parameter_value_set;
  std::shared_ptr<ContextualTextDispatchActivationV2>
      contextual_text_activation;
  // Proven only by the authenticated SBOS admission bridge after observing
  // the exact three-record begin/root/end package. Inline/direct envelopes
  // leave this false and cannot execute standalone-only operation 793.
  bool standalone_package_root = false;
};

struct SblrDispatchResult {
  bool accepted = false;
  bool envelope_validated = false;
  bool dispatched_to_api = false;
  bool logical_graph_populated = false;
  bool logical_properties_populated = false;
  bool optimizer_admitted = false;
  bool optimizer_admission_degraded = false;
  bool optimizer_benchmark_clean_ready = false;
  bool optimizer_selected = false;
  bool physical_dag_published = false;
  bool physical_dag_executed = false;
  bool runtime_actuals_attached = false;
  bool canonical_result_published = false;
  std::size_t optimizer_admission_stage_count = 0;
  std::size_t logical_node_count = 0;
  std::size_t logical_property_count = 0;
  std::size_t physical_node_count = 0;
  std::size_t canonical_result_column_count = 0;
  std::size_t canonical_result_row_count = 0;
  std::string selected_plan_uuid;
  std::string canonical_result_bytes;
  scratchbird::engine::internal_api::EngineApiResult api_result;
  // Typed semantic extension for opcode 793. The base result remains
  // populated for generic dispatch consumers, while this carrier preserves
  // all twelve Core fields and the accepted IPEV without inventing bytes.
  std::optional<scratchbird::engine::internal_api::EnginePlanImportRowsResult>
      plan_import_rows_result;
  std::vector<SblrEnvelopeDiagnostic> diagnostics;
};

struct SblrQueryPreflightResult {
  bool ok = false;
  SblrOperationEnvelope materialized_envelope;
  std::string diagnostic_id;
  std::string detail;
};

struct QueryExecuteResultHandleFieldV1 {
  std::string name;
  std::string descriptor;
  std::string value;
};

struct QueryExecuteResultHandleV1 {
  std::string execution_uuid;
  std::string result_set_uuid;
  std::string row_descriptor_uuid;
  std::string snapshot_uuid;
};

struct QueryExecuteResultHandleValidationV1 {
  bool ok = false;
  QueryExecuteResultHandleV1 handle;
  std::string diagnostic_id;
  std::string detail;
};

bool IsClusterOperationId(std::string_view operation_id);
SblrQueryPreflightResult PreflightSblrQueryOperation(
    SblrDispatchRequest request);
QueryExecuteResultHandleValidationV1 ValidateQueryExecuteResultHandleV1(
    std::string_view result_shape_id,
    std::uint32_t result_shape_version,
    const std::vector<QueryExecuteResultHandleFieldV1>& fields);
SblrDispatchResult DispatchSblrOperation(SblrDispatchRequest request);
SblrDispatchResult DecodeAndDispatchSblrOperation(
    std::string_view encoded_envelope,
    scratchbird::engine::internal_api::EngineRequestContext context,
    scratchbird::engine::internal_api::EngineApiRequest api_request = {});
std::string SerializeSblrDispatchResultToJson(const SblrDispatchResult& result);

}  // namespace scratchbird::engine::sblr
