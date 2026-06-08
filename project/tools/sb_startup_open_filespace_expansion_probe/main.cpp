// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SB-STARTUP-OPEN-FILESPACE-EXPANSION-PROBE-ANCHOR
#include "filespace_identity.hpp"
#include "filespace_secondary.hpp"
#include "page_filespace_handoff.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::uuid::GenerateEngineIdentityV7;
using scratchbird::storage::filespace::AddFilespaceDirectoryEntry;
using scratchbird::storage::filespace::AllocateNextSecondaryPhysicalFilespaceId;
using scratchbird::storage::filespace::EvaluateSecondaryFilespaceLifecycle;
using scratchbird::storage::filespace::FilespaceDirectory;
using scratchbird::storage::filespace::FilespaceDirectoryEntry;
using scratchbird::storage::filespace::FilespaceLifecycleBlocker;
using scratchbird::storage::filespace::FilespaceLifecycleBlockerKind;
using scratchbird::storage::filespace::FilespaceOpenSafetyMode;
using scratchbird::storage::filespace::FilespacePageIdToString;
using scratchbird::storage::filespace::InitializeSecondaryFilespaceState;
using scratchbird::storage::filespace::MakeFilespacePageId;
using scratchbird::storage::filespace::MakePrimaryFilespacePageId;
using scratchbird::storage::filespace::PhysicalFilespaceHeader;
using scratchbird::storage::filespace::PlanPrimaryShadowPromotion;
using scratchbird::storage::filespace::PlanSecondaryFilespaceMove;
using scratchbird::storage::filespace::PreallocateSecondaryFilespacePages;
using scratchbird::storage::filespace::SecondaryFilespacePolicy;
using scratchbird::storage::filespace::ValidateStartupFilespaceDirectory;
using scratchbird::storage::filespace::kActivePrimaryPhysicalFilespaceId;
using scratchbird::storage::page::AgentFilespaceLowWaterPages;
using scratchbird::storage::page::EvaluatePageFilespaceAgentRequest;
using scratchbird::storage::page::PageFilespaceAgentRequest;
using scratchbird::storage::page::PageFilespaceAgentRequestKind;
using scratchbird::storage::page::PageFilespaceAgentRequestPolicy;
using scratchbird::storage::page::PageFilespaceAgentRequestState;

namespace {

TypedUuid Id(UuidKind kind, scratchbird::core::platform::u64 salt) {
  const auto generated = GenerateEngineIdentityV7(kind, salt);
  return generated.ok() ? generated.value : TypedUuid{};
}

void Check(bool condition, const std::string& label) {
  if (!condition) {
    std::cerr << "FAIL " << label << '\n';
    std::exit(1);
  }
  std::cout << "PASS " << label << '\n';
}

}  // namespace

