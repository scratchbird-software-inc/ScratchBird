// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "engine/sblr/sblr_dispatch.hpp"
#include "engine/sblr/sblr_local_metrics_read.hpp"
#include "engine/sblr/sblr_opcode_registry.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>

namespace sb = scratchbird::engine::sblr;

namespace {

[[noreturn]] void Fail(const std::string& what) { std::cerr << what << '\n'; std::exit(1); }
void Require(bool condition, const std::string& what) { if (!condition) Fail(what); }

sb::SblrLocalMetricsReadRequest Request() {
  sb::SblrLocalMetricsReadRequest request;
  request.query_class = sb::SblrLocalMetricsQueryClass::registry;
  request.page_size = 32;
  request.selector = "sys.metrics.*";
  request.request_uuid[0] = 1;
  return request;
}

sb::SblrOperationEnvelope Envelope(const sb::SblrLocalMetricsReadCodecResult& encoded) {
  auto envelope = sb::MakeSblrEnvelope("engine.op.read_metrics", "SBLR_READ_METRICS", "ia10b.local-metrics");
  envelope.opcode_code = 0x0c01;
  envelope.parser_package_uuid = "11111111-1111-1111-1111-111111111111";
  envelope.registry_snapshot_uuid = "22222222-2222-2222-2222-222222222222";
  envelope.operands = {sb::MakeSblrLocalMetricsReadOperand(encoded)};
  return envelope;
}

sb::SblrDispatchResult Dispatch(const sb::SblrLocalMetricsReadCodecResult& encoded,
                                bool authenticated = true, bool cancelled = false) {
  scratchbird::engine::internal_api::EngineRequestContext context;
  context.security_context_present = authenticated;
  if (authenticated) {
    context.trust_mode = scratchbird::engine::internal_api::EngineTrustMode::embedded_in_process;
    context.trace_tags.push_back("security.fixture_trace_authority");
    context.trace_tags.push_back("right:OBS_METRICS_READ_ALL");
  }
  if (cancelled) context.query_cancellation_requested = [] { return true; };
  sb::SblrDispatchRequest request;
  request.context = std::move(context);
  request.envelope = Envelope(encoded);
  return sb::DispatchSblrOperation(std::move(request));
}

}  // namespace

int main() {
  auto encoded = sb::EncodeSblrLocalMetricsReadRequest(Request());
  Require(encoded.ok, "local metrics request encode");
  const auto decoded = sb::DecodeSblrLocalMetricsReadRequest(
      encoded.canonical_bytes.data(), encoded.canonical_bytes.size());
  Require(decoded.ok && decoded.request.selector == "sys.metrics.*",
          "local metrics request roundtrip");
  auto corrupted = encoded.canonical_bytes;
  corrupted.back() ^= 1;
  Require(!sb::DecodeSblrLocalMetricsReadRequest(corrupted.data(), corrupted.size()).ok,
          "local metrics digest refusal");

  auto rejected = Request();
  rejected.selector = "cluster.sys.metrics.*";
  Require(!sb::EncodeSblrLocalMetricsReadRequest(rejected).ok,
          "local metrics cluster scope refusal");
  rejected = Request();
  rejected.cursor_digest[0] = 1;
  Require(!sb::EncodeSblrLocalMetricsReadRequest(rejected).ok,
          "local metrics unissued cursor refusal");

  const auto envelope = Envelope(encoded);
  const auto* entry = sb::LookupSblrOperation("engine.op.read_metrics");
  Require(entry != nullptr && entry->code == 0x0c01 &&
              entry->opcode == "SBLR_READ_METRICS" &&
              entry->operand_contract == "metrics_read_request" &&
              entry->result_contract == "metrics_result_set" &&
              entry->executor_id == "engine.op.read_metrics" &&
              entry->transaction_effect == sb::SblrOpcodeTransactionEffect::read &&
              entry->security_class == sb::SblrOpcodeSecurityClass::authenticated &&
              entry->requires_security_context &&
              !entry->requires_transaction_context &&
              !entry->requires_cluster_authority &&
              entry->executor_evidence_required &&
              !entry->executor_evidence_accepted &&
              entry->missing_executor_evidence_diagnostic ==
                  "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
          "exact fail-closed local metrics registry contract");
  Require(sb::LookupSblrOpcodeCode(0x0c01) == entry,
          "unique local metrics opcode identity");

  const auto unavailable = sb::ValidateSblrOpcodeForEnvelope(envelope);
  Require(!unavailable.ok &&
              unavailable.diagnostic_id ==
                  "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
          "registered local metrics executor evidence refusal");

  scratchbird::engine::internal_api::EngineRequestContext context;
  context.security_context_present = true;
  unsigned cancellation_probes = 0;
  context.query_cancellation_requested = [&] {
    ++cancellation_probes;
    return true;
  };
  const auto refused = sb::DispatchSblrLocalMetricsRead(envelope, context);
  Require(!refused.accepted &&
              refused.diagnostic_id ==
                  "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING" &&
              refused.evidence.empty() && cancellation_probes == 0,
          "local metrics evidence refusal before cancellation and API access");

  const auto full_dispatch = Dispatch(encoded, true, true);
  Require(!full_dispatch.accepted && !full_dispatch.dispatched_to_api &&
              !full_dispatch.api_result.ok &&
              full_dispatch.api_result.evidence.empty() &&
              !full_dispatch.api_result.diagnostics.empty() &&
              full_dispatch.api_result.diagnostics.front().code ==
                  "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
          "local metrics full dispatch publishes no synthetic result");

  auto zero_prefix = envelope;
  std::fill_n(zero_prefix.operands.front().value_body.begin(), 16, 0);
  Require(!sb::DecodeSblrLocalMetricsReadOperand(zero_prefix).ok,
          "local metrics zero descriptor prefix refusal");
  return 0;
}
