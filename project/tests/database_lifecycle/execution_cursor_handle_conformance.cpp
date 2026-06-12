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
  descriptor.descriptor_epoch = 12;
  descriptor.canonical_type_id = seed;
  descriptor.family = engine::ExecutionTypeFamily::signed_integer;
  descriptor.width_class = engine::ExecutionTypeWidthClass::fixed;
  descriptor.stable_name = std::string(name);
  descriptor.bit_width = 64;
  return descriptor;
}

engine::ExecutionRelationDescriptor CursorRelation() {
  engine::ExecutionRelationDescriptor relation;
  relation.relation_descriptor_uuid = Uuid(0x70);
  relation.descriptor_epoch = 5;
  relation.relation_kind = engine::ExecutionRelationKind::cursor;
  relation.stable_name = "edr016.cursor.relation";
  relation.columns.push_back(
      {0, Descriptor(0x10, "id"), "id", "id", "id", false});
  relation.snapshot_uuid = Uuid(0x80);
  relation.security_context_required = true;
  relation.security_policy_uuid = Uuid(0x90);
  relation.memory_policy_uuid = Uuid(0xa0);
  relation.memory_policy_epoch = 8;
  return relation;
}

engine::ExecutionCursorHandle ValidHandle(engine::PortalState state) {
  engine::ExecutionCursorHandle handle;
  handle.cursor_uuid = Uuid(0xb0);
  handle.relation_descriptor = CursorRelation();
  handle.state = state;
  handle.owner_session_uuid = Uuid(0xc0);
  handle.owner_transaction_uuid = Uuid(0xd0);
  handle.snapshot_uuid = Uuid(0xe0);
  handle.security_policy_uuid = Uuid(0xf0);
  handle.close_owner = true;
  handle.transferable = state == engine::PortalState::detached;
  handle.backpressure_enabled = true;
  handle.stream_window_rows = state == engine::PortalState::closed ? 0 : 64;
  handle.lifetime_epoch = 13;
  handle.cleanup_required = state == engine::PortalState::cleanup_pending;
  return handle;
}

void RequireStatus(const engine::ExecutionCursorHandle& handle,
                   engine::ExecutionCursorHandleStatus expected,
                   std::string_view message) {
  const auto result = engine::ValidateExecutionCursorHandle(handle);
  Require(!result.ok(), message);
  Require(result.status == expected,
          "EDR-016 cursor handle validation status mismatch");
}

void TestValidPortalStates() {
  const engine::PortalState states[] = {
      engine::PortalState::declared, engine::PortalState::open,
      engine::PortalState::fetching, engine::PortalState::exhausted,
      engine::PortalState::detached, engine::PortalState::closed,
      engine::PortalState::cleanup_pending, engine::PortalState::error};
  for (const auto state : states) {
    const auto result = engine::ValidateExecutionCursorHandle(ValidHandle(state));
    Require(result.ok(), "EDR-016 valid cursor handle was rejected");
  }
}

void TestIdentityAndRelationFailures() {
  auto handle = ValidHandle(engine::PortalState::open);
  handle.cursor_uuid = {};
  RequireStatus(handle, engine::ExecutionCursorHandleStatus::cursor_uuid_required,
                "EDR-016 accepted cursor handle without UUID");

  handle = ValidHandle(engine::PortalState::open);
  handle.relation_descriptor.relation_descriptor_uuid = {};
  const auto invalid_relation = engine::ValidateExecutionCursorHandle(handle);
  Require(!invalid_relation.ok(),
          "EDR-016 accepted cursor handle with invalid relation descriptor");
  Require(invalid_relation.status ==
              engine::ExecutionCursorHandleStatus::relation_descriptor_invalid,
          "EDR-016 relation descriptor failure status mismatch");
  Require(invalid_relation.relation_status ==
              engine::ExecutionRelationDescriptorStatus::descriptor_uuid_required,
          "EDR-016 relation descriptor diagnostic was not preserved");

  handle = ValidHandle(engine::PortalState::open);
  handle.relation_descriptor.relation_kind =
      engine::ExecutionRelationKind::rowset;
  RequireStatus(
      handle,
      engine::ExecutionCursorHandleStatus::relation_descriptor_kind_invalid,
      "EDR-016 accepted non-cursor relation descriptor for cursor handle");
}

void TestOwnerSnapshotAndPolicyFailures() {
  auto handle = ValidHandle(engine::PortalState::open);
  handle.owner_session_uuid = {};
  RequireStatus(
      handle,
      engine::ExecutionCursorHandleStatus::owner_session_uuid_required,
      "EDR-016 accepted cursor handle without owner session UUID");

  handle = ValidHandle(engine::PortalState::open);
  handle.owner_transaction_uuid = {};
  RequireStatus(
      handle,
      engine::ExecutionCursorHandleStatus::owner_transaction_uuid_required,
      "EDR-016 accepted cursor handle without owner transaction UUID");

  handle = ValidHandle(engine::PortalState::open);
  handle.snapshot_uuid = {};
  RequireStatus(handle,
                engine::ExecutionCursorHandleStatus::snapshot_uuid_required,
                "EDR-016 accepted cursor handle without snapshot UUID");

  handle = ValidHandle(engine::PortalState::open);
  handle.security_policy_uuid = {};
  RequireStatus(
      handle,
      engine::ExecutionCursorHandleStatus::security_policy_uuid_required,
      "EDR-016 accepted cursor handle without security policy UUID");

  handle = ValidHandle(engine::PortalState::open);
  handle.lifetime_epoch = 0;
  RequireStatus(handle,
                engine::ExecutionCursorHandleStatus::lifetime_epoch_required,
                "EDR-016 accepted cursor handle without lifetime epoch");
}

void TestStateBackpressureAndCleanupFailures() {
  auto handle = ValidHandle(engine::PortalState::open);
  handle.state = static_cast<engine::PortalState>(0xff);
  RequireStatus(handle,
                engine::ExecutionCursorHandleStatus::portal_state_invalid,
                "EDR-016 accepted invalid portal state");

  handle = ValidHandle(engine::PortalState::fetching);
  handle.stream_window_rows = 0;
  RequireStatus(
      handle,
      engine::ExecutionCursorHandleStatus::backpressure_window_required,
      "EDR-016 accepted fetching cursor without backpressure window");

  handle = ValidHandle(engine::PortalState::cleanup_pending);
  handle.close_owner = false;
  RequireStatus(handle,
                engine::ExecutionCursorHandleStatus::cleanup_owner_required,
                "EDR-016 accepted cleanup-pending cursor without close owner");

  handle = ValidHandle(engine::PortalState::closed);
  handle.stream_window_rows = 1;
  RequireStatus(handle,
                engine::ExecutionCursorHandleStatus::closed_handle_has_open_window,
                "EDR-016 accepted closed cursor with open stream window");
}

}  // namespace

int main() {
  TestValidPortalStates();
  TestIdentityAndRelationFailures();
  TestOwnerSnapshotAndPolicyFailures();
  TestStateBackpressureAndCleanupFailures();
  return EXIT_SUCCESS;
}
