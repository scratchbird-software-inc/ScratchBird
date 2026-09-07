// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "engine/executor/descriptor_value_runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace scratchbird::engine::sblr {

// Owns overflow-safe query runtime memory accounting over already-materialized
// descriptor values. It does not grant memory, select plans, or publish MGA
// visibility.
bool CheckedAdd(std::uint64_t left,
                std::uint64_t right,
                std::uint64_t* result);
bool CheckedMultiply(std::uint64_t left,
                     std::uint64_t right,
                     std::uint64_t* result);

bool RuntimeMaterializedBatchMemoryBytes(
    const scratchbird::engine::executor::DescriptorBatch& batch,
    std::uint64_t* bytes,
    scratchbird::engine::executor::DescriptorCancellationProbe
        cancellation_requested = nullptr,
    const void* cancellation_context = nullptr,
    bool* cancellation_observed = nullptr,
    bool* cancellation_probe_failed = nullptr);

bool RuntimeTypedValueMemoryBytes(
    const scratchbird::engine::internal_api::EngineTypedValue& value,
    std::uint64_t* bytes);

void PublishRuntimeMemoryObservation(
    scratchbird::engine::executor::CanonicalPhysicalDispatchStepResult* step,
    std::uint64_t current_memory_bytes,
    std::uint64_t peak_memory_bytes);

bool AddBatchMemoryBytes(
    const scratchbird::engine::executor::DescriptorBatch& batch,
    std::uint64_t* memory_bytes,
    scratchbird::engine::executor::DescriptorCancellationProbe
        cancellation_requested = nullptr,
    const void* cancellation_context = nullptr,
    bool* cancellation_observed = nullptr,
    bool* cancellation_probe_failed = nullptr);

bool BoundDescriptorBatchLiveMemoryBytes(
    const scratchbird::engine::executor::DescriptorBatch& batch,
    std::uint64_t* memory_bytes);

bool QueryDistinctAuxiliaryMemoryBytes(std::size_t row_count,
                                       std::size_t column_count,
                                       std::uint64_t* memory_bytes);

bool QueryDistinctOutputMemoryBytes(
    const scratchbird::engine::executor::DescriptorBatch& batch,
    const std::vector<scratchbird::engine::executor::
                          CanonicalDescriptorOrderTerm>& equality_terms,
    std::size_t maximum_value_comparisons,
    std::uint64_t* memory_bytes,
    std::string* detail);

bool AddBatchProjectionMemoryBytes(
    const scratchbird::engine::executor::DescriptorBatch& batch,
    const std::vector<std::size_t>& projected_columns,
    std::uint64_t* memory_bytes);

bool AddBatchRowRangeMemoryBytes(
    const scratchbird::engine::executor::DescriptorBatch& batch,
    std::size_t first_row,
    std::size_t row_count,
    std::uint64_t* memory_bytes);

bool LogicalBitVectorPayloadBytes(std::size_t bit_count,
                                  std::uint64_t* bytes);

std::uint64_t CanonicalUnsignedDecimalWidth(std::uint64_t value);

std::optional<std::size_t> SelectedNodeAggregateMemoryBound(
    const scratchbird::engine::executor::TypedPhysicalNodeDag& dag,
    const scratchbird::engine::executor::PhysicalNodeRecord& node);

}  // namespace scratchbird::engine::sblr
