// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_SBLR_ADMISSION_VALIDATOR

#pragma once

#include "diagnostics.hpp"
#include "../engine/sblr/sblr_engine_envelope.hpp"
#include "../engine/sblr/sblr_opcode_stream.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace scratchbird::server_engine_bridge {
struct StatementContextReceiptView;
}

namespace scratchbird::server {

enum class ServerSblrPayloadKind : std::uint8_t {
  invalid = 0,
  opcode_stream = 1,
  operation_envelope = 2,
};

inline constexpr std::uint64_t
    kServerSblrLocalGatewayOwnershipRegistryGenerationV1 = 1;
inline constexpr std::uint64_t
    kServerSblrPackageExecutorRegistryGenerationV1 = 1;

enum class ServerSblrGatewayEvidenceSource : std::uint8_t {
  invalid = 0,
  local_observed = 1,
};

enum class ServerSblrGatewayDisposition : std::uint8_t {
  invalid = 0,
  pass_through = 1,
  handled = 2,
  async_accepted = 3,
  refused = 4,
};

struct ServerSblrGatewayDecisionEvidence {
  ServerSblrGatewayEvidenceSource source =
      ServerSblrGatewayEvidenceSource::invalid;
  ServerSblrGatewayDisposition disposition =
      ServerSblrGatewayDisposition::invalid;
  std::uint64_t provider_observation_generation = 0;
  std::array<std::uint8_t, 32> canonical_payload_sha256{};
  std::string route_snapshot_uuid;
  std::uint64_t route_epoch = 0;
  std::uint64_t route_generation = 0;
  std::string security_snapshot_uuid;
  std::uint64_t security_epoch = 0;
  std::uint64_t security_observation_generation = 0;
  bool cluster_context_active = false;
  bool cluster_transaction_active = false;
  bool route_fence_present = false;
};

struct ServerSblrPackageExecutorEvidence {
  std::string begin_executor_id;
  std::string end_executor_id;
  std::string registry_snapshot_uuid;
  std::uint64_t executor_evidence_generation = 0;
  std::array<std::uint8_t, 32> canonical_payload_sha256{};
};

struct ServerSblrAdmissionRequest {
  // Retired frame/text input. Retained only for source compatibility and
  // deterministic refusal; it is never decoded into executable authority.
  std::string encoded_sblr_envelope;
  bool cluster_authority_active = false;
  std::string encoded_sblr_container;
  std::string encoded_execution_envelope;
  std::string admitted_parser_package_uuid;
  std::uint32_t admitted_parser_package_version_major = 0;
  std::uint32_t admitted_parser_package_version_minor = 0;
  std::uint32_t admitted_parser_package_version_patch = 0;
  std::string admitted_registry_snapshot_uuid;
  std::string authenticated_principal_uuid;
  std::string catalog_snapshot_uuid;
  std::string engine_mga_statement_uuid;
  std::string engine_mga_snapshot_uuid;
  std::uint64_t catalog_epoch = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t resource_epoch = 0;
  std::string route_snapshot_uuid;
  std::uint64_t route_epoch = 0;
  std::uint64_t route_generation = 0;
  std::string security_snapshot_uuid;
  std::uint64_t security_observation_generation = 0;
  bool route_snapshot_engine_owned = false;
  bool security_snapshot_engine_owned = false;
  bool cluster_transaction_active = false;
  bool route_fence_present = false;
  std::uint64_t package_reservation_handle = 0;
  ServerSblrPayloadKind reserved_payload_kind = ServerSblrPayloadKind::invalid;
  std::uint64_t reserved_payload_size = 0;
  std::uint32_t reserved_record_count = 0;
  std::uint64_t reserved_resource_policy_generation = 0;
  std::array<std::uint8_t, 32> reserved_payload_sha256{};
  // The statement receipt remains the sole owner of external source-artifact
  // bytes. The dispatch boundary supplies this ephemeral resolved copy only
  // after exact receipt/reference validation; callers cannot self-authorize it.
  bool source_artifact_resolved_by_engine = false;
  std::vector<std::uint8_t> resolved_source_artifact_bytes;
  // Prepare metadata admission defers package reservation until execution
  // has a live statement receipt.
  bool package_reservation_deferred = false;
};

struct ServerSblrAdmissionTokenData {
  std::vector<std::uint8_t> canonical_container_bytes;
  std::vector<std::uint8_t> canonical_execution_envelope_bytes;
  std::vector<std::uint8_t> canonical_operation_bytes;
  std::array<std::uint8_t, 32> container_sha256{};
  std::array<std::uint8_t, 32> execution_envelope_sha256{};
  std::array<std::uint8_t, 32> operation_sha256{};
  std::array<std::uint8_t, 32> admission_binding_sha256{};
  scratchbird::engine::sblr::SblrOperationEnvelope operation;
  bool opcode_stream = false;
  ServerSblrGatewayDecisionEvidence gateway_evidence;
  ServerSblrPackageExecutorEvidence package_executor_evidence;
  scratchbird::engine::sblr::SblrOpcodeStream stream;
  std::string authenticated_principal_uuid;
  std::string catalog_snapshot_uuid;
  std::string engine_mga_statement_uuid;
  std::string engine_mga_snapshot_uuid;
  std::uint64_t catalog_epoch = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t resource_epoch = 0;
  std::uint64_t package_reservation_handle = 0;
  ServerSblrPayloadKind reserved_payload_kind = ServerSblrPayloadKind::invalid;
  std::uint64_t reserved_payload_size = 0;
  std::uint32_t reserved_record_count = 0;
  std::uint64_t reserved_resource_policy_generation = 0;
};

using ServerSblrAdmissionToken =
    std::shared_ptr<const ServerSblrAdmissionTokenData>;

struct ServerSblrAdmissionResult {
  bool admitted = false;
  bool requires_public_abi_dispatch = false;
  std::string operation_family;
  std::string operation_id;
  std::uint64_t row_count_hint = 0;
  ServerSblrAdmissionToken admission_token;
  std::vector<ServerDiagnostic> diagnostics;
};

void BindServerSblrGatewayReceiptObservation(
    const scratchbird::server_engine_bridge::StatementContextReceiptView& view,
    ServerSblrAdmissionRequest* request);

ServerSblrAdmissionResult AdmitServerSblrEnvelope(
    const ServerSblrAdmissionRequest& request);

// SEARCH_KEY: SB_SERVER_SBLR_IMMUTABLE_ADMISSION_TOKEN
bool DispatchAdmittedServerSblrToken(
    const ServerSblrAdmissionToken& token,
    const std::function<void(const ServerSblrAdmissionTokenData&)>& dispatch_probe);

}  // namespace scratchbird::server
