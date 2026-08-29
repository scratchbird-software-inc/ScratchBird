// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "security/security_principal_lifecycle.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_DML_UPDATE_IMMUTABLE_AUTHORITY_PROVIDER_V1
// Engine-private typed authority provider for the three immutable sets used by
// dml.update_rows.  This is not an SBOP/SBOS carrier and accepts no SQL text,
// names, opaque lifecycle payload, metadata-epoch alias, or caller-issued
// security-snapshot identity.

inline constexpr const char* kDmlUpdateAuthorityDiagnosticInvalid =
    "SBLR.OPERAND_INVALID";
inline constexpr const char* kDmlUpdateAuthorityDiagnosticStale =
    "MGA.TRANSACTION.STALE";
inline constexpr const char* kDmlUpdateAuthorityDiagnosticUnsupported =
    "SBLR.OPERATION_UNSUPPORTED";
inline constexpr const char* kDmlUpdateAuthorityDiagnosticAccessDenied =
    "SECURITY.ACCESS_DENIED";

using EngineDmlUpdateSha256V1 = std::array<std::uint8_t, 32>;

enum class EngineDmlUpdateRowPolicyPhaseV1 : std::uint8_t {
  using_filter = 1,
  with_check = 2,
};

enum class EngineDmlUpdateConstraintClassV1 : std::uint8_t {
  not_null = 1,
  domain = 2,
  check = 3,
  unique = 4,
  exclusion = 5,
  primary_key = 6,
  foreign_key = 7,
  generated_value = 8,
  policy = 9,
};

enum class EngineDmlUpdateConstraintTimingV1 : std::uint8_t {
  immediate_row = 1,
  immediate_statement = 2,
  deferred_transaction = 3,
  deferred_prepare = 4,
  commit_time = 5,
};

enum class EngineDmlUpdateReservationModeV1 : std::uint8_t {
  no_key_reservation = 1,
  row_reservation = 2,
  statement_reservation = 3,
  transaction_reservation = 4,
};

enum class EngineDmlUpdateTriggerEventV1 : std::uint8_t {
  update = 2,
};

enum class EngineDmlUpdateTriggerTimingV1 : std::uint8_t {
  before_statement = 1,
  before_row = 2,
  after_row = 3,
  after_statement = 4,
};

enum class EngineDmlUpdateTriggerSecurityModeV1 : std::uint8_t {
  invoker = 1,
  definer = 2,
};

struct EngineDmlUpdateRelationOccurrenceAuthorityV1 {
  std::string relation_uuid;
  std::uint64_t relation_generation = 0;
  std::string relation_occurrence_uuid;
  std::uint64_t relation_occurrence_generation = 0;

  bool operator==(
      const EngineDmlUpdateRelationOccurrenceAuthorityV1&) const = default;
};

// One durable policy-catalog source row plus the exact effective projection
// produced by the security policy authority.  Multiple source rows for one
// phase may carry the same effective projection and collapse to one frozen
// row; conflicting projections exact-refuse.
struct EngineDmlUpdateRowPolicyAuthoritySourceV1 {
  bool visible = true;
  bool deleted = false;
  std::string target_relation_uuid;
  std::uint64_t target_relation_generation = 0;
  std::string source_policy_uuid;
  std::uint64_t source_policy_generation = 0;
  std::string source_policy_version_uuid;
  std::uint64_t effective_transaction_number = 0;
  EngineDmlUpdateRowPolicyPhaseV1 phase =
      EngineDmlUpdateRowPolicyPhaseV1::using_filter;
  std::string effective_policy_uuid;
  std::uint64_t effective_policy_generation = 0;
  std::string source_expression_uuid;
  std::uint64_t source_expression_generation = 0;
  EngineDmlUpdateSha256V1 source_expression_evidence_sha256{};
  std::string expression_uuid;
  std::uint64_t expression_generation = 0;
  EngineDmlUpdateSha256V1 expression_evidence_sha256{};
  std::string catalog_snapshot_uuid;
  std::uint64_t catalog_generation = 0;
  std::string security_snapshot_uuid;
  std::uint64_t security_snapshot_generation = 0;
  std::uint64_t security_generation = 0;
  std::uint64_t policy_catalog_generation = 0;
  EngineDmlUpdateSha256V1 source_policy_catalog_vector_sha256{};
};

