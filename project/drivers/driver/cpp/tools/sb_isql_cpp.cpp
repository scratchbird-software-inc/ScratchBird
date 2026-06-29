// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "scratchbird/client/connection.h"
#include "sb_statement_chunker.hpp"

#include <openssl/sha.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

using json = nlohmann::json;

namespace {

const std::set<std::string> kPageSizes{"4k", "8k", "16k", "32k", "64k", "128k"};
const std::set<std::string> kRoutes{"embedded", "ipc_local", "listener-parser", "manager-listener-parser"};
const std::set<std::string> kParserModes{"server-parser", "standalone-parser", "driver-sblr-uuid"};
const std::set<std::string> kSslModes{"disable", "allow", "prefer", "require", "verify-ca", "verify-full"};
const std::set<std::string> kSupportedArgs{
    "--database",
    "--host",
    "--port",
    "--user",
    "--password",
    "--role",
    "--sslmode",
    "--sslrootcert",
    "--sslcert",
    "--sslkey",
    "--ipc-path",
    "--route",
    "--parser-mode",
    "--page-size",
    "--namespace",
    "--input",
    "--output",
    "--error",
    "--diagnostics",
    "--metrics",
    "--transcript",
    "--summary",
    "--stop-on-error",
    "--expected-refusals",
    "--statement-timeout-ms",
    "--fetch-size",
    "--concurrency-worker",
    "--create-database",
    "--create-emulation-mode",
    "--run-id",
    "--language-resource-pack",
    "--language-resource-identity",
    "--language-resource-hash",
    "--language-profile",
    "--syntax-profile",
    "--topology-profile",
    "--standard-english-fallback",
};

int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool networkRoute(const std::string& route) {
    return route == "listener-parser" || route == "manager-listener-parser";
}

uint32_t pageSizeBytesForLabel(const std::string& pageSize) {
    if (pageSize == "4k") return 4096;
    if (pageSize == "8k") return 8192;
    if (pageSize == "16k") return 16384;
    if (pageSize == "32k") return 32768;
    if (pageSize == "64k") return 65536;
    if (pageSize == "128k") return 131072;
    return 0;
}

std::string transportModeForRoute(const std::string& route, const std::string& sslmode) {
    if (route == "embedded") {
        return "embedded_no_network_transport";
    }
    if (route == "ipc_local") {
        return "local_ipc_no_tls";
    }
    return sslmode == "disable" ? "tls_disabled" : "tls_required";
}

std::string endpointKindForRoute(const std::string& route) {
    if (route == "embedded") {
        return "embedded_bridge";
    }
    if (route == "ipc_local") {
        return "unix_domain_socket";
    }
    return "tcp";
}

std::string transportImplementationForRoute(const std::string& route) {
    if (route == "embedded") {
        return "native_cpp_embedded_bridge";
    }
    if (route == "ipc_local") {
        return "native_cpp_local_ipc_bridge";
    }
    return "native_cpp_tcp";
}

std::string tlsPolicyForRoute(const std::string& route, const std::string& sslmode) {
    if (!networkRoute(route)) {
        return "not_applicable_non_network_route";
    }
    return sslmode == "disable" ? "explicit_non_tls_test_route" : "scratchbird_tls_1_3_floor";
}

std::string required(const std::map<std::string, std::string>& args, const std::string& key) {
    auto it = args.find(key);
    if (it == args.end() || it->second.empty()) {
        throw std::runtime_error("missing required argument " + key);
    }
    return it->second;
}

std::string valueOrDefault(const std::map<std::string, std::string>& args,
                           const std::string& key,
                           const std::string& fallback) {
    auto it = args.find(key);
    return it == args.end() ? fallback : it->second;
}

bool booleanArg(const std::map<std::string, std::string>& args,
                const std::string& key,
                bool fallback) {
    auto it = args.find(key);
    if (it == args.end()) {
        return fallback;
    }
    std::string value = it->second;
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    if (value == "true" || value == "1" || value == "yes" || value == "on") {
        return true;
    }
    if (value == "false" || value == "0" || value == "no" || value == "off") {
        return false;
    }
    throw std::runtime_error("invalid boolean value for " + key + ": " + it->second);
}

void writeText(const std::string& path, const std::string& text) {
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

void appendText(const std::string& path, const std::string& text) {
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }
    std::ofstream out(path, std::ios::binary | std::ios::app);
    out << text;
}

void appendJsonl(const std::string& path, const json& record) {
    appendText(path, record.dump() + "\n");
}

std::string sha256Text(const std::string& text) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(text.data()), text.size(), digest);
    std::ostringstream out;
    out << "sha256:";
    for (unsigned char byte : digest) {
        out << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    }
    return out.str();
}

