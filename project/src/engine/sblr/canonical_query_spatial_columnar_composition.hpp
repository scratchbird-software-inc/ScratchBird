// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "canonical_query_execute.hpp"
#include "canonical_query_model_family_composition_support.hpp"

namespace scratchbird::engine::internal_api {
struct MgaRelationColumnStorageDescriptor;
}

namespace scratchbird::engine::sblr {

// Exact persisted column and current datatype-authority binding. A bound
// occurrence may refer to its owning datatype descriptor without conflating
// that identity with the distinct persisted column descriptor UUID.
bool Rcp079ExactColumnarJoinColumnBindingV1(
    const internal_api::EngineRequestContext& context,
    const internal_api::RelationalTypeDescriptor& relational,
    const internal_api::MgaRelationColumnStorageDescriptor& persisted,
    std::string* refusal_detail = nullptr);

// Executes admitted production spatial/columnar sources and the exact
// two-columnar-source join route. These routes consume and revalidate an
// engine-issued MGA statement boundary and cannot create or finalize one.
CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalSpatialColumnarFamilyQuery(
    const CanonicalCurrentHeapExecutionRequest& input,
    Rcp079CapturedModelLegV1* leg_capture = nullptr);

CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalColumnarFamilyJoinQuery(
    const CanonicalCurrentHeapExecutionRequest& input);

bool IsCanonicalSpatialColumnarContextualRouteCandidate(
    const internal_api::TypedRelationalDag& dag) noexcept;

#if !defined(SCRATCHBIRD_QOW_QUERY_ROUTE_CONTRACT_ONLY)
std::uint32_t CanonicalSpatialColumnarContextualInternalProofMaskForTest();
#endif

}  // namespace scratchbird::engine::sblr
