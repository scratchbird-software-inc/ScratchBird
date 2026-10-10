// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "engine/internal_api/mga_relation_store/stored_scalar_payload.hpp"
#include "engine/internal_api/mga_relation_store/stored_integer_descriptor.hpp"
#include "engine/executor/descriptor_value_runtime.hpp"
#include "engine/public_abi_int64_payload.hpp"
#include "wire/public_result_packet.hpp"
#include "engine/sblr/sblr_projection_value_runtime.hpp"
#include "engine/sblr/sblr_projection_binary_literals.hpp"
#include "engine/sblr/sblr_operator_runtime.hpp"
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
void TimestampOctets() {
  // Projection does not choose between historical UTC and civil timestamps;
  // the admitted descriptor owns that distinction. Preserve every stored bit.
  for (const char* type : {"timestamp", "TIMESTAMP", "TimeStamp"}) {
    const auto sentinel = Sentinel(type);
    for (unsigned length = 0; length < 129; ++length) {
      std::string bytes(length, '\0');
      for (unsigned i = 0; i < length; ++i) bytes[i] = static_cast<char>(i * 37 + 129);
      auto value = sentinel;
      if (length != 16) {
        Require(!api::RestoreStoredScalarPayloadV1(bytes, State::value, &value) && Equal(value, sentinel),
                "timestamp width refusal mutated the destination");
        value.binary_value.assign(bytes.begin(), bytes.end());
        value.encoded_value.clear();
        value.setState(State::value);
        Require(!api::StoredScalarPayloadMatchesV1(value, bytes, State::value),
                "timestamp receipt accepted a non-native width");
        continue;
      }
      Require(api::RestoreStoredScalarPayloadV1(bytes, State::value, &value) &&
                  value.descriptor == sentinel.descriptor && value.encoded_value.empty() &&
                  value.binary_value == std::vector<std::uint8_t>(bytes.begin(), bytes.end()) &&
                  api::StoredScalarPayloadMatchesV1(value, bytes, State::value),
              "timestamp projection altered the descriptor or native octets");
      value.encoded_value = bytes;
      Require(!api::StoredScalarPayloadMatchesV1(value, bytes, State::value),
              "timestamp receipt accepted mixed payload arms");
      value.encoded_value.clear();
      for (std::size_t i = 0; i < 16; ++i) {
        value.binary_value[i] ^= 1;
        Require(!api::StoredScalarPayloadMatchesV1(value, bytes, State::value),
                "timestamp receipt lost an octet");
        value.binary_value[i] ^= 1;
      }
    }
  }
}

