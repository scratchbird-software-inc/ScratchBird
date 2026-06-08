// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SB-FILESPACE-SECONDARY-CLOSURE-ANCHOR
#include "filespace_secondary.hpp"

#include <algorithm>
#include <utility>

namespace scratchbird::storage::filespace {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status SecondaryOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::storage_disk};
}

Status SecondaryErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::storage_disk};
}

SecondaryFilespaceResult SecondaryError(std::string code, std::string key, std::string detail = {}) {
  SecondaryFilespaceResult result;
  result.status = SecondaryErrorStatus();
  result.diagnostic = MakeSecondaryFilespaceDiagnostic(result.status, std::move(code), std::move(key), std::move(detail));
  return result;
}

FilespaceMoveResult MoveError(FilespaceMovePlan plan,
                              std::vector<FilespaceLifecycleBlocker> blockers,
                              std::string code,
                              std::string key,
                              std::string detail = {}) {
  FilespaceMoveResult result;
  result.status = SecondaryErrorStatus();
  result.decision = FilespaceMoveDecision::blocked;
  result.plan = std::move(plan);
  result.blockers = std::move(blockers);
  result.diagnostic = MakeSecondaryFilespaceDiagnostic(result.status, std::move(code), std::move(key), std::move(detail));
  return result;
}

bool HasBlockers(const std::vector<FilespaceLifecycleBlocker>& blockers) {
  return std::any_of(blockers.begin(), blockers.end(), [](const auto& blocker) {
    return blocker.kind != FilespaceLifecycleBlockerKind::none;
  });
}

}  // namespace

const char* FilespaceOpenSafetyModeName(FilespaceOpenSafetyMode mode) {
  switch (mode) {
    case FilespaceOpenSafetyMode::normal:
      return "normal";
    case FilespaceOpenSafetyMode::read_only:
      return "read_only";
    case FilespaceOpenSafetyMode::maintenance:
      return "maintenance";
    case FilespaceOpenSafetyMode::restricted_open:
      return "restricted_open";
    case FilespaceOpenSafetyMode::recovery_required:
      return "recovery_required";
  }
  return "unknown";
}

const char* FilespaceLifecycleBlockerKindName(FilespaceLifecycleBlockerKind kind) {
  switch (kind) {
    case FilespaceLifecycleBlockerKind::none:
      return "none";
    case FilespaceLifecycleBlockerKind::page_allocation:
      return "page_allocation";
    case FilespaceLifecycleBlockerKind::transaction:
      return "transaction";
    case FilespaceLifecycleBlockerKind::backup:
      return "backup";
    case FilespaceLifecycleBlockerKind::archive:
      return "archive";
    case FilespaceLifecycleBlockerKind::agent:
      return "agent";
    case FilespaceLifecycleBlockerKind::operator_hold:
      return "operator_hold";
    case FilespaceLifecycleBlockerKind::recovery:
      return "recovery";
    case FilespaceLifecycleBlockerKind::unknown:
      return "unknown";
  }
  return "unknown";
}

const char* FilespaceMoveDecisionName(FilespaceMoveDecision decision) {
  switch (decision) {
    case FilespaceMoveDecision::allowed:
      return "allowed";
    case FilespaceMoveDecision::blocked:
      return "blocked";
    case FilespaceMoveDecision::no_action:
      return "no_action";
  }
  return "unknown";
}

u64 NormalizeSecondaryTargetFreePages(const SecondaryFilespacePolicy& policy) {
  const u64 min_pages = policy.min_free_pages == 0 ? 4 : policy.min_free_pages;
  const u64 target_pages = policy.target_free_pages == 0 ? 8 : policy.target_free_pages;
  return std::max(min_pages, target_pages);
}

u64 SecondaryLowWaterPages(const SecondaryFilespacePolicy& policy) {
  const auto target = static_cast<double>(NormalizeSecondaryTargetFreePages(policy));
  const double ratio = policy.low_water_ratio <= 0.0 ? 0.50 : policy.low_water_ratio;
  return static_cast<u64>(std::max(1.0, target * ratio));
}

bool SecondaryFilespaceNeedsMoreSpace(const SecondaryFilespaceState& state,
                                      const SecondaryFilespacePolicy& policy) {
  return state.free_pages + state.preallocated_pages <= SecondaryLowWaterPages(policy);
}

