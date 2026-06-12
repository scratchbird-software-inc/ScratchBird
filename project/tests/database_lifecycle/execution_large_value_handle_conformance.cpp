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
    engine::ExecutionTypeFamily family = engine::ExecutionTypeFamily::blob) {
  engine::ExecutionTypeDescriptor descriptor;
  descriptor.descriptor_uuid = Uuid(seed);
  descriptor.descriptor_epoch = 11;
  descriptor.canonical_type_id = seed;
  descriptor.family = family;
  descriptor.width_class = engine::ExecutionTypeWidthClass::variable;
  descriptor.stable_name = "large-value-fixture";
  return descriptor;
}

engine::ExecutionLargeValueHandle ValidHandle() {
  engine::ExecutionLargeValueHandle handle;
  handle.kind = engine::ExecutionLargeValueKind::lob_handle;
  handle.lifecycle_state =
      engine::ExecutionLargeValueLifecycleState::committed_visible;
  handle.value_state = engine::ExecutionValueState::lob_handle;
  handle.descriptor = Descriptor(0x10);
  handle.value_uuid = Uuid(0x30);
  handle.owner_transaction_uuid = Uuid(0x40);
  handle.visibility_snapshot_uuid = Uuid(0x50);
  handle.cleanup_policy_uuid = Uuid(0x60);
  handle.owner_transaction_number = 42;
  handle.committed_transaction_number = 42;
  handle.total_bytes = 1025;
  handle.chunk_payload_bytes = 512;
  handle.chunk_count = 3;
  handle.content_hash = "sha256:large-value-fixture";
  handle.integrity_verified = true;
  handle.storage_reference_authoritative = true;
  handle.parser_independent = true;
  handle.stream_final = true;
  return handle;
}

engine::ExecutionLargeValueVisibilityContext VisibleContext(
    const engine::ExecutionLargeValueHandle& handle) {
  engine::ExecutionLargeValueVisibilityContext context;
  context.reader_transaction_uuid = Uuid(0x80);
  context.reader_transaction_number = 100;
  context.visible_committed_high_watermark = 100;
  context.authoritative_cleanup_horizon_transaction_number =
      handle.owner_transaction_number + 10;
  context.cleanup_horizon_authoritative = true;
  return context;
}

void RequireOk(const engine::ExecutionLargeValueHandle& handle,
               const engine::ExecutionLargeValueVisibilityContext& context,
               bool expected_visible,
               bool expected_cleanup,
               std::string_view message) {
  const auto result = engine::ValidateExecutionLargeValueHandle(handle, context);
  Require(result.ok(), message);
  Require(result.visible == expected_visible,
          "EDR-009 large value visibility mismatch");
  Require(result.cleanup_eligible == expected_cleanup,
          "EDR-009 large value cleanup eligibility mismatch");
}

void RequireStatus(
    const engine::ExecutionLargeValueHandle& handle,
    const engine::ExecutionLargeValueVisibilityContext& context,
    engine::ExecutionLargeValueValidationStatus expected,
    std::string_view message) {
  const auto result = engine::ValidateExecutionLargeValueHandle(handle, context);
  Require(!result.ok(), message);
  Require(result.status == expected,
          "EDR-009 large value validation status mismatch");
}

void TestVisibleLifecycleStates() {
  auto handle = ValidHandle();
  auto context = VisibleContext(handle);
  RequireOk(handle, context, true, false,
            "EDR-009 rejected committed visible large value handle");

  context.visible_committed_high_watermark = 41;
  RequireOk(handle, context, false, false,
            "EDR-009 rejected committed handle outside reader snapshot");

  handle = ValidHandle();
  handle.lifecycle_state =
      engine::ExecutionLargeValueLifecycleState::durable_uncommitted;
  handle.committed_transaction_number = 0;
  context = VisibleContext(handle);
  context.include_own_uncommitted = true;
  context.reader_transaction_uuid = handle.owner_transaction_uuid;
  context.reader_transaction_number = handle.owner_transaction_number;
  RequireOk(handle, context, true, false,
            "EDR-009 rejected own uncommitted large value visibility");

  context.reader_transaction_number = handle.owner_transaction_number + 1;
  RequireOk(handle, context, false, false,
            "EDR-009 rejected non-owner uncommitted invisibility");

  handle = ValidHandle();
  handle.lifecycle_state = engine::ExecutionLargeValueLifecycleState::rolled_back;
  handle.committed_transaction_number = 0;
  context = VisibleContext(handle);
  RequireOk(handle, context, false, true,
            "EDR-009 rejected rolled-back cleanup-eligible large value");

  handle.lifecycle_state =
      engine::ExecutionLargeValueLifecycleState::cleanup_pending;
  RequireOk(handle, context, false, true,
            "EDR-009 rejected cleanup-pending large value with MGA horizon");
}

