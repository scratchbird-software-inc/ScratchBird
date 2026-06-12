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

engine::DescriptorMigrationArtifactDescriptor Artifact(
    std::uint8_t seed,
    engine::DescriptorMigrationArtifactKind kind,
    engine::DescriptorMigrationAction action,
    engine::Uuid descriptor_uuid) {
  engine::DescriptorMigrationArtifactDescriptor artifact;
  artifact.artifact_uuid = Uuid(seed);
  artifact.artifact_kind = kind;
  artifact.action = action;
  artifact.source_descriptor_uuid = descriptor_uuid;
  artifact.source_descriptor_epoch = 10;
  artifact.target_descriptor_uuid = descriptor_uuid;
  artifact.target_descriptor_epoch = 11;
  if (action == engine::DescriptorMigrationAction::invalidate) {
    artifact.invalidation_required = true;
  } else if (action == engine::DescriptorMigrationAction::migrate) {
    artifact.migration_supported = true;
    artifact.migration_proof_uuid = Uuid(seed + 1);
    artifact.artifact_snapshot_uuid = Uuid(seed + 2);
  } else {
    artifact.epoch_independent = true;
    artifact.artifact_snapshot_uuid = Uuid(seed + 2);
  }
  return artifact;
}

engine::DescriptorMigrationPlanDescriptor ValidPlan() {
  engine::DescriptorMigrationPlanDescriptor plan;
  plan.migration_uuid = Uuid(0x10);
  plan.descriptor_uuid = Uuid(0x20);
  plan.source_descriptor_epoch = 10;
  plan.target_descriptor_epoch = 11;
  plan.descriptor_snapshot_uuid = Uuid(0x30);
  plan.stable_name = "edr035.descriptor.migration";
  plan.artifacts.push_back(Artifact(
      0x40, engine::DescriptorMigrationArtifactKind::cache_entry,
      engine::DescriptorMigrationAction::invalidate, plan.descriptor_uuid));
  plan.artifacts.push_back(Artifact(
      0x50, engine::DescriptorMigrationArtifactKind::cursor_handle,
      engine::DescriptorMigrationAction::migrate, plan.descriptor_uuid));
  plan.artifacts.push_back(Artifact(
      0x60, engine::DescriptorMigrationArtifactKind::rowset_value,
      engine::DescriptorMigrationAction::migrate, plan.descriptor_uuid));
  plan.artifacts.push_back(Artifact(
      0x70, engine::DescriptorMigrationArtifactKind::data_packet,
      engine::DescriptorMigrationAction::invalidate, plan.descriptor_uuid));
  plan.artifacts.push_back(Artifact(
      0x80, engine::DescriptorMigrationArtifactKind::index_metadata,
      engine::DescriptorMigrationAction::migrate, plan.descriptor_uuid));
  return plan;
}

void PrintMismatch(engine::DescriptorMigrationStatus expected,
                   engine::DescriptorMigrationStatus actual) {
  std::cerr << "expected=" << engine::DescriptorMigrationStatusName(expected)
            << " actual=" << engine::DescriptorMigrationStatusName(actual)
            << '\n';
}

void RequireStatus(const engine::DescriptorMigrationPlanDescriptor& plan,
                   engine::DescriptorMigrationStatus expected,
                   std::string_view message) {
  const auto result = engine::ValidateDescriptorMigrationPlan(plan);
  Require(!result.ok(), message);
  if (result.status != expected) {
    PrintMismatch(expected, result.status);
    Fail("EDR-035 descriptor migration status mismatch");
  }
}

void TestValidPlans() {
  Require(engine::ValidateDescriptorMigrationPlan(ValidPlan()).ok(),
          "EDR-035 rejected valid descriptor migration plan");

  auto plan = ValidPlan();
  plan.artifacts.push_back(Artifact(
      0x90, engine::DescriptorMigrationArtifactKind::index_metadata,
      engine::DescriptorMigrationAction::preserve, plan.descriptor_uuid));
  Require(engine::ValidateDescriptorMigrationPlan(plan).ok(),
          "EDR-035 rejected epoch-independent preserved artifact");
}

void TestPlanIdentityFailures() {
  auto plan = ValidPlan();
  plan.migration_uuid = {};
  RequireStatus(plan, engine::DescriptorMigrationStatus::migration_uuid_required,
                "EDR-035 accepted migration plan without UUID");

  plan = ValidPlan();
  plan.descriptor_uuid = {};
  RequireStatus(plan, engine::DescriptorMigrationStatus::descriptor_uuid_required,
                "EDR-035 accepted migration plan without descriptor UUID");

  plan = ValidPlan();
  plan.source_descriptor_epoch = 0;
  RequireStatus(plan, engine::DescriptorMigrationStatus::source_epoch_required,
                "EDR-035 accepted migration plan without source epoch");

  plan = ValidPlan();
  plan.target_descriptor_epoch = 0;
  RequireStatus(plan, engine::DescriptorMigrationStatus::target_epoch_required,
                "EDR-035 accepted migration plan without target epoch");

  plan = ValidPlan();
  plan.target_descriptor_epoch = plan.source_descriptor_epoch;
  RequireStatus(plan, engine::DescriptorMigrationStatus::target_epoch_not_newer,
                "EDR-035 accepted migration plan without newer target epoch");

  plan = ValidPlan();
  plan.descriptor_snapshot_uuid = {};
  RequireStatus(
      plan, engine::DescriptorMigrationStatus::descriptor_snapshot_uuid_required,
      "EDR-035 accepted migration plan without descriptor snapshot UUID");

  plan = ValidPlan();
  plan.stable_name.clear();
  RequireStatus(plan, engine::DescriptorMigrationStatus::stable_name_required,
                "EDR-035 accepted migration plan without stable name");

  plan = ValidPlan();
  plan.descriptor_authoritative = false;
  RequireStatus(
      plan, engine::DescriptorMigrationStatus::descriptor_not_authoritative,
      "EDR-035 accepted non-authoritative migration plan");

  plan = ValidPlan();
  plan.parser_independent = false;
  RequireStatus(plan, engine::DescriptorMigrationStatus::descriptor_parser_dependent,
                "EDR-035 accepted parser-dependent migration plan");
}

