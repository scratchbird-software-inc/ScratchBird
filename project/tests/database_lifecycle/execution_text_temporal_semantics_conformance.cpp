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

engine::Uuid TypedUuid(std::uint8_t seed) {
  engine::Uuid uuid;
  for (std::size_t index = 0; index < 16; ++index) {
    uuid.bytes[index] = static_cast<std::uint8_t>(seed + index);
  }
  return uuid;
}

engine::ExecutionTypeDescriptor Descriptor(
    std::uint8_t seed,
    std::string_view name,
    engine::ExecutionTypeFamily family) {
  engine::ExecutionTypeDescriptor descriptor;
  descriptor.descriptor_uuid = TypedUuid(seed);
  descriptor.descriptor_epoch = 7;
  descriptor.canonical_type_id = seed;
  descriptor.family = family;
  descriptor.width_class = engine::ExecutionTypeWidthClass::variable;
  descriptor.stable_name = std::string(name);
  descriptor.length = family == engine::ExecutionTypeFamily::character ? 64 : 0;
  return descriptor;
}

void AttachResource(engine::ExecutionTypeDescriptor* descriptor,
                    engine::ExecutionTypeModifierFlag flag,
                    engine::Uuid engine::ExecutionTypeDescriptor::*field,
                    std::uint8_t uuid_seed) {
  descriptor->modifier_flags |= engine::ExecutionTypeModifierFlagBit(flag);
  descriptor->*field = TypedUuid(uuid_seed);
}

engine::ExecutionTypeDescriptor TextDescriptor() {
  auto descriptor =
      Descriptor(0x10, "utf8-text", engine::ExecutionTypeFamily::character);
  AttachResource(&descriptor,
                 engine::ExecutionTypeModifierFlag::charset_uuid,
                 &engine::ExecutionTypeDescriptor::charset_uuid, 0x50);
  AttachResource(&descriptor,
                 engine::ExecutionTypeModifierFlag::collation_uuid,
                 &engine::ExecutionTypeDescriptor::collation_uuid, 0x60);
  return descriptor;
}

engine::ExecutionTypeDescriptor ZonedTemporalDescriptor() {
  auto descriptor =
      Descriptor(0x20, "timestamp-tz", engine::ExecutionTypeFamily::temporal);
  AttachResource(&descriptor,
                 engine::ExecutionTypeModifierFlag::timezone_uuid,
                 &engine::ExecutionTypeDescriptor::timezone_uuid, 0x70);
  return descriptor;
}

engine::ExecutionResourceBinding Binding(const engine::Uuid& uuid,
                                         std::string_view version) {
  engine::ExecutionResourceBinding binding;
  binding.resource_uuid = uuid;
  binding.activation_epoch = 1;
  binding.version_token = std::string(version);
  return binding;
}

engine::ExecutionTextTemporalSemanticsRequest TextRequest(
    engine::ExecutionTextTemporalOperation operation) {
  engine::ExecutionTextTemporalSemanticsRequest request;
  request.descriptor = TextDescriptor();
  request.operation = operation;
  request.charset = Binding(request.descriptor.charset_uuid, "charset-v1");
  request.collation = Binding(request.descriptor.collation_uuid, "collation-v1");
  return request;
}

engine::ExecutionTextTemporalSemanticsRequest TemporalRequest(
    engine::ExecutionTextTemporalOperation operation) {
  engine::ExecutionTextTemporalSemanticsRequest request;
  request.descriptor = ZonedTemporalDescriptor();
  request.operation = operation;
  request.timezone_policy = engine::ExecutionTimezonePolicy::
      descriptor_timezone_required;
  request.timezone = Binding(request.descriptor.timezone_uuid, "tzdb-v1");
  return request;
}

void RequireOk(const engine::ExecutionTextTemporalSemanticsRequest& request,
               std::string_view message) {
  const auto result = engine::ValidateExecutionTextTemporalSemantics(request);
  Require(result.ok(), message);
}

