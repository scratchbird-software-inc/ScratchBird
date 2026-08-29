// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "datatype_descriptor.hpp"
#include "datatype_layout.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::core::datatypes {

inline constexpr const char* kCurrentCoreDatatypeCatalogManifestKey =
    "MDF-012-CURRENT-CORE-DATATYPE-CATALOG-MANIFEST";

struct DatatypeCatalogDescriptorRow {
  TypedUuid descriptor_uuid;
  CanonicalTypeId type_id = CanonicalTypeId::unknown;
  TypeFamily family = TypeFamily::unknown;
  std::string stable_name;
  std::string sys_table_name;
  u64 descriptor_epoch = 0;
  bool descriptor_authoritative = true;
  bool reference_name_is_alias_only = true;
};

struct DatatypeCatalogLayoutRow {
  CanonicalTypeId type_id = CanonicalTypeId::unknown;
  std::string sys_table_name;
  DatatypeStorageClass storage_class = DatatypeStorageClass::unknown;
  DatatypeBinaryEncoding encoding = DatatypeBinaryEncoding::unknown;
  u32 inline_bytes = 0;
  bool may_overflow_to_toast = false;
};

struct DatatypeImplementationTraceRow {
  CanonicalTypeId type_id = CanonicalTypeId::unknown;
  TypeFamily family = TypeFamily::unknown;
  std::string trace_key;
  std::string implementation_source_path;
  bool descriptor_api_exercised = false;
  bool layout_api_exercised = false;
  bool parser_spelling_is_authority = false;
};

struct DatatypeCatalogManifest {
  std::string manifest_key;
  u64 catalog_epoch = 0;
  std::vector<DatatypeCatalogDescriptorRow> descriptor_rows;
  std::vector<DatatypeCatalogLayoutRow> layout_rows;
  std::vector<DatatypeImplementationTraceRow> trace_rows;
};

struct DatatypeCatalogManifestResult {
  Status status;
  DatatypeCatalogManifest manifest;
  DiagnosticRecord diagnostic;
  std::vector<DiagnosticRecord> diagnostics;

  bool ok() const {
    return status.ok() && diagnostics.empty();
  }
};

class DatatypeCatalogCache {
 public:
  DatatypeCatalogManifestResult Load(DatatypeCatalogManifest manifest);
  void Invalidate();
  DatatypeCatalogManifestResult Lookup(CanonicalTypeId type_id) const;
  u64 generation() const { return generation_; }
  bool valid() const { return valid_; }

 private:
  DatatypeCatalogManifest manifest_;
  u64 generation_ = 0;
  bool valid_ = false;
};

TypedUuid StableDatatypeDescriptorUuid(CanonicalTypeId type_id,
                                       const std::string& stable_name);
DatatypeCatalogManifestResult LoadCurrentCoreDatatypeCatalogManifest();
DatatypeCatalogManifestResult ValidateDatatypeCatalogManifest(
    const DatatypeCatalogManifest& manifest);
DatatypeCatalogManifestResult LookupDatatypeCatalogRow(
    const DatatypeCatalogManifest& manifest,
    CanonicalTypeId type_id);

struct DatatypeTypeCodecIdentityRowV1 {
  std::string catalog_snapshot_uuid;
  u64 catalog_generation = 0;
  u64 registry_generation = 0;
  std::string descriptor_uuid;
  u64 descriptor_generation = 0;
  std::string type_uuid;
  u64 type_generation = 0;
  std::string codec_id;
  u16 codec_version = 0;
  u64 codec_generation = 0;
  u32 canonical_value_bytes = 0;
  bool null_supported = false;
  // Exact Core semantic fields consumed by typed authority carriers.  These
  // are part of the registry row; consumers must not infer them from names,
  // widths, codecs, or host enums.
  std::string canonical_name;
  std::uint8_t datatype_identity_code = 0;
  std::uint8_t null_encoding_code = 0;
  std::uint8_t byte_order_code = 0;
  bool signed_code = false;
  std::uint8_t representation_code = 0;
  u32 canonical_value_minimum_bytes = 0;
  u32 canonical_value_maximum_bytes = 0;
  u32 canonical_value_exact_bytes = 0;
  // The public/native result type code is the engine's canonical binary type
  // code.  It is deliberately distinct from datatype_identity_code, which is
  // the closed u8 row ordinal used only by the DUDV typed-UPDATE carrier.
  u32 canonical_binary_type_code = 0;
  // codec_uuid and the fields below are populated when the admitted Core row
  // carries variable-width/text semantics.  Empty/default values mean the
  // fixed-width row has no such extension; consumers may not infer one.
  std::string codec_uuid;
  bool canonical_value_variable_width = false;
  bool canonical_value_exact_zero_is_width_marker = false;
  std::string canonical_byte_order;
  std::string canonical_representation;
  std::string canonical_charset;
  bool shortest_form_utf8_required = false;
  bool implicit_normalization_allowed = false;
  bool descriptor_bound_collation_required = false;
  bool empty_value_distinct_from_sql_null = false;
  bool sql_null_requires_zero_payload = false;
  bool variable_width_storage_without_truncation = false;
  std::string invalid_encoding_diagnostic_id;
};

