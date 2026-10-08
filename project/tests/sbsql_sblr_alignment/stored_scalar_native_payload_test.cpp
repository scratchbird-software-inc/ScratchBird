// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "engine/internal_api/mga_relation_store/stored_scalar_payload.hpp"
#include "engine/internal_api/mga_relation_store/stored_int64_descriptor.hpp"
#include "engine/executor/descriptor_value_runtime.hpp"
#include "engine/public_abi_int64_payload.hpp"
#include "wire/public_result_packet.hpp"
#include "engine/sblr/sblr_projection_value_runtime.hpp"
#include "engine/sblr/sblr_sequence_runtime.hpp"
#include "engine/internal_api/dml/constraint_enforcement.hpp"
#include "engine/internal_api/dml/dml_executable_trigger_runtime.hpp"
#include "engine/functions/dispatch/function_dispatch.hpp"
#include "engine/functions/registry/function_seed_registry.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <new>

namespace { long fail_after = -1; }
void* operator new(std::size_t n) {
  if (fail_after == 0) throw std::bad_alloc();
  if (fail_after > 0) --fail_after;
  if (auto* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace api = scratchbird::engine::internal_api;
using State = api::EngineValueState;
unsigned checks = 0;
void Require(bool ok, const char* message) {
  ++checks;
  if (!ok) { std::cerr << message << " check=" << checks << '\n'; std::exit(1); }
}
api::EngineTypedValue Sentinel(std::string type) {
  api::EngineTypedValue value;
  value.descriptor.canonical_type_name = std::move(type);
  value.descriptor.descriptor_kind = "scalar";
  value.descriptor.descriptor_uuid.bytes = {1,2,3,4,5,6,0x70,8,0x80,10,11,12,13,14,15,16};
  value.descriptor.type_uuid = value.descriptor.descriptor_uuid;
  value.descriptor.type_uuid.bytes[15] = 17;
  value.descriptor.encoded_descriptor = "independent bound descriptor sentinel";
  value.descriptor.datatype_descriptor_generation = 91;
  value.encoded_value = "stale text must be replaced";
  value.binary_value = {0xff, 0, 0x7f};
  value.setState(State::error);
  return value;
}
bool Equal(const api::EngineTypedValue& a, const api::EngineTypedValue& b) {
  return a.descriptor == b.descriptor && a.encoded_value == b.encoded_value &&
         a.binary_value == b.binary_value && a.state == b.state && a.is_null == b.is_null;
}
void UuidValues() {
  for (const char* type : {"uuid", "uuidv7", "UUID", "UuIdV7"}) {
    const auto sentinel = Sentinel(type);
    for (unsigned offset = 0; offset != 16; ++offset) {
      for (unsigned octet = 0; octet != 256; ++octet) {
        std::string bytes(16, '\0');
        for (unsigned n = 0; n != 16; ++n) bytes[n] = static_cast<char>(n * 17);
        bytes[offset] = static_cast<char>(octet);
        auto value = sentinel;
        Require(api::RestoreStoredScalarPayloadV1(bytes, State::value, &value),
                "UUID data bits incorrectly refused as system authority");
        Require(value.descriptor == sentinel.descriptor && value.state == State::value &&
                    !value.is_null && value.encoded_value.empty() && value.binary_value.size() == 16,
                "UUID projection changed descriptor/state or retained a text shadow");
        for (unsigned n = 0; n != 16; ++n)
          Require(value.binary_value[n] == static_cast<unsigned char>(bytes[n]),
                  "UUID projection changed an octet");
        Require(api::StoredScalarPayloadMatchesV1(value, bytes, State::value),
                "exact binary UUID receipt rejected");
        auto changed_bytes = bytes; changed_bytes[(offset + 1) % 16] ^= 1;
        Require(!api::StoredScalarPayloadMatchesV1(value, changed_bytes, State::value),
                "UUID receipt accepted different bytes");
        value.encoded_value = bytes;
        Require(!api::StoredScalarPayloadMatchesV1(value, bytes, State::value),
                "UUID receipt accepted an auxiliary text arm");
        value.binary_value.clear();
        Require(!api::StoredScalarPayloadMatchesV1(value, bytes, State::value),
                "UUID receipt accepted bytes in the text arm only");
      }
    }
    auto nil = sentinel;
    Require(api::RestoreStoredScalarPayloadV1(std::string(16, '\0'), State::value, &nil) &&
                nil.binary_value == std::vector<std::uint8_t>(16, 0) && !nil.is_null,
            "nil UUID data was changed into absence/NULL");
    for (unsigned length = 0; length <= 128; ++length) {
      if (length == 16) continue;
      auto value = sentinel;
      Require(!api::RestoreStoredScalarPayloadV1(std::string(length, 'a'), State::value, &value) &&
                  Equal(value, sentinel), "wrong UUID width mutated output or was accepted");
    }
    auto value = sentinel;
    Require(!api::RestoreStoredScalarPayloadV1(
                "019f0000-0000-7000-8000-000000000001", State::value, &value) &&
                Equal(value, sentinel), "display UUID text was parsed inside the engine");
  }
}
void OtherCarriersAndState() {
  const std::array<std::string, 5> payloads{
      "", "<NULL>", std::string("\0\xff\r\n|", 5), std::string(16, '\0'), std::string(257, 'x')};
  for (const char* type : {"binary", "bytes", "blob", "BINARY", "ByTeS", "BLOB", "text", "int64"}) {
    const bool integer = std::string_view(type) == "int64";
    const bool binary = std::string_view(type) != "text";
    const auto sentinel = Sentinel(type);
    for (const auto& bytes : payloads) {
      auto value = sentinel;
      if (integer) {
        Require(!api::RestoreStoredScalarPayloadV1(bytes, State::value, &value) && Equal(value, sentinel),
                "INT64 accepted an invalid width or changed its output");
        continue;
      }
      Require(api::RestoreStoredScalarPayloadV1(bytes, State::value, &value), "scalar projection failed");
      const std::vector<std::uint8_t> expected(bytes.begin(), bytes.end());
      Require(value.descriptor == sentinel.descriptor && !value.is_null && value.state == State::value &&
                  (binary ? value.encoded_value.empty() && value.binary_value == expected
                          : value.binary_value.empty() && value.encoded_value == bytes),
              "scalar payload kind/state was inferred from its content");
      Require(api::StoredScalarPayloadMatchesV1(value, bytes, State::value), "scalar receipt mismatch");
      value.is_null = true;
      Require(!api::StoredScalarPayloadMatchesV1(value, bytes, State::value), "inconsistent NULL flag accepted");
    }
    auto value = sentinel;
    Require(api::RestoreStoredScalarPayloadV1({}, State::sql_null, &value) &&
                value.descriptor == sentinel.descriptor && value.is_null &&
                value.encoded_value.empty() && value.binary_value.empty() &&
                api::StoredScalarPayloadMatchesV1(value, {}, State::sql_null),
            "explicit SQL NULL did not clear both payload arms");
    value = sentinel;
    Require(!api::RestoreStoredScalarPayloadV1("x", State::sql_null, &value) && Equal(value, sentinel),
            "SQL NULL with payload accepted or mutated output");
    for (unsigned state = 2; state < 256; ++state) {
      Require(!api::RestoreStoredScalarPayloadV1({}, static_cast<State>(state), &value) &&
                  Equal(value, sentinel), "non-scalar/sentinel state admitted");
      Require(!api::StoredScalarPayloadMatchesV1(value, {}, static_cast<State>(state)),
              "receipt accepted a non-value state");
    }
  }
  Require(!api::RestoreStoredScalarPayloadV1({}, State::value, nullptr), "null destination accepted");
  auto empty_type = Sentinel(""); const auto original = empty_type;
  Require(!api::RestoreStoredScalarPayloadV1("x", State::value, &empty_type) && Equal(empty_type, original),
          "absent type silently selected a text carrier");
}
void AliasAndAllocation() {
  for (const char* type : {"text", "binary", "uuid", "int64"}) {
    auto value = Sentinel(type);
    value.encoded_value = std::string(type == std::string_view("uuid") ? 16 :
                                    type == std::string_view("int64") ? 8 : 257, 'q');
    const auto bytes = value.encoded_value;
    Require(api::RestoreStoredScalarPayloadV1(value.encoded_value, State::value, &value) &&
                api::StoredScalarPayloadMatchesV1(value, bytes, State::value), "aliased text source was overwritten");
    if (!value.binary_value.empty()) {
      const std::string_view alias(reinterpret_cast<const char*>(value.binary_value.data()), value.binary_value.size());
      Require(api::RestoreStoredScalarPayloadV1(alias, State::value, &value) &&
                  api::StoredScalarPayloadMatchesV1(value, bytes, State::value), "aliased binary source was overwritten");
    }
    unsigned faults = 0; bool completed = false;
    const auto sentinel = Sentinel(type);
    for (long fault = 0; fault != 16; ++fault) {
      value = sentinel; fail_after = fault;
      try {
        const bool ok = api::RestoreStoredScalarPayloadV1(bytes, State::value, &value);
        fail_after = -1;
        Require(ok && api::StoredScalarPayloadMatchesV1(value, bytes, State::value), "allocation sweep projection failed");
        completed = true; break;
      } catch (const std::bad_alloc&) {
        fail_after = -1; ++faults;
        Require(Equal(value, sentinel), "allocation failure partially published scalar");
      }
    }
    Require(completed && faults != 0, "allocation fault path was not exercised");
  }
}
void Int64Values() {
  for (const char* type : {"int64", "INT64"}) {
    const auto sentinel = Sentinel(type);
    for (const auto& bytes : {std::string(8, '\0'), std::string(8, '\xff'),
          std::string("\0\0\0\0\0\0\0\x80", 8),
          std::string("\xff\xff\xff\xff\xff\xff\xff\x7f", 8),
          std::string("12345678")}) {
      auto value = sentinel;
      Require(api::RestoreStoredScalarPayloadV1(bytes, State::value, &value) &&
                  api::StoredScalarPayloadMatchesV1(value, bytes, State::value) &&
                  value.binary_value == std::vector<std::uint8_t>(bytes.begin(), bytes.end()) &&
                  value.encoded_value.empty(),
              "INT64 zero, signed bound, -1 or text-looking binary pattern changed");
    }
    for (unsigned offset = 0; offset < 8; ++offset) {
      for (unsigned octet = 0; octet < 256; ++octet) {
        // Includes NULs and every octet at every position. No display parser
        // or production encoder as oracle.
        std::string bytes(8, '\0');
        bytes[offset] = static_cast<char>(octet);
        auto value = sentinel;
        Require(api::RestoreStoredScalarPayloadV1(bytes, State::value, &value) &&
                    value.descriptor == sentinel.descriptor && !value.is_null &&
                    value.encoded_value.empty() && value.binary_value.size() == 8,
                "INT64 restore did not preserve its native carrier and descriptor");
        for (unsigned i = 0; i < 8; ++i)
          Require(value.binary_value[i] == static_cast<unsigned char>(bytes[i]),
                  "INT64 restore changed an octet");
        Require(api::StoredScalarPayloadMatchesV1(value, bytes, State::value),
                "INT64 native receipt did not match");
        auto changed = bytes; changed[(offset + 1) % 8] ^= 1;
        Require(!api::StoredScalarPayloadMatchesV1(value, changed, State::value),
                "INT64 receipt accepted different bytes");
        value.encoded_value = bytes;
        Require(!api::StoredScalarPayloadMatchesV1(value, bytes, State::value),
                "INT64 receipt accepted both payload arms");
        value.binary_value.clear();
        Require(!api::StoredScalarPayloadMatchesV1(value, bytes, State::value),
                "INT64 receipt accepted the encoded arm");
      }
    }
    for (unsigned length = 0; length <= 128; ++length) {
      if (length == 8) continue;
      auto value = sentinel;
      const std::string bytes(length, '1');
      Require(!api::RestoreStoredScalarPayloadV1(bytes, State::value, &value) && Equal(value, sentinel),
              "INT64 invalid width accepted or changed output");
      value.setState(State::value); value.encoded_value.clear();
      value.binary_value.assign(bytes.begin(), bytes.end());
      Require(!api::StoredScalarPayloadMatchesV1(value, bytes, State::value),
              "INT64 receipt admitted a malformed native width");
    }
  }
}
void NativeRegisteredAggregate() {
  namespace sblr = scratchbird::engine::sblr;
  namespace functions = scratchbird::engine::functions;
  const auto package = functions::BuildStandardFunctionSeedPackage();
  const auto* entry = package.registry.Lookup("sb.aggregate.regr_count");
  Require(entry != nullptr, "registered regression-count function missing");
  for (unsigned null_mask = 0; null_mask < 4; ++null_mask) {
    functions::FunctionCallRequest request;
    request.context.function_uuid = entry->function_uuid;
    request.context.security_allowed = request.context.policy_allowed = true;
    for (unsigned ordinal = 0; ordinal < 2; ++ordinal) {
      api::EngineProjectionFunctionArgument argument;
      argument.type_name = "int64";
      argument.descriptor.canonical_type_name = "int64";
      if (null_mask & (1u << ordinal)) {
        argument.is_null = true;
        argument.state = State::sql_null;
      } else {
        argument.binary_value.assign(8, 0);
        argument.binary_value[0] = static_cast<std::uint8_t>(ordinal + 2);
      }
      request.arguments.push_back({"arg" + std::to_string(ordinal),
          sblr::SblrValueFromProjectionArgument(argument)});
    }
    const auto result = functions::DispatchFunctionCall(package.registry, std::move(request)).result;
    Require(result.ok() && result.scalar_values.size() == 1 &&
                sblr::ProjectionSblrValueResolved(result.scalar_values.front()),
            "registered REGR_COUNT result rejected by the production projection bridge");
    const auto value = sblr::EngineTypedValueFromSblrValue(result.scalar_values.front());
    std::vector<std::uint8_t> expected(8, 0);
    expected[0] = null_mask == 0 ? 1 : 0;
    Require(value.binary_value == expected && value.encoded_value.empty() && !value.isSqlNull(),
            "registered REGR_COUNT lost native count or NULL-pair semantics");
  }
}

void NativeSequenceConsumers() {
  namespace sblr = scratchbird::engine::sblr;
  auto identity = Sentinel("int64").descriptor.descriptor_uuid;
  identity.bytes.back() = 99;
  sblr::SblrExecutionContext context;
  context.local_transaction_id = 7;
  context.transaction_uuid = identity;
  context.transaction_uuid.bytes.back() = 98;
  auto& registry = sblr::ProcessSblrSequenceRegistry();
  sblr::SblrSequenceDefinition definition;
  definition.sequence_uuid = identity;
  Require(sblr::RegisterSblrSequence(&registry, definition, context).ok(),
          "native sequence fixture registration failed");
  Require(sblr::RegisterSblrSequenceAlias(&registry, identity, "native_int64_default", context).ok(),
          "native sequence alias registration failed");
  sblr::SblrSequenceRequest request;
  request.context = context;
  request.sequence_uuid = identity;
  request.is_called = false;
  for (const auto number : {std::int64_t{0}, std::int64_t{-1}, std::int64_t{42},
                           std::numeric_limits<std::int64_t>::min(),
                           std::numeric_limits<std::int64_t>::max()}) {
    request.set_value = number;
    for (const auto& result : {sblr::SetSblrSequenceValue(&registry, request),
                              sblr::NextSblrSequenceValue(&registry, request),
                              sblr::CurrentSblrSequenceValue(&registry, request)}) {
      Require(result.ok() && result.scalar_values.size() == 1 &&
                  sblr::ProjectionSblrValueResolved(result.scalar_values.front()),
              "sequence result was rejected by the scalar publication bridge");
      const auto value = sblr::EngineTypedValueFromSblrValue(result.scalar_values.front());
      Require(value.binary_value.size() == 8 && value.encoded_value.empty(),
              "sequence publication retained a text carrier");
      for (unsigned byte = 0; byte < 8; ++byte)
        Require(value.binary_value[byte] == static_cast<std::uint8_t>(
                    static_cast<std::uint64_t>(number) >> (8 * byte)),
                "sequence publication changed signed boundary bits");
    }
  }
  api::EngineRequestContext engine_context;
  engine_context.local_transaction_id = context.local_transaction_id;
  engine_context.transaction_uuid = context.transaction_uuid;
  api::CrudTableRecord table;
  table.table_uuid = identity;
  table.columns = {{"id", "canonical=int64;default=sequence_next:native_int64_default"}};
  request.set_value = 41;
  Require(sblr::SetSblrSequenceValue(&registry, request).ok(), "default sequence reset failed");
  const auto defaults = api::ApplyConstraintDefaultsForInsert(engine_context, table, {});
  Require(defaults.ok && defaults.values.size() == 1 && defaults.values.front().first == "id" &&
              defaults.values.front().second == "41",
          "legacy default consumer lost its native sequence value");
  std::uint64_t audit_id = 123;
  api::EngineApiDiagnostic diagnostic;
  Require(api::dml_trigger_runtime::NextSequenceAuditId(engine_context, identity, &audit_id, &diagnostic) &&
              audit_id == 42, "trigger consumer lost its native sequence value");
  for (auto number : {std::int64_t{-1}, std::int64_t{0}}) {
    request.set_value = number;
    Require(sblr::SetSblrSequenceValue(&registry, request).ok(), "invalid audit fixture reset failed");
    audit_id = 123;
    Require(!api::dml_trigger_runtime::NextSequenceAuditId(engine_context, identity, &audit_id, &diagnostic) &&
                audit_id == 123 && diagnostic.code == "TRIGGER.RUNTIME.SEQUENCE_VALUE_INVALID",
            "trigger admitted nonpositive sequence id or changed output on refusal");
  }
}

void PublicInt64Transport() {
  namespace packet = scratchbird::wire::public_result;
  using scratchbird::engine::PublicInt64ScalarPayloadV1;
  const std::array<std::pair<std::string, std::int64_t>, 5> cases{{
      {std::string(8, '\0'), 0}, {std::string(8, '\xff'), -1},
      {std::string("\x01\0\0\0\0\0\0\0", 8), 1},
      {std::string("\0\0\0\0\0\0\0\x80", 8), INT64_MIN},
      {std::string("\xff\xff\xff\xff\xff\xff\xff\x7f", 8), INT64_MAX}}};
  for (const auto& [bytes, expected] : cases) {
    api::EngineTypedValue value;
    value.descriptor.canonical_type_name = "int64";
    value.binary_value.assign(bytes.begin(), bytes.end());
    std::string_view payload = "sentinel";
    Require(PublicInt64ScalarPayloadV1(value, &payload) && payload == bytes,
            "public INT64 projection lost native bytes");
    namespace sblr = scratchbird::engine::sblr;
    api::EngineProjectionFunctionArgument argument;
    argument.type_name = "int64";
    argument.descriptor = value.descriptor;
    argument.binary_value = value.binary_value;
    const auto native = sblr::SblrValueFromProjectionArgument(argument);
    Require(sblr::ProjectionSblrValueResolved(native) && native.has_int64_value &&
                native.int64_value == expected && native.encoded_value.empty() && native.text_value.empty(),
            "INT64 SBLR input bridge lost native bits");
    const auto projected = sblr::EngineTypedValueFromSblrValue(native);
    Require(projected.binary_value == value.binary_value && projected.encoded_value.empty() &&
                projected.descriptor == value.descriptor,
            "INT64 SBLR output bridge lost bits or descriptor");
    for (unsigned mutation = 0; mutation < 7; ++mutation) {
      auto invalid = native;
      if (mutation == 0) invalid.encoded_value = "0";
      if (mutation == 1) invalid.text_value = "0";
      if (mutation == 2) invalid.binary_value.assign(8, 0);
      if (mutation == 3) invalid.has_uint64_value = true;
      if (mutation == 4) invalid.is_null = true;
      if (mutation == 5) invalid.charset_name = "utf8";
      if (mutation == 6) invalid.collation_name = "binary";
      Require(!sblr::ProjectionSblrValueResolved(invalid), "conflicting INT64 SBLR representation accepted");
      bool refused = false;
      try { (void)sblr::EngineTypedValueFromSblrValue(invalid); }
      catch (const std::invalid_argument&) { refused = true; }
      Require(refused, "INT64 SBLR publisher accepted conflicting representation");
    }
    std::string encoded;
    Require(packet::Encode(std::vector<packet::FieldView>{{"v", packet::Kind::signed_integer, payload}}, &encoded),
            "public INT64 framing failed");
    std::vector<packet::Field> fields;
    Require(packet::Decode(encoded, &fields) && fields.size() == 1 &&
                fields[0].value == bytes && packet::AsSigned(fields[0]) == expected &&
                !packet::AsUnsigned(fields[0]), "signed binary result bits/kind changed");
    value.encoded_value = "1";
    payload = "sentinel";
    Require(!PublicInt64ScalarPayloadV1(value, &payload) && payload == "sentinel",
            "public INT64 accepted dual payload or changed destination on refusal");
    value.encoded_value.clear();
    value.is_null = true;
    Require(!PublicInt64ScalarPayloadV1(value, &payload), "inconsistent NULL accepted");
  }
  for (unsigned n = 0; n < 129; ++n) {
    if (n == 8) continue;
    api::EngineTypedValue value;
    value.descriptor.canonical_type_name = "INT64";
    value.binary_value.assign(n, 0);
    std::string_view payload = "sentinel";
    Require(!PublicInt64ScalarPayloadV1(value, &payload) && payload == "sentinel",
            "public INT64 wrong width accepted");
    Require(!packet::Valid({"v", packet::Kind::signed_integer, std::string(n, '1')}),
            "wire INT64 wrong width accepted");
  }
  api::EngineTypedValue null;
  null.descriptor.canonical_type_name = "int64";
  null.setState(State::sql_null);
  std::string_view payload;
  Require(PublicInt64ScalarPayloadV1(null, &payload) && payload.empty(),
          "NULL manufactured an integer payload");
  null.binary_value.assign(8, 0);
  Require(!PublicInt64ScalarPayloadV1(null, &payload), "NULL retained native payload");
  for (unsigned kind = 7; kind < 256; ++kind)
    Require(!packet::Valid({"v", static_cast<packet::Kind>(kind), std::string(8, '\0')}),
            "unknown wire kind accepted");
}
void StoredInt64Descriptors() {
  namespace dt = scratchbird::core::datatypes;
  unsigned rows = 0;
  for (const auto& row : dt::CurrentDatatypeTypeCodecIdentityRowsV1()) {
    if (row.canonical_binary_type_code != static_cast<std::uint32_t>(dt::CanonicalTypeId::int64)) continue;
    ++rows;
    api::EngineRequestContext context;
    context.datatype_catalog_snapshot_uuid = row.catalog_snapshot_uuid;
    context.datatype_catalog_generation = row.catalog_generation;
    context.datatype_registry_generation = row.registry_generation;
    auto source = Sentinel("int64").descriptor;
    source.type_uuid = row.type_uuid;
    source.datatype_descriptor_uuid = row.descriptor_uuid;
    source.datatype_descriptor_generation = row.descriptor_generation;
    source.descriptor_kind = "scalar";
    for (bool nullable : {false, true}) {
      api::CatalogColumnMetadata metadata;
      metadata.identities = {{"type_uuid", row.type_uuid},
          {"datatype_descriptor_uuid", row.descriptor_uuid}};
      if (!row.codec_uuid.is_nil()) metadata.identities["codec_uuid"] = row.codec_uuid;
      metadata.text = {{"canonical", "int64"}, {"canonical_type", "int64"}, {"type", "int64"},
          {"nullable", nullable ? "true" : "false"},
          {"nullability", nullable ? "nullable" : "non_null"},
          {"not_null", nullable ? "false" : "true"},
          {"datatype_descriptor_generation", std::to_string(row.descriptor_generation)},
          {"type_generation", std::to_string(row.type_generation)},
          {"codec_id", row.codec_id}, {"codec_version", std::to_string(row.codec_version)},
          {"codec_generation", std::to_string(row.codec_generation)},
          {"null_encoding", std::to_string(row.null_encoding_code)}, {"primary_key", "false"}};
      Require(api::EncodeCatalogColumnMetadata(metadata, &source.encoded_descriptor), "fixture encoding failed");
      auto output = Sentinel("output").descriptor;
      std::string detail;
      const bool projected = api::ProjectStoredInt64DescriptorV1(context, source, nullable, &output, &detail);
      if (!projected) std::cerr << "cohort=" << row.catalog_generation << " nullable=" << nullable
                                << " detail=" << detail << '\n';
      Require(projected, "valid exact catalog INT64 projection refused");
      auto expected = source;
      expected.descriptor_kind = "scalar";
      expected.encoded_descriptor = nullable ? "nullability=nullable" : "nullability=non_null";
      Require(output == expected && detail.empty(), "INT64 projection lost occurrence/datatype binding");
      auto aliased = source;
      Require(api::ProjectStoredInt64DescriptorV1(context, aliased, nullable, &aliased, &detail) &&
                  aliased == expected, "aliased projection changed source before validation");
      unsigned faults = 0;
      bool completed = false;
      for (long allocation = 0; allocation < 1024; ++allocation) {
        auto candidate = Sentinel("output").descriptor;
        const auto original = candidate;
        const auto original_source = source;
        fail_after = allocation;
        try {
          const bool ok = api::ProjectStoredInt64DescriptorV1(context, source, nullable, &candidate, &detail);
          fail_after = -1;
          Require(ok && candidate == expected, "allocation sweep refused valid descriptor");
          completed = true;
          break;
        } catch (const std::bad_alloc&) {
          fail_after = -1;
          ++faults;
          Require(candidate == original && source == original_source, "failed allocation partially published descriptor");
        }
      }
      Require(completed && faults != 0, "descriptor allocation fault path not exercised");
      api::EngineTypedValue value;
      value.descriptor = output;
      Require(api::RestoreStoredScalarPayloadV1(std::string(8, '\xff'), State::value, &value) &&
                  value.binary_value == std::vector<std::uint8_t>(8, 255), "projected payload changed");
      std::int64_t decoded = 42;
      Require(scratchbird::engine::executor::DecodeBoundInt64Value(value, &decoded, &detail) && decoded == -1,
              "projected stored descriptor rejected by strict scalar decoder");
      if (nullable) {
        Require(api::RestoreStoredScalarPayloadV1({}, State::sql_null, &value) && value.is_null,
                "projected nullable column lost NULL");
        decoded = 42;
        Require(!scratchbird::engine::executor::DecodeBoundInt64Value(value, &decoded, &detail) && decoded == 42,
                "NULL decoded into a PRESENT integer or changed output");
      }
      const auto reject = [&](const auto& ctx, const auto& invalid) {
        auto unchanged = Sentinel("output").descriptor;
        const auto original = unchanged;
        Require(!api::ProjectStoredInt64DescriptorV1(ctx, invalid, nullable, &unchanged, &detail) &&
                    unchanged == original && !detail.empty(), "invalid projection accepted or output changed");
      };
      for (unsigned change = 0; change < 8; ++change) {
        auto invalid = source;
        if (change == 0) invalid.type_uuid = {};
        if (change == 1) invalid.datatype_descriptor_uuid = {};
        if (change == 2) ++invalid.datatype_descriptor_generation;
        if (change == 3) invalid.descriptor_uuid.bytes[6] = 0x40;
        if (change == 4) invalid.charset_uuid = source.descriptor_uuid;
        if (change == 5) invalid.collation_uuid = source.descriptor_uuid;
        if (change == 6) invalid.canonical_type_name = "text";
        if (change == 7) invalid.encoded_descriptor.clear();
        reject(context, invalid);
      }
      for (const auto kind : {"", "domain", "rowset", "catalog.column"}) {
        auto invalid = source;
        invalid.descriptor_kind = kind;
        reject(context, invalid);
      }
      for (const auto kind : {"executor.scalar", "canonical_type_descriptor"}) {
        auto alternate = source;
        alternate.descriptor_kind = kind;
        Require(api::ProjectStoredInt64DescriptorV1(context, alternate, nullable, &output, &detail) &&
                    output == expected, "execution scalar kind refused");
      }
      {
        auto changed = metadata;
        changed.text.erase("nullable");
        changed.text.erase("nullability");
        auto invalid = source;
        Require(api::EncodeCatalogColumnMetadata(changed, &invalid.encoded_descriptor), "missing nullability encoding failed");
        reject(context, invalid);
      }
      for (unsigned change = 0; change < 3; ++change) {
        auto invalid = context;
        if (change == 0) invalid.datatype_catalog_snapshot_uuid = {};
        if (change == 1) invalid.datatype_catalog_generation = UINT64_MAX;
        if (change == 2) invalid.datatype_registry_generation = UINT64_MAX;
        reject(invalid, source);
      }
      for (const auto& [key, unused] : metadata.text) {
        if (key == "primary_key") continue;
        auto changed = metadata;
        changed.text[key] = "conflicting";
        auto invalid = source;
        Require(api::EncodeCatalogColumnMetadata(changed, &invalid.encoded_descriptor), "mutation encoding failed");
        reject(context, invalid);
      }
      for (const auto key : {"width", "precision", "scale", "timezone_profile_id", "unknown_modifier"}) {
        auto changed = metadata;
        changed.text[key] = "1";
        auto invalid = source;
        Require(api::EncodeCatalogColumnMetadata(changed, &invalid.encoded_descriptor), "modifier encoding failed");
        reject(context, invalid);
      }
      for (const auto key : {"type_uuid", "datatype_descriptor_uuid", "codec_uuid", "domain_uuid"}) {
        auto changed = metadata;
        changed.identities[key] = source.descriptor_uuid;
        auto invalid = source;
        Require(api::EncodeCatalogColumnMetadata(changed, &invalid.encoded_descriptor), "identity encoding failed");
        reject(context, invalid);
      }
      for (const auto key : {"nullable", "nullability"}) {
        auto changed = metadata;
        changed.text.erase(key);
        auto single = source;
        Require(api::EncodeCatalogColumnMetadata(changed, &single.encoded_descriptor) &&
                    api::ProjectStoredInt64DescriptorV1(context, single, nullable, &output, &detail),
                "single consistent nullability spelling refused");
      }
    }
  }
  Require(rows != 0, "no INT64 registry cohort exercised");
}

int main() {
  StoredInt64Descriptors();
  UuidValues(); Int64Values(); OtherCarriersAndState(); AliasAndAllocation();
  PublicInt64Transport();
  NativeSequenceConsumers();
  NativeRegisteredAggregate();
  std::cout << "stored scalar native payload checks=" << checks << '\n';
}
