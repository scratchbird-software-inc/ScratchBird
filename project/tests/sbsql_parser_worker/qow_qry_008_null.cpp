// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "../support/binary_uuid_fixture.hpp"
#include "../support/exact_datatype_descriptor_fixture.hpp"
#include "api_types.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace api = scratchbird::engine::internal_api;

namespace scratchbird::engine::internal_api {
bool QowCanonicalSqlNullStateV1(const EngineTypedValue& value);
EngineTypedValue QowPropagateSqlNullAfterScalarV1(
    const EngineDescriptor& result_descriptor,
    EngineTypedValue computed_value);
}  // namespace scratchbird::engine::internal_api

namespace {

bool Require(const bool condition, const std::string_view message) {
  if (!condition) std::cerr << message << '\n';
  return condition;
}

api::EngineDescriptor DecimalDescriptor() {
  return scratchbird::tests::ExactScalarDescriptorFixture(
      scratchbird::core::datatypes::CanonicalTypeId::decimal, "decimal",
      scratchbird::tests::FixtureUuidLiteral(
          "019f0000-0000-7200-8000-000000000811"),
      "nullability=nullable;precision=18;scale=2");
}

bool ValidateNullPropagation() {
  const auto descriptor = DecimalDescriptor();
  api::EngineTypedValue computed;
  computed.descriptor = descriptor;
  computed.is_null = true;
  computed.state = api::EngineValueState::sql_null;

  const auto result = api::QowPropagateSqlNullAfterScalarV1(
      descriptor, computed);
  bool passed = true;
  passed &= Require(api::QowCanonicalSqlNullStateV1(computed),
                    "canonical SQL NULL state was refused");
  passed &= Require(
      result.state == api::EngineValueState::sql_null && result.is_null &&
          result.isSqlNull() && result.encoded_value.empty() &&
          result.binary_value.empty() &&
          result.descriptor.descriptor_uuid == descriptor.descriptor_uuid &&
          result.descriptor.encoded_descriptor ==
              descriptor.encoded_descriptor,
      "physical scalar SQL NULL was substituted or lost its descriptor");
  return passed;
}

bool ValidateMalformedNullRefusal() {
  api::EngineTypedValue malformed;
  malformed.descriptor = DecimalDescriptor();
  malformed.is_null = false;
  malformed.state = api::EngineValueState::sql_null;

  const auto result = api::QowPropagateSqlNullAfterScalarV1(
      DecimalDescriptor(), malformed);
  bool passed = true;
  passed &= Require(!api::QowCanonicalSqlNullStateV1(malformed),
                    "SQL NULL state without its null marker was accepted");
  passed &= Require(result.state == api::EngineValueState::error &&
                        !result.is_null && result.encoded_value.empty() &&
                        result.binary_value.empty(),
                    "malformed SQL NULL state was silently repaired");
  return passed;
}

bool ValidateDescriptorAuthorityRefusal() {
  const auto descriptor = DecimalDescriptor();
  api::EngineTypedValue value;
  value.descriptor = descriptor;
  value.is_null = true;
  value.state = api::EngineValueState::sql_null;

  auto unbound = descriptor;
  unbound.datatype_descriptor_uuid = {};
  const auto unbound_result = api::QowPropagateSqlNullAfterScalarV1(
      unbound, value);
  auto non_null = descriptor;
  non_null.encoded_descriptor =
      "nullability=non_null;precision=18;scale=2";
  const auto non_null_result = api::QowPropagateSqlNullAfterScalarV1(
      non_null, value);

  bool passed = true;
  passed &= Require(unbound_result.state == api::EngineValueState::error &&
                        !unbound_result.is_null &&
                        unbound_result.encoded_value.empty() &&
                        unbound_result.binary_value.empty(),
                    "SQL NULL acquired an unbound result descriptor");
  passed &= Require(non_null_result.state == api::EngineValueState::error &&
                        !non_null_result.is_null &&
                        non_null_result.encoded_value.empty() &&
                        non_null_result.binary_value.empty(),
                    "SQL NULL acquired a non-NULL result descriptor");
  return passed;
}

bool ValidateNullPayloadRefusal() {
  api::EngineTypedValue substituted;
  substituted.descriptor = DecimalDescriptor();
  substituted.encoded_value = "0";
  substituted.is_null = true;
  substituted.state = api::EngineValueState::sql_null;

  bool passed = true;
  passed &= Require(!api::QowCanonicalSqlNullStateV1(substituted),
                    "SQL NULL carrying a zero substitute was accepted");
  return passed;
}

}  // namespace

// QOW-TEST-QRY-008-NULL-V1
int main() {
  bool passed = true;
  passed &= ValidateNullPropagation();
  passed &= ValidateMalformedNullRefusal();
  passed &= ValidateDescriptorAuthorityRefusal();
  passed &= ValidateNullPayloadRefusal();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
