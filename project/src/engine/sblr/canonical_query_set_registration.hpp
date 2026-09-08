// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "engine/executor/executor_foundation.hpp"
#include "engine/internal_api/api_types.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace scratchbird::engine::sblr {

namespace exec = scratchbird::engine::executor;

struct LiveSetNodeRegistrationProfile {
  exec::CanonicalSetOperationKind operation =
      exec::CanonicalSetOperationKind::kUnion;
  exec::CanonicalSetOperationAlignment alignment =
      exec::CanonicalSetOperationAlignment::kOrdinal;
  exec::CanonicalSetOperationQuantifier quantifier =
      exec::CanonicalSetOperationQuantifier::kAll;
  exec::CanonicalSetOperationEqualityProfile equality_profile =
      exec::CanonicalSetOperationEqualityProfile::kExactTyped;
  exec::CanonicalSetOperationTypeProfile type_profile =
      exec::CanonicalSetOperationTypeProfile::kExact;
  std::string implementation_id;
  std::vector<exec::ExecutorColumnDescriptor> result_columns;
  std::vector<exec::CanonicalSetOperationCollationBinding>
      collation_bindings;
  std::size_t maximum_output_row_count{0};
  std::size_t maximum_equality_comparison_count{0};
};

using LiveSetRegistrationProfiles =
    std::unordered_map<std::uint64_t, LiveSetNodeRegistrationProfile>;

// Validates only the already-issued, operator-local runtime memory receipt.
// It does not select a plan, construct a snapshot, or publish transaction
// finality.
bool ValidateLiveSetMemoryReceipt(
    const exec::TypedPhysicalNodeDag& dag,
    const exec::PhysicalNodeRecord& node,
    const exec::CanonicalSetOperationAllResult& result,
    std::uint64_t* current_memory_bytes,
    std::uint64_t* peak_memory_bytes,
    std::string* detail);

bool CanonicalSetOperationExecutionReceiptMatches(
    const exec::CanonicalSetOperationAllRequest& request,
    const exec::PhysicalNodeRecord& node,
    const exec::CanonicalSetOperationAllResult& result);

// Registers a binary set operation over two bounded, already-materialized
// typed inputs. The callback may only construct the revalidation handle for
// the engine-selected MGA statement context.
exec::CanonicalPhysicalExecutorRegistration MakeLiveSetOperationRegistration(
    LiveSetRegistrationProfiles prepared_set_nodes,
    std::string implementation_id,
    std::string capability_uuid,
    scratchbird::engine::internal_api::EngineRequestContext mga_context);

}  // namespace scratchbird::engine::sblr
