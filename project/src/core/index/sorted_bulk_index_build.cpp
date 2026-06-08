// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sorted_bulk_index_build.hpp"

#include "uuid.hpp"

#include <algorithm>
#include <optional>
#include <string_view>

namespace scratchbird::core::index {
namespace {

namespace page = scratchbird::storage::page;

using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::byte;

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::engine};
}

Status ErrorStatus() {
  return {StatusCode::platform_required_feature_missing,
          Severity::error,
          Subsystem::engine};
}

DiagnosticRecord Diagnostic(Status status,
                            std::string code,
                            std::string key,
                            std::string detail = {}) {
  std::vector<scratchbird::core::platform::DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(code),
                        std::move(key),
                        std::move(arguments),
                        {},
                        "core.index.sorted_bulk_build",
                        status.ok() ? "" : "refuse before exact index append");
}

SortedBulkIndexBuildResult Refuse(std::string code,
                                  std::string key,
                                  std::string detail = {}) {
  SortedBulkIndexBuildResult result;
  result.status = ErrorStatus();
  result.diagnostic = Diagnostic(result.status,
                                 std::move(code),
                                 std::move(key),
                                 std::move(detail));
  return result;
}

SortedBulkIndexBuildResult RefuseWithEvidence(SortedBulkIndexBuildResult result,
                                              std::string code,
                                              std::string key,
                                              std::string detail = {}) {
  result.status = ErrorStatus();
  result.accepted = false;
  result.diagnostic = Diagnostic(result.status,
                                 std::move(code),
                                 std::move(key),
                                 std::move(detail));
  return result;
}

void AddEvidence(SortedBulkIndexBuildResult* result,
                 std::string kind,
                 std::string id) {
  if (result != nullptr) {
    result->evidence.push_back({std::move(kind), std::move(id)});
  }
}

void AddUniqueNonAuthorityEvidence(SortedBulkIndexBuildResult* result) {
  AddEvidence(result, "sorted_bulk_unique_proof_visibility_authority", "false");
  AddEvidence(result, "sorted_bulk_unique_proof_authorization_authority", "false");
  AddEvidence(result, "sorted_bulk_unique_proof_transaction_finality_authority",
              "false");
  AddEvidence(result, "sorted_bulk_unique_proof_cleanup_authority", "false");
  AddEvidence(result, "sorted_bulk_unique_proof_recovery_authority", "false");
  AddEvidence(result, "sorted_bulk_unique_proof_commit_publication_inferred",
              "false");
  AddEvidence(result, "sorted_bulk_unique_proof_physical_append_authorized",
              "false");
  AddEvidence(result, "sorted_bulk_unique_proof_root_publish_authorized",
              "false");
  AddEvidence(result, "sorted_bulk_unique_proof_before_physical_append", "true");
  AddEvidence(result, "sorted_bulk_unique_proof_before_root_publish", "true");
}

bool OrderedBuildFamily(IndexFamily family) {
  switch (family) {
    case IndexFamily::btree:
    case IndexFamily::unique_btree:
    case IndexFamily::expression:
    case IndexFamily::partial:
    case IndexFamily::covering:
      return true;
    default:
      return false;
  }
}

