// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

inline constexpr char kSblrProceduralBlockV1Contract[] =
    "sblr.procedural.block.v1";

enum class EngineProceduralSlotKind {
  local,
  result,
};

enum class EngineProceduralType {
  character,
  int32,
};

struct EngineProceduralSlotV1 {
  std::string slot_id;
  EngineProceduralSlotKind kind = EngineProceduralSlotKind::local;
  EngineProceduralType type = EngineProceduralType::character;
  std::uint32_t character_length = 0;
  bool nullable = true;
};

enum class EngineProceduralInstructionKind {
  assign,
};

enum class EngineProceduralExpressionKind {
  substring,
};

enum class EngineProceduralExpressionSourceKind {
  context_variable,
};

enum class EngineProceduralSubstringLengthKind {
  to_end,
};

struct EngineProceduralInstructionV1 {
  EngineProceduralInstructionKind kind =
      EngineProceduralInstructionKind::assign;
  std::string target_slot_id;
  EngineProceduralExpressionKind expression_kind =
      EngineProceduralExpressionKind::substring;
  EngineProceduralExpressionSourceKind source_kind =
      EngineProceduralExpressionSourceKind::context_variable;
  std::string source_id;
  EngineProceduralType source_cast_type = EngineProceduralType::character;
  std::int64_t start_value = 1;
  EngineProceduralSubstringLengthKind length_kind =
      EngineProceduralSubstringLengthKind::to_end;
};

// This first contract is deliberately a zero-yield execution slice. Output
// aliases and wire descriptors remain parser-owned presentation metadata; the
// engine receives only canonical slot semantics needed for execution.
struct EngineProceduralBlockV1 {
  std::string contract = kSblrProceduralBlockV1Contract;
  std::string block_kind = "anonymous";
  std::uint32_t input_count = 0;
  std::uint32_t local_count = 0;
  std::uint32_t output_count = 0;
  std::uint32_t yield_count = 0;
  std::vector<EngineProceduralSlotV1> slots;
  std::vector<EngineProceduralInstructionV1> instructions;
};

}  // namespace scratchbird::engine::internal_api
