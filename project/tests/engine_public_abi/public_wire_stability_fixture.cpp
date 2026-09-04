// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "scratchbird/engine/sblr/lowering.hpp"
#include "scratchbird/engine/sblr/raising.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::array<std::uint8_t, 16> CanonicalUuid(std::uint8_t suffix) {
  std::array<std::uint8_t, 16> value{};
  value[0] = 0x12;
  value[6] = 0x70;
  value[8] = 0x80;
  value[15] = suffix;
  return value;
}

std::array<std::uint8_t, 132> CanonicalAnchor(std::uint8_t request_suffix) {
  std::array<std::uint8_t, 132> anchor{};
  const auto engine_uuid = CanonicalUuid(0x21);
  const auto dialect_uuid = CanonicalUuid(0x22);
  const auto parser_uuid = CanonicalUuid(0x31);
  const auto bundle_uuid = CanonicalUuid(0x24);
  const auto request_uuid = CanonicalUuid(request_suffix);
  std::copy(engine_uuid.begin(), engine_uuid.end(), anchor.begin());
  std::copy(dialect_uuid.begin(), dialect_uuid.end(), anchor.begin() + 16);
  std::copy(parser_uuid.begin(), parser_uuid.end(), anchor.begin() + 32);
  anchor[48] = 1;
  anchor[52] = 1;
  anchor[60] = 1;
  anchor[68] = 1;
  std::copy(bundle_uuid.begin(), bundle_uuid.end(), anchor.begin() + 76);
  anchor[92] = 1;
  anchor[100] = static_cast<std::uint8_t>(
      scratchbird::engine::SblrPayloadKind::operation_envelope);
  std::copy(request_uuid.begin(), request_uuid.end(), anchor.begin() + 116);
  return anchor;
}

std::vector<std::uint8_t> HexBytes(std::string_view hex) {
  const auto digit = [](char value) -> int {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return -1;
  };
  if ((hex.size() & 1u) != 0) return {};
  std::vector<std::uint8_t> bytes;
  bytes.reserve(hex.size() / 2);
  for (std::size_t offset = 0; offset < hex.size(); offset += 2) {
    const int high = digit(hex[offset]);
    const int low = digit(hex[offset + 1]);
    if (high < 0 || low < 0) return {};
    bytes.push_back(static_cast<std::uint8_t>((high << 4) | low));
  }
  return bytes;
}

}  // namespace

int main() {
  namespace engine = scratchbird::engine;
  namespace sblr = scratchbird::engine::sblr;

  const std::uint8_t payload[] = {'p', 'l', 'a', 'n'};
  const auto expected = HexBytes(
      "53424c5201000100000000000000000004000000000000000400000000000000"
      "2200000000000000100000008400000012000000000070008000000000000021"
      "1200000000007000800000000000002212000000000070008000000000000031"
      "0100000001000000000000000100000000000000010000000000000012000000"
      "0000700080000000000000240100000000000000020000000000000000000000"
      "00000000120000000000700080000000000000252000000004000000706c616e"
      "fe0000000c0000005b89464ad400000000000000");

  engine::SblrCanonicalContainer container;
  container.canonical_anchor = CanonicalAnchor(0x25);
  container.operation_payload.assign(payload, payload + sizeof(payload));
  const auto encoded = engine::EncodeSblrContainer(container);
  if (encoded != expected) return 1;

  const auto decoded = engine::DecodeSblrContainerBytes(
      expected.data(), expected.size());
  if (decoded.status != scratchbird::engine::SblrCodecStatus::ok ||
      decoded.container.canonical_anchor != container.canonical_anchor ||
      decoded.container.operation_payload != container.operation_payload ||
      !decoded.container.capability_profile_pin.empty() ||
      !decoded.container.lowering_metadata.empty() ||
      !decoded.container.source_map.empty() ||
      !decoded.container.diagnostic_vector.empty()) {
    return 2;
  }
  if (engine::EncodeSblrContainer(decoded.container) != expected) return 3;

  // The legacy source-compatible facade has no authority to invent an anchor.
  const auto anchorless = sblr::EnvelopeBuilder()
                              .operation(
                                  scratchbird::engine::SblrOperationFamily::relational_query,
                                  1)
                              .descriptor(1, payload, sizeof(payload))
                              .append_bytes(payload, sizeof(payload))
                              .encode();
  if (!anchorless.empty()) return 4;

  const std::string operation_payload =
      "operation_id=cluster.inspect_provider\n"
      "opcode=SBLR_CLUSTER_INSPECT_PROVIDER\n"
      "sblr_operation_family=sblr.cluster.private_operation.v3\n"
      "result_shape=cluster.provider.info.v1\n"
      "diagnostic_shape=engine.diagnostic.v1\n"
      "trace_key=cluster-provider-info-binary-round-trip\n"
      "contains_sql_text=false\n"
      "parser_resolved_names_to_uuids=true\n"
      "requires_security_context=true\n"
      "requires_transaction_context=false\n"
      "requires_cluster_authority=false\n";
  engine::SblrCanonicalContainer operation_container;
  operation_container.canonical_anchor = CanonicalAnchor(0x26);
  operation_container.operation_payload.assign(operation_payload.begin(),
                                               operation_payload.end());
  const auto encoded_operation =
      engine::EncodeSblrContainer(operation_container);
  if (encoded_operation.empty()) return 5;
  const auto decoded_operation = engine::DecodeSblrContainerBytes(
      encoded_operation.data(), encoded_operation.size());
  if (decoded_operation.status != scratchbird::engine::SblrCodecStatus::ok ||
      decoded_operation.container.canonical_anchor !=
          operation_container.canonical_anchor ||
      decoded_operation.container.operation_payload !=
          operation_container.operation_payload) {
    return 6;
  }
  if (engine::EncodeSblrContainer(decoded_operation.container) !=
      encoded_operation) {
    return 7;
  }
  return 0;
}
