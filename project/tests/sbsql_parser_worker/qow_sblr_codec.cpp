// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: QOW_CT003_CANONICAL_SBLR_SBEE_SBOP_DIRECT_GATE_V1

#include "scratchbird/engine/sblr_envelope.hpp"
#include "hash_digest.hpp"
#include "sblr_admission.hpp"
#include "sblr_engine_envelope.hpp"
#include "sblr_opcode_registry.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using Bytes = std::vector<std::uint8_t>;
namespace public_sblr = scratchbird::engine;
namespace operation = scratchbird::engine::sblr;
namespace server = scratchbird::server;

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "QOW-CT-003 failure: " << message << '\n';
    std::exit(1);
  }
}

Bytes U16(std::uint16_t value) {
  Bytes out;
  public_sblr::SblrAppendU16(out, value);
  return out;
}
Bytes U32(std::uint32_t value) {
  Bytes out;
  public_sblr::SblrAppendU32(out, value);
  return out;
}
Bytes U64(std::uint64_t value) {
  Bytes out;
  public_sblr::SblrAppendU64(out, value);
  return out;
}
void Add16(Bytes* out, std::uint16_t value) {
  public_sblr::SblrAppendU16(*out, value);
}
void Add32(Bytes* out, std::uint32_t value) {
  public_sblr::SblrAppendU32(*out, value);
}
void Add64(Bytes* out, std::uint64_t value) {
  public_sblr::SblrAppendU64(*out, value);
}

std::array<std::uint8_t, 16> Uuid(std::uint8_t suffix, bool version7 = false) {
  std::array<std::uint8_t, 16> uuid{};
  uuid[0] = 0x12;
  uuid[1] = 0x34;
  uuid[6] = version7 ? 0x70 : 0x40;
  uuid[8] = 0x80;
  uuid[15] = suffix;
  return uuid;
}

Bytes UuidField(const std::array<std::uint8_t, 16>& uuid) {
  return {uuid.begin(), uuid.end()};
}

std::string UuidText(const std::array<std::uint8_t, 16>& uuid) {
  constexpr char kHex[] = "0123456789abcdef";
  std::string text;
  for (std::size_t i = 0; i < uuid.size(); ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) text.push_back('-');
    text.push_back(kHex[uuid[i] >> 4]);
    text.push_back(kHex[uuid[i] & 0x0f]);
  }
  return text;
}

Bytes OptionalUuid(const std::array<std::uint8_t, 16>& uuid) {
  Bytes out{1};
  out.insert(out.end(), uuid.begin(), uuid.end());
  return out;
}

