// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "engine/internal_api/sblr_ddl_alter_trigger_coordinator.hpp"
#include "engine/sblr/sblr_ddl_alter_trigger_runtime.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <string>

namespace {

namespace sblr = scratchbird::engine::sblr;
namespace engine_api = scratchbird::engine::internal_api;

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

sblr::SblrDdlAlterTriggerDescriptorV1 Descriptor(
    const sblr::SblrDdlAlterTriggerRequestV1& request) {
  sblr::SblrDdlAlterTriggerDescriptorV1 value;
  value.receipt = request.receipt;
  value.occurrence = request.occurrence;
  value.trigger_occurrence = request.trigger_occurrence;
  value.action_mask = request.action_mask;
  value.enabled_state = request.enabled_state;
  value.position = request.position;
  value.security_mode = request.security_mode;
  value.failure_policy = request.failure_policy;
  Fill(&value.trigger_uuid, 0x20);
  value.trigger_generation = 3;
  Fill(&value.target_relation_uuid, 0x30);
  value.target_relation_generation = 5;
  Fill(&value.target_relation_descriptor_uuid, 0x40);
  value.target_relation_descriptor_generation = 7;
  Fill(&value.schema_uuid, 0x50);
  value.schema_generation = 11;
  Fill(&value.owning_transaction_uuid, 0x60);
  value.owning_local_transaction_id = 13;
  Fill(&value.statement_snapshot_uuid, 0x70);
  Fill(&value.catalog_epoch_uuid, 0x80);
  value.catalog_generation = 17;
  Fill(&value.security_context_uuid, 0x90);
  value.security_epoch = 19;
  Fill(&value.policy_snapshot_uuid, 0xa0);
  value.policy_generation = 23;
  Fill(&value.resource_grant_uuid, 0xb0);
  value.resource_generation = 29;
  Fill(&value.owner_principal_uuid, 0xc0);
  Fill(&value.body_sblr_uuid, 0xd0);
  value.body_sblr_generation = 31;
  Fill(&value.recovery_uuid, 0xe0);
  value.recovery_generation = 37;
  value.syntax_demand_sha256 = request.evidence;
  Fill(&value.authority_bundle_sha256, 0x51);
  value.availability = 41;
  return value;
}

}  // namespace

