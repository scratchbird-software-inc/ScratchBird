// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-INDEX-API-SBLR-CLOSURE-ANCHOR
// DPC_INDEX_VALIDATION_REPAIR_TOOLING

#include "index_management.hpp"
#include "index_optimizer_integration.hpp"

namespace scratchbird::core::index {

enum class IndexCanonicalOperation : u32 {
  create = 1,
  alter = 2,
  drop = 3,
  rebuild = 4,
  verify = 5,
  rebalance = 6,
  refresh = 7,
  move = 8,
  query_candidates = 9,
  explain = 10,
  reference_catalog_projection = 11,
  unsupported_reference_feature = 12,
  validate_index_family = 13,
  repair_index_family = 14,
  rebuild_index_family = 15,
  discard_unpublished_index_state = 16
};

struct IndexSblrOperationEnvelope {
  u32 envelope_version = 1;
  IndexCanonicalOperation operation = IndexCanonicalOperation::query_candidates;
  TypedUuid index_uuid;
  IndexFamily family = IndexFamily::unknown;
  std::string semantic_profile_id = "sb_native_default";
  bool names_resolved_to_uuids = true;
  bool contains_sql_text = false;
  bool parser_surface_is_reference = false;
  IndexValidationRepairFamily validation_family =
      IndexValidationRepairFamily::ordered_table_candidate_set;
  std::string reference_name;
  std::string reference_command;
};

struct IndexApiPlan {
  Status status;
  bool admitted = false;
  bool mutates_state = false;
  bool parser_shaping_required = true;
  bool reference_catalog_projection = false;
  bool emulated = false;
  bool policy_blocked = false;
  std::vector<std::string> steps;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && admitted && !policy_blocked; }
};

IndexApiPlan BindIndexSblrOperation(const IndexSblrOperationEnvelope& envelope);
IndexValidationRepairResult BindIndexValidationRepairSblrOperation(
    const IndexSblrOperationEnvelope& envelope,
    IndexValidationRepairRequest request);
IndexApiPlan MapReferenceIndexCommandToCanonicalOperation(const IndexSblrOperationEnvelope& envelope);
DiagnosticRecord MakeIndexApiSblrDiagnostic(Status status,
                                            std::string diagnostic_code,
                                            std::string message_key,
                                            std::string detail = {});

}  // namespace scratchbird::core::index
