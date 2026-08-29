// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "engine/internal_api/api_types.hpp"
#include "engine/internal_api/catalog/name_resolution_api.hpp"
#include "engine/sblr/contextual_text_literal_v2_codec.hpp"
#include "core/datatypes/datatype_operations.hpp"
#include "engine/sblr/sblr_literal_runtime.hpp"
#include "engine/internal_api/sblr_executor_availability_registry.hpp"

#include <array>
#include <cstdint>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_CONTEXTUAL_TEXT_LITERAL_AUTHORITY_V2
// Receipt-owned, process-local authority.  Resolver interfaces below are
// engine-only seams: a parser/server caller can provide exact carrier bytes,
// but can never implement or substitute a resolver.

struct EngineContextualTextLiteralBudgetV2 {
  std::uint64_t literal_negotiation_byte_grant = 0;
  std::uint64_t canonical_body_aggregate_grant = 0;
};

struct EngineResolvedContextualTextTargetV2 {
  std::uint64_t literal_occurrence = 0;
  std::vector<std::uint8_t> exact_public_relation_projection_v3;
  std::vector<std::uint8_t> exact_sbtltd02;
};

struct EngineContextualTextComparisonResourceSnapshotV2;

inline constexpr std::size_t kContextualTextComposedMaximumSbxnBytesV2 =
    642875;
inline constexpr std::size_t kContextualTextComposedMaximumSblfBytesV2 =
    1134555;

struct EngineContextualTextComposedTransferRecordV2 {
  scratchbird::engine::sblr::ContextualTextUuidV2 final_receipt_uuid{};
  scratchbird::engine::sblr::ContextualTextUuidV2 admission_token_uuid{};
  scratchbird::engine::sblr::ContextualTextUuidV2 preliminary_receipt_uuid{};
  scratchbird::engine::sblr::ContextualTextUuidV2 profile_set_uuid{};
  std::uint64_t profile_set_generation = 0;
  // Exact ordinary SBLA/SBEL evidence retained beside (but not inserted into)
  // the Core composed-transfer hash material where the Core domain does not
  // name it explicitly.
  scratchbird::engine::sblr::ContextualTextSha256V2
      admission_token_binding_sha256{};
  scratchbird::engine::sblr::ContextualTextSha256V2 bound_ast_sha256{};
  scratchbird::engine::sblr::ContextualTextSha256V2 complete_sbxn_sha256{};
  std::vector<std::uint8_t> exact_evidence_material;
  scratchbird::engine::sblr::ContextualTextSha256V2 evidence_sha256{};

  bool operator==(
      const EngineContextualTextComposedTransferRecordV2&) const = default;
};

class EngineContextualTextTargetAuthorityResolverV2 {
 public:
  virtual ~EngineContextualTextTargetAuthorityResolverV2() = default;

  // Authenticates the private budget policy/namespace rows.  It returns only
  // ceilings; the authority provider issues the receipt-local grant UUID.
  virtual bool BindBudget(
      const EngineRequestContext& context,
      const scratchbird::engine::sblr::
          ContextualTextLiteralNegotiationRequestV2& request,
      std::size_t exact_request_bytes,
      EngineContextualTextLiteralBudgetV2* budget,
      EngineApiDiagnostic* diagnostic) const = 0;

  // Must select one visible, sealed MGA sidecar by exact receipt/source/
  // relation/column identity and resolve the complete live contextual-policy
  // row set.  Success is engine authority; input demand fields remain claims.
  virtual bool ResolveTarget(
      const EngineRequestContext& context,
      const scratchbird::engine::sblr::ContextualTextLiteralDemandV2& demand,
      EngineResolvedContextualTextTargetV2* target,
      EngineApiDiagnostic* diagnostic) const = 0;

  virtual bool RevalidateTarget(
      const EngineRequestContext& context,
      const scratchbird::engine::sblr::ContextualTextLiteralDemandV2& demand,
      const EngineResolvedContextualTextTargetV2& target,
      EngineApiDiagnostic* diagnostic) const = 0;