int main() {
  sblr::SblrDdlAlterTriggerRequestV1 request;
  Fill(&request.receipt, 0x10);
  request.occurrence = 1;
  request.trigger_occurrence = 2;
  request.command_identity = 1;
  request.action_mask = sblr::kDdlAlterTriggerActionEnabled |
                        sblr::kDdlAlterTriggerActionPosition |
                        sblr::kDdlAlterTriggerActionSecurity |
                        sblr::kDdlAlterTriggerActionCompile |
                        sblr::kDdlAlterTriggerActionValidate |
                        sblr::kDdlAlterTriggerActionFailurePolicy;
  request.enabled_state = sblr::DdlAlterTriggerEnabledStateV1::kDisabled;
  request.position = 17;
  request.security_mode = sblr::DdlAlterTriggerSecurityModeV1::kDefiner;
  request.failure_policy =
      sblr::DdlAlterTriggerFailurePolicyV1::kMandatory;
  request.trigger_name_atoms = {{"app", false}, {"Audit Trigger", true}};

  const auto request_bytes =
      sblr::EncodeSblrDdlAlterTriggerRequestV1(request);
  sblr::SblrDdlAlterTriggerRequestV1 decoded_request;
  std::string detail;
  if (!Require(request_bytes.size() ==
                   sblr::kSblrDdlAlterTriggerRequestV1Bytes,
               "TAQX exact extent mismatch") ||
      !Require(sblr::DecodeSblrDdlAlterTriggerRequestV1(
                   request_bytes.data(), request_bytes.size(),
                   &decoded_request, &detail),
               "TAQX strict decode failed") ||
      !Require(decoded_request.trigger_name_atoms.size() == 2 &&
                   decoded_request.trigger_name_atoms[0].raw_utf8 == "app" &&
                   !decoded_request.trigger_name_atoms[0].quoted &&
                   decoded_request.trigger_name_atoms[1].raw_utf8 ==
                       "Audit Trigger" &&
                   decoded_request.trigger_name_atoms[1].quoted &&
                   decoded_request.action_mask == request.action_mask &&
                   decoded_request.position == 17 &&
                   decoded_request.enabled_state ==
                       sblr::DdlAlterTriggerEnabledStateV1::kDisabled &&
                   decoded_request.security_mode ==
                       sblr::DdlAlterTriggerSecurityModeV1::kDefiner &&
                   decoded_request.failure_policy ==
                       sblr::DdlAlterTriggerFailurePolicyV1::kMandatory,
               "TAQX syntax demand changed on round trip") ||
      !Require(sblr::EncodeSblrDdlAlterTriggerRequestV1(decoded_request) ==
                   request_bytes,
               "TAQX canonical replay changed bytes")) {
    return 1;
  }

  auto malformed_request = request_bytes;
  malformed_request[62 + decoded_request.trigger_name_atoms[0].raw_utf8.size()] =
      1;
  if (!Require(!sblr::DecodeSblrDdlAlterTriggerRequestV1(
                   malformed_request.data(), malformed_request.size(),
                   &decoded_request, &detail),
               "TAQX nonzero atom padding was accepted")) {
    return 2;
  }
  malformed_request = request_bytes;
  malformed_request[46] = 0;
  malformed_request[47] = 0;
  if (!Require(!sblr::DecodeSblrDdlAlterTriggerRequestV1(
                   malformed_request.data(), malformed_request.size(),
                   &decoded_request, &detail),
               "TAQX empty action set was accepted")) {
    return 3;
  }
  malformed_request = request_bytes;
  malformed_request[830] ^= 1;
  if (!Require(!sblr::DecodeSblrDdlAlterTriggerRequestV1(
                   malformed_request.data(), malformed_request.size(),
                   &decoded_request, &detail),
               "TAQX evidence drift was accepted")) {
    return 4;
  }

  const auto descriptor_value = Descriptor(decoded_request);
  engine_api::SblrDdlAlterTriggerAuthorityInputV1 input;
  input.descriptor = descriptor_value;
  const auto coordinated =
      engine_api::CompileSblrDdlAlterTriggerDescriptor(input);
  if (!Require(coordinated.ok,
               "engine authority projection did not compile TADX")) {
    return 5;
  }
  const auto descriptor_bytes =
      sblr::EncodeSblrDdlAlterTriggerDescriptorV1(coordinated.descriptor,
                                                  false);
  sblr::SblrDdlAlterTriggerDescriptorV1 decoded_descriptor;
  if (!Require(descriptor_bytes.size() ==
                   sblr::kSblrDdlAlterTriggerDescriptorV1Bytes,
               "TADX exact extent mismatch") ||
      !Require(sblr::DecodeSblrDdlAlterTriggerDescriptorV1(
                   descriptor_bytes.data(), descriptor_bytes.size(),
                   &decoded_descriptor, &detail, false),
               "TADX strict decode failed") ||
      !Require(decoded_descriptor.trigger_uuid == descriptor_value.trigger_uuid &&
                   decoded_descriptor.action_mask == decoded_request.action_mask &&
                   decoded_descriptor.position == decoded_request.position &&
                   decoded_descriptor.syntax_demand_sha256 ==
                       decoded_request.evidence,
               "TADX changed engine authority")) {
    return 6;
  }

  auto operand_bytes = descriptor_bytes;
  std::copy_n("TADO", 4, operand_bytes.begin());
  sblr::SblrDdlAlterTriggerDescriptorV1 decoded_operand;
  if (!Require(std::equal(operand_bytes.begin() + 4, operand_bytes.end(),
                          descriptor_bytes.begin() + 4),
               "TADX to TADO projection changed authority bytes") ||
      !Require(sblr::DecodeSblrDdlAlterTriggerDescriptorV1(
                   operand_bytes.data(), operand_bytes.size(),
                   &decoded_operand, &detail, true),
               "TADO strict decode failed") ||
      !Require(decoded_operand.evidence == decoded_descriptor.evidence &&
                   decoded_operand.recovery_uuid ==
                       decoded_descriptor.recovery_uuid,
               "TADO changed descriptor evidence or recovery identity")) {
    return 7;
  }
  auto malformed_operand = operand_bytes;
  malformed_operand[47] |= 0x80;
  if (!Require(!sblr::DecodeSblrDdlAlterTriggerDescriptorV1(
                   malformed_operand.data(), malformed_operand.size(),
                   &decoded_operand, &detail, true),
               "TADO reserved action flag was accepted")) {
    return 8;
  }
  malformed_operand = operand_bytes;
  malformed_operand[416] ^= 1;
  if (!Require(!sblr::DecodeSblrDdlAlterTriggerDescriptorV1(
                   malformed_operand.data(), malformed_operand.size(),
                   &decoded_operand, &detail, true),
               "TADO descriptor evidence drift was accepted")) {
    return 9;
  }

  sblr::SblrDdlAlterTriggerResultV1 result;
  result.receipt = decoded_descriptor.receipt;
  result.trigger_uuid = decoded_descriptor.trigger_uuid;
  result.trigger_generation = decoded_descriptor.trigger_generation + 1;
  result.target_relation_uuid = decoded_descriptor.target_relation_uuid;
  result.target_relation_generation =
      decoded_descriptor.target_relation_generation;
  result.schema_uuid = decoded_descriptor.schema_uuid;
  result.schema_generation = decoded_descriptor.schema_generation;
  result.owning_transaction_uuid = decoded_descriptor.owning_transaction_uuid;
  result.owning_local_transaction_id =
      decoded_descriptor.owning_local_transaction_id;
  result.statement_snapshot_uuid = decoded_descriptor.statement_snapshot_uuid;
  Fill(&result.catalog_row_uuid, 0xf0);
  Fill(&result.mutation_uuid, 0x01);
  result.body_sblr_uuid = decoded_descriptor.body_sblr_uuid;
  result.body_sblr_generation = decoded_descriptor.body_sblr_generation;
  result.catalog_generation = decoded_descriptor.catalog_generation;
  result.security_epoch = decoded_descriptor.security_epoch;
  result.resource_generation = decoded_descriptor.resource_generation;
  result.descriptor_evidence_sha256 = decoded_descriptor.evidence;
  result.availability = decoded_descriptor.availability;
  Fill(&result.publication_barrier, 0x12);
  const auto result_bytes =
      sblr::EncodeSblrDdlAlterTriggerResultV1(result);
  sblr::SblrDdlAlterTriggerResultV1 decoded_result;
  if (!Require(result_bytes.size() ==
                   sblr::kSblrDdlAlterTriggerResultV1Bytes,
               "TARS exact extent mismatch") ||
      !Require(sblr::DecodeSblrDdlAlterTriggerResultV1(
                   result_bytes.data(), result_bytes.size(), &decoded_result,
                   &detail),
               "TARS strict decode failed") ||
      !Require(decoded_result.trigger_generation ==
                       decoded_descriptor.trigger_generation + 1 &&
                   decoded_result.descriptor_evidence_sha256 ==
                       decoded_descriptor.evidence,
               "TARS changed terminal authority")) {
    return 10;
  }
  auto malformed_result = result_bytes;
  malformed_result[256] ^= 1;
  if (!Require(!sblr::DecodeSblrDdlAlterTriggerResultV1(
                   malformed_result.data(), malformed_result.size(),
                   &decoded_result, &detail),
               "TARS result evidence drift was accepted")) {
    return 11;
  }
  malformed_result = result_bytes;
  malformed_result[312] = 1;
  return Require(!sblr::DecodeSblrDdlAlterTriggerResultV1(
                     malformed_result.data(), malformed_result.size(),
                     &decoded_result, &detail),
                 "TARS reserved-byte drift was accepted")
             ? 0
             : 12;
}
