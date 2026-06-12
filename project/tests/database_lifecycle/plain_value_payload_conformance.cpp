// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "scratchbird/engine/value.hpp"

#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

namespace {

namespace engine = scratchbird::engine;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

std::vector<std::uint8_t> Bytes(std::initializer_list<std::uint8_t> bytes) {
  return std::vector<std::uint8_t>(bytes);
}

std::span<const std::uint8_t> View(const std::vector<std::uint8_t>& bytes) {
  return std::span<const std::uint8_t>(bytes.data(), bytes.size());
}

std::uint64_t PayloadLengthField(const std::vector<std::uint8_t>& encoded) {
  std::uint64_t value = 0;
  for (std::size_t index = 8; index < engine::kPlainValuePayloadHeaderSize;
       ++index) {
    value = (value << 8) | encoded[index];
  }
  return value;
}

std::vector<std::uint8_t> EncodeOrFail(engine::ExecutionValueState state,
                                       const std::vector<std::uint8_t>& payload) {
  const auto encoded = engine::serializePlainValue(state, View(payload));
  Require(encoded.ok(),
          "EDR-003 serializePlainValue unexpectedly rejected valid input");
  Require(encoded.encoded.size() ==
              engine::kPlainValuePayloadHeaderSize + payload.size(),
          "EDR-003 encoded payload size mismatch");
  Require(encoded.encoded[0] == engine::kPlainValuePayloadMagic0 &&
              encoded.encoded[1] == engine::kPlainValuePayloadMagic1 &&
              encoded.encoded[2] == engine::kPlainValuePayloadMagic2 &&
              encoded.encoded[3] == engine::kPlainValuePayloadMagic3,
          "EDR-003 encoded payload did not use SBC1 magic");
  Require(encoded.encoded[4] == engine::kPlainValuePayloadMajorVersion &&
              encoded.encoded[5] == engine::kPlainValuePayloadMinorVersion,
          "EDR-003 encoded payload version mismatch");
  Require(encoded.encoded[6] == static_cast<std::uint8_t>(state),
          "EDR-003 encoded payload state mismatch");
  Require(encoded.encoded[7] == 0,
          "EDR-003 encoded payload reserved byte must be zero");
  Require(PayloadLengthField(encoded.encoded) == payload.size(),
          "EDR-003 encoded payload length field mismatch");
  return encoded.encoded;
}

void RequireRoundTrip(engine::ExecutionValueState state,
                      const std::vector<std::uint8_t>& payload) {
  const auto encoded = EncodeOrFail(state, payload);
  const auto decoded = engine::deserializePlainValue(View(encoded));
  Require(decoded.ok(),
          "EDR-003 deserializePlainValue unexpectedly rejected valid input");
  Require(decoded.value.state == state,
          "EDR-003 decoded payload state mismatch");
  Require(decoded.value.payload == payload,
          "EDR-003 decoded payload bytes mismatch");
}

void RequireDecodeFailure(const std::vector<std::uint8_t>& encoded,
                          engine::PlainValuePayloadStatus expected) {
  const auto decoded = engine::deserializePlainValue(View(encoded));
  Require(!decoded.ok(),
          "EDR-003 malformed payload unexpectedly decoded successfully");
  Require(decoded.status == expected,
          "EDR-003 malformed payload diagnostic status mismatch");
}

void TestValidPlainValuePayloadRoundTrips() {
  RequireRoundTrip(engine::ExecutionValueState::value,
                   Bytes({0x00, 0x01, 0x7f, 0x80, 0xff}));
  RequireRoundTrip(engine::ExecutionValueState::sql_null, {});
  RequireRoundTrip(engine::ExecutionValueState::missing, {});
  RequireRoundTrip(engine::ExecutionValueState::default_requested, {});
  RequireRoundTrip(engine::ExecutionValueState::unknown, {});
  RequireRoundTrip(engine::ExecutionValueState::error,
                   Bytes({'S', 'Q', 'L', 'S', 'T', 'A', 'T', 'E'}));
  RequireRoundTrip(engine::ExecutionValueState::lob_handle,
                   Bytes({0x10, 0x20, 0x30, 0x40}));
  RequireRoundTrip(engine::ExecutionValueState::protected_value,
                   Bytes({0xa0, 0xb0, 0xc0}));
}

void TestPlainValuePayloadRejectsMalformedInput() {
  const auto good = EncodeOrFail(engine::ExecutionValueState::value,
                                 Bytes({0x01}));

  RequireDecodeFailure(Bytes({'S', 'B', 'C'}),
                       engine::PlainValuePayloadStatus::truncated_header);

  auto invalid_magic = good;
  invalid_magic[0] = 'X';
  RequireDecodeFailure(invalid_magic,
                       engine::PlainValuePayloadStatus::invalid_magic);

  auto unsupported_version = good;
  unsupported_version[4] = 2;
  RequireDecodeFailure(unsupported_version,
                       engine::PlainValuePayloadStatus::unsupported_version);

  auto invalid_reserved = good;
  invalid_reserved[7] = 1;
  RequireDecodeFailure(invalid_reserved,
                       engine::PlainValuePayloadStatus::invalid_reserved);

  auto invalid_state = good;
  invalid_state[6] = 0xff;
  RequireDecodeFailure(invalid_state,
                       engine::PlainValuePayloadStatus::invalid_state);

  auto length_mismatch = good;
  length_mismatch.push_back(0x00);
  RequireDecodeFailure(length_mismatch,
                       engine::PlainValuePayloadStatus::payload_length_mismatch);

  auto payload_not_allowed = good;
  payload_not_allowed[6] =
      static_cast<std::uint8_t>(engine::ExecutionValueState::missing);
  RequireDecodeFailure(payload_not_allowed,
                       engine::PlainValuePayloadStatus::payload_not_allowed);
}

void TestPlainValuePayloadSerializeRejectsInvalidStatePayloadPair() {
  const auto encoded = engine::serializePlainValue(
      engine::ExecutionValueState::missing, View(Bytes({0x01})));
  Require(!encoded.ok(),
          "EDR-003 serializePlainValue accepted payload for missing state");
  Require(encoded.status == engine::PlainValuePayloadStatus::payload_not_allowed,
          "EDR-003 invalid state payload serialize status mismatch");
}

void TestExecutionValueSerializationCompatibility() {
  engine::ExecutionValue value;
  value.setState(engine::ExecutionValueState::value);
  value.encoded_value = Bytes({0x01, 0x02, 0x03});

  auto encoded = engine::serializePlainValue(value);
  Require(encoded.ok(),
          "EDR-003 serializePlainValue(ExecutionValue) rejected value state");
  auto decoded = engine::deserializePlainValue(View(encoded.encoded));
  Require(decoded.ok() && decoded.value.state == engine::ExecutionValueState::value,
          "EDR-003 ExecutionValue value-state round trip failed");
  Require(decoded.value.payload == value.encoded_value,
          "EDR-003 ExecutionValue payload bytes mismatch");

  value.is_null = true;
  encoded = engine::serializePlainValue(value);
  Require(encoded.ok(),
          "EDR-003 serializePlainValue rejected legacy SQL null value");
  decoded = engine::deserializePlainValue(View(encoded.encoded));
  Require(decoded.ok() && decoded.value.state == engine::ExecutionValueState::sql_null,
          "EDR-003 legacy SQL null did not serialize as SQL null state");
  Require(decoded.value.payload.empty(),
          "EDR-003 legacy SQL null serialized stale payload bytes");

  value.setState(engine::ExecutionValueState::error);
  value.is_null = true;
  value.encoded_value = Bytes({0xee});
  encoded = engine::serializePlainValue(value);
  Require(encoded.ok(),
          "EDR-003 serializePlainValue rejected special state with stale flag");
  decoded = engine::deserializePlainValue(View(encoded.encoded));
  Require(decoded.ok() && decoded.value.state == engine::ExecutionValueState::error,
          "EDR-003 stale null flag changed special state serialization");
  Require(decoded.value.payload == Bytes({0xee}),
          "EDR-003 special state payload bytes mismatch");
}

}  // namespace

int main() {
  TestValidPlainValuePayloadRoundTrips();
  TestPlainValuePayloadRejectsMalformedInput();
  TestPlainValuePayloadSerializeRejectsInvalidStatePayloadPair();
  TestExecutionValueSerializationCompatibility();
  return EXIT_SUCCESS;
}
