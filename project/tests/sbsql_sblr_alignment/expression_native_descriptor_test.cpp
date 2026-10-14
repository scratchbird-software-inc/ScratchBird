// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/engine/sblr/canonical_relational_expression.cpp"
#include "canonical_query_object_free_composition_support.hpp"
#include "canonical_query_aggregate_registration.hpp"
#include "canonical_query_aggregate_composition.hpp"
#include "canonical_aggregate_registry.hpp"
#include "../support/canonical_int64_literal_fixture.hpp"
#include "../support/binary_uuid_fixture.hpp"
#include "query/historical_timestamp_scalar.hpp"
#include "mga_relation_store/stored_scalar_descriptor.hpp"
#include "sbl_numeric.hpp"
#include <cstdlib>
#include <iostream>
#include <source_location>
#include <bit>
#include <new>
#include <stdexcept>

// One-shot allocation failures leave diagnostics/retry allocations available.
thread_local long descriptor_allocation_budget = -1;
void* operator new(std::size_t size) {
  if (descriptor_allocation_budget == 0) {
    descriptor_allocation_budget = -1;
    throw std::bad_alloc();
  }
  if (descriptor_allocation_budget > 0) --descriptor_allocation_budget;
  if (auto* allocation = std::malloc(size ? size : 1)) return allocation;
  throw std::bad_alloc();
}
void operator delete(void* allocation) noexcept { std::free(allocation); }
void operator delete(void* allocation, std::size_t) noexcept { std::free(allocation); }
namespace s = scratchbird::engine::sblr;
namespace a = scratchbird::engine::internal_api;
namespace d = scratchbird::core::datatypes;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  if (!ok) { std::cerr << "failure at " << at.line() << '\n'; std::abort(); }
}
void CheckV3RelationalBindings() {
  unsigned cases = 0;
  for (const auto& identity : d::CurrentDatatypeTypeCodecIdentityRowsV3()) {
    const auto& row = identity.legacy_fields;
    if (row.catalog_generation != 5 && row.catalog_generation != 10 && row.catalog_generation != 11) continue;
    const auto type = static_cast<d::CanonicalTypeId>(row.canonical_binary_type_code);
    if (type != d::CanonicalTypeId::boolean && type != d::CanonicalTypeId::character &&
        type != d::CanonicalTypeId::int64) continue;
    ++cases;
    a::RelationalTypeDescriptor descriptor;
    descriptor.descriptor_id = 1;
    descriptor.datatype_identity_authoritative = true;
    descriptor.descriptor_uuid = row.descriptor_uuid;
    descriptor.descriptor_generation = row.descriptor_generation;
    descriptor.type_uuid = row.type_uuid;
    descriptor.type_generation = row.type_generation;
    descriptor.codec_id = row.codec_id;
    descriptor.codec_version = row.codec_version;
    descriptor.codec_generation = row.codec_generation;
    descriptor.datatype_catalog_snapshot_uuid = row.catalog_snapshot_uuid;
    descriptor.datatype_catalog_generation = row.catalog_generation;
    descriptor.datatype_registry_generation = row.registry_generation;
    descriptor.statement_receipt_uuid = scratchbird::tests::FixtureUuid(1086, 1001);
    descriptor.nullability = a::RelationalNullability::kNonNull;
    const a::EngineDatatypeCohort cohort{row.catalog_snapshot_uuid, row.catalog_generation, row.registry_generation};
    Check(s::ExactCanonicalCoreDatatypeTypeUuidV1(d::CanonicalTypeName(type), cohort) == row.type_uuid);
    for (unsigned mutation = 0; mutation < 4; ++mutation) {
      auto bad = cohort;
      if (mutation == 0) bad = {};
      if (mutation == 1) ++bad.catalog_generation;
      if (mutation == 2) ++bad.registry_generation;
      if (mutation == 3) bad.catalog_snapshot_uuid.bytes[0] ^= 1;
      Check(s::ExactCanonicalCoreDatatypeTypeUuidV1(d::CanonicalTypeName(type), bad).is_nil());
    }
    Check(s::ExactCanonicalCoreDatatypeTypeUuidV1("not-a-datatype", cohort).is_nil());
    {
      auto wrong_profile = descriptor;
      wrong_profile.decimal_codec_profile = a::RelationalTypeDescriptor::DecimalCodecProfile{
          a::EngineUuid{{0x01,0xa1,0x18,0x6b,0xdf,0x04,0x76,0x0a,0xa3,0xd5,0xd0,0x2d,0x2e,0xe3,0x01,0x54}}, 1};
      Check(!s::SameExactRelationalTypeDescriptorV2(wrong_profile, descriptor));
      Check(!s::ExactCanonicalBooleanRelationalDescriptorV1(wrong_profile, a::RelationalNullability::kNonNull));
      Check(!s::ExactCanonicalTextRelationalDescriptorV1(wrong_profile, a::RelationalNullability::kNonNull));
      a::EngineDescriptor output;
      Check(!s::BuildExactCanonicalScalarRuntimeDescriptorV1(wrong_profile, type, &output));
    }
    for (unsigned mutation = 0; mutation < 9; ++mutation) {
      auto candidate = descriptor;
      if (mutation == 1) ++candidate.datatype_catalog_generation;
      if (mutation == 2) ++candidate.datatype_registry_generation;
      if (mutation == 3) candidate.datatype_catalog_snapshot_uuid.bytes[0] ^= 1;
      if (mutation == 4) ++candidate.type_generation;
      if (mutation == 5) ++candidate.codec_generation;
      if (mutation == 6) ++candidate.codec_version;
      if (mutation == 7) candidate.codec_id += ".invalid";
      if (mutation == 8) candidate.statement_receipt_uuid = {};
      if (type != d::CanonicalTypeId::character) {
        a::EngineDescriptor output;
        Check(s::BuildExactCanonicalScalarRuntimeDescriptorV1(candidate, type, &output) == (mutation == 0));
        if (mutation == 0) Check(output.datatype_cohort == cohort &&
            output.datatype_descriptor_uuid == row.descriptor_uuid &&
            output.datatype_descriptor_generation == row.descriptor_generation &&
            output.type_uuid == row.type_uuid && output.canonical_type_name == d::CanonicalTypeName(type));
      }
      if (type == d::CanonicalTypeId::boolean) {
        a::EngineDescriptor output;
        Check(s::BuildExactCanonicalBooleanRuntimeDescriptorV1(candidate,
            a::RelationalNullability::kNonNull, &output) == (mutation == 0));
        if (mutation == 0) Check(output.datatype_cohort == cohort);
      }
      if (type == d::CanonicalTypeId::character)
        Check(s::ExactCanonicalTextRelationalDescriptorV1(candidate,
            a::RelationalNullability::kNonNull) == (mutation == 0));
      a::TypedRelationalDag dag;
      dag.descriptors = {candidate};
      a::RelationalExpressionRecord expression;
      expression.expression_id = 1;
      expression.result_descriptor_id = 1;
      expression.expression_kind = a::RelationalExpressionKind::kParameter;
      expression.parameter_typed_value_v1.emplace();
      expression.parameter_typed_value_v1->descriptor_uuid = candidate.descriptor_uuid;
      expression.parameter_typed_value_v1->descriptor_generation = candidate.descriptor_generation;
      expression.parameter_typed_value_v1->value_state = "value";
      dag.expressions = {expression};
      s::CanonicalRelationalExpressionRuntime runtime(dag);
      std::string name, detail;
      Check(runtime.InferType(1, std::nullopt, &name, &detail) == (mutation == 0));
      if (mutation == 0) Check(name == d::CanonicalTypeName(type));
    }
  }
  Check(cases == 9);
}

