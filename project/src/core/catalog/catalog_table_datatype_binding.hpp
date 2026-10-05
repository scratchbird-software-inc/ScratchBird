// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_table_definition.hpp"
#include "datatype_interval.hpp"
#include <array>

namespace scratchbird::core::catalog {
// A durable occurrence, not a datatype registry identity or statement receipt.
// All members are owned fixed values. No host object image is persisted.
struct CatalogIntervalOccurrence {
  CatalogDefinitionReference occurrence;
  std::array<byte, datatypes::kIntervalProfileMaterialBytesV3> profile_material{};
  std::array<byte, 32> profile_fingerprint{};
};
enum class CatalogIntervalBindingError : u8 {
  none, invalid_profile, invalid_column, invalid_occurrence_metadata,
  invalid_payload, invalid_occurrence, profile_mismatch, column_binding_mismatch
};
struct CatalogIntervalBindingFailure {
  CatalogIntervalBindingError error = CatalogIntervalBindingError::none;
  CatalogValueError value_error = CatalogValueError::none;
  std::optional<datatypes::IntervalDiagnosticFactV3> datatype_diagnostic;
  std::optional<CatalogRecordDiagnosticView> metadata_diagnostic;
};
struct CatalogIntervalOccurrenceResult : CatalogIntervalBindingFailure {
  std::optional<CatalogIntervalOccurrence> definition;
  bool ok() const { return error == CatalogIntervalBindingError::none && definition.has_value(); }
};
struct CatalogIntervalOccurrenceEncodeResult : CatalogIntervalBindingFailure {
  std::vector<byte> bytes;
  bool ok() const { return error == CatalogIntervalBindingError::none; }
};
struct CatalogIntervalColumnBinding {
  CatalogColumnDefinition column;
  CatalogIntervalOccurrence value_definition;
};
struct CatalogIntervalColumnBindingResult : CatalogIntervalBindingFailure {
  std::optional<CatalogIntervalColumnBinding> binding;
  bool ok() const { return error == CatalogIntervalBindingError::none && binding.has_value(); }
};
CatalogValueSchemaView CatalogIntervalOccurrenceSchemaView();
// These APIs never select current authority, issue an identity or repair a
// binding. Caller retains immutable profile/source throughout the call. The
// supplied control is consumed by the datatype validator; it is not a catalog
// memory grant. Owning encoding allocations may throw std::bad_alloc.
CatalogIntervalOccurrenceEncodeResult EncodeCatalogIntervalOccurrence(
    const CatalogIntervalOccurrence&, const datatypes::IntervalValidatedProfileHandleV3&,
    const datatypes::IntervalExecutionControlV3& = {});
CatalogIntervalOccurrenceResult DecodeCatalogIntervalOccurrence(
    std::string_view, const datatypes::IntervalValidatedProfileHandleV3&,
    const datatypes::IntervalExecutionControlV3& = {});
// Complete caller-selected metadata only, not native lookup/visibility/security.
// No heap allocations, borrowed result data, identity issuance or storage effects.
CatalogIntervalColumnBindingResult BindCatalogIntervalColumn(
    const CatalogMetadataVersionView& column, const CatalogMetadataVersionView& occurrence,
    const datatypes::IntervalValidatedProfileHandleV3&,
    const datatypes::IntervalExecutionControlV3& = {});
}  // namespace scratchbird::core::catalog
