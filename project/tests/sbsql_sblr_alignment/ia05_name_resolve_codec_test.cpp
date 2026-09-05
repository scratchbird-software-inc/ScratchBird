// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "engine/sblr/sblr_engine_envelope.hpp"
#include "engine/sblr/sblr_name_resolve_runtime.hpp"
#include "wire/parser_server_ipc/sbps_statement_management_bind_codec.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace bind = scratchbird::wire::sbps_statement_management;
namespace sblr = scratchbird::engine::sblr;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "CSC-TEST-003614: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) Fail(message);
}

template <std::size_t N>
std::array<std::uint8_t, N> Bytes(std::uint8_t seed) {
  std::array<std::uint8_t, N> out{};
  for (std::size_t index = 0; index != N; ++index) {
    out[index] = static_cast<std::uint8_t>(seed + index);
  }
  return out;
}

void RefuseRequest(std::vector<std::uint8_t> bytes,
                   std::string_view message) {
  sblr::SblrNameResolveRequestV1 decoded;
  std::string detail;
  Require(!sblr::DecodeSblrNameResolveRequestV1(
              bytes.data(), bytes.size(), &decoded, &detail),
          message);
}

void RefuseDescriptor(std::vector<std::uint8_t> bytes,
                      std::string_view message) {
  sblr::SblrNameResolveDescriptorV1 decoded;
  std::string detail;
  Require(!sblr::DecodeSblrNameResolveDescriptorV1(
              bytes.data(), bytes.size(), &decoded, &detail),
          message);
}

void RefuseResult(std::vector<std::uint8_t> bytes,
                  std::string_view message) {
  sblr::SblrNameResolveResultV1 decoded;
  std::string detail;
  Require(!sblr::DecodeSblrNameResolveResultV1(
              bytes.data(), bytes.size(), &decoded, &detail),
          message);
}

}  // namespace

