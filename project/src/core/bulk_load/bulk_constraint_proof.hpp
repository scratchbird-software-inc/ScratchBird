// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-BULK-CONSTRAINT-PROOF-ANCHOR
#include "runtime_platform.hpp"
#include "uuid.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::bulk_load {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::u64;

struct BulkConstraintProofEvidence {
  std::string evidence_kind;
  std::string evidence_id;
};

struct BulkConstraintProofKeyRef {
  std::string encoded_key;
  std::string row_uuid;
  std::string version_uuid;
  u64 source_ordinal = 0;
  bool null_key = false;
};

struct BulkUniqueProofRequest {
  std::string constraint_uuid;
  std::string index_uuid;
  std::string table_uuid;
  std::string column_name;
  bool nulls_distinct = true;
  bool incoming_keys_presorted = false;
  std::vector<BulkConstraintProofKeyRef> incoming_keys;
  std::vector<BulkConstraintProofKeyRef> visible_keys;
};

struct BulkForeignKeyProofRequest {
  std::string constraint_uuid;
  std::string child_table_uuid;
  std::string child_column_name;
  std::string parent_table_uuid;
  std::string parent_column_name;
  std::string parent_index_uuid;
  bool batch_local_parent_allowed = true;
  std::vector<BulkConstraintProofKeyRef> child_keys;
  std::vector<BulkConstraintProofKeyRef> visible_parent_keys;
  std::vector<BulkConstraintProofKeyRef> batch_parent_keys;
};

struct BulkConstraintProofRequest {
  TypedUuid database_uuid;
  TypedUuid object_uuid;
  TypedUuid transaction_uuid;
  u64 local_transaction_id = 0;
  std::string route = "direct_physical_bulk";
  bool direct_physical_bulk = true;
  bool strict_bulk_load = false;
  std::vector<BulkUniqueProofRequest> unique_proofs;
  std::vector<BulkForeignKeyProofRequest> foreign_key_proofs;
};

struct BulkConstraintProofResult {
  Status status;
  bool accepted = false;
  bool refused = false;
  bool unique_proof_selected = false;
  bool foreign_key_proof_selected = false;
  u64 unique_constraint_count = 0;
  u64 unique_incoming_key_count = 0;
  u64 unique_visible_key_count = 0;
  u64 foreign_key_constraint_count = 0;
  u64 foreign_key_child_ref_count = 0;
  u64 foreign_key_parent_key_count = 0;
  std::string refusal_reason;
  std::vector<BulkConstraintProofEvidence> evidence;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && accepted; }
};

BulkConstraintProofResult ProveBulkConstraints(
    const BulkConstraintProofRequest& request);

DiagnosticRecord MakeBulkConstraintProofDiagnostic(Status status,
                                                   std::string diagnostic_code,
                                                   std::string message_key,
                                                   std::string detail = {});

}  // namespace scratchbird::core::bulk_load
