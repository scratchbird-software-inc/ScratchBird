// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "mga_savepoint_store.hpp"
namespace scratchbird::engine::internal_api::savepoint_replay {
void ConsumeMarkerBytes(std::string*, SavepointParsedState*, bool final);
void NormalizeSavepointRowRollbackRanges(SavepointParsedState*);
}
