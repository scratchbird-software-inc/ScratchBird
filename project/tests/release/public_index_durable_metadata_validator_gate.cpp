// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_artifacts.hpp"
#include "index_metapage.hpp"
#include "index_validation_repair_tooling.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

namespace index = scratchbird::core::index;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

platform::TypedUuid MakeUuid(platform::UuidKind kind, platform::u64 salt) {
  const auto generated =
      uuid::GenerateEngineIdentityV7(kind, 1780000000000ULL + salt);
  Require(generated.ok(), "PCR-052 generated UUID creation failed");
  return generated.value;
}

index::IndexMetapageControl MetapageForDescriptor(
    const index::IndexFamilyDescriptor& descriptor,
    platform::u64 salt) {
  index::IndexMetapageControl control;
  control.index_uuid = MakeUuid(platform::UuidKind::object, salt);
  control.family = descriptor.family;
  control.root_page_number = 100 + salt;
  control.resource_epoch = 200 + salt;
  control.mutation_epoch = 300 + salt;
  control.root_generation = 400 + salt;
  control.page_count = 8;
  control.tuple_count_estimate = 64;
  control.layout_version = 1;
  control.semantic_profile_id = descriptor.default_semantic_profile;
  return control;
}

index::IndexValidationRepairTarget ValidationTarget(
    const index::IndexFamilyDescriptor& descriptor,
    platform::u64 salt) {
  index::IndexValidationRepairTarget target;
  target.database_uuid = MakeUuid(platform::UuidKind::database, salt + 1);
  target.table_uuid = MakeUuid(platform::UuidKind::object, salt + 2);
  target.index_uuid = MakeUuid(platform::UuidKind::object, salt + 3);
  target.generation_uuid = MakeUuid(platform::UuidKind::object, salt + 4);
  target.physical_family = descriptor.family;
  return target;
}

index::IndexFamilyValidationRepairProof CompleteProof() {
  index::IndexFamilyValidationRepairProof proof;
  proof.catalog_uuid_binding_proven = true;
  proof.exact_base_table_source_present = true;
  proof.physical_generation_present = true;
  proof.physical_generation_checksum_valid = true;
  proof.runtime_provider_attached = true;
  proof.runtime_epoch_current = true;
  proof.repair_output_validated = true;
  proof.rebuild_output_validated = true;
  proof.proof_token = "public-pcr052-family-validator-proof";
  return proof;
}

void RequireDurableMetadata(const index::IndexMetapageControl& control) {
  Require(control.durable_metadata_present,
          "PCR-052 durable metadata extension missing");
  Require(control.metadata_format_version == 2,
          "PCR-052 metadata format version missing");
  Require(control.minimum_reader_format_version <=
              control.metadata_format_version,
          "PCR-052 metadata reader compatibility invalid");
  Require(control.checksum_profile ==
              scratchbird::storage::page::PageBodyChecksumProfile::strong,
          "PCR-052 metapage checksum profile must be strong by default");
  Require(control.format_compatible,
          "PCR-052 metapage format compatibility not bound");
  Require(control.identity_bound && control.identity_hash != 0,
          "PCR-052 metapage identity hash not bound");
  Require(control.descriptor_hash_bound && control.descriptor_hash != 0,
          "PCR-052 descriptor hash not bound");
  Require(control.route_capability_bound && control.route_capability_hash != 0,
          "PCR-052 route capability hash not bound");
  Require(control.provider_evidence_hash_bound &&
              control.provider_evidence_hash != 0 &&
              control.provider_evidence_count != 0,
          "PCR-052 provider evidence hash not bound");
  Require(control.family_validator_required,
          "PCR-052 family validator should be required for persistent family");
  Require(control.family_validator_passed,
          "PCR-052 family validator did not pass");
  Require(control.metadata_checksum_low64 != 0,
          "PCR-052 metadata checksum low64 missing");
  Require(control.metadata_checksum_high64 != 0,
          "PCR-052 metadata checksum high64 missing");
}

