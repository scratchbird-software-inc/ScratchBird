// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "datatype_catalog_manifest.hpp"

#include <cstdlib>
#include <iostream>
#include <set>
#include <string_view>

namespace {

namespace dt = scratchbird::core::datatypes;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool HasDiagnostic(const dt::DatatypeCatalogManifestResult& result,
                   std::string_view diagnostic_code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.diagnostic_code == diagnostic_code) {
      return true;
    }
  }
  return false;
}

bool TypedUuidEquals(const scratchbird::core::platform::TypedUuid& left,
                     const scratchbird::core::platform::TypedUuid& right) {
  return left.kind == right.kind && left.value == right.value;
}

void TestDescriptorCatalogLoadsAllCanonicalRows() {
  const auto loaded = dt::LoadCurrentCoreDatatypeCatalogManifest();
  Require(loaded.ok(), "MDF-012 catalog loader failed");
  Require(loaded.manifest.manifest_key ==
              dt::kCurrentCoreDatatypeCatalogManifestKey,
          "MDF-012 catalog manifest key mismatch");
  Require(loaded.manifest.descriptor_rows.size() ==
              dt::BuiltinDatatypeDescriptors().size(),
          "MDF-012 descriptor catalog row count mismatch");
  Require(loaded.manifest.layout_rows.size() ==
              dt::BuiltinDatatypeDescriptors().size(),
          "MDF-012 layout catalog row count mismatch");
  Require(loaded.manifest.trace_rows.size() ==
              dt::BuiltinDatatypeDescriptors().size(),
          "MDF-012 trace row count mismatch");

  std::set<std::string> sys_tables;
  for (const auto& row : loaded.manifest.descriptor_rows) {
    sys_tables.insert(row.sys_table_name);
    Require(row.descriptor_uuid.valid(), "MDF-012 descriptor UUID was nil");
    Require(row.descriptor_authoritative,
            "MDF-012 descriptor row must be authoritative");
    Require(row.reference_name_is_alias_only,
            "MDF-012 reference names must remain alias-only");
  }
  for (const auto& row : loaded.manifest.layout_rows) {
    sys_tables.insert(row.sys_table_name);
  }
  Require(sys_tables.count("sys.datatype_descriptor") == 1,
          "MDF-012 sys.datatype_descriptor missing");
  Require(sys_tables.count("sys.datatype_storage_layout") == 1,
          "MDF-012 sys.datatype_storage_layout missing");
}

void TestStableUuidAndCacheInvalidation() {
  const auto first = dt::LoadCurrentCoreDatatypeCatalogManifest();
  const auto second = dt::LoadCurrentCoreDatatypeCatalogManifest();
  Require(TypedUuidEquals(first.manifest.descriptor_rows.front().descriptor_uuid,
                          second.manifest.descriptor_rows.front().descriptor_uuid),
          "MDF-012 stable_descriptor_uuid changed across reload");

  dt::DatatypeCatalogCache cache;
  const auto loaded = cache.Load(first.manifest);
  Require(loaded.ok(), "MDF-012 catalog cache load failed");
  const auto generation = cache.generation();
  const auto looked_up = cache.Lookup(dt::CanonicalTypeId::uuid);
  Require(looked_up.ok(), "MDF-012 catalog cache lookup failed");
  cache.Invalidate();
  Require(cache.generation() == generation + 1,
          "MDF-012 catalog cache generation did not advance");
  const auto invalidated = cache.Lookup(dt::CanonicalTypeId::uuid);
  Require(!invalidated.ok(), "MDF-012 invalidated catalog cache was readable");
  Require(HasDiagnostic(invalidated,
                        "SB-DATATYPE-CATALOG-CACHE-INVALIDATED"),
          "MDF-012 cache invalidation diagnostic not emitted");
}

void TestFailClosedCatalogValidation() {
  auto loaded = dt::LoadCurrentCoreDatatypeCatalogManifest();
  loaded.manifest.descriptor_rows[0].descriptor_authoritative = false;
  const auto authority =
      dt::ValidateDatatypeCatalogManifest(loaded.manifest);
  Require(!authority.ok(),
          "MDF-012 accepted non-authoritative descriptor catalog row");
  Require(HasDiagnostic(authority,
                        "SB-DATATYPE-CATALOG-AUTHORITY-VIOLATION"),
          "MDF-012 authority diagnostic not emitted");

  loaded = dt::LoadCurrentCoreDatatypeCatalogManifest();
  loaded.manifest.trace_rows.pop_back();
  const auto missing_trace =
      dt::ValidateDatatypeCatalogManifest(loaded.manifest);
  Require(!missing_trace.ok(), "MDF-012 accepted missing trace row");
  Require(HasDiagnostic(missing_trace,
                        "SB-DATATYPE-CATALOG-TRACE-ROW-MISSING"),
          "MDF-012 missing trace diagnostic not emitted");

  const auto unknown =
      dt::LookupDatatypeCatalogRow(
          dt::LoadCurrentCoreDatatypeCatalogManifest().manifest,
          dt::CanonicalTypeId::unknown);
  Require(!unknown.ok(), "MDF-012 accepted unknown canonical type");
  Require(HasDiagnostic(
              unknown,
              "SB-DATATYPE-CATALOG-UNSUPPORTED-CANONICAL-TYPE"),
          "MDF-012 unsupported canonical type diagnostic not emitted");
}

}  // namespace

int main() {
  // MDF-012-CURRENT-CORE-DATATYPE-CATALOG-MANIFEST
  // DEFER-DTYPE-DESCRIPTOR-IMPLEMENTATION
  // DEFER-DTYPE-CATALOG-DDL
  // DEFER-DTYPE-CLOSURE-MATRIX-TRACE
  TestDescriptorCatalogLoadsAllCanonicalRows();
  TestStableUuidAndCacheInvalidation();
  TestFailClosedCatalogValidation();
  std::cout << "current_core_datatype_catalog_manifest_gate=passed\n";
  return EXIT_SUCCESS;
}