int main() {
  sblr::SblrNameResolveRequestV1 request;
  request.statement_receipt_uuid = Bytes<16>(1);
  request.occurrence = 7;
  request.catalog_generation = 11;
  request.security_epoch = 13;
  request.resource_epoch = 17;
  const auto request_bytes = sblr::EncodeSblrNameResolveRequestV1(request);
  sblr::SblrNameResolveRequestV1 decoded_request;
  std::string detail;
  Require(request_bytes.size() == sblr::kSblrNameResolveRequestBytes &&
              sblr::DecodeSblrNameResolveRequestV1(
                  request_bytes.data(), request_bytes.size(),
                  &decoded_request, &detail) &&
              sblr::EncodeSblrNameResolveRequestV1(decoded_request) ==
                  request_bytes,
          "canonical SBNQ did not round trip byte-identically");

  auto malformed = request_bytes;
  malformed[0] = 'X';
  RefuseRequest(malformed, "SBNQ with wrong magic was accepted");
  malformed = request_bytes;
  malformed[4] = 2;
  RefuseRequest(malformed, "SBNQ with wrong version was accepted");
  malformed = request_bytes;
  malformed[12] = 1;
  RefuseRequest(malformed, "SBNQ with nonzero flags was accepted");
  malformed = request_bytes;
  std::fill(malformed.begin() + 16, malformed.begin() + 32, 0);
  RefuseRequest(malformed, "SBNQ with nil receipt was accepted");
  malformed = request_bytes;
  std::fill(malformed.begin() + 32, malformed.begin() + 40, 0);
  RefuseRequest(malformed, "SBNQ with zero occurrence was accepted");
  malformed = request_bytes;
  malformed.pop_back();
  RefuseRequest(malformed, "truncated SBNQ was accepted");
  malformed = request_bytes;
  malformed.push_back(0);
  RefuseRequest(malformed, "SBNQ trailing bytes were accepted");

  sblr::SblrNameResolveDescriptorV1 descriptor;
  descriptor.resolution_uuid = Bytes<16>(21);
  descriptor.statement_receipt_uuid = request.statement_receipt_uuid;
  descriptor.catalog_snapshot_uuid = Bytes<16>(41);
  descriptor.catalog_generation = request.catalog_generation;
  descriptor.security_context_uuid = Bytes<16>(61);
  descriptor.namespace_uuid = Bytes<16>(81);
  descriptor.namespace_generation = 19;
  descriptor.canonical_name_utf8 = "customers";
  descriptor.resolution_mode = 1;
  descriptor.object_class = 2;
  descriptor.case_folding_profile = 1;
  descriptor.executor_availability_generation = 23;
  descriptor.parser_package_uuid = Bytes<16>(101);
  descriptor.language_profile_uuid = Bytes<16>(121);
  descriptor.security_epoch = request.security_epoch;
  descriptor.resource_epoch = request.resource_epoch;
  const auto descriptor_bytes =
      sblr::EncodeSblrNameResolveDescriptorV1(descriptor);
  sblr::SblrNameResolveDescriptorV1 decoded_descriptor;
  Require(descriptor_bytes.size() ==
                  sblr::kSblrNameResolveDescriptorPrefixBytes + 9 &&
              sblr::DecodeSblrNameResolveDescriptorV1(
                  descriptor_bytes.data(), descriptor_bytes.size(),
                  &decoded_descriptor, &detail) &&
              decoded_descriptor.object_class == 2 &&
              decoded_descriptor.canonical_name_utf8 == "customers" &&
              sblr::EncodeSblrNameResolveDescriptorV1(decoded_descriptor) ==
                  descriptor_bytes,
          "canonical SNRD did not round trip byte-identically");

  auto invalid_descriptor = descriptor;
  invalid_descriptor.namespace_uuid = {};
  Require(sblr::EncodeSblrNameResolveDescriptorV1(invalid_descriptor).empty(),
          "exact-mode SNRD with no namespace was encoded");
  invalid_descriptor = descriptor;
  invalid_descriptor.object_class = 17;
  Require(sblr::EncodeSblrNameResolveDescriptorV1(invalid_descriptor).empty(),
          "SNRD with unknown Core object class was encoded");
  invalid_descriptor = descriptor;
  invalid_descriptor.canonical_name_utf8 =
      std::string("bad\0name", 8);
  Require(sblr::EncodeSblrNameResolveDescriptorV1(invalid_descriptor).empty(),
          "SNRD with embedded NUL was encoded");
  invalid_descriptor = descriptor;
  invalid_descriptor.canonical_name_utf8 = std::string("\xc0\xaf", 2);
  Require(sblr::EncodeSblrNameResolveDescriptorV1(invalid_descriptor).empty(),
          "SNRD with invalid UTF-8 was encoded");

  malformed = descriptor_bytes;
  malformed[12] = 1;
  RefuseDescriptor(malformed, "SNRD with nonzero flags was accepted");
  malformed = descriptor_bytes;
  malformed[151] = 1;
  RefuseDescriptor(malformed, "SNRD with nonzero reserved byte was accepted");
  malformed = descriptor_bytes;
  malformed[116] ^= 1;
  RefuseDescriptor(malformed, "SNRD name-hash tamper was accepted");
  malformed = descriptor_bytes;
  malformed[160] ^= 1;
  RefuseDescriptor(malformed, "SNRD descriptor-hash tamper was accepted");
  malformed = descriptor_bytes;
  malformed.back() ^= 1;
  RefuseDescriptor(malformed, "SNRD name-body tamper was accepted");
  malformed = descriptor_bytes;
  malformed.pop_back();
  RefuseDescriptor(malformed, "truncated SNRD was accepted");
  malformed = descriptor_bytes;
  malformed.push_back(0);
  RefuseDescriptor(malformed, "SNRD trailing bytes were accepted");

  sblr::SblrNameResolveResultV1 resolved;
  resolved.resolution_uuid = descriptor.resolution_uuid;
  resolved.resolved_object_uuid = Bytes<16>(31);
  resolved.resolved_namespace_uuid = descriptor.namespace_uuid;
  resolved.object_descriptor_generation = 29;
  resolved.catalog_generation = descriptor.catalog_generation;
  resolved.security_epoch = descriptor.security_epoch;
  resolved.redaction_profile_uuid = Bytes<16>(51);
  resolved.status = 1;
  resolved.visibility = 1;
  resolved.object_class = 2;
  resolved.publication_evidence_uuid = Bytes<16>(71);
  resolved.resolution_material_sha256 = Bytes<32>(91);
  resolved.executor_evidence_sha256 = Bytes<32>(131);
  const auto result_bytes = sblr::EncodeSblrNameResolveResultV1(resolved);
  sblr::SblrNameResolveResultV1 decoded_result;
  Require(result_bytes.size() == sblr::kSblrNameResolveResultBytes &&
              sblr::DecodeSblrNameResolveResultV1(
                  result_bytes.data(), result_bytes.size(), &decoded_result,
                  &detail) &&
              sblr::EncodeSblrNameResolveResultV1(decoded_result) ==
                  result_bytes,
          "canonical resolved SNRR did not round trip byte-identically");

  auto not_found = resolved;
  not_found.resolved_object_uuid = {};
  not_found.resolved_namespace_uuid = {};
  not_found.object_descriptor_generation = 0;
  not_found.status = 2;
  not_found.visibility = 2;
  not_found.publication_evidence_uuid = Bytes<16>(72);
  not_found.resolution_material_sha256 = Bytes<32>(92);
  not_found.executor_evidence_sha256 = Bytes<32>(132);
  const auto not_found_bytes =
      sblr::EncodeSblrNameResolveResultV1(not_found);
  Require(not_found_bytes.size() == sblr::kSblrNameResolveResultBytes &&
              sblr::DecodeSblrNameResolveResultV1(
                  not_found_bytes.data(), not_found_bytes.size(),
                  &decoded_result, &detail) &&
              decoded_result.status == 2 && decoded_result.visibility == 2,
          "canonical nondisclosing SNRR did not round trip");

  malformed = result_bytes;
  malformed[12] = 1;
  RefuseResult(malformed, "SNRR with nonzero flags was accepted");
  malformed = result_bytes;
  malformed[104] = 2;
  RefuseResult(malformed, "SNRR with inconsistent resolved status was accepted");
  malformed = result_bytes;
  std::fill(malformed.begin() + 32, malformed.begin() + 48, 0);
  RefuseResult(malformed, "resolved SNRR with nil object was accepted");
  malformed = result_bytes;
  malformed[108] = 1;
  RefuseResult(malformed, "SNRR with nonzero reserved byte was accepted");
  malformed = result_bytes;
  malformed.pop_back();
  RefuseResult(malformed, "truncated SNRR was accepted");
  malformed = result_bytes;
  malformed.push_back(0);
  RefuseResult(malformed, "SNRR trailing bytes were accepted");

  bind::NameResolveBindRequestV1 bind_request;
  bind_request.authenticated_receipt_uuid = request.statement_receipt_uuid;
  bind_request.occurrence = request.occurrence;
  bind_request.resolution_mode = 1;
  bind_request.object_class = 2;
  bind_request.target_name_atoms = {{"customers", false}};
  bind_request.namespace_name_atoms = {{"app", false}};
  std::vector<std::uint8_t> bind_bytes;
  Require(bind::EncodeNameResolveBindRequestV1(
              bind_request, &bind_bytes, &detail),
          "canonical SNBQ encoding failed");
  bind::NameResolveBindRequestV1 decoded_bind;
  Require(bind::DecodeNameResolveBindRequestV1(
              bind_bytes.data(), bind_bytes.size(), &decoded_bind, &detail) &&
              decoded_bind.target_name_atoms.size() == 1 &&
              decoded_bind.namespace_name_atoms.size() == 1 &&
              decoded_bind.target_name_atoms.front().raw_utf8 == "customers" &&
              decoded_bind.namespace_name_atoms.front().raw_utf8 == "app" &&
              bind::NameResolveBindRequestEvidenceV1(decoded_bind) ==
                  decoded_bind.request_evidence_sha256,
          "canonical SNBQ did not round trip with exact atom evidence");

  auto invalid_bind_request = bind_request;
  invalid_bind_request.object_class = 17;
  std::vector<std::uint8_t> rejected_bind;
  Require(!bind::EncodeNameResolveBindRequestV1(
              invalid_bind_request, &rejected_bind, &detail) &&
              rejected_bind.empty(),
          "SNBQ with unknown Core object class was encoded");
  auto malformed_bind = bind_bytes;
  malformed_bind[12] = 1;
  Require(!bind::DecodeNameResolveBindRequestV1(
              malformed_bind.data(), malformed_bind.size(), &decoded_bind,
              &detail),
          "SNBQ with nonzero flags was accepted");
  malformed_bind = bind_bytes;
  malformed_bind[64] ^= 1;
  Require(!bind::DecodeNameResolveBindRequestV1(
              malformed_bind.data(), malformed_bind.size(), &decoded_bind,
              &detail),
          "SNBQ with atom hash tamper was accepted");
  malformed_bind = bind_bytes;
  malformed_bind[160 + 2 + 9] = 2;
  Require(!bind::DecodeNameResolveBindRequestV1(
              malformed_bind.data(), malformed_bind.size(), &decoded_bind,
              &detail),
          "SNBQ with invalid quoted flag was accepted");
  malformed_bind = bind_bytes;
  malformed_bind.push_back(0);
  Require(!bind::DecodeNameResolveBindRequestV1(
              malformed_bind.data(), malformed_bind.size(), &decoded_bind,
              &detail),
          "SNBQ trailing bytes were accepted");

  bind::NameResolveBindAckV1 bind_ack;
  bind_ack.authenticated_receipt_uuid = request.statement_receipt_uuid;
  bind_ack.occurrence = request.occurrence;
  bind_ack.binding_uuid = Bytes<16>(151);
  bind_ack.binding_generation = 1;
  bind_ack.resolution_uuid = descriptor.resolution_uuid;
  bind_ack.descriptor_sha256 = decoded_descriptor.descriptor_sha256;
  bind_ack.request_evidence_sha256 = decoded_bind.request_evidence_sha256;
  std::vector<std::uint8_t> ack_bytes;
  Require(bind::EncodeNameResolveBindAckV1(bind_ack, &ack_bytes, &detail),
          "canonical SNBA encoding failed");
  bind::NameResolveBindAckV1 decoded_ack;
  Require(bind::DecodeNameResolveBindAckV1(
              ack_bytes.data(), ack_bytes.size(), &decoded_ack, &detail) &&
              bind::NameResolveBindAckEvidenceV1(decoded_ack) ==
                  decoded_ack.acknowledgement_evidence_sha256,
          "canonical SNBA did not round trip with exact evidence");
  auto malformed_ack = ack_bytes;
  malformed_ack[144] ^= 1;
  Require(!bind::DecodeNameResolveBindAckV1(
              malformed_ack.data(), malformed_ack.size(), &decoded_ack,
              &detail),
          "SNBA acknowledgement-evidence tamper was accepted");
  malformed_ack = ack_bytes;
  malformed_ack.push_back(0);
  Require(!bind::DecodeNameResolveBindAckV1(
              malformed_ack.data(), malformed_ack.size(), &decoded_ack,
              &detail),
          "SNBA trailing bytes were accepted");

  auto member = sblr::MakeSblrEnvelope(
      "engine.op.name_resolve", "SBLR_NAME_RESOLVE",
      "ia05.name_resolve.codec_contract");
  member.opcode_code = sblr::kSblrNameResolveOpcodeCode;
  member.result_shape = "name_resolve_result";
  member.diagnostic_shape = "diagnostic_vector";
  member.parser_package_uuid =
      "019d0000-0000-7000-8000-000000003614";
  member.registry_snapshot_uuid =
      "019d0000-0000-7000-8000-000000003615";
  member.requires_security_context = true;
  member.requires_transaction_context = true;
  member.parser_resolved_names_to_uuids = true;
  sblr::SblrOperand operand;
  operand.ordinal = 1;
  operand.type = "name_resolve_descriptor.v1";
  operand.name = "name";
  operand.value_kind = sblr::SblrValueKind::name_resolve_descriptor;
  operand.value_body = descriptor_bytes;
  member.operands.push_back(std::move(operand));
  Require(sblr::ValidateSblrEnvelope(member).ok,
          "exact NAME_RESOLVE SBOP member was rejected");

  auto wrong_operand = member;
  wrong_operand.operands.front().type = "name_resolve_descriptor";
  Require(!sblr::ValidateSblrEnvelope(wrong_operand).ok,
          "legacy NAME_RESOLVE operand type was accepted");
  wrong_operand = member;
  wrong_operand.operands.front().name = "target";
  Require(!sblr::ValidateSblrEnvelope(wrong_operand).ok,
          "NAME_RESOLVE operand with wrong name was accepted");
  wrong_operand = member;
  wrong_operand.operands.front().value_body.back() ^= 1;
  Require(!sblr::ValidateSblrEnvelope(wrong_operand).ok,
          "NAME_RESOLVE operand with corrupt SNRD was accepted");
  return EXIT_SUCCESS;
}
