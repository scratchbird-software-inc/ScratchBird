// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "crud_support/crud_store.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: PRF_CONSTRAINT_DML_ENFORCEMENT
// Engine-owned logical-integrity enforcement for constraint metadata that is
// already represented in MGA relation descriptors/index metadata. Catalog
// descriptors remain authoritative when the catalog worker lands; this layer
// fails closed when only external execution artifacts are available.

struct ConstraintDmlValidationResult {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  std::vector<std::pair<std::string, std::string>> values;
  std::vector<EngineEvidenceReference> evidence;
};

struct ConstraintDmlProofContext {
  std::string database_uuid;
  std::string transaction_uuid;
  std::string principal_uuid;
  std::string isolation_level;
  std::string trace_tag_fingerprint;
  std::uint64_t local_transaction_id = 0;
  std::uint64_t snapshot_visible_through_local_transaction_id = 0;
  std::uint64_t catalog_generation_id = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t resource_epoch = 0;
  std::uint64_t name_resolution_epoch = 0;
  bool security_context_present = false;
};

struct ConstraintDmlValidationCache {
  std::map<std::string, std::vector<std::pair<std::string, std::map<std::string, std::string>>>>
      constraint_columns_by_table_uuid;
  std::map<std::string, std::vector<CrudRowVersionRecord>> visible_rows_by_table_uuid;
  std::set<std::string> visible_rows_built_for_table_uuid;
  std::map<std::string, std::map<std::string, std::set<std::string>>> unique_key_rows_by_index_uuid;
  std::set<std::string> unique_key_rows_built_for_index_uuid;
  std::set<std::string> index_backed_unique_preflight_proofs;
  std::map<std::string, std::set<std::string>> column_values_by_table_column;
  std::set<std::string> column_values_built_for_table_column;
  std::vector<std::pair<std::string, ConstraintDmlProofContext>> validation_proofs;
  std::map<std::string, std::string> validation_proof_payloads;
};

struct ConstraintDmlValidationOptions {
  bool validate_unique_constraints = true;
  bool validate_foreign_key_constraints = true;
};

std::optional<std::string> FindConstraintDmlProofPayload(
    const ConstraintDmlValidationCache* cache,
    const EngineRequestContext& context,
    const std::string& proof_kind,
    const std::string& proof_identity,
    std::vector<EngineEvidenceReference>* evidence = nullptr);

void StoreConstraintDmlProof(
    ConstraintDmlValidationCache* cache,
    const EngineRequestContext& context,
    const std::string& proof_kind,
    const std::string& proof_identity,
    const std::string& payload = {},
    std::vector<EngineEvidenceReference>* evidence = nullptr);

void RecordIndexBackedUniquePreflightProof(
    ConstraintDmlValidationCache* cache,
    const EngineRequestContext& context,
    const CrudIndexRecord& index,
    const std::string& row_uuid,
    const std::vector<std::pair<std::string, std::string>>& values,
    std::vector<EngineEvidenceReference>* evidence = nullptr);

ConstraintDmlValidationResult ApplyConstraintDefaultsForInsert(
    const EngineRequestContext& context,
    const CrudTableRecord& table,
    const std::vector<std::pair<std::string, std::string>>& input_values,
    ConstraintDmlValidationCache* cache = nullptr);

ConstraintDmlValidationResult ValidateImmediateRowConstraints(
    const EngineRequestContext& context,
    const CrudState& state,
    const CrudTableRecord& table,
    const std::string& row_uuid,
    const std::vector<std::pair<std::string, std::string>>& values,
    const std::string& mutation_kind,
    ConstraintDmlValidationCache* cache = nullptr);

ConstraintDmlValidationResult ValidateImmediateRowConstraintsWithOptions(
    const EngineRequestContext& context,
    const CrudState& state,
    const CrudTableRecord& table,
    const std::string& row_uuid,
    const std::vector<std::pair<std::string, std::string>>& values,
    const std::string& mutation_kind,
    const ConstraintDmlValidationOptions& options,
    ConstraintDmlValidationCache* cache = nullptr);

EngineApiDiagnostic ValidateImmediateDeleteConstraints(
    const EngineRequestContext& context,
    const CrudState& state,
    const CrudTableRecord& table,
    const CrudRowVersionRecord& deleted_row);

EngineApiDiagnostic ValidateImmediateParentKeyUpdateConstraints(
    const EngineRequestContext& context,
    const CrudState& state,
    const CrudTableRecord& table,
    const CrudRowVersionRecord& old_row,
    const std::vector<std::pair<std::string, std::string>>& new_values);

bool UpdateTouchesImmediateConstraintColumns(
    const CrudTableRecord& table,
    const std::vector<std::string>& assigned_columns,
    const ConstraintDmlValidationOptions& options = ConstraintDmlValidationOptions{});

bool UpdateTouchesParentKeyColumns(const CrudTableRecord& table,
                                   const std::vector<std::string>& assigned_columns);

EngineApiDiagnostic ValidateDeferredTransactionConstraints(const EngineRequestContext& context);

}  // namespace scratchbird::engine::internal_api
