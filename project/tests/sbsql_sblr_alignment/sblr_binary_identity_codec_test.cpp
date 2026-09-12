// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Component codec checks only: fixture identities are not admission authority.
#include "engine/sblr/sblr_engine_envelope.hpp"
#include "engine/sblr/sblr_opcode_stream.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <type_traits>

namespace op = scratchbird::engine::sblr;
using Uuid = scratchbird::core::platform::Uuid;
using Bytes = std::vector<std::uint8_t>;
static_assert(sizeof(Uuid) == 16);
static_assert(std::is_same_v<decltype(op::SblrOperationEnvelope::parser_package_uuid), Uuid>);
static_assert(std::is_same_v<decltype(op::SblrOperationEnvelope::registry_snapshot_uuid), Uuid>);
static_assert(std::is_same_v<decltype(op::SblrOpcodeStream::package_descriptor_uuid), Uuid>);
static_assert(std::is_same_v<decltype(op::SblrOpcodeStreamAdmission::admitted_registry_snapshot_uuid), Uuid>);

namespace {
std::size_t checks = 0;
void Require(bool condition, const char* detail) {
  ++checks;
  if (!condition) throw std::runtime_error(detail);
}
Uuid Identity(std::uint8_t ordinal) {
  return {{0x01,0xa0,0,0,0x7c,0x0a,0x7f,0xff,0xbf,0xff,0,0x09,0x5c,0x0d,0,ordinal}};
}
void Append(Bytes& bytes, std::uint64_t value, unsigned width) {
  for (unsigned i = 0; i < width; ++i)
    bytes.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}
std::size_t ReadOffset(std::string_view bytes, std::size_t offset) {
  std::uint64_t value = 0;
  Require(offset + 8 <= bytes.size(), "truncated offset");
  for (unsigned i = 0; i < 8; ++i)
    value |= std::uint64_t(static_cast<std::uint8_t>(bytes[offset + i])) << (8 * i);
  Require(value <= bytes.size(), "offset outside fixture");
  return static_cast<std::size_t>(value);
}
void FixCrc(std::string& bytes, bool package = false) {
  const auto crc = op::SblrCrc32c(
      reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size() - (package ? 12 : 16));
  for (unsigned i = 0; i < 4; ++i)
    bytes[bytes.size() - 12 + i] = static_cast<char>(crc >> (i * 8));
}
op::SblrOperand Reference(op::SblrValueKind kind, Uuid uuid = Identity(3)) {
  op::SblrOperand value;
  value.ordinal = 1;
  value.type = "wire.value";
  value.name = "fixture";
  value.value_kind = kind;
  value.value_body.assign(uuid.bytes.begin(), uuid.bytes.end());
  return value;
}
op::SblrOperationEnvelope Operation() {
  auto value = op::MakeSblrEnvelope("query.execute", "SBLR_QUERY_EXECUTE", "identity.codec.fixture");
  value.opcode_code = 0x1207;
  value.parser_package_uuid = Identity(1);
  value.registry_snapshot_uuid = Identity(2);
  value.result_shape = "query_execute_result";
  value.diagnostic_shape = "diagnostic_vector";
  value.operands.push_back(Reference(op::SblrValueKind::uuid_ref));
  return value;
}
void CheckOperandWire(const op::SblrOperand& operand, bool expected) {
  // Build record bytes independently from the production record encoder.
  Bytes bytes;
  Append(bytes, 1, 4);
  for (const auto* text : {&operand.type, &operand.name}) {
    Append(bytes, text->size(), 4);
    bytes.insert(bytes.end(), text->begin(), text->end());
  }
  Append(bytes, static_cast<std::uint16_t>(operand.value_kind), 2);
  Append(bytes, 0, 2);
  Append(bytes, operand.value_body.size(), 8);
  bytes.insert(bytes.end(), operand.value_body.begin(), operand.value_body.end());
  const auto decoded = op::DecodeSblrCanonicalOperandRecords(
      bytes.data(), bytes.size(), 1, op::SblrOperandRecordDecodeProfile::canonical_envelope_v1);
  Require(decoded.ok == expected, "operand wire identity validation differs");
  if (expected) {
    Require(decoded.operands.size() == 1 && decoded.operands.front().value_body == operand.value_body &&
            decoded.canonical_bytes == bytes, "operand wire value changed");
  } else {
    Require(decoded.operands.empty() && decoded.canonical_bytes.empty(),
            "rejected operand wire published partial output");
  }
}
op::SblrOperationEnvelope Frame(bool begin, Uuid package) {
  auto value = Operation();
  value.operation_id = begin ? "engine.op.package_begin" : "engine.op.package_end";
  value.opcode = begin ? "SBLR_PACKAGE_BEGIN" : "SBLR_PACKAGE_END";
  value.opcode_code = begin ? 1 : 2;
  value.operands = {Reference(op::SblrValueKind::descriptor_ref, package)};
  value.operands.front().type = begin ? "package.header" : "package.footer";
  value.operands.front().name = "package_descriptor";
  return value;
}
void RoundTrip(const op::SblrOperationEnvelope& input) {
  const auto encoded = op::EncodeSblrEnvelope(input);
  Require(!encoded.empty(), "SBOP encoding rejected fixture");
  const auto decoded = op::DecodeSblrEnvelope(encoded);
  Require(decoded.ok, "SBOP decoding rejected fixture");
  Require(decoded.envelope.parser_package_uuid == input.parser_package_uuid &&
          decoded.envelope.registry_snapshot_uuid == input.registry_snapshot_uuid,
          "SBOP altered binary identity");
  Require(op::EncodeSblrEnvelope(decoded.envelope) == encoded, "SBOP byte round trip differs");
  Require(decoded.envelope.operands.front().value_body == input.operands.front().value_body,
          "SBOP altered operand bytes");
}
void TestOperation() {
  auto input = Operation();
  RoundTrip(input);
  const auto original = op::EncodeSblrEnvelope(input);
  // Independent manifest offsets: header64, entry24, absolute offset at+8.
  const auto producer_offset = ReadOffset(original, 64 + 2 * 24 + 8);
  const auto registry_offset = ReadOffset(original, 64 + 3 * 24 + 8);
  for (const auto offset : {producer_offset, registry_offset}) {
    const auto& expected = offset == producer_offset ? input.parser_package_uuid : input.registry_snapshot_uuid;
    for (std::size_t i = 0; i < 16; ++i)
      Require(static_cast<std::uint8_t>(original[offset + i]) == expected.bytes[i],
              "SBOP identity is not exact raw16");
  }
  for (auto member : {&op::SblrOperationEnvelope::parser_package_uuid,
                      &op::SblrOperationEnvelope::registry_snapshot_uuid}) {
    auto missing = input;
    missing.*member = {};
    Require(!op::ValidateSblrEnvelope(missing).ok && op::EncodeSblrEnvelope(missing).empty(),
            "nil system identity encoded");
    for (unsigned byte = 0; byte < 256; ++byte) {
      for (const std::size_t position : {6u, 8u}) {
        auto changed = input;
        (changed.*member).bytes[position] = static_cast<std::uint8_t>(byte);
        const bool valid = position == 6 ? (byte >> 4) == 7 : (byte >> 6) == 2;
        Require(op::ValidateSblrEnvelope(changed).ok == valid, "system UUID version/variant check differs");
        if (valid) {
          RoundTrip(changed);
        } else {
          Require(op::EncodeSblrEnvelope(changed).empty(), "invalid system UUID encoded");
          auto corrupted = original;
          const auto offset = member == &op::SblrOperationEnvelope::parser_package_uuid
              ? producer_offset : registry_offset;
          corrupted[offset + position] = static_cast<char>(byte);
          FixCrc(corrupted);
          const auto decoded = op::DecodeSblrEnvelope(corrupted);
          Require(!decoded.ok && !decoded.diagnostics.empty() &&
                  decoded.diagnostics.front().code == "SBLR.OPERATION.HEADER_INVALID",
                  "invalid wire identity not rejected at header validation");
        }
      }
    }
  }
}
void TestReferencesAndUserData() {
  // Extended root-only carriers cannot evade their opcode gate inside a list.
  for (const auto size : {320u, 384u}) {
    auto input = Operation();
    Bytes nested;
    Append(nested, 1, 4);
    Append(nested, static_cast<std::uint16_t>(op::SblrValueKind::descriptor_ref), 2);
    Append(nested, 0, 2);
    Append(nested, size, 8);
    nested.resize(nested.size() + size);
    input.operands.front().value_kind = op::SblrValueKind::list;
    input.operands.front().value_body = std::move(nested);
    Require(!op::ValidateSblrEnvelope(input).ok, "nested carrier bypassed opcode-bound descriptor validation");
    CheckOperandWire(input.operands.front(), false);
  }
  const std::array kinds{op::SblrValueKind::uuid_ref, op::SblrValueKind::descriptor_ref,
      op::SblrValueKind::policy_ref, op::SblrValueKind::principal_ref, op::SblrValueKind::udr_ref};
  for (const auto kind : kinds) {
    for (unsigned version = 0; version < 16; ++version) {
      auto uuid = Identity(3);
      uuid.bytes[6] = static_cast<std::uint8_t>(version << 4);
      auto input = Operation();
      input.operands = {Reference(kind, uuid)};
      Require(op::ValidateSblrEnvelope(input).ok == (version == 7), "reference version check differs");
      CheckOperandWire(input.operands.front(), version == 7);
      Bytes nested;
      Append(nested, 1, 4);
      Append(nested, static_cast<std::uint16_t>(kind), 2);
      Append(nested, 0, 2);
      Append(nested, 16, 8);
      nested.insert(nested.end(), uuid.bytes.begin(), uuid.bytes.end());
      input.operands.front().value_kind = op::SblrValueKind::list;
      input.operands.front().value_body = nested;
      Require(op::ValidateSblrEnvelope(input).ok == (version == 7), "nested reference version check differs");
      CheckOperandWire(input.operands.front(), version == 7);
    }
  }
  for (unsigned version = 1; version <= 8; ++version) {
    auto user_uuid = Identity(90);
    user_uuid.bytes[6] = static_cast<std::uint8_t>(version << 4);
    auto input = Operation();
    auto literal = Reference(op::SblrValueKind::literal_typed);
    Append(literal.value_body, 16, 8);
    literal.value_body.insert(literal.value_body.end(), user_uuid.bytes.begin(), user_uuid.bytes.end());
    input.operands = {literal};
    RoundTrip(input);
    CheckOperandWire(input.operands.front(), true);
    input.operands.front().value_body[6] = 0x40;
    Require(!op::ValidateSblrEnvelope(input).ok, "user value exception leaked into descriptor identity");
    CheckOperandWire(input.operands.front(), false);
  }
}
void TestPackage() {
  op::SblrOpcodeStream stream;
  stream.package_descriptor_uuid = Identity(4);
  stream.registry_snapshot_uuid = Identity(2);
  stream.operations = {Frame(true, stream.package_descriptor_uuid), Operation(),
                       Frame(false, stream.package_descriptor_uuid)};
  auto bytes = op::EncodeSblrOpcodeStream(stream);
  Require(!bytes.empty(), "SBOS fixture encoding failed");
  const std::string encoded(bytes.begin(), bytes.end());
  const auto decoded = op::DecodeSblrOpcodeStream(encoded);
  Require(decoded.ok && decoded.stream.package_descriptor_uuid == stream.package_descriptor_uuid &&
          decoded.stream.registry_snapshot_uuid == stream.registry_snapshot_uuid &&
          decoded.canonical_bytes == bytes, "SBOS binary round trip differs");
  for (const auto offset : {24u, 40u}) {
    const auto& identity = offset == 24 ? stream.package_descriptor_uuid : stream.registry_snapshot_uuid;
    Require(std::equal(identity.bytes.begin(), identity.bytes.end(), bytes.begin() + offset),
            "SBOS header is not exact raw16");
    for (unsigned byte = 0; byte < 256; ++byte) {
      for (const auto position : {6u, 8u}) {
        if ((position == 6 ? byte >> 4 == 7 : byte >> 6 == 2)) continue;
        auto corrupted = encoded;
        corrupted[offset + position] = static_cast<char>(byte);
        FixCrc(corrupted, true);
        const auto rejected = op::DecodeSblrOpcodeStream(corrupted);
        Require(!rejected.ok && rejected.diagnostic_id == "DATATYPE.DESCRIPTOR.INVALID",
                "SBOS wire system identity accepted");
      }
    }
  }
  for (std::size_t byte = 0; byte < 16; ++byte) {
    auto crossed = stream;
    crossed.operations[1].registry_snapshot_uuid.bytes[byte] ^= 1;
    Require(op::EncodeSblrOpcodeStream(crossed).empty(), "SBOS ignored registry byte mismatch");
    crossed = stream;
    crossed.operations.back().operands.front().value_body[byte] ^= 1;
    Require(op::EncodeSblrOpcodeStream(crossed).empty(), "SBOS ignored package byte mismatch");
  }
  op::SblrOpcodeStreamAdmission admission;
  admission.admitted_registry_snapshot_uuid = stream.registry_snapshot_uuid;
  admission.authenticated = admission.descriptor_class_accepted = true;
  admission.gateway_pass_through = admission.executor_evidence_accepted = true;
  Require(op::AdmitSblrOpcodeStream(encoded, admission).ok, "component package expectation rejected");
  for (std::size_t byte = 0; byte < 16; ++byte) {
    auto crossed = admission;
    crossed.admitted_registry_snapshot_uuid.bytes[byte] ^= 1;
    Require(!op::AdmitSblrOpcodeStream(encoded, crossed).ok, "package ignored admitted registry byte mismatch");
  }
}
}
int main() {
  try {
    TestOperation();
    TestReferencesAndUserData();
    TestPackage();
    std::cout << "PASS binary SBOP/SBOS identity codec checks=" << checks << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
    return 1;
  }
}
