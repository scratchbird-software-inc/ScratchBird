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
bool QowCanonicalDescriptorIdentityV1(const EngineDescriptor& descriptor);
EngineTypedValue QowPreserveCanonicalDescriptorAfterScalarV1(
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
      "019f0000-0000-7200-8000-000000000801";
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "decimal";
  descriptor.encoded_descriptor =
      "type_uuid=019f0000-0000-7300-8000-000000000802;"
      "nullability=nullable;precision=18;scale=2";
  return descriptor;
}

bool ValidateDescriptorPreservation() {
  const auto descriptor = DecimalDescriptor();
  api::EngineTypedValue computed;
  computed.descriptor.descriptor_kind = "scalar";
  computed.descriptor.canonical_type_name = "decimal";
  computed.descriptor.encoded_descriptor = "type=decimal";
  computed.encoded_value = "3.00";
  computed.state = api::EngineValueState::value;

  const auto result = api::QowPreserveCanonicalDescriptorAfterScalarV1(
      descriptor, computed);
  bool passed = true;
  passed &= Require(api::QowCanonicalDescriptorIdentityV1(descriptor),
                    "canonical scalar descriptor was refused");
  passed &= Require(
      result.descriptor.descriptor_uuid.canonical ==
              descriptor.descriptor_uuid.canonical &&
          result.descriptor.descriptor_kind == descriptor.descriptor_kind &&
          result.descriptor.canonical_type_name ==
              descriptor.canonical_type_name &&
          result.descriptor.encoded_descriptor ==
              descriptor.encoded_descriptor &&
          result.encoded_value == "3.00" &&
          result.state == api::EngineValueState::value,
      "physical scalar result did not preserve canonical descriptor identity");
  return passed;
}

bool ValidateDescriptorRefusal() {
  auto malformed = DecimalDescriptor();
  malformed.descriptor_uuid.canonical = "not-a-uuid";
  auto incomplete = DecimalDescriptor();
  incomplete.encoded_descriptor.clear();

  bool passed = true;
  passed &= Require(!api::QowCanonicalDescriptorIdentityV1(malformed),
                    "malformed descriptor UUID was accepted");
  passed &= Require(!api::QowCanonicalDescriptorIdentityV1(incomplete),
                    "incomplete descriptor record was accepted");
  return passed;
}

}  // namespace

// QOW-TEST-QRY-008-DESC-V1
int main() {
  bool passed = true;
  passed &= ValidateDescriptorPreservation();
  passed &= ValidateDescriptorRefusal();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
