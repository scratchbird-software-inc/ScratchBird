// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "control_plane.hpp"
#include "listener_config.hpp"

#include <cstdint>

namespace scratchbird::listener {

enum class ListenerTlsTerminationOwner : std::uint8_t {
  kNone = 0,
  kParserWorker = 1,
};

struct ListenerTlsHandoffPolicy {
  bool tls_required{false};
  bool ca_configured{false};
  bool cert_configured{false};
  bool key_configured{false};
  ListenerTlsTerminationOwner termination_owner{ListenerTlsTerminationOwner::kNone};
  bool channel_binding_required{false};
  bool downgrade_refusal_enforced{false};
  bool certificate_evidence_required{false};
  bool listener_decrypts_client_stream{false};
};

void ApplyListenerTlsHandoffPolicy(const ListenerConfig& config,
                                   HandoffSocketPayload* payload);
ListenerTlsHandoffPolicy DecodeListenerTlsHandoffPolicy(
    const HandoffSocketPayload& payload);

const char* listener_tls_policy_implementation_anchor();

}  // namespace scratchbird::listener
