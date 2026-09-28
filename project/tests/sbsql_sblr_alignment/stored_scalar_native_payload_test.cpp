// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "engine/internal_api/mga_relation_store/stored_scalar_payload.hpp"

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
    const bool binary = std::string_view(type) != "text" && std::string_view(type) != "int64";
    const auto sentinel = Sentinel(type);
    for (const auto& bytes : payloads) {
      auto value = sentinel;
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
  for (const char* type : {"text", "binary", "uuid"}) {
    auto value = Sentinel(type);
    value.encoded_value = std::string(type == std::string_view("uuid") ? 16 : 257, 'q');
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
int main() {
  UuidValues(); OtherCarriersAndState(); AliasAndAllocation();
  std::cout << "stored scalar native payload checks=" << checks << '\n';
}
