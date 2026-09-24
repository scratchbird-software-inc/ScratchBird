// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "mga_relation_store/mga_relation_descriptor.hpp"
#include "catalog/column_metadata_codec.hpp"
#include "core/datatypes/canonical_utf8.hpp"
#include "core/hash/hash_digest.hpp"
#include "core/uuid/uuid.hpp"
#include <algorithm>
#include <array>
#include <set>
#include <string_view>
#include <vector>

namespace scratchbird::engine::internal_api {
inline constexpr EngineUuid kBulkImportTextConverterUuidV1{{
    0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xb7,0x75}};

// Column metadata is a typed binary frame; only scalar spelling input is text.
inline bool DecodeBulkImportColumnMetadata(std::string_view encoded,
                                          CatalogColumnMetadata* output) {
  if (encoded.starts_with("SBMETA"))
    return DecodeCatalogColumnMetadata(encoded, output);
  if (encoded.find('\0') != std::string_view::npos ||
      !core::datatypes::ValidateCanonicalUtf8(
          reinterpret_cast<const std::uint8_t*>(encoded.data()), encoded.size()))
    return false;
  return AdmitCatalogColumnMetadata(encoded, output);
}

inline bool BulkImportColumnMetadataValid(std::string_view encoded) {
  CatalogColumnMetadata fields;
  return DecodeBulkImportColumnMetadata(encoded, &fields);
}

inline bool BulkImportColumnHasForbiddenDefaultOrConstraint(std::string_view encoded) {
  CatalogColumnMetadata fields;
  if (!DecodeBulkImportColumnMetadata(encoded, &fields)) return true;
  constexpr std::array<std::string_view, 12> forbidden{{
      "default", "default_uuid", "generated", "identity",
      "primary_key", "unique", "foreign_key", "references",
      "check", "constraint_uuid", "constraint_kind", "constraint"}};
  const auto prohibited = [&](std::string key) {
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch) {
      return static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch);
    });
    return std::find(forbidden.begin(), forbidden.end(), key) != forbidden.end();
  };
  for (const auto& [key, value] : fields.text)
    if (prohibited(key)) return true;
  for (const auto& [key, value] : fields.identities)
    if (prohibited(key)) return true;
  return false;
}

// Hash projection only. Does not manufacture catalog, datatype, constraint,
// trigger, resource or converter authority. Both bind and execute use this
// exact Core BULK-IMPORT-COLUMN-DIGEST-BINARY-002 implementation.
inline bool ComputeBulkImportColumnDigestV2(const MgaRelationStorageDescriptor& descriptor,
                                            std::array<std::uint8_t,32>* output) {
  if(!output || descriptor.columns.empty() || descriptor.columns.size()>65535 ||
     !descriptor.relation_generation || !descriptor.descriptor_generation) return false;
  std::vector<const MgaRelationColumnStorageDescriptor*> columns;
  columns.reserve(descriptor.columns.size());
  for(const auto& column:descriptor.columns) columns.push_back(&column);
  std::sort(columns.begin(),columns.end(),[](const auto* a,const auto* b){return a->ordinal<b->ordinal;});
  constexpr std::string_view domain="ScratchBird.BulkImportStreamColumnDescriptorSet.V2";
  std::vector<std::uint8_t> bytes(domain.begin(),domain.end());
  const auto number=[&](std::uint64_t value,unsigned width) {
    for(unsigned i=0;i<width;++i) bytes.push_back(static_cast<std::uint8_t>(value>>(8*i)));
  };
  const auto uuid=[&](const EngineUuid& value,bool optional=false) {
    if(!core::uuid::IsEngineIdentityUuid(value) && !(optional && value.is_nil())) return false;
    bytes.insert(bytes.end(),value.bytes.begin(),value.bytes.end()); return true;
  };
  const auto utf8=[](std::string_view value) {
    return value.find('\0')==std::string_view::npos &&
        core::datatypes::ValidateCanonicalUtf8(
            reinterpret_cast<const std::uint8_t*>(value.data()),value.size());
  };
  const auto label=[&](std::string_view value) {
    if(value.empty() || value.size()>65535 || !utf8(value)) return false;
    number(value.size(),2); bytes.insert(bytes.end(),value.begin(),value.end()); return true;
  };
  if(!uuid(descriptor.relation_uuid)) return false;
  number(descriptor.relation_generation,8);
  if(!uuid(descriptor.descriptor_uuid)) return false;
  number(descriptor.descriptor_generation,8); number(columns.size(),4);
  std::set<std::array<std::uint8_t,16>> identities;
  std::uint32_t previous=0; bool first=true;
  for(const auto* column:columns) {
    if((!first && previous==column->ordinal) || !column->column_generation ||
       !identities.insert(column->column_uuid.bytes).second) return false;
    first=false; previous=column->ordinal;
    number(column->ordinal,4);
    if(!uuid(column->column_uuid)) return false;
    number(column->column_generation,8);
    if(!label(column->canonical_name_key) || !uuid(column->value_descriptor.descriptor_uuid) ||
       !label(column->value_descriptor.descriptor_kind) ||
       !label(column->value_descriptor.canonical_type_name) ||
       !BulkImportColumnMetadataValid(column->value_descriptor.encoded_descriptor)) return false;
    const auto& encoded=column->value_descriptor.encoded_descriptor;
    const std::vector<std::uint8_t> encoded_bytes(encoded.begin(),encoded.end());
    const auto inner=core::hash::ComputeSha256Digest(encoded_bytes);
    if(!inner.ok() || inner.digest_bytes!=32) return false;
    bytes.insert(bytes.end(),inner.digest.begin(),inner.digest.end());
    number(column->nullable,1); number(column->generated,1); number(column->identity_column,1);
    if(!label(column->storage_class) || !uuid(column->charset_uuid,true) ||
       !uuid(column->collation_uuid,true)) return false;
    number(column->character_length,4); number(column->max_inline_bytes,8);
    if(!label(column->overflow_policy)) return false;
  }
  const auto hash=core::hash::ComputeSha256Digest(bytes);
  if(!hash.ok() || hash.digest_bytes!=32 ||
     std::none_of(hash.digest.begin(),hash.digest.end(),[](auto b){return b!=0;})) return false;
  *output=hash.digest;
  return true;
}
} // namespace scratchbird::engine::internal_api