  // Optional engine-only zero-wire seam for resolvers that already retain a
  // complete immutable comparison-resource snapshot. The authority provider
  // independently validates it and otherwise resolves the same snapshot from
  // the live engine resource registry during Prepare/JointConsume.
  virtual bool CopyComparisonResourceSnapshot(
      const EngineRequestContext&,
      const scratchbird::engine::sblr::ContextualTextLiteralDemandV2&,
      const EngineResolvedContextualTextTargetV2&,
      const scratchbird::engine::sblr::ContextualTextLiteralProfileV2&,
      EngineContextualTextComparisonResourceSnapshotV2*) const {
    return false;
  }
};

// Ordered, engine-verified identities for one contextual literal occurrence.
// These are produced only by the canonical graph verifier and retained beside
// the staged/consumed value so execution can resolve the exact literal
// expression without decoding or inferring graph structure again.
struct EngineContextualTextVerifiedGraphBindingV2 {
  std::uint64_t literal_occurrence = 0;
  std::uint64_t node_id = 0;
  std::uint32_t literal_expression_id = 0;
  std::uint32_t comparison_expression_id = 0;
  std::uint32_t target_expression_id = 0;
  std::uint32_t source_node_id = 0;
  std::uint32_t literal_descriptor_handle = 0;
  std::uint32_t target_descriptor_handle = 0;
  std::array<std::string, 17> exact_relational_descriptor_v2_fields{};
  std::string canonical_type_name;
  bool element_profile_empty = false;
  std::array<std::string, 17>
      exact_target_relational_descriptor_v2_fields{};
  std::string target_canonical_type_name;
  bool target_element_profile_empty = false;

  bool operator==(
      const EngineContextualTextVerifiedGraphBindingV2&) const = default;
};

class EngineContextualTextGraphAuthorityVerifierV2 {
 public:
  virtual ~EngineContextualTextGraphAuthorityVerifierV2() = default;
  // On success, publishes exactly one binding per issued demand/execute
  // mapping in their canonical order. Failure publishes no bindings.
  virtual bool VerifyPrepareEvidence(
      const EngineRequestContext& context,
      const scratchbird::engine::sblr::
          ContextualTextLiteralNegotiationRequestV2& issued_request,
      const scratchbird::engine::sblr::ContextualTextLiteralExecuteV2& execute,
      const std::vector<std::uint8_t>& exact_sbel_v1,
      const std::vector<std::uint8_t>& exact_canonical_sbos,
      const EngineContextualTextComposedTransferRecordV2& composed_transfer,
      const std::vector<std::uint8_t>&
          exact_pre_contextual_operand_records,
      std::uint32_t pre_contextual_operand_count,
      const std::vector<std::uint8_t>& exact_sbxn,
      std::vector<EngineContextualTextVerifiedGraphBindingV2>*
          verified_bindings,
      EngineApiDiagnostic* diagnostic) const = 0;
};

struct EngineContextualTextLiteralAuthorityIssueRequestV2 {
  EngineRequestContext context;
  std::vector<std::uint8_t> exact_sbtlnr02;
  const EngineContextualTextTargetAuthorityResolverV2* target_resolver =
      nullptr;
};

struct EngineContextualTextLiteralAuthorityIssueResultV2;
struct EngineContextualTextLiteralTransferRequestV2;
struct EngineContextualTextLiteralTransferResultV2;
struct EngineContextualTextLiteralAuthorityPrepareRequestV2;
struct EngineContextualTextLiteralAuthorityPrepareResultV2;
struct EngineContextualTextLiteralJointConsumeRequestV2;
struct EngineContextualTextLiteralJointConsumeResultV2;
struct EngineContextualTextLiteralAuthoritySnapshotV2;
struct EngineContextualTextPreparedResourceEstimateV2;
struct EngineContextualTextLeaseResourceEstimateV2;

class EngineContextualTextLiteralAuthorityHandleV2 final {
 public:
  struct Authority;

