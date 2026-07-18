// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

struct EngineSelectRowsRequest;

// SEARCH_KEY: SB_ENGINE_GLOBAL_AGGREGATE_VIEW_DESCRIPTOR_V1
//
// SQL-free engine descriptor for a bounded persisted global-aggregate view.
// It records only neutral UUID/descriptor authority, the admitted expression,
// and the exact result contract. Parser family, dialect, SQL text, transaction
// finality, and storage authority are deliberately absent.
inline constexpr const char* kEngineGlobalAggregateViewMarkerV1 =
    "engine.global_aggregate_view.v1";
inline constexpr const char* kEngineGlobalAggregateViewInt32MultiplyV1 =
    "int32_literal_times_int32_field_to_int64";

struct EngineGlobalAggregateViewDescriptor {
  bool present = false;
  EngineUuid view_uuid;
  EngineUuid view_descriptor_uuid;
  std::uint64_t view_descriptor_generation = 0;
  EngineUuid source_relation_uuid;
  EngineUuid source_relation_descriptor_uuid;
  std::uint64_t source_relation_descriptor_generation = 0;
  EngineUuid source_column_uuid;
  EngineUuid source_column_descriptor_uuid;
  std::int32_t expression_literal_int32 = 0;
  EngineUuid aggregate_function_uuid;
  std::string result_alias;
  EngineDescriptor result_descriptor;
  EngineApiDiagnostic diagnostic;
};

struct EngineGlobalAggregateViewCreatePreparation {
  bool ok = false;
  bool altered_existing = false;
  EngineApiDiagnostic diagnostic;
  EngineGlobalAggregateViewDescriptor descriptor;
  std::vector<std::string> canonical_persisted_options;
};

bool IsEngineGlobalAggregateViewCreateRequest(
    const EngineApiRequest& request);

// Validates existing request fields against the exact current MGA relation
// descriptor and allocates the view/descriptor identities owned by the engine.
// CREATE OR ALTER retains an existing view UUID and always allocates a new
// descriptor UUID with the next generation.
EngineGlobalAggregateViewCreatePreparation
PrepareEngineGlobalAggregateViewCreate(const EngineApiRequest& request);

// Loads the descriptor visible to the exact selected MGA transaction. An
// ordinary persisted view is classified with present=false and a successful
// diagnostic; a malformed bounded descriptor fails closed.
EngineGlobalAggregateViewDescriptor DescribeEngineGlobalAggregateView(
    const EngineRequestContext& context,
    const std::string& view_uuid);

// Bounded semantic descriptor returned by neutral name resolution. It exposes
// the marker, view descriptor identity/generation, and result details only;
// source relation, source column, literal, and expression internals remain
// engine-side expansion state.
EngineDescriptor EngineGlobalAggregateViewSemanticDescriptor(
    const EngineGlobalAggregateViewDescriptor& descriptor);

bool IsEngineGlobalAggregateViewSelectRequest(
    const EngineSelectRowsRequest& request);

// Revalidates the caller's semantic descriptor and the persisted source
// descriptor, then expands the view into a direct typed aggregate request.
// The returned request is still executed by the ordinary one-scan MGA select
// path; this function performs no row scan and no aggregation.
EngineApiDiagnostic ExpandEngineGlobalAggregateViewSelect(
    const EngineSelectRowsRequest& request,
    EngineSelectRowsRequest* expanded,
    EngineGlobalAggregateViewDescriptor* descriptor);

}  // namespace scratchbird::engine::internal_api
