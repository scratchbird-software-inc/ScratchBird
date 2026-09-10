// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "api_types.hpp"
#include "typed_delete_carrier_codec.hpp"
#include <memory>

namespace scratchbird::engine::internal_api {
struct EngineDmlDeleteDatatypeAuthorityRequestV1 {
  EngineRequestContext context;
  std::vector<std::uint8_t> exact_descriptor_dddc;
  std::vector<std::uint8_t> exact_predicate_duev;
};
struct EngineDmlDeleteDatatypeAuthorityResultV1;

// Read-only datatype/operator capability, not DELETE privilege, row-policy,
// resource, mutation-graph or transaction-finality authority.
class EngineDmlDeleteDatatypeAuthorityHandleV1 final {
 public:
  bool valid() const noexcept { return authority_ != nullptr; }
 private:
  struct Authority;
  std::shared_ptr<const Authority> authority_;
  friend EngineDmlDeleteDatatypeAuthorityResultV1 CaptureDmlDeleteDatatypeAuthorityV1(
      const EngineDmlDeleteDatatypeAuthorityRequestV1&);
  friend EngineApiDiagnostic RevalidateDmlDeleteDatatypeAuthorityV1(
      const EngineRequestContext&, const EngineDmlDeleteDatatypeAuthorityResultV1&);
  friend bool MatchesDmlDeleteDatatypeCaptureV1(const EngineDmlDeleteDatatypeAuthorityResultV1&,
      const wire::TypedDeleteDescriptorCarrier&, const wire::TypedUpdatePredicateVector&);
};
struct EngineDmlDeleteDatatypeAuthorityResultV1 {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  wire::TypedUpdateDatatypeAuthorityVector datatypes;
  wire::TypedUpdateBuiltinOperatorAuthorityVector operators;
  EngineDmlDeleteDatatypeAuthorityHandleV1 handle;
};

EngineDmlDeleteDatatypeAuthorityResultV1 CaptureDmlDeleteDatatypeAuthorityV1(
    const EngineDmlDeleteDatatypeAuthorityRequestV1&);
EngineApiDiagnostic RevalidateDmlDeleteDatatypeAuthorityV1(
    const EngineRequestContext&, const EngineDmlDeleteDatatypeAuthorityResultV1&);
// Exact source capture, not just equality of the referenced datatype rows.
bool MatchesDmlDeleteDatatypeCaptureV1(const EngineDmlDeleteDatatypeAuthorityResultV1&,
    const wire::TypedDeleteDescriptorCarrier&, const wire::TypedUpdatePredicateVector&);
// Re-resolves exact durable projections against live registries, without
// manufacturing a volatile handle or admitting replay of a DELETE operation.
EngineApiDiagnostic RevalidateRecoveredDmlDeleteDatatypeAuthorityV1(
    const EngineRequestContext&, const wire::TypedDeleteDescriptorCarrier&,
    const wire::TypedUpdatePredicateVector&, const wire::TypedUpdateDatatypeAuthorityVector&,
    const wire::TypedUpdateBuiltinOperatorAuthorityVector&);
}  // namespace scratchbird::engine::internal_api
