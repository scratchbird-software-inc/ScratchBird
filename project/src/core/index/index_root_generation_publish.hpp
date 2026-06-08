// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-INDEX-ROOT-GENERATION-PUBLISH-ANCHOR

#include "index_metapage.hpp"
#include "sorted_bulk_index_build.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::index {

struct IndexRootGenerationPublishEvidence {
  std::string evidence_kind;
  std::string evidence_id;
};

struct IndexRootGenerationPublishRequest {
  IndexMetapageControl current_metapage;
  SortedBulkIndexCandidateRootGeneration candidate;
  bool candidate_tree_validation_proof = false;
  bool durable_metadata_write_evidence = false;
  bool mga_finality_authority_evidence = false;
  std::string durable_metadata_evidence_token;
  std::string mga_authority_evidence_token;
  std::string publish_fence_token;
  std::vector<byte> durable_metapage_image;
};

struct IndexRootGenerationPublishResult {
  Status status;
  DiagnosticRecord diagnostic;
  IndexMetapageControl old_metapage;
  IndexMetapageControl published_metapage;
  IndexMetapageControl reopened_metapage;
  scratchbird::storage::page::IndexBtreePhysicalTreeImage published_tree_image;
  std::vector<byte> published_metapage_image;
  std::vector<IndexRootGenerationPublishEvidence> evidence;
  bool published = false;
  bool root_publish_authorized = false;
  bool physical_append_authorized = false;
  bool transaction_finality_authority = false;
  bool recovery_authority = false;
  bool runtime_route_capability = false;
  bool rollback_safe_metadata_contract = false;
  bool reopen_safe_metadata_contract = false;
  bool old_root_metadata_preserved = false;

  bool ok() const {
    return status.ok() && published && root_publish_authorized;
  }
};

IndexRootGenerationPublishResult PublishIndexRootGeneration(
    const IndexRootGenerationPublishRequest& request);

}  // namespace scratchbird::core::index
