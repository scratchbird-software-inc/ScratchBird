// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace scratchbird::engine::sblr {
struct SblrOpcodeStream;
}

namespace scratchbird::server {

// Requires a successfully decoded canonical stream. Selects the single command
// with an optional validated SOURCE_MAP companion; does not alter any bytes or
// grant descriptor, receipt, executor, or routing authority.
std::optional<std::size_t> SelectServerSblrCommandIndex(
    const scratchbird::engine::sblr::SblrOpcodeStream& stream);

enum class LocalSblrGatewayDisposition : std::uint8_t {
  kPassThrough = 1,
  kHandled = 2,
  kAsyncAccepted = 3,
  kRefused = 4,
};

struct LocalSblrGatewayRequest {
  std::vector<std::uint8_t> canonical_sbos;
  std::uint16_t root_opcode_code = 0;
  std::string root_opcode;
  std::string root_operation_id;
  std::string route_snapshot_uuid;
  std::uint64_t route_epoch = 0;
  std::uint64_t route_generation = 0;
  std::string security_snapshot_uuid;
  std::uint64_t security_epoch = 0;
  std::uint64_t security_observation_generation = 0;
  bool route_snapshot_engine_owned = false;
  bool security_snapshot_engine_owned = false;
  bool cluster_context_active = false;
  bool cluster_transaction_active = false;
  bool route_fence_present = false;
};

struct LocalSblrGatewayDecision {
  bool ok = false;
  LocalSblrGatewayDisposition disposition =
      LocalSblrGatewayDisposition::kRefused;
  std::string diagnostic_id;
  std::array<std::uint8_t, 32> canonical_payload_sha256{};
  std::string route_snapshot_uuid;
  std::uint64_t route_epoch = 0;
  std::uint64_t route_generation = 0;
  std::string security_snapshot_uuid;
  std::uint64_t security_epoch = 0;
  std::uint64_t security_observation_generation = 0;
  std::uint64_t gateway_observation_generation = 0;
  bool cluster_context_active = false;
  bool cluster_transaction_active = false;
  bool route_fence_present = false;
};

LocalSblrGatewayDecision AdmitLocalNoClusterSblrGateway(
    const LocalSblrGatewayRequest& request);

}  // namespace scratchbird::server
