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
    {0x060Fu, "ddl.create_trigger", "SBLR_DDL_CREATE_TRIGGER"},
    {0x0610u, "ddl.trigger.alter", "SBLR_DDL_ALTER_TRIGGER"},
    {0x0611u, "ddl.trigger.drop", "SBLR_DDL_DROP_TRIGGER"},
    {0x0612u, "ddl.create_procedure", "SBLR_DDL_CREATE_PROCEDURE"},
    {0x0613u, "ddl.procedure.alter", "SBLR_DDL_ALTER_PROCEDURE"},
    {0x0614u, "ddl.procedure.drop", "SBLR_DDL_DROP_PROCEDURE"},
    {0x0615u, "ddl.create_function", "SBLR_DDL_CREATE_FUNCTION"},
    {0x0616u, "ddl.function.alter", "SBLR_DDL_ALTER_FUNCTION"},
    {0x0617u, "ddl.function.drop", "SBLR_DDL_DROP_FUNCTION"},
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
        !Require(entry->support == sblr::SblrOpcodeSupport::local_profile_refusal,
                 "operation must be refused by the local builtin profile") ||
        !Require(entry->refusal_diagnostic == "PROFILE.BUILTIN_PROFILE_UNAVAILABLE",
                 "Core profile-unavailable diagnostic drifted") ||
        !Require(!entry->executor_evidence_required,
                 "refusal must occur before executor-evidence admission")) {
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
    if (!Require(!validation.ok, "pre-dispatch profile refusal was admitted") ||
        !Require(validation.entry == entry, "refusal did not bind registered identity") ||
        !Require(validation.diagnostic_id == "PROFILE.BUILTIN_PROFILE_UNAVAILABLE",
                 "pre-dispatch diagnostic drifted") ||
        !Require(validation.detail ==
                     "operation_is_refused_by_registered_sblr_profile:" +
                         std::string(refusal.operation_id),
                 "refusal detail drifted")) {
      return EXIT_FAILURE;
    }
  }
  return EXIT_SUCCESS;
}
