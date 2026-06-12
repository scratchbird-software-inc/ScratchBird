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
  descriptor.descriptor_epoch = 21;
  descriptor.canonical_type_id = seed;
  descriptor.family = engine::ExecutionTypeFamily::character;
  descriptor.width_class = engine::ExecutionTypeWidthClass::variable;
  descriptor.stable_name = std::string(name);
  return descriptor;
}

engine::ExecutionRelationDescriptor ResultRelation(std::uint8_t seed,
                                                   std::string_view name) {
  engine::ExecutionRelationDescriptor relation;
  relation.relation_descriptor_uuid = Uuid(seed);
  relation.descriptor_epoch = 21;
  relation.relation_kind = engine::ExecutionRelationKind::result_channel;
  relation.stable_name = std::string(name);
  relation.columns.push_back(
      {0, Descriptor(static_cast<std::uint8_t>(seed + 1), "payload"),
       "payload", "payload", "payload", true});
  relation.snapshot_uuid = Uuid(static_cast<std::uint8_t>(seed + 2));
  relation.security_context_required = true;
  relation.security_policy_uuid = Uuid(static_cast<std::uint8_t>(seed + 3));
  relation.memory_policy_uuid = Uuid(static_cast<std::uint8_t>(seed + 4));
  relation.memory_policy_epoch = 21;
  return relation;
}

engine::ExecutionResultChannelDescriptor Channel(
    std::uint32_t ordinal,
    std::string_view name,
    std::uint8_t seed,
    engine::ExecutionResultChannelRenderingPolicy rendering_policy,
    bool default_channel = false) {
  engine::ExecutionResultChannelDescriptor channel;
  channel.result_channel_uuid = Uuid(seed);
  channel.ordinal = ordinal;
  channel.stable_name = std::string(name);
  channel.relation_descriptor = ResultRelation(seed, name);
  channel.rendering_policy = rendering_policy;
  channel.default_channel = default_channel;
  return channel;
}

engine::ExecutionResultChannelSetDescriptor ValidChannelSet() {
  engine::ExecutionResultChannelSetDescriptor descriptor;
  descriptor.result_channel_set_uuid = Uuid(0x20);
  descriptor.descriptor_epoch = 21;
  descriptor.routine_uuid = Uuid(0x30);
  descriptor.stable_name = "edr021.result.channels";
  descriptor.channels.push_back(Channel(
      0, "rows", 0x40,
      engine::ExecutionResultChannelRenderingPolicy::ordered_stream, true));
  descriptor.channels.push_back(Channel(
      1, "diagnostics", 0x50,
      engine::ExecutionResultChannelRenderingPolicy::diagnostic_side_channel));
  auto nested = Channel(
      2, "row_details", 0x60,
      engine::ExecutionResultChannelRenderingPolicy::nested_envelope);
  nested.parent_result_channel_uuid =
      descriptor.channels.front().result_channel_uuid;
  nested.nesting_depth = 1;
  descriptor.channels.push_back(nested);
  return descriptor;
}

engine::ExecutionRoutineSignatureDescriptor ValidRoutineSignature(
    const engine::ExecutionResultChannelSetDescriptor& channel_set) {
  engine::ExecutionRoutineSignatureDescriptor signature;
  signature.routine_signature_uuid = Uuid(0x70);
  signature.signature_epoch = 21;
  signature.routine_uuid = channel_set.routine_uuid;
  signature.routine_kind = engine::ExecutionRoutineKind::procedure;
  signature.stable_name = "edr021.routine";
  for (const auto& channel : channel_set.channels) {
    if (!engine::ExecutionDataPacketUuidIsNil(
            channel.parent_result_channel_uuid)) {
      continue;
    }
    engine::ExecutionRoutineResultDescriptor result;
    result.ordinal =
        static_cast<std::uint32_t>(signature.result_descriptors.size());
    result.stable_name = channel.stable_name;
    result.relation_descriptor = channel.relation_descriptor;
    result.default_result = channel.default_channel;
    signature.result_descriptors.push_back(result);
  }
  return signature;
}

void RequireStatus(const engine::ExecutionResultChannelSetDescriptor& descriptor,
                   engine::ExecutionResultChannelSetStatus expected,
                   std::string_view message) {
  const auto result =
      engine::ValidateExecutionResultChannelSetDescriptor(descriptor);
  Require(!result.ok(), message);
  Require(result.status == expected,
          "EDR-021 result channel validation status mismatch");
}

void RequireRoutineStatus(
    const engine::ExecutionRoutineSignatureDescriptor& signature,
    const engine::ExecutionResultChannelSetDescriptor& descriptor,
    engine::ExecutionResultChannelSetStatus expected,
    std::string_view message) {
  const auto result =
      engine::ValidateExecutionResultChannelSetForRoutineSignature(signature,
                                                                  descriptor);
  Require(!result.ok(), message);
  Require(result.status == expected,
          "EDR-021 result channel routine binding status mismatch");
}

