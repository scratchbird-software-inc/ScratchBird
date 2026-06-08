// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_runtime.hpp"

namespace scratchbird::core::agents::implemented_agents {

// SEARCH_KEY: SB_AGENT_IMPLEMENTATION_distributed_query_metrics_agent
// Canonical distributed_query_metrics_agent behavior is compile-time stubbed in core.
// CanonicalAgentRegistry owns exposure and fail-closed classification.
// Live cluster behavior must route through the external cluster provider library.
const char* distributed_query_metrics_agent_implementation_anchor() { return "distributed_query_metrics_agent"; }

}  // namespace scratchbird::core::agents::implemented_agents
