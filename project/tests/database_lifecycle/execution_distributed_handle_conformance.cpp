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
  descriptor.descriptor_epoch = 23;
  descriptor.canonical_type_id = seed;
  descriptor.family = engine::ExecutionTypeFamily::character;
  descriptor.width_class = engine::ExecutionTypeWidthClass::variable;
  descriptor.stable_name = std::string(name);
  return descriptor;
}

engine::ExecutionRelationDescriptor RemoteFragmentRelation() {
  engine::ExecutionRelationDescriptor relation;
  relation.relation_descriptor_uuid = Uuid(0x20);
  relation.descriptor_epoch = 23;
  relation.relation_kind = engine::ExecutionRelationKind::remote_fragment;
  relation.stable_name = "edr023.remote.fragment";
  relation.columns.push_back(
      {0, Descriptor(0x30, "payload"), "payload", "payload", "payload",
       true});
  relation.snapshot_uuid = Uuid(0x40);
  relation.security_context_required = true;
  relation.security_policy_uuid = Uuid(0x50);
  relation.memory_policy_uuid = Uuid(0x60);
  relation.memory_policy_epoch = 23;
  relation.coordinator_fragment_uuid = Uuid(0x70);
  relation.worker_fragment_uuid = Uuid(0x80);
  relation.fragment_ordinal = 1;
  return relation;
}

engine::ExecutionDistributedHandleDescriptor ValidHandle(
    engine::ExecutionDistributedHandleKind handle_kind =
        engine::ExecutionDistributedHandleKind::distributed_cursor) {
  engine::ExecutionDistributedHandleDescriptor descriptor;
  descriptor.distributed_handle_uuid = Uuid(0x90);
  descriptor.coordinator_handle_uuid = Uuid(0xa0);
  descriptor.worker_handle_uuid = Uuid(0xb0);
  descriptor.coordinator_node_uuid = Uuid(0xc0);
  descriptor.worker_node_uuid = Uuid(0xd0);
  descriptor.owner_session_uuid = Uuid(0xe0);
  descriptor.owner_transaction_uuid = Uuid(0xf0);
  descriptor.snapshot_uuid = Uuid(0x10);
  descriptor.security_policy_uuid = Uuid(0x11);
  descriptor.coordinator_epoch = 23;
  descriptor.worker_epoch = 23;
  descriptor.handle_kind = handle_kind;
  descriptor.remote_fragment_descriptor = RemoteFragmentRelation();
  descriptor.portal_state = engine::PortalState::fetching;
  descriptor.stream_window.max_rows_per_credit = 8;
  descriptor.stream_window.max_bytes_per_credit = 1024;
  descriptor.stream_window.granted_row_credits = 2;
  descriptor.stream_window.in_flight_rows = 4;
  descriptor.stream_window.in_flight_bytes = 2048;
  descriptor.stream_window.next_sequence = 10;
  descriptor.stream_window.acknowledged_sequence = 7;
  return descriptor;
}

void RequireStatus(const engine::ExecutionDistributedHandleDescriptor& handle,
                   engine::ExecutionDistributedHandleStatus expected,
                   std::string_view message) {
  const auto result =
      engine::ValidateExecutionDistributedHandleDescriptor(handle);
  Require(!result.ok(), message);
  Require(result.status == expected,
          "EDR-023 distributed handle validation status mismatch");
}

void TestValidHandleProfiles() {
  const engine::ExecutionDistributedHandleKind kinds[] = {
      engine::ExecutionDistributedHandleKind::distributed_cursor,
      engine::ExecutionDistributedHandleKind::distributed_table_value,
      engine::ExecutionDistributedHandleKind::remote_result_channel};
  for (const auto kind : kinds) {
    Require(engine::ValidateExecutionDistributedHandleDescriptor(
                ValidHandle(kind))
                .ok(),
            "EDR-023 rejected valid distributed handle kind");
  }

  auto handle = ValidHandle();
  handle.cancellation_requested = true;
  handle.cancellation_acknowledged = true;
  handle.cancellation_uuid = Uuid(0x12);
  Require(engine::ValidateExecutionDistributedHandleDescriptor(handle).ok(),
          "EDR-023 rejected valid cancellation evidence");

  handle = ValidHandle();
  handle.portal_state = engine::PortalState::cleanup_pending;
  handle.cleanup_required = true;
  handle.cleanup_owner_uuid = Uuid(0x13);
  Require(engine::ValidateExecutionDistributedHandleDescriptor(handle).ok(),
          "EDR-023 rejected valid cleanup-pending distributed handle");

  handle = ValidHandle();
  handle.stream_window.final_sequence_known = true;
  handle.stream_window.final_sequence = handle.stream_window.next_sequence;
  Require(engine::ValidateExecutionDistributedHandleDescriptor(handle).ok(),
          "EDR-023 rejected valid known final stream sequence");
}

