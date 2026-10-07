// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "registry/function_seed_registry.hpp"
#include "dispatch/function_dispatch.hpp"
#include "common/function_result_helpers.hpp"
#include "sblr/sblr_special_forms.hpp"
#include "sblr/sblr_operator_runtime.hpp"
#include "sblr/sblr_projection_value_runtime.hpp"
#include "sblr/canonical_query_object_free_composition_support.hpp"
#include "sblr/canonical_query_descriptor_support.hpp"
#include "sblr/canonical_query_aggregate_registration.hpp"
#include "internal_api/query/expression_api.hpp"
#include "internal_api/catalog/datatype_bootstrap_identity.hpp"
#include "datatype_catalog_manifest.hpp"
#include "datatype_operations.hpp"
#include "../support/binary_uuid_fixture.hpp"
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>

// Warmed component-operation allocation evidence; no failure injection or
// skipped validation. Exact counts are observations, not allocator ABI rules.
thread_local bool track_native_cast_allocations = false;
thread_local std::size_t native_cast_allocations = 0;
void* operator new(std::size_t bytes) {
  if (track_native_cast_allocations) ++native_cast_allocations;
  if (void* allocation = std::malloc(bytes ? bytes : 1)) return allocation;
  throw std::bad_alloc();
}
void operator delete(void* allocation) noexcept { std::free(allocation); }
void operator delete(void* allocation, std::size_t) noexcept { std::free(allocation); }

namespace s = scratchbird::engine::sblr;
namespace f = scratchbird::engine::functions;
unsigned checks = 0;
void Check(bool pass, const char* message) {
  ++checks;
  if (!pass) { std::cerr << message << '\n'; std::exit(1); }
}
std::shared_ptr<const scratchbird::engine::internal_api::EngineDescriptor> Binding(
    std::string type) {
  if (type == "varbinary") type = "binary";
  auto descriptor = scratchbird::engine::executor::MakeExecutorDescriptor(type);
  descriptor.descriptor_kind = "scalar";
  descriptor.encoded_descriptor = "nullability=nullable";
  return std::make_shared<const scratchbird::engine::internal_api::EngineDescriptor>(std::move(descriptor));
}
s::SblrValue Bound(s::SblrValue value) {
  value.projection_descriptor = Binding(value.descriptor_id);
  return value;
}
s::SblrResult CallValues(const f::FunctionRegistry& registry, const char* name,
                         std::vector<s::SblrValue> values) {
  const auto* entry = registry.Lookup(name);
  Check(entry != nullptr, "cast absent from actual callable registry");
  f::FunctionCallRequest request;
  if (values.size() == 2 && values[1].payload_kind == s::SblrValuePayloadKind::text &&
      (std::string_view(name).find("cast") != std::string_view::npos))
    request.result_descriptor = Binding(values[1].text_value);
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
  if (!result.ok() || result.scalar_values.size() != 1 || !result.rows.empty()) {
    for (const auto& diagnostic : result.diagnostics) {
      std::cerr << diagnostic.diagnostic_id << ':' << diagnostic.message_key;
      for (const auto& field : diagnostic.fields) {
        if (const auto* value = std::get_if<std::string>(&field.value))
          std::cerr << ' ' << field.key << '=' << *value;
      }
      std::cerr << '\n';
    }
  }
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
    auto value = exec::MakeExecutorDescriptor(name);
    value.descriptor_kind = "scalar";
    value.descriptor_uuid = scratchbird::tests::FixtureUuid(2085, ordinal);
    value.encoded_descriptor = "nullability=nullable";
    Check(!value.type_uuid.is_nil() &&
              !value.datatype_descriptor_uuid.is_nil() &&
              value.datatype_descriptor_generation != 0,
          "cast datatype absent from actual catalog");
    return value;
  };
  const auto binary = descriptor("binary", 1), uuid = descriptor("uuid", 2), text = descriptor("text", 3);
  auto mismatched_identity = uuid;
  mismatched_identity.canonical_type_name = "int64";
  scratchbird::engine::ExecutionTypeDescriptor ignored_descriptor;
  std::string mismatch_detail;
  Check(!exec::BuildBoundExecutionTypeDescriptor(
            mismatched_identity,
            scratchbird::core::datatypes::CanonicalTypeId::int64,
            &ignored_descriptor, &mismatch_detail),
        "executor binder accepted a registry row for a different canonical type");
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
  auto unbound_null = null;
  unbound_null.descriptor.datatype_descriptor_uuid = {};
  const auto null_bytes = exec::CastDescriptorValue(unbound_null, binary, &diagnostic);
  Check(!diagnostic.ok &&
            diagnostic.diagnostic_code == "DATATYPE.DESCRIPTOR.INVALID" &&
            null_bytes.descriptor.canonical_type_name.empty(),
        "descriptor-incomplete VALUES UUID NULL did not fail closed");
  auto bound_null = null;
  bound_null.descriptor = uuid;
  auto mismatched_null = bound_null;
  mismatched_null.descriptor = mismatched_identity;
  (void)exec::CastDescriptorValue(mismatched_null, binary, &diagnostic);
  Check(!diagnostic.ok &&
            diagnostic.diagnostic_code == "DATATYPE.DESCRIPTOR.INVALID",
        "executor NULL cast accepted mismatched datatype registry identity");
  Check(!api::QowApplyCanonicalDescriptorCoercionV1(
            mismatched_null, binary, true, &coerced, &category, &detail) &&
            detail.rfind("DATATYPE.DESCRIPTOR.INVALID", 0) == 0,
        "query NULL cast accepted mismatched datatype registry identity");
  const auto bound_null_bytes =
      exec::CastDescriptorValue(bound_null, binary, &diagnostic);
  Check(diagnostic.ok && bound_null_bytes.is_null &&
            bound_null_bytes.state == api::EngineValueState::sql_null &&
            bound_null_bytes.binary_value.empty() &&
            bound_null_bytes.encoded_value.empty() &&
            bound_null_bytes.descriptor == binary,
        "exactly bound UUID NULL cast lost its descriptor or zero payload");
  auto unbound_target = binary;
  unbound_target.descriptor_uuid = {};
  (void)exec::CastDescriptorValue(bound_null, unbound_target, &diagnostic);
  Check(!diagnostic.ok &&
            diagnostic.diagnostic_code == "DATATYPE.DESCRIPTOR.INVALID",
        "NULL cast did not reject an unbound target occurrence canonically");
  auto malformed_null = bound_null;
  malformed_null.binary_value.push_back(0);
  (void)exec::CastDescriptorValue(malformed_null, binary, &diagnostic);
  Check(!diagnostic.ok &&
            diagnostic.diagnostic_code == "DATATYPE.NULL_STATE.INVALID",
        "NULL cast wrapped or lost the canonical malformed-state diagnostic");
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