void CheckFunctionResultAdmission(const a::EngineTypedValue& input) {
  const auto type = d::CanonicalTypeIdFromStableName(input.descriptor.canonical_type_name);
  if (type == d::CanonicalTypeId::uuid || type == d::CanonicalTypeId::binary) return;
  a::EngineCanonicalExpressionEvaluationRequest request;
  request.consumer = a::EngineCanonicalExpressionConsumer::projection;
  request.operation = a::EngineCanonicalExpressionOperation::scalar_function;
  request.precomputed_value = input;
  request.result_descriptor = input.descriptor;
  request.result_descriptor.descriptor_uuid = scratchbird::tests::FixtureUuid(1086, 1021);
  const auto evaluate = [&](const auto& candidate, bool accepted) {
    a::EngineCanonicalExpressionEvaluationResult result;
    result.value = input;
    std::string detail;
    const bool ok = a::QowEvaluateCanonicalTypedExpressionV1(candidate, &result, &detail);
    if (ok != accepted) std::cerr << input.descriptor.canonical_type_name << ':' << detail << '\n';
    Check(ok == accepted);
    if (ok) {
      Check(result.value.descriptor == candidate.result_descriptor &&
          result.value.state == candidate.precomputed_value->state &&
          result.value.is_null == candidate.precomputed_value->is_null &&
          result.value.binary_value == candidate.precomputed_value->binary_value &&
          result.value.encoded_value == candidate.precomputed_value->encoded_value);
    } else {
      Check(!detail.empty() && result.value.state == a::EngineValueState::error &&
            result.value.binary_value.empty() && result.value.encoded_value.empty());
    }
  };
  evaluate(request, true);
  bool exhausted = false;
  unsigned injected_failures = 0;
  for (long budget = 0; budget < 2048; ++budget) {
    a::EngineCanonicalExpressionEvaluationResult result;
    result.value = input;
    std::string detail;
    bool accepted = false;
    descriptor_allocation_budget = budget;
    try { accepted = a::QowEvaluateCanonicalTypedExpressionV1(request, &result, &detail); }
    catch (const std::bad_alloc&) {}
    const bool injected = descriptor_allocation_budget == -1;
    descriptor_allocation_budget = -1;
    if (!injected) { Check(accepted); exhausted = true; break; }
    ++injected_failures;
    Check(!accepted && result.value.state == a::EngineValueState::error &&
        result.value.encoded_value.empty() && result.value.binary_value.empty());
    evaluate(request, true);
  }
  Check(exhausted && injected_failures > 0);
  for (bool source_side : {true, false}) {
    for (unsigned mutation = 0; mutation < 8; ++mutation) {
      auto bad = request;
      auto& descriptor = source_side ? bad.precomputed_value->descriptor : bad.result_descriptor;
      if (mutation == 0) ++descriptor.datatype_cohort.catalog_generation;
      if (mutation == 1) ++descriptor.datatype_descriptor_generation;
      if (mutation == 2) descriptor.type_uuid.bytes[0] ^= 1;
      if (mutation == 3) descriptor.charset_uuid = scratchbird::tests::FixtureUuid(1086, 1022);
      if (mutation == 4) descriptor.collation_uuid = scratchbird::tests::FixtureUuid(1086, 1022);
      if (mutation == 5) descriptor.descriptor_uuid = {};
      if (mutation >= 6) {
        a::CatalogColumnMetadata fields;
        Check(a::AdmitCatalogColumnMetadata(descriptor.encoded_descriptor, &fields));
        if (mutation == 6) fields.text["unknown_modifier"] = "1";
        if (mutation == 7) fields.identities["unknown_uuid"] = scratchbird::tests::FixtureUuid(1086, 1022);
        Check(a::EncodeCatalogColumnMetadata(fields, &descriptor.encoded_descriptor));
      }
      evaluate(bad, false);
      evaluate(request, true);
    }
  }
  for (unsigned mutation = 0; mutation < 5; ++mutation) {
    auto bad = request;
    auto& value = *bad.precomputed_value;
    if (mutation == 0) { value.setState(a::EngineValueState::sql_null); value.binary_value = {0}; }
    if (mutation == 1) { value.setState(a::EngineValueState::value); value.is_null = true; }
    if (mutation == 2) value.state = a::EngineValueState::missing;
    if (mutation == 3) { value.setState(a::EngineValueState::value); value.encoded_value = "invalid"; }
    if (mutation == 4) { value.setState(a::EngineValueState::value); value.binary_value.assign(3, 0); }
    evaluate(bad, false);
  }
  for (bool source_nullable : {false, true}) for (bool target_nullable : {false, true}) {
    auto candidate = request;
    for (bool source_side : {true, false}) {
      auto& descriptor = source_side ? candidate.precomputed_value->descriptor : candidate.result_descriptor;
      a::CatalogColumnMetadata fields;
      Check(a::AdmitCatalogColumnMetadata(descriptor.encoded_descriptor, &fields));
      fields.text.erase("nullable");
      fields.text["nullability"] = (source_side ? source_nullable : target_nullable) ? "nullable" : "non_null";
      Check(a::EncodeCatalogColumnMetadata(fields, &descriptor.encoded_descriptor));
    }
    if (!input.is_null) evaluate(candidate, true);
    candidate.precomputed_value->setState(a::EngineValueState::sql_null);
    candidate.precomputed_value->encoded_value.clear(); candidate.precomputed_value->binary_value.clear();
    evaluate(candidate, source_nullable && target_nullable);
  }
  if (type == d::CanonicalTypeId::decimal) {
    for (bool source_side : {true, false}) for (const auto key : {"precision", "scale", "decimal_codec_generation"}) {
      auto bad = request;
      auto& descriptor = source_side ? bad.precomputed_value->descriptor : bad.result_descriptor;
      a::CatalogColumnMetadata fields;
      Check(a::AdmitCatalogColumnMetadata(descriptor.encoded_descriptor, &fields));
      fields.text.at(key) = std::to_string(std::stoul(fields.text.at(key)) + 1);
      Check(a::EncodeCatalogColumnMetadata(fields, &descriptor.encoded_descriptor));
      evaluate(bad, false);
    }
  }
  if (type == d::CanonicalTypeId::boolean) {
    auto boolean = request;
    boolean.precomputed_value->setState(a::EngineValueState::value);
    boolean.precomputed_value->binary_value.clear();
    for (const auto spelling : {"true", "false", "TRUE", "FALSE", "0", "1"}) {
      boolean.precomputed_value->encoded_value = spelling;
      evaluate(boolean, false);
    }
    boolean.precomputed_value->encoded_value.clear();
    for (const auto byte : {0u, 1u, 2u, 255u}) {
      boolean.precomputed_value->binary_value = {static_cast<std::uint8_t>(byte)};
      evaluate(boolean, byte <= 1);
    }
  }
  if (!input.is_null && type != d::CanonicalTypeId::decimal && type != d::CanonicalTypeId::boolean) {
    auto bits = request;
    for (const auto byte : {0x80, 0xff}) {
      std::fill(bits.precomputed_value->binary_value.begin(), bits.precomputed_value->binary_value.end(), byte);
      evaluate(bits, true); // Signed extremes and REAL64 NaN payload stay bit-exact.
    }
    if (type == d::CanonicalTypeId::real64) {
      bits.precomputed_value->binary_value = {0,0,0,0,0,0,0,0x80};
      evaluate(bits, true); // Negative zero.
    }
  }
}

