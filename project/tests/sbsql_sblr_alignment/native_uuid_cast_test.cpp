// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "registry/function_seed_registry.hpp"
#include "dispatch/function_dispatch.hpp"
#include "common/function_result_helpers.hpp"
#include "sblr/sblr_special_forms.hpp"
#include "sblr/sblr_operator_runtime.hpp"
#include "sblr/sblr_projection_value_runtime.hpp"
#include "sblr/canonical_query_object_free_composition_support.hpp"
#include "sblr/canonical_query_aggregate_registration.hpp"
#include "internal_api/query/expression_api.hpp"
#include "../support/binary_uuid_fixture.hpp"
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace s = scratchbird::engine::sblr;
namespace f = scratchbird::engine::functions;
void Check(bool pass, const char* message) {
  if (!pass) { std::cerr << message << '\n'; std::exit(1); }
}
s::SblrResult CallValues(const f::FunctionRegistry& registry, const char* name,
                         std::vector<s::SblrValue> values) {
  const auto* entry = registry.Lookup(name);
  Check(entry != nullptr, "cast absent from actual callable registry");
  f::FunctionCallRequest request;
  request.context.function_uuid = entry->function_uuid;
  request.context.security_allowed = request.context.policy_allowed = true;
  Check(registry.BindCallContext(request.context) != nullptr, "binary callable binding failed");
  for (auto& value : values) request.arguments.push_back({"arg", std::move(value)});
  return f::DispatchFunctionCall(registry, std::move(request)).result;
}
s::SblrResult Call(const f::FunctionRegistry& registry, const char* name,
                   const s::SblrValue& value, const char* target) {
  return CallValues(registry, name, {value, f::MakeTextValue("text", target)});
}
const s::SblrValue& Scalar(const s::SblrResult& result) {
  Check(result.ok() && result.scalar_values.size() == 1 && result.rows.empty(), "cast did not produce one actual scalar");
  return result.scalar_values.front();
}
void Published(const s::SblrValue& value, const std::vector<std::uint8_t>& expected,
               const char* type) {
  Check(value.descriptor_id == type && s::ProjectionSblrValueResolved(value), "native cast representation unresolved");
  Check(value.encoded_value.empty() && value.text_value.empty(), "native cast leaked UUID/binary through text");
  const auto published = s::EngineTypedValueFromSblrValue(value);
  Check(!published.is_null && published.binary_value == expected && published.encoded_value.empty(),
        "publication lost or reformatted exact 128 data bits");
}
void Truth(const s::SblrResult& result, bool expected) {
  const auto& value = Scalar(result);
  Check(!value.is_null && value.has_int64_value && value.int64_value == (expected ? 1 : 0),
        "native predicate returned the wrong SQL truth value");
}
void NativeComparisons() {
  std::vector<s::SblrUuid> ids(130);
  ids[1].bytes.fill(0xff);
  for (unsigned n = 2; n < ids.size(); ++n) ids[n].bytes[(n - 2) / 8] = std::uint8_t(1u << ((n - 2) % 8));
  for (const auto& lhs : ids) for (const auto& rhs : ids) {
    int order = 0;
    for (unsigned n = 0; n < 16; ++n) {
      if (lhs.bytes[n] == rhs.bytes[n]) continue;
      order = lhs.bytes[n] < rhs.bytes[n] ? -1 : 1;
      break;
    }
    const auto left = s::MakeSblrUuidValue(lhs), right = s::MakeSblrUuidValue(rhs);
    for (const auto& [operation, truth] : {
        std::pair{"op_eq", order == 0}, {"op_ne", order != 0},
        {"op_lt", order < 0}, {"op_le", order <= 0},
        {"op_gt", order > 0}, {"op_ge", order >= 0}, {"op_is_distinct", order != 0}})
      Truth(s::EvaluateSblrComparison(operation, left, right, {}), truth);
    Truth(s::EvaluateSblrInListForm("in", left, {right}), order == 0);
    const auto nullif = s::EvaluateSblrNullIfForm("nullif", left, right);
    Check(Scalar(nullif).is_null == (order == 0), "NULLIF compared empty UUID text fields");
    if (order != 0) Check(Scalar(nullif).uuid_value == lhs, "NULLIF changed retained UUID data");
    Truth(s::EvaluateSblrBetweenForm("between", left, right, right), order == 0);
  }
  const auto native = s::MakeSblrUuidValue(ids[2]), other = s::MakeSblrUuidValue(ids[3]);
  const auto null = f::MakeNullValue("uuid");
  Check(Scalar(s::EvaluateSblrComparison("op_eq", native, null, {})).is_null, "UUID = NULL is not UNKNOWN");
  Truth(s::EvaluateSblrComparison("op_is_distinct", native, null, {}), true);
  Truth(s::EvaluateSblrComparison("op_is_distinct", null, null, {}), false);
  Truth(s::EvaluateSblrComparison("op_is_null", native, other, {}), false);
  Truth(s::EvaluateSblrComparison("op_is_null", null, other, {}), true);
  Check(Scalar(s::EvaluateSblrInListForm("in", native, {other, null})).is_null, "IN lost UNKNOWN on an unmatched NULL candidate");
  Truth(s::EvaluateSblrInListForm("in", native, {other, null, native}), true);
  Check(Scalar(s::EvaluateSblrBetweenForm("between", native, null, native)).is_null, "BETWEEN lost NULL semantics");
  unsigned probes = 0;
  const auto thunk = [&](s::SblrValue value) { return [&, value] {
    ++probes; auto result = s::MakeSblrSuccess("thunk"); result.scalar_values.push_back(value); return result;
  }; };
  Truth(s::EvaluateSblrInListFormLazy("in", thunk(native),
      {thunk(other), thunk(native), []() -> s::SblrResult { throw std::logic_error("IN evaluated past its matching candidate"); }}, {}), true);
  Check(probes == 3, "lazy IN skipped or repeated an evaluated expression");
  for (const auto& bad : {f::MakeTextValue("text", ""), f::MakeBinaryValue("binary", std::vector<std::uint8_t>(16))}) {
    Check(!s::EvaluateSblrComparison("op_eq", native, bad, {}).ok(), "UUID implicitly compared to text/binary");
    Check(!s::EvaluateSblrNullIfForm("nullif", native, bad).ok(), "NULLIF implicitly compared UUID to text/binary");
    Check(!s::EvaluateSblrInListForm("in", native, {bad}).ok(), "IN implicitly compared UUID to text/binary");
    Check(!s::EvaluateSblrBetweenForm("between", native, bad, native).ok(), "BETWEEN implicitly compared UUID to text/binary");
  }
  const auto low = f::MakeBinaryValue("binary", {0x7f}), high = f::MakeBinaryValue("binary", {0x80});
  Truth(s::EvaluateSblrComparison("op_lt", low, high, {}), true);
  Truth(s::EvaluateSblrInListForm("in", low, {high}), false);
  Check(!Scalar(s::EvaluateSblrNullIfForm("nullif", low, high)).is_null, "NULLIF compared empty binary text fields");
}
void NativeBuiltinDescriptors() {
  namespace exec = scratchbird::engine::executor;
  const auto binary = exec::MakeExecutorDescriptor("binary");
  Check(binary.datatype_descriptor_uuid == scratchbird::tests::FixtureUuidLiteral("2d010000-6269-7e61-b279-000000000000") &&
        binary.type_uuid == scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d743") &&
        s::ExactCanonicalCoreDatatypeTypeUuidV1("binary") == binary.type_uuid,
        "current binary datatype descriptor and type differ from independent registry identities");
  for (const auto* type : {"boolean", "int8", "int16", "int32", "int64", "uint8", "uint64",
                           "real64", "text", "uuid", "binary"}) {
    const auto descriptor = exec::MakeExecutorDescriptor(type);
    const auto expected = s::ExactCanonicalCoreDatatypeTypeUuidV1(
        std::string_view(type) == "text" ? "character" : type);
    Check(!expected.is_nil() && !descriptor.descriptor_uuid.is_nil() &&
          descriptor.type_uuid == expected &&
          descriptor.datatype_descriptor_uuid == descriptor.descriptor_uuid &&
          descriptor.datatype_descriptor_generation != 0,
          "internal builtin descriptor omitted its actual catalog identity");
  }
  Check(exec::MakeExecutorDescriptor("bigint").type_uuid == exec::MakeExecutorDescriptor("int64").type_uuid &&
        exec::MakeExecutorDescriptor("bytes").type_uuid == exec::MakeExecutorDescriptor("binary").type_uuid,
        "internal datatype aliases did not retain the actual Core type identity");
  Check(exec::MakeExecutorDescriptor("unknown_user_type").descriptor_uuid.is_nil(),
        "builtin constructor invented a user-defined datatype binding");
  exec::DescriptorRuntimeDiagnostic diagnostic;
  const auto cast = exec::CastDescriptorValue(exec::EncodeTextValue("42"),
      exec::MakeExecutorDescriptor("int64"), &diagnostic);
  Check(diagnostic.ok && cast.encoded_value == "42" && cast.binary_value.empty(),
        "bound internal scalar constructors could not execute their ordinary cast");
}