void RequireArtifactPlanner(const index::IndexMetapageControl& control) {
  index::IndexArtifactRequest request;
  request.operation = index::IndexArtifactOperation::export_definition;
  request.index_uuid = control.index_uuid;
  request.family = control.family;
  request.semantic_profile_id = control.semantic_profile_id;
  request.finality_proven = true;
  request.durable_metadata_present = true;
  request.durable_metadata = control;

  const auto decision = index::PlanIndexArtifactOperation(request);
  Require(decision.ok(), "PCR-052 artifact planner refused valid metadata");
  Require(decision.durable_metadata_valid,
          "PCR-052 artifact planner did not mark metadata valid");
  Require(decision.checksum_profile_bound && decision.format_compatible,
          "PCR-052 artifact planner did not bind checksum/format");
  Require(decision.identity_bound && decision.descriptor_hash_bound &&
              decision.route_capability_hash_bound &&
              decision.provider_evidence_hash_bound,
          "PCR-052 artifact planner did not bind all metadata hashes");
  Require(decision.family_validator_passed,
          "PCR-052 artifact planner did not consume family validator proof");

  request.durable_metadata_present = false;
  const auto missing = index::PlanIndexArtifactOperation(request);
  Require(!missing.ok(),
          "PCR-052 artifact planner accepted missing durable metadata");

  request.durable_metadata_present = true;
  request.durable_metadata.descriptor_hash ^= 0x5aULL;
  const auto tampered = index::PlanIndexArtifactOperation(request);
  Require(!tampered.ok(),
          "PCR-052 artifact planner accepted tampered durable metadata");
}

void RequireFamilyValidator(
    const index::IndexFamilyDescriptor& descriptor,
    platform::u64 salt) {
  index::IndexFamilyValidationRepairRequest request;
  request.operation = index::IndexValidationRepairOperation::validate;
  request.family = descriptor.family;
  request.target = ValidationTarget(descriptor, salt);
  request.proof = CompleteProof();
  const auto result = index::ExecuteIndexFamilyValidationRepairOperation(request);
  Require(result.ok(), "PCR-052 family validator refused complete proof");
  Require(result.open_allowed && result.planner_visible,
          "PCR-052 family validator did not restore planner visibility");
  Require(!result.provider_authority && !result.transaction_finality_authority &&
              !result.visibility_authority && !result.security_authority &&
              !result.recovery_authority,
          "PCR-052 family validator claimed engine authority");
}

}  // namespace

int main() {
  std::size_t persistent_families = 0;
  std::size_t tamper_refusals = 0;

  for (const auto& descriptor : index::BuiltinIndexFamilyDescriptors()) {
    if (descriptor.persistence != index::IndexPersistenceClass::persistent ||
        descriptor.completion !=
            index::IndexCompletionStatus::accepted_requires_full_implementation) {
      continue;
    }
    ++persistent_families;
    const auto control =
        MetapageForDescriptor(descriptor, 52000 + persistent_families);
    const auto built = index::BuildIndexMetapageControl(control);
    Require(built.ok(), "PCR-052 metapage build refused valid control");
    RequireDurableMetadata(built.control);

    const auto parsed = index::ParseIndexMetapageControl(built.serialized);
    Require(parsed.ok(), "PCR-052 metapage parse refused valid metadata");
    RequireDurableMetadata(parsed.control);
    Require(parsed.control.metadata_checksum_low64 ==
                built.control.metadata_checksum_low64,
            "PCR-052 metapage checksum changed across round trip");

    auto corrupted = built.serialized;
    Require(!corrupted.empty(), "PCR-052 serialized metapage empty");
    corrupted.back() ^= static_cast<platform::byte>(0x5a);
    const auto corrupted_parse = index::ParseIndexMetapageControl(corrupted);
    Require(!corrupted_parse.ok(),
            "PCR-052 corrupted metapage metadata parsed successfully");
    ++tamper_refusals;

    auto tampered_control = parsed.control;
    tampered_control.provider_evidence_hash ^= 0x22ULL;
    const auto validation =
        index::ValidateIndexMetapageDurableMetadata(tampered_control);
    Require(!validation.ok(),
            "PCR-052 tampered metapage control validated successfully");

    RequireArtifactPlanner(parsed.control);
    RequireFamilyValidator(descriptor, 62000 + persistent_families);
  }

  Require(persistent_families != 0,
          "PCR-052 no persistent index families were tested");
  Require(tamper_refusals == persistent_families,
          "PCR-052 tamper refusal count mismatch");

  std::cout << "public_index_durable_metadata_validator_gate=passed "
            << "persistent_families=" << persistent_families
            << " tamper_refusals=" << tamper_refusals << '\n';
  return EXIT_SUCCESS;
}
