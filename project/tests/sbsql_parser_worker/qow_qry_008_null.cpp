// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

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
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical =
      "019f0000-0000-7200-8000-000000000811";
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "decimal";
  descriptor.encoded_descriptor =
      "type_uuid=019f0000-0000-7300-8000-000000000812;"
      "nullability=nullable;precision=18;scale=2";
  return descriptor;
}

bool ValidateNullPropagation() {
  const auto descriptor = DecimalDescriptor();
  api::EngineTypedValue computed;
  computed.descriptor.canonical_type_name = "decimal";
  computed.is_null = false;
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
          result.descriptor.descriptor_uuid.canonical ==
              descriptor.descriptor_uuid.canonical &&
          result.descriptor.encoded_descriptor ==
              descriptor.encoded_descriptor,
      "physical scalar SQL NULL was substituted or lost its descriptor");
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
  passed &= ValidateNullPayloadRefusal();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
