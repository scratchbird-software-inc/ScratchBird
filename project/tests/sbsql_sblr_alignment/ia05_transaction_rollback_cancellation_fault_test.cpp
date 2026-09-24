// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "../support/binary_uuid_fixture.hpp"
#include "engine/sblr/sblr_dispatch.hpp"
#include "engine/sblr/sblr_engine_envelope.hpp"
#include "engine/sblr/sblr_transaction_rollback_runtime.hpp"

#include <atomic>
#include <iostream>

namespace s = scratchbird::engine::sblr;

int main() {
  s::SblrTransactionRollbackOptionsV1 options;
  options.transaction_uuid = scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-000000000355").bytes;
  options.local_transaction_id = 9;
  options.admitted_handle_evidence_sha256[0] = 2;
  const auto body = s::EncodeSblrTransactionRollbackOptionsV1(&options);
  auto envelope = s::MakeSblrEnvelope(
      "engine.op.txn_rollback", "SBLR_TXN_ROLLBACK", "ia05.txn_rollback.cancel");
  envelope.opcode_code = 258;
  envelope.result_shape = "rollback_result";
  envelope.diagnostic_shape = "diagnostic_vector";
  envelope.parser_package_uuid = scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-000000000356");
  envelope.registry_snapshot_uuid = scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-000000000357");
  envelope.parser_resolved_names_to_uuids = true;
  s::SblrOperand operand;
  operand.ordinal = 1;
  operand.type = "transaction.rollback.options";
  operand.name = "options";
  operand.value_kind = s::SblrValueKind::transaction_rollback_options;
  operand.value_body = body;
  envelope.operands.push_back(std::move(operand));
  const auto validation = s::ValidateSblrEnvelope(envelope);
  if (!validation.ok) {
    for (const auto& item : validation.diagnostics) std::cerr << item.code << ':' << item.message << '\n';
    return 1;
  }

  std::atomic<unsigned> cancellation_checks{0};
  scratchbird::engine::internal_api::EngineRequestContext context;
  context.security_context_present = true;
  context.transaction_uuid.bytes = options.transaction_uuid;
  context.local_transaction_id = options.local_transaction_id;
  context.query_cancellation_requested = [&] {
    ++cancellation_checks;
    return true;
  };
  const auto dispatched = s::DispatchSblrOperation(
      {context, std::move(envelope), {}, std::nullopt});
  std::cerr << "cancellation_checks=" << cancellation_checks << '\n';
  for (const auto& item : dispatched.diagnostics) std::cerr << item.code << ':' << item.message << '\n';
  for (const auto& item : dispatched.api_result.diagnostics) std::cerr << item.code << ':' << item.detail << '\n';
  if (dispatched.accepted || dispatched.api_result.ok ||
      !dispatched.api_result.evidence.empty() || cancellation_checks != 1 ||
      dispatched.api_result.diagnostics.empty() ||
      dispatched.api_result.diagnostics.front().code != "PROCESS.CANCELLED" ||
      dispatched.api_result.diagnostics.front().message_key !=
          "sblr.txn_rollback.cancelled_before_durable_decision") {
    return 1;
  }
  return 0;
}
