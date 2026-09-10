// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "dml/delete_api.hpp"
#include "dml/delete_durable_owner_registry.hpp"

namespace scratchbird::engine::internal_api {
struct EngineDmlDeleteCandidateMutationV1 {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  EngineDeleteRowsResult result;
  wire::TypedUpdateHash effect_set_sha256{};
};
// Only the canonical coordinator calls this under the owning inventory guard
// and an already durable native statement intent. Candidate selection is an
// engine-owned bounded MGA stream, never a caller-supplied row list.
EngineDmlDeleteCandidateMutationV1 ExecuteDmlDeleteCandidateMutationV1(
    const EngineRequestContext&, DmlDeleteExecutionLeaseV1&);
}  // namespace scratchbird::engine::internal_api
