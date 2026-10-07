// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "binary_uuid_fixture.hpp"
#include "catalog/datatype_bootstrap_identity.hpp"
#include "datatype_catalog_manifest.hpp"
#include "engine/executor/descriptor_value_runtime.hpp"
#include "engine/internal_api/query/plan_api.hpp"
#include "hash_digest.hpp"
#include <stdexcept>

namespace scratchbird::tests {

inline engine::internal_api::RelationalTypeDescriptor Int64LiteralDescriptor(
    std::uint32_t id) {
  namespace api = engine::internal_api;
  const auto builtin = engine::executor::MakeExecutorDescriptor("int64");
  const auto identity = core::datatypes::LookupDatatypeTypeCodecIdentityV1(
      api::kBootstrapDatatypeCatalogUuid, api::kBootstrapDatatypeCatalogGeneration,
      api::kBootstrapDatatypeRegistryGeneration, builtin.datatype_descriptor_uuid,
      builtin.datatype_descriptor_generation);
  if (!identity.ok) throw std::runtime_error("INT64 fixture codec unavailable");
  const auto& row = identity.row;
  api::RelationalTypeDescriptor result;
  result.descriptor_id = id;
  result.descriptor_uuid = row.descriptor_uuid;
  result.type_uuid = row.type_uuid;
  result.nullability = api::RelationalNullability::kNonNull;
  result.datatype_identity_authoritative = true;
  result.descriptor_generation = row.descriptor_generation;
  result.type_generation = row.type_generation;
  result.codec_id = row.codec_id;
  result.codec_version = row.codec_version;
  result.codec_generation = row.codec_generation;
  result.statement_receipt_uuid = FixtureUuid(1401, 500);
  result.datatype_catalog_snapshot_uuid = row.catalog_snapshot_uuid;
  result.datatype_catalog_generation = row.catalog_generation;
  result.datatype_registry_generation = row.registry_generation;
  return result;
}

inline void SetInt64Literal(engine::internal_api::RelationalExpressionRecord& expression,
    const engine::internal_api::RelationalTypeDescriptor& descriptor,
    std::int64_t value) {
  engine::internal_api::RelationalExpressionRecord::LiteralTypedValueV1 typed;
  typed.descriptor_uuid = descriptor.descriptor_uuid;
  typed.descriptor_generation = descriptor.descriptor_generation;
  typed.value_state = "value";
  const auto bits = static_cast<std::uint64_t>(value);
  for (unsigned shift = 0; shift < 64; shift += 8)
    typed.canonical_value_bytes.push_back(static_cast<std::uint8_t>(bits >> shift));
  const auto digest = core::hash::ComputeSha256Digest(typed.canonical_value_bytes);
  if (!digest.ok()) throw std::runtime_error("INT64 fixture digest unavailable");
  typed.canonical_value_sha256 = digest.digest;
  expression.literal_or_parameter_ref.reset();
  expression.literal_typed_value_v1 = std::move(typed);
}

inline constexpr const char* kInvalidInt64LiteralCases[] = {
    "missing authority", "missing statement receipt", "wrong cohort", "catalog generation",
    "registry generation", "descriptor UUID", "descriptor generation", "type UUID",
    "type generation", "codec id", "codec version", "codec generation",
    "unknown nullability", "collation", "timezone", "width", "precision", "scale",
    "literal descriptor", "literal generation", "literal state", "literal digest",
    "short payload", "oversized payload", "mixed carriers", "wrong literal kind"};

inline void InvalidateInt64Literal(unsigned which,
    engine::internal_api::RelationalTypeDescriptor& descriptor,
    engine::internal_api::RelationalExpressionRecord& expression) {
  namespace api = engine::internal_api;
  auto& typed = *expression.literal_typed_value_v1;
  switch (which) {
    case 0: descriptor.datatype_identity_authoritative = false; break;
    case 1: descriptor.statement_receipt_uuid = {}; break;
    case 2: descriptor.datatype_catalog_snapshot_uuid = {}; break;
    case 3: ++descriptor.datatype_catalog_generation; break;
    case 4: ++descriptor.datatype_registry_generation; break;
    case 5: descriptor.descriptor_uuid = FixtureUuid(1401, 501); break;
    case 6: ++descriptor.descriptor_generation; break;
    case 7: descriptor.type_uuid = FixtureUuid(1401, 502); break;
    case 8: ++descriptor.type_generation; break;
    case 9: descriptor.codec_id = "unexpected-codec"; break;
    case 10: ++descriptor.codec_version; break;
    case 11: ++descriptor.codec_generation; break;
    case 12: descriptor.nullability = api::RelationalNullability::kUnknown; break;
    case 13: descriptor.collation_uuid = FixtureUuid(1401, 504); break;
    case 14: descriptor.timezone_profile_id = "unexpected"; break;
    case 15: descriptor.width = 64; break;
    case 16: descriptor.precision = 19; break;
    case 17: descriptor.scale = 0; break;
    case 18: typed.descriptor_uuid = FixtureUuid(1401, 505); break;
    case 19: ++typed.descriptor_generation; break;
    case 20: typed.value_state = "sql_null"; break;
    case 21: typed.canonical_value_sha256[0] ^= 1; break;
    case 22: typed.canonical_value_bytes.pop_back(); break;
    case 23: typed.canonical_value_bytes.push_back(0); break;
    case 24: expression.literal_or_parameter_ref = "17"; break;
    case 25: expression.literal_kind = api::RelationalLiteralKind::kString; break;
    default: throw std::runtime_error("unknown INT64 fixture mutation");
  }
  // Wrong widths must fail codec admission, not merely their old digest.
  if (which == 22 || which == 23) {
    const auto digest = core::hash::ComputeSha256Digest(typed.canonical_value_bytes);
    if (!digest.ok()) throw std::runtime_error("INT64 mutation digest unavailable");
    typed.canonical_value_sha256 = digest.digest;
  }
}

}  // namespace scratchbird::tests