void OtherCarriersAndState() {
  const std::array<std::string, 5> payloads{
      "", "<NULL>", std::string("\0\xff\r\n|", 5), std::string(16, '\0'), std::string(257, 'x')};
  for (const char* type : {"binary", "bytes", "blob", "BINARY", "ByTeS", "BLOB", "text", "int64", "timestamp", "TIMESTAMP"}) {
    const bool integer = std::string_view(type) == "int64";
    const bool timestamp = std::string_view(type) == "timestamp" || std::string_view(type) == "TIMESTAMP";
    const bool binary = std::string_view(type) != "text";
    const auto sentinel = Sentinel(type);
    for (const auto& bytes : payloads) {
      auto value = sentinel;
      if (integer || (timestamp && bytes.size() != 16)) {
        Require(!api::RestoreStoredScalarPayloadV1(bytes, State::value, &value) && Equal(value, sentinel),
                "fixed-width scalar accepted an invalid width or changed its output");
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

void NativeReal64Projection() {
  namespace sblr = scratchbird::engine::sblr;
  namespace functions = scratchbird::engine::functions;
  const auto descriptor = Sentinel("real64").descriptor;
  const auto argument_for = [&](std::uint64_t bits) {
    api::EngineProjectionFunctionArgument argument;
    argument.type_name = "real64";
    argument.descriptor = descriptor;
    argument.binary_value.resize(8);
    for (unsigned i = 0; i < 8; ++i)
      argument.binary_value[i] = static_cast<std::uint8_t>(bits >> (8 * i));
    return argument;
  };
  // This adapter transfers payloads, not datatype authority or special-value
  // admission. Even NaN payloads and signed zero must survive without arithmetic.
  const auto roundtrip = [&](std::uint64_t bits) {
    const auto argument = argument_for(bits);
    fail_after = 0;
    const bool encoding_valid = sblr::ProjectionArgumentEncodingValid(argument);
    fail_after = -1;
    Require(encoding_valid, "REAL64 payload validation copied/allocated binding metadata");
    const auto value = sblr::SblrValueFromProjectionArgument(argument);
    Require(sblr::ProjectionArgumentEncodingValid(argument) &&
                sblr::ProjectionSblrValueResolved(value) &&
                sblr::SblrReal64PayloadValid(value) &&
                std::bit_cast<std::uint64_t>(value.real64_value) == bits,
            "REAL64 projection changed native bits or retained text");
    const auto restored = sblr::EngineTypedValueFromSblrValue(value);
    Require(restored.descriptor == descriptor && restored.binary_value == argument.binary_value &&
                restored.encoded_value.empty() && restored.state == State::value && !restored.is_null,
            "REAL64 publication lost binding, state or exact binary64 bits");
    const auto negated = sblr::EvaluateSblrUnaryArithmetic("op_unary_minus", value, {});
    Require(negated.ok() && negated.scalar_values.size() == 1 &&
                sblr::ProjectionSblrValueResolved(negated.scalar_values.front()) &&
                negated.scalar_values.front().projection_descriptor == value.projection_descriptor &&
                std::bit_cast<std::uint64_t>(negated.scalar_values.front().real64_value) ==
                    (bits ^ (std::uint64_t{1} << 63)),
            "REAL64 unary minus lost native bits or retained descriptor binding");
  };
  for (const std::uint64_t bits : {0ull, 0x8000000000000000ull, 1ull,
       0x000fffffffffffffull, 0x0010000000000000ull, 0x3ff4000000000000ull,
       0x7fefffffffffffffull, 0x7ff0000000000000ull, 0xfff0000000000000ull,
       0x7ff8000000000001ull, 0x7ff0000000000001ull}) roundtrip(bits);
  for (unsigned bit = 0; bit < 64; ++bit) roundtrip(std::uint64_t{1} << bit);
  for (unsigned change = 0; change < 9; ++change) {
    auto argument = argument_for(0x3ff4000000000000ull);
    if (change == 0) argument.encoded_value = "1.25";
    if (change == 1) argument.binary_value.pop_back();
    if (change == 2) argument.binary_value.push_back(0);
    if (change == 3) argument.state = State::error;
    if (change == 4) argument.is_null = true;
    if (change == 5) argument.state = State::sql_null;
    if (change == 6) { argument.state = State::sql_null; argument.is_null = true; }
    if (change == 7) argument.descriptor.canonical_type_name = "real128";
    if (change == 8) { argument.binary_value.clear(); argument.encoded_value = "1.25"; }
    Require(!sblr::ProjectionArgumentEncodingValid(argument) &&
                !sblr::ProjectionSblrValueResolved(sblr::SblrValueFromProjectionArgument(argument)),
            "REAL64 projection accepted conflicting, lexical or malformed input");
  }
  auto null = argument_for(0);
  null.binary_value.clear(); null.is_null = true; null.state = State::sql_null;
  const auto null_value = sblr::SblrValueFromProjectionArgument(null);
  const auto restored_null = sblr::EngineTypedValueFromSblrValue(null_value);
  Require(sblr::ProjectionArgumentEncodingValid(null) &&
              sblr::ProjectionSblrValueResolved(null_value) &&
              restored_null.descriptor == descriptor && restored_null.state == State::sql_null &&
              restored_null.is_null && restored_null.binary_value.empty() && restored_null.encoded_value.empty(),
          "REAL64 typed NULL lost its binding or gained payload");
  for (unsigned change = 0; change < 12; ++change) {
    auto value = sblr::SblrValueFromProjectionArgument(argument_for(0x3ff4000000000000ull));
    if (change == 0) value.text_value = "1.25";
    if (change == 1) value.encoded_value = "1.25";
    if (change == 2) value.binary_value.assign(8, 0);
    if (change == 3) value.has_int64_value = true;
    if (change == 4) value.has_uint64_value = true;
    if (change == 5) value.has_real64_value = false;
    if (change == 6) value.payload_kind = sblr::SblrValuePayloadKind::text;
    if (change == 7) value.uuid_value = descriptor.descriptor_uuid;
    if (change == 8) value.uuid_array_value.push_back(descriptor.descriptor_uuid);
    if (change == 9) value.charset_name = "UTF8";
    if (change == 10) value.collation_name = "binary";
    if (change == 11) value.is_null = true;
    Require(!sblr::ProjectionSblrValueResolved(value), "REAL64 result accepted conflicting payload");
    const auto negated = sblr::EvaluateSblrUnaryArithmetic("op_unary_minus", value, {});
    Require(!negated.ok() && negated.scalar_values.empty(),
            "REAL64 unary minus accepted conflicting payload");
    bool refused = false;
    try { (void)sblr::EngineTypedValueFromSblrValue(value); }
    catch (const std::invalid_argument&) { refused = true; }
    Require(refused, "REAL64 result publication did not refuse malformed carrier");
  }
  const auto package = functions::BuildStandardFunctionSeedPackage();
  const auto* entry = package.registry.Lookup("sb.scalar.round");
  Require(entry != nullptr, "registered ROUND function missing");
  functions::FunctionCallRequest request;
  request.context.function_uuid = entry->function_uuid;
  request.context.security_allowed = request.context.policy_allowed = true;
  request.arguments.push_back({"value", sblr::SblrValueFromProjectionArgument(argument_for(0x3ff4000000000000ull))});
  api::EngineProjectionFunctionArgument scale;
  scale.type_name = "int64";
  scale.binary_value.assign(8, 0); scale.binary_value[0] = 1;
  request.arguments.push_back({"scale", sblr::SblrValueFromProjectionArgument(scale)});
  const auto rounded = functions::DispatchFunctionCall(package.registry, std::move(request)).result;
  Require(rounded.ok() && rounded.scalar_values.size() == 1 &&
              sblr::ProjectionSblrValueResolved(rounded.scalar_values.front()) &&
              rounded.scalar_values.front().real64_value == 1.3,
          "registered ROUND failed to consume and publish native REAL64");
  const auto output = sblr::EngineTypedValueFromSblrValue(rounded.scalar_values.front());
  Require(output.binary_value == argument_for(std::bit_cast<std::uint64_t>(1.3)).binary_value &&
              output.encoded_value.empty(), "registered ROUND published REAL64 text");
  const auto* average = package.registry.Lookup("sb.aggregate.regr_avgx");
  Require(average != nullptr, "registered REGR_AVGX function missing");
  functions::FunctionCallRequest aggregate;
  aggregate.context.function_uuid = average->function_uuid;
  aggregate.context.security_allowed = aggregate.context.policy_allowed = true;
  aggregate.arguments.push_back({"y", sblr::SblrValueFromProjectionArgument(argument_for(0x3ff4000000000000ull))});
  aggregate.arguments.push_back({"x", sblr::SblrValueFromProjectionArgument(argument_for(0x3ff4000000000000ull))});
  const auto averaged = functions::DispatchFunctionCall(package.registry, aggregate).result;
  Require(averaged.ok() && averaged.scalar_values.size() == 1 &&
              sblr::ProjectionSblrValueResolved(averaged.scalar_values.front()) &&
              sblr::EngineTypedValueFromSblrValue(averaged.scalar_values.front()).binary_value ==
                  argument_for(0x3ff4000000000000ull).binary_value,
          "registered REGR_AVGX failed native REAL64 publication");
  aggregate.arguments.front().value.encoded_value = "1.25";
  const auto conflicting = functions::DispatchFunctionCall(package.registry, aggregate).result;
  Require(!conflicting.ok() && conflicting.scalar_values.empty() &&
              !conflicting.diagnostics.empty() &&
              conflicting.diagnostics.front().diagnostic_id == "SB_DIAG_FUNCTION_INVALID_INPUT",
          "direct registered function accepted conflicting REAL64 carriers");
  sblr::SblrValue left, right;
  left.descriptor_id = right.descriptor_id = "vector";
  left.is_null = right.is_null = false;
  left.payload_kind = right.payload_kind = sblr::SblrValuePayloadKind::descriptor_payload;
  left.encoded_value = "[1,2]"; right.encoded_value = "[3,4]";
  const auto dot = sblr::EvaluateSblrVectorOperator("operator.vector.dot", left, right, {});
  Require(dot.ok() && dot.scalar_values.size() == 1 &&
              sblr::ProjectionSblrValueResolved(dot.scalar_values.front()) &&
              sblr::EngineTypedValueFromSblrValue(dot.scalar_values.front()).binary_value ==
                  argument_for(std::bit_cast<std::uint64_t>(11.0)).binary_value,
          "vector operator published a REAL64 text shadow");
  functions::FunctionCallRequest nested;
  nested.context.function_uuid = entry->function_uuid;
  nested.context.security_allowed = nested.context.policy_allowed = true;
  nested.arguments.push_back({"value", dot.scalar_values.front()});
  const auto rounded_dot = functions::DispatchFunctionCall(package.registry, std::move(nested)).result;
  Require(rounded_dot.ok() && rounded_dot.scalar_values.size() == 1 &&
              sblr::ProjectionSblrValueResolved(rounded_dot.scalar_values.front()) &&
              rounded_dot.scalar_values.front().real64_value == 11.0,
          "nested registered function rejected native REAL64 operator output");
}

void NativeReal64LiteralBinding() {
  namespace sblr = scratchbird::engine::sblr;
  namespace dt = scratchbird::core::datatypes;
  const auto catalog = dt::LoadCurrentCoreDatatypeCatalogManifest();
  const auto row = dt::LookupDatatypeCatalogRow(catalog.manifest, dt::CanonicalTypeId::real64);
  Require(catalog.ok() && row.ok() && row.manifest.descriptor_rows.size() == 1,
          "REAL64 literal fixture requires the current Core descriptor");
  sblr::SblrOperand operand;
  operand.ordinal = 1;
  operand.type = "real64";
  operand.name = "projection_0_value";
  operand.value_kind = sblr::SblrValueKind::literal_typed;
  const auto& uuid = row.manifest.descriptor_rows.front().descriptor_uuid.value;
  operand.value_body.assign(uuid.bytes.begin(), uuid.bytes.end());
  operand.value_body.resize(32, 0);
  operand.value_body[16] = 8;
  operand.value_body[30] = 0xf4; operand.value_body[31] = 0x3f; // 1.25 LE8
  sblr::SblrOperationEnvelope envelope;
  envelope.operands.push_back(operand);
  api::EngineApiRequest request;
  Require(sblr::ProjectSblrReal64Literals(envelope, &request) &&
              request.projection.real64_literals.size() == 1 &&
              request.projection.real64_literals.front().first == "projection_0_" &&
              request.projection.real64_literals.front().second[6] == 0xf4 &&
              request.projection.real64_literals.front().second[7] == 0x3f,
          "REAL64 literal projection lost the Core-typed payload");
  const auto retained = request.projection.real64_literals;
  Require(sblr::ProjectSblrReal64Literals(envelope, &request) &&
              request.projection.real64_literals == retained,
          "REAL64 literal reconciliation changed an identical binding");
  for (unsigned change = 0; change < 12; ++change) {
    auto malformed = envelope;
    auto& item = malformed.operands.front();
    if (change == 0) item.ordinal = 2;
    if (change == 1) item.value = "1.25";
    if (change == 2) item.value_kind = sblr::SblrValueKind::uuid_ref;
    if (change == 3) item.value_flags = 1;
    if (change == 4) item.value_body.pop_back();
    if (change == 5) item.value_body.push_back(0);
    if (change == 6) item.value_body[0] ^= 1;
    if (change == 7) item.value_body[16] = 7;
    if (change == 8) item.value_body[23] = 1;
    if (change == 9) item.value_body[24] ^= 1;
    if (change == 10) item.name = "projection__value";
    if (change == 11) { malformed.operands.push_back(item); malformed.operands.back().ordinal = 2; }
    Require(!sblr::ProjectSblrReal64Literals(malformed, &request) &&
                request.projection.real64_literals == retained,
            "malformed/conflicting REAL64 literal partially replaced the retained binding");
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
  for (unsigned length = 0; length != 17; ++length)
    Require(packet::Valid({"v", packet::Kind::real64, std::string(length, '\0')}) == (length == 8),
            "REAL64 wire kind did not enforce its exact native width");
  for (unsigned kind = 8; kind < 256; ++kind)
    Require(!packet::Valid({"v", static_cast<packet::Kind>(kind), std::string(8, '\0')}),
            "unknown wire kind accepted");
}
void StoredIntegerDescriptors() {
  namespace dt = scratchbird::core::datatypes;
  unsigned int32_rows = 0, int64_rows = 0;
  for (const auto& row : dt::CurrentDatatypeTypeCodecIdentityRowsV1()) {
    const bool int32 = row.canonical_binary_type_code == static_cast<std::uint32_t>(dt::CanonicalTypeId::int32);
    if (!int32 && row.canonical_binary_type_code != static_cast<std::uint32_t>(dt::CanonicalTypeId::int64)) continue;
    const char* type = int32 ? "int32" : "int64";
    const unsigned width = int32 ? 4 : 8;
    if (int32) ++int32_rows; else ++int64_rows;
    api::EngineRequestContext context;
    context.datatype_catalog_snapshot_uuid = row.catalog_snapshot_uuid;
    context.datatype_catalog_generation = row.catalog_generation;
    context.datatype_registry_generation = row.registry_generation;
    auto source = Sentinel(type).descriptor;
    source.type_uuid = row.type_uuid;
    source.datatype_descriptor_uuid = row.descriptor_uuid;
    source.datatype_descriptor_generation = row.descriptor_generation;
    source.descriptor_kind = "scalar";
    for (bool nullable : {false, true}) {
      api::CatalogColumnMetadata metadata;
      metadata.identities = {{"type_uuid", row.type_uuid},
          {"datatype_descriptor_uuid", row.descriptor_uuid}};
      if (!row.codec_uuid.is_nil()) metadata.identities["codec_uuid"] = row.codec_uuid;
      metadata.text = {{"canonical", type}, {"canonical_type", type}, {"type", type},
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
      const bool projected = api::ProjectStoredIntegerDescriptorV1(context, source, nullable, &output, &detail);
      if (!projected) std::cerr << "cohort=" << row.catalog_generation << " nullable=" << nullable
                                << " detail=" << detail << '\n';
      Require(projected, "valid exact catalog INT64 projection refused");
      auto expected = source;
      expected.descriptor_kind = "scalar";
      expected.encoded_descriptor = nullable ? "nullability=nullable" : "nullability=non_null";
      Require(output == expected && detail.empty(), "INT64 projection lost occurrence/datatype binding");
      auto aliased = source;
      Require(api::ProjectStoredIntegerDescriptorV1(context, aliased, nullable, &aliased, &detail) &&
                  aliased == expected, "aliased projection changed source before validation");
      unsigned faults = 0;
      bool completed = false;
      for (long allocation = 0; allocation < 1024; ++allocation) {
        auto candidate = Sentinel("output").descriptor;
        const auto original = candidate;
        const auto original_source = source;
        fail_after = allocation;
        try {
          const bool ok = api::ProjectStoredIntegerDescriptorV1(context, source, nullable, &candidate, &detail);
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
      Require(api::RestoreStoredScalarPayloadV1(std::string(width, '\xff'), State::value, &value) &&
                  value.binary_value == std::vector<std::uint8_t>(width, 255), "projected payload changed");
      std::int64_t decoded = 42;
      const auto read = scratchbird::engine::executor::DecodeInt64Value(value);
      Require(read.ok() && read.value == -1,
              "projected stored descriptor rejected by strict scalar decoder");
      if (!int32)
        Require(scratchbird::engine::executor::DecodeBoundInt64Value(value, &decoded, &detail) &&
                    decoded == -1, "INT64 strict bound decoder rejected stored projection");
      if (nullable) {
        Require(api::RestoreStoredScalarPayloadV1({}, State::sql_null, &value) && value.is_null,
                "projected nullable column lost NULL");
        decoded = 42;
        Require(!scratchbird::engine::executor::DecodeInt64Value(value).ok() &&
                    !scratchbird::engine::executor::DecodeBoundInt64Value(value, &decoded, &detail) && decoded == 42,
                "NULL decoded into a PRESENT integer or changed output");
      }
      const auto reject = [&](const auto& ctx, const auto& invalid) {
        auto unchanged = Sentinel("output").descriptor;
        const auto original = unchanged;
        Require(!api::ProjectStoredIntegerDescriptorV1(ctx, invalid, nullable, &unchanged, &detail) &&
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
        Require(api::ProjectStoredIntegerDescriptorV1(context, alternate, nullable, &output, &detail) &&
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
                    api::ProjectStoredIntegerDescriptorV1(context, single, nullable, &output, &detail),
                "single consistent nullability spelling refused");
      }
    }
  }
  Require(int32_rows != 0 && int64_rows != 0, "both signed integer registry cohorts were not exercised");
}

int main() {
  StoredIntegerDescriptors();
  UuidValues(); Int64Values(); TimestampOctets(); OtherCarriersAndState(); AliasAndAllocation();
  PublicInt64Transport();
  NativeSequenceConsumers();
  NativeRegisteredAggregate();
  NativeReal64Projection();
  NativeReal64LiteralBinding();
  std::cout << "stored scalar native payload checks=" << checks << '\n';
}
