// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::sblr {

using DiagnosticControlUuid = std::array<std::uint8_t, 16>;
using DiagnosticControlSha256 = std::array<std::uint8_t, 32>;

enum class DiagnosticControlScopeKind : std::uint8_t {
  session = 1,
  request = 2,
  process = 3,
  subsystem = 4,
  cluster_provider = 5,
};

enum class DiagnosticControlAction : std::uint8_t {
  enable = 1,
  disable = 2,
  quarantine = 3,
  release = 4,
  set_level = 5,
  set_sampling = 6,
};

enum class DiagnosticControlParameterKind : std::uint16_t {
  none = 0,
  level = 1,
  sampling = 2,
};

enum class DiagnosticControlResultStatus : std::uint8_t {
  applied = 1,
  already_applied = 2,
  refused = 3,
};

struct SblrDiagnosticControlParameterV1 {
  DiagnosticControlParameterKind kind = DiagnosticControlParameterKind::none;
  std::uint16_t version = 1;
  std::uint32_t length = 0;
  std::uint32_t value = 0;
  DiagnosticControlUuid parameter_uuid{};
};

struct SblrDiagnosticControlDescriptorV1 {
  std::uint8_t flags = 0;
  DiagnosticControlUuid operation_uuid{};
  DiagnosticControlUuid authenticated_receipt_uuid{};
  DiagnosticControlUuid target_scope_uuid{};
  std::uint64_t target_scope_generation = 0;
  DiagnosticControlScopeKind scope_kind = DiagnosticControlScopeKind::session;
  DiagnosticControlAction action = DiagnosticControlAction::enable;
  DiagnosticControlUuid diagnostic_state_uuid{};
  std::uint64_t diagnostic_state_generation = 0;
  DiagnosticControlUuid security_context_uuid{};
  DiagnosticControlUuid policy_snapshot_uuid{};
  std::uint64_t policy_generation = 0;
  DiagnosticControlUuid transaction_uuid{};
  DiagnosticControlUuid route_provider_evidence_uuid{};
  DiagnosticControlUuid confirmation_token_uuid{};
  SblrDiagnosticControlParameterV1 parameter{};
  DiagnosticControlSha256 parameter_sha256{};
  DiagnosticControlSha256 descriptor_evidence_sha256{};
  DiagnosticControlSha256 descriptor_sha256{};
};

struct SblrDiagnosticControlResultV1 {
  DiagnosticControlResultStatus status = DiagnosticControlResultStatus::applied;
  DiagnosticControlUuid operation_uuid{};
  DiagnosticControlUuid target_scope_uuid{};
  std::uint64_t old_generation = 0;
  std::uint64_t new_generation = 0;
  DiagnosticControlUuid audit_event_uuid{};
  DiagnosticControlUuid evidence_uuid{};
  DiagnosticControlSha256 material_sha256{};
  DiagnosticControlUuid recovery_replay_evidence_uuid{};
  DiagnosticControlUuid publication_barrier_uuid{};
  std::uint64_t diagnostic_vector_count = 0;
};

struct DiagnosticControlWireLayout {
  static constexpr std::size_t descriptor_size = 320;
  static constexpr std::size_t result_size = 192;
};

// The Core packet defines the fixed carriers independently of the currently
// unresolved SBLR operation-id/value-kind allocation. These codecs therefore
// do not assign or infer an envelope value-kind code.
std::vector<std::uint8_t> EncodeSblrDiagnosticControlDescriptorV1(
    const SblrDiagnosticControlDescriptorV1& descriptor,
    std::string* detail = nullptr);
bool DecodeSblrDiagnosticControlDescriptorV1(
    const std::uint8_t* bytes,
    std::size_t size,
    SblrDiagnosticControlDescriptorV1* descriptor,
    std::string* detail = nullptr);

std::vector<std::uint8_t> EncodeSblrDiagnosticControlResultV1(
    const SblrDiagnosticControlResultV1& result,
    std::string* detail = nullptr);
bool DecodeSblrDiagnosticControlResultV1(
    const std::uint8_t* bytes,
    std::size_t size,
    SblrDiagnosticControlResultV1* result,
    std::string* detail = nullptr);

}  // namespace scratchbird::engine::sblr