std::string readInput(const std::string& path) {
    if (path == "-") {
        std::ostringstream buffer;
        buffer << std::cin.rdbuf();
        return buffer.str();
    }
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

void addExpectedRefusalIds(const json& value, std::set<std::string>& ids) {
    if (!value.is_array()) {
        return;
    }
    for (const auto& item : value) {
        if (item.is_string()) {
            ids.insert(item.get<std::string>());
        } else if (item.is_object() && item.contains("statement_id") && item["statement_id"].is_string()) {
            ids.insert(item["statement_id"].get<std::string>());
        }
    }
}

std::set<std::string> loadExpectedRefusals(const std::string& path) {
    if (path.empty()) {
        return {};
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("expected refusal file not found: " + path);
    }
    json doc;
    in >> doc;
    std::set<std::string> ids;
    if (doc.is_array()) {
        addExpectedRefusalIds(doc, ids);
        return ids;
    }
    if (!doc.is_object()) {
        throw std::runtime_error("expected refusals must be a JSON object or array");
    }
    if (doc.contains("statement_ids")) {
        addExpectedRefusalIds(doc["statement_ids"], ids);
    }
    if (doc.contains("expected_refusals")) {
        addExpectedRefusalIds(doc["expected_refusals"], ids);
    }
    if (doc.contains("expected_diagnostics") && doc["expected_diagnostics"].is_object()) {
        for (const auto& item : doc["expected_diagnostics"].items()) {
            ids.insert(item.key());
        }
    }
    return ids;
}

// The statement chunker (quote-aware split on the active terminator, SET TERM
// client directive, `--` comment-aware) lives in the shared header
// sb_statement_chunker.hpp so every C++ tool uses one identical implementation.
// Verified against tests/conformance/drivers/chunker_conformance/cases.json.

std::string stripLeadingTrivia(const std::string& sql) {
    size_t pos = 0;
    while (pos < sql.size()) {
        while (pos < sql.size() && std::isspace(static_cast<unsigned char>(sql[pos]))) {
            ++pos;
        }
        if (pos + 1 < sql.size() && sql[pos] == '-' && sql[pos + 1] == '-') {
            const size_t newline = sql.find('\n', pos + 2);
            if (newline == std::string::npos) {
                return "";
            }
            pos = newline + 1;
            continue;
        }
        if (pos + 1 < sql.size() && sql[pos] == '/' && sql[pos + 1] == '*') {
            const size_t close = sql.find("*/", pos + 2);
            if (close == std::string::npos) {
                return "";
            }
            pos = close + 2;
            continue;
        }
        break;
    }
    return sql.substr(pos);
}

std::string firstTokenLower(const std::string& sql) {
    std::istringstream in(stripLeadingTrivia(sql));
    std::string first;
    in >> first;
    for (char& ch : first) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return first;
}

std::vector<std::string> leadingTokensLower(const std::string& sql, std::size_t limit) {
    std::istringstream in(stripLeadingTrivia(sql));
    std::vector<std::string> tokens;
    std::string token;
    while (tokens.size() < limit && in >> token) {
        for (char& ch : token) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        tokens.push_back(token);
    }
    return tokens;
}

std::string savepointName(const std::string& sql) {
    const auto tokens = leadingTokensLower(sql, 4);
    if (tokens.empty()) {
        return "";
    }
    if (tokens[0] == "savepoint" && tokens.size() >= 2) {
        return tokens[1];
    }
    if (tokens[0] == "release" && tokens.size() >= 3 && tokens[1] == "savepoint") {
        return tokens[2];
    }
    if (tokens[0] == "rollback" && tokens.size() >= 3 && tokens[1] == "to") {
        if (tokens[2] == "savepoint" && tokens.size() >= 4) {
            return tokens[3];
        }
        return tokens[2];
    }
    return "";
}

std::string lowerAscii(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

bool skipSqlTrivia(const std::string& text, std::size_t* pos) {
    bool advanced = false;
    while (*pos < text.size()) {
        while (*pos < text.size() && std::isspace(static_cast<unsigned char>(text[*pos]))) {
            ++(*pos);
            advanced = true;
        }
        if (*pos + 1 < text.size() && text[*pos] == '-' && text[*pos + 1] == '-') {
            const std::size_t newline = text.find('\n', *pos + 2);
            *pos = newline == std::string::npos ? text.size() : newline + 1;
            advanced = true;
            continue;
        }
        if (*pos + 1 < text.size() && text[*pos] == '/' && text[*pos + 1] == '*') {
            const std::size_t close = text.find("*/", *pos + 2);
            *pos = close == std::string::npos ? text.size() : close + 2;
            advanced = true;
            continue;
        }
        break;
    }
    return advanced;
}

void skipSqlStringLiteral(const std::string& text, std::size_t* pos) {
    if (*pos >= text.size() || text[*pos] != '\'') {
        return;
    }
    ++(*pos);
    while (*pos < text.size()) {
        if (text[*pos] == '\'') {
            ++(*pos);
            if (*pos < text.size() && text[*pos] == '\'') {
                ++(*pos);
                continue;
            }
            return;
        }
        ++(*pos);
    }
}

void skipSqlQuotedIdentifier(const std::string& text, std::size_t* pos) {
    if (*pos >= text.size() || text[*pos] != '"') {
        return;
    }
    ++(*pos);
    while (*pos < text.size()) {
        if (text[*pos] == '"') {
            ++(*pos);
            if (*pos < text.size() && text[*pos] == '"') {
                ++(*pos);
                continue;
            }
            return;
        }
        ++(*pos);
    }
}

std::string readSqlTokenLower(const std::string& text, std::size_t* pos) {
    skipSqlTrivia(text, pos);
    if (*pos >= text.size()) {
        return "";
    }
    if (text[*pos] == '"') {
        std::string token;
        ++(*pos);
        while (*pos < text.size()) {
            if (text[*pos] == '"') {
                ++(*pos);
                if (*pos < text.size() && text[*pos] == '"') {
                    token.push_back('"');
                    ++(*pos);
                    continue;
                }
                break;
            }
            token.push_back(text[*pos]);
            ++(*pos);
        }
        return lowerAscii(token);
    }
    const std::size_t begin = *pos;
    while (*pos < text.size()) {
        const char ch = text[*pos];
        if (std::isspace(static_cast<unsigned char>(ch)) || ch == '(' || ch == ')' ||
            ch == ',' || ch == ';') {
            break;
        }
        if (ch == '-' && *pos + 1 < text.size() && text[*pos + 1] == '-') {
            break;
        }
        if (ch == '/' && *pos + 1 < text.size() && text[*pos + 1] == '*') {
            break;
        }
        ++(*pos);
    }
    return lowerAscii(text.substr(begin, *pos - begin));
}

bool skipSqlParenthesized(const std::string& text, std::size_t* pos) {
    skipSqlTrivia(text, pos);
    if (*pos >= text.size() || text[*pos] != '(') {
        return false;
    }
    int depth = 0;
    while (*pos < text.size()) {
        if (text[*pos] == '\'') {
            skipSqlStringLiteral(text, pos);
            continue;
        }
        if (text[*pos] == '"') {
            skipSqlQuotedIdentifier(text, pos);
            continue;
        }
        if (*pos + 1 < text.size() &&
            ((text[*pos] == '-' && text[*pos + 1] == '-') ||
             (text[*pos] == '/' && text[*pos + 1] == '*'))) {
            skipSqlTrivia(text, pos);
            continue;
        }
        if (text[*pos] == '(') {
            ++depth;
            ++(*pos);
            continue;
        }
        if (text[*pos] == ')') {
            --depth;
            ++(*pos);
            if (depth == 0) {
                return true;
            }
            continue;
        }
        ++(*pos);
    }
    return false;
}

std::string mainStatementTokenLower(const std::string& sql) {
    const std::string text = stripLeadingTrivia(sql);
    std::size_t pos = 0;
    std::string token = readSqlTokenLower(text, &pos);
    if (token != "with") {
        return token;
    }
    token = readSqlTokenLower(text, &pos);
    if (token == "recursive") {
        token = readSqlTokenLower(text, &pos);
    }
    while (!token.empty()) {
        skipSqlTrivia(text, &pos);
        if (pos < text.size() && text[pos] == '(' && !skipSqlParenthesized(text, &pos)) {
            return "with";
        }
        bool sawAs = false;
        for (int guard = 0; guard < 32; ++guard) {
            const std::string word = readSqlTokenLower(text, &pos);
            if (word.empty()) {
                return "with";
            }
            if (word == "as") {
                sawAs = true;
                break;
            }
        }
        if (!sawAs) {
            return "with";
        }
        std::size_t beforeOptional = pos;
        std::string optional = readSqlTokenLower(text, &pos);
        if (optional == "not") {
            std::size_t afterNot = pos;
            if (readSqlTokenLower(text, &pos) != "materialized") {
                pos = afterNot;
            }
        } else if (optional != "materialized") {
            pos = beforeOptional;
        }
        if (!skipSqlParenthesized(text, &pos)) {
            return "with";
        }
        skipSqlTrivia(text, &pos);
        if (pos < text.size() && text[pos] == ',') {
            ++pos;
            token = readSqlTokenLower(text, &pos);
            continue;
        }
        const std::string main = readSqlTokenLower(text, &pos);
        return main.empty() ? "with" : main;
    }
    return "with";
}

std::string copyInputForStatement(const std::string& sql) {
    static const std::string kMarker = "-- SB_COPY_INPUT ";
    std::istringstream lines(sql);
    std::string line;
    std::string payload;
    while (std::getline(lines, line)) {
        const size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) {
            continue;
        }
        if (line.compare(start, kMarker.size(), kMarker) != 0) {
            continue;
        }
        std::string row = line.substr(start + kMarker.size());
        if (!row.empty() && row.back() == '\r') {
            row.pop_back();
        }
        payload += row;
        payload += '\n';
    }
    return payload;
}

std::string classify(const std::string& sql) {
    const auto first = mainStatementTokenLower(sql);
    if (first == "graph" || first == "document" || first == "kv" || first == "timeseries" ||
        first == "fulltext" || first == "opensearch" || first == "search" || first == "reindex") {
        return "multimodel";
    }
    if (first == "backup" || first == "restore" || first == "archive" ||
        first == "replicate" || first == "changefeed") {
        return "archive";
    }
    if (first == "migrate" || first == "maintenance" || first == "repair" ||
        first == "config" || first == "storage" || first == "show") {
        return "admin";
    }
    if (first == "session" || first == "connect" || first == "disconnect") return "session";
    if (first == "create" || first == "alter" || first == "drop") return "ddl";
    if (first == "insert" || first == "update" || first == "delete" || first == "merge" || first == "upsert") return "dml";
    if (first == "commit" || first == "rollback" || first == "savepoint" ||
        first == "begin" || first == "start" || first == "release") {
        return "transaction";
    }
    if (first == "grant" || first == "revoke") return "security_refusal";
    if (first == "copy") return "copy";
    return sql.find("sys.") != std::string::npos ? "metadata" : "query";
}

bool statementReturnsRows(const std::string& sql) {
    const auto first = mainStatementTokenLower(sql);
    if (first == "select" || first == "values" || first == "show" || first == "explain") {
        return true;
    }
    std::string lowered = sql;
    for (char& ch : lowered) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return (" " + lowered + " ").find(" returning ") != std::string::npos;
}

json currentProcessMetrics() {
    long long vsizeKb = 1;
    long long rssKb = 1;
    std::ifstream statm("/proc/self/statm");
    long long sizePages = 0;
    long long residentPages = 0;
    if (statm >> sizePages >> residentPages) {
        constexpr long long kPageKb = 4;
        vsizeKb = std::max(1LL, sizePages * kPageKb);
        rssKb = std::max(1LL, residentPages * kPageKb);
    }
    return {{"client", {{"max_rss_kb", rssKb},
                        {"max_vsize_kb", vsizeKb},
                        {"last_rss_kb", rssKb},
                        {"last_vsize_kb", vsizeKb}}}};
}

std::map<std::string, std::string> parseArgs(int argc, char** argv) {
    std::map<std::string, std::string> args;
    for (int i = 1; i < argc; ++i) {
        std::string key(argv[i]);
        if (key.rfind("--", 0) != 0) {
            throw std::runtime_error("unexpected positional argument: " + key);
        }
        if (!kSupportedArgs.count(key)) {
            throw std::runtime_error("unsupported argument: " + key);
        }
        if (key == "--stop-on-error" || key == "--create-database") {
            if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) {
                args[key] = argv[++i];
            } else {
                args[key] = "true";
            }
            continue;
        }
        if (i + 1 >= argc || std::string(argv[i + 1]).rfind("--", 0) == 0) {
            throw std::runtime_error("missing value for " + key);
        }
        args[key] = argv[++i];
    }
    return args;
}

