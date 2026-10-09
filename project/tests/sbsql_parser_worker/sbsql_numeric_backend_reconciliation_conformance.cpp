// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "../support/diagnostic_value_fixture.hpp"

#include "../support/binary_uuid_fixture.hpp"
#include "datatype_operations.hpp"
#include "catalog/datatype_bootstrap_identity.hpp"
#include "datatype_catalog_manifest.hpp"
#include "datatype_descriptor.hpp"
#include "query/expression_api.hpp"
#include "runtime_capabilities.hpp"
#include "sbl_numeric.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_opcode_registry.hpp"

#include <cstdint>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace dt = scratchbird::core::datatypes;
namespace numeric = scratchbird::libraries::sbl_numeric;
namespace platform = scratchbird::core::platform;
namespace sblr = scratchbird::engine::sblr;

constexpr std::string_view kInt128Max = "170141183460469231731687303715884105727";
constexpr std::string_view kInt128Min = "-170141183460469231731687303715884105728";
constexpr std::string_view kUint128Max = "340282366920938463463374607431768211455";
constexpr std::string_view kReferenceReal128Provider = "MPFR/GMP binary128 reference";

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

api::EngineRequestContext EngineContext() {
  api::EngineRequestContext context;
  context.request_id = "sbsql-numeric-backend-reconciliation";
  context.database_uuid = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000170001");
  context.session_uuid = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000170002");
  context.principal_uuid = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000170003");
  context.security_context_present = true;
  context.trace_tags.push_back("numeric_backend_reconciliation");
  return context;
}

scratchbird::engine::ExecutionTypeDescriptor ExecutionDescriptor(dt::CanonicalTypeId type) {
  const auto manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
  Require(manifest.ok(), "numeric fixture catalog unavailable");
  const auto row = dt::LookupDatatypeCatalogRow(manifest.manifest, type);
  Require(row.ok() && row.manifest.descriptor_rows.size() == 1,
          "numeric fixture descriptor missing or ambiguous");
  dt::CatalogExecutionTypeMetadata metadata;
  metadata.descriptor_uuid = row.manifest.descriptor_rows.front().descriptor_uuid;
  metadata.descriptor_epoch = row.manifest.descriptor_rows.front().descriptor_epoch;
  const auto bound = dt::LookupExecutionTypeDescriptorFromCatalog(type, metadata);
  Require(bound.ok(), "numeric fixture execution descriptor refused");
  return bound.descriptor;
}

dt::DatatypeOperationValue IntegerValue(dt::CanonicalTypeId type, std::string_view decimal) {
  dt::DatatypeOperationValue value;
  value.type_id = type;
  value.descriptor = ExecutionDescriptor(type);
  Require(type == dt::CanonicalTypeId::int128
              ? dt::EncodeCanonicalInt128Value(decimal, &value.encoded_value)
              : type == dt::CanonicalTypeId::uint128 &&
                    dt::EncodeCanonicalUint128Value(decimal, &value.encoded_value),
          "numeric fixture integer encoding failed");
  Require(value.encoded_value.size() == 16, "numeric fixture must be native LE16");
  return value;
}

std::string Display(const dt::DatatypeOperationValue& value) {
  const auto rendered = dt::RenderDatatypeValueForDisplay({value});
  Require(rendered.ok(), "numeric result display failed");
  return rendered.display_value;
}

