// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "scratchbird/engine/value.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

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

engine::ExecutionTypeDescriptor Descriptor(std::uint8_t seed,
                                           std::string_view name) {
  engine::ExecutionTypeDescriptor descriptor;
  descriptor.descriptor_uuid = Uuid(seed);
  descriptor.descriptor_epoch = 19;
  descriptor.canonical_type_id = seed;
  descriptor.family = engine::ExecutionTypeFamily::character;
  descriptor.width_class = engine::ExecutionTypeWidthClass::variable;
  descriptor.stable_name = std::string(name);
  return descriptor;
}

engine::ResultColumnDescriptor Column(std::uint32_t ordinal,
                                      std::uint8_t seed,
                                      std::string_view name) {
  return {ordinal,
          Descriptor(seed, name),
          std::string(name),
          std::string(name),
          std::string(name),
          true};
}

engine::ExecutionRelationDescriptor Relation(engine::ExecutionRelationKind kind,
                                             std::uint8_t seed,
                                             std::string_view name) {
  engine::ExecutionRelationDescriptor relation;
  relation.relation_descriptor_uuid = Uuid(seed);
  relation.descriptor_epoch = 19;
  relation.relation_kind = kind;
  relation.stable_name = std::string(name);
  relation.columns.push_back(Column(0, static_cast<std::uint8_t>(seed + 1),
                                    "payload"));
  relation.snapshot_uuid = Uuid(static_cast<std::uint8_t>(seed + 2));
  relation.security_context_required = true;
  relation.security_policy_uuid = Uuid(static_cast<std::uint8_t>(seed + 3));
  relation.memory_policy_uuid = Uuid(static_cast<std::uint8_t>(seed + 4));
  relation.memory_policy_epoch = 19;
  return relation;
}

engine::ExecutionRoutineParameterDescriptor ScalarParameter(
    std::uint32_t ordinal,
    std::string_view name) {
  engine::ExecutionRoutineParameterDescriptor parameter;
  parameter.ordinal = ordinal;
  parameter.stable_name = std::string(name);
  parameter.direction = engine::ExecutionRoutineParameterDirection::in;
  parameter.parameter_kind = engine::ExecutionRoutineParameterKind::scalar;
  parameter.scalar_descriptor = Descriptor(0x10, name);
  parameter.has_default = true;
  parameter.default_descriptor_authoritative = true;
  return parameter;
}

engine::ExecutionRoutineParameterDescriptor RelationParameter(
    std::uint32_t ordinal,
    std::string_view name,
    engine::ExecutionRoutineParameterDirection direction,
    engine::ExecutionRoutineParameterKind parameter_kind,
    engine::ExecutionRelationKind relation_kind,
    std::uint8_t seed) {
  engine::ExecutionRoutineParameterDescriptor parameter;
  parameter.ordinal = ordinal;
  parameter.stable_name = std::string(name);
  parameter.direction = direction;
  parameter.parameter_kind = parameter_kind;
  parameter.relation_descriptor = Relation(relation_kind, seed, name);
  return parameter;
}

engine::ExecutionRoutineResultDescriptor Result(std::uint32_t ordinal,
                                                std::string_view name,
                                                std::uint8_t seed) {
  engine::ExecutionRoutineResultDescriptor result;
  result.ordinal = ordinal;
  result.stable_name = std::string(name);
  result.relation_descriptor =
      Relation(engine::ExecutionRelationKind::result_channel, seed, name);
  result.default_result = ordinal == 0;
  return result;
}

engine::ExecutionRoutineSignatureDescriptor ValidSignature(
    engine::ExecutionRoutineKind routine_kind) {
  engine::ExecutionRoutineSignatureDescriptor signature;
  signature.routine_signature_uuid = Uuid(0x60);
  signature.signature_epoch = 19;
  signature.routine_uuid = Uuid(0x70);
  signature.routine_kind = routine_kind;
  signature.stable_name = "edr019.routine.signature";
  signature.parameter_descriptors.push_back(ScalarParameter(0, "name_filter"));
  signature.parameter_descriptors.push_back(RelationParameter(
      1, "cursor_arg", engine::ExecutionRoutineParameterDirection::inout,
      engine::ExecutionRoutineParameterKind::cursor,
      engine::ExecutionRelationKind::cursor, 0x80));
  signature.parameter_descriptors.push_back(RelationParameter(
      2, "rowset_out", engine::ExecutionRoutineParameterDirection::out,
      engine::ExecutionRoutineParameterKind::rowset,
      engine::ExecutionRelationKind::rowset, 0x90));
  signature.parameter_descriptors.push_back(RelationParameter(
      3, "source_table", engine::ExecutionRoutineParameterDirection::in,
      engine::ExecutionRoutineParameterKind::table_value,
      engine::ExecutionRelationKind::table_value, 0xa0));
  signature.result_descriptors.push_back(Result(0, "rows", 0xb0));
  signature.result_descriptors.push_back(Result(1, "diagnostics", 0xc0));
  return signature;
}

