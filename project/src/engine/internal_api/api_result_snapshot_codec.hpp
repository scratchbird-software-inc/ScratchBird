// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "api_types.hpp"
#include <span>
namespace scratchbird::engine::internal_api {
// Private SAPI2 replay snapshot, not canonical IPC or execution authority.
// Malformed input leaves outputs unchanged; allocation failure propagates.
bool EncodeEngineApiResultSnapshot(const EngineApiResult&, std::vector<std::uint8_t>*);
bool DecodeEngineApiResultSnapshot(std::span<const std::uint8_t>, EngineApiResult*);
}
