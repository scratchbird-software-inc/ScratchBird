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
  descriptor.descriptor_epoch = 20;
  descriptor.canonical_type_id = seed;
  descriptor.family = engine::ExecutionTypeFamily::signed_integer;
  descriptor.width_class = engine::ExecutionTypeWidthClass::fixed;
  descriptor.stable_name = std::string(name);
  descriptor.bit_width = 64;
  return descriptor;
}

engine::ExecutionRelationDescriptor Relation(engine::ExecutionRelationKind kind) {
  engine::ExecutionRelationDescriptor relation;
  relation.relation_descriptor_uuid = Uuid(0x50);
  relation.descriptor_epoch = 20;
  relation.relation_kind = kind;
  relation.stable_name = "edr020.ownership.transfer.relation";
  relation.columns.push_back(
      {0, Descriptor(0x10, "id"), "id", "id", "id", false});
  relation.snapshot_uuid = Uuid(0x60);
  relation.security_context_required = true;
  relation.security_policy_uuid = Uuid(0x70);
  relation.memory_policy_uuid = Uuid(0x80);
  relation.memory_policy_epoch = 20;
  if (kind == engine::ExecutionRelationKind::remote_fragment) {
    relation.coordinator_fragment_uuid = Uuid(0x90);
    relation.worker_fragment_uuid = Uuid(0xa0);
  }
  return relation;
}

engine::ExecutionOwnershipTransferRecord ValidTransfer(
    engine::ExecutionOwnershipTransferMode mode) {
  engine::ExecutionOwnershipTransferRecord record;
  record.transfer_uuid = Uuid(0xb0);
  record.handle_uuid = Uuid(0xc0);
  record.relation_descriptor = Relation(engine::ExecutionRelationKind::cursor);
  record.source_context = engine::ExecutionOwnershipContextKind::server;
  record.target_context = engine::ExecutionOwnershipContextKind::routine;
  record.source_owner_uuid = Uuid(0xd0);
  record.target_owner_uuid = Uuid(0xe0);
  record.source_transaction_uuid = Uuid(0xf0);
  record.target_transaction_uuid = Uuid(0x20);
  record.before_state = engine::PortalState::open;
  record.after_state = engine::PortalState::open;
  record.transfer_mode = mode;
  record.transfer_epoch = 20;
  record.audit_required = true;
  record.audit_event_uuid = Uuid(0x30);

  switch (mode) {
    case engine::ExecutionOwnershipTransferMode::pass_by_value:
      record.close_owner = true;
      break;
    case engine::ExecutionOwnershipTransferMode::pass_by_reference:
      record.close_owner = false;
      break;
    case engine::ExecutionOwnershipTransferMode::detach:
      record.after_state = engine::PortalState::detached;
      record.close_owner = true;
      break;
    case engine::ExecutionOwnershipTransferMode::cleanup:
      record.after_state = engine::PortalState::cleanup_pending;
      record.close_owner = true;
      record.cleanup_required = true;
      break;
    case engine::ExecutionOwnershipTransferMode::close:
      record.after_state = engine::PortalState::closed;
      record.close_owner = true;
      break;
  }
  return record;
}

void RequireStatus(const engine::ExecutionOwnershipTransferRecord& record,
                   engine::ExecutionOwnershipTransferStatus expected,
                   std::string_view message) {
  const auto result = engine::ValidateExecutionOwnershipTransferRecord(record);
  Require(!result.ok(), message);
  Require(result.status == expected,
          "EDR-020 ownership transfer validation status mismatch");
}

void TestValidTransferModes() {
  const engine::ExecutionOwnershipTransferMode modes[] = {
      engine::ExecutionOwnershipTransferMode::pass_by_value,
      engine::ExecutionOwnershipTransferMode::pass_by_reference,
      engine::ExecutionOwnershipTransferMode::detach,
      engine::ExecutionOwnershipTransferMode::cleanup,
      engine::ExecutionOwnershipTransferMode::close};
  for (const auto mode : modes) {
    Require(engine::ValidateExecutionOwnershipTransferRecord(
                ValidTransfer(mode))
                .ok(),
            "EDR-020 valid ownership transfer was rejected");
  }
}