void NativeExecutorUuidValue(const scratchbird::engine::internal_api::EngineTypedValue& native) {
  namespace api = scratchbird::engine::internal_api;
  namespace exec = scratchbird::engine::executor;
  const auto descriptor = [](const char* name, unsigned ordinal) {
    api::EngineDescriptor value;
    value.descriptor_kind = "scalar";
    value.canonical_type_name = name;
    value.descriptor_uuid = scratchbird::tests::FixtureUuid(2085, ordinal);
    value.type_uuid = s::ExactCanonicalCoreDatatypeTypeUuidV1(
        std::string_view(name) == "text" ? "character" : name);
    value.encoded_descriptor = "nullability=nullable";
    Check(!value.type_uuid.is_nil(), "cast datatype absent from actual catalog");
    return value;
  };
  const auto binary = descriptor("binary", 1), uuid = descriptor("uuid", 2), text = descriptor("text", 3);
  const auto preserved = [&](const auto& result, const auto& target) {
    Check(result.descriptor == target && result.state == api::EngineValueState::value &&
          !result.is_null && result.encoded_value.empty() && result.binary_value == native.binary_value,
          "descriptor cast or bound function changed native UUID data");
  };
  exec::DescriptorRuntimeDiagnostic diagnostic;
  const auto bytes = exec::CastDescriptorValue(native, binary, &diagnostic);
  Check(diagnostic.ok, "executor UUID to binary cast refused"); preserved(bytes, binary);
  const auto restored = exec::CastDescriptorValue(bytes, uuid, &diagnostic);
  Check(diagnostic.ok, "executor binary to UUID cast refused"); preserved(restored, uuid);
  const auto rebound = exec::CastDescriptorValue(native, uuid, &diagnostic);
  Check(diagnostic.ok, "executor UUID descriptor rebind refused"); preserved(rebound, uuid);
  const auto version = exec::ExtractDescriptorField(native, "version", &diagnostic);
  Check(diagnostic.ok && version.encoded_value == std::to_string(native.binary_value[6] >> 4) &&
        version.binary_value.empty() && !version.is_null, "UUID version extraction did not read native bits");
  api::EngineTypedValue coerced;
  std::string category, detail;
  Check(api::QowApplyCanonicalDescriptorCoercionV1(native, binary, true, &coerced, &category, &detail),
        "canonical UUID to binary coercion refused"); preserved(coerced, binary);
  Check(!api::QowApplyCanonicalDescriptorCoercionV1(native, binary, false, &coerced, &category, &detail),
        "UUID to binary was silently admitted as an implicit cast");
  api::EngineCanonicalExpressionEvaluationRequest request;
  request.consumer = api::EngineCanonicalExpressionConsumer::projection;
  request.operation = api::EngineCanonicalExpressionOperation::scalar_function;
  request.precomputed_value = native;
  request.result_descriptor = uuid;
  api::EngineCanonicalExpressionEvaluationResult result;
  Check(api::QowEvaluateCanonicalTypedExpressionV1(request, &result, &detail),
        "bound native UUID function result was refused"); preserved(result.value, uuid);
  auto null = native;
  null.binary_value.clear(); null.is_null = true; null.state = api::EngineValueState::sql_null;
  const auto null_bytes = exec::CastDescriptorValue(null, binary, &diagnostic);
  Check(diagnostic.ok && null_bytes.is_null && null_bytes.state == api::EngineValueState::sql_null &&
        null_bytes.binary_value.empty() && null_bytes.encoded_value.empty(), "UUID cast lost payload-free NULL");
  for (const auto& source : {native, null}) {
    (void)exec::CastDescriptorValue(source, text, &diagnostic);
    Check(!diagnostic.ok, "executor admitted UUID to text cast");
  }
  auto rendered = null;
  rendered.descriptor = text;
  (void)exec::CastDescriptorValue(rendered, uuid, &diagnostic);
  Check(!diagnostic.ok, "NULL text cast bypassed UUID conversion admission");
  rendered.state = api::EngineValueState::value; rendered.is_null = false;
  rendered.encoded_value = "00000000-0000-0000-0000-000000000000";
  (void)exec::CastDescriptorValue(rendered, uuid, &diagnostic);
  Check(!diagnostic.ok, "executor interpreted textual UUID data as a native UUID");
}

