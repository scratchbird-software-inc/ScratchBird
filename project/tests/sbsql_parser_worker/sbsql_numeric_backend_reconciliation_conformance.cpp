// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "datatype_operations.hpp"
#include "query/expression_api.hpp"
#include "runtime_capabilities.hpp"
#include "sbl_numeric.hpp"
#include "sblr_dispatch.hpp"

#include <cstdint>
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

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

api::EngineRequestContext EngineContext() {
  api::EngineRequestContext context;
  context.request_id = "sbsql-numeric-backend-reconciliation";
  context.database_uuid.canonical = "019f0000-0000-7000-8000-000000170001";
  context.session_uuid.canonical = "019f0000-0000-7000-8000-000000170002";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000170003";
  context.security_context_present = true;
  context.trace_tags.push_back("numeric_backend_reconciliation");
  return context;
}

api::EngineDescriptor Descriptor(std::string type) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = std::move(type);
  descriptor.encoded_descriptor = "canonical_type=" + descriptor.canonical_type_name;
  return descriptor;
}

api::EngineTypedValue TypedValue(std::string type, std::string encoded) {
  api::EngineTypedValue value;
  value.descriptor = Descriptor(std::move(type));
  value.encoded_value = std::move(encoded);
  value.is_null = false;
  return value;
}

std::string FirstDetail(const api::EngineApiResult& result) {
  return result.diagnostics.empty() ? std::string{} : result.diagnostics.front().detail;
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && evidence.evidence_id == id) { return true; }
  }
  return false;
}

std::string DiagnosticDetail(const platform::DiagnosticRecord& diagnostic) {
  for (const auto& argument : diagnostic.arguments) {
    if (argument.key == "detail") { return argument.value; }
  }
  return {};
}

void AddOptionOperand(sblr::SblrOperationEnvelope* envelope,
                      std::string name,
                      std::string value) {
  sblr::SblrOperand operand;
  operand.type = "option";
  operand.name = std::move(name);
  operand.value = std::move(value);
  envelope->operands.push_back(std::move(operand));
}