struct EngineDmlUpdateConstraintAuthoritySourceV1 {
  bool visible = true;
  bool deleted = false;
  std::string catalog_snapshot_uuid;
  std::uint64_t catalog_generation = 0;
  std::string target_relation_uuid;
  std::uint64_t target_relation_generation = 0;
  bool manager_execution_order_present = false;
  std::uint64_t manager_execution_order = 0;
  EngineDmlUpdateConstraintClassV1 constraint_class =
      EngineDmlUpdateConstraintClassV1::not_null;
  EngineDmlUpdateConstraintTimingV1 timing =
      EngineDmlUpdateConstraintTimingV1::immediate_row;
  EngineDmlUpdateReservationModeV1 reservation_mode =
      EngineDmlUpdateReservationModeV1::no_key_reservation;
  std::string constraint_uuid;
  std::uint64_t constraint_generation = 0;
  std::string expression_uuid;
  std::uint64_t expression_generation = 0;
  std::string reservation_profile_uuid;
  std::uint64_t reservation_profile_generation = 0;
  EngineDmlUpdateSha256V1 dependency_set_sha256{};
};

struct EngineDmlUpdateTriggerAuthoritySourceV1 {
  bool visible = true;
  bool deleted = false;
  std::string catalog_snapshot_uuid;
  std::uint64_t catalog_generation = 0;
  std::string target_relation_uuid;
  std::uint64_t target_relation_generation = 0;
  std::uint64_t security_generation = 0;
  bool firing_order_present = false;
  std::uint64_t firing_order = 0;
  EngineDmlUpdateTriggerEventV1 event =
      EngineDmlUpdateTriggerEventV1::update;
  EngineDmlUpdateTriggerTimingV1 timing =
      EngineDmlUpdateTriggerTimingV1::before_statement;
  EngineDmlUpdateTriggerSecurityModeV1 security_mode =
      EngineDmlUpdateTriggerSecurityModeV1::invoker;
  bool autonomous = false;
  bool external_transaction = false;
  std::string trigger_uuid;
  std::uint64_t trigger_generation = 0;
  std::string body_sblr_uuid;
  std::uint64_t body_sblr_generation = 0;
  std::string execution_security_context_uuid;
  std::uint64_t execution_security_generation = 0;
  std::string recursion_profile_uuid;
  std::uint64_t recursion_profile_generation = 0;
  std::uint32_t maximum_depth = 0;
  EngineDmlUpdateSha256V1 dependency_set_sha256{};
};

struct EngineDmlUpdateFrozenRowPolicyRecordV1 {
  std::uint32_t policy_ordinal = 0;
  EngineDmlUpdateRowPolicyPhaseV1 phase =
      EngineDmlUpdateRowPolicyPhaseV1::using_filter;
  std::string effective_policy_uuid;
  std::uint64_t effective_policy_generation = 0;
  std::string expression_uuid;
  std::uint64_t expression_generation = 0;
  EngineDmlUpdateSha256V1 expression_evidence_sha256{};
  std::string security_snapshot_uuid;
  std::uint64_t security_generation = 0;
  EngineDmlUpdateSha256V1 source_policy_catalog_vector_sha256{};
  EngineDmlUpdateSha256V1 record_evidence_sha256{};

  bool operator==(const EngineDmlUpdateFrozenRowPolicyRecordV1&) const =
      default;
};

struct EngineDmlUpdateFrozenConstraintRecordV1 {
  std::uint32_t constraint_ordinal = 0;
  EngineDmlUpdateConstraintClassV1 constraint_class =
      EngineDmlUpdateConstraintClassV1::not_null;
  EngineDmlUpdateConstraintTimingV1 timing =
      EngineDmlUpdateConstraintTimingV1::immediate_row;
  EngineDmlUpdateReservationModeV1 reservation_mode =
      EngineDmlUpdateReservationModeV1::no_key_reservation;
  std::string constraint_uuid;
  std::uint64_t constraint_generation = 0;
  std::string expression_uuid;
  std::uint64_t expression_generation = 0;
  std::string reservation_profile_uuid;
  std::uint64_t reservation_profile_generation = 0;
  EngineDmlUpdateSha256V1 dependency_set_sha256{};
  EngineDmlUpdateSha256V1 record_evidence_sha256{};

  bool operator==(const EngineDmlUpdateFrozenConstraintRecordV1&) const =
      default;
};

struct EngineDmlUpdateFrozenTriggerRecordV1 {
  std::uint32_t trigger_ordinal = 0;
  EngineDmlUpdateTriggerEventV1 event =
      EngineDmlUpdateTriggerEventV1::update;
  EngineDmlUpdateTriggerTimingV1 timing =
      EngineDmlUpdateTriggerTimingV1::before_statement;
  EngineDmlUpdateTriggerSecurityModeV1 security_mode =
      EngineDmlUpdateTriggerSecurityModeV1::invoker;
  std::string trigger_uuid;
  std::uint64_t trigger_generation = 0;
  std::string body_sblr_uuid;
  std::uint64_t body_sblr_generation = 0;
  std::string execution_security_context_uuid;
  std::uint64_t execution_security_generation = 0;
  std::string recursion_profile_uuid;
  std::uint64_t recursion_profile_generation = 0;
  std::uint32_t maximum_depth = 0;
  EngineDmlUpdateSha256V1 dependency_set_sha256{};
  EngineDmlUpdateSha256V1 record_evidence_sha256{};

