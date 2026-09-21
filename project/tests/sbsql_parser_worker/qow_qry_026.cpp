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
bool QowReadCanonicalProjectionExpressionsV1(
    const EngineApiRequest&, std::uint64_t, std::vector<EngineProjectionExpression>*,
    std::size_t*, std::size_t*, std::string*, std::string*);
bool QowBindCanonicalParameterSlotsV1(
    const std::vector<EngineDescriptor>& parameter_descriptors,
    const std::vector<std::pair<std::string, EngineTypedValue>>& supplied_parameters,
    std::vector<EngineTypedValue>* bound_values,
    std::string* refusal_reason,
    std::string* refusal_detail);
}  // namespace scratchbird::engine::internal_api

namespace {

unsigned checks = 0;
bool Require(const bool condition, const std::string_view message) {
  ++checks;
  if (!condition) std::cerr << message << '\n';
  return condition;
}

api::EngineDescriptor Descriptor(const unsigned suffix,
                                 std::string type,
                                 const bool nullable) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.bytes =
      {0x01, 0x9f, 0, 0, 0, 0, 0x72, 0, 0x80, 0, 0, 0, 0, 0, 0x26,
       static_cast<std::uint8_t>(suffix)};
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = std::move(type);
  // Private fixture identities; this test supplies no catalog authority.
  descriptor.type_uuid.bytes =
      {0x01, 0x9f, 0, 0, 0, 0, 0x72, 0, 0x80, 0, 0, 0, 0, 0, 0x27,
       static_cast<std::uint8_t>(descriptor.canonical_type_name == "int64" ? 1 : 2)};
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
                     values[0].descriptor.descriptor_uuid ==
                         fixture.descriptors[0].descriptor_uuid,
                 "non-NULL parameter descriptor/value identity was not preserved") &&
         Require(values[1].state == api::EngineValueState::sql_null &&
                     values[1].is_null && values[1].encoded_value.empty() &&
                     values[1].descriptor.descriptor_uuid ==
                         fixture.descriptors[1].descriptor_uuid,
                 "nullable parameter did not preserve canonical SQL NULL state");
}

