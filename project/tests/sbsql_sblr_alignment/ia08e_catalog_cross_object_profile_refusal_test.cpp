// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "engine/sblr/sblr_opcode_registry.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace sblr = scratchbird::engine::sblr;

namespace {

constexpr std::array<std::string_view, 56> kRefusedOpcodes{{
    "SBLR_DDL_ALTER_VIEW", "SBLR_DDL_DROP_VIEW",
    "SBLR_DDL_CREATE_PACKAGE", "SBLR_DDL_ALTER_PACKAGE", "SBLR_DDL_DROP_PACKAGE",
    "SBLR_DDL_CREATE_INDEX_TEMPLATE", "SBLR_DDL_CREATE_SEQUENCE", "SBLR_DDL_CREATE_VIEW",
    "SBLR_DDL_ALTER_SEQUENCE", "SBLR_DDL_DROP_SEQUENCE",
    "SBLR_DDL_CREATE_MATERIALIZED_VIEW", "SBLR_DDL_REFRESH_MATERIALIZED_VIEW",
    "SBLR_DDL_DROP_MATERIALIZED_VIEW", "SBLR_DDL_CREATE_TYPE", "SBLR_DDL_ALTER_TYPE",
    "SBLR_DDL_DROP_TYPE", "SBLR_DDL_RENAME_OBJECT", "SBLR_DDL_COMMENT_ON_OBJECT",
    "SBLR_DDL_CREATE_SYNONYM", "SBLR_DDL_DROP_SYNONYM", "SBLR_DDL_CREATE_FOREIGN_TABLE",
    "SBLR_DDL_DROP_FOREIGN_TABLE", "SBLR_DDL_CREATE_FDW", "SBLR_DDL_DROP_FDW",
    "SBLR_DDL_CREATE_RULE", "SBLR_DDL_DROP_RULE", "SBLR_DDL_CREATE_PUBLICATION",
    "SBLR_DDL_ALTER_PUBLICATION", "SBLR_DDL_DROP_PUBLICATION",
    "SBLR_DDL_CREATE_SUBSCRIPTION", "SBLR_DDL_ALTER_SUBSCRIPTION",
    "SBLR_DDL_DROP_SUBSCRIPTION", "SBLR_DDL_CREATE_AGGREGATE",
    "SBLR_DDL_DROP_AGGREGATE", "SBLR_DDL_CREATE_OPERATOR", "SBLR_DDL_DROP_OPERATOR",
    "SBLR_DDL_CREATE_OPERATOR_CLASS", "SBLR_DDL_DROP_OPERATOR_CLASS",
    "SBLR_DDL_CREATE_OPERATOR_FAMILY", "SBLR_DDL_ALTER_OPERATOR_FAMILY",
    "SBLR_DDL_DROP_OPERATOR_FAMILY", "SBLR_DDL_CREATE_CAST", "SBLR_DDL_DROP_CAST",
    "SBLR_DDL_CREATE_COLLATION", "SBLR_DDL_ALTER_COLLATION", "SBLR_DDL_DROP_COLLATION",
    "SBLR_DDL_CREATE_EXTENSION", "SBLR_DDL_ALTER_EXTENSION", "SBLR_DDL_DROP_EXTENSION",
    "SBLR_DDL_CREATE_EVENT_TRIGGER", "SBLR_DDL_ALTER_EVENT_TRIGGER",
    "SBLR_DDL_DROP_EVENT_TRIGGER", "SBLR_DDL_CREATE_DICTIONARY",
    "SBLR_DDL_DROP_DICTIONARY", "SBLR_DDL_CREATE_NAMED_COLLECTION",
    "SBLR_DDL_DROP_NAMED_COLLECTION",
}};

bool Require(bool condition, std::string_view detail) {
  if (condition) return true;
  std::cerr << "CSC-TEST-000000 IA-08E: " << detail << '\n';
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
    if (matches == 0) {
      std::cerr << "CSC-TEST-000000 IA-08E: catalog opcode missing from registry: "
                << opcode << '\n';
      return EXIT_FAILURE;
    }
  }
  return EXIT_SUCCESS;
}
