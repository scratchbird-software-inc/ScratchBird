// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "engine/sblr/sblr_dispatch.hpp"

#include <array>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace sblr = scratchbird::engine::sblr;

namespace {

struct OperationSpec {
  std::string_view operation_id;
  std::string_view opcode;
  std::uint16_t opcode_code;
  std::string_view operand_type;
  std::string_view operand_name;
  std::string_view result_shape;
  bool requires_transaction;
};

constexpr std::array<OperationSpec, 3> kOperations{{
    {"engine.op.diagnostic_refusal", "SBLR_DIAGNOSTIC_REFUSAL", 6400,
     "diagnostic.refusal", "refusal", "diagnostic_refusal_result", false},
    {"engine.op.diagnostic_reset", "SBLR_DIAGNOSTIC_RESET", 6401,
     "diagnostic.reset", "reset", "diagnostic_reset_result", true},
    {"engine.op.descriptor_transform", "SBLR_DESCRIPTOR_TRANSFORM", 6402,
     "descriptor.transform", "transform", "descriptor_transform_result",
     false},
}};

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) Fail(message);
}

sblr::SblrOperationEnvelope ExactEnvelope(const OperationSpec& spec) {
  auto envelope = sblr::MakeSblrEnvelope(
      std::string(spec.operation_id), std::string(spec.opcode),
      "ia01.diagnostic_family.cancellation");
  envelope.opcode_code = spec.opcode_code;
  envelope.result_shape = spec.result_shape;
  envelope.diagnostic_shape = "diagnostic_vector";
  envelope.parser_package_uuid = "018f1000-0000-7000-8000-000000003852";
  envelope.registry_snapshot_uuid = "018f1000-0000-7000-8000-000000003856";
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = spec.requires_transaction;
  envelope.parser_resolved_names_to_uuids = true;
  sblr::SblrOperand operand;
  operand.ordinal = 1;
  operand.type = spec.operand_type;
  operand.name = spec.operand_name;
  operand.value_kind = sblr::SblrValueKind::descriptor_ref;
  operand.value_body.assign(16, 0);
  operand.value_body.front() = 1;
  envelope.operands.push_back(std::move(operand));
  return envelope;
}

sblr::SblrDispatchRequest Request(const OperationSpec& spec,
                                  std::atomic<unsigned>* cancellation_probes) {
  sblr::SblrDispatchRequest request;
  request.context.security_context_present = true;
  request.context.transaction_uuid.canonical =
      "018f1000-0000-7000-8000-000000003854";
  request.context.local_transaction_id = 1;
  request.context.query_cancellation_requested = [cancellation_probes] {
    cancellation_probes->fetch_add(1, std::memory_order_relaxed);
    return true;
  };
  request.envelope = ExactEnvelope(spec);
  return request;
}

}  // namespace

int main() {
  for (const auto& spec : kOperations) {
    std::atomic<unsigned> cancellation_probes{0};
    auto request = Request(spec, &cancellation_probes);
    const auto canonical_before = sblr::EncodeSblrEnvelope(request.envelope);
    Require(!canonical_before.empty(),
            "cancellation fixture did not produce a canonical envelope");

    const auto preflight = sblr::PreflightSblrQueryOperation(request);
    Require(!preflight.ok &&
                preflight.diagnostic_id ==
                    "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
            "cancellation bypassed the mandatory executor-evidence gate");
    Require(cancellation_probes.load(std::memory_order_relaxed) == 0,
            "cancellation was observed before executor evidence admission");

    const auto dispatched = sblr::DispatchSblrOperation(std::move(request));
    Require(!dispatched.accepted && !dispatched.dispatched_to_api &&
                !dispatched.physical_dag_published &&
                !dispatched.physical_dag_executed &&
                !dispatched.canonical_result_published &&
                dispatched.diagnostics.size() == 1 &&
                dispatched.diagnostics.front().code ==
                    "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
            "cancelled diagnostic operation crossed the evidence boundary");
    Require(cancellation_probes.load(std::memory_order_relaxed) == 0,
            "unadmitted diagnostic operation reached executor cancellation");

    auto malformed = Request(spec, &cancellation_probes);
    malformed.envelope.operands.front().value_body.assign(16, 0);
    const auto malformed_preflight =
        sblr::PreflightSblrQueryOperation(std::move(malformed));
    Require(!malformed_preflight.ok &&
                malformed_preflight.diagnostic_id == "SBLR.OPERAND_INVALID",
            "malformed descriptor did not precede evidence/cancellation");
    Require(cancellation_probes.load(std::memory_order_relaxed) == 0,
            "malformed descriptor reached executor cancellation");
  }
  return EXIT_SUCCESS;
}
