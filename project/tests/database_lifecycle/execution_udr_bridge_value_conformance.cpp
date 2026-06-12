// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "scratchbird/engine/value.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace engine = scratchbird::engine;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

engine::Uuid Uuid(std::uint8_t seed) {
  engine::Uuid uuid;
  for (std::size_t index = 0; index < 16; ++index) {
    uuid.bytes[index] = static_cast<std::uint8_t>(seed + index);
  }
  return uuid;
}

engine::ExecutionTypeDescriptor Descriptor(
    std::uint8_t seed,
    engine::ExecutionTypeFamily family = engine::ExecutionTypeFamily::signed_integer) {
  engine::ExecutionTypeDescriptor descriptor;
  descriptor.descriptor_uuid = Uuid(seed);
  descriptor.descriptor_epoch = 17;
  descriptor.canonical_type_id = seed;
  descriptor.family = family;
  descriptor.width_class = engine::ExecutionTypeWidthClass::fixed;
  descriptor.stable_name = "udr-bridge-value-fixture";
  descriptor.bit_width = 64;
  return descriptor;
}

engine::ExecutionUdrTypeMapping TypeMapping(
    const engine::ExecutionTypeDescriptor& descriptor) {
  engine::ExecutionUdrTypeMapping mapping;
  mapping.mapping_name = "cpp.udr.int64.value";
  mapping.mapping_version = "sb_udr_v1";
  mapping.descriptor = descriptor;
  mapping.cpp_abi = true;
  mapping.descriptor_preserving = true;
  mapping.parser_independent = true;
  return mapping;
}

engine::ExecutionUdrBridgeValue ValidBridge(
    engine::ExecutionUdrValueDirection direction =
        engine::ExecutionUdrValueDirection::input,
    engine::ExecutionValueState value_state = engine::ExecutionValueState::value) {
  engine::ExecutionUdrBridgeValue value;
  value.runtime_kind = engine::ExecutionUdrRuntimeKind::cpp;
  value.direction = direction;
  value.value_state = value_state;
  value.descriptor = Descriptor(0x10);
  value.value_uuid = Uuid(0x40);
  value.type_mapping = TypeMapping(value.descriptor);
  value.descriptor_preserved = true;
  value.payload_present = engine::ExecutionValueStateHasPayload(value_state);
  value.parser_independent = true;
  return value;
}

void RequireOk(const engine::ExecutionUdrBridgeValue& value,
               std::string_view message) {
  const auto result = engine::ValidateExecutionUdrBridgeValue(value);
  Require(result.ok(), message);
}

void RequireStatus(const engine::ExecutionUdrBridgeValue& value,
                   engine::ExecutionUdrBridgeValueStatus expected,
                   std::string_view message) {
  const auto result = engine::ValidateExecutionUdrBridgeValue(value);
  Require(!result.ok(), message);
  Require(result.status == expected,
          "EDR-011 UDR bridge value validation status mismatch");
}

void TestAcceptedCppBridgeValues() {
  RequireOk(ValidBridge(engine::ExecutionUdrValueDirection::input,
                        engine::ExecutionValueState::value),
            "EDR-011 rejected C++ UDR input value");
  RequireOk(ValidBridge(engine::ExecutionUdrValueDirection::input,
                        engine::ExecutionValueState::sql_null),
            "EDR-011 rejected C++ UDR input SQL null");
  RequireOk(ValidBridge(engine::ExecutionUdrValueDirection::output,
                        engine::ExecutionValueState::error),
            "EDR-011 rejected C++ UDR output error value");
  RequireOk(ValidBridge(engine::ExecutionUdrValueDirection::inout,
                        engine::ExecutionValueState::lob_handle),
            "EDR-011 rejected C++ UDR LOB handle bridge value");
  RequireOk(ValidBridge(engine::ExecutionUdrValueDirection::inout,
                        engine::ExecutionValueState::protected_value),
            "EDR-011 rejected C++ UDR protected bridge value");
}

void TestDescriptorAndValueIdentityFailures() {
  auto value = ValidBridge();
  value.descriptor.descriptor_epoch = 0;
  const auto descriptor_result = engine::ValidateExecutionUdrBridgeValue(value);
  Require(!descriptor_result.ok(), "EDR-011 accepted invalid bridge descriptor");
  Require(descriptor_result.status ==
              engine::ExecutionUdrBridgeValueStatus::descriptor_invalid,
          "EDR-011 descriptor status mismatch");
  Require(descriptor_result.descriptor_status ==
              engine::ExecutionDataPacketStatus::descriptor_missing_epoch,
          "EDR-011 descriptor diagnostic was not preserved");

  value = ValidBridge();
  value.value_uuid = {};
  RequireStatus(value,
                engine::ExecutionUdrBridgeValueStatus::value_uuid_required,
                "EDR-011 accepted bridge value without value UUID");

  value = ValidBridge();
  value.descriptor_preserved = false;
  RequireStatus(value,
                engine::ExecutionUdrBridgeValueStatus::descriptor_not_preserved,
                "EDR-011 accepted bridge value without descriptor preservation");
}

