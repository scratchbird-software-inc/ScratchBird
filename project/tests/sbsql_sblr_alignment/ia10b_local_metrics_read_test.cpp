// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "../support/binary_uuid_fixture.hpp"
#include "engine/sblr/sblr_dispatch.hpp"
#include "engine/sblr/sblr_local_metrics_read.hpp"
#include "engine/sblr/sblr_opcode_registry.hpp"
#include "hash_digest.hpp"

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
  request.request_uuid = scratchbird::tests::FixtureUuidLiteral(
      "019f2000-0000-7000-8000-000000000001").bytes;
  return request;
}

sb::SblrOperationEnvelope Envelope(const sb::SblrLocalMetricsReadCodecResult& encoded) {
  auto envelope = sb::MakeSblrEnvelope("engine.op.read_metrics", "SBLR_READ_METRICS", "ia10b.local-metrics");
  envelope.opcode_code = 0x0c01;
  envelope.parser_package_uuid = scratchbird::tests::FixtureUuidLiteral("019f2000-0000-7000-8000-000000000002");
  envelope.registry_snapshot_uuid = scratchbird::tests::FixtureUuidLiteral("019f2000-0000-7000-8000-000000000003");
  envelope.operands = {sb::MakeSblrLocalMetricsReadOperand(encoded)};
  return envelope;
}

sb::SblrDispatchResult Dispatch(const sb::SblrLocalMetricsReadCodecResult& encoded,
                                bool authenticated = true, bool cancelled = false) {
  scratchbird::engine::internal_api::EngineRequestContext context;
  context.security_context_present = authenticated;
  if (authenticated) {
    context.trust_mode = scratchbird::engine::internal_api::EngineTrustMode::embedded_in_process;
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
  constexpr auto descriptor = scratchbird::tests::FixtureUuidLiteral(
      "01a0e93a-c1c8-7e42-979f-d4a0a14dc1d2");
  Require(std::equal(descriptor.bytes.begin(), descriptor.bytes.end(),
                     envelope.operands.front().value_body.begin()),
          "independent local metrics carrier descriptor");
  Require(sb::ValidateSblrEnvelope(envelope).ok,
          "local metrics generic native SBOP validation");
  const auto wire = sb::EncodeSblrEnvelope(envelope);
  const auto roundtrip = sb::DecodeSblrEnvelope(wire);
  Require(!wire.empty() && roundtrip.ok &&
              sb::EncodeSblrEnvelope(roundtrip.envelope) == wire,
          "local metrics canonical SBOP roundtrip");
  const auto nested = sb::DecodeSblrLocalMetricsReadOperand(roundtrip.envelope);
  Require(nested.ok && nested.request.request_uuid == Request().request_uuid &&
              nested.canonical_bytes == encoded.canonical_bytes,
          "local metrics native request retained across SBOP");
  for (unsigned byte = 0; byte < 16; ++byte) {
    for (unsigned value = 0; value < 256; ++value) {
      if (value == descriptor.bytes[byte]) continue;
      auto bad = envelope;
      bad.operands.front().value_body[byte] = static_cast<std::uint8_t>(value);
      const auto refused_carrier = sb::DecodeSblrLocalMetricsReadOperand(bad);
      Require(!refused_carrier.ok && refused_carrier.canonical_bytes.empty(),
              "local metrics descriptor mutation admitted");
    }
  }
  const auto reject_identity = [&](const std::array<std::uint8_t, 16>& id) {
    auto invalid = Request();
    invalid.request_uuid = id;
    const auto refused_encode = sb::EncodeSblrLocalMetricsReadRequest(invalid);
    Require(!refused_encode.ok && refused_encode.canonical_bytes.empty(),
            "invalid metrics system identity encoded");
    auto resealed = encoded.canonical_bytes;
    std::copy(id.begin(), id.end(), resealed.begin() + 48);
    const auto digest = scratchbird::core::hash::ComputeSha256Digest(
        resealed.data(), resealed.size() - 32);
    Require(digest.ok(), "metrics invalid-identity resealing failed");
    std::copy(digest.digest.begin(), digest.digest.end(), resealed.end() - 32);
    const auto refused_decode = sb::DecodeSblrLocalMetricsReadRequest(
        resealed.data(), resealed.size());
    Require(!refused_decode.ok && refused_decode.canonical_bytes.empty() &&
                refused_decode.request.request_uuid == std::array<std::uint8_t, 16>{},
            "resealed invalid metrics system identity decoded");
  };
  reject_identity({});
  for (unsigned version = 0; version < 16; ++version) {
    if (version == 7) continue;
    auto id = Request().request_uuid;
    id[6] = static_cast<std::uint8_t>(version << 4);
    reject_identity(id);
  }
  for (unsigned variant : {0u, 0x40u, 0xc0u}) {
    auto id = Request().request_uuid;
    id[8] = static_cast<std::uint8_t>(variant);
    reject_identity(id);
  }
  auto legacy_carrier = envelope;
  std::fill_n(legacy_carrier.operands.front().value_body.begin(), 16, 0);
  legacy_carrier.operands.front().value_body[0] = 1;
  Require(!sb::DecodeSblrLocalMetricsReadOperand(legacy_carrier).ok &&
              !sb::ValidateSblrEnvelope(legacy_carrier).ok,
          "legacy local metrics discriminator admitted");
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