  EngineContextualTextLiteralAuthorityHandleV2() = default;
  EngineContextualTextLiteralAuthorityHandleV2(
      EngineContextualTextLiteralAuthorityHandleV2&&) noexcept = default;
  EngineContextualTextLiteralAuthorityHandleV2& operator=(
      EngineContextualTextLiteralAuthorityHandleV2&&) noexcept = default;
  EngineContextualTextLiteralAuthorityHandleV2(
      const EngineContextualTextLiteralAuthorityHandleV2&) = delete;
  EngineContextualTextLiteralAuthorityHandleV2& operator=(
      const EngineContextualTextLiteralAuthorityHandleV2&) = delete;

  [[nodiscard]] bool valid() const noexcept { return authority_ != nullptr; }

 private:
  std::shared_ptr<Authority> authority_;

  friend struct EngineContextualTextLiteralAuthorityIssueResultV2;
  friend struct EngineContextualTextLiteralAuthorityPrepareResultV2;
  friend struct EngineContextualTextLiteralJointConsumeResultV2;
  friend bool CopyContextualTextLiteralAuthoritySnapshotV2(
      const EngineContextualTextLiteralAuthorityHandleV2&,
      EngineContextualTextLiteralAuthoritySnapshotV2*, EngineApiDiagnostic*);
  friend EngineApiDiagnostic RevokeContextualTextLiteralAuthorityV2(
      EngineContextualTextLiteralAuthorityHandleV2*, std::string_view);
  friend EngineContextualTextLiteralAuthorityIssueResultV2
  IssueContextualTextLiteralAuthorityV2(
      const EngineContextualTextLiteralAuthorityIssueRequestV2&);
  friend EngineContextualTextLiteralTransferResultV2
  TransferContextualTextLiteralAuthorityV2(
      const EngineContextualTextLiteralTransferRequestV2&);
  friend bool ValidateContextualTextComposedSbxnPartitionV2(
      const EngineRequestContext&,
      const EngineContextualTextLiteralAuthorityHandleV2&,
      const std::vector<std::uint8_t>&,
      const std::vector<std::uint64_t>&,
      EngineApiDiagnostic*);
  friend EngineContextualTextLiteralAuthorityPrepareResultV2
  PrepareContextualTextLiteralAuthorityV2(
      const EngineContextualTextLiteralAuthorityPrepareRequestV2&);
  friend EngineContextualTextLiteralJointConsumeResultV2
  JointConsumeContextualTextLiteralAuthorityV2(
      const EngineContextualTextLiteralJointConsumeRequestV2&);
};

struct EngineContextualTextLiteralAuthorityIssueResultV2 {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  scratchbird::engine::sblr::ContextualTextLiteralProfileSetV2 profile_set;
  std::vector<std::uint8_t> exact_sbtlns02;
  EngineContextualTextLiteralAuthorityHandleV2 authority;
};

EngineContextualTextLiteralAuthorityIssueResultV2
IssueContextualTextLiteralAuthorityV2(
    const EngineContextualTextLiteralAuthorityIssueRequestV2& request);

struct EngineContextualTextLiteralTransferRequestV2 {
  EngineRequestContext context;
  EngineContextualTextLiteralAuthorityHandleV2* authority = nullptr;
  scratchbird::engine::sblr::ContextualTextUuidV2 final_receipt_uuid{};
  scratchbird::engine::sblr::ContextualTextUuidV2 admission_token_uuid{};
  scratchbird::engine::sblr::ContextualTextSha256V2
      admission_token_binding_sha256{};
  scratchbird::engine::sblr::ContextualTextSha256V2 v1_demand_sha256{};
  scratchbird::engine::sblr::ContextualTextSha256V2
      v1_ordered_profile_sha256{};
  scratchbird::engine::sblr::ContextualTextSha256V2 bound_ast_sha256{};
  scratchbird::engine::sblr::ContextualTextSha256V2 complete_sbxn_sha256{};
};

struct EngineContextualTextLiteralTransferResultV2 {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  EngineContextualTextComposedTransferRecordV2 record;
};

// Additive query.execute-1.1 decoders. They retain every SBXN/SBLF V1 byte and
// field rule while applying only the composed contextual byte ceilings. The
// ordinary V1 decoders and their smaller limits remain unchanged.
scratchbird::engine::sblr::SblrExpressionNodeTableCodecResultV1
DecodeContextualTextComposedSbxnV2(const std::uint8_t* bytes,
                                   std::size_t size);