void TestValidResultChannelSet() {
  const auto channel_set = ValidChannelSet();
  Require(engine::ValidateExecutionResultChannelSetDescriptor(channel_set).ok(),
          "EDR-021 rejected valid multiple/nested result channel set");
  Require(engine::ValidateExecutionResultChannelSetForRoutineSignature(
              ValidRoutineSignature(channel_set), channel_set)
              .ok(),
          "EDR-021 rejected valid routine result channel binding");
}

void TestSetIdentityFailures() {
  auto channel_set = ValidChannelSet();
  channel_set.result_channel_set_uuid = {};
  RequireStatus(channel_set,
                engine::ExecutionResultChannelSetStatus::
                    channel_set_uuid_required,
                "EDR-021 accepted channel set without UUID");

  channel_set = ValidChannelSet();
  channel_set.descriptor_epoch = 0;
  RequireStatus(channel_set,
                engine::ExecutionResultChannelSetStatus::
                    descriptor_epoch_required,
                "EDR-021 accepted channel set without descriptor epoch");

  channel_set = ValidChannelSet();
  channel_set.routine_uuid = {};
  RequireStatus(channel_set,
                engine::ExecutionResultChannelSetStatus::routine_uuid_required,
                "EDR-021 accepted channel set without routine UUID");

  channel_set = ValidChannelSet();
  channel_set.stable_name.clear();
  RequireStatus(channel_set,
                engine::ExecutionResultChannelSetStatus::stable_name_required,
                "EDR-021 accepted channel set without stable name");

  channel_set = ValidChannelSet();
  channel_set.descriptor_authoritative = false;
  RequireStatus(
      channel_set,
      engine::ExecutionResultChannelSetStatus::descriptor_not_authoritative,
      "EDR-021 accepted non-authoritative channel set");

  channel_set = ValidChannelSet();
  channel_set.parser_independent = false;
  RequireStatus(
      channel_set,
      engine::ExecutionResultChannelSetStatus::descriptor_parser_dependent,
      "EDR-021 accepted parser-dependent channel set");

  channel_set = ValidChannelSet();
  channel_set.channels.clear();
  RequireStatus(channel_set,
                engine::ExecutionResultChannelSetStatus::channels_required,
                "EDR-021 accepted channel set without channels");
}

void TestChannelDescriptorFailures() {
  auto channel_set = ValidChannelSet();
  channel_set.channels[0].result_channel_uuid = {};
  RequireStatus(channel_set,
                engine::ExecutionResultChannelSetStatus::channel_uuid_required,
                "EDR-021 accepted channel without UUID");

  channel_set = ValidChannelSet();
  channel_set.channels[1].ordinal = 7;
  RequireStatus(channel_set,
                engine::ExecutionResultChannelSetStatus::
                    channel_ordinal_mismatch,
                "EDR-021 accepted non-canonical channel ordinal");

  channel_set = ValidChannelSet();
  channel_set.channels[0].stable_name.clear();
  RequireStatus(channel_set,
                engine::ExecutionResultChannelSetStatus::channel_name_required,
                "EDR-021 accepted unnamed result channel");

  channel_set = ValidChannelSet();
  channel_set.channels[0].descriptor_authoritative = false;
  RequireStatus(
      channel_set,
      engine::ExecutionResultChannelSetStatus::
          channel_descriptor_not_authoritative,
      "EDR-021 accepted non-authoritative result channel");

  channel_set = ValidChannelSet();
  channel_set.channels[0].parser_independent = false;
  RequireStatus(
      channel_set,
      engine::ExecutionResultChannelSetStatus::channel_descriptor_parser_dependent,
      "EDR-021 accepted parser-dependent result channel");

  channel_set = ValidChannelSet();
  channel_set.channels[0].relation_descriptor.relation_descriptor_uuid = {};
  const auto invalid_relation =
      engine::ValidateExecutionResultChannelSetDescriptor(channel_set);
  Require(!invalid_relation.ok(),
          "EDR-021 accepted invalid result-channel relation descriptor");
  Require(invalid_relation.status ==
              engine::ExecutionResultChannelSetStatus::
                  relation_descriptor_invalid,
          "EDR-021 relation descriptor failure status mismatch");
  Require(invalid_relation.relation_status ==
              engine::ExecutionRelationDescriptorStatus::descriptor_uuid_required,
          "EDR-021 relation descriptor diagnostic was not preserved");

  channel_set = ValidChannelSet();
  channel_set.channels[0].relation_descriptor.relation_kind =
      engine::ExecutionRelationKind::rowset;
  RequireStatus(channel_set,
                engine::ExecutionResultChannelSetStatus::relation_kind_invalid,
                "EDR-021 accepted non-result-channel relation descriptor");
}