// Function callbacks here are counted component producers, not live registry
// authority. Descriptor construction must not execute them to learn metadata.
void CheckProjectDescriptorInference(a::RelationalTypeDescriptor source_descriptor,
                                     const a::EngineTypedValue& input_value) {
  CheckFunctionResultAdmission(input_value);
  namespace planner = scratchbird::engine::planner;
  source_descriptor.descriptor_id = 1;
  auto output_descriptor = source_descriptor;
  output_descriptor.descriptor_id = 2;
  output_descriptor.descriptor_uuid = scratchbird::tests::FixtureUuid(1086, 1010);
  a::EngineDescriptor expected;
  Check(s::BuildExactCanonicalScalarRuntimeDescriptorV1(output_descriptor,
      d::CanonicalTypeIdFromStableName(input_value.descriptor.canonical_type_name), &expected));
  a::TypedRelationalDag dag;
  dag.descriptors = {source_descriptor, output_descriptor};
  a::RelationalExpressionRecord leaf, function;
  leaf.expression_id = 1;
  leaf.expression_kind = a::RelationalExpressionKind::kIdentifier;
  leaf.result_descriptor_id = 1;
  leaf.bound_name_uuid = scratchbird::tests::FixtureUuid(1086, 1011);
  function.expression_id = 2;
  function.expression_kind = a::RelationalExpressionKind::kParenthesized;
  function.result_descriptor_id = 2;
  function.child_expression_ids = {1};
  dag.expressions = {leaf, function};
  dag.outputs = {{1, 2, 2, "result", 2, true, 0}};
  planner::CanonicalLogicalRelationalNode root, source;
  root.logical_node_id = 2;
  root.output_descriptor_ids = {2};
  root.bound_expression_ids = {2};
  source.logical_node_id = 1;
  source.output_descriptor_ids = {1};
  s::MaterializedValues input;
  input.ok = true;
  input.batch.columns.push_back({"input", input_value.descriptor,
      source_descriptor.nullability == a::RelationalNullability::kNullable, 1});
  input.result_bindings.resize(1);
  unsigned calls = 0;
  s::CanonicalRelationalExpressionRuntimeServices services;
  services.function_evaluator = [&](const a::EngineUuid& identity,
      const std::vector<a::EngineTypedValue>& arguments, const a::EngineDescriptor& target,
      a::EngineTypedValue* value, std::string*, std::string*) {
    ++calls;
    Check(identity == scratchbird::tests::FixtureUuid(1086, 1012) && arguments.empty() && target == expected);
    *value = input_value;
    value->descriptor = target;
    return true;
  };
  for (const unsigned rows : {0, 1, 3}) {
    input.batch.rows.assign(rows, {{input_value}});
    calls = 0;
    const auto prepared = s::PrepareExpressionProjectRootForComposition(dag, root, source, input, services);
    if (!prepared.ok) std::cerr << prepared.detail << '\n';
    Check(prepared.ok && calls == 0 && prepared.expression_output_batch.rows.size() == rows);
    Check(prepared.expression_output_batch.columns.front().descriptor == expected);
    for (const auto& row : prepared.expression_output_batch.rows) {
      const auto& value = row.values.front();
      Check(value.descriptor == expected && value.binary_value == input_value.binary_value &&
            value.encoded_value == input_value.encoded_value && value.state == input_value.state);
    }
    s::CanonicalRelationalExpressionRuntime runtime(dag, services);
    auto output = expected;
    Check(runtime.DescribeBoundDescriptor(2, expected.canonical_type_name, &output, &input.detail));
    Check(output == expected && calls == 0);
    auto invalid = dag;
    ++invalid.descriptors.back().codec_generation;
    s::CanonicalRelationalExpressionRuntime stale(invalid, services);
    Check(!stale.DescribeBoundDescriptor(2, expected.canonical_type_name, &output, &input.detail));
    Check(output == expected && calls == 0); // Atomic refusal, no execution.
    Check(!s::PrepareExpressionProjectRootForComposition(invalid, root, source, input, services).ok);
    Check(calls == 0);
  }
  if (expected.canonical_type_name == "int64") {
    s::CanonicalRelationalExpressionRowBinding binding;
    binding.row_descriptor_ids = {1};
    binding.slots.push_back({1, 1, 0, s::CanonicalRelationalExpressionRowSlotKind::input_identifier});
    const std::vector<a::EngineTypedValue> row{input_value};
    auto object_free = dag;
    auto& literal = object_free.expressions.front();
    literal.expression_kind = a::RelationalExpressionKind::kLiteral;
    literal.bound_name_uuid.reset();
    literal.literal_kind = a::RelationalLiteralKind::kNumeric;
    literal.literal_typed_value_v1.emplace();
    auto& carrier = *literal.literal_typed_value_v1;
    carrier.descriptor_uuid = source_descriptor.descriptor_uuid;
    carrier.descriptor_generation = source_descriptor.descriptor_generation;
    carrier.value_state = "value";
    carrier.canonical_value_bytes = input_value.binary_value;
    carrier.canonical_value_sha256 = scratchbird::core::hash::ComputeSha256Digest(carrier.canonical_value_bytes).digest;
    for (unsigned operation = 0; operation < 4; ++operation) {
      bool exhausted = false;
      unsigned faults = 0;
      for (long budget = 0; budget < 2048; ++budget) {
        // Fresh runtime before each injection; a warmed graph can hide the
        // allocation that occurs between installing and releasing row state.
        s::CanonicalRelationalExpressionRuntime candidate(operation == 3 ? object_free : dag, services);
        a::EngineTypedValue value;
        a::EngineDescriptor descriptor = expected;
        std::string type, detail;
        bool accepted = false;
        descriptor_allocation_budget = budget;
        try {
          if (operation == 0) accepted = candidate.InferTypeForConsumer(2, binding, row,
              a::EngineCanonicalExpressionConsumer::projection, &type, &detail);
          if (operation == 1) accepted = candidate.EvaluateForConsumer(2, "int64", binding, row,
              a::EngineCanonicalExpressionConsumer::projection, &value, &detail);
          if (operation == 2) accepted = candidate.DescribeBoundDescriptor(2, "int64", &descriptor, &detail);
          if (operation == 3) accepted = candidate.InferType(2, std::nullopt, &type, &detail);
        } catch (const std::bad_alloc&) {}
        const bool injected = descriptor_allocation_budget == -1;
        descriptor_allocation_budget = -1;
        if (!injected) { Check(accepted); exhausted = true; break; }
        ++faults;
        Check(!accepted);
        if (operation == 2) Check(descriptor == expected);
        if (operation == 3) {
          Check(candidate.InferType(2, std::nullopt, &type, &detail) && type == "int64");
          Check(candidate.Evaluate(2, type, &value, &detail));
          Check(value.descriptor == expected && value.binary_value == input_value.binary_value);
          continue;
        }
        const bool retry = candidate.InferTypeForConsumer(2, binding, row,
            a::EngineCanonicalExpressionConsumer::projection, &type, &detail);
        if (!retry) std::cerr << "exception retry operation=" << operation << " budget=" << budget << ':' << detail << '\n';
        Check(retry && type == "int64");
        Check(candidate.EvaluateForConsumer(2, "int64", binding, row,
            a::EngineCanonicalExpressionConsumer::projection, &value, &detail));
        Check(value.descriptor == expected && value.binary_value == input_value.binary_value);
      }
      Check(exhausted && faults > 0);
    }
  }
  // The existing object-free function runtime supplies a real callback-count
  // oracle. It is not the independently restricted row-function profile.
  auto& callable = dag.expressions.back();
  callable.expression_kind = a::RelationalExpressionKind::kFunctionCall;
  callable.function_uuid = scratchbird::tests::FixtureUuid(1086, 1012);
  callable.child_expression_ids.clear();
  s::CanonicalRelationalExpressionRuntime runtime(dag, services);
  std::string type;
  Check(runtime.InferType(2, std::nullopt, &type, &input.detail));
  a::EngineDescriptor described;
  Check(runtime.DescribeBoundDescriptor(2, type, &described, &input.detail));
  Check(calls == 0 && described == expected);
  a::EngineTypedValue evaluated;
  const bool evaluated_ok = runtime.Evaluate(2, type, &evaluated, &input.detail);
  if (!evaluated_ok) std::cerr << type << ":" << input.detail << '\n';
  Check(evaluated_ok);
  Check(calls == 1 && evaluated.descriptor == described &&
        evaluated.binary_value == input_value.binary_value &&
        evaluated.encoded_value == input_value.encoded_value &&
        evaluated.state == input_value.state);
  // A callable service does not admit the separately bounded row-function
  // profile. Retain that refusal rather than treating metadata as authority.
  Check(!s::PrepareExpressionProjectRootForComposition(dag, root, source, input, services).ok);
  Check(calls == 1);
  // VALUES uses the same exact descriptor builder, without substituting an
  // incomplete column descriptor for REAL64/DECIMAL/BOOLEAN/smaller integers.
  a::RelationalDagNode node;
  node.node_id = 2;
  node.node_kind = a::RelationalDagNodeKind::kValues;
  node.output_descriptor_ids = {2};
  node.values_row_ids = {1, 2, 3};
  dag.nodes = {node};
  dag.values_rows = {{1, {2}}, {2, {2}}, {3, {2}}};
  calls = 0;
  const auto values = s::MaterializeValues(dag, root, services);
  if (!values.ok) std::cerr << values.detail << '\n';
  Check(values.ok && calls == 3 && values.batch.rows.size() == 3 &&
        values.batch.columns.front().descriptor == described);
  for (const auto& row : values.batch.rows) {
    const auto& value = row.values.front();
    Check(value.descriptor == described && value.binary_value == input_value.binary_value &&
          value.encoded_value == input_value.encoded_value && value.state == input_value.state);
  }
  ++dag.descriptors.back().codec_generation;
  const auto refused = s::MaterializeValues(dag, root, services);
  Check(!refused.ok && refused.batch.columns.empty() && refused.batch.rows.empty() && calls == 3);
  --dag.descriptors.back().codec_generation;
  bool throw_once = true;
  auto throwing = services;
  throwing.function_evaluator = [&](const auto& id, const auto& arguments,
      const auto& target, auto* value, auto* diagnostic, auto* detail) {
    if (throw_once) { throw_once = false; throw std::runtime_error("component callback failure"); }
    return services.function_evaluator(id, arguments, target, value, diagnostic, detail);
  };
  s::CanonicalRelationalExpressionRuntime retry_runtime(dag, throwing);
  bool observed = false;
  try { (void)retry_runtime.Evaluate(2, type, &evaluated, &input.detail); }
  catch (const std::runtime_error&) { observed = true; }
  Check(observed && evaluated.state == a::EngineValueState::error &&
        evaluated.binary_value.empty() && evaluated.encoded_value.empty());
  Check(retry_runtime.Evaluate(2, type, &evaluated, &input.detail) && calls == 4);
  Check(evaluated.descriptor == described && evaluated.binary_value == input_value.binary_value &&
        evaluated.encoded_value == input_value.encoded_value && evaluated.state == input_value.state);
  Check(s::CanonicalFunctionResultStateValidV1(input_value));
  for (unsigned mutation = 0; mutation < 11; ++mutation) {
    auto malformed = input_value;
    malformed.descriptor = expected;
    malformed.binary_value.clear(); malformed.encoded_value.clear();
    if (mutation == 0) { malformed.state = a::EngineValueState::sql_null; malformed.is_null = false; }
    if (mutation == 1) { malformed.state = a::EngineValueState::value; malformed.is_null = true; }
    if (mutation == 2) { malformed.setState(a::EngineValueState::sql_null); malformed.binary_value = {0}; }
    if (mutation >= 3 && mutation < 10) {
      malformed.state = static_cast<a::EngineValueState>(mutation == 9 ? 255 : mutation - 1);
      malformed.is_null = false;
    }
    if (mutation == 10) { malformed.setState(a::EngineValueState::sql_null); malformed.encoded_value = "NULL"; }
    Check(!s::CanonicalFunctionResultStateValidV1(malformed));
    auto malformed_services = services;
    malformed_services.function_evaluator = [&](const auto&, const auto&, const auto&,
        auto* value, auto*, auto*) { *value = malformed; return true; };
    s::CanonicalRelationalExpressionRuntime malformed_runtime(dag, malformed_services);
    Check(!malformed_runtime.Evaluate(2, type, &evaluated, &input.detail));
    Check(input.detail.starts_with("QOW-DIAG-RCP024-FUNCTION-RESULT-REFUSAL-V1:"));
    Check(evaluated.state == a::EngineValueState::error && evaluated.binary_value.empty() && evaluated.encoded_value.empty());
  }
}

