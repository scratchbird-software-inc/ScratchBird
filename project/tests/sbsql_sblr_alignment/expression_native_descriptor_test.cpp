// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/engine/sblr/canonical_relational_expression.cpp"
#include "../support/binary_uuid_fixture.hpp"
#include <cstdlib>
#include <iostream>
#include <source_location>
namespace s = scratchbird::engine::sblr;
namespace a = scratchbird::engine::internal_api;
namespace d = scratchbird::core::datatypes;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  if (!ok) { std::cerr << "failure at " << at.line() << '\n'; std::abort(); }
}
int main() {
  const auto rows = d::CurrentDatatypeTypeCodecIdentityRowsV1();
  const auto row = std::ranges::find_if(rows, [](const auto& r) {
    return r.canonical_name == "boolean";
  });
  Check(row != rows.end());
  a::RelationalTypeDescriptor bound;
  bound.datatype_identity_authoritative = true;
  bound.statement_receipt_uuid = scratchbird::tests::FixtureUuid(1086, 1);
  bound.datatype_catalog_snapshot_uuid = row->catalog_snapshot_uuid;
  bound.datatype_catalog_generation = row->catalog_generation;
  bound.datatype_registry_generation = row->registry_generation;
  bound.descriptor_uuid = row->descriptor_uuid;
  bound.descriptor_generation = row->descriptor_generation;
  bound.type_uuid = row->type_uuid;
  bound.type_generation = row->type_generation;
  bound.codec_id = row->codec_id;
  bound.codec_version = row->codec_version;
  bound.codec_generation = row->codec_generation;
  bound.nullability = a::RelationalNullability::kNonNull;
  a::EngineDescriptor output;
  Check(s::BuildExactCanonicalBooleanRuntimeDescriptorV1(
      bound, a::RelationalNullability::kNullable, &output));
  Check(output.descriptor_uuid == row->descriptor_uuid &&
        output.type_uuid == row->type_uuid &&
        output.datatype_descriptor_uuid == row->descriptor_uuid &&
        output.datatype_descriptor_generation == row->descriptor_generation);
  Check(output.encoded_descriptor.find("uuid=") == std::string::npos &&
        output.encoded_descriptor.ends_with("nullability=nullable"));
  auto crossed = bound;
  ++crossed.codec_generation;
  Check(!s::BuildExactCanonicalBooleanRuntimeDescriptorV1(
      crossed, a::RelationalNullability::kNullable, &output));
  crossed = bound; crossed.statement_receipt_uuid = {};
  Check(!s::BuildExactCanonicalBooleanRuntimeDescriptorV1(
      crossed, a::RelationalNullability::kNullable, &output));
  crossed = bound; crossed.datatype_catalog_snapshot_uuid.bytes[15] ^= 1;
  Check(!s::BuildExactCanonicalBooleanRuntimeDescriptorV1(
      crossed, a::RelationalNullability::kNullable, &output));
  crossed = bound; crossed.statement_receipt_uuid.bytes[6] = 0x40;
  Check(!s::BuildExactCanonicalBooleanRuntimeDescriptorV1(
      crossed, a::RelationalNullability::kNullable, &output));
}
