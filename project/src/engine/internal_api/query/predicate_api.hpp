// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "query/expression_api.hpp"

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_QUERY_PREDICATE_API
struct EngineBindPredicateRequest : EngineApiRequest {};
struct EngineBindPredicateResult : EngineApiResult {};
EngineBindPredicateResult EngineBindPredicate(const EngineBindPredicateRequest& request);

struct EngineEvaluatePredicateRequest : EngineApiRequest {
  EngineComparisonPredicateOperator predicate_operator{
      EngineComparisonPredicateOperator::unspecified};
  EnginePredicateConsumer consumer{EnginePredicateConsumer::unspecified};
  EngineTypedValue left_value;
  EngineTypedValue right_value;
  EngineSqlTruthValue left_truth{EngineSqlTruthValue::unspecified};
  EngineSqlTruthValue right_truth{EngineSqlTruthValue::unspecified};
  EngineDescriptor result_descriptor;
};

struct EngineEvaluatePredicateResult : EngineApiResult {
  EngineSqlTruthValue truth_value{EngineSqlTruthValue::unknown};
  EngineTypedValue value;
  int comparison = 0;
  bool passes_consumer = false;
};

EngineEvaluatePredicateResult EngineEvaluatePredicate(
    const EngineEvaluatePredicateRequest& request);

}  // namespace scratchbird::engine::internal_api
