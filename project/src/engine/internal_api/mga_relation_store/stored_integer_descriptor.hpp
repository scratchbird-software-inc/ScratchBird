// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "api_types.hpp"
#include "catalog/column_metadata_codec.hpp"
#include "datatype_catalog_manifest.hpp"
#include "core/uuid/uuid.hpp"

namespace scratchbird::engine::internal_api {

// Pure projection after owned catalog acquisition and statement admission.
// Never supplies a default cohort, grants authority, or interprets payloads.
// Storage declaration attributes are removed only after all supplied type,
// codec and nullability evidence has been checked against the exact cohort.
inline bool ProjectStoredIntegerDescriptorV1(
    const EngineRequestContext& context, const EngineDescriptor& source,
    bool nullable, EngineDescriptor* output, std::string* detail) {
  if (!output || !detail) return false;
  const auto refuse = [&](const char* reason) { *detail = reason; return false; };
  namespace dt = scratchbird::core::datatypes;
  const auto type = source.canonical_type_name == "int32" ? dt::CanonicalTypeId::int32
      : source.canonical_type_name == "int64" ? dt::CanonicalTypeId::int64
                                            : dt::CanonicalTypeId::unknown;
  if ((source.descriptor_kind != "scalar" && source.descriptor_kind != "executor.scalar" &&
       source.descriptor_kind != "canonical_type_descriptor") ||
      !core::uuid::IsEngineIdentityUuid(source.descriptor_uuid) ||
      type == dt::CanonicalTypeId::unknown ||
      !source.charset_uuid.is_nil() || !source.collation_uuid.is_nil())
    return refuse("stored signed integer occurrence identity or resources are invalid");
  const auto lookup = dt::LookupDatatypeTypeCodecIdentityV3(
      context.datatype_catalog_snapshot_uuid, context.datatype_catalog_generation,
      context.datatype_registry_generation, source.datatype_descriptor_uuid,
      source.datatype_descriptor_generation);
  const auto& row = lookup.row.legacy_fields;
  if (!lookup.ok || row.type_uuid != source.type_uuid ||
      row.canonical_binary_type_code !=
          static_cast<std::uint32_t>(type) ||
      row.canonical_value_exact_bytes != (type == dt::CanonicalTypeId::int32 ? 4 : 8))
    return refuse("stored signed integer datatype is not bound to the supplied catalog cohort");
  // Slot NULL is an external containing state with no payload. The codec's
  // null_supported flag describes its own value carrier, not column nullability.
  CatalogColumnMetadata fields;
  if (!AdmitCatalogColumnMetadata(source.encoded_descriptor, &fields))
    return refuse("stored signed integer metadata is malformed");
  for (const auto& [name, identity] : fields.identities) {
    if ((name == "type_uuid" && identity == row.type_uuid) ||
        (name == "datatype_descriptor_uuid" && identity == row.descriptor_uuid) ||
        (name == "codec_uuid" && identity == row.codec_uuid && !identity.is_nil())) continue;
    return refuse("stored signed integer identity metadata disagrees with its binding");
  }
  bool nullability_present = false;
  for (const auto& [name, value] : fields.text) {
    if (name == "nullable" || name == "nullability" || name == "not_null") {
      const auto expected = name == "nullability" ? (nullable ? "nullable" : "non_null") :
          name == "nullable" ? (nullable ? "true" : "false") : (nullable ? "false" : "true");
      if (value != expected) return refuse("stored signed integer nullability authorities disagree");
      if (name != "not_null") nullability_present = true;
      continue;
    }
    if (name == "canonical" || name == "canonical_type" || name == "type") {
      if (value != source.canonical_type_name)
        return refuse("stored signed integer declaration labels disagree");
      continue;
    }
    if ((name == "datatype_descriptor_generation" && value == std::to_string(row.descriptor_generation)) ||
        (name == "type_generation" && value == std::to_string(row.type_generation)) ||
        (name == "codec_id" && value == row.codec_id) ||
        (name == "codec_version" && value == std::to_string(row.codec_version)) ||
        (name == "codec_generation" && value == std::to_string(row.codec_generation)) ||
        (name == "null_encoding" && value == std::to_string(row.null_encoding_code))) continue;
    // Only declaration properties with no scalar execution semantics can be
    // omitted. In particular, domains, policies and unknown modifiers refuse.
    if (name == "primary_key" || name == "pk" || name == "unique" ||
        name == "generated" || name == "identity" || name == "default" ||
        name == "default_value") continue;
    return refuse("stored signed integer has unsupported or conflicting execution metadata");
  }
  if (!nullability_present) return refuse("stored signed integer nullability authority is absent");
  auto staged = source;
  staged.descriptor_kind = "scalar";
  staged.encoded_descriptor = nullable ? "nullability=nullable" : "nullability=non_null";
  *output = std::move(staged);
  detail->clear();
  return true;
}
}  // namespace scratchbird::engine::internal_api
