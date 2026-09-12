// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/uuid/uuid.hpp"
#include "engine/sblr/sblr_engine_envelope.hpp"
#include <algorithm>
#include <array>
#include <string_view>

namespace scratchbird::engine::sblr {

inline constexpr std::array<std::string_view, 7> kRelationalContextIdentitySlots{
    "relational_bound_sblr_tree_uuid", "relational_catalog_epoch_uuid",
    "relational_security_context_uuid", "relational_statement_uuid",
    "relational_owning_transaction_uuid", "relational_statement_snapshot_uuid",
    "relational_statement_metadata_snapshot_uuid"};

inline bool IsRelationalContextIdentitySlot(std::string_view name) noexcept {
  return std::find(kRelationalContextIdentitySlots.begin(),
                   kRelationalContextIdentitySlots.end(), name) !=
      kRelationalContextIdentitySlots.end();
}

// Structural reference decoding only. The owning query admission still
// enforces order, exact receipt/MGA context and ownership. No text fallback.
inline bool DecodeRelationalContextIdentity(
    std::string_view type, std::string_view name, SblrValueKind kind,
    const std::uint8_t* data, std::size_t size,
    core::platform::Uuid* output) noexcept {
  if (!output || type != "uuid" || !IsRelationalContextIdentitySlot(name) ||
      kind != SblrValueKind::uuid_ref || size != 16 || !data) return false;
  core::platform::Uuid decoded;
  std::copy_n(data, 16, decoded.bytes.begin());
  if (!core::uuid::IsEngineIdentityUuid(decoded)) return false;
  *output = decoded;
  return true;
}

}  // namespace scratchbird::engine::sblr
