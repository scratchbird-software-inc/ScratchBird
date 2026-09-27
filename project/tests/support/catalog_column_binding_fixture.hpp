// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "../../src/engine/internal_api/catalog/datatype_bootstrap_identity.hpp"
#include "../../src/engine/internal_api/mga_relation_store/mga_relation_store.hpp"
#include "../../src/core/datatypes/datatype_catalog_manifest.hpp"
#include "../../src/core/datatypes/datatype_operations.hpp"
#include <stdexcept>

namespace scratchbird::tests {
// For test database publishers only. Consumers must keep the receipt read from
// their actual catalog; this helper is not a replacement for name resolution.
inline void UseBootstrapDatatypeCohort(engine::internal_api::EngineRequestContext& context) {
  namespace api = engine::internal_api;
  context.datatype_catalog_snapshot_uuid = api::kBootstrapDatatypeCatalogUuid;
  context.datatype_catalog_generation = api::kBootstrapDatatypeCatalogGeneration;
  context.datatype_registry_generation = api::kBootstrapDatatypeRegistryGeneration;
}

inline void BindFixtureColumnDatatype(
    const engine::internal_api::EngineRequestContext& context,
    core::datatypes::CanonicalTypeId type,
    engine::internal_api::EngineColumnDefinition& column) {
  namespace api = engine::internal_api;
  namespace dt = core::datatypes;
  if (context.datatype_catalog_snapshot_uuid != api::kBootstrapDatatypeCatalogUuid ||
      context.datatype_catalog_generation != api::kBootstrapDatatypeCatalogGeneration ||
      context.datatype_registry_generation != api::kBootstrapDatatypeRegistryGeneration)
    throw std::invalid_argument("fixture publisher is not using the current bootstrap cohort");
  const auto manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
  if (!manifest.ok()) throw std::runtime_error("fixture datatype catalog unavailable");
  const auto result = dt::LookupDatatypeCatalogRow(manifest.manifest, type);
  if (!result.ok() || result.manifest.descriptor_rows.size() != 1)
    throw std::invalid_argument("fixture datatype missing or ambiguous");
  const auto& descriptor = result.manifest.descriptor_rows.front();
  const auto binding = dt::LookupDatatypeTypeCodecIdentityV1(
      context.datatype_catalog_snapshot_uuid, context.datatype_catalog_generation,
      context.datatype_registry_generation, descriptor.descriptor_uuid.value,
      descriptor.descriptor_epoch);
  if (column.requested_column_uuid.is_nil())
    column.requested_column_uuid = api::GenerateCrudEngineUuid("object");
  if (column.descriptor.descriptor_uuid.is_nil())
    column.descriptor.descriptor_uuid = api::GenerateCrudEngineUuid("object");
  column.descriptor.datatype_descriptor_uuid = descriptor.descriptor_uuid.value;
  column.descriptor.datatype_descriptor_generation = descriptor.descriptor_epoch;
  // The manifest is wider than the codec registry. A manifest-only datatype
  // retains its catalog identity; it does not acquire an invented codec tuple.
  column.descriptor.type_uuid = binding.ok ? binding.row.type_uuid : descriptor.descriptor_uuid.value;
}
} // namespace scratchbird::tests
