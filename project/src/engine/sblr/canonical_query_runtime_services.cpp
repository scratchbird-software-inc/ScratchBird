// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_query_execute.hpp"
#include "canonical_query_model_family_composition_support.hpp"
#include "query/expression_api.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scratchbird::engine::sblr {
namespace api = scratchbird::engine::internal_api;
namespace exec = scratchbird::engine::executor;
namespace opt = scratchbird::engine::optimizer;
namespace plan = scratchbird::engine::planner;

// SEARCH_KEY: SB_ENGINE_CANONICAL_QUERY_RUNTIME_SERVICES_AUTHORITY
// Delegates canonical scalar comparison to the engine and polls bounded
// cancellation callbacks. Owns no plan selection, storage, snapshot construction,
// transaction finality, or public route selection.

#if !defined(SCRATCHBIRD_QOW_QUERY_ROUTE_CONTRACT_ONLY)
namespace {

bool CompareCanonicalQueryScalarsV1(
    const api::EngineRequestContext& context,
    const api::EngineTypedValue& left,
    const api::EngineTypedValue& right,
    int* comparison,
    std::string* diagnostic_id,
    std::string* refusal_detail) {
  if (comparison == nullptr || diagnostic_id == nullptr ||
      refusal_detail == nullptr) {
    return false;
  }
  *comparison = 0;
  diagnostic_id->clear();
  refusal_detail->clear();
  api::EngineCompareCanonicalScalarValuesRequest request;
  request.borrowed_context = &context;
  request.borrowed_left_value = &left;
  request.borrowed_right_value = &right;
  const auto compared = api::EngineCompareCanonicalScalarValues(request);
  if (!compared.ok) {
    if (!compared.diagnostics.empty()) {
      *diagnostic_id = compared.diagnostics.front().code;
      *refusal_detail = compared.diagnostics.front().detail;
    } else {
      *diagnostic_id =
          "QOW-DIAG-RCP024-COMPARISON-AUTHORITY-REFUSAL-V1";
      *refusal_detail = "canonical scalar comparison was refused";
    }
    return false;
  }
  *comparison = compared.comparison < 0
                    ? -1
                    : (compared.comparison > 0 ? 1 : 0);
  return true;
}

}  // namespace

bool CompareCanonicalRelationalScalarsV1(
    const api::EngineRequestContext& context,
    const api::EngineTypedValue& left,
    const api::EngineTypedValue& right,
    int* comparison,
    std::string* diagnostic_id,
    std::string* refusal_detail) {
  return CompareCanonicalQueryScalarsV1(
      context, left, right, comparison, diagnostic_id, refusal_detail);
}
#endif

namespace {

bool PollLiveCancellationProbeImpl(
    const std::function<bool()>& cancellation_requested,
    LiveCancellationProbeState* state) noexcept {
  if (state == nullptr) return true;
  if (*state != LiveCancellationProbeState::kRunning) return true;
  try {
    if (!cancellation_requested || !cancellation_requested()) return false;
    *state = LiveCancellationProbeState::kCancelled;
  } catch (...) {
    *state = LiveCancellationProbeState::kProbeFailed;
  }
  return true;
}

}  // namespace

bool PollLiveCancellationProbe(
    const std::function<bool()>& cancellation_requested,
    LiveCancellationProbeState* state) noexcept {
  return PollLiveCancellationProbeImpl(cancellation_requested, state);
}

}  // namespace scratchbird::engine::sblr
