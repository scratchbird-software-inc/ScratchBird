// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-INDEX-MANAGEMENT-CLOSURE-ANCHOR
// DPC_INDEX_VALIDATION_REPAIR_TOOLING

#include "index_artifacts.hpp"
#include "index_family_management_surface.hpp"
#include "index_maintenance.hpp"
#include "index_ownership.hpp"
#include "index_validation_repair_tooling.hpp"

namespace scratchbird::core::index {

enum class IndexManagementOperation : u32 {
  create = 1,
  alter = 2,
  drop = 3,
  rebuild = 4,
  verify = 5,
  rebalance = 6,
  refresh = 7,
  move = 8,
  inspect = 9,
  support_bundle = 10,
  repair = 11,
  discard_unpublished = 12
};

struct IndexManagementRequest {
  IndexManagementOperation operation = IndexManagementOperation::inspect;
  TypedUuid index_uuid;
  IndexFamily family = IndexFamily::unknown;
  IndexSubsystemOwner caller = IndexSubsystemOwner::management_api;
  bool policy_allows_mutation = false;
  bool read_only_database = false;
  bool allow_sensitive_support_data = false;
};

struct IndexManagementPlan {
  Status status;
  bool admitted = false;
  bool mutating = false;
  bool redaction_required = true;
  std::vector<std::string> steps;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && admitted; }
};

IndexManagementPlan PlanIndexManagementOperation(const IndexManagementRequest& request);
IndexFamilyManagementSurface BuildIndexFamilyManagementCatalogView(
    const IndexFamilyManagementSurfaceRequest& request = {});
IndexFamilyManagementSurface BuildIndexFamilyManagementSupportBundle(
    const IndexFamilyManagementSurfaceRequest& request = {});
IndexValidationRepairResult PlanIndexManagementValidationRepairOperation(
    IndexValidationRepairRequest request,
    IndexSubsystemOwner caller = IndexSubsystemOwner::management_api);
DiagnosticRecord MakeIndexManagementDiagnostic(Status status,
                                               std::string diagnostic_code,
                                               std::string message_key,
                                               std::string detail = {});

}  // namespace scratchbird::core::index
