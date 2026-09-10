// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "api_types.hpp"
#include "typed_delete_carrier_codec.hpp"

namespace scratchbird::engine::internal_api {

enum class EngineDmlDeleteRecoveryDispositionV1 : std::uint8_t {
  refused,
  abandon_unexecuted,
  rollback_statement,
  statement_already_rewound,
  publish_prepared_result,
  published,
  aborted,
  transaction_rolled_back,
};
struct EngineDmlDeleteRecoveryObservationV1 {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  EngineDmlDeleteRecoveryDispositionV1 disposition = EngineDmlDeleteRecoveryDispositionV1::refused;
  scratchbird::wire::TypedDeleteJournalRecord head;
};

// Read-only MGA classification, not mutation or result-return authority.
// Caller still authenticates the durable operation owner and revalidates all
// descriptor/security/resource providers before admitting execution/replay.
// No caller-supplied flags can assert inventory finality or a statement barrier.
EngineDmlDeleteRecoveryObservationV1 ObserveDmlDeleteRecoveryAuthorityV1(
    const EngineRequestContext& context,
    std::span<const std::vector<std::uint8_t>> exact_journal_chain);

}  // namespace scratchbird::engine::internal_api
