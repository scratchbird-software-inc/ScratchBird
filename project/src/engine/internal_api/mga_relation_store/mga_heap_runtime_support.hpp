// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "mga_relation_store/mga_relation_store.hpp"

#include <cstdint>
#include <string>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_MGA_HEAP_RUNTIME_SUPPORT_INTERFACE
// Narrow executor-facing observation and memory-accounting interface. This
// interface reports work performed by the MGA row reader; it does not own row
// visibility, transaction state, or durable publication.
struct HeapReadRuntimeObservation {
  std::uint64_t operator_wait_ns = 0;
  std::uint64_t storage_bytes_read = 0;
  std::uint64_t decoded_bytes = 0;
  bool complete = true;
};

bool HeapReadMemoryAdd(std::uint64_t value, std::uint64_t* total);
bool HeapReadMemoryMultiply(std::uint64_t left,
                            std::uint64_t right,
                            std::uint64_t* product);
bool AddHeapReadOwnedStringMemory(const std::string& value,
                                  std::uint64_t* total);
bool AccountHeapReadEngineDescriptorMemory(
    const EngineDescriptor& descriptor,
    std::uint64_t* total);

MgaVisibleHeapRelationReadResult ReadVisibleMgaHeapRelationWithObservation(
    const EngineRequestContext& context,
    const MgaVisibleHeapRelationReadRequest& request,
    HeapReadRuntimeObservation* runtime_observation,
    const PreparedMgaHeapReadAuthority* prepared_authority = nullptr);

MgaVisibleHeapRelationCountResult CountVisibleMgaHeapRelationWithObservation(
    const EngineRequestContext& context,
    const MgaVisibleHeapRelationCountRequest& request,
    HeapReadRuntimeObservation* runtime_observation,
    const PreparedMgaHeapReadAuthority* prepared_authority = nullptr);

}  // namespace scratchbird::engine::internal_api
