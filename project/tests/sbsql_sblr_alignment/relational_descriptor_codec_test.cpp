#include "security/native_identity_option.hpp"
#include "engine/sblr/native_shard_placement.hpp"
// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Structural codec evidence, not receipt authority or query execution.
#include "engine/sblr/relational_descriptor_codec.hpp"
#include "engine/sblr/prepared_relational_query.hpp"
#include "wire/parser_server_ipc/sbps_name_resolution_request_codec.hpp"
#include "wire/parser_server_ipc/sbps_session_context_codec.hpp"
#include "wire/parser_server_ipc/public_relation_type_shape_codec.hpp"
#include "wire/parser_server_ipc/public_relation_projection_codec.hpp"
#include "wire/parser_server_ipc/sbps_name_metadata_reply_codec.hpp"
#include "hash_digest.hpp"
#include "wire/projection_fields.hpp"
#include "wire/language_bundle_identity.hpp"
#include "engine/sblr/native_row_field.hpp"
#include "server/native_identity_selector.hpp"
#include "engine/sblr/sblr_engine_envelope.hpp"
#include "binder/descriptor_authority.hpp"
#include "binder/relational_property_identity.hpp"
#include "binder/engine_function_identity.hpp"
#include "wire/contextual_operand_freeze.hpp"
#include "lowering/relational_identity_operand.hpp"
#include "wire/native_query_artifact.hpp"
#include "engine/internal_api/query/contextual_text_descriptor_match.hpp"
#include "engine/internal_api/dml/datatype_operator_registry_projection.hpp"
#include "engine/internal_api/security/security_principal_lifecycle.hpp"
#include "engine/internal_api/mga_relation_store/mga_binary_identity_codec.hpp"
#include "engine/internal_api/mga_relation_store/mga_relation_descriptor.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <new>
#include <stdexcept>
#include <type_traits>

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
using Expression = api::RelationalExpressionRecord;
Expression ExpressionFixture(unsigned flags = 31) {
  Expression e;
  e.expression_id = 0x01020304; e.result_descriptor_id = 0x11223344;
  e.expression_kind = api::RelationalExpressionKind::kBinary;
  e.child_expression_ids = {1, 0x09080706, 1};
  if (flags & 1) e.function_uuid = Id(81);
  if (flags & 2) e.bound_name_uuid = Id(82);
  if (flags & 4) e.literal_kind = api::RelationalLiteralKind::kString;
  if (flags & 8) e.operator_name = "independent_operator_fixture";
  if (flags & 16) e.literal_or_parameter_ref = std::string("literal\0with|delimiters,and:bytes", 32);
  return e;
}
Bytes ExpressionOracle(const Expression& e) {
  Bytes out;
  Append(out, 1, 2);
  Append(out, (e.function_uuid ? 1 : 0) | (e.bound_name_uuid ? 2 : 0) |
      (e.literal_kind ? 4 : 0) | (e.operator_name ? 8 : 0) | (e.literal_or_parameter_ref ? 16 : 0), 2);
  Append(out, e.expression_id, 4); Append(out, static_cast<unsigned>(e.expression_kind), 1);
  Append(out, e.literal_kind ? static_cast<unsigned>(*e.literal_kind) : 0, 1); Append(out, 0, 2);
  Append(out, e.result_descriptor_id, 4); Append(out, e.child_expression_ids.size(), 4);
  Append(out, e.operator_name ? e.operator_name->size() : 0, 4);
  Append(out, e.literal_or_parameter_ref ? e.literal_or_parameter_ref->size() : 0, 4);
  for (const auto uuid : {e.function_uuid.value_or(Uuid{}), e.bound_name_uuid.value_or(Uuid{})})
    out.insert(out.end(), uuid.bytes.begin(), uuid.bytes.end());
  for (auto child : e.child_expression_ids) Append(out, child, 4);
  if (e.operator_name) out.insert(out.end(), e.operator_name->begin(), e.operator_name->end());
  if (e.literal_or_parameter_ref) out.insert(out.end(), e.literal_or_parameter_ref->begin(), e.literal_or_parameter_ref->end());
  return out;
}
void RejectExpression(const Bytes& bytes) {
  auto output = ExpressionFixture(0); const auto before = ExpressionOracle(output);
  Require(!wire::DecodeRelationalExpressionV1(bytes.data(), bytes.size(), &output) &&
          ExpressionOracle(output) == before, "malformed expression accepted or changed destination");
}
void TestExpressionLayout() {
  namespace parser = scratchbird::parser::sbsql;
  for (unsigned flags = 0; flags < 32; ++flags) for (unsigned kind = 1; kind <= 7; ++kind) {
    const auto last_literal = flags & 4 ? 12u : 1u;
    for (unsigned literal = 1; literal <= last_literal; ++literal) {
      auto e = ExpressionFixture(flags);
      e.expression_kind = static_cast<api::RelationalExpressionKind>(kind);
      if (flags & 4) e.literal_kind = static_cast<api::RelationalLiteralKind>(literal);
      Bytes encoded{0xfa};
      Require(wire::EncodeRelationalExpressionV1(e, &encoded) && encoded == ExpressionOracle(e),
              "expression bytes differ from independent field-order oracle");
      Expression decoded;
      Require(wire::DecodeRelationalExpressionV1(encoded.data(), encoded.size(), &decoded) &&
              ExpressionOracle(decoded) == encoded && !decoded.literal_typed_value_v1 &&
              !decoded.parameter_typed_value_v1 && !decoded.contextual_text_literal_v2,
              "expression decoding changed identity or fabricated runtime authority");
      parser::BoundExpressionAstRecord bound;
      bound.expression_id = e.expression_id; bound.result_descriptor_id = e.result_descriptor_id;
      bound.expression_kind = static_cast<parser::NativeExpressionAstKind>(kind - 1);
      if (flags & 4) bound.literal_kind = static_cast<parser::NativeLiteralAstKind>(literal - 1);
      bound.child_expression_ids = e.child_expression_ids; bound.bound_function_uuid = e.function_uuid;
      bound.bound_name_uuid = e.bound_name_uuid; bound.canonical_operator_name = e.operator_name;
      bound.literal_or_parameter_ref = e.literal_or_parameter_ref;
      const auto operand = parser::MakeRelationalExpressionOperand(bound);
      Require(operand && operand->value.empty() && operand->canonical_value_kind == 214 &&
              operand->canonical_value_body == encoded && operand->name == "slot_16909060" &&
              parser::DecodeRelationalExpressionOperand(*operand, &decoded),
              "BoundAST expression lost exact binary identity or fields");
    }
  }
  auto e = ExpressionFixture(); const auto base = ExpressionOracle(e);
  for (std::size_t length = 0; length < base.size(); ++length)
    RejectExpression(Bytes(base.begin(), base.begin() + length));
  auto changed = base; changed.push_back(0); RejectExpression(changed);
  for (auto offset : {0u, 1u, 10u, 11u}) { changed = base; changed[offset] ^= 0x80; RejectExpression(changed); }
  for (unsigned bit = 5; bit < 16; ++bit) {
    changed = base; changed[2 + bit / 8] |= static_cast<std::uint8_t>(1u << (bit % 8)); RejectExpression(changed);
  }
  for (unsigned bit = 0; bit < 5; ++bit) {
    changed = base; changed[2] &= static_cast<std::uint8_t>(~(1u << bit)); RejectExpression(changed);
  }
  for (auto offset : {4u, 12u, 60u}) { changed = base; std::fill_n(changed.begin() + offset, 4, 0); RejectExpression(changed); }
  for (auto offset : {16u, 20u, 24u}) { changed = base; std::fill_n(changed.begin() + offset, 4, 0xff); RejectExpression(changed); }
  for (unsigned value = 0; value < 256; ++value) {
    if (value < 1 || value > 7) { changed = base; changed[8] = value; RejectExpression(changed); }
    if (value < 1 || value > 12) { changed = base; changed[9] = value; RejectExpression(changed); }
  }
  for (auto offset : {28u, 44u}) {
    changed = base; std::fill_n(changed.begin() + offset, 16, 0); RejectExpression(changed);
    for (unsigned version = 0; version < 16; ++version) if (version != 7) {
      changed = base; changed[offset + 6] = static_cast<std::uint8_t>(version << 4); RejectExpression(changed);
    }
    for (unsigned variant : {0u, 1u, 3u}) {
      changed = base; changed[offset + 8] = static_cast<std::uint8_t>(variant << 6); RejectExpression(changed);
    }
    changed = base; changed.erase(changed.begin() + offset, changed.begin() + offset + 16);
    const std::string text = "019d0000-0000-7000-8000-000000000001";
    changed.insert(changed.begin() + offset, text.begin(), text.end()); RejectExpression(changed);
    for (unsigned byte = 0; byte < 16; ++byte) {
      changed = base; changed[offset + byte] ^= 1;
      Expression decoded;
      Require(wire::DecodeRelationalExpressionV1(changed.data(), changed.size(), &decoded) &&
              ExpressionOracle(decoded) == changed, "binary UUID byte substitution was normalized or lost");
    }
  }
  const auto reject_input = [](const Expression& value) {
    Bytes output{0xfa};
    Require(!wire::EncodeRelationalExpressionV1(value, &output) && output == Bytes{0xfa},
            "invalid runtime expression encoded or changed destination");
  };
  for (const auto op : {std::string{}, std::string(257, 'x'), std::string("bad\0operator", 12), std::string("\xc0\x80", 2)}) {
    e = ExpressionFixture(); e.operator_name = op; reject_input(e); RejectExpression(ExpressionOracle(e));
  }
  e = ExpressionFixture(); e.literal_typed_value_v1.emplace(); reject_input(e);
  e = ExpressionFixture(); e.parameter_typed_value_v1.emplace(); reject_input(e);
  e = ExpressionFixture(); e.contextual_text_literal_v2.emplace(); reject_input(e);
  e = ExpressionFixture(16); e.child_expression_ids.clear(); e.literal_or_parameter_ref = std::string(65536 - 60, '\0');
  Bytes encoded; Expression decoded;
  Require(wire::EncodeRelationalExpressionV1(e, &encoded) && encoded.size() == 65536 &&
          wire::DecodeRelationalExpressionV1(encoded.data(), encoded.size(), &decoded) &&
          decoded.literal_or_parameter_ref == e.literal_or_parameter_ref, "maximum binary reference lost");
  e.literal_or_parameter_ref->push_back(0); reject_input(e); RejectExpression(ExpressionOracle(e));
  e.literal_or_parameter_ref = "";
  Require(wire::EncodeRelationalExpressionV1(e, &encoded) &&
          wire::DecodeRelationalExpressionV1(encoded.data(), encoded.size(), &decoded) &&
          decoded.literal_or_parameter_ref && decoded.literal_or_parameter_ref->empty(), "empty reference became absent");
  e.literal_or_parameter_ref.reset();
  Require(wire::EncodeRelationalExpressionV1(e, &encoded) &&
          wire::DecodeRelationalExpressionV1(encoded.data(), encoded.size(), &decoded) &&
          !decoded.literal_or_parameter_ref, "absent reference became present");
}
void TestExpressionPlacementAndFaults() {
  auto expression = ExpressionFixture(); expression.expression_id = 1;
  const auto encoded = ExpressionOracle(expression);
  wire::SblrOperand operand;
  operand.ordinal = 1; operand.type = "relational_expression_v2"; operand.name = "slot_1";
  operand.value_kind = wire::SblrValueKind::relational_expression; operand.value_body = encoded;
  auto envelope = wire::MakeSblrEnvelope("query.execute", "SBLR_QUERY_EXECUTE", "expression.codec.fixture");
  envelope.opcode_code = 4615; envelope.parser_package_uuid = Id(6); envelope.registry_snapshot_uuid = Id(7);
  envelope.result_shape = "query_execute_result"; envelope.diagnostic_shape = "diagnostic_vector";
  envelope.operands = {operand};
  const auto bytes = wire::EncodeSblrEnvelope(envelope);
  Require(!bytes.empty() && wire::DecodeSblrEnvelope(bytes).ok, "binary expression SBOP round trip");
  for (auto name : {"slot_01", "slot_2", "1", "slot_0"}) {
    auto wrong = envelope; wrong.operands[0].name = name;
    Require(!wire::ValidateSblrEnvelope(wrong).ok, "expression handle substitution admitted");
  }
  auto wrong = envelope; wrong.operation_id = "engine.op.package_begin"; wrong.opcode = "SBLR_PACKAGE_BEGIN"; wrong.opcode_code = 1;
  Require(!wire::ValidateSblrEnvelope(wrong).ok, "expression escaped query root");
  wrong = envelope; wrong.operands[0].value = "shadow";
  Require(!wire::ValidateSblrEnvelope(wrong).ok, "expression text shadow admitted");
  const auto wrapper_descriptor = Id(10);
  Bytes wrapped(wrapper_descriptor.bytes.begin(), wrapper_descriptor.bytes.end());
  Append(wrapped, encoded.size(), 8); wrapped.insert(wrapped.end(), encoded.begin(), encoded.end());
  for (auto type : {"relational_expression_v1", "relational_expression_v2"}) {
    wrong = envelope; wrong.operands[0].type = type; wrong.operands[0].value_kind = wire::SblrValueKind::literal_typed;
    wrong.operands[0].value_body = wrapped;
    Require(!wire::ValidateSblrEnvelope(wrong).ok, "expression typed-text wrapper admitted");
  }
  Bytes nested; Append(nested, 1, 4); Append(nested, 214, 2); Append(nested, 0, 2); Append(nested, encoded.size(), 8);
  nested.insert(nested.end(), encoded.begin(), encoded.end());
  wrong = envelope; wrong.operands[0].type = "list"; wrong.operands[0].name = "nested";
  wrong.operands[0].value_kind = wire::SblrValueKind::list; wrong.operands[0].value_body = nested;
  Require(!wire::ValidateSblrEnvelope(wrong).ok, "expression nested outside registered slot");
  Bytes output;
  allocations = 0; Require(wire::EncodeRelationalExpressionV1(expression, &output), "expression encode allocation baseline");
  const auto encode_sites = allocations;
  for (std::size_t site = 0; site < encode_sites; ++site) {
    output = {0xaa}; bool threw = false; fail_after = site;
    try { (void)wire::EncodeRelationalExpressionV1(expression, &output); } catch (const std::bad_alloc&) { threw = true; }
    fail_after = -1; Require(threw && output == Bytes{0xaa}, "expression encode failure changed output"); ++faults;
  }
  Expression decoded;
  allocations = 0; Require(wire::DecodeRelationalExpressionV1(encoded.data(), encoded.size(), &decoded), "expression decode allocation baseline");
  const auto decode_sites = allocations;
  for (std::size_t site = 0; site < decode_sites; ++site) {
    decoded = ExpressionFixture(0); const auto before = ExpressionOracle(decoded); bool threw = false; fail_after = site;
    try { (void)wire::DecodeRelationalExpressionV1(encoded.data(), encoded.size(), &decoded); } catch (const std::bad_alloc&) { threw = true; }
    fail_after = -1; Require(threw && ExpressionOracle(decoded) == before, "expression decode failure changed output"); ++faults;
  }
  Require(encode_sites > 0 && decode_sites >= 3, "expression allocation campaign empty");
}

void TestExpressionReservationFreeze() {
  namespace parser = scratchbird::parser::sbsql;
  const auto expression = ExpressionFixture();
  parser::SblrOperand operand;
  operand.type = "relational_expression_v2";
  operand.name = "slot_" + std::to_string(expression.expression_id);
  operand.canonical_value_kind = 214;
  operand.canonical_value_body = ExpressionOracle(expression);
  const auto frozen = parser::FreezeContextualOperandsV3({operand}, {});
  Bytes expected;
  Append(expected, 3, 2); Append(expected, 1, 4); Append(expected, 0, 1);
  for (const auto& text : {operand.type, operand.name, operand.value}) {
    Append(expected, text.size(), 4);
    expected.insert(expected.end(), text.begin(), text.end());
  }
  Append(expected, 214, 2); Append(expected, operand.canonical_value_body.size(), 4);
  expected.insert(expected.end(), operand.canonical_value_body.begin(), operand.canonical_value_body.end());
  Require(frozen && *frozen == expected, "reservation lost exact binary expression bytes");
  for (std::size_t byte = 0; byte < operand.canonical_value_body.size(); ++byte) {
    auto changed = operand; changed.canonical_value_body[byte] ^= 1;
    const auto observed = parser::FreezeContextualOperandsV3({changed}, {});
    Require(!observed || observed != frozen, "reservation ignored expression byte change");
  }
  const auto refuse = [](const parser::SblrOperand& candidate) {
    Require(!parser::FreezeContextualOperandsV3({candidate}, {}),
            "reservation accepted malformed expression carrier");
  };
  for (auto type : {"relational_expression_v1", "literal", "relational_descriptor_v3"}) {
    auto changed = operand; changed.type = type; refuse(changed);
  }
  for (auto name : {"slot_016909060", "slot_2", "16909060", "slot_0"}) {
    auto changed = operand; changed.name = name; refuse(changed);
  }
  for (auto kind : {0u, 1u, 213u}) {
    auto changed = operand; changed.canonical_value_kind = kind; refuse(changed);
  }
  auto changed = operand; changed.value = "shadow"; refuse(changed);
  changed = operand; changed.canonical_value_body.pop_back(); refuse(changed);
  changed = operand; changed.canonical_value_body.push_back(0); refuse(changed);
  parser::SblrOperand legacy;
  legacy.type = "relational_expression_v1"; legacy.name = "1";
  legacy.value = "1|-|9|-|-|2|-|78"; refuse(legacy);
  Require(!parser::FreezeContextualOperandsV3({operand, operand}, {}),
          "reservation accepted duplicate expression handle");

  // Literal contents and bound UUIDs are immutable even when their descriptor
  // is eligible for contextual replacement. Only the descriptor slot is elided.
  auto descriptor = Fixture(1); descriptor.descriptor_id = expression.result_descriptor_id;
  parser::SblrOperand descriptor_operand;
  descriptor_operand.type = "relational_descriptor_v3";
  descriptor_operand.name = "slot_" + std::to_string(descriptor.descriptor_id);
  descriptor_operand.canonical_value_kind = 213;
  Require(wire::EncodeRelationalTypeDescriptorV1(descriptor, &descriptor_operand.canonical_value_body),
          "reservation descriptor setup");
  const std::unordered_set<std::uint32_t> mutable_handles{descriptor.descriptor_id};
  const std::vector<parser::SblrOperand> input{operand, descriptor_operand};
  const auto mutable_frozen = parser::FreezeContextualOperandsV3(input, mutable_handles);
  Require(mutable_frozen.has_value(), "valid expression plus mutable descriptor refused");
  changed = operand; changed.canonical_value_body[28] ^= 1;
  Require(parser::FreezeContextualOperandsV3({changed, descriptor_operand}, mutable_handles) != mutable_frozen,
          "mutable descriptor elided expression identity");
  allocations = 0;
  Require(parser::FreezeContextualOperandsV3(input, mutable_handles) == mutable_frozen,
          "expression reservation allocation baseline");
  const auto sites = allocations;
  for (std::size_t site = 0; site < sites; ++site) {
    bool threw = false; fail_after = site;
    try { (void)parser::FreezeContextualOperandsV3(input, mutable_handles); }
    catch (const std::bad_alloc&) { threw = true; }
    fail_after = -1;
    Require(threw && input.front().canonical_value_body == operand.canonical_value_body &&
            parser::FreezeContextualOperandsV3(input, mutable_handles) == mutable_frozen,
            "expression reservation allocation failure published partial or changed input");
    ++faults;
  }
  Require(sites > 0, "expression reservation allocation campaign empty");
}

Bytes NodeBindingOracle(const wire::RelationalNodeBindingRecord& binding) {
  Bytes out;
  Append(out, 1, 2); Append(out, 0, 2); Append(out, binding.node_id, 4);
  Append(out, binding.semantic_variant_id.size(), 4); Append(out, binding.bound_expression_ids.size(), 4);
  Append(out, binding.required_object_uuids.size(), 4); Append(out, binding.required_property_uuids.size(), 4);
  Append(out, binding.delivered_property_uuids.size(), 4); Append(out, 0, 4);
  out.insert(out.end(), binding.semantic_variant_id.begin(), binding.semantic_variant_id.end());
  for (const auto id : binding.bound_expression_ids) Append(out, id, 4);
  for (const auto* values : {&binding.required_object_uuids, &binding.required_property_uuids,
                              &binding.delivered_property_uuids})
    for (const auto& id : *values) out.insert(out.end(), id.bytes.begin(), id.bytes.end());
  return out;
}

void TestNodeBinding() {
  namespace parser = scratchbird::parser::sbsql;
  wire::RelationalNodeBindingRecord binding{19, "SBLR_MODEL_SOURCE_V1", {1, 2, 1},
      {Id(10), Id(11)}, {Id(12), Id(13)}, {Id(13), Id(12)}};
  for (unsigned flags = 0; flags < 16; ++flags) {
    auto candidate = binding;
    if (!(flags & 1)) candidate.bound_expression_ids.clear();
    if (!(flags & 2)) candidate.required_object_uuids.clear();
    if (!(flags & 4)) candidate.required_property_uuids.clear();
    if (!(flags & 8)) candidate.delivered_property_uuids.clear();
    Bytes encoded{0xf0}; wire::RelationalNodeBindingRecord decoded;
    Require(wire::EncodeRelationalNodeBindingV1(candidate, &encoded) && encoded == NodeBindingOracle(candidate),
            "node binding differs from independent layout oracle");
    Require(wire::DecodeRelationalNodeBindingV1(encoded.data(), encoded.size(), &decoded) &&
            NodeBindingOracle(decoded) == encoded, "node binding decode lost ordering or identity roles");
    const auto operand = parser::MakeRelationalNodeBindingOperand(candidate);
    Require(operand && operand->canonical_value_body == encoded && operand->canonical_value_kind == 215 &&
            operand->name == "slot_19" && operand->value.empty() &&
            parser::DecodeRelationalNodeBindingOperand(*operand, &decoded), "parser node binding differs");
  }
  const auto bytes = NodeBindingOracle(binding);
  const auto reject = [&](const Bytes& malformed) {
    auto output = binding; output.node_id = 99; output.semantic_variant_id = "previous-owner";
    output.bound_expression_ids = {9}; output.required_object_uuids.clear();
    const auto before = NodeBindingOracle(output);
    Require(!wire::DecodeRelationalNodeBindingV1(malformed.data(), malformed.size(), &output) &&
            NodeBindingOracle(output) == before, "invalid node binding accepted or changed destination");
  };
  for (std::size_t length = 0; length < bytes.size(); ++length)
    reject(Bytes(bytes.begin(), bytes.begin() + length));
  auto changed = bytes; changed.push_back(0); reject(changed);
  for (unsigned offset : {0u, 1u, 2u, 3u, 28u, 29u, 30u, 31u}) {
    changed = bytes; changed[offset] ^= 0x80; reject(changed);
  }
  changed = bytes; std::fill_n(changed.begin() + 4, 4, 0); reject(changed);
  for (unsigned offset : {8u, 12u, 16u, 20u, 24u}) {
    changed = bytes; std::fill_n(changed.begin() + offset, 4, 0xff); reject(changed);
  }
  changed = bytes; changed[32] = 0; reject(changed);
  const auto handle_offset = 32 + binding.semantic_variant_id.size();
  changed = bytes; std::fill_n(changed.begin() + handle_offset, 4, 0); reject(changed);
  const auto identity_offset = handle_offset + 4 * binding.bound_expression_ids.size();
  for (std::size_t offset = identity_offset; offset < bytes.size(); offset += 16) {
    changed = bytes; std::fill_n(changed.begin() + offset, 16, 0); reject(changed);
    for (unsigned version = 0; version < 16; ++version) if (version != 7) {
      changed = bytes; changed[offset + 6] = version << 4; reject(changed);
    }
    for (unsigned variant : {0u, 1u, 3u}) {
      changed = bytes; changed[offset + 8] = variant << 6; reject(changed);
    }
    changed = bytes; changed.erase(changed.begin() + offset, changed.begin() + offset + 16);
    const std::string text = "019d0000-0000-7000-8000-000000000001";
    changed.insert(changed.begin() + offset, text.begin(), text.end()); reject(changed);
  }
  const auto operand = parser::MakeRelationalNodeBindingOperand(binding);
  const auto frozen = parser::FreezeContextualOperandsV3({*operand}, {});
  Require(frozen && !parser::FreezeContextualOperandsV3({*operand, *operand}, {}),
          "node binding reservation refused valid or accepted duplicate handle");
  for (std::size_t byte = 0; byte < bytes.size(); ++byte) {
    auto mutated = *operand; mutated.canonical_value_body[byte] ^= 1;
    const auto result = parser::FreezeContextualOperandsV3({mutated}, {});
    Require(!result || result != frozen, "reservation ignored node binding byte");
  }
  auto envelope = wire::MakeSblrEnvelope("query.execute", "SBLR_QUERY_EXECUTE", "node.binding.fixture");
  envelope.opcode_code = 4615; envelope.parser_package_uuid = Id(6); envelope.registry_snapshot_uuid = Id(7);
  envelope.result_shape = "query_execute_result"; envelope.diagnostic_shape = "diagnostic_vector";
  wire::SblrOperand canonical;
  canonical.ordinal = 1; canonical.type = operand->type; canonical.name = operand->name;
  canonical.value_kind = wire::SblrValueKind::relational_node_binding; canonical.value_body = bytes;
  envelope.operands = {canonical};
  Require(wire::DecodeSblrEnvelope(wire::EncodeSblrEnvelope(envelope)).ok, "binary node binding SBOP round trip");
  for (auto name : {"slot_019", "slot_20", "19", "slot_0"}) {
    auto wrong = envelope; wrong.operands[0].name = name;
    Require(!wire::ValidateSblrEnvelope(wrong).ok, "node binding handle substitution admitted");
  }
  auto wrong = envelope; wrong.operands[0].value = "shadow";
  Require(!wire::ValidateSblrEnvelope(wrong).ok, "node binding text shadow admitted");
  wrong = envelope; wrong.operation_id = "engine.op.package_begin";
  wrong.opcode = "SBLR_PACKAGE_BEGIN"; wrong.opcode_code = 1;
  Require(!wire::ValidateSblrEnvelope(wrong).ok, "node binding escaped query root");
  wrong = envelope; wrong.operands[0].type = "relational_node_binding_v1";
  Require(!wire::ValidateSblrEnvelope(wrong).ok, "legacy node binding admitted");
  Bytes nested; Append(nested, 1, 4); Append(nested, 215, 2); Append(nested, 0, 2); Append(nested, bytes.size(), 8);
  nested.insert(nested.end(), bytes.begin(), bytes.end());
  wrong = envelope; wrong.operands[0].type = "list"; wrong.operands[0].name = "nested";
  wrong.operands[0].value_kind = wire::SblrValueKind::list; wrong.operands[0].value_body = nested;
  Require(!wire::ValidateSblrEnvelope(wrong).ok, "nested node binding admitted");

  auto invalid = binding; invalid.semantic_variant_id = std::string(257, 'a');
  Bytes output{0xf0};
  Require(!wire::EncodeRelationalNodeBindingV1(invalid, &output) && output == Bytes{0xf0},
          "oversized variant encoded");
  reject(NodeBindingOracle(invalid));
  auto maximum = binding; maximum.semantic_variant_id = "abcd";
  maximum.bound_expression_ids.assign((65536 - 36) / 4, 1);
  maximum.required_object_uuids.clear(); maximum.required_property_uuids.clear(); maximum.delivered_property_uuids.clear();
  Require(wire::EncodeRelationalNodeBindingV1(maximum, &output) && output.size() == 65536,
          "maximum node binding refused");
  maximum.bound_expression_ids.push_back(1);
  Require(!wire::EncodeRelationalNodeBindingV1(maximum, &output) && output.size() == 65536,
          "oversized node binding encoded or changed output");
  reject(NodeBindingOracle(maximum));
  allocations = 0; Require(wire::EncodeRelationalNodeBindingV1(binding, &output), "node binding encode baseline");
  const auto encode_sites = allocations;
  for (std::size_t site = 0; site < encode_sites; ++site) {
    output = {0xfa}; bool threw = false; fail_after = site;
    try { (void)wire::EncodeRelationalNodeBindingV1(binding, &output); } catch (const std::bad_alloc&) { threw = true; }
    fail_after = -1; Require(threw && output == Bytes{0xfa}, "node binding encode fault changed output"); ++faults;
  }
  wire::RelationalNodeBindingRecord decoded;
  allocations = 0; Require(wire::DecodeRelationalNodeBindingV1(bytes.data(), bytes.size(), &decoded), "node binding decode baseline");
  const auto decode_sites = allocations;
  for (std::size_t site = 0; site < decode_sites; ++site) {
    decoded = binding; decoded.node_id = 99; decoded.semantic_variant_id = "previous-owner";
    decoded.bound_expression_ids = {9}; decoded.required_object_uuids.clear();
    const auto before = NodeBindingOracle(decoded);
    bool threw = false; fail_after = site;
    try { (void)wire::DecodeRelationalNodeBindingV1(bytes.data(), bytes.size(), &decoded); } catch (const std::bad_alloc&) { threw = true; }
    fail_after = -1; Require(threw && NodeBindingOracle(decoded) == before, "node binding decode fault changed output"); ++faults;
  }
  Require(encode_sites > 0 && decode_sites >= 5, "node binding allocation campaign empty");
}

Bytes WindowInvocationOracle(const api::RelationalWindowInvocationRecord& value) {
  Bytes out;
  Append(out, 1, 2); Append(out, 0, 2); Append(out, value.invocation_id, 4);
  Append(out, value.relation_node_id, 4); Append(out, value.function_expression_id, 4);
  Append(out, value.window_definition_id, 4); Append(out, value.result_descriptor_id, 4);
  Append(out, value.function_abi_version, 2); Append(out, 0, 2);
  Append(out, value.builtin_id.size(), 4); Append(out, value.output_name_utf8.size(), 4);
  Append(out, value.argument_expression_ids.size(), 4);
  out.insert(out.end(), value.function_uuid.bytes.begin(), value.function_uuid.bytes.end());
  out.insert(out.end(), value.builtin_id.begin(), value.builtin_id.end());
  out.insert(out.end(), value.output_name_utf8.begin(), value.output_name_utf8.end());
  for (const auto argument : value.argument_expression_ids) Append(out, argument, 4);
  return out;
}

void TestWindowInvocation() {
  namespace parser = scratchbird::parser::sbsql;
  api::RelationalWindowInvocationRecord value;
  value.invocation_id = 0x01020304; value.relation_node_id = 2;
  value.function_expression_id = 3; value.window_definition_id = 4;
  value.result_descriptor_id = 5; value.function_abi_version = 0x1234;
  value.builtin_id = "sb.window.test_binary_identity"; value.function_uuid = Id(40);
  value.output_name_utf8 = std::string("original\0output|label", 21) + "\xc3\xa9";
  value.argument_expression_ids = {6, 7, 6};
  const auto original = WindowInvocationOracle(value);
  Bytes encoded{0xfa}; api::RelationalWindowInvocationRecord decoded;
  Require(wire::EncodeRelationalWindowInvocationV1(value, &encoded) && encoded == original,
          "window invocation differs from independent byte oracle");
  Require(wire::DecodeRelationalWindowInvocationV1(encoded.data(), encoded.size(), &decoded) &&
          WindowInvocationOracle(decoded) == original, "window invocation lost exact fields");
  const auto reject = [&](const Bytes& bad) {
    auto output = value; output.invocation_id = 99; output.output_name_utf8 = "old-output";
    output.argument_expression_ids = {99}; const auto before = WindowInvocationOracle(output);
    Require(!wire::DecodeRelationalWindowInvocationV1(bad.data(), bad.size(), &output) &&
            WindowInvocationOracle(output) == before, "invalid window invocation accepted or changed output");
  };
  for (std::size_t length = 0; length < original.size(); ++length)
    reject(Bytes(original.begin(), original.begin() + length));
  auto changed = original; changed.push_back(0); reject(changed);
  for (unsigned offset : {0u, 1u, 2u, 3u, 26u, 27u}) {
    changed = original; changed[offset] ^= 0x80; reject(changed);
  }
  for (unsigned offset : {4u, 8u, 12u, 16u, 20u}) {
    changed = original; std::fill_n(changed.begin() + offset, 4, 0); reject(changed);
  }
  changed = original; changed[24] = changed[25] = 0; reject(changed);
  for (unsigned offset : {28u, 32u, 36u}) {
    changed = original; std::fill_n(changed.begin() + offset, 4, 0xff); reject(changed);
  }
  changed = original; std::fill_n(changed.begin() + 40, 16, 0); reject(changed);
  for (unsigned version = 0; version < 16; ++version) if (version != 7) {
    changed = original; changed[46] = version << 4; reject(changed);
  }
  for (unsigned variant : {0u, 1u, 3u}) {
    changed = original; changed[48] = variant << 6; reject(changed);
  }
  changed = original; changed.erase(changed.begin() + 40, changed.begin() + 56);
  const std::string uuid_text = "019d0000-0000-7000-8000-000000000001";
  changed.insert(changed.begin() + 40, uuid_text.begin(), uuid_text.end()); reject(changed);
  changed = original; std::fill_n(changed.end() - 4, 4, 0); reject(changed);
  for (bool builtin : {false, true}) for (const auto text : {std::string{}, std::string("\xc0\x80", 2)}) {
    auto bad = value;
    (builtin ? bad.builtin_id : bad.output_name_utf8) = text;
    encoded = {0xfa};
    Require(!wire::EncodeRelationalWindowInvocationV1(bad, &encoded) && encoded == Bytes{0xfa},
            "invalid window text encoded or changed destination");
    reject(WindowInvocationOracle(bad));
  }
  auto bad = value; bad.builtin_id = std::string(257, 'a');
  Require(!wire::EncodeRelationalWindowInvocationV1(bad, &encoded), "oversize builtin admitted");
  reject(WindowInvocationOracle(bad));
  auto maximum = value; maximum.argument_expression_ids.clear();
  maximum.output_name_utf8.assign(65536 - 56 - maximum.builtin_id.size(), 'x');
  Require(wire::EncodeRelationalWindowInvocationV1(maximum, &encoded) && encoded.size() == 65536 &&
          wire::DecodeRelationalWindowInvocationV1(encoded.data(), encoded.size(), &decoded) &&
          WindowInvocationOracle(decoded) == encoded, "maximum window invocation lost");
  maximum.output_name_utf8.push_back('x');
  Require(!wire::EncodeRelationalWindowInvocationV1(maximum, &encoded) && encoded.size() == 65536,
          "oversize window invocation admitted or changed destination");
  reject(WindowInvocationOracle(maximum));
  parser::BoundWindowInvocationAstRecord bound;
  bound.invocation_id = value.invocation_id; bound.function_expression_id = value.function_expression_id;
  bound.window_definition_id = value.window_definition_id; bound.function_abi_version = value.function_abi_version;
  bound.builtin_id = value.builtin_id; bound.bound_function_uuid = value.function_uuid;
  bound.result_descriptor_id = value.result_descriptor_id; bound.output_name_utf8 = value.output_name_utf8;
  bound.argument_expression_ids = value.argument_expression_ids;
  const auto operand = parser::MakeRelationalWindowInvocationOperand(bound, value.relation_node_id);
  Require(operand && operand->value.empty() && operand->name == "slot_16909060" &&
          operand->canonical_value_kind == 216 && operand->canonical_value_body == original &&
          parser::DecodeRelationalWindowInvocationOperand(*operand, &decoded), "BoundAST window fields changed");
  const auto frozen = parser::FreezeContextualOperandsV3({*operand}, {});
  Require(frozen && !parser::FreezeContextualOperandsV3({*operand, *operand}, {}),
          "window reservation refused valid or accepted duplicate invocation");
  for (std::size_t byte = 0; byte < original.size(); ++byte) {
    auto altered = *operand; altered.canonical_value_body[byte] ^= 1;
    const auto observed = parser::FreezeContextualOperandsV3({altered}, {});
    Require(!observed || observed != frozen, "window reservation ignored changed byte");
  }
  auto envelope = wire::MakeSblrEnvelope("query.execute", "SBLR_QUERY_EXECUTE", "window.invocation.fixture");
  envelope.opcode_code = 4615; envelope.parser_package_uuid = Id(6); envelope.registry_snapshot_uuid = Id(7);
  envelope.result_shape = "query_execute_result"; envelope.diagnostic_shape = "diagnostic_vector";
  wire::SblrOperand canonical;
  canonical.ordinal = 1; canonical.type = operand->type; canonical.name = operand->name;
  canonical.value_kind = wire::SblrValueKind::relational_window_invocation; canonical.value_body = original;
  envelope.operands = {canonical};
  Require(wire::DecodeSblrEnvelope(wire::EncodeSblrEnvelope(envelope)).ok, "window invocation SBOP round trip");
  for (auto name : {"slot_016909060", "slot_2", "16909060", "slot_0"}) {
    auto wrong = envelope; wrong.operands[0].name = name;
    Require(!wire::ValidateSblrEnvelope(wrong).ok, "window invocation slot substitution accepted");
  }
  auto wrong = envelope; wrong.operands[0].type = "relational_window_invocation_v1";
  Require(!wire::ValidateSblrEnvelope(wrong).ok, "legacy window invocation admitted");
  wrong = envelope; wrong.operands[0].value = "shadow";
  Require(!wire::ValidateSblrEnvelope(wrong).ok, "window text shadow admitted");
  wrong = envelope; wrong.operation_id = "engine.op.package_begin"; wrong.opcode = "SBLR_PACKAGE_BEGIN"; wrong.opcode_code = 1;
  Require(!wire::ValidateSblrEnvelope(wrong).ok, "window invocation escaped query root");
  Bytes nested; Append(nested, 1, 4); Append(nested, 216, 2); Append(nested, 0, 2); Append(nested, original.size(), 8);
  nested.insert(nested.end(), original.begin(), original.end());
  wrong = envelope; wrong.operands[0].type = "list"; wrong.operands[0].name = "nested";
  wrong.operands[0].value_kind = wire::SblrValueKind::list; wrong.operands[0].value_body = nested;
  Require(!wire::ValidateSblrEnvelope(wrong).ok, "nested window invocation admitted");
  allocations = 0; Require(wire::EncodeRelationalWindowInvocationV1(value, &encoded), "window encode fault baseline");
  const auto encode_sites = allocations;
  for (std::size_t site = 0; site < encode_sites; ++site) {
    encoded = {0xfa}; bool threw = false; fail_after = site;
    try { (void)wire::EncodeRelationalWindowInvocationV1(value, &encoded); } catch (const std::bad_alloc&) { threw = true; }
    fail_after = -1; Require(threw && encoded == Bytes{0xfa}, "window encode fault changed output"); ++faults;
  }
  allocations = 0; Require(wire::DecodeRelationalWindowInvocationV1(original.data(), original.size(), &decoded), "window decode fault baseline");
  const auto decode_sites = allocations;
  for (std::size_t site = 0; site < decode_sites; ++site) {
    decoded = value; decoded.invocation_id = 99; decoded.output_name_utf8 = "old-output";
    const auto before = WindowInvocationOracle(decoded); bool threw = false; fail_after = site;
    try { (void)wire::DecodeRelationalWindowInvocationV1(original.data(), original.size(), &decoded); } catch (const std::bad_alloc&) { threw = true; }
    fail_after = -1; Require(threw && WindowInvocationOracle(decoded) == before, "window decode fault changed output"); ++faults;
  }
  Require(encode_sites > 0 && decode_sites >= 3, "window allocation campaign empty");
}

Bytes RowPatternOracle(const api::RelationalRowPatternRecord& value) {
  Bytes out;
  Append(out, 1, 2); Append(out, (value.stable_row_identity_tie_break_allowed ? 1 : 0) | (value.skip_target_key ? 2 : 0), 2);
  Append(out, value.pattern_id, 4); Append(out, value.relation_node_id, 4);
  Append(out, value.partition_expression_ids.size(), 4); Append(out, value.ordering_terms.size(), 4);
  Append(out, value.variables.size(), 4); Append(out, value.measure_expression_ids.size(), 4);
  Append(out, value.skip_target_key ? value.skip_target_key->size() : 0, 4);
  Append(out, value.maximum_partition_rows, 4); Append(out, value.maximum_active_states, 4);
  Append(out, value.maximum_output_rows, 4); Append(out, static_cast<unsigned>(value.rows_per_match), 1);
  Append(out, static_cast<unsigned>(value.after_match_skip), 1); Append(out, 0, 2);
  Require(out.size() == 48, "row-pattern prefix differs from Core layout");
  if (value.skip_target_key) out.insert(out.end(), value.skip_target_key->begin(), value.skip_target_key->end());
  for (const auto id : value.partition_expression_ids) Append(out, id, 4);
  for (const auto& term : value.ordering_terms) {
    Append(out, term.expression_id, 4); Append(out, static_cast<unsigned>(term.direction), 1);
    Append(out, static_cast<unsigned>(term.null_placement), 1); Append(out, term.collation_uuid.is_nil() ? 0 : 1, 2);
    out.insert(out.end(), term.collation_uuid.bytes.begin(), term.collation_uuid.bytes.end());
  }
  for (const auto& variable : value.variables) {
    Append(out, variable.canonical_name_key.size(), 4); Append(out, variable.minimum_occurrences, 4);
    Append(out, variable.maximum_occurrences.value_or(0), 4); Append(out, variable.define_expression_id.value_or(0), 4);
    Append(out, (variable.maximum_occurrences ? 1 : 0) | (variable.define_expression_id ? 2 : 0) |
        (variable.reluctant ? 4 : 0) | (variable.define_always_true ? 8 : 0), 1);
    Append(out, 0, 3); out.insert(out.end(), variable.canonical_name_key.begin(), variable.canonical_name_key.end());
  }
  for (const auto id : value.measure_expression_ids) Append(out, id, 4);
  return out;
}

void TestRowPattern() {
  namespace parser = scratchbird::parser::sbsql;
  api::RelationalRowPatternRecord value;
  value.pattern_id = 17; value.relation_node_id = 23;
  value.partition_expression_ids = {11, 13, 11}; value.measure_expression_ids = {31, 32, 31};
  value.ordering_terms = {{21, api::RelationalPropertySortDirection::kAscending, api::RelationalPropertyNullPlacement::kNullsLast, Id(51)},
                         {22, api::RelationalPropertySortDirection::kDescending, api::RelationalPropertyNullPlacement::kNullsFirst, {}}};
  value.skip_target_key = "variable_gamma_long_key";
  value.variables = {{"variable_alpha_long_key", 0, 0, false, 41, true},
                     {"variable_beta_long_key", UINT32_MAX, UINT32_MAX, true, 42, false},
                     {"variable_gamma_long_key", 1, std::nullopt, false, std::nullopt, true}};
  value.rows_per_match = api::RelationalRowPatternRowsPerMatch::kAll;
  value.after_match_skip = api::RelationalRowPatternAfterMatchSkip::kToLastVariable;
  value.maximum_partition_rows = 0x01020304; value.maximum_active_states = 0x05060708; value.maximum_output_rows = UINT32_MAX;
  value.stable_row_identity_tie_break_allowed = true;
  Bytes encoded;
  api::RelationalRowPatternRecord decoded;
  for (unsigned flags = 0; flags < 16; ++flags) for (unsigned header = 0; header < 4; ++header)
    for (unsigned rows = 1; rows <= 2; ++rows) for (unsigned skip = 1; skip <= 4; ++skip) {
      auto candidate = value;
      candidate.rows_per_match = static_cast<api::RelationalRowPatternRowsPerMatch>(rows);
      candidate.after_match_skip = static_cast<api::RelationalRowPatternAfterMatchSkip>(skip);
      candidate.stable_row_identity_tie_break_allowed = header & 1;
      if (!(header & 2)) candidate.skip_target_key.reset();
      for (auto& variable : candidate.variables) {
        variable.maximum_occurrences = flags & 1 ? std::optional<std::uint32_t>{0} : std::nullopt;
        variable.define_expression_id = flags & 2 ? std::optional<std::uint32_t>{47} : std::nullopt;
        variable.reluctant = flags & 4; variable.define_always_true = flags & 8;
      }
      Require(wire::EncodeRelationalRowPatternV1(candidate, &encoded) && encoded == RowPatternOracle(candidate),
              "row-pattern encoder changed variable flags or sequence");
      Require(wire::DecodeRelationalRowPatternV1(encoded.data(), encoded.size(), &decoded) &&
              RowPatternOracle(decoded) == encoded, "row-pattern decoder changed optional state");
    }
  const auto original = RowPatternOracle(value);
  const auto order_start = 48 + value.skip_target_key->size() + 4 * value.partition_expression_ids.size();
  const auto variable_start = order_start + 24 * value.ordering_terms.size();
  const auto reject = [&](const Bytes& bytes) {
    auto sentinel = value; sentinel.pattern_id = 99; sentinel.variables.pop_back();
    const auto before = RowPatternOracle(sentinel);
    Require(!wire::DecodeRelationalRowPatternV1(bytes.data(), bytes.size(), &sentinel) &&
            RowPatternOracle(sentinel) == before, "malformed row pattern published or accepted");
  };
  Require(!wire::DecodeRelationalRowPatternV1(nullptr, original.size(), &decoded) &&
          !wire::DecodeRelationalRowPatternV1(original.data(), original.size(), nullptr) &&
          !wire::EncodeRelationalRowPatternV1(value, nullptr), "row-pattern null pointer accepted");
  for (std::size_t size = 0; size < original.size(); ++size) reject(Bytes(original.begin(), original.begin() + size));
  auto bad = original; bad.push_back(0); reject(bad);
  for (const auto offset : {0u, 1u, 3u, 46u, 47u}) { bad = original; bad[offset] ^= 0x80; reject(bad); }
  for (unsigned bit = 1; bit < 8; ++bit) { bad = original; bad[2] ^= 1u << bit; reject(bad); }
  for (const auto offset : {4u, 8u}) { bad = original; std::fill_n(bad.begin() + offset, 4, 0); reject(bad); }
  for (const auto offset : {12u, 16u, 20u, 24u, 28u}) { bad = original; std::fill_n(bad.begin() + offset, 4, 0xff); reject(bad); }
  for (unsigned tag = 0; tag < 256; ++tag) {
    if (tag < 1 || tag > 2) { bad = original; bad[44] = tag; reject(bad); }
    if (tag < 1 || tag > 4) { bad = original; bad[45] = tag; reject(bad); }
  }
  bad = original; bad[48] = 0xff; reject(bad);
  bad = original; std::fill_n(bad.begin() + order_start, 4, 0); reject(bad);
  bad = original; std::fill_n(bad.begin() + original.size() - 4, 4, 0); reject(bad);
  bad = original; std::fill_n(bad.begin() + 48 + value.skip_target_key->size(), 4, 0); reject(bad);
  for (unsigned byte = 0; byte < 256; ++byte) {
    if ((byte & 0xf0) != 0x70) { bad = original; bad[order_start + 8 + 6] = byte; reject(bad); }
    if ((byte & 0xc0) != 0x80) { bad = original; bad[order_start + 8 + 8] = byte; reject(bad); }
  }
  for (const auto offset : {4u, 5u, 6u, 7u}) { bad = original; bad[order_start + offset] = 0xff; reject(bad); }
  bad = original; bad[order_start + 6] = 0; reject(bad);
  bad = original; bad[order_start + 24 + 6] = 1; reject(bad);
  bad = original; std::fill_n(bad.begin() + variable_start, 4, 0xff); reject(bad);
  bad = original; std::fill_n(bad.begin() + variable_start, 4, 0); reject(bad);
  for (const auto offset : {16u, 17u, 18u, 19u}) { bad = original; bad[variable_start + offset] |= 0x80; reject(bad); }
  bad = original; std::fill_n(bad.begin() + variable_start + 12, 4, 0); reject(bad);
  bad = original; bad[variable_start + 16] &= ~2u; reject(bad);
  bad = original; bad[variable_start + 16] &= ~1u; bad[variable_start + 8] = 1; reject(bad);
  bad = original; bad[variable_start + 20] = 0xff; reject(bad);
  // Even an invalid final variable is checked before decoded vectors allocate.
  const auto last_variable = variable_start + 40 + value.variables[0].canonical_name_key.size() + value.variables[1].canonical_name_key.size();
  bad = original; bad[last_variable + 20] = 0xff;
  bool rejected_without_allocation = false; fail_after = 0;
  try { rejected_without_allocation = !wire::DecodeRelationalRowPatternV1(bad.data(), bad.size(), &decoded); }
  catch (const std::bad_alloc&) {}
  fail_after = -1;
  Require(rejected_without_allocation, "row-pattern decoder allocated before complete validation");
  auto maximum = value; maximum.partition_expression_ids.clear(); maximum.ordering_terms.clear();
  maximum.variables.clear(); maximum.measure_expression_ids.clear(); maximum.skip_target_key = std::string(65536 - 48, 'x');
  Require(wire::EncodeRelationalRowPatternV1(maximum, &encoded) && encoded.size() == 65536 &&
          wire::DecodeRelationalRowPatternV1(encoded.data(), encoded.size(), &decoded) && RowPatternOracle(decoded) == encoded,
          "maximum row-pattern body lost");
  maximum.skip_target_key->push_back('x'); encoded = {0xfa};
  Require(!wire::EncodeRelationalRowPatternV1(maximum, &encoded) && encoded == Bytes{0xfa}, "oversized row pattern published");
  reject(RowPatternOracle(maximum));
  maximum.skip_target_key = "";
  Require(wire::EncodeRelationalRowPatternV1(maximum, &encoded) &&
          wire::DecodeRelationalRowPatternV1(encoded.data(), encoded.size(), &decoded) &&
          decoded.skip_target_key && decoded.skip_target_key->empty(), "present empty skip target became absent");
  maximum.skip_target_key.reset();
  Require(wire::EncodeRelationalRowPatternV1(maximum, &encoded) && encoded.size() == 48 &&
          wire::DecodeRelationalRowPatternV1(encoded.data(), encoded.size(), &decoded) && !decoded.skip_target_key,
          "absent skip target became present");
  maximum.variables = {{std::string(65536 - 68, 'a'), 0, std::nullopt, false, std::nullopt, false}};
  Require(wire::EncodeRelationalRowPatternV1(maximum, &encoded) && encoded.size() == 65536 &&
          wire::DecodeRelationalRowPatternV1(encoded.data(), encoded.size(), &decoded) && RowPatternOracle(decoded) == encoded,
          "maximum variable name lost");
  auto invalid = value; invalid.variables[1].define_expression_id = 0; encoded = {0xfa};
  Require(!wire::EncodeRelationalRowPatternV1(invalid, &encoded) && encoded == Bytes{0xfa}, "zero DEFINE handle published");
  invalid = value; invalid.variables[1].canonical_name_key.clear();
  Require(!wire::EncodeRelationalRowPatternV1(invalid, &encoded), "empty variable name published");
  parser::BoundRowPatternAstRecord bound;
  bound.pattern_id = value.pattern_id; bound.relation_id = value.relation_node_id;
  bound.partition_expression_ids = value.partition_expression_ids; bound.measure_expression_ids = value.measure_expression_ids;
  bound.rows_per_match = parser::NativeRowPatternRowsPerMatch::kAll;
  bound.after_match_skip = parser::NativeRowPatternAfterMatchSkip::kToLastVariable;
  bound.skip_target_key = value.skip_target_key; bound.maximum_partition_rows = value.maximum_partition_rows;
  bound.maximum_active_states = value.maximum_active_states; bound.maximum_output_rows = value.maximum_output_rows;
  bound.stable_row_identity_tie_break_allowed = value.stable_row_identity_tie_break_allowed;
  for (const auto& term : value.ordering_terms)
    bound.ordering_terms.push_back({term.expression_id, static_cast<parser::NativeSortDirection>(static_cast<unsigned>(term.direction) - 1),
                                  static_cast<parser::NativeNullPlacement>(static_cast<unsigned>(term.null_placement) - 1)});
  for (const auto& v : value.variables) bound.variables.push_back({v.canonical_name_key, v.minimum_occurrences, v.maximum_occurrences,
                                                                v.reluctant, v.define_expression_id, v.define_always_true});
  const auto operand = parser::MakeRelationalRowPatternOperand(bound, value.ordering_terms);
  Require(operand && operand->canonical_value_body == original && operand->canonical_value_kind == 219 && operand->name == "slot_17" &&
          parser::DecodeRelationalRowPatternOperand(*operand, &decoded) && RowPatternOracle(decoded) == original,
          "row-pattern BoundAST adapter changed fields or truncated variables");
  for (const auto rows : {parser::NativeRowPatternRowsPerMatch::kOne, parser::NativeRowPatternRowsPerMatch::kAll}) {
    for (const auto skip : {parser::NativeRowPatternAfterMatchSkip::kPastLastRow, parser::NativeRowPatternAfterMatchSkip::kToNextRow,
                            parser::NativeRowPatternAfterMatchSkip::kToFirstVariable, parser::NativeRowPatternAfterMatchSkip::kToLastVariable}) {
      auto varied = bound; varied.rows_per_match = rows; varied.after_match_skip = skip;
      auto expected = value; expected.rows_per_match = static_cast<api::RelationalRowPatternRowsPerMatch>(rows);
      expected.after_match_skip = static_cast<api::RelationalRowPatternAfterMatchSkip>(skip);
      const auto adapted = parser::MakeRelationalRowPatternOperand(varied, value.ordering_terms);
      Require(adapted && adapted->canonical_value_body == RowPatternOracle(expected), "row-pattern adapter enum mapping differs");
    }
  }
  auto crossed = value.ordering_terms; std::swap(crossed[0], crossed[1]);
  Require(!parser::MakeRelationalRowPatternOperand(bound, crossed), "row-pattern reordered bound ordering admitted");
  crossed = value.ordering_terms; crossed.pop_back();
  Require(!parser::MakeRelationalRowPatternOperand(bound, crossed), "row-pattern missing bound ordering admitted");
  const auto frozen = parser::FreezeContextualOperandsV3({*operand}, {});
  Require(frozen && !parser::FreezeContextualOperandsV3({*operand, *operand}, {}), "row-pattern duplicate identity frozen");
  for (std::size_t byte = 0; byte < original.size(); ++byte) {
    auto altered = *operand; altered.canonical_value_body[byte] ^= 1;
    const auto observed = parser::FreezeContextualOperandsV3({altered}, {});
    Require(!observed || observed != frozen, "row-pattern freeze ignored changed field");
  }
  auto envelope = wire::MakeSblrEnvelope("query.execute", "SBLR_QUERY_EXECUTE", "row.pattern.fixture");
  envelope.opcode_code = 4615; envelope.parser_package_uuid = Id(6); envelope.registry_snapshot_uuid = Id(7);
  envelope.result_shape = "query_execute_result"; envelope.diagnostic_shape = "diagnostic_vector";
  wire::SblrOperand canonical; canonical.ordinal = 1; canonical.type = operand->type; canonical.name = operand->name;
  canonical.value_kind = wire::SblrValueKind::relational_row_pattern; canonical.value_body = original; envelope.operands = {canonical};
  Require(wire::DecodeSblrEnvelope(wire::EncodeSblrEnvelope(envelope)).ok, "row-pattern SBOP round trip failed");
  for (const auto* name : {"17", "slot_017", "slot_23", "slot_0"}) {
    auto wrong = envelope; wrong.operands[0].name = name;
    Require(!wire::ValidateSblrEnvelope(wrong).ok, "row-pattern pattern/node slot substitution admitted");
  }
  auto wrong = envelope; wrong.operands[0].type = "relational_row_pattern_v1";
  Require(!wire::ValidateSblrEnvelope(wrong).ok, "legacy row-pattern admitted");
  wrong = envelope; wrong.operands[0].value = "shadow";
  Require(!wire::ValidateSblrEnvelope(wrong).ok, "row-pattern text shadow admitted");
  wrong = envelope; wrong.operation_id = "engine.op.package_begin"; wrong.opcode = "SBLR_PACKAGE_BEGIN"; wrong.opcode_code = 1;
  Require(!wire::ValidateSblrEnvelope(wrong).ok, "row-pattern escaped query root");
  Bytes nested; Append(nested, 1, 4); Append(nested, 219, 2); Append(nested, 0, 2); Append(nested, original.size(), 8);
  nested.insert(nested.end(), original.begin(), original.end());
  wrong = envelope; wrong.operands[0].type = "list"; wrong.operands[0].name = "nested";
  wrong.operands[0].value_kind = wire::SblrValueKind::list; wrong.operands[0].value_body = nested;
  Require(!wire::ValidateSblrEnvelope(wrong).ok, "nested row pattern admitted");
  allocations = 0; Require(wire::EncodeRelationalRowPatternV1(value, &encoded), "row-pattern encode fault baseline");
  const auto encode_sites = allocations;
  for (std::size_t site = 0; site < encode_sites; ++site) {
    encoded = {0xfa}; bool threw = false; fail_after = site;
    try { (void)wire::EncodeRelationalRowPatternV1(value, &encoded); } catch (const std::bad_alloc&) { threw = true; }
    fail_after = -1; Require(threw && encoded == Bytes{0xfa}, "row-pattern encode fault published"); ++faults;
  }
  allocations = 0; Require(wire::DecodeRelationalRowPatternV1(original.data(), original.size(), &decoded), "row-pattern decode fault baseline");
  const auto decode_sites = allocations;
  for (std::size_t site = 0; site < decode_sites; ++site) {
    decoded = value; decoded.pattern_id = 99; decoded.variables.pop_back(); const auto before = RowPatternOracle(decoded);
    bool threw = false; fail_after = site;
    try { (void)wire::DecodeRelationalRowPatternV1(original.data(), original.size(), &decoded); } catch (const std::bad_alloc&) { threw = true; }
    fail_after = -1; Require(threw && RowPatternOracle(decoded) == before, "row-pattern decode fault published"); ++faults;
  }
  Require(encode_sites > 0 && decode_sites >= 8, "row-pattern allocation campaign incomplete");
}

void TestPropertyIdentityLifecycle() {
  namespace parser = scratchbird::parser::sbsql;
  using Role = parser::BoundRelationalPropertyRole;
  parser::BoundNativeRelationalDocument base;
  base.bound = true;
  parser::BoundRelationAstRecord sort; sort.relation_id = 1; sort.relation_kind = parser::NativeRelationAstKind::kSort;
  parser::BoundRelationAstRecord window; window.relation_id = 2; window.relation_kind = parser::NativeRelationAstKind::kWindow;
  window.window_invocation_ids = {11, 12, 13};
  parser::BoundRelationAstRecord second_window = window; second_window.relation_id = 3;
  parser::BoundRelationAstRecord match; match.relation_id = 4; match.relation_kind = parser::NativeRelationAstKind::kMatchRecognize;
  base.relations = {sort, window, second_window, match};
  for (const auto id : {21u, 22u}) { parser::BoundWindowDefinitionAstRecord d; d.window_id = id; base.window_definitions.push_back(d); }
  for (const auto id : {11u, 12u, 13u}) {
    parser::BoundWindowInvocationAstRecord i; i.invocation_id = id; i.window_definition_id = id == 13 ? 22 : 21;
    base.window_invocations.push_back(i);
  }
  parser::BoundRowPatternAstRecord pattern; pattern.pattern_id = 31; pattern.relation_id = 4; base.row_patterns = {pattern};
  auto bound = base;
  Require(parser::FinalizeRelationalPropertyIdentities(&bound) && bound.property_identities.size() == 19 &&
          parser::ValidateRelationalPropertyIdentities(bound), "actual runtime property identity issuance failed");
  const auto retained = bound.property_identities;
  Require(parser::FinalizeRelationalPropertyIdentities(&bound) && bound.property_identities == retained,
          "repeat binding changed retained property identities");
  auto fresh = base;
  Require(parser::FinalizeRelationalPropertyIdentities(&fresh), "second actual runtime property issuance failed");
  for (const auto& record : fresh.property_identities)
    Require(std::ranges::none_of(retained, [&](const auto& previous) { return previous.uuid == record.uuid; }),
            "independent bound documents share a property UUID");
  Require(parser::FindRelationalPropertyIdentity(bound, {2, 21, Role::kWindowOrdering}) !=
          parser::FindRelationalPropertyIdentity(bound, {3, 21, Role::kWindowOrdering}), "window nodes share identity");
  parser::BoundRelationAstRecord cte; cte.relation_id = 5; cte.relation_kind = parser::NativeRelationAstKind::kCte;
  bound.relations.push_back(cte);
  Require(parser::FinalizeRelationalPropertyIdentities(&bound) && bound.property_identities == retained, "CTE wrapper reissued producer IDs");
  auto extended = bound; sort.relation_id = 6; extended.relations.push_back(sort);
  Require(parser::FinalizeRelationalPropertyIdentities(&extended) && extended.property_identities.size() == 20 &&
          std::equal(retained.begin(), retained.end(), extended.property_identities.begin()),
          "binding extension lost previous IDs");
  auto partial = bound; partial.property_identities.resize(7);
  const auto partial_before = partial.property_identities;
  unsigned attempts = 0;
  Require(!parser::property_identity_detail::Finalize(&partial, [&]() -> std::optional<Uuid> {
    if (++attempts == 3) return std::nullopt; return Id(100 + attempts);
  }) && attempts == 3 && partial.property_identities == partial_before, "issuer failure partially published IDs");
  for (unsigned invalid = 0; invalid < 5; ++invalid) {
    auto candidate = bound;
    if (invalid == 0) candidate.property_identities[0].uuid = {};
    if (invalid == 1) candidate.property_identities[0].uuid = candidate.property_identities[1].uuid;
    if (invalid == 2) std::swap(candidate.property_identities[0], candidate.property_identities[1]);
    if (invalid == 3) candidate.property_identities[0].key.relation_id = 99;
    if (invalid == 4) candidate.property_identities[0].uuid.bytes[6] = 0x40;
    const auto before = candidate.property_identities;
    Require(!parser::ValidateRelationalPropertyIdentities(candidate) && !parser::FinalizeRelationalPropertyIdentities(&candidate) &&
            candidate.property_identities == before, "bad retained map repaired or accepted");
  }
  for (const auto bad_id : {Uuid{}, Id(1)}) {
    auto candidate = partial; candidate.property_identities[0].uuid = Id(1);
    const auto before = candidate.property_identities;
    Require(!parser::property_identity_detail::Finalize(&candidate, [&]() -> std::optional<Uuid> { return bad_id; }) &&
            candidate.property_identities == before, "nil or colliding issuer output accepted");
  }
  auto stale = bound; stale.relations.erase(stale.relations.begin());
  Require(!parser::FinalizeRelationalPropertyIdentities(&stale) && stale.property_identities == retained,
          "obsolete property key silently discarded");
  auto dangling = base; dangling.relations[1].window_invocation_ids.push_back(999);
  Require(!parser::FinalizeRelationalPropertyIdentities(&dangling) && dangling.property_identities.empty(),
          "dangling window reference got an identity");
  auto duplicate_node = base; duplicate_node.relations.push_back(duplicate_node.relations.front());
  Require(!parser::FinalizeRelationalPropertyIdentities(&duplicate_node), "duplicate relation identity admitted");
  parser::BoundStatement reservation; reservation.bound = true; reservation.native_relational = bound;
  parser::SblrEnvelope lowered;
  const auto frozen = parser::FreezeContextualReservationSkeletonV2(reservation, lowered, {});
  Require(frozen.has_value(), "retained property map did not freeze");
  for (std::size_t index = 0; index < retained.size(); ++index) for (unsigned byte = 0; byte < 16; ++byte) {
    auto changed = reservation; changed.native_relational.property_identities[index].uuid.bytes[byte] ^= 1;
    const auto observed = parser::FreezeContextualReservationSkeletonV2(changed, lowered, {});
    Require(!observed || observed != frozen, "reservation ignored retained property UUID byte");
  }
  auto missing = reservation; missing.native_relational.property_identities.pop_back();
  Require(!parser::FreezeContextualReservationSkeletonV2(missing, lowered, {}), "reservation accepted missing property key");
  for (const auto field : {0, 1, 2}) {
    auto changed = reservation;
    if (field == 0) ++changed.native_relational.property_identities[0].key.relation_id;
    if (field == 1) ++changed.native_relational.property_identities[0].key.subject_id;
    if (field == 2) changed.native_relational.property_identities[0].key.role = Role::kWindowFrame;
    Require(!parser::FreezeContextualReservationSkeletonV2(changed, lowered, {}), "reservation accepted changed property key");
  }
  auto baseline = partial;
  unsigned sequence = 100; allocations = 0;
  Require(parser::property_identity_detail::Finalize(&baseline, [&]() -> std::optional<Uuid> { return Id(++sequence); }),
          "property lifecycle allocation baseline failed");
  const auto sites = allocations;
  for (std::size_t site = 0; site < sites; ++site) {
    auto candidate = partial; sequence = 100; bool threw = false; fail_after = site;
    try { (void)parser::property_identity_detail::Finalize(&candidate, [&]() -> std::optional<Uuid> { return Id(++sequence); }); }
    catch (const std::bad_alloc&) { threw = true; }
    fail_after = -1;
    Require(threw && candidate.property_identities == partial_before, "property lifecycle fault published partial identity map"); ++faults;
  }
  Require(sites > retained.size(), "property identity allocation campaign incomplete");
}

Bytes PropertyOracle(const api::RelationalPropertyRecord& value) {
  Bytes out;
  Append(out, 1, 2);
  Append(out, (value.window_frame_descriptor_uuid.is_nil() ? 0 : 1) |
      (value.locality_uuid.is_nil() ? 0 : 2) | (value.security_visibility_context_uuid.is_nil() ? 0 : 4), 2);
  Append(out, value.origin_node_id, 4);
  out.insert(out.end(), value.property_uuid.bytes.begin(), value.property_uuid.bytes.end());
  for (const auto tag : {static_cast<unsigned>(value.property_kind), static_cast<unsigned>(value.distribution_kind),
                        static_cast<unsigned>(value.materialization_kind), static_cast<unsigned>(value.rewindability_kind),
                        static_cast<unsigned>(value.locality_kind)}) Append(out, tag, 1);
  Append(out, 0, 3);
  Append(out, value.expression_ids.size(), 4); Append(out, value.ordering_terms.size(), 4);
  Append(out, value.dependency_property_uuids.size(), 4); Append(out, 0, 4);
  for (const auto& id : {value.window_frame_descriptor_uuid, value.locality_uuid, value.security_visibility_context_uuid})
    out.insert(out.end(), id.bytes.begin(), id.bytes.end());
  Append(out, value.security_visibility_generation, 8);
  Require(out.size() == 104, "property prefix differs from Core layout");
  for (const auto id : value.expression_ids) Append(out, id, 4);
  for (const auto& term : value.ordering_terms) {
    Append(out, term.expression_id, 4); Append(out, static_cast<unsigned>(term.direction), 1);
    Append(out, static_cast<unsigned>(term.null_placement), 1); Append(out, term.collation_uuid.is_nil() ? 0 : 1, 2);
    out.insert(out.end(), term.collation_uuid.bytes.begin(), term.collation_uuid.bytes.end());
  }
  for (const auto& id : value.dependency_property_uuids) out.insert(out.end(), id.bytes.begin(), id.bytes.end());
  return out;
}

void TestProperty() {
  namespace parser = scratchbird::parser::sbsql;
  api::RelationalPropertyRecord value;
  value.property_uuid = Id(41); value.origin_node_id = 0x01020304;
  value.expression_ids = {17, 19, 17};
  value.ordering_terms = {
      {21, api::RelationalPropertySortDirection::kAscending, api::RelationalPropertyNullPlacement::kNullsLast, Id(42)},
      {22, api::RelationalPropertySortDirection::kDescending, api::RelationalPropertyNullPlacement::kNullsFirst, {}}};
  value.dependency_property_uuids = {Id(43), Id(44), Id(43)};
  value.window_frame_descriptor_uuid = Id(45); value.locality_uuid = Id(46);
  value.security_visibility_context_uuid = Id(47); value.security_visibility_generation = 0x0102030405060708ULL;
  value.distribution_kind = api::RelationalPropertyDistributionKind::kOwnerRouted;
  value.materialization_kind = api::RelationalPropertyMaterializationKind::kSpillBacked;
  value.rewindability_kind = api::RelationalPropertyRewindabilityKind::kMarkRestore;
  value.locality_kind = api::RelationalPropertyLocalityKind::kRemoteAllowed;
  // Structural round trips deliberately retain combinations that graph semantics
  // must reject. The carrier must not silently rewrite or default any field.
  Bytes encoded;
  api::RelationalPropertyRecord decoded;
  for (unsigned kind = 1; kind <= 14; ++kind) for (unsigned flags = 0; flags < 8; ++flags)
    for (unsigned variant = 0; variant < 42; ++variant) {
      auto candidate = value; candidate.property_kind = static_cast<api::RelationalPropertyKind>(kind);
      if (!(flags & 1)) candidate.window_frame_descriptor_uuid = {};
      if (!(flags & 2)) candidate.locality_uuid = {};
      if (!(flags & 4)) candidate.security_visibility_context_uuid = {};
      candidate.distribution_kind = static_cast<api::RelationalPropertyDistributionKind>(variant % 7);
      candidate.materialization_kind = static_cast<api::RelationalPropertyMaterializationKind>(variant % 4);
      candidate.rewindability_kind = static_cast<api::RelationalPropertyRewindabilityKind>((variant + 1) % 4);
      candidate.locality_kind = static_cast<api::RelationalPropertyLocalityKind>(variant % 6);
      candidate.security_visibility_generation = variant % 2 ? UINT64_MAX : 0;
      Require(wire::EncodeRelationalPropertyV1(candidate, &encoded) && encoded == PropertyOracle(candidate),
              "property fields or byte order changed");
      Require(wire::DecodeRelationalPropertyV1(encoded.data(), encoded.size(), &decoded) &&
              PropertyOracle(decoded) == encoded, "property decode lost generic or specialized state");
    }
  const auto original = PropertyOracle(value);
  const auto reject = [&](const Bytes& bytes) {
    auto sentinel = value; sentinel.property_uuid = Id(99); sentinel.expression_ids = {97};
    const auto before = PropertyOracle(sentinel);
    Require(!wire::DecodeRelationalPropertyV1(bytes.data(), bytes.size(), &sentinel) &&
            PropertyOracle(sentinel) == before, "malformed property accepted or output changed");
  };
  Require(!wire::DecodeRelationalPropertyV1(nullptr, original.size(), &decoded) &&
          !wire::DecodeRelationalPropertyV1(original.data(), original.size(), nullptr) &&
          !wire::EncodeRelationalPropertyV1(value, nullptr), "property null argument accepted");
  for (std::size_t size = 0; size < original.size(); ++size) reject(Bytes(original.begin(), original.begin() + size));
  auto bad = original; bad.push_back(0); reject(bad);
  for (const auto offset : {0u, 1u, 3u, 29u, 30u, 31u, 44u, 45u, 46u, 47u}) {
    bad = original; bad[offset] ^= 0x80; reject(bad);
  }
  for (unsigned bit = 0; bit < 8; ++bit) { bad = original; bad[2] ^= 1u << bit; reject(bad); }
  for (const auto offset : {4u, 104u, 108u, 112u, 116u, 140u}) {
    bad = original; std::fill_n(bad.begin() + offset, 4, 0); reject(bad);
  }
  for (const auto offset : {32u, 36u, 40u}) {
    bad = original; std::fill_n(bad.begin() + offset, 4, 0xff); reject(bad);
  }
  for (unsigned tag = 0; tag < 256; ++tag) {
    for (const auto [offset, maximum] : {std::pair{24u, 14u}, {25u, 6u}, {26u, 3u}, {27u, 3u}, {28u, 5u},
                                        {120u, 2u}, {121u, 2u}, {144u, 2u}, {145u, 2u}}) {
      if (tag > maximum || (tag == 0 && (offset == 24 || offset >= 120))) {
        bad = original; bad[offset] = tag; reject(bad);
      }
    }
  }
  for (const auto offset : {8u, 48u, 64u, 80u, 124u, 164u, 180u, 196u}) {
    bad = original; std::fill_n(bad.begin() + offset, 16, 0); reject(bad);
    for (unsigned byte = 0; byte < 256; ++byte) {
      if ((byte & 0xf0) != 0x70) { bad = original; bad[offset + 6] = byte; reject(bad); }
      if ((byte & 0xc0) != 0x80) { bad = original; bad[offset + 8] = byte; reject(bad); }
    }
  }
  for (const auto offset : {122u, 123u, 146u, 147u}) {
    bad = original; bad[offset] ^= 0x80; reject(bad);
  }
  bad = original; bad[122] = 0; reject(bad);
  bad = original; bad[146] = 1; reject(bad);
  auto empty = value; empty.expression_ids.clear(); empty.ordering_terms.clear(); empty.dependency_property_uuids.clear();
  Require(wire::EncodeRelationalPropertyV1(empty, &encoded) && encoded.size() == 104 &&
          wire::DecodeRelationalPropertyV1(encoded.data(), encoded.size(), &decoded) &&
          PropertyOracle(decoded) == encoded, "empty property vectors changed");
  auto maximum = empty; maximum.expression_ids.assign((65536 - 104) / 4, 1);
  Require(wire::EncodeRelationalPropertyV1(maximum, &encoded) && encoded.size() == 65536 &&
          wire::DecodeRelationalPropertyV1(encoded.data(), encoded.size(), &decoded) &&
          decoded.expression_ids == maximum.expression_ids, "maximum property lost");
  maximum.expression_ids.push_back(1); encoded = {0xfa};
  Require(!wire::EncodeRelationalPropertyV1(maximum, &encoded) && encoded == Bytes{0xfa}, "oversized property published");
  reject(PropertyOracle(maximum));
  for (unsigned invalid = 0; invalid < 9; ++invalid) {
    auto candidate = value;
    if (invalid == 0) candidate.property_uuid = {};
    if (invalid == 1) candidate.origin_node_id = 0;
    if (invalid == 2) candidate.expression_ids[0] = 0;
    if (invalid == 3) candidate.ordering_terms[0].collation_uuid.bytes[6] = 0x40;
    if (invalid == 4) candidate.dependency_property_uuids[0] = {};
    if (invalid == 5) candidate.locality_uuid.bytes[8] = 0;
    if (invalid == 6) candidate.security_visibility_context_uuid.bytes[6] = 0x50;
    if (invalid == 7) candidate.window_frame_descriptor_uuid.bytes[6] = 0x40;
    if (invalid == 8) candidate.distribution_kind = static_cast<api::RelationalPropertyDistributionKind>(255);
    encoded = {0xab};
    Require(!wire::EncodeRelationalPropertyV1(candidate, &encoded) && encoded == Bytes{0xab},
            "invalid property source published");
  }
  const auto operand = parser::MakeRelationalPropertyOperand(value);
  Require(operand && operand->type == "relational_property_v3" && operand->name == "property" &&
          operand->canonical_value_kind == 218 && operand->canonical_value_body == original && operand->value.empty() &&
          parser::DecodeRelationalPropertyOperand(*operand, &decoded) && PropertyOracle(decoded) == original,
          "property lowering adapter changed exact fields");
  const auto frozen = parser::FreezeContextualOperandsV3({*operand}, {});
  Require(frozen && !parser::FreezeContextualOperandsV3({*operand, *operand}, {}), "property freeze duplicate identity accepted");
  auto second = value; second.property_uuid = Id(98);
  const auto other = parser::MakeRelationalPropertyOperand(second);
  Require(other && parser::FreezeContextualOperandsV3({*operand, *other}, {}), "distinct property identities rejected by name");
  for (std::size_t index = 0; index < original.size(); ++index) {
    auto altered = *operand; altered.canonical_value_body[index] ^= 1;
    const auto observed = parser::FreezeContextualOperandsV3({altered}, {});
    Require(!observed || observed != frozen, "property freeze ignored changed byte");
  }
  for (const auto* legacy : {"relational_property_v1", "relational_property_v2"}) {
    auto altered = *operand; altered.type = legacy;
    Require(!parser::FreezeContextualOperandsV3({altered}, {}) && !parser::DecodeRelationalPropertyOperand(altered, &decoded),
            "legacy property frozen or decoded");
  }
  auto envelope = wire::MakeSblrEnvelope("query.execute", "SBLR_QUERY_EXECUTE", "property.binary.fixture");
  envelope.opcode_code = 4615; envelope.parser_package_uuid = Id(6); envelope.registry_snapshot_uuid = Id(7);
  envelope.result_shape = "query_execute_result"; envelope.diagnostic_shape = "diagnostic_vector";
  wire::SblrOperand canonical;
  canonical.ordinal = 1; canonical.type = operand->type; canonical.name = operand->name;
  canonical.value_kind = wire::SblrValueKind::relational_property; canonical.value_body = original;
  envelope.operands = {canonical};
  Require(wire::DecodeSblrEnvelope(wire::EncodeSblrEnvelope(envelope)).ok, "property SBOP round trip failed");
  auto other_canonical = canonical; other_canonical.ordinal = 2; other_canonical.value_body = other->canonical_value_body;
  envelope.operands.push_back(other_canonical);
  Require(wire::DecodeSblrEnvelope(wire::EncodeSblrEnvelope(envelope)).ok, "distinct SBOP property bodies rejected by name");
  for (const auto* name : {"slot_1", "property_01a07c0a0d5c70008000ff0000000029", "Property", ""}) {
    auto wrong = envelope; wrong.operands[0].name = name;
    Require(!wire::ValidateSblrEnvelope(wrong).ok, "property name substituted for binary identity");
  }
  for (const auto* legacy : {"relational_property_v1", "relational_property_v2"}) {
    auto wrong = envelope; wrong.operands[0].type = legacy;
    Require(!wire::ValidateSblrEnvelope(wrong).ok, "legacy property SBOP admitted");
  }
  auto wrong = envelope; wrong.operands[0].value = "shadow";
  Require(!wire::ValidateSblrEnvelope(wrong).ok, "property text shadow accepted");
  wrong = envelope; wrong.operation_id = "engine.op.package_begin"; wrong.opcode = "SBLR_PACKAGE_BEGIN"; wrong.opcode_code = 1;
  Require(!wire::ValidateSblrEnvelope(wrong).ok, "property escaped query root");
  for (const auto container : {wire::SblrValueKind::list, wire::SblrValueKind::map}) {
    Bytes nested; Append(nested, 1, 4);
    if (container == wire::SblrValueKind::map) { Append(nested, 1, 4); nested.push_back('x'); }
    Append(nested, 218, 2); Append(nested, 0, 2); Append(nested, original.size(), 8);
    nested.insert(nested.end(), original.begin(), original.end());
    wrong = envelope; wrong.operands[0].type = "nested"; wrong.operands[0].name = "nested";
    wrong.operands[0].value_kind = container; wrong.operands[0].value_body = nested;
    Require(!wire::ValidateSblrEnvelope(wrong).ok, "nested property accepted");
  }
  allocations = 0; Require(wire::EncodeRelationalPropertyV1(value, &encoded), "property encode fault baseline");
  const auto encode_sites = allocations;
  for (std::size_t site = 0; site < encode_sites; ++site) {
    encoded = {0xfa}; bool threw = false; fail_after = site;
    try { (void)wire::EncodeRelationalPropertyV1(value, &encoded); } catch (const std::bad_alloc&) { threw = true; }
    fail_after = -1; Require(threw && encoded == Bytes{0xfa}, "property encode fault published"); ++faults;
  }
  allocations = 0; Require(wire::DecodeRelationalPropertyV1(original.data(), original.size(), &decoded), "property decode fault baseline");
  const auto decode_sites = allocations;
  for (std::size_t site = 0; site < decode_sites; ++site) {
    decoded = value; decoded.property_uuid = Id(99); decoded.expression_ids = {97};
    const auto before = PropertyOracle(decoded); bool threw = false; fail_after = site;
    try { (void)wire::DecodeRelationalPropertyV1(original.data(), original.size(), &decoded); } catch (const std::bad_alloc&) { threw = true; }
    fail_after = -1; Require(threw && PropertyOracle(decoded) == before, "property decode fault published"); ++faults;
  }
  Require(encode_sites > 0 && decode_sites == 3, "property allocation fault campaign incomplete");
}

Bytes WindowDefinitionOracle(const api::RelationalWindowDefinitionRecord& value) {
  Bytes out;
  Append(out, 1, 2);
  Append(out, (value.canonical_name_key ? 1 : 0) | (value.inherited_window_id ? 2 : 0) |
      (value.frame_unit ? 4 : 0) | (value.frame_start ? 8 : 0) | (value.frame_end ? 16 : 0), 2);
  Append(out, value.window_id, 4); Append(out, value.relation_node_id, 4);
  Append(out, value.inherited_window_id.value_or(0), 4);
  Append(out, value.partition_expression_ids.size(), 4); Append(out, value.ordering_terms.size(), 4);
  Append(out, value.canonical_name_key ? value.canonical_name_key->size() : 0, 4);
  Append(out, value.frame_unit ? static_cast<unsigned>(*value.frame_unit) : 0, 1);
  Append(out, static_cast<unsigned>(value.exclusion), 1); Append(out, 0, 2);
  for (const auto& bound : {value.frame_start, value.frame_end}) {
    Append(out, bound ? static_cast<unsigned>(bound->bound_kind) : 0, 1);
    Append(out, bound && bound->offset_expression_id ? 1 : 0, 1); Append(out, 0, 2);
    Append(out, bound ? bound->offset_expression_id.value_or(0) : 0, 4);
  }
  if (value.canonical_name_key) out.insert(out.end(), value.canonical_name_key->begin(), value.canonical_name_key->end());
  for (const auto id : value.partition_expression_ids) Append(out, id, 4);
  for (const auto& term : value.ordering_terms) {
    Append(out, term.expression_id, 4); Append(out, static_cast<unsigned>(term.direction), 1);
    Append(out, static_cast<unsigned>(term.null_placement), 1); Append(out, term.collation_uuid.is_nil() ? 0 : 1, 2);
    out.insert(out.end(), term.collation_uuid.bytes.begin(), term.collation_uuid.bytes.end());
  }
  return out;
}

void TestWindowDefinition() {
  namespace parser = scratchbird::parser::sbsql;
  api::RelationalWindowDefinitionRecord value;
  value.window_id = 17; value.relation_node_id = 18; value.inherited_window_id = 19;
  value.canonical_name_key = std::string("name\0|with_utf8_", 15) + "\xc3\xa9";
  value.partition_expression_ids = {20, 21, 20};
  value.ordering_terms = {{22, api::RelationalPropertySortDirection::kAscending,
      api::RelationalPropertyNullPlacement::kNullsLast, Id(60)},
      {23, api::RelationalPropertySortDirection::kDescending,
       api::RelationalPropertyNullPlacement::kNullsFirst, {}}};
  value.frame_unit = api::RelationalWindowFrameUnit::kRows;
  value.frame_start = api::RelationalWindowFrameBoundRecord{api::RelationalWindowFrameBoundKind::kPreceding, 24};
  value.frame_end = api::RelationalWindowFrameBoundRecord{api::RelationalWindowFrameBoundKind::kFollowing, 25};
  value.exclusion = api::RelationalWindowFrameExclusion::kTies;
  for (unsigned flags = 0; flags < 32; ++flags) for (unsigned unit = 1; unit <= 3; ++unit)
    for (unsigned exclusion = 1; exclusion <= 4; ++exclusion) for (unsigned start = 1; start <= 5; ++start)
      for (unsigned end = 1; end <= 5; ++end) {
        auto candidate = value;
        if (!(flags & 1)) candidate.canonical_name_key.reset();
        if (!(flags & 2)) candidate.inherited_window_id.reset();
        candidate.frame_unit = static_cast<api::RelationalWindowFrameUnit>(unit);
        if (!(flags & 4)) candidate.frame_unit.reset();
        candidate.frame_start->bound_kind = static_cast<api::RelationalWindowFrameBoundKind>(start);
        candidate.frame_end->bound_kind = static_cast<api::RelationalWindowFrameBoundKind>(end);
        if (start % 2) candidate.frame_start->offset_expression_id.reset();
        if (end % 2) candidate.frame_end->offset_expression_id.reset();
        if (!(flags & 8)) candidate.frame_start.reset();
        if (!(flags & 16)) candidate.frame_end.reset();
        candidate.exclusion = static_cast<api::RelationalWindowFrameExclusion>(exclusion);
        Bytes encoded{0xfa}; api::RelationalWindowDefinitionRecord decoded;
        Require(wire::EncodeRelationalWindowDefinitionV1(candidate, &encoded) && encoded == WindowDefinitionOracle(candidate),
                "window definition differs from independent state/byte oracle");
        Require(wire::DecodeRelationalWindowDefinitionV1(encoded.data(), encoded.size(), &decoded) &&
                WindowDefinitionOracle(decoded) == encoded, "window definition lost optional state or ordering");
      }
  const auto original = WindowDefinitionOracle(value);
  const auto reject = [&](const Bytes& bytes) {
    auto output = value; output.window_id = 99; output.canonical_name_key = "previous";
    output.frame_start.reset(); const auto before = WindowDefinitionOracle(output);
    Require(!wire::DecodeRelationalWindowDefinitionV1(bytes.data(), bytes.size(), &output) &&
            WindowDefinitionOracle(output) == before, "invalid window definition accepted or changed output");
  };
  for (std::size_t length = 0; length < original.size(); ++length) reject(Bytes(original.begin(), original.begin() + length));
  auto changed = original; changed.push_back(0); reject(changed);
  for (unsigned offset : {0u, 1u, 30u, 31u, 34u, 35u, 42u, 43u}) {
    changed = original; changed[offset] ^= 0x80; reject(changed);
  }
  for (unsigned bit = 5; bit < 16; ++bit) {
    changed = original; changed[2 + bit / 8] |= 1u << (bit % 8); reject(changed);
  }
  for (unsigned bit = 0; bit < 5; ++bit) {
    changed = original; changed[2] &= ~(1u << bit); reject(changed);
  }
  for (unsigned offset : {4u, 8u, 12u, 36u, 44u}) {
    changed = original; std::fill_n(changed.begin() + offset, 4, 0); reject(changed);
  }
  for (unsigned offset : {16u, 20u, 24u}) {
    changed = original; std::fill_n(changed.begin() + offset, 4, 0xff); reject(changed);
  }
  for (unsigned offset : {33u, 41u}) for (unsigned invalid : {0u, 2u, 255u}) {
    changed = original; changed[offset] = invalid; reject(changed);
  }
  for (unsigned invalid = 0; invalid < 256; ++invalid) {
    for (auto [offset, maximum] : {std::pair{28u, 3u}, {29u, 4u}, {32u, 5u}, {40u, 5u}})
      if (invalid < 1 || invalid > maximum) { changed = original; changed[offset] = invalid; reject(changed); }
  }
  const auto partitions = 48 + value.canonical_name_key->size();
  changed = original; std::fill_n(changed.begin() + partitions, 4, 0); reject(changed);
  const auto ordering = partitions + value.partition_expression_ids.size() * 4;
  for (std::size_t offset : {ordering, ordering + 24}) {
    changed = original; std::fill_n(changed.begin() + offset, 4, 0); reject(changed);
    for (unsigned invalid = 0; invalid < 256; ++invalid) if (invalid != 1 && invalid != 2) {
      changed = original; changed[offset + 4] = invalid; reject(changed);
      changed = original; changed[offset + 5] = invalid; reject(changed);
    }
    changed = original; changed[offset + 6] ^= 1; reject(changed);
    changed = original; changed[offset + 7] = 1; reject(changed);
  }
  for (unsigned version = 0; version < 16; ++version) if (version != 7) {
    changed = original; changed[ordering + 14] = version << 4; reject(changed);
  }
  for (unsigned variant : {0u, 1u, 3u}) {
    changed = original; changed[ordering + 16] = variant << 6; reject(changed);
  }
  changed = original; std::fill_n(changed.begin() + ordering + 8, 16, 0); reject(changed);
  auto maximum = value; maximum.partition_expression_ids.clear(); maximum.ordering_terms.clear();
  maximum.canonical_name_key = std::string(65536 - 48, 'x');
  Bytes encoded; api::RelationalWindowDefinitionRecord decoded;
  Require(wire::EncodeRelationalWindowDefinitionV1(maximum, &encoded) && encoded.size() == 65536 &&
          wire::DecodeRelationalWindowDefinitionV1(encoded.data(), encoded.size(), &decoded) &&
          WindowDefinitionOracle(decoded) == encoded, "maximum window definition lost");
  maximum.canonical_name_key->push_back('x');
  Require(!wire::EncodeRelationalWindowDefinitionV1(maximum, &encoded) && encoded.size() == 65536,
          "oversize window definition encoded or changed output");
  reject(WindowDefinitionOracle(maximum));
  maximum.canonical_name_key = "";
  Require(wire::EncodeRelationalWindowDefinitionV1(maximum, &encoded) &&
          wire::DecodeRelationalWindowDefinitionV1(encoded.data(), encoded.size(), &decoded) &&
          decoded.canonical_name_key && decoded.canonical_name_key->empty(), "empty window name became absent");
  maximum.canonical_name_key.reset();
  Require(wire::EncodeRelationalWindowDefinitionV1(maximum, &encoded) &&
          wire::DecodeRelationalWindowDefinitionV1(encoded.data(), encoded.size(), &decoded) &&
          !decoded.canonical_name_key, "absent window name became present");
  parser::BoundWindowDefinitionAstRecord bound;
  bound.window_id = value.window_id; bound.canonical_name_key = value.canonical_name_key;
  bound.inherited_window_id = value.inherited_window_id; bound.partition_expression_ids = value.partition_expression_ids;
  for (const auto& term : value.ordering_terms) {
    parser::BoundOrderingAstTerm source;
    source.expression_id = term.expression_id;
    source.direction = static_cast<parser::NativeSortDirection>(static_cast<unsigned>(term.direction) - 1);
    source.null_placement = static_cast<parser::NativeNullPlacement>(static_cast<unsigned>(term.null_placement) - 1);
    bound.ordering_terms.push_back(source);
  }
  bound.frame_unit = parser::NativeWindowFrameUnit::kRows;
  bound.frame_start = parser::BoundWindowFrameBoundAstRecord{parser::NativeWindowFrameBoundKind::kPreceding, 24};
  bound.frame_end = parser::BoundWindowFrameBoundAstRecord{parser::NativeWindowFrameBoundKind::kFollowing, 25};
  bound.exclusion = parser::NativeWindowFrameExclusion::kTies;
  const auto operand = parser::MakeRelationalWindowDefinitionOperand(bound, value.relation_node_id, value.ordering_terms);
  Require(operand && operand->canonical_value_body == original && operand->canonical_value_kind == 217 &&
          operand->name == "slot_17" && operand->value.empty() &&
          parser::DecodeRelationalWindowDefinitionOperand(*operand, &decoded), "BoundAST window definition changed");
  auto mismatched_ordering = value.ordering_terms;
  std::swap(mismatched_ordering[0], mismatched_ordering[1]);
  Require(!parser::MakeRelationalWindowDefinitionOperand(bound, value.relation_node_id, mismatched_ordering),
          "window definition adapter accepted reordered bound terms");
  mismatched_ordering = value.ordering_terms; mismatched_ordering.pop_back();
  Require(!parser::MakeRelationalWindowDefinitionOperand(bound, value.relation_node_id, mismatched_ordering),
          "window definition adapter accepted truncated bound terms");
  mismatched_ordering = value.ordering_terms;
  mismatched_ordering.front().null_placement = api::RelationalPropertyNullPlacement::kNullsFirst;
  Require(!parser::MakeRelationalWindowDefinitionOperand(bound, value.relation_node_id, mismatched_ordering),
          "window definition adapter accepted changed bound null placement");
  const auto frozen = parser::FreezeContextualOperandsV3({*operand}, {});
  Require(frozen && !parser::FreezeContextualOperandsV3({*operand, *operand}, {}),
          "window freeze refused valid or accepted duplicate definition");
  for (std::size_t byte = 0; byte < original.size(); ++byte) {
    auto altered = *operand; altered.canonical_value_body[byte] ^= 1;
    const auto observed = parser::FreezeContextualOperandsV3({altered}, {});
    Require(!observed || observed != frozen, "window definition freeze ignored changed byte");
  }
  auto envelope = wire::MakeSblrEnvelope("query.execute", "SBLR_QUERY_EXECUTE", "window.definition.fixture");
  envelope.opcode_code = 4615; envelope.parser_package_uuid = Id(6); envelope.registry_snapshot_uuid = Id(7);
  envelope.result_shape = "query_execute_result"; envelope.diagnostic_shape = "diagnostic_vector";
  wire::SblrOperand canonical;
  canonical.ordinal = 1; canonical.type = operand->type; canonical.name = operand->name;
  canonical.value_kind = wire::SblrValueKind::relational_window_definition; canonical.value_body = original;
  envelope.operands = {canonical};
  Require(wire::DecodeSblrEnvelope(wire::EncodeSblrEnvelope(envelope)).ok, "window definition SBOP round trip");
  for (auto name : {"slot_017", "slot_18", "17", "slot_0"}) {
    auto wrong = envelope; wrong.operands[0].name = name;
    Require(!wire::ValidateSblrEnvelope(wrong).ok, "window definition slot substitution accepted");
  }
  auto wrong = envelope; wrong.operands[0].type = "relational_window_definition_v1";
  Require(!wire::ValidateSblrEnvelope(wrong).ok, "legacy window definition admitted");
  wrong = envelope; wrong.operands[0].value = "shadow";
  Require(!wire::ValidateSblrEnvelope(wrong).ok, "window definition text shadow admitted");
  wrong = envelope; wrong.operation_id = "engine.op.package_begin"; wrong.opcode = "SBLR_PACKAGE_BEGIN"; wrong.opcode_code = 1;
  Require(!wire::ValidateSblrEnvelope(wrong).ok, "window definition escaped query root");
  Bytes nested; Append(nested, 1, 4); Append(nested, 217, 2); Append(nested, 0, 2); Append(nested, original.size(), 8);
  nested.insert(nested.end(), original.begin(), original.end());
  wrong = envelope; wrong.operands[0].type = "list"; wrong.operands[0].name = "nested";
  wrong.operands[0].value_kind = wire::SblrValueKind::list; wrong.operands[0].value_body = nested;
  Require(!wire::ValidateSblrEnvelope(wrong).ok, "nested window definition admitted");
  allocations = 0; Require(wire::EncodeRelationalWindowDefinitionV1(value, &encoded), "window definition encode fault baseline");
  const auto encode_sites = allocations;
  for (std::size_t site = 0; site < encode_sites; ++site) {
    encoded = {0xfa}; bool threw = false; fail_after = site;
    try { (void)wire::EncodeRelationalWindowDefinitionV1(value, &encoded); } catch (const std::bad_alloc&) { threw = true; }
    fail_after = -1; Require(threw && encoded == Bytes{0xfa}, "window definition encode fault changed output"); ++faults;
  }
  allocations = 0; Require(wire::DecodeRelationalWindowDefinitionV1(original.data(), original.size(), &decoded), "window definition decode fault baseline");
  const auto decode_sites = allocations;
  for (std::size_t site = 0; site < decode_sites; ++site) {
    decoded = value; decoded.window_id = 99; decoded.canonical_name_key = "previous";
    const auto before = WindowDefinitionOracle(decoded); bool threw = false; fail_after = site;
    try { (void)wire::DecodeRelationalWindowDefinitionV1(original.data(), original.size(), &decoded); } catch (const std::bad_alloc&) { threw = true; }
    fail_after = -1; Require(threw && WindowDefinitionOracle(decoded) == before, "window definition decode fault changed output"); ++faults;
  }
  Require(encode_sites > 0 && decode_sites >= 3, "window definition allocation campaign empty");
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
namespace {
void TestFrozenOperands() {
  namespace parser = scratchbird::parser::sbsql;
  const auto operand_for = [](const Descriptor& descriptor) {
    parser::SblrOperand operand;
    operand.type = "relational_descriptor_v3";
    operand.name = "slot_" + std::to_string(descriptor.descriptor_id);
    operand.canonical_value_kind = 213;
    operand.canonical_value_body = Oracle(descriptor);
    return operand;
  };
  auto descriptor = Fixture();
  descriptor.descriptor_id = 17;
  const auto operand = operand_for(descriptor);
  const auto projection = parser::FreezeContextualOperandsV3({operand}, {});
  Require(projection.has_value(), "immutable descriptor freeze refused");
  // Independent field-by-field oracle, including embedded zero bytes.
  Bytes expected{3, 0, 1, 0, 0, 0, 0};
  for (const auto& field : {operand.type, operand.name, operand.value}) {
    Append(expected, field.size(), 4);
    expected.insert(expected.end(), field.begin(), field.end());
  }
  Append(expected, 213, 2); Append(expected, operand.canonical_value_body.size(), 4);
  expected.insert(expected.end(), operand.canonical_value_body.begin(), operand.canonical_value_body.end());
  Require(*projection == expected, "frozen projection differs from independent framing oracle");
  const auto mutable_projection = parser::FreezeContextualOperandsV3({operand}, {17});
  Require(mutable_projection == std::optional<Bytes>{{3, 0, 1, 0, 0, 0, 1, 17, 0, 0, 0}},
          "mutable descriptor projection differs from exact handle oracle");
  // Every byte is either rejected as structurally invalid or changes the
  // immutable projection; an excluded descriptor must still pass decoding.
  for (std::size_t byte = 0; byte < operand.canonical_value_body.size(); ++byte) {
    auto changed = operand;
    changed.canonical_value_body[byte] ^= 1;
    const auto observed = parser::FreezeContextualOperandsV3({changed}, {});
    Require(!observed || observed != projection, "freeze ignored changed binary descriptor byte");
    Descriptor decoded;
    const bool valid = wire::DecodeRelationalTypeDescriptorV1(
        changed.canonical_value_body.data(), changed.canonical_value_body.size(), &decoded) &&
        decoded.descriptor_id == 17;
    const auto excluded = parser::FreezeContextualOperandsV3({changed}, {17});
    Require(valid ? excluded == mutable_projection : !excluded,
            "mutable descriptor exclusion skipped structural validation");
  }
  auto changed_descriptor = descriptor;
  changed_descriptor.descriptor_uuid = Id(42);
  Require(parser::FreezeContextualOperandsV3({operand_for(changed_descriptor)}, {17}) == mutable_projection,
          "negotiated descriptor replacement changed reserved handle projection");
  for (const auto name : {"17", "slot_017", "slot_0", "slot_18", "slot_17x"}) {
    auto wrong = operand; wrong.name = name;
    Require(!parser::FreezeContextualOperandsV3({wrong}, {17}), "freeze accepted inexact descriptor slot");
  }
  for (const auto type : {"relational_descriptor_v1", "relational_descriptor_v2", "other"}) {
    auto wrong = operand; wrong.type = type;
    Require(!parser::FreezeContextualOperandsV3({wrong}, {17}), "freeze accepted wrong descriptor type");
  }
  auto wrong = operand; wrong.canonical_value_kind = 0;
  Require(!parser::FreezeContextualOperandsV3({wrong}, {17}), "freeze accepted wrong descriptor kind");
  wrong = operand; wrong.value = "shadow";
  Require(!parser::FreezeContextualOperandsV3({wrong}, {17}), "freeze accepted shadow descriptor value");
  Require(!parser::FreezeContextualOperandsV3({operand, operand}, {}), "freeze accepted duplicate immutable handle");
  Require(!parser::FreezeContextualOperandsV3({operand, operand}, {17}), "freeze accepted duplicate mutable handle");
  Require(!parser::FreezeContextualOperandsV3({operand}, {18}), "freeze accepted missing mutable handle");
  Require(!parser::FreezeContextualOperandsV3({operand}, {0}), "freeze accepted zero mutable handle");
  parser::SblrOperand generic{"literal", "value", {}};
  generic.canonical_value_kind = 4;
  generic.canonical_value_body = {0, 1, 0, 2, 0};
  const auto generic_frozen = parser::FreezeContextualOperandsV3({generic}, {});
  for (std::size_t byte = 0; byte < generic.canonical_value_body.size(); ++byte) {
    auto changed = generic; changed.canonical_value_body[byte] ^= 1;
    Require(parser::FreezeContextualOperandsV3({changed}, {}) != generic_frozen,
            "freeze ignored generic binary body byte");
  }
  auto changed = generic; ++changed.canonical_value_kind;
  Require(parser::FreezeContextualOperandsV3({changed}, {}) != generic_frozen, "freeze ignored generic kind");
  for (auto member : {&parser::SblrOperand::type, &parser::SblrOperand::name}) {
    changed = generic; (changed.*member).push_back('\0');
    Require(parser::FreezeContextualOperandsV3({changed}, {}) != generic_frozen, "freeze ignored field length");
  }
  Require(parser::FreezeContextualOperandsV3({operand, generic}, {}) !=
          parser::FreezeContextualOperandsV3({generic, operand}, {}), "freeze ignored operand order");
  parser::SblrOperand left{"ab", "c", "d"}, right{"a", "bc", "d"};
  Require(parser::FreezeContextualOperandsV3({left}, {}) != parser::FreezeContextualOperandsV3({right}, {}),
          "freeze conflated adjacent fields");
  changed = generic; changed.value = "shadow";
  Require(!parser::FreezeContextualOperandsV3({changed}, {}), "freeze accepted mixed binary and text value");
  const std::vector<parser::SblrOperand> input{operand, generic};
  const std::unordered_set<std::uint32_t> handles{17};
  const auto expected_success = parser::FreezeContextualOperandsV3(input, handles);
  std::size_t failure_sites = 0;
  for (long site = 0; site < 1000; ++site) {
    fail_after = site;
    try {
      const auto result = parser::FreezeContextualOperandsV3(input, handles);
      fail_after = -1;
      Require(result == expected_success, "allocation probe published partial frozen output");
      break;
    } catch (const std::bad_alloc&) {
      fail_after = -1; ++failure_sites; ++faults;
      Require(input[0].canonical_value_body == operand.canonical_value_body &&
              input[1].canonical_value_body == generic.canonical_value_body && handles.contains(17),
              "allocation failure changed reservation input");
    }
  }
  Require(failure_sites > 0 && failure_sites < 1000, "freeze allocation fault sweep incomplete");
}
}
namespace {
void TestReservationSnapshot() {
  namespace parser = scratchbird::parser::sbsql;
  parser::BoundStatement bound;
  bound.bound = true; bound.native_relational_recognized = true;
  bound.parser_api_major = 3; bound.protocol_version = 2;
  bound.catalog_epoch = 5; bound.security_policy_epoch = 6; bound.descriptor_epoch = 7;
  bound.parser_package_uuid = Id(1); bound.parser_package_version = "1.0";
  bound.parser_build_id = "fixture"; bound.command_registry_snapshot_uuid = Id(2);
  bound.session_uuid = Id(3); bound.connection_uuid = Id(4);
  bound.database_uuid = Id(5); bound.dialect_profile_uuid = Id(6);
  bound.resolved_object_uuids = {Id(7), Id(8)};
  auto& native = bound.native_relational;
  native.bound_ast_uuid = Id(9); native.security_context_uuid = Id(10);
  native.statement_uuid = Id(11); native.owning_transaction_uuid = Id(12);
  native.statement_snapshot_uuid = Id(13); native.statement_metadata_snapshot_uuid = Id(14);
  native.scopes.resize(1); native.scopes[0].scope_id = 1;
  native.scopes[0].catalog_epoch_uuid = Id(15);
  native.expressions.resize(1); native.expressions[0].expression_id = 1;
  native.expressions[0].bound_function_uuid = Id(16);
  native.expressions[0].bound_name_uuid = Id(17);
  native.window_invocations.resize(1);
  native.window_invocations[0].invocation_id = 1;
  native.window_invocations[0].window_definition_id = 1;
  native.window_invocations[0].bound_function_uuid = Id(18);
  native.window_definitions.resize(1); native.window_definitions[0].window_id = 1;
  native.relations.resize(1); native.relations[0].bound_object_uuid = Id(19);
  native.relations[0].relation_id = 1;
  native.relations[0].relation_kind = parser::NativeRelationAstKind::kWindow;
  native.relations[0].window_invocation_ids = {1};
  native.bound = true;
  Require(parser::FinalizeRelationalPropertyIdentities(&native), "reservation fixture property identities missing");
  native.catalog_relation_sources.resize(1);
  auto& source = native.catalog_relation_sources[0];
  source.object_uuid = Id(20); source.resolved_schema_uuid = Id(21);
  source.parent_object_uuid = Id(22); source.model_search_analyzer_uuid = Id(23);
  source.model_spatial_crs_uuids = {Id(24), Id(25)};
  source.model_columnar_project_column_uuids = {Id(26), Id(27)};
  source.columns.resize(1); source.columns[0].column_uuid = Id(28);
  auto descriptor = Fixture(0); descriptor.descriptor_id = 17;
  descriptor.collation_uuid = Id(29);
  native.descriptors.resize(1);
  auto& binding = native.descriptors[0];
  binding.descriptor_id = 17; binding.descriptor_uuid = descriptor.descriptor_uuid;
  binding.type_uuid = descriptor.type_uuid; binding.collation_uuid = descriptor.collation_uuid;
  binding.nullability = parser::BoundNullability::kNullable;
  bound.descriptor_refs = {binding.descriptor_uuid};
  parser::SblrEnvelope lowered;
  lowered.descriptor_refs = bound.descriptor_refs;
  lowered.resolved_object_uuids = {Id(30), Id(31)};
  lowered.descriptor_requirements = {"parser.local.requirement"};
  parser::SblrOperand operand{"relational_descriptor_v3", "slot_17", {}};
  operand.canonical_value_kind = 213; operand.canonical_value_body = Oracle(descriptor);
  lowered.operands.push_back(operand);
  const auto freeze = [&](const auto& b, const auto& l) {
    return parser::FreezeContextualReservationSkeletonV2(b, l, {});
  };
  const auto frozen = freeze(bound, lowered);
  Require(frozen.has_value(), "full reservation projection refused");
  Bytes prefix;
  for (const auto value : {1, 1}) Append(prefix, value, 2);
  for (const auto value : {1, 3, 2}) Append(prefix, value, 4);
  for (const auto value : {5, 6, 7}) Append(prefix, value, 8);
  prefix.insert(prefix.end(), bound.parser_package_uuid.bytes.begin(), bound.parser_package_uuid.bytes.end());
  for (const auto text : {"1.0", "fixture"}) {
    const std::string field(text); Append(prefix, field.size(), 4);
    prefix.insert(prefix.end(), field.begin(), field.end());
  }
  for (const auto id : {Id(2), Id(3), Id(4), Id(5), Id(6)})
    prefix.insert(prefix.end(), id.bytes.begin(), id.bytes.end());
  Require(frozen->size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), frozen->begin()),
          "full reservation metadata differs from independent binary prefix oracle");
  const auto changed_snapshot = [&](const auto& b, const auto& l) {
    const auto changed = freeze(b, l);
    Require(changed && changed != frozen, "full reservation lost an immutable field");
  };
  for (auto member : {&parser::BoundStatement::parser_package_uuid,
                      &parser::BoundStatement::command_registry_snapshot_uuid,
                      &parser::BoundStatement::session_uuid, &parser::BoundStatement::connection_uuid,
                      &parser::BoundStatement::database_uuid, &parser::BoundStatement::dialect_profile_uuid}) {
    for (std::size_t byte = 0; byte < 16; ++byte) {
      auto copy = bound; (copy.*member).bytes[byte] ^= 1; changed_snapshot(copy, lowered);
    }
  }
  for (auto member : {&parser::BoundNativeRelationalDocument::bound_ast_uuid,
                      &parser::BoundNativeRelationalDocument::security_context_uuid,
                      &parser::BoundNativeRelationalDocument::statement_uuid,
                      &parser::BoundNativeRelationalDocument::owning_transaction_uuid,
                      &parser::BoundNativeRelationalDocument::statement_snapshot_uuid,
                      &parser::BoundNativeRelationalDocument::statement_metadata_snapshot_uuid}) {
    for (std::size_t byte = 0; byte < 16; ++byte) {
      auto copy = bound; (copy.native_relational.*member).bytes[byte] ^= 1; changed_snapshot(copy, lowered);
    }
  }
  for (auto member : {&parser::BoundDescriptorAstRecord::type_uuid,
                      &parser::BoundDescriptorAstRecord::statement_receipt_uuid,
                      &parser::BoundDescriptorAstRecord::datatype_catalog_snapshot_uuid}) {
    for (std::size_t byte = 0; byte < 16; ++byte) {
      auto copy = bound; (copy.native_relational.descriptors[0].*member).bytes[byte] ^= 1;
      changed_snapshot(copy, lowered);
    }
  }
  // This is equality material, not identity admission: every raw bit,
  // including variant/version bits, remains visible without text conversion.
  for (std::size_t byte = 0; byte < 16; ++byte) {
    auto copy = bound; copy.native_relational.descriptors[0].descriptor_uuid.bytes[byte] ^= 1;
    auto l = lowered; copy.descriptor_refs[0] = copy.native_relational.descriptors[0].descriptor_uuid;
    l.descriptor_refs = copy.descriptor_refs; changed_snapshot(copy, l);
    copy = bound; copy.native_relational.scopes[0].catalog_epoch_uuid.bytes[byte] ^= 1; changed_snapshot(copy, lowered);
    copy = bound; copy.native_relational.window_invocations[0].bound_function_uuid.bytes[byte] ^= 1;
    changed_snapshot(copy, lowered);
    copy = bound; copy.native_relational.catalog_relation_sources[0].columns[0].column_uuid.bytes[byte] ^= 1;
    changed_snapshot(copy, lowered);
  }
  for (auto member : {&parser::BoundCatalogRelationSourceAstRecord::object_uuid,
                      &parser::BoundCatalogRelationSourceAstRecord::resolved_schema_uuid,
                      &parser::BoundCatalogRelationSourceAstRecord::model_search_analyzer_uuid}) {
    for (std::size_t byte = 0; byte < 16; ++byte) {
      auto copy = bound; (copy.native_relational.catalog_relation_sources[0].*member).bytes[byte] ^= 1;
      changed_snapshot(copy, lowered);
    }
  }
  const auto optional_check = [&](auto access) {
    for (std::size_t byte = 0; byte < 16; ++byte) {
      auto copy = bound; access(copy)->bytes[byte] ^= 1; changed_snapshot(copy, lowered);
    }
    auto absent = bound; access(absent).reset(); changed_snapshot(absent, lowered);
    auto present_nil = bound; access(present_nil) = Uuid{};
    Require(freeze(absent, lowered) != freeze(present_nil, lowered), "optional nil conflated with absence");
  };
  optional_check([](auto& b) -> auto& { return b.native_relational.descriptors[0].collation_uuid; });
  optional_check([](auto& b) -> auto& { return b.native_relational.expressions[0].bound_function_uuid; });
  optional_check([](auto& b) -> auto& { return b.native_relational.expressions[0].bound_name_uuid; });
  optional_check([](auto& b) -> auto& { return b.native_relational.relations[0].bound_object_uuid; });
  optional_check([](auto& b) -> auto& { return b.native_relational.catalog_relation_sources[0].parent_object_uuid; });
  const auto vector_check = [&](auto access) {
    for (std::size_t index = 0; index < 2; ++index) for (std::size_t byte = 0; byte < 16; ++byte) {
      auto copy = bound; access(copy)[index].bytes[byte] ^= 1; changed_snapshot(copy, lowered);
    }
    auto copy = bound; std::swap(access(copy)[0], access(copy)[1]); changed_snapshot(copy, lowered);
    copy = bound; access(copy).pop_back(); changed_snapshot(copy, lowered);
  };
  vector_check([](auto& b) -> auto& { return b.resolved_object_uuids; });
  vector_check([](auto& b) -> auto& { return b.native_relational.catalog_relation_sources[0].model_spatial_crs_uuids; });
  vector_check([](auto& b) -> auto& { return b.native_relational.catalog_relation_sources[0].model_columnar_project_column_uuids; });
  for (std::size_t index = 0; index < 2; ++index) for (std::size_t byte = 0; byte < 16; ++byte) {
    auto l = lowered; l.resolved_object_uuids[index].bytes[byte] ^= 1; changed_snapshot(bound, l);
  }
  auto l = lowered; l.descriptor_requirements[0] += ".changed"; changed_snapshot(bound, l);
  auto copy = bound; copy.native_relational.scopes.clear(); changed_snapshot(copy, lowered);
  copy = bound; copy.descriptor_refs[0] = Id(50);
  Require(!freeze(copy, lowered), "full reservation accepted crossed bound descriptor reference");
  l = lowered; l.descriptor_refs[0] = Id(50);
  Require(!freeze(bound, l), "full reservation accepted crossed lowered descriptor reference");
  copy = bound; copy.native_relational.descriptors.push_back(binding); copy.descriptor_refs.push_back(binding.descriptor_uuid);
  l = lowered; l.descriptor_refs = copy.descriptor_refs;
  Require(!freeze(copy, l), "full reservation accepted duplicate descriptor handles");
  const std::unordered_set<std::uint32_t> mutable_handles{17};
  const auto reserved = parser::FreezeContextualReservationSkeletonV2(bound, lowered, mutable_handles);
  copy = bound; copy.native_relational.descriptors[0].descriptor_uuid = Id(50);
  copy.descriptor_refs[0] = Id(50); l = lowered; l.descriptor_refs = copy.descriptor_refs;
  auto negotiated = descriptor; negotiated.descriptor_uuid = Id(50);
  l.operands[0].canonical_value_body = Oracle(negotiated);
  Require(reserved && parser::FreezeContextualReservationSkeletonV2(copy, l, mutable_handles) == reserved,
          "full reservation rejected exact contextual descriptor replacement");
  copy.native_relational.statement_uuid = Id(51);
  Require(parser::FreezeContextualReservationSkeletonV2(copy, l, mutable_handles) != reserved,
          "contextual exclusion escaped into statement identity");
  std::size_t failure_sites = 0;
  for (long site = 0; site < 1000; ++site) {
    fail_after = site;
    try {
      const auto result = freeze(bound, lowered); fail_after = -1;
      Require(result == frozen, "full reservation published partial allocation result"); break;
    } catch (const std::bad_alloc&) {
      fail_after = -1; ++faults; ++failure_sites;
      Require(freeze(bound, lowered) == frozen, "full reservation fault changed input state");
    }
  }
  Require(failure_sites > 0 && failure_sites < 1000, "full reservation allocation sweep incomplete");
}
}
namespace {
void TestNativeDescriptorBinaryKeys() {
  namespace parser = scratchbird::parser::sbsql;
  namespace dt = scratchbird::core::datatypes;
  static_assert(std::is_same_v<parser::NativeDescriptorIdentityKey, std::pair<Uuid, Uuid>>);
  std::map<parser::NativeDescriptorIdentityKey, std::uint32_t> cache;
  for (unsigned descriptor = 1; descriptor <= 16; ++descriptor)
    for (unsigned type = 1; type <= 16; ++type)
      Require(cache.emplace(parser::NativeDescriptorIdentityKey{Id(descriptor), Id(type)},
                            descriptor * 16 + type).second, "binary descriptor key merged distinct roles");
  Require(cache.size() == 256, "binary descriptor cache lost keys");
  for (unsigned descriptor = 1; descriptor <= 16; ++descriptor) for (unsigned type = 1; type <= 16; ++type) {
    const parser::NativeDescriptorIdentityKey key{Id(descriptor), Id(type)};
    fail_after = 0; const auto found = cache.find(key); fail_after = -1;
    Require(found != cache.end() && found->second == descriptor * 16 + type, "binary descriptor lookup allocated or selected wrong identity");
  }
  const auto original_size = cache.size(); bool threw = false; fail_after = 0;
  try { cache.emplace(parser::NativeDescriptorIdentityKey{Id(99), Id(100)}, 1); } catch (const std::bad_alloc&) { threw = true; }
  fail_after = -1;
  Require(threw && cache.size() == original_size && !cache.contains({Id(99), Id(100)}), "binary descriptor cache insertion fault published entry"); ++faults;
  const auto manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
  Require(manifest.ok(), "canonical datatype manifest unavailable");
  for (auto type : {dt::CanonicalTypeId::uuid, dt::CanonicalTypeId::uint64, dt::CanonicalTypeId::real64,
                   dt::CanonicalTypeId::boolean, dt::CanonicalTypeId::geometry, dt::CanonicalTypeId::int64}) {
    const auto row = std::find_if(manifest.manifest.descriptor_rows.begin(), manifest.manifest.descriptor_rows.end(),
                                 [type](const auto& value) { return value.type_id == type; });
    Require(row != manifest.manifest.descriptor_rows.end(), "expected catalog type row absent");
    const auto registry = dt::CurrentDatatypeTypeCodecIdentityRowsV1();
    const auto binding = std::find_if(registry.begin(), registry.end(), [&](const auto& candidate) {
      return candidate.descriptor_uuid == row->descriptor_uuid.value && candidate.descriptor_generation == row->descriptor_epoch;
    });
    const auto actual = parser::LookupNativeCanonicalTypeIdentity(manifest.manifest, type);
    Require(binding == registry.end() ? !actual : actual == binding->type_uuid,
            "native type lookup confused the descriptor identity with its registered type");
  }
  Require(!parser::LookupNativeCanonicalTypeIdentity(manifest.manifest, dt::CanonicalTypeId::unknown),
          "native type lookup guessed unknown type");
  Require(!parser::LookupNativeCanonicalTypeIdentity({}, dt::CanonicalTypeId::uuid),
          "native type lookup ignored missing manifest");
  auto invalid = manifest.manifest; invalid.descriptor_rows.push_back(invalid.descriptor_rows.front());
  Require(!parser::LookupNativeCanonicalTypeIdentity(invalid, dt::CanonicalTypeId::uuid),
          "native type lookup accepted duplicate manifest rows");
  invalid = manifest.manifest;
  const auto row = std::find_if(invalid.descriptor_rows.begin(), invalid.descriptor_rows.end(),
                               [](const auto& value) { return value.type_id == dt::CanonicalTypeId::uuid; });
  row->descriptor_uuid.value = {};
  Require(!parser::LookupNativeCanonicalTypeIdentity(invalid, dt::CanonicalTypeId::uuid), "native type lookup accepted nil identity");
}

void TestEngineIssuedFunctionIdentities() {
  namespace parser = scratchbird::parser::sbsql;
  scratchbird::parser::ipc::ParserStatementContext context;
  const auto lookup = [&](std::string_view name) {
    fail_after = 0;
    const auto result = parser::EngineIssuedAggregateFunctionUuid(context, name);
    fail_after = -1;
    return result;
  };
  Require(!lookup("SUM"), "missing aggregate profile silently manufactured identity");
  context.aggregate_function_profiles = {
      {1, "sb.aggregate.sum", Id(17), true},
      {1, "sb.aggregate.count", Id(18), true},
      {1, "sb.aggregate.bool_and", Id(19), true},
      {1, "sb.aggregate.regr_sxy", Id(20), true}};
  for (auto name : {"SUM", "sum", "SuM"})
    Require(lookup(name) == Id(17), "aggregate binding changed engine-issued binary UUID");
  Require(lookup("COUNT") == Id(18) && lookup("Bool_And") == Id(19) && lookup("REGR_SXY") == Id(20),
          "aggregate role selected a different profile");
  for (auto name : {"", "SUM ", " SUM", "SUM()", "sb.aggregate.sum", "UNKNOWN", "COUNT_STAR"})
    Require(!lookup(name), "aggregate lookup guessed an undeclared alias or builtin");
  Require(!lookup(std::string_view("SUM\0", 4)), "aggregate lookup ignored trailing NUL");
  const auto valid = context.aggregate_function_profiles;
  for (unsigned version = 0; version <= 15; ++version) if (version != 7) {
    context.aggregate_function_profiles = valid;
    context.aggregate_function_profiles[0].function_uuid.bytes[6] = version << 4;
    Require(!lookup("SUM"), "aggregate accepted non-system UUID version");
  }
  context.aggregate_function_profiles = valid;
  context.aggregate_function_profiles[0].function_uuid = {};
  Require(!lookup("SUM"), "aggregate accepted nil identity");
  for (unsigned variant : {0u, 0x40u, 0xc0u, 0xe0u}) {
    context.aggregate_function_profiles = valid;
    context.aggregate_function_profiles[0].function_uuid.bytes[8] = variant;
    Require(!lookup("SUM"), "aggregate accepted non-RFC variant");
  }
  context.aggregate_function_profiles = valid;
  context.aggregate_function_profiles[0].abi_version = 2;
  Require(!lookup("SUM"), "aggregate silently accepted unsupported ABI");
  context.aggregate_function_profiles = valid;
  context.aggregate_function_profiles[0].executable = false;
  Require(!lookup("SUM"), "aggregate advertised unavailable implementation");
  for (bool conflicting : {false, true}) {
    context.aggregate_function_profiles = valid;
    auto duplicate = valid[0]; if (conflicting) duplicate.function_uuid = Id(99);
    context.aggregate_function_profiles.push_back(duplicate);
    Require(!lookup("SUM"), "aggregate ambiguous engine profile selected first match");
    context.aggregate_function_profiles.front().executable = false;
    Require(!lookup("SUM"), "aggregate invalid duplicate hidden by first executable match");
    std::reverse(context.aggregate_function_profiles.begin(), context.aggregate_function_profiles.end());
    Require(!lookup("SUM"), "aggregate ambiguity depended on profile order");
    Require(lookup("COUNT") == Id(18), "aggregate ambiguity in another role suppressed valid lookup");
  }
  context.aggregate_function_profiles = valid;
  context.aggregate_function_profiles[0].function_uuid = Id(55);
  Require(lookup("SUM") == Id(55), "aggregate identity cached or substituted a fixed UUID");
}

void TestDmlRegistryBinaryProjection() {
  namespace p = api::datatype_operator_projection;
  namespace w = scratchbird::wire;
  namespace d = scratchbird::core::datatypes;
  static_assert(noexcept(p::TypedUuid(Uuid{}, nullptr)));
  static_assert(noexcept(p::UuidValue(w::TypedUpdateUuid{})));
  static_assert(std::is_nothrow_move_assignable_v<w::TypedUpdateDatatypeAuthorityRecord>);
  static_assert(std::is_nothrow_move_assignable_v<w::TypedUpdateBuiltinOperatorAuthorityRecord>);
  for (unsigned version = 0; version < 16; ++version) {
    for (unsigned variant = 0; variant < 4; ++variant) {
      auto identity = Id(37);
      identity.bytes[6] = static_cast<std::uint8_t>(version << 4);
      identity.bytes[8] = static_cast<std::uint8_t>(variant << 6);
      auto out = Id(99).bytes;
      fail_after = 0;
      const bool accepted = p::TypedUuid(identity, &out);
      const auto returned = p::UuidValue(identity.bytes);
      fail_after = -1;
      Require(accepted == (version == 7 && variant == 2), "DML registry identity shape admitted incorrectly");
      Require(out == (accepted ? identity.bytes : Id(99).bytes), "DML identity conversion partially published");
      Require(returned == identity, "DML binary wrapper changed UUID bytes");
    }
  }
  Require(!p::TypedUuid(Id(3), nullptr), "DML UUID conversion accepted null output");
  api::EngineRequestContext context;
  const auto rows = d::CurrentDatatypeTypeCodecIdentityRowsV1();
  Require(rows.size() == 18, "six predecessor plus twelve successor datatype rows");
  const auto state = [](const w::TypedUpdateDatatypeAuthorityRecord& v) {
    return std::tie(v.exact_bytes, v.datatype_ordinal, v.datatype_identity_code, v.null_encoding_code,
        v.byte_order_code, v.is_signed, v.descriptor_uuid, v.descriptor_generation,
        v.type_uuid, v.type_generation, v.codec_version, v.canonical_name, v.codec_id,
        v.representation_code, v.codec_generation, v.canonical_value_minimum_bytes,
        v.canonical_value_maximum_bytes, v.canonical_value_exact_bytes, v.datatype_snapshot_uuid,
        v.datatype_catalog_generation, v.datatype_registry_generation, v.record_evidence_sha256);
  };
  for (const auto& row : rows) {
    context.datatype_catalog_snapshot_uuid = row.catalog_snapshot_uuid;
    context.datatype_catalog_generation = row.catalog_generation;
    context.datatype_registry_generation = row.registry_generation;
    p::DatatypeReference reference{row.descriptor_uuid.bytes, row.descriptor_generation,
        row.type_uuid.bytes, row.type_generation, row.codec_id, row.codec_version, row.codec_generation};
    w::TypedUpdateDatatypeAuthorityRecord out;
    if (row.datatype_identity_code == 0 && row.canonical_name != "text") {
      Require(!p::BuildDatatypeRecord(context, reference, &out),
              "new query codecs must not invent a closed DUDR datatype code");
      continue;
    }
    Require(p::BuildDatatypeRecord(context, reference, &out), "live binary datatype projection failed");
    Require(out.descriptor_uuid == reference.descriptor_uuid && out.type_uuid == reference.type_uuid &&
        out.datatype_snapshot_uuid == context.datatype_catalog_snapshot_uuid.bytes &&
        out.codec_id == reference.codec_id && out.codec_generation == reference.codec_generation &&
        out.descriptor_generation == reference.descriptor_generation && out.type_generation == reference.type_generation,
        "datatype projection lost exact registry binding");
    if (row.canonical_name == "text") {
      Require(static_cast<unsigned>(out.datatype_identity_code) == 6 &&
          static_cast<unsigned>(out.byte_order_code) == 3 && static_cast<unsigned>(out.representation_code) == 4 &&
          out.canonical_value_minimum_bytes == 0 && out.canonical_value_exact_bytes == 0 &&
          out.canonical_value_maximum_bytes == 16777216, "TEXT v2 Core projection differs from oracle");
    } else {
      Require(static_cast<unsigned>(out.datatype_identity_code) == row.datatype_identity_code &&
          out.canonical_value_exact_bytes == row.canonical_value_bytes, "fixed-width projection changed semantics");
    }
    out.descriptor_uuid = Id(99).bytes;
    out.type_uuid = Id(98).bytes;
    out.canonical_name = "unpublished caller sentinel";
    out.codec_id = "unpublished codec sentinel";
    out.exact_bytes = {1, 2, 3, 4};
    const auto saved = out;
    for (unsigned mutation = 0; mutation < 10; ++mutation) {
      auto changed = reference; auto changed_context = context;
      switch (mutation) {
        case 0: changed.descriptor_uuid[15] ^= 0x80; break;
        case 1: ++changed.descriptor_generation; break;
        case 2: changed.type_uuid[15] ^= 0x80; break;
        case 3: ++changed.type_generation; break;
        case 4: changed.codec_id += ".forged"; break;
        case 5: ++changed.codec_version; break;
        case 6: ++changed.codec_generation; break;
        case 7: changed_context.datatype_catalog_snapshot_uuid.bytes[15] ^= 0x80; break;
        case 8: ++changed_context.datatype_catalog_generation; break;
        case 9: ++changed_context.datatype_registry_generation; break;
      }
      Require(!p::BuildDatatypeRecord(changed_context, changed, &out) && state(out) == state(saved),
          "failed datatype projection published or accepted changed authority");
    }
    w::TypedUpdateDatatypeAuthorityRecord fresh;
    fresh.descriptor_uuid = Id(99).bytes;
    fresh.type_uuid = Id(98).bytes;
    const auto empty_saved = fresh;
    const auto before = allocations;
    Require(p::BuildDatatypeRecord(context, reference, &fresh), "projection fault baseline");
    const auto sites = allocations - before;
    for (std::size_t site = 0; site < sites; ++site) {
      auto fault_out = empty_saved; bool threw = false; fail_after = site;
      try { (void)p::BuildDatatypeRecord(context, reference, &fault_out); }
      catch (const std::bad_alloc&) { threw = true; }
      fail_after = -1;
      Require(threw && state(fault_out) == state(empty_saved), "datatype projection fault changed caller output"); ++faults;
    }
  }
  const auto live = d::LoadCurrentBuiltinOperatorRegistrySnapshotIdentityV1();
  Require(live.ok, "live operator snapshot unavailable");
  w::TypedUpdateDescriptorCarrier descriptor;
  descriptor.builtin_operator_snapshot_uuid = live.snapshot_uuid.bytes;
  descriptor.builtin_operator_registry_generation = live.registry_generation;
  w::TypedUpdatePredicateVector predicate;
  predicate.records.resize(3);
  for (unsigned i = 0; i < 2; ++i) {
    auto& node = predicate.records[i];
    node.output_descriptor_uuid = rows[0].descriptor_uuid.bytes;
    node.output_descriptor_generation = rows[0].descriptor_generation;
    node.output_type_uuid = rows[0].type_uuid.bytes;
    node.output_type_generation = rows[0].type_generation;
  }
  predicate.records[2].operator_uuid = live.equality_operator_uuid.bytes;
  predicate.records[2].operator_generation = live.equality_operator_generation;
  const auto operator_state = [](const w::TypedUpdateBuiltinOperatorAuthorityRecord& v) {
    return std::tie(v.exact_bytes, v.operator_ordinal, v.semantic_code, v.operand_arity,
        v.null_behavior_code, v.accepted_state, v.operator_uuid, v.operator_generation,
        v.operator_snapshot_uuid, v.operator_registry_generation, v.left_descriptor_uuid,
        v.left_descriptor_generation, v.left_type_uuid, v.left_type_generation,
        v.right_descriptor_uuid, v.right_descriptor_generation, v.right_type_uuid,
        v.right_type_generation, v.result_descriptor_uuid, v.result_descriptor_generation,
        v.result_type_uuid, v.result_type_generation, v.result_codec_version, v.operator_family_code,
        v.result_codec_generation, v.result_codec_id, v.operand_identity_rule,
        v.result_null_encoding_code, v.record_evidence_sha256);
  };
  w::TypedUpdateBuiltinOperatorAuthorityRecord operation;
  Require(p::BuildOperatorRecord(descriptor, predicate, &operation), "binary operator projection failed");
  Require(operation.operator_uuid == live.equality_operator_uuid.bytes && operation.operator_ordinal == 1 &&
      operation.left_descriptor_uuid == rows[0].descriptor_uuid.bytes &&
      operation.right_descriptor_uuid == rows[0].descriptor_uuid.bytes &&
      operation.result_descriptor_uuid == rows[4].descriptor_uuid.bytes &&
      operation.result_type_uuid == rows[4].type_uuid.bytes && operation.result_codec_id == "datatype.boolean.u8.v1",
      "equality projection changed exact operand/result bindings");
  operation.operator_uuid = Id(99).bytes;
  operation.result_codec_id = "unpublished caller operator";
  operation.exact_bytes = {4, 3, 2, 1};
  const auto saved_operation = operation;
  for (unsigned mutation = 0; mutation < 10; ++mutation) {
    auto changed = predicate; auto changed_descriptor = descriptor;
    switch (mutation) {
      case 0: changed_descriptor.builtin_operator_snapshot_uuid[15] ^= 0x80; break;
      case 1: ++changed_descriptor.builtin_operator_registry_generation; break;
      case 2: changed.records[2].operator_uuid[15] ^= 0x80; break;
      case 3: ++changed.records[2].operator_generation; break;
      case 4: changed.records[0].output_descriptor_uuid[15] ^= 0x80; break;
      case 5: ++changed.records[0].output_descriptor_generation; break;
      case 6: changed.records[1].output_type_uuid[15] ^= 0x80; break;
      case 7: ++changed.records[1].output_type_generation; break;
      case 8: changed.records.pop_back(); break;
      case 9: changed.records.push_back(changed.records[2]); break;
    }
    Require(!p::BuildOperatorRecord(changed_descriptor, changed, &operation) &&
        operator_state(operation) == operator_state(saved_operation), "operator projection partially published changed binding");
  }
  w::TypedUpdateBuiltinOperatorAuthorityRecord fresh;
  fresh.operator_uuid = Id(99).bytes;
  const auto empty_saved = fresh;
  const auto before = allocations;
  Require(p::BuildOperatorRecord(descriptor, predicate, &fresh), "operator fault baseline");
  const auto sites = allocations - before;
  for (std::size_t site = 0; site < sites; ++site) {
    auto fault_out = empty_saved; bool threw = false; fail_after = site;
    try { (void)p::BuildOperatorRecord(descriptor, predicate, &fault_out); }
    catch (const std::bad_alloc&) { threw = true; }
    fail_after = -1;
    Require(threw && operator_state(fault_out) == operator_state(empty_saved), "operator projection fault changed caller output"); ++faults;
  }
}

void TestDmlIdentityOperands() {
  namespace parser = scratchbird::parser::sbsql;
  constexpr std::array<std::string_view, 12> roles{
      "target_object_uuid", "source_uuid", "routine_object_uuid",
      "insert_select_source_uuid_0", "insert_select_source_uuid_1", "target_schema_uuid",
      "grantee_uuid", "member_principal_uuid", "container_uuid",
      "principal_uuid", "policy_uuid", "role_uuid"};
  for (const auto role : roles) {
    const auto identity = Id(37);
    const auto operand = parser::MakeDmlObjectIdentityOperand(role, identity);
    Require(operand && operand->name == role && operand->type == "uuid" && operand->value.empty() &&
            operand->canonical_value_kind == 1 &&
            operand->canonical_value_body == Bytes(identity.bytes.begin(), identity.bytes.end()),
            "DML identity is not exact raw16 uuid_ref");
    parser::SblrEnvelope envelope; envelope.resolved_object_uuids = {identity}; envelope.operands = {*operand};
    Uuid decoded = Id(99); fail_after = 0;
    const bool decoded_ok = parser::DecodeDmlObjectIdentityOperand(*operand, &decoded);
    const auto found = parser::FindDmlObjectIdentity(envelope, role);
    fail_after = -1;
    Require(decoded_ok && decoded == identity && found == identity, "DML bound identity lookup allocated or lost bytes");
    const auto reject = [&](const parser::SblrOperand& bad) {
      decoded = Id(99); envelope.operands = {bad};
      Require(!parser::DecodeDmlObjectIdentityOperand(bad, &decoded) && decoded == Id(99), "DML decode published malformed identity");
      Require(!parser::FindDmlObjectIdentity(envelope, role), "DML malformed identity became binding authority");
    };
    auto bad = *operand; bad.type = "text"; reject(bad);
    bad = *operand; bad.value = "01a07c0a-0d5c-7000-8000-ff0000000025"; reject(bad);
    bad = *operand; bad.canonical_value_kind = 4; reject(bad);
    bad = *operand; bad.canonical_value_body.assign(16, 0); reject(bad);
    for (unsigned version = 0; version <= 15; ++version) if (version != 7) {
      bad = *operand; bad.canonical_value_body[6] = version << 4; reject(bad);
    }
    for (std::size_t length = 0; length < 16; ++length) {
      bad = *operand; bad.canonical_value_body.resize(length); reject(bad);
    }
    bad = *operand; bad.canonical_value_body.push_back(0); reject(bad);
    envelope.operands = {*operand, *operand};
    Require(!parser::FindDmlObjectIdentity(envelope, role), "DML duplicate identity slot accepted");
    envelope.operands.back() = *parser::MakeDmlObjectIdentityOperand(role, Id(38));
    envelope.resolved_object_uuids.push_back(Id(38));
    Require(!parser::FindDmlObjectIdentity(envelope, role), "DML conflicting identity slot accepted");
    envelope.operands = {*operand}; envelope.resolved_object_uuids = {Id(38)};
    Require(!parser::FindDmlObjectIdentity(envelope, role), "DML unbound identity accepted");
    envelope.operands.clear(); envelope.payload = "{\"target_object_uuid\":\"01a07c0a-0d5c-7000-8000-ff0000000025\"}";
    Require(!parser::FindDmlObjectIdentity(envelope, role), "DML identity reconstructed from JSON");
    const auto before = allocations;
    Require(parser::MakeDmlObjectIdentityOperand(role, identity).has_value(), "DML construction allocation baseline");
    const auto sites = allocations - before;
    for (std::size_t site = 0; site < sites; ++site) {
      bool threw = false; fail_after = site;
      try { (void)parser::MakeDmlObjectIdentityOperand(role, identity); } catch (const std::bad_alloc&) { threw = true; }
      fail_after = -1; Require(threw, "DML construction fault silently admitted"); ++faults;
    }
  }
  Require(!parser::MakeDmlObjectIdentityOperand("unrecognized", Id(37)), "unrecognized DML role admitted");
  Require(!parser::MakeDmlObjectIdentityOperand(roles[0], Uuid{}), "nil DML object admitted");
}

void TestSingleDdlTargetIdentity() {
  namespace parser = scratchbird::parser::sbsql;
  const auto prior = Id(99);
  const std::array<Uuid, 1> resolved{{Id(81)}};
  Uuid target = prior;
  fail_after = 0;
  const bool admitted = parser::BindSingleDdlTargetIdentity(resolved, &target);
  fail_after = -1;
  Require(admitted && target == resolved.front(), "DDL target binding lost binary identity or allocated");
  const auto reject = [&](std::span<const Uuid> values) {
    target = prior;
    fail_after = 0;
    const bool accepted = parser::BindSingleDdlTargetIdentity(values, &target);
    fail_after = -1;
    Require(!accepted && target == prior, "invalid DDL target binding published authority");
  };
  reject({});
  std::array<Uuid, 2> ambiguous{{resolved.front(), Id(82)}};
  reject(ambiguous);
  ambiguous.back() = ambiguous.front(); reject(ambiguous);
  std::array<Uuid, 1> bad{{{}}}; reject(bad);
  for (unsigned version = 0; version != 16; ++version) {
    if (version == 7) continue;
    bad = resolved;
    bad.front().bytes[6] = static_cast<std::uint8_t>(version << 4);
    reject(bad);
  }
  for (const unsigned variant : {0x00, 0x40, 0xc0, 0xe0}) {
    bad = resolved;
    bad.front().bytes[8] = static_cast<std::uint8_t>(variant);
    reject(bad);
  }
  Require(!parser::BindSingleDdlTargetIdentity(resolved, nullptr), "null DDL identity output admitted");
  using Requirement = parser::BoundTargetRequirement;
  target = prior;
  fail_after = 0;
  const bool no_target = parser::BindOperationTargetIdentity({}, Requirement::none, &target);
  fail_after = -1;
  Require(no_target && target.is_nil(), "explicit target absence failed or retained stale authority");
  for (const auto requirement : {Requirement::none, static_cast<Requirement>(99)}) {
    target = prior;
    fail_after = 0;
    const bool accepted = parser::BindOperationTargetIdentity(resolved, requirement, &target);
    fail_after = -1;
    Require(!accepted && target == prior, "operation adopted an unrelated target or invalid requirement");
  }
  target = prior;
  fail_after = 0;
  const bool invalid_empty = parser::BindOperationTargetIdentity({}, static_cast<Requirement>(99), &target);
  const bool missing_output = parser::BindOperationTargetIdentity({}, Requirement::none, nullptr);
  fail_after = -1;
  Require(!invalid_empty && target == prior, "unknown target requirement cleared authority");
  Require(!missing_output, "target absence admitted null publication output");
  std::array<Uuid, 71> cohort;
  for (std::size_t i = 0; i < cohort.size(); ++i) cohort[i] = Id(static_cast<unsigned>(i + 1));
  cohort.back() = cohort[1];  // repeated dependency occurrences retain their position
  std::span<const Uuid> dependencies = resolved;
  for (const std::size_t count : {std::size_t{1}, std::size_t{2}, cohort.size()}) {
    target = prior;
    fail_after = 0;
    const bool bound = parser::BindTargetWithRelatedIdentities(
        std::span<const Uuid>(cohort).first(count), &target, &dependencies);
    fail_after = -1;
    Require(bound && target == cohort.front() && dependencies.data() == cohort.data() + 1 &&
                dependencies.size() == count - 1,
            "target/dependency binding allocated, reordered, copied, or truncated its cohort");
  }
  const auto reject_cohort = [&](std::span<const Uuid> values) {
    target = prior; dependencies = resolved;
    fail_after = 0;
    const bool bound = parser::BindTargetWithRelatedIdentities(values, &target, &dependencies);
    fail_after = -1;
    Require(!bound && target == prior && dependencies.data() == resolved.data() &&
                dependencies.size() == resolved.size(), "invalid dependency cohort published a prefix");
  };
  reject_cohort({});
  auto invalid_cohort = cohort; invalid_cohort.front() = {}; reject_cohort(invalid_cohort);
  invalid_cohort = cohort; invalid_cohort.back() = {}; reject_cohort(invalid_cohort);
  invalid_cohort = cohort; invalid_cohort.back().bytes[6] = 0x40; reject_cohort(invalid_cohort);
  invalid_cohort = cohort; invalid_cohort.back().bytes[8] = 0xc0; reject_cohort(invalid_cohort);
  target = prior; dependencies = resolved;
  fail_after = 0;
  const bool missing_target = parser::BindTargetWithRelatedIdentities(cohort, nullptr, &dependencies);
  const bool missing_dependencies = parser::BindTargetWithRelatedIdentities(cohort, &target, nullptr);
  fail_after = -1;
  Require(!missing_target && dependencies.data() == resolved.data() && dependencies.size() == 1,
          "null target altered dependency output");
  Require(!missing_dependencies && target == prior, "null dependencies altered target output");
  parser::SblrEnvelope lowered;
  lowered.resolved_object_uuids.assign(resolved.begin(), resolved.end());
  const std::array<std::pair<std::string_view, Uuid>, 1> bindings{{
      {"target_object_uuid", resolved.front()}}};
  Require(parser::AppendBoundDmlObjectIdentities(&lowered, bindings) &&
              lowered.operands.size() == 1 && lowered.operands.front().value.empty() &&
              parser::FindDmlObjectIdentity(lowered, "target_object_uuid") == resolved.front() &&
              !parser::FindDmlObjectIdentity(lowered, "target_schema_uuid"),
          "single DDL target publication manufactured schema or text authority");
  for (const auto alias : {"index_target_uuid", "statistics_target_uuid", "target_table_uuid",
                          "index_object_uuid", "index_template_object_uuid", "object_uuid", "schema_uuid",
                          "function_object_uuid", "procedure_object_uuid", "trigger_object_uuid",
                          "target_filespace_uuid", "owner_object_uuid", "grant_uuid", "membership_uuid"}) {
    auto shadow = lowered;
    auto operand = shadow.operands.front();
    operand.name = alias;
    shadow.operands.push_back(std::move(operand));
    fail_after = 0;
    const bool accepted = parser::ValidateBoundObjectIdentityOperands(shadow);
    fail_after = -1;
    Require(!accepted, "parser admitted a legacy catalog identity alias beside binary authority");
  }
}

void TestProjectionFunctionIdentityTransport() {
  namespace parser = scratchbird::parser::sbsql;
  for (const auto role : {"projection_0_function_uuid", "projection_17_arg_2_arg_0_function_uuid"}) {
    std::string_view path = "unchanged";
    fail_after = 0;
    const bool decoded = wire::DecodeProjectionFunctionIdentityPath(role, &path);
    fail_after = -1;
    Require(decoded && path == std::string_view(role).substr(0, std::string_view(role).size() - 13),
            "projection function path lost occurrence or allocated");
  }
  for (const auto role : {"projection_function_uuid", "projection_00_function_uuid",
      "projection_-1_function_uuid", "projection_0_arg__function_uuid",
      "projection_0_arg_01_function_uuid", "projection_0_child_1_function_uuid",
      "projection_0_arg_1function_uuid", "projection_184467440737095516160_function_uuid"}) {
    std::string_view path = "unchanged";
    Require(!wire::DecodeProjectionFunctionIdentityPath(role, &path) && path == "unchanged",
            "malformed function occurrence path admitted or changed output");
    parser::SblrEnvelope bad;
    parser::SblrOperand operand; operand.name = role;
    bad.operands.push_back(operand);
    Require(!parser::ValidateBoundObjectIdentityOperands(bad), "malformed function role escaped parser verification");
  }
  parser::SblrEnvelope lowered;
  lowered.resolved_object_uuids = {Id(11), Id(12), Id(13)};
  const std::array<std::pair<std::string_view, Uuid>, 3> bindings{{
      {"target_object_uuid", Id(11)}, {"projection_0_function_uuid", Id(12)},
      {"projection_0_arg_0_function_uuid", Id(13)}}};
  Require(parser::AppendBoundDmlObjectIdentities(&lowered, bindings) &&
              parser::ValidateBoundObjectIdentityOperands(lowered), "function binary publication failed");
  auto envelope = wire::MakeSblrEnvelope("query.evaluate_projection", "SBLR_QUERY_EVALUATE_PROJECTION", "function.identity.fixture");
  envelope.opcode_code = 0x040e; envelope.parser_package_uuid = Id(6); envelope.registry_snapshot_uuid = Id(7);
  envelope.result_shape = "scalar_result"; envelope.diagnostic_shape = "diagnostic_vector";
  for (const auto& source : lowered.operands) {
    wire::SblrOperand operand;
    operand.ordinal = static_cast<std::uint32_t>(envelope.operands.size() + 1);
    operand.type = source.type; operand.name = source.name;
    operand.value_kind = static_cast<wire::SblrValueKind>(source.canonical_value_kind);
    operand.value_body = source.canonical_value_body;
    envelope.operands.push_back(std::move(operand));
  }
  const auto bytes = wire::EncodeSblrEnvelope(envelope);
  const auto decoded = wire::DecodeSblrEnvelope(bytes);
  Require(!bytes.empty() && decoded.ok, "function identities lost during SBOP transport");
  api::EngineApiRequest request;
  allocations = 0;
  Require(wire::ProjectSblrBoundTargetIdentity(decoded.envelope, &request), "function engine projection failed");
  const auto sites = allocations;
  const std::vector<std::pair<std::string, Uuid>> expected{{"projection_0_", Id(12)}, {"projection_0_arg_0_", Id(13)}};
  Require(request.target_object.uuid == Id(11) && request.projection.function_identities == expected,
          "engine lost binary function occurrence authority");
  for (std::size_t site = 0; site < sites; ++site) {
    api::EngineApiRequest failed;
    wire::SblrIdentityProjectionFailure failure;
    fail_after = site;
    const bool ok = wire::ProjectSblrBoundTargetIdentity(decoded.envelope, &failed, &failure);
    fail_after = -1;
    Require(!ok && failure == wire::SblrIdentityProjectionFailure::allocation_failed &&
                failed.target_object.uuid.is_nil() && failed.projection.function_identities.empty(),
            "function projection allocation fault published partial target/bindings");
    ++faults;
  }
  const auto reject = [&](const auto& bad) {
    api::EngineApiRequest previous;
    Require(!wire::ProjectSblrBoundTargetIdentity(bad, &previous) &&
                previous.target_object.uuid.is_nil() && previous.projection.function_identities.empty(),
            "malformed function identity published partial output");
  };
  for (std::size_t slot = 1; slot != 3; ++slot) {
    auto bad = envelope; bad.operands[slot].value_body.assign(16, 0); reject(bad);
    bad = envelope; bad.operands[slot].value_body.resize(15); reject(bad);
    bad = envelope; bad.operands[slot].value_body.push_back(0); reject(bad);
    bad = envelope; bad.operands[slot].value_flags = 1; reject(bad);
    bad = envelope; bad.operands[slot].value = "text identity"; reject(bad);
    bad = envelope; bad.operands[slot].ordinal = 0; reject(bad);
    bad = envelope; bad.operands[slot].type = "text"; reject(bad);
    bad = envelope; bad.operands[slot].value_kind = wire::SblrValueKind::literal_typed; reject(bad);
    for (unsigned version = 0; version != 16; ++version) {
      if (version == 7) continue;
      bad = envelope; bad.operands[slot].value_body[6] = static_cast<std::uint8_t>(version << 4); reject(bad);
    }
  }
  auto bad = envelope; bad.operands.push_back(bad.operands.back()); bad.operands.back().ordinal = 4; reject(bad);
  bad = envelope; bad.operands.back().name = "projection_0_arg_00_function_uuid"; reject(bad);
  auto conflict = request; conflict.projection.function_identities[1].second = Id(99);
  const auto before = conflict.projection.function_identities;
  Require(!wire::ProjectSblrBoundTargetIdentity(envelope, &conflict) && conflict.projection.function_identities == before,
          "function identity conflict was overwritten");
  for (const auto suffix : {"", ":", ":text"}) {
    auto shadow = request; shadow.option_envelopes.push_back(std::string(bindings[1].first) + suffix);
    Require(!wire::ProjectSblrBoundTargetIdentity(envelope, &shadow) && shadow.projection.function_identities == expected,
            "function identity text option bypassed binary binding");
  }
}

void TestSecurityPolicyIdentityCohorts() {
  namespace parser = scratchbird::parser::sbsql;
  const parser::BoundPolicyObjectIdentities previous{Id(91), Id(92), Id(93), Id(94)};
  for (const bool subject : {false, true}) {
    for (const bool schema : {false, true}) {
      std::vector<Uuid> resolved{Id(11), Id(12)};
      if (subject) resolved.push_back(Id(13));
      if (schema) resolved.push_back(Id(14));
      parser::BoundPolicyObjectIdentities output = previous;
      fail_after = 0;
      const bool ok = parser::BindPolicyObjectIdentities(resolved, subject, schema, &output);
      fail_after = -1;
      Require(ok && output.policy_uuid == Id(11) && output.target_object_uuid == Id(12) &&
                  output.role_uuid == (subject ? Id(13) : Uuid{}) &&
                  output.target_schema_uuid == (schema ? Id(14) : Uuid{}),
              "policy cohort allocated, lost declared roles or retained absent authority");
      const auto reject = [&](std::span<const Uuid> bad) {
        output = previous; fail_after = 0;
        const bool accepted = parser::BindPolicyObjectIdentities(bad, subject, schema, &output);
        fail_after = -1;
        Require(!accepted && output == previous, "invalid policy cohort changed output");
      };
      for (std::size_t size = 0; size < resolved.size(); ++size)
        reject(std::span(resolved).first(size));
      auto excess = resolved; excess.push_back(Id(15)); reject(excess);
      reject(std::vector<Uuid>(70, Id(11)));
      for (std::size_t slot = 0; slot < resolved.size(); ++slot) {
        auto bad = resolved; bad[slot] = {}; reject(bad);
        for (unsigned version = 0; version < 16; ++version) {
          if (version == 7) continue;
          bad = resolved; bad[slot].bytes[6] = static_cast<std::uint8_t>(version << 4); reject(bad);
        }
        for (unsigned variant : {0u, 0x40u, 0xc0u, 0xe0u}) {
          bad = resolved; bad[slot].bytes[8] = static_cast<std::uint8_t>(variant); reject(bad);
        }
      }
      Require(!parser::BindPolicyObjectIdentities(resolved, subject, schema, nullptr),
              "null policy cohort output admitted");
      std::fill(resolved.begin(), resolved.end(), Id(11));
      Require(parser::BindPolicyObjectIdentities(resolved, subject, schema, &output) &&
                  output.policy_uuid == output.target_object_uuid,
              "structural policy binding imposed object-type or authorization policy");
    }
  }
}

void TestSecurityDclIdentityPairs() {
  namespace parser = scratchbird::parser::sbsql;
  static_assert(std::is_same_v<decltype(api::EngineSecurityGrantPrivilegeRequest{}.grantee_uuid), Uuid>);
  static_assert(std::is_same_v<decltype(api::EngineSecurityGrantPrivilegeRequest{}.grant_uuid), Uuid>);
  static_assert(std::is_same_v<decltype(api::EngineSecurityRevokePrivilegeRequest{}.target_object_uuid), Uuid>);
  static_assert(std::is_same_v<decltype(api::EngineSecurityGrantMembershipRequest{}.membership_uuid), Uuid>);
  static_assert(std::is_same_v<decltype(api::EngineSecurityGrantMembershipRequest{}.container_uuid), Uuid>);
  static_assert(std::is_same_v<decltype(api::EngineSecurityRevokeMembershipRequest{}.member_principal_uuid), Uuid>);
  const std::array<Uuid, 2> pair{Id(11), Id(12)};
  Uuid first = Id(90), second = Id(91);
  fail_after = 0;
  const bool bound = parser::BindSecurityDclIdentityPair(pair, &first, &second);
  fail_after = -1;
  Require(bound && first == pair[0] && second == pair[1], "DCL pair allocated or lost binding order");
  const auto reject_binding = [&](std::span<const Uuid> bad) {
    first = Id(90); second = Id(91);
    fail_after = 0;
    const bool accepted = parser::BindSecurityDclIdentityPair(bad, &first, &second);
    fail_after = -1;
    Require(!accepted && first == Id(90) && second == Id(91), "DCL malformed pair changed output");
  };
  reject_binding({}); reject_binding(std::span(pair).first(1));
  reject_binding(std::vector<Uuid>(3, Id(11)));
  reject_binding(std::vector<Uuid>(70, Id(11)));
  for (std::size_t slot = 0; slot != 2; ++slot) {
    auto bad = pair; bad[slot] = {}; reject_binding(bad);
    for (unsigned version = 0; version != 16; ++version) {
      if (version == 7) continue;
      bad = pair; bad[slot].bytes[6] = static_cast<std::uint8_t>(version << 4); reject_binding(bad);
    }
    for (unsigned variant : {0u, 0x40u, 0xc0u, 0xe0u}) {
      bad = pair; bad[slot].bytes[8] = static_cast<std::uint8_t>(variant); reject_binding(bad);
    }
  }
  first = Id(90); second = Id(91);
  Require(!parser::BindSecurityDclIdentityPair(pair, nullptr, &second) && second == Id(91), "null first output admitted");
  Require(!parser::BindSecurityDclIdentityPair(pair, &first, nullptr) && first == Id(90), "null second output admitted");
  Require(!parser::BindSecurityDclIdentityPair(pair, &first, &first) && first == Id(90), "aliased pair output admitted");
  auto reversed = pair;
  Require(parser::BindSecurityDclIdentityPair(reversed, &reversed[1], &reversed[0]) &&
              reversed[0] == pair[1] && reversed[1] == pair[0], "input aliasing lost original pair");
  const std::array<Uuid, 2> same{Id(11), Id(11)};
  Require(parser::BindSecurityDclIdentityPair(same, &first, &second) && first == second,
          "structural binder incorrectly imposed authorization on equal identities");

  for (const auto kind : {wire::SblrSecurityDclKind::privilege, wire::SblrSecurityDclKind::membership}) {
    const bool privilege = kind == wire::SblrSecurityDclKind::privilege;
    wire::SblrOperationEnvelope envelope;
    const auto append = [&](auto& target, std::string_view role, Uuid identity) {
      wire::SblrOperand operand;
      operand.ordinal = static_cast<std::uint32_t>(target.operands.size() + 1);
      operand.name = role; operand.type = "uuid"; operand.value_kind = wire::SblrValueKind::uuid_ref;
      operand.value_body.assign(identity.bytes.begin(), identity.bytes.end());
      target.operands.push_back(std::move(operand));
    };
    append(envelope, privilege ? "target_object_uuid" : "member_principal_uuid", pair[0]);
    append(envelope, privilege ? "grantee_uuid" : "container_uuid", pair[1]);
    wire::SblrSecurityDclIdentities expected;
    if (privilege) { expected.target_object_uuid = pair[0]; expected.grantee_uuid = pair[1]; }
    else { expected.member_principal_uuid = pair[0]; expected.container_uuid = pair[1]; }
    wire::SblrSecurityDclIdentities output;
    fail_after = 0;
    const bool accepted = wire::DecodeSblrSecurityDclIdentities(envelope, kind, &output);
    fail_after = -1;
    Require(accepted && output == expected, "DCL engine pair allocated or changed semantic roles");
    const auto reject = [&](const auto& bad) {
      output = expected; fail_after = 0;
      const bool ok = wire::DecodeSblrSecurityDclIdentities(bad, kind, &output);
      fail_after = -1;
      Require(!ok && output == expected, "DCL malformed engine pair published partial identities");
    };
    auto bad = envelope; bad.operands.clear(); reject(bad);
    bad = envelope; bad.operands.pop_back(); reject(bad);
    for (const auto role : {"source_uuid", "target_schema_uuid", "related_object_0_uuid"}) {
      bad = envelope; append(bad, role, Id(13)); reject(bad);
    }
    bad = envelope;
    bad.operands[1].name = privilege ? "container_uuid" : "grantee_uuid"; reject(bad);
    bad = envelope; bad.operands[1] = bad.operands[0]; bad.operands[1].ordinal = 2; reject(bad);
    for (std::size_t slot = 0; slot != 2; ++slot) {
      bad = envelope; bad.operands[slot].value_body.assign(16, 0); reject(bad);
      bad = envelope; bad.operands[slot].value = "text shadow"; reject(bad);
      bad = envelope; bad.operands[slot].ordinal = 0; reject(bad);
    }
    Require(!wire::DecodeSblrSecurityDclIdentities(envelope, kind, nullptr), "null DCL engine output admitted");
    Require(!wire::DecodeSblrSecurityDclIdentities(envelope,
                static_cast<wire::SblrSecurityDclKind>(99), &output) && output == expected,
            "invalid DCL kind admitted or changed output");
    bad = envelope; std::swap(bad.operands[0], bad.operands[1]);
    bad.operands[0].ordinal = 1; bad.operands[1].ordinal = 2;
    Require(wire::DecodeSblrSecurityDclIdentities(bad, kind, &output) && output == expected,
            "operand order substituted for named DCL role");
  }
}

void TestBoundDmlIdentityPublication() {
  namespace parser = scratchbird::parser::sbsql;
  using Binding = std::pair<std::string_view, Uuid>;
  const std::array<Binding, 12> bindings{{
      {"target_object_uuid", Id(31)}, {"source_uuid", Id(32)},
      {"routine_object_uuid", Id(33)}, {"insert_select_source_uuid_0", Id(34)},
      {"insert_select_source_uuid_1", Id(35)}, {"target_schema_uuid", Id(36)},
      {"grantee_uuid", Id(37)}, {"member_principal_uuid", Id(38)}, {"container_uuid", Id(39)},
      {"principal_uuid", Id(40)}, {"policy_uuid", Id(41)}, {"role_uuid", Id(42)}}};
  parser::SblrEnvelope original;
  for (const auto& [role, identity] : bindings) {
    (void)role;
    original.resolved_object_uuids.push_back(identity);
  }
  parser::SblrOperand prefix;
  prefix.name = "unrelated_parameter";
  prefix.type = "literal";
  prefix.value = "keep this value";
  original.operands.push_back(prefix);
  original.payload = "parser-local metadata";
  const auto same_state = [](const auto& left, const auto& right) {
    return left.resolved_object_uuids == right.resolved_object_uuids &&
        left.payload == right.payload &&
        std::equal(left.operands.begin(), left.operands.end(),
                   right.operands.begin(), right.operands.end(),
                   [](const auto& a, const auto& b) {
                     return a.type == b.type && a.name == b.name &&
                         a.value == b.value &&
                         a.canonical_value_kind == b.canonical_value_kind &&
                         a.canonical_value_body == b.canonical_value_body;
                   });
  };
  auto published = original;
  const auto before = allocations;
  Require(parser::AppendBoundDmlObjectIdentities(&published, bindings),
          "bound DML cohort did not publish");
  const auto sites = allocations - before;
  Require(published.operands.size() == original.operands.size() + bindings.size(),
          "bound DML cohort published wrong operand count");
  for (const auto& [role, identity] : bindings) {
    Require(parser::FindDmlObjectIdentity(published, role) == identity,
            "bound DML cohort changed raw identity or role");
  }
  auto prefix_only = published;
  prefix_only.operands.resize(original.operands.size());
  Require(same_state(prefix_only, original), "bound DML cohort changed prior state");
  for (std::size_t site = 0; site < sites; ++site) {
    auto failed = original;
    bool threw = false;
    fail_after = site;
    try { (void)parser::AppendBoundDmlObjectIdentities(&failed, bindings); }
    catch (const std::bad_alloc&) { threw = true; }
    fail_after = -1;
    Require(threw && same_state(failed, original),
            "bound DML allocation fault published a partial cohort");
    ++faults;
  }
  const auto reject = [&](auto bad, parser::SblrEnvelope initial) {
    const auto saved = initial;
    Require(!parser::AppendBoundDmlObjectIdentities(&initial, bad),
            "invalid bound DML cohort was admitted");
    Require(same_state(initial, saved), "invalid bound DML cohort changed output");
  };
  auto bad = bindings;
  bad.back().second = Id(99); reject(bad, original);
  bad = bindings; bad.back().second = {}; reject(bad, original);
  bad = bindings; bad.back().first = "unknown_identity_role"; reject(bad, original);
  bad = bindings; bad.back().first = bindings.front().first; reject(bad, original);
  reject(bindings, published);
  auto shadowed = original;
  shadowed.operands.front().name = bindings.back().first;
  reject(bindings, shadowed);
  for (unsigned version = 0; version != 16; ++version) {
    if (version == 7) continue;
    bad = bindings;
    bad.back().second.bytes[6] = static_cast<std::uint8_t>(version << 4);
    auto cohort = original;
    cohort.resolved_object_uuids.back() = bad.back().second;
    reject(bad, cohort);
  }
  std::vector<Binding> oversized(bindings.begin(), bindings.end());
  oversized.push_back(bindings.front()); reject(oversized, original);
  Require(!parser::AppendBoundDmlObjectIdentities(nullptr, bindings),
          "null bound DML destination admitted");
  auto empty = original;
  Require(parser::AppendBoundDmlObjectIdentities(&empty, {}) && same_state(empty, original),
          "empty DML identity cohort changed prior state");
  const std::array<Binding, 2> routine{{
      {"target_object_uuid", Id(31)}, {"routine_object_uuid", Id(31)}}};
  auto invocation = original;
  Require(parser::AppendBoundDmlObjectIdentities(&invocation, routine) &&
              parser::FindDmlObjectIdentity(invocation, routine[0].first) == Id(31) &&
              parser::FindDmlObjectIdentity(invocation, routine[1].first) == Id(31),
          "routine target and invocation roles lost common binary authority");
}

void TestBoundObjectEngineProjection() {
  namespace parser = scratchbird::parser::sbsql;
  const std::array<std::pair<std::string_view, Uuid>, 12> bindings{{
      {"target_object_uuid", Id(41)}, {"source_uuid", Id(42)},
      {"routine_object_uuid", Id(43)}, {"insert_select_source_uuid_0", Id(44)},
      {"insert_select_source_uuid_1", Id(45)}, {"target_schema_uuid", Id(62)},
      {"grantee_uuid", Id(46)}, {"member_principal_uuid", Id(47)}, {"container_uuid", Id(48)},
      {"principal_uuid", Id(49)}, {"policy_uuid", Id(50)}, {"role_uuid", Id(51)}}};
  parser::SblrEnvelope lowered;
  for (const auto& binding : bindings) lowered.resolved_object_uuids.push_back(binding.second);
  Require(parser::AppendBoundDmlObjectIdentities(&lowered, bindings), "projection fixture binding failed");
  auto envelope = wire::MakeSblrEnvelope("query.execute", "SBLR_QUERY_EXECUTE", "identity.projection.fixture");
  envelope.opcode_code = 4615; envelope.parser_package_uuid = Id(6); envelope.registry_snapshot_uuid = Id(7);
  envelope.result_shape = "query_execute_result"; envelope.diagnostic_shape = "diagnostic_vector";
  for (const auto& source : lowered.operands) {
    wire::SblrOperand operand;
    operand.ordinal = static_cast<std::uint32_t>(envelope.operands.size() + 1);
    operand.type = source.type; operand.name = source.name;
    operand.value_kind = static_cast<wire::SblrValueKind>(source.canonical_value_kind);
    operand.value_body = source.canonical_value_body;
    envelope.operands.push_back(std::move(operand));
  }
  // Real SBOP byte encoding/decoding, not SQL execution or receipt admission.
  const auto bytes = wire::EncodeSblrEnvelope(envelope);
  const auto decoded = wire::DecodeSblrEnvelope(bytes);
  Require(!bytes.empty() && decoded.ok, "bound identities did not survive SBOP transport");
  wire::SblrBoundObjectIdentities identities;
  api::EngineApiRequest request;
  request.context.principal_uuid = Id(61);
  request.target_database.uuid = Id(63);
  request.target_object.object_kind = "existing-object-kind";
  request.option_envelopes = {"ordinary_option:retained"};
  fail_after = 0;
  const bool decoded_ok = wire::DecodeSblrBoundObjectIdentities(decoded.envelope, &identities);
  const bool projected = wire::ProjectSblrBoundTargetIdentity(decoded.envelope, &request);
  fail_after = -1;
  Require(decoded_ok && projected && request.target_object.uuid == bindings[0].second,
          "engine projection allocated or lost the raw target identity");
  Require(request.context.principal_uuid == Id(61) && request.target_schema.uuid == Id(62) &&
              request.target_database.uuid == Id(63) &&
              request.target_object.object_kind == "existing-object-kind" &&
              request.option_envelopes == std::vector<std::string>{"ordinary_option:retained"},
          "identity projection changed unrelated request authority");
  for (std::size_t i = 0; i < bindings.size(); ++i) {
    Require(identities.Find(bindings[i].first) == bindings[i].second &&
                decoded.envelope.operands[i].value_body ==
                    Bytes(bindings[i].second.bytes.begin(), bindings[i].second.bytes.end()),
            "engine identity role or canonical bytes changed");
  }
  Require(!identities.Find("unknown_role"), "unknown role supplied identity authority");
  const auto saved = identities;
  const auto reject = [&](const wire::SblrOperationEnvelope& bad) {
    identities = saved;
    api::EngineApiRequest prior;
    prior.target_object.uuid = Id(99);
    fail_after = 0;
    const bool accepted_decode = wire::DecodeSblrBoundObjectIdentities(bad, &identities);
    const bool accepted_projection = wire::ProjectSblrBoundTargetIdentity(bad, &prior);
    fail_after = -1;
    Require(!accepted_decode && identities.values == saved.values,
            "malformed identity decode published partial state");
    Require(!accepted_projection && prior.target_object.uuid == Id(99),
            "malformed identity projection changed bound target");
  };
  for (std::size_t role = 0; role < bindings.size(); ++role) {
    for (std::size_t length = 0; length != 16; ++length) {
      auto bad = decoded.envelope; bad.operands[role].value_body.resize(length); reject(bad);
    }
    auto bad = decoded.envelope; bad.operands[role].value_body.push_back(0); reject(bad);
    bad = decoded.envelope; bad.operands[role].type = "text"; reject(bad);
    bad = decoded.envelope; bad.operands[role].value = "UUID text shadow"; reject(bad);
    bad = decoded.envelope; bad.operands[role].value_flags = 1; reject(bad);
    bad = decoded.envelope; bad.operands[role].value_kind = wire::SblrValueKind::literal_typed; reject(bad);
    bad = decoded.envelope; bad.operands[role].ordinal = 0; reject(bad);
    bad = decoded.envelope; bad.operands[role].value_body.assign(16, 0); reject(bad);
    bad = decoded.envelope; bad.operands.push_back(bad.operands[role]);
    bad.operands.back().ordinal = static_cast<std::uint32_t>(bad.operands.size()); reject(bad);
    for (unsigned version = 0; version != 16; ++version) {
      if (version == 7) continue;
      bad = decoded.envelope; bad.operands[role].value_body[6] = static_cast<std::uint8_t>(version << 4); reject(bad);
    }
    for (const unsigned variant : {0x00, 0x40, 0xc0, 0xe0}) {
      bad = decoded.envelope; bad.operands[role].value_body[8] = static_cast<std::uint8_t>(variant); reject(bad);
    }
  }
  for (const auto& [role, identity] : bindings) {
    (void)identity;
    for (const auto suffix : {"", ":", ":00000000-0000-7000-8000-000000000051"}) {
      auto shadow = request;
      shadow.option_envelopes.push_back(std::string(role) + suffix);
      Require(!wire::ProjectSblrBoundTargetIdentity(decoded.envelope, &shadow) &&
                  shadow.target_object.uuid == request.target_object.uuid,
              "text option accepted as second identity authority");
    }
  }
  for (const auto alias : {"index_target_uuid", "statistics_target_uuid", "target_table_uuid",
                          "index_object_uuid", "index_template_object_uuid", "object_uuid", "schema_uuid",
                          "function_object_uuid", "procedure_object_uuid", "trigger_object_uuid",
                          "target_filespace_uuid", "owner_object_uuid", "grant_uuid", "membership_uuid"}) {
    for (const auto suffix : {"", ":", ":00000000-0000-7000-8000-000000000051"}) {
      auto shadow = request;
      shadow.option_envelopes.push_back(std::string(alias) + suffix);
      fail_after = 0;
      const bool accepted = wire::ProjectSblrBoundTargetIdentity(decoded.envelope, &shadow);
      fail_after = -1;
      Require(!accepted && shadow.target_object.uuid == request.target_object.uuid &&
                  shadow.target_schema.uuid == request.target_schema.uuid,
              "legacy catalog option alias admitted or changed identity");
    }
    auto shadow = decoded.envelope;
    shadow.operands.front().name = alias;
    reject(shadow);
  }
  auto conflict = request; conflict.target_object.uuid = Id(99);
  Require(!wire::ProjectSblrBoundTargetIdentity(decoded.envelope, &conflict) &&
              conflict.target_object.uuid == Id(99), "conflicting binary target was overwritten");
  auto schema_conflict = request;
  schema_conflict.target_object.uuid = {};
  schema_conflict.target_schema.uuid = Id(98);
  Require(!wire::ProjectSblrBoundTargetIdentity(decoded.envelope, &schema_conflict) &&
              schema_conflict.target_object.uuid.is_nil() && schema_conflict.target_schema.uuid == Id(98),
          "schema conflict published partial target authority");
  auto invalid_schema = request;
  invalid_schema.target_object.uuid = {};
  invalid_schema.target_schema.uuid.bytes[6] = 0x40;
  const auto prior_schema = invalid_schema.target_schema.uuid;
  Require(!wire::ProjectSblrBoundTargetIdentity(decoded.envelope, &invalid_schema) &&
              invalid_schema.target_object.uuid.is_nil() && invalid_schema.target_schema.uuid == prior_schema,
          "invalid prebound schema accepted or changed target");
  auto invalid_prior = request; invalid_prior.target_object.uuid.bytes[6] = 0x40;
  const auto invalid_identity = invalid_prior.target_object.uuid;
  Require(!wire::ProjectSblrBoundTargetIdentity(decoded.envelope, &invalid_prior) &&
              invalid_prior.target_object.uuid == invalid_identity,
          "invalid prior target replaced without refusal");
  auto no_target = decoded.envelope; no_target.operands.clear();
  auto retained = request;
  Require(wire::ProjectSblrBoundTargetIdentity(no_target, &retained) &&
              retained.target_object.uuid == request.target_object.uuid,
          "absent target operand cleared existing bound authority");
  Require(!wire::ProjectSblrBoundTargetIdentity(decoded.envelope, nullptr) &&
              !wire::DecodeSblrBoundObjectIdentities(decoded.envelope, nullptr),
          "null identity projection output admitted");
}

void TestRelatedObjectIdentityProjection() {
  namespace parser = scratchbird::parser::sbsql;
  parser::SblrEnvelope single;
  single.resolved_object_uuids = {Id(71)};
  const auto single_before = allocations;
  Require(parser::AppendBoundSingleQueryObjectIdentity(&single, Id(71)),
          "single-source binary target refused");
  const auto single_sites = allocations - single_before;
  Require(single.operands.size() == 1 && single.operands.front().name == "target_object_uuid" &&
              single.operands.front().value.empty() &&
              parser::FindDmlObjectIdentity(single, "target_object_uuid") == Id(71),
          "single-source target was rendered as text or changed identity");
  Require(!parser::AppendBoundSingleQueryObjectIdentity(&single, Id(71)) && single.operands.size() == 1,
          "single-source target was published twice");
  for (std::size_t site = 0; site < single_sites; ++site) {
    parser::SblrEnvelope failed;
    failed.resolved_object_uuids = {Id(71)};
    fail_after = static_cast<long>(site);
    bool accepted = false;
    try { accepted = parser::AppendBoundSingleQueryObjectIdentity(&failed, Id(71)); }
    catch (const std::bad_alloc&) {}
    fail_after = -1;
    Require(!accepted && failed.operands.empty() && failed.resolved_object_uuids == single.resolved_object_uuids,
            "single-source allocation failure published partial target");
    ++faults;
  }
  for (unsigned version = 0; version < 16; ++version) {
    if (version == 7) continue;
    auto invalid = Id(71); invalid.bytes[6] = static_cast<std::uint8_t>(version << 4);
    parser::SblrEnvelope failed;
    failed.resolved_object_uuids = {invalid};
    Require(!parser::AppendBoundSingleQueryObjectIdentity(&failed, invalid) && failed.operands.empty(),
            "non-v7 single-source identity accepted");
  }
  parser::SblrEnvelope missing;
  missing.resolved_object_uuids = {Uuid{}};
  Require(!parser::AppendBoundSingleQueryObjectIdentity(&missing, {}) && missing.operands.empty(),
          "missing table target became source-free query");
  Require(!parser::AppendBoundSingleQueryObjectIdentity(&missing, Id(71)) && missing.operands.empty(),
          "unbound table identity accepted");
  Require(!parser::AppendBoundSingleQueryObjectIdentity(nullptr, Id(71)), "null target output accepted");
  std::vector<Uuid> related;
  for (unsigned i = 0; i < 70; ++i) related.push_back(Id(static_cast<std::uint8_t>(100 + i)));
  related[1] = related[0]; // Self-join occurrences are distinct slots, not distinct object UUIDs.
  parser::SblrEnvelope initial;
  initial.resolved_object_uuids = related;
  initial.resolved_object_uuids.push_back(Id(71));
  initial.payload = "non-identity metadata";
  auto lowered = initial;
  const auto before = allocations;
  Require(parser::AppendBoundQueryObjectIdentities(&lowered, Id(71), related),
          "binary query source cohort refused");
  const auto parser_sites = allocations - before;
  Require(lowered.operands.size() == 71, "query sources truncated at a legacy relation limit");
  fail_after = 0;
  const bool parser_identities_valid = parser::ValidateBoundObjectIdentityOperands(lowered);
  fail_after = -1;
  Require(parser_identities_valid, "parser verifier allocated or rejected the complete binary source cohort");
  const auto reject_parser_identity = [&](const parser::SblrEnvelope& candidate) {
    fail_after = 0;
    const bool accepted = parser::ValidateBoundObjectIdentityOperands(candidate);
    fail_after = -1;
    Require(!accepted, "parser identity verifier accepted malformed or unbound authority");
  };
  for (const auto role : {"related_object_00_uuid", "related_object_-1_uuid", "related_object__uuid",
                           "related_object_184467440737095516160_uuid", "related_object_70_uuid"}) {
    auto invalid = lowered; invalid.operands.back().name = role;
    reject_parser_identity(invalid);
  }
  auto invalid_parser = lowered;
  invalid_parser.operands.back().name = "related_object_0_uuid";
  reject_parser_identity(invalid_parser);
  invalid_parser = lowered; invalid_parser.operands.erase(invalid_parser.operands.begin() + 30);
  reject_parser_identity(invalid_parser);
  invalid_parser = lowered; invalid_parser.operands.back().value = "text shadow";
  reject_parser_identity(invalid_parser);
  invalid_parser = lowered; invalid_parser.operands.back().canonical_value_body.resize(15);
  reject_parser_identity(invalid_parser);
  invalid_parser = lowered; invalid_parser.resolved_object_uuids.pop_back();
  reject_parser_identity(invalid_parser);
  invalid_parser = lowered; invalid_parser.operands.push_back(invalid_parser.operands.front());
  reject_parser_identity(invalid_parser);
  invalid_parser = lowered; invalid_parser.operands.front().canonical_value_kind = 0;
  reject_parser_identity(invalid_parser);
  invalid_parser = lowered; invalid_parser.operands.front().type = "text";
  reject_parser_identity(invalid_parser);
  auto shuffled_parser = lowered;
  std::reverse(shuffled_parser.operands.begin(), shuffled_parser.operands.end());
  Require(parser::ValidateBoundObjectIdentityOperands(shuffled_parser),
          "parser identity verifier confused operand order with semantic source order");
  parser::SblrEnvelope legacy_text_only;
  legacy_text_only.payload = "{\"target_object_uuid\":\"00000000-0000-7000-8000-000000000001\"}";
  Require(!parser::FindDmlObjectIdentity(legacy_text_only, "target_object_uuid"),
          "legacy metadata supplied missing binary target authority");
  for (std::size_t site = 0; site < parser_sites; ++site) {
    auto failed = initial;
    fail_after = static_cast<long>(site);
    bool accepted = false;
    try { accepted = parser::AppendBoundQueryObjectIdentities(&failed, Id(71), related); }
    catch (const std::bad_alloc&) {}
    fail_after = -1;
    Require(!accepted && failed.operands.empty() && failed.resolved_object_uuids == initial.resolved_object_uuids &&
                failed.payload == initial.payload, "query binding allocation failure published a prefix");
    ++faults;
  }
  auto foreign = related; foreign.back() = Id(250);
  auto failed = initial;
  Require(!parser::AppendBoundQueryObjectIdentities(&failed, Id(71), foreign) && failed.operands.empty(),
          "foreign related identity changed parser envelope");
  auto duplicate = lowered;
  Require(!parser::AppendBoundQueryObjectIdentities(&duplicate, Id(71), related) &&
              duplicate.operands.size() == lowered.operands.size(), "duplicate source cohort appended");

  auto envelope = wire::MakeSblrEnvelope("query.execute", "SBLR_QUERY_EXECUTE", "query.sources.fixture");
  envelope.opcode_code = 4615; envelope.parser_package_uuid = Id(6); envelope.registry_snapshot_uuid = Id(7);
  envelope.result_shape = "query_execute_result"; envelope.diagnostic_shape = "diagnostic_vector";
  for (const auto& source : lowered.operands) {
    wire::SblrOperand operand;
    operand.ordinal = static_cast<std::uint32_t>(envelope.operands.size() + 1);
    operand.type = source.type; operand.name = source.name;
    operand.value_kind = static_cast<wire::SblrValueKind>(source.canonical_value_kind);
    operand.value_body = source.canonical_value_body;
    envelope.operands.push_back(std::move(operand));
  }
  const auto bytes = wire::EncodeSblrEnvelope(envelope);
  const auto decoded = wire::DecodeSblrEnvelope(bytes);
  Require(!bytes.empty() && decoded.ok, "related source SBOP transport failed");
  api::EngineApiRequest request;
  request.target_schema.uuid = Id(72);
  auto projection_failure = wire::SblrIdentityProjectionFailure::allocation_failed;
  const auto engine_before = allocations;
  Require(wire::ProjectSblrBoundTargetIdentity(decoded.envelope, &request, &projection_failure), "related source projection refused");
  const auto engine_sites = allocations - engine_before;
  Require(projection_failure == wire::SblrIdentityProjectionFailure::none,
          "successful projection retained stale failure status");
  Require(request.target_object.uuid == Id(71) && request.target_schema.uuid == Id(72) &&
              request.related_objects.size() == related.size(), "related source projection lost authority");
  for (std::size_t i = 0; i < related.size(); ++i)
    Require(request.related_objects[i].uuid == related[i], "related source occurrence or raw identity changed");
  for (std::size_t site = 0; site < engine_sites; ++site) {
    api::EngineApiRequest unchanged;
    unchanged.target_schema.uuid = Id(72);
    fail_after = static_cast<long>(site);
    const bool accepted = wire::ProjectSblrBoundTargetIdentity(decoded.envelope, &unchanged, &projection_failure);
    fail_after = -1;
    Require(projection_failure == wire::SblrIdentityProjectionFailure::allocation_failed,
            "related projection allocation failure reported invalid operand");
    Require(!accepted && unchanged.target_object.uuid.is_nil() && unchanged.target_schema.uuid == Id(72) &&
                unchanged.related_objects.empty(), "related projection allocation failure published partial authority");
    ++faults;
  }
  auto shuffled = decoded.envelope;
  std::reverse(shuffled.operands.begin(), shuffled.operands.end());
  for (std::size_t i = 0; i < shuffled.operands.size(); ++i) shuffled.operands[i].ordinal = i + 1;
  auto retained = request;
  retained.related_objects.front().object_kind = "prebound-kind";
  Require(wire::ProjectSblrBoundTargetIdentity(shuffled, &retained) &&
              retained.related_objects.front().object_kind == "prebound-kind" &&
              retained.related_objects.back().uuid == related.back(), "operand order changed semantic source indices");
  auto prebound = request;
  prebound.target_object.uuid = {};
  for (auto& ref : prebound.related_objects) ref.object_kind = std::string(80, 'k');
  auto projected_prebound = prebound;
  const auto prebound_before = allocations;
  Require(wire::ProjectSblrBoundTargetIdentity(decoded.envelope, &projected_prebound),
          "matching prebound cohort refused");
  const auto prebound_sites = allocations - prebound_before;
  for (std::size_t site = 0; site < prebound_sites; ++site) {
    auto unchanged = prebound;
    fail_after = static_cast<long>(site);
    const bool accepted = wire::ProjectSblrBoundTargetIdentity(decoded.envelope, &unchanged, &projection_failure);
    fail_after = -1;
    Require(projection_failure == wire::SblrIdentityProjectionFailure::allocation_failed,
            "prebound allocation failure reported invalid operand");
    Require(!accepted && unchanged.target_object.uuid.is_nil() && unchanged.target_schema.uuid == Id(72) &&
                unchanged.related_objects.size() == prebound.related_objects.size() &&
                std::equal(unchanged.related_objects.begin(), unchanged.related_objects.end(),
                    prebound.related_objects.begin(), [](const auto& a, const auto& b) {
                      return a.uuid == b.uuid && a.object_kind == b.object_kind;
                    }), "prebound projection fault changed identity or metadata");
    ++faults;
  }
  auto no_sources = decoded.envelope; no_sources.operands.resize(1);
  auto unchanged_sources = prebound;
  Require(wire::ProjectSblrBoundTargetIdentity(no_sources, &unchanged_sources) &&
              unchanged_sources.related_objects.size() == 70 &&
              unchanged_sources.related_objects.back().uuid == related.back(),
          "absent source operands cleared prebound authority");
  const auto reject = [&](const wire::SblrOperationEnvelope& bad) {
    api::EngineApiRequest unchanged;
    unchanged.target_schema.uuid = Id(72);
    Require(!wire::ProjectSblrBoundTargetIdentity(bad, &unchanged, &projection_failure) &&
                unchanged.target_object.uuid.is_nil() && unchanged.target_schema.uuid == Id(72) &&
                unchanged.related_objects.empty(), "malformed related source published request authority");
    Require(projection_failure == wire::SblrIdentityProjectionFailure::invalid_operand,
            "invalid operand retained stale allocation failure status");
  };
  for (const auto name : {"related_object_00_uuid", "related_object_-1_uuid", "related_object__uuid",
                          "related_object_184467440737095516160_uuid", "related_object_70_uuid"}) {
    auto bad = decoded.envelope; bad.operands.back().name = name; reject(bad);
  }
  auto bad = decoded.envelope; bad.operands.back().name = "related_object_0_uuid"; reject(bad);
  bad = decoded.envelope; bad.operands.pop_back();
  bad.operands.back().name = "related_object_69_uuid"; reject(bad);
  for (std::size_t length = 0; length <= 17; ++length) {
    if (length == 16) continue;
    bad = decoded.envelope; bad.operands.back().value_body.resize(length); reject(bad);
  }
  bad = decoded.envelope; bad.operands.back().value = "text UUID shadow"; reject(bad);
  bad = decoded.envelope; bad.operands.back().type = "text"; reject(bad);
  bad = decoded.envelope; bad.operands.back().value_flags = 1; reject(bad);
  bad = decoded.envelope; bad.operands.back().ordinal = 0; reject(bad);
  bad = decoded.envelope; bad.operands.back().value_kind = wire::SblrValueKind::literal_typed; reject(bad);
  for (unsigned version = 0; version < 16; ++version) {
    if (version == 7) continue;
    bad = decoded.envelope; bad.operands.back().value_body[6] = version << 4; reject(bad);
  }
  for (const unsigned variant : {0x00, 0x40, 0xc0, 0xe0}) {
    bad = decoded.envelope; bad.operands.back().value_body[8] = variant; reject(bad);
  }
  auto conflict = request; conflict.target_object.uuid = {};
  conflict.related_objects.back().uuid = Id(250);
  Require(!wire::ProjectSblrBoundTargetIdentity(decoded.envelope, &conflict) &&
              conflict.target_object.uuid.is_nil() && conflict.related_objects.back().uuid == Id(250),
          "prebound related-object conflict published target");
  conflict = request; conflict.target_object.uuid = {}; conflict.related_objects.pop_back();
  Require(!wire::ProjectSblrBoundTargetIdentity(decoded.envelope, &conflict) &&
              conflict.target_object.uuid.is_nil() && conflict.related_objects.size() == 69,
          "prebound related count mismatch accepted");
  auto shadow = request; shadow.option_envelopes.push_back("related_object_0_uuid:");
  Require(!wire::ProjectSblrBoundTargetIdentity(decoded.envelope, &shadow), "legacy text source authority accepted");
}

void TestContextIdentityOperands() {
  namespace parser = scratchbird::parser::sbsql;
  constexpr std::array<std::string_view, 7> expected_names{
      "relational_bound_sblr_tree_uuid", "relational_catalog_epoch_uuid",
      "relational_security_context_uuid", "relational_statement_uuid",
      "relational_owning_transaction_uuid", "relational_statement_snapshot_uuid",
      "relational_statement_metadata_snapshot_uuid"};
  Require(wire::kRelationalContextIdentitySlots == expected_names, "context identity slots differ from contract");
  auto envelope = wire::MakeSblrEnvelope("query.execute", "SBLR_QUERY_EXECUTE", "context.identity.fixture");
  envelope.opcode_code = 4615; envelope.parser_package_uuid = Id(6); envelope.registry_snapshot_uuid = Id(7);
  envelope.result_shape = "query_execute_result"; envelope.diagnostic_shape = "diagnostic_vector";
  for (std::size_t slot = 0; slot < expected_names.size(); ++slot) {
    const auto name = expected_names[slot]; const auto identity = Id(static_cast<std::uint8_t>(40 + slot));
    const auto native = parser::MakeRelationalContextIdentityOperand(name, identity);
    Require(native && native->type == "uuid" && native->name == name && native->value.empty() &&
            native->canonical_value_kind == 1 && native->canonical_value_body == Bytes(identity.bytes.begin(), identity.bytes.end()),
            "native identity operand changed binary bytes or used text");
    Uuid decoded = Id(90);
    fail_after = 0;
    const bool decoded_without_allocation = wire::DecodeRelationalContextIdentity(
        "uuid", name, wire::SblrValueKind::uuid_ref, identity.bytes.data(), 16, &decoded);
    fail_after = -1;
    Require(decoded_without_allocation && decoded == identity, "binary reference decode allocated or changed identity");
    const auto decode = [&](const Bytes& bytes, wire::SblrValueKind kind = wire::SblrValueKind::uuid_ref,
                            std::string_view type = "uuid") {
      auto output = Id(90);
      const bool ok = wire::DecodeRelationalContextIdentity(type, name, kind, bytes.data(), bytes.size(), &output);
      if (!ok) Require(output == Id(90), "reference refusal modified published UUID");
      return ok;
    };
    for (std::size_t size = 0; size < 24; ++size) {
      if (size == 16) continue;
      Bytes bytes(identity.bytes.begin(), identity.bytes.end()); bytes.resize(size);
      Require(!decode(bytes), "context reference accepted inexact byte length");
    }
    for (unsigned kind = 0; kind <= 213; ++kind) {
      if (kind == 1) continue;
      Require(!decode(native->canonical_value_body, static_cast<wire::SblrValueKind>(kind)), "context reference accepted wrong value kind");
    }
    Require(!decode(native->canonical_value_body, wire::SblrValueKind::uuid_ref, "text"), "context reference accepted wrong type");
    Require(!decode(Bytes(16, 0)), "context reference accepted nil");
    Require(!wire::DecodeRelationalContextIdentity("uuid", name, wire::SblrValueKind::uuid_ref, nullptr, 16, &decoded), "context reference dereferenced null input");
    Require(!wire::DecodeRelationalContextIdentity("uuid", name, wire::SblrValueKind::uuid_ref, identity.bytes.data(), 16, nullptr), "context reference accepted null output");
    for (std::size_t byte = 0; byte < 16; ++byte) for (unsigned value = 0; value < 256; ++value) {
      auto changed = identity; changed.bytes[byte] = static_cast<std::uint8_t>(value);
      const bool expected = byte == 6 ? value >> 4 == 7 : byte == 8 ? value >> 6 == 2 : true;
      auto output = Id(90);
      Require(wire::DecodeRelationalContextIdentity("uuid", name, wire::SblrValueKind::uuid_ref,
              changed.bytes.data(), 16, &output) == expected, "context identity version or variant policy differs");
      Require(expected ? output == changed : output == Id(90), "context decoder lost raw bits or modified failed output");
    }
    wire::SblrOperand canonical;
    canonical.ordinal = static_cast<std::uint32_t>(slot + 1); canonical.type = native->type;
    canonical.name = native->name; canonical.value_kind = wire::SblrValueKind::uuid_ref;
    canonical.value_body = native->canonical_value_body; envelope.operands.push_back(canonical);
    const auto frozen = parser::FreezeContextualOperandsV3({*native}, {});
    Require(frozen.has_value(), "valid binary context freeze refused");
    auto invalid = *native; invalid.value = "uuid text shadow";
    Require(!parser::FreezeContextualOperandsV3({invalid}, {}), "context freeze accepted text shadow");
    invalid = *native; invalid.canonical_value_body[6] = 0x40;
    Require(!parser::FreezeContextualOperandsV3({invalid}, {}), "context freeze accepted non-system identity");
    std::size_t failure_sites = 0;
    for (long site = 0; site < 20; ++site) {
      fail_after = site;
      try {
        const auto made = parser::MakeRelationalContextIdentityOperand(name, identity); fail_after = -1;
        Require(made && made->canonical_value_body == native->canonical_value_body,
                "identity operand construction published partial result"); break;
      } catch (const std::bad_alloc&) { fail_after = -1; ++faults; ++failure_sites; }
    }
    Require(failure_sites > 0 && failure_sites < 20, "identity operand allocation sweep incomplete");
  }
  const auto encoded = wire::EncodeSblrEnvelope(envelope);
  const auto decoded = wire::DecodeSblrEnvelope(encoded);
  Require(!encoded.empty() && decoded.ok && decoded.envelope.operands.size() == 7,
          "context identity SBOP round trip refused");
  for (std::size_t slot = 0; slot < envelope.operands.size(); ++slot) {
    Require(decoded.envelope.operands[slot].value_body == envelope.operands[slot].value_body,
            "SBOP changed context identity bytes");
    auto wrong = envelope; wrong.operands[slot].type = "text";
    Require(!wire::ValidateSblrEnvelope(wrong).ok, "SBOP accepted context name with wrong type");
    wrong = envelope; wrong.operands[slot].value_kind = wire::SblrValueKind::literal_typed;
    const auto type_id = Id(80); Bytes wrapper(type_id.bytes.begin(), type_id.bytes.end());
    const std::string old_text = "019d0000-0000-7000-8000-000000000001";
    Append(wrapper, old_text.size(), 8); wrapper.insert(wrapper.end(), old_text.begin(), old_text.end());
    wrong.operands[slot].value_body = wrapper;
    Require(!wire::ValidateSblrEnvelope(wrong).ok, "SBOP admitted retired typed-text context identity");
    wrong = envelope; wrong.operands[slot].value = old_text;
    Require(!wire::ValidateSblrEnvelope(wrong).ok, "SBOP admitted context identity text shadow");
  }
  auto wrong_root = envelope; wrong_root.operation_id = "engine.op.package_begin";
  wrong_root.opcode = "SBLR_PACKAGE_BEGIN"; wrong_root.opcode_code = 1;
  Require(!wire::ValidateSblrEnvelope(wrong_root).ok, "context identity escaped query root");
  Require(!parser::MakeRelationalContextIdentityOperand("unknown_context", Id(1)), "unknown context slot admitted");
  Require(!parser::MakeRelationalContextIdentityOperand(expected_names[0], Uuid{}), "nil context identity constructed");
  // User UUID bytes are values, not one of the seven system reference roles.
  auto user_value = envelope; user_value.operands.resize(1);
  auto& literal = user_value.operands[0]; literal.name = "user_uuid_value";
  literal.value_kind = wire::SblrValueKind::literal_typed;
  const auto descriptor_id = Id(80); literal.value_body.assign(descriptor_id.bytes.begin(), descriptor_id.bytes.end());
  Append(literal.value_body, 16, 8);
  auto user_uuid = Id(81); user_uuid.bytes[6] = 0x40;
  literal.value_body.insert(literal.value_body.end(), user_uuid.bytes.begin(), user_uuid.bytes.end());
  Require(wire::ValidateSblrEnvelope(user_value).ok, "system context policy rejected earlier-version user UUID data");
}
}
namespace {
void TestNativeArtifactPublication() {
  namespace parser = scratchbird::parser::sbsql;
  namespace sb = scratchbird::engine;
  using Status = parser::NativeQueryArtifactStatus;
  scratchbird::parser::ipc::ParserCanonicalSblrSubmission submission;
  submission.statement_uuid = Id(60);
  auto operation = wire::MakeSblrEnvelope("query.execute", "SBLR_QUERY_EXECUTE", "artifact.fixture");
  operation.opcode_code = 4615; operation.parser_package_uuid = Id(61);
  operation.registry_snapshot_uuid = Id(62); operation.result_shape = "query_execute_result";
  operation.diagnostic_shape = "diagnostic_vector";
  const auto frame = [&](bool begin) {
    auto result = operation; result.operation_id = begin ? "engine.op.package_begin" : "engine.op.package_end";
    result.opcode = begin ? "SBLR_PACKAGE_BEGIN" : "SBLR_PACKAGE_END"; result.opcode_code = begin ? 1 : 2;
    wire::SblrOperand operand; operand.ordinal = 1; operand.name = "package_descriptor";
    operand.type = begin ? "package.header" : "package.footer"; operand.value_kind = wire::SblrValueKind::descriptor_ref;
    const auto id = Id(63); operand.value_body.assign(id.bytes.begin(), id.bytes.end());
    result.operands = {operand}; return result;
  };
  wire::SblrOpcodeStream stream; stream.package_descriptor_uuid = Id(63);
  stream.registry_snapshot_uuid = Id(62); stream.operations = {frame(true), operation, frame(false)};
  submission.canonical_operation_bytes = wire::EncodeSblrOpcodeStream(stream);
  Require(!submission.canonical_operation_bytes.empty(), "artifact SBOS fixture refused");
  sb::SblrCanonicalContainer container;
  const auto anchor_uuid = [&](std::size_t offset, Uuid id) {
    std::copy(id.bytes.begin(), id.bytes.end(), container.canonical_anchor.begin() + offset);
  };
  anchor_uuid(0, Id(64)); anchor_uuid(16, Id(65)); anchor_uuid(32, Id(61));
  anchor_uuid(76, Id(62)); anchor_uuid(116, submission.statement_uuid);
  for (const auto offset : {48, 52, 60, 68, 92, 100}) container.canonical_anchor[offset] = 1;
  container.operation_payload = submission.canonical_operation_bytes;
  submission.canonical_container_bytes = sb::EncodeSblrContainer(container);
  const auto number = [](std::uint64_t value, unsigned width) { Bytes out; Append(out, value, width); return out; };
  const auto structure = [&](std::uint32_t format, std::uint32_t id) {
    Bytes out; Append(out, format, 4); Append(out, 1, 2); Append(out, 0, 2);
    Append(out, 4, 8); Append(out, id, 4); return out;
  };
  sb::SblrExecutionEnvelopeV1 ingress; auto& f = ingress.fields;
  f[0].assign(submission.statement_uuid.bytes.begin(), submission.statement_uuid.bytes.end());
  f[1] = number(1, 2); f[2] = number(0, 2); f[3] = number(0x00010001, 4); f[4] = number(1, 2);
  f[5] = {1}; Append(f[5], submission.canonical_operation_bytes.size(), 8);
  f[5].insert(f[5].end(), submission.canonical_operation_bytes.begin(), submission.canonical_operation_bytes.end());
  f[6] = {0}; f[7] = {1}; Append(f[7], sb::SblrCrc32c(submission.canonical_operation_bytes.data(), submission.canonical_operation_bytes.size()), 4);
  f[8] = number(submission.canonical_operation_bytes.size(), 8); f[9] = number(1, 2);
  f[10] = {1}; f[11] = {1};
  const auto dialect = Id(65); const auto user = Id(66);
  f[10].insert(f[10].end(), dialect.bytes.begin(), dialect.bytes.end());
  f[11].insert(f[11].end(), user.bytes.begin(), user.bytes.end());
  f[12] = structure(0x1001, 1); f[13] = structure(0x1002, 2);
  f[14] = {0}; f[15] = number(1, 8); f[16] = number(0, 4); f[17] = number(0, 4);
  f[18] = number(0, 4); f[19] = {0}; f[20] = number(0, 4); f[21] = structure(0x1005, 5);
  f[22] = {0}; f[23] = {0}; f[24] = {0}; f[25] = number(0, 2); f[26] = {0}; f[27] = {0};
  submission.canonical_execution_envelope_bytes = sb::EncodeSblrExecutionEnvelopeV1(ingress);
  Require(!submission.canonical_container_bytes.empty() && !submission.canonical_execution_envelope_bytes.empty(),
          "canonical artifact fixtures refused");
  const auto limit = std::max({submission.canonical_container_bytes.size(), submission.canonical_operation_bytes.size(),
                               submission.canonical_execution_envelope_bytes.size()});
  std::string output = "sentinel";
  Require(parser::PublishNativeQueryArtifact(submission, limit, &output) == Status::kOk &&
          output == std::string(submission.canonical_container_bytes.begin(), submission.canonical_container_bytes.end()),
          "publication did not return exact canonical container bytes");
  const auto refuse = [&](const auto& input, std::size_t budget, Status status) {
    std::string sentinel = "unchanged";
    Require(parser::PublishNativeQueryArtifact(input, budget, &sentinel) == status && sentinel == "unchanged",
            "artifact refusal changed output or reported wrong status");
  };
  refuse(submission, limit - 1, Status::kOverBudget);
  auto wrong = submission; wrong.canonical_operation_bytes.clear(); refuse(wrong, limit, Status::kIncomplete);
  wrong = submission; wrong.canonical_container_bytes.clear(); refuse(wrong, limit, Status::kIncomplete);
  wrong = submission; wrong.canonical_execution_envelope_bytes.clear(); refuse(wrong, limit, Status::kIncomplete);
  wrong = submission; wrong.statement_uuid = Uuid{}; refuse(wrong, limit, Status::kIncomplete);
  wrong = submission; wrong.statement_uuid.bytes[6] = 0x40; refuse(wrong, limit, Status::kInvalid);
  for (auto member : {&scratchbird::parser::ipc::ParserCanonicalSblrSubmission::canonical_container_bytes,
                      &scratchbird::parser::ipc::ParserCanonicalSblrSubmission::canonical_operation_bytes,
                      &scratchbird::parser::ipc::ParserCanonicalSblrSubmission::canonical_execution_envelope_bytes}) {
    wrong = submission; (wrong.*member)[0] ^= 1; refuse(wrong, limit, Status::kInvalid);
    wrong = submission; (wrong.*member).pop_back(); refuse(wrong, limit, Status::kInvalid);
  }
  for (std::size_t byte = 0; byte < 16; ++byte) {
    wrong = submission; wrong.statement_uuid.bytes[byte] ^= 1; refuse(wrong, limit, Status::kInvalid);
    auto changed = container; changed.canonical_anchor[116 + byte] ^= 1;
    wrong = submission; wrong.canonical_container_bytes = sb::EncodeSblrContainer(changed);
    if (!wrong.canonical_container_bytes.empty()) refuse(wrong, limit, Status::kInvalid);
    auto crossed = ingress; crossed.fields[0][byte] ^= 1;
    wrong = submission; wrong.canonical_execution_envelope_bytes = sb::EncodeSblrExecutionEnvelopeV1(crossed);
    if (!wrong.canonical_execution_envelope_bytes.empty()) refuse(wrong, limit, Status::kInvalid);
  }
  auto changed_stream = stream; changed_stream.operations[1].trace_key = "artifact.other";
  wrong = submission; wrong.canonical_operation_bytes = wire::EncodeSblrOpcodeStream(changed_stream);
  refuse(wrong, limit + 1024, Status::kInvalid);
  std::size_t failure_sites = 0;
  for (long site = 0; site < 1000; ++site) {
    std::string sentinel = "unchanged"; fail_after = site;
    try {
      const auto status = parser::PublishNativeQueryArtifact(submission, limit, &sentinel); fail_after = -1;
      Require(status == Status::kOk && sentinel == output, "artifact allocation sweep published partial result"); break;
    } catch (const std::bad_alloc&) { fail_after = -1; ++faults; ++failure_sites;
      Require(sentinel == "unchanged", "artifact allocation failure modified output"); }
  }
  Require(failure_sites > 0 && failure_sites < 1000, "artifact allocation sweep incomplete");
}
}
namespace {
// Independent carrier fixtures: this is shape/rebinding coverage, not a live
// receipt, parser route, parameter execution or catalog-authority oracle.
api::SblrParameterSetSnapshot PreparedAuthority(unsigned count, bool match = false) {
  api::SblrParameterSetSnapshot authority;
  authority.state = api::SblrParameterSetState::active;
  authority.parameter_set_descriptor_uuid = Id(80);
  authority.descriptor_generation = 9;
  for (unsigned i = 0; i < count; ++i)
    authority.slots.push_back({i, Id(30 + i + (match ? 1 : 0)), Id(100 + i),
        0x0102030405060708ULL, api::SblrParameterDirection::in, true});
  return authority;
}

wire::SblrOperationEnvelope PreparedFixture(unsigned count, bool match = false, std::uint64_t literal_node_id = 1) {
  auto op = wire::MakeSblrEnvelope("query.execute", "SBLR_QUERY_EXECUTE", "prepared.binary.fixture");
  op.opcode_code = 4615; op.parser_package_uuid = Id(6); op.registry_snapshot_uuid = Id(7);
  op.result_shape = "query_execute_result"; op.diagnostic_shape = "diagnostic_vector";
  const auto add = [&](std::string type, std::string name, wire::SblrValueKind kind, Bytes bytes) {
    wire::SblrOperand item;
    item.ordinal = op.operands.size() + 1; item.type = std::move(type); item.name = std::move(name);
    item.value_kind = kind; item.value_body = std::move(bytes); op.operands.push_back(std::move(item));
  };
  const auto text = [&](std::string type, std::string name, std::string value) {
    const auto id = Id(99); Bytes body(id.bytes.begin(), id.bytes.end());
    Append(body, value.size(), 8); body.insert(body.end(), value.begin(), value.end());
    add(std::move(type), std::move(name), wire::SblrValueKind::literal_typed, std::move(body));
  };
  const auto binding = [&](std::uint32_t node, std::string variant, std::vector<std::uint32_t> expressions,
                           std::vector<Uuid> objects, std::vector<Uuid> properties) {
    Bytes bytes;
    Require(wire::EncodeRelationalNodeBindingV1({node, std::move(variant), std::move(expressions),
        std::move(objects), properties, properties}, &bytes), "prepared binding encoding");
    add("relational_node_binding_v2", "slot_" + std::to_string(node), wire::SblrValueKind::relational_node_binding, std::move(bytes));
  };
  text("uint16", "relational_wire_version", "2");
  const std::array names{"relational_bound_sblr_tree_uuid", "relational_catalog_epoch_uuid",
      "relational_security_context_uuid", "relational_statement_uuid", "relational_owning_transaction_uuid",
      "relational_statement_snapshot_uuid", "relational_statement_metadata_snapshot_uuid"};
  for (unsigned i = 0; i < names.size(); ++i) {
    const auto id = Id(10 + i); add("uuid", names[i], wire::SblrValueKind::uuid_ref, {id.bytes.begin(), id.bytes.end()});
  }
  text("uint64", "relational_local_transaction_id", "1");
  text("uint64", "relational_snapshot_visible_through_local_transaction_id", "0");
  text("uint32", "relational_root_node_id", match ? "2" : "1");
  std::vector<Descriptor> descriptors;
  const auto descriptor_count = count ? count + (match ? 1 : 0) : 1;
  for (unsigned i = 0; i < descriptor_count; ++i) {
    auto descriptor = Fixture(1); descriptor.descriptor_id = i + 1;
    descriptor.descriptor_uuid = Id(30 + i);
    descriptors.push_back(descriptor);
    // Independent fixed-field oracle, not the production encoder under test.
    add("relational_descriptor_v3", "slot_" + std::to_string(i + 1),
        wire::SblrValueKind::relational_type_descriptor, Oracle(descriptor));
  }
  Bytes table_bytes;
  std::string handles; std::vector<std::uint32_t> ids;
  if (!count) {
    wire::SblrExpressionLiteralNodeV1 node;
    node.node_id = literal_node_id; node.parent_operand_ordinal = 1;
    node.descriptor_uuid = descriptors[0].descriptor_uuid.bytes;
    node.descriptor_generation = descriptors[0].descriptor_generation;
    const auto literal = wire::EncodeSblrLiteralInt64LeV1(42);
    node.literal_body.assign(literal.begin(), literal.end());
    table_bytes = wire::EncodeSblrExpressionNodeTableV1({{node}});
    const auto hash = scratchbird::core::hash::ComputeSha256Digest(table_bytes);
    Require(!table_bytes.empty() && hash.ok(), "prepared literal table encoding");
    Bytes ref; Append(ref, 1, 2); Append(ref, 0, 2); Append(ref, 1, 4); Append(ref, literal_node_id, 8);
    ref.insert(ref.end(), hash.digest.begin(), hash.digest.end());
    ref.insert(ref.end(), node.descriptor_uuid.begin(), node.descriptor_uuid.end());
    Append(ref, node.descriptor_generation, 8);
    add("relational_expression_v1", "1", wire::SblrValueKind::expression_node_ref, std::move(ref));
    handles = "1"; ids = {1};
  } else {
    wire::SblrParameterNodeTableV1 table;
    for (unsigned i = 0; i < count; ++i) {
      const auto& descriptor = descriptors[i + (match ? 1 : 0)];
      table.nodes.push_back({i + 1, i + 1, i, Id(80).bytes, 9,
                            Id(100 + i).bytes, descriptor.descriptor_generation});
      if (i) handles += ',';
      handles += std::to_string(i + 1); ids.push_back(i + 1);
    }
    table_bytes = wire::EncodeSblrParameterNodeTableV1(table);
    constexpr std::string_view domain = "ScratchBird.SblrParameterNodeTable.V1";
    Bytes material(domain.begin(), domain.end()); material.insert(material.end(), table_bytes.begin(), table_bytes.end());
    const auto hash = scratchbird::core::hash::ComputeSha256Digest(material);
    Require(!table_bytes.empty() && hash.ok(), "prepared parameter table encoding");
    for (unsigned i = 0; i < count; ++i) {
      wire::SblrParameterNodeReferenceV1 ref{i + 1, i + 1, hash.digest, Id(80).bytes, 9, i};
      add("relational_expression_v1", std::to_string(i + 1), wire::SblrValueKind::parameter_node_ref,
          wire::EncodeSblrParameterNodeReferenceV1(ref));
    }
  }
  if (!match) {
    for (unsigned i = 0; i < (count ? count : 1); ++i)
      text("relational_output_v1", "slot_" + std::to_string(i + 1),
          "1|" + std::to_string(i + 1) + "|" + std::to_string(i + 1) + "|1|" + std::to_string(i) + "|78");
    text("relational_values_row_v1", "slot_1", handles);
    text("relational_node_v1", "slot_1", "13|0|-|" + handles + "|1");
    binding(1, "values.literal-table.v1", ids, {}, {});
  } else {
    const Uuid function{{0x01,0x9d,0xff,0xbb,0xf0,0x00,0x7e,0x2c,0xb4,0x37,0xeb,0xbb,0xc2,0xd4,0xf3,0x5b}};
    Expression expression;
    expression.expression_id = count + 1; expression.expression_kind = api::RelationalExpressionKind::kFunctionCall;
    expression.result_descriptor_id = 1; expression.child_expression_ids = ids; expression.function_uuid = function;
    add("relational_expression_v2", "slot_" + std::to_string(count + 1),
        wire::SblrValueKind::relational_expression, ExpressionOracle(expression));
    for (unsigned node = 1; node <= 2; ++node)
      text("relational_output_v1", "slot_" + std::to_string(node),
          std::to_string(node) + "|" + std::to_string(count + 1) + "|1|1|0|67656e65726174655f736572696573");
    text("relational_node_v1", "slot_1", "17|0|-|1|-");
    binding(1, "table-function.generate-series.v1", {count + 1}, {function}, {});
    text("relational_table_function_v1", "slot_1", handles);
    text("relational_node_v1", "slot_2", "16|0|1|1|-");
    binding(2, "match-recognize.a-plus.true.all-rows.v1", {count + 1}, {}, {Id(91), Id(92)});
    api::RelationalPropertyRecord partition, ordering;
    partition.property_uuid = Id(91); partition.origin_node_id = 2;
    partition.property_kind = api::RelationalPropertyKind::kPartitioning; partition.expression_ids = {count + 1};
    ordering.property_uuid = Id(92); ordering.origin_node_id = 2;
    ordering.ordering_terms = {{count + 1, api::RelationalPropertySortDirection::kAscending,
                              api::RelationalPropertyNullPlacement::kNullsLast, {}}};
    api::RelationalRowPatternRecord pattern;
    pattern.pattern_id = 1; pattern.relation_node_id = 2; pattern.partition_expression_ids = {count + 1};
    pattern.ordering_terms = ordering.ordering_terms; pattern.variables = {{"a", 1, {}, false, {}, true}};
    pattern.rows_per_match = api::RelationalRowPatternRowsPerMatch::kAll;
    pattern.after_match_skip = api::RelationalRowPatternAfterMatchSkip::kPastLastRow;
    pattern.maximum_partition_rows = 10000; pattern.maximum_active_states = 2; pattern.maximum_output_rows = 10000;
    pattern.stable_row_identity_tie_break_allowed = true;
    add("relational_row_pattern_v2", "slot_1", wire::SblrValueKind::relational_row_pattern, RowPatternOracle(pattern));
    for (const auto& property : {partition, ordering})
      add("relational_property_v3", "property", wire::SblrValueKind::relational_property, PropertyOracle(property));
  }
  add(count ? "expression.parameter_node_table.v1" : "expression.node_table.v1",
      count ? "parameter_nodes" : "expression_nodes",
      count ? wire::SblrValueKind::parameter_node_table : wire::SblrValueKind::expression_node_table, std::move(table_bytes));
  return op;
}

void TestPreparedRelationalQuery() {
  using Profile = wire::PreparedRelationalQueryProfileV1;
  auto nontrivial_literal = PreparedFixture(0, false, 7);
  Require(wire::ValidatePreparedRelationalQueryV1(nontrivial_literal, Profile::kLiteralValues),
          "literal node identity need not equal its relational expression slot");
  nontrivial_literal.operands[12].value_body[8] = 1;
  Require(!wire::ValidatePreparedRelationalQueryV1(nontrivial_literal, Profile::kLiteralValues),
          "mismatched literal node/reference identity accepted");
  const wire::PreparedRelationalContextV1 context{Id(201), Id(202), Id(203), Id(204), Id(205), Id(206),
      std::numeric_limits<std::uint64_t>::max(), std::numeric_limits<std::uint64_t>::max(), Id(207), Id(5), 4, 5};
  for (const auto [count, match] : {std::pair{0u,false}, {1u,false}, {2u,false}, {16u,false}, {2u,true}, {3u,true}}) {
    const auto original = PreparedFixture(count, match);
    const auto parameters = PreparedAuthority(count, match);
    const auto profile = match ? Profile::kParameterMatchRecognize : count ? Profile::kParameterValues : Profile::kLiteralValues;
    Require(wire::ValidateSblrEnvelope(original).ok, "prepared independent envelope invalid");
    Require(wire::ValidatePreparedRelationalQueryV1(original, profile, &parameters), "prepared binary profile refused");
    for (auto other : {Profile::kLiteralValues, Profile::kParameterValues, Profile::kParameterMatchRecognize})
      if (other != profile) Require(!wire::ValidatePreparedRelationalQueryV1(original, other, &parameters), "prepared profile confused");
    const auto bytes = wire::EncodeSblrEnvelope(original);
    const auto decoded = wire::DecodeSblrEnvelope(bytes);
    Require(decoded.ok && wire::ValidatePreparedRelationalQueryV1(decoded.envelope, profile, &parameters), "prepared SBOP round trip failed");
    auto changed = original;
    Require(wire::RebindPreparedRelationalQueryV1(&changed, context, &parameters), "prepared context rebinding refused");
    Require(wire::ValidatePreparedRelationalQueryV1(changed, profile, &parameters), "rebound query lost canonical shape");
    auto expected = original;
    for (unsigned i = 0; i < 6; ++i) {
      const auto id = Id(201 + i);
      expected.operands[2 + i].value_body.assign(id.bytes.begin(), id.bytes.end());
    }
    for (unsigned i : {8u, 9u}) {
      auto& body = expected.operands[i].value_body; body.resize(16);
      constexpr std::string_view max_u64 = "18446744073709551615";
      Append(body, 20, 8); body.insert(body.end(), max_u64.begin(), max_u64.end());
    }
    for (auto& operand : expected.operands) {
      if (operand.value_kind != wire::SblrValueKind::relational_type_descriptor) continue;
      Descriptor descriptor;
      Require(wire::DecodeRelationalTypeDescriptorV1(operand.value_body.data(),
                  operand.value_body.size(), &descriptor), "expected descriptor decode failed");
      descriptor.statement_receipt_uuid = Id(207);
      operand.value_body = Oracle(descriptor);
    }
    Require(wire::EncodeSblrEnvelope(changed) == wire::EncodeSblrEnvelope(expected),
            "rebinding changed metadata beyond context and descriptor receipt ownership");
    for (unsigned mismatch = 0; mismatch != 3; ++mismatch) {
      auto stale = context;
      if (mismatch == 0) stale.datatype_catalog_snapshot_uuid = Id(208);
      if (mismatch == 1) ++stale.datatype_catalog_generation;
      if (mismatch == 2) ++stale.datatype_registry_generation;
      auto untouched = original;
      Require(!wire::RebindPreparedRelationalQueryV1(&untouched, stale, &parameters) &&
                  wire::EncodeSblrEnvelope(untouched) == bytes,
              "prepared rebind accepted a changed datatype cohort or partially published");
    }
    auto zero_visible = context; zero_visible.snapshot_visible_through_local_transaction_id = 0;
    Require(wire::RebindPreparedRelationalQueryV1(&changed, zero_visible, &parameters), "zero visible horizon refused");
    for (auto member : {&wire::PreparedRelationalContextV1::catalog_epoch_uuid, &wire::PreparedRelationalContextV1::security_context_uuid,
        &wire::PreparedRelationalContextV1::statement_uuid, &wire::PreparedRelationalContextV1::transaction_uuid,
        &wire::PreparedRelationalContextV1::statement_snapshot_uuid, &wire::PreparedRelationalContextV1::statement_metadata_snapshot_uuid,
        &wire::PreparedRelationalContextV1::statement_receipt_uuid, &wire::PreparedRelationalContextV1::datatype_catalog_snapshot_uuid}) {
      for (bool nil : {false, true}) {
        auto invalid = context; if (nil) invalid.*member = {}; else (invalid.*member).bytes[6] = 0x40;
        changed = original;
        Require(!wire::RebindPreparedRelationalQueryV1(&changed, invalid, &parameters) && wire::EncodeSblrEnvelope(changed) == bytes,
                "invalid context identity accepted or partially published");
      }
    }
    auto invalid = context; invalid.local_transaction_id = 0; changed = original;
    Require(!wire::RebindPreparedRelationalQueryV1(&changed, invalid, &parameters) && wire::EncodeSblrEnvelope(changed) == bytes,
            "zero local transaction accepted");
    for (std::size_t index = 0; index < original.operands.size(); ++index) {
      for (unsigned mutation = 0; mutation < 5; ++mutation) {
        changed = original; auto& item = changed.operands[index];
        if (mutation == 0) ++item.ordinal;
        if (mutation == 1) item.type += ".retired";
        if (mutation == 2) item.name += ".wrong";
        if (mutation == 3) item.value = "text shadow";
        if (mutation == 4) item.value_flags = 1;
        Require(!wire::ValidatePreparedRelationalQueryV1(changed, profile, &parameters), "malformed prepared slot accepted");
      }
    }
    // Every allocation in recognition, copying and replacement is faulted.
    changed = original; const auto before_allocations = allocations;
    Require(wire::RebindPreparedRelationalQueryV1(&changed, context, &parameters), "allocation baseline refused");
    const auto sites = allocations - before_allocations;
    Require(sites > 0, "prepared fault coverage empty");
    for (std::size_t site = 0; site < sites; ++site) {
      changed = original; bool published = false;
      fail_after = static_cast<long>(site);
      try { published = wire::RebindPreparedRelationalQueryV1(&changed, context, &parameters); } catch (const std::bad_alloc&) {}
      fail_after = -1;
      Require(!published && wire::EncodeSblrEnvelope(changed) == bytes, "allocation failure partially rebound query");
      ++faults;
    }
  }
  Require(!wire::RebindPreparedRelationalQueryV1(nullptr, context), "null prepared target accepted");
}

void TestPreparedRelationalSubstitutions() {
  using Profile = wire::PreparedRelationalQueryProfileV1;
  const wire::PreparedRelationalContextV1 context{Id(201), Id(202), Id(203), Id(204), Id(205), Id(206), 123, 0, Id(207), Id(5), 4, 5};
  const auto reject = [&](const wire::SblrOperationEnvelope& input, Profile profile,
                          const api::SblrParameterSetSnapshot& parameters) {
    Require(!wire::ValidatePreparedRelationalQueryV1(input, profile, &parameters), "prepared semantic shape substitution accepted");
    // These are canonical envelopes with invalid profile relationships. Their
    // serialization gives an independent full-envelope failure-atomicity check.
    const auto before = wire::EncodeSblrEnvelope(input);
    Require(!before.empty(), "substitution fixture not structurally canonical");
    auto copy = input;
    Require(!wire::RebindPreparedRelationalQueryV1(&copy, context, &parameters) && wire::EncodeSblrEnvelope(copy) == before,
            "invalid shape rebind published partial state");
  };
  for (unsigned count : {0u, 1u, 3u}) {
    auto original = PreparedFixture(count);
    const auto parameters = PreparedAuthority(count);
    const auto profile = count ? Profile::kParameterValues : Profile::kLiteralValues;
    for (unsigned index : {8u, 9u}) {
      for (const std::string_view number : {"", "00", "-1", "+1", "18446744073709551616", "1x"}) {
        auto changed = original; auto& body = changed.operands[index].value_body;
        body.resize(16); Append(body, number.size(), 8); body.insert(body.end(), number.begin(), number.end());
        reject(changed, profile, parameters);
      }
    }
    auto changed = original;
    changed.operands[11].value_body[8 + 15] ^= 1; reject(changed, profile, parameters); // descriptor/reference identity disagreement
    changed = original; changed.operands[11].value_body[88] ^= 1; reject(changed, profile, parameters); // descriptor generation
    changed = original; changed.operands[10].value_body.back() = '2'; reject(changed, profile, parameters); // wrong root
    const auto output_index = count ? 11 + 2 * count : 13;
    changed = original; changed.operands[output_index].value_body[24] = '2'; reject(changed, profile, parameters); // wrong output owner
    changed = original; changed.operands[output_index].value_body.back() = 'z'; reject(changed, profile, parameters); // invalid label hex
    changed = original; changed.operands[output_index].value_body[24 + 2] = '9'; reject(changed, profile, parameters); // nonexistent expression
  }
  auto original = PreparedFixture(2, true);
  const auto parameters = PreparedAuthority(2, true);
  constexpr auto profile = Profile::kParameterMatchRecognize;
  constexpr unsigned tail = 19;
  for (auto index : {16u, tail + 1, tail + 4, tail + 5, tail + 6, tail + 7}) {
    auto changed = original;
    if (index == 16) changed.operands[index].value_body[43] ^= 1; // function identity
    if (index == tail + 1 || index == tail + 4) {
      wire::RelationalNodeBindingRecord binding;
      const auto& bytes = changed.operands[index].value_body;
      Require(wire::DecodeRelationalNodeBindingV1(bytes.data(), bytes.size(), &binding), "binding fixture decode");
      if (index == tail + 1) binding.required_object_uuids[0] = Id(88);
      else binding.delivered_property_uuids[0] = Id(88);
      Require(wire::EncodeRelationalNodeBindingV1(binding, &changed.operands[index].value_body), "binding substitution encoding");
    }
    if (index == tail + 5) changed.operands[index].value_body[36] = 3; // pattern state budget
    if (index == tail + 6) changed.operands[index].value_body[24] = 2; // grouping is not partitioning
    if (index == tail + 7) changed.operands[index].value_body[23] ^= 1; // broken ordering identity
    reject(changed, profile, parameters);
  }
  // Property identities are retained runtime UUIDs, never fixed classifier constants.
  auto changed = original;
  for (unsigned i = 0; i < 2; ++i) {
    const auto id = Id(150 + i);
    std::copy(id.bytes.begin(), id.bytes.end(), changed.operands[tail + 6 + i].value_body.begin() + 8);
  }
  wire::RelationalNodeBindingRecord binding;
  const auto& bytes = changed.operands[tail + 4].value_body;
  Require(wire::DecodeRelationalNodeBindingV1(bytes.data(), bytes.size(), &binding), "property crosslink decode");
  binding.required_property_uuids = {Id(150), Id(151)}; binding.delivered_property_uuids = binding.required_property_uuids;
  Require(wire::EncodeRelationalNodeBindingV1(binding, &changed.operands[tail + 4].value_body), "property crosslink encode");
  Require(wire::ValidatePreparedRelationalQueryV1(changed, profile, &parameters) && wire::RebindPreparedRelationalQueryV1(&changed, context, &parameters),
          "nonconstant linked property identities refused");
  binding.required_property_uuids = {Id(150), Id(150)}; binding.delivered_property_uuids = binding.required_property_uuids;
  Require(wire::EncodeRelationalNodeBindingV1(binding, &changed.operands[tail + 4].value_body), "duplicate property fixture");
  const auto duplicate = Id(150);
  std::copy(duplicate.bytes.begin(), duplicate.bytes.end(), changed.operands[tail + 7].value_body.begin() + 8);
  reject(changed, profile, parameters);
}

void TestPreparedSlotAuthorityFailures() {
  const wire::PreparedRelationalContextV1 context{Id(201), Id(202), Id(203), Id(204), Id(205), Id(206),
      123, 0, Id(207), Id(5), 4, 5};
  for (bool match : {false, true}) {
    const auto original = PreparedFixture(2, match);
    const auto good = PreparedAuthority(2, match);
    const auto profile = match ? wire::PreparedRelationalQueryProfileV1::kParameterMatchRecognize :
                                wire::PreparedRelationalQueryProfileV1::kParameterValues;
    Require(wire::ValidatePreparedRelationalQueryV1(original, profile, &good),
            "distinct issued slot and datatype identities refused");
    const auto reject = [&](const api::SblrParameterSetSnapshot* parameters) {
      Require(!wire::ValidatePreparedRelationalQueryV1(original, profile, parameters),
              "substituted prepared parameter authority admitted");
      auto copy = original;
      Require(!wire::RebindPreparedRelationalQueryV1(&copy, context, parameters) &&
                  wire::EncodeSblrEnvelope(copy) == wire::EncodeSblrEnvelope(original),
              "invalid slot authority partially rebound query");
    };
    reject(nullptr);
    for (unsigned mutation = 0; mutation < 14; ++mutation) {
      auto changed = good;
      switch (mutation) {
        case 0: changed.state = api::SblrParameterSetState::revoked; break;
        case 1: changed.parameter_set_descriptor_uuid = {}; break;
        case 2: ++changed.descriptor_generation; break;
        case 3: changed.slots.pop_back(); break;
        case 4: changed.slots[0].slot_ordinal = 1; break;
        case 5: changed.slots[0].slot_uuid = Id(222); break;
        case 6: changed.slots[0].slot_uuid.bytes[6] = 0x40; break;
        case 7: changed.slots[0].datatype_descriptor_uuid = Id(222); break;
        case 8: changed.slots[0].datatype_descriptor_uuid.bytes[6] = 0x40; break;
        case 9: ++changed.slots[0].datatype_descriptor_generation; break;
        case 10: changed.slots[0].datatype_descriptor_generation = 0; break;
        case 11: changed.slots[0].direction = api::SblrParameterDirection::out; break;
        case 12: changed.slots[0].nullable = false; break;
        case 13: changed.slots[1].slot_uuid = changed.slots[0].slot_uuid; break;
      }
      reject(&changed);
    }
  }
}

namespace name_ipc = scratchbird::parser::ipc;
name_ipc::PsNameResolveRequestV1 NameRequestFixture(unsigned components = 4) {
  name_ipc::PsNameResolveRequestV1 value;
  value.session_uuid = Id(1); value.identifier_profile_uuid = Id(2);
  value.current_schema_uuid = Id(3); value.session_language_uuid = Id(4);
  value.search_path = {Id(5), Id(6)};
  value.source_identifier.qualification_kind = components;
  for (unsigned i = 0; i < components; ++i)
    value.source_identifier.components.push_back({"qualified_component_" + std::to_string(i) + "_\xc3\xa9", bool(i & 1), bool(i & 2)});
  value.source_identifier.source_span_present = true;
  value.source_identifier.source_start_utf8_byte = 0;
  value.source_identifier.source_length_utf8_bytes = 37;
  value.object_kind_filter = {3, 4, 5, 18}; value.required_privileges = {2, 3};
  value.include_grant_summary = true;
  value.catalog_generation = 0x0102030405060708ULL; value.security_epoch = 3; value.policy_generation = 4;
  return value;
}
Bytes NameRequestOracle(const name_ipc::PsNameResolveRequestV1& value) {
  const auto field = [](Bytes& out, unsigned id, const Bytes& bytes) {
    Append(out, id, 2); Append(out, bytes.size(), 4); out.insert(out.end(), bytes.begin(), bytes.end());
  };
  const auto scalar = [](std::uint64_t value, unsigned width) { Bytes result; Append(result, value, width); return result; };
  const auto identity = [](Uuid id) { return Bytes(id.bytes.begin(), id.bytes.end()); };
  const auto codes = [&](const auto& values) { Bytes out = scalar(values.size(), 2); out.insert(out.end(), values.begin(), values.end()); return out; };
  Bytes components = scalar(value.source_identifier.components.size(), 2);
  for (const auto& item : value.source_identifier.components) {
    Bytes part{1, 0}; field(part, 1, {item.spelling.begin(), item.spelling.end()});
    field(part, 2, {static_cast<std::uint8_t>(item.quoted)}); field(part, 3, {static_cast<std::uint8_t>(item.case_folded)});
    Append(components, part.size(), 4); components.insert(components.end(), part.begin(), part.end());
  }
  Bytes qualified{1, 0}; field(qualified, 1, {value.source_identifier.qualification_kind}); field(qualified, 2, components);
  field(qualified, 3, {static_cast<std::uint8_t>(value.source_identifier.source_span_present)});
  field(qualified, 4, scalar(value.source_identifier.source_start_utf8_byte, 8));
  field(qualified, 5, scalar(value.source_identifier.source_length_utf8_bytes, 8));
  Bytes search = scalar(value.search_path.size(), 2);
  for (const auto& id : value.search_path) search.insert(search.end(), id.bytes.begin(), id.bytes.end());
  Bytes out{1, 0}; field(out, 1, identity(value.session_uuid)); field(out, 2, identity(value.identifier_profile_uuid));
  field(out, 3, qualified); field(out, 4, identity(value.current_schema_uuid)); field(out, 5, search);
  field(out, 6, identity(value.session_language_uuid)); field(out, 7, codes(value.object_kind_filter));
  field(out, 8, codes(value.required_privileges)); field(out, 9, {static_cast<std::uint8_t>(value.include_grant_summary)});
  field(out, 10, scalar(value.catalog_generation, 8)); field(out, 11, scalar(value.security_epoch, 8)); field(out, 12, scalar(value.policy_generation, 8));
  return out;
}
std::size_t NameFieldOffset(const Bytes& bytes, unsigned id, std::size_t base = 0) {
  Require(base <= bytes.size() && bytes.size() - base >= 2, "name oracle malformed revision");
  std::size_t cursor = base + 2;
  while (cursor < bytes.size()) {
    Require(bytes.size() - cursor >= 6, "name oracle malformed field");
    const unsigned field = unsigned(bytes[cursor]) | unsigned(bytes[cursor + 1]) << 8;
    const std::uint32_t length = std::uint32_t(bytes[cursor + 2]) | std::uint32_t(bytes[cursor + 3]) << 8 |
        std::uint32_t(bytes[cursor + 4]) << 16 | std::uint32_t(bytes[cursor + 5]) << 24;
    Require(length <= bytes.size() - cursor - 6, "name oracle malformed length");
    if (field == id) return cursor;
    cursor += 6 + length;
  }
  throw std::runtime_error("name oracle missing field");
}
void TestCanonicalNameRequest() {
  const auto fixture = NameRequestFixture(); const auto golden = NameRequestOracle(fixture);
  const auto reject = [&](const Bytes& bytes) {
    auto output = NameRequestFixture(1); const auto before = output;
    const auto allocation_count = allocations;
    Require(!name_ipc::DecodePsNameResolveRequestV1(bytes, 1u << 20, &output), "malformed canonical name request accepted");
    Require(allocations == allocation_count && output == before, "malformed name request allocated or published output");
  };
  const auto roundtrip = [&](const auto& input) {
    const auto expected = NameRequestOracle(input); Bytes bytes{0xaa};
    Require(name_ipc::EncodePsNameResolveRequestV1(input, expected.size(), &bytes) && bytes == expected, "name request differs from TLV oracle");
    name_ipc::PsNameResolveRequestV1 decoded;
    Require(name_ipc::DecodePsNameResolveRequestV1(expected, expected.size(), &decoded) && decoded == input, "name request roundtrip lost fields");
    bytes = {0xaa};
    Require(!name_ipc::EncodePsNameResolveRequestV1(input, expected.size() - 1, &bytes) && bytes == Bytes{0xaa}, "name request exceeded encode budget");
    Require(!name_ipc::DecodePsNameResolveRequestV1(expected, expected.size() - 1, &decoded) && decoded == input, "name request exceeded decode budget");
  };
  for (unsigned count = 1; count <= 4; ++count) {
    auto value = NameRequestFixture(count); roundtrip(value);
    value.source_identifier.source_span_present = false; value.source_identifier.source_length_utf8_bytes = 0; roundtrip(value);
  }
  for (unsigned kind = 1; kind <= 19; ++kind) for (unsigned privilege = 1; privilege <= 12; ++privilege) {
    auto value = fixture; value.object_kind_filter = {static_cast<std::uint8_t>(kind)};
    value.required_privileges = {static_cast<std::uint8_t>(privilege)}; roundtrip(value);
  }
  auto value = fixture; value.search_path.clear(); roundtrip(value);
  value = fixture; value.search_path = {Id(6), Id(5), Id(6)}; roundtrip(value); // retain order, no invented set semantics
  value = fixture; value.search_path.assign(65535, Id(5)); roundtrip(value);
  value.search_path.push_back(Id(6));
  Bytes refused_bytes{0xbb};
  const auto oversized_allocations = allocations;
  Require(!name_ipc::EncodePsNameResolveRequestV1(value, 1u << 24, &refused_bytes) &&
          refused_bytes.size() == 1 && refused_bytes[0] == 0xbb, "unrepresentable name search count accepted");
  Require(allocations == oversized_allocations, "oversized name vector allocated staging bytes");
  value = fixture; value.source_identifier.components[0].spelling.assign(65536, 'x'); roundtrip(value);
  const auto reject_model = [&](const auto& malformed) {
    Bytes output{0xdd}; const auto before = allocations;
    Require(!name_ipc::EncodePsNameResolveRequestV1(malformed, 1u << 20, &output), "invalid name model encoded");
    Require(allocations == before && output.size() == 1 && output[0] == 0xdd, "invalid name model allocated or modified output");
    reject(NameRequestOracle(malformed));
  };
  value = fixture; value.source_identifier.components[3].spelling.clear(); reject_model(value);
  value = fixture; value.source_identifier.components[3].spelling = std::string("n\0x", 3); reject_model(value);
  value = fixture; value.source_identifier.components[3].spelling = "\xed\xa0\x80"; reject_model(value);
  value = fixture; value.required_privileges.clear(); reject_model(value);
  value = fixture; value.object_kind_filter = {3, 3}; reject_model(value);
  value = fixture; value.current_schema_uuid = {}; reject_model(value);
  value = fixture; value.source_identifier.source_span_present = false; reject_model(value);
  for (std::size_t length = 0; length < golden.size(); ++length) reject(Bytes(golden.begin(), golden.begin() + length));
  auto changed = golden; changed.push_back(0); reject(changed);
  for (unsigned field = 1; field <= 12; ++field) {
    const auto offset = NameFieldOffset(golden, field);
    changed = golden; changed[offset] = 0; reject(changed);
    changed = golden; changed[offset] = 13; reject(changed);
    changed = golden; changed[offset + 2] ^= 1; reject(changed);
    changed = golden; std::fill_n(changed.begin() + offset + 2, 4, 0xff); reject(changed);
  }
  for (unsigned field : {1u, 2u, 4u, 6u}) {
    const auto start = NameFieldOffset(golden, field) + 6;
    changed = golden; std::fill_n(changed.begin() + start, 16, 0); reject(changed);
    for (unsigned version = 0; version < 16; ++version) if (version != 7) {
      changed = golden; changed[start + 6] = version << 4; reject(changed);
    }
  }
  for (unsigned field : {7u, 8u}) {
    const auto start = NameFieldOffset(golden, field) + 6;
    changed = golden; changed[start + 3] = changed[start + 2]; reject(changed);
    for (unsigned code : {0u, 20u, 255u}) { changed = golden; changed[start + 2] = code; reject(changed); }
    changed = golden; changed[start] = 0; reject(changed);
  }
  const auto qualified = NameFieldOffset(golden, 3) + 6;
  const auto parts = NameFieldOffset(golden, 2, qualified) + 6;
  const auto component = parts + 2 + 4;
  const auto spelling = NameFieldOffset(golden, 1, component) + 6;
  for (unsigned octet : {0u, 0xc0u, 0xffu}) { changed = golden; changed[spelling] = octet; reject(changed); }
  for (unsigned field : {2u, 3u}) {
    changed = golden; changed[NameFieldOffset(golden, field, component) + 6] = 2; reject(changed);
  }
  changed = golden; changed[qualified] = 2; reject(changed);
  changed = golden; changed[NameFieldOffset(golden, 1, qualified) + 6] = 3; reject(changed);
  changed = golden; changed[NameFieldOffset(golden, 3, qualified) + 6] = 0; reject(changed);
  changed = golden; std::fill_n(changed.begin() + NameFieldOffset(golden, 5, qualified) + 6, 8, 0); reject(changed);
  // A bad final identity must be found before strings/vectors are allocated.
  changed = golden; changed[NameFieldOffset(golden, 5) + 6 + 2 + 16 + 6] = 0x40; reject(changed);
  const Bytes before_bytes{0xfe};
  Bytes encoded;
  const auto before_encode = allocations;
  Require(name_ipc::EncodePsNameResolveRequestV1(fixture, golden.size(), &encoded), "name encode baseline");
  const auto encode_sites = allocations - before_encode;
  name_ipc::PsNameResolveRequestV1 decoded;
  const auto before_decode = allocations;
  Require(name_ipc::DecodePsNameResolveRequestV1(golden, golden.size(), &decoded), "name decode baseline");
  const auto decode_sites = allocations - before_decode;
  Require(encode_sites && decode_sites, "name fault coverage empty");
  for (std::size_t site = 0; site < encode_sites; ++site) {
    encoded = before_bytes; bool threw = false; fail_after = site;
    try { (void)name_ipc::EncodePsNameResolveRequestV1(fixture, golden.size(), &encoded); } catch (const std::bad_alloc&) { threw = true; }
    fail_after = -1; Require(threw && encoded == before_bytes, "name encode allocation published partial bytes"); ++faults;
  }
  const auto sentinel = NameRequestFixture(1);
  for (std::size_t site = 0; site < decode_sites; ++site) {
    decoded = sentinel; bool threw = false; fail_after = site;
    try { (void)name_ipc::DecodePsNameResolveRequestV1(golden, golden.size(), &decoded); } catch (const std::bad_alloc&) { threw = true; }
    fail_after = -1; Require(threw && decoded == sentinel, "name decode allocation published partial request"); ++faults;
  }
  name_ipc::PsNameResolveContextV1 context{fixture.session_uuid, fixture.identifier_profile_uuid, fixture.current_schema_uuid,
      fixture.session_language_uuid, fixture.search_path, fixture.catalog_generation, fixture.security_epoch, fixture.policy_generation};
  Require(name_ipc::MatchesPsNameResolveContextV1(fixture, context), "exact name session context refused");
  for (auto member : {&name_ipc::PsNameResolveContextV1::session_uuid, &name_ipc::PsNameResolveContextV1::identifier_profile_uuid,
      &name_ipc::PsNameResolveContextV1::current_schema_uuid, &name_ipc::PsNameResolveContextV1::session_language_uuid}) {
    auto bad = context; bad.*member = Id(123);
    Require(!name_ipc::MatchesPsNameResolveContextV1(fixture, bad), "cross-context name UUID admitted");
  }
  for (auto member : {&name_ipc::PsNameResolveContextV1::catalog_generation, &name_ipc::PsNameResolveContextV1::security_epoch,
      &name_ipc::PsNameResolveContextV1::policy_generation}) {
    auto bad = context; ++(bad.*member);
    Require(!name_ipc::MatchesPsNameResolveContextV1(fixture, bad), "stale name generation admitted");
  }
  auto bad = context; std::reverse(bad.search_path.begin(), bad.search_path.end());
  Require(!name_ipc::MatchesPsNameResolveContextV1(fixture, bad), "name search-path ordering ignored");
  fail_after = 0; const bool matched = name_ipc::MatchesPsNameResolveContextV1(fixture, context); fail_after = -1;
  Require(matched, "name context comparison allocated");
}

void TestTransactionalNameRequest() {
  name_ipc::PsNameResolveRequestV2 fixture{ NameRequestFixture(), 0x1020304050607080ULL, Id(81), 1 };
  const auto oracle = [](const auto& value) {
    Bytes bytes = NameRequestOracle(value.name);
    bytes[0] = 2;
    Append(bytes, 13, 2); Append(bytes, 24, 4); Append(bytes, value.local_transaction_id, 8);
    bytes.insert(bytes.end(), value.transaction_uuid.bytes.begin(), value.transaction_uuid.bytes.end());
    Append(bytes, 14, 2); Append(bytes, 1, 4); bytes.push_back(value.projection_flags);
    return bytes;
  };
  const auto golden = oracle(fixture);
  const auto roundtrip = [&](const auto& input) {
    const auto expected = oracle(input);
    Bytes encoded{0xaa}; name_ipc::PsNameResolveRequestV2 decoded;
    Require(name_ipc::EncodePsNameResolveRequestV2(input, expected.size(), &encoded) && encoded == expected,
            "transactional name request differs from independent fourteen-field oracle");
    Require(name_ipc::DecodePsNameResolveRequestV2(expected, expected.size(), &decoded) && decoded == input,
            "transactional name request lost transaction, flags or base context");
  };
  for (unsigned parts = 1; parts <= 4; ++parts) {
    for (unsigned flags = 0; flags <= 1; ++flags) {
      auto input = fixture; input.name = NameRequestFixture(parts); input.projection_flags = flags;
      roundtrip(input);
    }
  }
  auto maximum = fixture; maximum.local_transaction_id = std::numeric_limits<std::uint64_t>::max(); roundtrip(maximum);
  const auto reject = [&](const Bytes& bytes) {
    auto output = fixture;
    const auto before = allocations;
    Require(!name_ipc::DecodePsNameResolveRequestV2(bytes, 1u << 20, &output) && output == fixture && allocations == before,
            "invalid transactional name request allocated or changed output");
  };
  for (std::size_t size = 0; size < golden.size(); ++size) reject(Bytes(golden.begin(), golden.begin() + size));
  auto bad = golden; bad.push_back(0); reject(bad);
  for (unsigned revision : {0u, 1u, 3u, 255u}) { bad = golden; bad[0] = revision; reject(bad); }
  for (unsigned field = 1; field <= 14; ++field) {
    bad = golden; bad[NameFieldOffset(golden, field)] = 0; reject(bad);
    bad = golden; ++bad[NameFieldOffset(golden, field) + 2]; reject(bad);
  }
  const auto selector = NameFieldOffset(golden, 13) + 6;
  bad = golden; std::fill_n(bad.begin() + selector, 8, 0); reject(bad);
  bad = golden; std::fill_n(bad.begin() + selector + 8, 16, 0); reject(bad);
  for (unsigned version = 0; version < 16; ++version) {
    if (version == 7) continue;
    bad = golden; bad[selector + 14] = static_cast<std::uint8_t>(version << 4); reject(bad);
  }
  for (unsigned variant = 0; variant <= 255; ++variant) {
    if ((variant & 0xc0) == 0x80) continue;
    bad = golden; bad[selector + 16] = variant; reject(bad);
  }
  for (unsigned flags = 2; flags <= 255; ++flags) { bad = golden; bad.back() = flags; reject(bad); }
  reject(NameRequestOracle(fixture.name));
  name_ipc::PsNameResolveRequestV1 old_output = fixture.name;
  Require(!name_ipc::DecodePsNameResolveRequestV1(golden, golden.size(), &old_output) && old_output == fixture.name,
          "revision1 decoder accepted transactional revision2");
  Bytes encoded{0xaa}; auto output = fixture;
  Require(!name_ipc::EncodePsNameResolveRequestV2(fixture, golden.size() - 1, &encoded) && encoded == Bytes{0xaa},
          "transaction selector escaped encode resource budget");
  Require(!name_ipc::DecodePsNameResolveRequestV2(golden, golden.size() - 1, &output) && output == fixture,
          "transaction selector escaped decode resource budget");
  Require(!name_ipc::DecodePsNameResolveRequestV2(golden, golden.size(), nullptr) &&
              !name_ipc::EncodePsNameResolveRequestV2(fixture, golden.size(), nullptr), "null transactional output admitted");
  const Bytes invalid_output{0xaa};
  for (unsigned invalid = 0; invalid != 4; ++invalid) {
    auto value = fixture;
    if (invalid == 0) value.local_transaction_id = 0;
    if (invalid == 1) value.transaction_uuid = {};
    if (invalid == 2) value.transaction_uuid.bytes[6] = 0x40;
    if (invalid == 3) value.projection_flags = 2;
    const auto before = allocations;
    Require(!name_ipc::EncodePsNameResolveRequestV2(value, golden.size(), &encoded) &&
                encoded == invalid_output && allocations == before,
            "invalid transactional name model changed output");
  }
  name_ipc::PsNameResolveContextV2 context{{fixture.name.session_uuid, fixture.name.identifier_profile_uuid,
      fixture.name.current_schema_uuid, fixture.name.session_language_uuid, fixture.name.search_path,
      fixture.name.catalog_generation, fixture.name.security_epoch, fixture.name.policy_generation},
      fixture.local_transaction_id, fixture.transaction_uuid, true};
  fail_after = 0;
  const bool matched = name_ipc::MatchesPsNameResolveContextV2(fixture, context);
  fail_after = -1;
  Require(matched, "exact transactional context did not match allocation-free");
  for (std::size_t byte = 0; byte < 16; ++byte) {
    auto changed = context; changed.transaction_uuid.bytes[byte] ^= 1;
    Require(!name_ipc::MatchesPsNameResolveContextV2(fixture, changed), "cross-transaction UUID context admitted");
  }
  auto changed = context; changed.local_transaction_id = 0;
  Require(!name_ipc::MatchesPsNameResolveContextV2(fixture, changed), "zero transaction context admitted");
  changed = context; ++changed.local_transaction_id;
  Require(!name_ipc::MatchesPsNameResolveContextV2(fixture, changed), "cross-transaction local id admitted");
  changed = context; changed.relation_projection_allowed = false;
  Require(!name_ipc::MatchesPsNameResolveContextV2(fixture, changed), "projection flag self-enabled capability");
  auto no_projection = fixture; no_projection.projection_flags = 0;
  Require(name_ipc::MatchesPsNameResolveContextV2(no_projection, changed), "identity-only request needs projection capability");
  changed = context; ++changed.name.catalog_generation;
  Require(!name_ipc::MatchesPsNameResolveContextV2(fixture, changed), "wrong transaction metadata generation admitted");
  allocations = 0;
  Require(name_ipc::EncodePsNameResolveRequestV2(fixture, golden.size(), &encoded), "transactional encode fault baseline");
  const auto encode_sites = allocations;
  allocations = 0;
  Require(name_ipc::DecodePsNameResolveRequestV2(golden, golden.size(), &output), "transactional decode fault baseline");
  const auto decode_sites = allocations;
  for (std::size_t site = 0; site < encode_sites; ++site) {
    encoded = {0xaa}; bool threw = false; fail_after = site;
    try { (void)name_ipc::EncodePsNameResolveRequestV2(fixture, golden.size(), &encoded); }
    catch (const std::bad_alloc&) { threw = true; }
    fail_after = -1; Require(threw && encoded == Bytes{0xaa}, "transactional encode fault published partial output"); ++faults;
  }
  const name_ipc::PsNameResolveRequestV2 sentinel{NameRequestFixture(1), 123, Id(99), 0};
  for (std::size_t site = 0; site < decode_sites; ++site) {
    output = sentinel; bool threw = false; fail_after = site;
    try { (void)name_ipc::DecodePsNameResolveRequestV2(golden, golden.size(), &output); }
    catch (const std::bad_alloc&) { threw = true; }
    fail_after = -1; Require(threw && output == sentinel, "transactional decode fault published partial model"); ++faults;
  }
}

name_ipc::PsSessionContextV1 SessionFixture() {
  name_ipc::PsSessionContextV1 value;
  value.session_uuid = Id(1); value.database_uuid = Id(2); value.effective_user_uuid = Id(3); value.principal_uuid = Id(4);
  value.roles = {Id(5), Id(6)}; value.groups = {Id(7)};
  value.default_schema_uuid = Id(8); value.current_schema_uuid = Id(9); value.search_path = {Id(10), Id(11)};
  value.default_language_uuid = Id(12); value.session_language_uuid = Id(13); value.default_charset_uuid = Id(14);
  value.default_collation_uuid = Id(15); value.default_timezone_uuid = Id(16);
  value.catalog_generation = 17; value.security_epoch = 18; value.policy_generation = 19;
  value.name_resolution_epoch = 20; value.resource_epoch = std::numeric_limits<std::uint64_t>::max();
  value.transaction_policy_uuid = Id(22); value.cache_reload_policy_uuid = Id(23); value.disclosure_policy_uuid = Id(24);
  return value;
}
Bytes SessionOracle(const name_ipc::PsSessionContextV1& s) {
  Bytes result{1, 0};
  const auto id = [&](unsigned field, Uuid value) {
    Append(result, field, 2); Append(result, 16, 4); result.insert(result.end(), value.bytes.begin(), value.bytes.end());
  };
  const auto vector = [&](unsigned field, const std::vector<Uuid>& values) {
    Append(result, field, 2); Append(result, 2 + values.size() * 16, 4); Append(result, values.size(), 2);
    for (const auto& value : values) result.insert(result.end(), value.bytes.begin(), value.bytes.end());
  };
  const auto number = [&](unsigned field, std::uint64_t value) { Append(result, field, 2); Append(result, 8, 4); Append(result, value, 8); };
  id(1, s.session_uuid); id(2, s.database_uuid); id(3, s.effective_user_uuid); id(4, s.principal_uuid);
  vector(5, s.roles); vector(6, s.groups); id(7, s.default_schema_uuid); id(8, s.current_schema_uuid); vector(9, s.search_path);
  id(10, s.default_language_uuid); id(11, s.session_language_uuid); id(12, s.default_charset_uuid);
  id(13, s.default_collation_uuid); id(14, s.default_timezone_uuid);
  number(15, s.catalog_generation); number(16, s.security_epoch); number(17, s.policy_generation);
  number(18, s.name_resolution_epoch); number(19, s.resource_epoch);
  id(20, s.transaction_policy_uuid); id(21, s.cache_reload_policy_uuid); id(22, s.disclosure_policy_uuid);
  return result;
}
void TestCanonicalSessionContext() {
  using Transition = name_ipc::PsSessionContextTransitionV1;
  const auto fixture = SessionFixture(); const auto golden = SessionOracle(fixture);
  constexpr std::size_t limit = 1u << 24;
  const auto roundtrip = [&](const auto& value) {
    const auto oracle = SessionOracle(value); Bytes bytes;
    Require(name_ipc::EncodePsSessionContextV1(value, oracle.size(), &bytes) && bytes == oracle, "session TLV differs from independent oracle");
    name_ipc::PsSessionContextV1 decoded;
    Require(name_ipc::DecodePsSessionContextV1(oracle, oracle.size(), &decoded) && decoded == value, "session snapshot lost a field");
    bytes = {0xab};
    Require(!name_ipc::EncodePsSessionContextV1(value, oracle.size() - 1, &bytes) && bytes.size() == 1 && bytes[0] == 0xab,
            "session encoder exceeded negotiated budget");
    Require(!name_ipc::DecodePsSessionContextV1(oracle, oracle.size() - 1, &decoded) && decoded == value,
            "session decoder exceeded budget or published state");
  };
  roundtrip(fixture);
  auto value = fixture; value.roles.clear(); value.groups.clear(); value.search_path.clear(); roundtrip(value);
  for (auto member : {&name_ipc::PsSessionContextV1::roles, &name_ipc::PsSessionContextV1::groups, &name_ipc::PsSessionContextV1::search_path}) {
    value = fixture; (value.*member).assign(65535, Id(55)); roundtrip(value);
    (value.*member).push_back(Id(56)); Bytes output{0xaa}; const auto before = allocations;
    Require(!name_ipc::EncodePsSessionContextV1(value, limit, &output) && output.size() == 1 && output[0] == 0xaa && allocations == before,
            "oversized session identity vector staged allocation or bytes");
  }
  const auto reject = [&](const Bytes& bytes) {
    auto decoded = fixture; const auto before = allocations;
    Require(!name_ipc::DecodePsSessionContextV1(bytes, limit, &decoded) && decoded == fixture && allocations == before,
            "invalid session payload allocated or published state");
  };
  for (std::size_t length = 0; length < golden.size(); ++length) reject(Bytes(golden.begin(), golden.begin() + length));
  auto changed = golden; changed.push_back(0); reject(changed);
  for (unsigned field = 1; field <= 22; ++field) {
    const auto offset = NameFieldOffset(golden, field);
    changed = golden; changed[offset] = 0; reject(changed);
    changed = golden; changed[offset] = 23; reject(changed);
    changed = golden; changed[offset + 2] ^= 1; reject(changed);
    changed = golden; std::fill_n(changed.begin() + offset + 2, 4, 0xff); reject(changed);
  }
  for (unsigned field : {1u, 2u, 3u, 4u, 7u, 8u, 10u, 11u, 12u, 13u, 14u, 20u, 21u, 22u}) {
    const auto offset = NameFieldOffset(golden, field) + 6;
    changed = golden; std::fill_n(changed.begin() + offset, 16, 0); reject(changed);
    for (unsigned version = 0; version < 16; ++version) if (version != 7) {
      changed = golden; changed[offset + 6] = version << 4; reject(changed);
    }
  }
  for (unsigned field : {5u, 6u, 9u}) {
    const auto offset = NameFieldOffset(golden, field) + 6;
    changed = golden; changed[offset] = 0xff; reject(changed);
    changed = golden; changed[offset + 2 + 6] = 0x40; reject(changed);
  }
  std::optional<name_ipc::PsSessionContextV1> snapshot;
  Require(!name_ipc::PublishPsSessionContextV1(golden, limit, fixture.session_uuid, Transition::session_update, &snapshot) && !snapshot,
          "session update created initial binding");
  Require(!name_ipc::PublishPsSessionContextV1(golden, limit, Id(88), Transition::accepted_attach, &snapshot) && !snapshot,
          "attach header and payload session mismatch admitted");
  Require(name_ipc::PublishPsSessionContextV1(golden, limit, fixture.session_uuid, Transition::accepted_attach, &snapshot) && snapshot == fixture,
          "initial accepted attach refused");
  Require(!name_ipc::PublishPsSessionContextV1(golden, limit, fixture.session_uuid, Transition::accepted_attach, &snapshot) && snapshot == fixture,
          "second session attachment installed");
  Require(!name_ipc::PublishPsSessionContextV1(golden, limit, fixture.session_uuid, static_cast<Transition>(99), &snapshot), "unknown session transition admitted");
  for (auto member : {&name_ipc::PsSessionContextV1::session_uuid, &name_ipc::PsSessionContextV1::database_uuid,
      &name_ipc::PsSessionContextV1::effective_user_uuid, &name_ipc::PsSessionContextV1::principal_uuid}) {
    value = fixture; value.*member = Id(90); const auto invalid = SessionOracle(value); const auto before = allocations;
    Require(!name_ipc::PublishPsSessionContextV1(invalid, limit, value.session_uuid, Transition::session_update, &snapshot) &&
            snapshot == fixture && allocations == before, "immutable session binding changed or allocated");
  }
  auto update = fixture;
  update.roles = {Id(30)}; update.groups = {Id(31), Id(32)}; update.search_path = {Id(34), Id(33)};
  update.default_schema_uuid = Id(35); update.current_schema_uuid = Id(36);
  update.default_language_uuid = Id(37); update.session_language_uuid = Id(38); update.default_charset_uuid = Id(39);
  update.default_collation_uuid = Id(40); update.default_timezone_uuid = Id(41);
  update.catalog_generation = 42; update.security_epoch = 43; update.policy_generation = 44;
  update.name_resolution_epoch = 45; update.resource_epoch = 46;
  update.transaction_policy_uuid = Id(47); update.cache_reload_policy_uuid = Id(48); update.disclosure_policy_uuid = Id(49);
  const auto update_bytes = SessionOracle(update);
  Require(name_ipc::PublishPsSessionContextV1(update_bytes, limit, fixture.session_uuid, Transition::session_update, &snapshot) && snapshot == update,
          "complete mutable session update refused or partially retained old fields");
  name_ipc::PsNameResolveContextV1 projected;
  Require(name_ipc::ProjectPsNameResolveContextV1(*snapshot, Id(80), &projected), "published session name-context projection refused");
  auto request = NameRequestFixture();
  request.session_uuid = fixture.session_uuid; request.identifier_profile_uuid = Id(80);
  request.current_schema_uuid = Id(36); request.session_language_uuid = Id(38); request.search_path = {Id(34), Id(33)};
  request.catalog_generation = 42; request.security_epoch = 43; request.policy_generation = 44;
  Require(name_ipc::MatchesPsNameResolveContextV1(request, projected), "name request did not match exact newly published context");
  const auto projected_before = projected;
  Require(!name_ipc::ProjectPsNameResolveContextV1(*snapshot, {}, &projected) &&
          name_ipc::MatchesPsNameResolveContextV1(request, projected), "missing admitted identifier profile fabricated authority");
  Bytes encoded;
  auto before = allocations; Require(name_ipc::EncodePsSessionContextV1(fixture, limit, &encoded), "session encode allocation baseline");
  const auto encode_sites = allocations - before;
  snapshot = fixture; before = allocations;
  Require(name_ipc::PublishPsSessionContextV1(update_bytes, limit, fixture.session_uuid, Transition::session_update, &snapshot), "session publication baseline");
  const auto publish_sites = allocations - before;
  Require(encode_sites && publish_sites, "session fault coverage empty");
  for (std::size_t site = 0; site < encode_sites; ++site) {
    encoded = {0xba}; bool threw = false; fail_after = site;
    try { (void)name_ipc::EncodePsSessionContextV1(fixture, limit, &encoded); } catch (const std::bad_alloc&) { threw = true; }
    fail_after = -1; Require(threw && encoded.size() == 1 && encoded[0] == 0xba, "session encoder fault partially published"); ++faults;
  }
  for (std::size_t site = 0; site < publish_sites; ++site) {
    snapshot = fixture; bool threw = false; fail_after = site;
    try { (void)name_ipc::PublishPsSessionContextV1(update_bytes, limit, fixture.session_uuid, Transition::session_update, &snapshot); }
    catch (const std::bad_alloc&) { threw = true; }
    fail_after = -1; Require(threw && snapshot == fixture, "session update allocation fault partially published"); ++faults;
    snapshot.reset(); threw = false; fail_after = site;
    try { (void)name_ipc::PublishPsSessionContextV1(update_bytes, limit, fixture.session_uuid, Transition::accepted_attach, &snapshot); }
    catch (const std::bad_alloc&) { threw = true; }
    fail_after = -1; Require(threw && !snapshot, "session attach allocation fault installed partial binding"); ++faults;
  }
  projected = projected_before;
  projected.current_schema_uuid = Id(100);
  auto prior_request = request; prior_request.current_schema_uuid = Id(100);
  fail_after = 0; bool threw = false;
  try { (void)name_ipc::ProjectPsNameResolveContextV1(update, Id(80), &projected); } catch (const std::bad_alloc&) { threw = true; }
  fail_after = -1;
  Require(threw && name_ipc::MatchesPsNameResolveContextV1(prior_request, projected), "name context projection fault changed authority"); ++faults;
}

Bytes NameMessages(bool nonempty) {
  namespace mv = scratchbird::wire::message_vector;
  mv::MessageSet set; set.set_uuid = Id(70).bytes; set.registry_generation = 2; set.max_render_bytes = 4096;
  if (nonempty) {
    std::map<std::string, mv::ValueTlv> fields;
    const auto context_identity = Id(71);
    for (const auto* key : {"audit_event_uuid", "catalog_generation", "channel_uuid", "database_uuid", "donor_rendering_hint",
        "donor_rendering_profile_uuid", "execution_ref_uuid", "locale_uuid", "message_template_uuid", "security_generation",
        "server_uuid", "session_uuid", "source_map_ref", "transaction_uuid"}) fields.emplace(key, mv::ValueTlv{key, mv::ValueType::null_value, {}});
    for (const auto* key : {"channel_uuid", "server_uuid", "session_uuid", "redaction_policy_uuid"})
      fields[key] = {key, mv::ValueType::system_uuid, Bytes(context_identity.bytes.begin(), context_identity.bytes.end())};
    for (const auto& [key, value] : {std::pair{"canonical_source_kind", 0u}, {"finality_state", 0u}, {"policy_generation", 3u}, {"registry_epoch", 2u}}) {
      Bytes number; Append(number, value, 8); fields[key] = {key, mv::ValueType::unsigned64, std::move(number)};
    }
    for (const auto& [key, value] : {std::pair{"required_outcome", "reject_and_require_explicit_target"},
        {"retry_class", "never_retry_without_catalog_or_authorization_change"}})
      fields[key] = {key, mv::ValueType::utf8, Bytes(value, value + std::char_traits<char>::length(value))};
    Bytes context; Append(context, 1, 2); Append(context, fields.size(), 2); Append(context, 0, 4);
    for (const auto& [key, field] : fields) {
      Bytes encoded; Require(mv::EncodeValueTlv(field, &encoded) == mv::ValueCodecError::none, "message context fixture TLV");
      context.insert(context.end(), encoded.begin(), encoded.end());
    }
    mv::MessageRecord record; record.vector_uuid = Id(72).bytes; record.canonical_source_uuid = Id(73).bytes;
    record.policy_generation = 3; record.source_component = 2;
    record.text = {"en", "SB_RESOURCE_ALIAS_AMBIGUOUS", "resource.alias.ambiguous", "", ""};
    record.context = {"$context", mv::ValueType::context, std::move(context)};
    set.records.push_back(std::move(record));
  }
  Bytes output;
  Require(mv::EncodeMessageSet(set, &output) == mv::ValueCodecError::none, "canonical name MessageSet fixture");
  return output;
}
Bytes NameReplyOracle(const name_ipc::PsNameResolveResponseV1& r) {
  const auto field = [](Bytes& bytes, unsigned id, const Bytes& body) {
    Append(bytes, id, 2); Append(bytes, body.size(), 4); bytes.insert(bytes.end(), body.begin(), body.end());
  };
  const auto number = [](std::uint64_t n) { Bytes out; Append(out, n, 8); return out; };
  Bytes out{1, 0}; field(out, 1, {r.outcome}); field(out, 2, {r.resolved_object_uuid.bytes.begin(), r.resolved_object_uuid.bytes.end()});
  if (r.resolved_object_kind) field(out, 3, {*r.resolved_object_kind});
  if (r.visible_display_name) field(out, 4, {r.visible_display_name->begin(), r.visible_display_name->end()});
  if (r.grant_summary) {
    Bytes summary{1, 0}; field(summary, 1, number(r.grant_summary->generation));
    Bytes privileges; Append(privileges, r.grant_summary->effective_privileges.size(), 2);
    privileges.insert(privileges.end(), r.grant_summary->effective_privileges.begin(), r.grant_summary->effective_privileges.end());
    field(summary, 2, privileges); field(summary, 3, {r.grant_summary->visibility}); field(out, 5, summary);
  }
  field(out, 6, number(r.catalog_generation)); field(out, 7, number(r.security_epoch)); field(out, 8, number(r.name_resolution_epoch));
  Bytes messages; Append(messages, r.canonical_messages.size(), 4); messages.insert(messages.end(), r.canonical_messages.begin(), r.canonical_messages.end());
  field(out, 9, messages); return out;
}
void TestCanonicalNameReplies() {
  constexpr std::size_t limit = 1u << 20;
  name_ipc::PsNameResolveResponseV1 resolved;
  resolved.outcome = 1; resolved.resolved_object_uuid = Id(55); resolved.resolved_object_kind = 3;
  resolved.visible_display_name = "visible_\xc3\xa9_name";
  resolved.security_epoch = 5; resolved.catalog_generation = 4; resolved.name_resolution_epoch = 6;
  resolved.grant_summary = name_ipc::PsGrantSummaryV1{5, {2, 3, 7}, 2}; resolved.canonical_messages = NameMessages(false);
  const auto errors = NameMessages(true);
  const auto roundtrip = [&](const auto& value, bool permit_empty_hidden) {
    const auto expected = NameReplyOracle(value); Bytes bytes;
    Require(name_ipc::EncodePsNameResolveResponseV1(value, expected.size(), permit_empty_hidden, &bytes) && bytes == expected,
            "name reply differs from independent TLV oracle");
    name_ipc::PsNameResolveResponseV1 decoded;
    Require(name_ipc::DecodePsNameResolveResponseV1(expected, expected.size(), permit_empty_hidden, &decoded) && decoded == value,
            "name reply lost fields or diagnostics");
    Require(!name_ipc::DecodePsNameResolveResponseV1(expected, expected.size() - 1, permit_empty_hidden, &decoded) && decoded == value,
            "name reply exceeded decode budget");
    bytes = {0xcc};
    Require(!name_ipc::EncodePsNameResolveResponseV1(value, expected.size() - 1, permit_empty_hidden, &bytes) && bytes.size() == 1 && bytes[0] == 0xcc,
            "name reply exceeded encode budget");
  };
  for (unsigned kind = 1; kind <= 19; ++kind) {
    auto value = resolved; value.resolved_object_kind = kind; roundtrip(value, false);
    value.grant_summary.reset(); roundtrip(value, false);
    value.visible_display_name = ""; roundtrip(value, false); // present is not absent
  }
  for (unsigned visibility : {1u, 2u}) {
    auto value = resolved; value.grant_summary->visibility = visibility; value.grant_summary->effective_privileges.clear(); roundtrip(value, false);
  }
  const auto reject = [&](const name_ipc::PsNameResolveResponseV1& value, bool policy = false) {
    Bytes output{0xba}; auto decoded = resolved;
    Require(!name_ipc::EncodePsNameResolveResponseV1(value, limit, policy, &output) && output.size() == 1 && output[0] == 0xba,
            "invalid name reply encoded or changed output");
    const auto bytes = NameReplyOracle(value);
    Require(!name_ipc::DecodePsNameResolveResponseV1(bytes, limit, policy, &decoded) && decoded == resolved, "invalid name reply disclosed partial state");
  };
  for (unsigned outcome = 2; outcome <= 6; ++outcome) {
    auto value = resolved; value.outcome = outcome; value.resolved_object_uuid = {};
    value.resolved_object_kind.reset(); value.visible_display_name.reset(); value.grant_summary.reset();
    value.canonical_messages = errors; roundtrip(value, false);
    auto bad = value; bad.resolved_object_uuid = Id(99); reject(bad);
    bad = value; bad.resolved_object_kind = 3; reject(bad);
    bad = value; bad.visible_display_name = ""; reject(bad);
    bad = value; bad.grant_summary = resolved.grant_summary; reject(bad);
    value.canonical_messages = resolved.canonical_messages; reject(value);
    if (outcome == 3) roundtrip(value, true); else reject(value, true);
  }
  auto bad = resolved; bad.resolved_object_uuid = {}; reject(bad);
  bad = resolved; bad.resolved_object_uuid.bytes[6] = 0x40; reject(bad);
  bad = resolved; bad.visible_display_name.reset(); reject(bad);
  bad = resolved; bad.visible_display_name = std::string("visible\0hidden", 14); reject(bad);
  bad = resolved; bad.resolved_object_kind = 20; reject(bad);
  bad = resolved; ++bad.grant_summary->generation; reject(bad);
  bad = resolved; bad.grant_summary->effective_privileges = {3, 3}; reject(bad);
  bad = resolved; bad.grant_summary->effective_privileges = {13}; reject(bad);
  bad = resolved; bad.grant_summary->visibility = 3; reject(bad);
  bad = resolved; bad.canonical_messages[52] ^= 1; reject(bad); // SBMV CRC
  bad = resolved; bad.canonical_messages.clear(); reject(bad); // missing vector is not clean empty set
  const auto golden = NameReplyOracle(resolved);
  const auto reject_bytes = [&](const Bytes& bytes) {
    auto out = resolved;
    Require(!name_ipc::DecodePsNameResolveResponseV1(bytes, limit, false, &out) && out == resolved, "malformed name reply partially published");
  };
  for (std::size_t length = 0; length < golden.size(); ++length) reject_bytes(Bytes(golden.begin(), golden.begin() + length));
  auto changed = golden; changed.push_back(0); reject_bytes(changed);
  for (unsigned field = 1; field <= 9; ++field) {
    const auto start = NameFieldOffset(golden, field);
    changed = golden; changed[start] = 0; reject_bytes(changed);
    changed = golden; changed[start + 2] ^= 1; reject_bytes(changed);
  }
  for (bool nonempty : {false, true}) {
    auto value = resolved; if (nonempty) value.canonical_messages = errors;
    const auto canonical = NameReplyOracle(value);
    Bytes encoded; auto before = allocations;
    Require(name_ipc::EncodePsNameResolveResponseV1(value, limit, false, &encoded), "name reply allocation baseline");
    const auto encode_sites = allocations - before;
    name_ipc::PsNameResolveResponseV1 decoded; before = allocations;
    Require(name_ipc::DecodePsNameResolveResponseV1(canonical, limit, false, &decoded), "name reply decode baseline");
    const auto decode_sites = allocations - before;
    for (std::size_t site = 0; site < encode_sites; ++site) {
      encoded = {0xfa}; bool ok = false; fail_after = site;
      try { ok = name_ipc::EncodePsNameResolveResponseV1(value, limit, false, &encoded); } catch (const std::bad_alloc&) {}
      fail_after = -1; Require(!ok && encoded.size() == 1 && encoded[0] == 0xfa, "name reply fault encoded partial bytes"); ++faults;
    }
    for (std::size_t site = 0; site < decode_sites; ++site) {
      decoded = resolved; bool ok = false; fail_after = site;
      try { ok = name_ipc::DecodePsNameResolveResponseV1(canonical, limit, false, &decoded); } catch (const std::bad_alloc&) {}
      fail_after = -1; Require(!ok && decoded == resolved, "name reply fault disclosed partial object"); ++faults;
    }
  }
  for (unsigned kind = 1; kind <= 4; ++kind) for (unsigned disclosure = 1; disclosure <= 5; ++disclosure) {
    name_ipc::PsUuidRenderRequestV1 request{Id(1), Id(2), static_cast<std::uint8_t>(kind), Id(3), Id(4), static_cast<std::uint8_t>(disclosure)};
    Bytes expected{1, 0};
    for (unsigned field = 1; field <= 6; ++field) {
      Append(expected, field, 2);
      if (field == 3 || field == 6) { Append(expected, 1, 4); expected.push_back(field == 3 ? kind : disclosure); }
      else { Append(expected, 16, 4); const auto id = Id(field > 3 ? field - 1 : field); expected.insert(expected.end(), id.bytes.begin(), id.bytes.end()); }
    }
    Bytes bytes; name_ipc::PsUuidRenderRequestV1 decoded;
    Require(name_ipc::EncodePsUuidRenderRequestV1(request, 104, &bytes) && bytes == expected &&
            name_ipc::DecodePsUuidRenderRequestV1(expected, 104, &decoded) && decoded == request, "UUID render request oracle mismatch");
    Require(!name_ipc::EncodePsUuidRenderRequestV1(request, 103, &bytes) && !name_ipc::DecodePsUuidRenderRequestV1(expected, 103, &decoded),
            "UUID render request budget ignored");
    for (std::size_t size = 0; size < expected.size(); ++size)
      Require(!name_ipc::DecodePsUuidRenderRequestV1(std::span(expected).first(size), 104, &decoded) && decoded == request,
              "truncated UUID render request admitted");
    for (unsigned field = 1; field <= 6; ++field) {
      const auto offset = NameFieldOffset(expected, field);
      auto malformed = expected; malformed[offset] = 0;
      Require(!name_ipc::DecodePsUuidRenderRequestV1(malformed, 104, &decoded) && decoded == request,
              "UUID render request field order accepted");
      malformed = expected; malformed[offset + 2] ^= 1;
      Require(!name_ipc::DecodePsUuidRenderRequestV1(malformed, 104, &decoded) && decoded == request,
              "UUID render request field length accepted");
      if (field == 3 || field == 6) {
        for (unsigned code : {0u, field == 3 ? 5u : 6u, 255u}) {
          auto invalid = request;
          (field == 3 ? invalid.requested_name_class : invalid.disclosure_context) = code;
          bytes = {0xbe}; malformed = expected; malformed[offset + 6] = code;
          Require(!name_ipc::EncodePsUuidRenderRequestV1(invalid, 104, &bytes) && bytes == Bytes{0xbe} &&
                  !name_ipc::DecodePsUuidRenderRequestV1(malformed, 104, &decoded) && decoded == request,
                  "UUID render request unknown enum accepted");
        }
      } else {
        for (unsigned version = 0; version <= 15; ++version) if (version != 7) {
          auto invalid = request;
          auto& id = field == 1 ? invalid.session_uuid : field == 2 ? invalid.object_uuid :
                     field == 4 ? invalid.session_language_uuid : invalid.identifier_profile_uuid;
          id.bytes[6] = version << 4;
          bytes = {0xbe}; malformed = expected; malformed[offset + 12] = version << 4;
          Require(!name_ipc::EncodePsUuidRenderRequestV1(invalid, 104, &bytes) && bytes == Bytes{0xbe} &&
                  !name_ipc::DecodePsUuidRenderRequestV1(malformed, 104, &decoded) && decoded == request,
                  "UUID render request non-system identity accepted");
        }
      }
    }
    const auto before = allocations;
    Require(name_ipc::EncodePsUuidRenderRequestV1(request, 104, &bytes), "UUID render request allocation baseline");
    const auto sites = allocations - before;
    for (std::size_t site = 0; site < sites; ++site) {
      bytes = {0xbf}; bool ok = false; fail_after = site;
      try { ok = name_ipc::EncodePsUuidRenderRequestV1(request, 104, &bytes); } catch (const std::bad_alloc&) {}
      fail_after = -1;
      Require(!ok && bytes == Bytes{0xbf}, "UUID render request fault published partial bytes"); ++faults;
    }
    fail_after = 0;
    const bool no_allocation = name_ipc::DecodePsUuidRenderRequestV1(expected, 104, &decoded);
    fail_after = -1;
    Require(no_allocation && decoded == request, "UUID render request decoding allocated");
  }
}

Bytes RenderReplyOracle(const name_ipc::PsUuidRenderResponseV1& value) {
  const auto field = [](Bytes& out, unsigned id, const Bytes& body) {
    Append(out, id, 2); Append(out, body.size(), 4); out.insert(out.end(), body.begin(), body.end());
  };
  Bytes out{1, 0}; field(out, 1, {value.outcome});
  if (value.display_path) {
    Bytes path; Append(path, value.display_path->size(), 2);
    for (const auto& item : *value.display_path) {
      Append(path, item.size(), 4); path.insert(path.end(), item.begin(), item.end());
    }
    field(out, 2, path);
  }
  if (value.quoted_flags) {
    Bytes flags; Append(flags, value.quoted_flags->size(), 2);
    for (bool flag : *value.quoted_flags) flags.push_back(flag ? 1 : 0);
    field(out, 3, flags);
  }
  field(out, 4, {value.language_uuid.bytes.begin(), value.language_uuid.bytes.end()});
  Bytes epoch; Append(epoch, value.name_resolution_epoch, 8); field(out, 5, epoch);
  Bytes messages; Append(messages, value.canonical_messages.size(), 4);
  messages.insert(messages.end(), value.canonical_messages.begin(), value.canonical_messages.end());
  field(out, 6, messages); return out;
}
void TestCanonicalRenderReplies() {
  constexpr std::size_t limit = 1u << 20;
  name_ipc::PsUuidRenderResponseV1 rendered;
  rendered.outcome = 1; rendered.display_path = {"schema", "name_\xc3\xa9"};
  rendered.quoted_flags = {false, true}; rendered.language_uuid = Id(66);
  rendered.name_resolution_epoch = 0x0102030405060708ULL;
  rendered.canonical_messages = NameMessages(false);
  const auto messages = NameMessages(true);
  const auto roundtrip = [&](const auto& value) {
    const auto expected = RenderReplyOracle(value); Bytes bytes{0xaa}; auto decoded = rendered;
    Require(name_ipc::EncodePsUuidRenderResponseV1(value, expected.size(), &bytes) && bytes == expected,
            "render reply independent TLV oracle mismatch");
    Require(name_ipc::DecodePsUuidRenderResponseV1(expected, expected.size(), &decoded) && decoded == value,
            "render reply lost path, language, flags, epoch or diagnostics");
    Require(!name_ipc::DecodePsUuidRenderResponseV1(expected, expected.size() - 1, &decoded) && decoded == value,
            "render reply decode exceeded budget");
    bytes = {0xaa};
    Require(!name_ipc::EncodePsUuidRenderResponseV1(value, expected.size() - 1, &bytes) && bytes == Bytes{0xaa},
            "render reply encode exceeded budget");
  };
  for (unsigned count = 1; count <= 4; ++count) for (unsigned mask = 0; mask < (1u << count); ++mask) {
    auto value = rendered; value.display_path->assign(count, "path_\xf0\x9f\x8c\x90"); value.quoted_flags->assign(count, false);
    for (unsigned i = 0; i < count; ++i) (*value.quoted_flags)[i] = (mask & (1u << i)) != 0;
    roundtrip(value);
  }
  auto value = rendered; (*value.display_path)[0] = ""; roundtrip(value);
  value = rendered; (*value.display_path)[1].assign(65536, 'a'); roundtrip(value);
  const auto reject_bytes = [&](const Bytes& bytes) {
    auto decoded = rendered;
    Require(!name_ipc::DecodePsUuidRenderResponseV1(bytes, limit, &decoded) && decoded == rendered,
            "invalid render reply disclosed partial state");
  };
  const auto reject = [&](const auto& bad) {
    Bytes encoded{0xac};
    Require(!name_ipc::EncodePsUuidRenderResponseV1(bad, limit, &encoded) && encoded == Bytes{0xac},
            "invalid render reply encoded");
    reject_bytes(RenderReplyOracle(bad));
  };
  for (unsigned outcome = 2; outcome <= 4; ++outcome) {
    value = rendered; value.outcome = outcome; value.display_path.reset(); value.quoted_flags.reset();
    value.language_uuid = {}; value.canonical_messages = messages; roundtrip(value);
    auto bad = value; bad.display_path.emplace(); reject(bad);
    bad = value; bad.quoted_flags.emplace(); reject(bad);
    bad = value; bad.language_uuid = Id(66); reject(bad);
    bad = value; bad.canonical_messages = rendered.canonical_messages; reject(bad);
  }
  for (unsigned outcome : {0u, 5u, 255u}) { value = rendered; value.outcome = outcome; reject(value); }
  value = rendered; value.display_path.reset(); reject(value);
  value = rendered; value.quoted_flags.reset(); reject(value);
  value = rendered; value.display_path->clear(); value.quoted_flags->clear(); reject(value);
  value = rendered; value.display_path->resize(5, "name"); value.quoted_flags->resize(5); reject(value);
  value = rendered; value.quoted_flags->pop_back(); reject(value);
  value = rendered; value.language_uuid = {}; reject(value);
  for (unsigned version = 0; version <= 15; ++version) if (version != 7) {
    value = rendered; value.language_uuid.bytes[6] = version << 4; reject(value);
  }
  for (const auto& text : {std::string("a\0b", 3), std::string("\xc0\x80", 2),
                           std::string("\xed\xa0\x80", 3), std::string("\xf4\x90\x80\x80", 4), std::string("\x80", 1)}) {
    value = rendered; (*value.display_path)[0] = text; reject(value);
  }
  value = rendered; value.canonical_messages.clear(); reject(value);
  value = rendered; value.canonical_messages[52] ^= 1; reject(value);
  const auto golden = RenderReplyOracle(rendered);
  for (std::size_t n = 0; n < golden.size(); ++n) reject_bytes(Bytes(golden.begin(), golden.begin() + n));
  auto changed = golden; changed.push_back(0); reject_bytes(changed);
  changed = golden; changed[0] = 2; reject_bytes(changed);
  for (unsigned field = 1; field <= 6; ++field) {
    const auto offset = NameFieldOffset(golden, field);
    changed = golden; changed[offset] = 0; reject_bytes(changed);
    changed = golden; changed[offset + 2] ^= 1; reject_bytes(changed);
  }
  const auto path_offset = NameFieldOffset(golden, 2) + 6;
  const auto flags_offset = NameFieldOffset(golden, 3) + 6;
  for (unsigned count : {0u, 1u, 3u, 5u, 255u}) {
    changed = golden; changed[path_offset] = count; reject_bytes(changed);
    changed = golden; changed[flags_offset] = count; reject_bytes(changed);
  }
  changed = golden; changed[flags_offset + 2] = 2; reject_bytes(changed);
  changed = golden; changed[path_offset + 2] = 255; reject_bytes(changed);
  for (bool nonempty : {false, true}) {
    value = rendered; if (nonempty) value.canonical_messages = messages;
    (*value.display_path)[0].assign(64, 's'); (*value.display_path)[1].assign(64, 'n');
    const auto canonical = RenderReplyOracle(value); Bytes bytes; auto decoded = rendered;
    auto before = allocations;
    Require(name_ipc::EncodePsUuidRenderResponseV1(value, limit, &bytes), "render encode allocation baseline");
    const auto encode_sites = allocations - before; before = allocations;
    Require(name_ipc::DecodePsUuidRenderResponseV1(canonical, limit, &decoded), "render decode allocation baseline");
    const auto decode_sites = allocations - before;
    for (std::size_t site = 0; site < encode_sites; ++site) {
      bytes = {0xaf}; bool ok = false; fail_after = site;
      try { ok = name_ipc::EncodePsUuidRenderResponseV1(value, limit, &bytes); } catch (const std::bad_alloc&) {}
      fail_after = -1; Require(!ok && bytes == Bytes{0xaf}, "render encode allocation fault published partial bytes"); ++faults;
    }
    for (std::size_t site = 0; site < decode_sites; ++site) {
      decoded = rendered; bool ok = false; fail_after = site;
      try { ok = name_ipc::DecodePsUuidRenderResponseV1(canonical, limit, &decoded); } catch (const std::bad_alloc&) {}
      fail_after = -1; Require(!ok && decoded == rendered, "render decode allocation fault disclosed partial state"); ++faults;
    }
  }
}

void TestContextualDescriptorAgreement() {
  auto literal = Fixture(1 | 2 | 8);
  literal.nullability = api::RelationalNullability::kNonNull;
  literal.codec_id = wire::kContextualTextCodecIdentifierV2;
  wire::ContextualTextLiteralProfileV2 profile;
  profile.literal_descriptor_handle = literal.descriptor_id;
  profile.target_descriptor_handle = literal.descriptor_id + 1;
  profile.descriptor_uuid = literal.descriptor_uuid.bytes;
  profile.type_uuid = literal.type_uuid.bytes;
  profile.collation_uuid = literal.collation_uuid->bytes;
  profile.statement_receipt_uuid = literal.statement_receipt_uuid.bytes;
  profile.catalog_snapshot_uuid = literal.datatype_catalog_snapshot_uuid.bytes;
  profile.descriptor_generation = literal.descriptor_generation;
  profile.type_generation = literal.type_generation;
  profile.codec_version = literal.codec_version;
  profile.codec_generation = literal.codec_generation;
  profile.catalog_generation = literal.datatype_catalog_generation;
  profile.datatype_registry_generation = literal.datatype_registry_generation;
  profile.target_character_limit = 0;
  auto target = literal; target.descriptor_id = profile.target_descriptor_handle;
  target.nullability = api::RelationalNullability::kNullable;
  Require(api::MatchesContextualTextDescriptorV2(literal, profile, false) &&
          api::MatchesContextualTextDescriptorV2(target, profile, true),
          "exact contextual descriptor agreement refused");
  Require(!api::MatchesContextualTextDescriptorV2(literal, profile, true) &&
          !api::MatchesContextualTextDescriptorV2(target, profile, false),
          "literal and target descriptor handles crossed");
  const auto reject = [&](const Descriptor& changed) {
    Require(!api::MatchesContextualTextDescriptorV2(changed, profile, false),
            "changed contextual descriptor accepted");
  };
  for (auto member : {&Descriptor::descriptor_uuid, &Descriptor::type_uuid,
                      &Descriptor::statement_receipt_uuid, &Descriptor::datatype_catalog_snapshot_uuid}) {
    for (std::size_t byte = 0; byte < 16; ++byte) {
      auto changed = literal; (changed.*member).bytes[byte] ^= 1; reject(changed);
    }
  }
  for (std::size_t byte = 0; byte < 16; ++byte) {
    auto changed = literal; changed.collation_uuid->bytes[byte] ^= 1; reject(changed);
  }
  for (auto member : {&Descriptor::descriptor_generation, &Descriptor::type_generation,
                      &Descriptor::codec_generation, &Descriptor::datatype_catalog_generation,
                      &Descriptor::datatype_registry_generation}) {
    auto changed = literal; ++(changed.*member); reject(changed);
  }
  auto changed = literal; ++changed.descriptor_id; reject(changed);
  changed = literal; ++changed.codec_version; reject(changed);
  changed = literal; changed.codec_id += ".other"; reject(changed);
  changed = literal; changed.datatype_identity_authoritative = false; reject(changed);
  changed = literal; changed.collation_uuid.reset(); reject(changed);
  changed = literal; changed.timezone_profile_id = ""; reject(changed);
  changed = literal; changed.width.reset(); reject(changed);
  changed = literal; changed.width = 1; reject(changed);
  changed = literal; changed.precision = 0; reject(changed);
  changed = literal; changed.scale = 0; reject(changed);
  changed = literal; changed.nullability = api::RelationalNullability::kNullable; reject(changed);
  changed = literal; changed.nullability = api::RelationalNullability::kUnknown; reject(changed);
  target.nullability = api::RelationalNullability::kUnknown;
  Require(!api::MatchesContextualTextDescriptorV2(target, profile, true), "unknown target nullability accepted");
  target.nullability = api::RelationalNullability::kNonNull;
  Require(api::MatchesContextualTextDescriptorV2(target, profile, true), "non-null target refused");
  profile.target_character_limit = std::numeric_limits<std::uint32_t>::max();
  literal.width = std::numeric_limits<std::uint32_t>::max();
  Require(api::MatchesContextualTextDescriptorV2(literal, profile, false), "maximum bounded width refused");
  ++profile.target_character_limit;
  Require(!api::MatchesContextualTextDescriptorV2(literal, profile, false), "unrepresentable width accepted");
  profile.target_character_limit = std::numeric_limits<std::uint64_t>::max();
  Require(!api::MatchesContextualTextDescriptorV2(literal, profile, false), "finite width treated as unbounded");
  literal.width.reset();
  Require(api::MatchesContextualTextDescriptorV2(literal, profile, false), "unbounded width refused");
  Bytes bytes; Require(wire::EncodeRelationalTypeDescriptorV1(literal, &bytes), "contextual snapshot encoding refused");
  Descriptor decoded;
  Require(wire::DecodeRelationalTypeDescriptorV1(bytes.data(), bytes.size(), &decoded) &&
          decoded == literal && api::MatchesContextualTextDescriptorV2(decoded, profile, false),
          "contextual snapshot round trip lost identity");
  fail_after = 0;
  const bool no_allocation = api::MatchesContextualTextDescriptorV2(decoded, profile, false);
  fail_after = -1;
  Require(no_allocation, "contextual descriptor comparison allocated or failed");
}
}
namespace {
void TestBinaryTransactionRouting() {
  namespace ipc = scratchbird::parser::ipc;
  Require(ipc::ParserClientConfig{}.require_transaction_routing_v2,
          "execution did not request typed transaction outcomes by default");
  const std::uint64_t local_ids[]{0, 1, 0x0102030405060708ULL,
                                std::numeric_limits<std::uint64_t>::max()};
  for (const auto local_id : local_ids) {
    for (unsigned version = 0; version != 16; ++version) {
      for (unsigned variant = 0; variant != 4; ++variant) {
        auto identity = Id(17);
        identity.bytes[6] = static_cast<std::uint8_t>(version << 4);
        identity.bytes[8] = static_cast<std::uint8_t>(variant << 6);
        const ipc::ParserTransactionSelector selector{local_id, identity};
        const bool expected = local_id != 0 && version == 7 && variant == 2;
        Require(selector.present() == expected,
                "transaction selector accepted an invalid binary system identity");
        for (unsigned route = 0; route != 3; ++route) {
          const ipc::ParserTransactionRouting routing{
              static_cast<ipc::ParserTransactionRoute>(route), selector};
          const auto before = allocations;
          fail_after = 0;
          const bool valid = routing.valid();
          fail_after = -1;
          Require(valid == (route == 1 && expected) && allocations == before,
                  "transaction routing identity validation allocated or selected the wrong route");
          Require(routing.selector.local_transaction_id == local_id &&
                      routing.selector.transaction_uuid == identity,
                  "transaction validation changed the engine-issued selector");
        }
      }
    }
  }
  for (unsigned route = 0; route != 256; ++route) {
    ipc::ParserTransactionRouting routing;
    routing.route = static_cast<ipc::ParserTransactionRoute>(route);
    Require(routing.valid() == (route == 0 || route == 2),
            "absent selector or unknown transaction route was admitted");
    routing.selector.local_transaction_id = 1;
    Require(!routing.valid(), "id-only selector was admitted");
    routing.selector = {0, Id(19)};
    Require(!routing.valid(), "UUID-only selector was admitted");
  }
  ipc::ParserTransactionRouting selected{
      ipc::ParserTransactionRoute::kSelected, {71, Id(21)}};
  Require(selected.valid(), "engine-issued binary transaction selector refused");
  selected.selector.transaction_uuid.bytes[15] ^= 1;
  Require(selected.valid(),
          "structural transaction validation tried to invent engine authority");
  // Equality to an owning session/transaction belongs to server admission;
  // shape validation must not synthesize, repair or authorize a selector.
}
}

namespace {
void TestPublicRelationTypeShape() {
  namespace ipc = scratchbird::parser::ipc;
  using Shape = ipc::PublicRelationTypeShapeV1;
  constexpr auto limit = ipc::kPublicRelationTypeShapeMaximumBytes;
  const Shape sentinel{9, 8, 7, "previous/shape"};
  for (unsigned flags = 0; flags != 16; ++flags) {
    for (const std::uint32_t scalar : {0u, 1u, 0x01020304u, 0xffffffffu}) {
      Shape shape;
      if (flags & 1) shape.width = scalar;
      if (flags & 2) shape.precision = scalar;
      if (flags & 4) shape.scale = scalar;
      if (flags & 8) shape.timezone_profile_id = "bound/timezone";
      Bytes oracle{1, 0, static_cast<std::uint8_t>(flags), 0};
      for (unsigned mask : {1u, 2u, 4u})
        Append(oracle, flags & mask ? scalar : 0, 4);
      if (flags & 8) {
        Append(oracle, 14, 4);
        const std::string timezone = "bound/timezone";
        oracle.insert(oracle.end(), timezone.begin(), timezone.end());
      }
      Bytes encoded{0xaa};
      Require(ipc::EncodePublicRelationTypeShapeV1(shape, limit, &encoded) &&
                  encoded == oracle, "public type shape byte oracle mismatch");
      Shape decoded = sentinel;
      Require(ipc::DecodePublicRelationTypeShapeV1(oracle, limit, &decoded) &&
                  decoded == shape, "public type shape presence/value lost");
      for (std::size_t n = 0; n != oracle.size(); ++n) {
        decoded = sentinel;
        Require(!ipc::DecodePublicRelationTypeShapeV1(
                    std::span<const std::uint8_t>(oracle).first(n), limit, &decoded) &&
                    decoded == sentinel, "truncated shape published state");
      }
      auto trailing = oracle; trailing.push_back(0);
      decoded = sentinel;
      Require(!ipc::DecodePublicRelationTypeShapeV1(trailing, limit, &decoded) &&
                  decoded == sentinel, "surplus shape bytes admitted");
      encoded = {0xaa};
      Require(!ipc::EncodePublicRelationTypeShapeV1(shape, oracle.size() - 1, &encoded) &&
                  encoded == Bytes{0xaa}, "shape encode ignored byte ceiling");
      Require(!ipc::DecodePublicRelationTypeShapeV1(oracle, oracle.size() - 1, &decoded) &&
                  decoded == sentinel, "shape decode ignored byte ceiling");
    }
  }
  Bytes absent(16, 0); absent[0] = 1;
  for (const auto offset : {0u, 1u, 2u, 3u, 4u, 8u, 12u}) {
    for (unsigned bit = 0; bit != 8; ++bit) {
      if (offset == 2 && bit < 4) continue; // optional presence is valid
      auto malformed = absent; malformed[offset] ^= static_cast<std::uint8_t>(1u << bit);
      Shape decoded = sentinel;
      Require(!ipc::DecodePublicRelationTypeShapeV1(malformed, limit, &decoded) &&
                  decoded == sentinel, "noncanonical shape prefix admitted");
    }
  }
  for (const auto& text : {std::string{}, std::string("x\0y", 3),
          std::string("\xc0\xaf", 2), std::string("\xed\xa0\x80", 3),
          std::string("\xf4\x90\x80\x80", 4), std::string(4097, 'x')}) {
    Shape bad; bad.timezone_profile_id = text;
    Bytes output{0xaa};
    Require(!ipc::EncodePublicRelationTypeShapeV1(bad, limit + 1, &output) &&
                output == Bytes{0xaa}, "invalid timezone key encoded");
    auto bytes = absent; bytes[2] = 8; Append(bytes, text.size(), 4);
    bytes.insert(bytes.end(), text.begin(), text.end());
    Shape decoded = sentinel;
    Require(!ipc::DecodePublicRelationTypeShapeV1(bytes, limit + 1, &decoded) &&
                decoded == sentinel, "invalid timezone key decoded");
  }
  Shape maximum{0, 0xffffffffu, 0, std::string(4096, 'z')};
  Bytes encoded;
  Require(ipc::EncodePublicRelationTypeShapeV1(maximum, limit, &encoded) &&
              encoded.size() == limit, "maximum shape refused");
  Shape decoded;
  Require(ipc::DecodePublicRelationTypeShapeV1(encoded, limit, &decoded) &&
              decoded == maximum, "maximum shape changed");
  auto wrong_length = encoded; wrong_length[16] ^= 1;
  decoded = sentinel; allocations = 0;
  const bool bad_length = ipc::DecodePublicRelationTypeShapeV1(wrong_length, limit, &decoded);
  const auto validation_allocations = allocations;
  Require(!bad_length && validation_allocations == 0 && decoded == sentinel,
          "malformed shape allocated or published");
  Bytes oversized(1024 * 1024, 0);
  allocations = 0;
  const bool oversized_ok = ipc::DecodePublicRelationTypeShapeV1(oversized, oversized.size(), &decoded);
  const auto oversized_allocations = allocations;
  Require(!oversized_ok && oversized_allocations == 0 && decoded == sentinel,
          "oversized shape allocated or published");
  // Count and fail every allocation site using fresh outputs, avoiding retained
  // string capacity masking direct-to-caller publication bugs.
  Bytes output;
  allocations = 0;
  Require(ipc::EncodePublicRelationTypeShapeV1(maximum, limit, &output), "shape allocation seed");
  const auto encode_sites = allocations;
  for (std::size_t site = 0; site != encode_sites; ++site) {
    Bytes target{0xaa}; bool threw = false;
    fail_after = static_cast<long>(site);
    try { ipc::EncodePublicRelationTypeShapeV1(maximum, limit, &target); }
    catch (const std::bad_alloc&) { threw = true; }
    fail_after = -1;
    Require(threw && target == Bytes{0xaa}, "shape encode fault published output");
    ++faults;
  }
  Shape fresh;
  allocations = 0;
  Require(ipc::DecodePublicRelationTypeShapeV1(encoded, limit, &fresh), "shape decode allocation seed");
  const auto decode_sites = allocations;
  for (std::size_t site = 0; site != decode_sites; ++site) {
    Shape target = sentinel; bool threw = false;
    fail_after = static_cast<long>(site);
    try { ipc::DecodePublicRelationTypeShapeV1(encoded, limit, &target); }
    catch (const std::bad_alloc&) { threw = true; }
    fail_after = -1;
    Require(threw && target == sentinel, "shape decode fault published output");
    ++faults;
  }
  Require(encode_sites != 0 && decode_sites != 0, "shape fault coverage empty");
  Require(!ipc::EncodePublicRelationTypeShapeV1(maximum, limit, nullptr) &&
              !ipc::DecodePublicRelationTypeShapeV1(encoded, limit, nullptr),
          "null shape outputs accepted");
}

void TestNativeUuidRowField() {
  wire::SblrOperand operand;
  operand.ordinal = 1; operand.type = "row_field_binary16.uuid"; operand.name = "id";
  operand.value_kind = wire::SblrValueKind::literal_typed;
  const auto type = Id(40), row = Id(41);
  operand.value_body.assign(type.bytes.begin(), type.bytes.end());
  operand.value_body.insert(operand.value_body.end(), {32,0,0,0,0,0,0,0});
  operand.value_body.insert(operand.value_body.end(), row.bytes.begin(), row.bytes.end());
  const Bytes uuid{0,0x0a,0x0d,0x7c,0xff,0x5c,0x40,0,0x80,0,0,0,0,0,0,1};
  operand.value_body.insert(operand.value_body.end(), uuid.begin(), uuid.end());
  const auto decoded = wire::DecodeNativeRowField(operand);
  Require(decoded && decoded->row_uuid == row && decoded->payload.size() == 16 &&
          std::equal(uuid.begin(), uuid.end(), reinterpret_cast<const std::uint8_t*>(decoded->payload.data())),
          "UUID row field must preserve raw bytes including arbitrary user UUID versions");
  for (std::size_t size = 0; size < operand.value_body.size(); ++size) {
    auto truncated = operand; truncated.value_body.resize(size);
    Require(!wire::DecodeNativeRowField(truncated), "truncated UUID row field admitted");
  }
  auto envelope = wire::MakeSblrEnvelope("query.execute", "SBLR_QUERY_EXECUTE", "row.codec.fixture");
  envelope.opcode_code = 4615; envelope.parser_package_uuid = Id(6); envelope.registry_snapshot_uuid = Id(7);
  envelope.result_shape = "query_execute_result"; envelope.diagnostic_shape = "diagnostic_vector";
  envelope.operands = {operand};
  const auto sbop = wire::DecodeSblrEnvelope(wire::EncodeSblrEnvelope(envelope));
  Require(sbop.ok && sbop.envelope.operands.size() == 1 &&
          wire::DecodeNativeRowField(sbop.envelope.operands.front()).has_value() &&
          sbop.envelope.operands.front().value_body == operand.value_body,
          "native UUID row field did not survive canonical SBOP framing");
  auto wrong = operand; wrong.value_body.push_back(0); wrong.value_body[16] = 33;
  Require(!wire::DecodeNativeRowField(wrong), "oversized UUID value admitted");
  wrong = operand; wrong.value_body[30] = 0x40;
  Require(!wire::DecodeNativeRowField(wrong), "nonengine row UUID admitted");
  wrong = operand; wrong.type = "row_null_field_binary16.uuid";
  Require(!wire::DecodeNativeRowField(wrong), "null row UUID value retained payload");
  wrong.value_body.resize(40); wrong.value_body[16] = 16;
  Require(wire::DecodeNativeRowField(wrong).has_value(), "null UUID row field rejected");
}

void TestNativeSecurityIdentityOption() {
  namespace api = scratchbird::engine::internal_api;
  api::EngineApiRequest request;
  const auto identity = Id(91);
  const auto sentinel = Id(92);
  const std::string prefix = "internal_group_uuid:";
  const std::string bytes(reinterpret_cast<const char*>(identity.bytes.data()), 16);
  api::EngineUuid output = sentinel;
  request.option_envelopes = {"external_group:example", prefix + bytes};
  Require(api::ReadSecurityIdentityOption(request, "internal_group_uuid", &output) &&
              output == identity, "security identity option must preserve binary16");
  Require(!api::ReadSecurityIdentityOption(request, "internal_group_uuid", nullptr),
          "security identity option accepted null output");
  const auto refuse = [&](std::vector<std::string> options) {
    request.option_envelopes = std::move(options);
    output = sentinel;
    Require(!api::ReadSecurityIdentityOption(request, "internal_group_uuid", &output) &&
                output == sentinel, "security identity option must refuse without output mutation");
  };
  refuse({});
  refuse({prefix});
  refuse({prefix + bytes.substr(0, 15)});
  refuse({prefix + bytes + "x"});
  refuse({prefix + "018f0000-0000-7000-8000-00000000d001"});
  refuse({prefix + std::string(16, '\0')});
  auto wrong_version = bytes;
  wrong_version[6] = static_cast<char>((wrong_version[6] & 0x0f) | 0x40);
  refuse({prefix + wrong_version});
  refuse({prefix + bytes, prefix + bytes});
  refuse({prefix + bytes, prefix + "bad"});
}

void TestNativeShardPlacement() {
  wire::SblrOperationEnvelope envelope;
  const auto id_operand = [](std::string name, Uuid id) {
    wire::SblrOperand operand;
    operand.type = "uuid"; operand.name = std::move(name);
    operand.value_kind = wire::SblrValueKind::uuid_ref;
    operand.value_body.assign(id.bytes.begin(),id.bytes.end());
    return operand;
  };
  envelope.operands = {id_operand("shard_uuid",Id(61)),
                       id_operand("source_filespace_uuid",Id(62)),
                       id_operand("target_filespace_uuid",Id(63))};
  api::EngineShardPlacementDescriptor output;
  Require(wire::ReadNativeShardPlacementDescriptor(envelope,"",&output) &&
          output.shard_uuid==Id(61) && output.source_filespace_uuid==Id(62) &&
          output.target_filespace_uuid==Id(63), "shard descriptor binary16 changed");
  const auto refused = [&](const wire::SblrOperationEnvelope& bad) {
    auto sentinel=output;
    Require(!wire::ReadNativeShardPlacementDescriptor(bad,"",&sentinel) &&
            sentinel.shard_uuid==output.shard_uuid &&
            sentinel.target_filespace_uuid==output.target_filespace_uuid,
            "malformed shard descriptor published output");
  };
  for (std::size_t size : {0U,15U,17U,36U}) {
    auto bad=envelope;bad.operands[0].value_body.resize(size);refused(bad);
  }
  auto bad=envelope;bad.operands[0].value_kind=wire::SblrValueKind::literal_typed;refused(bad);
  bad=envelope;bad.operands[0].value="019d0000-0000-7000-8000-000000000001";refused(bad);
  bad=envelope;bad.operands[0].value_body[6]=0x40;refused(bad);
  bad=envelope;bad.operands.push_back(bad.operands[0]);refused(bad);
  bad=envelope;bad.operands.erase(bad.operands.begin());refused(bad);
  bad=envelope;bad.operands[0].value_flags=1;refused(bad);
  wire::SblrOperand epoch;
  epoch.type="text";epoch.name="placement_epoch";
  epoch.value_kind=wire::SblrValueKind::literal_typed;
  epoch.value_body.resize(24);epoch.value_body[16]=2;
  epoch.value_body.push_back('4');epoch.value_body.push_back('1');
  auto numeric=envelope;numeric.operands.push_back(epoch);
  Require(wire::ReadNativeShardPlacementDescriptor(numeric,"",&output) &&
          output.placement_epoch==41, "shard epoch changed");
  bad=numeric;bad.operands.back().value_body[16]=3;refused(bad);
  bad=numeric;bad.operands.back().value_body.back()='x';refused(bad);
  bad=numeric;bad.operands.push_back(epoch);refused(bad);
  auto prefixed=envelope;
  for (auto& operand:prefixed.operands) operand.name="merge_input_0_"+operand.name;
  Require(wire::ReadNativeShardPlacementDescriptor(prefixed,"merge_input_0_",&output) &&
          output.shard_uuid==Id(61), "merge descriptor binary identity mismatch");
  Require(!wire::ReadNativeShardPlacementDescriptor(prefixed,"merge_input_1_",&output),
          "missing merge descriptor fabricated identity");
}

void TestNativeIdentitySelector() {
  namespace server = scratchbird::server;
  const auto expected = Id(44);
  const std::string bytes(reinterpret_cast<const char*>(expected.bytes.data()),16);
  Require(server::NativeIdentitySelectorMatches(bytes, expected), "native database selector mismatch");
  Require(!server::NativeIdentitySelectorMatches(bytes, Id(45)), "different native selector matched");
  Uuid output = Id(46);
  for (const auto size : {0,15,17,36}) {
    Require(!server::ReadNativeIdentitySelector(std::string(size,'0'), &output) && output == Id(46),
            "bad identity selector admitted or mutated output");
  }
  auto invalid = bytes; invalid[6] = 0x40;
  Require(!server::ReadNativeIdentitySelector(invalid, &output), "nonengine selector admitted");
  Require(!server::NativeIdentitySelectorMatches("01a07c0a-0d5c-7000-8000-ff000000002c",expected),
          "text UUID selector admitted by server");
}

void TestLanguageBundleIdentity() {
  namespace w = scratchbird::wire;
  w::LanguageBundleIdentityV1 input{Id(41), {}, Id(43)};
  w::LanguageBundleIdentityBytesV1 bytes{};
  Require(w::EncodeLanguageBundleIdentityV1(input, &bytes), "bundle identity encode");
  const w::LanguageBundleIdentityBytesV1 expected{
      0x01,0xa0,0x7c,0x0a,0x0d,0x5c,0x70,0,0x80,0,0xff,0,0,0,0,41,
      0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
      0x01,0xa0,0x7c,0x0a,0x0d,0x5c,0x70,0,0x80,0,0xff,0,0,0,0,43};
  Require(bytes == expected, "bundle UUID16 byte oracle");
  w::LanguageBundleIdentityV1 decoded;
  Require(w::DecodeLanguageBundleIdentityV1(bytes, &decoded) && decoded == input,
          "bundle identity round trip");
  const auto saved = decoded;
  for (std::size_t size = 0; size < bytes.size(); ++size) {
    Require(!w::DecodeLanguageBundleIdentityV1(std::span(bytes).first(size), &decoded) && decoded == saved,
            "truncated bundle packet mutated output");
  }
  std::vector<std::uint8_t> trailing(bytes.begin(), bytes.end()); trailing.push_back(0);
  Require(!w::DecodeLanguageBundleIdentityV1(trailing, &decoded) && decoded == saved,
          "bundle trailing byte admitted");
  for (std::size_t offset : {0u, 16u, 32u}) {
    auto invalid = bytes;
    invalid[offset + 6] = 0x40; invalid[offset + 8] = 0x80;
    Require(!w::DecodeLanguageBundleIdentityV1(invalid, &decoded) && decoded == saved,
            "bundle nonengine UUID admitted");
  }
  auto nil_bundle = bytes; std::fill_n(nil_bundle.begin(), 16, 0);
  Require(!w::DecodeLanguageBundleIdentityV1(nil_bundle, &decoded) && decoded == saved,
          "nil bundle admitted");
  input.bundle_uuid = {};
  Require(!w::EncodeLanguageBundleIdentityV1(input, &bytes) && bytes == expected,
          "failed bundle encode changed bytes");
}

void TestBinaryProjectionFields() {
  namespace fields = scratchbird::wire::projection_fields;
  const auto id = Id(0x7c);
  const std::vector<std::string> input = {
      "rpvs1", fields::Identity(id), fields::Identity({}),
      std::string("a\0|b", 4)};
  const auto bytes = fields::Encode(input);
  Require(bytes.size() == 12 + 4 * 4 + 5 + 32 + 4,
          "binary projection field byte count");
  Require(bytes.substr(0, 8) == "SBPF0001" &&
          static_cast<unsigned char>(bytes[8]) == 4 &&
          bytes.substr(25, 16) == fields::Identity(id),
          "projection byte oracle");
  std::vector<std::string_view> decoded;
  Require(fields::Decode(bytes, &decoded) && decoded.size() == input.size(),
          "projection round trip");
  for (std::size_t i=0; i<input.size(); ++i)
    Require(decoded[i] == input[i], "projection field bytes changed");
  Uuid identity;
  Require(fields::Read(decoded[1], &identity) && identity == id,
          "projection identity decode");
  Require(fields::Read(decoded[2], &identity) && identity.is_nil(),
          "projection optional nil identity");
  for (std::size_t n=0; n<bytes.size(); ++n) {
    const auto previous = decoded;
    const auto before = allocations;
    Require(!fields::Decode(std::string_view(bytes).substr(0,n), &decoded),
            "projection accepted truncated carrier");
    Require(allocations == before && decoded == previous,
            "projection malformed carrier allocated or mutated output");
  }
  Require(!fields::Decode(bytes + "x", &decoded), "projection trailing byte");
  auto excessive = bytes; excessive[8] = static_cast<char>(255); excessive[9] = 127;
  Require(!fields::Decode(excessive, &decoded), "projection unbounded count");
  excessive = bytes; excessive[12] = static_cast<char>(255); excessive[15] = 127;
  Require(!fields::Decode(excessive, &decoded), "projection unbounded field");
  Require(!fields::Decode("rpvs1|legacy|text", &decoded), "legacy projection accepted");
  identity=id;
  Require(!fields::Read("019d0000-0000-7000-8000-00000000d711", &identity) && identity==id,
          "text UUID accepted or modified output");
  auto invalid=fields::Identity(id); invalid[6]=0x40;
  Require(!fields::Read(invalid, &identity) && identity==id,
          "invalid system UUID accepted");
}

void TestPublicRelationProjection() {
  namespace ipc = scratchbird::parser::ipc;
  constexpr auto limit = ipc::kPublicRelationProjectionMaximumBytesV4;
  ipc::PublicRelationProjectionV4 p;
  p.descriptor_uuid = Id(1); p.relation_uuid = Id(2); p.schema_uuid = Id(3);
  p.datatype_catalog_snapshot_uuid = Id(4);
  p.descriptor_generation = 1; p.validated_resource_epoch = 2;
  p.datatype_catalog_generation = 3; p.datatype_registry_generation = 4;
  ipc::PublicRelationProjectionColumnV4 c;
  c.column_uuid = Id(5); c.descriptor_uuid = Id(6); c.type_uuid = Id(7);
  c.datatype_descriptor_uuid = Id(8);
  c.canonical_name = "c"; c.descriptor_kind = "scalar"; c.canonical_type_name = "type";
  c.codec_id = "codec"; c.codec_version = 1; c.codec_generation = 2;
  c.descriptor_generation = 3; c.type_generation = 4;
  c.canonical_value_width = 8; c.null_encoding = 2;
  p.columns.push_back(c);
  Bytes expected;
  const auto id = [&](const Uuid& value) {
    expected.insert(expected.end(), value.bytes.begin(), value.bytes.end());
  };
  const auto text = [&](const std::string& value) {
    Append(expected, value.size(), 4);
    expected.insert(expected.end(), value.begin(), value.end());
  };
  id(Id(1)); id(Id(2)); id(Id(3)); Append(expected, 1, 8); Append(expected, 2, 8);
  id(Id(4)); Append(expected, 3, 8); Append(expected, 4, 8); Append(expected, 1, 4);
  Require(expected.size() == 100, "projection header oracle extent");
  id(Id(5)); Append(expected, 0, 4); text("c"); id(Id(6));
  text("scalar"); text("type"); Append(expected, 16, 4);
  const auto shape_offset = expected.size();
  expected.insert(expected.end(), {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0});
  const auto attributes_offset = expected.size();
  Append(expected, 0, 1); id({}); text(""); id({}); text("");
  Append(expected, 0, 4); Append(expected, 0, 4); Append(expected, 0, 4);
  text(""); id(Id(8)); Append(expected, 3, 8); id(Id(7)); Append(expected, 4, 8); text("codec");
  Append(expected, 1, 2); Append(expected, 2, 8); Append(expected, 8, 4);
  Append(expected, 2, 1);
  Require(expected.size() == 308, "projection column oracle extent");
  Bytes encoded;
  Require(ipc::EncodePublicRelationProjectionV4(p, limit, &encoded) && encoded == expected,
          "projection independent byte oracle mismatch");
  ipc::PublicRelationProjectionV4 decoded;
  Require(ipc::DecodePublicRelationProjectionV4(expected, limit, &decoded) && decoded == p,
          "projection binary identity roundtrip mismatch");
  const auto rejects = [&](const Bytes& bad) {
    auto before = p;
    allocations = 0;
    const bool ok = ipc::DecodePublicRelationProjectionV4(bad, limit, &before);
    const auto allocated = allocations;
    Require(!ok && allocated == 0 && before == p,
            "malformed projection allocated or changed caller");
  };
  for (std::size_t n = 0; n != expected.size(); ++n)
    rejects(Bytes(expected.begin(), expected.begin() + n));
  auto bad = expected; bad.push_back(0); rejects(bad);
  for (const auto offset : {0u, 16u, 32u, 64u, 100u, 125u, 236u, 260u}) {
    for (unsigned version = 0; version != 16; ++version)
      for (unsigned variant = 0; variant != 4; ++variant) {
        if (version == 7 && variant == 2) continue;
        bad = expected;
        bad[offset + 6] = static_cast<std::uint8_t>(version << 4);
        bad[offset + 8] = static_cast<std::uint8_t>(variant << 6);
        rejects(bad);
      }
  }
  for (const auto offset : {48u, 56u, 80u, 88u, 96u}) {
    bad = expected; std::fill_n(bad.begin() + offset, offset == 96 ? 4 : 8, 0);
    rejects(bad);
  }
  bad = expected; bad[shape_offset] = 2; rejects(bad);
  for (unsigned bit = 4; bit != 8; ++bit) {
    bad = expected; bad[attributes_offset] |= 1u << bit; rejects(bad);
  }
  bad = expected; std::fill_n(bad.begin() + 96, 4, 0xff); rejects(bad);
  bad = expected; bad[124] = 0; rejects(bad); // NUL column name
  bad = expected; bad[124] = 0xff; rejects(bad);
  bad = expected; std::fill_n(bad.begin() + 120, 4, 0xff); rejects(bad);
  bad.assign(limit + 1, 0); rejects(bad);
  auto scalar = p;
  scalar.columns.front().shape.width = 64;
  scalar.columns.front().scalar_metadata = "width=64;nullable=false";
  Bytes scalar_bytes;
  Require(ipc::EncodePublicRelationProjectionV4(scalar, limit, &scalar_bytes), "consistent scalar metadata refused");
  auto scalar_bad = scalar_bytes;
  const std::string scalar_needle = "width=64";
  const auto scalar_at = std::search(scalar_bad.begin(), scalar_bad.end(), scalar_needle.begin(), scalar_needle.end());
  Require(scalar_at != scalar_bad.end(), "scalar metadata oracle missing");
  scalar_at[6] = '5';
  rejects(scalar_bad);
  scalar.columns.front().scalar_metadata = "width=65";
  Bytes scalar_unchanged{0xcc};
  Require(!ipc::EncodePublicRelationProjectionV4(scalar, limit, &scalar_unchanged) && scalar_unchanged == Bytes{0xcc},
          "conflicting scalar metadata admitted");
  auto identity_text = p;
  identity_text.columns.front().scalar_metadata = "type_uuid=019d0000-0000-7000-8000-00000000d719";
  Bytes refused{0xcc};
  Require(!ipc::EncodePublicRelationProjectionV4(identity_text, limit, &refused) && refused == Bytes{0xcc},
          "textual UUID descriptor attribute entered the binary projection");
  auto duplicate = p; duplicate.columns.push_back(c); duplicate.columns.back().ordinal = 1;
  Bytes unchanged{0xaa};
  Require(!ipc::EncodePublicRelationProjectionV4(p, expected.size() - 1, &unchanged) &&
              unchanged == Bytes{0xaa}, "projection encoder ignored resource ceiling");
  auto previous = p;
  Require(!ipc::DecodePublicRelationProjectionV4(expected, expected.size() - 1, &previous) &&
              previous == p, "projection decoder ignored resource ceiling");
  for (const auto offset : {0u, 16u, 32u, 64u, 100u, 125u, 236u, 260u}) {
    bad = expected; std::fill_n(bad.begin() + offset, 16, 0); rejects(bad);
  }
  Require(!ipc::EncodePublicRelationProjectionV4(duplicate, limit, &unchanged) &&
              unchanged == Bytes{0xaa}, "duplicate projection column admitted");
  bad = expected; bad[96] = 2;
  bad.insert(bad.end(), expected.begin() + 100, expected.end());
  bad[expected.size() + 16] = 1;
  rejects(bad);
  auto resource = p;
  auto& column = resource.columns.front();
  column.charset_uuid = Id(8); column.collation_uuid = Id(9);
  column.charset_name = "utf8"; column.collation_name = "unicode";
  column.charset_min_bytes = 1; column.charset_max_bytes = 4;
  column.charset_variable_width = true; // LOB character_length remains exact zero
  column.shape = {0, 38, 0, std::string(100, 't')};
  column.nullable = column.generated = column.identity_column = true;
  column.canonical_name.assign(100, 'n'); column.codec_id.assign(100, 'c');
  Require(ipc::EncodePublicRelationProjectionV4(resource, limit, &encoded) &&
              ipc::DecodePublicRelationProjectionV4(encoded, limit, &decoded) &&
              decoded == resource, "resource/LOB/shape projection changed");
  column.charset_uuid = {};
  Require(!ipc::EncodePublicRelationProjectionV4(resource, limit, &unchanged) &&
              unchanged == Bytes{0xaa}, "orphan projection resources admitted");
  column.charset_uuid = Id(8);
  auto invalid_resource = resource;
  invalid_resource.columns.front().charset_max_bytes = 0;
  Require(!ipc::EncodePublicRelationProjectionV4(invalid_resource, limit, &unchanged) &&
              unchanged == Bytes{0xaa}, "invalid charset width admitted");
  invalid_resource = resource;
  invalid_resource.columns.front().collation_uuid.bytes[6] = 0x40;
  Require(!ipc::EncodePublicRelationProjectionV4(invalid_resource, limit, &unchanged) &&
              unchanged == Bytes{0xaa}, "non-v7 collation admitted");
  invalid_resource = resource;
  invalid_resource.columns.front().shape.timezone_profile_id = std::string(4097, 't');
  Require(!ipc::EncodePublicRelationProjectionV4(invalid_resource, limit, &unchanged) &&
              unchanged == Bytes{0xaa}, "oversized nested shape admitted");
  allocations = 0; Bytes encoded_again;
  Require(ipc::EncodePublicRelationProjectionV4(resource, limit, &encoded_again),
          "projection fault baseline encode");
  const auto encode_sites = allocations;
  for (std::size_t i = 0; i != encode_sites; ++i) {
    Bytes target{0xaa}; bool threw = false; fail_after = static_cast<long>(i);
    try { ipc::EncodePublicRelationProjectionV4(resource, limit, &target); }
    catch (const std::bad_alloc&) { threw = true; }
    fail_after = -1;
    Require(threw && target == Bytes{0xaa}, "projection encode fault published");
    ++faults;
  }
  ipc::PublicRelationProjectionV4 fresh;
  allocations = 0;
  Require(ipc::DecodePublicRelationProjectionV4(encoded, limit, &fresh), "projection decode baseline");
  const auto decode_sites = allocations;
  for (std::size_t i = 0; i != decode_sites; ++i) {
    auto target = p; bool threw = false; fail_after = static_cast<long>(i);
    try { ipc::DecodePublicRelationProjectionV4(encoded, limit, &target); }
    catch (const std::bad_alloc&) { threw = true; }
    fail_after = -1;
    Require(threw && target == p, "projection decode fault published");
    ++faults;
  }
  Require(encode_sites >= 2 && decode_sites >= 3, "projection fault coverage empty");
}
void TestMgaRelationStorageCodec() {
  api::MgaRelationStorageDescriptor source;
  source.descriptor_uuid = Id(1); source.database_uuid = Id(2); source.schema_uuid = Id(3);
  source.relation_uuid = Id(4); source.primary_filespace_uuid = Id(5);
  source.relation_generation = UINT64_MAX; source.descriptor_generation = 17;
  source.page_size = 8192; source.root_page_number = 0x1020304050607080ULL;
  source.allocation_root_page_number = UINT64_MAX;
  source.required_evidence_kinds = {"relation_descriptor", "row_version", "transaction_inventory", "dirty_manifest"};
  api::MgaRelationColumnStorageDescriptor column;
  column.column_uuid = Id(6); column.column_generation = UINT64_MAX;
  column.canonical_name_key = "a_long_display_name_not_identity";
  column.value_descriptor.descriptor_uuid = Id(7);
  column.value_descriptor.datatype_descriptor_uuid = Id(8);
  column.value_descriptor.datatype_descriptor_generation = UINT64_MAX;
  column.value_descriptor.type_uuid = Id(9);
  column.value_descriptor.charset_uuid = column.charset_uuid = Id(10);
  column.value_descriptor.collation_uuid = column.collation_uuid = Id(11);
  column.value_descriptor.descriptor_kind = "scalar";
  column.value_descriptor.canonical_type_name = "presentation_only";
  column.value_descriptor.encoded_descriptor = "charset_uuid=this_text_is_not_identity_authority";
  column.character_length = 256; column.nullable = false; column.generated = true;
  column.identity_column = true; column.max_inline_bytes = UINT64_MAX;
  source.columns.push_back(column);
  auto second = column; second.column_uuid = Id(12); second.ordinal = 1;
  second.charset_uuid = second.value_descriptor.charset_uuid = {};
  second.collation_uuid = second.value_descriptor.collation_uuid = {};
  source.columns.push_back(second);
  api::MgaRelationIndexStorageDescriptor index;
  index.index_uuid = Id(13); index.family = "btree"; index.profile = "actual_profile_metadata";
  index.unique = true; index.approximate = true;
  index.key_envelopes = {"", "a,b", std::string("x\0y", 3)};
  index.include_columns = {"one|two", "three=four"};
  index.predicate_kind = "bound_expression"; index.predicate_column = "display";
  index.predicate_value = std::string("p\0q", 3);
  source.indexes.push_back(index);
  const auto encoded = api::SerializeMgaRelationStorageDescriptor(source);
  const auto field = [](auto& fields, std::string_view key) -> auto& {
    const auto found = std::find_if(fields.begin(), fields.end(), [&](const auto& f) { return f.first == key; });
    if (found == fields.end()) throw std::runtime_error("missing expected relation field");
    return found->second;
  };
  Require(field(encoded, "identity_encoding") == std::string("\x02\x00", 2), "relation encoding revision oracle");
  const std::vector<std::pair<std::string, Uuid>> identities{
    {"descriptor_uuid", Id(1)}, {"database_uuid", Id(2)}, {"schema_uuid", Id(3)},
    {"relation_uuid", Id(4)}, {"primary_filespace_uuid", Id(5)},
    {"column.0.uuid", Id(6)}, {"column.0.descriptor_uuid", Id(7)},
    {"column.0.charset_uuid", Id(10)}, {"column.0.collation_uuid", Id(11)},
    {"column.1.uuid", Id(12)}, {"column.1.charset_uuid", {}}, {"column.1.collation_uuid", {}},
    {"index.0.uuid", Id(13)}};
  for (const auto& [key, identity] : identities) {
    const auto& bytes = field(encoded, key);
    Require(bytes.size() == 16 && std::equal(identity.bytes.begin(), identity.bytes.end(),
        reinterpret_cast<const std::uint8_t*>(bytes.data())), "production relation field differs from binary16 oracle");
  }
  const Bytes list_oracle{3,0,0,0, 0,0,0,0, 3,0,0,0, 'a',',','b', 3,0,0,0, 'x',0,'y'};
  const auto& keys = field(encoded, "index.0.key_envelopes");
  Require(Bytes(keys.begin(), keys.end()) == list_oracle, "index list differs from count/length oracle");
  const auto decoded = api::DeserializeMgaRelationStorageDescriptor(encoded);
  Require(decoded == source && api::SerializeMgaRelationStorageDescriptor(decoded) == encoded,
      "production descriptor roundtrip lost identities, generations, resources or metadata");
  const auto refused = [&](const auto& fields) {
    const auto result = api::DeserializeMgaRelationStorageDescriptor(fields);
    Require(result.descriptor_uuid.is_nil() && result.relation_uuid.is_nil() && result.columns.empty() &&
        result.indexes.empty() && result.descriptor_status == "invalid_binary_relation_fields",
        "malformed relation fields published partial descriptor authority");
  };
  for (const auto& [key, identity] : identities) {
    for (unsigned mutation = 0; mutation < 6; ++mutation) {
      auto bad = encoded;
      if (mutation == 0) std::erase_if(bad, [&](const auto& f) { return f.first == key; });
      if (mutation == 1) bad.emplace_back(key, field(encoded, key));
      if (mutation == 2) field(bad, key).resize(15);
      if (mutation == 3) field(bad, key).push_back('\0');
      if (mutation == 4) field(bad, key) = "019d3b15-0000-7000-8000-000000000001";
      if (mutation == 5) field(bad, key)[6] = 0x40;
      refused(bad);
    }
  }
  for (const auto* key : {"relation_generation", "descriptor_generation", "column.0.generation", "column.1.generation"}) {
    for (const auto* value : {"", "0", "-1", "+1", "2junk", "18446744073709551616"}) {
      auto bad = encoded; field(bad, key) = value; refused(bad);
    }
    auto bad = encoded; std::erase_if(bad, [&](const auto& f) { return f.first == key; }); refused(bad);
  }
  for (const auto* key : {"column_count", "index_count"}) {
    auto bad = encoded; field(bad, key) = "18446744073709551615"; refused(bad);
  }
  for (unsigned mutation = 0; mutation < 5; ++mutation) {
    auto bad = encoded;
    if (mutation == 0) field(bad, "identity_encoding") = std::string("\x01\x00", 2);
    if (mutation == 1) std::erase_if(bad, [](const auto& f) { return f.first == "identity_encoding"; });
    if (mutation == 2) field(bad, "column.0.datatype_binding_v1").resize(47);
    if (mutation == 3) field(bad, "column.0.charset_uuid").assign(16, '\0');
    if (mutation == 4) bad.emplace_back("column.0.name_key", "competing_name");
    refused(bad);
  }
  for (const auto* key : {"index.0.key_envelopes", "index.0.include_columns"}) {
    for (std::size_t length = 0; length < field(encoded, key).size(); ++length) {
      auto bad = encoded; field(bad, key).resize(length); refused(bad);
    }
    auto bad = encoded; field(bad, key).push_back('\0'); refused(bad);
    bad = encoded; field(bad, key).assign(4, char(0xff)); refused(bad);
  }
  auto invalid_source = source; invalid_source.columns[0].value_descriptor.charset_uuid = Id(99);
  bool rejected = false;
  try { (void)api::SerializeMgaRelationStorageDescriptor(invalid_source); } catch (const std::invalid_argument&) { rejected = true; }
  Require(rejected, "serializer allowed conflicting bound resource identities");
  for (unsigned direction = 0; direction < 2; ++direction) {
    auto destination_fields = encoded; auto destination_descriptor = source;
    auto before = allocations;
    if (direction == 0) destination_fields = api::SerializeMgaRelationStorageDescriptor(source);
    else destination_descriptor = api::DeserializeMgaRelationStorageDescriptor(encoded);
    const auto sites = allocations - before;
    Require(sites != 0, "actual relation codec allocation coverage empty");
    for (std::size_t site = 0; site < sites; ++site) {
      destination_fields = {{"untouched", "destination"}};
      destination_descriptor = {}; destination_descriptor.descriptor_uuid = Id(99);
      fail_after = site; bool threw = false;
      try {
        if (direction == 0) destination_fields = api::SerializeMgaRelationStorageDescriptor(source);
        else destination_descriptor = api::DeserializeMgaRelationStorageDescriptor(encoded);
      } catch (const std::bad_alloc&) { threw = true; }
      fail_after = -1;
      Require(threw && (direction == 0 ? destination_fields == std::vector<std::pair<std::string,std::string>>{{"untouched", "destination"}} :
          destination_descriptor.descriptor_uuid == Id(99) && destination_descriptor.columns.empty()),
          "actual relation codec allocation failure partially published"); ++faults;
    }
  }
}

void TestMgaBinaryIdentityFields() {
  const auto identity = Id(37);
  std::string encoded = "unchanged";
  Require(api::EncodeMgaIdentityField(identity, false, &encoded) && encoded.size() == 16 &&
      std::equal(identity.bytes.begin(), identity.bytes.end(),
                 reinterpret_cast<const std::uint8_t*>(encoded.data())),
      "relation identity differs from independent binary16 oracle");
  std::vector<std::pair<std::string, std::string>> fields{{"owner", encoded}};
  auto decoded = Id(99);
  const auto before = allocations;
  Require(api::ReadMgaIdentityField(fields, "owner", false, &decoded) && decoded == identity && allocations == before,
      "relation identity did not decode allocation-free");
  for (unsigned mutation = 0; mutation < 9; ++mutation) {
    auto bad = fields;
    if (mutation == 0) bad.clear();
    if (mutation == 1) bad.push_back(bad.front());
    if (mutation == 2) bad[0].second.resize(15);
    if (mutation == 3) bad[0].second.push_back('\0');
    if (mutation == 4) bad[0].second = "019d3b15-0000-7000-8000-000000000001";
    if (mutation == 5) bad[0].second[6] = 0x40;
    if (mutation == 6) bad[0].second[8] = 0;
    if (mutation == 7) bad[0].second.assign(16, '\0');
    if (mutation == 8) bad[0].first = "another_owner";
    decoded = Id(99); const auto allocation_before = allocations;
    Require(!api::ReadMgaIdentityField(bad, "owner", false, &decoded) && decoded == Id(99) && allocations == allocation_before,
        "invalid binary identity accepted, allocated or changed output");
  }
  for (unsigned version = 0; version < 16; ++version) {
    auto value = identity; value.bytes[6] = static_cast<std::uint8_t>((version << 4) | 3);
    encoded = "unchanged";
    const auto allocation_before = allocations;
    const bool accepted = api::EncodeMgaIdentityField(value, true, &encoded);
    Require(accepted == (version == 7), "optional identity changed system UUID version policy");
    if (!accepted) Require(encoded == "unchanged" && allocations == allocation_before,
        "invalid identity encode allocated or changed output");
  }
  encoded = "unchanged";
  Require(api::EncodeMgaIdentityField({}, true, &encoded) && encoded == std::string(16, '\0'),
      "absent optional identity must encode sixteen zero octets");
  fields[0].second = encoded; decoded = identity;
  Require(api::ReadMgaIdentityField(fields, "owner", true, &decoded) && decoded.is_nil(),
      "optional absent identity failed decode");
  Require(!api::EncodeMgaIdentityField(identity, false, nullptr) &&
      !api::ReadMgaIdentityField(fields, "owner", true, nullptr), "null identity output admitted");
  for (unsigned optional = 0; optional < 2; ++optional) {
    encoded = "unchanged"; const auto count_before = allocations;
    Require(api::EncodeMgaIdentityField(optional ? api::EngineUuid{} : identity, optional, &encoded),
        "identity encode allocation baseline failed");
    const auto sites = allocations - count_before;
    Require(sites != 0, "identity encode allocation coverage empty");
    for (std::size_t site = 0; site < sites; ++site) {
      encoded = "unchanged"; bool ok = false; fail_after = site;
      try { ok = api::EncodeMgaIdentityField(optional ? api::EngineUuid{} : identity, optional, &encoded); }
      catch (const std::bad_alloc&) {}
      fail_after = -1;
      Require(!ok && encoded == "unchanged", "identity encode failure partially published"); ++faults;
    }
  }
}

void TestBoundColumnCreationIdentity() {
  api::EngineColumnDefinition column;
  column.requested_column_uuid = Id(1); column.ordinal = 0; column.nullable = false;
  column.descriptor.descriptor_uuid = Id(2); column.descriptor.type_uuid = Id(3);
  column.descriptor.datatype_descriptor_uuid = Id(4); column.descriptor.datatype_descriptor_generation = 42;
  column.descriptor.charset_uuid = Id(6); column.descriptor.collation_uuid = Id(7);
  column.descriptor.descriptor_kind = "long_bound_column_descriptor_kind";
  column.descriptor.canonical_type_name = "presentation_label_not_identity_authority";
  column.descriptor.encoded_descriptor = "uninterpreted_metadata_does_not_define_a_uuid";
  std::vector<api::EngineColumnDefinition> cohort{column, column};
  cohort[1].requested_column_uuid = Id(5); cohort[1].ordinal = 1;
  const auto before_cohort = allocations;
  Require(api::HasMgaBoundColumnIdentities(cohort) && allocations == before_cohort,
          "valid bound cohort with shared descriptor allocated or refused");
  Require(!api::HasMgaBoundColumnIdentities({}), "empty creation cohort admitted");
  for (unsigned mutation = 0; mutation < 12; ++mutation) {
    auto bad = cohort;
    if (mutation == 0) bad[1].ordinal = 0;
    if (mutation == 1) bad[1].requested_column_uuid = column.requested_column_uuid;
    if (mutation == 2) bad[1].requested_column_uuid = {};
    if (mutation == 3) bad[1].descriptor.descriptor_uuid = {};
    if (mutation == 4) bad[1].descriptor.datatype_descriptor_uuid = {};
    if (mutation == 5) bad[1].descriptor.datatype_descriptor_generation = 0;
    if (mutation == 6) bad[1].descriptor.type_uuid = {};
    if (mutation == 7) bad[1].requested_column_uuid.bytes[6] = 0x40;
    if (mutation == 8) bad[1].descriptor.descriptor_uuid.bytes[8] = 0;
    if (mutation == 9) { bad[1].descriptor.collation_uuid = Id(9); bad[1].descriptor.collation_uuid.bytes[6] = 0x40; }
    if (mutation == 10) bad[1].descriptor.charset_uuid = {};
    if (mutation == 11) bad[1].descriptor.charset_uuid.bytes[6] = 0x40;
    const auto before = allocations;
    Require(!api::HasMgaBoundColumnIdentities(bad) && allocations == before,
            "invalid creation cohort admitted or allocated");
  }
  api::MgaRelationColumnStorageDescriptor original;
  original.column_uuid = Id(80); original.column_generation = 99; original.ordinal = 7;
  original.canonical_name_key = "existing_canonical_column_name";
  original.value_descriptor = column.descriptor; original.value_descriptor.descriptor_uuid = Id(81);
  original.nullable = true; original.generated = true; original.identity_column = true;
  original.charset_uuid = Id(82); original.collation_uuid = Id(83); original.character_length = 128;
  original.storage_class = "existing_storage_class_is_retained"; original.max_inline_bytes = 4097;
  original.overflow_policy = "existing_overflow_policy_is_retained";
  auto expected = original; expected.column_uuid = column.requested_column_uuid;
  expected.column_generation = UINT64_MAX; expected.ordinal = column.ordinal;
  expected.value_descriptor = column.descriptor; expected.nullable = column.nullable;
  expected.charset_uuid = column.descriptor.charset_uuid; expected.collation_uuid = column.descriptor.collation_uuid;
  auto projected = original;
  Require(api::BindMgaColumnStorageIdentity(column, UINT64_MAX, &projected) && projected == expected,
          "actual storage identity projection lost bound facts or replaced unrelated metadata");
  for (unsigned invalid = 0; invalid < 6; ++invalid) {
    auto bad = column; auto generation = UINT64_MAX;
    if (invalid == 0) generation = 0;
    if (invalid == 1) bad.requested_column_uuid = {};
    if (invalid == 2) bad.descriptor.descriptor_uuid = {};
    if (invalid == 3) bad.descriptor.datatype_descriptor_generation = 0;
    if (invalid == 4) bad.descriptor.type_uuid.bytes[6] = 0x40;
    if (invalid == 5) { bad.descriptor.collation_uuid = Id(9); bad.descriptor.collation_uuid.bytes[8] = 0; }
    projected = original; const auto before = allocations;
    Require(!api::BindMgaColumnStorageIdentity(bad, generation, &projected) && projected == original && allocations == before,
            "invalid storage projection generated a binding or changed output");
  }
  Require(!api::BindMgaColumnStorageIdentity(column, 42, nullptr), "null storage projection output accepted");
  projected = original; auto before = allocations;
  Require(api::BindMgaColumnStorageIdentity(column, UINT64_MAX, &projected), "storage identity projection allocation baseline");
  const auto sites = allocations - before; Require(sites != 0, "storage projection allocation coverage empty");
  for (std::size_t site = 0; site < sites; ++site) {
    projected = original; bool ok = false; fail_after = site;
    try { ok = api::BindMgaColumnStorageIdentity(column, UINT64_MAX, &projected); } catch (const std::bad_alloc&) {}
    fail_after = -1;
    Require(!ok && projected == original, "storage identity projection allocation failure partially published"); ++faults;
  }
}

void TestColumnDatatypeBinding() {
  api::EngineDescriptor fixture;
  fixture.descriptor_uuid = Id(1); fixture.datatype_descriptor_uuid = Id(2); fixture.type_uuid = Id(3);
  fixture.collation_uuid = Id(4); fixture.datatype_descriptor_generation = 0x1020304050607080ULL;
  fixture.descriptor_kind = "occurrence"; fixture.canonical_type_name = "presentation_only";
  fixture.encoded_descriptor = "datatype_descriptor_uuid=not_authority;generation=1";
  Bytes expected{'S','B','D','T',1,0,48,0};
  const auto datatype = Id(2); expected.insert(expected.end(), datatype.bytes.begin(), datatype.bytes.end());
  Append(expected, fixture.datatype_descriptor_generation, 8);
  const auto type = Id(3); expected.insert(expected.end(), type.bytes.begin(), type.bytes.end());
  std::string encoded = "unchanged";
  Require(api::EncodeMgaColumnDatatypeBinding(fixture, &encoded) &&
          Bytes(encoded.begin(), encoded.end()) == expected, "datatype binding differs from independent binary oracle");
  auto decoded = fixture; decoded.datatype_descriptor_uuid = Id(99); decoded.type_uuid = Id(98);
  decoded.datatype_descriptor_generation = 99;
  const auto before_decode = allocations;
  Require(api::DecodeMgaColumnDatatypeBinding(expected, &decoded) && decoded == fixture && allocations == before_decode,
          "datatype binding decode changed occurrence metadata or allocated");
  const std::vector<std::pair<std::string, std::string>> fields{
      {"column.1.datatype_binding_v1", "not this column"},
      {"column.0.datatype_binding_v1", encoded},
      {"column.0.encoded_descriptor", "datatype_descriptor_uuid=text-is-not-authority"}};
  decoded.datatype_descriptor_generation = 7;
  auto before_fields = allocations;
  Require(api::ReadMgaColumnDatatypeBindingField(fields, "column.0.", &decoded) && decoded == fixture && allocations == before_fields,
          "actual metadata field reader lost binary binding or allocated");
  auto invalid_fields = fields; invalid_fields.push_back(fields[1]);
  before_fields = allocations;
  Require(!api::ReadMgaColumnDatatypeBindingField(invalid_fields, "column.0.", &decoded) && decoded == fixture && allocations == before_fields,
          "duplicate datatype binding fields accepted");
  invalid_fields = fields; invalid_fields.erase(invalid_fields.begin() + 1);
  before_fields = allocations;
  Require(!api::ReadMgaColumnDatatypeBindingField(invalid_fields, "column.0.", &decoded) && decoded == fixture && allocations == before_fields,
          "missing binary binding recovered from legacy text");
  Require(!api::ReadMgaColumnDatatypeBindingField(fields, "column.1.", &decoded) && decoded == fixture &&
          !api::ReadMgaColumnDatatypeBindingField(fields, "column.01.", &decoded) && decoded == fixture &&
          !api::ReadMgaColumnDatatypeBindingField(fields, "column.0.", nullptr), "metadata field reader ignored column scope or null output");
  const auto reject = [&](const Bytes& bytes) {
    auto out = fixture; const auto before = allocations;
    Require(!api::DecodeMgaColumnDatatypeBinding(bytes, &out) && out == fixture && allocations == before,
            "invalid datatype binding modified output or allocated");
  };
  for (std::size_t n = 0; n < expected.size(); ++n) reject(Bytes(expected.begin(), expected.begin() + n));
  auto bad = expected; bad.push_back(0); reject(bad);
  for (unsigned offset = 0; offset < 8; ++offset) for (unsigned value = 0; value < 256; ++value) {
    if (value == expected[offset]) continue;
    bad = expected; bad[offset] = value; reject(bad);
  }
  for (unsigned start : {8u, 32u}) {
    bad = expected; std::fill_n(bad.begin() + start, 16, 0); reject(bad);
    for (unsigned version = 0; version < 16; ++version) if (version != 7) {
      bad = expected; bad[start + 6] = version << 4; reject(bad);
    }
    for (unsigned variant = 0; variant < 256; ++variant) if ((variant & 0xc0) != 0x80) {
      bad = expected; bad[start + 8] = variant; reject(bad);
    }
  }
  bad = expected; std::fill_n(bad.begin() + 24, 8, 0); reject(bad);
  for (auto generation : {std::uint64_t{1}, UINT64_MAX}) {
    auto input = fixture; input.datatype_descriptor_generation = generation;
    Require(api::EncodeMgaColumnDatatypeBinding(input, &encoded), "valid datatype generation refused");
    decoded = fixture;
    Require(api::DecodeMgaColumnDatatypeBinding({reinterpret_cast<const std::uint8_t*>(encoded.data()), encoded.size()}, &decoded) && decoded == input,
            "datatype generation was narrowed or defaulted");
  }
  for (unsigned invalid = 0; invalid != 5; ++invalid) {
    auto input = fixture;
    if (invalid == 0) input.datatype_descriptor_uuid = {};
    if (invalid == 1) input.type_uuid = {};
    if (invalid == 2) input.datatype_descriptor_generation = 0;
    if (invalid == 3) input.datatype_descriptor_uuid.bytes[6] = 0x40;
    if (invalid == 4) input.type_uuid.bytes[8] = 0;
    encoded = "unchanged"; const auto before = allocations;
    Require(!api::EncodeMgaColumnDatatypeBinding(input, &encoded) && encoded == "unchanged" && allocations == before,
            "invalid binding encoder used occurrence or text fallback");
  }
  Require(!api::EncodeMgaColumnDatatypeBinding(fixture, nullptr) &&
          !api::DecodeMgaColumnDatatypeBinding(expected, nullptr), "null datatype binding output accepted");
  encoded.clear(); auto before = allocations;
  Require(api::EncodeMgaColumnDatatypeBinding(fixture, &encoded), "datatype binding allocation baseline");
  const auto sites = allocations - before;
  Require(sites != 0, "datatype binding allocation failure coverage empty");
  for (std::size_t site = 0; site < sites; ++site) {
    encoded = "unchanged"; bool ok = false; fail_after = site;
    try { ok = api::EncodeMgaColumnDatatypeBinding(fixture, &encoded); } catch (const std::bad_alloc&) {}
    fail_after = -1; Require(!ok && encoded == "unchanged", "datatype binding fault partially published"); ++faults;
  }
}

void TestTransactionalNameReply() {
  namespace ipc = scratchbird::parser::ipc;
  constexpr std::size_t limit = 2u << 20;
  ipc::PsNameResolveRequestV2 request{NameRequestFixture(), 0x1020304050607080ULL, Id(81), 1};
  ipc::PsNameResolveResponseV2 fixture;
  fixture.name.outcome = 1; fixture.name.resolved_object_uuid = Id(2);
  fixture.name.resolved_object_kind = 3; fixture.name.visible_display_name = "relation";
  fixture.name.catalog_generation = request.name.catalog_generation;
  fixture.name.security_epoch = request.name.security_epoch; fixture.name.name_resolution_epoch = 6;
  fixture.name.canonical_messages = NameMessages(false);
  fixture.local_transaction_id = request.local_transaction_id; fixture.transaction_uuid = request.transaction_uuid;
  fixture.relation_projection.emplace(); auto& p = *fixture.relation_projection;
  p.descriptor_uuid = Id(1); p.relation_uuid = Id(2); p.schema_uuid = Id(3);
  p.datatype_catalog_snapshot_uuid = Id(4); p.descriptor_generation = 1;
  p.validated_resource_epoch = 2; p.datatype_catalog_generation = 3; p.datatype_registry_generation = 4;
  ipc::PublicRelationProjectionColumnV4 column;
  column.column_uuid = Id(5); column.descriptor_uuid = Id(6); column.type_uuid = Id(7);
  column.datatype_descriptor_uuid = Id(8);
  column.canonical_name = "c"; column.descriptor_kind = "scalar"; column.canonical_type_name = "type";
  column.codec_id = "codec"; column.codec_version = 1; column.codec_generation = 2;
  column.descriptor_generation = 3; column.type_generation = 4;
  column.canonical_value_width = 8; column.null_encoding = 2; p.columns.push_back(column);
  // Independent literal layout oracle, including the complete embedded v4 record.
  Bytes projection;
  const auto id = [&](const Uuid& value) { projection.insert(projection.end(), value.bytes.begin(), value.bytes.end()); };
  const auto text = [&](std::string_view value) {
    Append(projection, value.size(), 4); projection.insert(projection.end(), value.begin(), value.end());
  };
  id(Id(1)); id(Id(2)); id(Id(3)); Append(projection, 1, 8); Append(projection, 2, 8);
  id(Id(4)); Append(projection, 3, 8); Append(projection, 4, 8); Append(projection, 1, 4);
  id(Id(5)); Append(projection, 0, 4); text("c"); id(Id(6)); text("scalar"); text("type");
  Append(projection, 16, 4); projection.insert(projection.end(), {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0});
  Append(projection, 0, 1); id({}); text(""); id({}); text("");
  Append(projection, 0, 4); Append(projection, 0, 4); Append(projection, 0, 4);
  text(""); id(Id(8)); Append(projection, 3, 8); id(Id(7)); Append(projection, 4, 8); text("codec");
  Append(projection, 1, 2); Append(projection, 2, 8); Append(projection, 8, 4); Append(projection, 2, 1);
  Require(projection.size() == 308, "transaction reply projection oracle size");
  const auto oracle = [&](const ipc::PsNameResolveResponseV2& value) {
    auto bytes = NameReplyOracle(value.name); bytes[0] = 2;
    Append(bytes, 10, 2); Append(bytes, 24, 4); Append(bytes, value.local_transaction_id, 8);
    bytes.insert(bytes.end(), value.transaction_uuid.bytes.begin(), value.transaction_uuid.bytes.end());
    if (value.relation_projection) {
      Append(bytes, 11, 2); Append(bytes, projection.size() + 2, 4);
      bytes.push_back(1); bytes.push_back(4); bytes.insert(bytes.end(), projection.begin(), projection.end());
    }
    return bytes;
  };
  const auto golden = oracle(fixture);
  const auto roundtrip = [&](const auto& value, bool hidden = false) {
    const auto bytes = oracle(value); Bytes encoded{0xcc}; ipc::PsNameResolveResponseV2 decoded;
    Require(ipc::EncodePsNameResolveResponseV2(value, bytes.size(), hidden, &encoded) && encoded == bytes,
            "transaction reply encoder differs from independent oracle");
    Require(ipc::DecodePsNameResolveResponseV2(bytes, bytes.size(), hidden, &decoded) && decoded == value,
            "transaction reply roundtrip loses metadata");
    encoded = {0xcc};
    Require(!ipc::EncodePsNameResolveResponseV2(value, bytes.size() - 1, hidden, &encoded) && encoded.size() == 1 && encoded[0] == 0xcc,
            "transaction reply encoder ignored whole-payload budget");
    Require(!ipc::DecodePsNameResolveResponseV2(bytes, bytes.size() - 1, hidden, &decoded) && decoded == value,
            "transaction reply decoder ignored whole-payload budget");
    auto old = value.name;
    Require(!ipc::DecodePsNameResolveResponseV1(bytes, limit, hidden, &old) && old == value.name,
            "revision1 decoder accepted revision2 response");
  };
  roundtrip(fixture);
  auto value = fixture; value.local_transaction_id = UINT64_MAX; roundtrip(value);
  value = fixture; value.name.grant_summary = ipc::PsGrantSummaryV1{value.name.security_epoch, {2, 3}, 1}; roundtrip(value);
  for (unsigned kind = 1; kind <= 19; ++kind) {
    value = fixture; value.relation_projection.reset(); value.name.resolved_object_kind = kind; roundtrip(value);
  }
  const auto errors = NameMessages(true);
  for (unsigned outcome = 2; outcome <= 6; ++outcome) {
    value = fixture; value.relation_projection.reset(); value.name.outcome = outcome;
    value.name.resolved_object_uuid = {}; value.name.resolved_object_kind.reset(); value.name.visible_display_name.reset();
    value.name.canonical_messages = errors; roundtrip(value);
    if (outcome == 3) { value.name.canonical_messages = NameMessages(false); roundtrip(value, true); }
  }
  const auto reject = [&](const Bytes& bytes, bool before_allocation = true) {
    auto out = fixture; const auto before = allocations;
    Require(!ipc::DecodePsNameResolveResponseV2(bytes, limit, false, &out) && out == fixture,
            "invalid transaction reply disclosed partial output");
    Require(!before_allocation || allocations == before, "malformed transaction envelope allocated");
  };
  for (std::size_t n = 0; n < golden.size(); ++n) {
    // At field11's boundary this is a structurally valid name-only reply;
    // the request/context check below rejects it for a projection request.
    if (n != NameFieldOffset(golden, 11)) reject(Bytes(golden.begin(), golden.begin() + n));
  }
  auto bad = golden; bad.push_back(0); reject(bad);
  bad = golden; Append(bad, 12, 2); Append(bad, 0, 4); reject(bad);
  value = fixture; value.name.outcome = 6; value.name.resolved_object_uuid = {};
  value.name.resolved_object_kind.reset(); value.name.visible_display_name.reset(); value.name.canonical_messages = errors;
  reject(oracle(value)); // Valid non-resolved base cannot carry any projection.
  value = fixture; value.name.grant_summary = ipc::PsGrantSummaryV1{value.name.security_epoch, {2, 3}, 1};
  bad = oracle(value); bad[NameFieldOffset(bad, 5) + 6] = 2; reject(bad);
  for (unsigned revision : {0u, 1u, 3u, 255u, 256u, 65535u}) {
    bad = golden; bad[0] = revision; bad[1] = revision >> 8; reject(bad);
  }
  reject(NameReplyOracle(fixture.name));
  for (unsigned field : {1u, 2u, 3u, 4u, 6u, 7u, 8u, 9u, 10u, 11u}) {
    const auto offset = NameFieldOffset(golden, field);
    bad = golden; bad[offset] = 0; reject(bad);
    bad = golden; bad[offset + 2] ^= 1; reject(bad);
  }
  const auto tx = NameFieldOffset(golden, 10) + 6;
  const auto ext = NameFieldOffset(golden, 11) + 6;
  bad = golden; std::fill_n(bad.begin() + tx, 8, 0); reject(bad);
  bad = golden; std::fill_n(bad.begin() + tx + 8, 16, 0); reject(bad);
  for (unsigned v = 0; v < 16; ++v) if (v != 7) {
    bad = golden; bad[tx + 14] = v << 4; reject(bad);
  }
  for (unsigned v = 0; v < 256; ++v) {
    if ((v & 0xc0) != 0x80) { bad = golden; bad[tx + 16] = v; reject(bad); }
    if (v != 1) { bad = golden; bad[ext] = v; reject(bad); }
    if (v != 4) { bad = golden; bad[ext + 1] = v; reject(bad); }
  }
  // Complete nested projection must validate before even the MessageSet allocates.
  for (auto offset : {16u, 48u, 56u, 72u, 80u, 88u, 96u}) {
    bad = golden; bad[ext + 2 + offset] ^= 1;
    // Some offsets are valid generation values, tested through context below.
    if (offset == 16 || offset == 48 || offset == 96) reject(bad);
  }
  bad = golden; std::fill_n(bad.begin() + ext + 2 + 56, 8, 0); reject(bad);
  bad = golden; bad[ext + 2 + 100 + 16] = 1; reject(bad); // column ordinal
  bad = golden; bad[NameFieldOffset(golden, 9) + 6 + 4 + 52] ^= 1; reject(bad, false);
  const auto reject_model = [&](const auto& model) {
    Bytes out{0xae}; const auto before = allocations;
    Require(!ipc::EncodePsNameResolveResponseV2(model, limit, false, &out) && out.size() == 1 && out[0] == 0xae && allocations == before,
            "invalid transaction reply model allocated or changed output");
  };
  value = fixture; value.local_transaction_id = 0; reject_model(value);
  value = fixture; value.transaction_uuid = {}; reject_model(value);
  value = fixture; value.transaction_uuid.bytes[6] = 0x40; reject_model(value);
  value = fixture; value.relation_projection->relation_uuid = Id(90); reject_model(value);
  value = fixture; value.relation_projection->columns.clear(); reject_model(value);
  value = fixture; value.name.outcome = 2; reject_model(value);
  ipc::PsNameResolveResponseContextV2 context;
  context.request = {{request.name.session_uuid, request.name.identifier_profile_uuid,
      request.name.current_schema_uuid, request.name.session_language_uuid, request.name.search_path,
      request.name.catalog_generation, request.name.security_epoch, request.name.policy_generation},
      request.local_transaction_id, request.transaction_uuid, true};
  context.name_resolution_epoch = fixture.name.name_resolution_epoch; context.resource_epoch = p.validated_resource_epoch;
  context.datatype_catalog_snapshot_uuid = p.datatype_catalog_snapshot_uuid;
  context.datatype_catalog_generation = p.datatype_catalog_generation; context.datatype_registry_generation = p.datatype_registry_generation;
  fail_after = 0;
  const bool matched = ipc::MatchesPsResolvedNameResponseV2(fixture, request, context);
  fail_after = -1; Require(matched, "exact reply correlation failed or allocated");
  const auto mismatch = [&](const auto& response) {
    Require(!ipc::MatchesPsResolvedNameResponseV2(response, request, context), "mismatched transaction response admitted");
  };
  value = fixture; ++value.local_transaction_id; mismatch(value);
  for (unsigned n = 0; n < 16; ++n) { value = fixture; value.transaction_uuid.bytes[n] ^= 1; mismatch(value); }
  value = fixture; ++value.name.catalog_generation; mismatch(value);
  value = fixture; ++value.name.security_epoch; mismatch(value);
  value = fixture; ++value.name.name_resolution_epoch; mismatch(value);
  value = fixture; value.name.resolved_object_kind = 19; mismatch(value);
  value = fixture; value.relation_projection.reset(); mismatch(value);
  value = fixture; ++value.relation_projection->validated_resource_epoch; mismatch(value);
  value = fixture; ++value.relation_projection->datatype_catalog_generation; mismatch(value);
  value = fixture; ++value.relation_projection->datatype_registry_generation; mismatch(value);
  value = fixture; value.relation_projection->datatype_catalog_snapshot_uuid = Id(90); mismatch(value);
  value = fixture; value.name.outcome = 6; mismatch(value);
  auto changed_request = request; changed_request.projection_flags = 0;
  Require(!ipc::MatchesPsResolvedNameResponseV2(fixture, changed_request, context), "unsolicited projection accepted");
  value = fixture; value.relation_projection.reset();
  Require(ipc::MatchesPsResolvedNameResponseV2(value, changed_request, context), "name-only response correlation failed");
  value.name.grant_summary = ipc::PsGrantSummaryV1{value.name.security_epoch, {2}, 1};
  changed_request.name.include_grant_summary = false;
  Require(!ipc::MatchesPsResolvedNameResponseV2(value, changed_request, context), "unsolicited grants disclosed");
  auto changed_context = context; changed_context.request.relation_projection_allowed = false;
  Require(!ipc::MatchesPsResolvedNameResponseV2(fixture, request, changed_context), "unadmitted projection capability accepted");
  changed_context = context; ++changed_context.request.name.policy_generation;
  Require(!ipc::MatchesPsResolvedNameResponseV2(fixture, request, changed_context), "stale request context accepted");
  Require(!ipc::EncodePsNameResolveResponseV2(fixture, limit, false, nullptr) &&
          !ipc::DecodePsNameResolveResponseV2(golden, limit, false, nullptr), "null response output accepted");
  for (bool projected : {false, true}) for (bool messages : {false, true}) {
    value = fixture; if (!projected) value.relation_projection.reset(); if (messages) value.name.canonical_messages = errors;
    const auto canonical = oracle(value); Bytes encoded;
    auto before = allocations;
    Require(ipc::EncodePsNameResolveResponseV2(value, limit, false, &encoded), "transaction reply encode baseline");
    const auto encode_sites = allocations - before;
    ipc::PsNameResolveResponseV2 decoded; before = allocations;
    Require(ipc::DecodePsNameResolveResponseV2(canonical, limit, false, &decoded), "transaction reply decode baseline");
    const auto decode_sites = allocations - before;
    Require(encode_sites && decode_sites, "transaction reply fault coverage empty");
    for (std::size_t site = 0; site < encode_sites; ++site) {
      encoded = {0xcc}; bool ok = false; fail_after = site;
      try { ok = ipc::EncodePsNameResolveResponseV2(value, limit, false, &encoded); } catch (const std::bad_alloc&) {}
      fail_after = -1; Require(!ok && encoded.size() == 1 && encoded[0] == 0xcc, "reply encode fault partially published"); ++faults;
    }
    for (std::size_t site = 0; site < decode_sites; ++site) {
      decoded = fixture; bool ok = false; fail_after = site;
      try { ok = ipc::DecodePsNameResolveResponseV2(canonical, limit, false, &decoded); } catch (const std::bad_alloc&) {}
      fail_after = -1; Require(!ok && decoded == fixture, "reply decode fault partially published"); ++faults;
    }
  }
}
} // namespace

int main() {
  try {
    TestLayout(); TestAllocationAtomicity(); TestOperationPlacement(); TestNumericBinding(); TestFrozenOperands();
    TestReservationSnapshot();
    TestContextIdentityOperands();
    TestDmlIdentityOperands();
    TestSingleDdlTargetIdentity();
    TestSecurityDclIdentityPairs();
    TestSecurityPolicyIdentityCohorts();
    TestProjectionFunctionIdentityTransport();
    TestBoundDmlIdentityPublication();
    TestBoundObjectEngineProjection();
    TestRelatedObjectIdentityProjection();
    TestDmlRegistryBinaryProjection();
    TestEngineIssuedFunctionIdentities();
    TestNativeDescriptorBinaryKeys();
    TestBinaryTransactionRouting();
    TestNativeArtifactPublication();
    TestContextualDescriptorAgreement();
    TestExpressionLayout(); TestExpressionPlacementAndFaults(); TestExpressionReservationFreeze();
    TestNodeBinding();
    TestWindowInvocation();
    TestWindowDefinition();
    TestProperty();
    TestPropertyIdentityLifecycle();
    TestRowPattern();
    TestPreparedRelationalQuery();
  TestPreparedSlotAuthorityFailures();
    TestPreparedRelationalSubstitutions();
    TestCanonicalNameRequest();
    TestTransactionalNameRequest();
    TestCanonicalSessionContext();
    TestCanonicalNameReplies();
    TestTransactionalNameReply();
    TestColumnDatatypeBinding();
    TestBoundColumnCreationIdentity();
    TestMgaBinaryIdentityFields();
    TestMgaRelationStorageCodec();
    TestCanonicalRenderReplies();
    TestPublicRelationTypeShape();
    TestPublicRelationProjection();
    TestBinaryProjectionFields();
    TestLanguageBundleIdentity();
    TestNativeUuidRowField();
    TestNativeIdentitySelector();
    TestNativeShardPlacement();
    TestNativeSecurityIdentityOption();
    std::cout << "PASS relational descriptor checks=" << checks << " allocation_faults=" << faults << '\n';
    return 0;
  } catch (const std::exception& error) {
    fail_after = -1;
    std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n'; return 1;
  }
}
