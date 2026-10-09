// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "query/historical_timestamp_scalar.hpp"

namespace scratchbird::engine::internal_api {
// Projection after owned catalog/statement admission. No default cohort,
// timezone lookup, payload conversion, or grant of statement authority.
inline bool ProjectStoredHistoricalTimestampDescriptorV1(
    const EngineRequestContext& context, const EngineDescriptor& source,
    bool nullable, EngineDescriptor* output, std::string* detail) {
  if (!output || !detail) return false;
  const auto refuse = [&] { *detail = "CTI.TEMPORAL.DESCRIPTOR_INVALID"; return false; };
  if ((source.descriptor_kind != "scalar" && source.descriptor_kind != "canonical_type_descriptor") ||
      source.canonical_type_name != "timestamp" ||
      !core::uuid::IsEngineIdentityUuid(source.descriptor_uuid) ||
      !source.charset_uuid.is_nil() || !source.collation_uuid.is_nil()) return refuse();
  const auto binding = core::datatypes::LookupDatatypeTypeCodecIdentityV1(
      context.datatype_catalog_snapshot_uuid, context.datatype_catalog_generation,
      context.datatype_registry_generation, source.datatype_descriptor_uuid,
      source.datatype_descriptor_generation);
  if (!binding.ok || binding.row.type_uuid != source.type_uuid) return refuse();
  const auto& row = binding.row;
  CatalogColumnMetadata fields;
  if (!DecodeCatalogColumnMetadata(source.encoded_descriptor, &fields)) return refuse();
  CatalogColumnMetadata expected;
  expected.identities = {{"type_uuid", row.type_uuid},
                         {"datatype_descriptor_uuid", row.descriptor_uuid},
                         {"codec_uuid", row.codec_uuid}};
  if (fields.identities != expected.identities) return refuse();
  expected.text = {{"canonical", "timestamp"}, {"nullable", nullable ? "true" : "false"},
                   {"datatype_descriptor_generation", std::to_string(row.descriptor_generation)},
                   {"type_generation", std::to_string(row.type_generation)},
                   {"codec_id", row.codec_id}, {"codec_version", std::to_string(row.codec_version)},
                   {"codec_generation", std::to_string(row.codec_generation)},
                   {"null_encoding", std::to_string(row.null_encoding_code)}};
  // Frozen catalog declarations may retain this historical spelling. The
  // exact codec, not the label, determines whether it is the UTC tuple.
  if (fields.text.contains("canonical") && fields.text.at("canonical") == "timestamp_tz")
    expected.text["canonical"] = "timestamp_tz";
  if (fields.text.contains("timezone_profile_id")) expected.text["timezone_profile_id"] = "UTC";
  if (fields.text != expected.text) return refuse();
  RelationalTypeDescriptor descriptor;
  descriptor.descriptor_uuid = source.descriptor_uuid;
  descriptor.descriptor_generation = row.descriptor_generation;
  descriptor.type_uuid = row.type_uuid;
  descriptor.type_generation = row.type_generation;
  descriptor.codec_id = row.codec_id;
  descriptor.codec_version = row.codec_version;
  descriptor.codec_generation = row.codec_generation;
  descriptor.datatype_catalog_snapshot_uuid = row.catalog_snapshot_uuid;
  descriptor.datatype_catalog_generation = row.catalog_generation;
  descriptor.datatype_registry_generation = row.registry_generation;
  descriptor.statement_receipt_uuid = context.statement_receipt_uuid;
  descriptor.datatype_identity_authoritative = true;
  descriptor.nullability = nullable ? RelationalNullability::kNullable : RelationalNullability::kNonNull;
  return BuildHistoricalTimestampScalarDescriptorV1(descriptor, output, detail);
}
} // namespace scratchbird::engine::internal_api
