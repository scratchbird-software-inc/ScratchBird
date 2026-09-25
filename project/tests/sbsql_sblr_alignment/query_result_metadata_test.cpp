// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../support/binary_uuid_fixture.hpp"
#include "engine/sblr/canonical_query_result_metadata.hpp"
#include "engine/executor/descriptor_value_runtime.hpp"
#include "query/expression_api.hpp"

#include <array>
#include <iostream>
#include <stdexcept>

namespace api = scratchbird::engine::internal_api;
namespace engine = scratchbird::engine::sblr;
namespace wire = scratchbird::wire;
namespace dt = scratchbird::core::datatypes;
namespace {
std::size_t checks = 0;
void Check(bool value, const char* label) {
  ++checks;
  if (!value) throw std::runtime_error(label);
}
struct Identity {
  api::EngineUuid descriptor;
  api::EngineUuid type;
  const char* codec;
  dt::CanonicalTypeId code;
  std::uint32_t width;
};
// Independently fixed admitted descriptor/type/codec expectations. Never call
// the production lookup to manufacture the expected values.
constexpr std::array<Identity, 6> identities{{
  {scratchbird::tests::FixtureUuidLiteral("01000000-626f-7f6c-a561-6e0000000000"), scratchbird::tests::FixtureUuidLiteral("01000000-626f-7f6c-a561-6e0000000000"), "datatype.boolean.u8.v1", dt::CanonicalTypeId::boolean, 1},
  {scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d716"), scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d717"), "datatype.int32.le.v1", dt::CanonicalTypeId::int32, 4},
  {scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d711"), scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d712"), "datatype.int64.le.v1", dt::CanonicalTypeId::int64, 8},
  {scratchbird::tests::FixtureUuidLiteral("a0000000-6465-7369-ad61-6c0000000000"), scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d713"), "datatype.decimal.base1e9.le.v1", dt::CanonicalTypeId::decimal, 24},
  {scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d714"), scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d715"), "datatype.int128.le.v1", dt::CanonicalTypeId::int128, 16},
  {scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d718"), scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d719"), "datatype.text.utf8.v1", dt::CanonicalTypeId::character, 0}
}};

wire::TypedResultUuid Bytes(const api::EngineUuid& identity) {
  return identity.bytes;
}

struct Fixture {
  api::EngineRequestContext context;
  api::TypedRelationalDag dag;
  api::EngineResultShape shape;
  std::string code, detail;
  Fixture(const Identity& id = identities[2], std::size_t columns = 1,
          std::size_t rows = 0, api::RelationalNullability nullable = api::RelationalNullability::kNonNull) {
    context.statement_receipt_uuid = scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-000000001001");
    context.statement_snapshot_uuid = scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-000000001002");
    context.datatype_catalog_snapshot_uuid = scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d701");
    context.datatype_catalog_generation = 1;
    context.datatype_registry_generation = 1;
    context.maximum_typed_result_transport_bytes_per_packet = 65536;
    dag.root_node_id = 9;
    api::RelationalDagNode root;
    root.node_id = 9;
    root.node_kind = api::RelationalDagNodeKind::kProject;
    api::RelationalTypeDescriptor descriptor;
    descriptor.descriptor_id = 1;
    descriptor.descriptor_uuid = id.descriptor;
    descriptor.type_uuid = id.type;
    descriptor.nullability = nullable;
    descriptor.datatype_identity_authoritative = true;
    descriptor.descriptor_generation = descriptor.type_generation = descriptor.codec_generation = 1;
    descriptor.codec_id = id.codec;
    descriptor.codec_version = 1;
    descriptor.statement_receipt_uuid = context.statement_receipt_uuid;
    descriptor.datatype_catalog_snapshot_uuid = context.datatype_catalog_snapshot_uuid;
    descriptor.datatype_catalog_generation = descriptor.datatype_registry_generation = 1;
    dag.descriptors.push_back(descriptor);
    shape.result_kind = "rows";
    for (std::size_t i = 0; i < columns; ++i) {
      root.output_descriptor_ids.push_back(1);
      api::RelationalOutputRecord output;
      output.output_id = static_cast<std::uint32_t>(i + 1);
      output.relation_node_id = 9;
      output.descriptor_id = 1;
      output.visible = true;
      output.ordinal = static_cast<std::uint32_t>(i);
      output.output_name_utf8 = "same;=résumé";
      dag.outputs.push_back(output);
      api::EngineDescriptor executed;
      executed.descriptor_uuid = id.descriptor;
      // Neither this display spelling nor the rendered row value defines type.
      executed.canonical_type_name = "not_a_type_authority";
      executed.type_uuid = id.type;
      executed.encoded_descriptor = "display text is not identity authority";
      shape.columns.push_back(executed);
    }
    dag.nodes.push_back(root);
    for (std::size_t n = 0; n < rows; ++n) {
      api::EngineRowValue row;
      for (const auto& column : shape.columns) {
        api::EngineTypedValue value;
        value.descriptor = column;
        value.encoded_value = "FALSE-looking_text_not_type_authority";
        row.fields.emplace_back("same;=résumé", value);
      }
      shape.rows.push_back(row);
    }
  }
  bool Run() {
    return engine::PreserveCanonicalQueryResultMetadataV1(context, dag, &shape, &code, &detail);
  }
};

