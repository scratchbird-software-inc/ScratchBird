// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/engine/sblr/canonical_relational_expression.cpp"
#include "canonical_query_object_free_composition_support.hpp"
#include "../support/binary_uuid_fixture.hpp"
#include "query/historical_timestamp_scalar.hpp"
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
  // Frozen UTC cohorts remain distinct from the current civil timestamp.
  unsigned historical_cohorts = 0;
  for (const auto& identity : d::CurrentDatatypeTypeCodecIdentityRowsV1()) {
    if (identity.canonical_name != "timestamp") continue;
    a::RelationalTypeDescriptor source;
    source.datatype_identity_authoritative = true;
    source.descriptor_uuid = scratchbird::tests::FixtureUuid(1086, 900);
    source.statement_receipt_uuid = scratchbird::tests::FixtureUuid(1086, 901);
    source.datatype_catalog_snapshot_uuid = identity.catalog_snapshot_uuid;
    source.datatype_catalog_generation = identity.catalog_generation;
    source.datatype_registry_generation = identity.registry_generation;
    source.descriptor_generation = identity.descriptor_generation;
    source.type_uuid = identity.type_uuid;
    source.type_generation = identity.type_generation;
    source.codec_id = identity.codec_id;
    source.codec_version = identity.codec_version;
    source.codec_generation = identity.codec_generation;
    source.nullability = a::RelationalNullability::kNullable;
    a::EngineDescriptor descriptor;
    std::string detail;
    const bool historical = identity.codec_id == "datatype.timestamp.utc_tuple.le.v1";
    Check(a::BuildHistoricalTimestampScalarDescriptorV1(source, &descriptor, &detail) == historical);
    if (!historical) continue;
    ++historical_cohorts;
    auto stale = source;
    ++stale.codec_generation;
    a::EngineDescriptor untouched;
    untouched.canonical_type_name = "unchanged";
    Check(!a::BuildHistoricalTimestampScalarDescriptorV1(stale, &untouched, &detail));
    Check(untouched.canonical_type_name == "unchanged");
    a::EngineTypedValue value;
    value.descriptor = descriptor;
    value.setState(a::EngineValueState::value);
    value.binary_value.assign(16, 0);
    a::HistoricalTimestampScalarPartsV1 parts;
    Check(a::DecodeHistoricalTimestampScalarV1(value, &parts, &detail));
    Check(parts.unix_seconds == 0 && parts.nanoseconds == 0 && !parts.is_null);
    auto previous = value;
    for (unsigned byte = 0; byte < 8; ++byte) previous.binary_value[byte] = 255;
    int comparison = 9;
    Check(a::CompareHistoricalTimestampScalarsV1(previous, value, &comparison, &detail));
    Check(comparison == -1);
    Check(a::QowCompareCanonicalNonCollatedScalarsV1(previous, value, &comparison, &detail));
    Check(comparison == -1);
    a::EngineSqlTruthValue truth = a::EngineSqlTruthValue::unspecified;
    Check(a::QowEvaluateCanonicalComparisonTruthV1(previous, value, comparison,
        a::EngineComparisonPredicateOperator::less_than, &truth, &detail));
    Check(truth == a::EngineSqlTruthValue::true_value);
    source.descriptor_id = 1;
    auto other_statement = source;
    other_statement.statement_receipt_uuid = scratchbird::tests::FixtureUuid(1086, 902);
    auto crossed_statement = value;
    Check(a::BuildHistoricalTimestampScalarDescriptorV1(other_statement, &crossed_statement.descriptor, &detail));
    Check(!a::CompareHistoricalTimestampScalarsV1(crossed_statement, value, &comparison, &detail));
    a::TypedRelationalDag timestamp_dag;
    timestamp_dag.descriptors = {source};
    a::RelationalExpressionRecord temporal;
    temporal.expression_id = 1;
    temporal.expression_kind = a::RelationalExpressionKind::kLiteral;
    temporal.result_descriptor_id = 1;
    temporal.literal_kind = a::RelationalLiteralKind::kTemporal;
    a::RelationalExpressionRecord::LiteralTypedValueV1 carrier;
    carrier.descriptor_uuid = source.descriptor_uuid;
    carrier.descriptor_generation = source.descriptor_generation;
    carrier.value_state = "value";
    carrier.canonical_value_bytes = previous.binary_value;
    carrier.canonical_value_sha256 = scratchbird::core::hash::ComputeSha256Digest(carrier.canonical_value_bytes).digest;
    temporal.literal_typed_value_v1 = carrier;
    timestamp_dag.expressions = {temporal};
    s::CanonicalRelationalExpressionRuntime runtime(timestamp_dag);
    a::EngineTypedValue evaluated;
    Check(runtime.EvaluateForConsumer(1, "timestamp",
        a::EngineCanonicalExpressionConsumer::projection, &evaluated, &detail));
    Check(evaluated.binary_value == previous.binary_value && evaluated.encoded_value.empty() &&
          evaluated.descriptor == descriptor);
    scratchbird::engine::executor::DescriptorBatch batch;
    batch.columns.push_back({"historical", descriptor, true, 1});
    Check(scratchbird::engine::executor::ValidateDescriptorBatch(batch).ok);
    batch.rows.push_back({{evaluated}});
    const auto batch_result = scratchbird::engine::executor::ValidateDescriptorBatch(batch);
    if (!batch_result.ok) std::cerr << batch_result.diagnostic_code << ':' << batch_result.detail << '\n';
    Check(batch_result.ok);
    ++batch.columns[0].descriptor.datatype_descriptor_generation;
    batch.rows.clear();
    Check(!scratchbird::engine::executor::ValidateDescriptorBatch(batch).ok);
    for (unsigned mutation = 0; mutation < 7; ++mutation) {
      auto bad = value;
      if (mutation == 0) bad.binary_value.pop_back();
      if (mutation == 1) bad.binary_value[12] = 1;
      if (mutation == 2) { bad.binary_value[8] = 0; bad.binary_value[9] = 202;
        bad.binary_value[10] = 154; bad.binary_value[11] = 59; } // 1e9 nanos
      if (mutation == 3) bad.encoded_value = "1970-01-01T00:00:00Z";
      if (mutation == 4) ++bad.descriptor.datatype_descriptor_generation;
      if (mutation == 5) bad.is_null = true;
      if (mutation == 6) bad.setState(a::EngineValueState::sql_null);
      parts.unix_seconds = 42;
      Check(!a::DecodeHistoricalTimestampScalarV1(bad, &parts, &detail));
      Check(parts.unix_seconds == 42 && !detail.empty());
    }
    value.setState(a::EngineValueState::sql_null);
    value.binary_value.clear();
    Check(a::DecodeHistoricalTimestampScalarV1(value, &parts, &detail) && parts.is_null);
    Check(a::QowEvaluateCanonicalComparisonTruthV1(previous, value, 0,
        a::EngineComparisonPredicateOperator::equal, &truth, &detail));
    Check(truth == a::EngineSqlTruthValue::unknown);
    Check(!a::QowEvaluateCanonicalComparisonTruthV1(crossed_statement, value, 0,
        a::EngineComparisonPredicateOperator::equal, &truth, &detail));
  }
  Check(historical_cohorts != 0);
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
    if (type == d::CanonicalTypeId::uint64 || type == d::CanonicalTypeId::real64) {
      auto literal_dag = dag;
      auto& literal = literal_dag.expressions.front();
      literal.expression_kind = a::RelationalExpressionKind::kLiteral;
      literal.bound_name_uuid.reset();
      literal.literal_kind = a::RelationalLiteralKind::kNumeric;
      a::RelationalExpressionRecord::LiteralTypedValueV1 carrier;
      carrier.descriptor_uuid = descriptor.descriptor_uuid;
      carrier.descriptor_generation = descriptor.descriptor_generation;
      carrier.value_state = "value";
      carrier.canonical_value_bytes = type == d::CanonicalTypeId::uint64
          ? scratchbird::engine::executor::EncodeUint64Value(UINT64_MAX).binary_value
          : scratchbird::engine::executor::EncodeReal64Value(-1.5).binary_value;
      carrier.canonical_value_sha256 = scratchbird::core::hash::ComputeSha256Digest(carrier.canonical_value_bytes).digest;
      literal.literal_typed_value_v1 = carrier;
      s::CanonicalRelationalExpressionRuntime runtime(literal_dag);
      a::EngineTypedValue native_literal;
      Check(runtime.EvaluateForConsumer(1, runtime_descriptor.canonical_type_name,
          a::EngineCanonicalExpressionConsumer::projection, &native_literal, &detail));
      Check(native_literal.encoded_value.empty() && native_literal.binary_value == carrier.canonical_value_bytes &&
            native_literal.descriptor == runtime_descriptor);
      ++literal.literal_typed_value_v1->descriptor_generation;
      s::CanonicalRelationalExpressionRuntime stale_literal(literal_dag);
      Check(!stale_literal.EvaluateForConsumer(1, runtime_descriptor.canonical_type_name,
          a::EngineCanonicalExpressionConsumer::projection, &native_literal, &detail));
      Check(native_literal.state == a::EngineValueState::error && native_literal.binary_value.empty());
    }
    ++dag.descriptors.front().codec_generation;
    Check(!s::PrepareExpressionProjectRootForComposition(dag, root, source, input, {}).ok);
    input.batch.rows.clear();
    Check(!s::PrepareExpressionProjectRootForComposition(dag, root, source, input, {}).ok);
  }
}
