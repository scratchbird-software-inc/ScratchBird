// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "sblr_engine_envelope.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace scratchbird::engine::sblr {

struct SblrDispatchRequest {
  scratchbird::engine::internal_api::EngineRequestContext context;
  SblrOperationEnvelope envelope;
  scratchbird::engine::internal_api::EngineApiRequest api_request;
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
  std::vector<SblrEnvelopeDiagnostic> diagnostics;
};

bool IsClusterOperationId(std::string_view operation_id);
SblrDispatchResult DispatchSblrOperation(SblrDispatchRequest request);
SblrDispatchResult DecodeAndDispatchSblrOperation(
    std::string_view encoded_envelope,
    scratchbird::engine::internal_api::EngineRequestContext context,
    scratchbird::engine::internal_api::EngineApiRequest api_request = {});
std::string SerializeSblrDispatchResultToJson(const SblrDispatchResult& result);

}  // namespace scratchbird::engine::sblr