void RequireStatus(
    const engine::ExecutionTextTemporalSemanticsRequest& request,
    engine::ExecutionTextTemporalSemanticsStatus expected,
    std::string_view message) {
  const auto result = engine::ValidateExecutionTextTemporalSemantics(request);
  Require(!result.ok(), message);
  Require(result.status == expected,
          "EDR-008 text/temporal semantics status mismatch");
}

void TestDescriptorBoundTextSemantics() {
  RequireOk(TextRequest(engine::ExecutionTextTemporalOperation::text_render),
            "EDR-008 rejected descriptor-bound text rendering");
  RequireOk(TextRequest(engine::ExecutionTextTemporalOperation::text_compare),
            "EDR-008 rejected descriptor-bound text comparison");
  RequireOk(TextRequest(engine::ExecutionTextTemporalOperation::text_index_key),
            "EDR-008 rejected descriptor-bound text index key");

  auto request = TextRequest(engine::ExecutionTextTemporalOperation::text_render);
  request.descriptor.modifier_flags &=
      ~engine::ExecutionTypeModifierFlagBit(
          engine::ExecutionTypeModifierFlag::charset_uuid);
  RequireStatus(request,
                engine::ExecutionTextTemporalSemanticsStatus::charset_uuid_missing,
                "EDR-008 accepted text rendering without charset UUID");

  request = TextRequest(engine::ExecutionTextTemporalOperation::text_compare);
  request.descriptor.modifier_flags &=
      ~engine::ExecutionTypeModifierFlagBit(
          engine::ExecutionTypeModifierFlag::collation_uuid);
  RequireStatus(request,
                engine::ExecutionTextTemporalSemanticsStatus::collation_uuid_missing,
                "EDR-008 accepted text comparison without collation UUID");

  request = TextRequest(engine::ExecutionTextTemporalOperation::text_compare);
  request.collation.resource_uuid = TypedUuid(0x90);
  RequireStatus(
      request,
      engine::ExecutionTextTemporalSemanticsStatus::collation_binding_mismatch,
      "EDR-008 accepted mismatched collation binding");

  request = TextRequest(engine::ExecutionTextTemporalOperation::text_compare);
  request.charset.activation_epoch = 0;
  RequireStatus(request,
                engine::ExecutionTextTemporalSemanticsStatus::charset_epoch_required,
                "EDR-008 accepted charset binding without activation epoch");

  request = TextRequest(engine::ExecutionTextTemporalOperation::text_compare);
  request.collation.version_token.clear();
  RequireStatus(
      request,
      engine::ExecutionTextTemporalSemanticsStatus::collation_version_required,
      "EDR-008 accepted collation binding without version token");

  request = TextRequest(engine::ExecutionTextTemporalOperation::text_compare);
  request.collation.parser_dependent = true;
  RequireStatus(
      request,
      engine::ExecutionTextTemporalSemanticsStatus::collation_parser_dependent,
      "EDR-008 accepted parser-dependent collation binding");

  request = TextRequest(engine::ExecutionTextTemporalOperation::text_compare);
  request.charset.available = false;
  RequireStatus(request,
                engine::ExecutionTextTemporalSemanticsStatus::charset_unavailable,
                "EDR-008 accepted unavailable charset binding");

  request = TextRequest(engine::ExecutionTextTemporalOperation::text_compare);
  request.descriptor.family = engine::ExecutionTypeFamily::binary;
  RequireStatus(
      request,
      engine::ExecutionTextTemporalSemanticsStatus::descriptor_family_unsupported,
      "EDR-008 accepted text semantics for non-character descriptor");
}