api::EngineDescriptor Descriptor(std::string type) {
  const auto bound = ExecutionDescriptor(dt::CanonicalTypeIdFromStableName(type));
  api::EngineDescriptor descriptor;
  std::copy(std::begin(bound.descriptor_uuid.bytes), std::end(bound.descriptor_uuid.bytes),
            descriptor.datatype_descriptor_uuid.bytes.begin());
  descriptor.datatype_descriptor_generation = bound.descriptor_epoch;
  descriptor.descriptor_uuid = scratchbird::tests::FixtureUuid(1700,
      static_cast<std::uint64_t>(dt::CanonicalTypeIdFromStableName(type)));
  const auto identity = dt::LookupDatatypeTypeCodecIdentityV1(
      api::kBootstrapDatatypeCatalogUuid, api::kBootstrapDatatypeCatalogGeneration,
      api::kBootstrapDatatypeRegistryGeneration, descriptor.datatype_descriptor_uuid,
      descriptor.datatype_descriptor_generation);
  Require(identity.ok, "numeric fixture current codec binding failed");
  descriptor.type_uuid = identity.row.type_uuid;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = std::move(type);
  descriptor.encoded_descriptor = "nullability=non_null;width=128";
  return descriptor;
}

api::EngineTypedValue TypedValue(std::string type, std::string encoded) {
  api::EngineTypedValue value;
  value.descriptor = Descriptor(type);
  const auto native = IntegerValue(dt::CanonicalTypeIdFromStableName(type), encoded);
  value.binary_value.assign(native.encoded_value.begin(), native.encoded_value.end());
  value.setState(api::EngineValueState::value);
  return value;
}

std::string Display(const api::EngineTypedValue& value) {
  Require(value.state == api::EngineValueState::value && !value.is_null &&
              value.encoded_value.empty() && value.binary_value.size() == 16,
          "engine numeric result must contain only native LE16");
  dt::DatatypeOperationValue native;
  native.type_id = dt::CanonicalTypeIdFromStableName(value.descriptor.canonical_type_name);
  native.descriptor = ExecutionDescriptor(native.type_id);
  native.encoded_value.assign(value.binary_value.begin(), value.binary_value.end());
  return Display(native);
}

std::string FirstDetail(const api::EngineApiResult& result) {
  return result.diagnostics.empty() ? std::string{} : result.diagnostics.front().detail;
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && (std::holds_alternative<std::string>(evidence.evidence_id) && std::get<std::string>(evidence.evidence_id) == id)) { return true; }
  }
  return false;
}

std::string DiagnosticDetail(const platform::DiagnosticRecord& diagnostic) {
  for (const auto& argument : diagnostic.arguments) {
    if (argument.key == "detail") { if (const auto* text = argument.text()) return *text; }
  }
  return {};
}

sblr::SblrOperationEnvelope NumericEnvelope() {
  constexpr std::string_view operation_id =
      "engine.op.query_apply_numeric_operation";
  const auto* registry_entry = sblr::LookupSblrOperation(operation_id);
  Require(registry_entry != nullptr && registry_entry->code == 1036 &&
              registry_entry->opcode == "SBLR_QUERY_APPLY_NUMERIC_OPERATION",
          "canonical numeric SBLR identity did not resolve");
  auto envelope = sblr::MakeSblrEnvelope(
      std::string(operation_id), registry_entry->opcode,
      "trace.cbq017.numeric_backend.query.apply_numeric_operation");
  envelope.opcode_code = registry_entry->code;
  envelope.parser_package_uuid =
      scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000170011");
  envelope.registry_snapshot_uuid =
      scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000170012");
  envelope.result_shape = "typed_value";
  envelope.diagnostic_shape = "diagnostic_vector";
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = false;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  return envelope;
}

numeric::NumericResult RunNumeric(numeric::NumericType type,
                                  numeric::NumericOperation operation,
                                  std::string left,
                                  std::string right = {}) {
  numeric::NumericRequest request;
  request.type = type;
  request.operation = operation;
  request.left = {type, std::move(left), false};
  request.right = {type, std::move(right), false};
  request.context.precision = 38;
  request.context.scale = 0;
  request.context.allow_special_values = true;
  return numeric::ApplyNumericOperation(request);
}