bool DecodeContextualTextComposedLiteralFinalizeRequestV2(
    const std::uint8_t* bytes, std::size_t size,
    scratchbird::engine::sblr::SblrLiteralFinalizeRequestV1* out);

// Called only by ordinary SBLA finalization while holding the receipt mutex.
// It validates the complete numeric/contextual SBXN partition and atomically
// retains the preliminary-to-final transfer record in the contextual set.
EngineContextualTextLiteralTransferResultV2
TransferContextualTextLiteralAuthorityV2(
    const EngineContextualTextLiteralTransferRequestV2& request);

bool ValidateContextualTextComposedSbxnPartitionV2(
    const EngineRequestContext& context,
    const EngineContextualTextLiteralAuthorityHandleV2& authority,
    const std::vector<std::uint8_t>& exact_sbxn,
    const std::vector<std::uint64_t>& numeric_node_ids,
    EngineApiDiagnostic* diagnostic);

struct PreparedContextualTextValueV2 {
  std::uint64_t literal_occurrence = 0;
  std::uint64_t node_id = 0;
  std::uint32_t literal_descriptor_handle = 0;
  scratchbird::engine::sblr::ContextualTextUuidV2 descriptor_uuid{};
  std::uint64_t descriptor_generation = 0;
  scratchbird::engine::sblr::ContextualTextUuidV2 type_uuid{};
  std::uint64_t type_generation = 0;
  scratchbird::engine::sblr::ContextualTextUuidV2 codec_uuid{};
  std::string codec_id;
  std::uint16_t codec_version = 0;
  std::uint64_t codec_generation = 0;
  scratchbird::engine::sblr::ContextualTextSha256V2 canonical_body_sha256{};
  std::vector<std::uint8_t> canonical_body;
  std::uint32_t scalar_count = 0;
  scratchbird::engine::sblr::ContextualTextUuidV2 profile_set_uuid{};
  std::uint64_t profile_set_generation = 0;
  scratchbird::engine::sblr::ContextualTextUuidV2 literal_binding_uuid{};
  std::uint64_t literal_binding_generation = 0;
  scratchbird::engine::sblr::ContextualTextSha256V2 target_context_sha256{};
};

struct ContextualTextTypedValueV2 : PreparedContextualTextValueV2 {
  scratchbird::engine::sblr::ContextualTextUuidV2 consumed_profile_uuid{};
};

// Complete comparison-ready resource authority staged before the joint
// consume barrier. Execution borrows this immutable value and never performs
// a fresh resource lookup or reconstructs a collation seed after consume.
struct EngineContextualTextComparisonResourceSnapshotV2 {
  scratchbird::engine::sblr::ContextualTextUuidV2 charset_uuid{};
  std::uint64_t charset_generation = 0;
  std::string charset_name;
  std::string charset_uuid_canonical;
  std::uint64_t charset_resource_epoch = 0;
  std::uint64_t charset_family_epoch = 0;
  scratchbird::engine::sblr::ContextualTextUuidV2 collation_uuid{};
  std::uint64_t collation_generation = 0;
  std::string collation_name;
  std::string collation_uuid_canonical;
  std::uint64_t collation_resource_epoch = 0;
  std::uint64_t collation_family_epoch = 0;
  scratchbird::core::datatypes::DatatypeTextSeedAuthority text_seed;
  EngineResolvedResourceDescriptor charset_resource;
  EngineResolvedResourceDescriptor collation_resource;
  scratchbird::engine::sblr::ContextualTextSha256V2
      target_projection_sha256{};
  scratchbird::engine::sblr::ContextualTextSha256V2
      descriptor_evidence_sha256{};
  std::vector<std::uint8_t> exact_public_relation_projection_v3;
  std::vector<std::uint8_t> exact_sbtltd02;
};

