// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "scratchbird/engine/sblr/lowering.hpp"
#include "scratchbird/engine/sblr/raising.hpp"

#include <cstdint>
#include <string>
#include <vector>

int main() {
  const std::uint8_t payload[] = {'p', 'l', 'a', 'n'};
  const std::vector<std::uint8_t> expected = {
      83, 66, 76, 82, 1, 0, 0, 0, 2, 0, 10, 0, 1, 0, 1, 0, 0, 0, 0, 0, 4, 0, 0, 0,
      198, 1, 77, 159, 0, 0, 0, 0, 1, 0, 0, 0, 4, 0, 0, 0, 112, 108, 97, 110, 112, 108, 97, 110,
  };
  const auto encoded = scratchbird::engine::sblr::EnvelopeBuilder()
                           .operation(scratchbird::engine::SblrOperationFamily::relational_query, 1)
                           .descriptor(1, payload, sizeof(payload))
                           .append_bytes(payload, sizeof(payload))
                           .encode();
  if (encoded != expected) {
    return 1;
  }
  const auto decoded = scratchbird::engine::sblr::EnvelopeReader::decode(expected.data(), expected.size());
  if (decoded.status != scratchbird::engine::SblrCodecStatus::ok ||
      decoded.envelope.family != scratchbird::engine::SblrOperationFamily::relational_query ||
      decoded.envelope.opcode != 1 || decoded.envelope.descriptors.size() != 1 ||
      decoded.envelope.canonical_bytes.size() != sizeof(payload)) {
    return 2;
  }
  const auto reencoded = scratchbird::engine::EncodeSblrEnvelope(decoded.envelope);
  if (reencoded != expected) {
    return 3;
  }
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
  const auto encoded_operation = scratchbird::engine::sblr::EnvelopeBuilder()
                                     .operation(scratchbird::engine::SblrOperationFamily::management_inspect, 1)
                                     .payload_kind(scratchbird::engine::SblrPayloadKind::operation_envelope)
                                     .append_bytes(reinterpret_cast<const std::uint8_t*>(operation_payload.data()),
                                                   operation_payload.size())
                                     .encode();
  const auto decoded_operation =
      scratchbird::engine::sblr::EnvelopeReader::decode(encoded_operation.data(), encoded_operation.size());
  if (decoded_operation.status != scratchbird::engine::SblrCodecStatus::ok ||
      decoded_operation.envelope.payload_kind != scratchbird::engine::SblrPayloadKind::operation_envelope ||
      decoded_operation.envelope.family != scratchbird::engine::SblrOperationFamily::management_inspect ||
      decoded_operation.envelope.opcode != 1 ||
      decoded_operation.envelope.descriptors.size() != 0) {
    return 4;
  }
  const std::string round_trip_payload(decoded_operation.envelope.canonical_bytes.begin(),
                                       decoded_operation.envelope.canonical_bytes.end());
  if (round_trip_payload != operation_payload) {
    return 5;
  }
  const auto reencoded_operation = scratchbird::engine::EncodeSblrEnvelope(decoded_operation.envelope);
  if (reencoded_operation != encoded_operation) {
    return 6;
  }
  return 0;
}