void TestStructuralFailures() {
  auto handle = ValidHandle();
  auto context = VisibleContext(handle);
  handle.descriptor.descriptor_epoch = 0;
  const auto descriptor_result =
      engine::ValidateExecutionLargeValueHandle(handle, context);
  Require(!descriptor_result.ok(), "EDR-009 accepted invalid descriptor");
  Require(descriptor_result.status ==
              engine::ExecutionLargeValueValidationStatus::descriptor_invalid,
          "EDR-009 descriptor status mismatch");
  Require(descriptor_result.descriptor_status ==
              engine::ExecutionDataPacketStatus::descriptor_missing_epoch,
          "EDR-009 descriptor diagnostic was not preserved");

  handle = ValidHandle();
  handle.descriptor.family = engine::ExecutionTypeFamily::signed_integer;
  RequireStatus(
      handle, context,
      engine::ExecutionLargeValueValidationStatus::descriptor_family_unsupported,
      "EDR-009 accepted unsupported descriptor family");

  handle = ValidHandle();
  handle.value_uuid = {};
  RequireStatus(handle, context,
                engine::ExecutionLargeValueValidationStatus::value_uuid_required,
                "EDR-009 accepted large value without value UUID");

  handle = ValidHandle();
  handle.value_state = static_cast<engine::ExecutionValueState>(0xff);
  RequireStatus(handle, context,
                engine::ExecutionLargeValueValidationStatus::value_state_invalid,
                "EDR-009 accepted invalid value-state code");

  handle = ValidHandle();
  handle.value_state = engine::ExecutionValueState::value;
  RequireStatus(
      handle, context,
      engine::ExecutionLargeValueValidationStatus::value_state_kind_mismatch,
      "EDR-009 accepted LOB handle without LOB value-state");

  handle = ValidHandle();
  handle.owner_transaction_uuid = {};
  RequireStatus(
      handle, context,
      engine::ExecutionLargeValueValidationStatus::owner_transaction_uuid_required,
      "EDR-009 accepted large value without owner transaction UUID");

  handle = ValidHandle();
  handle.owner_transaction_number = 0;
  RequireStatus(
      handle, context,
      engine::ExecutionLargeValueValidationStatus::owner_transaction_number_required,
      "EDR-009 accepted large value without owner transaction number");
}

void TestPayloadAndIntegrityFailures() {
  auto handle = ValidHandle();
  auto context = VisibleContext(handle);
  handle.total_bytes = 0;
  RequireStatus(handle, context,
                engine::ExecutionLargeValueValidationStatus::total_bytes_required,
                "EDR-009 accepted external value without byte count");

  handle = ValidHandle();
  handle.chunk_payload_bytes = 0;
  RequireStatus(
      handle, context,
      engine::ExecutionLargeValueValidationStatus::chunk_payload_bytes_required,
      "EDR-009 accepted external value without chunk payload size");

  handle = ValidHandle();
  handle.chunk_count = 0;
  RequireStatus(handle, context,
                engine::ExecutionLargeValueValidationStatus::chunk_count_required,
                "EDR-009 accepted external value without chunk count");

  handle = ValidHandle();
  handle.chunk_count = 2;
  RequireStatus(handle, context,
                engine::ExecutionLargeValueValidationStatus::chunk_count_mismatch,
                "EDR-009 accepted mismatched chunk count");

  handle = ValidHandle();
  handle.content_hash.clear();
  RequireStatus(handle, context,
                engine::ExecutionLargeValueValidationStatus::content_hash_required,
                "EDR-009 accepted external value without content hash");

  handle = ValidHandle();
  handle.integrity_verified = false;
  RequireStatus(handle, context,
                engine::ExecutionLargeValueValidationStatus::integrity_required,
                "EDR-009 accepted external value without integrity evidence");

  handle = ValidHandle();
  handle.storage_reference_authoritative = false;
  RequireStatus(
      handle, context,
      engine::ExecutionLargeValueValidationStatus::
          storage_reference_not_authoritative,
      "EDR-009 accepted non-authoritative storage reference");

  handle = ValidHandle();
  handle.parser_independent = false;
  RequireStatus(handle, context,
                engine::ExecutionLargeValueValidationStatus::parser_dependent,
                "EDR-009 accepted parser-dependent large value handle");
}

