// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::sblr {

using ProceduralBodyUuid = std::array<std::uint8_t, 16>;
using ProceduralBodySha256 = std::array<std::uint8_t, 32>;

constexpr std::uint16_t kProceduralBodyNullProfileV1 = 1;
constexpr std::uint16_t kProceduralBodyPsqlNullNodeKindV1 = 1;
constexpr std::size_t kSblrProceduralBodyV1Bytes = 288;

// Engine-owned immutable procedure-body artifact. V1 deliberately admits one
// closed body profile only: a single typed psql_null_stmt node. The artifact is
// persisted as canonical bytes and addressed by UUID, generation, and SHA-256;
// parser text is never retained as execution authority.
struct SblrProceduralBodyV1 {
  ProceduralBodyUuid body_uuid{};
  std::uint64_t body_generation{0};
  ProceduralBodyUuid procedure_uuid{};
  std::uint64_t procedure_generation{0};
  std::uint64_t node_executor_availability_generation{0};
  ProceduralBodySha256 node_identity_sha256{};
  ProceduralBodySha256 node_descriptor_sha256{};
  ProceduralBodySha256 node_executor_sha256{};
  ProceduralBodySha256 node_result_sha256{};
  ProceduralBodySha256 effect_set_sha256{};
  ProceduralBodySha256 evidence_sha256{};
};

SblrProceduralBodyV1 MakeSblrPsqlNullProceduralBodyV1(
    const ProceduralBodyUuid& body_uuid,
    std::uint64_t body_generation,
    const ProceduralBodyUuid& procedure_uuid,
    std::uint64_t procedure_generation,
    std::uint64_t node_executor_availability_generation);

std::vector<std::uint8_t> EncodeSblrProceduralBodyV1(
    const SblrProceduralBodyV1& value);

bool DecodeSblrProceduralBodyV1(const std::uint8_t* bytes,
                                std::size_t size,
                                SblrProceduralBodyV1* out,
                                std::string* detail);

}  // namespace scratchbird::engine::sblr