std::vector<engine::ExecutionRoutineCallArgumentDescriptor> ValidArguments(
    const engine::ExecutionRoutineSignatureDescriptor& signature) {
  std::vector<engine::ExecutionRoutineCallArgumentDescriptor> arguments;
  arguments.reserve(signature.parameter_descriptors.size());
  for (const auto& parameter : signature.parameter_descriptors) {
    engine::ExecutionRoutineCallArgumentDescriptor argument;
    argument.ordinal = parameter.ordinal;
    argument.stable_name = parameter.stable_name;
    argument.argument_kind = parameter.parameter_kind;
    argument.scalar_descriptor = parameter.scalar_descriptor;
    argument.relation_descriptor = parameter.relation_descriptor;
    arguments.push_back(argument);
  }
  return arguments;
}

void RequireStatus(
    const engine::ExecutionRoutineSignatureDescriptor& signature,
    engine::ExecutionRoutineSignatureDescriptorStatus expected,
    std::string_view message) {
  const auto result = engine::ValidateExecutionRoutineSignatureDescriptor(
      signature);
  Require(!result.ok(), message);
  Require(result.status == expected,
          "EDR-019 routine signature validation status mismatch");
}

void RequireCallStatus(
    const engine::ExecutionRoutineSignatureDescriptor& signature,
    const std::vector<engine::ExecutionRoutineCallArgumentDescriptor>&
        arguments,
    const std::vector<engine::ExecutionRoutineResultDescriptor>& results,
    engine::ExecutionRoutineCallValidationStatus expected,
    std::string_view message) {
  const auto result = engine::ValidateExecutionRoutineCallAgainstSignature(
      signature, arguments, results);
  Require(!result.ok(), message);
  Require(result.status == expected,
          "EDR-019 routine call validation status mismatch");
}

void TestValidRoutineSignatures() {
  const auto procedure =
      ValidSignature(engine::ExecutionRoutineKind::procedure);
  const auto function = ValidSignature(engine::ExecutionRoutineKind::function);
  Require(engine::ValidateExecutionRoutineSignatureDescriptor(procedure).ok(),
          "EDR-019 rejected valid procedure signature descriptor");
  Require(engine::ValidateExecutionRoutineSignatureDescriptor(function).ok(),
          "EDR-019 rejected valid function signature descriptor");

  const auto arguments = ValidArguments(procedure);
  Require(engine::ValidateExecutionRoutineCallAgainstSignature(
              procedure, arguments, procedure.result_descriptors)
              .ok(),
          "EDR-019 rejected valid routine call descriptor frame");
}

void TestSignatureIdentityFailures() {
  auto signature = ValidSignature(engine::ExecutionRoutineKind::procedure);
  signature.routine_signature_uuid = {};
  RequireStatus(signature,
                engine::ExecutionRoutineSignatureDescriptorStatus::
                    signature_uuid_required,
                "EDR-019 accepted routine signature without UUID");

  signature = ValidSignature(engine::ExecutionRoutineKind::procedure);
  signature.signature_epoch = 0;
  RequireStatus(signature,
                engine::ExecutionRoutineSignatureDescriptorStatus::
                    signature_epoch_required,
                "EDR-019 accepted routine signature without epoch");

  signature = ValidSignature(engine::ExecutionRoutineKind::procedure);
  signature.routine_uuid = {};
  RequireStatus(signature,
                engine::ExecutionRoutineSignatureDescriptorStatus::
                    routine_uuid_required,
                "EDR-019 accepted routine signature without routine UUID");

  signature = ValidSignature(engine::ExecutionRoutineKind::procedure);
  signature.stable_name.clear();
  RequireStatus(signature,
                engine::ExecutionRoutineSignatureDescriptorStatus::
                    stable_name_required,
                "EDR-019 accepted routine signature without stable name");

  signature = ValidSignature(engine::ExecutionRoutineKind::procedure);
  signature.descriptor_authoritative = false;
  RequireStatus(signature,
                engine::ExecutionRoutineSignatureDescriptorStatus::
                    descriptor_not_authoritative,
                "EDR-019 accepted non-authoritative routine signature");

  signature = ValidSignature(engine::ExecutionRoutineKind::procedure);
  signature.parser_independent = false;
  RequireStatus(signature,
                engine::ExecutionRoutineSignatureDescriptorStatus::
                    descriptor_parser_dependent,
                "EDR-019 accepted parser-dependent routine signature");

  signature = ValidSignature(engine::ExecutionRoutineKind::procedure);
  signature.routine_kind = static_cast<engine::ExecutionRoutineKind>(0xff);
  RequireStatus(
      signature,
      engine::ExecutionRoutineSignatureDescriptorStatus::routine_kind_invalid,
      "EDR-019 accepted invalid routine kind");
}

