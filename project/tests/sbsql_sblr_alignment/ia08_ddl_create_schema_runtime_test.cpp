// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "engine/sblr/sblr_ddl_create_schema_runtime.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

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

sblr::SblrDdlCreateSchemaDescriptorV1 Descriptor(
    const sblr::SblrDdlCreateSchemaRequestV1& request) {
  sblr::SblrDdlCreateSchemaDescriptorV1 value;
  value.receipt = request.receipt;
  value.occurrence = request.occurrence;
  value.schema_occurrence = request.schema_occurrence;
  Fill(&value.schema_uuid, 0x20);
  value.schema_generation = 1;
  Fill(&value.database_uuid, 0x30);
  Fill(&value.owning_transaction_uuid, 0x40);
  value.owning_local_transaction_id = 7;
  Fill(&value.statement_snapshot_uuid, 0x50);
  Fill(&value.catalog_epoch_uuid, 0x60);
  value.catalog_generation = 11;
  Fill(&value.security_context_uuid, 0x70);
  value.security_epoch = 13;
  Fill(&value.policy_snapshot_uuid, 0x80);
  value.policy_generation = 17;
  Fill(&value.resource_grant_uuid, 0x90);
  value.resource_generation = 19;
  Fill(&value.owner_principal_uuid, 0xa0);
  Fill(&value.binding_uuid, 0xb0);
  Fill(&value.recovery_uuid, 0xc0);
  value.binding_generation = 1;
  value.recovery_generation = 1;
  Fill(&value.normalized_path_sha256, 0x11);
  value.syntax_demand_sha256 = request.evidence;
  Fill(&value.authorization_evidence_sha256, 0x31);
  value.availability = 23;
  return value;
}

}  // namespace

