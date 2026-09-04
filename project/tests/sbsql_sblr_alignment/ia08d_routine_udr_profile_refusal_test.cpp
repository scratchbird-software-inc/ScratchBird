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
};

constexpr std::array<RefusalCase, 9> kRefusalCases{{
    {0x060Fu, "engine.op.ddl_create_trigger", "SBLR_DDL_CREATE_TRIGGER"},
    {0x0610u, "engine.op.ddl_alter_trigger", "SBLR_DDL_ALTER_TRIGGER"},
    {0x0611u, "engine.op.ddl_drop_trigger", "SBLR_DDL_DROP_TRIGGER"},
    {0x0612u, "engine.op.ddl_create_procedure", "SBLR_DDL_CREATE_PROCEDURE"},
    {0x0613u, "engine.op.ddl_alter_procedure", "SBLR_DDL_ALTER_PROCEDURE"},
    {0x0614u, "engine.op.ddl_drop_procedure", "SBLR_DDL_DROP_PROCEDURE"},
    {0x0615u, "engine.op.ddl_create_function", "SBLR_DDL_CREATE_FUNCTION"},
    {0x0616u, "engine.op.ddl_alter_function", "SBLR_DDL_ALTER_FUNCTION"},
    {0x0617u, "engine.op.ddl_drop_function", "SBLR_DDL_DROP_FUNCTION"},
}};

bool Require(bool condition, std::string_view detail) {
  if (condition) return true;
  std::cerr << "CSC-TEST-002621 IA-08D: " << detail << '\n';
  return false;
}

}  // namespace

int main() {
  for (const auto& refusal : kRefusalCases) {
    const auto* entry = sblr::LookupSblrOperation(refusal.operation_id);
    if (!Require(entry != nullptr, "operation missing from registry") ||
        !Require(entry->code == refusal.code, "opcode code drifted") ||
        !Require(entry->opcode == refusal.opcode, "opcode identity drifted") ||
        !Require(entry->support == sblr::SblrOpcodeSupport::local_profile_refusal ||
                     entry->support == sblr::SblrOpcodeSupport::implemented,
                 "operation has unsupported registry disposition")) {
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
    const bool profile_refusal =
        entry->support == sblr::SblrOpcodeSupport::local_profile_refusal &&
        !entry->executor_evidence_required;
    const auto expected = profile_refusal
                              ? "PROFILE.BUILTIN_PROFILE_UNAVAILABLE"
                              : "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";
    if (!Require(!validation.ok, "pre-dispatch refusal was admitted") ||
        !Require(validation.entry == entry, "refusal did not bind registered identity") ||
        !Require(validation.diagnostic_id == expected,
                 "pre-dispatch diagnostic drifted") ||
        !Require(profile_refusal ? validation.detail ==
                                      "operation_is_refused_by_registered_sblr_profile:" +
                                          std::string(refusal.operation_id)
                                : validation.detail.find("executor") != std::string::npos,
                 "refusal detail drifted")) {
      return EXIT_FAILURE;
    }
  }
  return EXIT_SUCCESS;
}