// Execution-map materialization is fully allocated and canonicalized during
// state-neutral Prepare. JointConsume only noexcept-moves it into the lease.
struct EngineContextualTextPreparedRuntimeMaterializationV2 {
  EngineContextualTextPreparedRuntimeMaterializationV2() = default;
  EngineContextualTextPreparedRuntimeMaterializationV2(
      EngineContextualTextPreparedRuntimeMaterializationV2&&) noexcept =
      default;
  EngineContextualTextPreparedRuntimeMaterializationV2& operator=(
      EngineContextualTextPreparedRuntimeMaterializationV2&&) noexcept =
      default;
  EngineContextualTextPreparedRuntimeMaterializationV2(
      const EngineContextualTextPreparedRuntimeMaterializationV2&) = delete;
  EngineContextualTextPreparedRuntimeMaterializationV2& operator=(
      const EngineContextualTextPreparedRuntimeMaterializationV2&) = delete;

  EngineContextualTextVerifiedGraphBindingV2 graph_binding;
  EngineTypedValue value;
  EngineDescriptor target_descriptor;
  std::vector<std::uint8_t> exact_literal_relational_descriptor_v2_bytes;
  std::vector<std::uint8_t> exact_target_relational_descriptor_v2_bytes;
  scratchbird::engine::sblr::ContextualTextLiteralProfileV2 exact_profile;
  EngineContextualTextComparisonResourceSnapshotV2 comparison_resources;
};

// Immutable execution entry exposed by the state-neutral prepared-set borrow
// seam. Every allocation, canonicalization, graph binding, descriptor, and
// resource lookup represented here completed before JointConsume. The entry
// is move-only and its borrowed lifetime ends when its owning Prepared set is
// moved, consumed, or destroyed.
struct EngineContextualTextPreparedExecutionEntryV2 {
  EngineContextualTextPreparedExecutionEntryV2() = default;
  EngineContextualTextPreparedExecutionEntryV2(
      EngineContextualTextPreparedExecutionEntryV2&&) noexcept = default;
  EngineContextualTextPreparedExecutionEntryV2& operator=(
      EngineContextualTextPreparedExecutionEntryV2&&) noexcept = default;
  EngineContextualTextPreparedExecutionEntryV2(
      const EngineContextualTextPreparedExecutionEntryV2&) = delete;
  EngineContextualTextPreparedExecutionEntryV2& operator=(
      const EngineContextualTextPreparedExecutionEntryV2&) = delete;

  PreparedContextualTextValueV2 prepared_value;
  EngineContextualTextPreparedRuntimeMaterializationV2 runtime_materialization;
  std::uint64_t comparison_occurrence = 0;
  std::uint32_t target_descriptor_handle = 0;
  std::uint8_t literal_argument_ordinal = 0;
  std::uint8_t target_argument_ordinal = 0;
  scratchbird::engine::sblr::ContextualTextUuidV2 equality_operation_uuid{};
  std::uint64_t equality_operation_generation = 0;
};

enum class EngineContextualTextResourceEstimateStatusV2 : std::uint8_t {
  ok = 0,
  invalid_owner = 1,
  arithmetic_overflow = 2,
};

// Provider-owned logical-byte accounting. Static object storage contributes
// sizeof(T), retained containers contribute their current capacity, and
// snapshots copied during JointConsume contribute their exact source payload
// size. Allocator metadata and scratch owned inside the executor-availability
// loader, target/MGA resolver, resource-catalog loader, or cancellation
// callback are intentionally outside this provider-owned estimate.
struct EngineContextualTextPreparedResourceEstimateV2 {
  bool ok = false;
  EngineContextualTextResourceEstimateStatusV2 status =
      EngineContextualTextResourceEstimateStatusV2::invalid_owner;
  // Current provider-owned Prepared State, retained container capacities, and
  // all nested staged payload capacities. Shared Authority storage is not
  // double-counted.
  std::uint64_t prepared_retained_bytes = 0;
  // Additional provider-owned logical peak while Prepared remains live:
  // lease reservation, exact authority snapshots, final resource-validation
  // temporaries, and success publication. This excludes prepared_retained_bytes.
  std::uint64_t joint_incremental_peak_bytes = 0;
  // Provider-owned logical bytes left in the execution lease after the staged
  // payload is moved and Prepared is invalidated.
  std::uint64_t post_consume_lease_retained_bytes = 0;
};

