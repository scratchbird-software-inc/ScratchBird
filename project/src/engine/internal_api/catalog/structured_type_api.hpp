// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_STRUCTURED_TYPE_RUNTIME
// Engine-owned structured-type descriptor runtime. Callers submit bound
// UUID/SBLR descriptors only; SQL text is never executable authority here.

struct EngineCreateStructuredTypeRequest : EngineApiRequest {};
struct EngineCreateStructuredTypeResult : EngineApiResult {};
EngineCreateStructuredTypeResult EngineCreateStructuredType(
    const EngineCreateStructuredTypeRequest& request);

struct EngineAlterStructuredTypeRequest : EngineApiRequest {};
struct EngineAlterStructuredTypeResult : EngineApiResult {};
EngineAlterStructuredTypeResult EngineAlterStructuredType(
    const EngineAlterStructuredTypeRequest& request);

struct EngineDropStructuredTypeRequest : EngineApiRequest {};
struct EngineDropStructuredTypeResult : EngineApiResult {};
EngineDropStructuredTypeResult EngineDropStructuredType(
    const EngineDropStructuredTypeRequest& request);

struct EngineShowStructuredTypeRequest : EngineApiRequest {};
struct EngineShowStructuredTypeResult : EngineApiResult {};
EngineShowStructuredTypeResult EngineShowStructuredType(
    const EngineShowStructuredTypeRequest& request);

struct EngineShowStructuredTypesRequest : EngineApiRequest {};
struct EngineShowStructuredTypesResult : EngineApiResult {};
EngineShowStructuredTypesResult EngineShowStructuredTypes(
    const EngineShowStructuredTypesRequest& request);

struct EngineEvaluateStructuredTypeConstructorRequest : EngineApiRequest {};
struct EngineEvaluateStructuredTypeConstructorResult : EngineApiResult {};
EngineEvaluateStructuredTypeConstructorResult EngineEvaluateStructuredTypeConstructor(
    const EngineEvaluateStructuredTypeConstructorRequest& request);

struct EngineEvaluateStructuredTypeCastRequest : EngineApiRequest {};
struct EngineEvaluateStructuredTypeCastResult : EngineApiResult {};
EngineEvaluateStructuredTypeCastResult EngineEvaluateStructuredTypeCast(
    const EngineEvaluateStructuredTypeCastRequest& request);

struct EngineCompareStructuredTypeValuesRequest : EngineApiRequest {};
struct EngineCompareStructuredTypeValuesResult : EngineApiResult {};
EngineCompareStructuredTypeValuesResult EngineCompareStructuredTypeValues(
    const EngineCompareStructuredTypeValuesRequest& request);

struct EngineSerializeStructuredTypeValueRequest : EngineApiRequest {};
struct EngineSerializeStructuredTypeValueResult : EngineApiResult {};
EngineSerializeStructuredTypeValueResult EngineSerializeStructuredTypeValue(
    const EngineSerializeStructuredTypeValueRequest& request);

}  // namespace scratchbird::engine::internal_api