void TestRuntimeAndTypeMappingFailures() {
  auto value = ValidBridge();
  value.runtime_kind = engine::ExecutionUdrRuntimeKind::non_cpp;
  RequireStatus(value,
                engine::ExecutionUdrBridgeValueStatus::non_cpp_runtime_prohibited,
                "EDR-011 accepted non-C++ UDR bridge runtime");

  value = ValidBridge();
  value.runtime_kind = engine::ExecutionUdrRuntimeKind::unknown;
  RequireStatus(value,
                engine::ExecutionUdrBridgeValueStatus::runtime_not_cpp,
                "EDR-011 accepted unknown UDR bridge runtime");

  value = ValidBridge();
  value.type_mapping.mapping_name.clear();
  RequireStatus(value,
                engine::ExecutionUdrBridgeValueStatus::type_mapping_required,
                "EDR-011 accepted bridge value without type mapping name");

  value = ValidBridge();
  value.type_mapping.mapping_version.clear();
  RequireStatus(
      value,
      engine::ExecutionUdrBridgeValueStatus::type_mapping_version_required,
      "EDR-011 accepted bridge value without type mapping version");

  value = ValidBridge();
  value.type_mapping.descriptor.descriptor_epoch = 0;
  const auto mapping_descriptor_result =
      engine::ValidateExecutionUdrBridgeValue(value);
  Require(!mapping_descriptor_result.ok(),
          "EDR-011 accepted invalid type mapping descriptor");
  Require(mapping_descriptor_result.status ==
              engine::ExecutionUdrBridgeValueStatus::
                  type_mapping_descriptor_invalid,
          "EDR-011 type mapping descriptor status mismatch");
  Require(mapping_descriptor_result.type_mapping_descriptor_status ==
              engine::ExecutionDataPacketStatus::descriptor_missing_epoch,
          "EDR-011 type mapping descriptor diagnostic was not preserved");

  value = ValidBridge();
  value.type_mapping.descriptor.descriptor_uuid = Uuid(0x70);
  RequireStatus(
      value,
      engine::ExecutionUdrBridgeValueStatus::type_mapping_descriptor_mismatch,
      "EDR-011 accepted type mapping descriptor identity mismatch");

  value = ValidBridge();
  value.type_mapping.cpp_abi = false;
  RequireStatus(value,
                engine::ExecutionUdrBridgeValueStatus::type_mapping_not_cpp_abi,
                "EDR-011 accepted non-C++ type mapping ABI");

  value = ValidBridge();
  value.type_mapping.descriptor_preserving = false;
  RequireStatus(
      value,
      engine::ExecutionUdrBridgeValueStatus::
          type_mapping_not_descriptor_preserving,
      "EDR-011 accepted non-preserving type mapping");

  value = ValidBridge();
  value.type_mapping.parser_independent = false;
  RequireStatus(
      value,
      engine::ExecutionUdrBridgeValueStatus::type_mapping_parser_dependent,
      "EDR-011 accepted parser-dependent UDR type mapping");
}

void TestStatePayloadAndDirectionFailures() {
  auto value = ValidBridge();
  value.value_state = static_cast<engine::ExecutionValueState>(0xff);
  RequireStatus(value,
                engine::ExecutionUdrBridgeValueStatus::value_state_invalid,
                "EDR-011 accepted invalid bridge value-state code");

  value = ValidBridge(engine::ExecutionUdrValueDirection::input,
                      engine::ExecutionValueState::error);
  RequireStatus(value,
                engine::ExecutionUdrBridgeValueStatus::input_value_state_invalid,
                "EDR-011 accepted error value as UDR input");

  value = ValidBridge(engine::ExecutionUdrValueDirection::input,
                      engine::ExecutionValueState::value);
  value.payload_present = false;
  RequireStatus(value,
                engine::ExecutionUdrBridgeValueStatus::input_payload_required,
                "EDR-011 accepted input value without payload");

  value = ValidBridge(engine::ExecutionUdrValueDirection::output,
                      engine::ExecutionValueState::default_requested);
  RequireStatus(
      value,
      engine::ExecutionUdrBridgeValueStatus::output_value_state_invalid,
      "EDR-011 accepted default request as UDR output value");

  value = ValidBridge(engine::ExecutionUdrValueDirection::output,
                      engine::ExecutionValueState::error);
  value.payload_present = false;
  RequireStatus(value,
                engine::ExecutionUdrBridgeValueStatus::output_payload_required,
                "EDR-011 accepted output error without payload");

  value = ValidBridge();
  value.parser_independent = false;
  RequireStatus(value,
                engine::ExecutionUdrBridgeValueStatus::parser_dependent,
                "EDR-011 accepted parser-dependent bridge value");

  value = ValidBridge();
  value.direction = static_cast<engine::ExecutionUdrValueDirection>(0xff);
  RequireStatus(value,
                engine::ExecutionUdrBridgeValueStatus::direction_invalid,
                "EDR-011 accepted invalid UDR value direction");
}

}  // namespace

int main() {
  TestAcceptedCppBridgeValues();
  TestDescriptorAndValueIdentityFailures();
  TestRuntimeAndTypeMappingFailures();
  TestStatePayloadAndDirectionFailures();
  return EXIT_SUCCESS;
}