void TestIdentityFailures() {
  auto handle = ValidHandle();
  handle.distributed_handle_uuid = {};
  RequireStatus(handle,
                engine::ExecutionDistributedHandleStatus::
                    distributed_handle_uuid_required,
                "EDR-023 accepted distributed handle without UUID");

  handle = ValidHandle();
  handle.coordinator_handle_uuid = {};
  RequireStatus(handle,
                engine::ExecutionDistributedHandleStatus::
                    coordinator_handle_uuid_required,
                "EDR-023 accepted handle without coordinator handle UUID");

  handle = ValidHandle();
  handle.worker_handle_uuid = {};
  RequireStatus(
      handle,
      engine::ExecutionDistributedHandleStatus::worker_handle_uuid_required,
      "EDR-023 accepted handle without worker handle UUID");

  handle = ValidHandle();
  handle.worker_handle_uuid = handle.coordinator_handle_uuid;
  RequireStatus(handle,
                engine::ExecutionDistributedHandleStatus::
                    coordinator_worker_handle_collision,
                "EDR-023 accepted identical coordinator/worker handles");

  handle = ValidHandle();
  handle.coordinator_node_uuid = {};
  RequireStatus(handle,
                engine::ExecutionDistributedHandleStatus::
                    coordinator_node_uuid_required,
                "EDR-023 accepted handle without coordinator node UUID");

  handle = ValidHandle();
  handle.worker_node_uuid = {};
  RequireStatus(
      handle, engine::ExecutionDistributedHandleStatus::worker_node_uuid_required,
      "EDR-023 accepted handle without worker node UUID");
}

void TestContextFailures() {
  auto handle = ValidHandle();
  handle.owner_session_uuid = {};
  RequireStatus(
      handle,
      engine::ExecutionDistributedHandleStatus::owner_session_uuid_required,
      "EDR-023 accepted handle without owner session UUID");

  handle = ValidHandle();
  handle.owner_transaction_uuid = {};
  RequireStatus(handle,
                engine::ExecutionDistributedHandleStatus::
                    owner_transaction_uuid_required,
                "EDR-023 accepted handle without owner transaction UUID");

  handle = ValidHandle();
  handle.snapshot_uuid = {};
  RequireStatus(handle,
                engine::ExecutionDistributedHandleStatus::snapshot_uuid_required,
                "EDR-023 accepted handle without snapshot UUID");

  handle = ValidHandle();
  handle.security_policy_uuid = {};
  RequireStatus(
      handle,
      engine::ExecutionDistributedHandleStatus::security_policy_uuid_required,
      "EDR-023 accepted handle without security policy UUID");

  handle = ValidHandle();
  handle.coordinator_epoch = 0;
  RequireStatus(handle,
                engine::ExecutionDistributedHandleStatus::
                    coordinator_epoch_required,
                "EDR-023 accepted handle without coordinator epoch");

  handle = ValidHandle();
  handle.worker_epoch = 0;
  RequireStatus(handle,
                engine::ExecutionDistributedHandleStatus::worker_epoch_required,
                "EDR-023 accepted handle without worker epoch");
}

void TestDescriptorAndRelationFailures() {
  auto handle = ValidHandle();
  handle.descriptor_authoritative = false;
  RequireStatus(handle,
                engine::ExecutionDistributedHandleStatus::
                    descriptor_not_authoritative,
                "EDR-023 accepted non-authoritative distributed descriptor");

  handle = ValidHandle();
  handle.parser_independent = false;
  RequireStatus(
      handle,
      engine::ExecutionDistributedHandleStatus::descriptor_parser_dependent,
      "EDR-023 accepted parser-dependent distributed descriptor");

  handle = ValidHandle();
  handle.handle_kind =
      static_cast<engine::ExecutionDistributedHandleKind>(0xff);
  RequireStatus(handle,
                engine::ExecutionDistributedHandleStatus::handle_kind_invalid,
                "EDR-023 accepted invalid distributed handle kind");

  handle = ValidHandle();
  handle.remote_fragment_descriptor.relation_descriptor_uuid = {};
  const auto relation_result =
      engine::ValidateExecutionDistributedHandleDescriptor(handle);
  Require(!relation_result.ok(),
          "EDR-023 accepted invalid remote fragment relation descriptor");
  Require(relation_result.status ==
              engine::ExecutionDistributedHandleStatus::
                  relation_descriptor_invalid,
          "EDR-023 relation failure status mismatch");
  Require(relation_result.relation_status ==
              engine::ExecutionRelationDescriptorStatus::
                  descriptor_uuid_required,
          "EDR-023 relation diagnostic was not preserved");

  handle = ValidHandle();
  handle.remote_fragment_descriptor.relation_kind =
      engine::ExecutionRelationKind::table_value;
  RequireStatus(handle,
                engine::ExecutionDistributedHandleStatus::relation_kind_invalid,
                "EDR-023 accepted non-remote-fragment relation descriptor");

  handle = ValidHandle();
  handle.portal_state = static_cast<engine::PortalState>(0xff);
  RequireStatus(handle,
                engine::ExecutionDistributedHandleStatus::portal_state_invalid,
                "EDR-023 accepted invalid distributed portal state");
}