SecondaryFilespaceResult InitializeSecondaryFilespaceState(const PhysicalFilespaceHeader& header,
                                                          u16 physical_filespace_id,
                                                          std::string path,
                                                          u64 total_pages,
                                                          u64 free_pages) {
  if (!header.database_uuid.valid() || !header.filespace_uuid.valid() || path.empty()) {
    return SecondaryError("SB-FILESPACE-SECONDARY-IDENTITY-INVALID",
                          "storage.filespace.secondary.identity_invalid",
                          "secondary filespace requires database UUID, filespace UUID, and path");
  }
  if (!IsValidPhysicalFilespaceId(physical_filespace_id) ||
      physical_filespace_id == kActivePrimaryPhysicalFilespaceId) {
    return SecondaryError("SB-FILESPACE-SECONDARY-PHYSICAL-ID-INVALID",
                          "storage.filespace.secondary.physical_id_invalid",
                          "secondary filespaces require a non-primary, non-reserved physical id");
  }
  if (free_pages > total_pages) {
    return SecondaryError("SB-FILESPACE-SECONDARY-FREE-PAGES-INVALID",
                          "storage.filespace.secondary.free_pages_invalid",
                          "free pages cannot exceed total pages");
  }
  SecondaryFilespaceResult result;
  result.status = SecondaryOkStatus();
  result.state.database_uuid = header.database_uuid;
  result.state.filespace_uuid = header.filespace_uuid;
  result.state.physical_filespace_id = physical_filespace_id;
  result.state.role = header.role;
  result.state.lifecycle_state = header.state;
  result.state.path = std::move(path);
  result.state.total_pages = total_pages;
  result.state.free_pages = free_pages;
  result.state.allocated_pages = total_pages - free_pages;
  result.state.header_validated = true;
  result.state.promotion_candidate = header.role == FilespaceRole::primary_candidate ||
                                     header.role == FilespaceRole::primary_shadow ||
                                     header.role == FilespaceRole::secondary_data;
  return result;
}

SecondaryFilespaceResult ExtendSecondaryFilespace(SecondaryFilespaceState state,
                                                  const SecondaryFilespacePolicy& policy,
                                                  u64 additional_pages) {
  if (additional_pages == 0) {
    return SecondaryError("SB-FILESPACE-SECONDARY-EXTEND-PAGES-REQUIRED",
                          "storage.filespace.secondary.extend_pages_required",
                          "extend requires additional pages");
  }
  if (!policy.allow_auto_extend) {
    return SecondaryError("SB-FILESPACE-SECONDARY-EXTEND-DISABLED",
                          "storage.filespace.secondary.extend_disabled",
                          "policy disables automatic secondary filespace extension");
  }
  if (policy.max_pages != 0 && state.total_pages + additional_pages > policy.max_pages) {
    return SecondaryError("SB-FILESPACE-SECONDARY-EXTEND-MAX-PAGES",
                          "storage.filespace.secondary.extend_max_pages",
                          "extend would exceed policy max_pages");
  }
  state.total_pages += additional_pages;
  state.free_pages += additional_pages;
  SecondaryFilespaceResult result;
  result.status = SecondaryOkStatus();
  result.state = std::move(state);
  return result;
}

SecondaryFilespaceResult PreallocateSecondaryFilespacePages(SecondaryFilespaceState state,
                                                            const SecondaryFilespacePolicy& policy,
                                                            u64 pages) {
  if (pages == 0) {
    return SecondaryError("SB-FILESPACE-SECONDARY-PREALLOCATE-PAGES-REQUIRED",
                          "storage.filespace.secondary.preallocate_pages_required",
                          "preallocate requires pages");
  }
  if (state.free_pages < pages) {
    auto extended = ExtendSecondaryFilespace(state, policy, pages - state.free_pages);
    if (!extended.ok()) {
      return extended;
    }
    state = std::move(extended.state);
  }
  state.free_pages -= pages;
  state.preallocated_pages += pages;
  SecondaryFilespaceResult result;
  result.status = SecondaryOkStatus();
  result.state = std::move(state);
  return result;
}

