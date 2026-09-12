// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Structural codec evidence, not receipt authority or query execution.
#include "engine/sblr/relational_descriptor_codec.hpp"
#include "engine/sblr/sblr_engine_envelope.hpp"
#include "binder/descriptor_authority.hpp"
#include "wire/contextual_operand_freeze.hpp"
#include "lowering/relational_identity_operand.hpp"
#include "wire/native_query_artifact.hpp"
#include "engine/internal_api/query/contextual_text_descriptor_match.hpp"

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
  native.window_invocations[0].bound_function_uuid = Id(18);
  native.relations.resize(1); native.relations[0].bound_object_uuid = Id(19);
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
int main() {
  try {
    TestLayout(); TestAllocationAtomicity(); TestOperationPlacement(); TestNumericBinding(); TestFrozenOperands();
    TestReservationSnapshot();
    TestContextIdentityOperands();
    TestNativeArtifactPublication();
    TestContextualDescriptorAgreement();
    std::cout << "PASS relational descriptor checks=" << checks << " allocation_faults=" << faults << '\n';
    return 0;
  } catch (const std::exception& error) {
    fail_after = -1;
    std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n'; return 1;
  }
}
