// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "scratchbird/client/driver_config.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <set>
#include <sstream>

namespace scratchbird {
namespace client {

namespace {
std::string toLower(const std::string& value) {
    std::string out = value;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return out;
}

std::string trim(std::string value) {
    auto first = value.find_first_not_of(" \t\n\r");
    if (first == std::string::npos) {
        return "";
    }
    auto last = value.find_last_not_of(" \t\n\r");
    value.erase(last + 1);
    value.erase(0, first);
    return value;
}

bool parseUint32(const std::string& value, uint32_t& out) {
    try {
        size_t idx = 0;
        unsigned long parsed = std::stoul(value, &idx);
        if (idx != value.size()) {
            return false;
        }
        out = static_cast<uint32_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

bool parseBool(const std::string& value, bool& out) {
    std::string lower = toLower(trim(value));
    if (lower == "true" || lower == "1" || lower == "yes" || lower == "on") {
        out = true;
        return true;
    }
    if (lower == "false" || lower == "0" || lower == "no" || lower == "off") {
        out = false;
        return true;
    }
    return false;
}

bool parseCompressionMode(const std::string& value, bool& enabled) {
    const std::string lower = toLower(trim(value));
    if (lower.empty() || lower == "off" || lower == "none" || lower == "false" ||
        lower == "0" || lower == "disable" || lower == "disabled") {
        enabled = false;
        return true;
    }
    if (lower == "zstd" || lower == "true" || lower == "1" ||
        lower == "on" || lower == "enable" || lower == "enabled") {
        enabled = true;
        return true;
    }
    return false;
}

bool parseProtocol(const std::string& value, std::string& normalized) {
    std::string lower = toLower(trim(value));
    if (lower == "native" || lower == "scratchbird" ||
        lower == "scratchbird-native" || lower == "scratchbird_native") {
        normalized = "native";
        return true;
    }
    return false;
}

bool parseFrontDoorMode(const std::string& value, std::string& normalized) {
    std::string lower = toLower(trim(value));
    if (lower.empty() || lower == "direct") {
        normalized = "direct";
        return true;
    }
    if (lower == "manager_proxy" || lower == "manager-proxy" || lower == "managed") {
        normalized = "manager_proxy";
        return true;
    }
    return false;
}

bool parseTransportMode(const std::string& value, std::string& normalized) {
    std::string lower = toLower(trim(value));
    if (lower == "embedded" || lower == "inproc" || lower == "in-process" ||
        lower == "in_process") {
        normalized = "embedded";
        return true;
    }
    if (lower.empty() || lower == "inet" || lower == "inet_listener" ||
        lower == "listener" || lower == "tcp" || lower == "tcp_listener" ||
        lower == "network") {
        normalized = "inet_listener";
        return true;
    }
    if (lower == "managed" || lower == "manager" || lower == "manager_proxy" ||
        lower == "manager-proxy" || lower == "mcp") {
        normalized = "managed";
        return true;
    }
    if (lower == "local" || lower == "ipc" || lower == "local_ipc" ||
        lower == "local-ipc" || lower == "unix" || lower == "unix_socket" ||
        lower == "unix-socket" || lower == "pipe") {
        normalized = "local_ipc";
        return true;
    }
    return false;
}

bool parseIpcMethod(const std::string& value, std::string& normalized) {
    std::string lower = toLower(trim(value));
    if (lower.empty() || lower == "auto") {
        normalized = "auto";
        return true;
    }
    if (lower == "unix" || lower == "unix_socket" || lower == "unix-socket") {
        normalized = "unix";
        return true;
    }
    if (lower == "pipe" || lower == "named_pipe" || lower == "named-pipe") {
        normalized = "pipe";
        return true;
    }
    if (lower == "tcp") {
        normalized = "tcp";
        return true;
    }
    return false;
}

std::vector<std::string> parseMethodList(const std::string& value) {
    std::vector<std::string> methods;
    std::string token;
    std::stringstream ss(value);
    while (std::getline(ss, token, ',')) {
        std::string normalized = toLower(trim(token));
        if (!normalized.empty()) {
            methods.push_back(normalized);
        }
    }
    return methods;
}

bool hasOverlappingMethods(const std::vector<std::string>& required,
                           const std::vector<std::string>& forbidden,
                           std::string& overlap) {
    std::set<std::string> required_set(required.begin(), required.end());
    for (const auto& method : forbidden) {
        if (required_set.find(method) != required_set.end()) {
            overlap = method;
            return true;
        }
    }
    overlap.clear();
    return false;
}

uint32_t normalizeTimeoutMs(const std::string& key, const std::string& value) {
    uint32_t parsed = 0;
    if (!parseUint32(value, parsed)) {
        return 0;
    }
    std::string lower = toLower(key);
    if (lower.find("_ms") != std::string::npos || lower.find("ms") != std::string::npos) {
        return parsed;
    }
    return parsed * 1000;
}
}

network::SSLMode parseSslMode(const std::string& value) {
    auto mode = toLower(value);
    if (mode == "disable" || mode == "disabled") {
        return network::SSLMode::DISABLED;
    }
    if (mode == "allow") {
        return network::SSLMode::ALLOW;
    }
    if (mode == "prefer") {
        return network::SSLMode::PREFER;
    }
    if (mode == "require") {
        return network::SSLMode::REQUIRE;
    }
    if (mode == "verify-ca" || mode == "verify_ca") {
        return network::SSLMode::VERIFY_CA;
    }
    if (mode == "verify-full" || mode == "verify_full") {
        return network::SSLMode::VERIFY_FULL;
    }
    return network::SSLMode::REQUIRE;
}

protocol::AuthMethod parseAuthMethod(const std::string& value, bool* ok) {
    std::string lower = toLower(value);
    if (lower == "scram-sha-256" || lower == "scram_sha_256" || lower == "scram256") {
        if (ok) {
            *ok = true;
        }
        return protocol::AuthMethod::ScramSha256;
    }
    if (lower == "scram-sha-512" || lower == "scram_sha_512" || lower == "scram512") {
        if (ok) {
            *ok = true;
        }
        return protocol::AuthMethod::ScramSha512;
    }
    if (lower == "token" || lower == "bearer" || lower == "jwt" || lower == "oidc-id-token") {
        if (ok) {
            *ok = true;
        }
        return protocol::AuthMethod::Token;
    }
    if (lower == "password" || lower == "plain") {
        if (ok) {
            *ok = true;
        }
        return protocol::AuthMethod::Password;
    }
    if (lower == "md5") {
        if (ok) {
            *ok = true;
        }
        return protocol::AuthMethod::Md5;
    }
    if (lower == "peer" || lower == "peer_uid") {
        if (ok) {
            *ok = true;
        }
        return protocol::AuthMethod::Peer;
    }
    if (lower == "reattach") {
        if (ok) {
            *ok = true;
        }
        return protocol::AuthMethod::Reattach;
    }
    if (ok) {
        *ok = false;
    }
    return protocol::AuthMethod::ScramSha256;
}

core::Status parseKeyValueConnectionString(const std::string& conn_str,
                                           std::map<std::string, std::string>& params,
                                           core::ErrorContext* ctx) {
    params.clear();

    size_t start = 0;
    while (start < conn_str.size()) {
        size_t end = conn_str.find(';', start);
        if (end == std::string::npos) {
            end = conn_str.size();
        }
        std::string token = conn_str.substr(start, end - start);
        start = end + 1;
        token = trim(token);
        if (token.empty()) {
            continue;
        }
        size_t eq = token.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        std::string key = trim(token.substr(0, eq));
        std::string value = trim(token.substr(eq + 1));
        if (value.size() >= 2 && value.front() == '{' && value.back() == '}') {
            value = value.substr(1, value.size() - 2);
        }
        if (!key.empty()) {
            params[toLower(key)] = value;
        }
    }

    if (params.empty() && ctx) {
        ctx->message = "Empty connection string";
    }
    return core::Status::OK;
}

core::Status parseScratchbirdUrl(const std::string& url,
                                 NetworkClientConfig& config,
                                 core::ErrorContext* ctx) {
    const std::string kPrefix = "scratchbird://";
    if (url.rfind(kPrefix, 0) != 0) {
        if (ctx) {
            ctx->message = "Unsupported URL scheme";
        }
        return core::Status::INVALID_ARGUMENT;
    }

    std::string remainder = url.substr(kPrefix.size());
    std::string query;
    size_t query_pos = remainder.find('?');
    if (query_pos != std::string::npos) {
        query = remainder.substr(query_pos + 1);
        remainder = remainder.substr(0, query_pos);
    }

    std::string userinfo;
    std::string hostport;
    std::string database;

    size_t at_pos = remainder.find('@');
    if (at_pos != std::string::npos) {
        userinfo = remainder.substr(0, at_pos);
        hostport = remainder.substr(at_pos + 1);
    } else {
        hostport = remainder;
    }

    size_t slash_pos = hostport.find('/');
    if (slash_pos != std::string::npos) {
        database = hostport.substr(slash_pos + 1);
        hostport = hostport.substr(0, slash_pos);
    }

    std::string host = hostport;
    std::string port_str;
    size_t colon_pos = hostport.rfind(':');
    if (colon_pos != std::string::npos && colon_pos + 1 < hostport.size()) {
        host = hostport.substr(0, colon_pos);
        port_str = hostport.substr(colon_pos + 1);
    }

    if (!userinfo.empty()) {
        size_t colon = userinfo.find(':');
        if (colon == std::string::npos) {
            config.username = userinfo;
        } else {
            config.username = userinfo.substr(0, colon);
            config.password = userinfo.substr(colon + 1);
        }
    }

    if (!host.empty()) {
        config.host = host;
    }
    if (!port_str.empty()) {
        uint32_t port_val = 0;
        if (!parseUint32(port_str, port_val) || port_val > 65535) {
            if (ctx) {
                ctx->message = "Invalid port in URL";
            }
            return core::Status::INVALID_ARGUMENT;
        }
        config.port = static_cast<uint16_t>(port_val);
    }
    if (!database.empty()) {
        config.database = database;
    }

    if (!query.empty()) {
        std::map<std::string, std::string> params;
        std::stringstream ss(query);
        std::string pair;
        while (std::getline(ss, pair, '&')) {
            size_t eq = pair.find('=');
            if (eq == std::string::npos) {
                continue;
            }
            std::string key = toLower(trim(pair.substr(0, eq)));
            std::string value = trim(pair.substr(eq + 1));
            if (!key.empty()) {
                params[key] = value;
            }
        }
        auto status = applyConnectionParams(params, config, ctx);
        if (status != core::Status::OK) {
            return status;
        }
    }

    return core::Status::OK;
}

core::Status applyConnectionParams(const std::map<std::string, std::string>& params,
                                   NetworkClientConfig& config,
                                   core::ErrorContext* ctx) {
    for (const auto& entry : params) {
        const std::string& key = entry.first;
        const std::string& value = entry.second;
        if (key == "server" || key == "host") {
            if (value.rfind("unix:", 0) == 0) {
                config.transport_mode = "local_ipc";
                config.front_door_mode = "direct";
                config.ipc_method = "unix";
                config.ipc_path = value.substr(5);
                continue;
            }
            config.host = value;
        } else if (key == "port") {
            uint32_t parsed = 0;
            if (!parseUint32(value, parsed) || parsed > 65535) {
                if (ctx) {
                    ctx->message = "Invalid port";
                }
                return core::Status::INVALID_ARGUMENT;
            }
            config.port = static_cast<uint16_t>(parsed);
        } else if (key == "database" || key == "db" || key == "dbname") {
            config.database = value;
        } else if (key == "uid" || key == "user" || key == "username") {
            config.username = value;
        } else if (key == "pwd" || key == "password") {
            config.password = value;
        } else if (key == "role") {
            config.role = value;
        } else if (key == "schema" || key == "current_schema" ||
                   key == "currentschema" || key == "search_path" ||
                   key == "searchpath") {
            config.schema = value;
        } else if (key == "transport" || key == "transport_mode" || key == "mode") {
            std::string normalized;
            if (!parseTransportMode(value, normalized)) {
                if (ctx) {
                    ctx->message =
                        "transport_mode must be embedded, inet_listener, local_ipc, or managed";
                }
                return core::Status::INVALID_ARGUMENT;
            }
            config.transport_mode = normalized;
            if (normalized == "managed") {
                config.front_door_mode = "manager_proxy";
            } else if ((normalized == "embedded" || normalized == "inet_listener" ||
                        normalized == "local_ipc") &&
                       config.front_door_mode.empty()) {
                config.front_door_mode = "direct";
            }
        } else if (key == "ipc_method") {
            std::string normalized;
            if (!parseIpcMethod(value, normalized)) {
                if (ctx) {
                    ctx->message = "ipc_method must be auto, unix, pipe, or tcp";
                }
                return core::Status::INVALID_ARGUMENT;
            }
            config.ipc_method = normalized;
            if (normalized != "tcp") {
                config.transport_mode = "local_ipc";
                config.front_door_mode = "direct";
            }
        } else if (key == "ipc_path" || key == "socket_path" || key == "pipe_name") {
            config.ipc_path = value;
            if (!value.empty()) {
                config.transport_mode = "local_ipc";
                config.front_door_mode = "direct";
            }
        } else if (key == "protocol" || key == "parser" || key == "dialect") {
            std::string normalized;
            if (!parseProtocol(value, normalized)) {
                if (ctx) {
                    ctx->message = "Only protocol=native is supported; connect to the native parser listener/port.";
                }
                return core::Status::INVALID_ARGUMENT;
            }
            config.protocol = normalized;
        } else if (key == "front_door_mode" || key == "frontdoormode" ||
                   key == "connection_mode" || key == "ingress_mode") {
            std::string normalized;
            if (!parseFrontDoorMode(value, normalized)) {
                if (ctx) {
                    ctx->message = "front_door_mode must be direct or manager_proxy";
                }
                return core::Status::INVALID_ARGUMENT;
            }
            config.front_door_mode = normalized;
            if (normalized == "manager_proxy") {
                config.transport_mode = "managed";
            }
        } else if (key == "manager_auth_token" || key == "mcp_auth_token" ||
                   key == "managerauthtoken") {
            config.manager_auth_token = value;
        } else if (key == "manager_username" || key == "mcp_username") {
            config.manager_username = value;
        } else if (key == "manager_database" || key == "mcp_database") {
            config.manager_database = value;
        } else if (key == "manager_connection_profile" || key == "mcp_connection_profile") {
            config.manager_connection_profile = value;
        } else if (key == "manager_client_intent" || key == "mcp_client_intent") {
            config.manager_client_intent = value;
        } else if (key == "manager_client_flags" || key == "mcp_client_flags") {
            uint32_t parsed = 0;
            if (!parseUint32(value, parsed) || parsed > 65535u) {
                if (ctx) {
                    ctx->message = "Invalid manager_client_flags";
                }
                return core::Status::INVALID_ARGUMENT;
            }
            config.manager_client_flags = static_cast<uint16_t>(parsed);
        } else if (key == "manager_auth_fast_path" || key == "mcp_auth_fast_path") {
            bool parsed = false;
            if (parseBool(value, parsed)) {
                config.manager_auth_fast_path = parsed;
            }
        } else if (key == "client_flags" || key == "connect_client_flags") {
            uint32_t parsed = 0;
            if (!parseUint32(value, parsed) || parsed > 65535u) {
                if (ctx) {
                    ctx->message = "Invalid connect_client_flags";
                }
                return core::Status::INVALID_ARGUMENT;
            }
            config.connect_client_flags = static_cast<uint16_t>(parsed);
        } else if (key == "auth_method_id" || key == "authmethodid") {
            config.auth_method_id = trim(value);
        } else if (key == "auth_token" || key == "authtoken" ||
                   key == "bearer_token" || key == "bearertoken" ||
                   key == "token") {
            config.auth_token = value;
        } else if (key == "auth_method_payload" || key == "authmethodpayload") {
            config.auth_method_payload = value;
        } else if (key == "auth_payload_json" || key == "authpayloadjson") {
            config.auth_payload_json = value;
        } else if (key == "auth_payload_b64" || key == "authpayloadb64") {
            config.auth_payload_b64 = value;
        } else if (key == "auth_provider_profile" || key == "authproviderprofile") {
            config.auth_provider_profile = trim(value);
        } else if (key == "auth_required_methods" || key == "authrequiredmethods") {
            config.auth_required_methods = parseMethodList(value);
        } else if (key == "auth_forbidden_methods" || key == "authforbiddenmethods") {
            config.auth_forbidden_methods = parseMethodList(value);
        } else if (key == "auth_require_channel_binding" || key == "authrequirechannelbinding") {
            bool parsed = false;
            if (!parseBool(value, parsed)) {
                if (ctx) {
                    ctx->message = "Invalid auth_require_channel_binding (expected true/false)";
                }
                return core::Status::INVALID_ARGUMENT;
            }
            config.auth_require_channel_binding = parsed;
        } else if (key == "workload_identity_token" || key == "workloadidentitytoken") {
            config.workload_identity_token = value;
        } else if (key == "proxy_principal_assertion" || key == "proxyprincipalassertion" ||
                   key == "proxy_assertion") {
            config.proxy_principal_assertion = value;
        } else if (key == "applicationname" || key == "application_name" || key == "app") {
            config.application_name = value;
        } else if (key == "ssl" || key == "sslmode") {
            config.ssl_mode = parseSslMode(value);
        } else if (key == "sslcert") {
            config.ssl_cert = value;
        } else if (key == "sslkey") {
            config.ssl_key = value;
        } else if (key == "sslrootcert") {
            config.ssl_root_cert = value;
        } else if (key == "binary_transfer" || key == "binarytransfer") {
            bool parsed = false;
            if (!parseBool(value, parsed)) {
                if (ctx) {
                    ctx->message = "Invalid binary_transfer (expected true/false)";
                }
                return core::Status::INVALID_ARGUMENT;
            }
            config.binary_transfer = parsed;
        } else if (key == "enable_copy_streaming" || key == "copystreaming" ||
                   key == "copy_streaming") {
            bool parsed = false;
            if (!parseBool(value, parsed)) {
                if (ctx) {
                    ctx->message = "Invalid enable_copy_streaming (expected true/false)";
                }
                return core::Status::INVALID_ARGUMENT;
            }
            config.enable_copy_streaming = parsed;
        } else if (key == "compression") {
            bool compression_enabled = false;
            if (!parseCompressionMode(value, compression_enabled)) {
                if (ctx) {
                    ctx->message = "compression must be off or zstd";
                }
                return core::Status::INVALID_ARGUMENT;
            }
            config.enable_compression = compression_enabled;
        } else if (key == "connecttimeout" || key == "connect_timeout" || key == "timeout") {
            uint32_t ms = normalizeTimeoutMs(key, value);
            if (ms > 0) {
                config.connect_timeout_ms = ms;
            }
        } else if (key == "readtimeout" || key == "read_timeout") {
            uint32_t ms = normalizeTimeoutMs(key, value);
            if (ms > 0) {
                config.read_timeout_ms = ms;
            }
        } else if (key == "writetimeout" || key == "write_timeout") {
            uint32_t ms = normalizeTimeoutMs(key, value);
            if (ms > 0) {
                config.write_timeout_ms = ms;
            }
        } else if (key == "querytimeout" || key == "query_timeout") {
            uint32_t ms = normalizeTimeoutMs(key, value);
            if (ms > 0) {
                config.read_timeout_ms = ms;
                config.write_timeout_ms = ms;
            }
        } else if (key == "authmethod" || key == "auth_method") {
            bool ok = false;
            config.auth_method = parseAuthMethod(value, &ok);
            if (!ok && ctx) {
                ctx->message = "Unsupported auth_method";
            }
            if (!ok) {
                return core::Status::INVALID_ARGUMENT;
            }
        } else if (key == "allowpasswordfallback") {
            bool parsed = false;
            if (parseBool(value, parsed)) {
                config.allow_password_fallback = parsed;
            }
        } else if (key == "dsn" && config.host.empty()) {
            config.host = value;
        }
    }

    std::string overlap_method;
    if (hasOverlappingMethods(config.auth_required_methods,
                              config.auth_forbidden_methods,
                              overlap_method)) {
        if (ctx) {
            ctx->message = "Auth pinning profile is invalid: method appears in both required and forbidden sets (" +
                           overlap_method + ")";
        }
        return core::Status::INVALID_ARGUMENT;
    }

    return core::Status::OK;
}

core::Status parseDriverConnectionString(const std::string& conn_str,
                                         NetworkClientConfig& config,
                                         core::ErrorContext* ctx) {
    std::string trimmed = trim(conn_str);
    if (trimmed.rfind("scratchbird://", 0) == 0) {
        auto status = parseScratchbirdUrl(trimmed, config, ctx);
        if (status != core::Status::OK) {
            return status;
        }
    } else {
        std::map<std::string, std::string> params;
        auto status = parseKeyValueConnectionString(trimmed, params, ctx);
        if (status != core::Status::OK) {
            return status;
        }
        status = applyConnectionParams(params, config, ctx);
        if (status != core::Status::OK) {
            return status;
        }
    }

    applyDriverDefaults(config);

    std::string overlap_method;
    if (hasOverlappingMethods(config.auth_required_methods,
                              config.auth_forbidden_methods,
                              overlap_method)) {
        if (ctx) {
            ctx->message =
                "Auth pinning profile is invalid: method appears in both required and forbidden sets (" +
                overlap_method + ")";
        }
        return core::Status::INVALID_ARGUMENT;
    }
    return core::Status::OK;
}

void applyDriverDefaults(NetworkClientConfig& config) {
    if (config.transport_mode.empty()) {
        config.transport_mode = "inet_listener";
    }
    if (config.host.empty()) {
        config.host = "127.0.0.1";
    }
    if (config.application_name.empty()) {
        config.application_name = "scratchbird_driver";
    }
    if (config.front_door_mode.empty()) {
        config.front_door_mode = (toLower(config.transport_mode) == "managed") ? "manager_proxy" : "direct";
    }
    applyDriverDefaultsFromEnv(config);

    std::string normalized_transport;
    if (!parseTransportMode(config.transport_mode, normalized_transport)) {
        normalized_transport = "inet_listener";
    }
    config.transport_mode = normalized_transport;

    std::string normalized_front_door;
    if (!parseFrontDoorMode(config.front_door_mode, normalized_front_door)) {
        normalized_front_door = "direct";
    }
    config.front_door_mode = normalized_front_door;

    if (config.front_door_mode == "manager_proxy") {
        config.transport_mode = "managed";
    }
    if (config.transport_mode == "managed") {
        config.front_door_mode = "manager_proxy";
    }

    if (config.manager_username.empty()) {
        config.manager_username = config.username.empty() ? "admin" : config.username;
    }
    if (config.manager_database.empty()) {
        config.manager_database = config.database;
    }
    if (config.manager_connection_profile.empty()) {
        config.manager_connection_profile = "SBsql";
    }
    if (config.manager_client_intent.empty()) {
        config.manager_client_intent = "SBsql";
    }
}

void applyDriverDefaultsFromEnv(NetworkClientConfig& config) {
    constexpr const char* kDefaultSessionSchema = "users.public";
    auto getEnv = [](const char* key) -> const char* {
        return std::getenv(key);
    };
    auto parseU32 = [](const char* value, uint32_t& out) -> bool {
        if (!value) {
            return false;
        }
        char* end = nullptr;
        unsigned long parsed = std::strtoul(value, &end, 10);
        if (!end || *end != '\0') {
            return false;
        }
        out = static_cast<uint32_t>(parsed);
        return true;
    };
    auto parseU16 = [&](const char* value, uint16_t& out) -> bool {
        uint32_t tmp = 0;
        if (!parseU32(value, tmp) || tmp > 65535u) {
            return false;
        }
        out = static_cast<uint16_t>(tmp);
        return true;
    };

    if (const char* host = getEnv("SCRATCHBIRD_HOST")) {
        config.host = host;
    }
    if (const char* port = getEnv("SCRATCHBIRD_PORT")) {
        parseU16(port, config.port);
    }
    if (const char* db = getEnv("SCRATCHBIRD_DB")) {
        config.database = db;
    }
    if (const char* user = getEnv("SCRATCHBIRD_USER")) {
        config.username = user;
    }
    if (const char* pass = getEnv("SCRATCHBIRD_PASSWORD")) {
        config.password = pass;
    }
    if (const char* protocol = getEnv("SCRATCHBIRD_PROTOCOL")) {
        std::string normalized;
        if (parseProtocol(protocol, normalized)) {
            config.protocol = normalized;
        } else {
            config.protocol = "native";
        }
    }
    if (const char* parser = getEnv("SCRATCHBIRD_PARSER")) {
        std::string normalized;
        if (parseProtocol(parser, normalized)) {
            config.protocol = normalized;
        } else {
            config.protocol = "native";
        }
    }
    if (const char* transport = getEnv("SCRATCHBIRD_TRANSPORT_MODE")) {
        std::string normalized;
        if (parseTransportMode(transport, normalized)) {
            config.transport_mode = normalized;
        }
    }
    if (const char* transport = getEnv("SCRATCHBIRD_MODE")) {
        std::string normalized;
        if (parseTransportMode(transport, normalized)) {
            config.transport_mode = normalized;
        }
    }
    if (const char* ipc_method = getEnv("SCRATCHBIRD_IPC_METHOD")) {
        std::string normalized;
        if (parseIpcMethod(ipc_method, normalized)) {
            config.ipc_method = normalized;
        }
    }
    if (const char* ipc_path = getEnv("SCRATCHBIRD_IPC_PATH")) {
        config.ipc_path = ipc_path;
        if (config.transport_mode != "managed") {
            config.transport_mode = "local_ipc";
        }
    }
    if (const char* role = getEnv("SCRATCHBIRD_ROLE")) {
        config.role = role;
    }
    if (const char* front_door_mode = getEnv("SCRATCHBIRD_FRONT_DOOR_MODE")) {
        std::string normalized;
        if (parseFrontDoorMode(front_door_mode, normalized)) {
            config.front_door_mode = normalized;
        }
    }
    if (const char* manager_token = getEnv("SCRATCHBIRD_MANAGER_AUTH_TOKEN")) {
        config.manager_auth_token = manager_token;
    }
    if (const char* auth_token = getEnv("SCRATCHBIRD_AUTH_TOKEN")) {
        config.auth_token = auth_token;
    }
    if (const char* manager_user = getEnv("SCRATCHBIRD_MANAGER_USERNAME")) {
        config.manager_username = manager_user;
    }
    if (const char* manager_db = getEnv("SCRATCHBIRD_MANAGER_DATABASE")) {
        config.manager_database = manager_db;
    }
    if (const char* manager_profile = getEnv("SCRATCHBIRD_MANAGER_CONNECTION_PROFILE")) {
        config.manager_connection_profile = manager_profile;
    }
    if (const char* manager_intent = getEnv("SCRATCHBIRD_MANAGER_CLIENT_INTENT")) {
        config.manager_client_intent = manager_intent;
    }
    if (const char* manager_flags = getEnv("SCRATCHBIRD_MANAGER_CLIENT_FLAGS")) {
        uint32_t parsed = 0;
        if (parseU32(manager_flags, parsed) && parsed <= 65535u) {
            config.manager_client_flags = static_cast<uint16_t>(parsed);
        }
    }
    if (const char* manager_fast_path = getEnv("SCRATCHBIRD_MANAGER_AUTH_FAST_PATH")) {
        config.manager_auth_fast_path = (std::string(manager_fast_path) == "1" ||
                                         std::string(manager_fast_path) == "true" ||
                                         std::string(manager_fast_path) == "TRUE");
    }
    if (const char* schema = getEnv("SCRATCHBIRD_SCHEMA")) {
        config.schema = schema;
    }
    if (const char* app = getEnv("SCRATCHBIRD_APP_NAME")) {
        config.application_name = app;
    }
    if (const char* sslmode = getEnv("SCRATCHBIRD_SSLMODE")) {
        config.ssl_mode = parseSslMode(sslmode);
    }
    if (const char* ssl_cert = getEnv("SCRATCHBIRD_SSL_CERT")) {
        config.ssl_cert = ssl_cert;
    }
    if (const char* ssl_key = getEnv("SCRATCHBIRD_SSL_KEY")) {
        config.ssl_key = ssl_key;
    }
    if (const char* ssl_root = getEnv("SCRATCHBIRD_SSL_ROOT_CERT")) {
        config.ssl_root_cert = ssl_root;
    }
    if (const char* auth = getEnv("SCRATCHBIRD_AUTH_METHOD")) {
        bool ok = false;
        auto method = parseAuthMethod(auth, &ok);
        if (ok) {
            config.auth_method = method;
        }
    }
    if (const char* client_flags = getEnv("SCRATCHBIRD_CONNECT_CLIENT_FLAGS")) {
        uint32_t parsed = 0;
        if (parseU32(client_flags, parsed) && parsed <= 65535u) {
            config.connect_client_flags = static_cast<uint16_t>(parsed);
        }
    }
    if (const char* method_id = getEnv("SCRATCHBIRD_AUTH_METHOD_ID")) {
        config.auth_method_id = trim(method_id);
    }
    if (const char* method_payload = getEnv("SCRATCHBIRD_AUTH_METHOD_PAYLOAD")) {
        config.auth_method_payload = method_payload;
    }
    if (const char* payload_json = getEnv("SCRATCHBIRD_AUTH_PAYLOAD_JSON")) {
        config.auth_payload_json = payload_json;
    }
    if (const char* payload_b64 = getEnv("SCRATCHBIRD_AUTH_PAYLOAD_B64")) {
        config.auth_payload_b64 = payload_b64;
    }
    if (const char* provider_profile = getEnv("SCRATCHBIRD_AUTH_PROVIDER_PROFILE")) {
        config.auth_provider_profile = trim(provider_profile);
    }
    if (const char* required = getEnv("SCRATCHBIRD_AUTH_REQUIRED_METHODS")) {
        config.auth_required_methods = parseMethodList(required);
    }
    if (const char* forbidden = getEnv("SCRATCHBIRD_AUTH_FORBIDDEN_METHODS")) {
        config.auth_forbidden_methods = parseMethodList(forbidden);
    }
    if (const char* require_cb = getEnv("SCRATCHBIRD_AUTH_REQUIRE_CHANNEL_BINDING")) {
        bool parsed = false;
        if (parseBool(require_cb, parsed)) {
            config.auth_require_channel_binding = parsed;
        }
    }
    if (const char* workload_token = getEnv("SCRATCHBIRD_WORKLOAD_IDENTITY_TOKEN")) {
        config.workload_identity_token = workload_token;
    }
    if (const char* proxy_assertion = getEnv("SCRATCHBIRD_PROXY_PRINCIPAL_ASSERTION")) {
        config.proxy_principal_assertion = proxy_assertion;
    }
    if (const char* allow_pw = getEnv("SCRATCHBIRD_ALLOW_PASSWORD_FALLBACK")) {
        config.allow_password_fallback = (std::string(allow_pw) == "1" ||
                                          std::string(allow_pw) == "true" ||
                                          std::string(allow_pw) == "TRUE");
    }
    if (const char* binary_transfer = getEnv("SCRATCHBIRD_BINARY_TRANSFER")) {
        bool parsed = false;
        if (parseBool(binary_transfer, parsed)) {
            config.binary_transfer = parsed;
        }
    }
    if (const char* compression_mode = getEnv("SCRATCHBIRD_COMPRESSION")) {
        bool compression_enabled = false;
        if (parseCompressionMode(compression_mode, compression_enabled)) {
            config.enable_compression = compression_enabled;
        }
    }
    if (const char* compression = getEnv("SCRATCHBIRD_ENABLE_COMPRESSION")) {
        config.enable_compression = (std::string(compression) == "1" ||
                                     std::string(compression) == "true" ||
                                     std::string(compression) == "TRUE");
    }
    if (const char* connect_to = getEnv("SCRATCHBIRD_CONNECT_TIMEOUT_MS")) {
        parseU32(connect_to, config.connect_timeout_ms);
    }
    if (const char* read_to = getEnv("SCRATCHBIRD_READ_TIMEOUT_MS")) {
        parseU32(read_to, config.read_timeout_ms);
    }
    if (const char* write_to = getEnv("SCRATCHBIRD_WRITE_TIMEOUT_MS")) {
        parseU32(write_to, config.write_timeout_ms);
    }
    if (const char* copy_window = getEnv("SCRATCHBIRD_COPY_WINDOW_BYTES")) {
        parseU32(copy_window, config.copy_window_bytes);
    }
    if (const char* copy_chunk = getEnv("SCRATCHBIRD_COPY_CHUNK_BYTES")) {
        parseU32(copy_chunk, config.copy_chunk_bytes);
    }
    if (const char* copy_streaming = getEnv("SCRATCHBIRD_ENABLE_COPY_STREAMING")) {
        bool parsed = false;
        if (parseBool(copy_streaming, parsed)) {
            config.enable_copy_streaming = parsed;
        }
    }

    if (config.schema.empty()) {
        config.schema = kDefaultSessionSchema;
    }

    // Backward-compatible driver-prefixed environment names.
    if (const char* driver_host = getEnv("SCRATCHBIRD_DRIVER_HOST")) {
        if (config.host.empty() || config.host == "127.0.0.1") {
            config.host = driver_host;
        }
    }
    if (const char* driver_port = getEnv("SCRATCHBIRD_DRIVER_PORT")) {
        if (config.port == network::DEFAULT_NATIVE_PORT) {
            parseU16(driver_port, config.port);
        }
    }
    if (const char* driver_ipc_method = getEnv("SCRATCHBIRD_DRIVER_IPC_METHOD")) {
        std::string normalized;
        if (parseIpcMethod(driver_ipc_method, normalized) &&
            (config.ipc_method.empty() || config.ipc_method == "auto")) {
            config.ipc_method = normalized;
        }
    }
    if (const char* driver_ipc_path = getEnv("SCRATCHBIRD_DRIVER_IPC_PATH")) {
        if (config.ipc_path.empty()) {
            config.ipc_path = driver_ipc_path;
            if (config.transport_mode != "managed") {
                config.transport_mode = "local_ipc";
            }
        }
    }
    if (const char* driver_db = getEnv("SCRATCHBIRD_DRIVER_DATABASE")) {
        if (config.database.empty()) {
            config.database = driver_db;
        }
    }
    if (const char* driver_app = getEnv("SCRATCHBIRD_DRIVER_APPLICATION_NAME")) {
        if (config.application_name.empty() || config.application_name == "scratchbird_odbc" ||
            config.application_name == "scratchbird_driver") {
            config.application_name = driver_app;
        }
    }
    if (const char* driver_sslmode = getEnv("SCRATCHBIRD_DRIVER_SSLMODE")) {
        config.ssl_mode = parseSslMode(driver_sslmode);
    }
    if (const char* driver_connect_to = getEnv("SCRATCHBIRD_DRIVER_CONNECT_TIMEOUT_MS")) {
        parseU32(driver_connect_to, config.connect_timeout_ms);
    }
    if (const char* driver_ssl_cert = getEnv("SCRATCHBIRD_DRIVER_SSL_CERT")) {
        config.ssl_cert = driver_ssl_cert;
    }
    if (const char* driver_ssl_key = getEnv("SCRATCHBIRD_DRIVER_SSL_KEY")) {
        config.ssl_key = driver_ssl_key;
    }
    if (const char* driver_ssl_root = getEnv("SCRATCHBIRD_DRIVER_SSL_ROOT_CERT")) {
        config.ssl_root_cert = driver_ssl_root;
    }
}

} // namespace client
} // namespace scratchbird
