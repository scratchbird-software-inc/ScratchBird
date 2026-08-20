// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "sblr_engine_envelope.hpp"
#include "api_types.hpp"
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
struct SblrLocalBackupArchiveCodecResult { bool ok=false; std::vector<std::uint8_t> bytes; std::string diagnostic_id; std::string detail; };
struct SblrLocalBackupArchiveDispatchResult { bool accepted=false; std::string diagnostic_id; std::string detail; std::vector<scratchbird::engine::internal_api::EngineEvidenceReference> evidence; };
SblrLocalBackupArchiveCodecResult EncodeSblrLocalBackupArchiveFrame(std::uint16_t opcode, const std::vector<std::uint8_t>& payload);
SblrLocalBackupArchiveCodecResult DecodeSblrLocalBackupArchiveFrame(std::uint16_t opcode, const std::uint8_t* data, std::size_t size);
bool IsSblrLocalBackupArchiveOperation(std::string_view operation_id) noexcept;
SblrLocalBackupArchiveDispatchResult DispatchSblrLocalBackupArchive(const SblrOperationEnvelope& envelope, const scratchbird::engine::internal_api::EngineRequestContext& context);
}