struct EngineContextualTextLeaseResourceEstimateV2 {
  bool ok = false;
  EngineContextualTextResourceEstimateStatusV2 status =
      EngineContextualTextResourceEstimateStatusV2::invalid_owner;
  std::uint64_t post_consume_lease_retained_bytes = 0;
};

// Execution-map metadata remains separate from the exact Core typed-value
// fields.  Dispatch resolves by occurrence/node/literal handle first and never
// by the repeated d718 descriptor UUID.
struct ContextualTextExecutionAuthorityEntryV2 {
  ContextualTextExecutionAuthorityEntryV2() = default;
  ContextualTextExecutionAuthorityEntryV2(
      ContextualTextExecutionAuthorityEntryV2&&) noexcept = default;
  ContextualTextExecutionAuthorityEntryV2& operator=(
      ContextualTextExecutionAuthorityEntryV2&&) noexcept = default;
  ContextualTextExecutionAuthorityEntryV2(
      const ContextualTextExecutionAuthorityEntryV2&) = delete;
  ContextualTextExecutionAuthorityEntryV2& operator=(
      const ContextualTextExecutionAuthorityEntryV2&) = delete;

  ContextualTextTypedValueV2 typed_value;
  EngineContextualTextPreparedRuntimeMaterializationV2 runtime_materialization;
  std::uint64_t comparison_occurrence = 0;
  std::uint32_t target_descriptor_handle = 0;
  std::uint8_t literal_argument_ordinal = 0;
  std::uint8_t target_argument_ordinal = 0;
  scratchbird::engine::sblr::ContextualTextUuidV2 equality_operation_uuid{};
  std::uint64_t equality_operation_generation = 0;
};

class PreparedContextualTextLiteralSetV2 final {
 public:
  struct State;
  PreparedContextualTextLiteralSetV2();
  ~PreparedContextualTextLiteralSetV2();
  PreparedContextualTextLiteralSetV2(
      PreparedContextualTextLiteralSetV2&&) noexcept;
  PreparedContextualTextLiteralSetV2& operator=(
      PreparedContextualTextLiteralSetV2&&) noexcept;
  PreparedContextualTextLiteralSetV2(
      const PreparedContextualTextLiteralSetV2&) = delete;
  PreparedContextualTextLiteralSetV2& operator=(
      const PreparedContextualTextLiteralSetV2&) = delete;
  [[nodiscard]] bool valid() const noexcept { return state_ != nullptr; }

 private:
  std::unique_ptr<State> state_;
  friend struct EngineContextualTextLiteralAuthorityPrepareResultV2;
  friend struct EngineContextualTextLiteralJointConsumeResultV2;
  friend EngineContextualTextLiteralAuthorityPrepareResultV2
  PrepareContextualTextLiteralAuthorityV2(
      const EngineContextualTextLiteralAuthorityPrepareRequestV2&);
  friend EngineContextualTextLiteralJointConsumeResultV2
  JointConsumeContextualTextLiteralAuthorityV2(
      const EngineContextualTextLiteralJointConsumeRequestV2&);
  friend std::span<const EngineContextualTextPreparedExecutionEntryV2>
  ViewPreparedContextualTextLiteralSetV2(
      const PreparedContextualTextLiteralSetV2&) noexcept;
  friend const EngineContextualTextPreparedExecutionEntryV2*
  FindPreparedContextualTextExecutionEntryV2(
      const PreparedContextualTextLiteralSetV2&,
      std::uint64_t, std::uint64_t, std::uint32_t, std::uint32_t,
      std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t,
      std::uint8_t, std::uint8_t,
      const scratchbird::engine::sblr::ContextualTextUuidV2&,
      std::uint64_t) noexcept;
  friend EngineContextualTextPreparedResourceEstimateV2
  EstimatePreparedContextualTextLiteralResourcesV2(
      const PreparedContextualTextLiteralSetV2&) noexcept;
};