void validate(const std::map<std::string, std::string>& args) {
    if (!kPageSizes.count(required(args, "--page-size"))) throw std::runtime_error("unsupported page size");
    if (!kRoutes.count(required(args, "--route"))) throw std::runtime_error("unsupported route");
    if (!kParserModes.count(required(args, "--parser-mode"))) throw std::runtime_error("unsupported parser mode");
    if (!kSslModes.count(valueOrDefault(args, "--sslmode", "require"))) throw std::runtime_error("unsupported sslmode");
}

void addTiming(std::map<std::string, int64_t>& timings, const std::string& group, int64_t started) {
    timings[group] += nowNs() - started;
}

std::string statusMessage(const scratchbird::core::ErrorContext& ctx) {
    return ctx.message.empty() ? "driver operation failed" : ctx.message;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto args = parseArgs(argc, argv);
        validate(args);
        const std::string summaryPath = required(args, "--summary");
        const std::string runRoot = summaryPath.substr(0, summaryPath.find_last_of('/'));
        const std::map<std::string, std::string> paths{
            {"events", runRoot + "/command-events.jsonl"},
            {"wire", runRoot + "/wire-transcript.jsonl"},
            {"timing", runRoot + "/timing-groups.json"},
            {"digests", runRoot + "/result-digests.json"},
            {"metadata", runRoot + "/metadata-snapshots.json"},
            {"route_env", runRoot + "/route-environment.json"},
            {"refusals", runRoot + "/security-refusals.json"},
            {"api", runRoot + "/native-api-coverage.json"},
            {"review", runRoot + "/code-example-review.json"},
            {"junit", runRoot + "/junit.xml"},
            {"stdout", runRoot + "/stdout.log"},
            {"stderr", runRoot + "/stderr.log"},
        };
        for (const auto& path : {required(args, "--output"), required(args, "--error"), required(args, "--diagnostics"),
                                 required(args, "--metrics"), required(args, "--transcript"), summaryPath}) {
            writeText(path, "");
        }
        for (const auto& item : paths) writeText(item.second, "");

        std::map<std::string, int64_t> timings;
        std::map<std::string, int> api{{"scratchbird::client::Connection", 0}, {"connect", 0}, {"prepare", 0},
                                       {"execute", 0}, {"executeQuery", 0}, {"metadataQuery", 0},
                                       {"commit", 0}, {"rollback", 0}, {"ResultSet::next", 0}};
        json testcases = json::array();
        json failures = json::array();
        json digests = json::array();
        json securityRefusals = json::array();
        const int64_t started = nowNs();
        const std::string route = required(args, "--route");
        const std::string parserMode = required(args, "--parser-mode");
        const std::string pageSize = required(args, "--page-size");
        const uint32_t expectedPageSizeBytes = pageSizeBytesForLabel(pageSize);
        const std::string sslmode = valueOrDefault(args, "--sslmode", "require");
        const std::string transportMode = transportModeForRoute(route, sslmode);
        const std::string tlsPolicy = tlsPolicyForRoute(route, sslmode);
        const std::string languageResourcePack =
            valueOrDefault(args, "--language-resource-pack", "project/resources/seed-packs/initial-resource-pack/resources/i18n/sbsql-language-resource-pack");
        const std::string languageResourceIdentity =
            valueOrDefault(args, "--language-resource-identity", "sbsql.common_resource_pack.v1");
        const std::string languageResourceHash =
            valueOrDefault(args, "--language-resource-hash", "sha256:752c7a9823bdad00b48ab318c8b2d5d6d53b2739ecfe43f565952fd510f4e3dc");
        const std::string languageProfile = valueOrDefault(args, "--language-profile", "en-US");
        const std::string syntaxProfile = valueOrDefault(args, "--syntax-profile", "sbsql.v3");
        const std::string topologyProfile = valueOrDefault(args, "--topology-profile", "topology.sbsql.canonical.v1");
        const bool standardEnglishFallback = booleanArg(args, "--standard-english-fallback", true);
        const std::set<std::string> expectedRefusals =
            loadExpectedRefusals(valueOrDefault(args, "--expected-refusals", ""));

        scratchbird::client::Connection conn;
        api["scratchbird::client::Connection"]++;
        scratchbird::client::ConnectionConfig config;
        config.host = required(args, "--host");
        config.tcp_port = static_cast<uint16_t>(std::stoi(required(args, "--port")));
        config.database_name = required(args, "--database");
        config.username = required(args, "--user");
        config.password = required(args, "--password");
        config.role = valueOrDefault(args, "--role", "");
        config.ssl_mode = sslmode;
        config.ssl_root_cert = valueOrDefault(args, "--sslrootcert", "");
        config.ssl_cert = valueOrDefault(args, "--sslcert", "");
        config.ssl_key = valueOrDefault(args, "--sslkey", "");
        if (route == "embedded") {
            config.transport_mode = "embedded";
        } else if (route == "ipc_local") {
            config.transport_mode = "local_ipc";
            config.ipc_method = "unix";
            config.ipc_path = valueOrDefault(args, "--ipc-path", "");
        } else {
            config.transport_mode = "inet_listener";
        }
        config.front_door_mode = route == "manager-listener-parser" ? "manager_proxy" : "direct";
        config.application_name = "SBIsqlCpp";
        config.query_timeout_ms = static_cast<uint32_t>(
            std::stoul(valueOrDefault(args, "--statement-timeout-ms", "30000")));
        config.read_timeout_ms = config.query_timeout_ms;
        config.write_timeout_ms = config.query_timeout_ms;
        config.enable_copy_streaming = true;
        config.binary_transfer = true;

        scratchbird::core::ErrorContext ctx;
        const int64_t connectStarted = nowNs();
        auto status = conn.connect(config, &ctx);
        if (status == scratchbird::core::Status::OK) {
            api["connect"]++;
            addTiming(timings, "connection", connectStarted);
            appendJsonl(required(args, "--transcript"), {{"event", "connect"},
                                                         {"driver", "cpp"},
                                                         {"route", route},
                                                         {"parser_mode", parserMode},
                                                         {"page_size", pageSize},
                                                         {"sslmode", sslmode},
                                                         {"transport_mode", transportMode},
                                                         {"transport_endpoint_kind", endpointKindForRoute(route)},
                                                         {"driver_transport_implementation", transportImplementationForRoute(route)},
                                                         {"cpp_library_boundary", "native_cpp_driver"},
                                                         {"tls_policy", tlsPolicy},
                                                         {"engine_sql_text_execution", false}});
            appendJsonl(paths.at("wire"), {{"event", "server_admission_required"},
                                           {"driver_or_parser_finality", "forbidden"},
                                           {"parser_output_to_engine_required", true},
                                           {"engine_sql_text_execution", false}});
        } else {
            failures.push_back({{"statement_id", "connect"}, {"message", statusMessage(ctx)}});
        }

        json routeEnvironment{{"run_id", valueOrDefault(args, "--run-id", "manual")},
                              {"driver", "cpp"},
                              {"route", route},
                              {"sslmode", sslmode},
                              {"parser_mode", parserMode},
                              {"concurrency_mode", "single"},
                              {"namespace", required(args, "--namespace")},
                              {"page_size", pageSize},
                              {"expected_page_size_bytes", expectedPageSizeBytes},
                              {"actual_page_size_bytes", nullptr},
                              {"page_size_verification_source", "SHOW DATABASE"},
                              {"page_size_verification_status", "fail"},
                              {"failure_reason", "not_probed"},
                              {"transport_mode", transportMode},
                              {"transport_endpoint_kind", endpointKindForRoute(route)},
                              {"driver_transport_implementation", transportImplementationForRoute(route)}};
        if (status != scratchbird::core::Status::OK) {
            routeEnvironment["failure_reason"] = "connect_failed";
        }
        writeText(paths.at("route_env"), routeEnvironment.dump(2) + "\n");

        if (failures.empty() && booleanArg(args, "--create-database", false)) {
            const int64_t createStarted = nowNs();
            status = conn.attachCreate(valueOrDefault(args, "--create-emulation-mode", "sbsql"),
                                       required(args, "--database"),
                                       &ctx);
            if (status == scratchbird::core::Status::OK) {
                api["attachCreate"]++;
                addTiming(timings, "database_create", createStarted);
            } else {
                failures.push_back({{"statement_id", "database_create"},
                                    {"message", statusMessage(ctx)},
                                    {"create_emulation_mode", valueOrDefault(args, "--create-emulation-mode", "sbsql")}});
            }
        }

        if (failures.empty()) {
            scratchbird::client::ResultSet databaseInfo;
            scratchbird::core::ErrorContext pageCtx;
            const int64_t pageProbeStarted = nowNs();
            const auto pageStatus = conn.executeQuery("SHOW DATABASE", &databaseInfo, &pageCtx);
            api["executeQuery"]++;
            api["execute"]++;
            addTiming(timings, "metadata", pageProbeStarted);
            if (pageStatus == scratchbird::core::Status::OK && databaseInfo.next()) {
                api["ResultSet::next"]++;
                const int pageSizeColumn = databaseInfo.getColumnIndex("page_size_bytes");
                if (pageSizeColumn >= 0 && !databaseInfo.isNull(static_cast<size_t>(pageSizeColumn))) {
                    const auto actual = static_cast<uint32_t>(
                        databaseInfo.getInt32(static_cast<size_t>(pageSizeColumn)));
                    routeEnvironment["actual_page_size_bytes"] = actual;
                    routeEnvironment["page_size_verification_status"] =
                        actual == expectedPageSizeBytes ? "pass" : "fail";
                    if (actual != expectedPageSizeBytes) {
                        routeEnvironment["failure_reason"] = "actual_page_size_mismatch";
                    }
                } else {
                    routeEnvironment["failure_reason"] = "show_database_missing_page_size_bytes";
                }
            } else {
                routeEnvironment["failure_reason"] = statusMessage(pageCtx);
            }
            writeText(paths.at("route_env"), routeEnvironment.dump(2) + "\n");
            if (route != "embedded" &&
                routeEnvironment.value("page_size_verification_status", "fail") != "pass") {
                failures.push_back({{"statement_id", "route_page_size"},
                                    {"message", "route page-size verification failed"},
                                    {"expected_page_size_bytes", expectedPageSizeBytes},
                                    {"actual_page_size_bytes",
                                     routeEnvironment.value("actual_page_size_bytes", json(nullptr))}});
            }
        }
        if (failures.empty() && parserMode != "server-parser") {
            failures.push_back({{"statement_id", "parser_mode"}, {"message", parserMode + " is not yet implemented by the C++ native tool; it fails closed"}});
        }

        if (failures.empty()) {
            const auto statements = sbchunk::splitStatements(readInput(required(args, "--input")));
            std::string scriptName = std::filesystem::path(required(args, "--input")).filename().string();
            if (scriptName.empty()) {
                scriptName = required(args, "--input");
            }
            for (size_t index = 0; index < statements.size(); ++index) {
                const auto& sql = statements[index];
                const std::string statementId = scriptName + ":" + std::to_string(index + 1);
                const bool expectedRefusal = expectedRefusals.count(statementId) != 0;
                const std::string expectedOutcome = expectedRefusal ? "refusal" : "success";
                const std::string group = classify(sql);
                const int64_t statementStarted = nowNs();
                std::string outcome = "success";
                int64_t rowCount = -1;
                std::string resultDigest;
                std::string diagnostic;
                std::string sqlState = "00000";

                if (group == "transaction") {
                    const auto tokens = leadingTokensLower(sql, 4);
                    const std::string first = tokens.empty() ? "" : tokens[0];
                    const std::string second = tokens.size() > 1 ? tokens[1] : "";
                    if (first == "commit") {
                        status = conn.commit(&ctx);
                        api["commit"]++;
                    } else if (first == "rollback" && second != "to") {
                        status = conn.rollback(&ctx);
                        api["rollback"]++;
                    } else if (first == "savepoint") {
                        status = conn.savepoint(savepointName(sql), &ctx);
                        api["savepoint"]++;
                    } else if (first == "release" && second == "savepoint") {
                        status = conn.releaseSavepoint(savepointName(sql), &ctx);
                        api["releaseSavepoint"]++;
                    } else if (first == "rollback" && second == "to") {
                        status = conn.rollbackTo(savepointName(sql), &ctx);
                        api["rollbackTo"]++;
                    } else {
                        status = conn.beginTransaction(&ctx);
                    }
                    rowCount = 0;
                    resultDigest = sha256Text("transaction");
                } else {
                    scratchbird::client::ResultSet results;
                    int64_t rowsAffected = 0;
                    const bool copyStatement = firstTokenLower(sql) == "copy";
                    std::string copyInputPayload;
                    std::istringstream copyInput;
                    std::ostringstream copyOutput;
                    if (copyStatement) {
                        copyInputPayload = copyInputForStatement(sql);
                        if (!copyInputPayload.empty()) {
                            copyInput.str(copyInputPayload);
                            conn.setCopyInputSizeHintBytes(copyInputPayload.size());
                            conn.setCopyPreallocationFactorPercent(82);
                            conn.setCopyInputStream(&copyInput);
                        }
                        conn.setCopyOutputStream(&copyOutput);
                    }
                    if (statementReturnsRows(sql)) {
                        status = conn.executeQuery(sql, &results, &ctx);
                        api["executeQuery"]++;
                        api["execute"]++;
                    } else {
                        status = conn.execute(sql, &rowsAffected, &ctx);
                        api["execute"]++;
                    }
                    if (copyStatement) {
                        conn.setCopyInputStream(nullptr);
                        conn.setCopyInputSizeHintBytes(0);
                        conn.setCopyOutputStream(nullptr);
                        appendJsonl(paths.at("wire"), {{"event", "copy_stream"},
                                                       {"statement_id", statementId},
                                                       {"driver_payload_kind", "copy_canonical_rows"},
                                                       {"engine_payload_kind", "canonical_rows"},
                                                       {"copy_input_bytes", copyInputPayload.size()},
                                                       {"copy_output_bytes", copyOutput.str().size()},
                                                       {"copy_output_sha256", sha256Text(copyOutput.str())},
                                                       {"engine_sql_text_execution", false},
                                                       {"mga_authority", "engine"}});
                    }
                    json rows = json::array();
                    if (status == scratchbird::core::Status::OK && statementReturnsRows(sql)) {
                        while (results.next()) {
                            api["ResultSet::next"]++;
                            json row = json::array();
                            for (size_t column = 0; column < results.getColumnCount(); ++column) {
                                row.push_back(results.isNull(column) ? json(nullptr) : json(results.getString(column)));
                            }
                            rows.push_back(row);
                        }
                        rowCount = static_cast<int64_t>(rows.size());
                        resultDigest = sha256Text(rows.dump());
                        appendText(required(args, "--output"), json({{"statement_id", statementId}, {"rows", rows}}).dump() + "\n");
                    } else if (status == scratchbird::core::Status::OK) {
                        rowCount = rowsAffected;
                        resultDigest = sha256Text("rows_affected:" + std::to_string(rowsAffected));
                        json outputRecord{{"statement_id", statementId}, {"rows_affected", rowsAffected}};
                        if (copyStatement) {
                            outputRecord["copy_input_bytes"] = copyInputPayload.size();
                            if (!copyOutput.str().empty()) {
                                outputRecord["copy_output_bytes"] = copyOutput.str().size();
                                outputRecord["copy_output_sha256"] = sha256Text(copyOutput.str());
                            }
                        }
                        appendText(required(args, "--output"), outputRecord.dump() + "\n");
                    }
                }

                if (status != scratchbird::core::Status::OK) {
                    outcome = "refusal";
                    diagnostic = statusMessage(ctx);
                    sqlState = ctx.sqlstate ? ctx.sqlstate : "HY000";
                    appendJsonl(required(args, "--diagnostics"), {{"statement_id", statementId}, {"sqlstate", sqlState}, {"message", diagnostic}});
                    appendText(required(args, "--error"), statementId + ": " + diagnostic + "\n");
                    if (expectedRefusal) {
                        securityRefusals.push_back({{"statement_id", statementId}, {"sqlstate", sqlState}, {"diagnostic_code", diagnostic}});
                    } else {
                        failures.push_back({{"statement_id", statementId}, {"message", diagnostic}});
                    }
                    if (!expectedRefusal && booleanArg(args, "--stop-on-error", true)) {
                        addTiming(timings, group, statementStarted);
                        break;
                    }
                } else if (expectedRefusal) {
                    outcome = "unexpected_success";
                    diagnostic = "statement succeeded but was expected to refuse";
                    failures.push_back({{"statement_id", statementId}, {"message", diagnostic}});
                }
                addTiming(timings, group, statementStarted);
                const auto event = json{{"run_id", valueOrDefault(args, "--run-id", "manual")},
                                        {"driver_name", "cpp"},
                                        {"driver_version", "unknown"},
                                         {"route", route},
                                         {"parser_mode", parserMode},
                                         {"page_size", pageSize},
                                         {"language_profile", languageProfile},
                                         {"language_resource_identity", languageResourceIdentity},
                                         {"language_resource_hash", languageResourceHash},
                                         {"syntax_profile", syntaxProfile},
                                         {"topology_profile", topologyProfile},
                                         {"namespace", required(args, "--namespace")},
                                        {"script", required(args, "--input")},
                                        {"statement_index", index + 1},
                                        {"statement_id", statementId},
                                        {"command_group", group},
                                        {"sql_hash", sha256Text(sql)},
                                        {"expected_outcome", expectedOutcome},
                                        {"actual_outcome", outcome},
                                        {"sqlstate", sqlState},
                                        {"diagnostic_code", diagnostic},
                                        {"canonical_message_vector", json::array()},
                                        {"row_count", rowCount},
                                        {"result_digest", resultDigest},
                                        {"elapsed_ns", nowNs() - statementStarted},
                                        {"server_revalidation_state", "required"},
                                        {"language_profile", languageProfile},
                                        {"language_resource_pack", languageResourcePack},
                                        {"language_resource_identity", languageResourceIdentity},
                                        {"language_resource_hash", languageResourceHash},
                                        {"syntax_profile", syntaxProfile},
                                        {"topology_profile", topologyProfile},
                                        {"standard_english_fallback", standardEnglishFallback},
                                        {"parser_output_to_engine_required", true},
                                        {"engine_sql_text_execution", false},
                                        {"sql_text_artifact", "sha256_only"},
                                        {"transaction_id_observed", nullptr},
                                        {"mga_authority", "engine"},
                                        {"native_api_surface", "cpp"},
                                        {"code_example_section", "prepare_execute_fetch"}};
                appendJsonl(paths.at("events"), event);
                testcases.push_back(event);
                digests.push_back({{"statement_id", statementId}, {"row_count", rowCount}, {"result_digest", resultDigest}});
            }

            scratchbird::client::ResultSet metadata;
            const int64_t metadataStarted = nowNs();
            status = conn.metadataQuery("tables", &metadata, &ctx);
            api["metadataQuery"]++;
            int64_t metadataRows = 0;
            if (status == scratchbird::core::Status::OK) {
                while (metadata.next()) ++metadataRows;
            }
            writeText(paths.at("metadata"), json({{"tables_digest", sha256Text(std::to_string(metadataRows))}, {"row_count", metadataRows}}).dump() + "\n");
            addTiming(timings, "metadata", metadataStarted);
        }

        conn.disconnect();
        timings["overall"] = nowNs() - started;
        const json processMetrics = currentProcessMetrics();
        const json summaryJson{{"run_id", valueOrDefault(args, "--run-id", "manual")},
                               {"driver_name", "cpp"},
                               {"route", route},
                               {"parser_mode", parserMode},
                               {"page_size", pageSize},
                               {"namespace", required(args, "--namespace")},
                               {"sslmode", sslmode},
                               {"transport_mode", transportMode},
                               {"transport_endpoint_kind", endpointKindForRoute(route)},
                               {"driver_transport_implementation", transportImplementationForRoute(route)},
                               {"cpp_library_boundary", "native_cpp_driver"},
                               {"tls_policy", tlsPolicy},
                               {"expected_page_size_bytes", expectedPageSizeBytes},
                               {"actual_page_size_bytes", routeEnvironment.value("actual_page_size_bytes", json(nullptr))},
                               {"page_size_verification_status", routeEnvironment.value("page_size_verification_status", "fail")},
                               {"language_resource_pack", languageResourcePack},
                               {"language_resource_identity", languageResourceIdentity},
                               {"language_resource_hash", languageResourceHash},
                               {"language_resource_authority", "shared_server_parser_resource_pack"},
                               {"language_profile", languageProfile},
                               {"syntax_profile", syntaxProfile},
                               {"topology_profile", topologyProfile},
                               {"standard_english_fallback", standardEnglishFallback},
                               {"status", failures.empty() ? "pass" : "fail"},
                               {"failure_count", failures.size()},
                               {"elapsed_ns", timings["overall"]},
                               {"process_metrics", processMetrics},
                               {"server_revalidation_required", true},
                               {"parser_output_to_engine_required", true},
                               {"engine_sql_text_execution", false},
                               {"driver_or_parser_finality", "forbidden"},
                               {"mga_authority", "engine"}};
        writeText(summaryPath, summaryJson.dump() + "\n");
        writeText(required(args, "--metrics"), json({{"role", "client"},
                                                     {"rss_kb", processMetrics["client"]["last_rss_kb"]},
                                                     {"vsize_kb", processMetrics["client"]["last_vsize_kb"]}}).dump() + "\n");
        writeText(paths.at("timing"), json(timings).dump() + "\n");
        writeText(paths.at("digests"), digests.dump() + "\n");
        writeText(paths.at("refusals"), securityRefusals.dump() + "\n");
        writeText(paths.at("api"), json(api).dump() + "\n");
        writeText(paths.at("review"), json({{"driver", "cpp"}, {"public_api_only", true}, {"shells_out_to_other_driver", false},
                                             {"source_is_canonical_example", true}, {"sections", {"connection", "prepare", "execute", "fetch", "metadata", "diagnostics", "transaction"}}}).dump() + "\n");
        std::ostringstream junit;
        junit << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
              << "<testsuite name=\"SBIsqlCpp\" tests=\"" << std::max<size_t>(testcases.size(), 1) << "\" failures=\"" << failures.size() << "\">\n"
              << "  <testcase classname=\"scratchbird.cpp\" name=\"run\"></testcase>\n"
              << "</testsuite>\n";
        writeText(paths.at("junit"), junit.str());
        appendText(paths.at("stdout"), std::string("SBIsqlCpp status=") + (failures.empty() ? "pass" : "fail") + "\n");
        return failures.empty() ? 0 : 1;
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << "\n";
        return 1;
    }
}
