// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/engine/sblr/canonical_relational_expression.cpp"
#include "canonical_query_object_free_composition_support.hpp"
#include "../support/binary_uuid_fixture.hpp"
#include <cstdlib>
#include <iostream>
#include <source_location>
#include <bit>
namespace s = scratchbird::engine::sblr;
namespace a = scratchbird::engine::internal_api;
namespace d = scratchbird::core::datatypes;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  if (!ok) { std::cerr << "failure at " << at.line() << '\n'; std::abort(); }
}
int main() {
  // Native POINT coordinates retain numeric semantics and normalize signed
  // floating zero, without parsing execution payloads as decimal text.
  for (const double number : {0.0, -0.0, -1.5, 255.0}) {
    auto value = scratchbird::engine::executor::EncodeReal64Value(number);
    double coordinate = 42;
    Check(s::DecodeSpatialPointCoordinate(value, &coordinate) && coordinate == number);
    if (number == 0.0) Check(std::bit_cast<std::uint64_t>(coordinate) == 0);
    value.encoded_value = "0";
    Check(!s::DecodeSpatialPointCoordinate(value, &coordinate));
  }
  for (const std::int64_t number : {INT64_C(-65536), INT64_C(0), INT64_C(65536)}) {
    auto value = scratchbird::engine::executor::EncodeInt64Value(number);
    double coordinate = 42;
    Check(s::DecodeSpatialPointCoordinate(value, &coordinate) && coordinate == number);
    value.binary_value.pop_back();
    Check(!s::DecodeSpatialPointCoordinate(value, &coordinate));
  }
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
  Check(s::BuildExactCanonicalBooleanRuntimeDescriptorV1(
      bound, a::RelationalNullability::kNonNull, &output));
  std::string canonical, detail;
  for (const auto* text : {"true", "false"}) {
    Check(s::CanonicalizeLiteralPayload("boolean", output, text, &canonical, &detail));
    Check(canonical == text);
  }
  for (const auto* invalid : {"", "TRUE", "False", "1", "0", "truth"}) {
    canonical = "unchanged";
    Check(!s::CanonicalizeLiteralPayload("boolean", output, invalid, &canonical, &detail));
    Check(canonical == "unchanged" && !detail.empty());
  }
  auto stale = output;
  ++stale.datatype_descriptor_generation;
  canonical = "unchanged";
  Check(!s::CanonicalizeLiteralPayload("boolean", stale, "true", &canonical, &detail));
  Check(canonical == "unchanged" && !detail.empty());
  // Planning with no rows and planning from real values must retain exactly
  // the same native descriptor, including datatype UUID and generation.
  for (const auto type : {d::CanonicalTypeId::uuid, d::CanonicalTypeId::binary,
                          d::CanonicalTypeId::boolean, d::CanonicalTypeId::int32,
                          d::CanonicalTypeId::int64, d::CanonicalTypeId::uint64,
                          d::CanonicalTypeId::real64}) {
    const auto identity = std::ranges::find_if(rows, [&](const auto& item) {
      return item.canonical_binary_type_code == static_cast<std::uint32_t>(type);
    });
    Check(identity != rows.end());
    auto descriptor = bound;
    descriptor.descriptor_id = 1;
    descriptor.descriptor_uuid = scratchbird::tests::FixtureUuid(1086, 2);
    descriptor.descriptor_generation = identity->descriptor_generation;
    descriptor.type_uuid = identity->type_uuid;
    descriptor.type_generation = identity->type_generation;
    descriptor.codec_id = identity->codec_id;
    descriptor.codec_version = identity->codec_version;
    descriptor.codec_generation = identity->codec_generation;
    descriptor.datatype_catalog_snapshot_uuid = identity->catalog_snapshot_uuid;
    descriptor.datatype_catalog_generation = identity->catalog_generation;
    descriptor.datatype_registry_generation = identity->registry_generation;
    a::EngineDescriptor runtime_descriptor;
    Check(s::BuildExactCanonicalScalarRuntimeDescriptorV1(descriptor, type, &runtime_descriptor));
    a::TypedRelationalDag dag;
    dag.descriptors = {descriptor};
    a::RelationalExpressionRecord expression;
    expression.expression_id = 1;
    expression.expression_kind = a::RelationalExpressionKind::kIdentifier;
    expression.result_descriptor_id = 1;
    expression.bound_name_uuid = scratchbird::tests::FixtureUuid(1086, 3);
    dag.expressions = {expression};
    dag.outputs = {{1, 2, 1, "native_value", 1, true, 0}};
    scratchbird::engine::planner::CanonicalLogicalRelationalNode root, source;
    root.logical_node_id = 2;
    root.output_descriptor_ids = {1};
    root.bound_expression_ids = {1};
    source.logical_node_id = 1;
    source.output_descriptor_ids = {1};
    s::MaterializedValues input;
    input.ok = true;
    input.batch.columns.push_back({"native_value", runtime_descriptor, false, 1});
    input.result_bindings.resize(1);
    const auto empty = s::PrepareExpressionProjectRootForComposition(dag, root, source, input, {});
    if (!empty.ok) std::cerr << empty.detail << '\n';
    Check(empty.ok && empty.expression_output_batch.rows.empty());
    Check(empty.expression_output_batch.columns.front().descriptor == runtime_descriptor);
    a::EngineTypedValue value;
    value.descriptor = runtime_descriptor;
    if (type == d::CanonicalTypeId::boolean) value.encoded_value = "true";
    else value.binary_value.resize(type == d::CanonicalTypeId::uuid ? 16 :
        type == d::CanonicalTypeId::int32 ? 4 :
        type == d::CanonicalTypeId::int64 || type == d::CanonicalTypeId::uint64 ||
        type == d::CanonicalTypeId::real64 ? 8 : 2, 0);
    input.batch.rows.push_back({{value}});
    const auto populated = s::PrepareExpressionProjectRootForComposition(dag, root, source, input, {});
    if (!populated.ok) std::cerr << populated.detail << '\n';
    Check(populated.ok && populated.expression_output_batch.rows.size() == 1);
    Check(populated.expression_output_batch.columns.front().descriptor == runtime_descriptor);
    const auto& actual = populated.expression_output_batch.rows.front().values.front();
    Check(actual.descriptor == value.descriptor && actual.encoded_value == value.encoded_value &&
          actual.binary_value == value.binary_value && actual.state == value.state &&
          actual.is_null == value.is_null);
    ++dag.descriptors.front().codec_generation;
    Check(!s::PrepareExpressionProjectRootForComposition(dag, root, source, input, {}).ok);
    input.batch.rows.clear();
    Check(!s::PrepareExpressionProjectRootForComposition(dag, root, source, input, {}).ok);
  }
}