int main() {
  const auto database_uuid = Id(UuidKind::database, 10);
  const auto primary_filespace_uuid = Id(UuidKind::filespace, 11);
  const auto secondary_filespace_uuid = Id(UuidKind::filespace, 12);
  const auto policy_uuid = Id(UuidKind::object, 13);

  const auto primary_page_id = MakePrimaryFilespacePageId(42);
  Check(primary_page_id.ok() && primary_page_id.page_id.physical_filespace_id == kActivePrimaryPhysicalFilespaceId,
        "SOF-014 primary physical filespace id zero");
  Check(FilespacePageIdToString(primary_page_id.page_id) == "0:42",
        "SOF-014 filespace page id string form");
  Check(!MakeFilespacePageId(1, 7).ok(),
        "SOF-014 reserved physical filespace id rejected");

  FilespaceDirectory directory;
  FilespaceDirectoryEntry primary;
  primary.database_uuid = database_uuid;
  primary.filespace_uuid = primary_filespace_uuid;
  primary.physical_filespace_id = 0;
  primary.role = scratchbird::storage::filespace::FilespaceRole::active_primary;
  primary.state = scratchbird::storage::filespace::FilespaceState::online;
  primary.path = "primary.sbfs";
  primary.page_size = 16384;
  primary.active = true;
  Check(AddFilespaceDirectoryEntry(&directory, primary).ok(),
        "SOF-020 startup directory accepts active primary");
  Check(ValidateStartupFilespaceDirectory(directory).ok(),
        "SOF-020 startup directory validates active primary");
  const auto next_secondary = AllocateNextSecondaryPhysicalFilespaceId(directory);
  Check(next_secondary.ok() && next_secondary.physical_filespace_id == 2,
        "SOF-020 secondary physical id allocation starts at two");

  PhysicalFilespaceHeader header;
  header.database_uuid = database_uuid;
  header.filespace_uuid = secondary_filespace_uuid;
  header.role = scratchbird::storage::filespace::FilespaceRole::primary_candidate;
  header.state = scratchbird::storage::filespace::FilespaceState::online;
  const auto secondary = InitializeSecondaryFilespaceState(header, 2, "secondary.sbfs", 64, 6);
  Check(secondary.ok() && secondary.state.promotion_candidate,
        "SOF-015 secondary filespace state initializes with promotion candidate");

  SecondaryFilespacePolicy filespace_policy;
  filespace_policy.policy_uuid = policy_uuid;
  auto preallocated = PreallocateSecondaryFilespacePages(secondary.state, filespace_policy, 4);
  Check(preallocated.ok() && preallocated.state.preallocated_pages == 4,
        "SOF-016 secondary preallocation consumes free pages");

  auto normal_gate = EvaluateSecondaryFilespaceLifecycle(preallocated.state,
                                                        FilespaceOpenSafetyMode::normal,
                                                        {});
  Check(normal_gate.ok() && normal_gate.gate.can_accept_writes,
        "SOF-023 normal open allows writes without blockers");
  FilespaceLifecycleBlocker page_blocker;
  page_blocker.kind = FilespaceLifecycleBlockerKind::page_allocation;
  page_blocker.owner_subsystem = "page_allocation_manager";
  page_blocker.reason = "relocation pending";
  auto blocked_gate = EvaluateSecondaryFilespaceLifecycle(preallocated.state,
                                                         FilespaceOpenSafetyMode::normal,
                                                         {page_blocker});
  Check(blocked_gate.ok() && !blocked_gate.gate.can_accept_writes && !blocked_gate.gate.can_promote_to_primary,
        "SOF-024 blockers fence writes and promotion");

  scratchbird::storage::filespace::FilespaceMovePlan move_plan;
  move_plan.database_uuid = database_uuid;
  move_plan.filespace_uuid = secondary_filespace_uuid;
  move_plan.physical_filespace_id = 2;
  move_plan.source_path = "secondary.sbfs";
  move_plan.target_path = "secondary-new.sbfs";
  move_plan.operator_approved = true;
  move_plan.page_agent_relocation_complete = true;
  move_plan.startup_open_safe = true;
  Check(PlanSecondaryFilespaceMove(move_plan, {}).ok(),
        "SOF-025 secondary filespace move requires explicit approvals");
  auto current_primary = preallocated.state;
  current_primary.path = "primary.sbfs";
  current_primary.allocated_pages = 1;
  Check(PlanPrimaryShadowPromotion(current_primary, preallocated.state, {}).ok(),
        "SOF-026 primary shadow promotion is physically gated");

  PageFilespaceAgentRequestPolicy handoff_policy;
  handoff_policy.policy_uuid = policy_uuid;
  Check(AgentFilespaceLowWaterPages(handoff_policy) == 4,
        "SOF-027 low-water default remains half of eight pages");
  PageFilespaceAgentRequest reserve_request;
  reserve_request.request_uuid = Id(UuidKind::object, 14);
  reserve_request.database_uuid = database_uuid;
  reserve_request.filespace_uuid = secondary_filespace_uuid;
  reserve_request.policy_uuid = policy_uuid;
  reserve_request.kind = PageFilespaceAgentRequestKind::reserve_pages;
  reserve_request.requesting_agent = "filespace_capacity_manager";
  reserve_request.responding_agent = "page_allocation_manager";
  reserve_request.page_family = "row_data";
  reserve_request.requested_pages = 4;
  const auto reserve_decision = EvaluatePageFilespaceAgentRequest(reserve_request, handoff_policy);
  Check(reserve_decision.ok() &&
            reserve_decision.state == PageFilespaceAgentRequestState::waiting_page_agent,
        "SOF-029 page agent owns page reservation requests");

  PageFilespaceAgentRequest bad_boundary = reserve_request;
  bad_boundary.responding_agent = "filespace_capacity_manager";
  Check(!EvaluatePageFilespaceAgentRequest(bad_boundary, handoff_policy).ok(),
        "SOF-030 filespace agent cannot manage pages");

  PageFilespaceAgentRequest extend_request = reserve_request;
  extend_request.kind = PageFilespaceAgentRequestKind::extend_filespace;
  extend_request.requesting_agent = "page_allocation_manager";
  extend_request.responding_agent = "filespace_capacity_manager";
  Check(EvaluatePageFilespaceAgentRequest(extend_request, handoff_policy).ok(),
        "SOF-031 filespace agent owns file extension requests");

  return 0;
}
