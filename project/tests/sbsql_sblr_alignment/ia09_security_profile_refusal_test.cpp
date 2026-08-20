// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "engine/sblr/sblr_opcode_registry.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace sblr = scratchbird::engine::sblr;

namespace {

constexpr std::array<std::string_view, 15> kRefusedOpcodes{{
    "SBLR_SEC_CREATE_USER", "SBLR_SEC_ALTER_USER", "SBLR_SEC_CREATE_ROLE",
    "SBLR_SEC_GRANT", "SBLR_SEC_REVOKE", "SBLR_SEC_CREATE_GROUP_MAPPING",
    "SBLR_SEC_ALTER_POLICY", "SBLR_SEC_DROP_USER", "SBLR_SEC_ALTER_ROLE",
    "SBLR_SEC_DROP_ROLE", "SBLR_SEC_CREATE_POLICY", "SBLR_SEC_DROP_POLICY",
    "SBLR_SEC_AUTHENTICATE", "SBLR_SEC_DEAUTHENTICATE",
    "SBLR_SEC_DROP_GROUP_MAPPING",
}};

bool Require(bool condition, std::string_view detail) {
  if (condition) return true;
  std::cerr << "CSC-TEST-003281 IA-09: " << detail << '\n';
  return false;
}

}  // namespace

int main() {
  const auto& registry = sblr::StaticSblrOpcodeRegistry();
  for (const auto opcode : kRefusedOpcodes) {
    std::size_t matches = 0;
    for (const auto& entry : registry) {
      if (entry.opcode != opcode) continue;
      ++matches;
      if (!Require(entry.support == sblr::SblrOpcodeSupport::local_profile_refusal,
                   "operation must be refused by the local builtin profile") ||
          !Require(entry.refusal_diagnostic == "PROFILE.BUILTIN_PROFILE_UNAVAILABLE",
                   "Core profile-unavailable diagnostic drifted") ||
          !Require(!entry.executor_evidence_required,
                   "refusal must occur before executor-evidence admission")) {
        return EXIT_FAILURE;
      }

      sblr::SblrOperationEnvelope envelope;
      envelope.opcode_code = entry.code;
      envelope.operation_id = entry.operation_id;
      envelope.opcode = entry.opcode;
      envelope.requires_security_context = entry.requires_security_context;
      envelope.requires_transaction_context = entry.requires_transaction_context;
      envelope.requires_cluster_authority = entry.requires_cluster_authority;
      const auto validation = sblr::ValidateSblrOpcodeForEnvelope(envelope);
      if (!Require(!validation.ok, "pre-dispatch profile refusal was admitted") ||
          !Require(validation.entry == &entry, "refusal did not bind registered identity") ||
          !Require(validation.diagnostic_id == "PROFILE.BUILTIN_PROFILE_UNAVAILABLE",
                   "pre-dispatch diagnostic drifted") ||
          !Require(validation.detail ==
                       "operation_is_refused_by_registered_sblr_profile:" + entry.operation_id,
                   "refusal detail drifted")) {
        return EXIT_FAILURE;
      }
    }
    if (!Require(matches == 1, "security opcode must bind exactly one registered operation")) {
      return EXIT_FAILURE;
    }
  }
  return EXIT_SUCCESS;
}