  bool operator==(const EngineDmlUpdateFrozenTriggerRecordV1&) const =
      default;
};

struct EngineDmlUpdateFrozenRowPolicySetV1 {
  std::string set_uuid;
  std::uint64_t set_generation = 0;
  EngineDmlUpdateSha256V1 vector_sha256{};
  std::vector<EngineDmlUpdateFrozenRowPolicyRecordV1> records;

  bool operator==(const EngineDmlUpdateFrozenRowPolicySetV1&) const = default;
};

struct EngineDmlUpdateFrozenConstraintSetV1 {
  std::string set_uuid;
  std::uint64_t set_generation = 0;
  EngineDmlUpdateSha256V1 vector_sha256{};
  std::vector<EngineDmlUpdateFrozenConstraintRecordV1> records;

  bool operator==(const EngineDmlUpdateFrozenConstraintSetV1&) const = default;
};

struct EngineDmlUpdateFrozenTriggerSetV1 {
  std::string set_uuid;
  std::uint64_t set_generation = 0;
  EngineDmlUpdateSha256V1 vector_sha256{};
  std::vector<EngineDmlUpdateFrozenTriggerRecordV1> records;

  bool operator==(const EngineDmlUpdateFrozenTriggerSetV1&) const = default;
};

struct EngineDmlUpdateImmutableAuthoritySnapshotV1 {
  std::string authenticated_statement_receipt_uuid;
  std::uint64_t structural_occurrence_id = 0;
  EngineDmlUpdateRelationOccurrenceAuthorityV1 relation_occurrence;
  std::string catalog_snapshot_uuid;
  std::uint64_t catalog_generation = 0;
  EngineSecurityPolicySnapshotAuthorityV1 security_policy_snapshot;
  EngineDmlUpdateFrozenRowPolicySetV1 row_policy_set;
  EngineDmlUpdateFrozenConstraintSetV1 constraint_set;
  EngineDmlUpdateFrozenTriggerSetV1 trigger_set;

  bool operator==(const EngineDmlUpdateImmutableAuthoritySnapshotV1&) const =
      default;
};

struct EngineDmlUpdateImmutableAuthorityFreezeRequestV1 {
  EngineRequestContext context;
  std::string authenticated_statement_receipt_uuid;
  std::uint64_t structural_occurrence_id = 0;
  EngineDmlUpdateRelationOccurrenceAuthorityV1 relation_occurrence;
  std::string catalog_snapshot_uuid;
  std::uint64_t catalog_generation = 0;
  // Optional engine-issued lifecycle token.  When present it is revalidated
  // against the durable security catalog and reused exactly; raw callers
  // cannot forge a snapshot by filling row source fields.
  std::optional<EngineSecurityPolicySnapshotAuthorityV1>
      security_policy_snapshot_authority;
  bool autonomous_transaction = false;
  bool external_transaction = false;
  std::vector<EngineDmlUpdateRowPolicyAuthoritySourceV1> row_policies;
  std::vector<EngineDmlUpdateConstraintAuthoritySourceV1> constraints;
  std::vector<EngineDmlUpdateTriggerAuthoritySourceV1> triggers;
};

struct EngineDmlUpdateImmutableAuthorityFreezeResultV1 {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  EngineDmlUpdateImmutableAuthoritySnapshotV1 snapshot;
};

struct EngineDmlUpdateImmutableAuthorityRevalidateRequestV1 {
  EngineDmlUpdateImmutableAuthorityFreezeRequestV1 current;
  EngineDmlUpdateImmutableAuthoritySnapshotV1 admitted;
};

struct EngineDmlUpdateImmutableAuthorityRevalidateResultV1 {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
};

EngineDmlUpdateImmutableAuthorityFreezeResultV1
FreezeDmlUpdateImmutableAuthorityV1(
    const EngineDmlUpdateImmutableAuthorityFreezeRequestV1& request);
EngineDmlUpdateImmutableAuthorityRevalidateResultV1
RevalidateDmlUpdateImmutableAuthorityV1(
    const EngineDmlUpdateImmutableAuthorityRevalidateRequestV1& request);

void ResetDmlUpdateImmutableAuthorityProviderForTestV1();

}  // namespace scratchbird::engine::internal_api
