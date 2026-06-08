// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "query/predicate_api.hpp"

#include "behavior_support/api_behavior_store.hpp"

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_QUERY_PREDICATE_API_BEHAVIOR
EngineBindPredicateResult EngineBindPredicate(const EngineBindPredicateRequest& request) {
  auto result = MakeApiBehaviorSuccess<EngineBindPredicateResult>(request.context, "query.bind_predicate");
  AddApiBehaviorEvidence(&result, "query_binding", "predicate");
  AddApiBehaviorRow(&result, {{"predicate_kind", request.predicate.predicate_kind}, {"predicate_envelope", request.predicate.canonical_predicate_envelope}, {"bound_value_count", std::to_string(request.predicate.bound_values.size())}});
  return result;
}

}  // namespace scratchbird::engine::internal_api
