// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "mga_relation_store/mga_contextual_text_descriptor.hpp"

#include "api_diagnostics.hpp"
#include "catalog/name_resolution_api.hpp"
#include "catalog/column_metadata_codec.hpp"
#include "crud_support/crud_store.hpp"
#include "datatype_catalog_manifest.hpp"
#include "descriptor_value_runtime.hpp"
#include "uuid.hpp"
#include "mga_relation_store/mga_binary_identity_codec.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api {
namespace {

// SEARCH_KEY: SB_ENGINE_MGA_CONTEXTUAL_TEXT_DESCRIPTOR_IMPLEMENTATION_AUTHORITY
// Owns canonical relation-descriptor parsing, contextual-text identity
// validation/rewrite, public projection material, and sealed descriptor
// construction. It consumes policy and descriptor authority but owns neither
// transaction visibility nor finality.

constexpr EngineUuid kLegacyTextDescriptorUuid{{0x2c,0x01,0x00,0x00,0x63,0x68,0x71,0x72,0xa1,0x63,0x74,0x65,0x72,0x00,0x00,0x00}};
constexpr EngineUuid kLegacyTextTypeUuid{{0x2c,0x01,0x00,0x00,0x63,0x68,0x71,0x72,0xa1,0x63,0x74,0x65,0x72,0x00,0x00,0x00}};
constexpr EngineUuid kCanonicalTextDescriptorUuid{{0x01,0x9d,0x00,0x00,0x00,0x00,0x70,0x00,0x80,0x00,0x00,0x00,0x00,0x00,0xd7,0x18}};
constexpr EngineUuid kCanonicalTextTypeUuid{{0x01,0x9d,0x00,0x00,0x00,0x00,0x70,0x00,0x80,0x00,0x00,0x00,0x00,0x00,0xd7,0x19}};
constexpr EngineUuid kCanonicalTextCodecUuid{{0x01,0x9d,0x00,0x00,0x00,0x00,0x70,0x00,0x80,0x00,0x00,0x00,0x00,0x00,0xd7,0x1a}};
constexpr std::string_view kCanonicalTextCodecId =
    "datatype.text.utf8.v1";

constexpr EngineUuid kCanonicalTextDescriptorIdentity{{0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x18}};
constexpr EngineUuid kCanonicalTextTypeIdentity{{0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x19}};
constexpr EngineUuid kCanonicalTextCodecIdentity{{0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x1a}};

EngineApiDiagnostic OkDiagnostic() {
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

}  // namespace

std::string EncodeStringListAsCrudPairs(const std::vector<std::string>& values) {
  std::vector<std::pair<std::string, std::string>> pairs;
  for (std::size_t i = 0; i < values.size(); ++i) {
    pairs.push_back({std::to_string(i), values[i]});
  }
  return EncodeCrudPairs(pairs);
}

std::string RelationDescriptorTrimAscii(std::string value) {
  std::size_t first = 0;
  while (first < value.size() &&
         (value[first] == ' ' || value[first] == '\t' ||
          value[first] == '\n' || value[first] == '\r')) {
    ++first;
  }
  std::size_t last = value.size();
  while (last > first &&
         (value[last - 1] == ' ' || value[last - 1] == '\t' ||
          value[last - 1] == '\n' || value[last - 1] == '\r')) {
    --last;
  }
  return value.substr(first, last - first);
}

std::string RelationDescriptorLowerAscii(std::string value) {
  for (char& ch : value) {
    if (ch >= 'A' && ch <= 'Z') {
      ch = static_cast<char>(ch - 'A' + 'a');
    }
  }
  return value;
}

std::map<std::string, std::string> RelationDescriptorFields(const std::string& descriptor) {
  CatalogColumnMetadata fields;
  return AdmitCatalogColumnMetadata(descriptor, &fields) ? fields.text
      : std::map<std::string, std::string>{};
}
std::optional<std::map<std::string, std::string>>
StrictRelationDescriptorFields(const std::string& descriptor) {
  CatalogColumnMetadata fields;
  if (!DecodeCatalogColumnMetadata(descriptor, &fields)) return std::nullopt;
  return fields.text;
}
bool ReplaceExactRelationDescriptorIdentities(std::string* descriptor,
    const std::map<std::string, std::pair<EngineUuid, EngineUuid>>& replacements) {
  CatalogColumnMetadata fields;
  if (!descriptor || replacements.empty() || !DecodeCatalogColumnMetadata(*descriptor, &fields)) return false;
  for (const auto& [key, replacement] : replacements) {
    const auto it = fields.identities.find(key);
    if (it == fields.identities.end() || it->second != replacement.first) return false;
    it->second = replacement.second;
  }
  return EncodeCatalogColumnMetadata(fields, descriptor);
}
// Binary carriers only: fixed width bytes, never a textual UUID spelling.
bool CanonicalNonNilMigrationUuid(const std::string_view bytes) {
  EngineUuid id;
  if (bytes.size() != 16) return false;
  std::copy_n(reinterpret_cast<const std::uint8_t*>(bytes.data()), 16, id.bytes.begin());
  return core::uuid::IsEngineIdentityUuid(id);
}

bool CanonicalNonNilMigrationUuid(const EngineUuid& value) {
  return core::uuid::IsEngineIdentityUuid(value);
}

bool ExactCanonicalTextIdentityAuthorityAvailable(
    const EngineRequestContext& context) {
  if (!CanonicalNonNilMigrationUuid(
          context.datatype_catalog_snapshot_uuid) ||
      context.datatype_catalog_generation == 0 ||
      context.datatype_registry_generation == 0) {
    return false;
  }
  const auto identity =
      scratchbird::core::datatypes::LookupDatatypeTypeCodecIdentityV1(
          context.datatype_catalog_snapshot_uuid,
          context.datatype_catalog_generation,
          context.datatype_registry_generation,
          kCanonicalTextDescriptorIdentity, 1);
  return identity.ok &&
         identity.row.catalog_snapshot_uuid ==
             context.datatype_catalog_snapshot_uuid &&
         identity.row.catalog_generation ==
             context.datatype_catalog_generation &&
         identity.row.registry_generation ==
             context.datatype_registry_generation &&
         identity.row.descriptor_uuid == kCanonicalTextDescriptorIdentity &&
         identity.row.descriptor_generation == 1 &&
         identity.row.type_uuid == kCanonicalTextTypeIdentity &&
         identity.row.type_generation == 1 &&
         identity.row.codec_uuid == kCanonicalTextCodecIdentity &&
         identity.row.codec_id == kCanonicalTextCodecId &&
         identity.row.codec_version == 1 &&
         identity.row.codec_generation == 1 &&
         identity.row.canonical_name == "text" &&
         identity.row.null_supported &&
         identity.row.null_encoding_code == 1 &&
         identity.row.canonical_value_variable_width &&
         identity.row.canonical_value_exact_zero_is_width_marker &&
         identity.row.canonical_value_minimum_bytes == 0 &&
         identity.row.canonical_value_maximum_bytes == 16777216 &&
         identity.row.canonical_value_exact_bytes == 0 &&
         identity.row.canonical_charset == "UTF-8" &&
         identity.row.shortest_form_utf8_required &&
         !identity.row.implicit_normalization_allowed &&
         identity.row.descriptor_bound_collation_required &&
         identity.row.empty_value_distinct_from_sql_null &&
         identity.row.sql_null_requires_zero_payload &&
         identity.row.variable_width_storage_without_truncation &&
         identity.row.invalid_encoding_diagnostic_id ==
             "CTB.TEXT.INVALID_ENCODING";
}

bool ExactTextDescriptorResourceShape(
    const EngineRequestContext& context,
    const CatalogColumnMetadata& metadata) {
  const auto& fields = metadata.text;
  static const std::set<std::string> kAllowedFields{
      "type", "canonical", "nullable", "default", "column_uuid",
      "datatype_descriptor_uuid", "datatype_descriptor_generation",
      "type_uuid", "type_generation", "codec_uuid", "codec_id",
      "codec_version", "codec_generation", "null_encoding",
      "charset_uuid", "charset_generation", "collation_uuid",
      "collation_generation", "resource_epoch", "character_length",
      "primary_key", "pk", "unique", "unique_key", "generated",
      "identity", "domain_uuid", "candidate_key_constraint_uuid",
      "candidate_key_descriptor_uuid", "key_descriptor_uuid",
      "support_uuid", "support_index_uuid", "index_uuid",
      "support_family", "candidate_key_class"};
  for (const auto& [key, value] : fields) {
    (void)value;
    if (!kAllowedFields.contains(key)) return false;
  }
  for (const auto& [key, value] : metadata.identities) {
    if (!kAllowedFields.contains(key)) return false;
  }
  const auto type = fields.find("type");
  const auto canonical = fields.find("canonical");
  if ((type == fields.end()) == (canonical == fields.end())) return false;
  const auto& type_name =
      type != fields.end() ? type->second : canonical->second;
  if (RelationDescriptorLowerAscii(type_name) != "text") return false;

  const auto nullable = fields.find("nullable");
  if (nullable == fields.end() ||
      (nullable->second != "true" && nullable->second != "false") ||
      fields.contains("nullability")) {
    return false;
  }
  const auto charset = metadata.identities.find("charset_uuid");
  const auto charset_generation = fields.find("charset_generation");
  const auto collation = metadata.identities.find("collation_uuid");
  const auto collation_generation = fields.find("collation_generation");
  const auto resource_epoch = fields.find("resource_epoch");
  const bool has_resource_authority =
      charset != metadata.identities.end() || charset_generation != fields.end() ||
      collation != metadata.identities.end() || collation_generation != fields.end() ||
      resource_epoch != fields.end();
  if (has_resource_authority) {
    if (charset == metadata.identities.end() || charset_generation == fields.end() ||
        collation == metadata.identities.end() || collation_generation == fields.end() ||
        resource_epoch == fields.end()) {
      return false;
    }
    const auto parse_exact_u64 = [](const std::string& text,
                                    std::uint64_t* value) {
      if (value == nullptr || text.empty()) return false;
      const auto parsed = std::from_chars(
          text.data(), text.data() + text.size(), *value, 10);
      return parsed.ec == std::errc{} &&
             parsed.ptr == text.data() + text.size() && *value != 0 &&
             std::to_string(*value) == text;
    };
    std::uint64_t charset_generation_value = 0;
    std::uint64_t collation_generation_value = 0;
    std::uint64_t resource_epoch_value = 0;
    if (!parse_exact_u64(charset_generation->second,
                         &charset_generation_value) ||
        !parse_exact_u64(collation_generation->second,
                         &collation_generation_value) ||
        !parse_exact_u64(resource_epoch->second, &resource_epoch_value) ||
        resource_epoch_value != context.resource_epoch) {
      return false;
    }
    const EngineUuid charset_uuid = charset->second;
    const auto live_charset = LookupEngineResourceDescriptorByUuid(
        context, charset_uuid, "charset");
    const EngineUuid collation_uuid = collation->second;
    const auto live_collation = LookupEngineResourceDescriptorByUuid(
        context, collation_uuid, "collation");
    if (!live_charset.ok || !live_collation.ok ||
        live_charset.resource_descriptor.resource_uuid !=
            charset_uuid ||
        live_collation.resource_descriptor.resource_uuid !=
            collation_uuid ||
        live_collation.resource_descriptor.parent_resource_uuid !=
            charset_uuid ||
        live_charset.resource_descriptor.family_epoch !=
            charset_generation_value ||
        live_collation.resource_descriptor.family_epoch !=
            collation_generation_value ||
        live_charset.resource_descriptor.resource_epoch !=
            resource_epoch_value ||
        live_collation.resource_descriptor.resource_epoch !=
            resource_epoch_value) {
      return false;
    }
  }
  const auto length = fields.find("character_length");
  if (length != fields.end()) {
    std::uint64_t parsed = 0;
    const auto converted = std::from_chars(
        length->second.data(), length->second.data() + length->second.size(),
        parsed, 10);
    if (length->second.empty() || converted.ec != std::errc{} ||
        converted.ptr != length->second.data() + length->second.size() ||
        parsed == 0 || parsed > 16777216 ||
        std::to_string(parsed) != length->second) {
      return false;
    }
  }
  return true;
}

bool ExactCanonicalMigratedTextDescriptor(const EngineRequestContext& context,
    std::string_view descriptor, const EngineUuid& column_uuid) {
  CatalogColumnMetadata fields;
  if (!ExactCanonicalTextIdentityAuthorityAvailable(context) ||
      !CanonicalNonNilMigrationUuid(column_uuid) ||
      !DecodeCatalogColumnMetadata(descriptor, &fields) ||
      !ExactTextDescriptorResourceShape(context, fields)) return false;
  const auto exact = [&](const std::string& key, const std::string& value) {
    const auto it = fields.text.find(key);
    return it != fields.text.end() && it->second == value;
  };
  return BinaryCatalogUuid(fields, "column_uuid") == column_uuid &&
      BinaryCatalogUuid(fields, "datatype_descriptor_uuid") == kCanonicalTextDescriptorUuid &&
      BinaryCatalogUuid(fields, "type_uuid") == kCanonicalTextTypeUuid &&
      BinaryCatalogUuid(fields, "codec_uuid") == kCanonicalTextCodecUuid &&
      exact("datatype_descriptor_generation", "1") && exact("type_generation", "1") &&
      exact("codec_id", std::string(kCanonicalTextCodecId)) && exact("codec_version", "1") &&
      exact("codec_generation", "1") && exact("null_encoding", "1");
}
bool RewriteLegacyTextDescriptor(const EngineRequestContext& context,
    std::string* descriptor, const EngineUuid& column_uuid) {
  CatalogColumnMetadata fields;
  if (!descriptor || !ExactCanonicalTextIdentityAuthorityAvailable(context) ||
      !CanonicalNonNilMigrationUuid(column_uuid) ||
      !DecodeCatalogColumnMetadata(*descriptor, &fields) ||
      !ExactTextDescriptorResourceShape(context, fields) ||
      BinaryCatalogUuid(fields, "datatype_descriptor_uuid") != kLegacyTextDescriptorUuid ||
      BinaryCatalogUuid(fields, "type_uuid") != kLegacyTextTypeUuid) return false;
  if (fields.identities.contains("column_uuid") &&
      BinaryCatalogUuid(fields, "column_uuid") != column_uuid) return false;
  for (const char* key : {"datatype_descriptor_generation", "type_generation",
       "codec_uuid", "codec_id", "codec_version", "codec_generation", "null_encoding"}) {
    if (fields.text.contains(key) || fields.identities.contains(key)) return false;
  }
  fields.identities["column_uuid"] = column_uuid;
  fields.identities["datatype_descriptor_uuid"] = kCanonicalTextDescriptorUuid;
  fields.identities["type_uuid"] = kCanonicalTextTypeUuid;
  fields.identities["codec_uuid"] = kCanonicalTextCodecUuid;
  for (const char* key : {"datatype_descriptor_generation", "type_generation",
       "codec_version", "codec_generation", "null_encoding"}) fields.text[key] = "1";
  fields.text["codec_id"] = kCanonicalTextCodecId;
  std::string encoded;
  if (!EncodeCatalogColumnMetadata(fields, &encoded) ||
      !ExactCanonicalMigratedTextDescriptor(context, encoded, column_uuid)) return false;
  descriptor->swap(encoded);
  return true;
}

EngineApiDiagnostic ContextualTextMgaDiagnostic(std::string detail) {
  return MakeEngineApiDiagnostic(
      "CTB.TEXT.DESCRIPTOR_INVALID",
      "mga.contextual_text_sidecar_set_v2.invalid",
      std::move(detail), true);
}

bool CopyContextualUuidV2(const EngineUuid& value,
                          MgaContextualTextUuidV2* output,
                          const bool allow_nil) {
  if (!output || !(core::uuid::IsEngineIdentityUuid(value) ||
                   (allow_nil && value.is_nil()))) return false;
  *output = value.bytes;
  return true;
}

bool CopyContextualUuidV2(const std::string_view bytes,
    MgaContextualTextUuidV2* output, const bool allow_nil) {
  if (bytes.size() != 16) return false;
  EngineUuid id;
  std::copy_n(reinterpret_cast<const std::uint8_t*>(bytes.data()), 16, id.bytes.begin());
  return CopyContextualUuidV2(id, output, allow_nil);
}
EngineUuid ContextualUuidNativeV2(const MgaContextualTextUuidV2& value) {
  return EngineUuid{value};
}

bool ParseCanonicalPositiveU64(
    const std::map<std::string, std::string>& fields,
    const std::string_view key,
    std::uint64_t* output) {
  if (output == nullptr) return false;
  const auto found = fields.find(std::string(key));
  if (found == fields.end() || found->second.empty()) return false;
  std::uint64_t parsed = 0;
  const auto converted = std::from_chars(
      found->second.data(), found->second.data() + found->second.size(),
      parsed, 10);
  if (converted.ec != std::errc{} ||
      converted.ptr != found->second.data() + found->second.size() ||
      parsed == 0 || std::to_string(parsed) != found->second) {
    return false;
  }
  *output = parsed;
  return true;
}

std::vector<MgaContextualTextDescriptorFieldPairV2>
RawContextualDescriptorFieldsV2(
    const std::vector<std::pair<std::string, std::string>>& fields) {
  std::vector<MgaContextualTextDescriptorFieldPairV2> raw;
  raw.reserve(fields.size());
  for (const auto& [key, value] : fields) {
    raw.push_back({{key.begin(), key.end()}, {value.begin(), value.end()}});
  }
  return raw;
}

bool BuildMgaContextualTextProjectionMaterialV2(
    const EngineRequestContext& context,
    const MgaRelationStorageDescriptor& relation,
    const EngineContextualTextPolicyRowSetV2& exact_policy_rows,
    MgaContextualTextProjectionMaterialV2* output,
    EngineApiDiagnostic* diagnostic) {
  if (output == nullptr || diagnostic == nullptr) return false;
  *output = {};
  if (relation.database_uuid != context.database_uuid ||
      relation.descriptor_generation == 0 || relation.columns.empty() ||
      !CopyContextualUuidV2(relation.descriptor_uuid,
                           &output->public_projection
                                .relation_descriptor_uuid) ||
      !CopyContextualUuidV2(relation.relation_uuid,
                           &output->public_projection.relation_uuid) ||
      !CopyContextualUuidV2(relation.schema_uuid,
                           &output->public_projection.schema_uuid)) {
    *diagnostic = ContextualTextMgaDiagnostic(
        "relation projection identity is invalid");
    return false;
  }
  const bool catalog_context_exact =
      CanonicalNonNilMigrationUuid(
          context.datatype_catalog_snapshot_uuid) &&
      context.datatype_catalog_generation == 1 &&
      context.datatype_registry_generation == 1 &&
      CopyContextualUuidV2(
          context.datatype_catalog_snapshot_uuid,
          &output->public_projection.catalog_snapshot_uuid);
  output->public_projection.relation_descriptor_generation =
      relation.descriptor_generation;
  output->public_projection.resource_epoch = context.resource_epoch;
  output->public_projection.catalog_generation =
      context.datatype_catalog_generation;
  output->public_projection.registry_generation =
      context.datatype_registry_generation;
  if (relation.columns.size() >
      std::numeric_limits<std::uint32_t>::max()) {
    *diagnostic = ContextualTextMgaDiagnostic(
        "relation projection generation or extent is invalid");
    return false;
  }

  output->public_projection.columns.reserve(relation.columns.size());
  output->projected_columns.reserve(relation.columns.size());
  std::set<std::uint32_t> ordinals;
  std::set<EngineUuid> column_uuids;
  for (const auto& column : relation.columns) {
    EnginePublicRelationProjectionColumnV3 projected;
    MgaContextualTextProjectedColumnV2 contextual;
    if (!ordinals.insert(column.ordinal).second ||
        !column_uuids.insert(column.column_uuid).second ||
        !CopyContextualUuidV2(column.column_uuid,
                             &projected.column_uuid) ||
        !CopyContextualUuidV2(column.column_uuid,
                             &contextual.column_uuid) ||
        !CopyContextualUuidV2(
            column.value_descriptor.descriptor_uuid,
            &projected.descriptor_uuid)) {
      *diagnostic = ContextualTextMgaDiagnostic(
          "projected column identity is invalid or duplicated");
      return false;
    }
    projected.ordinal = column.ordinal;
    projected.canonical_name = column.canonical_name_key;
    projected.descriptor_kind = column.value_descriptor.descriptor_kind;
    projected.canonical_type_name =
        column.value_descriptor.canonical_type_name;
    projected.encoded_type_descriptor =
        column.value_descriptor.encoded_descriptor;
    if (column.nullable) projected.attributes |= 0x01u;
    if (column.generated) projected.attributes |= 0x02u;
    if (column.identity_column) projected.attributes |= 0x04u;
    projected.character_length = column.character_length;
    contextual.column_ordinal = column.ordinal;
    if (catalog_context_exact) {
      contextual.projected_datatype_catalog_generation =
          context.datatype_catalog_generation;
      contextual.projected_datatype_registry_generation =
          context.datatype_registry_generation;
      contextual.projected_resource_epoch = context.resource_epoch;
      contextual.projected_datatype_catalog_snapshot_uuid =
          output->public_projection.catalog_snapshot_uuid;
    }

    std::optional<EngineResolvedResourceDescriptor> charset;
    std::optional<EngineResolvedResourceDescriptor> collation;
    if (!column.charset_uuid.is_nil()) {
      EngineUuid requested;
      requested = column.charset_uuid;
      const auto live = LookupEngineResourceDescriptorByUuid(
          context, requested, "charset");
      if (!live.ok || !live.resource_descriptor.present ||
          live.resource_descriptor.resource_uuid !=
              column.charset_uuid ||
          live.resource_descriptor.resource_epoch != context.resource_epoch ||
          live.resource_descriptor.family_epoch == 0 ||
          live.resource_descriptor.max_bytes == 0 ||
          live.resource_descriptor.min_bytes == 0 ||
          live.resource_descriptor.max_bytes <
              live.resource_descriptor.min_bytes ||
          !CopyContextualUuidV2(column.charset_uuid,
                               &projected.charset_uuid)) {
        *diagnostic = ContextualTextMgaDiagnostic(
            "projected charset resource is stale or invalid");
        return false;
      }
      charset = live.resource_descriptor;
      projected.charset_name = charset->canonical_name;
      projected.charset_min_bytes = charset->min_bytes;
      projected.charset_max_bytes = charset->max_bytes;
      if (charset->variable_width) projected.attributes |= 0x08u;
    }
    if (!column.collation_uuid.is_nil()) {
      EngineUuid requested;
      requested = column.collation_uuid;
      const auto live = LookupEngineResourceDescriptorByUuid(
          context, requested, "collation");
      if (!live.ok || !live.resource_descriptor.present ||
          live.resource_descriptor.resource_uuid !=
              column.collation_uuid ||
          live.resource_descriptor.parent_resource_uuid !=
              column.charset_uuid ||
          live.resource_descriptor.resource_epoch != context.resource_epoch ||
          live.resource_descriptor.family_epoch == 0 ||
          !CopyContextualUuidV2(column.collation_uuid,
                               &projected.collation_uuid)) {
        *diagnostic = ContextualTextMgaDiagnostic(
            "projected collation resource is stale or invalid");
        return false;
      }
      collation = live.resource_descriptor;
      projected.collation_name = collation->canonical_name;
    }
    if (column.charset_uuid.is_nil() != column.collation_uuid.is_nil()) {
      *diagnostic = ContextualTextMgaDiagnostic(
          "projected charset and collation authority is incomplete");
      return false;
    }

    CatalogColumnMetadata encoded_metadata;
    if (!DecodeCatalogColumnMetadata(column.value_descriptor.encoded_descriptor, &encoded_metadata)) {
      *diagnostic = ContextualTextMgaDiagnostic("binary column metadata required");
      return false;
    }
    EngineUuid canonical_datatype_descriptor_uuid = column.value_descriptor.datatype_descriptor_uuid;
    const auto embedded_identity = BinaryCatalogUuid(encoded_metadata, "datatype_descriptor_uuid");
    if (canonical_datatype_descriptor_uuid.is_nil()) canonical_datatype_descriptor_uuid = embedded_identity;
    if ((!embedded_identity.is_nil() && embedded_identity != canonical_datatype_descriptor_uuid) ||
        canonical_datatype_descriptor_uuid.is_nil()) {
      *diagnostic = ContextualTextMgaDiagnostic("conflicting or missing datatype identity");
      return false;
    }
    if (!CopyContextualUuidV2(
            canonical_datatype_descriptor_uuid,
            &contextual.projected_datatype_descriptor_uuid)) {
      *diagnostic = ContextualTextMgaDiagnostic(
          "projected canonical datatype descriptor UUID is invalid");
      return false;
    }
    const auto datatype = catalog_context_exact
        ? scratchbird::core::datatypes::LookupDatatypeTypeCodecIdentityV1(
              context.datatype_catalog_snapshot_uuid,
              context.datatype_catalog_generation,
              context.datatype_registry_generation,
              canonical_datatype_descriptor_uuid, 1)
        : scratchbird::core::datatypes::DatatypeTypeCodecIdentityLookupV1{};
    if (datatype.ok) {
      const auto& row = datatype.row;
      projected.identity_present = true;
      projected.descriptor_generation = row.descriptor_generation;
      projected.type_generation = row.type_generation;
      projected.codec_id = row.codec_id;
      projected.codec_version = row.codec_version;
      projected.codec_generation = row.codec_generation;
      projected.canonical_value_width = row.canonical_value_bytes;
      projected.null_encoding = row.null_encoding_code;
      contextual.projected_datatype_descriptor_generation =
          row.descriptor_generation;
      if (!CopyContextualUuidV2(row.type_uuid, &projected.type_uuid) ||
          projected.null_encoding == 0) {
        *diagnostic = ContextualTextMgaDiagnostic(
            "projected datatype registry row is invalid");
        return false;
      }
    }

    const bool canonical_text_identity =
        canonical_datatype_descriptor_uuid == kCanonicalTextDescriptorIdentity;
    if (canonical_text_identity) {
      if (!catalog_context_exact || context.resource_epoch == 0 ||
          !ExactCanonicalTextIdentityAuthorityAvailable(context) ||
          !datatype.ok ||
          !scratchbird::core::datatypes::
              IsExactCanonicalTextTypeCodecIdentityV1(datatype.row) ||
          !ExactCanonicalMigratedTextDescriptor(
              context, column.value_descriptor.encoded_descriptor,
              column.column_uuid) ||
          projected.canonical_type_name != "text" ||
          projected.canonical_value_width != 0 ||
          projected.null_encoding != 1) {
        *diagnostic = ContextualTextMgaDiagnostic(
            "canonical d718 column does not match its exact registry row");
        return false;
      }
      const auto encoded_fields = StrictRelationDescriptorFields(
          column.value_descriptor.encoded_descriptor);
      if (!encoded_fields) {
        *diagnostic = ContextualTextMgaDiagnostic(
            "canonical d718 encoded descriptor is not exact");
        return false;
      }
      const bool comparable = charset.has_value() && collation.has_value();
      if (comparable) {
        std::uint64_t charset_generation = 0;
        std::uint64_t collation_generation = 0;
        std::uint64_t carried_resource_epoch = 0;
        const auto exact_field = [&](const std::string_view key, const EngineUuid& value) {
          return BinaryCatalogUuid(encoded_metadata, std::string(key)) == value;
        };
        if (!exact_field("charset_uuid", column.charset_uuid) ||
            !exact_field("collation_uuid", column.collation_uuid) ||
            !ParseCanonicalPositiveU64(*encoded_fields,
                                       "charset_generation",
                                       &charset_generation) ||
            !ParseCanonicalPositiveU64(*encoded_fields,
                                       "collation_generation",
                                       &collation_generation) ||
            !ParseCanonicalPositiveU64(*encoded_fields, "resource_epoch",
                                       &carried_resource_epoch) ||
            charset_generation != charset->family_epoch ||
            collation_generation != collation->family_epoch ||
            carried_resource_epoch != context.resource_epoch) {
          *diagnostic = ContextualTextMgaDiagnostic(
              "canonical d718 resource authority differs from live rows");
          return false;
        }
        const auto carried_length = encoded_fields->find("character_length");
        if ((column.character_length == 0) !=
                (carried_length == encoded_fields->end()) ||
            (carried_length != encoded_fields->end() &&
             carried_length->second !=
                 std::to_string(column.character_length))) {
          *diagnostic = ContextualTextMgaDiagnostic(
              "canonical d718 character limit differs from projection");
          return false;
        }
        sblr::ContextualTextDescriptorV2 descriptor;
        descriptor.flags = 1;
        descriptor.malformed_sequence_policy = 1;
        descriptor.null_encoding = 1;
        descriptor.descriptor_uuid =
            contextual.projected_datatype_descriptor_uuid;
        descriptor.descriptor_generation = datatype.row.descriptor_generation;
        descriptor.type_uuid = projected.type_uuid;
        descriptor.type_generation = datatype.row.type_generation;
        if (!CopyContextualUuidV2(datatype.row.codec_uuid,
                                  &descriptor.codec_uuid)) {
          *diagnostic = ContextualTextMgaDiagnostic(
              "canonical d718 codec UUID is invalid");
          return false;
        }
        descriptor.codec_version = datatype.row.codec_version;
        descriptor.codec_generation = datatype.row.codec_generation;
        descriptor.character_limit =
            column.character_length == 0
                ? std::numeric_limits<std::uint64_t>::max()
                : column.character_length;
        if (column.character_length == 0) {
          descriptor.byte_limit = std::numeric_limits<std::uint64_t>::max();
        } else if (column.character_length >
                   std::numeric_limits<std::uint64_t>::max() /
                       charset->max_bytes) {
          *diagnostic = ContextualTextMgaDiagnostic(
              "canonical d718 byte limit overflows u64");
          return false;
        } else {
          descriptor.byte_limit =
              static_cast<std::uint64_t>(column.character_length) *
              charset->max_bytes;
        }
        descriptor.charset_uuid = projected.charset_uuid;
        descriptor.charset_generation = charset_generation;
        descriptor.collation_uuid = projected.collation_uuid;
        descriptor.collation_generation = collation_generation;
        descriptor.normalization_policy_uuid =
            exact_policy_rows.normalization.identity_uuid;
        descriptor.normalization_policy_generation =
            exact_policy_rows.normalization.generation;
        descriptor.render_policy_uuid = exact_policy_rows.render.identity_uuid;
        descriptor.render_policy_generation =
            exact_policy_rows.render.generation;
        descriptor.canonicalization_profile_uuid =
            exact_policy_rows.canonicalization.identity_uuid;
        descriptor.canonicalization_profile_generation =
            exact_policy_rows.canonicalization.generation;
        descriptor.comparison_contract_uuid =
            exact_policy_rows.comparison.identity_uuid;
        descriptor.comparison_contract_generation =
            exact_policy_rows.comparison.generation;
        descriptor.equality_operation_uuid =
            exact_policy_rows.equality.identity_uuid;
        descriptor.equality_operation_generation =
            exact_policy_rows.equality.generation;
        descriptor.datatype_catalog_snapshot_uuid =
            output->public_projection.catalog_snapshot_uuid;
        descriptor.datatype_catalog_generation =
            context.datatype_catalog_generation;
        descriptor.datatype_registry_generation =
            context.datatype_registry_generation;
        descriptor.resource_epoch = context.resource_epoch;
        contextual.comparable_persisted_text = true;
        contextual.expected_text_descriptor = std::move(descriptor);
      }
    }
    output->public_projection.columns.push_back(std::move(projected));
    output->projected_columns.push_back(std::move(contextual));
  }

  *diagnostic = OkDiagnostic();
  return true;
}

bool BindFreshCanonicalTextColumnIdentitiesV2(
    CrudTableRecord* table,
    MgaRelationStorageDescriptor* relation_descriptor,
    EngineApiDiagnostic* diagnostic) {
  if (table == nullptr || relation_descriptor == nullptr ||
      diagnostic == nullptr ||
      table->columns.size() != relation_descriptor->columns.size()) {
    if (diagnostic != nullptr) {
      *diagnostic = ContextualTextMgaDiagnostic(
          "fresh table and relation column projections differ");
    }
    return false;
  }
  std::set<EngineUuid> column_uuids;
  for (std::size_t index = 0; index != table->columns.size(); ++index) {
    auto& table_column = table->columns[index];
    auto& relation_column = relation_descriptor->columns[index];
    if (table_column.first != relation_column.canonical_name_key) {
      *diagnostic = ContextualTextMgaDiagnostic(
          "fresh table and relation column order differs");
      return false;
    }
    CatalogColumnMetadata fields;
    if (!DecodeCatalogColumnMetadata(table_column.second, &fields)) {
      *diagnostic = ContextualTextMgaDiagnostic("fresh column binary metadata is invalid");
      return false;
    }
    if (BinaryCatalogUuid(fields, "datatype_descriptor_uuid") == kCanonicalTextDescriptorUuid) {
      const auto carried = fields.identities.find("column_uuid");
      if (carried != fields.identities.end()) {
        if (!CanonicalNonNilMigrationUuid(carried->second)) {
          *diagnostic = ContextualTextMgaDiagnostic("fresh canonical column UUID is invalid");
          return false;
        }
        relation_column.column_uuid = carried->second;
      } else {
        if (!CanonicalNonNilMigrationUuid(relation_column.column_uuid)) {
          *diagnostic = ContextualTextMgaDiagnostic("fresh canonical generated column UUID is invalid");
          return false;
        }
        fields.identities["column_uuid"] = relation_column.column_uuid;
        if (!EncodeCatalogColumnMetadata(fields, &table_column.second)) {
          *diagnostic = ContextualTextMgaDiagnostic("fresh column metadata encoding failed");
          return false;
        }
      }
      relation_column.value_descriptor.datatype_descriptor_uuid = kCanonicalTextDescriptorUuid;
      relation_column.value_descriptor.encoded_descriptor =
          table_column.second;
      // The persisted relation column owns a distinct public descriptor
      // handle.  The canonical datatype descriptor remains embedded in the
      // exact registry suffix and is projected separately into the live DAG.
      // This is the same split retained by the canonical TEXT migration path.
      relation_column.value_descriptor.descriptor_uuid =
          relation_column.column_uuid;
      relation_column.value_descriptor.canonical_type_name = "text";
    }
    if (!CanonicalNonNilMigrationUuid(
            relation_column.column_uuid) ||
        !column_uuids.insert(relation_column.column_uuid).second) {
      *diagnostic = ContextualTextMgaDiagnostic(
          "fresh relation column UUID is invalid or duplicated");
      return false;
    }
  }
  *diagnostic = OkDiagnostic();
  return true;
}

bool BuildMgaSealedContextualTextDescriptorMaterialV2(
    const EngineRequestContext& context,
    const CrudTableRecord& table,
    MgaRelationStorageDescriptor relation_descriptor,
    const EngineContextualTextPolicyRowSetV2& exact_policy_rows,
    MgaSealedContextualTextDescriptorMaterialV2* output,
    EngineApiDiagnostic* diagnostic) {
  if (output == nullptr || diagnostic == nullptr) return false;
  *output = {};
  if (table.creator_tx == 0 || table.event_sequence == 0 ||
      relation_descriptor.relation_uuid != table.table_uuid ||
      relation_descriptor.relation_generation != table.event_sequence) {
    *diagnostic = ContextualTextMgaDiagnostic(
        "sealed descriptor table or relation owner is invalid");
    return false;
  }
  MgaSealedContextualTextDescriptorMaterialV2 material;
  material.relation_descriptor = std::move(relation_descriptor);
  const auto base = SerializeMgaRelationStorageDescriptor(
      material.relation_descriptor);
  material.base_fields = RawContextualDescriptorFieldsV2(base);
  if (!BuildMgaContextualTextProjectionMaterialV2(
          context, material.relation_descriptor, exact_policy_rows,
          &material.projection, diagnostic)) {
    return false;
  }
  MgaContextualTextSidecarSetOwnerV2 owner;
  owner.creator_transaction_id = table.creator_tx;
  owner.event_sequence = table.event_sequence;
  owner.relation_descriptor_generation =
      material.relation_descriptor.descriptor_generation;
  if (!CopyContextualUuidV2(table.table_uuid, &owner.relation_uuid) ||
      !CopyContextualUuidV2(
          material.relation_descriptor.descriptor_uuid,
          &owner.relation_descriptor_uuid)) {
    *diagnostic = ContextualTextMgaDiagnostic(
        "sealed descriptor owner UUID is invalid");
    return false;
  }
  MgaContextualTextSidecarSetDiagnosticV2 sidecar_diagnostic;
  if (!BuildMgaContextualTextSidecarSetV2(
          owner, material.base_fields, material.projection.projected_columns,
          &material.sealed_set, &sidecar_diagnostic)) {
    *diagnostic = MakeEngineApiDiagnostic(
        sidecar_diagnostic.code.empty()
            ? "CTB.TEXT.DESCRIPTOR_INVALID"
            : sidecar_diagnostic.code,
        "mga.contextual_text_sidecar_set_v2.build_failed",
        sidecar_diagnostic.detail, true);
    return false;
  }
  *output = std::move(material);
  *diagnostic = OkDiagnostic();
  return true;
}

std::string RelationDescriptorFieldOrEmpty(
    const std::map<std::string, std::string>& fields,
    std::initializer_list<const char*> keys) {
  for (const char* key : keys) {
    const auto found = fields.find(key);
    if (found != fields.end()) { return found->second; }
  }
  return {};
}

bool RelationDescriptorBoolField(
    const std::map<std::string, std::string>& fields,
    std::initializer_list<const char*> keys) {
  const std::string value = RelationDescriptorLowerAscii(
      RelationDescriptorFieldOrEmpty(fields, keys));
  return value == "1" || value == "true" || value == "yes" || value == "on";
}

bool RelationDescriptorRequiresDeferredStore(
    const std::map<std::string, std::string>& fields) {
  const std::string timing = RelationDescriptorLowerAscii(
      RelationDescriptorFieldOrEmpty(fields, {"enforcement_timing", "timing"}));
  if (timing == "deferred" || timing == "transaction_end" ||
      timing == "initially_deferred") {
    return true;
  }
  return RelationDescriptorBoolField(fields, {"deferrable", "initially_deferred"});
}

std::optional<EngineUuid> ParentTableUuidFromRelationDescriptor(const std::string& descriptor) {
  CatalogColumnMetadata fields;
  if (!AdmitCatalogColumnMetadata(descriptor, &fields)) return std::nullopt;
  for (const char* key : {"referenced_table_uuid", "foreign_table_uuid"}) {
    const auto it = fields.identities.find(key);
    if (it != fields.identities.end() && !it->second.is_nil()) return it->second;
  }
  return std::nullopt;
}

std::optional<std::set<EngineUuid>> InsertTargetRelationScope(const EngineRequestContext& context,
                                                const RelationReadSnapshot& metadata,
                                                const EngineUuid& table_uuid) {
  std::set<EngineUuid> table_scope;
  if (table_uuid.is_nil()) { return table_scope; }
  table_scope.insert(table_uuid);
  const auto table = FindVisibleCrudTable(metadata,
                                          table_uuid,
                                          context.local_transaction_id);
  if (!table) { return table_scope; }
  for (const auto& [column_name, descriptor] : table->columns) {
    (void)column_name;
    const auto parent = ParentTableUuidFromRelationDescriptor(descriptor);
    if (parent && !parent->is_nil()) {
      table_scope.insert(*parent);
    }
  }
  for (const auto& candidate : metadata.tables) {
    if (candidate.table_uuid.is_nil() || candidate.table_uuid == table_uuid) {
      continue;
    }
    if (!CrudCreatorVisible(metadata,
                            candidate.creator_tx,
                            candidate.event_sequence,
                            context.local_transaction_id)) {
      continue;
    }
    for (const auto& [column_name, descriptor] : candidate.columns) {
      (void)column_name;
      const auto parent = ParentTableUuidFromRelationDescriptor(descriptor);
      if (parent && *parent == table_uuid) {
        table_scope.insert(candidate.table_uuid);
        break;
      }
    }
  }
  return table_scope;
}



}  // namespace scratchbird::engine::internal_api
