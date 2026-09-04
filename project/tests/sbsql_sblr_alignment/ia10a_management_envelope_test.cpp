// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "engine/sblr/sblr_dispatch.hpp"
#include "engine/sblr/sblr_management_envelope.hpp"
#include "hash_digest.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace sb = scratchbird::engine::sblr;
namespace hash = scratchbird::core::hash;

namespace {

constexpr char kUuidA[] = "11111111-1111-1111-1111-111111111111";
constexpr char kUuidB[] = "22222222-2222-2222-2222-222222222222";
constexpr char kUuidC[] = "33333333-3333-3333-3333-333333333333";

[[noreturn]] void Fail(const std::string& what) { std::cerr << what << '\n'; std::exit(1); }
void Require(bool condition, const std::string& what) { if (!condition) Fail(what); }

std::vector<std::string> Names(sb::SblrManagementEnvelopeKind kind) {
  using K = sb::SblrManagementEnvelopeKind;
  if (kind == K::operation) return {"operation_uuid", "opcode", "opcode_version_major", "opcode_version_minor", "target_database_uuid", "target_filespace_uuid", "target_cluster_uuid", "target_node_uuid", "target_object_uuid", "security_context_uuid", "policy_snapshot_uuid", "policy_epoch", "request_source", "idempotency_key", "causal_transaction_id", "requested_snapshot", "wait_mode", "timeout_ms", "dry_run", "audit_reason", "diagnostic_locale", "disclosure_class", "operation_generation"};
  if (kind == K::payload) return {"operation_uuid", "payload_schema_uuid", "payload_version_major", "payload_version_minor", "canonical_serialization_hash", "payload_body"};
  if (kind == K::result) return {"operation_uuid", "terminal_state", "summary_code", "affected_object_counts", "durable_state_refs", "diagnostic_vector_uuid", "metric_snapshot_refs", "evidence_bundle_uuid"};
  if (kind == K::progress) return {"operation_uuid", "phase", "phase_generation", "total_work_estimate", "completed_work", "current_object_uuid", "retry_count", "last_safe_retry_point"};
  if (kind == K::diagnostic) return {"operation_uuid", "diagnostic_code", "severity", "object_uuid", "search_key", "safe_human_text", "disclosure_class", "retry_class"};
  return {"operation_uuid", "metric_scope", "metric_path", "sample_window", "metric_row_refs", "classification"};
}

sb::SblrManagementEnvelopeRecord Record(sb::SblrManagementEnvelopeKind kind,
                                        const std::string& operation_uuid) {
  sb::SblrManagementEnvelopeRecord record; record.kind = kind;
  for (const auto& name : Names(kind)) {
    std::string value = "1";
    if (name == "operation_uuid") value = operation_uuid;
    else if (name.find("uuid") != std::string::npos) value = kUuidB;
    else if (name == "opcode") value = "mga.checkpoint";
    else if (name == "request_source") value = "test_harness";
    else if (name == "wait_mode") value = "try";
    else if (name == "dry_run") value = "false";
    else if (name == "audit_reason") value = "focused_test";
    else if (name == "diagnostic_locale") value = "en";
    else if (name == "disclosure_class") value = "operational";
    else if (name == "terminal_state") value = "completed";
    else if (name == "metric_scope") value = "local";
    else if (name == "metric_path") value = "sys.metrics.mga.cmo.operations_accepted_total";
    else if (name == "payload_body") value = "payload";
    record.fields.push_back({name, {value.begin(), value.end()}});
  }
  if (kind == sb::SblrManagementEnvelopeKind::payload) {
    const auto digest = hash::ComputeSha256Digest(
        record.fields.back().value);
    const auto rendered = hash::HexLower(digest.digest);
    for (auto& field : record.fields) if (field.name == "canonical_serialization_hash") field.value.assign(rendered.begin(), rendered.end());
  }
  return record;
}

sb::SblrOperationEnvelope Envelope(const sb::SblrManagementEnvelopeRecord& record) {
  const auto encoded = sb::EncodeSblrManagementEnvelopeRecord(record);
  Require(encoded.ok, "record encoding: " + encoded.detail);
  auto envelope = sb::MakeSblrEnvelope(
      std::string(sb::ManagementEnvelopeOperationId(record.kind)),
      std::string(sb::ManagementEnvelopeOpcode(record.kind)), "ia10a.management-envelope");
  envelope.opcode_code = sb::ManagementEnvelopeOpcodeCode(record.kind);
  envelope.parser_package_uuid = kUuidB;
  envelope.registry_snapshot_uuid = kUuidC;
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.operands = {sb::MakeSblrManagementEnvelopeOperand(encoded)};
  return envelope;
}

sb::SblrDispatchResult Dispatch(const sb::SblrManagementEnvelopeRecord& record,
                                bool security = true, bool cancelled = false) {
  scratchbird::engine::internal_api::EngineRequestContext context;
  context.security_context_present = security;
  context.local_transaction_id = 1;
  context.transaction_uuid.canonical = kUuidA;
  if (cancelled) context.query_cancellation_requested = [] { return true; };
  sb::SblrDispatchRequest request; request.context = std::move(context); request.envelope = Envelope(record);
  return sb::DispatchSblrOperation(std::move(request));
}

}  // namespace

int main() {
  using K = sb::SblrManagementEnvelopeKind;
  for (const auto kind : {K::operation, K::payload, K::result, K::progress, K::diagnostic, K::metric_snapshot_ref}) {
    const auto encoded = sb::EncodeSblrManagementEnvelopeRecord(Record(kind, kUuidA));
    Require(encoded.ok, "CSC-TEST-003328 encode");
    const auto decoded = sb::DecodeSblrManagementEnvelopeRecord(encoded.canonical_bytes.data(), encoded.canonical_bytes.size());
    Require(decoded.ok && decoded.record.kind == kind, "CSC-TEST-003328 roundtrip");
    auto corrupted = encoded.canonical_bytes; corrupted[0] ^= 1;
    Require(!sb::DecodeSblrManagementEnvelopeRecord(corrupted.data(), corrupted.size()).ok, "CSC-TEST-003328 magic refusal");
    corrupted = encoded.canonical_bytes; corrupted.back() ^= 1;
    Require(!sb::DecodeSblrManagementEnvelopeRecord(corrupted.data(), corrupted.size()).ok, "CSC-TEST-003328 crc refusal");
  }

  for (const auto kind : {K::operation, K::payload, K::result, K::progress, K::diagnostic, K::metric_snapshot_ref}) {
    const auto refused = Dispatch(Record(kind, kUuidA));
    Require(!refused.accepted, "management executor evidence must be refused");
    Require(!refused.api_result.diagnostics.empty() &&
                refused.api_result.diagnostics.front().code ==
                    "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
            "management refusal diagnostic: " +
                (refused.api_result.diagnostics.empty()
                     ? std::string("<none>")
                     : refused.api_result.diagnostics.front().code));
    Require(!refused.dispatched_to_api && !refused.canonical_result_published,
            "management refusal published state");
  }
  Require(!Dispatch(Record(K::operation, kUuidB), true, true).accepted,
          "CSC-TEST-003333 cancellation refusal");
  Require(!Dispatch(Record(K::operation, kUuidC), false).accepted,
          "security refusal");
  return 0;
}
