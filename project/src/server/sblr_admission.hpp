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

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::server {

struct ServerSblrAdmissionRequest {
  std::string encoded_sblr_envelope;
  bool cluster_authority_active = false;
};

struct ServerSblrAdmissionResult {
  bool admitted = false;
  bool requires_public_abi_dispatch = false;
  std::string operation_family;
  std::string operation_id;
  std::uint64_t row_count_hint = 0;
  std::vector<ServerDiagnostic> diagnostics;
};

ServerSblrAdmissionResult AdmitServerSblrEnvelope(
    const ServerSblrAdmissionRequest& request);

}  // namespace scratchbird::server
