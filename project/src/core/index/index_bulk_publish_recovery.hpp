// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-INDEX-BULK-PUBLISH-RECOVERY-ANCHOR

#include "index_metapage.hpp"
#include "sorted_bulk_index_build.hpp"

#include <optional>
#include <string>
#include <vector>

namespace scratchbird::core::index {

enum class IndexBulkPublishCrashPoint {
  crash_before_root_publish,
  crash_during_root_publish,
  crash_after_root_publish
};

enum class IndexBulkPublishActiveRoot {
  none,
  old_root,
  new_root
};

struct IndexBulkPublishRecoveryEvidence {
  std::string evidence_kind;
  std::string evidence_id;
};

struct IndexBulkPublishRecoveryState {
  bool old_metapage_present = false;
  IndexMetapageControl old_metapage;
  bool old_tree_image_present = false;
  scratchbird::storage::page::IndexBtreePhysicalTreeImage old_tree_image;
  std::optional<SortedBulkIndexCandidateRootGeneration> candidate_generation;
  std::optional<scratchbird::storage::page::IndexBtreePhysicalTreeImage>
      candidate_tree_image;
  std::optional<std::vector<byte>> durable_metapage_image;
  IndexBulkPublishCrashPoint crash_point =
      IndexBulkPublishCrashPoint::crash_before_root_publish;
  bool candidate_tree_validation_proof = false;
  bool durable_metadata_write_evidence = false;
  bool root_publish_authorization_proof = false;
  bool mga_finality_authority_evidence = false;
  std::string durable_metadata_evidence_token;
  std::string root_publish_authorization_token;
  std::string mga_authority_evidence_token;
  std::string publish_fence_token;
};

struct IndexBulkPublishRecoveryResult {
  Status status;
  DiagnosticRecord diagnostic;
  IndexBulkPublishActiveRoot active_root = IndexBulkPublishActiveRoot::none;
  IndexMetapageControl active_metapage;
  scratchbird::storage::page::IndexBtreePhysicalTreeImage active_tree_image;
  scratchbird::storage::page::IndexBtreePhysicalTreeImage repaired_tree_image;
  std::vector<IndexBulkPublishRecoveryEvidence> evidence;
  std::string crash_classification;
  std::string repair_classification;
  bool recovered = false;
  bool fail_closed = false;
  bool old_or_new_validated_root_only = false;
  bool half_root_exposed = false;
  bool orphan_generation_classified = false;
  bool root_publish_authorized = false;
  bool transaction_finality_authority = false;
  bool runtime_route_capability = false;
  bool benchmark_clean = false;

  bool ok() const { return status.ok() && recovered && !half_root_exposed; }
};

IndexBulkPublishRecoveryResult RecoverSortedBulkRootPublish(
    const IndexBulkPublishRecoveryState& state);

}  // namespace scratchbird::core::index