void CheckExplicitDecimalProfiles() {
  namespace ex = scratchbird::engine::executor;
  unsigned cohorts = 0;
  for (const auto& identity : d::CurrentDatatypeTypeCodecIdentityRowsV3()) {
    const auto& row = identity.legacy_fields;
    if ((row.catalog_generation != 5 && row.catalog_generation != 10 && row.catalog_generation != 11) ||
        row.canonical_binary_type_code != static_cast<std::uint32_t>(d::CanonicalTypeId::decimal)) continue;
    ++cohorts;
    a::RelationalTypeDescriptor source;
    source.descriptor_id = 1;
    source.datatype_identity_authoritative = true;
    source.descriptor_uuid = scratchbird::tests::FixtureUuid(1086, 1003);
    source.descriptor_generation = row.descriptor_generation;
    source.type_uuid = row.type_uuid;
    source.type_generation = row.type_generation;
    source.codec_id = row.codec_id;
    source.codec_version = row.codec_version;
    source.codec_generation = row.codec_generation;
    source.datatype_catalog_snapshot_uuid = row.catalog_snapshot_uuid;
    source.datatype_catalog_generation = row.catalog_generation;
    source.datatype_registry_generation = row.registry_generation;
    source.statement_receipt_uuid = scratchbird::tests::FixtureUuid(1086, 1004);
    // Independent Core profile identity, not the frozen base-row codec UUID.
    const a::EngineUuid codec_uuid{{0x01,0xa1,0x18,0x6b,0xdf,0x04,0x76,0x0a,
                                  0xa3,0xd5,0xd0,0x2d,0x2e,0xe3,0x01,0x54}};
    source.decimal_codec_profile = a::RelationalTypeDescriptor::DecimalCodecProfile{codec_uuid, 1};
    for (const auto nullable : {a::RelationalNullability::kNonNull, a::RelationalNullability::kNullable}) {
      source.nullability = nullable;
      for (unsigned precision = 1; precision <= 38; ++precision) {
        source.precision = precision;
        for (unsigned scale = 0; scale <= precision; ++scale) {
          source.scale = scale;
          a::EngineDescriptor projected;
          Check(s::BuildExactCanonicalScalarRuntimeDescriptorV1(source, d::CanonicalTypeId::decimal, &projected));
          a::CatalogColumnMetadata fields;
          Check(a::DecodeCatalogColumnMetadata(projected.encoded_descriptor, &fields));
          Check(fields.identities.at("decimal_codec_uuid") == codec_uuid &&
                fields.text.at("decimal_codec_generation") == "1");
          scratchbird::engine::ExecutionTypeDescriptor execution;
          d::DatatypeDecimalCodecBindingV1 codec;
          Check(ex::BuildBoundDecimalExecutionTypeDescriptor(projected, &execution, &codec));
          Check(codec.codec_uuid == codec_uuid && execution.precision == precision && execution.scale == scale);
        }
      }
      source.precision = 18; source.scale = 0;
      a::EngineDescriptor target;
      Check(s::BuildExactCanonicalScalarRuntimeDescriptorV1(source, d::CanonicalTypeId::decimal, &target));
      {
        auto stored = target;
        const auto column = scratchbird::tests::FixtureUuid(1086, 1005);
        a::CatalogColumnMetadata fields;
        Check(a::DecodeCatalogColumnMetadata(stored.encoded_descriptor, &fields));
        fields.identities.emplace("column_uuid", column);
        fields.identities.emplace("type_uuid", row.type_uuid);
        fields.identities.emplace("datatype_descriptor_uuid", row.descriptor_uuid);
        if (!row.codec_uuid.is_nil()) fields.identities.emplace("codec_uuid", row.codec_uuid);
        Check(a::EncodeCatalogColumnMetadata(fields, &stored.encoded_descriptor));
        Check(s::SamePersistedRowDescriptor(1, source, stored, std::nullopt, nullptr, nullptr, column));
        s::CanonicalRelationalExpressionRuntimeServices services;
        unsigned calls = 0;
        services.prevalidated_persisted_row_descriptor_authority = [&](auto, const auto&, const auto&, auto) {
          ++calls;
          return std::optional<bool>{true};
        };
        Check(s::SamePersistedRowDescriptor(1, source, stored, std::nullopt, &services, nullptr, column));
        Check(calls == 1);
        fields.identities.at("decimal_codec_uuid").bytes[0] ^= 1;
        Check(a::EncodeCatalogColumnMetadata(fields, &stored.encoded_descriptor));
        Check(!s::SamePersistedRowDescriptor(1, source, stored, std::nullopt, &services, nullptr, column));
        Check(calls == 1); // A prevalidated hook cannot mask a crossed profile.
      }
      auto real = ex::MakeExecutorDescriptor("real64", nullable == a::RelationalNullability::kNullable
          ? "nullability=nullable" : "nullability=non_null",
          {row.catalog_snapshot_uuid, row.catalog_generation, row.registry_generation});
      a::EngineTypedValue value;
      value.descriptor = real;
      value.binary_value = {0,0,0,0,0,0,4,0x40}; // Exact IEEE754 2.5, LE8.
      ex::DescriptorRuntimeDiagnostic diagnostic;
      const auto cast = ex::CastDescriptorValue(value, target, &diagnostic);
      std::vector<std::uint8_t> expected(24, 0);
      expected[1] = 1; expected[2] = 1; expected[4] = 2; // Half-even -> 2.
      Check(diagnostic.ok && diagnostic.numeric_facts.inexact && cast.binary_value == expected &&
            cast.encoded_value.empty() && cast.descriptor == target);
      CheckProjectDescriptorInference(source, cast);
      if (nullable == a::RelationalNullability::kNullable) {
        value.binary_value.clear(); value.setState(a::EngineValueState::sql_null);
        const auto null = ex::CastDescriptorValue(value, target, &diagnostic);
        Check(diagnostic.ok && null.is_null && null.binary_value.empty() && null.descriptor == target);
        CheckProjectDescriptorInference(source, null);
      }
      auto missing = source; missing.decimal_codec_profile.reset();
      Check(s::BuildExactCanonicalScalarRuntimeDescriptorV1(missing, d::CanonicalTypeId::decimal, &target));
      const auto refused = ex::CastDescriptorValue(value, target, &diagnostic);
      Check(!diagnostic.ok && diagnostic.diagnostic_code == "DATATYPE.DESCRIPTOR.INVALID" && refused.binary_value.empty());
    }
    for (unsigned bit = 0; bit < 128; ++bit) {
      auto bad = source;
      bad.decimal_codec_profile->uuid.bytes[bit / 8] ^= 1u << (bit % 8);
      a::EngineDescriptor output;
      Check(!s::BuildExactCanonicalScalarRuntimeDescriptorV1(bad, d::CanonicalTypeId::decimal, &output));
    }
    for (unsigned mutation = 0; mutation < 8; ++mutation) {
      auto bad = source;
      if (mutation == 0) ++bad.decimal_codec_profile->generation;
      if (mutation == 1) bad.precision.reset();
      if (mutation == 2) bad.scale.reset();
      if (mutation == 3) bad.precision = 0;
      if (mutation == 4) bad.scale = 19;
      if (mutation == 5) ++bad.datatype_registry_generation;
      if (mutation == 6) ++bad.codec_generation;
      if (mutation == 7) bad.statement_receipt_uuid = {};
      a::EngineDescriptor output;
      Check(!s::BuildExactCanonicalScalarRuntimeDescriptorV1(bad, d::CanonicalTypeId::decimal, &output));
    }
  }
  Check(cohorts == 3);
}