void TestArtifactShapeFailures() {
  auto plan = ValidPlan();
  plan.artifacts.clear();
  RequireStatus(plan, engine::DescriptorMigrationStatus::artifacts_required,
                "EDR-035 accepted migration plan without artifacts");

  plan = ValidPlan();
  plan.artifacts.resize(engine::kDescriptorMigrationMaxArtifacts + 1);
  RequireStatus(
      plan, engine::DescriptorMigrationStatus::artifact_count_exceeds_limit,
      "EDR-035 accepted excessive migration artifacts");

  plan = ValidPlan();
  plan.artifacts[0].artifact_uuid = {};
  RequireStatus(plan, engine::DescriptorMigrationStatus::artifact_uuid_required,
                "EDR-035 accepted artifact without UUID");

  plan = ValidPlan();
  plan.artifacts[0].artifact_kind =
      static_cast<engine::DescriptorMigrationArtifactKind>(0xff);
  RequireStatus(plan, engine::DescriptorMigrationStatus::artifact_kind_invalid,
                "EDR-035 accepted invalid artifact kind");

  plan = ValidPlan();
  plan.artifacts[0].action =
      static_cast<engine::DescriptorMigrationAction>(0xff);
  RequireStatus(plan, engine::DescriptorMigrationStatus::action_invalid,
                "EDR-035 accepted invalid migration action");
}

void TestArtifactDescriptorMismatchFailures() {
  auto plan = ValidPlan();
  plan.artifacts[0].source_descriptor_uuid = Uuid(0xa0);
  RequireStatus(
      plan, engine::DescriptorMigrationStatus::artifact_source_uuid_mismatch,
      "EDR-035 accepted source descriptor UUID mismatch");

  plan = ValidPlan();
  plan.artifacts[0].source_descriptor_epoch += 1;
  RequireStatus(
      plan, engine::DescriptorMigrationStatus::artifact_source_epoch_mismatch,
      "EDR-035 accepted source descriptor epoch mismatch");

  plan = ValidPlan();
  plan.artifacts[0].target_descriptor_uuid = Uuid(0xa1);
  RequireStatus(
      plan, engine::DescriptorMigrationStatus::artifact_target_uuid_mismatch,
      "EDR-035 accepted target descriptor UUID mismatch");

  plan = ValidPlan();
  plan.artifacts[0].target_descriptor_epoch += 1;
  RequireStatus(
      plan, engine::DescriptorMigrationStatus::artifact_target_epoch_mismatch,
      "EDR-035 accepted target descriptor epoch mismatch");
}

void TestActionFailures() {
  auto plan = ValidPlan();
  plan.artifacts[0].invalidation_required = false;
  RequireStatus(plan, engine::DescriptorMigrationStatus::invalidate_flag_required,
                "EDR-035 accepted invalidate action without invalidation flag");

  plan = ValidPlan();
  plan.artifacts[1].migration_supported = false;
  RequireStatus(plan, engine::DescriptorMigrationStatus::migration_not_supported,
                "EDR-035 accepted migrate action without support flag");

  plan = ValidPlan();
  plan.artifacts[1].migration_proof_uuid = {};
  RequireStatus(plan, engine::DescriptorMigrationStatus::migration_proof_required,
                "EDR-035 accepted migrate action without proof UUID");

  plan = ValidPlan();
  plan.artifacts[1].artifact_snapshot_uuid = {};
  RequireStatus(plan, engine::DescriptorMigrationStatus::artifact_snapshot_required,
                "EDR-035 accepted migrate action without artifact snapshot");

  plan = ValidPlan();
  plan.artifacts[0] = Artifact(
      0x91, engine::DescriptorMigrationArtifactKind::cache_entry,
      engine::DescriptorMigrationAction::preserve, plan.descriptor_uuid);
  plan.artifacts[0].epoch_independent = false;
  RequireStatus(
      plan,
      engine::DescriptorMigrationStatus::
          preserve_without_epoch_independent_proof,
      "EDR-035 accepted preserved artifact without epoch-independent proof");
}

}  // namespace

int main() {
  TestValidPlans();
  TestPlanIdentityFailures();
  TestArtifactShapeFailures();
  TestArtifactDescriptorMismatchFailures();
  TestActionFailures();
  return EXIT_SUCCESS;
}
