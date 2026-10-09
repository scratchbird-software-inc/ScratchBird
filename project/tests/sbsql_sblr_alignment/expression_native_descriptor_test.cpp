// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/engine/sblr/canonical_relational_expression.cpp"
#include "canonical_query_object_free_composition_support.hpp"
#include "canonical_query_aggregate_registration.hpp"
#include "canonical_aggregate_registry.hpp"
#include "../support/binary_uuid_fixture.hpp"
#include "query/historical_timestamp_scalar.hpp"
#include "mga_relation_store/stored_scalar_descriptor.hpp"
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
    a::EngineRequestContext storage_context;
    storage_context.datatype_catalog_snapshot_uuid = identity.catalog_snapshot_uuid;
    storage_context.datatype_catalog_generation = identity.catalog_generation;
    storage_context.datatype_registry_generation = identity.registry_generation;
    storage_context.statement_receipt_uuid = source.statement_receipt_uuid;
    a::EngineDescriptor stored;
    stored.descriptor_uuid = source.descriptor_uuid;
    stored.descriptor_kind = "canonical_type_descriptor";
    stored.canonical_type_name = "timestamp";
    stored.datatype_descriptor_uuid = identity.descriptor_uuid;
    stored.datatype_descriptor_generation = identity.descriptor_generation;
    stored.type_uuid = identity.type_uuid;
    a::CatalogColumnMetadata metadata;
    metadata.identities = {{"type_uuid", identity.type_uuid},
                           {"datatype_descriptor_uuid", identity.descriptor_uuid},
                           {"codec_uuid", identity.codec_uuid}};
    metadata.text = {{"canonical", "timestamp"}, {"nullable", "true"},
                     {"datatype_descriptor_generation", std::to_string(identity.descriptor_generation)},
                     {"type_generation", std::to_string(identity.type_generation)},
                     {"codec_id", identity.codec_id}, {"codec_version", std::to_string(identity.codec_version)},
                     {"codec_generation", std::to_string(identity.codec_generation)},
                     {"null_encoding", std::to_string(identity.null_encoding_code)}};
    Check(a::EncodeCatalogColumnMetadata(metadata, &stored.encoded_descriptor));
    a::EngineDescriptor projected;
    Check(a::ProjectStoredHistoricalTimestampDescriptorV1(storage_context, stored, true, &projected, &detail) == historical);
    Check(a::StoredScalarProjectionRequiredV1(stored));
    a::EngineDescriptor dispatched;
    Check(a::ProjectStoredScalarDescriptorV1(storage_context, stored, true, &dispatched, &detail) == historical);
    if (historical) Check(dispatched == projected);
    if (!historical) {
      std::array<std::uint8_t, 16> epoch{};
      std::int64_t unchanged = 42;
      Check(!a::DecodeHistoricalTimestampStoredNanosecondsV1(identity, epoch, &unchanged, &detail) && unchanged == 42);
      continue;
    }
    Check(projected == descriptor);
    // Preserve an explicitly declared frozen UTC publication profile without
    // inventing it for unannotated storage or admitting civil/timezone codecs.
    auto profiled_source = source;
    profiled_source.timezone_profile_id = "timestamp_timezone_profile";
    a::EngineDescriptor profiled;
    Check(a::BuildHistoricalTimestampScalarDescriptorV1(profiled_source, &profiled, &detail));
    a::CatalogColumnMetadata profile_metadata;
    Check(a::DecodeCatalogColumnMetadata(profiled.encoded_descriptor, &profile_metadata));
    Check(profile_metadata.text.at("timezone_profile_id") == "timestamp_timezone_profile");
    bool nullable = false;
    a::EngineUuid receipt;
    Check(a::ResolveHistoricalTimestampScalarIdentityV1(profiled, &nullable, &detail, &receipt) == &identity);
    Check(nullable && receipt == source.statement_receipt_uuid);
    a::EngineTypedValue profiled_value;
    Check(a::EncodeHistoricalTimestampNanosecondsV1(profiled, -1, &profiled_value, &detail));
    std::int64_t profiled_nanos = 0;
    Check(a::DecodeHistoricalTimestampNanosecondsV1(profiled_value, &profiled_nanos, &detail) && profiled_nanos == -1);
    a::CatalogColumnMetadata unprofiled_metadata;
    Check(a::DecodeCatalogColumnMetadata(descriptor.encoded_descriptor, &unprofiled_metadata));
    Check(!unprofiled_metadata.text.contains("timezone_profile_id"));
    for (const auto* invalid_profile : {"", "UTC", "America/Toronto", "timestamp_tz"}) {
      auto bad_source = source;
      bad_source.timezone_profile_id = invalid_profile;
      auto unchanged = descriptor;
      Check(!a::BuildHistoricalTimestampScalarDescriptorV1(bad_source, &unchanged, &detail));
      Check(unchanged == descriptor);
      auto bad_metadata = profile_metadata;
      bad_metadata.text["timezone_profile_id"] = invalid_profile;
      auto bad_descriptor = profiled;
      Check(a::EncodeCatalogColumnMetadata(bad_metadata, &bad_descriptor.encoded_descriptor));
      Check(!a::ResolveHistoricalTimestampScalarIdentityV1(bad_descriptor, &nullable, &detail));
    }
    auto extra_metadata = profile_metadata;
    extra_metadata.text["unexpected"] = "not authority";
    auto extra_descriptor = profiled;
    Check(a::EncodeCatalogColumnMetadata(extra_metadata, &extra_descriptor.encoded_descriptor));
    Check(!a::ResolveHistoricalTimestampScalarIdentityV1(extra_descriptor, &nullable, &detail));
    for (unsigned size = 0; size <= 32; ++size) {
      if (size == 16) continue;
      std::vector<std::uint8_t> wrong(size);
      std::int64_t unchanged = 42;
      Check(!a::DecodeHistoricalTimestampStoredNanosecondsV1(identity, wrong, &unchanged, &detail) && unchanged == 42);
    }
    for (unsigned bit = 0; bit < 32; ++bit) {
      std::array<std::uint8_t, 16> wrong{};
      wrong[12 + bit / 8] = 1u << (bit % 8);
      std::int64_t unchanged = 42;
      Check(!a::DecodeHistoricalTimestampStoredNanosecondsV1(identity, wrong, &unchanged, &detail) && unchanged == 42);
    }
    const auto reject_storage = [&](const auto& context, const auto& candidate) {
      auto unchanged = descriptor;
      Check(!a::ProjectStoredHistoricalTimestampDescriptorV1(context, candidate, true, &unchanged, &detail));
      Check(unchanged == descriptor);
    };
    for (const auto& [name, value] : metadata.text) {
      auto bad = metadata;
      bad.text[name] = "invalid";
      auto candidate = stored;
      Check(a::EncodeCatalogColumnMetadata(bad, &candidate.encoded_descriptor));
      reject_storage(storage_context, candidate);
    }
    for (const auto& [name, value] : metadata.identities) {
      auto bad = metadata;
      bad.identities.erase(name);
      auto candidate = stored;
      Check(a::EncodeCatalogColumnMetadata(bad, &candidate.encoded_descriptor));
      reject_storage(storage_context, candidate);
    }
    for (unsigned mutation = 0; mutation < 4; ++mutation) {
      auto bad = storage_context;
      if (mutation == 0) bad.datatype_catalog_snapshot_uuid = {};
      if (mutation == 1) ++bad.datatype_catalog_generation;
      if (mutation == 2) ++bad.datatype_registry_generation;
      if (mutation == 3) bad.statement_receipt_uuid = {};
      reject_storage(bad, stored);
    }
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
    for (const std::int64_t nanos : {INT64_MIN, INT64_C(-1000000001), INT64_C(-1),
                                    INT64_C(0), INT64_C(999999999), INT64_MAX}) {
      a::EngineTypedValue encoded;
      Check(a::EncodeHistoricalTimestampNanosecondsV1(descriptor, nanos, &encoded, &detail));
      std::int64_t decoded = 42;
      Check(a::DecodeHistoricalTimestampNanosecondsV1(encoded, &decoded, &detail) && decoded == nanos);
      decoded = 42;
      Check(a::DecodeHistoricalTimestampStoredNanosecondsV1(identity, encoded.binary_value,
                                                           &decoded, &detail) && decoded == nanos);
      Check(encoded.binary_value.size() == 16 && encoded.encoded_value.empty());
      if (nanos == INT64_MIN || nanos == INT64_MAX) {
        auto overflow = encoded;
        // Change the LE32 fraction by one with explicit carry/borrow.
        for (unsigned byte = 8; byte < 12; ++byte) {
          const auto before = overflow.binary_value[byte];
          if (nanos == INT64_MIN) {
            --overflow.binary_value[byte];
            if (before != 0) break;
          } else {
            ++overflow.binary_value[byte];
            if (before != 255) break;
          }
        }
        Check(a::DecodeHistoricalTimestampScalarV1(overflow, &parts, &detail));
        decoded = 42;
        Check(!a::DecodeHistoricalTimestampNanosecondsV1(overflow, &decoded, &detail) && decoded == 42);
        Check(!a::DecodeHistoricalTimestampStoredNanosecondsV1(identity, overflow.binary_value,
                                                              &decoded, &detail) && decoded == 42);
      }
    }
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
    namespace ex = scratchbird::engine::executor;
    ex::CanonicalDescriptorOrderTerm order;
    order.expression_descriptor_id = 1;
    order.direction = ex::CanonicalDescriptorOrderDirection::ascending;
    order.null_placement = ex::CanonicalDescriptorNullPlacement::last;
    const auto ordered = ex::CompareCanonicalDescriptorOrderValues(previous, value, order);
    if (!ordered.diagnostic.ok) std::cerr << ordered.diagnostic.detail << '\n';
    Check(ordered.diagnostic.ok && ordered.comparison == -1);
    order.direction = ex::CanonicalDescriptorOrderDirection::descending;
    Check(ex::CompareCanonicalDescriptorOrderValues(previous, value, order).comparison == 1);
    const auto first_key = ex::MakeCanonicalDescriptorEqualityKey(previous, order);
    const auto second_key = ex::MakeCanonicalDescriptorEqualityKey(value, order);
    Check(first_key.diagnostic.ok && second_key.diagnostic.ok &&
          first_key.equality_key != second_key.equality_key);
    Check(ex::MakeCanonicalDescriptorEqualityKey(value, order).equality_key == second_key.equality_key);
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
    for (const auto direction : {ex::CanonicalDescriptorOrderDirection::ascending,
                                  ex::CanonicalDescriptorOrderDirection::descending}) {
      order.direction = direction;
      for (const auto placement : {ex::CanonicalDescriptorNullPlacement::first,
                                    ex::CanonicalDescriptorNullPlacement::last}) {
        order.null_placement = placement;
        const auto null_order = ex::CompareCanonicalDescriptorOrderValues(value, previous, order);
        Check(null_order.diagnostic.ok && null_order.comparison ==
            (placement == ex::CanonicalDescriptorNullPlacement::first ? -1 : 1));
        Check(ex::MakeCanonicalDescriptorEqualityKey(value, order).diagnostic.ok);
      }
    }
    auto invalid_order = order;
    invalid_order.timezone_epoch = 1;
    Check(!ex::CompareCanonicalDescriptorOrderValues(value, previous, invalid_order).diagnostic.ok);
    auto stale_null = value;
    ++stale_null.descriptor.datatype_descriptor_generation;
    Check(!ex::CompareCanonicalDescriptorOrderValues(stale_null, previous, order).diagnostic.ok);
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
    if (type == d::CanonicalTypeId::int64 || type == d::CanonicalTypeId::boolean) {
      namespace exec = scratchbird::engine::executor;
      const bool boolean = type == d::CanonicalTypeId::boolean;
      const auto result_name = boolean ? "boolean" : "real64";
      const auto result_id = boolean ? d::CanonicalTypeId::boolean : d::CanonicalTypeId::real64;
      const auto functions = boolean
          ? std::vector{exec::CanonicalAggregateFunction::bool_and,
                        exec::CanonicalAggregateFunction::bool_or,
                        exec::CanonicalAggregateFunction::every}
          : std::vector{exec::CanonicalAggregateFunction::avg,
                                 exec::CanonicalAggregateFunction::stddev_pop,
                                 exec::CanonicalAggregateFunction::variance_pop,
                                 exec::CanonicalAggregateFunction::stddev_samp,
                                 exec::CanonicalAggregateFunction::variance_samp,
                                 exec::CanonicalAggregateFunction::approx_median,
                                 exec::CanonicalAggregateFunction::percentile_cont,
                                 exec::CanonicalAggregateFunction::percentile_disc,
                                 exec::CanonicalAggregateFunction::approx_percentile_cont,
                                 exec::CanonicalAggregateFunction::approx_percentile_disc};
      for (const auto function : functions) {
        auto aggregate_dag = dag;
        const auto* registration = exec::LookupCanonicalAggregateByFunctionV1(function);
        Check(registration != nullptr);
        const auto real_identity = std::ranges::find_if(rows, [&](const auto& item) {
          return item.type_uuid == s::ExactCanonicalCoreDatatypeTypeUuidV1(result_name);
        });
        Check(real_identity != rows.end());
        auto result_type = descriptor;
        result_type.descriptor_id = 2;
        result_type.descriptor_uuid = scratchbird::tests::FixtureUuid(1086, 401);
        result_type.descriptor_generation = real_identity->descriptor_generation;
        result_type.type_uuid = real_identity->type_uuid;
        result_type.type_generation = real_identity->type_generation;
        result_type.codec_id = real_identity->codec_id;
        result_type.codec_version = real_identity->codec_version;
        result_type.codec_generation = real_identity->codec_generation;
        result_type.datatype_catalog_snapshot_uuid = real_identity->catalog_snapshot_uuid;
        result_type.datatype_catalog_generation = real_identity->catalog_generation;
        result_type.datatype_registry_generation = real_identity->registry_generation;
        result_type.nullability = a::RelationalNullability::kNullable;
        aggregate_dag.descriptors.push_back(result_type);
        a::RelationalExpressionRecord aggregate_expression;
        aggregate_expression.expression_id = 2;
        aggregate_expression.expression_kind = a::RelationalExpressionKind::kFunctionCall;
        aggregate_expression.result_descriptor_id = 2;
        aggregate_expression.function_uuid = registration->function_uuid;
        aggregate_expression.child_expression_ids = {1};
        const bool percentile = function == exec::CanonicalAggregateFunction::percentile_cont ||
            function == exec::CanonicalAggregateFunction::percentile_disc ||
            function == exec::CanonicalAggregateFunction::approx_percentile_cont ||
            function == exec::CanonicalAggregateFunction::approx_percentile_disc;
        if (percentile) {
          const auto decimal_identity = std::ranges::find_if(rows, [](const auto& item) {
            return item.canonical_binary_type_code == static_cast<std::uint32_t>(d::CanonicalTypeId::decimal);
          });
          Check(decimal_identity != rows.end());
          auto fraction_descriptor = descriptor;
          fraction_descriptor.descriptor_id = 3;
          fraction_descriptor.descriptor_uuid = scratchbird::tests::FixtureUuid(1086, 402);
          fraction_descriptor.descriptor_generation = decimal_identity->descriptor_generation;
          fraction_descriptor.type_uuid = decimal_identity->type_uuid;
          fraction_descriptor.type_generation = decimal_identity->type_generation;
          fraction_descriptor.codec_id = decimal_identity->codec_id;
          fraction_descriptor.codec_version = decimal_identity->codec_version;
          fraction_descriptor.codec_generation = decimal_identity->codec_generation;
          fraction_descriptor.datatype_catalog_snapshot_uuid = decimal_identity->catalog_snapshot_uuid;
          fraction_descriptor.datatype_catalog_generation = decimal_identity->catalog_generation;
          fraction_descriptor.datatype_registry_generation = decimal_identity->registry_generation;
          const auto encoded = s::EncodeSblrLiteralExactDecimalV1("0.25");
          Check(encoded.ok);
          fraction_descriptor.precision = encoded.precision;
          fraction_descriptor.scale = encoded.scale;
          aggregate_dag.descriptors.push_back(fraction_descriptor);
          a::RelationalExpressionRecord fraction;
          fraction.expression_id = 3;
          fraction.result_descriptor_id = 3;
          fraction.expression_kind = a::RelationalExpressionKind::kLiteral;
          fraction.literal_kind = a::RelationalLiteralKind::kNumeric;
          a::RelationalExpressionRecord::LiteralTypedValueV1 carrier;
          carrier.descriptor_uuid = fraction_descriptor.descriptor_uuid;
          carrier.descriptor_generation = fraction_descriptor.descriptor_generation;
          carrier.value_state = "value";
          carrier.canonical_value_bytes.assign(encoded.canonical_bytes.begin(), encoded.canonical_bytes.end());
          carrier.canonical_value_sha256 = scratchbird::core::hash::ComputeSha256Digest(carrier.canonical_value_bytes).digest;
          fraction.literal_typed_value_v1 = carrier;
          aggregate_dag.expressions.push_back(fraction);
          aggregate_expression.child_expression_ids = {3, 1};
        }
        aggregate_dag.expressions.push_back(aggregate_expression);
        aggregate_dag.outputs = {{1, 2, 2, "aggregate_value", 2, true, 0}};
        auto aggregate_root = root;
        aggregate_root.output_descriptor_ids = {2};
        aggregate_root.bound_expression_ids = {2};
        const auto prepared = s::PrepareGlobalAggregateRootForComposition(
            aggregate_dag, aggregate_root, source, input, function, false, false, false);
        if (!prepared.ok) std::cerr << prepared.detail << '\n';
        Check(prepared.ok);
        a::EngineDescriptor expected;
        Check(s::BuildExactCanonicalScalarRuntimeDescriptorV1(result_type, result_id, &expected));
        Check(prepared.result_column.descriptor == expected && prepared.result_column.nullable);
        if (percentile) {
          Check(prepared.direct_arguments.size() == 1 &&
                prepared.direct_arguments.front().descriptor.canonical_type_name == "decimal" &&
                prepared.direct_arguments.front().descriptor.descriptor_uuid ==
                    scratchbird::tests::FixtureUuid(1086, 402) &&
                prepared.direct_arguments.front().encoded_value.empty() &&
                prepared.direct_arguments.front().binary_value.size() == 24);
        }
        auto stale_dag = aggregate_dag;
        ++stale_dag.descriptors[1].codec_generation;
        const auto refused = s::PrepareGlobalAggregateRootForComposition(
            stale_dag, aggregate_root, source, input, function, false, false, false);
        Check(!refused.ok && refused.result_bindings.empty());
      }
    }
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