void TestRenderingAndNestingFailures() {
  auto channel_set = ValidChannelSet();
  channel_set.channels[0].rendering_policy =
      static_cast<engine::ExecutionResultChannelRenderingPolicy>(0xff);
  RequireStatus(channel_set,
                engine::ExecutionResultChannelSetStatus::
                    rendering_policy_invalid,
                "EDR-021 accepted invalid rendering policy");

  channel_set = ValidChannelSet();
  channel_set.channels[0].rendering_policy =
      engine::ExecutionResultChannelRenderingPolicy::hidden_internal;
  RequireStatus(channel_set,
                engine::ExecutionResultChannelSetStatus::hidden_public_channel,
                "EDR-021 accepted public hidden-internal channel");

  channel_set = ValidChannelSet();
  channel_set.allow_multiple_channels = false;
  RequireStatus(channel_set,
                engine::ExecutionResultChannelSetStatus::
                    multiple_channels_not_allowed,
                "EDR-021 accepted multiple channels when disabled");

  channel_set = ValidChannelSet();
  channel_set.allow_nested_channels = false;
  RequireStatus(channel_set,
                engine::ExecutionResultChannelSetStatus::
                    nested_channels_not_allowed,
                "EDR-021 accepted nested channel when disabled");

  channel_set = ValidChannelSet();
  channel_set.channels[2].parent_result_channel_uuid = Uuid(0xe0);
  RequireStatus(channel_set,
                engine::ExecutionResultChannelSetStatus::
                    parent_channel_not_found,
                "EDR-021 accepted nested channel without parent");

  channel_set = ValidChannelSet();
  channel_set.channels[0].parent_result_channel_uuid =
      channel_set.channels[2].result_channel_uuid;
  channel_set.channels[0].nesting_depth = 1;
  channel_set.channels[0].rendering_policy =
      engine::ExecutionResultChannelRenderingPolicy::nested_envelope;
  RequireStatus(channel_set,
                engine::ExecutionResultChannelSetStatus::
                    parent_channel_order_invalid,
                "EDR-021 accepted nested channel before its parent");

  channel_set = ValidChannelSet();
  channel_set.channels[2].nesting_depth = 3;
  RequireStatus(channel_set,
                engine::ExecutionResultChannelSetStatus::
                    nesting_depth_mismatch,
                "EDR-021 accepted wrong nested channel depth");

  channel_set = ValidChannelSet();
  channel_set.channels[2].rendering_policy =
      engine::ExecutionResultChannelRenderingPolicy::ordered_stream;
  RequireStatus(channel_set,
                engine::ExecutionResultChannelSetStatus::
                    nested_rendering_policy_required,
                "EDR-021 accepted nested channel without nested render policy");

  channel_set = ValidChannelSet();
  channel_set.channels[1].default_channel = true;
  RequireStatus(channel_set,
                engine::ExecutionResultChannelSetStatus::
                    duplicate_default_channel,
                "EDR-021 accepted duplicate default result channel");
}

void TestRoutineBindingFailures() {
  auto channel_set = ValidChannelSet();
  auto signature = ValidRoutineSignature(channel_set);

  signature.routine_signature_uuid = {};
  const auto invalid_signature =
      engine::ValidateExecutionResultChannelSetForRoutineSignature(signature,
                                                                  channel_set);
  Require(!invalid_signature.ok(),
          "EDR-021 accepted invalid routine signature for channel set");
  Require(invalid_signature.status ==
              engine::ExecutionResultChannelSetStatus::routine_signature_invalid,
          "EDR-021 routine signature failure status mismatch");
  Require(invalid_signature.routine_signature_status ==
              engine::ExecutionRoutineSignatureDescriptorStatus::
                  signature_uuid_required,
          "EDR-021 routine signature diagnostic was not preserved");

  channel_set = ValidChannelSet();
  signature = ValidRoutineSignature(channel_set);
  channel_set.routine_uuid = Uuid(0xf0);
  RequireRoutineStatus(
      signature, channel_set,
      engine::ExecutionResultChannelSetStatus::routine_uuid_mismatch,
      "EDR-021 accepted channel set for wrong routine UUID");

  channel_set = ValidChannelSet();
  signature = ValidRoutineSignature(channel_set);
  signature.result_descriptors.pop_back();
  RequireRoutineStatus(
      signature, channel_set,
      engine::ExecutionResultChannelSetStatus::routine_result_count_mismatch,
      "EDR-021 accepted result channel count mismatch");

  channel_set = ValidChannelSet();
  signature = ValidRoutineSignature(channel_set);
  signature.result_descriptors[1].stable_name = "other";
  RequireRoutineStatus(
      signature, channel_set,
      engine::ExecutionResultChannelSetStatus::routine_result_name_mismatch,
      "EDR-021 accepted routine result name mismatch");

  channel_set = ValidChannelSet();
  signature = ValidRoutineSignature(channel_set);
  signature.result_descriptors[1].relation_descriptor.descriptor_epoch += 1;
  RequireRoutineStatus(
      signature, channel_set,
      engine::ExecutionResultChannelSetStatus::
          routine_result_descriptor_mismatch,
      "EDR-021 accepted routine result descriptor identity mismatch");
}

}  // namespace

int main() {
  TestValidResultChannelSet();
  TestSetIdentityFailures();
  TestChannelDescriptorFailures();
  TestRenderingAndNestingFailures();
  TestRoutineBindingFailures();
  return EXIT_SUCCESS;
}