template <typename Mutation>
void Reject(Mutation mutation, const char* expected = "DATATYPE.DESCRIPTOR.INVALID") {
  Fixture f;
  Check(f.Run(), "negative baseline valid");
  mutation(f);
  Check(!f.Run(), "mutation must fail");
  Check(!f.shape.query_metadata, "failure clears stale metadata atomically");
  Check(f.code == expected && !f.detail.empty(), "exact negative diagnostic");
}
}

int main() try {
  constexpr std::size_t expected_cases = 6 * 3 * 4 * 3;
  std::cout << "expected_schema_tuples=" << expected_cases << '\n';
  std::size_t cases = 0;
  for (const auto& id : identities)
  for (const auto nullable : {api::RelationalNullability::kNonNull, api::RelationalNullability::kNullable, api::RelationalNullability::kUnknown})
  for (std::size_t columns : {1U, 2U, 3U, 4U})
  for (std::size_t rows : {0U, 1U, 2U}) {
    Fixture f(id, columns, rows, nullable);
    Check(f.Run(), f.detail.c_str());
    Check(f.code.empty() && f.detail.empty(), "success has no failure diagnostic");
    const auto frozen = f.shape.query_metadata;
    Check(frozen && frozen->columns.size() == columns, "complete schema including zero rows");
    Check(frozen->statement_receipt_uuid == Bytes(f.context.statement_receipt_uuid), "binary receipt");
    Check(frozen->statement_snapshot_uuid == Bytes(f.context.statement_snapshot_uuid), "binary snapshot");
    Check(frozen->datatype_catalog_snapshot_uuid == Bytes(f.context.datatype_catalog_snapshot_uuid), "binary catalog snapshot");
    Check(frozen->datatype_catalog_generation == 1 && frozen->datatype_registry_generation == 1, "exact cohort generations");
    for (std::size_t i = 0; i < columns; ++i) {
      const auto& c = frozen->columns[i];
      Check(c.transport.ordinal == i && c.transport.name_occurrence == i, "ordered duplicate names");
      Check(c.transport.name == "same;=résumé", "verbatim punctuation and unicode name");
      Check(c.bound_descriptor_uuid == Bytes(id.descriptor), "bound descriptor identity");
      Check(c.transport.descriptor_uuid == Bytes(id.descriptor) && c.transport.type_uuid == Bytes(id.type), "exact binary registry identities");
      Check(c.transport.descriptor_generation == 1 && c.transport.type_generation == 1, "type generations");
      Check(c.transport.codec_id == id.codec && c.transport.codec_version == 1 && c.transport.codec_generation == 1, "codec tuple");
      Check(c.transport.canonical_type_id == id.code && c.transport.canonical_value_bytes == id.width, "registry type and width independent of rendered values");
      Check(static_cast<unsigned>(c.transport.nullability) == static_cast<unsigned>(nullable) - 1, "exact nullability");
    }
    f.dag.outputs[0].output_name_utf8 = "changed";
    f.dag.descriptors.clear();
    f.shape.columns.clear();
    Check(frozen->columns[0].transport.name == "same;=résumé", "owned lifetime independent of DAG and rows");
    ++cases;
  }
  Check(cases == expected_cases, "fixed expected case population");
  Reject([](auto& f) { f.context.statement_receipt_uuid = {}; });
  Reject([](auto& f) { f.context.statement_receipt_uuid.bytes[6] = 0x40; });
  Reject([](auto& f) { f.context.statement_snapshot_uuid = {}; });
  Reject([](auto& f) { f.context.datatype_catalog_generation = 2; });
  Reject([](auto& f) { f.context.datatype_registry_generation = 2; });
  Reject([](auto& f) { f.dag.root_node_id = 0; });
  Reject([](auto& f) { f.dag.nodes.push_back(f.dag.nodes[0]); });
  Reject([](auto& f) { f.dag.outputs.clear(); });
  Reject([](auto& f) { f.dag.outputs[0].ordinal = 1; });
  Reject([](auto& f) { f.dag.outputs[0].descriptor_id = 2; });
  Reject([](auto& f) { f.dag.outputs[0].visible = false; });
  Reject([](auto& f) { f.dag.descriptors.push_back(f.dag.descriptors[0]); });
  Reject([](auto& f) { f.dag.descriptors[0].datatype_identity_authoritative = false; });
  Reject([](auto& f) { f.dag.descriptors[0].statement_receipt_uuid.bytes.back() ^= 1; });
  Reject([](auto& f) { f.dag.descriptors[0].datatype_catalog_snapshot_uuid.bytes.back() ^= 1; });
  Reject([](auto& f) { f.dag.descriptors[0].datatype_catalog_generation = 0; });
  Reject([](auto& f) { f.dag.descriptors[0].datatype_registry_generation = 0; });
  Reject([](auto& f) { f.dag.descriptors[0].descriptor_generation = 2; });
  Reject([](auto& f) { f.dag.descriptors[0].type_generation = 2; });
  Reject([](auto& f) { f.dag.descriptors[0].codec_generation = 2; });
  Reject([](auto& f) { f.dag.descriptors[0].codec_version = 2; });
  Reject([](auto& f) { f.dag.descriptors[0].codec_id += ".invented"; });
  Reject([](auto& f) { f.dag.descriptors[0].type_uuid = identities[0].type; });
  Reject([](auto& f) { f.dag.descriptors[0].nullability = static_cast<api::RelationalNullability>(255); });
  Reject([](auto& f) { f.dag.descriptors[0].collation_uuid = api::EngineUuid{}; });
  Reject([](auto& f) { f.dag.outputs[0].output_name_utf8 = std::string("a\0b", 3); });
  Reject([](auto& f) { f.dag.outputs[0].output_name_utf8.assign(4097, 'a'); });
  Reject([](auto& f) { f.shape.columns[0].descriptor_uuid.bytes.back() ^= 1; });
  Reject([](auto& f) { f.shape.columns[0].type_uuid = identities[0].type; });
  Reject([](auto& f) { f.shape.columns[0].type_uuid = {}; });
  Reject([](auto& f) { f.shape.result_kind = "command"; });
  Reject([](auto& f) { f.shape.columns.clear(); });
  Reject([](auto& f) { f.context.maximum_typed_result_transport_bytes_per_packet = 1; }, "RESOURCE.BUDGET_EXCEEDED");
  Reject([](auto& f) { f.context.maximum_typed_result_transport_bytes_per_packet = 0; }, "RESOURCE.BUDGET_EXCEEDED");
  Reject([](auto& f) { f.context.maximum_typed_result_transport_bytes_per_packet = api::kMaximumTypedResultTransportBytesPerPacket + 1; }, "RESOURCE.BUDGET_EXCEEDED");
  // Four columns, two rows: entry + each column + each row + final barrier.
  for (std::size_t phase = 1; phase <= 8; ++phase) {
    Fixture f(identities[2], 4, 2);
    std::size_t calls = 0;
    f.context.query_cancellation_requested = [&] { return ++calls == phase; };
    Check(!f.Run() && !f.shape.query_metadata && f.code == "PROCESS.CANCELLED" && calls == phase,
          "cancellation at each observed publication phase");
  }
  Reject([](auto& f) { f.context.query_cancellation_requested = []() -> bool { throw std::runtime_error("injected"); }; }, "SBLR.EXECUTION_FAILED");
  for (int mutation = 0; mutation < 3; ++mutation) {
    Fixture f(identities[2], 2, 1);
    if (mutation == 0) f.shape.rows[0].fields.pop_back();
    if (mutation == 1) f.shape.rows[0].fields[0].first = "wrong";
    if (mutation == 2) f.shape.rows[0].fields[0].second.descriptor.encoded_descriptor += ";corrupt=1";
    Check(!f.Run() && !f.shape.query_metadata, "actual row mismatch rejected atomically");
  }
  for (const auto nullable : {api::RelationalNullability::kNonNull, api::RelationalNullability::kNullable, api::RelationalNullability::kUnknown}) {
    Fixture f(identities[2], 1, 1, nullable);
    auto& value = f.shape.rows[0].fields[0].second;
    value.state = api::EngineValueState::sql_null;
    value.is_null = true;
    value.encoded_value.clear();
    Check(f.Run() == (nullable != api::RelationalNullability::kNonNull), "SQL NULL obeys column nullability");
    value.binary_value.push_back(0);
    Check(!f.Run() && !f.shape.query_metadata, "SQL NULL cannot carry bytes");
  }
  {
    Fixture f(identities[2], 1, 1);
    auto& value = f.shape.rows[0].fields[0].second;
    value.state = api::EngineValueState::sql_null;
    value.is_null = true;
    value.encoded_value.clear();
    f.shape.null_extended_columns = {true};
    Check(f.Run() && f.shape.query_metadata->columns[0].transport.nullability ==
        wire::TypedResultNullability::nullable, "engine outer-join widening survives publication");
    f.shape.null_extended_columns.push_back(true);
    Check(!f.Run(), "outer-join widening requires exact schema cardinality");
  }
  {
    namespace exec = scratchbird::engine::executor;
    Fixture f(identities[5], 1, 1);
    auto value = f.shape.rows[0].fields[0].second;
    value.descriptor.canonical_type_name = "text";
    value.descriptor.encoded_descriptor.clear();
    exec::CanonicalDescriptorOrderTerm term;
    term.expression_descriptor_id = 1;
    term.collation_uuid = scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-000000009901");
    value.descriptor.collation_uuid = term.collation_uuid;
    term.resource_epoch = term.collation_epoch = 1;
    auto& seed = term.text_seed;
    seed.active = true;
    seed.database_uuid = scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-000000009902");
    seed.charset_uuid = scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-000000009903");
    seed.collation_uuid = term.collation_uuid;
    seed.resource_epoch = seed.collation_epoch = 1;
    seed.comparison_profile = scratchbird::core::resources::CollationProfile::utf8_binary;
    seed.seed_pack_name = "seed"; seed.seed_pack_version = "1";
    seed.charset_name = "UTF8"; seed.collation_name = "short";
    // Component recipe only: no claim of a live resource catalog receipt.
    for (const auto& text : {std::string{}, std::string("a\0;",3)}) {
      value.encoded_value = text;
      value.binary_value.clear();
      const auto plan = exec::PlanCanonicalDescriptorEqualityKey(value, term);
      const auto key = exec::MakeCanonicalDescriptorEqualityKey(value, term);
      Check(plan.diagnostic.ok && key.diagnostic.ok &&
            key.equality_key.size() <= plan.retained_key_bytes,
            "binary UUID collation cohort exceeded equality-key allocation for short text");
    }
  }
  {
    api::EngineTypedValue left, right;
    left.descriptor.descriptor_kind = "scalar";
    left.descriptor.canonical_type_name = "uuid";
    left.descriptor.encoded_descriptor = "nullability=non_null";
    left.descriptor.descriptor_uuid = scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d734");
    left.descriptor.type_uuid = scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d735");
    left.state = api::EngineValueState::value;
    right = left;
    const std::string bytes("\x01\x02\x03\x04\x05\x06\x70\x00\x80\x00\x09\x0a\x3b\x7c\x00\xff",16);
    left.binary_value.assign(bytes.begin(),bytes.end());
    right.encoded_value = bytes;
    int comparison = 99;
    std::string detail;
    Check(api::QowCompareCanonicalNonCollatedScalarsV1(left,right,&comparison,&detail) && comparison == 0,
          "binary-vector and retained binary-string UUID carriers did not compare equally");
    right.encoded_value.back() = '\x7f';
    Check(api::QowCompareCanonicalNonCollatedScalarsV1(left,right,&comparison,&detail) && comparison > 0,
          "UUID octet ordering drifted");
    left.binary_value.pop_back();
    Check(!api::QowCompareCanonicalNonCollatedScalarsV1(left,right,&comparison,&detail),
          "malformed UUID comparison became default-success empty value");
    left.binary_value.assign(bytes.begin(),bytes.end());
    left.encoded_value = bytes;
    Check(!api::QowCompareCanonicalNonCollatedScalarsV1(left,right,&comparison,&detail),
          "ambiguous UUID dual carrier was admitted");
  }
  {
    api::EngineTypedValue left, right;
    left.descriptor.descriptor_kind = "scalar";
    left.descriptor.canonical_type_name = "text";
    left.descriptor.encoded_descriptor = "nullability=non_null";
    left.descriptor.descriptor_uuid = identities[5].descriptor;
    left.descriptor.type_uuid = identities[5].type;
    left.descriptor.collation_uuid = scratchbird::tests::FixtureUuid(1900,1);
    left.encoded_value = "a"; right = left;
    api::EngineSqlTruthValue truth;
    std::string detail;
    Check(api::QowEvaluateCanonicalComparisonTruthV1(left,right,0,api::EngineComparisonPredicateOperator::equal,&truth,&detail) && truth == api::EngineSqlTruthValue::true_value,
          "TEXT truth seam lost binary collation identity");
    right.descriptor.collation_uuid = scratchbird::tests::FixtureUuid(1900,2);
    Check(!api::QowEvaluateCanonicalComparisonTruthV1(left,right,0,api::EngineComparisonPredicateOperator::equal,&truth,&detail), "mismatched binary collation admitted");
    left.descriptor.collation_uuid = {}; right.descriptor.collation_uuid = {};
    Check(!api::QowEvaluateCanonicalComparisonTruthV1(left,right,0,api::EngineComparisonPredicateOperator::equal,&truth,&detail), "absent collation admitted");
    left.descriptor.collation_uuid = scratchbird::tests::FixtureUuid(1900,1); right=left;
    api::EngineCanonicalExpressionEvaluationRequest request;
    request.consumer = api::EngineCanonicalExpressionConsumer::projection;
    request.operation = api::EngineCanonicalExpressionOperation::text_concat;
    request.left_value = left; request.right_value = right;
    request.result_descriptor = left.descriptor;
    api::EngineCanonicalExpressionEvaluationResult result;
    Check(api::QowEvaluateCanonicalTypedExpressionV1(request,&result,&detail) && result.value.encoded_value == "aa", "TEXT concatenation lost binary collation binding");
    request.result_descriptor.collation_uuid = scratchbird::tests::FixtureUuid(1900,2);
    Check(!api::QowEvaluateCanonicalTypedExpressionV1(request,&result,&detail), "concatenation accepted mismatched binary collation");
  }
  for (unsigned state = 2; state <= 7; ++state) {
    Fixture f(identities[2], 1, 1);
    f.shape.rows[0].fields[0].second.state = static_cast<api::EngineValueState>(state);
    Check(!f.Run() && !f.shape.query_metadata, "non-result states cannot be published as values");
  }
  Fixture hidden(identities[2], 2, 0);
  hidden.dag.outputs[0].visible = false;
  hidden.shape.columns.erase(hidden.shape.columns.begin());
  Check(hidden.Run() && hidden.shape.query_metadata->columns.size() == 1 &&
        hidden.shape.query_metadata->columns[0].transport.ordinal == 0 &&
        hidden.shape.query_metadata->columns[0].transport.name_occurrence == 0,
        "hidden outputs do not consume published ordinals or occurrences");
  Fixture limits;
  const auto exact_bytes = 128 + 92 + limits.dag.outputs[0].output_name_utf8.size() + std::string(identities[2].codec).size();
  limits.context.maximum_typed_result_transport_bytes_per_packet = exact_bytes;
  Check(limits.Run(), "exact packet budget boundary admitted");
  --limits.context.maximum_typed_result_transport_bytes_per_packet;
  Check(!limits.Run() && limits.code == "RESOURCE.BUDGET_EXCEEDED", "one byte below required budget rejected");
  for (std::size_t width : {16384U, 16385U}) {
    Fixture f(identities[2], width, 0);
    f.context.maximum_typed_result_transport_bytes_per_packet = api::kMaximumTypedResultTransportBytesPerPacket;
    Check(f.Run() == (width == 16384), "exact column-count boundary");
    if (width == 16384)
      Check(f.shape.query_metadata->columns.back().transport.name_occurrence == 16383,
            "maximum duplicate occurrence retained");
  }
  for (std::size_t length : {0U, 4096U}) {
    Fixture f;
    f.dag.outputs[0].output_name_utf8.assign(length, 'x');
    Check(f.Run() && f.shape.query_metadata->columns[0].transport.name.size() == length,
          "empty and maximum-length display names remain exact");
  }
  Fixture bound;
  const auto logical_descriptor = scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-000000009001");
  bound.dag.descriptors[0].descriptor_uuid = logical_descriptor;
  bound.shape.columns[0].descriptor_uuid = logical_descriptor;
  Check(bound.Run() &&
        bound.shape.query_metadata->columns[0].bound_descriptor_uuid == Bytes(logical_descriptor) &&
        bound.shape.query_metadata->columns[0].transport.descriptor_uuid == Bytes(identities[2].descriptor),
        "bound expression identity never substitutes for canonical registry descriptor");
  for (unsigned octet = 0; octet <= 255; ++octet) {
    Fixture f;
    f.dag.outputs[0].output_name_utf8.assign(1, static_cast<char>(octet));
    Check(f.Run() == (octet > 0 && octet < 128), "all single-byte name encodings");
  }
  for (const std::string name : {"\xc2\xa2", "\xe2\x82\xac", "\xf0\x9f\x8c\x8d", "\xf4\x8f\xbf\xbf"}) {
    Fixture f;
    f.dag.outputs[0].output_name_utf8 = name;
    Check(f.Run(), "valid multibyte scalar name");
  }
  for (const std::string name : {"\xc0\x80", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe2\x82"}) {
    Fixture f;
    f.dag.outputs[0].output_name_utf8 = name;
    Check(!f.Run() && !f.shape.query_metadata, "noncanonical multibyte name");
  }
  for (const bool mixed_limit_chain : {false, true}) {
  for (std::uint32_t depth = 1; depth <= 16; ++depth) {
    Fixture f(identities[2], 2, depth % 3);
    for (std::uint32_t n = 0; n < depth; ++n) {
      api::RelationalDagNode cte;
      cte.node_id = 10 + n;
      cte.node_kind = mixed_limit_chain && n % 2 == 0
          ? api::RelationalDagNodeKind::kLimit : api::RelationalDagNodeKind::kCte;
      cte.input_node_ids = {f.dag.root_node_id};
      cte.output_descriptor_ids = {1, 1};
      f.dag.nodes.push_back(cte);
      f.dag.root_node_id = cte.node_id;
    }
    // An unrelated first node must never be mistaken for the bound producer.
    auto decoy = f.dag.nodes.front();
    decoy.node_id = 2;
    f.dag.nodes.insert(f.dag.nodes.begin(), decoy);
    auto decoy_output = f.dag.outputs.front();
    decoy_output.relation_node_id = 2;
    decoy_output.output_name_utf8 = "wrong-first-leaf";
    f.dag.outputs.insert(f.dag.outputs.begin(), decoy_output);
    Check(f.Run() && f.shape.query_metadata->columns.size() == 2 &&
          f.shape.query_metadata->columns[1].transport.name_occurrence == 1 &&
          f.shape.query_metadata->columns[0].transport.name == "same;=résumé",
          "nested CTE forwards exact child schema including empty rows");
    f.dag.nodes.back().output_descriptor_ids.pop_back();
    Check(!f.Run() && !f.shape.query_metadata, "CTE descriptor mismatch is not a fallback");
  }
  for (int failure = 0; failure < 4; ++failure) {
    Fixture f;
    api::RelationalDagNode cte;
    cte.node_id = 10;
    cte.node_kind = api::RelationalDagNodeKind::kCte;
    if (mixed_limit_chain) cte.node_kind = api::RelationalDagNodeKind::kLimit;
    cte.input_node_ids = {9};
    cte.output_descriptor_ids = {1};
    if (failure == 0) cte.input_node_ids.clear();
    if (failure == 1) cte.input_node_ids = {9, 9};
    if (failure == 2) cte.input_node_ids = {99};
    if (failure == 3) cte.input_node_ids = {10};
    f.dag.nodes.push_back(cte);
    f.dag.root_node_id = 10;
    Check(!f.Run() && !f.shape.query_metadata, "missing ambiguous or cyclic CTE producer refused");
  }
  }
  Check(checks == 6561 + 8 + 5, "fixed check population including binary/outer-join regressions");
  std::cout << "PASS schema_tuples=" << cases << " checks=" << checks << '\n';
  return 0;
} catch (const std::exception& e) {
  std::cerr << "FAIL checks=" << checks << " " << e.what() << '\n';
  return 1;
}