void TestCapabilityManifest() {
  const auto check = platform::CheckMandatoryRuntimeCapabilities();
  for (const auto& diagnostic : check.diagnostics) {
    std::cerr << diagnostic.diagnostic_code << '\n';
  }
  Require(check.ok(), "mandatory numeric runtime capabilities are not present");

  bool saw_int128 = false;
  bool saw_uint128 = false;
  bool saw_real128 = false;
  std::string real128_provider;
  for (const auto& capability : check.manifest.capabilities) {
    saw_int128 = saw_int128 || (capability.key == "numeric.int128" &&
                                capability.state == platform::CapabilityState::present);
    saw_uint128 = saw_uint128 || (capability.key == "numeric.uint128" &&
                                  capability.state == platform::CapabilityState::present);
    if (capability.key == "numeric.real128" &&
        capability.state == platform::CapabilityState::present) {
      saw_real128 = true;
      real128_provider = capability.provider;
    }
  }
  Require(saw_int128, "numeric.int128 capability missing from manifest");
  Require(saw_uint128, "numeric.uint128 capability missing from manifest");
  Require(saw_real128, "numeric.real128 capability missing from manifest");
  Require(real128_provider == kReferenceReal128Provider,
          "numeric.real128 capability reports an unsupported provider");
  Require(real128_provider == numeric::Real128BackendName(),
          "numeric.real128 capability provider does not match the compiled sbl_numeric backend");
  std::cout << "real128_backend_truth=passed;provider="
            << real128_provider << '\n';
}

void TestSblNumericIntegerFamilies() {
  Require(static_cast<std::uint16_t>(numeric::NumericType::decimal) == 0 &&
              static_cast<std::uint16_t>(numeric::NumericType::decimal_float) == 1 &&
              static_cast<std::uint16_t>(numeric::NumericType::real128) == 2 &&
              static_cast<std::uint16_t>(numeric::NumericType::int128) == 3 &&
              static_cast<std::uint16_t>(numeric::NumericType::uint128) == 4,
          "public NumericType enum values drifted");

  auto result = RunNumeric(numeric::NumericType::int128,
                           numeric::NumericOperation::canonicalize,
                           std::string(kInt128Max));
  Require(result.status == numeric::NumericStatusCode::ok &&
              result.value.encoded == kInt128Max,
          "int128 canonical max failed");

  result = RunNumeric(numeric::NumericType::int128,
                      numeric::NumericOperation::add,
                      "170141183460469231731687303715884105726",
                      "1");
  Require(result.status == numeric::NumericStatusCode::ok &&
              result.value.encoded == kInt128Max,
          "int128 add to max failed");

  result = RunNumeric(numeric::NumericType::int128,
                      numeric::NumericOperation::add,
                      std::string(kInt128Max),
                      "1");
  Require(result.status == numeric::NumericStatusCode::overflow &&
              result.diagnostic_code == "numeric.int128_out_of_range",
          "int128 overflow did not fail closed");

  result = RunNumeric(numeric::NumericType::int128,
                      numeric::NumericOperation::divide,
                      std::string(kInt128Min),
                      "-1");
  Require(result.status == numeric::NumericStatusCode::overflow &&
              result.diagnostic_code == "numeric.int128_out_of_range",
          "int128 min/-1 overflow did not fail closed");

  result = RunNumeric(numeric::NumericType::uint128,
                      numeric::NumericOperation::canonicalize,
                      std::string(kUint128Max));
  Require(result.status == numeric::NumericStatusCode::ok &&
              result.value.encoded == kUint128Max,
          "uint128 canonical max failed");

  result = RunNumeric(numeric::NumericType::uint128,
                      numeric::NumericOperation::add,
                      "340282366920938463463374607431768211454",
                      "1");
  Require(result.status == numeric::NumericStatusCode::ok &&
              result.value.encoded == kUint128Max,
          "uint128 add to max failed");

  result = RunNumeric(numeric::NumericType::uint128,
                      numeric::NumericOperation::add,
                      std::string(kUint128Max),
                      "1");
  Require(result.status == numeric::NumericStatusCode::overflow &&
              result.diagnostic_code == "numeric.uint128_out_of_range",
          "uint128 overflow did not fail closed");

  result = RunNumeric(numeric::NumericType::uint128,
                      numeric::NumericOperation::canonicalize,
                      "-1");
  Require(result.status == numeric::NumericStatusCode::invalid_left,
          "uint128 negative input was accepted");
}

