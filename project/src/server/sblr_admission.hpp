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

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace scratchbird::server {

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
  std::string authenticated_principal_uuid;
  std::string catalog_snapshot_uuid;
  std::string engine_mga_statement_uuid;
  std::string engine_mga_snapshot_uuid;
  std::uint64_t catalog_epoch = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t resource_epoch = 0;
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

ServerSblrAdmissionResult AdmitServerSblrEnvelope(
    const ServerSblrAdmissionRequest& request);

// SEARCH_KEY: SB_SERVER_SBLR_IMMUTABLE_ADMISSION_TOKEN
bool DispatchAdmittedServerSblrToken(
    const ServerSblrAdmissionToken& token,
    const std::function<void(const ServerSblrAdmissionTokenData&)>& dispatch_probe);

}  // namespace scratchbird::server