Bytes Struct(std::uint32_t format, Bytes body) {
  Bytes out;
  Add32(&out, format);
  Add16(&out, 1);
  Add16(&out, 0);
  Add64(&out, body.size());
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

Bytes OptionalStruct(std::uint32_t format, Bytes body) {
  Bytes out{1};
  auto value = Struct(format, std::move(body));
  out.insert(out.end(), value.begin(), value.end());
  return out;
}

Bytes InlineReference(const Bytes& value) {
  Bytes out{1};
  Add64(&out, value.size());
  out.insert(out.end(), value.begin(), value.end());
  return out;
}

Bytes CrcChecksum(const Bytes& value) {
  Bytes out{1};
  Add32(&out, public_sblr::SblrCrc32c(value.data(), value.size()));
  return out;
}

Bytes Nested(operation::SblrValueKind kind, const Bytes& body) {
  Bytes out;
  Add16(&out, static_cast<std::uint16_t>(kind));
  Add16(&out, 0);
  Add64(&out, body.size());
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

Bytes UuidBody(std::uint8_t suffix) {
  const auto uuid = Uuid(suffix);
  return {uuid.begin(), uuid.end()};
}

operation::SblrOperand Operand(std::uint32_t ordinal,
                               std::string slot,
                               operation::SblrValueKind kind,
                               Bytes body) {
  operation::SblrOperand value;
  value.ordinal = ordinal;
  value.type = "wire.value";
  value.name = std::move(slot);
  value.value_kind = kind;
  value.value_body = std::move(body);
  return value;
}

operation::SblrOperationEnvelope CanonicalOperation() {
  auto envelope = operation::MakeSblrEnvelope(
      "query.execute", "SBLR_QUERY_EXECUTE", "qow.ct003.trace");
  envelope.opcode_code = 0x1207;
  envelope.parser_package_uuid = UuidText(Uuid(0x31));
  envelope.registry_snapshot_uuid = UuidText(Uuid(0x32));

  std::uint32_t ordinal = 1;
  envelope.operands.push_back(Operand(ordinal++, "slot.uuid_ref",
      operation::SblrValueKind::uuid_ref, UuidBody(1)));
  envelope.operands.push_back(Operand(ordinal++, "slot.descriptor_ref",
      operation::SblrValueKind::descriptor_ref, UuidBody(2)));
  envelope.operands.push_back(Operand(ordinal++, "slot.policy_ref",
      operation::SblrValueKind::policy_ref, UuidBody(3)));
  envelope.operands.push_back(Operand(ordinal++, "slot.principal_ref",
      operation::SblrValueKind::principal_ref, UuidBody(4)));
  Bytes literal = UuidBody(5);
  Add64(&literal, 3);
  literal.insert(literal.end(), {0x01, 0x02, 0x03});
  envelope.operands.push_back(Operand(ordinal++, "slot.literal_typed",
      operation::SblrValueKind::literal_typed, literal));
  Bytes parameter;
  Add32(&parameter, 1);
  const auto parameter_uuid = UuidBody(6);
  parameter.insert(parameter.end(), parameter_uuid.begin(), parameter_uuid.end());
  envelope.operands.push_back(Operand(ordinal++, "slot.parameter_slot",
      operation::SblrValueKind::parameter_slot, parameter));
  Bytes target;
  Add32(&target, 1);
  const auto target_uuid = UuidBody(7);
  target.insert(target.end(), target_uuid.begin(), target_uuid.end());
  envelope.operands.push_back(Operand(ordinal++, "slot.result_target",
      operation::SblrValueKind::result_target, target));
  Bytes proof = UuidBody(8);
  Add64(&proof, 2);
  proof.insert(proof.end(), {0xaa, 0xbb});
  envelope.operands.push_back(Operand(ordinal++, "slot.proof_token",
      operation::SblrValueKind::proof_token, proof));
  Bytes epoch;
  Add16(&epoch, 1);
  Add16(&epoch, 0);
  Add64(&epoch, 42);
  envelope.operands.push_back(Operand(ordinal++, "slot.epoch_token",
      operation::SblrValueKind::epoch_token, epoch));
  Bytes profile = UuidBody(9);
  Add64(&profile, 7);
  envelope.operands.push_back(Operand(ordinal++, "slot.profile_ref",
      operation::SblrValueKind::profile_ref, profile));
  Bytes artifact = UuidBody(10);
  Add64(&artifact, 4);
  artifact.push_back(1);
  Add32(&artifact, 0x11223344);
  envelope.operands.push_back(Operand(ordinal++, "slot.artifact_ref",
      operation::SblrValueKind::artifact_ref, artifact));
  envelope.operands.push_back(Operand(ordinal++, "slot.udr_ref",
      operation::SblrValueKind::udr_ref, UuidBody(11)));
  Bytes list;
  Add32(&list, 2);
  const auto nested_null = Nested(operation::SblrValueKind::null_value, {});
  const auto nested_uuid = Nested(operation::SblrValueKind::uuid_ref, UuidBody(12));
  list.insert(list.end(), nested_null.begin(), nested_null.end());
  list.insert(list.end(), nested_uuid.begin(), nested_uuid.end());
  envelope.operands.push_back(Operand(ordinal++, "slot.list",
      operation::SblrValueKind::list, list));
  Bytes map;
  Add32(&map, 2);
  Add32(&map, 1);
  map.push_back('a');
  map.insert(map.end(), nested_null.begin(), nested_null.end());
  Add32(&map, 1);
  map.push_back('b');
  map.insert(map.end(), nested_uuid.begin(), nested_uuid.end());
  envelope.operands.push_back(Operand(ordinal++, "slot.map",
      operation::SblrValueKind::map, map));
  envelope.operands.push_back(Operand(ordinal++, "slot.null",
      operation::SblrValueKind::null_value, {}));
  return envelope;
}

Bytes OperationBytes() {
  const std::string encoded = operation::EncodeSblrEnvelope(CanonicalOperation());
  Require(!encoded.empty(), "canonical SBOP encoder refused its golden fixture");
  return {encoded.begin(), encoded.end()};
}

public_sblr::SblrCanonicalContainer Container(const Bytes& sbop) {
  public_sblr::SblrCanonicalContainer container;
  const auto engine = Uuid(0x21);
  const auto dialect = Uuid(0x22);
  const auto parser = Uuid(0x31);
  const auto bundle = Uuid(0x24);
  const auto request = Uuid(0x25, true);
  std::copy(engine.begin(), engine.end(), container.canonical_anchor.begin());
  std::copy(dialect.begin(), dialect.end(), container.canonical_anchor.begin() + 16);
  std::copy(parser.begin(), parser.end(), container.canonical_anchor.begin() + 32);
  container.canonical_anchor[48] = 1;
  container.canonical_anchor[52] = 1;
  container.canonical_anchor[60] = 1;
  container.canonical_anchor[68] = 1;
  std::copy(bundle.begin(), bundle.end(), container.canonical_anchor.begin() + 76);
  container.canonical_anchor[92] = 1;
  container.canonical_anchor[100] = 2;
  std::copy(request.begin(), request.end(), container.canonical_anchor.begin() + 116);
  container.operation_payload = sbop;
  return container;
}

public_sblr::SblrExecutionEnvelopeV1 Ingress(const Bytes& sbop) {
  public_sblr::SblrExecutionEnvelopeV1 envelope;
  auto& f = envelope.fields;
  f[0] = UuidField(Uuid(0x41, true));
  f[1] = U16(1);
  f[2] = U16(0);
  f[3] = U32(0x00010001);
  f[4] = U16(2);
  f[5] = {0};
  f[6] = InlineReference(sbop);
  f[7] = CrcChecksum(sbop);
  f[8] = U64(sbop.size());
  f[9] = U16(1);
  f[10] = OptionalUuid(Uuid(0x22));
  f[11] = OptionalUuid(Uuid(0x42));
  f[12] = Struct(0x1001, {1});
  f[13] = Struct(0x1002, {2});
  f[14] = OptionalStruct(0x1003, {3});
  f[15] = U64(1);
  f[16] = U32(0);
  f[17] = U32(0);
  f[18] = U32(0);
  f[19] = OptionalStruct(0x1004, {4});
  f[20] = U32(0);
  f[21] = Struct(0x1005, {5});
  f[22] = {0};
  f[23] = {0};
  f[24] = {0};
  f[25] = U16(0);
  f[26] = {0};
  f[27] = {0};
  return envelope;
}

server::ServerSblrAdmissionRequest Request(const Bytes& sbop) {
  const auto container = public_sblr::EncodeSblrContainer(Container(sbop));
  const auto sbee = public_sblr::EncodeSblrExecutionEnvelopeV1(Ingress(sbop));
  Require(!container.empty() && !sbee.empty(), "outer or SBEE golden encoder failed");
  server::ServerSblrAdmissionRequest request;
  request.encoded_sblr_container.assign(container.begin(), container.end());
  request.encoded_execution_envelope.assign(sbee.begin(), sbee.end());
  request.admitted_parser_package_uuid = UuidText(Uuid(0x31));
  request.admitted_parser_package_version_major = 1;
  request.admitted_registry_snapshot_uuid = UuidText(Uuid(0x32));
  request.authenticated_principal_uuid = UuidText(Uuid(0x42));
  request.catalog_snapshot_uuid = UuidText(Uuid(0x43));
  request.engine_mga_statement_uuid = UuidText(Uuid(0x44));
  request.engine_mga_snapshot_uuid = UuidText(Uuid(0x45));
  request.catalog_epoch = 7;
  request.security_epoch = 8;
  request.resource_epoch = 9;
  return request;
}

bool HasDiagnostic(const operation::SblrDecodeResult& result,
                   std::string_view code) {
  return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                     [code](const auto& diagnostic) {
                       return diagnostic.code == code;
                     });
}

void RequireSbopRefused(const Bytes& bytes, std::string_view label) {
  const auto decoded = operation::DecodeSblrEnvelope(
      {reinterpret_cast<const char*>(bytes.data()), bytes.size()});
  Require(!decoded.ok, std::string(label) + " was admitted");
}

void RewriteSbopCrc(Bytes* bytes) {
  const std::size_t trailer = bytes->size() - 16;
  public_sblr::SblrStoreU32(*bytes, trailer + 4,
                            operation::SblrCrc32c(bytes->data(), trailer));
}

std::array<std::uint8_t, 32> ExpectedProvenance(const Bytes& sbop) {
  constexpr char kDomain[] = "ScratchBird.SBOP.ProducerProvenance.V1\0";
  Bytes input(std::begin(kDomain), std::end(kDomain) - 1);
  const auto append_section = [&sbop, &input](std::size_t ordinal) {
    const std::size_t entry = 64 + ordinal * 24;
    const auto offset = public_sblr::SblrReadU64(sbop.data() + entry + 8);
    const auto size = public_sblr::SblrReadU64(sbop.data() + entry + 16);
    input.insert(input.end(), sbop.begin() + static_cast<std::ptrdiff_t>(offset),
                 sbop.begin() + static_cast<std::ptrdiff_t>(offset + size));
  };
  append_section(2);
  append_section(3);
  input.insert(input.end(), sbop.begin() + 16, sbop.begin() + 22);
  append_section(0);
  append_section(1);
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(input);
  Require(digest.ok(), "independent provenance SHA-256 failed");
  return digest.digest;
}

void TestMagicAndRoundTrips(const Bytes& sbop) {
  Require(public_sblr::kSblrEnvelopeMagic == 0x524c4253u,
          "SBLR LE magic changed");
  Require(public_sblr::kSblrExecutionEnvelopeMagic == 0x45454253u,
          "SBEE LE magic changed");
  Require(operation::kSblrOperationMagic == 0x504f4253u,
          "SBOP LE magic changed");
  Require(operation::kSblrOperationTrailerMagic == 0x544f4253u,
          "SBOT LE magic changed");
  Require(std::equal(sbop.begin(), sbop.begin() + 4,
                     std::array<std::uint8_t, 4>{'S', 'B', 'O', 'P'}.begin()),
          "SBOP literal bytes differ");
  Require(std::equal(sbop.end() - 16, sbop.end() - 12,
                     std::array<std::uint8_t, 4>{'S', 'B', 'O', 'T'}.begin()),
          "SBOT literal bytes differ");
  const auto decoded = operation::DecodeSblrEnvelope(
      {reinterpret_cast<const char*>(sbop.data()), sbop.size()});
  Require(decoded.ok && decoded.canonical_bytes == sbop,
          "SBOP decode/re-encode was not exact");
  Require(decoded.envelope.operands.size() == 15,
          "all fifteen typed value kinds were not retained");
  const auto provenance_offset = public_sblr::SblrReadU64(
      sbop.data() + 64 + 8 * 24 + 8);
  const auto provenance_size = public_sblr::SblrReadU64(
      sbop.data() + 64 + 8 * 24 + 16);
  const auto expected_provenance = ExpectedProvenance(sbop);
  Require(provenance_size == expected_provenance.size() &&
              std::equal(expected_provenance.begin(), expected_provenance.end(),
                         sbop.begin() + static_cast<std::ptrdiff_t>(provenance_offset)),
          "producer provenance domain is not the frozen explicit-NUL domain");

  const auto outer = public_sblr::EncodeSblrContainer(Container(sbop));
  Require(outer.size() > 60 && outer[0] == 'S' && outer[1] == 'B' &&
              outer[2] == 'L' && outer[3] == 'R',
          "canonical outer literal is not SBLR");
  const auto decoded_outer = public_sblr::DecodeSblrContainerBytes(
      outer.data(), outer.size());
  Require(decoded_outer.status == public_sblr::SblrCodecStatus::ok &&
              decoded_outer.container.operation_payload == sbop,
          "canonical outer roundtrip failed");

  const auto ingress_value = Ingress(sbop);
  const auto sbee = public_sblr::EncodeSblrExecutionEnvelopeV1(ingress_value);
  Require(sbee.size() > 48 && sbee[0] == 'S' && sbee[1] == 'B' &&
              sbee[2] == 'E' && sbee[3] == 'E',
          "canonical ingress literal is not SBEE");
  const auto decoded_sbee = public_sblr::DecodeSblrExecutionEnvelopeV1Bytes(
      sbee.data(), sbee.size());
  Require(decoded_sbee.status == public_sblr::SblrCodecStatus::ok &&
              decoded_sbee.envelope.fields == ingress_value.fields,
          "SBEE 28-field roundtrip failed");
}

void TestSbopRefusals(const Bytes& golden) {
  auto mutation = golden;
  mutation[0] ^= 1;
  Require(HasDiagnostic(operation::DecodeSblrEnvelope(
              {reinterpret_cast<const char*>(mutation.data()), mutation.size()}),
              "SBLR.OPERATION.MAGIC_INVALID"),
          "bad SBOP magic did not receive its stable diagnostic");

  mutation = golden;
  mutation[4] = 2;
  Require(HasDiagnostic(operation::DecodeSblrEnvelope(
              {reinterpret_cast<const char*>(mutation.data()), mutation.size()}),
              "SBLR.OPERATION.VERSION_INVALID"),
          "bad SBOP version was admitted");

  mutation = golden;
  mutation[64] = 9;
  RewriteSbopCrc(&mutation);
  Require(HasDiagnostic(operation::DecodeSblrEnvelope(
              {reinterpret_cast<const char*>(mutation.data()), mutation.size()}),
              "SBLR.OPERATION.SECTION_TABLE_INVALID"),
          "wrong section tag was admitted");

  mutation = golden;
  mutation[68] = 0;
  RewriteSbopCrc(&mutation);
  Require(HasDiagnostic(operation::DecodeSblrEnvelope(
              {reinterpret_cast<const char*>(mutation.data()), mutation.size()}),
              "SBLR.OPERATION.SECTION_TABLE_INVALID"),
          "absent required section was admitted");

  mutation = golden;
  const std::uint64_t first_offset = public_sblr::SblrReadU64(mutation.data() + 72);
  public_sblr::SblrStoreU64(mutation, 72, first_offset + 1);
  RewriteSbopCrc(&mutation);
  Require(HasDiagnostic(operation::DecodeSblrEnvelope(
              {reinterpret_cast<const char*>(mutation.data()), mutation.size()}),
              "SBLR.OPERATION.SECTION_OVERLAP_OR_GAP"),
          "section gap was admitted");

  mutation = golden;
  mutation[mutation.size() - 12] ^= 1;
  Require(HasDiagnostic(operation::DecodeSblrEnvelope(
              {reinterpret_cast<const char*>(mutation.data()), mutation.size()}),
              "SBLR.OPERATION.CRC_MISMATCH"),
          "SBOP CRC mutation was admitted");

  mutation = golden;
  const std::uint64_t provenance_offset = public_sblr::SblrReadU64(
      mutation.data() + 64 + 8 * 24 + 8);
  mutation[provenance_offset] ^= 1;
  RewriteSbopCrc(&mutation);
  Require(HasDiagnostic(operation::DecodeSblrEnvelope(
              {reinterpret_cast<const char*>(mutation.data()), mutation.size()}),
              "SBLR.OPERATION.PROVENANCE_MISMATCH"),
          "producer provenance mutation was admitted");

  mutation = golden;
  mutation.push_back(0);
  Require(HasDiagnostic(operation::DecodeSblrEnvelope(
              {reinterpret_cast<const char*>(mutation.data()), mutation.size()}),
              "SBLR.OPERATION.TOTAL_SIZE_MISMATCH"),
          "trailing operation byte was admitted");

  const auto zero = operation::ValidateSblrOpcodeIdentity(
      0, "query.execute", "SBLR_QUERY_EXECUTE");
  const auto unknown = operation::ValidateSblrOpcodeIdentity(
      0xffff, "query.execute", "SBLR_QUERY_EXECUTE");
  const auto swapped = operation::ValidateSblrOpcodeIdentity(
      0x1207, "query.execute", "SBLR_QUERY_PREPARE");
  Require(!zero.ok && !unknown.ok && !swapped.ok &&
              swapped.diagnostic_id == "SBLR.OPERATION.OPCODE_IDENTITY_MISMATCH",
          "numeric/key/mnemonic mismatch did not fail closed");

  auto bad_operand = CanonicalOperation();
  bad_operand.operands[0].ordinal = 2;
  Require(operation::EncodeSblrEnvelope(bad_operand).empty(),
          "bad operand ordinal was encoded");
  bad_operand = CanonicalOperation();
  bad_operand.operands[13].value_body[25] = 'a';
  Require(operation::EncodeSblrEnvelope(bad_operand).empty(),
          "duplicate/unsorted map key was encoded");
  bad_operand = CanonicalOperation();
  bad_operand.operands[14].value_body.push_back(0);
  Require(operation::EncodeSblrEnvelope(bad_operand).empty(),
          "nonempty null body was encoded");
  bad_operand = CanonicalOperation();
  bad_operand.operands[0].name = "Slot.Invalid";
  Require(operation::EncodeSblrEnvelope(bad_operand).empty(),
          "noncanonical slot text was encoded");
  bad_operand = CanonicalOperation();
  bad_operand.operands[4].value_body.resize(24 + 65'537, 0);
  public_sblr::SblrStoreU64(bad_operand.operands[4].value_body, 16, 65'537);
  Require(operation::EncodeSblrEnvelope(bad_operand).empty(),
          "scalar limit overflow was encoded");
  bad_operand = CanonicalOperation();
  bad_operand.operands[0].value_flags = 1;
  Require(operation::EncodeSblrEnvelope(bad_operand).empty(),
          "reserved operand flag was encoded");
  bad_operand = CanonicalOperation();
  bad_operand.operands[10].value_body[24] = 0;
  Require(operation::EncodeSblrEnvelope(bad_operand).empty(),
          "unknown artifact checksum kind was encoded");
  bad_operand = CanonicalOperation();
  bad_operand.operation_id = "Query.execute";
  Require(operation::EncodeSblrEnvelope(bad_operand).empty(),
          "invalid operation-key grammar was encoded");
  bad_operand = CanonicalOperation();
  bad_operand.trace_key = "trace\ncontrol";
  Require(operation::EncodeSblrEnvelope(bad_operand).empty(),
          "C0 control text was encoded");
  bad_operand = CanonicalOperation();
  bad_operand.result_shape.assign(1025, 'a');
  Require(operation::EncodeSblrEnvelope(bad_operand).empty(),
          "shape text limit was exceeded");
  bad_operand = CanonicalOperation();
  bad_operand.operands[0].value = "legacy_text_lane";
  Require(operation::EncodeSblrEnvelope(bad_operand).empty(),
          "legacy text operand lane was encoded");

  Bytes nested = Nested(operation::SblrValueKind::null_value, {});
  for (std::uint32_t depth = 0; depth < operation::kSblrOperationMaximumDepth; ++depth) {
    Bytes body;
    Add32(&body, 1);
    body.insert(body.end(), nested.begin(), nested.end());
    nested = Nested(operation::SblrValueKind::list, body);
  }
  bad_operand = CanonicalOperation();
  bad_operand.operands[12].value_body.assign(nested.begin() + 12, nested.end());
  Require(operation::EncodeSblrEnvelope(bad_operand).empty(),
          "nested value depth limit was exceeded");
  bad_operand = CanonicalOperation();
  bad_operand.operands[12].value_body = U32(operation::kSblrOperationMaximumValues);
  Require(operation::EncodeSblrEnvelope(bad_operand).empty(),
          "aggregate value-count limit was exceeded");

  for (std::size_t index = 0; index + 1 < bad_operand.operands.size(); ++index) {
    bad_operand = CanonicalOperation();
    bad_operand.operands[index].value_body.clear();
    Require(operation::EncodeSblrEnvelope(bad_operand).empty(),
            "malformed typed operand body was encoded");
  }
  bad_operand = CanonicalOperation();
  bad_operand.operands[12].value_body[6] = 1;
  Require(operation::EncodeSblrEnvelope(bad_operand).empty(),
          "nested reserved value flag was encoded");

  bad_operand = CanonicalOperation();
  bad_operand.operation_id = "query..execute";
  Require(operation::EncodeSblrEnvelope(bad_operand).empty(),
          "empty operation-key component was encoded");
  bad_operand = CanonicalOperation();
  bad_operand.opcode = "sblr_query_execute";
  Require(operation::EncodeSblrEnvelope(bad_operand).empty(),
          "lowercase opcode mnemonic was encoded");
  bad_operand = CanonicalOperation();
  bad_operand.diagnostic_shape = std::string("bad\0shape", 9);
  Require(operation::EncodeSblrEnvelope(bad_operand).empty(),
          "NUL diagnostic identity was encoded");
  bad_operand = CanonicalOperation();
  bad_operand.trace_key = std::string("\xc0\x80", 2);
  Require(operation::EncodeSblrEnvelope(bad_operand).empty(),
          "non-shortest UTF-8 was encoded");

  bad_operand = CanonicalOperation();
  auto& sha_artifact = bad_operand.operands[10].value_body;
  sha_artifact.resize(57, 0x5a);
  sha_artifact[24] = 2;
  const std::string sha_encoded = operation::EncodeSblrEnvelope(bad_operand);
  Require(!sha_encoded.empty() && operation::DecodeSblrEnvelope(sha_encoded).ok,
          "SHA-256 artifact checksum kind did not roundtrip");
}

void TestSbopFixedBoundaryMatrix(const Bytes& golden) {
  for (std::size_t offset = 0; offset < 64; ++offset) {
    auto mutation = golden;
    mutation[offset] ^= 1;
    RewriteSbopCrc(&mutation);
    RequireSbopRefused(mutation, "SBOP header-byte mutation");
  }
  for (std::size_t offset = 64; offset < 64 + 9 * 24; ++offset) {
    auto mutation = golden;
    mutation[offset] ^= 1;
    RewriteSbopCrc(&mutation);
    RequireSbopRefused(mutation, "SBOP section-table-byte mutation");
  }

  auto mutation = golden;
  mutation.resize(mutation.size() - 1);
  RequireSbopRefused(mutation, "SBOP truncation");
  mutation = golden;
  public_sblr::SblrStoreU64(mutation, 64 + 16,
                            static_cast<std::uint64_t>(mutation.size()));
  RewriteSbopCrc(&mutation);
  RequireSbopRefused(mutation, "SBOP section overrun");
}

void TestOpcodeRegistryMatrix() {
  const auto& registry = operation::StaticSblrOpcodeRegistry();
  Require(!registry.empty(), "public opcode registry is empty");
  std::vector<std::uint16_t> codes;
  codes.reserve(registry.size());
  for (const auto& entry : registry) {
    if (entry.code != 0) codes.push_back(entry.code);
  }
  std::sort(codes.begin(), codes.end());
  std::size_t assigned_rows = 0;
  std::size_t unassigned_rows = 0;
  std::size_t duplicate_rows = 0;
  for (const auto& entry : registry) {
    if (entry.code == 0) {
      ++unassigned_rows;
      const auto refused = operation::ValidateSblrOpcodeIdentity(
          entry.code, entry.operation_id, entry.opcode);
      Require(!refused.ok &&
                  refused.diagnostic_id ==
                      "SBLR.OPERATION.OPCODE_IDENTITY_MISMATCH",
              "unassigned legacy registry row did not fail closed");
      continue;
    }
    ++assigned_rows;
    const bool duplicate =
        std::equal_range(codes.begin(), codes.end(), entry.code).second -
            std::equal_range(codes.begin(), codes.end(), entry.code).first >
        1;
    const auto validation = operation::ValidateSblrOpcodeIdentity(
        entry.code, entry.operation_id, entry.opcode);
    if (duplicate) {
      ++duplicate_rows;
      Require(operation::LookupSblrOpcodeCode(entry.code) == nullptr &&
                  !validation.ok &&
                  validation.diagnostic_id ==
                      "SBLR.OPERATION.OPCODE_IDENTITY_MISMATCH",
              "duplicate public opcode identity did not fail closed");
    } else {
      Require(operation::LookupSblrOpcodeCode(entry.code) == &entry &&
                  validation.ok,
              std::string("canonical numeric/key/mnemonic registry row is not exact: ") +
                  entry.opcode + " code=" + std::to_string(entry.code));
    }
  }
  Require(assigned_rows != 0 && unassigned_rows != 0 && duplicate_rows != 0,
          "assigned/unassigned/duplicate public registry partition changed");
}

void TestAdmissionAndImmutableToken(const Bytes& sbop) {
  std::size_t dispatch_calls = 0;
  auto request = Request(sbop);
  const auto admitted = server::AdmitServerSblrEnvelope(request);
  Require(admitted.admitted && admitted.admission_token &&
              admitted.operation_id == "query.execute",
          "canonical bundle did not produce an immutable admission token");
  const auto token_operation_bytes = admitted.admission_token->canonical_operation_bytes;
  request.encoded_sblr_container.assign("destroyed after admission");
  request.encoded_execution_envelope.assign("destroyed after admission");
  Require(server::DispatchAdmittedServerSblrToken(
              admitted.admission_token,
              [&dispatch_calls, &token_operation_bytes](const auto& token) {
                ++dispatch_calls;
                Require(token.canonical_operation_bytes == token_operation_bytes,
                        "token changed when original bytes changed");
                Require(token.operation.operation_id == "query.execute",
                        "dispatch consumer did not receive typed operation");
              }) && dispatch_calls == 1,
          "success did not dispatch exactly once through the immutable token");

  const auto expect_refusal = [&dispatch_calls](server::ServerSblrAdmissionRequest bad,
                                                std::string_view label) {
    const auto before = dispatch_calls;
    const auto result = server::AdmitServerSblrEnvelope(bad);
    Require(!result.admitted && !result.admission_token,
            std::string(label) + " was admitted");
    Require(!server::DispatchAdmittedServerSblrToken(
                result.admission_token,
                [&dispatch_calls](const auto&) { ++dispatch_calls; }) &&
                dispatch_calls == before,
            std::string(label) + " reached the dispatch probe");
  };

  auto bad = Request(sbop);
  bad.encoded_sblr_container.back() ^= 1;
  expect_refusal(std::move(bad), "outer CRC mutation");
  bad = Request(sbop);
  bad.encoded_execution_envelope.back() ^= 1;
  expect_refusal(std::move(bad), "SBEE field-section mutation");
  bad = Request(sbop);
  bad.admitted_registry_snapshot_uuid = UuidText(Uuid(0x55));
  expect_refusal(std::move(bad), "registry generation mismatch");
  bad = Request(sbop);
  bad.engine_mga_statement_uuid.clear();
  expect_refusal(std::move(bad), "missing engine MGA statement authority");
  bad = Request(sbop);
  bad.authenticated_principal_uuid = UuidText(Uuid(0x56));
  expect_refusal(std::move(bad), "authenticated principal mismatch");
  bad = {};
  bad.encoded_sblr_envelope = "operation_id=query.execute\nopcode=SBLR_QUERY_EXECUTE\n";
  expect_refusal(std::move(bad), "legacy newline operation payload");
  bad = {};
  bad.encoded_sblr_envelope.assign(32, '\0');
  bad.encoded_sblr_envelope.replace(0, 4, "SBLR");
  expect_refusal(std::move(bad), "legacy 32-byte FNV frame");
}

void TestOuterAndIngressMutations(const Bytes& sbop) {
  auto outer = public_sblr::EncodeSblrContainer(Container(sbop));
  auto mutation = outer;
  mutation[0] ^= 1;
  Require(public_sblr::DecodeSblrContainerBytes(mutation.data(), mutation.size()).status !=
              public_sblr::SblrCodecStatus::ok,
          "outer magic mutation was admitted");
  mutation = outer;
  mutation[40] = 0x20;
  Require(public_sblr::DecodeSblrContainerBytes(mutation.data(), mutation.size()).status !=
              public_sblr::SblrCodecStatus::ok,
          "out-of-order outer section was admitted");
  mutation = outer;
  mutation.push_back(0);
  Require(public_sblr::DecodeSblrContainerBytes(mutation.data(), mutation.size()).status !=
              public_sblr::SblrCodecStatus::ok,
          "outer trailing byte was admitted");

  auto ingress = Ingress(sbop);
  auto sbee = public_sblr::EncodeSblrExecutionEnvelopeV1(ingress);
  mutation = sbee;
  mutation[0] ^= 1;
  Require(public_sblr::DecodeSblrExecutionEnvelopeV1Bytes(
              mutation.data(), mutation.size()).status != public_sblr::SblrCodecStatus::ok,
          "SBEE magic mutation was admitted");
  mutation = sbee;
  mutation[24] ^= 1;
  Require(public_sblr::DecodeSblrExecutionEnvelopeV1Bytes(
              mutation.data(), mutation.size()).status ==
              public_sblr::SblrCodecStatus::checksum_invalid,
          "SBEE CRC mutation was admitted");
  ingress.fields[5] = InlineReference(sbop);
  Require(public_sblr::EncodeSblrExecutionEnvelopeV1(ingress).empty(),
          "simultaneous opcode and operation references were encoded");
  ingress = Ingress(sbop);
  ingress.fields[7] = {0};
  Require(public_sblr::EncodeSblrExecutionEnvelopeV1(ingress).empty(),
          "absent payload checksum was encoded");
  ingress = Ingress(sbop);
  ingress.fields[11] = {0};
  Require(public_sblr::EncodeSblrExecutionEnvelopeV1(ingress).empty(),
          "absent effective user without bootstrap flag was encoded");
  ingress = Ingress(sbop);
  ingress.fields[27] = {1};
  Require(public_sblr::EncodeSblrExecutionEnvelopeV1(ingress).empty(),
          "cluster context without header flag was encoded");

  const std::array<Bytes, 28> invalid_fields{{
      Bytes(16, 0), U16(2), U16(1), U32(0), U16(0), Bytes{2}, Bytes{0},
      Bytes{0}, U64(0), U16(0), Bytes{2}, Bytes{0}, Bytes{0}, Bytes{0},
      Bytes{2}, Bytes(7, 0), U32(262'145), U32(131'073), U32(262'145),
      Bytes{2}, U32(4'097), Bytes{0}, Bytes{2}, Bytes{2}, Bytes{3},
      U16(5), Bytes{2}, Bytes{2}}};
  for (std::size_t ordinal = 0; ordinal < invalid_fields.size(); ++ordinal) {
    auto invalid = Ingress(sbop);
    invalid.fields[ordinal] = invalid_fields[ordinal];
    Require(public_sblr::EncodeSblrExecutionEnvelopeV1(invalid).empty(),
            std::string("invalid SBEE ordinal field was encoded: ") +
                std::to_string(ordinal + 1));
  }

  const std::array<std::size_t, 10> header_offsets{{
      0, 4, 6, 8, 10, 12, 16, 24, 28, 32}};
  for (const auto offset : header_offsets) {
    mutation = sbee;
    mutation[offset] ^= offset == 12 ? 0x10 : 1;
    Require(public_sblr::DecodeSblrExecutionEnvelopeV1Bytes(
                mutation.data(), mutation.size()).status !=
                public_sblr::SblrCodecStatus::ok,
            "invalid SBEE header field was admitted");
  }
  mutation = sbee;
  mutation[40] = 1;
  Require(public_sblr::DecodeSblrExecutionEnvelopeV1Bytes(
              mutation.data(), mutation.size()).status !=
              public_sblr::SblrCodecStatus::ok,
          "nonzero SBEE reserved tail was admitted");
}

}  // namespace

int main() {
  const Bytes sbop = OperationBytes();
  TestMagicAndRoundTrips(sbop);
  TestSbopRefusals(sbop);
  TestSbopFixedBoundaryMatrix(sbop);
  TestOpcodeRegistryMatrix();
  TestOuterAndIngressMutations(sbop);
  TestAdmissionAndImmutableToken(sbop);
  std::cout << "qow_sblr_codec_v1=passed\n"
            << "evidence_level=E1_E2_development_candidate_only\n"
            << "dispatch_probe_success_calls=1\n"
            << "legacy_executable_routes=0\n"
            << "mga_authority=engine_owned_only\n";
  return 0;
}
