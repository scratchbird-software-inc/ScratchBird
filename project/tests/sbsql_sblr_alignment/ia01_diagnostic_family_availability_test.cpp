// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "engine/sblr/sblr_dispatch.hpp"
#include "engine/sblr/sblr_opcode_registry.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace api = scratchbird::engine::internal_api;
namespace sblr = scratchbird::engine::sblr;

namespace {

struct OperationSpec {
  std::string_view operation_id;
  std::string_view opcode;
  std::uint16_t opcode_code;
  std::string_view operand_contract;
  std::string_view operand_type;
  std::string_view operand_name;
  std::string_view result_shape;
  bool requires_transaction;
};

constexpr std::array<OperationSpec, 3> kOperations{{
    {"engine.op.diagnostic_refusal", "SBLR_DIAGNOSTIC_REFUSAL", 6400,
     "diagnostic_refusal_descriptor", "diagnostic.refusal", "refusal",
     "diagnostic_refusal_result", false},
    {"engine.op.diagnostic_reset", "SBLR_DIAGNOSTIC_RESET", 6401,
     "diagnostic_reset_descriptor", "diagnostic.reset", "reset",
     "diagnostic_reset_result", true},
    {"engine.op.descriptor_transform", "SBLR_DESCRIPTOR_TRANSFORM", 6402,
     "descriptor_transform_descriptor", "descriptor.transform", "transform",
     "descriptor_transform_result", false},
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
      "ia01.diagnostic_family.availability");
  envelope.opcode_code = spec.opcode_code;
  envelope.result_shape = spec.result_shape;
  envelope.diagnostic_shape = "diagnostic_vector";
  envelope.parser_package_uuid = "018f1000-0000-7000-8000-000000003859";
  envelope.registry_snapshot_uuid = "018f1000-0000-7000-8000-000000003860";
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

}  // namespace

int main() {
  for (const auto& spec : kOperations) {
    const auto* entry = sblr::LookupSblrOperation(spec.operation_id);
    Require(entry != nullptr, "diagnostic operation is absent from the registry");
    Require(entry->opcode == spec.opcode && entry->code == spec.opcode_code,
            "diagnostic operation identity drifted");
    Require(entry->operand_contract == spec.operand_contract &&
                entry->result_contract == spec.result_shape &&
                entry->executor_id == spec.operation_id,
            "diagnostic operation contract drifted");
    Require(entry->requires_security_context &&
                entry->requires_transaction_context ==
                    spec.requires_transaction &&
                entry->executor_evidence_required &&
                !entry->executor_evidence_accepted &&
                entry->missing_executor_evidence_diagnostic ==
                    "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
            "diagnostic operation availability contract drifted");

    auto envelope = ExactEnvelope(spec);
    const auto availability = sblr::ValidateSblrOpcodeForEnvelope(envelope);
    Require(!availability.ok &&
                availability.diagnostic_id ==
                    "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING" &&
                availability.detail ==
                    "executor_evidence_not_accepted:" +
                        std::string(spec.operation_id),
            "missing executor evidence did not fail closed exactly");

    sblr::SblrDispatchRequest request;
    request.context.security_context_present = true;
    request.context.transaction_uuid.canonical =
        "018f1000-0000-7000-8000-000000003855";
    request.context.local_transaction_id = 1;
    request.envelope = std::move(envelope);
    const auto dispatched = sblr::DispatchSblrOperation(std::move(request));
    Require(dispatched.envelope_validated && !dispatched.accepted &&
                !dispatched.dispatched_to_api &&
                !dispatched.canonical_result_published &&
                dispatched.diagnostics.size() == 1 &&
                dispatched.diagnostics.front().code ==
                    "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
            "diagnostic operation crossed the missing-evidence dispatch gate");
  }
  return EXIT_SUCCESS;
}
