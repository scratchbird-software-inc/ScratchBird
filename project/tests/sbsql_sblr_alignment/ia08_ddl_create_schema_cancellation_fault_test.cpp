// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "engine/internal_api/sblr_ddl_create_schema_coordinator.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <string>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace sblr = scratchbird::engine::sblr;

template <std::size_t N>
void Fill(std::array<std::uint8_t, N>* value, std::uint8_t seed) {
  for (std::size_t index = 0; index < N; ++index) {
    (*value)[index] = static_cast<std::uint8_t>(seed + index);
  }
}

bool Require(bool condition, const char* message) {
  if (!condition) std::cerr << message << '\n';
  return condition;
}

api::SblrDdlCreateSchemaAuthorityInputV1 Authority() {
  api::SblrDdlCreateSchemaAuthorityInputV1 authority;
  auto& value = authority.descriptor;
  Fill(&value.receipt, 0x10);
  value.occurrence = 1;
  value.schema_occurrence = 1;
  Fill(&value.schema_uuid, 0x20);
  value.schema_generation = 1;
  Fill(&value.database_uuid, 0x30);
  Fill(&value.owning_transaction_uuid, 0x40);
  value.owning_local_transaction_id = 5;
  Fill(&value.statement_snapshot_uuid, 0x50);
  Fill(&value.catalog_epoch_uuid, 0x60);
  value.catalog_generation = 7;
  Fill(&value.security_context_uuid, 0x70);
  value.security_epoch = 11;
  Fill(&value.policy_snapshot_uuid, 0x80);
  value.policy_generation = 13;
  Fill(&value.resource_grant_uuid, 0x90);
  value.resource_generation = 17;
  Fill(&value.owner_principal_uuid, 0xa0);
  Fill(&value.binding_uuid, 0xb0);
  Fill(&value.recovery_uuid, 0xc0);
  value.binding_generation = 1;
  value.recovery_generation = 1;
  Fill(&value.normalized_path_sha256, 0x11);
  Fill(&value.syntax_demand_sha256, 0x21);
  Fill(&value.authorization_evidence_sha256, 0x31);
  value.availability = 19;
  return authority;
}

}  // namespace

int main() {
  const auto authority = Authority();
  const auto compiled = api::CompileSblrDdlCreateSchemaDescriptor(authority);
  if (!Require(compiled.ok,
               "CREATE SCHEMA descriptor compilation failed")) {
    return 1;
  }

  auto operand_bytes =
      sblr::EncodeSblrDdlCreateSchemaDescriptorV1(compiled.descriptor, true);
  sblr::SblrDdlCreateSchemaDescriptorV1 operand;
  std::string detail;
  if (!Require(!operand_bytes.empty() &&
                   sblr::DecodeSblrDdlCreateSchemaDescriptorV1(
                       operand_bytes.data(), operand_bytes.size(), &operand,
                       &detail, true),
               "CREATE SCHEMA canonical operand construction failed")) {
    return 2;
  }

  const auto cancelled = api::ValidateSblrDdlCreateSchemaDescriptor(
      authority, operand, true);
  if (!Require(!cancelled.ok &&
                   cancelled.diagnostic.code == "PROCESS.CANCELLED",
               "CREATE SCHEMA cancellation was not observed before use")) {
    return 3;
  }

  const auto accepted = api::ValidateSblrDdlCreateSchemaDescriptor(
      authority, operand, false);
  if (!Require(accepted.ok && accepted.descriptor.evidence == operand.evidence,
               "CREATE SCHEMA exact operand was not accepted")) {
    return 4;
  }

  auto changed = operand;
  changed.catalog_generation += 1;
  changed.evidence = {};
  const auto changed_bytes =
      sblr::EncodeSblrDdlCreateSchemaDescriptorV1(changed, true);
  sblr::SblrDdlCreateSchemaDescriptorV1 changed_operand;
  if (!Require(!changed_bytes.empty() &&
                   sblr::DecodeSblrDdlCreateSchemaDescriptorV1(
                       changed_bytes.data(), changed_bytes.size(),
                       &changed_operand, &detail, true),
               "CREATE SCHEMA alternate canonical operand construction failed")) {
    return 5;
  }
  const auto mismatched = api::ValidateSblrDdlCreateSchemaDescriptor(
      authority, changed_operand, false);
  if (!Require(!mismatched.ok &&
                   mismatched.diagnostic.code == "MGA.AUTHORITY_MISMATCH",
               "CREATE SCHEMA changed authority was not refused")) {
    return 6;
  }

  const api::SblrDdlCreateSchemaAuthorityInputV1 empty_authority;
  const auto malformed =
      api::CompileSblrDdlCreateSchemaDescriptor(empty_authority);
  if (!Require(!malformed.ok &&
                   malformed.diagnostic.code == "SBLR.OPERAND_INVALID",
               "CREATE SCHEMA malformed authority was not refused")) {
    return 7;
  }
  return 0;
}
