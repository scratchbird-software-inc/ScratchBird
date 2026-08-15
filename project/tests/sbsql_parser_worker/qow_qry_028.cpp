// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "query/expression_api.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace api = scratchbird::engine::internal_api;

namespace scratchbird::engine::internal_api {
bool QowPreserveInvalidDescriptorStateAndCoerceV1(
    const EngineTypedValue& input_value,
    const EngineDescriptor& target_descriptor,
    bool explicit_cast,
    EngineTypedValue* output_value,
    std::string* cast_category,
    std::string* refusal_reason,
    std::string* refusal_detail);
}  // namespace scratchbird::engine::internal_api

namespace {

bool Require(const bool condition, const std::string_view message) {
  if (!condition) std::cerr << message << '\n';
  return condition;
}

api::EngineDescriptor Descriptor(const unsigned suffix,
                                 std::string type,
                                 std::string nullability = "nullable") {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical =
      "019f0000-0000-7200-8000-0000000028" +
      std::string(suffix < 10 ? "0" : "") + std::to_string(suffix);
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = std::move(type);
  descriptor.encoded_descriptor =
      "type=" + descriptor.canonical_type_name +
      ";nullability=" + std::move(nullability);
  return descriptor;
}

api::EngineTypedValue Value(const api::EngineDescriptor& descriptor,
                            std::string encoded) {
  api::EngineTypedValue value;
  value.descriptor = descriptor;
  value.encoded_value = std::move(encoded);
  value.state = api::EngineValueState::value;
  return value;
}

bool EqualValue(const api::EngineTypedValue& left,
                const api::EngineTypedValue& right) {
  return left.descriptor.descriptor_uuid.canonical ==
             right.descriptor.descriptor_uuid.canonical &&
         left.descriptor.descriptor_kind == right.descriptor.descriptor_kind &&
         left.descriptor.canonical_type_name ==
             right.descriptor.canonical_type_name &&
         left.descriptor.encoded_descriptor ==
             right.descriptor.encoded_descriptor &&
         left.encoded_value == right.encoded_value &&
         left.binary_value == right.binary_value &&
         left.is_null == right.is_null && left.state == right.state;
}

bool Coerce(const api::EngineTypedValue& input,
            const api::EngineDescriptor& target,
            api::EngineTypedValue* output,
            std::string* category,
            std::string* reason,
            std::string* detail) {
  return api::QowPreserveInvalidDescriptorStateAndCoerceV1(
      input, target, true, output, category, reason, detail);
}

bool AtomicPreservingRefusal(const api::EngineTypedValue& input,
                             const api::EngineDescriptor& target,
                             const std::string_view expected_reason) {
  api::EngineTypedValue output;
  std::string category;
  std::string reason;
  std::string detail;
  const bool ok = Coerce(
      input, target, &output, &category, &reason, &detail);
  return !ok && EqualValue(input, output) && category.empty() &&
         reason == expected_reason && !detail.empty();
}

bool ValidateSupportedCoercion() {
  const auto source = Descriptor(1, "int64");
  const auto target = Descriptor(2, "int64");
  const auto input = Value(source, "42");
  api::EngineTypedValue output;
  std::string category;
  std::string reason;
  std::string detail;
  const bool ok = Coerce(
      input, target, &output, &category, &reason, &detail);
  return Require(ok, "supported canonical coercion was refused") &&
         Require(reason.empty() && detail.empty() && !category.empty(),
                 "supported coercion emitted refusal state") &&
         Require(output.descriptor.descriptor_uuid.canonical ==
                     target.descriptor_uuid.canonical &&
                     output.encoded_value == "42" &&
                     output.state == api::EngineValueState::value,
                 "supported coercion did not preserve target descriptor identity");
}

bool ValidateRefusals() {
  bool passed = true;
  const auto source = Descriptor(3, "int64");
  const auto target = Descriptor(4, "int64");
  {
    auto input = Value(source, "17");
    input.state = api::EngineValueState::missing;
    passed &= Require(
        AtomicPreservingRefusal(input, target, "invalid_descriptor_state"),
        "missing descriptor state was replaced or accepted");
  }
  {
    auto input = Value(source, "invalid-payload");
    input.state = api::EngineValueState::error;
    input.binary_value = {0x28, 0xff};
    passed &= Require(
        AtomicPreservingRefusal(input, target, "invalid_descriptor_state"),
        "error descriptor state was replaced or accepted");
  }
  {
    auto input = Value(source, "19");
    auto malformed_target = target;
    malformed_target.descriptor_uuid.canonical = "malformed";
    passed &= Require(
        AtomicPreservingRefusal(input, malformed_target, "descriptor_invalid"),
        "malformed target descriptor was accepted or rewrote the input");
  }
  {
    auto input = Value(source, "23");
    const auto unsupported_target = Descriptor(5, "unknown_type");
    passed &= Require(
        AtomicPreservingRefusal(
            input, unsupported_target, "unsupported_descriptor_coercion"),
        "unsupported descriptor coercion acquired a substitute result");
  }
  {
    api::EngineTypedValue input;
    input.descriptor = Descriptor(6, "int64", "nullable");
    input.is_null = true;
    input.state = api::EngineValueState::sql_null;
    const auto non_null_target = Descriptor(7, "int64", "non_null");
    passed &= Require(
        AtomicPreservingRefusal(
            input, non_null_target, "unsupported_descriptor_coercion"),
        "SQL NULL was coerced into a non-NULL descriptor");
  }
  return passed;
}

bool ValidateLegacyDescriptorResolution() {
  auto legacy_descriptor = [](std::string type) {
    api::EngineDescriptor descriptor;
    descriptor.descriptor_kind = "scalar";
    descriptor.canonical_type_name = std::move(type);
    return descriptor;
  };
  namespace dt = scratchbird::core::datatypes;
  bool passed = true;
  passed &= Require(
      api::QowCanonicalTypeFromDescriptorV1(legacy_descriptor("int64")) ==
          dt::CanonicalTypeId::int64,
      "legacy canonical descriptor did not resolve its declared type");
  passed &= Require(
      api::QowCanonicalTypeFromDescriptorV1(
          legacy_descriptor("unknown_type")) == dt::CanonicalTypeId::unknown,
      "legacy unknown descriptor resolved to substitute text semantics");
  passed &= Require(
      api::QowCanonicalTypeFromDescriptorV1(legacy_descriptor("")) ==
          dt::CanonicalTypeId::unknown,
      "legacy missing descriptor resolved to substitute text semantics");
  auto domain = legacy_descriptor("domain");
  domain.descriptor_kind = "domain";
  domain.encoded_descriptor = "base_type=real64;nullability=nullable";
  passed &= Require(
      api::QowCanonicalTypeFromDescriptorV1(domain) ==
          dt::CanonicalTypeId::real64,
      "legacy domain descriptor did not resolve its canonical base type");
  return passed;
}

}  // namespace

// QOW-TEST-QRY-028-V1
int main() {
  bool passed = true;
  passed &= ValidateSupportedCoercion();
  passed &= ValidateRefusals();
  passed &= ValidateLegacyDescriptorResolution();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
