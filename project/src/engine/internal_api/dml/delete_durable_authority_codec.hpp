// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "dml/delete_effect_authority_provider.hpp"
#include "security/security_principal_lifecycle.hpp"
#include "sblr_executor_availability_registry.hpp"

namespace scratchbird::engine::internal_api {
// DDAB is immutable provider material, NOT a capability factory. Only a
// private authenticated engine coordinator may publish or recover authority.
// Pure codec success never confers execution, replay, resources or finality.
struct DmlDeleteDurableAuthorityBundleV1 {
  wire::TypedUpdateUuid database_uuid{}, session_uuid{}, principal_uuid{}, bundle_uuid{};
  wire::TypedUpdateUuid reserved_statement_savepoint_uuid{};
  std::uint64_t bundle_generation = 0;
  wire::TypedUpdateHash owner_context_sha256{}, bundle_evidence_sha256{};
  wire::TypedDeleteDescriptorCarrier descriptor;
  wire::TypedUpdatePredicateVector predicate;
  wire::TypedUpdateDatatypeAuthorityVector datatypes;
  wire::TypedUpdateBuiltinOperatorAuthorityVector operators;
  wire::TypedUpdateTargetOrderCarrier target_order;
  wire::TypedUpdateResourceBudgetCarrier resource_budget;
  wire::TypedUpdateRecoveryTokenCarrier recovery;
  EngineSecurityPolicySnapshotAuthorityV1 security;
  std::vector<std::string> matched_grant_uuids;
  EngineDmlDeleteEffectSnapshotV1 effects;
  SblrExecutorAvailabilitySnapshot executor;
  std::vector<std::uint8_t> exact_bytes;
};
inline constexpr std::size_t kDmlDeleteDurableAuthorityMaximumBytesV1 = 65536;
bool EncodeDmlDeleteDurableAuthorityBundleV1(const DmlDeleteDurableAuthorityBundleV1&,
    std::vector<std::uint8_t>*, EngineApiDiagnostic*);
bool DecodeDmlDeleteDurableAuthorityBundleV1(std::span<const std::uint8_t>,
    DmlDeleteDurableAuthorityBundleV1*, EngineApiDiagnostic*);
// Integrity preimage only: the caller must separately authenticate the exact
// live receipt and prove provider issuance/durable ownership.
bool ComputeDmlDeleteOwnerContextHashV1(const EngineRequestContext&, wire::TypedUpdateHash*);
// Structural owner comparison only; does not authenticate a caller or issue a
// capability. Private coordinators must obtain context from the live receipt.
bool MatchesDmlDeleteDurableAuthorityOwnerV1(const EngineRequestContext&,
    const DmlDeleteDurableAuthorityBundleV1&);
bool ComputeDmlDeleteSecuritySnapshotHashV1(const EngineSecurityPolicySnapshotAuthorityV1&,
    const std::vector<std::string>& matched_grants, wire::TypedUpdateHash*);
}  // namespace scratchbird::engine::internal_api