SecondaryFilespaceResult EvaluateSecondaryFilespaceLifecycle(const SecondaryFilespaceState& state,
                                                             FilespaceOpenSafetyMode open_mode,
                                                             const std::vector<FilespaceLifecycleBlocker>& blockers) {
  SecondaryFilespaceResult result;
  result.status = SecondaryOkStatus();
  result.state = state;
  result.gate.blockers = blockers;
  const bool blocked = HasBlockers(blockers);
  const bool unsafe_open = open_mode == FilespaceOpenSafetyMode::recovery_required ||
                           open_mode == FilespaceOpenSafetyMode::restricted_open;
  const bool read_only = open_mode == FilespaceOpenSafetyMode::read_only;
  const bool attached = state.lifecycle_state == FilespaceState::online ||
                        state.lifecycle_state == FilespaceState::maintenance ||
                        state.lifecycle_state == FilespaceState::creating;
  result.gate.can_attach = state.header_validated && !blocked && !unsafe_open;
  result.gate.can_accept_writes = attached && !blocked && !unsafe_open && !read_only;
  result.gate.can_promote_to_primary = state.header_validated && state.promotion_candidate &&
                                       !blocked && !unsafe_open && !read_only;
  result.gate.can_detach = state.header_validated && !blocked &&
                           (open_mode == FilespaceOpenSafetyMode::maintenance || read_only);
  result.gate.can_shrink = state.header_validated && !blocked &&
                           open_mode == FilespaceOpenSafetyMode::maintenance;
  result.gate.can_drop = result.gate.can_shrink && state.allocated_pages == 0 && state.preallocated_pages == 0;
  return result;
}

FilespaceMoveResult PlanSecondaryFilespaceMove(const FilespaceMovePlan& plan,
                                               const std::vector<FilespaceLifecycleBlocker>& blockers) {
  if (!plan.database_uuid.valid() || !plan.filespace_uuid.valid() ||
      plan.source_path.empty() || plan.target_path.empty()) {
    return MoveError(plan,
                     blockers,
                     "SB-FILESPACE-MOVE-PLAN-INVALID",
                     "storage.filespace.secondary.move_plan_invalid",
                     "move requires database UUID, filespace UUID, source path, and target path");
  }
  if (plan.source_path == plan.target_path) {
    FilespaceMoveResult result;
    result.status = SecondaryOkStatus();
    result.decision = FilespaceMoveDecision::no_action;
    result.plan = plan;
    result.blockers = blockers;
    return result;
  }
  if (HasBlockers(blockers)) {
    return MoveError(plan,
                     blockers,
                     "SB-FILESPACE-MOVE-BLOCKED",
                     "storage.filespace.secondary.move_blocked",
                     "filespace move has active blockers");
  }
  if (!plan.operator_approved || !plan.page_agent_relocation_complete || !plan.startup_open_safe) {
    return MoveError(plan,
                     blockers,
                     "SB-FILESPACE-MOVE-APPROVAL-MISSING",
                     "storage.filespace.secondary.move_approval_missing",
                     "move requires operator approval, page-agent relocation completion, and startup-open safety");
  }
  FilespaceMoveResult result;
  result.status = SecondaryOkStatus();
  result.decision = FilespaceMoveDecision::allowed;
  result.plan = plan;
  result.blockers = blockers;
  return result;
}

FilespaceMoveResult PlanPrimaryShadowPromotion(const SecondaryFilespaceState& current_primary,
                                               const SecondaryFilespaceState& candidate,
                                               const std::vector<FilespaceLifecycleBlocker>& blockers) {
  FilespaceMovePlan plan;
  plan.database_uuid = current_primary.database_uuid;
  plan.filespace_uuid = candidate.filespace_uuid;
  plan.physical_filespace_id = candidate.physical_filespace_id;
  plan.source_path = current_primary.path;
  plan.target_path = candidate.path;
  plan.operator_approved = true;
  plan.page_agent_relocation_complete = candidate.allocated_pages >= current_primary.allocated_pages;
  plan.startup_open_safe = current_primary.header_validated && candidate.header_validated;
  auto result = PlanSecondaryFilespaceMove(plan, blockers);
  if (result.ok() && candidate.physical_filespace_id == kActivePrimaryPhysicalFilespaceId) {
    result.status = SecondaryErrorStatus();
    result.decision = FilespaceMoveDecision::blocked;
    result.diagnostic = MakeSecondaryFilespaceDiagnostic(result.status,
                                                         "SB-FILESPACE-PROMOTE-CANDIDATE-ID-INVALID",
                                                         "storage.filespace.secondary.promote_candidate_id_invalid",
                                                         "candidate must use a secondary physical filespace id before promotion");
  }
  return result;
}

DiagnosticRecord MakeSecondaryFilespaceDiagnostic(Status status,
                                                  std::string diagnostic_code,
                                                  std::string message_key,
                                                  std::string detail) {
  std::vector<DiagnosticArgument> args;
  args.push_back({"detail", std::move(detail)});
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(args),
                        {},
                        "storage.filespace.secondary");
}

}  // namespace scratchbird::storage::filespace
