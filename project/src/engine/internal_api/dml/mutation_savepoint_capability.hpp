// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "api_types.hpp"
#include <cstdint>
#include <span>
#include <string_view>

namespace scratchbird::engine::internal_api {

// Engine-private producer identities, not parser options or a persisted format.
enum class MgaMutationProducer : std::uint8_t {
  row_version, row_directory, relation_descriptor, index_membership,
  index_candidate_pages, overflow_payload, allocation_reservation,
  immediate_constraint_read, deferred_constraint_reservation,
  literal_default, sequence_default, trigger_body, cascade_body,
  catalog_mutation, security_mutation, temporary_lifetime,
  large_value_reclaim, index_rebuild, serializable_conflict_tracking, metrics, external_effect, unknown,
  count
};
enum class MgaSavepointDisposition : std::uint8_t {
  write_set_rewind, idempotent_compensation, retained_transaction_effect,
  prohibited, unsupported
};
struct MgaMutationSavepointCapability {
  MgaMutationProducer producer;
  std::string_view name;
  MgaSavepointDisposition disposition;
  std::string_view authority;
};
std::span<const MgaMutationSavepointCapability> MgaMutationSavepointCapabilities();
const MgaMutationSavepointCapability& LookupMgaMutationSavepointCapability(
    MgaMutationProducer producer) noexcept;

enum class MgaDmlMutationKind : std::uint8_t { insert, update, delete_rows };
struct CrudIndexRecord;
bool IsAdmittedMgaSavepointIndexProfile(const CrudIndexRecord& index);

// Gate canonical operation effects at the common dispatcher, before any
// early-return executor branch. Operation identity must already be validated.
EngineApiDiagnostic AdmitMgaSavepointOperation(
    const EngineRequestContext& context, std::string_view operation_id);

// Lower-level producer guard for effects reachable from read operations or
// internal procedures. It cannot replace whole-statement preflight: it closes
// a nested/direct entry before that producer's first durable side effect.
EngineApiDiagnostic AdmitMgaSavepointProducer(
    const EngineRequestContext& context, MgaMutationProducer producer);

// Read the actual engine-owned boundary and complete target metadata BEFORE
// the initial/direct/bulk lane can write. Never accepts a caller's effect list.
// No boundary means no savepoint-specific restriction. The canonical binders
// pass require_statement_boundary because they subsequently create one.
EngineApiDiagnostic AdmitMgaDmlSavepointMutation(
    const EngineRequestContext& context, std::string_view target_relation_uuid,
    MgaDmlMutationKind mutation, bool require_statement_boundary = false);

}  // namespace scratchbird::engine::internal_api
