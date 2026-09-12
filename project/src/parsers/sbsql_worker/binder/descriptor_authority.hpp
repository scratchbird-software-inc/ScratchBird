// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "binder.hpp"
#include "core/datatypes/datatype_catalog_manifest.hpp"
#include "wire/parser_server_ipc/parser_client_types.hpp"
#include "core/uuid/uuid.hpp"

namespace scratchbird::parser::sbsql {

// This is a consistency projection of an already acquired engine context,
// never an issuer or an authorization check. The canonical dispatcher must
// compare the resulting full tuple with its live receipt before execution.
inline bool PreserveNativeDescriptorAuthority(
    NativeDescriptorBindingInput* descriptor,
    const ipc::ParserStatementContext& context) {
  namespace dt = core::datatypes;
  if (!descriptor) return false;
  const auto valid_uuid = [](const core::platform::Uuid& value) {
    return core::uuid::IsEngineIdentityUuid(value);
  };
  if (!valid_uuid(descriptor->descriptor_uuid) ||
      !valid_uuid(descriptor->type_uuid) ||
      !valid_uuid(context.literal_preliminary_receipt_uuid) ||
      !valid_uuid(context.literal_catalog_snapshot_uuid) ||
      context.literal_catalog_generation == 0 ||
      (descriptor->nullability != BoundNullability::kNonNull &&
       descriptor->nullability != BoundNullability::kNullable)) return false;

  // Read the one immutable registry, using the actual projected snapshot.
  // Do not guess a registry generation, use a synthetic snapshot, or select a
  // type by its name. More than one row requires richer projection, not a
  // nearest-generation choice. The engine checks the selected registry
  // generation against its independently retained live receipt.
  const dt::DatatypeTypeCodecIdentityRowV1* identity = nullptr;
  for (const auto& row : dt::CurrentDatatypeTypeCodecIdentityRowsV1()) {
    if (row.catalog_snapshot_uuid != context.literal_catalog_snapshot_uuid ||
        row.catalog_generation != context.literal_catalog_generation ||
        row.type_uuid != descriptor->type_uuid) continue;
    if (identity) return false;
    identity = &row;
  }
  if (!identity) return false;
  const bool any_authority = descriptor->descriptor_generation != 0 ||
      descriptor->type_generation != 0 || !descriptor->codec_id.empty() ||
      descriptor->codec_version != 0 || descriptor->codec_generation != 0 ||
      !descriptor->statement_receipt_uuid.is_nil() ||
      !descriptor->datatype_catalog_snapshot_uuid.is_nil() ||
      descriptor->datatype_catalog_generation != 0 ||
      descriptor->datatype_registry_generation != 0;
  if (any_authority) {
    // Partial or contradictory authority is never repaired by filling defaults.
    return descriptor->descriptor_generation == identity->descriptor_generation &&
        descriptor->type_generation == identity->type_generation &&
        descriptor->codec_id == identity->codec_id &&
        descriptor->codec_version == identity->codec_version &&
        descriptor->codec_generation == identity->codec_generation &&
        descriptor->statement_receipt_uuid == context.literal_preliminary_receipt_uuid &&
        descriptor->datatype_catalog_snapshot_uuid == identity->catalog_snapshot_uuid &&
        descriptor->datatype_catalog_generation == identity->catalog_generation &&
        descriptor->datatype_registry_generation == identity->registry_generation;
  }
  const bool nullable = descriptor->nullability == BoundNullability::kNullable;
  std::size_t literal_matches = 0;
  for (const auto& profile : context.literal_statement_descriptor_profiles) {
    if (profile.binding_descriptor_uuid != descriptor->descriptor_uuid) continue;
    ++literal_matches;
    if (profile.profile_version != 1 ||
        profile.statement_receipt_uuid != context.literal_preliminary_receipt_uuid ||
        profile.catalog_snapshot_uuid != identity->catalog_snapshot_uuid ||
        profile.catalog_generation != identity->catalog_generation ||
        profile.descriptor_uuid != identity->descriptor_uuid ||
        profile.descriptor_generation != identity->descriptor_generation ||
        profile.type_uuid != identity->type_uuid ||
        profile.codec_id != identity->codec_id ||
        profile.codec_version != identity->codec_version ||
        profile.codec_generation != identity->codec_generation ||
        profile.nullable != nullable) return false;
  }
  if (literal_matches > 1) return false;
  if (literal_matches == 0) {
    std::size_t matches = 0;
    for (const auto& profile : context.descriptor_profiles) {
      if (profile.descriptor_uuid != descriptor->descriptor_uuid) continue;
      ++matches;
      if (profile.type_uuid != identity->type_uuid || profile.nullable != nullable ||
          profile.collation_uuid != descriptor->collation_uuid.value_or(core::platform::Uuid{}) ||
          profile.width != descriptor->width_precision_scale.width.value_or(0) ||
          profile.precision != descriptor->width_precision_scale.precision.value_or(0) ||
          profile.scale != descriptor->width_precision_scale.scale.value_or(0) ||
          descriptor->timezone_profile_id.has_value()) return false;
    }
    if (matches != 1) return false;
  }
  // Stage all allocating copies before touching the caller's descriptor.
  auto projected = *descriptor;
  projected.descriptor_generation = identity->descriptor_generation;
  projected.type_generation = identity->type_generation;
  projected.codec_id = identity->codec_id;
  projected.codec_version = identity->codec_version;
  projected.codec_generation = identity->codec_generation;
  projected.statement_receipt_uuid = context.literal_preliminary_receipt_uuid;
  projected.datatype_catalog_snapshot_uuid = identity->catalog_snapshot_uuid;
  projected.datatype_catalog_generation = identity->catalog_generation;
  projected.datatype_registry_generation = identity->registry_generation;
  *descriptor = std::move(projected);
  return true;
}

// SBXN numeric proof must retain the exact authoritative lowering record.
// Numeric scalar literals have no collation, timezone or declared width;
// DECIMAL precision/scale come from its canonical literal encoder.
inline bool MatchesNativeNumericDescriptorRecord(
    std::span<const std::string_view> fields,
    const NativeDescriptorBindingInput& descriptor) {
  const auto optional_number = [](const std::optional<std::uint32_t>& n) {
    return n ? std::to_string(*n) : std::string("-");
  };
  return fields.size() == 17 &&
      descriptor.nullability == BoundNullability::kNonNull &&
      !descriptor.collation_uuid && !descriptor.timezone_profile_id &&
      !descriptor.width_precision_scale.width &&
      descriptor.descriptor_generation != 0 && descriptor.type_generation != 0 &&
      descriptor.codec_version != 0 && descriptor.codec_generation != 0 &&
      descriptor.datatype_catalog_generation != 0 && descriptor.datatype_registry_generation != 0 &&
      fields[0] == descriptor.descriptor_uuid &&
      fields[1] == std::to_string(descriptor.descriptor_generation) &&
      fields[2] == descriptor.type_uuid &&
      fields[3] == std::to_string(descriptor.type_generation) &&
      fields[4] == descriptor.codec_id &&
      fields[5] == std::to_string(descriptor.codec_version) &&
      fields[6] == std::to_string(descriptor.codec_generation) &&
      fields[7] == "0" && fields[8] == "-" && fields[9] == "-" && fields[10] == "-" &&
      fields[11] == optional_number(descriptor.width_precision_scale.precision) &&
      fields[12] == optional_number(descriptor.width_precision_scale.scale) &&
      fields[13] == descriptor.statement_receipt_uuid &&
      fields[14] == descriptor.datatype_catalog_snapshot_uuid &&
      fields[15] == std::to_string(descriptor.datatype_catalog_generation) &&
      fields[16] == std::to_string(descriptor.datatype_registry_generation);
}

}  // namespace scratchbird::parser::sbsql
