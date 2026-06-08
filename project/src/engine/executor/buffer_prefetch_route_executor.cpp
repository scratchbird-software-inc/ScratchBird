// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "buffer_prefetch_route_executor.hpp"

namespace scratchbird::engine::executor {

scratchbird::storage::page::BufferPrefetchReadaheadResult
ConsumeBufferPrefetchReadaheadRoute(
    const scratchbird::storage::page::BufferPrefetchReadaheadRequest& request) {
  auto result =
      scratchbird::storage::page::EvaluateBufferPrefetchReadaheadRoute(request);
  result.evidence.push_back(
      "executor.buffer_prefetch.route_consumed=true");
  result.evidence.push_back("executor.buffer_prefetch.route_label=" +
                            request.route_label);
  result.evidence.push_back(
      "executor.buffer_prefetch.prefetch_visibility_authority=false");
  result.evidence.push_back(
      "executor.buffer_prefetch.prefetch_finality_authority=false");
  result.evidence.push_back(
      "executor.buffer_prefetch.prefetch_recovery_authority=false");
  result.evidence.push_back(
      "executor.buffer_prefetch.prefetch_security_authority=false");
  result.evidence.push_back(
      "executor.buffer_prefetch.mga_inventory_remains_authority=true");
  result.evidence.push_back(
      "executor.buffer_prefetch.security_recheck_preserved=true");
  return result;
}

}  // namespace scratchbird::engine::executor
