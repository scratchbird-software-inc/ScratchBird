// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "sblr_engine_envelope.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::sblr {

inline constexpr std::uint32_t kSblrOpcodeStreamMagic = 0x534f4253u;
inline constexpr std::uint32_t kSblrOpcodeStreamTrailerMagic = 0x54534253u;
inline constexpr std::uint16_t kSblrOpcodeStreamHeaderSize = 64;
inline constexpr std::uint16_t kSblrOpcodeStreamTrailerSize = 16;

struct SblrOpcodeStream {
  std::string package_descriptor_uuid;
  std::string registry_snapshot_uuid;
  std::vector<SblrOperationEnvelope> operations;
};

struct SblrOpcodeStreamResult {
  bool ok = false;
  SblrOpcodeStream stream;
  std::vector<std::uint8_t> canonical_bytes;
  std::string diagnostic_id;
  std::string detail;
};

struct SblrOpcodeStreamAdmission {
  std::string admitted_registry_snapshot_uuid;
  bool authenticated = false;
  bool descriptor_class_accepted = false;
  bool gateway_pass_through = false;
  bool executor_evidence_accepted = false;
  bool cancelled = false;
  bool resource_budget_available = true;
};

std::vector<std::uint8_t> EncodeSblrOpcodeStream(
    const SblrOpcodeStream& stream);
SblrOpcodeStreamResult DecodeSblrOpcodeStream(std::string_view bytes);
SblrOpcodeStreamResult AdmitSblrOpcodeStream(
    std::string_view bytes,
    const SblrOpcodeStreamAdmission& admission);

}  // namespace scratchbird::engine::sblr
