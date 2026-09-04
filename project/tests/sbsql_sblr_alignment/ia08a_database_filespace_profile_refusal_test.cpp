// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "engine/sblr/sblr_opcode_registry.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace sblr = scratchbird::engine::sblr;

namespace {

struct RefusalCase {
  std::uint16_t code;
  std::string_view operation_id;
  std::string_view opcode;
  bool admitted;
};

constexpr std::array<RefusalCase, 6> kRefusalCases{{
    {0x0800u, "engine.op.filespace_create", "SBLR_FILESPACE_CREATE", false},
    {0x0808u, "engine.op.filespace_drop", "SBLR_FILESPACE_DROP", false},
    {0x1402u, "engine.op.database_checkpoint", "SBLR_DATABASE_CHECKPOINT", false},
    {0x1404u, "engine.op.database_alter", "SBLR_DATABASE_ALTER", false},
    // CREATE DATABASE has a local executor contract; retain a positive
    // admission assertion so this guard cannot regress it to a refusal.
    {0x1408u, "engine.op.lifecycle_create_database", "SBLR_LIFECYCLE_CREATE_DATABASE", true},
    {0x1416u, "lifecycle.drop_database", "SBLR_LIFECYCLE_DROP_DATABASE", false},
}};

bool Require(bool condition, std::string_view detail) {
  if (condition) return true;
  std::cerr << "CSC-TEST-003025 IA-08A: " << detail << '\n';
  return false;
}

}  // namespace

int main() {
  for (const auto& refusal : kRefusalCases) {
    // Resolve from the authoritative opcode tuple first.  Legacy registry
    // aliases are not valid operation identities, so verify the canonical
    // operation id on the resolved row below.
    const auto* entry = sblr::LookupSblrOpcode(refusal.opcode);
    if (!Require(entry != nullptr, "operation missing from registry") ||
        !Require(entry->operation_id == refusal.operation_id,
                 "canonical operation identity drifted") ||
        !Require(entry->code == refusal.code, "opcode code drifted") ||
        !Require(entry->opcode == refusal.opcode, "opcode identity drifted") ||
        !Require(entry->support == sblr::SblrOpcodeSupport::implemented,
                 "operation must be a specified implementation surface") ||
        !Require(entry->executor_evidence_required,
                 "specified operation must require executor evidence") ||
        !Require(entry->executor_evidence_accepted == refusal.admitted,
                 "executor evidence admission drifted")) {
      return EXIT_FAILURE;
    }

    sblr::SblrOperationEnvelope envelope;
    envelope.opcode_code = refusal.code;
    envelope.operation_id = std::string(refusal.operation_id);
    envelope.opcode = std::string(refusal.opcode);
    envelope.requires_security_context = entry->requires_security_context;
    envelope.requires_transaction_context = entry->requires_transaction_context;
    envelope.requires_cluster_authority = entry->requires_cluster_authority;
    const auto validation = sblr::ValidateSblrOpcodeForEnvelope(envelope);
    if (!Require(validation.ok == refusal.admitted,
                 refusal.admitted ? "implemented operation was refused"
                                  : "missing executor evidence was admitted") ||
        !Require(validation.entry == entry, "validation did not bind registered identity") ||
        !Require(refusal.admitted ||
                     validation.diagnostic_id == "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
                 "executor-evidence diagnostic drifted")) {
      return EXIT_FAILURE;
    }
  }
  return EXIT_SUCCESS;
}