bool UniqueBuildRequested(const SortedBulkIndexMetadata& metadata) {
  return metadata.unique || metadata.family == IndexFamily::unique_btree;
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

int CompareEncodedSortKey(std::string_view left, std::string_view right) {
  if (IsOrderPreservingIndexKeyEncoding(left) &&
      IsOrderPreservingIndexKeyEncoding(right)) {
    const auto compare = CompareEncodedIndexKeyBytes(left, right);
    if (compare.ok()) {
      return compare.comparison;
    }
  }
  return CompareUnsignedText(left, right);
}

bool EntryLess(const SortedBulkIndexEntry& left,
               const SortedBulkIndexEntry& right) {
  const int key_compare = CompareEncodedSortKey(left.encoded_key, right.encoded_key);
  if (key_compare != 0) {
    return key_compare < 0;
  }
  const int row_compare = CompareUnsignedText(left.row_uuid, right.row_uuid);
  if (row_compare != 0) {
    return row_compare < 0;
  }
  return CompareUnsignedText(left.version_uuid, right.version_uuid) < 0;
}

bool KeyEqual(std::string_view left, std::string_view right) {
  return CompareEncodedSortKey(left, right) == 0;
}

bool UniqueKeyParticipates(const SortedBulkIndexEntry& entry,
                           bool nulls_distinct) {
  return !(entry.null_key && nulls_distinct);
}

std::vector<byte> KeyBytes(const std::string& key) {
  return {key.begin(), key.end()};
}

std::vector<byte> PhysicalEncodedKeyFor(const SortedBulkIndexMetadata& metadata,
                                        const SortedBulkIndexEntry& entry) {
  if (IsOrderPreservingIndexKeyEncoding(entry.encoded_key)) {
    return KeyBytes(entry.encoded_key);
  }

  IndexKeyEncodingComponent component;
  component.kind = IndexKeyComponentKind::scalar;
  component.ordinal = 0;
  component.type_descriptor_uuid = metadata.index_uuid;
  component.payload = KeyBytes(entry.encoded_key);
  component.is_null = entry.null_key;
  const auto encoded = EncodeIndexKey({component}, {});
  return encoded.ok() ? encoded.encoded : std::vector<byte>{};
}

std::optional<TypedUuid> ParseProofUuid(UuidKind kind,
                                        const std::string& text) {
  const auto parsed = scratchbird::core::uuid::ParseTypedUuid(kind, text);
  if (!parsed.ok()) {
    return std::nullopt;
  }
  return parsed.value;
}

void AddReservationEvidence(
    const UniqueIndexReservationResult& source,
    SortedBulkIndexBuildResult* result) {
  AddEvidence(result, "sorted_bulk_unique_reservation_decision",
              UniqueIndexReservationDecisionName(source.decision));
  AddEvidence(result, "sorted_bulk_unique_reservation_conflict_state",
              UniqueIndexReservationConflictStateName(source.conflict_state));
  for (const auto& item : source.evidence) {
    AddEvidence(result, "sorted_bulk_unique_reservation_ledger_evidence", item);
  }
}

void AddPhysicalBulkEvidence(
    const page::IndexBtreePhysicalBulkBuildResult& source,
    SortedBulkIndexBuildResult* result) {
  for (const auto& item : source.evidence) {
    const auto separator = item.find('=');
    if (separator == std::string::npos) {
      AddEvidence(result, "sorted_bulk_physical_tree_evidence", item);
      continue;
    }
    AddEvidence(result,
                "sorted_bulk_physical_tree_" + item.substr(0, separator),
                item.substr(separator + 1));
  }
}

SortedBulkIndexBuildResult BuildCandidateRootGeneration(
    const SortedBulkIndexBuildRequest& request,
    SortedBulkIndexBuildResult result) {
  page::IndexBtreePhysicalBulkBuildRequest physical_request;
  physical_request.index_uuid = request.metadata.index_uuid;
  physical_request.page_size = request.metadata.physical_page_size;
  physical_request.leaf_entry_capacity = request.metadata.leaf_entry_capacity;
  physical_request.internal_entry_capacity =
      request.metadata.internal_entry_capacity;
  physical_request.sorted_order_proof_valid = true;
  physical_request.sorted_cells.reserve(result.entries.size());

  u64 covering_payload_count = 0;
  for (const auto& entry : result.entries) {
    const auto row_uuid = ParseProofUuid(UuidKind::row, entry.row_uuid);
    const auto version_uuid = ParseProofUuid(UuidKind::row, entry.version_uuid);
    if (!row_uuid || !version_uuid) {
      result.invalid_descriptor_refused = true;
      result.entries.clear();
      return RefuseWithEvidence(
          std::move(result),
          "SB-INDEX-SORTED-BULK-DESCRIPTOR-INVALID",
          "index.sorted_bulk.descriptor_invalid",
          "physical candidate root generation requires typed row/version UUIDs");
    }

    page::IndexBtreeCell cell;
    cell.key_ordinal = 0;
    cell.encoded_key = PhysicalEncodedKeyFor(request.metadata, entry);
    if (cell.encoded_key.empty()) {
      result.invalid_descriptor_refused = true;
      result.entries.clear();
      return RefuseWithEvidence(
          std::move(result),
          "SB-INDEX-SORTED-BULK-PHYSICAL-VALIDATION-REFUSED",
          "index.sorted_bulk.physical_validation_refused",
          "SB-INDEX-KEY-ENCODING-EMPTY");
    }
    cell.row_uuid = *row_uuid;
    cell.version_uuid = *version_uuid;
    physical_request.sorted_cells.push_back(std::move(cell));

    if (request.metadata.family == IndexFamily::covering) {
      if (entry.payload_value.empty()) {
        result.invalid_descriptor_refused = true;
        result.entries.clear();
        return RefuseWithEvidence(
            std::move(result),
            "SB-INDEX-SORTED-BULK-COVERING-PAYLOAD-MISSING",
            "index.sorted_bulk.covering_payload_missing");
      }
      ++covering_payload_count;
    }
  }
  std::stable_sort(physical_request.sorted_cells.begin(),
                   physical_request.sorted_cells.end(),
                   [](const auto& left, const auto& right) {
                     const auto key_compare =
                         CompareEncodedIndexKeys(left.encoded_key,
                                                 right.encoded_key);
                     if (key_compare.ok() && key_compare.comparison != 0) {
                       return key_compare.comparison < 0;
                     }
                     const int row_compare =
                         scratchbird::core::uuid::CompareUuid128(
                             left.row_uuid.value, right.row_uuid.value);
                     if (row_compare != 0) {
                       return row_compare < 0;
                     }
                     return scratchbird::core::uuid::CompareUuid128(
                                left.version_uuid.value,
                                right.version_uuid.value) < 0;
                   });

  const auto built =
      page::BuildIndexBtreePhysicalBulkLoadedTree(physical_request);
  AddPhysicalBulkEvidence(built, &result);
  if (!built.ok()) {
    result.entries.clear();
    if (built.diagnostic.diagnostic_code.find("ORDER-PROOF") !=
        std::string::npos) {
      return RefuseWithEvidence(
          std::move(result),
          "SB-INDEX-SORTED-BULK-ORDER-PROOF-INVALID",
          "index.sorted_bulk.order_proof_invalid",
          built.diagnostic.diagnostic_code);
    }
    if (built.diagnostic.diagnostic_code.find("UNSAFE-LEGACY-KEY") !=
        std::string::npos) {
      result.unsafe_key_refused = true;
      return RefuseWithEvidence(
          std::move(result),
          "SB-INDEX-SORTED-BULK-UNSAFE-LEGACY-KEY",
          "index.sorted_bulk.unsafe_key_encoding",
          "SBK1");
    }
    return RefuseWithEvidence(
        std::move(result),
        "SB-INDEX-SORTED-BULK-PHYSICAL-VALIDATION-REFUSED",
        "index.sorted_bulk.physical_validation_refused",
        built.diagnostic.diagnostic_code);
  }

  result.candidate_root_generation.created = true;
  result.candidate_root_generation.physical_leaf_pack =
      built.physical_leaf_pack;
  result.candidate_root_generation.branch_levels_built =
      built.branch_levels_built;
  result.candidate_root_generation.fence_keys_stored = built.fence_keys_stored;
  result.candidate_root_generation.validated_tree = built.report.valid;
  result.candidate_root_generation.candidate_root_page_present =
      built.tree.root_page_number != 0;
  result.candidate_root_generation.root_publish_authorized = false;
  result.candidate_root_generation.physical_append_authorized = false;
  result.candidate_root_generation.candidate_generation = 1;
  result.candidate_root_generation.root_page_number =
      built.tree.root_page_number;
  result.candidate_root_generation.page_count = built.report.page_count;
  result.candidate_root_generation.leaf_page_count = built.leaf_page_count;
  result.candidate_root_generation.branch_level_count =
      built.branch_level_count;
  result.candidate_root_generation.live_entry_count =
      built.report.tuple_live_entry_estimate;
  result.candidate_root_generation.tree = built.tree;
  result.candidate_root_generation.report = built.report;

  result.leaf_generation_count = built.leaf_page_count;
  result.root_generation_count = 1;
  AddEvidence(&result, "sorted_bulk_index_physical_leaf_pack", "true");
  AddEvidence(&result,
              "sorted_bulk_index_branch_levels_built",
              built.branch_levels_built ? "true" : "false");
  AddEvidence(&result,
              "sorted_bulk_index_fence_keys_stored",
              built.fence_keys_stored ? "true" : "false");
  AddEvidence(&result,
              "sorted_bulk_index_candidate_root_generation_created",
              "true");
  AddEvidence(&result,
              "sorted_bulk_index_candidate_tree_validated",
              built.report.valid ? "true" : "false");
  AddEvidence(&result,
              "sorted_bulk_index_candidate_root_page_present",
              built.tree.root_page_number != 0 ? "true" : "false");
  AddEvidence(&result, "sorted_bulk_index_candidate_root_page_number",
              std::to_string(built.tree.root_page_number));
  AddEvidence(&result, "sorted_bulk_index_candidate_page_count",
              std::to_string(built.report.page_count));
  AddEvidence(&result, "sorted_bulk_index_candidate_live_entry_count",
              std::to_string(built.report.tuple_live_entry_estimate));
  AddEvidence(&result, "sorted_bulk_index_root_publish_authorized", "false");
  AddEvidence(&result, "sorted_bulk_index_physical_append_authorized", "false");
  const auto* capability =
      FindBuiltinIndexFamilyPhysicalCapabilityState(request.metadata.family);
  AddEvidence(&result,
              "sorted_bulk_index_runtime_route_capability",
              capability != nullptr && capability->runtime_available ? "true"
                                                                     : "false");
  AddEvidence(&result, "sorted_bulk_index_durable_metadata_published", "false");
  AddEvidence(&result, "sorted_bulk_index_rollback_reopen_repair_closed",
              "false");
  AddEvidence(&result, "sorted_bulk_index_transaction_mga_finality_engine_owned",
              "true");
  AddEvidence(&result, "sorted_bulk_index_commit_inferred", "false");
  if (request.metadata.family == IndexFamily::covering) {
    AddEvidence(&result,
                "sorted_bulk_index_covering_payload_layout_consumed",
                "true");
    AddEvidence(&result,
                "sorted_bulk_index_covering_payload_record_count",
                std::to_string(covering_payload_count));
  }
  return result;
}

bool InputOrderMatchesSortedOrder(
    const std::vector<SortedBulkIndexRowInput>& rows,
    const std::vector<SortedBulkIndexEntry>& sorted) {
  if (rows.size() != sorted.size()) {
    return false;
  }
  for (std::size_t i = 0; i < rows.size(); ++i) {
    if (rows[i].encoded_key != sorted[i].encoded_key ||
        rows[i].row_uuid != sorted[i].row_uuid ||
        rows[i].version_uuid != sorted[i].version_uuid) {
      return false;
    }
  }
  return true;
}

bool CapabilityBlockerDiagnostic(const DiagnosticRecord& diagnostic) {
  return diagnostic.diagnostic_code.rfind("INDEX.CAPABILITY.", 0) == 0;
}

OrderedBuildPlan ProofOnlyOrderedPlan(const OrderedBuildRequest& request,
                                      const DiagnosticRecord& blocker) {
  OrderedBuildPlan plan;
  plan.status = OkStatus();
  plan.admitted = true;
  plan.mode = request.input_presorted && request.order_proof_valid
                  ? (request.rebuild ? OrderedBuildMode::rebuild_presorted
                                     : OrderedBuildMode::bulk_presorted)
                  : (request.rebuild ? OrderedBuildMode::rebuild_external_sort
                                     : OrderedBuildMode::bulk_external_sort);
  plan.requires_external_sort =
      !(request.input_presorted && request.order_proof_valid);
  plan.validates_uniqueness =
      request.unique || request.family == IndexFamily::unique_btree;
  plan.publishes_new_root = false;
  plan.commit_atomic = false;
  plan.diagnostic = blocker;
  plan.steps.push_back("capture_build_snapshot");
  if (plan.requires_external_sort) {
    plan.steps.push_back("sort_key_stream_before_leaf_pack");
  } else {
    plan.steps.push_back("consume_sorted_key_stream");
  }
  if (plan.validates_uniqueness) {
    plan.steps.push_back("validate_unique_key_runs");
  }
  plan.steps.push_back("proof_only_no_physical_leaf_pack");
  plan.steps.push_back("physical_root_publish_blocked_until_IRC_040_041");
  return plan;
}

SortedBulkIndexBuildResult ProveUniqueInputAndVisibleKeys(
    const SortedBulkIndexBuildRequest& request,
    SortedBulkIndexBuildResult result) {
  AddEvidence(&result, "sorted_bulk_unique_proof_selected", "true");
  AddEvidence(&result, "sorted_bulk_unique_proof_order",
              "encoded_key,row_uuid,version_uuid");
  AddEvidence(&result, "sorted_bulk_unique_proof_null_policy",
              request.metadata.unique_nulls_distinct ? "nulls_distinct"
                                                     : "nulls_not_distinct");
  AddUniqueNonAuthorityEvidence(&result);

  std::vector<SortedBulkIndexEntry> effective_incoming;
  effective_incoming.reserve(result.entries.size());
  u64 nulls_distinct_skipped = 0;
  for (const auto& entry : result.entries) {
    if (!UniqueKeyParticipates(entry, request.metadata.unique_nulls_distinct)) {
      ++nulls_distinct_skipped;
      continue;
    }
    effective_incoming.push_back(entry);
  }

  result.unique_effective_key_count =
      static_cast<u64>(effective_incoming.size());
  AddEvidence(&result, "sorted_bulk_unique_proof_incoming_key_count",
              std::to_string(result.entries.size()));
  AddEvidence(&result, "sorted_bulk_unique_proof_effective_key_count",
              std::to_string(result.unique_effective_key_count));
  AddEvidence(&result, "sorted_bulk_unique_proof_null_keys_skipped",
              std::to_string(nulls_distinct_skipped));

  u64 sorted_run_count = 0;
  bool have_previous = false;
  std::string previous_key;
  for (const auto& entry : effective_incoming) {
    if (!have_previous || !KeyEqual(entry.encoded_key, previous_key)) {
      ++sorted_run_count;
      previous_key = entry.encoded_key;
      have_previous = true;
      continue;
    }
    result.uniqueness_refused = true;
    result.unique_duplicate_run_count = 1;
    AddEvidence(&result, "sorted_bulk_unique_proof_duplicate_run_count", "1");
    AddEvidence(&result, "sorted_bulk_unique_proof_duplicate_absent", "false");
    AddEvidence(&result, "sorted_bulk_unique_proof_conflict_key",
                entry.encoded_key);
    result.entries.clear();
    return RefuseWithEvidence(
        std::move(result),
        "SB-INDEX-SORTED-BULK-UNIQUE-DUPLICATE-BATCH",
        "index.sorted_bulk.unique_duplicate_batch",
        entry.encoded_key);
  }
  result.sorted_key_run_count = sorted_run_count;
  AddEvidence(&result, "sorted_bulk_unique_proof_sorted_key_run_count",
              std::to_string(sorted_run_count));
  AddEvidence(&result, "sorted_bulk_unique_proof_duplicate_run_count", "0");
  AddEvidence(&result, "sorted_bulk_unique_proof_duplicate_absent", "true");

  std::vector<SortedBulkIndexEntry> visible;
  visible.reserve(request.visible_unique_keys.size());
  for (const auto& row : request.visible_unique_keys) {
    if (row.encoded_key.empty() || row.row_uuid.empty() ||
        row.version_uuid.empty()) {
      result.invalid_descriptor_refused = true;
      result.entries.clear();
      return RefuseWithEvidence(
          std::move(result),
          "SB-INDEX-SORTED-BULK-UNIQUE-DESCRIPTOR-INVALID",
          "index.sorted_bulk.unique_descriptor_invalid",
          "visible persisted unique proof key is incomplete");
    }
    if (IsUnsafeLegacyIndexKeyEncoding(row.encoded_key)) {
      result.unsafe_key_refused = true;
      AddEvidence(&result, "sorted_bulk_unique_proof_unsafe_key_encoding",
                  "SBK1");
      result.entries.clear();
      return RefuseWithEvidence(
          std::move(result),
          "SB-INDEX-SORTED-BULK-UNSAFE-LEGACY-KEY",
          "index.sorted_bulk.unsafe_key_encoding",
          "SBK1");
    }
    SortedBulkIndexEntry entry;
    entry.encoded_key = row.encoded_key;
    entry.row_uuid = row.row_uuid;
    entry.version_uuid = row.version_uuid;
    entry.payload_value = row.payload_value;
    entry.source_ordinal = row.source_ordinal;
    entry.null_key = row.null_key;
    if (UniqueKeyParticipates(entry, request.metadata.unique_nulls_distinct)) {
      visible.push_back(std::move(entry));
    }
  }
  std::stable_sort(visible.begin(), visible.end(), [](const auto& left,
                                                      const auto& right) {
    return EntryLess(left, right);
  });

  result.unique_visible_key_count = static_cast<u64>(visible.size());
  AddEvidence(&result, "sorted_bulk_unique_proof_visible_key_count",
              std::to_string(result.unique_visible_key_count));

  std::size_t incoming_index = 0;
  std::size_t visible_index = 0;
  while (incoming_index < effective_incoming.size() &&
         visible_index < visible.size()) {
    const int compare = CompareEncodedSortKey(
        effective_incoming[incoming_index].encoded_key,
        visible[visible_index].encoded_key);
    if (compare == 0) {
      result.uniqueness_refused = true;
      result.unique_visible_conflict_refused = true;
      AddEvidence(&result, "sorted_bulk_unique_proof_persisted_conflict_absent",
                  "false");
      AddEvidence(&result, "sorted_bulk_unique_proof_persisted_conflict_key",
                  effective_incoming[incoming_index].encoded_key);
      result.entries.clear();
      return RefuseWithEvidence(
          std::move(result),
          "SB-INDEX-SORTED-BULK-UNIQUE-PERSISTED-CONFLICT",
          "index.sorted_bulk.unique_persisted_conflict",
          effective_incoming[incoming_index].encoded_key);
    }
    if (compare < 0) {
      ++incoming_index;
    } else {
      ++visible_index;
    }
  }
  AddEvidence(&result, "sorted_bulk_unique_proof_persisted_conflict_absent",
              "true");

  if (request.unique_reservation_ledger != nullptr ||
      request.validate_unique_reservation_batch) {
    result.unique_reservation_ledger_used = true;
    AddEvidence(&result, "sorted_bulk_unique_proof_reservation_ledger_used",
                "true");
    if (request.unique_reservation_ledger == nullptr ||
        !request.unique_constraint_uuid.valid()) {
      result.invalid_descriptor_refused = true;
      result.entries.clear();
      return RefuseWithEvidence(
          std::move(result),
          "SB-INDEX-SORTED-BULK-UNIQUE-DESCRIPTOR-INVALID",
          "index.sorted_bulk.unique_descriptor_invalid",
          "reservation-ledger unique proof requires a constraint UUID");
    }
    if (!request.transaction_uuid.valid() ||
        request.local_transaction_id == 0) {
      result.missing_mga_proof_refused = true;
      result.entries.clear();
      return RefuseWithEvidence(
          std::move(result),
          "SB-INDEX-SORTED-BULK-MGA-PROOF-REQUIRED",
          "index.sorted_bulk.mga_proof_required",
          "reservation-ledger unique proof requires transaction identity");
    }

    UniqueIndexReservationLedger candidate = *request.unique_reservation_ledger;
    for (const auto& entry : effective_incoming) {
      const auto row_uuid = ParseProofUuid(UuidKind::row, entry.row_uuid);
      const auto version_uuid =
          ParseProofUuid(UuidKind::row, entry.version_uuid);
      if (!row_uuid || !version_uuid) {
        result.invalid_descriptor_refused = true;
        result.entries.clear();
        return RefuseWithEvidence(
            std::move(result),
            "SB-INDEX-SORTED-BULK-UNIQUE-DESCRIPTOR-INVALID",
            "index.sorted_bulk.unique_descriptor_invalid",
            "reservation-ledger unique proof requires typed row/version UUIDs");
      }

      UniqueIndexReservationRequest reservation;
      reservation.index_uuid = request.metadata.index_uuid;
      reservation.table_uuid = request.metadata.table_uuid;
      reservation.constraint_uuid = request.unique_constraint_uuid;
      reservation.row_uuid = *row_uuid;
      reservation.version_uuid = *version_uuid;
      reservation.transaction_uuid = request.transaction_uuid;
      reservation.local_transaction_id = request.local_transaction_id;
      reservation.encoded_key = KeyBytes(entry.encoded_key);
      reservation.null_policy =
          request.metadata.unique_nulls_distinct
              ? UniqueIndexReservationNullPolicy::nulls_distinct
              : UniqueIndexReservationNullPolicy::nulls_not_distinct;
      reservation.null_policy_proven = true;
      reservation.incoming_key_has_null = entry.null_key;
      reservation.partial_predicate_participates = true;
      reservation.partial_predicate_proven = true;
      reservation.active_conflict_policy =
          UniqueIndexReservationActiveConflictPolicy::refuse_candidate;
      reservation.transaction_state_proofs =
          request.unique_transaction_state_proofs;

      const auto reserved = ReserveUniqueIndexKey(&candidate, reservation);
      AddReservationEvidence(reserved, &result);
      if (!reserved.ok() || !reserved.reserved) {
        result.uniqueness_refused = true;
        result.entries.clear();
        return RefuseWithEvidence(
            std::move(result),
            "SB-INDEX-SORTED-BULK-UNIQUE-RESERVATION-REFUSED",
            "index.sorted_bulk.unique_reservation_refused",
            reserved.diagnostic.diagnostic_code);
      }
    }

    if (request.validate_unique_reservation_batch) {
      UniqueIndexCommitValidationRequest validation;
      validation.transaction_uuid = request.transaction_uuid;
      validation.local_transaction_id = request.local_transaction_id;
      validation.transaction_state_proofs =
          request.unique_transaction_state_proofs;
      validation.validation_evidence_token =
          request.unique_reservation_validation_evidence_token;
      const auto validated = ValidateUniqueIndexCommitBatch(&candidate,
                                                            validation);
      AddReservationEvidence(validated, &result);
      if (!validated.ok()) {
        result.entries.clear();
        if (validated.diagnostic.diagnostic_code ==
            "INDEX.UNIQUE_RESERVATION.COMMIT_VALIDATION_MGA_PROOF_REQUIRED") {
          result.missing_mga_proof_refused = true;
          return RefuseWithEvidence(
              std::move(result),
              "SB-INDEX-SORTED-BULK-MGA-PROOF-REQUIRED",
              "index.sorted_bulk.mga_proof_required",
              validated.diagnostic.diagnostic_code);
        }
        result.uniqueness_refused = true;
        return RefuseWithEvidence(
            std::move(result),
            "SB-INDEX-SORTED-BULK-UNIQUE-RESERVATION-REFUSED",
            "index.sorted_bulk.unique_reservation_refused",
            validated.diagnostic.diagnostic_code);
      }
      result.unique_reservation_validation_passed = true;
      AddEvidence(&result,
                  "sorted_bulk_unique_proof_reservation_validation_passed",
                  "true");
      AddEvidence(&result,
                  "sorted_bulk_unique_proof_reservation_finality_granted",
                  "false");
      AddEvidence(&result,
                  "sorted_bulk_unique_proof_durable_commit_required_for_publish",
                  "true");
    }
    *request.unique_reservation_ledger = std::move(candidate);
  } else {
    AddEvidence(&result, "sorted_bulk_unique_proof_reservation_ledger_used",
                "false");
  }

  AddEvidence(&result, "sorted_bulk_unique_proof_result", "accepted");
  AddEvidence(&result, "sorted_bulk_unique_proof_unique_absence", "proven");
  result.uniqueness_proven = true;
  return result;
}

}  // namespace