sblr::SblrOperationEnvelope NumericEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope("query.apply_numeric_operation",
                                         "SBLR_QUERY_APPLY_NUMERIC_OPERATION",
                                         "trace.cbq017.numeric_backend.query.apply_numeric_operation");
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
  for (const auto& capability : check.manifest.capabilities) {
    saw_int128 = saw_int128 || (capability.key == "numeric.int128" &&
                                capability.state == platform::CapabilityState::present);
    saw_uint128 = saw_uint128 || (capability.key == "numeric.uint128" &&
                                  capability.state == platform::CapabilityState::present);
    saw_real128 = saw_real128 || (capability.key == "numeric.real128" &&
                                  capability.state == platform::CapabilityState::present &&
                                  capability.provider.find("quadmath") != std::string::npos);
  }
  Require(saw_int128, "numeric.int128 capability missing from manifest");
  Require(saw_uint128, "numeric.uint128 capability missing from manifest");
  Require(saw_real128, "numeric.real128 quadmath capability missing from manifest");
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
                      numeric::NumericOperation::compare,
                      "NaN",
                      "1");
  Require(result.status == numeric::NumericStatusCode::unordered &&
              result.diagnostic_code == "numeric.real128_nan_unordered",
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
  request.left = {dt::CanonicalTypeId::int128, "170141183460469231731687303715884105726", false};
  request.right = {dt::CanonicalTypeId::int128, "1", false};
  auto result = dt::ApplyNumericOperation(request);
  Require(result.ok() && result.value.type_id == dt::CanonicalTypeId::int128 &&
              result.value.encoded_value == kInt128Max,
          "datatype int128 add failed");

  request.left.encoded_value = std::string(kInt128Max);
  request.right.encoded_value = "1";
  result = dt::ApplyNumericOperation(request);
  Require(!result.ok() && result.diagnostic.diagnostic_code == "SB_DATATYPE_NUMERIC_OPERATION_REJECTED",
          "datatype int128 overflow was accepted");

  request.operation = dt::DatatypeNumericOperationKind::multiply;
  request.type_id = dt::CanonicalTypeId::uint128;
  request.left = {dt::CanonicalTypeId::uint128, "18446744073709551616", false};
  request.right = {dt::CanonicalTypeId::uint128, "2", false};
  result = dt::ApplyNumericOperation(request);
  Require(result.ok() && result.value.type_id == dt::CanonicalTypeId::uint128 &&
              result.value.encoded_value == "36893488147419103232",
          "datatype uint128 multiply failed");

  dt::DatatypeCastRequest cast;
  cast.value = {dt::CanonicalTypeId::character, "1.25", false};
  cast.target_type_id = dt::CanonicalTypeId::real128;
  cast.explicit_cast = true;
  const auto cast_result = dt::CastDatatypeValue(cast);
  Require(cast_result.ok() && cast_result.value.type_id == dt::CanonicalTypeId::real128 &&
              cast_result.value.encoded_value.find("1.25") == 0,
          "datatype real128 cast did not use numeric backend");

  cast.value = {dt::CanonicalTypeId::character, "+000170141183460469231731687303715884105727", false};
  cast.target_type_id = dt::CanonicalTypeId::int128;
  const auto int128_cast_result = dt::CastDatatypeValue(cast);
  Require(int128_cast_result.ok() &&
              int128_cast_result.value.type_id == dt::CanonicalTypeId::int128 &&
              int128_cast_result.value.encoded_value == kInt128Max,
          "datatype int128 cast did not use numeric backend canonicalization");

  cast.value = {dt::CanonicalTypeId::character, "340282366920938463463374607431768211455", false};
  cast.target_type_id = dt::CanonicalTypeId::uint128;
  const auto uint128_cast_result = dt::CastDatatypeValue(cast);
  Require(uint128_cast_result.ok() &&
              uint128_cast_result.value.type_id == dt::CanonicalTypeId::uint128 &&
              uint128_cast_result.value.encoded_value == kUint128Max,
          "datatype uint128 max cast did not use numeric backend");

  cast.value = {dt::CanonicalTypeId::character, "340282366920938463463374607431768211456", false};
  const auto uint128_overflow = dt::CastDatatypeValue(cast);
  Require(!uint128_overflow.ok() &&
              DiagnosticDetail(uint128_overflow.diagnostic) == "numeric.uint128_out_of_range",
          "datatype uint128 overflow cast diagnostic drifted");

  cast.value = {dt::CanonicalTypeId::character, "-1", false};
  const auto uint128_negative = dt::CastDatatypeValue(cast);
  Require(!uint128_negative.ok() &&
              DiagnosticDetail(uint128_negative.diagnostic) == "numeric.uint128_left_invalid",
          "datatype uint128 negative cast diagnostic drifted");

  cast.value = {dt::CanonicalTypeId::character, "170141183460469231731687303715884105728", false};
  cast.target_type_id = dt::CanonicalTypeId::int128;
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
              result.value.encoded_value == kInt128Max &&
              HasEvidence(result, "datatype_numeric_operation", "add"),
          "engine API int128 numeric operation failed");

  request.left_value = TypedValue("uint128", "340282366920938463463374607431768211454");
  request.right_value = TypedValue("uint128", "1");
  request.descriptors.clear();
  request.descriptors.push_back(Descriptor("uint128"));
  result = api::EngineApplyNumericOperation(request);
  Require(result.ok && result.value.descriptor.canonical_type_name == "uint128" &&
              result.value.encoded_value == kUint128Max,
          "engine API uint128 numeric operation failed");

  request.left_value = TypedValue("int128", std::string(kInt128Max));
  request.right_value = TypedValue("int128", "1");
  request.descriptors.clear();
  request.descriptors.push_back(Descriptor("int128"));
  result = api::EngineApplyNumericOperation(request);
  Require(!result.ok &&
              FirstDetail(result) == "query.apply_numeric_operation:numeric.int128_out_of_range",
          "engine API int128 overflow diagnostic drifted");

  auto envelope = NumericEnvelope();
  AddOptionOperand(&envelope, "numeric_operation", "add");
  AddOptionOperand(&envelope, "left_type", "int128");
  AddOptionOperand(&envelope, "left_value", "170141183460469231731687303715884105726");
  AddOptionOperand(&envelope, "right_type", "int128");
  AddOptionOperand(&envelope, "right_value", "1");
  const auto dispatched = sblr::DispatchSblrOperation({EngineContext(), envelope, api::EngineApiRequest{}});
  for (const auto& diagnostic : dispatched.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : dispatched.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(dispatched.envelope_validated && dispatched.accepted && dispatched.dispatched_to_api,
          "SBLR int128 numeric route did not dispatch");
  Require(dispatched.api_result.ok &&
              dispatched.api_result.operation_id == "query.apply_numeric_operation" &&
              HasEvidence(dispatched.api_result, "datatype_numeric_operation", "add"),
          "SBLR int128 numeric route failed");
}

}  // namespace

int main() {
  TestCapabilityManifest();
  TestSblNumericIntegerFamilies();
  TestSblNumericReal128AndDecimalSeparation();
  TestDatatypeNumericOperations();
  TestEngineApiAndSblrRoutes();
  return EXIT_SUCCESS;
}
