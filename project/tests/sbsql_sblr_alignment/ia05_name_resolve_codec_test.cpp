// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "../support/binary_uuid_fixture.hpp"
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
  // PART B S3 (owner 2026-11-20..26, layout changes fully allowed): the descriptor carries the name steps AS GIVEN, each with
  // its explicit length, plus the explicit language UUID; it no longer carries a folded '.'-joined canonical name or a case
  // folding profile.
  descriptor.name_atoms = {"customers"};
  descriptor.name_language_uuid = Bytes<16>(141);
  descriptor.resolution_mode = 1;
  descriptor.object_class = 2;
  descriptor.executor_availability_generation = 23;
  descriptor.parser_package_uuid = Bytes<16>(101);
  descriptor.language_profile_uuid = Bytes<16>(121);
  descriptor.security_epoch = request.security_epoch;
  descriptor.resource_epoch = request.resource_epoch;
  const auto descriptor_bytes =
      sblr::EncodeSblrNameResolveDescriptorV1(descriptor);
  sblr::SblrNameResolveDescriptorV1 decoded_descriptor;
  Require(descriptor_bytes.size() ==
                  sblr::kSblrNameResolveDescriptorPrefixBytes + 4 + 9 &&
              sblr::DecodeSblrNameResolveDescriptorV1(
                  descriptor_bytes.data(), descriptor_bytes.size(),
                  &decoded_descriptor, &detail) &&
              decoded_descriptor.object_class == 2 &&
              decoded_descriptor.name_atoms ==
                  std::vector<std::string>{"customers"} &&
              decoded_descriptor.name_language_uuid ==
                  descriptor.name_language_uuid &&
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
  invalid_descriptor.name_atoms = {std::string("bad\0name", 8)};
  Require(sblr::EncodeSblrNameResolveDescriptorV1(invalid_descriptor).empty(),
          "SNRD with embedded NUL was encoded");
  invalid_descriptor = descriptor;
  invalid_descriptor.name_atoms = {std::string("\xc0\xaf", 2)};
  Require(sblr::EncodeSblrNameResolveDescriptorV1(invalid_descriptor).empty(),
          "SNRD with invalid UTF-8 was encoded");
  invalid_descriptor = descriptor;
  invalid_descriptor.name_atoms = {"ok", std::string("bad\0step", 8)};
  Require(sblr::EncodeSblrNameResolveDescriptorV1(invalid_descriptor).empty(),
          "SNRD with a NUL in a later step was encoded");
  invalid_descriptor = descriptor;
  invalid_descriptor.name_atoms.clear();
  Require(sblr::EncodeSblrNameResolveDescriptorV1(invalid_descriptor).empty(),
          "SNRD with no name step was encoded");
  invalid_descriptor = descriptor;
  invalid_descriptor.name_atoms = {std::string(
      sblr::kSblrNameResolveMaximumAtomBytes + 1, 'a')};
  Require(sblr::EncodeSblrNameResolveDescriptorV1(invalid_descriptor).empty(),
          "SNRD with an over-capacity step was encoded");
  invalid_descriptor = descriptor;
  invalid_descriptor.name_atoms.assign(sblr::kSblrNameResolveMaximumAtoms + 1, "a");
  Require(sblr::EncodeSblrNameResolveDescriptorV1(invalid_descriptor).empty(),
          "SNRD with too many steps was encoded");
  invalid_descriptor = descriptor;
  invalid_descriptor.name_language_uuid = {};
  Require(sblr::EncodeSblrNameResolveDescriptorV1(invalid_descriptor).empty(),
          "SNRD with no explicit language was encoded");

  // PART B S3: the steps are DATA, carried exactly. "" is a legal step; a step may contain '.', quotes and any case; steps are
  // never joined, split or folded; the case-only twins and the dotted step are DISTINCT descriptors (distinct atom hashes);
  // the language UUID is part of the descriptor hash.
  const auto round_trip = [&](const std::vector<std::string>& atoms) {
    auto value = descriptor;
    value.name_atoms = atoms;
    const auto bytes = sblr::EncodeSblrNameResolveDescriptorV1(value);
    sblr::SblrNameResolveDescriptorV1 back;
    Require(!bytes.empty() &&
                sblr::DecodeSblrNameResolveDescriptorV1(bytes.data(), bytes.size(),
                                                        &back, &detail) &&
                back.name_atoms == atoms &&
                sblr::EncodeSblrNameResolveDescriptorV1(back) == bytes,
            "SNRD with data steps did not round trip exactly");
    return back;
  };
  const auto empty_step = round_trip({""});
  const auto empty_in_path = round_trip({"", "", ""});
  const auto upper = round_trip({"CUSTOMERS"});
  const auto lower = round_trip({"customers"});
  const auto dotted_one = round_trip({"a.b"});
  const auto dotted_two = round_trip({"a", "b"});
  const auto quoted_text = round_trip({"\"Q\" x", "it's", "\xc3\xa9\xe4\xb8\xad\xf0\x9f\x98\x80"});
  const auto max_step = round_trip({std::string(sblr::kSblrNameResolveMaximumAtomBytes, 'z')});
  Require(empty_step.name_atoms.size() == 1 && empty_step.name_atoms[0].empty() &&
              empty_in_path.name_atoms.size() == 3 &&
              max_step.name_atoms[0].size() == sblr::kSblrNameResolveMaximumAtomBytes,
          "empty or maximum steps lost their exact length");
  Require(upper.name_atoms != lower.name_atoms &&
              upper.name_atoms_sha256 != lower.name_atoms_sha256 &&
              upper.descriptor_sha256 != lower.descriptor_sha256,
          "case-distinct steps were folded together by the descriptor codec");
  Require(dotted_one.name_atoms.size() == 1 && dotted_one.name_atoms[0] == "a.b" &&
              dotted_two.name_atoms.size() == 2 &&
              dotted_one.name_atoms_sha256 != dotted_two.name_atoms_sha256 &&
              dotted_one.descriptor_sha256 != dotted_two.descriptor_sha256,
          "a step containing '.' was joined or split by the descriptor codec");
  {
    // The BIND's step derivation (the function public_abi.cpp's name-resolve bind calls): the steps are the atoms as given.
    // Mutations of it (a fold, a '.' join, a split, a trim) fail here.
    for (const std::vector<std::string>& given : std::vector<std::vector<std::string>>{
             {"customers"}, {"CUSTOMERS"}, {"Customers"}, {"a.b"}, {"a", "b"}, {"Mixed.Case", "x y", ""}, {""}, {" lead", "trail "},
             {"\xc3\x89", "\xc3\xa9"}, {"a.b", "C"}}) {
      std::vector<std::string> steps;
      Require(sblr::BindSblrNameResolveStepsV1(given, &steps) && steps == given && steps.size() == given.size(),
              "the bind's steps are not the atoms exactly as given");
    }
    std::vector<std::string> none;
    Require(!sblr::BindSblrNameResolveStepsV1({}, &none) && none.empty(), "the bind accepted no atoms");
    auto a = descriptor, b = descriptor, c = descriptor;
    Require(sblr::BindSblrNameResolveStepsV1({"a.b", "C"}, &a.name_atoms) &&
                sblr::BindSblrNameResolveStepsV1({"A.B", "c"}, &b.name_atoms) &&
                sblr::BindSblrNameResolveStepsV1({"a", "b", "C"}, &c.name_atoms),
            "bind steps");
    const auto ab = sblr::EncodeSblrNameResolveDescriptorV1(a), bb = sblr::EncodeSblrNameResolveDescriptorV1(b),
               cb = sblr::EncodeSblrNameResolveDescriptorV1(c);
    Require(!ab.empty() && !bb.empty() && !cb.empty() && ab != bb && ab != cb && bb != cb,
            "descriptors of {a.b,C}, {A.B,c} and {a,b,C} are not three distinct descriptors");
    sblr::SblrNameResolveDescriptorV1 back;
    Require(sblr::DecodeSblrNameResolveDescriptorV1(ab.data(), ab.size(), &back, &detail) &&
                back.name_atoms == std::vector<std::string>{"a.b", "C"},
            "the dotted and mixed-case steps did not survive a bind, encode and decode round trip as given");
  }
  {
    auto other_language = descriptor;
    other_language.name_language_uuid = Bytes<16>(142);
    const auto other_bytes = sblr::EncodeSblrNameResolveDescriptorV1(other_language);
    Require(!other_bytes.empty() && other_bytes != descriptor_bytes &&
                other_bytes.size() == descriptor_bytes.size(),
            "the language UUID is not carried in the descriptor bytes");
  }
  {
    // The old (version 1) layout is not a version 2 descriptor.
    auto old_layout = descriptor_bytes;
    old_layout[4] = 1;
    RefuseDescriptor(old_layout, "a version 1 SNRD was accepted as the step-vector layout");
    // An atom length that does not fit the block, or a count that disagrees with the block, is a malformed extent.
    auto bad_count = descriptor_bytes;
    bad_count[150] = 2;
    RefuseDescriptor(bad_count, "SNRD whose step count disagrees with its block was accepted");
    auto bad_length = descriptor_bytes;
    bad_length[sblr::kSblrNameResolveDescriptorPrefixBytes] = 200;
    RefuseDescriptor(bad_length, "SNRD whose step length overruns its block was accepted");
  }

  malformed = descriptor_bytes;
  malformed[12] = 1;
  RefuseDescriptor(malformed, "SNRD with nonzero flags was accepted");
  malformed = descriptor_bytes;
  malformed[151] = 1;  // high byte of the u16 step count (was a reserved byte)
  RefuseDescriptor(malformed, "SNRD with an altered step count was accepted");
  malformed = descriptor_bytes;
  std::fill(malformed.begin() + 240, malformed.begin() + 256, 0);
  RefuseDescriptor(malformed, "SNRD with a zeroed language UUID was accepted");
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

  // PART B S3: the bind request's name steps are DATA too ("" legal, any UTF-8 without NUL, flagged quoted or not, the quoted flag
  // an opaque parser datum); the engine's old identifier-syntax rule on unquoted steps ("bad-name", "" refused) is gone from
  // the name-resolve bind (the catalog-epoch-check bind keeps it; see its codec test).
  for (const std::vector<bind::NameResolveNameAtomV1>& steps :
       std::vector<std::vector<bind::NameResolveNameAtomV1>>{
           {{"", false}},
           {{"", true}, {"", false}},
           {{"bad-name", false}},
           {{"a.b", false}, {"C D", true}},
           {{"Customers", false}, {"customers", true}},
           {{"\xc3\xa9\xe4\xb8\xad", false}}}) {
    auto data_request = bind_request;
    data_request.target_name_atoms = steps;
    data_request.namespace_name_atoms = {{"app", false}};
    std::vector<std::uint8_t> data_bytes;
    bind::NameResolveBindRequestV1 data_back;
    Require(bind::EncodeNameResolveBindRequestV1(data_request, &data_bytes, &detail) &&
                bind::DecodeNameResolveBindRequestV1(data_bytes.data(), data_bytes.size(),
                                                     &data_back, &detail) &&
                data_back.target_name_atoms.size() == steps.size(),
            "SNBQ with data name steps did not round trip");
    for (std::size_t i = 0; i < steps.size(); ++i)
      Require(data_back.target_name_atoms[i].raw_utf8 == steps[i].raw_utf8 &&
                  data_back.target_name_atoms[i].quoted == steps[i].quoted,
              "SNBQ step bytes or quoted datum changed in transit");
  }
  for (const std::string& bad : {std::string("a\0b", 3), std::string("\xc0\xaf", 2),
                                  std::string(4097, 'a')}) {
    auto data_request = bind_request;
    data_request.target_name_atoms = {{bad, true}};
    std::vector<std::uint8_t> refused;
    Require(!bind::EncodeNameResolveBindRequestV1(data_request, &refused, &detail),
            "SNBQ with a NUL, invalid UTF-8 or over-capacity step was encoded");
  }

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
      scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-000000003614");
  member.registry_snapshot_uuid =
      scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-000000003615");
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
