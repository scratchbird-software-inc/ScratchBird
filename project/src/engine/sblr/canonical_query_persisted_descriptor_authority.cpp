// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_query_execute.hpp"
#include "canonical_query_object_free_composition_support.hpp"
#include "canonical_query_scalar_support.hpp"
#include "datatype_catalog_manifest.hpp"
#include "catalog/name_resolution_api.hpp"
#include "catalog/column_metadata_codec.hpp"
#include "query/expression_api.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace scratchbird::engine::sblr {
namespace api = scratchbird::engine::internal_api;
namespace exec = scratchbird::engine::executor;
namespace dt = scratchbird::core::datatypes;

// SEARCH_KEY: SB_ENGINE_CANONICAL_QUERY_PERSISTED_DESCRIPTOR_AUTHORITY_AUTHORITY
// Validates persisted TEXT descriptor identity against supplied statement
// receipts and engine datatype/resource catalogs; binds the existing row service.
// Owns no descriptor construction, snapshot construction, or transaction finality.

#if !defined(SCRATCHBIRD_QOW_QUERY_ROUTE_CONTRACT_ONLY)
bool ValidateCanonicalPersistedTextRowDescriptorAuthorityV1(
    const api::EngineRequestContext& context,
    const api::RelationalTypeDescriptor& bound,
    const api::EngineDescriptor& persisted,
    const api::RelationalNullability effective_nullability,
    std::string* refusal_detail) {
  if (refusal_detail == nullptr) return false;
  refusal_detail->clear();
  const auto refuse = [&](std::string detail) {
    *refusal_detail = std::move(detail);
    return false;
  };
  if (!bound.datatype_identity_authoritative ||
      (bound.nullability != api::RelationalNullability::kNullable &&
       bound.nullability != api::RelationalNullability::kNonNull) ||
      (effective_nullability != api::RelationalNullability::kNullable &&
       effective_nullability != api::RelationalNullability::kNonNull) ||
      bound.statement_receipt_uuid.is_nil() ||
      bound.statement_receipt_uuid !=
          context.statement_receipt_uuid ||
      bound.datatype_catalog_snapshot_uuid.is_nil() ||
      bound.datatype_catalog_snapshot_uuid !=
          context.datatype_catalog_snapshot_uuid ||
      bound.datatype_catalog_generation == 0 ||
      bound.datatype_catalog_generation !=
          context.datatype_catalog_generation ||
      bound.datatype_registry_generation == 0 ||
      bound.datatype_registry_generation !=
          context.datatype_registry_generation ||
      bound.descriptor_generation == 0 || bound.type_generation == 0 ||
      bound.codec_id.empty() || bound.codec_version == 0 ||
      bound.codec_generation == 0 || !bound.collation_uuid.has_value() ||
      !core::uuid::IsEngineIdentityUuid(*bound.collation_uuid) || !bound.width.has_value() ||
      *bound.width == 0 || bound.timezone_profile_id.has_value() ||
      bound.precision.has_value() || bound.scale.has_value()) {
    return refuse("bound canonical TEXT receipt authority is incomplete");
  }

  api::CatalogColumnMetadata metadata;
  if (!api::DecodeCatalogColumnMetadata(persisted.encoded_descriptor, &metadata)) {
    return refuse("persisted canonical TEXT descriptor binary metadata is invalid");
  }
  const auto encoded_datatype_descriptor_uuid =
      api::BinaryCatalogUuid(metadata, "datatype_descriptor_uuid");
  const auto identity = dt::LookupDatatypeTypeCodecIdentityV1(
      bound.datatype_catalog_snapshot_uuid,
      bound.datatype_catalog_generation,
      bound.datatype_registry_generation, encoded_datatype_descriptor_uuid,
      bound.descriptor_generation);
  if (!identity.ok ||
      !dt::IsExactCanonicalTextTypeCodecIdentityV1(identity.row) ||
      (bound.descriptor_uuid != persisted.descriptor_uuid &&
       bound.descriptor_uuid != identity.row.descriptor_uuid) ||
      bound.descriptor_generation != identity.row.descriptor_generation ||
      bound.type_uuid != identity.row.type_uuid ||
      bound.type_generation != identity.row.type_generation ||
      bound.codec_id != identity.row.codec_id ||
      bound.codec_version != identity.row.codec_version ||
      bound.codec_generation != identity.row.codec_generation ||
      *bound.width > identity.row.canonical_value_maximum_bytes) {
    return refuse("bound canonical TEXT registry authority is stale");
  }
  if (!api::QowCanonicalDescriptorIdentityV1(persisted) ||
      persisted.descriptor_uuid == identity.row.descriptor_uuid ||
      persisted.descriptor_kind != "scalar" ||
      persisted.canonical_type_name != identity.row.canonical_name) {
    return refuse("persisted canonical TEXT outer descriptor is invalid");
  }

  const auto& fields = metadata.text;
  constexpr std::array<std::string_view, 11> kRequiredFields{
      "character_length", "nullable", "charset_generation",
      "collation_generation", "resource_epoch", "datatype_descriptor_generation",
      "type_generation", "codec_id", "codec_version", "codec_generation",
      "null_encoding"};
  constexpr std::array<std::string_view, 6> kRequiredIdentities{
      "charset_uuid", "collation_uuid", "datatype_descriptor_uuid",
      "type_uuid", "codec_uuid", "column_uuid"};
  const bool type_spelling = fields.contains("type");
  const bool canonical_spelling = fields.contains("canonical");
  if (fields.size() != kRequiredFields.size() + 1 ||
      metadata.identities.size() != kRequiredIdentities.size() ||
      type_spelling == canonical_spelling ||
      std::ranges::any_of(kRequiredFields, [&](const auto key) {
        return !fields.contains(std::string(key));
      }) ||
      std::ranges::any_of(kRequiredIdentities, [&](const auto key) {
        return !core::uuid::IsEngineIdentityUuid(
            api::BinaryCatalogUuid(metadata, std::string(key)));
      })) {
    return refuse("persisted canonical TEXT descriptor field set is not exact");
  }
  const std::string_view spelling = type_spelling ? "type" : "canonical";
  const auto exact = [&](const std::string_view key,
                         const std::string_view expected) {
    const auto found = fields.find(std::string(key));
    return found != fields.end() && found->second == expected;
  };
  const auto exact_uuid = [&](const std::string& key, const api::EngineUuid& expected) {
    const auto found = metadata.identities.find(key);
    return found != metadata.identities.end() && found->second == expected;
  };
  const auto parse_u64 = [&](const std::string_view key,
                             std::uint64_t* value) {
    const auto found = fields.find(std::string(key));
    if (value == nullptr || found == fields.end() || found->second.empty()) {
      return false;
    }
    const auto parsed = std::from_chars(
        found->second.data(), found->second.data() + found->second.size(),
        *value, 10);
    return parsed.ec == std::errc{} &&
           parsed.ptr == found->second.data() + found->second.size() &&
           *value != 0 && std::to_string(*value) == found->second;
  };

  std::uint64_t character_length = 0;
  std::uint64_t charset_generation = 0;
  std::uint64_t collation_generation = 0;
  std::uint64_t resource_epoch = 0;
  std::uint64_t descriptor_generation = 0;
  std::uint64_t type_generation = 0;
  std::uint64_t codec_version = 0;
  std::uint64_t codec_generation = 0;
  std::uint64_t null_encoding = 0;
  if (!parse_u64("character_length", &character_length) ||
      !parse_u64("charset_generation", &charset_generation) ||
      !parse_u64("collation_generation", &collation_generation) ||
      !parse_u64("resource_epoch", &resource_epoch) ||
      !parse_u64("datatype_descriptor_generation",
                 &descriptor_generation) ||
      !parse_u64("type_generation", &type_generation) ||
      !parse_u64("codec_version", &codec_version) ||
      !parse_u64("codec_generation", &codec_generation) ||
      !parse_u64("null_encoding", &null_encoding)) {
    return refuse(
        "persisted canonical TEXT numeric authority is non-canonical");
  }
  const auto expected_nullable =
      effective_nullability == api::RelationalNullability::kNullable
          ? std::string_view("true")
          : std::string_view("false");
  if (!exact(spelling, identity.row.canonical_name) ||
      character_length != *bound.width ||
      !exact_uuid("collation_uuid", *bound.collation_uuid) ||
      !exact("nullable", expected_nullable) ||
      resource_epoch == 0 || resource_epoch != context.resource_epoch ||
      !exact_uuid("datatype_descriptor_uuid", identity.row.descriptor_uuid) ||
      descriptor_generation != identity.row.descriptor_generation ||
      !exact_uuid("type_uuid", identity.row.type_uuid) ||
      type_generation != identity.row.type_generation ||
      !exact_uuid("codec_uuid", identity.row.codec_uuid) ||
      !exact("codec_id", identity.row.codec_id) ||
      codec_version != identity.row.codec_version ||
      codec_generation != identity.row.codec_generation ||
      null_encoding != identity.row.null_encoding_code) {
    return refuse(
        "persisted canonical TEXT descriptor differs from bound authority");
  }

  const auto charset_uuid = api::BinaryCatalogUuid(metadata, "charset_uuid");
  const auto charset = api::LookupEngineResourceDescriptorByUuid(
      context, charset_uuid, "charset");
  const auto collation_uuid = api::BinaryCatalogUuid(metadata, "collation_uuid");
  const auto collation = api::LookupEngineResourceDescriptorByUuid(
      context, collation_uuid, "collation");
  if (!charset.ok || !collation.ok ||
      !charset.resource_descriptor.present ||
      !collation.resource_descriptor.present ||
      charset.resource_descriptor.resource_family != "charset" ||
      collation.resource_descriptor.resource_family != "collation" ||
      charset.resource_descriptor.resource_uuid !=
          charset_uuid ||
      collation.resource_descriptor.resource_uuid !=
          collation_uuid ||
      collation.resource_descriptor.parent_resource_uuid !=
          charset_uuid ||
      charset.resource_descriptor.family_epoch != charset_generation ||
      collation.resource_descriptor.family_epoch != collation_generation ||
      charset.resource_descriptor.resource_epoch != resource_epoch ||
      collation.resource_descriptor.resource_epoch != resource_epoch) {
    return refuse(
        "persisted canonical TEXT resource authority is stale or crossed");
  }

  std::string canonical;
  if (!api::EncodeCatalogColumnMetadata(metadata, &canonical) ||
      canonical != persisted.encoded_descriptor) {
    return refuse(
        "persisted canonical TEXT descriptor serialization is not exact");
  }
  return true;
}
#endif

namespace {

void BindCanonicalPersistedRowDescriptorAuthorityV1(
    const api::EngineRequestContext& context,
    CanonicalRelationalExpressionRuntimeServices* services) {
  if (services == nullptr) return;
#if defined(SCRATCHBIRD_QOW_QUERY_ROUTE_CONTRACT_ONLY)
  (void)context;
  BindContractOnlyCanonicalPersistedRowDescriptorAuthorityV1(services);
#else
  services->persisted_row_descriptor_authority =
      [context = &context](
          const std::uint32_t,
          const api::RelationalTypeDescriptor& bound,
          const api::EngineDescriptor& persisted,
          const api::RelationalNullability effective_nullability,
          std::string* refusal_detail) {
        return ValidateCanonicalPersistedTextRowDescriptorAuthorityV1(
            *context, bound, persisted, effective_nullability,
            refusal_detail);
      };
#endif
}

}  // namespace

void BindCanonicalPersistedRowDescriptorAuthorityForComposition(
    const api::EngineRequestContext& context,
    CanonicalRelationalExpressionRuntimeServices* services) {
  BindCanonicalPersistedRowDescriptorAuthorityV1(context, services);
}

}  // namespace scratchbird::engine::sblr