void TestMgaVisibilityFailures() {
  auto handle = ValidHandle();
  auto context = VisibleContext(handle);
  handle.committed_transaction_number = 0;
  RequireStatus(handle, context,
                engine::ExecutionLargeValueValidationStatus::
                    commit_inventory_required,
                "EDR-009 accepted committed value without MGA inventory number");

  handle = ValidHandle();
  context = VisibleContext(handle);
  context.visible_committed_high_watermark = 0;
  RequireStatus(handle, context,
                engine::ExecutionLargeValueValidationStatus::
                    visibility_snapshot_required,
                "EDR-009 accepted committed value without snapshot high water");

  handle = ValidHandle();
  handle.kind = engine::ExecutionLargeValueKind::stream_handle;
  handle.stream_final = false;
  context = VisibleContext(handle);
  RequireStatus(handle, context,
                engine::ExecutionLargeValueValidationStatus::stream_final_required,
                "EDR-009 accepted committed stream before final chunk");

  handle = ValidHandle();
  handle.lifecycle_state =
      engine::ExecutionLargeValueLifecycleState::durable_uncommitted;
  handle.committed_transaction_number = 0;
  context = VisibleContext(handle);
  context.include_own_uncommitted = true;
  context.reader_transaction_uuid = {};
  context.reader_transaction_number = 0;
  RequireStatus(
      handle, context,
      engine::ExecutionLargeValueValidationStatus::reader_transaction_required,
      "EDR-009 accepted own-uncommitted visibility without reader transaction");

  handle = ValidHandle();
  handle.lifecycle_state =
      engine::ExecutionLargeValueLifecycleState::cleanup_reclaimed;
  context = VisibleContext(handle);
  context.cleanup_horizon_authoritative = false;
  RequireStatus(
      handle, context,
      engine::ExecutionLargeValueValidationStatus::
          cleanup_horizon_not_authoritative,
      "EDR-009 accepted cleanup without authoritative MGA horizon");

  context.cleanup_horizon_authoritative = true;
  context.authoritative_cleanup_horizon_transaction_number =
      handle.owner_transaction_number - 1;
  RequireStatus(
      handle, context,
      engine::ExecutionLargeValueValidationStatus::cleanup_horizon_before_owner,
      "EDR-009 accepted cleanup horizon before owner transaction");
}

void TestInlineValueHandle() {
  auto handle = ValidHandle();
  handle.kind = engine::ExecutionLargeValueKind::inline_payload;
  handle.lifecycle_state = engine::ExecutionLargeValueLifecycleState::inline_value;
  handle.value_state = engine::ExecutionValueState::value;
  handle.total_bytes = 16;
  handle.chunk_count = 0;
  handle.chunk_payload_bytes = 0;
  handle.content_hash.clear();
  handle.integrity_verified = false;
  auto context = VisibleContext(handle);
  RequireOk(handle, context, true, false,
            "EDR-009 rejected inline value handle");
}

}  // namespace

int main() {
  TestVisibleLifecycleStates();
  TestStructuralFailures();
  TestPayloadAndIntegrityFailures();
  TestMgaVisibilityFailures();
  TestInlineValueHandle();
  return EXIT_SUCCESS;
}
