// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SB-BULK-CONSTRAINT-PROOF-ANCHOR
#include "bulk_constraint_proof.hpp"

#include "index_key_encoding.hpp"

#include <algorithm>
#include <set>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace scratchbird::core::bulk_load {
namespace {

using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status ProofOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::engine};
}

Status ProofErrorStatus() {
  return {StatusCode::platform_required_feature_missing,
          Severity::error,
          Subsystem::engine};
}

void AddEvidence(BulkConstraintProofResult* result,
                 std::string kind,
                 std::string id) {
  if (result != nullptr) {
    result->evidence.push_back({std::move(kind), std::move(id)});
  }
}

int CompareUnsignedText(std::string_view left, std::string_view right) {
  const auto count = std::min(left.size(), right.size());
  for (std::size_t i = 0; i < count; ++i) {
    const auto l = static_cast<unsigned char>(left[i]);
    const auto r = static_cast<unsigned char>(right[i]);
    if (l < r) {
      return -1;
    }
    if (l > r) {
      return 1;
    }
  }
  return left.size() < right.size() ? -1 : (right.size() < left.size() ? 1 : 0);
}

int CompareEncodedProofKey(std::string_view left, std::string_view right) {
  if (scratchbird::core::index::IsOrderPreservingIndexKeyEncoding(left) &&
      scratchbird::core::index::IsOrderPreservingIndexKeyEncoding(right)) {
    const auto compare =
        scratchbird::core::index::CompareEncodedIndexKeyBytes(left, right);
    if (compare.ok()) {
      return compare.comparison;
    }
  }
  return CompareUnsignedText(left, right);
}

bool RefLess(const BulkConstraintProofKeyRef& left,
             const BulkConstraintProofKeyRef& right) {
  const int key_compare =
      CompareEncodedProofKey(left.encoded_key, right.encoded_key);
  if (key_compare != 0) {
    return key_compare < 0;
  }
  const int row_compare = CompareUnsignedText(left.row_uuid, right.row_uuid);
  if (row_compare != 0) {
    return row_compare < 0;
  }
  const int version_compare =
      CompareUnsignedText(left.version_uuid, right.version_uuid);
  if (version_compare != 0) {
    return version_compare < 0;
  }
  return left.source_ordinal < right.source_ordinal;
}

bool KeyEqual(std::string_view left, std::string_view right) {
  return CompareEncodedProofKey(left, right) == 0;
}

bool UnsafeLegacyKey(const BulkConstraintProofKeyRef& ref) {
  return scratchbird::core::index::IsUnsafeLegacyIndexKeyEncoding(
      ref.encoded_key);
}

std::string ConflictDetail(const std::string& reason,
                           const std::string& constraint_uuid,
                           const std::string& index_uuid,
                           const std::string& key) {
  return reason + ":constraint=" + constraint_uuid + ":index=" +
         index_uuid + ":key=" + key;
}

BulkConstraintProofResult Refuse(BulkConstraintProofResult result,
                                 std::string code,
                                 std::string message_key,
                                 std::string reason,
                                 std::string detail) {
  result.status = ProofErrorStatus();
  result.accepted = false;
  result.refused = true;
  result.refusal_reason = std::move(reason);
  AddEvidence(&result, "bulk_constraint_proof_conflict_reason",
              result.refusal_reason);
  AddEvidence(&result, "bulk_constraint_proof_refused", "true");
  result.diagnostic = MakeBulkConstraintProofDiagnostic(
      result.status,
      std::move(code),
      std::move(message_key),
      std::move(detail));
  return result;
}

void AddBaseEvidence(const BulkConstraintProofRequest& request,
                     BulkConstraintProofResult* result) {
  AddEvidence(result, "bulk_constraint_proof_route_selected",
              request.route.empty() ? "direct_physical_bulk" : request.route);
  AddEvidence(result, "bulk_constraint_proof_direct_physical_bulk",
              request.direct_physical_bulk ? "true" : "false");
  AddEvidence(result, "strict_bulk_load_constraint_proof_selected",
              request.strict_bulk_load ? "true" : "false");
  AddEvidence(result, "bulk_constraint_disabling", "false");
  AddEvidence(result, "bulk_constraint_proof_finality_authority",
              "mga_transaction_inventory");
  AddEvidence(result, "bulk_constraint_proof_transaction_finality_authority",
              "false");
  AddEvidence(result, "bulk_constraint_proof_visibility_authority", "false");
  AddEvidence(result, "bulk_constraint_proof_authorization_authority", "false");
  AddEvidence(result, "bulk_constraint_proof_recovery_authority", "false");
  AddEvidence(result, "parser_finality_authority", "false");
  AddEvidence(result, "reference_finality_authority", "false");
  AddEvidence(result, "mga_finality_authority",
              "engine_transaction_inventory");
  AddEvidence(result, "bulk_constraint_proof_local_transaction_id",
              std::to_string(request.local_transaction_id));
}

