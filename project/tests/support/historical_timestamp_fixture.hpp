// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "binary_uuid_fixture.hpp"
#include "catalog/datatype_bootstrap_identity.hpp"
#include "query/historical_timestamp_scalar.hpp"
#include "datatype_temporal_wire.hpp"
#include <stdexcept>

namespace scratchbird::tests {

// Component-only bootstrap binding. Live route fixtures must bind their own
// engine-issued statement receipt and catalog cohort instead of using this.
inline engine::internal_api::EngineDescriptor HistoricalTimestampFixtureDescriptor(
    engine::internal_api::EngineUuid occurrence, bool nullable = false) {
  namespace api = engine::internal_api;
  for (const auto& row : core::datatypes::CurrentDatatypeTypeCodecIdentityRowsV1()) {
    if (row.catalog_snapshot_uuid != api::kBootstrapDatatypeCatalogUuid ||
        row.catalog_generation != api::kBootstrapDatatypeCatalogGeneration ||
        row.registry_generation != api::kBootstrapDatatypeRegistryGeneration ||
        row.canonical_name != "timestamp") continue;
    api::RelationalTypeDescriptor source;
    source.descriptor_uuid = occurrence;
    source.descriptor_generation = row.descriptor_generation;
    source.type_uuid = row.type_uuid;
    source.type_generation = row.type_generation;
    source.codec_id = row.codec_id;
    source.codec_version = row.codec_version;
    source.codec_generation = row.codec_generation;
    source.datatype_catalog_snapshot_uuid = row.catalog_snapshot_uuid;
    source.datatype_catalog_generation = row.catalog_generation;
    source.datatype_registry_generation = row.registry_generation;
    source.statement_receipt_uuid = FixtureUuid(1401, 500);
    source.datatype_identity_authoritative = true;
    source.nullability = nullable ? api::RelationalNullability::kNullable : api::RelationalNullability::kNonNull;
    api::EngineDescriptor result;
    std::string detail;
    if (!api::BuildHistoricalTimestampScalarDescriptorV1(source, &result, &detail))
      throw std::runtime_error(detail);
    return result;
  }
  throw std::runtime_error("historical timestamp fixture cohort is absent");
}

inline engine::internal_api::EngineTypedValue HistoricalTimestampFixtureValue(
    const engine::internal_api::EngineDescriptor& descriptor, std::string_view text,
    bool is_null = false) {
  namespace api = engine::internal_api;
  api::EngineTypedValue result;
  result.descriptor = descriptor;
  result.setState(is_null ? api::EngineValueState::sql_null : api::EngineValueState::value);
  if (!is_null) {
    core::datatypes::ReferenceTemporalWireProfileRequest request;
    request.wire_profile = "timestamp_timezone_profile";
    request.encoded_value = text;
    request.fractional_second_precision = 9;
    request.require_timezone_seed = false;
    const auto parsed = core::datatypes::ValidateReferenceTemporalWireProfile(request);
    if (!parsed.ok() || !parsed.comparable_utc_key_available || parsed.used_timezone_seed ||
        parsed.comparable_fractional_picoseconds % 1000 != 0)
      throw std::runtime_error("invalid timestamp boundary fixture");
    const auto seconds = static_cast<std::uint64_t>(parsed.comparable_utc_whole_seconds);
    const auto nanos = static_cast<std::uint32_t>(parsed.comparable_fractional_picoseconds / 1000);
    result.binary_value.assign(16, 0);
    for (unsigned i = 0; i < 8; ++i) result.binary_value[i] = static_cast<std::uint8_t>(seconds >> (8*i));
    for (unsigned i = 0; i < 4; ++i) result.binary_value[8+i] = static_cast<std::uint8_t>(nanos >> (8*i));
  }
  api::HistoricalTimestampScalarPartsV1 checked;
  std::string detail;
  if (!api::DecodeHistoricalTimestampScalarV1(result, &checked, &detail)) throw std::runtime_error(detail);
  return result;
}
} // namespace scratchbird::tests