void TestIdentityAndDescriptorFailures() {
  auto record =
      ValidTransfer(engine::ExecutionOwnershipTransferMode::pass_by_value);
  record.transfer_uuid = {};
  RequireStatus(record,
                engine::ExecutionOwnershipTransferStatus::
                    transfer_uuid_required,
                "EDR-020 accepted transfer without transfer UUID");

  record = ValidTransfer(engine::ExecutionOwnershipTransferMode::pass_by_value);
  record.handle_uuid = {};
  RequireStatus(record,
                engine::ExecutionOwnershipTransferStatus::handle_uuid_required,
                "EDR-020 accepted transfer without handle UUID");

  record = ValidTransfer(engine::ExecutionOwnershipTransferMode::pass_by_value);
  record.relation_descriptor.relation_descriptor_uuid = {};
  const auto invalid_relation =
      engine::ValidateExecutionOwnershipTransferRecord(record);
  Require(!invalid_relation.ok(),
          "EDR-020 accepted invalid transfer relation descriptor");
  Require(invalid_relation.status ==
              engine::ExecutionOwnershipTransferStatus::
                  relation_descriptor_invalid,
          "EDR-020 relation descriptor failure status mismatch");
  Require(invalid_relation.relation_status ==
              engine::ExecutionRelationDescriptorStatus::descriptor_uuid_required,
          "EDR-020 relation descriptor diagnostic was not preserved");

  record = ValidTransfer(engine::ExecutionOwnershipTransferMode::pass_by_value);
  record.relation_descriptor = Relation(engine::ExecutionRelationKind::remote_fragment);
  RequireStatus(record,
                engine::ExecutionOwnershipTransferStatus::relation_kind_invalid,
                "EDR-020 accepted remote-fragment ownership transfer");

  record = ValidTransfer(engine::ExecutionOwnershipTransferMode::pass_by_value);
  record.transfer_epoch = 0;
  RequireStatus(record,
                engine::ExecutionOwnershipTransferStatus::transfer_epoch_required,
                "EDR-020 accepted transfer without epoch");

  record = ValidTransfer(engine::ExecutionOwnershipTransferMode::pass_by_value);
  record.descriptor_authoritative = false;
  RequireStatus(record,
                engine::ExecutionOwnershipTransferStatus::
                    descriptor_not_authoritative,
                "EDR-020 accepted non-authoritative transfer descriptor");

  record = ValidTransfer(engine::ExecutionOwnershipTransferMode::pass_by_value);
  record.parser_independent = false;
  RequireStatus(record,
                engine::ExecutionOwnershipTransferStatus::
                    descriptor_parser_dependent,
                "EDR-020 accepted parser-dependent transfer descriptor");
}

void TestContextOwnerAndTransactionFailures() {
  auto record =
      ValidTransfer(engine::ExecutionOwnershipTransferMode::pass_by_value);
  record.source_context =
      static_cast<engine::ExecutionOwnershipContextKind>(0xff);
  RequireStatus(record,
                engine::ExecutionOwnershipTransferStatus::source_context_invalid,
                "EDR-020 accepted invalid source context");

  record = ValidTransfer(engine::ExecutionOwnershipTransferMode::pass_by_value);
  record.target_context =
      static_cast<engine::ExecutionOwnershipContextKind>(0xff);
  RequireStatus(record,
                engine::ExecutionOwnershipTransferStatus::target_context_invalid,
                "EDR-020 accepted invalid target context");

  record = ValidTransfer(engine::ExecutionOwnershipTransferMode::pass_by_value);
  record.source_owner_uuid = {};
  RequireStatus(record,
                engine::ExecutionOwnershipTransferStatus::
                    source_owner_uuid_required,
                "EDR-020 accepted transfer without source owner UUID");

  record = ValidTransfer(engine::ExecutionOwnershipTransferMode::pass_by_value);
  record.target_owner_uuid = {};
  RequireStatus(record,
                engine::ExecutionOwnershipTransferStatus::
                    target_owner_uuid_required,
                "EDR-020 accepted transfer without target owner UUID");

  record = ValidTransfer(engine::ExecutionOwnershipTransferMode::pass_by_value);
  record.source_transaction_uuid = {};
  RequireStatus(record,
                engine::ExecutionOwnershipTransferStatus::
                    source_transaction_uuid_required,
                "EDR-020 accepted transfer without source transaction UUID");

  record = ValidTransfer(engine::ExecutionOwnershipTransferMode::pass_by_value);
  record.target_transaction_uuid = {};
  RequireStatus(record,
                engine::ExecutionOwnershipTransferStatus::
                    target_transaction_uuid_required,
                "EDR-020 accepted transfer without target transaction UUID");
}

