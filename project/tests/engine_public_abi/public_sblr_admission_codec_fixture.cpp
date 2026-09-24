// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_engine_envelope.hpp"
#include "sblr_transaction_begin_runtime.hpp"

namespace scratchbird::test {
std::string CanonicalOperationBytes() {
  namespace sblr = scratchbird::engine::sblr;
  auto envelope = sblr::MakeSblrEnvelope(
      "engine.op.txn_begin", "SBLR_TXN_BEGIN",
      "public-abi-canonical-codec-structure");
  envelope.opcode_code = 0x0100u;
  envelope.result_shape = "transaction_handle";
  envelope.diagnostic_shape = "diagnostic_vector";
  envelope.parser_package_uuid =
      {{0x01,0x9e,0x05,0xb1,0xf0,0x09,0x70,0x00,0x80,0,0,0,0,0,0,0x20}};
  envelope.registry_snapshot_uuid =
      {{0x01,0x9e,0x05,0xb1,0xf0,0x09,0x70,0x00,0x80,0,0,0,0,0,0,0x21}};

  sblr::SblrTransactionBeginOptionsV1 options;
  options.isolation_profile_uuid[0] = 1;
  options.isolation_profile_generation = 1;
  options.transaction_policy_snapshot_uuid[0] = 2;
  options.transaction_policy_generation = 1;
  options.read_mode = 1;
  options.authority_scope = 1;
  options.wait_policy = 1;

  sblr::SblrOperand operand;
  operand.type = "transaction.begin_options";
  operand.name = "options";
  operand.ordinal = 1;
  operand.value_kind = sblr::SblrValueKind::transaction_begin_options;
  operand.value_body =
      sblr::EncodeSblrTransactionBeginOptionsV1(&options);
  envelope.operands.push_back(std::move(operand));
  return sblr::EncodeSblrEnvelope(envelope);
}

bool ValidateCanonicalOperationBytes(std::string_view bytes) {
  namespace sblr = scratchbird::engine::sblr;
  const auto decoded = sblr::DecodeSblrEnvelope(bytes);
  if (!decoded.ok || decoded.envelope.operation_id != "engine.op.txn_begin" ||
      decoded.envelope.opcode != "SBLR_TXN_BEGIN" ||
      std::string(decoded.canonical_bytes.begin(), decoded.canonical_bytes.end()) != bytes ||
      sblr::EncodeSblrEnvelope(decoded.envelope) != bytes) return false;
  std::string corrupt(bytes);
  if (corrupt.empty()) return false;
  corrupt.back() ^= 0x01;
  return !sblr::DecodeSblrEnvelope(corrupt).ok;
}
}  // namespace scratchbird::test