void TestParameterDescriptorFailures() {
  auto signature = ValidSignature(engine::ExecutionRoutineKind::procedure);
  signature.parameter_descriptors[1].ordinal = 9;
  RequireStatus(signature,
                engine::ExecutionRoutineSignatureDescriptorStatus::
                    parameter_ordinal_mismatch,
                "EDR-019 accepted non-canonical parameter ordinal");

  signature = ValidSignature(engine::ExecutionRoutineKind::procedure);
  signature.parameter_descriptors[0].stable_name.clear();
  RequireStatus(signature,
                engine::ExecutionRoutineSignatureDescriptorStatus::
                    parameter_name_required,
                "EDR-019 accepted unnamed routine parameter");

  signature = ValidSignature(engine::ExecutionRoutineKind::procedure);
  signature.parameter_descriptors[0].direction =
      static_cast<engine::ExecutionRoutineParameterDirection>(0xff);
  RequireStatus(signature,
                engine::ExecutionRoutineSignatureDescriptorStatus::
                    parameter_direction_invalid,
                "EDR-019 accepted invalid routine parameter direction");

  signature = ValidSignature(engine::ExecutionRoutineKind::procedure);
  signature.parameter_descriptors[0].parameter_kind =
      static_cast<engine::ExecutionRoutineParameterKind>(0xff);
  RequireStatus(signature,
                engine::ExecutionRoutineSignatureDescriptorStatus::
                    parameter_kind_invalid,
                "EDR-019 accepted invalid routine parameter kind");

  signature = ValidSignature(engine::ExecutionRoutineKind::procedure);
  signature.parameter_descriptors[0].scalar_descriptor.descriptor_uuid = {};
  const auto scalar_result =
      engine::ValidateExecutionRoutineSignatureDescriptor(signature);
  Require(!scalar_result.ok(),
          "EDR-019 accepted invalid scalar parameter descriptor");
  Require(scalar_result.status ==
              engine::ExecutionRoutineSignatureDescriptorStatus::
                  scalar_parameter_descriptor_invalid,
          "EDR-019 scalar parameter failure status mismatch");
  Require(scalar_result.scalar_descriptor_status ==
              engine::ExecutionDataPacketStatus::descriptor_missing_uuid,
          "EDR-019 scalar descriptor diagnostic was not preserved");
  Require(scalar_result.parameter_index == 0,
          "EDR-019 scalar parameter index was not preserved");

  signature = ValidSignature(engine::ExecutionRoutineKind::procedure);
  signature.parameter_descriptors[1].relation_descriptor
      .relation_descriptor_uuid = {};
  const auto relation_result =
      engine::ValidateExecutionRoutineSignatureDescriptor(signature);
  Require(!relation_result.ok(),
          "EDR-019 accepted invalid relation parameter descriptor");
  Require(relation_result.status ==
              engine::ExecutionRoutineSignatureDescriptorStatus::
                  relation_parameter_descriptor_invalid,
          "EDR-019 relation parameter failure status mismatch");
  Require(relation_result.relation_status ==
              engine::ExecutionRelationDescriptorStatus::descriptor_uuid_required,
          "EDR-019 relation descriptor diagnostic was not preserved");
  Require(relation_result.parameter_index == 1,
          "EDR-019 relation parameter index was not preserved");

  signature = ValidSignature(engine::ExecutionRoutineKind::procedure);
  signature.parameter_descriptors[1].relation_descriptor.relation_kind =
      engine::ExecutionRelationKind::rowset;
  RequireStatus(signature,
                engine::ExecutionRoutineSignatureDescriptorStatus::
                    relation_parameter_kind_mismatch,
                "EDR-019 accepted cursor parameter with rowset descriptor");
}