void TestSblNumericReal128AndDecimalSeparation() {
  auto result = RunNumeric(numeric::NumericType::real128,
                           numeric::NumericOperation::add,
                           "1.25",
                           "2.5");
  Require(result.status == numeric::NumericStatusCode::ok &&
              result.value.encoded.find("3.75") == 0,
          "real128 add failed");

  result = RunNumeric(numeric::NumericType::real128,
                      numeric::NumericOperation::canonicalize,
                      "NaN");
  Require(result.status == numeric::NumericStatusCode::ok &&
              result.value.encoded == "NaN",
          "real128 NaN canonicalization failed");

  result = RunNumeric(numeric::NumericType::real128,
                      numeric::NumericOperation::canonicalize,
                      "sNaN");
  Require(result.status == numeric::NumericStatusCode::ok &&
              result.value.encoded == "sNaN",
          "real128 signaling NaN canonicalization failed");

  result = RunNumeric(numeric::NumericType::real128,
                      numeric::NumericOperation::canonicalize,
                      "Infinity");
  Require(result.status == numeric::NumericStatusCode::ok &&
              result.value.encoded == "Infinity",
          "real128 infinity canonicalization failed");

  result = RunNumeric(numeric::NumericType::real128,
                      numeric::NumericOperation::multiply,
                      "1.5",
                      "-2");
  Require(result.status == numeric::NumericStatusCode::ok &&
              result.value.encoded == "-3",
          "real128 multiply failed");

  result = RunNumeric(numeric::NumericType::real128,
                      numeric::NumericOperation::divide,
                      "1",
                      "0");
  Require(result.status == numeric::NumericStatusCode::divide_by_zero &&
              result.diagnostic_code == "NUMERIC.REAL128.DIVIDE_BY_ZERO",
          "real128 division by zero did not fail closed");

  result = RunNumeric(numeric::NumericType::real128,
                      numeric::NumericOperation::compare,
                      "NaN",
                      "1");
  Require(result.status == numeric::NumericStatusCode::unordered &&
              result.diagnostic_code == "NUMERIC.REAL128.INVALID",
          "real128 NaN compare was not unordered");

  result = RunNumeric(numeric::NumericType::decimal_float,
                      numeric::NumericOperation::canonicalize,
                      "NaN");
  Require(result.status == numeric::NumericStatusCode::ok &&
              result.value.type == numeric::NumericType::decimal_float &&
              result.value.encoded == "NaN",
          "decimal_float special value did not remain decimal_float");
}

