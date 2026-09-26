// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../support/binary_uuid_fixture.hpp"
#include "core/datatypes/datatype_catalog_manifest.hpp"
#include "engine/internal_api/catalog/column_metadata_codec.hpp"
#include "engine/sblr/canonical_relational_expression.hpp"
#include <cstdlib>
#include <iostream>

namespace api = scratchbird::engine::internal_api;
namespace sblr = scratchbird::engine::sblr;
namespace dt = scratchbird::core::datatypes;
using scratchbird::tests::FixtureUuidLiteral;
int main() {
  unsigned checks = 0, failures = 0;
  const auto check = [&](bool value, const char* message) {
    ++checks;
    if (!value) { ++failures; std::cerr << message << '\n'; }
  };
  const auto manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
  if (!manifest.ok()) return EXIT_FAILURE;
  const auto selected = dt::LookupDatatypeCatalogRow(manifest.manifest, dt::CanonicalTypeId::int64);
  if (!selected.ok() || selected.manifest.descriptor_rows.size() != 1) return EXIT_FAILURE;
  const auto& datatype = selected.manifest.descriptor_rows.front();
  const auto codec = dt::LookupDatatypeTypeCodecIdentityV1(
      FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d702"), 2, 2,
      datatype.descriptor_uuid.value, datatype.descriptor_epoch);
  if (!codec.ok) { std::cerr << "current datatype fixture cohort unavailable\n"; return EXIT_FAILURE; }
  const auto* identity = &codec.row;
  const auto column = FixtureUuidLiteral("019d0000-0000-7000-8000-000000001101");
  const auto occurrence = FixtureUuidLiteral("019d0000-0000-7000-8000-000000001102");
  api::RelationalTypeDescriptor bound;
  bound.descriptor_id = 1;
  bound.descriptor_uuid = occurrence;
  bound.type_uuid = identity->type_uuid;
  bound.nullability = api::RelationalNullability::kNullable;
  bound.datatype_identity_authoritative = true;
  bound.descriptor_generation = identity->descriptor_generation;
  bound.type_generation = identity->type_generation;
  bound.codec_id = identity->codec_id;
  bound.codec_version = identity->codec_version;
  bound.codec_generation = identity->codec_generation;
  bound.statement_receipt_uuid = FixtureUuidLiteral("019d0000-0000-7000-8000-000000001103");
  bound.datatype_catalog_snapshot_uuid = identity->catalog_snapshot_uuid;
  bound.datatype_catalog_generation = identity->catalog_generation;
  bound.datatype_registry_generation = identity->registry_generation;
  api::TypedRelationalDag dag;
  dag.descriptors.push_back(bound);
  api::RelationalExpressionRecord identifier;
  identifier.expression_id = 1;
  identifier.expression_kind = api::RelationalExpressionKind::kIdentifier;
  identifier.result_descriptor_id = 1;
  identifier.bound_name_uuid = column;
  dag.expressions.push_back(identifier);
  sblr::CanonicalRelationalExpressionRowBinding binding;
  binding.row_descriptor_ids = {1};
  binding.slots.push_back({1, 1, 0, sblr::CanonicalRelationalExpressionRowSlotKind::input_identifier});
  api::EngineTypedValue value;
  value.state = api::EngineValueState::value;
  value.encoded_value = "5";
  value.descriptor.descriptor_kind = "scalar";
  value.descriptor.canonical_type_name = "int64";
  value.descriptor.descriptor_uuid = occurrence;
  value.descriptor.type_uuid = identity->type_uuid;
  value.descriptor.datatype_descriptor_uuid = identity->descriptor_uuid;
  value.descriptor.datatype_descriptor_generation = identity->descriptor_generation;
  api::CatalogColumnMetadata metadata;
  metadata.identities = {{"column_uuid", column}, {"type_uuid", identity->type_uuid},
                         {"datatype_descriptor_uuid", identity->descriptor_uuid}};
  if (!identity->codec_uuid.is_nil()) metadata.identities["codec_uuid"] = identity->codec_uuid;
  metadata.text = {{"type", "int64"}, {"nullable", "true"},
      {"datatype_descriptor_generation", std::to_string(identity->descriptor_generation)},
      {"type_generation", std::to_string(identity->type_generation)},
      {"codec_id", identity->codec_id}, {"codec_version", std::to_string(identity->codec_version)},
      {"codec_generation", std::to_string(identity->codec_generation)},
      {"null_encoding", std::to_string(identity->null_encoding_code)}};
  if (!api::EncodeCatalogColumnMetadata(metadata, &value.descriptor.encoded_descriptor)) return EXIT_FAILURE;
  const auto accepted = [&](const auto& plan, const auto& cell, bool explain = false) {
    sblr::CanonicalRelationalExpressionRuntime runtime(plan);
    std::string type, detail;
    const bool ok = runtime.InferTypeForConsumer(1, binding, {cell},
        api::EngineCanonicalExpressionConsumer::filter, &type, &detail);
    if (!ok && explain) std::cerr << "descriptor_detail=" << detail << '\n';
    return ok && type == "int64";
  };
  check(column != occurrence && occurrence != identity->descriptor_uuid &&
            column != identity->type_uuid,
        "fixture did not separate column, occurrence and datatype identities");
  check(accepted(dag, value, true), "valid distinct native column identity was rejected");
  for (unsigned mutation = 0; mutation != 6; ++mutation) {
    auto invalid_dag = dag;
    auto invalid_value = value;
    auto invalid_metadata = metadata;
    if (mutation == 0) invalid_dag.expressions[0].bound_name_uuid = occurrence;
    if (mutation == 1) invalid_metadata.identities["column_uuid"] = occurrence;
    if (mutation == 2) invalid_dag.expressions[0].bound_name_uuid.reset();
    if (mutation == 3) {
      auto extra = identifier;
      extra.expression_id = 2;
      extra.bound_name_uuid = occurrence;
      invalid_dag.expressions.push_back(extra);
    }
    if (mutation == 4) ++invalid_dag.descriptors[0].datatype_registry_generation;
    if (mutation == 5) invalid_metadata.identities["type_uuid"] = occurrence;
    if (!api::EncodeCatalogColumnMetadata(invalid_metadata, &invalid_value.descriptor.encoded_descriptor))
      return EXIT_FAILURE;
    check(!accepted(invalid_dag, invalid_value), "crossed or stale persisted identity was admitted");
  }
  check(accepted(dag, value), "negative requests changed valid descriptor binding");
  std::cout << "persisted_column_binding_identity_checks=" << checks << " failures=" << failures << '\n';
  return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
