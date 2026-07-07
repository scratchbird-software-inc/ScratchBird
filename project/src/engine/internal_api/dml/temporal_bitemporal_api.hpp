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

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_TEMPORAL_BITEMPORAL_API
// Engine-owned temporal/bitemporal API. Callers submit bound UUID/SBLR
// descriptors only; this API never accepts SQL text as executable authority.

struct EngineCreateTemporalPeriodRequest : EngineApiRequest {};
struct EngineCreateTemporalPeriodResult : EngineApiResult {};
EngineCreateTemporalPeriodResult EngineCreateTemporalPeriod(
    const EngineCreateTemporalPeriodRequest& request);

struct EngineDropTemporalPeriodRequest : EngineApiRequest {};
struct EngineDropTemporalPeriodResult : EngineApiResult {};
EngineDropTemporalPeriodResult EngineDropTemporalPeriod(
    const EngineDropTemporalPeriodRequest& request);

struct EngineShowBitemporalPeriodsRequest : EngineApiRequest {};
struct EngineShowBitemporalPeriodsResult : EngineApiResult {};
EngineShowBitemporalPeriodsResult EngineShowBitemporalPeriods(
    const EngineShowBitemporalPeriodsRequest& request);

struct EngineShowBitemporalHistoryRequest : EngineApiRequest {};
struct EngineShowBitemporalHistoryResult : EngineApiResult {};
EngineShowBitemporalHistoryResult EngineShowBitemporalHistory(
    const EngineShowBitemporalHistoryRequest& request);

struct EngineReadBitemporalHistoryRequest : EngineApiRequest {};
struct EngineReadBitemporalHistoryResult : EngineApiResult {};
EngineReadBitemporalHistoryResult EngineReadBitemporalHistory(
    const EngineReadBitemporalHistoryRequest& request);

struct EngineApplyForPortionOfPeriodRequest : EngineApiRequest {};
struct EngineApplyForPortionOfPeriodResult : EngineApiResult {};
EngineApplyForPortionOfPeriodResult EngineApplyForPortionOfPeriod(
    const EngineApplyForPortionOfPeriodRequest& request);

}  // namespace scratchbird::engine::internal_api
