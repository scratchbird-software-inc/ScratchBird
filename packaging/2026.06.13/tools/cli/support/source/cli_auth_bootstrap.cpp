// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cli_auth_bootstrap.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace scratchbird::cli {

namespace {

void appendConnParam(std::vector<std::pair<std::string, std::string>>& params,
                     const std::string& key,
                     const std::string& value) {
    if (!key.empty() && !value.empty()) {
        params.emplace_back(key, value);
    }
}

std::string encodeConnectionValue(const std::string& value) {
    if (value.find(';') != std::string::npos || value.find(' ') != std::string::npos) {
        return "{" + value + "}";
    }
    return value;
}

std::string jsonEscape(const std::string& value) {
    std::ostringstream out;
    for (unsigned char ch : value) {
        switch (ch) {
        case '\\': out << "\\\\"; break;
        case '"': out << "\\\""; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (ch < 0x20) {
                static const char* hex = "0123456789abcdef";
                out << "\\u00" << hex[(ch >> 4) & 0x0f] << hex[ch & 0x0f];
            } else {
                out << static_cast<char>(ch);
            }
            break;
        }
    }
    return out.str();
}

const char* boolText(bool value) {
    return value ? "true" : "false";
}

} // namespace

std::string normalizeConnectionMode(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    if (value == "embedded" || value == "inproc" || value == "in-process" || value == "in_process") {
        return "embedded";
    }
    if (value == "local" || value == "ipc" || value == "local-ipc" || value == "local_ipc" ||
        value == "unix" || value == "pipe") {
        return "local_ipc";
    }
    if (value == "inet" || value == "listener" || value == "inet_listener" ||
        value == "tcp" || value == "network" || value.empty()) {
        return "inet_listener";
    }
    if (value == "managed" || value == "manager" || value == "manager_proxy" || value == "manager-proxy") {
        return "managed";
    }
    return {};
}

bool splitConnOption(const std::string& value, std::string& key, std::string& out_value) {
    const size_t eq = value.find('=');
    if (eq == std::string::npos || eq == 0 || eq + 1 >= value.size()) {
        return false;
    }
    key = value.substr(0, eq);
    out_value = value.substr(eq + 1);
    return !key.empty();
}

bool isLikelyConnectionString(const std::string& value) {
    if (value.rfind("scratchbird://", 0) == 0) {
        return true;
    }
    return value.find('=') != std::string::npos && value.find(';') != std::string::npos;
}

