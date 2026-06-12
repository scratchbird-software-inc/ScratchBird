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
#include <limits>
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

engine::ExecutionTypeDescriptor Descriptor(std::uint8_t seed) {
  engine::ExecutionTypeDescriptor descriptor;
  for (std::size_t index = 0; index < 16; ++index) {
    descriptor.descriptor_uuid.bytes[index] = static_cast<std::uint8_t>(seed + index);
  }
  descriptor.descriptor_epoch = 7;
  descriptor.canonical_type_id = seed;
  descriptor.family = engine::ExecutionTypeFamily::binary;
  descriptor.width_class = engine::ExecutionTypeWidthClass::variable;
  descriptor.stable_name = "edr_packet_descriptor";
  return descriptor;
}

engine::ExecutionDataPacket ValidPacket() {
  engine::ExecutionDataPacket packet;
  packet.descriptor_table.push_back(Descriptor(0x10));
  packet.descriptor_table.push_back(Descriptor(0x30));
  packet.payload_area = {0x01, 0x02, 0x03};
  packet.slot_table.push_back({0, engine::ExecutionValueState::value, 0, 2});
  packet.slot_table.push_back({1, engine::ExecutionValueState::error, 2, 1});
  packet.slot_table.push_back({0, engine::ExecutionValueState::sql_null, 0, 0});
  packet.slot_table.push_back({1, engine::ExecutionValueState::missing, 0, 0});
  return packet;
}

void RequireStatus(const engine::ExecutionDataPacket& packet,
                   engine::ExecutionDataPacketStatus expected,
                   std::string_view message) {
  const auto result = engine::ValidateExecutionDataPacket(packet);
  Require(!result.ok(), message);
  Require(result.status == expected,
          "EDR-004 packet validation status mismatch");
}

void TestValidExecutionDataPacket() {
  const auto result = engine::ValidateExecutionDataPacket(ValidPacket());
  Require(result.ok(), "EDR-004 valid execution data packet was rejected");

  engine::ExecutionDataPacket empty;
  Require(engine::ValidateExecutionDataPacket(empty).ok(),
          "EDR-004 empty packet should validate");
}

void TestExecutionDataPacketDescriptorFailures() {
  auto packet = ValidPacket();
  packet.major_version = 2;
  RequireStatus(packet, engine::ExecutionDataPacketStatus::unsupported_version,
                "EDR-004 accepted unsupported packet version");

  packet = ValidPacket();
  packet.descriptor_table.clear();
  RequireStatus(packet, engine::ExecutionDataPacketStatus::descriptor_table_required,
                "EDR-004 accepted slots without descriptor table");

  packet = ValidPacket();
  packet.slot_table.front().descriptor_index = 99;
  RequireStatus(packet, engine::ExecutionDataPacketStatus::descriptor_index_out_of_range,
                "EDR-004 accepted out-of-range descriptor index");

  packet = ValidPacket();
  packet.descriptor_table.front().descriptor_uuid = {};
  RequireStatus(packet, engine::ExecutionDataPacketStatus::descriptor_missing_uuid,
                "EDR-004 accepted descriptor without UUID");

  packet = ValidPacket();
  packet.descriptor_table.front().descriptor_epoch = 0;
  RequireStatus(packet, engine::ExecutionDataPacketStatus::descriptor_missing_epoch,
                "EDR-004 accepted descriptor without epoch");

  packet = ValidPacket();
  packet.descriptor_table.front().descriptor_authoritative = false;
  RequireStatus(packet, engine::ExecutionDataPacketStatus::descriptor_not_authoritative,
                "EDR-004 accepted non-authoritative descriptor");

  packet = ValidPacket();
  packet.descriptor_table.front().parser_independent = false;
  RequireStatus(packet, engine::ExecutionDataPacketStatus::descriptor_parser_dependent,
                "EDR-004 accepted parser-dependent descriptor");
}

void TestExecutionDataPacketSlotFailures() {
  auto packet = ValidPacket();
  packet.slot_table.front().state = static_cast<engine::ExecutionValueState>(0xff);
  RequireStatus(packet, engine::ExecutionDataPacketStatus::invalid_value_state,
                "EDR-004 accepted invalid slot value state");

  packet = ValidPacket();
  packet.slot_table.front().state = engine::ExecutionValueState::missing;
  packet.slot_table.front().payload_size = 1;
  RequireStatus(packet, engine::ExecutionDataPacketStatus::payload_not_allowed,
                "EDR-004 accepted payload for non-payload state");

  packet = ValidPacket();
  packet.slot_table.front().payload_offset =
      std::numeric_limits<std::uint64_t>::max();
  packet.slot_table.front().payload_size = 1;
  RequireStatus(packet, engine::ExecutionDataPacketStatus::payload_range_overflow,
                "EDR-004 accepted overflowing payload range");

  packet = ValidPacket();
  packet.slot_table.front().payload_offset = 8;
  packet.slot_table.front().payload_size = 1;
  RequireStatus(packet, engine::ExecutionDataPacketStatus::payload_range_out_of_bounds,
                "EDR-004 accepted out-of-bounds payload range");

  packet = ValidPacket();
  packet.slot_table[1].payload_offset = 1;
  packet.slot_table[1].payload_size = 2;
  RequireStatus(packet, engine::ExecutionDataPacketStatus::payload_range_overlap,
                "EDR-004 accepted overlapping payload ranges");

  packet = ValidPacket();
  packet.slot_table[1].payload_offset = 0;
  packet.slot_table[1].payload_size = 0;
  RequireStatus(packet, engine::ExecutionDataPacketStatus::payload_unreferenced,
                "EDR-004 accepted unreferenced payload bytes");
}

}  // namespace

int main() {
  TestValidExecutionDataPacket();
  TestExecutionDataPacketDescriptorFailures();
  TestExecutionDataPacketSlotFailures();
  return EXIT_SUCCESS;
}
