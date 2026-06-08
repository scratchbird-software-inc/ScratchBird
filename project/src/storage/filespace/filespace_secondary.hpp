// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-FILESPACE-SECONDARY-CLOSURE-ANCHOR
#include "filespace_header.hpp"
#include "filespace_identity.hpp"

#include <string>
#include <vector>

namespace scratchbird::storage::filespace {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u64;

enum class FilespaceOpenSafetyMode : u16 {
  normal,
  read_only,
  maintenance,
  restricted_open,
  recovery_required
};

enum class FilespaceLifecycleBlockerKind : u16 {
  none,
  page_allocation,
  transaction,
  backup,
  archive,
  agent,
  operator_hold,
  recovery,
  unknown
};

enum class FilespaceMoveDecision : u16 {
  allowed,
  blocked,
  no_action
};

struct SecondaryFilespacePolicy {
  u64 min_free_pages = 4;
  u64 target_free_pages = 8;
  double low_water_ratio = 0.50;
  u64 max_pages = 0;
  bool allow_auto_extend = true;
  bool allow_auto_shrink = false;
  bool allow_secondary_promotion = true;
  TypedUuid policy_uuid;
};

struct SecondaryFilespaceState {
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  u16 physical_filespace_id = 2;
  FilespaceRole role = FilespaceRole::secondary_data;
  FilespaceState lifecycle_state = FilespaceState::initializing;
  std::string path;
  u64 total_pages = 0;
  u64 free_pages = 0;
  u64 preallocated_pages = 0;
  u64 allocated_pages = 0;
  bool header_validated = false;
  bool promotion_candidate = false;
};

struct FilespaceLifecycleBlocker {
  FilespaceLifecycleBlockerKind kind = FilespaceLifecycleBlockerKind::none;
  std::string owner_subsystem;
  std::string reason;
  TypedUuid evidence_uuid;
};

struct FilespaceLifecycleGate {
  bool can_attach = false;
  bool can_accept_writes = false;
  bool can_promote_to_primary = false;
  bool can_detach = false;
  bool can_shrink = false;
  bool can_drop = false;
  std::vector<FilespaceLifecycleBlocker> blockers;
};

struct FilespaceMovePlan {
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  u16 physical_filespace_id = 0;
  std::string source_path;
  std::string target_path;
  bool operator_approved = false;
  bool page_agent_relocation_complete = false;
  bool startup_open_safe = false;
};

struct FilespaceMoveResult {
  Status status;
  FilespaceMoveDecision decision = FilespaceMoveDecision::blocked;
  DiagnosticRecord diagnostic;
  FilespaceMovePlan plan;
  std::vector<FilespaceLifecycleBlocker> blockers;

  bool ok() const { return status.ok() && decision == FilespaceMoveDecision::allowed; }
};

struct SecondaryFilespaceResult {
  Status status;
  DiagnosticRecord diagnostic;
  SecondaryFilespaceState state;
  FilespaceLifecycleGate gate;

  bool ok() const { return status.ok(); }
};

const char* FilespaceOpenSafetyModeName(FilespaceOpenSafetyMode mode);
const char* FilespaceLifecycleBlockerKindName(FilespaceLifecycleBlockerKind kind);
const char* FilespaceMoveDecisionName(FilespaceMoveDecision decision);

u64 NormalizeSecondaryTargetFreePages(const SecondaryFilespacePolicy& policy);
u64 SecondaryLowWaterPages(const SecondaryFilespacePolicy& policy);
bool SecondaryFilespaceNeedsMoreSpace(const SecondaryFilespaceState& state,
                                      const SecondaryFilespacePolicy& policy);

SecondaryFilespaceResult InitializeSecondaryFilespaceState(const PhysicalFilespaceHeader& header,
                                                          u16 physical_filespace_id,
                                                          std::string path,
                                                          u64 total_pages,
                                                          u64 free_pages);
SecondaryFilespaceResult ExtendSecondaryFilespace(SecondaryFilespaceState state,
                                                  const SecondaryFilespacePolicy& policy,
                                                  u64 additional_pages);
SecondaryFilespaceResult PreallocateSecondaryFilespacePages(SecondaryFilespaceState state,
                                                            const SecondaryFilespacePolicy& policy,
                                                            u64 pages);
SecondaryFilespaceResult EvaluateSecondaryFilespaceLifecycle(const SecondaryFilespaceState& state,
                                                             FilespaceOpenSafetyMode open_mode,
                                                             const std::vector<FilespaceLifecycleBlocker>& blockers);
FilespaceMoveResult PlanSecondaryFilespaceMove(const FilespaceMovePlan& plan,
                                               const std::vector<FilespaceLifecycleBlocker>& blockers);
FilespaceMoveResult PlanPrimaryShadowPromotion(const SecondaryFilespaceState& current_primary,
                                               const SecondaryFilespaceState& candidate,
                                               const std::vector<FilespaceLifecycleBlocker>& blockers);
DiagnosticRecord MakeSecondaryFilespaceDiagnostic(Status status,
                                                  std::string diagnostic_code,
                                                  std::string message_key,
                                                  std::string detail = {});

}  // namespace scratchbird::storage::filespace