void TestDescriptorBoundTemporalSemantics() {
  RequireOk(TemporalRequest(
                engine::ExecutionTextTemporalOperation::temporal_render),
            "EDR-008 rejected descriptor-bound temporal rendering");
  RequireOk(TemporalRequest(
                engine::ExecutionTextTemporalOperation::temporal_compare),
            "EDR-008 rejected descriptor-bound temporal comparison");
  RequireOk(TemporalRequest(
                engine::ExecutionTextTemporalOperation::temporal_index_key),
            "EDR-008 rejected descriptor-bound temporal index key");

  engine::ExecutionTextTemporalSemanticsRequest local_temporal;
  local_temporal.descriptor =
      Descriptor(0x21, "timestamp-local", engine::ExecutionTypeFamily::temporal);
  local_temporal.operation =
      engine::ExecutionTextTemporalOperation::temporal_render;
  RequireOk(local_temporal,
            "EDR-008 rejected temporal descriptor without timezone metadata");

  auto request =
      TemporalRequest(engine::ExecutionTextTemporalOperation::temporal_render);
  request.timezone_policy = engine::ExecutionTimezonePolicy::not_applicable;
  RequireStatus(
      request,
      engine::ExecutionTextTemporalSemanticsStatus::timezone_policy_required,
      "EDR-008 accepted zoned temporal descriptor without timezone policy");

  request = TemporalRequest(
      engine::ExecutionTextTemporalOperation::temporal_render);
  request.descriptor.modifier_flags &=
      ~engine::ExecutionTypeModifierFlagBit(
          engine::ExecutionTypeModifierFlag::timezone_uuid);
  RequireStatus(request,
                engine::ExecutionTextTemporalSemanticsStatus::timezone_uuid_missing,
                "EDR-008 accepted required timezone without timezone UUID");

  request = TemporalRequest(
      engine::ExecutionTextTemporalOperation::temporal_compare);
  request.timezone.resource_uuid = TypedUuid(0x91);
  RequireStatus(
      request,
      engine::ExecutionTextTemporalSemanticsStatus::timezone_binding_mismatch,
      "EDR-008 accepted mismatched timezone binding");

  request = TemporalRequest(
      engine::ExecutionTextTemporalOperation::temporal_compare);
  request.timezone.activation_epoch = 0;
  RequireStatus(
      request,
      engine::ExecutionTextTemporalSemanticsStatus::timezone_epoch_required,
      "EDR-008 accepted timezone binding without activation epoch");

  request = TemporalRequest(
      engine::ExecutionTextTemporalOperation::temporal_index_key);
  request.timezone.version_token.clear();
  RequireStatus(
      request,
      engine::ExecutionTextTemporalSemanticsStatus::timezone_version_required,
      "EDR-008 accepted timezone binding without version token");

  request = TemporalRequest(
      engine::ExecutionTextTemporalOperation::temporal_index_key);
  request.timezone.parser_dependent = true;
  RequireStatus(
      request,
      engine::ExecutionTextTemporalSemanticsStatus::timezone_parser_dependent,
      "EDR-008 accepted parser-dependent timezone binding");

  request = TemporalRequest(
      engine::ExecutionTextTemporalOperation::temporal_index_key);
  request.timezone.available = false;
  RequireStatus(request,
                engine::ExecutionTextTemporalSemanticsStatus::timezone_unavailable,
                "EDR-008 accepted unavailable timezone binding");
}

void TestDescriptorFailures() {
  auto request = TextRequest(engine::ExecutionTextTemporalOperation::text_render);
  request.descriptor.descriptor_epoch = 0;
  const auto result = engine::ValidateExecutionTextTemporalSemantics(request);
  Require(!result.ok(), "EDR-008 accepted invalid text descriptor");
  Require(result.status ==
              engine::ExecutionTextTemporalSemanticsStatus::descriptor_invalid,
          "EDR-008 invalid descriptor status mismatch");
  Require(result.descriptor_status ==
              engine::ExecutionDataPacketStatus::descriptor_missing_epoch,
          "EDR-008 descriptor diagnostic was not preserved");
}

}  // namespace

int main() {
  TestDescriptorBoundTextSemantics();
  TestDescriptorBoundTemporalSemantics();
  TestDescriptorFailures();
  return EXIT_SUCCESS;
}
