// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "dml/delete_api.hpp"
#include "mga_relation_store/mga_relation_descriptor.hpp"
#include "typed_update_carrier_codec.hpp"

namespace scratchbird::engine::internal_api {
struct EngineDmlDeletePredicateBindingResultV1 {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  MgaRelationStorageDescriptor relation;
  wire::TypedUpdatePredicateVector predicate;
  EnginePredicateEnvelope execution_predicate;
};

// Resolves a TRUE or fixed-width integer equality demand against the live
// relation and datatype/operator registries. DUEV is shared typed expression
// material only: this result is not DELETE privilege, a mutation capability,
// a durable owner bundle, or replay/transaction-finality authority.
EngineDmlDeletePredicateBindingResultV1 BindDmlDeletePredicateV1(
    const EngineRequestContext&, const EngineDmlDeleteRowsBindingDemandV1&,
    const wire::TypedUpdateUuid& descriptor_uuid, std::uint64_t descriptor_generation,
    const wire::TypedUpdateUuid& relation_occurrence_uuid,
    std::uint64_t relation_occurrence_generation);
}  // namespace scratchbird::engine::internal_api
