// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "sblr_engine_envelope.hpp"
#include "../internal_api/cluster/placement_api.hpp"
#include "../../core/uuid/uuid.hpp"
#include <algorithm>
#include <charconv>
#include <set>

namespace scratchbird::engine::sblr {
// Decode one admitted descriptor without allocating substitute identities.
// This representation check confers no placement, transaction or provider authority.
inline bool ReadNativeShardPlacementDescriptor(
    const SblrOperationEnvelope& envelope, const std::string& prefix,
    internal_api::EngineShardPlacementDescriptor* output) {
  if (!output) return false;
  internal_api::EngineShardPlacementDescriptor descriptor;
  std::set<std::string> seen;
  for (const auto& operand : envelope.operands) {
    if (!operand.name.starts_with(prefix)) continue;
    const auto field = std::string_view(operand.name).substr(prefix.size());
    internal_api::EngineUuid* identity = nullptr;
    if (field == "shard_uuid") identity = &descriptor.shard_uuid;
    else if (field == "source_filespace_uuid") identity = &descriptor.source_filespace_uuid;
    else if (field == "target_filespace_uuid") identity = &descriptor.target_filespace_uuid;
    else if (field != "range_begin" && field != "range_end" &&
             field != "placement_epoch" && field != "placement_generation") continue;
    if (!seen.insert(operand.name).second || !operand.value.empty() || operand.value_flags != 0)
      return false;
    if (identity) {
      if (operand.value_kind != SblrValueKind::uuid_ref || operand.value_body.size() != 16)
        return false;
      std::copy_n(operand.value_body.begin(),16,identity->bytes.begin());
      if (!core::uuid::IsEngineIdentityUuid(*identity)) return false;
      continue;
    }
    if (operand.value_kind != SblrValueKind::literal_typed || operand.value_body.size() < 24)
      return false;
    std::uint64_t size = 0;
    for (unsigned i=0;i<8;++i) size |= std::uint64_t(operand.value_body[16+i]) << (8*i);
    if (size != operand.value_body.size()-24) return false;
    const std::string_view value(reinterpret_cast<const char*>(operand.value_body.data()+24),size);
    if (field == "range_begin") descriptor.range_begin = value;
    else if (field == "range_end") descriptor.range_end = value;
    else {
      if (value.empty()) return false;
      std::uint64_t number = 0;
      const auto [end,error] = std::from_chars(value.data(),value.data()+value.size(),number);
      if (error != std::errc{} || end != value.data()+value.size()) return false;
      if (field == "placement_epoch") descriptor.placement_epoch = number;
      else descriptor.placement_generation = number;
    }
  }
  if (descriptor.shard_uuid.is_nil() || descriptor.target_filespace_uuid.is_nil()) return false;
  *output = std::move(descriptor);
  return true;
}
} // namespace scratchbird::engine::sblr
