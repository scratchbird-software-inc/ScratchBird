#include "engine/sblr/sblr_dispatch.hpp"
#include "engine/sblr/sblr_engine_envelope.hpp"
#include "engine/sblr/sblr_transaction_begin_runtime.hpp"
#include <atomic>
#include <cstdlib>
#include <iostream>

namespace sblr = scratchbird::engine::sblr;

int main() {
  sblr::SblrTransactionBeginOptionsV1 options;
  options.isolation_profile_uuid[0] = 1;
  options.isolation_profile_generation = 1;
  options.transaction_policy_snapshot_uuid[0] = 2;
  options.transaction_policy_generation = 1;
  options.read_mode = 1;
  options.authority_scope = 1;
  options.wait_policy = 1;
  auto body = sblr::EncodeSblrTransactionBeginOptionsV1(&options);
  if (body.empty()) return EXIT_FAILURE;
  auto envelope = sblr::MakeSblrEnvelope(
      "engine.op.txn_begin", "SBLR_TXN_BEGIN", "ia05.txn_begin.cancel");
  envelope.opcode_code = 256;
  envelope.result_shape = "transaction_handle";
  envelope.diagnostic_shape = "diagnostic_vector";
  envelope.parser_package_uuid = "019d0000-0000-7000-8000-000000000348";
  envelope.registry_snapshot_uuid = "019d0000-0000-7000-8000-000000000349";
  envelope.parser_resolved_names_to_uuids = true;
  sblr::SblrOperand operand;
  operand.ordinal = 1;
  operand.type = "transaction.begin.options";
  operand.name = "options";
  operand.value_kind = sblr::SblrValueKind::transaction_begin_options;
  operand.value_body = std::move(body);
  envelope.operands.push_back(std::move(operand));
  if (!sblr::ValidateSblrEnvelope(envelope).ok) return EXIT_FAILURE;
  std::atomic<unsigned> probes{0};
  scratchbird::engine::internal_api::EngineRequestContext context;
  context.security_context_present = true;
  context.query_cancellation_requested = [&] { ++probes; return true; };
  const auto dispatched = sblr::DispatchSblrOperation(
      {context, std::move(envelope), {}, std::nullopt});
  if (dispatched.accepted || dispatched.api_result.ok ||
      !dispatched.api_result.evidence.empty() || probes.load() != 1 ||
      dispatched.api_result.diagnostics.empty() ||
      dispatched.api_result.diagnostics.front().code != "PROCESS.CANCELLED" ||
      dispatched.api_result.diagnostics.front().message_key !=
          "sblr.txn_begin.cancelled_before_durable_start") {
    std::cerr << "CSC-TEST-002348 cancellation/publication contract failed\n";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