class ContextualTextExecutionAuthorityLeaseV2 final {
 public:
  struct State;
  ContextualTextExecutionAuthorityLeaseV2();
  ~ContextualTextExecutionAuthorityLeaseV2();
  ContextualTextExecutionAuthorityLeaseV2(
      ContextualTextExecutionAuthorityLeaseV2&&) noexcept;
  ContextualTextExecutionAuthorityLeaseV2& operator=(
      ContextualTextExecutionAuthorityLeaseV2&&) noexcept;
  ContextualTextExecutionAuthorityLeaseV2(
      const ContextualTextExecutionAuthorityLeaseV2&) = delete;
  ContextualTextExecutionAuthorityLeaseV2& operator=(
      const ContextualTextExecutionAuthorityLeaseV2&) = delete;
  [[nodiscard]] bool valid() const noexcept { return state_ != nullptr; }

 private:
  std::unique_ptr<State> state_;
  friend struct EngineContextualTextLiteralJointConsumeResultV2;
  friend EngineContextualTextLiteralJointConsumeResultV2
  JointConsumeContextualTextLiteralAuthorityV2(
      const EngineContextualTextLiteralJointConsumeRequestV2&);
  friend std::span<const ContextualTextExecutionAuthorityEntryV2>
  ViewContextualTextExecutionAuthorityLeaseV2(
      const ContextualTextExecutionAuthorityLeaseV2&) noexcept;
  friend const ContextualTextExecutionAuthorityEntryV2*
  FindContextualTextExecutionAuthorityEntryV2(
      const ContextualTextExecutionAuthorityLeaseV2&,
      std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t,
      std::uint32_t, std::uint32_t,
      const scratchbird::engine::sblr::ContextualTextUuidV2&,
      std::uint64_t) noexcept;
  friend EngineContextualTextLeaseResourceEstimateV2
  EstimateContextualTextExecutionAuthorityLeaseRetainedBytesV2(
      const ContextualTextExecutionAuthorityLeaseV2&) noexcept;
};

struct EngineContextualTextLiteralAuthorityPrepareRequestV2 {
  EngineRequestContext context;
  EngineContextualTextLiteralAuthorityHandleV2* authority = nullptr;
  const EngineContextualTextTargetAuthorityResolverV2* target_resolver =
      nullptr;
  const EngineContextualTextGraphAuthorityVerifierV2* graph_verifier = nullptr;
  std::vector<std::uint8_t> exact_sbel_v1;
  std::vector<std::uint8_t> exact_canonical_sbos;
  const EngineContextualTextComposedTransferRecordV2* composed_transfer =
      nullptr;
  std::vector<std::uint8_t> exact_sbtlxe02;
  std::vector<std::uint8_t> exact_pre_contextual_operand_records;
  std::uint32_t pre_contextual_operand_count = 0;
  std::vector<std::uint8_t> exact_sbxn;
};

struct EngineContextualTextLiteralAuthorityPrepareResultV2 {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  PreparedContextualTextLiteralSetV2 prepared;
};

EngineContextualTextLiteralAuthorityPrepareResultV2
PrepareContextualTextLiteralAuthorityV2(
    const EngineContextualTextLiteralAuthorityPrepareRequestV2& request);

struct EngineContextualTextLiteralJointConsumeRequestV2 {
  // Borrowed from the receipt-locked dispatch attempt. JointConsume neither
  // copies nor retains these values; both must remain live for the call.
  const EngineRequestContext* context = nullptr;
  EngineContextualTextLiteralAuthorityHandleV2* authority = nullptr;
  PreparedContextualTextLiteralSetV2* prepared = nullptr;
  const std::vector<std::uint8_t>* exact_sbel_v1 = nullptr;
  const EngineContextualTextComposedTransferRecordV2* composed_transfer =
      nullptr;
  // Private bridge-owned member of the computed joint prestate. The caller
  // holds its receipt mutex for the entire call. This pointer is never sourced
  // from a parser or serialized carrier.
  bool* receipt_literal_admission_consumed = nullptr;
};