void CheckDecimalBatchAdmission() {
  namespace ex = scratchbird::engine::executor;
  namespace num = scratchbird::libraries::sbl_numeric;
  for (const unsigned precision : {18u, 50u}) {
    const auto profile = num::ExactDecimalProfile{precision <= 38
        ? num::ExactDecimalCodec::le24_v1 : num::ExactDecimalCodec::le40_v1, precision, 2};
    auto descriptor = ex::MakeExecutorDescriptor("decimal", "nullability=nullable;precision=" +
        std::to_string(precision) + ";scale=2", {d::kDatatypeCohortV11, 11, 11});
    descriptor.descriptor_kind = "scalar";
    scratchbird::engine::ExecutionTypeDescriptor execution;
    std::string detail;
    Check(ex::BuildBoundExecutionTypeDescriptor(descriptor, d::CanonicalTypeId::decimal, &execution, &detail));
    d::DatatypeSortKeyRequest binding;
    Check(d::BindExactDecimalSortKeyProfile(execution, &binding));
    a::CatalogColumnMetadata fields;
    Check(a::AdmitCatalogColumnMetadata(descriptor.encoded_descriptor, &fields));
    fields.identities["decimal_codec_uuid"] = binding.decimal_codec_uuid;
    fields.text["decimal_codec_generation"] = "1";
    Check(a::EncodeCatalogColumnMetadata(fields, &descriptor.encoded_descriptor));
    a::EngineTypedValue present;
    present.descriptor = descriptor;
    const auto encoded = num::EncodeBoundExactDecimal("-12.34", profile);
    Check(encoded.ok());
    present.binary_value = encoded.bytes;
    CheckFunctionResultAdmission(present);
    a::EngineTypedValue null;
    null.descriptor = descriptor;
    null.setState(a::EngineValueState::sql_null);
    const auto check = [&](const ex::DescriptorBatch& batch, bool accepted) {
      const auto before = batch;
      const auto ordinary = ex::ValidateDescriptorBatch(batch);
      const auto canonical = ex::ValidateCanonicalDescriptorBatch(batch, {1});
      Check(ordinary.ok == accepted && canonical.ok == accepted);
      Check(s::CanonicalQueryDescriptorBatchesExactlyEqual(batch, before));
    };
    for (unsigned shape = 0; shape < 3; ++shape) {
      ex::DescriptorBatch batch;
      batch.columns.push_back({"decimal", descriptor, true, 1});
      if (shape != 0) batch.rows.push_back({{shape == 1 ? present : null}});
      check(batch, true);
      for (bool canonical : {false, true}) {
        bool exhausted = false;
        unsigned injected_failures = 0;
        for (long budget = 0; budget < 2048; ++budget) {
          const auto before = batch;
          const std::vector<std::uint32_t> ids{1};
          bool accepted = false;
          descriptor_allocation_budget = budget;
          try {
            accepted = canonical ? ex::ValidateCanonicalDescriptorBatch(batch, ids).ok
                                 : ex::ValidateDescriptorBatch(batch).ok;
          } catch (const std::bad_alloc&) {}
          const bool injected = descriptor_allocation_budget == -1;
          descriptor_allocation_budget = -1;
          Check(s::CanonicalQueryDescriptorBatchesExactlyEqual(batch, before));
          if (!injected) { Check(accepted); exhausted = true; break; }
          ++injected_failures;
          Check(!accepted);
          check(batch, true);
        }
        Check(exhausted && injected_failures > 0);
      }
      for (unsigned mutation = 0; mutation < 8; ++mutation) {
        auto bad = batch;
        auto altered = fields;
        if (mutation == 0) altered.identities.clear();
        if (mutation == 1) altered.text["decimal_codec_generation"] = "2";
        if (mutation == 2) altered.identities["decimal_codec_uuid"].bytes[0] ^= 1;
        if (mutation == 3) altered.text["scale"] = std::to_string(precision + 1);
        if (mutation == 4) altered.text["unknown_modifier"] = "1";
        if (mutation == 5) {
          altered.identities["codec_uuid"] = binding.decimal_codec_uuid;
          altered.identities["codec_uuid"].bytes[0] ^= 1;
          altered.text["codec_generation"] = "1";
        }
        auto& changed = bad.columns.front().descriptor;
        Check(a::EncodeCatalogColumnMetadata(altered, &changed.encoded_descriptor));
        if (mutation == 6) ++changed.datatype_cohort.catalog_generation;
        if (mutation == 7) bad.columns.front().nullable = false;
        if (!bad.rows.empty()) bad.rows.front().values.front().descriptor = changed;
        check(bad, false);
        check(batch, true);
      }
    }
    for (unsigned mutation = 0; mutation < 9; ++mutation) {
      ex::DescriptorBatch bad;
      bad.columns.push_back({"decimal", descriptor, true, 1});
      bad.rows.push_back({{present}});
      auto& value = bad.rows.front().values.front();
      if (mutation == 0) value.binary_value.pop_back();
      if (mutation == 1) value.binary_value.push_back(0);
      if (mutation == 2) value.encoded_value = "-12.34";
      if (mutation == 3) { value.binary_value.clear(); value.encoded_value = "-12.34"; }
      if (mutation == 4) value.binary_value[0] = 0xff;
      if (mutation == 5) value.setState(a::EngineValueState::sql_null); // Dirty NULL.
      if (mutation == 6) value.is_null = true;
      if (mutation == 7) value.state = a::EngineValueState::missing;
      if (mutation == 8) value.descriptor.descriptor_uuid.bytes[0] ^= 1;
      check(bad, false);
    }
  }
}

