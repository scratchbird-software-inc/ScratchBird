// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "query/projection_api.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>
#include <utility>
#include <vector>

namespace api = scratchbird::engine::internal_api;

namespace scratchbird::engine::internal_api {
bool QowBindCanonicalParameterSlotsV1(
    const std::vector<EngineDescriptor>& parameter_descriptors,
    const std::vector<std::pair<std::string, EngineTypedValue>>& supplied_parameters,
    std::vector<EngineTypedValue>* bound_values,
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
                                 const bool nullable) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical =
      "019f0000-0000-7200-8000-0000000026" +
      std::string(suffix < 10 ? "0" : "") + std::to_string(suffix);
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = std::move(type);
  descriptor.encoded_descriptor =
      "type=" + descriptor.canonical_type_name + ";nullability=" +
      (nullable ? "nullable" : "non_null");
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

api::EngineTypedValue NullValue(const api::EngineDescriptor& descriptor) {
  api::EngineTypedValue value;
  value.descriptor = descriptor;
  value.is_null = true;
  value.state = api::EngineValueState::sql_null;
  return value;
}

struct Fixture {
  std::vector<api::EngineDescriptor> descriptors;
  std::vector<std::pair<std::string, api::EngineTypedValue>> parameters;
};

Fixture CanonicalFixture() {
  Fixture fixture;
  fixture.descriptors.push_back(Descriptor(1, "int64", false));
  fixture.descriptors.push_back(Descriptor(2, "character", true));
  fixture.parameters.push_back(
      {"minimum_order_id", Value(fixture.descriptors[0], "41")});
  fixture.parameters.push_back(
      {"optional_label", NullValue(fixture.descriptors[1])});
  return fixture;
}

bool Bind(const Fixture& fixture,
          std::vector<api::EngineTypedValue>* values,
          std::string* reason,
          std::string* detail) {
  return api::QowBindCanonicalParameterSlotsV1(
      fixture.descriptors, fixture.parameters, values, reason, detail);
}

bool AtomicRefusal(const Fixture& fixture,
                   const std::string_view expected_reason) {
  std::vector<api::EngineTypedValue> values = {
      Value(Descriptor(9, "int64", false), "999")};
  std::string reason;
  std::string detail;
  const bool ok = Bind(fixture, &values, &reason, &detail);
  return !ok && values.empty() && reason == expected_reason && !detail.empty();
}

bool ValidatePositiveBinding() {
  const auto fixture = CanonicalFixture();
  std::vector<api::EngineTypedValue> values;
  std::string reason;
  std::string detail;
  const bool ok = Bind(fixture, &values, &reason, &detail);
  return Require(ok, "canonical typed parameters were refused") &&
         Require(reason.empty() && detail.empty(),
                 "successful parameter binding emitted refusal state") &&
         Require(values.size() == 2 && values[0].encoded_value == "41" &&
                     values[0].descriptor.descriptor_uuid.canonical ==
                         fixture.descriptors[0].descriptor_uuid.canonical,
                 "non-NULL parameter descriptor/value identity was not preserved") &&
         Require(values[1].state == api::EngineValueState::sql_null &&
                     values[1].is_null && values[1].encoded_value.empty() &&
                     values[1].descriptor.descriptor_uuid.canonical ==
                         fixture.descriptors[1].descriptor_uuid.canonical,
                 "nullable parameter did not preserve canonical SQL NULL state");
}

bool ValidateRefusals() {
  bool passed = true;
  {
    auto fixture = CanonicalFixture();
    fixture.parameters.pop_back();
    passed &= Require(AtomicRefusal(fixture, "parameter_missing"),
                      "missing parameter did not refuse atomically");
  }
  {
    auto fixture = CanonicalFixture();
    fixture.parameters.push_back(
        {"extra", Value(Descriptor(3, "int64", false), "1")});
    passed &= Require(AtomicRefusal(fixture, "parameter_extra"),
                      "extra parameter did not refuse atomically");
  }
  {
    auto fixture = CanonicalFixture();
    fixture.parameters[0].second.descriptor = fixture.descriptors[1];
    passed &= Require(AtomicRefusal(fixture, "parameter_wrong_type"),
                      "wrong-type parameter did not refuse atomically");
  }
  {
    auto fixture = CanonicalFixture();
    fixture.parameters[0].second = NullValue(fixture.descriptors[0]);
    passed &= Require(AtomicRefusal(fixture, "parameter_null_invalid"),
                      "NULL-invalid parameter did not refuse atomically");
  }
  {
    auto fixture = CanonicalFixture();
    fixture.parameters[1].first = fixture.parameters[0].first;
    passed &= Require(AtomicRefusal(fixture, "parameter_name_invalid"),
                      "duplicate parameter name did not refuse atomically");
  }
  {
    auto fixture = CanonicalFixture();
    fixture.parameters[0].second.state = api::EngineValueState::missing;
    passed &= Require(
        AtomicRefusal(fixture, "parameter_value_state_invalid"),
        "missing-state parameter acquired a substitute value");
  }
  return passed;
}

}  // namespace

// QOW-TEST-QRY-026-V1
int main() {
  bool passed = true;
  passed &= ValidatePositiveBinding();
  passed &= ValidateRefusals();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
