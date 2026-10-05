// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "dml/direct_bulk_scalar_projection.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace api = scratchbird::engine::internal_api;
namespace projection = api::dml::detail;

void Require(bool condition, const char* message) {
  if (!condition) { std::cerr << message << '\n'; std::exit(EXIT_FAILURE); }
}

template<class Operation> void Refuses(Operation operation) {
  try { operation(); }
  catch (const std::invalid_argument&) { return; }
  Require(false, "malformed retained scalar was accepted");
}

int main() {
  // This is a carrier-preservation test, not datatype admission. The owning
  // codecs validate values before projection; no new type semantics live here.
  for (const auto& [type, width] : {
           std::pair{"int8", 1u}, {"uint8", 1u}, {"int16", 2u},
           {"int32", 4u}, {"uint32", 4u}, {"int64", 8u}, {"uint64", 8u},
           {"int128", 16u}, {"uint128", 16u}, {"real64", 8u}, {"uuid", 16u}}) {
    for (const auto byte : {0u, 1u, 0x7fu, 0x80u, 0xffu}) {
      api::EngineTypedValue value;
      value.descriptor.canonical_type_name = type;
      value.binary_value.assign(width, byte);
      const auto stored = projection::DirectTypedStoredValue(value);
      Require(stored.isPresent() && stored.bytes.size() == width,
              "native scalar storage changed state or width");
      for (unsigned i = 0; i < width; ++i)
        Require(static_cast<unsigned char>(stored.bytes[i]) == byte,
                "native scalar storage rendered or changed a byte");
      value.encoded_value = "ambiguous";
      Refuses([&] { projection::DirectTypedStoredValue(value); });
      value.encoded_value.clear();
      value.setState(api::EngineValueState::sql_null);
      // Explicitly restore a malformed payload if setState clears it.
      value.binary_value.assign(width, byte);
      Refuses([&] { projection::DirectTypedStoredValue(value); });
      value.binary_value.clear();
      const auto null = projection::DirectTypedStoredValue(value);
      Require(null.isSqlNull() && null.bytes.empty(), "typed NULL lost its state");
    }
  }
  api::EngineTypedValue text;
  text.descriptor.canonical_type_name = "character";
  text.encoded_value = "unchanged text";
  Require(projection::DirectTypedStoredValue(text).bytes == text.encoded_value,
          "character payload changed");
  api::EngineTypedValue unresolved;
  unresolved.descriptor.canonical_type_name = "uint16";
  unresolved.binary_value = {42, 0};
  Refuses([&] { projection::DirectTypedStoredValue(unresolved); });
  unresolved.binary_value.clear();
  unresolved.encoded_value = "42";
  Refuses([&] { projection::DirectTypedStoredValue(unresolved); });
}