void CheckPreparedAggregateCohortCustody() {
  namespace ex = scratchbird::engine::executor;
  for (const auto cohort : {a::EngineDatatypeCohort{d::kDatatypeCohortV5, 5, 5},
                            a::EngineDatatypeCohort{d::kDatatypeCohortV11, 11, 11}}) {
    auto descriptor = ex::MakeExecutorDescriptor("int64", "nullability=non_null", cohort);
    descriptor.descriptor_kind = "scalar";
    descriptor.descriptor_uuid = scratchbird::tests::FixtureUuid(1086, 1002);
    s::PreparedAggregateValueBindingReceipt receipt{0, 1, descriptor.descriptor_uuid,
        descriptor.type_uuid, false, descriptor.canonical_type_name, descriptor.encoded_descriptor,
        cohort, descriptor.datatype_descriptor_uuid, descriptor.datatype_descriptor_generation};
    auto value = ex::EncodeInt64Value(5);
    value.descriptor = descriptor;
    for (bool empty : {false, true}) {
      ex::DescriptorBatch batch;
      batch.columns.push_back({"value", descriptor, false, 1});
      if (!empty) batch.rows.push_back({{value}});
      for (unsigned mutation = 0; mutation < 5; ++mutation) {
        auto candidate = batch;
        auto& binding = candidate.columns.front().descriptor;
        if (mutation == 1) binding.datatype_cohort = cohort.catalog_generation == 5
            ? a::EngineDatatypeCohort{d::kDatatypeCohortV11, 11, 11}
            : a::EngineDatatypeCohort{d::kDatatypeCohortV5, 5, 5};
        if (mutation == 2) binding.datatype_cohort = {};
        if (mutation == 3) binding.datatype_descriptor_uuid = {};
        if (mutation == 4) ++binding.datatype_descriptor_generation;
        for (auto& row : candidate.rows) row.values.front().descriptor = binding;
        std::string detail;
        Check(s::RevalidatePreparedAggregateValueBindings({0}, {1}, {receipt}, candidate, &detail) == (mutation == 0));
        Check(detail.empty() == (mutation == 0));
        s::PreparedGroupedCountSumRoot grouped;
        grouped.key_binding_receipts = {receipt};
        scratchbird::engine::executor::CanonicalDescriptorOrderTerm term;
        term.column = 0;
        term.expression_descriptor_id = 1;
        grouped.key_terms = {term};
        Check(s::RevalidatePreparedGroupedKeyBindings(grouped, candidate, &detail) == (mutation == 0));
        Check(detail.empty() == (mutation == 0));
      }
    }
    const auto manifest = d::LoadCurrentCoreDatatypeCatalogManifest();
    Check(manifest.ok());
    const auto storage = std::ranges::find_if(manifest.manifest.descriptor_rows,
        [](const auto& row) { return row.type_id == d::CanonicalTypeId::document; });
    Check(storage != manifest.manifest.descriptor_rows.end());
    Check(d::FindDatatypeTypeCodecIdentityV3(cohort.catalog_snapshot_uuid, cohort.catalog_generation,
        cohort.registry_generation, storage->descriptor_uuid.value, storage->descriptor_epoch) == nullptr);
    Check(s::ExactCanonicalCoreDatatypeTypeUuidV1(storage->stable_name, cohort) == storage->descriptor_uuid.value);
    auto unknown = cohort;
    ++unknown.registry_generation;
    Check(s::ExactCanonicalCoreDatatypeTypeUuidV1(storage->stable_name, unknown).is_nil());
  }
}

