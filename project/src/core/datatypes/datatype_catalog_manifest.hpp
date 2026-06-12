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

}  // namespace scratchbird::core::datatypes