void NativeUuidValues(const f::FunctionRegistry& registry) {
  namespace api = scratchbird::engine::internal_api;
  api::TypedRelationalDag dag;
  namespace dt = scratchbird::core::datatypes;
  const auto builtin = scratchbird::engine::executor::MakeExecutorDescriptor("uuid");
  const auto identity = dt::LookupDatatypeTypeCodecIdentityV1(
      api::kBootstrapDatatypeCatalogUuid, api::kBootstrapDatatypeCatalogGeneration,
      api::kBootstrapDatatypeRegistryGeneration, builtin.datatype_descriptor_uuid,
      builtin.datatype_descriptor_generation);
  Check(identity.ok, "VALUES fixture cannot resolve the actual UUID codec row");
  const auto& row = identity.row;
  api::RelationalTypeDescriptor descriptor;
  descriptor.descriptor_id = 1;
  descriptor.descriptor_uuid = row.descriptor_uuid;
  descriptor.type_uuid = row.type_uuid;
  descriptor.datatype_identity_authoritative = true;
  descriptor.descriptor_generation = row.descriptor_generation;
  descriptor.type_generation = row.type_generation;
  descriptor.codec_id = row.codec_id;
  descriptor.codec_version = row.codec_version;
  descriptor.codec_generation = row.codec_generation;
  descriptor.statement_receipt_uuid = scratchbird::tests::FixtureUuid(2081, 1);
  descriptor.datatype_catalog_snapshot_uuid = row.catalog_snapshot_uuid;
  descriptor.datatype_catalog_generation = row.catalog_generation;
  descriptor.datatype_registry_generation = row.registry_generation;
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
  null.descriptor = exec::MakeExecutorDescriptor("uuid");
  null.descriptor.descriptor_kind = "scalar";
  null.descriptor.descriptor_uuid = scratchbird::tests::FixtureUuid(2085, 4);
  null.descriptor.encoded_descriptor = "nullability=nullable";
  null.is_null = true;
  null.state = api::EngineValueState::sql_null;
  null.binary_value.clear();
  auto bound_native = native;
  bound_native.descriptor = null.descriptor;
  for (const auto placement : {exec::CanonicalDescriptorNullPlacement::first,
                               exec::CanonicalDescriptorNullPlacement::last}) {
    term.null_placement = placement;
    const auto compared = exec::CompareCanonicalDescriptorOrderValues(
        bound_native, null, term);
    Check(compared.diagnostic.ok && compared.comparison ==
          (placement == exec::CanonicalDescriptorNullPlacement::first ? 1 : -1),
          "UUID ordering lost explicit NULL placement");
  }
  auto flag_malformed_stale = bound_native;
  flag_malformed_stale.is_null = true;
  flag_malformed_stale.descriptor.descriptor_uuid = {};
  const auto precedence = exec::CompareCanonicalDescriptorOrderValues(
      flag_malformed_stale, bound_native, term);
  Check(!precedence.diagnostic.ok &&
            precedence.diagnostic.diagnostic_code ==
                "DATATYPE.DESCRIPTOR.INVALID",
        "UUID ordering reported malformed NULL state before stale descriptor authority");
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
  // Real expression evaluation -> supplied callback binding -> registered
  // cast -> Core -> native projection. This is a component seam, not IPC or
  // statement-security admission; the callback adds the callable's type label.
  for (const auto* type : {"uuid", "binary"}) {
    auto function_dag = dag;
    auto output_descriptor = descriptor;
    const auto runtime_target = Binding(type);
    const auto target_identity = dt::LookupDatatypeTypeCodecIdentityV1(
        api::kBootstrapDatatypeCatalogUuid, api::kBootstrapDatatypeCatalogGeneration,
        api::kBootstrapDatatypeRegistryGeneration, runtime_target->datatype_descriptor_uuid,
        runtime_target->datatype_descriptor_generation);
    Check(target_identity.ok, "relational callable target codec missing");
    const auto& target_row = target_identity.row;
    output_descriptor.descriptor_id = 2;
    output_descriptor.descriptor_uuid = target_row.descriptor_uuid;
    output_descriptor.type_uuid = target_row.type_uuid;
    output_descriptor.descriptor_generation = target_row.descriptor_generation;
    output_descriptor.type_generation = target_row.type_generation;
    output_descriptor.codec_id = target_row.codec_id;
    output_descriptor.codec_version = target_row.codec_version;
    output_descriptor.codec_generation = target_row.codec_generation;
    function_dag.descriptors.push_back(output_descriptor);
    api::RelationalExpressionRecord function;
    function.expression_id = 131;
    function.expression_kind = api::RelationalExpressionKind::kFunctionCall;
    function.result_descriptor_id = 2;
    function.function_uuid = registry.Lookup("data.scalar.cast")->function_uuid;
    function.child_expression_ids = {2}; // all-ones UUID, not a system UUIDv7.
    function_dag.expressions.push_back(function);
    bool called = false;
    api::EngineDescriptor expected_binding;
    Check(s::BuildExactCanonicalScalarRuntimeDescriptorV1(output_descriptor,
              dt::CanonicalTypeIdFromStableName(type), &expected_binding),
          "relational callable fixture target invalid");
    s::CanonicalRelationalExpressionRuntimeServices services;
    services.function_evaluator = [&](const api::EngineUuid& function_id,
        const std::vector<api::EngineTypedValue>& arguments,
        const api::EngineDescriptor& supplied_target, api::EngineTypedValue* output,
        std::string*, std::string*) {
      called = true;
      Check(arguments.size() == 1 && supplied_target == expected_binding &&
                !arguments.front().descriptor.datatype_descriptor_uuid.is_nil(),
            "relational callback dropped the bound argument/result descriptor");
      f::FunctionCallRequest request;
      request.context.function_uuid = function_id;
      request.context.security_allowed = request.context.policy_allowed = true;
      Check(registry.BindCallContext(request.context) != nullptr, "relational cast lookup failed");
      request.result_descriptor = std::make_shared<const api::EngineDescriptor>(supplied_target);
      request.arguments = {{"value", s::SblrValueFromProjectionArgument(
          api::MakeProjectionFunctionArgument("arg", arguments.front()))},
          {"target", f::MakeTextValue("text", type)}};
      const auto dispatched = f::DispatchFunctionCall(registry, std::move(request)).result;
      if (!dispatched.ok()) return false;
      *output = s::EngineTypedValueFromSblrValue(Scalar(dispatched));
      return true;
    };
    s::CanonicalRelationalExpressionRuntime runtime(function_dag, services);
    api::EngineTypedValue output;
    std::string detail;
    const bool ok = runtime.Evaluate(131, type, &output, &detail);
    if (!ok) std::cerr << detail << '\n';
    Check(ok && called && output.descriptor == expected_binding &&
              output.binary_value == std::vector<std::uint8_t>(16, 0xff) &&
              output.encoded_value.empty(), "relational callable cast failed exact native publication");
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
void NativeCastBindingContracts(const f::FunctionRegistry& registry) {
  namespace api = scratchbird::engine::internal_api;
  const auto target = Binding("uuid");
  auto uuid = Bound(s::MakeSblrUuidValue({}));
  auto binary = Bound(f::MakeBinaryValue("binary", std::vector<std::uint8_t>(16, 0xff)));
  const auto dispatch = [&](const char* name, s::SblrValue value,
                            std::shared_ptr<const api::EngineDescriptor> result_binding) {
    f::FunctionCallRequest request;
    request.context.function_uuid = registry.Lookup(name)->function_uuid;
    request.context.security_allowed = request.context.policy_allowed = true;
    Check(registry.BindCallContext(request.context) != nullptr, "bound cast function lookup");
    request.result_descriptor = std::move(result_binding);
    request.arguments = {{"value", std::move(value)}, {"target", f::MakeTextValue("text", "uuid")}};
    return f::DispatchFunctionCall(registry, std::move(request)).result;
  };
  for (const auto* name : {"data.scalar.cast", "sb.scalar.safe_cast", "sb.scalar.try_cast"}) {
    for (const auto& source : {uuid, binary}) {
      const auto result = dispatch(name, source, target);
      const auto& scalar = Scalar(result);
      Check(scalar.projection_descriptor == target &&
                s::EngineTypedValueFromSblrValue(scalar).descriptor == *target,
            "cast publication dropped or reconstructed its admitted target");
      for (bool null_source : {false, true}) {
        for (bool mutate_target : {false, true}) {
          for (unsigned mutation = 0; mutation < 12; ++mutation) {
            auto input = source;
            if (null_source) {
              input.is_null = true; input.payload_kind = s::SblrValuePayloadKind::none;
              input.uuid_value = {}; input.binary_value.clear();
            }
            auto binding = std::make_shared<api::EngineDescriptor>(
                mutate_target ? *target : *source.projection_descriptor);
            switch (mutation) {
              case 0: binding->descriptor_uuid = {}; break;
              case 1: binding->datatype_descriptor_uuid = {}; break;
              case 2: ++binding->datatype_descriptor_generation; break;
              case 3: binding->type_uuid = {}; break;
              case 4: binding->canonical_type_name = "int64"; break;
              case 5: binding->encoded_descriptor += ";width=16"; break;
              case 6: binding->encoded_descriptor += ";precision=16"; break;
              case 7: binding->charset_uuid = scratchbird::tests::FixtureUuid(2081, 20); break;
              case 8: binding->encoded_descriptor += ";nullability=nullable"; break;
              case 9: binding->encoded_descriptor = "nullability=unknown"; break;
              case 10: binding->descriptor_kind = "record"; break;
              case 11: binding->encoded_descriptor += ";uuid_ordering_profile=guid"; break;
            }
            if (!mutate_target) input.projection_descriptor = binding;
            const auto refused = dispatch(name, input, mutate_target ? binding : target);
            Check(!refused.ok() && refused.scalar_values.empty() &&
                      !refused.diagnostics.empty() &&
                      refused.diagnostics.front().diagnostic_id == "DATATYPE.DESCRIPTOR.INVALID",
                  "cast/SAFE_CAST/TRY_CAST laundered invalid source or target binding");
          }
        }
      }
      auto unbound = source; unbound.projection_descriptor.reset();
      Check(!dispatch(name, unbound, target).ok() && !dispatch(name, source, {}).ok(),
            "cast inferred missing authority from native payload or target spelling");
    }
    auto null = uuid; null.is_null = true; null.payload_kind = s::SblrValuePayloadKind::none;
    const auto null_result = dispatch(name, null, target);
    const auto& scalar = Scalar(null_result);
    Check(s::SblrNullPayloadEmpty(scalar) && scalar.projection_descriptor == target,
          "bound native NULL cast lost exact target or acquired a payload");
    auto bad_length = binary; bad_length.binary_value.resize(15);
    auto nonnull = std::make_shared<api::EngineDescriptor>(*target);
    nonnull->encoded_descriptor = "nullability=non_null";
    Check(!dispatch(name, bad_length, nonnull).ok(), "TRY_CAST created NULL for nonnullable target");
    auto nonnull_source = std::make_shared<api::EngineDescriptor>(*binary.projection_descriptor);
    nonnull_source->encoded_descriptor = "nullability=non_null";
    bad_length.projection_descriptor = nonnull_source;
    const auto length_result = dispatch(name, bad_length, target);
    if (std::string_view(name) == "sb.scalar.try_cast") {
      Check(s::SblrNullPayloadEmpty(Scalar(length_result)) &&
                Scalar(length_result).projection_descriptor == target,
            "TRY_CAST wrongly requires nullable source to publish nullable target");
    } else {
      Check(!length_result.ok() && length_result.scalar_values.empty(),
            "ordinary/SAFE_CAST suppressed native length failure");
    }
    auto bad_null = null; bad_null.binary_value = {0};
    Check(!dispatch(name, bad_null, target).ok(), "cast suppressed a malformed NULL carrier");
  }
  for (auto source : {uuid, binary}) {
    for (bool null_source : {false, true}) {
      if (null_source) {
        source.is_null = true; source.payload_kind = s::SblrValuePayloadKind::none;
        source.uuid_value = {}; source.binary_value.clear();
      }
      const auto engine = s::EngineTypedValueFromSblrValue(source);
      const auto argument = api::MakeProjectionFunctionArgument("arg", engine);
      Check(s::ProjectionArgumentEncodingValid(argument), "bound native projection argument invalid");
      const auto adapted = s::SblrValueFromProjectionArgument(argument);
      Check(s::ProjectionSblrValueResolved(adapted) && adapted.projection_descriptor &&
                *adapted.projection_descriptor == engine.descriptor,
            "projection argument dropped native datatype identity");
      const auto restored = s::EngineTypedValueFromSblrValue(adapted);
      Check(restored.descriptor == engine.descriptor && restored.binary_value == engine.binary_value &&
                restored.state == engine.state && restored.is_null == engine.is_null,
            "projection roundtrip altered binding, NULL state or native bytes");
      auto compatibility_null = argument;
      compatibility_null.binary_value.clear();
      for (bool flag : {false, true}) {
        compatibility_null.is_null = flag;
        compatibility_null.state = flag ? api::EngineValueState::value : api::EngineValueState::sql_null;
        const auto normalized = s::EngineTypedValueFromSblrValue(
            s::SblrValueFromProjectionArgument(compatibility_null));
        Check(s::ProjectionArgumentEncodingValid(compatibility_null) &&
                  normalized.state == api::EngineValueState::sql_null && normalized.is_null &&
                  normalized.binary_value.empty() && normalized.descriptor == engine.descriptor,
              "projection NULL compatibility input failed canonical state normalization");
        auto invalid = compatibility_null; invalid.binary_value = {0};
        Check(!s::ProjectionArgumentEncodingValid(invalid), "projection accepted NULL with binary payload");
      }
    }
  }
}
void NativeCastAllocationSample() {
  const auto uuid = Bound(s::MakeSblrUuidValue({}));
  const auto binary = Bound(f::MakeBinaryValue("binary", std::vector<std::uint8_t>(16)));
  for (const auto& [source, type] : {
      std::pair{uuid, "uuid"}, {uuid, "binary"}, {binary, "uuid"}}) {
    const auto target = Binding(type);
    Check(s::EvaluateSblrCastForm("cast", source, type, {}, true, false, target).ok(),
          "allocation sample warmup failed");
    native_cast_allocations = 0;
    track_native_cast_allocations = true;
    const auto result = s::EvaluateSblrCastForm("cast", source, type, {}, true, false, target);
    track_native_cast_allocations = false;
    Check(result.ok(), "allocation sample operation failed");
    std::cout << "native_cast_allocations " << source.descriptor_id << "->" << type
              << '=' << native_cast_allocations << '\n';
  }
}
int main() {
  const auto package = f::BuildStandardFunctionSeedPackage();
  NativeBinarySort();
  NativeBuiltinDescriptors();
  NativeUuidValues(package.registry);
  NativeComparisons();
  NativeCastBindingContracts(package.registry);
  const char* functions[] = {"data.scalar.cast", "sb.scalar.safe_cast", "sb.scalar.try_cast"};
  for (unsigned pattern = 0; pattern < 130; ++pattern) {
    s::SblrUuid id;
    if (pattern == 1) id.bytes.fill(0xff);
    if (pattern >= 2) id.bytes[(pattern - 2) / 8] = std::uint8_t(1u << ((pattern - 2) % 8));
    const std::vector<std::uint8_t> bytes(id.bytes.begin(), id.bytes.end());
    const auto uuid = Bound(s::MakeSblrUuidValue(id));
    const auto binary = Bound(f::MakeBinaryValue("binary", bytes));
    Published(Scalar(s::EvaluateSblrCastForm("cast", uuid, "uuid", {}, false, false, Binding("uuid"))), bytes, "uuid");
    Published(Scalar(s::EvaluateSblrCastForm("cast", uuid, "binary", {}, true, false, Binding("binary"))), bytes, "binary");
    Published(Scalar(s::EvaluateSblrCastForm("cast", binary, "uuid", {}, true, false, Binding("uuid"))), bytes, "uuid");
    Check(!s::EvaluateSblrCastForm("cast", uuid, "binary", {}, false, false, Binding("binary")).ok(), "implicit UUID/binary cast bypassed permission");
    Check(!s::EvaluateSblrCastForm("cast", binary, "uuid", {}, false, false, Binding("uuid")).ok(), "implicit binary/UUID cast bypassed permission");
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
      Check(!result.ok() && result.scalar_values.empty() &&
                !result.diagnostics.empty() &&
                result.diagnostics.front().diagnostic_id ==
                    "DATATYPE.DESCRIPTOR.INVALID",
            "descriptor-free SBLR NULL cast did not fail closed");
    }
  }
  s::SblrUuid id; id.bytes.fill(0x55);
  const auto native = Bound(s::MakeSblrUuidValue(id));
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
  Check(!Call(package.registry, functions[1], text, "uuid").ok() &&
            !Call(package.registry, functions[2], text, "uuid").ok(),
        "SAFE_CAST or TRY_CAST hid the forbidden text-to-UUID route");
  Check(!Call(package.registry, functions[1], native, "text").ok() &&
            !Call(package.registry, functions[2], native, "text").ok(),
        "SAFE_CAST or TRY_CAST hid a forbidden UUID presentation cast");
  for (unsigned length : {0u, 1u, 15u, 17u, 36u}) {
    const auto binary = Bound(f::MakeBinaryValue("binary", std::vector<std::uint8_t>(length, 0x55)));
    Check(!Call(package.registry, functions[0], binary, "uuid").ok(), "wrong UUID length cast successfully");
    Check(!Call(package.registry, functions[1], binary, "uuid").ok(),
          "SAFE_CAST hid an invalid UUID binary length");
    Check(s::SblrNullPayloadEmpty(
              Scalar(Call(package.registry, functions[2], binary, "uuid"))),
          "TRY_CAST invalid UUID length did not produce NULL");
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
  NativeCastAllocationSample();
  std::cout << "native UUID casts preserve all 128 bits through function and projection publication; checks=" << checks << '\n';
}
