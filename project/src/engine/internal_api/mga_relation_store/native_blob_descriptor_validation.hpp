// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "api_types.hpp"
#include "catalog/column_metadata_codec.hpp"
#include "datatype_storage_identity.hpp"
#include "datatype_operations.hpp"
#include "uuid.hpp"
#include <charconv>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>

namespace scratchbird::engine::internal_api {
// BASE-BLOB-NATIVE-COLUMN-RECEIVER-20261016. Pure representation validation:
// this neither issues a datatype/profile nor admits storage, a live value,
// indexes, memory, SQL execution or publication. Labels never select a type.
struct NativeBlobDescriptorValidationV1 {
  bool binary_blob = false;
  bool resource_failure = false;
  const char* reason = nullptr;
  bool ok() const noexcept { return reason == nullptr; }
};
namespace native_blob_descriptor_detail {
namespace dt = core::datatypes;
inline bool ExactGeneration(std::string_view text, std::uint64_t expected) noexcept {
  if (text.empty() || (text.size() > 1 && text.front() == '0')) return false;
  std::uint64_t value = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && value == expected;
}
inline bool ReferencesKnownBlobIdentity(const EngineDescriptor& value) {
  for (const auto& identity : dt::CurrentDatatypeTypeCodecIdentityRowsV3()) {
    const auto& row = identity.legacy_fields;
    if (row.canonical_binary_type_code == static_cast<std::uint32_t>(dt::CanonicalTypeId::blob) &&
        (value.datatype_descriptor_uuid == row.descriptor_uuid || value.type_uuid == row.type_uuid))
      return true;
  }
  // Exact frozen storage-only predecessor from DatatypeStorageIdentityV3.
  // Classification ONLY: its presence demands an actual retained cohort;
  // this constant cannot grant legacy or current codec/profile authority.
  constexpr EngineUuid predecessor{{0xf4,0x01,0,0,0x62,0x6c,0x7f,0x62,
                                    0x80,0,0,0,0,0,0,0}};
  return value.datatype_descriptor_uuid == predecessor || value.type_uuid == predecessor;
}
}

inline NativeBlobDescriptorValidationV1 ValidateNativeBinaryBlobDescriptorV1(
    const EngineDescriptor& descriptor, bool nullable, std::string_view metadata) noexcept {
  namespace dt = core::datatypes;
  NativeBlobDescriptorValidationV1 result;
  const auto refuse = [&](const char* reason) { result.reason = reason; return result; };
  try {
    const auto& cohort = descriptor.datatype_cohort;
    const bool present = !cohort.catalog_snapshot_uuid.is_nil() ||
                         cohort.catalog_generation || cohort.registry_generation;
    if (!present) {
      if (native_blob_descriptor_detail::ReferencesKnownBlobIdentity(descriptor)) {
        result.binary_blob = true;
        return refuse("binary_blob_requires_exact_datatype_cohort");
      }
      // Preserve unrelated, explicitly cohort-absent legacy representation.
      return result;
    }
    if (!core::uuid::IsEngineIdentityUuid(cohort.catalog_snapshot_uuid) ||
        !cohort.catalog_generation || !cohort.registry_generation)
      return refuse("column_datatype_cohort_invalid");
    const auto* codec = dt::FindDatatypeTypeCodecIdentityV3(cohort.catalog_snapshot_uuid,
        cohort.catalog_generation, cohort.registry_generation,
        descriptor.datatype_descriptor_uuid, descriptor.datatype_descriptor_generation);
    dt::DatatypeStorageIdentityV3 storage;
    dt::CanonicalTypeId type = dt::CanonicalTypeId::unknown;
    if (codec) {
      if (codec->legacy_fields.type_uuid != descriptor.type_uuid)
        return refuse("column_datatype_storage_identity_mismatch");
      type = static_cast<dt::CanonicalTypeId>(codec->legacy_fields.canonical_binary_type_code);
    } else {
      if (!dt::LookupDatatypeStorageIdentityV3(cohort.catalog_snapshot_uuid,
              cohort.catalog_generation, cohort.registry_generation,
              descriptor.datatype_descriptor_uuid, descriptor.datatype_descriptor_generation,
              &storage) || storage.type_uuid != descriptor.type_uuid)
        return refuse("column_datatype_storage_identity_invalid");
      type = storage.type_id;
    }
    if (type != dt::CanonicalTypeId::blob) return result;
    result.binary_blob = true;
    if (!core::uuid::IsEngineIdentityUuid(descriptor.descriptor_uuid) ||
        descriptor.descriptor_kind != "scalar")
      return refuse("binary_blob_occurrence_invalid");
    if (!descriptor.charset_uuid.is_nil() || !descriptor.collation_uuid.is_nil())
      return refuse("binary_blob_has_no_charset_or_collation");
    if (descriptor.canonical_type_name.empty() ||
        descriptor.canonical_type_name.find('\0') != std::string::npos)
      return refuse("binary_blob_declaration_label_invalid");
    const auto label_type = dt::CanonicalTypeIdFromStableName(descriptor.canonical_type_name);
    if (label_type != dt::CanonicalTypeId::unknown && label_type != dt::CanonicalTypeId::blob)
      return refuse("binary_blob_declaration_label_mismatch");
    CatalogColumnMetadata fields;
    if (!AdmitCatalogColumnMetadata(metadata, &fields))
      return refuse("binary_blob_metadata_invalid");
    for (const auto& [name, identity] : fields.identities) {
      if ((name == "type_uuid" && identity == descriptor.type_uuid) ||
          (name == "datatype_descriptor_uuid" && identity == descriptor.datatype_descriptor_uuid) ||
          (name == "codec_uuid" && codec && identity == codec->legacy_fields.codec_uuid)) continue;
      return refuse("binary_blob_identity_modifier_not_admitted");
    }
    const auto agrees = [&](const char* name, const char* value) {
      const auto found = fields.text.find(name);
      return found == fields.text.end() || found->second == value;
    };
    if (!agrees("nullable", nullable ? "true" : "false") ||
        !agrees("nullability", nullable ? "nullable" : "non_null") ||
        !agrees("not_null", nullable ? "false" : "true") ||
        (unsigned(fields.text.contains("nullable")) + unsigned(fields.text.contains("nullability")) +
         unsigned(fields.text.contains("not_null")) > 1))
      return refuse("binary_blob_nullability_mismatch");
    for (const auto& [name, value] : fields.text) {
      if (name == "type" || name == "canonical" || name == "canonical_type") {
        if (value != descriptor.canonical_type_name)
          return refuse("binary_blob_declaration_label_mismatch");
        continue;
      }
      if (name == "datatype_descriptor_generation" &&
          native_blob_descriptor_detail::ExactGeneration(value, descriptor.datatype_descriptor_generation)) continue;
      if (codec) {
        const auto& row = codec->legacy_fields;
        if ((name == "type_generation" && native_blob_descriptor_detail::ExactGeneration(value, row.type_generation)) ||
            (name == "codec_id" && value == row.codec_id) ||
            (name == "codec_version" && native_blob_descriptor_detail::ExactGeneration(value, row.codec_version)) ||
            (name == "codec_generation" && native_blob_descriptor_detail::ExactGeneration(value, row.codec_generation)) ||
            (name == "null_encoding" && native_blob_descriptor_detail::ExactGeneration(value, row.null_encoding_code))) continue;
      }
      if (name == "nullable" || name == "nullability" || name == "not_null" ||
          name == "primary_key" || name == "pk" || name == "unique" ||
          name == "generated" || name == "identity" || name == "default" ||
          name == "default_value" || name == "check") continue;
      return refuse("binary_blob_modifier_not_admitted");
    }
    return result;
  } catch (const std::bad_alloc&) {
    result.resource_failure = true;
    return refuse("binary_blob_descriptor_allocation_failed");
  } catch (const std::length_error&) {
    result.resource_failure = true;
    return refuse("binary_blob_descriptor_length_exceeded");
  }
}
inline NativeBlobDescriptorValidationV1 ValidateNativeBinaryBlobDescriptorV1(
    const EngineDescriptor& descriptor, bool nullable) noexcept {
  return ValidateNativeBinaryBlobDescriptorV1(descriptor, nullable, descriptor.encoded_descriptor);
}
} // namespace scratchbird::engine::internal_api
