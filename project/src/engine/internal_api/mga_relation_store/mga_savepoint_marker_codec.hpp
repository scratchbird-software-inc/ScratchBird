// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace scratchbird::engine::internal_api {

// Named SQL boundaries carry labels; native boundaries carry UUID binary(16).
// Internal map keys are not the durable representation.
struct MgaSavepointMarkerRecord {
  std::uint8_t kind = 0; // 1=create, 2=release, 3=rollback
  bool uuid_identity = false;
  std::string identity;
  std::uint64_t transaction = 0;
  std::uint64_t cutoffs[3]{};
  std::uint64_t upper[3]{};
};

inline constexpr unsigned char kMgaSavepointFrameLead = 0x89;
inline constexpr std::size_t kMgaSavepointFrameMaximum = 1024 * 1024;
std::string EncodeMgaSavepointMarker(const MgaSavepointMarkerRecord& record);
bool DecodeMgaSavepointMarker(std::string_view bytes,
                             MgaSavepointMarkerRecord* record);
// Zero means incomplete header; UINT32_MAX means invalid framing.
std::uint32_t MgaSavepointMarkerFrameSize(std::string_view prefix);
std::string MgaSavepointUuidKey(std::string_view uuid);

}  // namespace scratchbird::engine::internal_api