void TestDatatypeNumericOperations() {
  dt::DatatypeNumericOperationRequest request;
  request.operation = dt::DatatypeNumericOperationKind::add;
  request.type_id = dt::CanonicalTypeId::int128;
  request.left = IntegerValue(dt::CanonicalTypeId::int128, "170141183460469231731687303715884105726");
  request.right = IntegerValue(dt::CanonicalTypeId::int128, "1");
  request.result_descriptor = ExecutionDescriptor(request.type_id);
  auto result = dt::ApplyNumericOperation(request);
  Require(result.ok() && result.value.type_id == dt::CanonicalTypeId::int128 &&
              Display(result.value) == kInt128Max,
          "datatype int128 add failed");

  request.left = IntegerValue(dt::CanonicalTypeId::int128, kInt128Max);
  result = dt::ApplyNumericOperation(request);
  Require(!result.ok() && result.diagnostic.diagnostic_code == "NUMERIC.INT128.OVERFLOW" &&
              result.numeric_facts.overflow && result.value.encoded_value.empty(),
          "datatype int128 overflow was accepted");

  request.operation = dt::DatatypeNumericOperationKind::multiply;
  request.type_id = dt::CanonicalTypeId::uint128;
  request.left = IntegerValue(dt::CanonicalTypeId::uint128, "18446744073709551616");
  request.right = IntegerValue(dt::CanonicalTypeId::uint128, "2");
  request.result_descriptor = ExecutionDescriptor(request.type_id);
  result = dt::ApplyNumericOperation(request);
  Require(result.ok() && result.value.type_id == dt::CanonicalTypeId::uint128 &&
              Display(result.value) == "36893488147419103232",
          "datatype uint128 multiply failed");

  dt::DatatypeCastRequest cast;
  cast.value = {dt::CanonicalTypeId::character, "1.25", false};
  cast.value.descriptor = ExecutionDescriptor(dt::CanonicalTypeId::character);
  cast.target_type_id = dt::CanonicalTypeId::real128;
  cast.target_descriptor = ExecutionDescriptor(cast.target_type_id);
  cast.explicit_cast = true;
  const auto cast_result = dt::CastDatatypeValue(cast);
  if (!cast_result.ok()) std::cerr << cast_result.diagnostic.diagnostic_code << ':'
                                 << DiagnosticDetail(cast_result.diagnostic) << '\n';
  Require(cast_result.ok() && cast_result.value.type_id == dt::CanonicalTypeId::real128 &&
              cast_result.value.encoded_value ==
                  std::string("\x00\x00\x00\x00\x00\x00\x00\x00"
                              "\x00\x00\x00\x00\x00\x40\xff\x3f", 16),
          "datatype real128 cast did not use numeric backend");
  dt::DatatypeCastRequest render;
  render.value = cast_result.value;
  render.target_type_id = dt::CanonicalTypeId::character;
  render.target_descriptor = ExecutionDescriptor(render.target_type_id);
  render.explicit_cast = true;
  const auto rendered = dt::CastDatatypeValue(render);
  Require(rendered.ok() && rendered.value.encoded_value == "1.25",
          "datatype real128 character round trip changed the value");

  cast.value = {dt::CanonicalTypeId::character, "+000170141183460469231731687303715884105727", false};
  cast.value.descriptor = ExecutionDescriptor(cast.value.type_id);
  cast.target_type_id = dt::CanonicalTypeId::int128;
  cast.target_descriptor = ExecutionDescriptor(cast.target_type_id);
  const auto int128_cast_result = dt::CastDatatypeValue(cast);
  Require(int128_cast_result.ok() &&
              int128_cast_result.value.type_id == dt::CanonicalTypeId::int128 &&
              Display(int128_cast_result.value) == kInt128Max,
          "datatype int128 cast did not use numeric backend canonicalization");

  cast.value = {dt::CanonicalTypeId::character, "340282366920938463463374607431768211455", false};
  cast.value.descriptor = ExecutionDescriptor(cast.value.type_id);
  cast.target_type_id = dt::CanonicalTypeId::uint128;
  cast.target_descriptor = ExecutionDescriptor(cast.target_type_id);
  const auto uint128_cast_result = dt::CastDatatypeValue(cast);
  Require(uint128_cast_result.ok() &&
              uint128_cast_result.value.type_id == dt::CanonicalTypeId::uint128 &&
              Display(uint128_cast_result.value) == kUint128Max,
          "datatype uint128 max cast did not use numeric backend");

  cast.value = {dt::CanonicalTypeId::character, "340282366920938463463374607431768211456", false};
  cast.value.descriptor = ExecutionDescriptor(cast.value.type_id);
  const auto uint128_overflow = dt::CastDatatypeValue(cast);
  Require(!uint128_overflow.ok() &&
              DiagnosticDetail(uint128_overflow.diagnostic) == "numeric.uint128_out_of_range",
          "datatype uint128 overflow cast diagnostic drifted");

  cast.value = {dt::CanonicalTypeId::character, "-1", false};
  cast.value.descriptor = ExecutionDescriptor(cast.value.type_id);
  const auto uint128_negative = dt::CastDatatypeValue(cast);
  Require(!uint128_negative.ok() &&
              DiagnosticDetail(uint128_negative.diagnostic) == "numeric.uint128_left_invalid",
          "datatype uint128 negative cast diagnostic drifted");

  cast.value = {dt::CanonicalTypeId::character, "170141183460469231731687303715884105728", false};
  cast.value.descriptor = ExecutionDescriptor(cast.value.type_id);
  cast.target_type_id = dt::CanonicalTypeId::int128;
  cast.target_descriptor = ExecutionDescriptor(cast.target_type_id);
  const auto int128_overflow = dt::CastDatatypeValue(cast);
  Require(!int128_overflow.ok() &&
              DiagnosticDetail(int128_overflow.diagnostic) == "numeric.int128_out_of_range",
          "datatype int128 overflow cast diagnostic drifted");
}

