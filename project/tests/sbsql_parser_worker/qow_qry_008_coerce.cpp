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
#include <string>
#include <string_view>

namespace api = scratchbird::engine::internal_api;

namespace scratchbird::engine::internal_api {
bool QowApplyCanonicalDescriptorCoercionV1(
    const EngineTypedValue& input_value,
    const EngineDescriptor& target_descriptor,
    bool explicit_cast,
    EngineTypedValue* output_value,
    std::string* cast_category,
    std::string* refusal_detail);
}  // namespace scratchbird::engine::internal_api

namespace {

bool Require(const bool condition, const std::string_view message) {
  if (!condition) std::cerr << message << '\n';
  return condition;
}

api::EngineDescriptor Descriptor(const std::string& uuid_suffix,
                                 const std::string& type_name) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical =
      "019f0000-0000-7200-8000-0000000008" + uuid_suffix;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = type_name;
  descriptor.encoded_descriptor =
      "type_uuid=019f0000-0000-7300-8000-0000000008" + uuid_suffix +
      ";nullability=non_null";
  return descriptor;
}

bool ValidateAcceptedCoercion() {
  api::EngineTypedValue input;
  input.descriptor = Descriptor("21", "int64");
  input.encoded_value = "42";
  input.state = api::EngineValueState::value;
  const auto target = Descriptor("22", "decimal");

  api::EngineTypedValue output;
  std::string category;
  std::string refusal;
  const bool accepted = api::QowApplyCanonicalDescriptorCoercionV1(
      input, target, true, &output, &category, &refusal);
  bool passed = true;
  passed &= Require(accepted && refusal.empty(),
                    "accepted canonical descriptor coercion was refused");
  passed &= Require(
      output.descriptor.descriptor_uuid.canonical ==
              target.descriptor_uuid.canonical &&
          output.descriptor.canonical_type_name == "decimal" &&
          output.encoded_value == "42" &&
          output.state == api::EngineValueState::value && !category.empty(),
      "accepted coercion lost target descriptor or value state");
  return passed;
}

bool ValidateCoercionRefusal() {
  api::EngineTypedValue input;
  input.descriptor = Descriptor("31", "int64");
  input.encoded_value = "42";
  input.state = api::EngineValueState::value;

  auto unknown_target = Descriptor("32", "not_a_canonical_type");
  api::EngineTypedValue output;
  std::string category;
  std::string refusal;
  const bool unknown_accepted = api::QowApplyCanonicalDescriptorCoercionV1(
      input, unknown_target, true, &output, &category, &refusal);

  auto malformed_source = input;
  malformed_source.descriptor.descriptor_uuid.canonical.clear();
  refusal.clear();
  const bool malformed_accepted = api::QowApplyCanonicalDescriptorCoercionV1(
      malformed_source, Descriptor("33", "decimal"), true, &output,
      &category, &refusal);

  bool passed = true;
  passed &= Require(!unknown_accepted,
                    "unknown target descriptor coercion was accepted");
  passed &= Require(!malformed_accepted,
                    "coercion without source descriptor UUID was accepted");
  return passed;
}

}  // namespace

// QOW-TEST-QRY-008-COERCE-V1
int main() {
  bool passed = true;
  passed &= ValidateAcceptedCoercion();
  passed &= ValidateCoercionRefusal();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