bool ValidateRefusals() {
  bool passed = true;
  {
    Fixture fixture;
    passed &= Require(AtomicRefusal(fixture, "parameter_slots_missing"),
                      "undeclared parameter acquired a substitute value");
  }
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

bool ValidateBinaryIdentity() {
  static_assert(sizeof(api::EngineUuid) == 16);
  bool passed = true;
  for (unsigned version = 0; version < 16; ++version) {
    auto fixture = CanonicalFixture();
    fixture.descriptors[0].descriptor_uuid.bytes[6] = static_cast<std::uint8_t>((version << 4) | 2);
    fixture.parameters[0].second.descriptor = fixture.descriptors[0];
    std::vector<api::EngineTypedValue> values;
    std::string reason, detail;
    const bool ok = Bind(fixture, &values, &reason, &detail);
    passed &= Require(ok == (version == 7), "descriptor authority requires UUIDv7, not a user UUID version");
    if (!ok) passed &= Require(values.empty() && reason == "parameter_descriptor_invalid", "invalid descriptor version published no parameters");
  }
  for (unsigned variant = 0; variant < 256; ++variant) {
    auto fixture = CanonicalFixture();
    fixture.descriptors[0].descriptor_uuid.bytes[8] = static_cast<std::uint8_t>(variant);
    fixture.parameters[0].second.descriptor = fixture.descriptors[0];
    std::vector<api::EngineTypedValue> values;
    std::string reason, detail;
    const bool ok = Bind(fixture, &values, &reason, &detail);
    passed &= Require(ok == ((variant & 0xc0) == 0x80), "descriptor validates the exact RFC variant bits");
  }
  {
    auto fixture = CanonicalFixture();
    fixture.descriptors[0].descriptor_uuid = {};
    fixture.parameters[0].second.descriptor = fixture.descriptors[0];
    passed &= Require(AtomicRefusal(fixture, "parameter_descriptor_invalid"), "nil descriptor cannot authorize a parameter");
  }
  for (unsigned index = 0; index < 16; ++index) {
    auto fixture = CanonicalFixture();
    fixture.parameters[0].second.descriptor.descriptor_uuid.bytes[index] ^= 1;
    passed &= Require(AtomicRefusal(fixture, "parameter_wrong_type"), "parameter descriptor comparison must retain every identity byte");
  }
  return passed;
}

bool ValidateCompleteDescriptor() {
  bool passed = true;
  for (unsigned field = 0; field < 2; ++field) {
    for (unsigned index = 0; index < 16; ++index) {
      auto fixture = CanonicalFixture();
      auto& descriptor = fixture.parameters[0].second.descriptor;
      auto& identity = field == 0 ? descriptor.type_uuid : descriptor.collation_uuid;
      identity.bytes[index] ^= 1;
      passed &= Require(AtomicRefusal(fixture, "parameter_wrong_type"), "type/collation substitution must not pass descriptor binding");
    }
    for (unsigned version = 0; version < 16; ++version) {
      auto fixture = CanonicalFixture();
      auto& descriptor = fixture.descriptors[0];
      auto& identity = field == 0 ? descriptor.type_uuid : descriptor.collation_uuid;
      identity = descriptor.descriptor_uuid;
      identity.bytes[6] = static_cast<std::uint8_t>((version << 4) | 1);
      fixture.parameters[0].second.descriptor = descriptor;
      std::vector<api::EngineTypedValue> values;
      std::string reason, detail;
      passed &= Require(Bind(fixture, &values, &reason, &detail) == (version == 7), "type and present collation identities require UUIDv7");
    }
  }
  auto fixture = CanonicalFixture();
  fixture.descriptors[0].type_uuid = {};
  fixture.parameters[0].second.descriptor = fixture.descriptors[0];
  passed &= Require(AtomicRefusal(fixture, "parameter_descriptor_invalid"), "missing type identity cannot authorize typed parameter execution");
  for (unsigned field = 0; field < 3; ++field) {
    auto mismatch = CanonicalFixture();
    auto& descriptor = mismatch.parameters[0].second.descriptor;
    if (field == 0) descriptor.descriptor_kind = "other";
    if (field == 1) descriptor.canonical_type_name = "uint64";
    if (field == 2) descriptor.encoded_descriptor += ";precision=12";
    passed &= Require(AtomicRefusal(mismatch, "parameter_wrong_type"), "complete descriptor metadata must remain bound");
  }
  return passed;
}

bool ValidateProjectionFunctionIdentities() {
  api::EngineApiRequest request;
  request.option_envelopes = {
      "projection_0_expr_kind:function", "projection_0_type:int64",
      "projection_0_function_arg_count:1", "projection_0_arg_0_expr_kind:function",
      "projection_0_arg_0_type:int64", "projection_0_arg_0_function_arg_count:0"};
  const auto outer = Descriptor(71, "int64", false).descriptor_uuid;
  const auto inner = Descriptor(72, "int64", false).descriptor_uuid;
  request.projection.function_identities = {{"projection_0_", outer}, {"projection_0_arg_0_", inner}};
  std::vector<api::EngineProjectionExpression> expressions;
  std::size_t nodes = 0, depth = 0;
  std::string reason, detail;
  const auto bind = [&](const auto& input) {
    return api::QowReadCanonicalProjectionExpressionsV1(input, 1, &expressions,
                                                       &nodes, &depth, &reason, &detail);
  };
  bool passed = Require(bind(request) && expressions.size() == 1 && nodes == 2 && depth == 2 &&
                           expressions[0].function_uuid == outer && expressions[0].function_id.empty() &&
                           expressions[0].arguments.size() == 1 &&
                           expressions[0].arguments[0].function_uuid == inner,
                       "nested function binding must consume binary UUIDs without function labels");
  const auto reject = [&](const auto& bad) {
    expressions.resize(1); nodes = depth = 99;
    return Require(!bind(bad) && expressions.empty() && nodes == 0 && depth == 0 && reason == "function_uuid",
                   "invalid function binding must reject the complete graph");
  };
  auto bad = request; bad.projection.function_identities.clear(); passed &= reject(bad);
  bad = request; bad.projection.function_identities.pop_back(); passed &= reject(bad);
  bad = request; bad.projection.function_identities.emplace_back("projection_2_", inner); passed &= reject(bad);
  bad = request; bad.projection.function_identities.push_back(bad.projection.function_identities.front()); passed &= reject(bad);
  for (unsigned slot = 0; slot != 2; ++slot) {
    bad = request; bad.projection.function_identities[slot].second = {}; passed &= reject(bad);
    for (unsigned version = 0; version != 16; ++version) {
      if (version == 7) continue;
      bad = request;
      bad.projection.function_identities[slot].second.bytes[6] = static_cast<std::uint8_t>(version << 4);
      passed &= reject(bad);
    }
  }
  for (const auto option : {"projection_0_function_uuid", "projection_0_function_uuid:",
                             "projection_0_arg_0_function_uuid:text"}) {
    bad = request; bad.option_envelopes.push_back(option); passed &= reject(bad);
  }
  bad = request; bad.projection.function_identities[1].first = "projection_0_arg_00_";
  passed &= reject(bad);
  return passed;
}

}  // namespace

// QOW-TEST-QRY-026-V1
int main() {
  bool passed = true;
  passed &= ValidatePositiveBinding();
  passed &= ValidateRefusals();
  passed &= ValidateBinaryIdentity();
  passed &= ValidateCompleteDescriptor();
  passed &= ValidateProjectionFunctionIdentities();
  std::cout << checks << " typed parameter binding checks: " << (passed ? "passed" : "failed") << '\n';
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
