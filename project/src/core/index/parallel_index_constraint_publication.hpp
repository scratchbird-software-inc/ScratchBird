// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "index_bulk_publish_recovery.hpp"
#include "index_route_capability.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::core::index {

struct ParallelIndexConstraintRow {
  std::string encoded_key;
  std::string row_uuid;
  std::string version_uuid;
  std::string parent_key;
  std::string payload_value;
  std::int64_t check_value = 0;
};

struct ParallelIndexConstraintAuthorityProof {
  bool engine_mga_snapshot_bound = false;
  bool transaction_inventory_authoritative = false;
  bool security_context_bound = false;
  bool parser_client_or_reference_index_authority = false;
  bool index_metadata_visibility_or_finality_authority = false;
  bool index_metadata_recovery_authority = false;
  bool recovery_from_index_metadata_alone = false;
};

struct ParallelIndexConstraintPublicationRequest {
  std::string route_label = "dml_deferred_bulk_index_publish";
  IndexRouteKind route = IndexRouteKind::dml_insert;
  IndexFamily family = IndexFamily::unique_btree;
  bool unique = true;
  bool validate_foreign_keys = true;
  bool validate_checks = true;
  bool runtime_consumed = true;
  bool exact_fallback_available = true;
  bool route_capability_fresh = true;
  bool generation_validation_proof = true;
  bool publish_fence_available = true;
  bool worker_result_match = true;
  bool contract_only_evidence = false;
  u64 expected_route_capability_generation = 7;
  u64 observed_route_capability_generation = 7;
  u32 worker_count = 3;
  TypedUuid index_uuid;
  TypedUuid table_uuid;
  std::vector<ParallelIndexConstraintRow> rows;
  std::vector<std::string> parent_keys;
  std::vector<SortedBulkIndexRowInput> visible_unique_keys;
  ParallelIndexConstraintAuthorityProof authority;
  std::string publish_fence_token = "engine-owned-publish-fence";
  std::string mga_authority_token = "durable-mga-transaction-inventory";
  std::string security_epoch_token = "engine-security-epoch";
};

struct ParallelIndexConstraintEvidence {
  std::string evidence_kind;
  std::string evidence_id;
};

struct ParallelIndexConstraintPublicationResult {
  Status status;
  DiagnosticRecord diagnostic;
  bool accepted = false;
  bool benchmark_clean = false;
  bool fail_closed = false;
  bool fallback_available = false;
  bool runtime_consumed = false;
  bool route_capability_consumed = false;
  bool parallel_sorted_build_consumed = false;
  bool parallel_uniqueness_validated = false;
  bool parallel_foreign_key_validated = false;
  bool parallel_check_validated = false;
  bool publish_fence_validated = false;
  bool exact_publication = false;
  bool worker_evidence_consumed = false;
  bool worker_result_match = false;
  bool mga_visibility_recheck_proven = false;
  bool security_recheck_proven = false;
  bool parser_client_reference_authority = false;
  bool index_metadata_finality_authority = false;
  bool index_metadata_recovery_authority = false;
  bool recovery_from_index_metadata_alone = false;
  bool validated_generation_published = false;
  bool recovery_reopen_proven = false;
  bool half_root_exposed = false;
  u64 worker_count = 0;
  u64 candidate_generation = 0;
  u64 active_root_generation = 0;
  u64 row_count = 0;
  std::string route_label;
  std::string state_hash;
  SortedBulkIndexBuildResult sorted_build;
  IndexBulkPublishRecoveryResult recovery;
  std::vector<ParallelIndexConstraintEvidence> evidence;

  bool ok() const { return status.ok() && accepted && benchmark_clean; }
};

ParallelIndexConstraintPublicationResult
PublishParallelValidatedDeferredIndexGeneration(
    const ParallelIndexConstraintPublicationRequest& request);

}  // namespace scratchbird::core::index