void TestEngineApiAndSblrRoutes() {
  api::EngineApplyNumericOperationRequest request;
  request.context = EngineContext();
  request.numeric_operation = "add";
  request.left_value = TypedValue("int128", "170141183460469231731687303715884105726");
  request.right_value = TypedValue("int128", "1");
  request.descriptors.push_back(Descriptor("int128"));
  auto result = api::EngineApplyNumericOperation(request);
  Require(result.ok && result.value.descriptor.canonical_type_name == "int128" &&
              Display(result.value) == kInt128Max &&
              HasEvidence(result, "datatype_numeric_operation", "add"),
          "engine API int128 numeric operation failed");

  request.left_value = TypedValue("uint128", "340282366920938463463374607431768211454");
  request.right_value = TypedValue("uint128", "1");
  request.descriptors.clear();
  request.descriptors.push_back(Descriptor("uint128"));
  result = api::EngineApplyNumericOperation(request);
  Require(result.ok && result.value.descriptor.canonical_type_name == "uint128" &&
              Display(result.value) == kUint128Max,
          "engine API uint128 numeric operation failed");

  request.left_value = TypedValue("int128", std::string(kInt128Max));
  request.right_value = TypedValue("int128", "1");
  request.descriptors.clear();
  request.descriptors.push_back(Descriptor("int128"));
  result = api::EngineApplyNumericOperation(request);
  Require(!result.ok && !result.diagnostics.empty() &&
              result.diagnostics.front().code ==
                  "NUMERIC.INT128.OVERFLOW" &&
              FirstDetail(result) == "NUMERIC.INT128.OVERFLOW" &&
              result.numeric_facts.overflow &&
              result.value.state == api::EngineValueState::error &&
              result.value.binary_value.empty() && result.value.encoded_value.empty(),
          "engine API int128 overflow diagnostic drifted");

  auto envelope = NumericEnvelope();
  const auto dispatched = sblr::DispatchSblrOperation({EngineContext(), envelope, api::EngineApiRequest{}});
  Require(!dispatched.envelope_validated && !dispatched.accepted &&
              !dispatched.dispatched_to_api &&
              !dispatched.diagnostics.empty() &&
              dispatched.diagnostics.front().code == "SBLR.OPERAND_INVALID",
          "descriptor-less numeric SBLR bypassed engine-bound authority");
}

