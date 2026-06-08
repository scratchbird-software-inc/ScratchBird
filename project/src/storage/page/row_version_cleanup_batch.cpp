// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "row_version_cleanup_batch.hpp"

#include <algorithm>

namespace scratchbird::storage::page {

// DPC_STORAGE_VERSION_CLEANUP_AGENT

namespace mga = scratchbird::transaction::mga;

bool StorageRowVersionIsCurrentVisibleVersion(const RowVersionMetadata& metadata) {
  return metadata.state == mga::RowVersionState::committed &&
         metadata.creator_transaction_state == mga::TransactionState::committed &&
         !metadata.chain.has_next();
}

bool StorageRowVersionHasCleanupShape(const RowVersionMetadata& metadata) {
  if (metadata.state == mga::RowVersionState::rolled_back ||
      metadata.state == mga::RowVersionState::delete_marker) {
    return true;
  }
  return metadata.state == mga::RowVersionState::committed &&
         metadata.chain.has_next();
}

bool StorageRowVersionBelowCleanupHorizon(
    const RowVersionMetadata& metadata,
    const AuthoritativeCleanupHorizonResult& horizon) {
  return horizon.cleanup_horizon.valid() &&
         metadata.identity.creator_transaction.local_id.valid() &&
         metadata.identity.creator_transaction.local_id.value <
             horizon.cleanup_horizon.value;
}

StorageRowVersionCleanupCandidateBatch BuildStorageRowVersionCleanupCandidateBatch(
    const std::vector<RowVersionMetadata>& row_versions,
    const AuthoritativeCleanupHorizonResult& horizon,
    u64 max_candidate_row_versions) {
  StorageRowVersionCleanupCandidateBatch result;
  result.metrics.total_row_versions = static_cast<u64>(row_versions.size());
  if (max_candidate_row_versions != 0) {
    result.candidates.reserve(static_cast<std::size_t>(std::min<u64>(
        max_candidate_row_versions,
        static_cast<u64>(row_versions.size()))));
  }
  for (const RowVersionMetadata& row_version : row_versions) {
    if (StorageRowVersionIsCurrentVisibleVersion(row_version)) {
      ++result.metrics.current_visible_row_versions;
    }
    if (!StorageRowVersionHasCleanupShape(row_version) ||
        !StorageRowVersionBelowCleanupHorizon(row_version, horizon)) {
      continue;
    }
    ++result.metrics.cleanup_candidate_row_versions;
    if (max_candidate_row_versions != 0 &&
        static_cast<u64>(result.candidates.size()) < max_candidate_row_versions) {
      result.candidates.push_back(row_version);
    }
  }
  result.budget_exhausted =
      max_candidate_row_versions != 0 &&
      result.metrics.cleanup_candidate_row_versions > max_candidate_row_versions;
  return result;
}

}  // namespace scratchbird::storage::page
