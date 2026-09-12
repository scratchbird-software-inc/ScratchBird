// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "lowering/lowering.hpp"
#include "engine/sblr/relational_identity_codec.hpp"

namespace scratchbird::parser::sbsql {

inline std::optional<SblrOperand> MakeRelationalContextIdentityOperand(
    std::string_view name, const core::platform::Uuid& identity) {
  if (!engine::sblr::IsRelationalContextIdentitySlot(name) ||
      !core::uuid::IsEngineIdentityUuid(identity)) return std::nullopt;
  SblrOperand operand;
  operand.type = "uuid";
  operand.name = name;
  operand.canonical_value_kind = static_cast<std::uint16_t>(engine::sblr::SblrValueKind::uuid_ref);
  operand.canonical_value_body.assign(identity.bytes.begin(), identity.bytes.end());
  return operand;
}

}  // namespace scratchbird::parser::sbsql
