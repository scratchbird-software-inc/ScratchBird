// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "agents/parser_interface_manager.hpp"
#include "lifecycle/parser_lifecycle.hpp"

#include <string>
#include <vector>

namespace scratchbird::parser::sbsql {

// SEARCH_KEY: AEIC_PARSER_INTERFACE_LIFECYCLE_BRIDGE
// Applies parser-interface agent decisions to the real parser lifecycle. The
// bridge is engine/supervisor owned; parser packages never become execution,
// transaction, visibility, finality, SBLR admission, or recovery authority.
struct ParserInterfaceAgentLifecycleRouteRequest {
  ParserLifecycle* lifecycle = nullptr;
  const scratchbird::core::agents::implemented_agents::
      ParserInterfaceManagerResult* decision = nullptr;
  bool engine_supervisor_authority = false;
  bool durable_runtime_store_authority = false;
  bool parser_metrics_authoritative = false;
  bool parser_execution_authority = false;
  bool parser_finality_authority = false;
  bool parser_sblr_authority = false;
};

struct ParserInterfaceAgentLifecycleRouteResult {
  bool ok = false;
  bool fail_closed = true;
  bool no_action = false;
  bool drain_applied = false;
  bool quarantine_applied = false;
  ParserLifecycleState state = ParserLifecycleState::kConstructed;
  std::string diagnostic_code;
  std::vector<std::string> evidence;
};

ParserInterfaceAgentLifecycleRouteResult
ApplyParserInterfaceAgentLifecycleRoute(
    const ParserInterfaceAgentLifecycleRouteRequest& request);

}  // namespace scratchbird::parser::sbsql
