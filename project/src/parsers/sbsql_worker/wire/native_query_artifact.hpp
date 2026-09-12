// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "wire/parser_server_ipc/parser_client_types.hpp"
#include "scratchbird/engine/sblr_envelope.hpp"
#include "engine/sblr/sblr_opcode_stream.hpp"
#include "core/uuid/uuid.hpp"
#include <algorithm>

namespace scratchbird::parser::sbsql {

enum class NativeQueryArtifactStatus { kOk, kIncomplete, kInvalid, kOverBudget };

// Structural agreement only. Call after owning live-context finalization;
// success here cannot grant receipt, authorization or engine execution rights.
inline NativeQueryArtifactStatus PublishNativeQueryArtifact(
    const ipc::ParserCanonicalSblrSubmission& submission,
    std::size_t maximum_bytes, std::string* output) {
  namespace sb = scratchbird::engine;
  if (!output || !submission.complete() || submission.canonical_operation_bytes.empty())
    return NativeQueryArtifactStatus::kIncomplete;
  if (submission.canonical_container_bytes.size() > maximum_bytes ||
      submission.canonical_operation_bytes.size() > maximum_bytes ||
      submission.canonical_execution_envelope_bytes.size() > maximum_bytes)
    return NativeQueryArtifactStatus::kOverBudget;
  if (!core::uuid::IsEngineIdentityUuid(submission.statement_uuid))
    return NativeQueryArtifactStatus::kInvalid;
  const auto container = sb::DecodeSblrContainerBytes(
      submission.canonical_container_bytes.data(), submission.canonical_container_bytes.size());
  const auto ingress = sb::DecodeSblrExecutionEnvelopeV1Bytes(
      submission.canonical_execution_envelope_bytes.data(),
      submission.canonical_execution_envelope_bytes.size());
  const auto stream = sb::sblr::DecodeSblrOpcodeStream(std::string_view(
      reinterpret_cast<const char*>(submission.canonical_operation_bytes.data()),
      submission.canonical_operation_bytes.size()));
  sb::SblrExecutionEnvelopeSemanticView semantic;
  if (container.status != sb::SblrCodecStatus::ok ||
      ingress.status != sb::SblrCodecStatus::ok || !stream.ok ||
      stream.stream.operations.size() != 3 ||
      stream.stream.operations[1].operation_id != "query.execute" ||
      stream.stream.operations[1].opcode != "SBLR_QUERY_EXECUTE" ||
      stream.stream.operations[1].opcode_code != 4615 ||
      container.container.operation_payload != submission.canonical_operation_bytes ||
      !std::equal(submission.statement_uuid.bytes.begin(), submission.statement_uuid.bytes.end(),
                  container.container.canonical_anchor.begin() + 116) ||
      ingress.envelope.fields[0].size() != 16 ||
      !std::equal(submission.statement_uuid.bytes.begin(), submission.statement_uuid.bytes.end(),
                  ingress.envelope.fields[0].begin()) ||
      !sb::SblrValidateExecutionEnvelopeFields(ingress.envelope, &semantic) ||
      semantic.payload_kind != sb::SblrPayloadKind::opcode_stream ||
      semantic.opcode_ref_kind != 1 ||
      semantic.opcode_inline_size != submission.canonical_operation_bytes.size() ||
      !std::equal(submission.canonical_operation_bytes.begin(), submission.canonical_operation_bytes.end(),
                  semantic.opcode_inline_data))
    return NativeQueryArtifactStatus::kInvalid;
  std::string staged(submission.canonical_container_bytes.begin(), submission.canonical_container_bytes.end());
  output->swap(staged);
  return NativeQueryArtifactStatus::kOk;
}

}  // namespace scratchbird::parser::sbsql
