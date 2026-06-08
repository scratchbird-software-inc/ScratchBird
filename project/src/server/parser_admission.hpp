// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_PARSER_ADMISSION_INTERFACE

#pragma once

#include "diagnostics.hpp"
#include "parser_package_registry.hpp"
#include "sbps.hpp"

#include <string>
#include <vector>

namespace scratchbird::server {

struct ParserAdmissionRequest {
  sbps::HelloRequest hello;
  std::string database_selector;
  std::string listener_uuid;
  std::string worker_id;
  bool preauth_channel = true;
};

struct ParserAdmissionResult {
  bool admitted = false;
  std::string outcome = "rejected";
  std::string parser_channel_uuid;
  std::string session_uuid;
  std::string database_selector;
  std::vector<ServerDiagnostic> diagnostics;
};

ParserAdmissionResult AdmitParserPreauthForListener(const ParserPackageRegistry& registry,
                                                    const ParserAdmissionRequest& request);
bool ParserPreauthOperationAllowed(const std::string& operation_name);

}  // namespace scratchbird::server