bool EffectiveKeyRef(const BulkConstraintProofKeyRef& ref,
                     bool nulls_distinct) {
  if (nulls_distinct && ref.null_key) {
    return false;
  }
  return true;
}

BulkConstraintProofResult ProveUniqueConstraints(
    const BulkConstraintProofRequest& request,
    BulkConstraintProofResult result) {
  result.unique_proof_selected = !request.unique_proofs.empty();
  result.unique_constraint_count =
      static_cast<u64>(request.unique_proofs.size());
  if (!request.unique_proofs.empty()) {
    AddEvidence(&result, "bulk_unique_proof_shape", "sorted");
    AddEvidence(&result, "bulk_unique_proof_order",
                "encoded_key,row_uuid,version_uuid");
    AddEvidence(&result, "bulk_unique_proof_physical_append_selected_before_proof",
                "false");
    AddEvidence(&result, "bulk_unique_proof_publication_authority", "false");
    AddEvidence(&result, "bulk_unique_proof_transaction_finality_authority",
                "false");
    AddEvidence(&result, "bulk_unique_proof_visibility_authority", "false");
    AddEvidence(&result, "bulk_unique_proof_recovery_authority", "false");
    AddEvidence(&result, "bulk_unique_proof_constraint_count",
                std::to_string(result.unique_constraint_count));
    AddEvidence(&result, "bulk_unique_proof_persisted_source",
                "visible_rows_and_index_entries");
  }

  for (const auto& proof : request.unique_proofs) {
    if (proof.constraint_uuid.empty() || proof.index_uuid.empty() ||
        proof.table_uuid.empty()) {
      return Refuse(std::move(result),
                    "SB-BULK-CONSTRAINT-UNIQUE-DESCRIPTOR-INVALID",
                    "core.bulk_load.constraint.unique_descriptor_invalid",
                    "bulk_unique_proof_descriptor_invalid",
                    "unique constraint proof requires constraint and index UUIDs");
    }

    std::vector<BulkConstraintProofKeyRef> incoming;
    incoming.reserve(proof.incoming_keys.size());
    for (const auto& ref : proof.incoming_keys) {
      if (UnsafeLegacyKey(ref)) {
        AddEvidence(&result, "bulk_unique_proof_unsafe_key_encoding", "SBK1");
        AddEvidence(&result, "bulk_unique_proof_conflict_constraint",
                    proof.constraint_uuid);
        return Refuse(std::move(result),
                      "SB-BULK-CONSTRAINT-UNIQUE-UNSAFE-KEY-ENCODING",
                      "core.bulk_load.constraint.unique_unsafe_key_encoding",
                      "bulk_unique_proof_unsafe_key_encoding",
                      ConflictDetail("bulk_unique_proof_unsafe_key_encoding",
                                     proof.constraint_uuid,
                                     proof.index_uuid,
                                     "SBK1"));
      }
      if (EffectiveKeyRef(ref, proof.nulls_distinct)) {
        incoming.push_back(ref);
      }
    }
    std::stable_sort(incoming.begin(), incoming.end(), RefLess);
    result.unique_incoming_key_count += static_cast<u64>(incoming.size());
    AddEvidence(&result, "bulk_unique_proof_null_policy",
                proof.nulls_distinct ? "nulls_distinct"
                                     : "nulls_not_distinct");

    std::string previous_key;
    bool have_previous = false;
    u64 sorted_run_count = 0;
    for (const auto& ref : incoming) {
      if (!have_previous || !KeyEqual(ref.encoded_key, previous_key)) {
        ++sorted_run_count;
        previous_key = ref.encoded_key;
        have_previous = true;
        continue;
      }
      AddEvidence(&result, "bulk_unique_proof_conflict_key",
                  ref.encoded_key);
      AddEvidence(&result, "bulk_unique_proof_conflict_constraint",
                  proof.constraint_uuid);
      AddEvidence(&result, "bulk_unique_proof_duplicate_run_count", "1");
      AddEvidence(&result, "bulk_unique_proof_duplicate_run_absent", "false");
      return Refuse(std::move(result),
                    "SB-BULK-CONSTRAINT-UNIQUE-DUPLICATE-BATCH",
                    "core.bulk_load.constraint.unique_duplicate_batch",
                    "bulk_unique_proof_duplicate_in_batch",
                    ConflictDetail("bulk_unique_proof_duplicate_in_batch",
                                   proof.constraint_uuid,
                                   proof.index_uuid,
                                   ref.encoded_key));
    }
    AddEvidence(&result, "bulk_unique_proof_sorted_key_run_count",
                std::to_string(sorted_run_count));
    AddEvidence(&result, "bulk_unique_proof_duplicate_run_count", "0");
    AddEvidence(&result, "bulk_unique_proof_duplicate_run_absent", "true");

    std::set<std::string> visible_keys;
    for (const auto& ref : proof.visible_keys) {
      if (UnsafeLegacyKey(ref)) {
        AddEvidence(&result, "bulk_unique_proof_unsafe_key_encoding", "SBK1");
        AddEvidence(&result, "bulk_unique_proof_conflict_constraint",
                    proof.constraint_uuid);
        return Refuse(std::move(result),
                      "SB-BULK-CONSTRAINT-UNIQUE-UNSAFE-KEY-ENCODING",
                      "core.bulk_load.constraint.unique_unsafe_key_encoding",
                      "bulk_unique_proof_unsafe_key_encoding",
                      ConflictDetail("bulk_unique_proof_unsafe_key_encoding",
                                     proof.constraint_uuid,
                                     proof.index_uuid,
                                     "SBK1"));
      }
      if (EffectiveKeyRef(ref, proof.nulls_distinct)) {
        visible_keys.insert(ref.encoded_key);
      }
    }
    result.unique_visible_key_count += static_cast<u64>(visible_keys.size());
    for (const auto& ref : incoming) {
      if (visible_keys.count(ref.encoded_key) == 0) {
        continue;
      }
      AddEvidence(&result, "bulk_unique_proof_conflict_key",
                  ref.encoded_key);
      AddEvidence(&result, "bulk_unique_proof_conflict_constraint",
                  proof.constraint_uuid);
      return Refuse(std::move(result),
                    "SB-BULK-CONSTRAINT-UNIQUE-PERSISTED-CONFLICT",
                    "core.bulk_load.constraint.unique_persisted_conflict",
                    "bulk_unique_proof_persisted_conflict",
                    ConflictDetail("bulk_unique_proof_persisted_conflict",
                                   proof.constraint_uuid,
                                   proof.index_uuid,
                                   ref.encoded_key));
    }
  }

  if (!request.unique_proofs.empty()) {
    AddEvidence(&result, "bulk_unique_proof_result", "accepted");
    AddEvidence(&result, "bulk_unique_proof_batch_keys",
                std::to_string(result.unique_incoming_key_count));
    AddEvidence(&result, "bulk_unique_proof_visible_keys",
                std::to_string(result.unique_visible_key_count));
    AddEvidence(&result, "bulk_unique_proof_sorted_duplicate_runs_absent",
                "true");
    AddEvidence(&result, "bulk_unique_proof_persisted_conflict_absent",
                "true");
    for (const auto& proof : request.unique_proofs) {
      AddEvidence(&result, "constraint_proof_store",
                  "unique_preflight:" + proof.index_uuid);
      AddEvidence(&result, "constraint_proof_hit",
                  "unique_preflight:" + proof.index_uuid);
    }
  }
  return result;
}