void TestNativeIntegerCarrierRefusals() {
  for (const std::string type : {"int128", "uint128"}) {
    api::EngineApplyNumericOperationRequest arithmetic;
    arithmetic.context = EngineContext();
    arithmetic.numeric_operation = "add";
    arithmetic.left_value = TypedValue(type, "1");
    arithmetic.right_value = TypedValue(type, "2");
    arithmetic.descriptors = {Descriptor(type)};
    const auto added = api::EngineApplyNumericOperation(arithmetic);
    if (!added.ok) std::cerr << type << ':' << FirstDetail(added) << '\n';
    Require(added.ok && Display(added.value) == "3",
            "native wide-integer addition failed");
    auto alias = arithmetic.left_value;
    dt::DatatypeNumericFacts facts;
    std::string alias_detail;
    Require(api::QowApplyCanonicalNumericScalarV1(alias, arithmetic.right_value,
                alias.descriptor, dt::DatatypeNumericOperationKind::add, {},
                &alias, &alias_detail, &facts) && Display(alias) == "3",
            "wide-integer output alias destroyed an input");
    api::EngineCastValueRequest cast;
    cast.context = EngineContext();
    cast.input_value = arithmetic.left_value;
    cast.target_descriptor = arithmetic.descriptors.front();
    const auto identity = api::EngineCastValue(cast);
    Require(identity.ok && Display(identity.value) == "1" &&
                identity.value.descriptor == cast.target_descriptor,
            "native wide-integer identity cast lost its carrier or descriptor");
    int comparison = 9;
    std::string detail;
    Require(api::QowCompareCanonicalNonCollatedScalarsV1(
                arithmetic.left_value, arithmetic.right_value, &comparison, &detail) &&
                comparison == -1,
            "native wide-integer comparison failed");

    for (const auto width : {0u, 1u, 8u, 15u, 17u, 32u}) {
      auto malformed = arithmetic;
      malformed.left_value.binary_value.assign(width, 0);
      const auto refused = api::EngineApplyNumericOperation(malformed);
      Require(!refused.ok && !refused.diagnostics.empty() &&
                  refused.diagnostics.front().code == "NUMERIC.ENCODING.NONCANONICAL" &&
                  refused.numeric_facts.invalid &&
                  refused.value.state == api::EngineValueState::error &&
                  refused.value.binary_value.empty() && refused.value.encoded_value.empty(),
              "malformed wide-integer carrier published arithmetic output");
      comparison = 9;
      Require(!api::QowCompareCanonicalNonCollatedScalarsV1(
                  malformed.left_value, arithmetic.right_value, &comparison, &detail) &&
                  comparison == 0 && detail == "NUMERIC.ENCODING.NONCANONICAL",
              "malformed wide-integer comparison was admitted");
    }
    for (bool retain_binary : {false, true}) {
      auto malformed = arithmetic;
      malformed.left_value.encoded_value = "1";
      if (!retain_binary) malformed.left_value.binary_value.clear();
      const auto refused = api::EngineApplyNumericOperation(malformed);
      Require(!refused.ok && !refused.diagnostics.empty() &&
                  refused.diagnostics.front().code == "NUMERIC.ENCODING.NONCANONICAL" &&
                  refused.value.binary_value.empty() && refused.value.encoded_value.empty(),
              "text or dual wide-integer carrier was admitted");
      cast.input_value = malformed.left_value;
      Require(!api::EngineCastValue(cast).ok,
              "identity cast accepted text or dual wide-integer carrier");
    }
    auto unbound = arithmetic;
    unbound.left_value.descriptor.datatype_descriptor_uuid = {};
    const auto refused = api::EngineApplyNumericOperation(unbound);
    Require(!refused.ok && !refused.diagnostics.empty() &&
                refused.diagnostics.front().code == "DATATYPE.DESCRIPTOR.INVALID" &&
                refused.value.binary_value.empty() && refused.value.encoded_value.empty(),
            "wide-integer arithmetic inferred missing datatype authority");
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--native-integer128") {
    TestNativeIntegerCarrierRefusals();
    TestEngineApiAndSblrRoutes();
    return EXIT_SUCCESS;
  }
  Require(argc == 1, "unknown numeric regression option");
  TestCapabilityManifest();
  TestSblNumericIntegerFamilies();
  TestSblNumericReal128AndDecimalSeparation();
  TestDatatypeNumericOperations();
  TestNativeIntegerCarrierRefusals();
  TestEngineApiAndSblrRoutes();
  return EXIT_SUCCESS;
}