void TestStateModeAndAuditFailures() {
  auto record =
      ValidTransfer(engine::ExecutionOwnershipTransferMode::pass_by_value);
  record.before_state = static_cast<engine::PortalState>(0xff);
  RequireStatus(record,
                engine::ExecutionOwnershipTransferStatus::before_state_invalid,
                "EDR-020 accepted invalid source portal state");

  record = ValidTransfer(engine::ExecutionOwnershipTransferMode::pass_by_value);
  record.after_state = static_cast<engine::PortalState>(0xff);
  RequireStatus(record,
                engine::ExecutionOwnershipTransferStatus::after_state_invalid,
                "EDR-020 accepted invalid target portal state");

  record = ValidTransfer(engine::ExecutionOwnershipTransferMode::pass_by_value);
  record.transfer_mode =
      static_cast<engine::ExecutionOwnershipTransferMode>(0xff);
  RequireStatus(record,
                engine::ExecutionOwnershipTransferStatus::transfer_mode_invalid,
                "EDR-020 accepted invalid transfer mode");

  record = ValidTransfer(engine::ExecutionOwnershipTransferMode::pass_by_value);
  record.before_state = engine::PortalState::closed;
  RequireStatus(record,
                engine::ExecutionOwnershipTransferStatus::
                    closed_source_not_transferable,
                "EDR-020 accepted transfer from closed source handle");

  record =
      ValidTransfer(engine::ExecutionOwnershipTransferMode::pass_by_reference);
  record.close_owner = true;
  RequireStatus(record,
                engine::ExecutionOwnershipTransferStatus::
                    pass_by_reference_close_owner_forbidden,
                "EDR-020 accepted pass-by-reference close ownership");

  record = ValidTransfer(engine::ExecutionOwnershipTransferMode::pass_by_value);
  record.close_owner = false;
  RequireStatus(record,
                engine::ExecutionOwnershipTransferStatus::
                    pass_by_value_close_owner_required,
                "EDR-020 accepted pass-by-value without close owner");

  record = ValidTransfer(engine::ExecutionOwnershipTransferMode::detach);
  record.after_state = engine::PortalState::open;
  RequireStatus(record,
                engine::ExecutionOwnershipTransferStatus::detach_state_required,
                "EDR-020 accepted detach without detached state");

  record = ValidTransfer(engine::ExecutionOwnershipTransferMode::cleanup);
  record.cleanup_required = false;
  RequireStatus(record,
                engine::ExecutionOwnershipTransferStatus::cleanup_owner_required,
                "EDR-020 accepted cleanup transfer without cleanup ownership");

  record = ValidTransfer(engine::ExecutionOwnershipTransferMode::close);
  record.after_state = engine::PortalState::open;
  RequireStatus(record,
                engine::ExecutionOwnershipTransferStatus::close_state_required,
                "EDR-020 accepted close transfer without closed state");

  record = ValidTransfer(engine::ExecutionOwnershipTransferMode::pass_by_value);
  record.audit_event_uuid = {};
  RequireStatus(record,
                engine::ExecutionOwnershipTransferStatus::
                    audit_event_uuid_required,
                "EDR-020 accepted audited transfer without audit UUID");
}

}  // namespace

int main() {
  TestValidTransferModes();
  TestIdentityAndDescriptorFailures();
  TestContextOwnerAndTransactionFailures();
  TestStateModeAndAuditFailures();
  return EXIT_SUCCESS;
}
