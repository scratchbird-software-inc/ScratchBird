// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "listener_config.hpp"
#include "listener_socket_identity.hpp"
#include "parser_pool.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::listener {

constexpr std::size_t kListenerSupportBundleHistoryMax = 64;

struct ListenerSupportBundleEvent {
  std::uint64_t timestamp_ms{0};
  std::string event_type;
  std::string operation;
  std::string outcome;
  std::string diagnostic_code;
  std::string safe_detail;
};

struct ListenerSupportBundleSnapshot {
  ListenerConfig config;
  ListenerSocketIdentity identity;
  std::string lifecycle_state;
  bool draining{false};
  bool stop_requested{false};
  bool accepting_new_connections{false};
  std::uint64_t last_accept_sequence{0};
  std::uint64_t open_connections{0};
  std::uint64_t queue_depth{0};
  std::uint64_t pending_handoff_bindings{0};
  std::uint64_t handoff_complete_total{0};
  std::uint64_t reject_total{0};
  ParserPoolStatus pool_status;
  std::string metrics_json;
  std::vector<ListenerSupportBundleEvent> management_decisions;
  std::vector<ListenerSupportBundleEvent> runtime_events;
};

std::string RedactListenerSupportabilityText(std::string_view text);
std::string BuildListenerSupportBundleJson(const ListenerSupportBundleSnapshot& snapshot);

const char* listener_support_bundle_implementation_anchor();

}  // namespace scratchbird::listener