void CheckGroupedPreparationCohortCustody() {
  namespace ex = scratchbird::engine::executor;
  for (const auto cohort : {a::EngineDatatypeCohort{d::kDatatypeCohortV5, 5, 5},
                            a::EngineDatatypeCohort{d::kDatatypeCohortV10, 10, 10},
                            a::EngineDatatypeCohort{d::kDatatypeCohortV11, 11, 11}}) {
    a::TypedRelationalDag dag;
    for (unsigned id = 1; id <= 4; ++id) {
      auto descriptor = scratchbird::tests::Int64LiteralDescriptor(id, cohort);
      descriptor.descriptor_uuid = scratchbird::tests::FixtureUuid(1086, 1010 + id);
      if (id == 4) descriptor.nullability = a::RelationalNullability::kNullable;
      dag.descriptors.push_back(descriptor);
      a::RelationalExpressionRecord expression;
      expression.expression_id = id;
      expression.result_descriptor_id = id;
      if (id <= 2) {
        expression.expression_kind = a::RelationalExpressionKind::kIdentifier;
        expression.bound_name_uuid = scratchbird::tests::FixtureUuid(1086, 1020 + id);
      } else {
        expression.expression_kind = a::RelationalExpressionKind::kFunctionCall;
        expression.function_uuid = ex::LookupCanonicalAggregateByFunctionV1(id == 3
            ? ex::CanonicalAggregateFunction::count : ex::CanonicalAggregateFunction::sum)->function_uuid;
        if (id == 4) expression.child_expression_ids = {2};
      }
      dag.expressions.push_back(expression);
    }
    dag.outputs = {{1, 1, 1, "key", 1, true, 0}, {2, 1, 2, "value", 2, true, 1},
        {3, 2, 1, "key", 1, true, 0}, {4, 2, 3, "count", 3, true, 1},
        {5, 2, 4, "sum", 4, true, 2}};
    scratchbird::engine::planner::CanonicalLogicalRelationalNode root, source;
    source.logical_node_id = 1;
    source.output_descriptor_ids = {1, 2};
    root.logical_node_id = 2;
    root.output_descriptor_ids = root.bound_expression_ids = {1, 3, 4};
    const auto profile = s::MatchLiveGroupedCountSumProfileForComposition("aggregate.grouped-int64-key-count-sum.v1");
    for (bool populated : {false, true}) {
      s::MaterializedValues input;
      input.ok = true;
      input.result_bindings.resize(2);
      ex::DescriptorTuple tuple;
      for (unsigned index = 0; index < 2; ++index) {
        a::EngineDescriptor descriptor;
        Check(s::BuildExactCanonicalScalarRuntimeDescriptorV1(dag.descriptors[index], d::CanonicalTypeId::int64, &descriptor));
        input.batch.columns.push_back({index == 0 ? "key" : "value", descriptor, false, index + 1});
        auto value = ex::EncodeInt64Value(5);
        value.descriptor = descriptor;
        tuple.values.push_back(value);
      }
      if (populated) input.batch.rows.push_back(tuple);
      const auto valid = s::PrepareGroupedCountSumRootForComposition(dag, root, source, input, profile);
      if (!valid.ok) std::cerr << "grouped fixture: " << valid.detail << '\n';
      Check(valid.ok && valid.key_binding_receipts.size() == 1);
      Check(valid.key_binding_receipts.front().datatype_cohort == cohort);
      std::string detail;
      Check(s::RevalidatePreparedGroupedKeyBindings(valid, input.batch, &detail));
      for (unsigned mutation = 0; mutation < 4; ++mutation) {
        auto invalid = dag;
        auto& key = invalid.descriptors.front();
        if (mutation == 0) {
          key.datatype_catalog_snapshot_uuid = cohort.catalog_generation == 5 ? d::kDatatypeCohortV11 : d::kDatatypeCohortV5;
          key.datatype_catalog_generation = key.datatype_registry_generation = cohort.catalog_generation == 5 ? 11 : 5;
        }
        if (mutation == 1) ++key.codec_generation;
        if (mutation == 2) ++key.type_generation;
        if (mutation == 3) ++key.descriptor_generation;
        const auto refused = s::PrepareGroupedCountSumRootForComposition(invalid, root, source, input, profile);
        Check(!refused.ok && refused.key_binding_receipts.empty());
      }
    }
  }
}