struct EngineContextualTextLiteralJointConsumeResultV2 {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  ContextualTextExecutionAuthorityLeaseV2 lease;
};

// The receipt owner invokes this while holding its one process-local receipt
// mutex.  This function changes the private ordinary SBEL-token consumed bit
// and the provider set state together under the provider mutex.  Success is
// the sole non-failing issued->consumed-with-lease transition; refusal changes
// neither member of the computed pair.
EngineContextualTextLiteralJointConsumeResultV2
JointConsumeContextualTextLiteralAuthorityV2(
    const EngineContextualTextLiteralJointConsumeRequestV2& request);

enum class EngineContextualTextLiteralAuthorityStateV2 : std::uint8_t {
  issued = 1,
  consumed_with_lease = 2,
  revoked = 3,
};

struct EngineContextualTextLiteralAuthoritySnapshotV2 {
  EngineContextualTextLiteralAuthorityStateV2 state =
      EngineContextualTextLiteralAuthorityStateV2::revoked;
  EngineRequestContext pinned_context;
  scratchbird::engine::sblr::ContextualTextLiteralNegotiationRequestV2 request;
  scratchbird::engine::sblr::ContextualTextLiteralProfileSetV2 profile_set;
  std::vector<std::uint8_t> exact_sbtlns02;
  std::optional<EngineContextualTextComposedTransferRecordV2>
      composed_transfer;
};

bool CopyContextualTextLiteralAuthoritySnapshotV2(
    const EngineContextualTextLiteralAuthorityHandleV2& handle,
    EngineContextualTextLiteralAuthoritySnapshotV2* snapshot,
    EngineApiDiagnostic* diagnostic);
std::span<const EngineContextualTextPreparedExecutionEntryV2>
ViewPreparedContextualTextLiteralSetV2(
    const PreparedContextualTextLiteralSetV2& prepared) noexcept;
const EngineContextualTextPreparedExecutionEntryV2*
FindPreparedContextualTextExecutionEntryV2(
    const PreparedContextualTextLiteralSetV2& prepared,
    std::uint64_t literal_occurrence,
    std::uint64_t node_id,
    std::uint32_t literal_expression_id,
    std::uint32_t comparison_expression_id,
    std::uint32_t target_expression_id,
    std::uint32_t source_node_id,
    std::uint32_t literal_descriptor_handle,
    std::uint32_t target_descriptor_handle,
    std::uint8_t literal_argument_ordinal,
    std::uint8_t target_argument_ordinal,
    const scratchbird::engine::sblr::ContextualTextUuidV2&
        equality_operation_uuid,
    std::uint64_t equality_operation_generation) noexcept;
EngineContextualTextPreparedResourceEstimateV2
EstimatePreparedContextualTextLiteralResourcesV2(
    const PreparedContextualTextLiteralSetV2& prepared) noexcept;
std::span<const ContextualTextExecutionAuthorityEntryV2>
ViewContextualTextExecutionAuthorityLeaseV2(
    const ContextualTextExecutionAuthorityLeaseV2& lease) noexcept;
const ContextualTextExecutionAuthorityEntryV2*
FindContextualTextExecutionAuthorityEntryV2(
    const ContextualTextExecutionAuthorityLeaseV2& lease,
    std::uint32_t literal_expression_id,
    std::uint32_t comparison_expression_id,
    std::uint32_t target_expression_id,
    std::uint32_t source_node_id,
    std::uint32_t literal_descriptor_handle,
    std::uint32_t target_descriptor_handle,
    const scratchbird::engine::sblr::ContextualTextUuidV2&
        equality_operation_uuid,
    std::uint64_t equality_operation_generation) noexcept;
EngineContextualTextLeaseResourceEstimateV2
EstimateContextualTextExecutionAuthorityLeaseRetainedBytesV2(
    const ContextualTextExecutionAuthorityLeaseV2& lease) noexcept;
EngineApiDiagnostic RevokeContextualTextLiteralAuthorityV2(
    EngineContextualTextLiteralAuthorityHandleV2* handle,
    std::string_view reason);

}  // namespace scratchbird::engine::internal_api