struct DatatypeTypeCodecIdentityLookupV1 {
  bool ok = false;
  DatatypeTypeCodecIdentityRowV1 row;
  std::string diagnostic_id;
};

DatatypeTypeCodecIdentityLookupV1 LookupDatatypeTypeCodecIdentityV1(
    const std::string& catalog_snapshot_uuid,
    u64 catalog_generation,
    u64 registry_generation,
    const std::string& descriptor_uuid,
    u64 descriptor_generation);
DatatypeTypeCodecIdentityLookupV1 LookupCanonicalBooleanTypeCodecIdentityV1(
    const std::string& catalog_snapshot_uuid,
    u64 catalog_generation,
    u64 registry_generation);

// The canonical TEXT v1 row is the sole admitted variable-width identity
// whose exact canonical_value_bytes field is zero.  Zero is a descriptor
// width marker, never an empty-payload claim or a fixed-width value.  This
// predicate validates the complete immutable Core row so public projection
// producers and consumers cannot infer the exception from a name or width.
bool IsExactCanonicalTextTypeCodecIdentityV1(
    const DatatypeTypeCodecIdentityRowV1& row);

// The Core boolean v1 row is the sole admitted descriptor/type UUID alias.
// This predicate validates the complete immutable tuple; a UUID match alone
// never grants the exception.  Callers must separately establish that the
// containing-slot nullability is known (nullable or non-null, never unknown).
bool IsExactCanonicalBooleanDescriptorTypeAliasV1(
    const std::string& descriptor_uuid,
    u64 descriptor_generation,
    const std::string& type_uuid,
    u64 type_generation,
    const std::string& codec_id,
    u16 codec_version,
    u64 codec_generation,
    bool containing_slot_nullability_authoritative);

struct BuiltinOperatorTypeCodecIdentityRowV1 {
  std::string operator_snapshot_uuid;
  u64 operator_registry_generation = 0;
  std::string operator_uuid;
  u64 operator_generation = 0;
  std::uint8_t semantic_code = 0;
  std::uint8_t operand_arity = 0;
  std::uint8_t null_behavior_code = 0;
  std::uint8_t accepted_state = 0;
  std::string left_descriptor_uuid;
  u64 left_descriptor_generation = 0;
  std::string left_type_uuid;
  u64 left_type_generation = 0;
  std::string right_descriptor_uuid;
  u64 right_descriptor_generation = 0;
  std::string right_type_uuid;
  u64 right_type_generation = 0;
  std::string result_descriptor_uuid;
  u64 result_descriptor_generation = 0;
  std::string result_type_uuid;
  u64 result_type_generation = 0;
  std::string result_codec_id;
  u16 result_codec_version = 0;
  u64 result_codec_generation = 0;
  std::uint8_t operator_family_code = 0;
  std::uint8_t operand_identity_rule = 0;
  std::uint8_t result_null_encoding_code = 0;
};

struct BuiltinOperatorTypeCodecIdentityLookupV1 {
  bool ok = false;
  BuiltinOperatorTypeCodecIdentityRowV1 row;
  std::string diagnostic_id;
};

struct BuiltinOperatorRegistrySnapshotIdentityLookupV1 {
  bool ok = false;
  std::string snapshot_uuid;
  u64 registry_generation = 0;
  std::string equality_operator_uuid;
  u64 equality_operator_generation = 0;
  std::string diagnostic_id;
};

BuiltinOperatorRegistrySnapshotIdentityLookupV1
LoadCurrentBuiltinOperatorRegistrySnapshotIdentityV1();

BuiltinOperatorTypeCodecIdentityLookupV1
LookupBuiltinOperatorTypeCodecIdentityV1(
    const std::string& operator_snapshot_uuid,
    u64 operator_registry_generation,
    const std::string& operator_uuid,
    u64 operator_generation,
    const std::string& left_descriptor_uuid,
    u64 left_descriptor_generation,
    const std::string& left_type_uuid,
    u64 left_type_generation,
    const std::string& right_descriptor_uuid,
    u64 right_descriptor_generation,
    const std::string& right_type_uuid,
    u64 right_type_generation);

}  // namespace scratchbird::core::datatypes