void TestStreamWindowFailures() {
  auto handle = ValidHandle();
  handle.stream_window.max_rows_per_credit = 0;
  RequireStatus(handle,
                engine::ExecutionDistributedHandleStatus::
                    stream_row_credit_required,
                "EDR-023 accepted stream window without row credit");

  handle = ValidHandle();
  handle.stream_window.granted_row_credits = 0;
  RequireStatus(handle,
                engine::ExecutionDistributedHandleStatus::
                    stream_row_credit_required,
                "EDR-023 accepted stream window without granted credits");

  handle = ValidHandle();
  handle.stream_window.max_bytes_per_credit = 0;
  RequireStatus(handle,
                engine::ExecutionDistributedHandleStatus::
                    stream_byte_credit_required,
                "EDR-023 accepted stream window without byte credit");

  handle = ValidHandle();
  handle.stream_window.in_flight_rows = 17;
  RequireStatus(handle,
                engine::ExecutionDistributedHandleStatus::
                    in_flight_rows_exceed_window,
                "EDR-023 accepted stream rows beyond granted window");

  handle = ValidHandle();
  handle.stream_window.in_flight_bytes = 4097;
  RequireStatus(handle,
                engine::ExecutionDistributedHandleStatus::
                    in_flight_bytes_exceed_window,
                "EDR-023 accepted stream bytes beyond granted window");

  handle = ValidHandle();
  handle.stream_window.acknowledged_sequence = 11;
  RequireStatus(handle,
                engine::ExecutionDistributedHandleStatus::
                    acknowledged_sequence_ahead,
                "EDR-023 accepted acknowledged sequence ahead of next");

  handle = ValidHandle();
  handle.stream_window.final_sequence_known = true;
  handle.stream_window.final_sequence = 6;
  RequireStatus(handle,
                engine::ExecutionDistributedHandleStatus::
                    final_sequence_before_ack,
                "EDR-023 accepted final sequence before acknowledged sequence");
}

void TestCancellationAndCleanupFailures() {
  auto handle = ValidHandle();
  handle.cancellation_requested = true;
  RequireStatus(handle,
                engine::ExecutionDistributedHandleStatus::
                    cancellation_uuid_required,
                "EDR-023 accepted cancellation without cancellation UUID");

  handle = ValidHandle();
  handle.cancellation_acknowledged = true;
  RequireStatus(handle,
                engine::ExecutionDistributedHandleStatus::
                    cancellation_ack_without_request,
                "EDR-023 accepted cancellation ack without request");

  handle = ValidHandle();
  handle.cleanup_required = true;
  handle.portal_state = engine::PortalState::cleanup_pending;
  RequireStatus(handle,
                engine::ExecutionDistributedHandleStatus::
                    cleanup_owner_required,
                "EDR-023 accepted cleanup without cleanup owner");

  handle = ValidHandle();
  handle.cleanup_required = true;
  handle.cleanup_owner_uuid = Uuid(0x14);
  handle.portal_state = engine::PortalState::fetching;
  RequireStatus(handle,
                engine::ExecutionDistributedHandleStatus::
                    cleanup_state_required,
                "EDR-023 accepted cleanup on active fetching state");

  handle = ValidHandle();
  handle.portal_state = engine::PortalState::cleanup_pending;
  RequireStatus(handle,
                engine::ExecutionDistributedHandleStatus::
                    cleanup_required_for_state,
                "EDR-023 accepted cleanup-pending state without cleanup flag");
}

}  // namespace

int main() {
  TestValidHandleProfiles();
  TestIdentityFailures();
  TestContextFailures();
  TestDescriptorAndRelationFailures();
  TestStreamWindowFailures();
  TestCancellationAndCleanupFailures();
  return EXIT_SUCCESS;
}
