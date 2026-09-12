// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "sblr_admission.hpp"
#include "sblr_local_gateway.hpp"
#include "engine/sblr/sblr_opcode_registry.hpp"
#include "engine/sblr/sblr_transaction_commit_runtime.hpp"
#include "scratchbird/engine/sblr_envelope.hpp"

namespace scratchbird::server {

// The token retains the complete decoded package. Consumers must not assume
// the command occupies record1 when structural SOURCE_MAP metadata is present.
// This view grants no authority; engine dispatch still revalidates the receipt,
// original canonical bytes, reservations and evidence hashes.
inline const scratchbird::engine::sblr::SblrOperationEnvelope*
AdmittedServerSblrCommand(const ServerSblrAdmissionToken& token) {
  if (!token || !token->opcode_stream) return nullptr;
  const auto index = SelectServerSblrCommandIndex(token->stream);
  if (!index) return nullptr;
  const auto& command = token->stream.operations[*index];
  if (!scratchbird::engine::sblr::ValidateSblrOpcodeIdentity(
           command.opcode_code, command.operation_id, command.opcode).ok ||
      command.opcode_code != token->operation.opcode_code ||
      command.operation_id != token->operation.operation_id ||
      command.opcode != token->operation.opcode)
    return nullptr;
  return &command;
}

inline bool ServerSblrHasTerminalCursorPayload(
    const ServerSblrAdmissionToken& token) {
  const auto* command = AdmittedServerSblrCommand(token);
  return command != nullptr &&
         (command->operation_id == "engine.op.stmt_execute" ||
          command->operation_id == "engine.op.stmt_execute_direct" ||
          command->operation_id == "engine.op.catalog_introspect");
}

inline std::optional<scratchbird::engine::sblr::SblrTransactionCommitOptionsV1>
CanonicalTransactionCommitOptionsFromContainer(std::string_view encoded,
                                               std::string* detail) {
  namespace tx = scratchbird::engine::sblr;
  const auto container = scratchbird::engine::DecodeSblrContainerBytes(
      reinterpret_cast<const std::uint8_t*>(encoded.data()), encoded.size());
  if (container.status != scratchbird::engine::SblrCodecStatus::ok ||
      container.container.operation_payload.empty() ||
      scratchbird::engine::SblrReadU16(container.container.canonical_anchor.data() + 100) !=
          static_cast<std::uint16_t>(scratchbird::engine::SblrPayloadKind::opcode_stream)) {
    if (detail != nullptr) *detail = "canonical_commit_container_invalid";
    return std::nullopt;
  }
  const std::string_view operation_bytes(
      reinterpret_cast<const char*>(container.container.operation_payload.data()),
      container.container.operation_payload.size());
  const auto stream = tx::DecodeSblrOpcodeStream(operation_bytes);
  const auto index = stream.ok ? SelectServerSblrCommandIndex(stream.stream) : std::nullopt;
  if (!index) {
    if (detail != nullptr) *detail = "canonical_commit_stream_shape_invalid";
    return std::nullopt;
  }
  const auto& operation = stream.stream.operations[*index];
  if (operation.operation_id != "engine.op.txn_commit" ||
      operation.opcode != "SBLR_TXN_COMMIT" || operation.opcode_code != 257 ||
      operation.operands.size() != 1) {
    if (detail != nullptr) *detail = "canonical_commit_identity_invalid";
    return std::nullopt;
  }
  const auto& operand = operation.operands.front();
  if (operand.ordinal != 1 || operand.type != "transaction.commit.options" ||
      operand.name != "options" || operand.value_kind != tx::SblrValueKind::transaction_commit_options) {
    if (detail != nullptr) *detail = "canonical_commit_operand_identity_invalid";
    return std::nullopt;
  }
  tx::SblrTransactionCommitOptionsV1 options;
  std::string decode_detail;
  if (!tx::DecodeSblrTransactionCommitOptionsV1(
          operand.value_body.data(), operand.value_body.size(), &options, &decode_detail)) {
    if (detail != nullptr)
      *detail = decode_detail.empty() ? "canonical_commit_options_invalid" : decode_detail;
    return std::nullopt;
  }
  return options;
}

}  // namespace scratchbird::server
