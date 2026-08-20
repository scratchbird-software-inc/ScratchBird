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

// CREATE TABLE has both the native parser route and its canonical catalog-DDL
// operation; each must be stopped by the same pre-dispatch profile gate.
constexpr std::array<RefusalCase, 10> kRefusalCases{{
    {0x0600u, "ddl.create_schema", "SBLR_DDL_CREATE_SCHEMA"},
    {0x0601u, "ddl.create_table", "SBLR_DDL_CREATE_TABLE"},
    {0x0601u, "ddl.table.create", "SBLR_DDL_CREATE_TABLE"},
    {0x0602u, "ddl.table.alter", "SBLR_DDL_ALTER_TABLE"},
    {0x0603u, "ddl.table.drop", "SBLR_DDL_DROP_TABLE"},
    {0x0606u, "ddl.create_domain", "SBLR_DDL_CREATE_DOMAIN"},
    {0x0607u, "ddl.domain.drop", "SBLR_DDL_DROP_DOMAIN"},
    {0x0608u, "ddl.schema.drop", "SBLR_DDL_DROP_SCHEMA"},
    {0x0609u, "ddl.schema.alter", "SBLR_DDL_ALTER_SCHEMA"},
    {0x060Bu, "ddl.domain.alter", "SBLR_DDL_ALTER_DOMAIN"},
}};

bool Require(bool condition, std::string_view detail) {
  if (condition) return true;
  std::cerr << "CSC-TEST-002561 IA-08B: " << detail << '\n';
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
