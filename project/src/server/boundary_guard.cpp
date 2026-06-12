// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_PRODUCT_BOUNDARY_GUARD

#include "boundary_guard.hpp"

namespace scratchbird::server {

namespace {

std::string ResponsibilityName(ForbiddenServerResponsibility responsibility) {
  switch (responsibility) {
    case ForbiddenServerResponsibility::kSqlParsing:
      return "sql_parsing";
    case ForbiddenServerResponsibility::kReferenceDialectSemantics:
      return "compatibility_dialect_semantics";
    case ForbiddenServerResponsibility::kClientWireProtocolShaping:
      return "client_wire_protocol_shaping";
    case ForbiddenServerResponsibility::kEngineSemanticAuthority:
      return "engine_semantic_authority";
    case ForbiddenServerResponsibility::kClusterAuthority:
      return "cluster_authority";
  }
  return "unknown";
}

}  // namespace

ServerDiagnostic ForbiddenResponsibilityDiagnostic(ForbiddenServerResponsibility responsibility,
                                                   std::string detail) {
  return ServerDiagnostic{
      "SERVER.BOUNDARY.FORBIDDEN_RESPONSIBILITY",
      "server.boundary.forbidden_responsibility",
      ServerDiagnosticSeverity::kError,
      "The requested operation belongs to another ScratchBird product boundary.",
      {{"responsibility", ResponsibilityName(responsibility)}, {"detail", std::move(detail)}}};
}

ServerDiagnostic SkeletonNextStageDiagnostic(std::string requested_operation,
                                             std::string owning_execution_plan) {
  return ServerDiagnostic{
      "SERVER.SKELETON.NEXT_STAGE_REQUIRED",
      "server.skeleton.next_stage_required",
      ServerDiagnosticSeverity::kError,
      "The sb_server product skeleton is present, but this operation is owned by a later server implementation execution_plan.",
      {{"operation", std::move(requested_operation)}, {"owning_execution_plan", std::move(owning_execution_plan)}}};
}

}  // namespace scratchbird::server
