// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Structural codec evidence, not receipt authority or query execution.
#include "engine/sblr/relational_descriptor_codec.hpp"
#include "engine/sblr/sblr_engine_envelope.hpp"
#include "binder/descriptor_authority.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>

namespace {
long fail_after = -1;
std::size_t allocations = 0;
}
void* operator new(std::size_t size) {
  ++allocations;
  if (fail_after >= 0 && fail_after-- == 0) throw std::bad_alloc{};
  if (void* memory = std::malloc(size ? size : 1)) return memory;
  throw std::bad_alloc{};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace wire = scratchbird::engine::sblr;
namespace api = scratchbird::engine::internal_api;
using Uuid = scratchbird::core::platform::Uuid;
using Bytes = std::vector<std::uint8_t>;
using Descriptor = api::RelationalTypeDescriptor;
namespace {
std::size_t checks = 0, faults = 0;
void Require(bool condition, const char* detail) {
  ++checks;
  if (!condition) throw std::runtime_error(detail);
}
Uuid Id(std::uint8_t suffix) {
  return {{0x01,0xa0,0x7c,0x0a,0x0d,0x5c,0x70,0,0x80,0,0xff,0,0,0,0,suffix}};
}
Descriptor Fixture(unsigned flags = 63) {
  Descriptor d;
  d.descriptor_id = 0x01020304;
  d.descriptor_uuid = Id(1); d.type_uuid = Id(2);
  d.nullability = api::RelationalNullability::kNullable;
  if (flags & 2) d.collation_uuid = Id(3);
  if (flags & 4) d.timezone_profile_id = "fixture/timezone/profile";
  if (flags & 8) d.width = 0;
  if (flags & 16) d.precision = 32;
  if (flags & 32) d.scale = 0;
  if (flags & 1) {
    d.datatype_identity_authoritative = true;
    d.descriptor_generation = 0x0102030405060708ULL;
    d.type_generation = 2; d.codec_generation = 3;
    d.datatype_catalog_generation = 4; d.datatype_registry_generation = 5;
    d.codec_version = 0x1234; d.codec_id = "fixture.descriptor.codec";
    d.statement_receipt_uuid = Id(4); d.datatype_catalog_snapshot_uuid = Id(5);
  }
  return d;
}
void Append(Bytes& out, std::uint64_t value, unsigned width) {
  for (unsigned i = 0; i < width; ++i) out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}
Bytes Oracle(const Descriptor& d) {
  Bytes out;
  Append(out, 1, 2);
  Append(out, (d.datatype_identity_authoritative ? 1 : 0) | (d.collation_uuid ? 2 : 0) |
      (d.timezone_profile_id ? 4 : 0) | (d.width ? 8 : 0) | (d.precision ? 16 : 0) | (d.scale ? 32 : 0), 2);
  Append(out, d.descriptor_id, 4);
  for (const auto uuid : {d.descriptor_uuid, d.type_uuid, d.collation_uuid.value_or(Uuid{}),
                         d.statement_receipt_uuid, d.datatype_catalog_snapshot_uuid})
    out.insert(out.end(), uuid.bytes.begin(), uuid.bytes.end());
  for (const auto generation : {d.descriptor_generation, d.type_generation, d.codec_generation,
                               d.datatype_catalog_generation, d.datatype_registry_generation})
    Append(out, generation, 8);
  Append(out, d.codec_version, 2); Append(out, static_cast<std::uint8_t>(d.nullability), 1);
  Append(out, 0, 1); Append(out, d.width.value_or(0), 4); Append(out, d.precision.value_or(0), 4);
  Append(out, d.scale.value_or(0), 4); Append(out, d.codec_id.size(), 4);
  Append(out, d.timezone_profile_id ? d.timezone_profile_id->size() : 0, 4);
  Require(out.size() == 152, "oracle fixed prefix differs from specification");
  out.insert(out.end(), d.codec_id.begin(), d.codec_id.end());
  if (d.timezone_profile_id) out.insert(out.end(), d.timezone_profile_id->begin(), d.timezone_profile_id->end());
  return out;
}
void RoundTrip(const Descriptor& d) {
  const auto expected = Oracle(d);
  Bytes actual;
  Require(wire::EncodeRelationalTypeDescriptorV1(d, &actual), "valid descriptor encoding refused");
  Require(actual == expected, "descriptor byte encoding differs from independent oracle");
  Descriptor decoded;
  Require(wire::DecodeRelationalTypeDescriptorV1(expected.data(), expected.size(), &decoded), "valid descriptor decoding refused");
  Require(Oracle(decoded) == expected, "descriptor decoding changed a field or optional presence");
}
void Reject(const Bytes& bytes) {
  auto sentinel = Fixture();
  const auto before = Oracle(sentinel);
  Require(!wire::DecodeRelationalTypeDescriptorV1(bytes.data(), bytes.size(), &sentinel), "malformed descriptor accepted");
  Require(Oracle(sentinel) == before, "failed decode modified published descriptor");
}
void TestLayout() {
  for (unsigned flags = 0; flags < 64; ++flags) {
    auto d = Fixture(flags);
    const bool valid = !(flags & 32) || (flags & 16);
    Require(wire::ValidateRelationalTypeDescriptorV1(d) == valid, "presence/scale validation differs");
    if (valid) RoundTrip(d); else Reject(Oracle(d));
  }
  auto d = Fixture();
  d.codec_id.assign(256, 'x'); d.timezone_profile_id = std::string(1024, 'y');
  RoundTrip(d);
  Require(Oracle(d).size() == 1432, "maximum descriptor size differs");
  d.codec_id.push_back('x');
  Bytes sentinel{0xaa};
  Require(!wire::EncodeRelationalTypeDescriptorV1(d, &sentinel) && sentinel == Bytes{0xaa}, "oversized codec changed encode output");
  auto base = Oracle(Fixture());
  for (std::size_t size = 0; size < base.size(); ++size) Reject(Bytes(base.begin(), base.begin() + size));
  auto changed = base; changed.push_back(0); Reject(changed);
  for (const auto offset : {0u, 1u, 3u, 131u}) {
    changed = base; changed[offset] ^= 0x80; Reject(changed);
  }
  for (const auto offset : {8u, 24u, 40u, 56u, 72u}) {
    for (unsigned value = 0; value < 256; ++value) {
      for (const auto position : {6u, 8u}) {
        const bool valid = position == 6 ? value >> 4 == 7 : value >> 6 == 2;
        changed = base; changed[offset + position] = static_cast<std::uint8_t>(value);
        if (!valid) Reject(changed);
        else {
          Descriptor output;
          Require(wire::DecodeRelationalTypeDescriptorV1(changed.data(), changed.size(), &output), "valid identity bits refused");
          Require(Oracle(output) == changed, "decoder altered identity bits");
        }
      }
    }
  }
  for (const auto offset : {88u, 96u, 104u, 112u, 120u}) {
    changed = base; std::fill_n(changed.begin() + offset, 8, 0); Reject(changed);
  }
  for (unsigned nullability = 0; nullability < 256; ++nullability) {
    if (nullability == 1 || nullability == 2) continue;
    changed = base; changed[130] = static_cast<std::uint8_t>(nullability); Reject(changed);
  }
  changed = base; changed[2] &= ~1; Reject(changed);
  for (const auto offset : {144u, 148u}) {
    changed = base; std::fill_n(changed.begin() + offset, 4, 0xff); Reject(changed);
  }
  for (const auto bad : {std::string("bad\0codec", 9), std::string("\xc0\x80", 2), std::string("\xed\xa0\x80", 3)}) {
    d = Fixture(); d.codec_id = bad; Reject(Oracle(d));
    d = Fixture(); d.timezone_profile_id = bad; Reject(Oracle(d));
  }
}
void TestAllocationAtomicity() {
  const auto d = Fixture(); const auto encoded = Oracle(d);
  Bytes bytes{0xaa};
  allocations = 0;
  Require(wire::EncodeRelationalTypeDescriptorV1(d, &bytes), "encode allocation baseline failed");
  const auto encode_sites = allocations;
  for (std::size_t site = 0; site < encode_sites; ++site) {
    bytes = {0xaa}; bool threw = false;
    fail_after = static_cast<long>(site);
    try { (void)wire::EncodeRelationalTypeDescriptorV1(d, &bytes); } catch (const std::bad_alloc&) { threw = true; }
    fail_after = -1;
    Require(threw && bytes == Bytes{0xaa}, "encode allocation failure published output"); ++faults;
  }
  Descriptor decoded;
  allocations = 0;
  Require(wire::DecodeRelationalTypeDescriptorV1(encoded.data(), encoded.size(), &decoded), "decode allocation baseline failed");
  const auto decode_sites = allocations;
  for (std::size_t site = 0; site < decode_sites; ++site) {
    decoded = Fixture(0); const auto before = Oracle(decoded); bool threw = false;
    fail_after = static_cast<long>(site);
    try { (void)wire::DecodeRelationalTypeDescriptorV1(encoded.data(), encoded.size(), &decoded); } catch (const std::bad_alloc&) { threw = true; }
    fail_after = -1;
    Require(threw && Oracle(decoded) == before, "decode allocation failure published output"); ++faults;
  }
  Require(encode_sites > 0 && decode_sites >= 2, "allocation fault coverage was empty");
}
void TestOperationPlacement() {
  auto d = Fixture(); d.descriptor_id = 1;
  wire::SblrOperand operand;
  operand.ordinal = 1; operand.type = "relational_descriptor_v3"; operand.name = "slot_1";
  operand.value_kind = wire::SblrValueKind::relational_type_descriptor;
  Require(wire::EncodeRelationalTypeDescriptorV1(d, &operand.value_body), "SBOP fixture encoding failed");
  auto envelope = wire::MakeSblrEnvelope("query.execute", "SBLR_QUERY_EXECUTE", "descriptor.codec.fixture");
  envelope.opcode_code = 4615; envelope.parser_package_uuid = Id(6); envelope.registry_snapshot_uuid = Id(7);
  envelope.result_shape = "query_execute_result"; envelope.diagnostic_shape = "diagnostic_vector";
  envelope.operands = {operand};
  const auto bytes = wire::EncodeSblrEnvelope(envelope);
  Require(!bytes.empty() && wire::DecodeSblrEnvelope(bytes).ok, "binary descriptor SBOP round trip failed");
  for (const auto name : {"slot_01", "slot_2", "1", "slot_0"}) {
    auto wrong = envelope; wrong.operands.front().name = name;
    Require(!wire::ValidateSblrEnvelope(wrong).ok, "descriptor occurrence handle substitution accepted");
  }
  auto wrong = envelope; wrong.operation_id = "engine.op.package_begin"; wrong.opcode = "SBLR_PACKAGE_BEGIN"; wrong.opcode_code = 1;
  Require(!wire::ValidateSblrEnvelope(wrong).ok, "binary descriptor escaped query root");
  const auto descriptor_uuid = Id(10);
  Bytes wrapped(descriptor_uuid.bytes.begin(), descriptor_uuid.bytes.end());
  Append(wrapped, operand.value_body.size(), 8);
  wrapped.insert(wrapped.end(), operand.value_body.begin(), operand.value_body.end());
  for (const auto type : {"relational_descriptor_v1", "relational_descriptor_v2", "relational_descriptor_v3"}) {
    wrong = envelope; wrong.operands.front().type = type;
    wrong.operands.front().value_kind = wire::SblrValueKind::literal_typed;
    wrong.operands.front().value_body = wrapped;
    Require(!wire::ValidateSblrEnvelope(wrong).ok, "typed literal wrapper bypassed relational descriptor kind");
  }
  Bytes nested; Append(nested, 1, 4); Append(nested, 213, 2); Append(nested, 0, 2); Append(nested, operand.value_body.size(), 8);
  nested.insert(nested.end(), operand.value_body.begin(), operand.value_body.end());
  wrong = envelope; wrong.operands.front().value_kind = wire::SblrValueKind::list; wrong.operands.front().value_body = nested;
  Require(!wire::ValidateSblrEnvelope(wrong).ok, "binary descriptor nested outside registered slot");
}
void TestNumericBinding() {
  namespace parser = scratchbird::parser::sbsql;
  auto lowered = Fixture(1);
  lowered.nullability = api::RelationalNullability::kNonNull;
  lowered.precision = 20; lowered.scale = 5;
  parser::NativeDescriptorBindingInput expected;
  expected.nullability = parser::BoundNullability::kNonNull;
  expected.descriptor_uuid = lowered.descriptor_uuid;
  expected.type_uuid = lowered.type_uuid;
  expected.descriptor_generation = lowered.descriptor_generation;
  expected.type_generation = lowered.type_generation;
  expected.codec_id = lowered.codec_id;
  expected.codec_version = lowered.codec_version;
  expected.codec_generation = lowered.codec_generation;
  expected.statement_receipt_uuid = lowered.statement_receipt_uuid;
  expected.datatype_catalog_snapshot_uuid = lowered.datatype_catalog_snapshot_uuid;
  expected.datatype_catalog_generation = lowered.datatype_catalog_generation;
  expected.datatype_registry_generation = lowered.datatype_registry_generation;
  expected.width_precision_scale.precision = lowered.precision;
  expected.width_precision_scale.scale = lowered.scale;
  Require(parser::MatchesNativeNumericDescriptorRecord(lowered, expected), "actual numeric binding rejected exact binary descriptor");
  for (auto member : {&Descriptor::descriptor_uuid, &Descriptor::type_uuid,
                      &Descriptor::statement_receipt_uuid, &Descriptor::datatype_catalog_snapshot_uuid}) {
    for (std::size_t byte = 0; byte < 16; ++byte) {
      auto wrong = lowered; (wrong.*member).bytes[byte] ^= 1;
      Require(!parser::MatchesNativeNumericDescriptorRecord(wrong, expected), "numeric binding lost identity byte");
    }
  }
  for (auto member : {&Descriptor::descriptor_generation, &Descriptor::type_generation,
                      &Descriptor::codec_generation, &Descriptor::datatype_catalog_generation,
                      &Descriptor::datatype_registry_generation}) {
    auto wrong = lowered; ++(wrong.*member);
    Require(!parser::MatchesNativeNumericDescriptorRecord(wrong, expected), "numeric binding lost generation");
  }
  auto wrong = lowered; wrong.width = 0;
  Require(!parser::MatchesNativeNumericDescriptorRecord(wrong, expected), "numeric binding conflated absent and zero width");
  wrong = lowered; wrong.precision.reset();
  Require(!parser::MatchesNativeNumericDescriptorRecord(wrong, expected), "numeric binding dropped precision");
  wrong = lowered; wrong.scale = 0;
  Require(!parser::MatchesNativeNumericDescriptorRecord(wrong, expected), "numeric binding changed scale");
  wrong = lowered; wrong.codec_id += ".other";
  Require(!parser::MatchesNativeNumericDescriptorRecord(wrong, expected), "numeric binding ignored codec");
  wrong = lowered; ++wrong.codec_version;
  Require(!parser::MatchesNativeNumericDescriptorRecord(wrong, expected), "numeric binding ignored codec version");
  wrong = lowered; wrong.datatype_identity_authoritative = false;
  Require(!parser::MatchesNativeNumericDescriptorRecord(wrong, expected), "numeric binding accepted absent authority");
}
}
int main() {
  try {
    TestLayout(); TestAllocationAtomicity(); TestOperationPlacement(); TestNumericBinding();
    std::cout << "PASS relational descriptor checks=" << checks << " allocation_faults=" << faults << '\n';
    return 0;
  } catch (const std::exception& error) {
    fail_after = -1;
    std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n'; return 1;
  }
}