SortedBulkIndexBuildResult BuildSortedExactBulkIndex(
    const SortedBulkIndexBuildRequest& request) {
  if (!request.metadata.index_uuid.valid() ||
      !request.metadata.table_uuid.valid()) {
    return Refuse("SB-INDEX-SORTED-BULK-IDENTITY-REQUIRED",
                  "index.sorted_bulk.identity_required");
  }
  if (!OrderedBuildFamily(request.metadata.family)) {
    return Refuse("SB-INDEX-SORTED-BULK-FAMILY-REFUSED",
                  "index.sorted_bulk.family_refused",
                  IndexFamilyName(request.metadata.family));
  }

  auto result = SortedBulkIndexBuildResult{};
  result.entries.reserve(request.rows.size());
  for (const auto& row : request.rows) {
    if (row.encoded_key.empty() || row.row_uuid.empty() ||
        row.version_uuid.empty()) {
      return Refuse("SB-INDEX-SORTED-BULK-ENTRY-INVALID",
                    "index.sorted_bulk.entry_invalid");
    }
    if (IsUnsafeLegacyIndexKeyEncoding(row.encoded_key)) {
      result.unsafe_key_refused = true;
      AddEvidence(&result, "sorted_bulk_unique_proof_unsafe_key_encoding",
                  "SBK1");
      AddUniqueNonAuthorityEvidence(&result);
      result.entries.clear();
      return RefuseWithEvidence(std::move(result),
                                "SB-INDEX-SORTED-BULK-UNSAFE-LEGACY-KEY",
                                "index.sorted_bulk.unsafe_key_encoding",
                                "SBK1");
    }
    SortedBulkIndexEntry entry;
    entry.encoded_key = row.encoded_key;
    entry.row_uuid = row.row_uuid;
    entry.version_uuid = row.version_uuid;
    entry.payload_value = row.payload_value;
    entry.source_ordinal = row.source_ordinal;
    entry.null_key = row.null_key;
    result.entries.push_back(std::move(entry));
  }

  std::stable_sort(result.entries.begin(),
                   result.entries.end(),
                   [](const auto& left, const auto& right) {
                     return EntryLess(left, right);
                   });

  if (request.metadata.input_presorted &&
      request.metadata.order_proof_valid &&
      !InputOrderMatchesSortedOrder(request.rows, result.entries)) {
    result.entries.clear();
    return RefuseWithEvidence(
        std::move(result),
        "SB-INDEX-SORTED-BULK-ORDER-PROOF-INVALID",
        "index.sorted_bulk.order_proof_invalid");
  }

  u64 current_run = 0;
  std::string previous_key;
  bool have_previous_key = false;
  const u32 leaf_capacity =
      request.metadata.leaf_entry_capacity == 0
          ? 128
          : request.metadata.leaf_entry_capacity;
  for (std::size_t i = 0; i < result.entries.size(); ++i) {
    auto& entry = result.entries[i];
    if (!have_previous_key || !KeyEqual(entry.encoded_key, previous_key)) {
      ++current_run;
      previous_key = entry.encoded_key;
      have_previous_key = true;
    }
    entry.sorted_ordinal = static_cast<u64>(i);
    entry.key_run_ordinal = current_run == 0 ? 0 : current_run - 1;
    entry.leaf_ordinal = static_cast<u64>(i / leaf_capacity);
  }
  result.sorted_key_run_count = current_run;

  const bool unique_build = UniqueBuildRequested(request.metadata);
  if (unique_build) {
    result = ProveUniqueInputAndVisibleKeys(request, std::move(result));
    if (!result.status.ok()) {
      return result;
    }
  } else {
    AddEvidence(&result, "sorted_bulk_unique_proof_selected", "false");
  }

  OrderedBuildRequest plan_request;
  plan_request.index_uuid = request.metadata.index_uuid;
  plan_request.family = unique_build
                            ? IndexFamily::unique_btree
                            : request.metadata.family;
  plan_request.tuple_count_estimate = static_cast<u64>(request.rows.size());
  const bool budget_missing = request.metadata.page_budget == 0 &&
                              request.metadata.byte_budget == 0 &&
                              request.metadata.time_budget_microseconds == 0;
  plan_request.page_budget = budget_missing ? 1 : request.metadata.page_budget;
  plan_request.byte_budget = request.metadata.byte_budget;
  plan_request.time_budget_microseconds =
      request.metadata.time_budget_microseconds;
  plan_request.rebuild = request.metadata.rebuild;
  plan_request.input_presorted = request.metadata.input_presorted;
  plan_request.order_proof_valid = request.metadata.order_proof_valid;
  plan_request.allow_external_sort = true;
  plan_request.online = request.metadata.online;
  plan_request.unique = unique_build;
  plan_request.policy_allows_mutation =
      request.metadata.policy_allows_mutation;
  plan_request.read_only_database = request.metadata.read_only_database;

  result.ordered_plan = PlanOrderedBulkBuild(plan_request);
  if (!result.ordered_plan.ok()) {
    if (CapabilityBlockerDiagnostic(result.ordered_plan.diagnostic)) {
      AddEvidence(&result, "sorted_bulk_index_physical_capability_blocked",
                  result.ordered_plan.diagnostic.diagnostic_code);
      AddEvidence(&result, "sorted_bulk_index_root_publish_blocked",
                  "IRC-040_041_required");
      result.ordered_plan =
          ProofOnlyOrderedPlan(plan_request, result.ordered_plan.diagnostic);
    } else {
      result.status = result.ordered_plan.status;
      result.diagnostic = result.ordered_plan.diagnostic;
      return result;
    }
  }

  result = BuildCandidateRootGeneration(request, std::move(result));
  if (!result.status.ok() && !result.accepted) {
    return result;
  }

  result.status = OkStatus();
  result.accepted = true;
  result.bottom_up_build_selected = true;
  result.physical_append_authorized_by_proof = false;
  result.root_publish_authorized_by_proof = false;
  if (result.leaf_generation_count == 0) {
    result.leaf_generation_count =
        result.entries.empty()
            ? 1
            : ((static_cast<u64>(result.entries.size()) +
                static_cast<u64>(leaf_capacity) - 1) /
               static_cast<u64>(leaf_capacity));
  }
  result.root_generation_count = 1;
  result.root_publish_fence =
      "mga_index_append_path_after_bottom_up_root_generation";
  result.diagnostic = Diagnostic(result.status,
                                 "SB-INDEX-SORTED-BULK-OK",
                                 "index.sorted_bulk.ok");

  result.evidence.push_back({"sorted_bulk_index_build_selected", "true"});
  result.evidence.push_back({"sorted_bulk_index_build_mode", "bottom_up_exact"});
  result.evidence.push_back({"sorted_bulk_index_order",
                             "encoded_key,row_uuid,version_uuid"});
  result.evidence.push_back({"sorted_bulk_index_entry_count",
                             std::to_string(result.entries.size())});
  result.evidence.push_back({"sorted_bulk_index_key_run_count",
                             std::to_string(result.sorted_key_run_count)});
  result.evidence.push_back({"sorted_bulk_index_leaf_generation_count",
                             std::to_string(result.leaf_generation_count)});
  result.evidence.push_back({"sorted_bulk_index_root_generation_count",
                             std::to_string(result.root_generation_count)});
  result.evidence.push_back({"sorted_bulk_index_root_publish_fence",
                             result.root_publish_fence});
  result.evidence.push_back({"sorted_bulk_index_uniqueness_proof",
                             unique_build
                                 ? "sorted_duplicate_runs_absent"
                                 : "not_required"});
  result.evidence.push_back({"sorted_bulk_index_unique_visible_conflict_absent",
                             unique_build ? "true" : "not_required"});
  result.evidence.push_back({"sorted_bulk_index_unique_duplicate_runs_absent",
                             unique_build ? "true" : "not_required"});
  result.evidence.push_back({"sorted_bulk_index_physical_append_authorized_by_proof",
                             "false"});
  result.evidence.push_back({"sorted_bulk_index_root_publish_authorized_by_proof",
                             "false"});
  result.evidence.push_back({"sorted_bulk_index_retail_append_bypass",
                             "bottom_up_build_selected"});
  result.evidence.push_back({"mga_finality_authority",
                             "engine_transaction_inventory"});
  for (const auto& step : result.ordered_plan.steps) {
    result.evidence.push_back({"sorted_bulk_index_ordered_plan_step", step});
  }
  return result;
}

}  // namespace scratchbird::core::index
