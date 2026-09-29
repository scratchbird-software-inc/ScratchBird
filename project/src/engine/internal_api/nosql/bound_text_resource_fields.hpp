// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog/column_metadata_codec.hpp"
#include "catalog/name_resolution_api.hpp"
#include "mga_relation_store/mga_relation_descriptor.hpp"

namespace scratchbird::engine::internal_api {
// Model text payloads may be unadorned canonical UTF8 or carry an explicit
// catalog resource cohort. Reconstruct expected fields from live authority;
// callers still compare the entire metadata map, rejecting unknown fields.
inline bool BindExactModelTextResourceFields(
    const EngineRequestContext& context,
    const MgaRelationColumnStorageDescriptor& column,
    std::uint64_t codec_capacity, CatalogColumnMetadata* expected) {
  if (!expected) return false;
  if (column.charset_uuid.is_nil() && column.collation_uuid.is_nil())
    return column.character_length == 0;
  if (column.charset_uuid.is_nil() || column.collation_uuid.is_nil() ||
      !context.resource_epoch || !column.character_length ||
      column.character_length > codec_capacity) return false;
  const auto charset = LookupEngineResourceDescriptorByUuid(context, column.charset_uuid, "charset");
  const auto collation = LookupEngineResourceDescriptorByUuid(context, column.collation_uuid, "collation");
  if (!charset.ok || !collation.ok) return false;
  const auto& ch = charset.resource_descriptor;
  const auto& co = collation.resource_descriptor;
  if (!ch.present || !co.present || ch.database_uuid != context.database_uuid ||
      co.database_uuid != context.database_uuid || ch.resource_uuid != column.charset_uuid ||
      co.resource_uuid != column.collation_uuid || co.parent_resource_uuid != ch.resource_uuid ||
      ch.resource_epoch != context.resource_epoch || co.resource_epoch != context.resource_epoch ||
      !ch.family_epoch || !co.family_epoch) return false;
  expected->identities.emplace("charset_uuid", ch.resource_uuid);
  expected->identities.emplace("collation_uuid", co.resource_uuid);
  expected->text.emplace("charset_generation", std::to_string(ch.family_epoch));
  expected->text.emplace("collation_generation", std::to_string(co.family_epoch));
  expected->text.emplace("resource_epoch", std::to_string(context.resource_epoch));
  expected->text.emplace("character_length", std::to_string(column.character_length));
  return true;
}
}  // namespace scratchbird::engine::internal_api
