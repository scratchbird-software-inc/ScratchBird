// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "listener_tls_policy.hpp"

namespace scratchbird::listener {
namespace {

constexpr std::size_t kTlsStateRequired = 0;
constexpr std::size_t kTlsStateCaConfigured = 1;
constexpr std::size_t kTlsStateCertConfigured = 2;
constexpr std::size_t kTlsStateKeyConfigured = 3;
constexpr std::size_t kTlsStateTerminationOwner = 4;
constexpr std::size_t kTlsStateChannelBindingRequired = 5;
constexpr std::size_t kTlsStateDowngradeRefusalEnforced = 6;
constexpr std::size_t kTlsStateCertificateEvidenceRequired = 7;
constexpr std::size_t kTlsStateListenerDecrypts = 8;

std::uint8_t BoolByte(bool value) {
  return value ? 1u : 0u;
}

}  // namespace

void ApplyListenerTlsHandoffPolicy(const ListenerConfig& config,
                                   HandoffSocketPayload* payload) {
  if (payload == nullptr) return;
  payload->tls_active = config.tls_required;
  payload->tls_state.fill(0);
  payload->tls_state[kTlsStateRequired] = BoolByte(config.tls_required);
  payload->tls_state[kTlsStateCaConfigured] = BoolByte(!config.tls_ca_file.empty());
  payload->tls_state[kTlsStateCertConfigured] = BoolByte(!config.tls_cert_file.empty());
  payload->tls_state[kTlsStateKeyConfigured] = BoolByte(!config.tls_key_file.empty());
  if (config.tls_required) {
    payload->tls_state[kTlsStateTerminationOwner] =
        static_cast<std::uint8_t>(ListenerTlsTerminationOwner::kParserWorker);
    payload->tls_state[kTlsStateChannelBindingRequired] = 1;
    payload->tls_state[kTlsStateDowngradeRefusalEnforced] = 1;
    payload->tls_state[kTlsStateCertificateEvidenceRequired] = 1;
    payload->tls_state[kTlsStateListenerDecrypts] = 0;
  }
}

ListenerTlsHandoffPolicy DecodeListenerTlsHandoffPolicy(
    const HandoffSocketPayload& payload) {
  ListenerTlsHandoffPolicy policy;
  policy.tls_required = payload.tls_active || payload.tls_state[kTlsStateRequired] != 0;
  policy.ca_configured = payload.tls_state[kTlsStateCaConfigured] != 0;
  policy.cert_configured = payload.tls_state[kTlsStateCertConfigured] != 0;
  policy.key_configured = payload.tls_state[kTlsStateKeyConfigured] != 0;
  policy.termination_owner =
      static_cast<ListenerTlsTerminationOwner>(payload.tls_state[kTlsStateTerminationOwner]);
  policy.channel_binding_required =
      payload.tls_state[kTlsStateChannelBindingRequired] != 0;
  policy.downgrade_refusal_enforced =
      payload.tls_state[kTlsStateDowngradeRefusalEnforced] != 0;
  policy.certificate_evidence_required =
      payload.tls_state[kTlsStateCertificateEvidenceRequired] != 0;
  policy.listener_decrypts_client_stream =
      payload.tls_state[kTlsStateListenerDecrypts] != 0;
  return policy;
}

const char* listener_tls_policy_implementation_anchor() {
  return "listener_tls_parser_termination_policy.v1";
}

}  // namespace scratchbird::listener
