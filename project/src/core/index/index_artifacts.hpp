// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-INDEX-ARTIFACTS-CLOSURE-ANCHOR

#include "index_backup_restore.hpp"
#include "index_localization.hpp"
#include "index_metapage.hpp"
#include "index_resource_boundary.hpp"

namespace scratchbird::core::index {

enum class IndexArtifactOperation : u32 {
  export_definition = 1,
  import_definition = 2,
  donor_import = 3,
  seed_bind = 4,
  version_compare = 5
};

struct IndexArtifactRequest {
  IndexArtifactOperation operation = IndexArtifactOperation::export_definition;
  TypedUuid index_uuid;
  IndexFamily family = IndexFamily::unknown;
  std::string semantic_profile_id = "sb_native_default";
  std::string donor_name;
  bool preserve_uuid = true;
  bool policy_allows_import = false;
  bool resource_epoch_current = true;
  bool finality_proven = true;
  bool durable_metadata_present = false;
  IndexMetapageControl durable_metadata;
};

struct IndexArtifactDecision {
  Status status;
  bool allowed = false;
  bool requires_rebuild = false;
  bool requires_verify = true;
  bool emulated = false;
  bool policy_blocked = false;
  bool durable_metadata_valid = false;
  bool checksum_profile_bound = false;
  bool format_compatible = false;
  bool identity_bound = false;
  bool descriptor_hash_bound = false;
  bool route_capability_hash_bound = false;
  bool provider_evidence_hash_bound = false;
  bool family_validator_passed = false;
  std::string canonical_artifact_class;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && allowed && !policy_blocked; }
};

IndexArtifactDecision PlanIndexArtifactOperation(const IndexArtifactRequest& request);
DiagnosticRecord MakeIndexArtifactDiagnostic(Status status,
                                             std::string diagnostic_code,
                                             std::string message_key,
                                             std::string detail = {});

}  // namespace scratchbird::core::index
