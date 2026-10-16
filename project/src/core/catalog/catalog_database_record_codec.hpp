// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_value_codec.hpp"
#include <optional>
#include <string_view>

namespace scratchbird::core::catalog {
// Durable node-level datatype binding. Absence in the frozen V1 payload is
// explicit: it is never permission to infer the running binary's default.
struct CatalogDatabaseDatatypeCohort {
  TypedUuid catalog_snapshot_uuid;
  u64 catalog_generation = 0;
  u64 registry_generation = 0;
  bool operator==(const CatalogDatabaseDatatypeCohort& other) const {
    return catalog_snapshot_uuid.kind == other.catalog_snapshot_uuid.kind &&
        catalog_snapshot_uuid.value == other.catalog_snapshot_uuid.value &&
        catalog_generation == other.catalog_generation &&
        registry_generation == other.registry_generation;
  }
};

enum class CatalogDatabaseTextDefaultSource : u64 {
  explicit_selection = 1, creation_policy = 2, cluster_default = 3
};
// Persisted selectors/provenance, not resource, policy or cluster authority.
// The owning publisher admits the exact referenced objects and epochs. No
// charset names, host defaults or evaluator fallback are stored here.
struct CatalogDatabaseTextDefaults {
  TypedUuid charset_uuid, collation_uuid;
  u64 resource_activation_epoch = 0, charset_family_epoch = 0, collation_family_epoch = 0;
  CatalogDatabaseTextDefaultSource source = CatalogDatabaseTextDefaultSource::explicit_selection;
  TypedUuid source_object_uuid;
  u64 source_generation = 0;
  std::optional<TypedUuid> cluster_uuid;
  bool operator==(const CatalogDatabaseTextDefaults& other) const {
    const auto same=[](const TypedUuid& a,const TypedUuid& b) {
      return a.kind==b.kind&&a.value==b.value;
    };
    return same(charset_uuid,other.charset_uuid)&&same(collation_uuid,other.collation_uuid)&&
        resource_activation_epoch==other.resource_activation_epoch&&
        charset_family_epoch==other.charset_family_epoch&&collation_family_epoch==other.collation_family_epoch&&
        source==other.source&&same(source_object_uuid,other.source_object_uuid)&&
        source_generation==other.source_generation&&cluster_uuid.has_value()==other.cluster_uuid.has_value()&&
        (!cluster_uuid||same(*cluster_uuid,*other.cluster_uuid));
  }
};

// CATALOG_DATABASE_BINARY_PAYLOAD_V1/V2/V3_TEXT_DEFAULTS. Frozen V1/V2
// retain exact bytes and explicit absence; V3 requires the complete cohort
// and binary default pair/provenance. Structural success grants no authority.
struct CatalogDatabaseRecord {
  TypedUuid database_uuid;
  u64 database_header_format_major = 0;
  u64 database_header_format_minor = 0;
  u64 catalog_manifest_format_version = 0;
  u64 page_size = 0;
  u64 creation_unix_epoch_millis = 0;
  u64 feature_flags = 0;
  u64 compatibility_flags = 0;
  u64 creator_transaction_number = 0;
  std::optional<CatalogDatabaseDatatypeCohort> datatype_cohort;
  std::optional<CatalogDatabaseTextDefaults> text_defaults;
};
struct CatalogDatabaseRecordDecodeResult {
  CatalogValueError error = CatalogValueError::none;
  std::optional<CatalogDatabaseRecord> record;
  bool ok() const { return error == CatalogValueError::none && record.has_value(); }
};
const CatalogValueSchema& CatalogDatabaseRecordSchema();
const CatalogValueSchema& CatalogDatabaseRecordSchemaV2();
const CatalogValueSchema& CatalogDatabaseRecordSchemaV3();
inline constexpr std::size_t kCatalogDatabaseRecordMaximumBytes = 408;
// Fixed-width canonical serialization into caller-admitted backing. Complete
// validation and capacity/non-overlap admission precede all writes; no heap
// allocation, including schema setup. Refusal leaves backing unchanged.
std::optional<std::size_t> CatalogDatabaseRecordEncodedBytes(const CatalogDatabaseRecord&);
CatalogValueEncodeIntoResult EncodeCatalogDatabaseRecordInto(
    const CatalogDatabaseRecord&, std::span<byte> backing);
CatalogValueEncodeResult EncodeCatalogDatabaseRecord(const CatalogDatabaseRecord& record);
CatalogDatabaseRecordDecodeResult DecodeCatalogDatabaseRecord(std::string_view bytes);
}  // namespace scratchbird::core::catalog
