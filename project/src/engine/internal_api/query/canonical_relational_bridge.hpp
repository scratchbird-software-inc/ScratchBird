// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "query/plan_api.hpp"
#include "logical_plan.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

struct CanonicalRelationalPlanningScope {
  std::string catalog_epoch_uuid;
  std::string security_context_uuid;
  std::string statement_uuid;
  std::string owning_transaction_uuid;
  std::string statement_snapshot_uuid;
  std::string statement_metadata_snapshot_uuid;
  std::uint64_t local_transaction_id{0};
  std::uint64_t snapshot_visible_through_local_transaction_id{0};
  bool metadata_snapshot_engine_owned{false};
  bool authorization_context_engine_owned{false};
};

struct CanonicalRelationalBridgeIssue {
  std::string diagnostic_id;
  std::uint32_t logical_node_id{0};
  std::string field_id;
};

struct CanonicalRelationalBridgeResult {
  bool accepted{false};
  bool data_access_allowed{false};
  std::string catalog_epoch_uuid;
  std::string statement_uuid;
  std::string owning_transaction_uuid;
  std::string statement_snapshot_uuid;
  std::string statement_metadata_snapshot_uuid;
  std::uint64_t local_transaction_id{0};
  std::uint64_t snapshot_visible_through_local_transaction_id{0};
  scratchbird::engine::planner::CanonicalLogicalRelationalGraph logical_graph;
  scratchbird::engine::planner::CanonicalLogicalPropertyCatalog
      property_catalog;
  std::vector<CanonicalRelationalBridgeIssue> issues;
};

// QOW-ROUTE-STAGE-302-PROPERTY-BRIDGE-V1
CanonicalRelationalBridgeResult
PopulateCanonicalLogicalGraphFromAdmittedTypedRelationalDag(
    const TypedRelationalDag& dag,
    const CanonicalRelationalPlanningScope& engine_scope);

}  // namespace scratchbird::engine::internal_api
