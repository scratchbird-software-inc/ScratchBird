// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "scratchbird/client/connection.h"

namespace scratchbird::cli {

struct ConnectionBootstrapOptions {
    std::string database_path;
    std::string connection_string;
    std::string mode{"inet_listener"};
    std::string ipc_method{"auto"};
    std::string ipc_path;
    std::string front_door_mode{"direct"};
    std::string host{"localhost"};
    uint16_t port{3092};

    std::string manager_auth_token;
    std::string manager_username;
    std::string manager_database;
    std::string manager_connection_profile{"SBsql"};
    std::string manager_client_intent{"SBsql"};
    std::string manager_client_flags;
    std::string manager_auth_fast_path;

    std::string connect_client_flags{"256"};
    std::string auth_method_id;
    std::string auth_token;
    std::string auth_method_payload;
    std::string auth_payload_json;
    std::string auth_payload_b64;
    std::string auth_provider_profile;
    std::string auth_required_methods;
    std::string auth_forbidden_methods;
    std::string auth_require_channel_binding;
    std::string workload_identity_token;
    std::string proxy_principal_assertion;
    std::string ssl_mode;

    std::vector<std::pair<std::string, std::string>> conn_options;
};

std::string normalizeConnectionMode(std::string value);
bool splitConnOption(const std::string& value, std::string& key, std::string& out_value);
bool isLikelyConnectionString(const std::string& value);
std::string buildConnectionTarget(const ConnectionBootstrapOptions& options,
                                  const std::string& database_override = "");

std::string renderAuthProbeText(const client::AuthProbeResult& probe);
std::string renderResolvedAuthContextText(const client::ResolvedAuthContext& context);
std::string renderAuthProbeJson(const client::AuthProbeResult& probe);
std::string renderResolvedAuthContextJson(const client::ResolvedAuthContext& context);

} // namespace scratchbird::cli