void NativeUuidValues() {
  namespace api = scratchbird::engine::internal_api;
  api::TypedRelationalDag dag;
  api::RelationalTypeDescriptor descriptor;
  descriptor.descriptor_id = 1;
  descriptor.descriptor_uuid = scratchbird::tests::FixtureUuid(2081, 1);
  descriptor.type_uuid = s::ExactCanonicalCoreDatatypeTypeUuidV1("uuid");
  descriptor.nullability = api::RelationalNullability::kNonNull;
  Check(!descriptor.type_uuid.is_nil(), "VALUES UUID datatype absent from actual catalog");
  dag.descriptors.push_back(descriptor);
  api::RelationalDagNode node;
  node.node_id = 1;
  node.node_kind = api::RelationalDagNodeKind::kValues;
  node.output_descriptor_ids = {1};
  node.semantic_variant_id = "values.literal-table.v1";
  dag.outputs.push_back({1, 1, 1, "uuid_value", 1, true, 0});
  std::vector<std::vector<std::uint8_t>> expected;
  for (unsigned pattern = 0; pattern < 130; ++pattern) {
    std::vector<std::uint8_t> bytes(16);
    if (pattern == 1) std::fill(bytes.begin(), bytes.end(), 0xff);
    if (pattern >= 2) bytes[(pattern - 2) / 8] = std::uint8_t(1u << ((pattern - 2) % 8));
    api::RelationalExpressionRecord literal;
    literal.expression_id = pattern + 1;
    literal.expression_kind = api::RelationalExpressionKind::kLiteral;
    literal.result_descriptor_id = 1;
    literal.literal_kind = api::RelationalLiteralKind::kUuid;
    literal.literal_or_parameter_ref = std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    dag.expressions.push_back(std::move(literal));
    dag.values_rows.push_back({pattern + 1, {pattern + 1}});
    node.values_row_ids.push_back(pattern + 1);
    expected.push_back(std::move(bytes));
  }
  dag.nodes.push_back(std::move(node));
  s::plan::CanonicalLogicalRelationalNode logical;
  logical.logical_node_id = 1;
  const auto values = s::MaterializeValues(dag, logical, {});
  if (!values.ok) std::cerr << values.detail << '\n';
  Check(values.ok && values.batch.rows.size() == expected.size(), "native UUID VALUES materialization failed");
  for (std::size_t i = 0; i < expected.size(); ++i) {
    Check(values.batch.rows[i].values.size() == 1, "UUID VALUES row width changed");
    const auto& value = values.batch.rows[i].values.front();
    Check(value.state == api::EngineValueState::value && !value.is_null &&
          value.encoded_value.empty() && value.binary_value == expected[i],
          "VALUES reinterpreted or formatted a UUID data payload");
    NativeExecutorUuidValue(value);
  }
  namespace exec = scratchbird::engine::executor;
  exec::CanonicalDescriptorOrderTerm term;
  term.expression_descriptor_id = 1;
  auto sorted_bytes = expected;
  std::sort(sorted_bytes.begin(), sorted_bytes.end(), [](const auto& a, const auto& b) {
    for (unsigned byte = 0; byte < 16; ++byte)
      if (a[byte] != b[byte]) return a[byte] < b[byte];
    return false;
  });
  for (bool ascending : {true, false}) {
    exec::DescriptorRuntimeDiagnostic diagnostic;
    const auto sorted = exec::SortDescriptorBatchByColumn(values.batch, 0, ascending, &diagnostic);
    Check(diagnostic.ok && sorted.rows.size() == sorted_bytes.size(), "native descriptor sort failed");
    for (std::size_t i = 0; i < sorted_bytes.size(); ++i)
      Check(sorted.rows[i].values.front().binary_value ==
                sorted_bytes[ascending ? i : sorted_bytes.size() - i - 1] &&
            sorted.rows[i].values.front().encoded_value.empty(),
            "descriptor sort compared missing text instead of native UUID bytes");
  }
  std::vector<std::string> keys;
  for (const auto& row : values.batch.rows) {
    const auto key = exec::MakeCanonicalDescriptorEqualityKey(row.values.front(), term);
    if (!key.diagnostic.ok) std::cerr << key.diagnostic.detail << '\n';
    Check(key.diagnostic.ok, "native UUID equality key was refused");
    keys.push_back(key.equality_key);
  }
  for (std::size_t i = 0; i < expected.size(); ++i) {
    for (std::size_t j = 0; j < expected.size(); ++j) {
      const int order = expected[i] < expected[j] ? -1 : expected[j] < expected[i] ? 1 : 0;
      const auto compared = exec::CompareCanonicalDescriptorOrderValues(
          values.batch.rows[i].values.front(), values.batch.rows[j].values.front(), term);
      Check(compared.diagnostic.ok && compared.comparison == order,
            "UUID ordering lost native bits or used signed byte ordering");
      Check((keys[i] == keys[j]) == (order == 0), "UUID equality keys collapsed distinct values");
      int scalar_order = 0;
      std::string detail;
      Check(api::QowCompareCanonicalNonCollatedScalarsV1(
          values.batch.rows[i].values.front(), values.batch.rows[j].values.front(), &scalar_order, &detail) &&
          scalar_order == order, "canonical scalar UUID comparison lost native bits");
    }
  }
  const auto& native = values.batch.rows.front().values.front();
  auto null = native;
  null.is_null = true;
  null.state = api::EngineValueState::sql_null;
  null.binary_value.clear();
  for (const auto placement : {exec::CanonicalDescriptorNullPlacement::first,
                               exec::CanonicalDescriptorNullPlacement::last}) {
    term.null_placement = placement;
    const auto compared = exec::CompareCanonicalDescriptorOrderValues(native, null, term);
    Check(compared.diagnostic.ok && compared.comparison ==
          (placement == exec::CanonicalDescriptorNullPlacement::first ? 1 : -1),
          "UUID ordering lost explicit NULL placement");
  }
  for (unsigned mutation = 0; mutation < 5; ++mutation) {
    auto bad = native;
    if (mutation == 0) bad.binary_value.resize(15);
    if (mutation == 1) bad.binary_value.resize(17);
    if (mutation == 2) bad.encoded_value = std::string(16, '\0');
    if (mutation == 3) { bad.binary_value.clear(); bad.encoded_value = std::string(16, '\0'); }
    if (mutation == 4) { bad.is_null = true; bad.state = api::EngineValueState::sql_null; }
    auto malformed_batch = values.batch;
    malformed_batch.rows.front().values.front() = bad;
    Check(!exec::ValidateCanonicalDescriptorBatch(malformed_batch, {1}).ok,
          "canonical descriptor batch accepted malformed UUID carrier");
    Check(!exec::CompareCanonicalDescriptorOrderValues(bad, native, term).diagnostic.ok &&
          !exec::CompareCanonicalDescriptorOrderValues(bad, null, term).diagnostic.ok &&
          !exec::MakeCanonicalDescriptorEqualityKey(bad, term).diagnostic.ok,
          "UUID order/equality accepted malformed native payload");
    api::EngineTypedValue output;
    std::string category, detail;
    Check(!api::QowApplyCanonicalDescriptorCoercionV1(bad, native.descriptor, true, &output, &category, &detail),
          "canonical coercion accepted malformed UUID carrier");
    exec::DescriptorRuntimeDiagnostic diagnostic;
    const auto sorted = exec::SortDescriptorBatchByColumn(malformed_batch, 0, true, &diagnostic);
    Check(!diagnostic.ok && sorted.rows.empty(), "descriptor sort admitted a malformed UUID carrier");
    (void)exec::CastDescriptorValue(bad, native.descriptor, &diagnostic);
    Check(!diagnostic.ok, "executor cast accepted malformed UUID carrier");
    (void)exec::ExtractDescriptorField(bad, "version", &diagnostic);
    Check(!diagnostic.ok, "UUID extraction accepted malformed UUID carrier");
  }
  for (const auto& malformed : {std::string(15, '\0'), std::string(17, '\0'),
                               std::string("00000000-0000-0000-0000-000000000000")}) {
    auto changed = dag;
    changed.expressions.front().literal_or_parameter_ref = malformed;
    const auto refused = s::MaterializeValues(changed, logical, {});
    Check(!refused.ok && refused.batch.rows.empty() && refused.result_bindings.empty(),
          "malformed UUID VALUES literal published data");
  }
}
void NativeBinarySort() {
  namespace exec = scratchbird::engine::executor;
  using Cell = std::optional<std::vector<std::uint8_t>>;
  const std::vector<Cell> input = {std::vector<std::uint8_t>{0xff}, std::nullopt,
      std::vector<std::uint8_t>{0, 0}, std::vector<std::uint8_t>{},
      std::vector<std::uint8_t>{0x80}, std::vector<std::uint8_t>{0}};
  const std::vector<Cell> expected = {std::nullopt, std::vector<std::uint8_t>{},
      std::vector<std::uint8_t>{0}, std::vector<std::uint8_t>{0, 0},
      std::vector<std::uint8_t>{0x80}, std::vector<std::uint8_t>{0xff}};
  exec::DescriptorBatch batch;
  const auto descriptor = exec::MakeExecutorDescriptor("binary");
  batch.columns.push_back({"bytes", descriptor, true});
  for (const auto& cell : input) {
    auto value = exec::MakeExecutorValue(descriptor, {}, !cell.has_value());
    if (cell) value.binary_value = *cell;
    batch.rows.push_back({{value}});
  }
  for (bool ascending : {true, false}) {
    exec::DescriptorRuntimeDiagnostic diagnostic;
    const auto sorted = exec::SortDescriptorBatchByColumn(batch, 0, ascending, &diagnostic);
    Check(diagnostic.ok && sorted.rows.size() == expected.size(), "binary descriptor sort failed");
    for (std::size_t i = 0; i < expected.size(); ++i) {
      const auto& cell = expected[ascending ? i : expected.size() - i - 1];
      const auto& actual = sorted.rows[i].values.front();
      Check(actual.is_null == !cell.has_value() && actual.encoded_value.empty() &&
            actual.binary_value == cell.value_or(std::vector<std::uint8_t>{}),
            "binary sort lost unsigned order, prefix order, or NULL/empty distinction");
    }
  }
}
int main() {
  NativeBinarySort();
  NativeBuiltinDescriptors();
  NativeUuidValues();
  NativeComparisons();
  const auto package = f::BuildStandardFunctionSeedPackage();
  const char* functions[] = {"data.scalar.cast", "sb.scalar.safe_cast", "sb.scalar.try_cast"};
  for (unsigned pattern = 0; pattern < 130; ++pattern) {
    s::SblrUuid id;
    if (pattern == 1) id.bytes.fill(0xff);
    if (pattern >= 2) id.bytes[(pattern - 2) / 8] = std::uint8_t(1u << ((pattern - 2) % 8));
    const std::vector<std::uint8_t> bytes(id.bytes.begin(), id.bytes.end());
    const auto uuid = s::MakeSblrUuidValue(id);
    const auto binary = f::MakeBinaryValue("binary", bytes);
    Published(Scalar(s::EvaluateSblrCastForm("cast", uuid, "uuid", {}, false, false)), bytes, "uuid");
    Published(Scalar(s::EvaluateSblrCastForm("cast", uuid, "binary", {}, true, false)), bytes, "binary");
    Published(Scalar(s::EvaluateSblrCastForm("cast", binary, "uuid", {}, true, false)), bytes, "uuid");
    Check(!s::EvaluateSblrCastForm("cast", uuid, "binary", {}, false, false).ok(), "implicit UUID/binary cast bypassed permission");
    Check(!s::EvaluateSblrCastForm("cast", binary, "uuid", {}, false, false).ok(), "implicit binary/UUID cast bypassed permission");
    for (const auto* function : functions) {
      Published(Scalar(Call(package.registry, function, uuid, "uuid")), bytes, "uuid");
      Published(Scalar(Call(package.registry, function, uuid, "binary")), bytes, "binary");
      Published(Scalar(Call(package.registry, function, binary, "uuid")), bytes, "uuid");
      Published(Scalar(Call(package.registry, function, uuid, "varbinary")), bytes, "varbinary");
    }
    const auto nil = s::MakeSblrUuidValue({});
    Published(Scalar(CallValues(package.registry, "sb.scalar.greatest", {nil, uuid})), bytes, "uuid");
    Published(Scalar(CallValues(package.registry, "sb.scalar.least", {uuid, nil})), std::vector<std::uint8_t>(16), "uuid");
    const auto nullif = CallValues(package.registry, "sb.scalar.nullif", {uuid, nil});
    Check(Scalar(nullif).is_null == (pattern == 0), "registered NULLIF compared empty UUID text fields");
    if (pattern != 0) Published(Scalar(nullif), bytes, "uuid");
  }
  const auto null = f::MakeNullValue("uuid");
  for (const auto* function : functions) {
    for (const auto* target : {"uuid", "binary", "varbinary"}) {
      const auto result = Call(package.registry, function, null, target);
      const auto& value = Scalar(result);
      Check(value.descriptor_id == target && s::SblrNullPayloadEmpty(value), "cast NULL acquired payload or lost target");
    }
  }
  s::SblrUuid id; id.bytes.fill(0x55);
  const auto native = s::MakeSblrUuidValue(id);
  for (const auto* function : functions)
    Check(!Call(package.registry, function, native, "not_a_type").ok(), "safe cast manufactured a NULL with an unknown target type");
  for (unsigned gate = 0; gate < 3; ++gate) {
    f::FunctionCallRequest request;
    request.context.function_uuid = package.registry.Lookup(functions[0])->function_uuid;
    request.context.security_allowed = gate != 0;
    request.context.policy_allowed = gate != 1;
    request.context.dependency_available = gate != 2;
    request.arguments = {{"value", native}, {"target", f::MakeTextValue("text", "uuid")}};
    const auto result = f::DispatchFunctionCall(package.registry, std::move(request));
    Check(!result.result.ok() && result.result.scalar_values.empty(), "native cast bypassed a function admission gate");
  }
  const auto text = f::MakeTextValue("text", "019f1122-3344-7566-8788-99aabbccddee");
  Check(!Call(package.registry, functions[0], text, "uuid").ok(), "engine CAST parsed UUID text");
  Check(!Call(package.registry, functions[0], native, "text").ok(), "engine CAST formatted UUID text");
  for (const auto* function : {functions[1], functions[2]}) {
    Check(s::SblrNullPayloadEmpty(Scalar(Call(package.registry, function, text, "uuid"))), "safe text conversion produced UUID data");
    Check(s::SblrNullPayloadEmpty(Scalar(Call(package.registry, function, native, "text"))), "safe UUID conversion produced text");
  }
  for (unsigned length : {0u, 1u, 15u, 17u, 36u}) {
    const auto binary = f::MakeBinaryValue("binary", std::vector<std::uint8_t>(length, 0x55));
    Check(!Call(package.registry, functions[0], binary, "uuid").ok(), "wrong UUID length cast successfully");
    for (const auto* function : {functions[1], functions[2]})
      Check(s::SblrNullPayloadEmpty(Scalar(Call(package.registry, function, binary, "uuid"))), "safe invalid-length cast published data");
  }
  for (unsigned fault = 0; fault < 11; ++fault) {
    auto value = native;
    if (fault == 0) value.text_value = "hidden";
    if (fault == 1) value.encoded_value = std::string(16, '\0');
    if (fault == 2) value.binary_value = {0};
    if (fault == 3) value.uuid_array_value = {id};
    if (fault == 4) value.charset_name = "UTF8";
    if (fault == 5) value.collation_name = "binary";
    if (fault == 6) value.has_int64_value = true;
    if (fault == 7) value.has_uint64_value = true;
    if (fault == 8) value.has_real64_value = true;
    if (fault == 9) value.payload_kind = s::SblrValuePayloadKind::uuid_text;
    if (fault == 10) value.is_null = true;
    Check(!s::ProjectionSblrValueResolved(value), "dirty UUID considered resolved");
    bool refused = false;
    try { (void)s::EngineTypedValueFromSblrValue(value); } catch (const std::invalid_argument&) { refused = true; }
    Check(refused, "dirty UUID escaped projection publication");
    Check(!s::EvaluateSblrCastForm("cast", value, "uuid", {}, true, false).ok(), "dirty UUID cast source accepted");
    Check(!s::EvaluateSblrComparison("op_eq", value, native, {}).ok(), "dirty UUID comparison accepted");
    Check(!s::EvaluateSblrNullIfForm("nullif", value, native).ok(), "dirty UUID NULLIF accepted");
    Check(!s::EvaluateSblrInListForm("in", native, {value}).ok(), "dirty UUID IN candidate accepted");
    Check(!s::EvaluateSblrBetweenForm("between", native, value, native).ok(), "dirty UUID BETWEEN bound accepted");
    for (const auto* function : functions)
      Check(!Call(package.registry, function, value, "uuid").ok(), "safe or ordinary cast erased malformed representation");
    for (const auto* function : {"sb.scalar.nullif", "sb.scalar.greatest", "sb.scalar.least"})
      Check(!CallValues(package.registry, function, {value, native}).ok(), "registered selection accepted malformed UUID carrier");
  }
  bool refused = false;
  try { (void)f::MakeTextValue("uuid", "019f1122-3344-7566-8788-99aabbccddee"); }
  catch (const std::invalid_argument&) { refused = true; }
  Check(refused, "text factory still manufactures engine UUIDs");
  std::cout << "native UUID casts preserve all 128 bits through function and projection publication\n";
}
