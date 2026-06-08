// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_ownership.hpp"

namespace scratchbird::core::index {
namespace {
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
Status OkStatus() { return Status{StatusCode::ok, Severity::info, Subsystem::engine}; }
Status DeniedStatus() { return Status{StatusCode::platform_required_feature_missing, Severity::error, Subsystem::engine}; }
}

const char* IndexSubsystemOwnerName(IndexSubsystemOwner owner) {
  switch (owner) {
    case IndexSubsystemOwner::index_manager: return "index_manager";
    case IndexSubsystemOwner::memory_manager: return "memory_manager";
    case IndexSubsystemOwner::page_agent: return "page_agent";
    case IndexSubsystemOwner::filespace_agent: return "filespace_agent";
    case IndexSubsystemOwner::optimizer: return "optimizer";
    case IndexSubsystemOwner::transaction_manager: return "transaction_manager";
    case IndexSubsystemOwner::security_manager: return "security_manager";
    case IndexSubsystemOwner::metrics_registry: return "metrics_registry";
    case IndexSubsystemOwner::archive_manager: return "archive_manager";
    case IndexSubsystemOwner::parser_sblr: return "parser_sblr";
    case IndexSubsystemOwner::udr_registry: return "udr_registry";
    case IndexSubsystemOwner::management_api: return "management_api";
  }
  return "unknown";
}

IndexOwnershipDecision EvaluateIndexOwnership(std::string_view action, IndexSubsystemOwner caller) {
  IndexSubsystemOwner owner = IndexSubsystemOwner::index_manager;
  if (action == "memory_residency_request") owner = IndexSubsystemOwner::memory_manager;
  else if (action == "allocate_index_pages") owner = IndexSubsystemOwner::page_agent;
  else if (action == "request_filespace_growth") owner = IndexSubsystemOwner::filespace_agent;
  else if (action == "optimizer_path_selection") owner = IndexSubsystemOwner::optimizer;
  else if (action == "security_visibility") owner = IndexSubsystemOwner::security_manager;
  else if (action == "index_metrics_update") owner = IndexSubsystemOwner::metrics_registry;
  else if (action == "archive_lineage") owner = IndexSubsystemOwner::archive_manager;
  else if (action == "sblr_dispatch") owner = IndexSubsystemOwner::parser_sblr;
  else if (action == "helper_registration") owner = IndexSubsystemOwner::udr_registry;
  else if (action == "management_request") owner = IndexSubsystemOwner::management_api;
  const bool allowed = owner == caller;
  const Status status = allowed ? OkStatus() : DeniedStatus();
  IndexOwnershipDecision decision{status, allowed, owner, {}};
  if (!allowed) {
    decision.diagnostic = MakeIndexFamilyDiagnostic(status, "INDEX.OWNERSHIP.DENIED", "index.ownership.denied", std::string(action));
  }
  return decision;
}

}  // namespace scratchbird::core::index