BulkConstraintProofResult ProveForeignKeys(
    const BulkConstraintProofRequest& request,
    BulkConstraintProofResult result) {
  result.foreign_key_proof_selected = !request.foreign_key_proofs.empty();
  result.foreign_key_constraint_count =
      static_cast<u64>(request.foreign_key_proofs.size());
  if (!request.foreign_key_proofs.empty()) {
    AddEvidence(&result, "bulk_fk_proof_shape", "hash");
    AddEvidence(&result, "bulk_fk_proof_constraint_count",
                std::to_string(result.foreign_key_constraint_count));
    AddEvidence(&result, "bulk_fk_proof_parent_source",
                "visible_parent_rows_and_batch_local_rows");
  }

  for (const auto& proof : request.foreign_key_proofs) {
    if (proof.constraint_uuid.empty() || proof.parent_index_uuid.empty()) {
      return Refuse(std::move(result),
                    "SB-BULK-CONSTRAINT-FK-DESCRIPTOR-INVALID",
                    "core.bulk_load.constraint.fk_descriptor_invalid",
                    "bulk_fk_proof_descriptor_invalid",
                    "foreign-key proof requires constraint and parent index UUIDs");
    }

    std::unordered_set<std::string> parent_keys;
    for (const auto& ref : proof.visible_parent_keys) {
      if (!ref.null_key) {
        parent_keys.insert(ref.encoded_key);
      }
    }
    if (proof.batch_local_parent_allowed) {
      for (const auto& ref : proof.batch_parent_keys) {
        if (!ref.null_key) {
          parent_keys.insert(ref.encoded_key);
        }
      }
    }
    result.foreign_key_parent_key_count +=
        static_cast<u64>(parent_keys.size());

    for (const auto& child : proof.child_keys) {
      if (child.null_key) {
        continue;
      }
      ++result.foreign_key_child_ref_count;
      if (parent_keys.count(child.encoded_key) != 0) {
        continue;
      }
      AddEvidence(&result, "bulk_fk_proof_missing_parent_key",
                  child.encoded_key);
      AddEvidence(&result, "bulk_fk_proof_conflict_constraint",
                  proof.constraint_uuid);
      return Refuse(std::move(result),
                    "SB-BULK-CONSTRAINT-FK-PARENT-MISSING",
                    "core.bulk_load.constraint.fk_parent_missing",
                    "bulk_fk_proof_parent_missing",
                    ConflictDetail("bulk_fk_proof_parent_missing",
                                   proof.constraint_uuid,
                                   proof.parent_index_uuid,
                                   child.encoded_key));
    }
  }

  if (!request.foreign_key_proofs.empty()) {
    AddEvidence(&result, "bulk_fk_proof_result", "accepted");
    AddEvidence(&result, "bulk_fk_proof_child_refs",
                std::to_string(result.foreign_key_child_ref_count));
    AddEvidence(&result, "bulk_fk_proof_parent_keys",
                std::to_string(result.foreign_key_parent_key_count));
    AddEvidence(&result, "bulk_fk_proof_parent_existence",
                "visible_or_batch_parent_hash_hit");
  }
  return result;
}

}  // namespace

