// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <map>
#include <string>

#include "scratchbird/client/network_client.h"
#include "scratchbird/core/error_context.h"

namespace scratchbird {
namespace client {

network::SSLMode parseSslMode(const std::string& value);
protocol::AuthMethod parseAuthMethod(const std::string& value, bool* ok = nullptr);

core::Status parseKeyValueConnectionString(const std::string& conn_str,
                                           std::map<std::string, std::string>& params,
                                           core::ErrorContext* ctx = nullptr);
core::Status parseScratchbirdUrl(const std::string& url,
                                 NetworkClientConfig& config,
                                 core::ErrorContext* ctx = nullptr);
core::Status applyConnectionParams(const std::map<std::string, std::string>& params,
                                   NetworkClientConfig& config,
                                   core::ErrorContext* ctx = nullptr);
core::Status parseDriverConnectionString(const std::string& conn_str,
                                         NetworkClientConfig& config,
                                         core::ErrorContext* ctx = nullptr);

void applyDriverDefaults(NetworkClientConfig& config);

} // namespace client
} // namespace scratchbird
