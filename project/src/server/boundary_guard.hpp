// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_PRODUCT_BOUNDARY_GUARD

#pragma once

#include "diagnostics.hpp"

#include <string>

namespace scratchbird::server {

enum class ForbiddenServerResponsibility {
  kSqlParsing,
  kReferenceDialectSemantics,
  kClientWireProtocolShaping,
  kEngineSemanticAuthority,
  kClusterAuthority,
};

ServerDiagnostic ForbiddenResponsibilityDiagnostic(ForbiddenServerResponsibility responsibility,
                                                   std::string detail);
ServerDiagnostic SkeletonNextStageDiagnostic(std::string requested_operation,
                                             std::string owning_execution_plan);

}  // namespace scratchbird::server