int main() {
  sblr::SblrDdlCreateSchemaRequestV1 request;
  Fill(&request.receipt, 0x10);
  request.occurrence = 1;
  request.schema_occurrence = 2;
  request.command_identity = 1;
  request.name_atoms = {{"users", false}, {"Qa Schema", true}};

  auto request_bytes = sblr::EncodeSblrDdlCreateSchemaRequestV1(request);
  sblr::SblrDdlCreateSchemaRequestV1 decoded_request;
  std::string detail;
  if (!Require(request_bytes.size() == 896,
               "CSQX exact extent mismatch") ||
      !Require(sblr::DecodeSblrDdlCreateSchemaRequestV1(
                   request_bytes.data(), request_bytes.size(),
                   &decoded_request, &detail),
               "CSQX strict decode failed") ||
      !Require(decoded_request.name_atoms.size() == 2 &&
                   decoded_request.name_atoms[0].raw_utf8 == "users" &&
                   !decoded_request.name_atoms[0].quoted &&
                   decoded_request.name_atoms[1].raw_utf8 == "Qa Schema" &&
                   decoded_request.name_atoms[1].quoted &&
                   decoded_request.receipt == request.receipt &&
                   decoded_request.occurrence == request.occurrence &&
                   decoded_request.schema_occurrence ==
                       request.schema_occurrence,
               "CSQX syntax demand changed on round trip") ||
      !Require(sblr::EncodeSblrDdlCreateSchemaRequestV1(decoded_request) ==
                   request_bytes,
               "CSQX canonical replay changed bytes")) {
    return 1;
  }
  auto malformed_request = request_bytes;
  malformed_request[64 + decoded_request.name_atoms[0].raw_utf8.size()] = 1;
  if (!Require(!sblr::DecodeSblrDdlCreateSchemaRequestV1(
                   malformed_request.data(), malformed_request.size(),
                   &decoded_request, &detail),
               "CSQX nonzero atom padding was accepted")) {
    return 2;
  }
  malformed_request = request_bytes;
  malformed_request[832] ^= 1;
  if (!Require(!sblr::DecodeSblrDdlCreateSchemaRequestV1(
                   malformed_request.data(), malformed_request.size(),
                   &decoded_request, &detail),
               "CSQX evidence drift was accepted")) {
    return 3;
  }

  const auto descriptor_value = Descriptor(decoded_request);
  auto descriptor_bytes =
      sblr::EncodeSblrDdlCreateSchemaDescriptorV1(descriptor_value, false);
  sblr::SblrDdlCreateSchemaDescriptorV1 decoded_descriptor;
  if (!Require(descriptor_bytes.size() == 488,
               "CSDX exact extent mismatch") ||
      !Require(sblr::DecodeSblrDdlCreateSchemaDescriptorV1(
                   descriptor_bytes.data(), descriptor_bytes.size(),
                   &decoded_descriptor, &detail, false),
               "CSDX strict decode failed")) {
    return 4;
  }
  auto operand_bytes = descriptor_bytes;
  std::copy_n("CSDO", 4, operand_bytes.begin());
  sblr::SblrDdlCreateSchemaDescriptorV1 decoded_operand;
  if (!Require(std::equal(operand_bytes.begin() + 4, operand_bytes.end(),
                          descriptor_bytes.begin() + 4),
               "CSDX to CSDO projection changed authority bytes") ||
      !Require(sblr::DecodeSblrDdlCreateSchemaDescriptorV1(
                   operand_bytes.data(), operand_bytes.size(),
                   &decoded_operand, &detail, true),
               "CSDO strict decode failed") ||
      !Require(decoded_operand.evidence == decoded_descriptor.evidence &&
                   decoded_operand.schema_uuid ==
                       decoded_descriptor.schema_uuid &&
                   decoded_operand.binding_uuid ==
                       decoded_descriptor.binding_uuid &&
                   decoded_operand.recovery_uuid ==
                       decoded_descriptor.recovery_uuid,
               "CSDO changed engine authority")) {
    return 5;
  }
  auto malformed_operand = operand_bytes;
  malformed_operand[416] ^= 1;
  if (!Require(!sblr::DecodeSblrDdlCreateSchemaDescriptorV1(
                   malformed_operand.data(), malformed_operand.size(),
                   &decoded_operand, &detail, true),
               "CSDO descriptor evidence drift was accepted")) {
    return 6;
  }

  sblr::SblrDdlCreateSchemaResultV1 result;
  result.receipt = decoded_descriptor.receipt;
  result.schema_uuid = decoded_descriptor.schema_uuid;
  result.schema_generation = decoded_descriptor.schema_generation;
  result.parent_schema_uuid = decoded_descriptor.parent_schema_uuid;
  result.parent_namespace_generation =
      decoded_descriptor.parent_namespace_generation;
  result.database_uuid = decoded_descriptor.database_uuid;
  result.owning_transaction_uuid =
      decoded_descriptor.owning_transaction_uuid;
  result.owning_local_transaction_id =
      decoded_descriptor.owning_local_transaction_id;
  result.statement_snapshot_uuid = decoded_descriptor.statement_snapshot_uuid;
  Fill(&result.catalog_row_uuid, 0xd0);
  Fill(&result.mutation_uuid, 0xe0);
  result.catalog_generation = decoded_descriptor.catalog_generation;
  result.security_epoch = decoded_descriptor.security_epoch;
  result.resource_generation = decoded_descriptor.resource_generation;
  result.normalized_path_sha256 = decoded_descriptor.normalized_path_sha256;
  result.descriptor_evidence_sha256 = decoded_descriptor.evidence;
  result.availability = decoded_descriptor.availability;
  Fill(&result.publication_barrier, 0xf0);
  auto result_bytes = sblr::EncodeSblrDdlCreateSchemaResultV1(result);
  sblr::SblrDdlCreateSchemaResultV1 decoded_result;
  if (!Require(result_bytes.size() == 320,
               "CSRS exact extent mismatch") ||
      !Require(sblr::DecodeSblrDdlCreateSchemaResultV1(
                   result_bytes.data(), result_bytes.size(), &decoded_result,
                   &detail),
               "CSRS strict decode failed") ||
      !Require(decoded_result.schema_uuid == decoded_descriptor.schema_uuid &&
                   decoded_result.descriptor_evidence_sha256 ==
                       decoded_descriptor.evidence &&
                   decoded_result.availability ==
                       decoded_descriptor.availability,
               "CSRS changed descriptor authority")) {
    return 7;
  }
  auto malformed_result = result_bytes;
  malformed_result[256] ^= 1;
  if (!Require(!sblr::DecodeSblrDdlCreateSchemaResultV1(
                   malformed_result.data(), malformed_result.size(),
                   &decoded_result, &detail),
               "CSRS result evidence drift was accepted")) {
    return 8;
  }
  malformed_result = result_bytes;
  malformed_result[312] = 1;
  if (!Require(!sblr::DecodeSblrDdlCreateSchemaResultV1(
                   malformed_result.data(), malformed_result.size(),
                   &decoded_result, &detail),
               "CSRS reserved-byte drift was accepted")) {
    return 9;
  }
  return 0;
}