BulkConstraintProofResult ProveBulkConstraints(
    const BulkConstraintProofRequest& request) {
  BulkConstraintProofResult result;
  AddBaseEvidence(request, &result);

  if (request.unique_proofs.empty() && request.foreign_key_proofs.empty()) {
    AddEvidence(&result, "bulk_constraint_proof_no_constraints", "true");
    AddEvidence(&result, "bulk_constraint_proof_unique_selected", "false");
    AddEvidence(&result, "bulk_constraint_proof_fk_selected", "false");
    AddEvidence(&result, "bulk_constraint_proof_result", "accepted");
    result.status = ProofOkStatus();
    result.accepted = true;
    result.diagnostic = MakeBulkConstraintProofDiagnostic(
        result.status,
        "SB-BULK-CONSTRAINT-PROOF-OK",
        "core.bulk_load.constraint.proof_ok",
        "bulk constraint proof accepted with no unique or foreign-key constraints");
    return result;
  }

  if (!request.database_uuid.valid() || !request.object_uuid.valid() ||
      !request.transaction_uuid.valid() || request.local_transaction_id == 0) {
    return Refuse(std::move(result),
                  "SB-BULK-CONSTRAINT-AUTHORITY-REQUIRED",
                  "core.bulk_load.constraint.authority_required",
                  "bulk_constraint_proof_authority_missing",
                  "database, object, transaction UUIDs and local transaction ID are required");
  }

  result = ProveUniqueConstraints(request, std::move(result));
  if (!result.status.ok() && result.refused) {
    return result;
  }
  result = ProveForeignKeys(request, std::move(result));
  if (!result.status.ok() && result.refused) {
    return result;
  }

  AddEvidence(&result, "bulk_constraint_proof_result", "accepted");
  AddEvidence(&result, "bulk_constraint_proof_unique_selected",
              result.unique_proof_selected ? "true" : "false");
  AddEvidence(&result, "bulk_constraint_proof_fk_selected",
              result.foreign_key_proof_selected ? "true" : "false");
  result.status = ProofOkStatus();
  result.accepted = true;
  result.diagnostic = MakeBulkConstraintProofDiagnostic(
      result.status,
      "SB-BULK-CONSTRAINT-PROOF-OK",
      "core.bulk_load.constraint.proof_ok",
      "bulk constraint proof accepted before physical publication");
  return result;
}

DiagnosticRecord MakeBulkConstraintProofDiagnostic(Status status,
                                                   std::string diagnostic_code,
                                                   std::string message_key,
                                                   std::string detail) {
  std::vector<scratchbird::core::platform::DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "core.bulk_load.constraint_proof",
                        status.ok() ? "" : "refuse before row or index publication");
}

}  // namespace scratchbird::core::bulk_load