std::string buildConnectionTarget(const ConnectionBootstrapOptions& options,
                                  const std::string& database_override) {
    if (!database_override.empty() && isLikelyConnectionString(database_override)) {
        return database_override;
    }
    if (!options.connection_string.empty()) {
        return options.connection_string;
    }
    if (database_override.empty() && isLikelyConnectionString(options.database_path)) {
        return options.database_path;
    }

    std::string mode = normalizeConnectionMode(options.mode);
    if (mode.empty()) {
        mode = "inet_listener";
    }

    std::string ipc_method = options.ipc_method.empty() ? "auto" : options.ipc_method;
    std::string ipc_method_lower = ipc_method;
    std::transform(ipc_method_lower.begin(), ipc_method_lower.end(), ipc_method_lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    const bool tcp_listener_mode = (mode == "local_ipc" && ipc_method_lower == "tcp");
    const std::string transport_mode = tcp_listener_mode ? "inet_listener" : mode;

    std::vector<std::pair<std::string, std::string>> params;
    appendConnParam(params, "database", database_override.empty() ? options.database_path : database_override);
    appendConnParam(params, "protocol", "native");
    appendConnParam(params, "transport_mode", transport_mode);

    if (transport_mode == "local_ipc") {
        appendConnParam(params, "ipc_method", ipc_method);
        appendConnParam(params, "ipc_path", options.ipc_path);
    } else {
        appendConnParam(params, "host", options.host.empty() ? "127.0.0.1" : options.host);
        appendConnParam(params, "port", std::to_string(options.port));
    }

    std::string front_door = options.front_door_mode;
    if (transport_mode == "managed") {
        front_door = "manager_proxy";
    }
    appendConnParam(params, "front_door_mode", front_door);
    appendConnParam(params, "manager_auth_token", options.manager_auth_token);
    appendConnParam(params, "manager_username", options.manager_username);
    appendConnParam(params, "manager_database", options.manager_database);
    appendConnParam(params, "manager_connection_profile", options.manager_connection_profile);
    appendConnParam(params, "manager_client_intent", options.manager_client_intent);
    appendConnParam(params, "manager_client_flags", options.manager_client_flags);
    appendConnParam(params, "manager_auth_fast_path", options.manager_auth_fast_path);
    appendConnParam(params, "client_flags", options.connect_client_flags);
    appendConnParam(params, "auth_method_id", options.auth_method_id);
    appendConnParam(params, "auth_token", options.auth_token);
    appendConnParam(params, "auth_method_payload", options.auth_method_payload);
    appendConnParam(params, "auth_payload_json", options.auth_payload_json);
    appendConnParam(params, "auth_payload_b64", options.auth_payload_b64);
    appendConnParam(params, "auth_provider_profile", options.auth_provider_profile);
    appendConnParam(params, "auth_required_methods", options.auth_required_methods);
    appendConnParam(params, "auth_forbidden_methods", options.auth_forbidden_methods);
    appendConnParam(params, "auth_require_channel_binding", options.auth_require_channel_binding);
    appendConnParam(params, "workload_identity_token", options.workload_identity_token);
    appendConnParam(params, "proxy_principal_assertion", options.proxy_principal_assertion);
    appendConnParam(params, "sslmode", options.ssl_mode);

    for (const auto& kv : options.conn_options) {
        appendConnParam(params, kv.first, kv.second);
    }

    std::ostringstream conn;
    for (size_t i = 0; i < params.size(); ++i) {
        if (i > 0) {
            conn << ';';
        }
        conn << params[i].first << '=' << encodeConnectionValue(params[i].second);
    }
    return conn.str();
}

std::string renderAuthProbeText(const client::AuthProbeResult& probe) {
    std::ostringstream out;
    out << "reachable=" << boolText(probe.reachable) << '\n'
        << "ingress_mode=" << probe.ingress_mode << '\n'
        << "resolved_host=" << probe.resolved_host << '\n'
        << "resolved_port=" << probe.resolved_port << '\n'
        << "required_method=" << probe.required_method << '\n'
        << "required_plugin_method_id=" << probe.required_plugin_method_id << '\n'
        << "additional_continuation_possible=" << boolText(probe.additional_continuation_possible) << '\n';
    for (size_t i = 0; i < probe.admitted_methods.size(); ++i) {
        const auto& method = probe.admitted_methods[i];
        out << "admitted[" << i << "].wire_method=" << method.wire_method << '\n'
            << "admitted[" << i << "].plugin_method_id=" << method.plugin_method_id << '\n'
            << "admitted[" << i << "].executable_locally=" << boolText(method.executable_locally) << '\n'
            << "admitted[" << i << "].broker_required=" << boolText(method.broker_required) << '\n';
    }
    return out.str();
}

std::string renderResolvedAuthContextText(const client::ResolvedAuthContext& context) {
    std::ostringstream out;
    out << "ingress_mode=" << context.ingress_mode << '\n'
        << "resolved_auth_method=" << context.resolved_auth_method << '\n'
        << "resolved_auth_plugin_id=" << context.resolved_auth_plugin_id << '\n'
        << "manager_authenticated=" << boolText(context.manager_authenticated) << '\n'
        << "attached=" << boolText(context.attached) << '\n';
    return out.str();
}

std::string renderAuthProbeJson(const client::AuthProbeResult& probe) {
    std::ostringstream out;
    out << '{'
        << "\"reachable\":" << boolText(probe.reachable) << ','
        << "\"ingress_mode\":\"" << jsonEscape(probe.ingress_mode) << "\","
        << "\"resolved_host\":\"" << jsonEscape(probe.resolved_host) << "\","
        << "\"resolved_port\":" << probe.resolved_port << ','
        << "\"required_method\":\"" << jsonEscape(probe.required_method) << "\","
        << "\"required_plugin_method_id\":\"" << jsonEscape(probe.required_plugin_method_id) << "\","
        << "\"additional_continuation_possible\":" << boolText(probe.additional_continuation_possible) << ','
        << "\"admitted_methods\":[";
    for (size_t i = 0; i < probe.admitted_methods.size(); ++i) {
        if (i > 0) {
            out << ',';
        }
        const auto& method = probe.admitted_methods[i];
        out << '{'
            << "\"wire_method\":\"" << jsonEscape(method.wire_method) << "\","
            << "\"plugin_method_id\":\"" << jsonEscape(method.plugin_method_id) << "\","
            << "\"executable_locally\":" << boolText(method.executable_locally) << ','
            << "\"broker_required\":" << boolText(method.broker_required)
            << '}';
    }
    out << "]}";
    return out.str();
}

std::string renderResolvedAuthContextJson(const client::ResolvedAuthContext& context) {
    std::ostringstream out;
    out << '{'
        << "\"ingress_mode\":\"" << jsonEscape(context.ingress_mode) << "\","
        << "\"resolved_auth_method\":\"" << jsonEscape(context.resolved_auth_method) << "\","
        << "\"resolved_auth_plugin_id\":\"" << jsonEscape(context.resolved_auth_plugin_id) << "\","
        << "\"manager_authenticated\":" << boolText(context.manager_authenticated) << ','
        << "\"attached\":" << boolText(context.attached)
        << '}';
    return out.str();
}

} // namespace scratchbird::cli