void TestDefaultAndResultFailures() {
  auto signature = ValidSignature(engine::ExecutionRoutineKind::procedure);
  signature.parameter_descriptors[2].has_default = true;
  RequireStatus(signature,
                engine::ExecutionRoutineSignatureDescriptorStatus::
                    out_parameter_default_forbidden,
                "EDR-019 accepted default on OUT-only parameter");

  signature = ValidSignature(engine::ExecutionRoutineKind::procedure);
  signature.parameter_descriptors[0].default_descriptor_authoritative = false;
  RequireStatus(signature,
                engine::ExecutionRoutineSignatureDescriptorStatus::
                    default_descriptor_required,
                "EDR-019 accepted non-authoritative default descriptor");

  signature = ValidSignature(engine::ExecutionRoutineKind::procedure);
  signature.result_descriptors[1].ordinal = 7;
  RequireStatus(signature,
                engine::ExecutionRoutineSignatureDescriptorStatus::
                    result_ordinal_mismatch,
                "EDR-019 accepted non-canonical result ordinal");

  signature = ValidSignature(engine::ExecutionRoutineKind::procedure);
  signature.result_descriptors[0].stable_name.clear();
  RequireStatus(signature,
                engine::ExecutionRoutineSignatureDescriptorStatus::
                    result_name_required,
                "EDR-019 accepted unnamed result channel");

  signature = ValidSignature(engine::ExecutionRoutineKind::procedure);
  signature.result_descriptors[0].relation_descriptor.relation_descriptor_uuid =
      {};
  const auto invalid_result =
      engine::ValidateExecutionRoutineSignatureDescriptor(signature);
  Require(!invalid_result.ok(),
          "EDR-019 accepted invalid result relation descriptor");
  Require(invalid_result.status ==
              engine::ExecutionRoutineSignatureDescriptorStatus::
                  result_relation_descriptor_invalid,
          "EDR-019 result relation failure status mismatch");
  Require(invalid_result.relation_status ==
              engine::ExecutionRelationDescriptorStatus::descriptor_uuid_required,
          "EDR-019 result relation diagnostic was not preserved");
  Require(invalid_result.result_index == 0,
          "EDR-019 result relation index was not preserved");

  signature = ValidSignature(engine::ExecutionRoutineKind::procedure);
  signature.result_descriptors[0].relation_descriptor.relation_kind =
      engine::ExecutionRelationKind::rowset;
  RequireStatus(signature,
                engine::ExecutionRoutineSignatureDescriptorStatus::
                    result_relation_kind_invalid,
                "EDR-019 accepted non-result-channel result relation");

  signature = ValidSignature(engine::ExecutionRoutineKind::function);
  signature.result_descriptors.clear();
  RequireStatus(signature,
                engine::ExecutionRoutineSignatureDescriptorStatus::
                    function_result_required,
                "EDR-019 accepted function signature without result channel");
}

