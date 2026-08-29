// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "engine/sblr/sblr_opcode_registry.hpp"

#include <array>
#include <iostream>
#include <string_view>

namespace sblr = scratchbird::engine::sblr;

int main() {
  const auto* connect = sblr::LookupSblrOperation("bridge.connect");
  const auto* attach = sblr::LookupSblrOperation("bridge.attach");
  const auto* authenticate =
      sblr::LookupSblrOperation("bridge.authenticate");
  if (connect == nullptr || attach == nullptr || authenticate == nullptr ||
      connect->operation_id == attach->operation_id ||
      connect->code != attach->code || connect->opcode != attach->opcode) {
    std::cerr << "shared bridge opcode fixture drifted\n";
    return 1;
  }

  const auto connect_identity = sblr::ValidateSblrOpcodeIdentity(
      connect->code, connect->operation_id, connect->opcode);
  const auto attach_identity = sblr::ValidateSblrOpcodeIdentity(
      attach->code, attach->operation_id, attach->opcode);
  if (!connect_identity.ok || connect_identity.entry != connect ||
      !attach_identity.ok || attach_identity.entry != attach) {
    std::cerr << "exact operation id did not disambiguate shared opcode\n";
    return 1;
  }

  const auto mismatched_mnemonic = sblr::ValidateSblrOpcodeIdentity(
      attach->code, attach->operation_id, authenticate->opcode);
  const auto mismatched_code = sblr::ValidateSblrOpcodeIdentity(
      authenticate->code, connect->operation_id, connect->opcode);
  const auto unregistered_alias = sblr::ValidateSblrOpcodeIdentity(
      connect->code, "bridge.open_channel_alias", connect->opcode);
  for (const auto* refused :
       {&mismatched_mnemonic, &mismatched_code, &unregistered_alias}) {
    if (refused->ok ||
        refused->diagnostic_id !=
            "SBLR.OPERATION.OPCODE_IDENTITY_MISMATCH") {
      std::cerr << "mismatched shared-opcode identity did not fail closed\n";
      return 1;
    }
  }

  struct ExactPhysicalProviderIdentity {
    std::string_view operation_id;
    std::string_view opcode;
    std::uint16_t code;
    std::string_view operand;
    std::string_view result;
  };
  constexpr std::array<ExactPhysicalProviderIdentity, 5> exact_identities{{
      {"nosql.key_value_multiget", "SBLR_NOSQL_KEY_VALUE_MULTIGET", 0x020C,
       "key_value_multiget_descriptor", "key_value_multiget_result"},
      {"nosql.key_value_pipeline", "SBLR_NOSQL_KEY_VALUE_PIPELINE", 0x0313,
       "key_value_pipeline_descriptor", "key_value_pipeline_result"},
      {"nosql.key_value_atomic_program", "SBLR_NOSQL_KEY_VALUE_ATOMIC_PROGRAM",
       0x0314, "key_value_atomic_program_descriptor",
       "key_value_atomic_program_result"},
      {"dml.execute_native_bulk_ingest", "SBLR_DML_EXECUTE_NATIVE_BULK_INGEST",
       0x0315, "native_bulk_ingest_descriptor", "native_bulk_ingest_result"},
      {"nosql.backpressure_debt_plan", "SBLR_NOSQL_BACKPRESSURE_DEBT_PLAN",
       0x0E0B, "nosql_backpressure_debt_plan_descriptor",
       "nosql_backpressure_debt_plan_result"},
  }};
  for (const auto& expected : exact_identities) {
    const auto* entry = sblr::LookupSblrOperation(expected.operation_id);
    if (entry == nullptr || entry->operation_id != expected.operation_id ||
        entry->opcode != expected.opcode || entry->code != expected.code ||
        entry->operand_contract != expected.operand ||
        entry->result_contract != expected.result ||
        entry->executor_id != expected.operation_id ||
        !entry->executor_evidence_required ||
        !entry->executor_evidence_accepted) {
      std::cerr << "exact physical-provider identity drifted: "
                << expected.operation_id << '\n';
      return 1;
    }
    const auto identity = sblr::ValidateSblrOpcodeIdentity(
        expected.code, expected.operation_id, expected.opcode);
    if (!identity.ok || identity.entry != entry) {
      std::cerr << "exact physical-provider identity was not admitted: "
                << expected.operation_id << '\n';
      return 1;
    }
  }

  std::cout << "IA-01 shared opcode identity conformance passed\n";
  return 0;
}
