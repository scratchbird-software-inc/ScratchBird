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
#include <functional>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_MGA_EVENT_SEQUENCE_ALLOCATOR_AUTHORITY
// Owns durable, monotonically increasing event-sequence range allocation for
// MGA companion streams. Sequence values order stream records only; they are
// never transaction identity, visibility, or finality authority.
MgaEventSequenceRangeReservation ReserveEventSequenceRange(
    const EngineRequestContext& context,
    const std::string& stream_kind,
    const std::string& stream_path,
    std::uint64_t count,
    const std::function<std::uint64_t()>& bootstrap_loader,
    std::vector<std::string>* deferred_allocator_lines = nullptr);

void AbandonDeferredEventSequenceReservation(
    const MgaEventSequenceRangeReservation& reservation);

bool AppendDeferredEventSequenceAllocatorLines(
    const EngineRequestContext& context,
    std::vector<std::string>* lines,
    MgaRelationHotAppendCounters* counters);

std::uint64_t ScanNextRowEventSequence(const EngineRequestContext& context);
std::uint64_t NextRowEventSequence(const EngineRequestContext& context);
std::uint64_t ScanNextIndexEventSequence(const EngineRequestContext& context);
std::uint64_t NextIndexEventSequence(const EngineRequestContext& context);
std::uint64_t ScanNextMetadataEventSequence(const EngineRequestContext& context);
std::uint64_t NextMetadataEventSequence(const EngineRequestContext& context);

}  // namespace scratchbird::engine::internal_api