void TestRoutineCallFailures() {
  auto signature = ValidSignature(engine::ExecutionRoutineKind::procedure);
  auto arguments = ValidArguments(signature);
  auto results = signature.result_descriptors;

  auto invalid_signature = signature;
  invalid_signature.routine_signature_uuid = {};
  const auto invalid_signature_result =
      engine::ValidateExecutionRoutineCallAgainstSignature(
          invalid_signature, arguments, results);
  Require(!invalid_signature_result.ok(),
          "EDR-019 routine call accepted invalid signature descriptor");
  Require(invalid_signature_result.status ==
              engine::ExecutionRoutineCallValidationStatus::
                  signature_descriptor_invalid,
          "EDR-019 routine call signature failure status mismatch");
  Require(invalid_signature_result.signature_status ==
              engine::ExecutionRoutineSignatureDescriptorStatus::
                  signature_uuid_required,
          "EDR-019 routine call signature diagnostic was not preserved");

  auto short_arguments = arguments;
  short_arguments.pop_back();
  RequireCallStatus(
      signature, short_arguments, results,
      engine::ExecutionRoutineCallValidationStatus::argument_count_mismatch,
      "EDR-019 routine call accepted missing argument descriptor slot");

  signature = ValidSignature(engine::ExecutionRoutineKind::procedure);
  arguments = ValidArguments(signature);
  results = signature.result_descriptors;
  signature.parameter_descriptors[0].has_default = false;
  arguments[0].supplied = false;
  RequireCallStatus(
      signature, arguments, results,
      engine::ExecutionRoutineCallValidationStatus::missing_required_argument,
      "EDR-019 routine call accepted missing required argument");

  signature = ValidSignature(engine::ExecutionRoutineKind::procedure);
  arguments = ValidArguments(signature);
  results = signature.result_descriptors;
  signature.parameter_descriptors[0].has_default = false;
  arguments[0].default_requested = true;
  RequireCallStatus(
      signature, arguments, results,
      engine::ExecutionRoutineCallValidationStatus::default_argument_not_allowed,
      "EDR-019 routine call accepted default for non-defaulted argument");

  signature = ValidSignature(engine::ExecutionRoutineKind::procedure);
  arguments = ValidArguments(signature);
  results = signature.result_descriptors;
  arguments[1].argument_kind = engine::ExecutionRoutineParameterKind::rowset;
  RequireCallStatus(
      signature, arguments, results,
      engine::ExecutionRoutineCallValidationStatus::argument_kind_mismatch,
      "EDR-019 routine call accepted argument kind mismatch");

  signature = ValidSignature(engine::ExecutionRoutineKind::procedure);
  arguments = ValidArguments(signature);
  results = signature.result_descriptors;
  arguments[0].scalar_descriptor.descriptor_uuid = {};
  const auto invalid_scalar =
      engine::ValidateExecutionRoutineCallAgainstSignature(
          signature, arguments, results);
  Require(!invalid_scalar.ok(),
          "EDR-019 routine call accepted invalid scalar argument descriptor");
  Require(invalid_scalar.status ==
              engine::ExecutionRoutineCallValidationStatus::
                  scalar_argument_descriptor_invalid,
          "EDR-019 scalar argument status mismatch");
  Require(invalid_scalar.scalar_descriptor_status ==
              engine::ExecutionDataPacketStatus::descriptor_missing_uuid,
          "EDR-019 scalar argument diagnostic was not preserved");

  signature = ValidSignature(engine::ExecutionRoutineKind::procedure);
  arguments = ValidArguments(signature);
  results = signature.result_descriptors;
  arguments[1].relation_descriptor.relation_kind =
      engine::ExecutionRelationKind::rowset;
  RequireCallStatus(
      signature, arguments, results,
      engine::ExecutionRoutineCallValidationStatus::
          relation_argument_kind_mismatch,
      "EDR-019 routine call accepted relation argument kind mismatch");

  signature = ValidSignature(engine::ExecutionRoutineKind::procedure);
  arguments = ValidArguments(signature);
  results = signature.result_descriptors;
  arguments[1].relation_descriptor.descriptor_epoch += 1;
  RequireCallStatus(
      signature, arguments, results,
      engine::ExecutionRoutineCallValidationStatus::
          relation_argument_descriptor_mismatch,
      "EDR-019 routine call accepted relation descriptor identity mismatch");

  signature = ValidSignature(engine::ExecutionRoutineKind::procedure);
  arguments = ValidArguments(signature);
  results = signature.result_descriptors;
  results.pop_back();
  RequireCallStatus(
      signature, arguments, results,
      engine::ExecutionRoutineCallValidationStatus::result_count_mismatch,
      "EDR-019 routine call accepted missing result channel descriptor");

  signature = ValidSignature(engine::ExecutionRoutineKind::procedure);
  arguments = ValidArguments(signature);
  results = signature.result_descriptors;
  results[0].relation_descriptor.descriptor_epoch += 1;
  RequireCallStatus(
      signature, arguments, results,
      engine::ExecutionRoutineCallValidationStatus::result_descriptor_mismatch,
      "EDR-019 routine call accepted result descriptor identity mismatch");
}

}  // namespace

int main() {
  TestValidRoutineSignatures();
  TestSignatureIdentityFailures();
  TestParameterDescriptorFailures();
  TestDefaultAndResultFailures();
  TestRoutineCallFailures();
  return EXIT_SUCCESS;
}
