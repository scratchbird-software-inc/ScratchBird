// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "engine/sblr/sblr_engine_envelope.hpp"

#include <array>
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
};

constexpr std::array<OperationSpec, 3> kOperations{{
    {"engine.op.diagnostic_refusal", "SBLR_DIAGNOSTIC_REFUSAL", 6400,
     "diagnostic.refusal", "refusal", "diagnostic_refusal_result"},
    {"engine.op.diagnostic_reset", "SBLR_DIAGNOSTIC_RESET", 6401,
     "diagnostic.reset", "reset", "diagnostic_reset_result"},
    {"engine.op.descriptor_transform", "SBLR_DESCRIPTOR_TRANSFORM", 6402,
     "descriptor.transform", "transform", "descriptor_transform_result"},
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
      "ia01.diagnostic_family.contract");
  envelope.opcode_code = spec.opcode_code;
  envelope.result_shape = spec.result_shape;
  envelope.diagnostic_shape = "diagnostic_vector";
  envelope.parser_package_uuid = "018f1000-0000-7000-8000-000000003850";
  envelope.registry_snapshot_uuid = "018f1000-0000-7000-8000-000000003851";
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = spec.opcode_code == 6401;
  envelope.requires_cluster_authority = false;
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

void RequireOperandInvalid(const sblr::SblrOperationEnvelope& envelope,
                           std::string_view scenario) {
  const auto validation = sblr::ValidateSblrEnvelope(envelope);
  if (validation.ok || validation.diagnostics.empty() ||
      validation.diagnostics.front().code != "SBLR.OPERAND_INVALID") {
    std::cerr << scenario << " did not fail as SBLR.OPERAND_INVALID";
    if (!validation.diagnostics.empty()) {
      std::cerr << ": " << validation.diagnostics.front().code << ' '
                << validation.diagnostics.front().message;
    }
    std::cerr << '\n';
    std::exit(EXIT_FAILURE);
  }
}

}  // namespace

int main() {
  for (const auto& spec : kOperations) {
    const auto exact = ExactEnvelope(spec);
    const auto validation = sblr::ValidateSblrEnvelope(exact);
    Require(validation.ok, "exact diagnostic descriptor envelope was refused");

    const auto encoded = sblr::EncodeSblrEnvelope(exact);
    Require(!encoded.empty(), "exact diagnostic descriptor envelope did not encode");
    const auto decoded = sblr::DecodeSblrEnvelope(encoded);
    Require(decoded.ok, "exact diagnostic descriptor envelope did not decode");
    Require(sblr::EncodeSblrEnvelope(decoded.envelope) == encoded,
            "diagnostic descriptor envelope round trip was not byte exact");

    auto malformed = exact;
    malformed.operands.clear();
    RequireOperandInvalid(malformed, "missing operand");

    malformed = exact;
    malformed.operands.front().value_body.assign(16, 0);
    RequireOperandInvalid(malformed, "nil descriptor UUID");

    malformed = exact;
    malformed.operands.front().name.append("_wrong");
    RequireOperandInvalid(malformed, "wrong operand slot");

    malformed = exact;
    malformed.operands.front().value_kind = sblr::SblrValueKind::uuid_ref;
    RequireOperandInvalid(malformed, "wrong operand kind");

    malformed = exact;
    malformed.operands.push_back(malformed.operands.front());
    malformed.operands.back().ordinal = 2;
    RequireOperandInvalid(malformed, "extra operand");

    malformed = exact;
    malformed.result_shape.append("_wrong");
    RequireOperandInvalid(malformed, "wrong result shape");
  }
  return EXIT_SUCCESS;
}
