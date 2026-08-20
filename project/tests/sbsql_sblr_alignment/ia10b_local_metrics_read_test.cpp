// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "engine/sblr/sblr_dispatch.hpp"
#include "engine/sblr/sblr_local_metrics_read.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

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
  Require(encoded.ok, "CSC-TEST-003334 encode");
  const auto decoded = sb::DecodeSblrLocalMetricsReadRequest(
      encoded.canonical_bytes.data(), encoded.canonical_bytes.size());
  Require(decoded.ok && decoded.request.selector == "sys.metrics.*", "CSC-TEST-003334 roundtrip");
  auto corrupted = encoded.canonical_bytes;
  corrupted.back() ^= 1;
  Require(!sb::DecodeSblrLocalMetricsReadRequest(corrupted.data(), corrupted.size()).ok,
          "CSC-TEST-003334 digest refusal");

  auto rejected = Request();
  rejected.selector = "cluster.sys.metrics.*";
  Require(!sb::EncodeSblrLocalMetricsReadRequest(rejected).ok,
          "CSC-TEST-003335 cluster scope refusal");
  rejected = Request();
  rejected.cursor_digest[0] = 1;
  Require(!sb::EncodeSblrLocalMetricsReadRequest(rejected).ok,
          "CSC-TEST-003335 unissued cursor refusal");

  const auto dispatch = Dispatch(encoded);
  Require(dispatch.accepted && dispatch.api_result.ok, "CSC-TEST-003336 local projection admission");
  bool executor_evidence = false;
  for (const auto& evidence : dispatch.api_result.evidence) {
    executor_evidence = executor_evidence ||
        (evidence.evidence_kind == "executor_id" && evidence.evidence_id == "engine.op.read_metrics");
  }
  Require(executor_evidence, "CSC-TEST-003336 executor evidence");
  Require(!Dispatch(encoded, false).accepted, "CSC-TEST-003336 security refusal");
  Require(!Dispatch(encoded, true, true).accepted, "CSC-TEST-003337 cancellation before projection");
  return 0;
}