int main() {
  CheckV3RelationalBindings();
  CheckExplicitDecimalProfiles();
  CheckDecimalBatchAdmission();
  CheckPreparedAggregateCohortCustody();
  CheckGroupedPreparationCohortCustody();
  unsigned qualified_producer_receipts = 0;
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
    auto identifier_dag = timestamp_dag;
    auto& identifier = identifier_dag.expressions.front();
    identifier.expression_kind = a::RelationalExpressionKind::kIdentifier;
    identifier.literal_kind.reset();
    identifier.literal_typed_value_v1.reset();
    identifier.bound_name_uuid = scratchbird::tests::FixtureUuid(1086, 903);
    s::CanonicalRelationalExpressionRuntime identifier_runtime(identifier_dag);
    s::CanonicalRelationalExpressionRowBinding binding;
    binding.row_descriptor_ids = {1};
    binding.slots.push_back({1, 1, 0, s::CanonicalRelationalExpressionRowSlotKind::input_identifier});
    a::EngineTypedValue selected;
    Check(identifier_runtime.EvaluateForConsumer(1, "timestamp", binding,
        std::vector<a::EngineTypedValue>{evaluated},
        a::EngineCanonicalExpressionConsumer::projection, &selected, &detail));
    Check(selected.descriptor == evaluated.descriptor && selected.binary_value == evaluated.binary_value &&
          selected.encoded_value == evaluated.encoded_value && selected.state == evaluated.state &&
          selected.is_null == evaluated.is_null);
    Check(!identifier_runtime.EvaluateForConsumer(1, "timestamp", binding,
        std::vector<a::EngineTypedValue>{crossed_statement},
        a::EngineCanonicalExpressionConsumer::projection, &selected, &detail));
    for (unsigned mutation = 0; mutation < 4; ++mutation) {
      auto stale = evaluated;
      if (mutation == 0) ++stale.descriptor.datatype_descriptor_generation;
      if (mutation == 1) stale.descriptor.type_uuid = identifier.bound_name_uuid.value();
      if (mutation == 2) stale.descriptor.encoded_descriptor += ";unbound=true";
      if (mutation == 3) stale.descriptor.descriptor_uuid = identifier.bound_name_uuid.value();
      Check(!identifier_runtime.EvaluateForConsumer(1, "timestamp", binding,
          std::vector<a::EngineTypedValue>{stale},
          a::EngineCanonicalExpressionConsumer::projection, &selected, &detail));
    }
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
      Check(!identifier_runtime.EvaluateForConsumer(1, "timestamp", binding,
          std::vector<a::EngineTypedValue>{bad},
          a::EngineCanonicalExpressionConsumer::projection, &selected, &detail));
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
    Check(canonical.size() == 1 && static_cast<unsigned char>(canonical[0]) ==
          (std::string_view(text) == "true" ? 1u : 0u));
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
                          d::CanonicalTypeId::boolean, d::CanonicalTypeId::int8,
                          d::CanonicalTypeId::int16, d::CanonicalTypeId::int32,
                          d::CanonicalTypeId::int64, d::CanonicalTypeId::uint64,
                          d::CanonicalTypeId::real64}) {
    const auto identity = std::ranges::find_if(rows, [&](const auto& item) {
      return item.catalog_generation == 5 && item.registry_generation == 5 &&
          item.canonical_binary_type_code == static_cast<std::uint32_t>(type);
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
    if (type == d::CanonicalTypeId::int8 || type == d::CanonicalTypeId::int16 ||
        type == d::CanonicalTypeId::int32 || type == d::CanonicalTypeId::int64 ||
        type == d::CanonicalTypeId::boolean) {
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
          return item.catalog_snapshot_uuid == descriptor.datatype_catalog_snapshot_uuid &&
              item.catalog_generation == descriptor.datatype_catalog_generation &&
              item.registry_generation == descriptor.datatype_registry_generation &&
              item.type_uuid == s::ExactCanonicalCoreDatatypeTypeUuidV1(result_name,
              a::EngineDatatypeCohort{descriptor.datatype_catalog_snapshot_uuid,
                  descriptor.datatype_catalog_generation, descriptor.datatype_registry_generation});
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
            return item.catalog_generation == 5 && item.registry_generation == 5 &&
                item.canonical_binary_type_code == static_cast<std::uint32_t>(d::CanonicalTypeId::decimal);
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
        auto populated_input = input;
        auto source_value = boolean ? exec::EncodeBoolValue(true) : exec::EncodeInt64Value(5);
        if (!boolean) source_value.binary_value.resize(identity->canonical_value_exact_bytes);
        source_value.descriptor = runtime_descriptor;
        populated_input.batch.rows.push_back({{source_value}});
        const auto populated_prepared = s::PrepareGlobalAggregateRootForComposition(
            aggregate_dag, aggregate_root, source, populated_input, function, false, false, false);
        if (!populated_prepared.ok) std::cerr << populated_prepared.detail << '\n';
        Check(populated_prepared.ok);
        for (unsigned mutation = 0; mutation < 5; ++mutation) {
          auto invalid_input_dag = aggregate_dag;
          auto& source_binding = invalid_input_dag.descriptors.front();
          if (mutation == 0) {
            source_binding.datatype_catalog_snapshot_uuid = d::kDatatypeCohortV11;
            source_binding.datatype_catalog_generation = source_binding.datatype_registry_generation = 11;
          }
          if (mutation == 1) ++source_binding.codec_generation;
          if (mutation == 2) ++source_binding.type_generation;
          if (mutation == 3) ++source_binding.descriptor_generation;
          if (mutation == 4) ++source_binding.codec_version;
          for (bool populated : {false, true}) {
            const auto& input_variant = populated ? populated_input : input;
            const auto invalid_prepared = s::PrepareGlobalAggregateRootForComposition(
                invalid_input_dag, aggregate_root, source, input_variant, function, false, false, false);
            Check(!invalid_prepared.ok && invalid_prepared.result_bindings.empty());
          }
        }
        if (!prepared.exact_value_binding_receipts.empty()) {
          qualified_producer_receipts += prepared.exact_value_binding_receipts.size();
          std::string receipt_detail;
          Check(s::RevalidatePreparedAggregateValueBindings(prepared.value_columns,
              prepared.value_descriptor_ids, prepared.exact_value_binding_receipts,
              input.batch, &receipt_detail));
          auto replaced = input.batch;
          for (const auto& receipt : prepared.exact_value_binding_receipts) {
            const auto& original = input.batch.columns[receipt.value_column].descriptor;
            Check(receipt.datatype_cohort == original.datatype_cohort &&
                  receipt.datatype_descriptor_uuid == original.datatype_descriptor_uuid &&
                  receipt.datatype_descriptor_generation == original.datatype_descriptor_generation);
            replaced.columns[receipt.value_column].descriptor.datatype_cohort =
                {d::kDatatypeCohortV11, 11, 11};
          }
          Check(!s::RevalidatePreparedAggregateValueBindings(prepared.value_columns,
              prepared.value_descriptor_ids, prepared.exact_value_binding_receipts,
              replaced, &receipt_detail));
        }
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
    if (type == d::CanonicalTypeId::boolean) value.binary_value = {1};
    else value.binary_value.resize(type == d::CanonicalTypeId::uuid ? 16 :
        type == d::CanonicalTypeId::int8 ? 1 : type == d::CanonicalTypeId::int16 ? 2 :
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
    CheckProjectDescriptorInference(descriptor, value);
    if (type != d::CanonicalTypeId::boolean) {
      auto malformed = input;
      malformed.batch.rows.front().values.front().encoded_value = "0";
      Check(!s::PrepareExpressionProjectRootForComposition(dag, root, source, malformed, {}).ok);
      if (type != d::CanonicalTypeId::binary) {
        malformed = input;
        malformed.batch.rows.front().values.front().binary_value.pop_back();
        Check(!s::PrepareExpressionProjectRootForComposition(dag, root, source, malformed, {}).ok);
        malformed.batch.rows.front().values.front().binary_value.clear();
        Check(!s::PrepareExpressionProjectRootForComposition(dag, root, source, malformed, {}).ok);
      }
    }
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
  Check(qualified_producer_receipts > 0);
}
