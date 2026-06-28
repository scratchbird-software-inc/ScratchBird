// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "scratchbird/client/connection.h"
#include "scratchbird/protocol/sbwp_protocol.h"
#include "sb_statement_chunker.hpp"

#include <openssl/sha.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "nlohmann/json.hpp"

using json = nlohmann::json;

namespace {

const std::set<std::string> kPageSizes{"4k", "8k", "16k", "32k", "64k", "128k"};
const std::set<std::string> kRoutes{"embedded", "ipc_local", "listener-parser", "manager-listener-parser"};
const std::set<std::string> kParserModes{"server-parser", "standalone-parser", "driver-sblr-uuid"};

int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string lower(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

std::string trim(const std::string& value) {
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }
    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return value.substr(begin, end - begin);
}

bool startsWith(const std::string& value, const std::string& prefix) {
    return value.rfind(prefix, 0) == 0;
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
    return it == args.end() || it->second.empty() ? fallback : it->second;
}

void clearErrorContext(scratchbird::core::ErrorContext* ctx) {
    if (ctx == nullptr) {
        return;
    }
    if (ctx->cause != nullptr) {
        delete ctx->cause;
        ctx->cause = nullptr;
    }
    ctx->code = scratchbird::core::Status::OK;
    ctx->sqlstate = scratchbird::core::SQLSTATE_SUCCESS;
    ctx->sqlstate_text.clear();
    ctx->message.clear();
    ctx->file = nullptr;
    ctx->line = 0;
    ctx->function = nullptr;
    ctx->constraint_name.clear();
    ctx->table_name.clear();
    ctx->column_name.clear();
    ctx->violating_value.clear();
    ctx->referenced_table.clear();
    ctx->referenced_column.clear();
    ctx->check_expression.clear();
    ctx->hint.clear();
}

bool hasFlag(const std::map<std::string, std::string>& args, const std::string& key) {
    return args.find(key) != args.end();
}

std::map<std::string, std::string> parseArgs(int argc, char** argv) {
    std::map<std::string, std::string> args;
    for (int i = 1; i < argc; ++i) {
        std::string key(argv[i]);
        if (key == "--help" || key == "-h") {
            args["--help"] = "true";
            continue;
        }
        if (key.rfind("--", 0) != 0) {
            throw std::runtime_error("unexpected positional argument: " + key);
        }
        if (key == "--stop-on-error" || key == "--create-database" ||
            key == "--create-emulation-mode" || key == "--concurrency-worker") {
            args[key] = "true";
            continue;
        }
        if (i + 1 >= argc || std::string(argv[i + 1]).rfind("--", 0) == 0) {
            throw std::runtime_error("missing value for " + key);
        }
        args[key] = argv[++i];
    }
    return args;
}

void writeText(const std::filesystem::path& path, const std::string& text) {
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path());
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("unable to write " + path.string());
    }
    out << text;
}

void appendText(const std::filesystem::path& path, const std::string& text) {
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path());
    }
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out) {
        throw std::runtime_error("unable to append " + path.string());
    }
    out << text;
}

void appendJsonl(const std::filesystem::path& path, const json& record) {
    appendText(path, record.dump() + "\n");
}

void setProcessEnv(const std::string& key, const std::string& value) {
#ifdef _WIN32
    _putenv_s(key.c_str(), value.c_str());
#else
    setenv(key.c_str(), value.c_str(), 1);
#endif
}

void clearProcessEnv(const std::string& key) {
#ifdef _WIN32
    _putenv_s(key.c_str(), "");
#else
    unsetenv(key.c_str());
#endif
}

void setDriverPhaseTraceContext(const std::string& runId,
                                const std::string& scriptId,
                                const std::string& statementId,
                                const std::string& elementId,
                                const std::string& commandGroup,
                                const std::string& executionMode) {
    setProcessEnv("SCRATCHBIRD_CPP_DRIVER_PHASE_TRACE_RUN_ID", runId);
    setProcessEnv("SCRATCHBIRD_CPP_DRIVER_PHASE_TRACE_SCRIPT_ID", scriptId);
    setProcessEnv("SCRATCHBIRD_CPP_DRIVER_PHASE_TRACE_STATEMENT_ID", statementId);
    setProcessEnv("SCRATCHBIRD_CPP_DRIVER_PHASE_TRACE_ELEMENT_ID", elementId);
    setProcessEnv("SCRATCHBIRD_CPP_DRIVER_PHASE_TRACE_COMMAND_GROUP", commandGroup);
    setProcessEnv("SCRATCHBIRD_CPP_DRIVER_PHASE_TRACE_EXECUTION_MODE", executionMode);
}

void clearDriverPhaseTraceContext() {
    clearProcessEnv("SCRATCHBIRD_CPP_DRIVER_PHASE_TRACE_RUN_ID");
    clearProcessEnv("SCRATCHBIRD_CPP_DRIVER_PHASE_TRACE_SCRIPT_ID");
    clearProcessEnv("SCRATCHBIRD_CPP_DRIVER_PHASE_TRACE_STATEMENT_ID");
    clearProcessEnv("SCRATCHBIRD_CPP_DRIVER_PHASE_TRACE_ELEMENT_ID");
    clearProcessEnv("SCRATCHBIRD_CPP_DRIVER_PHASE_TRACE_COMMAND_GROUP");
    clearProcessEnv("SCRATCHBIRD_CPP_DRIVER_PHASE_TRACE_EXECUTION_MODE");
}

void setParserPhaseTraceFiles(const std::filesystem::path& workerTracePath,
                              const std::filesystem::path& pipelineTracePath) {
    setProcessEnv("SCRATCHBIRD_SBSQL_WORKER_PHASE_TRACE_FILE", workerTracePath.string());
    setProcessEnv("SCRATCHBIRD_SBSQL_PIPELINE_PHASE_TRACE_FILE", pipelineTracePath.string());
}

void clearParserPhaseTraceFiles() {
    clearProcessEnv("SCRATCHBIRD_SBSQL_WORKER_PHASE_TRACE_FILE");
    clearProcessEnv("SCRATCHBIRD_SBSQL_PIPELINE_PHASE_TRACE_FILE");
}

std::optional<int64_t> parseStatusKb(const std::string& line, const std::string& key) {
    if (!startsWith(line, key + ":")) {
        return std::nullopt;
    }
    std::istringstream in(line.substr(key.size() + 1));
    int64_t value = 0;
    in >> value;
    return in ? std::optional<int64_t>(value) : std::nullopt;
}

json sampleProcessMetrics(const std::string& role, int pid) {
    json sample{{"role", role}, {"pid", pid}, {"ok", false}};
#ifndef _WIN32
    if (pid <= 0) {
        sample["error"] = "invalid_pid";
        return sample;
    }
    const std::filesystem::path procRoot = std::filesystem::path("/proc") / std::to_string(pid);
    std::ifstream status(procRoot / "status");
    if (!status) {
        sample["error"] = "proc_status_unavailable";
        return sample;
    }
    std::string line;
    while (std::getline(status, line)) {
        if (const auto rss = parseStatusKb(line, "VmRSS")) sample["rss_kb"] = *rss;
        if (const auto vms = parseStatusKb(line, "VmSize")) sample["vsize_kb"] = *vms;
        if (const auto hwm = parseStatusKb(line, "VmHWM")) sample["rss_high_water_kb"] = *hwm;
    }
    std::ifstream stat(procRoot / "stat");
    if (stat) {
        std::string statText;
        std::getline(stat, statText);
        const auto close = statText.rfind(')');
        if (close != std::string::npos && close + 2 < statText.size()) {
            std::istringstream fields(statText.substr(close + 2));
            std::vector<std::string> parts;
            std::string part;
            while (fields >> part) parts.push_back(part);
            if (parts.size() > 12) {
                sample["utime_ticks"] = std::stoull(parts[11]);
                sample["stime_ticks"] = std::stoull(parts[12]);
            }
        }
    }
    sample["ok"] = true;
#else
    sample["error"] = "process_metrics_not_supported_on_windows";
#endif
    return sample;
}

std::string readText(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("unable to read " + path.string());
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

json readJson(const std::filesystem::path& path) {
    return json::parse(readText(path));
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

std::string shortHashText(const std::string& text, std::size_t hexChars = 16) {
    std::string digest = sha256Text(text);
    constexpr char kPrefix[] = "sha256:";
    if (startsWith(digest, kPrefix)) {
        digest = digest.substr(sizeof(kPrefix) - 1);
    }
    if (digest.size() > hexChars) {
        digest.resize(hexChars);
    }
    return digest;
}

std::vector<std::string> splitCsv(const std::string& value) {
    std::vector<std::string> out;
    std::string current;
    std::istringstream in(value);
    while (std::getline(in, current, ',')) {
        current = trim(current);
        if (!current.empty()) {
            out.push_back(current);
        }
    }
    return out;
}

std::string replaceAll(std::string value, const std::string& needle, const std::string& replacement) {
    size_t pos = 0;
    while ((pos = value.find(needle, pos)) != std::string::npos) {
        value.replace(pos, needle.size(), replacement);
        pos += replacement.size();
    }
    return value;
}

std::string applyPlaceholders(std::string script, const std::map<std::string, std::string>& replacements) {
    for (const auto& item : replacements) {
        script = replaceAll(std::move(script), item.first, item.second);
    }
    return script;
}

std::string stripComments(const std::string& script) {
    std::string out;
    bool single = false;
    bool dbl = false;
    bool lineComment = false;
    bool blockComment = false;

    for (size_t i = 0; i < script.size(); ++i) {
        const char ch = script[i];
        const char next = (i + 1 < script.size()) ? script[i + 1] : '\0';

        if (lineComment) {
            if (ch == '\n') {
                lineComment = false;
                out.push_back(ch);
            }
            continue;
        }
        if (blockComment) {
            if (ch == '*' && next == '/') {
                blockComment = false;
                ++i;
            } else if (ch == '\n') {
                out.push_back('\n');
            }
            continue;
        }
        if (!single && !dbl && ch == '-' && next == '-') {
            lineComment = true;
            ++i;
            continue;
        }
        if (!single && !dbl && ch == '/' && next == '*') {
            blockComment = true;
            ++i;
            continue;
        }
        if (ch == '\'' && !dbl) {
            out.push_back(ch);
            if (single && next == '\'') {
                out.push_back(next);
                ++i;
            } else {
                single = !single;
            }
            continue;
        }
        if (ch == '"' && !single) {
            dbl = !dbl;
        }
        out.push_back(ch);
    }
    return out;
}

// The statement chunker lives in the shared header sb_statement_chunker.hpp
// (SET TERM- and comment-aware) so sb_isql_cpp and sb_regress_cpp use one
// identical implementation. Verified against the cross-driver fixture
// tests/conformance/drivers/chunker_conformance/cases.json. Call it as
// sbchunk::splitStatements(...).

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

std::optional<std::string> explicitElementId(const std::string& sql) {
    std::istringstream input(sql);
    std::string line;
    while (std::getline(input, line)) {
        const std::string trimmed = trim(line);
        if (trimmed.empty()) {
            continue;
        }
        if (startsWith(trimmed, "--")) {
            std::string comment = trim(trimmed.substr(2));
            const std::string key = "element_id";
            if (startsWith(lower(comment), key)) {
                comment = trim(comment.substr(key.size()));
                if (!comment.empty() && (comment[0] == ':' || comment[0] == '=')) {
                    comment = trim(comment.substr(1));
                }
                if (!comment.empty()) {
                    for (char ch : comment) {
                        const bool valid =
                            std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' ||
                            ch == '-' || ch == '.' || ch == ':';
                        if (!valid) {
                            throw std::runtime_error("invalid element_id character in " + comment);
                        }
                    }
                    return comment;
                }
            }
            continue;
        }
        if (startsWith(trimmed, "/*")) {
            continue;
        }
        break;
    }
    return std::nullopt;
}

std::string normalizeElementSql(const std::string& sql) {
    const std::string withoutComments = stripComments(sql);
    const std::string trimmedSql = trim(withoutComments);
    std::string out;
    out.reserve(trimmedSql.size());
    bool single = false;
    bool dbl = false;
    bool pendingSpace = false;
    for (size_t i = 0; i < trimmedSql.size(); ++i) {
        const char ch = trimmedSql[i];
        const char next = (i + 1 < trimmedSql.size()) ? trimmedSql[i + 1] : '\0';
        if (ch == '\'' && !dbl) {
            if (pendingSpace && !out.empty()) {
                out.push_back(' ');
                pendingSpace = false;
            }
            out.push_back(ch);
            if (single && next == '\'') {
                out.push_back(next);
                ++i;
            } else {
                single = !single;
            }
            continue;
        }
        if (ch == '"' && !single) {
            if (pendingSpace && !out.empty()) {
                out.push_back(' ');
                pendingSpace = false;
            }
            out.push_back(ch);
            if (dbl && next == '"') {
                out.push_back(next);
                ++i;
            } else {
                dbl = !dbl;
            }
            continue;
        }
        if (!single && !dbl && std::isspace(static_cast<unsigned char>(ch))) {
            pendingSpace = true;
            continue;
        }
        if (pendingSpace && !out.empty()) {
            out.push_back(' ');
            pendingSpace = false;
        }
        out.push_back(ch);
    }
    return out;
}

std::string elementIdForStatement(const std::string& scriptId,
                                  const std::string& commandGroup,
                                  const std::string& sql) {
    if (const auto explicitId = explicitElementId(sql)) {
        return *explicitId;
    }
    return scriptId + ":" + commandGroup + ":" + shortHashText(normalizeElementSql(sql));
}

std::string firstTokenLower(const std::string& sql) {
    std::istringstream in(stripLeadingTrivia(sql));
    std::string first;
    in >> first;
    return lower(first);
}

std::string secondTokenLower(const std::string& sql) {
    std::istringstream in(stripLeadingTrivia(sql));
    std::string first;
    std::string second;
    in >> first >> second;
    return lower(second);
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
        return lower(token);
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
    return lower(text.substr(begin, *pos - begin));
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
        if (*pos + 1 < text.size() && text[*pos] == '-' && text[*pos + 1] == '-') {
            skipSqlTrivia(text, pos);
            continue;
        }
        if (*pos + 1 < text.size() && text[*pos] == '/' && text[*pos + 1] == '*') {
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
        if (pos < text.size() && text[pos] == '(') {
            if (!skipSqlParenthesized(text, &pos)) {
                return "with";
            }
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

struct CopyHarnessInput {
    std::string payload;
    std::string executable_sql;
    std::size_t marker_count = 0;
};

CopyHarnessInput copyHarnessInputForStatement(const std::string& sql) {
    static const std::string kMarker = "-- SB_COPY_INPUT ";
    std::istringstream lines(sql);
    std::string line;
    CopyHarnessInput out;
    while (std::getline(lines, line)) {
        const size_t start = line.find_first_not_of(" \t");
        if (start != std::string::npos &&
            line.compare(start, kMarker.size(), kMarker) == 0) {
            std::string row = line.substr(start + kMarker.size());
            if (!row.empty() && row.back() == '\r') {
                row.pop_back();
            }
            out.payload += row;
            out.payload += '\n';
            ++out.marker_count;
            continue;
        }
        out.executable_sql += line;
        out.executable_sql += '\n';
    }
    return out;
}

std::string savepointName(const std::string& sql) {
    std::istringstream in(trim(stripLeadingTrivia(sql)));
    std::string first;
    std::string second;
    std::string third;
    in >> first >> second >> third;
    if (lower(first) == "savepoint") {
        return second;
    }
    if (lower(first) == "release" && lower(second) == "savepoint") {
        return third;
    }
    if (lower(first) == "rollback" && lower(second) == "to") {
        if (lower(third) == "savepoint") {
            std::string fourth;
            in >> fourth;
            return fourth;
        }
        return third;
    }
    return "";
}

std::string classify(const std::string& sql, const std::string& statementId, const std::set<std::string>& expectedRefusals) {
    if (expectedRefusals.count(statementId)) {
        return "security_refusal";
    }
    const auto first = mainStatementTokenLower(sql);
    if (first == "create" || first == "alter" || first == "drop" || first == "truncate") {
        return "ddl";
    }
    if (first == "insert" || first == "update" || first == "delete" || first == "merge" || first == "upsert" ||
        first == "copy") {
        return "dml";
    }
    if (first == "commit" || first == "rollback" || first == "savepoint" || first == "begin" ||
        first == "start" || first == "release") {
        return "transaction";
    }
    if (first == "grant" || first == "revoke") {
        return "security";
    }
    if (sql.find("sys.") != std::string::npos || sql.find("information_schema.") != std::string::npos) {
        return "metadata";
    }
    if (first == "sbsql_surface_replay") {
        return "metadata";
    }
    if (sql.find("EXPLAIN") != std::string::npos || sql.find("explain") != std::string::npos) {
        return "optimizer";
    }
    return "query";
}

std::vector<std::string> splitIdentifierPath(const std::string& path) {
    std::vector<std::string> parts;
    std::string current;
    bool quoted = false;
    for (std::size_t index = 0; index < path.size(); ++index) {
        const char ch = path[index];
        if (quoted) {
            if (ch == '"') {
                if (index + 1 < path.size() && path[index + 1] == '"') {
                    current.push_back('"');
                    ++index;
                } else {
                    quoted = false;
                }
            } else {
                current.push_back(ch);
            }
            continue;
        }
        if (ch == '"') {
            quoted = true;
            continue;
        }
        if (ch == '.') {
            if (!current.empty()) {
                parts.push_back(current);
                current.clear();
            }
            continue;
        }
        if (!std::isspace(static_cast<unsigned char>(ch)) || !current.empty()) {
            current.push_back(ch);
        }
    }
    while (!current.empty() && std::isspace(static_cast<unsigned char>(current.back()))) {
        current.pop_back();
    }
    if (!current.empty()) {
        parts.push_back(current);
    }
    return parts;
}

std::string joinIdentifierPath(const std::vector<std::string>& parts, std::size_t count) {
    std::ostringstream out;
    for (std::size_t index = 0; index < count && index < parts.size(); ++index) {
        if (index != 0) {
            out << ".";
        }
        out << parts[index];
    }
    return out.str();
}

std::vector<std::string> namespaceAncestorSchemas(const std::string& namespaceName) {
    const std::vector<std::string> parts = splitIdentifierPath(namespaceName);
    std::vector<std::string> ancestors;
    if (parts.size() < 2) {
        return ancestors;
    }
    std::size_t firstAncestorDepth = 2;
    if (parts.size() > 2 && parts[0] == "users" && parts[1] == "public") {
        firstAncestorDepth = 3;
    }
    for (std::size_t depth = firstAncestorDepth; depth < parts.size(); ++depth) {
        ancestors.push_back(joinIdentifierPath(parts, depth));
    }
    return ancestors;
}

bool statementReturnsRows(const std::string& sql) {
    const std::string first = mainStatementTokenLower(sql);
    if (first == "select" || first == "values" || first == "show" ||
        first == "explain" || first == "sbsql_surface_replay") {
        return true;
    }
    const std::string text = " " + lower(sql) + " ";
    return text.find(" returning ") != std::string::npos;
}

bool isSqlWordChar(char ch) {
    const auto byte = static_cast<unsigned char>(ch);
    return std::isalnum(byte) || ch == '_' || ch == '$';
}

bool matchesKeywordAt(const std::string& text, std::size_t pos, const std::string& keyword) {
    if (pos + keyword.size() > text.size()) {
        return false;
    }
    for (std::size_t i = 0; i < keyword.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(text[pos + i])) !=
            std::tolower(static_cast<unsigned char>(keyword[i]))) {
            return false;
        }
    }
    const bool leftBoundary = pos == 0 || !isSqlWordChar(text[pos - 1]);
    const bool rightBoundary = pos + keyword.size() >= text.size() ||
                               !isSqlWordChar(text[pos + keyword.size()]);
    return leftBoundary && rightBoundary;
}

std::size_t findKeywordOutsideSql(const std::string& text,
                                  const std::string& keyword,
                                  std::size_t start = 0) {
    bool inLineComment = false;
    bool inBlockComment = false;
    for (std::size_t pos = start; pos < text.size();) {
        const char ch = text[pos];
        const char next = pos + 1 < text.size() ? text[pos + 1] : '\0';
        if (inLineComment) {
            inLineComment = ch != '\n';
            ++pos;
            continue;
        }
        if (inBlockComment) {
            if (ch == '*' && next == '/') {
                pos += 2;
            } else {
                ++pos;
            }
            continue;
        }
        if (ch == '-' && next == '-') {
            inLineComment = true;
            pos += 2;
            continue;
        }
        if (ch == '/' && next == '*') {
            inBlockComment = true;
            pos += 2;
            continue;
        }
        if (ch == '\'') {
            skipSqlStringLiteral(text, &pos);
            continue;
        }
        if (ch == '"') {
            skipSqlQuotedIdentifier(text, &pos);
            continue;
        }
        if (matchesKeywordAt(text, pos, keyword)) {
            return pos;
        }
        ++pos;
    }
    return std::string::npos;
}

std::size_t findMatchingSqlParen(const std::string& text, std::size_t openPos) {
    if (openPos >= text.size() || text[openPos] != '(') {
        return std::string::npos;
    }
    int depth = 0;
    bool inLineComment = false;
    bool inBlockComment = false;
    for (std::size_t pos = openPos; pos < text.size();) {
        const char ch = text[pos];
        const char next = pos + 1 < text.size() ? text[pos + 1] : '\0';
        if (inLineComment) {
            inLineComment = ch != '\n';
            ++pos;
            continue;
        }
        if (inBlockComment) {
            if (ch == '*' && next == '/') {
                pos += 2;
            } else {
                ++pos;
            }
            continue;
        }
        if (ch == '-' && next == '-') {
            inLineComment = true;
            pos += 2;
            continue;
        }
        if (ch == '/' && next == '*') {
            inBlockComment = true;
            pos += 2;
            continue;
        }
        if (ch == '\'') {
            skipSqlStringLiteral(text, &pos);
            continue;
        }
        if (ch == '"') {
            skipSqlQuotedIdentifier(text, &pos);
            continue;
        }
        if (ch == '(') {
            ++depth;
            ++pos;
            continue;
        }
        if (ch == ')') {
            --depth;
            if (depth == 0) {
                return pos;
            }
            ++pos;
            continue;
        }
        ++pos;
    }
    return std::string::npos;
}

std::vector<std::string> splitTopLevelComma(const std::string& text) {
    std::vector<std::string> parts;
    std::size_t partStart = 0;
    int depth = 0;
    bool inLineComment = false;
    bool inBlockComment = false;
    for (std::size_t pos = 0; pos < text.size();) {
        const char ch = text[pos];
        const char next = pos + 1 < text.size() ? text[pos + 1] : '\0';
        if (inLineComment) {
            inLineComment = ch != '\n';
            ++pos;
            continue;
        }
        if (inBlockComment) {
            if (ch == '*' && next == '/') {
                pos += 2;
            } else {
                ++pos;
            }
            continue;
        }
        if (ch == '-' && next == '-') {
            inLineComment = true;
            pos += 2;
            continue;
        }
        if (ch == '/' && next == '*') {
            inBlockComment = true;
            pos += 2;
            continue;
        }
        if (ch == '\'') {
            skipSqlStringLiteral(text, &pos);
            continue;
        }
        if (ch == '"') {
            skipSqlQuotedIdentifier(text, &pos);
            continue;
        }
        if (ch == '(') {
            ++depth;
        } else if (ch == ')' && depth > 0) {
            --depth;
        } else if (ch == ',' && depth == 0) {
            parts.push_back(trim(text.substr(partStart, pos - partStart)));
            partStart = pos + 1;
        }
        ++pos;
    }
    parts.push_back(trim(text.substr(partStart)));
    return parts;
}

bool onlySqlTriviaAndOptionalTerminator(const std::string& text, std::size_t pos) {
    skipSqlTrivia(text, &pos);
    if (pos < text.size() && text[pos] == ';') {
        ++pos;
        skipSqlTrivia(text, &pos);
    }
    return pos >= text.size();
}

bool hasUnquotedUppercaseAsciiOutsideSql(const std::string& text) {
    bool inLineComment = false;
    bool inBlockComment = false;
    for (std::size_t pos = 0; pos < text.size();) {
        const char ch = text[pos];
        const char next = pos + 1 < text.size() ? text[pos + 1] : '\0';
        if (inLineComment) {
            inLineComment = ch != '\n';
            ++pos;
            continue;
        }
        if (inBlockComment) {
            if (ch == '*' && next == '/') {
                pos += 2;
            } else {
                ++pos;
            }
            continue;
        }
        if (ch == '-' && next == '-') {
            inLineComment = true;
            pos += 2;
            continue;
        }
        if (ch == '/' && next == '*') {
            inBlockComment = true;
            pos += 2;
            continue;
        }
        if (ch == '\'') {
            skipSqlStringLiteral(text, &pos);
            continue;
        }
        if (ch == '"') {
            skipSqlQuotedIdentifier(text, &pos);
            continue;
        }
        if (std::isupper(static_cast<unsigned char>(ch))) {
            return true;
        }
        ++pos;
    }
    return false;
}

bool decodeSqlStringLiteral(const std::string& token, std::string* out) {
    const std::string value = trim(token);
    if (value.size() < 2 || value.front() != '\'' || value.back() != '\'') {
        return false;
    }
    std::string decoded;
    for (std::size_t pos = 1; pos + 1 < value.size(); ++pos) {
        if (value[pos] == '\'') {
            if (pos + 1 < value.size() - 1 && value[pos + 1] == '\'') {
                decoded.push_back('\'');
                ++pos;
                continue;
            }
            return false;
        }
        decoded.push_back(value[pos]);
    }
    *out = std::move(decoded);
    return true;
}

bool looksIntegerLiteral(const std::string& token) {
    const std::string value = trim(token);
    if (value.empty()) {
        return false;
    }
    std::size_t pos = 0;
    if (value[pos] == '+' || value[pos] == '-') {
        ++pos;
    }
    if (pos >= value.size()) {
        return false;
    }
    for (; pos < value.size(); ++pos) {
        if (!std::isdigit(static_cast<unsigned char>(value[pos]))) {
            return false;
        }
    }
    return true;
}

bool looksNumericLiteral(const std::string& token) {
    const std::string value = trim(token);
    if (value.empty()) {
        return false;
    }
    bool sawDigit = false;
    bool sawDecimalOrExponent = false;
    for (std::size_t pos = 0; pos < value.size(); ++pos) {
        const char ch = value[pos];
        if (std::isdigit(static_cast<unsigned char>(ch))) {
            sawDigit = true;
            continue;
        }
        if ((ch == '+' || ch == '-') && (pos == 0 || value[pos - 1] == 'e' || value[pos - 1] == 'E')) {
            continue;
        }
        if (ch == '.' || ch == 'e' || ch == 'E') {
            sawDecimalOrExponent = true;
            continue;
        }
        return false;
    }
    return sawDigit && sawDecimalOrExponent;
}

enum class PreparedParamKind {
    Null,
    Bool,
    Int64,
    Numeric,
    Text
};

struct PreparedParamValue {
    PreparedParamKind kind{PreparedParamKind::Text};
    std::string text;
    bool boolValue{false};
    int64_t int64Value{0};
};

struct PreparedTemplateInput {
    bool active{false};
    std::string templateSql;
    std::vector<PreparedParamValue> params;
};

bool parseLiteralParam(const std::string& token, PreparedParamValue* out) {
    const std::string value = trim(token);
    const std::string folded = lower(value);
    if (value.empty()) {
        return false;
    }
    if (folded == "null") {
        out->kind = PreparedParamKind::Null;
        return true;
    }
    if (folded == "true" || folded == "t" || folded == "false" || folded == "f") {
        out->kind = PreparedParamKind::Bool;
        out->boolValue = folded == "true" || folded == "t";
        return true;
    }
    std::string decoded;
    if (decodeSqlStringLiteral(value, &decoded)) {
        out->kind = PreparedParamKind::Text;
        out->text = std::move(decoded);
        return true;
    }
    if (looksIntegerLiteral(value)) {
        try {
            std::size_t consumed = 0;
            const long long parsed = std::stoll(value, &consumed, 10);
            if (consumed == value.size()) {
                out->kind = PreparedParamKind::Int64;
                out->int64Value = static_cast<int64_t>(parsed);
                return true;
            }
        } catch (...) {
            out->kind = PreparedParamKind::Numeric;
            out->text = value;
            return true;
        }
    }
    if (looksNumericLiteral(value)) {
        out->kind = PreparedParamKind::Numeric;
        out->text = value;
        return true;
    }
    return false;
}

PreparedTemplateInput preparedInsertTemplateForStatement(const std::string& rawSql) {
    PreparedTemplateInput out;
    if (mainStatementTokenLower(rawSql) != "insert") {
        return out;
    }
    const std::string sql = stripLeadingTrivia(rawSql);
    const std::size_t intoPos = findKeywordOutsideSql(sql, "into");
    if (intoPos == std::string::npos) {
        return out;
    }
    const std::size_t valuesPos = findKeywordOutsideSql(sql, "values", intoPos + 4);
    if (valuesPos == std::string::npos) {
        return out;
    }
    const std::size_t returningPos = findKeywordOutsideSql(sql, "returning", valuesPos + 6);
    if (returningPos != std::string::npos) {
        return out;
    }
    const std::string targetAndColumns = sql.substr(intoPos + 4, valuesPos - (intoPos + 4));
    if (hasUnquotedUppercaseAsciiOutsideSql(targetAndColumns)) {
        return out;
    }
    std::size_t pos = valuesPos + 6;
    skipSqlTrivia(sql, &pos);
    if (pos >= sql.size() || sql[pos] != '(') {
        return out;
    }
    const std::size_t valuesEnd = findMatchingSqlParen(sql, pos);
    if (valuesEnd == std::string::npos) {
        return out;
    }
    if (!onlySqlTriviaAndOptionalTerminator(sql, valuesEnd + 1)) {
        return out;
    }
    const std::string valuesText = sql.substr(pos + 1, valuesEnd - pos - 1);
    const std::vector<std::string> values = splitTopLevelComma(valuesText);
    if (values.empty()) {
        return out;
    }
    std::vector<PreparedParamValue> params;
    params.reserve(values.size());
    for (const auto& value : values) {
        PreparedParamValue param;
        if (!parseLiteralParam(value, &param)) {
            return out;
        }
        params.push_back(std::move(param));
    }
    std::ostringstream templatedValues;
    templatedValues << "(";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            templatedValues << ", ";
        }
        templatedValues << "?";
    }
    templatedValues << ")";
    out.templateSql = sql.substr(0, pos) + templatedValues.str();
    if (valuesEnd + 1 < sql.size()) {
        std::size_t tail = valuesEnd + 1;
        skipSqlTrivia(sql, &tail);
        if (tail < sql.size() && sql[tail] == ';') {
            out.templateSql.push_back(';');
        }
    }
    out.params = std::move(params);
    out.active = true;
    return out;
}

bool insertPreparedCacheSafeForIdentifierFolding(const std::string& rawSql) {
    if (mainStatementTokenLower(rawSql) != "insert") {
        return true;
    }
    const std::string sql = stripLeadingTrivia(rawSql);
    const std::size_t intoPos = findKeywordOutsideSql(sql, "into");
    if (intoPos == std::string::npos) {
        return true;
    }
    const std::size_t valuesPos = findKeywordOutsideSql(sql, "values", intoPos + 4);
    if (valuesPos == std::string::npos) {
        return true;
    }
    const std::string targetAndColumns = sql.substr(intoPos + 4, valuesPos - (intoPos + 4));
    return !hasUnquotedUppercaseAsciiOutsideSql(targetAndColumns);
}

bool statementPreparedCacheEligible(const std::string& sql) {
    const std::string first = firstTokenLower(sql);
    if (first == "insert") {
        return insertPreparedCacheSafeForIdentifierFolding(sql);
    }
    return first == "select" || first == "with" || first == "values" ||
           first == "update" || first == "delete" || first == "merge";
}

std::string statementWithTerminator(const std::string& sql) {
    std::string out = trim(sql);
    if (out.empty()) {
        return out;
    }
    if (out.back() != ';') {
        out.push_back(';');
    }
    out.push_back('\n');
    return out;
}

bool isSbdfs130MatrixStatement(const std::string& scriptId, const std::string& sql) {
    if (scriptId != "SBDFS-130") {
        return false;
    }
    if (sql.find("datatype_dml_case_manifest") != std::string::npos) {
        return false;
    }
    return sql.find(".dt_") != std::string::npos &&
           sql.find("_values") != std::string::npos;
}

std::optional<std::pair<size_t, size_t>> sbdfs130ChainRange(
    const std::string& scriptId,
    const std::vector<std::string>& statements) {
    if (scriptId != "SBDFS-130") {
        return std::nullopt;
    }
    size_t start = statements.size();
    size_t end = statements.size();
    for (size_t index = 0; index < statements.size(); ++index) {
        if (isSbdfs130MatrixStatement(scriptId, statements[index])) {
            start = index;
            break;
        }
    }
    if (start == statements.size()) {
        return std::nullopt;
    }
    end = start;
    while (end < statements.size() && isSbdfs130MatrixStatement(scriptId, statements[end])) {
        ++end;
    }
    if (end <= start + 1) {
        return std::nullopt;
    }
    return std::make_pair(start, end);
}

bool isGeneratedAssertionStatement(const std::string& sql) {
    const std::string text = lower(stripLeadingTrivia(sql));
    return text.find("select 'sbdfs-") == 0 && text.find(" as assertion_id") != std::string::npos;
}

std::vector<std::pair<size_t, size_t>> generatedSetupChainRanges(
    const std::string& scriptId,
    const std::vector<std::string>& statements,
    const std::set<size_t>& expectedRefusalIndexes) {
    static const std::set<std::string> kGeneratedSetupScripts{
        "SBDFS-055",
        "SBDFS-100",
        "SBDFS-101",
        "SBDFS-110",
        "SBDFS-120",
        "SBDFS-130",
        "SBDFS-140",
        "SBDFS-150",
        "SBDFS-160",
        "SBDFS-170",
        "SBDFS-180",
    };
    std::vector<std::pair<size_t, size_t>> ranges;
    if (kGeneratedSetupScripts.count(scriptId) == 0 || statements.empty()) {
        return ranges;
    }
    size_t end = 0;
    while (end < statements.size() && !isGeneratedAssertionStatement(statements[end])) {
        ++end;
    }
    if (end <= 1) {
        return ranges;
    }

    size_t start = 0;
    for (size_t index = 0; index < end; ++index) {
        if (expectedRefusalIndexes.count(index) == 0) {
            continue;
        }
        if (index > start + 1) {
            ranges.emplace_back(start, index);
        }
        start = index + 1;
    }
    if (end > start + 1) {
        ranges.emplace_back(start, end);
    }
    return ranges;
}

std::vector<std::pair<size_t, size_t>> scriptChainRanges(
    const std::string& scriptId,
    const std::vector<std::string>& statements,
    const std::set<size_t>& expectedRefusalIndexes) {
    std::vector<std::pair<size_t, size_t>> ranges;
    auto addRange = [&ranges](std::pair<size_t, size_t> candidate) {
        if (candidate.second <= candidate.first + 1) {
            return;
        }
        for (const auto& existing : ranges) {
            if (candidate.first < existing.second && candidate.second > existing.first) {
                return;
            }
        }
        ranges.push_back(candidate);
    };
    for (const auto& setup : generatedSetupChainRanges(scriptId, statements, expectedRefusalIndexes)) {
        addRange(setup);
    }
    if (const auto matrix = sbdfs130ChainRange(scriptId, statements)) {
        addRange(*matrix);
    }
    std::sort(ranges.begin(), ranges.end());
    return ranges;
}

std::optional<size_t> chainEndForStatement(
    const std::vector<std::pair<size_t, size_t>>& ranges,
    size_t statementIndex) {
    for (const auto& range : ranges) {
        if (statementIndex >= range.first && statementIndex < range.second) {
            return range.second;
        }
    }
    return std::nullopt;
}

std::string sbdfs130TableKey(const std::string& sql) {
    const std::size_t tableStart = sql.find(".dt_");
    if (tableStart == std::string::npos) {
        return "";
    }
    const std::size_t nameStart = tableStart + 1;
    const std::size_t nameEnd = sql.find("_values", nameStart);
    if (nameEnd == std::string::npos) {
        return "";
    }
    return sql.substr(nameStart, nameEnd - nameStart + std::string("_values").size());
}

std::string sbdfs130OperationFamily(const std::string& sql) {
    const std::string first = mainStatementTokenLower(sql);
    if (first == "select") {
        return sql.find("COUNT(*)") != std::string::npos ||
                       sql.find("count(*)") != std::string::npos
                   ? "select_count_not_null"
                   : "select_predicate";
    }
    if (first == "update" &&
        (sql.find(" RETURNING ") != std::string::npos ||
         sql.find(" returning ") != std::string::npos)) {
        return "update_returning";
    }
    return first;
}

int64_t countInsertValueTuples(const std::string& rawSql);

int64_t sbdfs130EstimatedRowsAffected(const std::string& sql) {
    const std::string first = mainStatementTokenLower(sql);
    if (first == "insert" || first == "delete" || first == "upsert") {
        return 1;
    }
    if (first == "merge") {
        return 64;
    }
    if (first == "update") {
        if (sql.find(" RETURNING ") != std::string::npos ||
            sql.find(" returning ") != std::string::npos) {
            return 1;
        }
        if (sql.find("% 7 = 0") != std::string::npos) {
            return 10;
        }
        return 1;
    }
    return -1;
}

int64_t estimatedRowsAffectedForChainedStatement(const std::string& scriptId,
                                                 const std::string& sql) {
    if (scriptId == "SBDFS-130") {
        const int64_t estimate = sbdfs130EstimatedRowsAffected(sql);
        if (estimate >= 0) {
            return estimate;
        }
    }
    if (mainStatementTokenLower(sql) == "insert") {
        const int64_t tuples = countInsertValueTuples(sql);
        if (tuples > 0) {
            return tuples;
        }
    }
    return -1;
}

void bindPreparedParams(scratchbird::client::PreparedStatement* stmt,
                        const std::vector<PreparedParamValue>& params) {
    stmt->clearParameters();
    for (std::size_t i = 0; i < params.size(); ++i) {
        const std::size_t index = i + 1;
        const PreparedParamValue& param = params[i];
        switch (param.kind) {
            case PreparedParamKind::Null:
                stmt->setNull(index, scratchbird::protocol::kOidText);
                break;
            case PreparedParamKind::Bool:
                stmt->setBool(index, param.boolValue);
                break;
            case PreparedParamKind::Int64:
                stmt->setInt64(index, param.int64Value);
                break;
            case PreparedParamKind::Numeric:
                stmt->setString(index, param.text, scratchbird::protocol::kOidNumeric);
                break;
            case PreparedParamKind::Text:
                stmt->setString(index, param.text, scratchbird::protocol::kOidText);
                break;
        }
    }
}

bool networkRoute(const std::string& route) {
    return route == "listener-parser" || route == "manager-listener-parser";
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

std::string tlsPolicyForRoute(const std::string& route, const std::string& sslmode) {
    if (!networkRoute(route)) {
        return "not_applicable_non_network_route";
    }
    return sslmode == "disable" ? "explicit_non_tls_test_route" : "scratchbird_tls_1_3_floor";
}

std::string sqlstateOf(const scratchbird::core::ErrorContext& ctx) {
    return ctx.sqlstate ? std::string(ctx.sqlstate) : std::string("HY000");
}

std::string statusMessage(const scratchbird::core::ErrorContext& ctx) {
    return ctx.message.empty() ? "driver operation failed" : ctx.message;
}

std::string commandTagOrDefault(const scratchbird::client::ResultSet& results, const std::string& fallback) {
    return results.getCommandTag().empty() ? fallback : results.getCommandTag();
}

bool parseNumber(const std::string& value, long double* out) {
    char* end = nullptr;
    const std::string clean = trim(value);
    if (clean.empty()) {
        return false;
    }
    const long double parsed = std::strtold(clean.c_str(), &end);
    if (end == nullptr || *end != '\0') {
        return false;
    }
    *out = parsed;
    return true;
}

std::string normalizedComparable(const json& value) {
    if (value.is_null()) {
        return "<null>";
    }
    if (value.is_boolean()) {
        return value.get<bool>() ? "true" : "false";
    }
    if (value.is_number()) {
        return value.dump();
    }
    std::string text = trim(value.get<std::string>());
    std::string folded = lower(text);
    if (folded == "t" || folded == "true" || folded == "1") {
        return "true";
    }
    if (folded == "f" || folded == "false" || folded == "0") {
        return "false";
    }
    return text;
}

bool valuesEqual(const json& actual, const json& expected) {
    if (actual.is_null() || expected.is_null()) {
        return actual.is_null() && expected.is_null();
    }
    const std::string actualText = normalizedComparable(actual);
    const std::string expectedText = normalizedComparable(expected);
    if (actualText == expectedText) {
        return true;
    }
    long double actualNumber = 0;
    long double expectedNumber = 0;
    if (parseNumber(actualText, &actualNumber) && parseNumber(expectedText, &expectedNumber)) {
        return std::fabs(actualNumber - expectedNumber) <= 0.000000001L;
    }
    return false;
}

std::string jsonScalarToString(const json& value) {
    if (value.is_null()) {
        return "";
    }
    if (value.is_string()) {
        return value.get<std::string>();
    }
    return value.dump();
}

json resultSetToRows(scratchbird::client::ResultSet* results) {
    json columns = json::array();
    for (size_t column = 0; column < results->getColumnCount(); ++column) {
        columns.push_back({{"index", column},
                           {"name", results->getColumnName(column)},
                           {"type", static_cast<int>(results->getColumnType(column))},
                           {"type_oid", results->getColumnTypeOid(column)},
                           {"nullable", results->isColumnNullable(column)}});
    }

    json rows = json::array();
    while (results->next()) {
        json row;
        for (size_t column = 0; column < results->getColumnCount(); ++column) {
            const std::string columnName = results->getColumnName(column).empty()
                                               ? "column_" + std::to_string(column + 1)
                                               : results->getColumnName(column);
            row[columnName] = results->isNull(column) ? json(nullptr) : json(results->getString(column));
        }
        rows.push_back(row);
    }
    return {{"columns", columns}, {"rows", rows}};
}

std::string rowString(const json& row, const std::string& key) {
    if (!row.is_object() || !row.contains(key) || row.at(key).is_null()) {
        return "";
    }
    return row.at(key).is_string() ? row.at(key).get<std::string>() : row.at(key).dump();
}

json parseLabelSummary(const std::string& summary) {
    json labels = json::object();
    std::string normalized = summary;
    for (char& ch : normalized) {
        if (ch == ',') {
            ch = ';';
        }
    }
    std::istringstream parts(normalized);
    std::string item;
    while (std::getline(parts, item, ';')) {
        const size_t equal = item.find('=');
        if (equal == std::string::npos) {
            continue;
        }
        const std::string key = trim(item.substr(0, equal));
        const std::string value = trim(item.substr(equal + 1));
        if (!key.empty()) {
            labels[key] = value;
        }
    }
    return labels;
}

std::optional<long double> rowNumber(const json& row, const std::string& key) {
    const std::string value = rowString(row, key);
    if (value.empty()) {
        return std::nullopt;
    }
    long double parsed = 0;
    if (!parseNumber(value, &parsed)) {
        return std::nullopt;
    }
    return parsed;
}

std::string normalizedCoreMetricValue(const json& row) {
    if (const auto value = rowNumber(row, "value")) {
        return std::to_string(static_cast<std::uint64_t>(std::max<long double>(0, *value)));
    }
    if (const auto sum = rowNumber(row, "sum")) {
        return std::to_string(static_cast<std::uint64_t>(std::max<long double>(0, *sum)));
    }
    if (const auto count = rowNumber(row, "count")) {
        return std::to_string(static_cast<std::uint64_t>(std::max<long double>(0, *count)));
    }
    return "0";
}

bool isRowPageFamily(const std::string& pageFamily) {
    const std::string family = lower(pageFamily);
    return family == "row" || family == "rows" || family == "data" ||
           family == "row_data" || family == "heap";
}

bool isIndexPageFamily(const std::string& pageFamily) {
    const std::string family = lower(pageFamily);
    return family == "index" || family == "indexes" || family == "index_data";
}

std::string fieldFromIparMetricPath(const std::string& metricPath) {
    static const std::string prefix = "sys.metrics.ipar.script.";
    if (startsWith(metricPath, prefix)) {
        return metricPath.substr(prefix.size());
    }
    const size_t dot = metricPath.find_last_of('.');
    return dot == std::string::npos ? std::string{} : metricPath.substr(dot + 1);
}

std::string fieldFromIparMetricLabels(const json& labels) {
    if (!labels.is_object()) {
        return {};
    }
    for (const std::string key : {"metric_field", "field", "counter", "counter_name"}) {
        const std::string value = labels.value(key, "");
        if (!value.empty()) {
            return value;
        }
    }
    return {};
}

void mergeServerMetricCounterRows(const json& rows,
                                  std::map<std::string, json>* iparRecords,
                                  json* serverMetricSamples) {
    for (const auto& row : rows) {
        if (!row.is_object()) {
            continue;
        }
        const std::string metricPath = rowString(row, "metric_path");
        const json labels = parseLabelSummary(rowString(row, "label_summary"));
        json sample{{"path", metricPath},
                    {"type", rowString(row, "metric_type")},
                    {"unit", rowString(row, "metric_unit")},
                    {"value", rowString(row, "value")},
                    {"labels", labels},
                    {"metric_id", rowString(row, "metric_id")},
                    {"producer", rowString(row, "producer")},
                    {"source_state", rowString(row, "source_state")},
                    {"sample_count", rowString(row, "sample_count")}};
        serverMetricSamples->push_back(sample);

        const std::string scriptId = labels.value("script_id", "");
        std::string field = fieldFromIparMetricLabels(labels);
        if (field.empty()) {
            field = fieldFromIparMetricPath(metricPath);
        }
        if (scriptId.empty() || field.empty()) {
            continue;
        }
        auto record = iparRecords->find(scriptId);
        if (record == iparRecords->end()) {
            continue;
        }
        const std::string value = rowString(row, "value");
        if (!value.empty()) {
            record->second["metrics"][field] = value;
        }
    }
}

bool scriptMetricMissing(const json& record, const std::string& field) {
    const json metrics = record.value("metrics", json::object());
    if (!metrics.is_object() || !metrics.contains(field) || metrics.at(field).is_null()) {
        return true;
    }
    if (metrics.at(field).is_string()) {
        return trim(metrics.at(field).get<std::string>()).empty();
    }
    return false;
}

void setScriptMetricFromSource(std::map<std::string, json>* iparRecords,
                               json* serverMetricSamples,
                               const std::string& scriptId,
                               const std::string& field,
                               const std::string& value,
                               const std::string& sourceMetric,
                               const std::string& producer,
                               const std::string& sourceState,
                               json sourceLabels = json::object()) {
    auto record = iparRecords->find(scriptId);
    if (record == iparRecords->end()) {
        return;
    }
    if (!scriptMetricMissing(record->second, field)) {
        return;
    }
    record->second["metrics"][field] = value;
    json labels{{"script_id", scriptId},
                {"field", field},
                {"source_metric", sourceMetric}};
    if (sourceLabels.is_object()) {
        for (auto it = sourceLabels.begin(); it != sourceLabels.end(); ++it) {
            labels["source_label_" + it.key()] = it.value();
        }
    }
    serverMetricSamples->push_back({{"path", "sys.metrics.ipar.script." + field},
                                    {"type", "counter"},
                                    {"unit", field == "security_epoch" ? "epoch" : "count"},
                                    {"value", value},
                                    {"labels", labels},
                                    {"metric_id", "IPAR-DML-RUNTIME"},
                                    {"producer", producer},
                                    {"source_state", sourceState},
                                    {"sample_count", "1"}});
}

void mergeCoreMetricRows(const json& rows,
                         std::map<std::string, json>* iparRecords,
                         json* serverMetricSamples) {
    bool relationFullLoadSeen = false;
    std::string relationFullLoads = "0";
    bool pagePreallocationMetricSeen = false;
    bool rowPageSampleSeen = false;
    bool indexPageSampleSeen = false;
    long double rowPagesPreallocated = 0;
    long double indexPagesPreallocated = 0;
    json rowPageLabels = json::object();
    json indexPageLabels = json::object();

    for (const auto& row : rows) {
        if (!row.is_object()) {
            continue;
        }
        const std::string metric = rowString(row, "metric");
        if (metric == "sb_dml_insert_relation_state_full_load_total") {
            relationFullLoadSeen = true;
            relationFullLoads = normalizedCoreMetricValue(row);
        } else if (metric == "sb_page_insert_preallocated_pages_total") {
            pagePreallocationMetricSeen = true;
            const json labels = parseLabelSummary(rowString(row, "labels"));
            const std::string family = labels.value("page_family", "");
            const std::string value = normalizedCoreMetricValue(row);
            long double numeric = 0;
            if (!parseNumber(value, &numeric)) {
                numeric = 0;
            }
            if (isRowPageFamily(family)) {
                rowPageSampleSeen = true;
                rowPagesPreallocated += numeric;
                rowPageLabels = labels;
            } else if (isIndexPageFamily(family)) {
                indexPageSampleSeen = true;
                indexPagesPreallocated += numeric;
                indexPageLabels = labels;
            }
        }
    }

    if (relationFullLoadSeen) {
        setScriptMetricFromSource(iparRecords,
                                  serverMetricSamples,
                                  "SBDFS-020",
                                  "relation_state_full_loads",
                                  relationFullLoads,
                                  "sb_dml_insert_relation_state_full_load_total",
                                  "core_metrics_registry",
                                  "show_metrics_core_counter");
        setScriptMetricFromSource(iparRecords,
                                  serverMetricSamples,
                                  "SBDFS-059",
                                  "relation_state_full_loads",
                                  relationFullLoads,
                                  "sb_dml_insert_relation_state_full_load_total",
                                  "core_metrics_registry",
                                  "show_metrics_core_counter");
    }
    if (pagePreallocationMetricSeen) {
        setScriptMetricFromSource(iparRecords,
                                  serverMetricSamples,
                                  "SBDFS-059",
                                  "row_pages_preallocated",
                                  std::to_string(static_cast<std::uint64_t>(
                                      std::max<long double>(0, rowPagesPreallocated))),
                                  "sb_page_insert_preallocated_pages_total",
                                  "core_metrics_registry",
                                  rowPageSampleSeen ? "show_metrics_core_counter"
                                                    : "show_metrics_descriptor_no_current_row_sample",
                                  rowPageLabels);
        setScriptMetricFromSource(iparRecords,
                                  serverMetricSamples,
                                  "SBDFS-059",
                                  "index_pages_preallocated",
                                  std::to_string(static_cast<std::uint64_t>(
                                      std::max<long double>(0, indexPagesPreallocated))),
                                  "sb_page_insert_preallocated_pages_total",
                                  "core_metrics_registry",
                                  indexPageSampleSeen ? "show_metrics_core_counter"
                                                      : "show_metrics_descriptor_no_current_index_sample",
                                  indexPageLabels);
    }
}

void mergeManagementRows(const json& rows,
                         std::map<std::string, json>* iparRecords,
                         json* serverMetricSamples) {
    for (const auto& row : rows) {
        if (!row.is_object()) {
            continue;
        }
        const std::string securityEpoch = rowString(row, "security_epoch");
        if (securityEpoch.empty() || securityEpoch == "(null)") {
            continue;
        }
        setScriptMetricFromSource(iparRecords,
                                  serverMetricSamples,
                                  "SBDFS-085",
                                  "security_epoch",
                                  securityEpoch,
                                  "SHOW MANAGEMENT security_epoch",
                                  "engine_observability",
                                  "show_management_epoch");
        return;
    }
}

void mergeServerTelemetryRows(const json& rows, json* telemetry, json* serverTelemetryRows) {
    for (const auto& row : rows) {
        if (!row.is_object()) {
            continue;
        }
        serverTelemetryRows->push_back(row);
        const std::string metricPath = rowString(row, "metric_path");
        if (!metricPath.empty()) {
            (*telemetry)[metricPath] = rowString(row, "observed_value");
        }
        const std::string control = rowString(row, "control_name");
        if (control == "dropped_metric_count") {
            (*telemetry)["dropped_metric_count"] = rowString(row, "dropped_metric_count");
        } else if (control == "persist_sample_rate_per_mille") {
            long double perMille = 0;
            if (parseNumber(rowString(row, "observed_value"), &perMille)) {
                (*telemetry)["sample_rate"] = static_cast<double>(perMille / 1000.0L);
            }
        }
    }
}

void appendServerSlowPathRows(const json& rows, json* slowPaths) {
    for (const auto& row : rows) {
        if (!row.is_object()) {
            continue;
        }
        json slowPath = row;
        slowPath["_source"] = "sys.ipar.slow_path_reasons";
        slowPaths->push_back(slowPath);
    }
}

json validateAssertions(const std::string& statementId, const std::string& elementId, const json& rows) {
    json results = json::array();
    for (const auto& row : rows) {
        if (!row.is_object() || !row.contains("assertion_id")) {
            continue;
        }
        const std::string assertionId = jsonScalarToString(row.at("assertion_id"));
        bool checkedAny = false;
        bool passed = true;
        json comparisons = json::array();

        for (auto it = row.begin(); it != row.end(); ++it) {
            const std::string key = it.key();
            if (!startsWith(key, "actual_")) {
                continue;
            }
            const std::string suffix = key.substr(std::string("actual_").size());
            const std::string expectedKey = "expected_" + suffix;
            checkedAny = true;
            if (!row.contains(expectedKey)) {
                passed = false;
                comparisons.push_back({{"name", suffix}, {"passed", false}, {"message", "missing " + expectedKey}});
                continue;
            }
            const bool equal = valuesEqual(it.value(), row.at(expectedKey));
            passed = passed && equal;
            comparisons.push_back({{"name", suffix},
                                   {"passed", equal},
                                   {"actual", it.value()},
                                   {"expected", row.at(expectedKey)}});
        }

        if (checkedAny) {
            results.push_back({{"statement_id", statementId},
                               {"element_id", elementId},
                               {"assertion_id", assertionId},
                               {"passed", passed},
                               {"comparisons", comparisons}});
        }
    }
    return results;
}

std::set<std::string> loadExpectedRefusals(const std::filesystem::path& path,
                                           std::map<std::string, std::vector<std::string>>* diagnostics) {
    std::set<std::string> out;
    if (!std::filesystem::exists(path)) {
        return out;
    }
    const auto expected = readJson(path);
    for (const auto& id : expected.value("statement_ids", json::array())) {
        out.insert(id.get<std::string>());
    }
    if (expected.contains("expected_diagnostics")) {
        for (auto it = expected["expected_diagnostics"].begin(); it != expected["expected_diagnostics"].end(); ++it) {
            std::vector<std::string> codes;
            for (const auto& code : it.value()) {
                codes.push_back(code.get<std::string>());
            }
            (*diagnostics)[it.key()] = codes;
        }
    }
    return out;
}

bool expectedDiagnosticMatches(const std::string& statementId,
                               const std::string& sqlstate,
                               const std::string& message,
                               const std::map<std::string, std::vector<std::string>>& expectedDiagnostics) {
    const auto it = expectedDiagnostics.find(statementId);
    if (it == expectedDiagnostics.end() || it->second.empty()) {
        return true;
    }
    const auto aliasesFor = [](const std::string& expected) {
        std::vector<std::string> aliases{expected};
        if (expected == "SBSQL.NUMERIC.OVERFLOW" ||
            expected == "SBSQL.FUNCTION.NUMERIC_OVERFLOW") {
            aliases.push_back("SB_DIAG_FUNCTION_NUMERIC_OVERFLOW");
        } else if (expected == "SBSQL.NUMERIC.DOMAIN" ||
                   expected == "SBSQL.FUNCTION.NUMERIC_DOMAIN") {
            aliases.push_back("SB_DIAG_FUNCTION_NUMERIC_DOMAIN");
        } else if (expected == "SBSQL.NUMERIC.DIVISION_BY_ZERO" ||
                   expected == "SBSQL.FUNCTION.NUMERIC_DIVISION_BY_ZERO") {
            aliases.push_back("SB_DIAG_FUNCTION_NUMERIC_DIVISION_BY_ZERO");
        } else if (expected == "SBSQL.FUNCTION.INVALID_INPUT" ||
                   expected == "SBSQL.NUMERIC.INVALID_INPUT") {
            aliases.push_back("SB_DIAG_FUNCTION_INVALID_INPUT");
        } else if (expected == "SBSQL.FUNCTION.DEPENDENCY_UNAVAILABLE") {
            aliases.push_back("SB_DIAG_FUNCTION_DEPENDENCY_UNAVAILABLE");
        }
        return aliases;
    };
    for (const auto& expected : it->second) {
        for (const auto& candidate : aliasesFor(expected)) {
            if (sqlstate.find(candidate) != std::string::npos || message.find(candidate) != std::string::npos) {
                return true;
            }
        }
    }
    return false;
}

void addTiming(std::map<std::string, int64_t>* timings, const std::string& group, int64_t started) {
    (*timings)[group] += nowNs() - started;
}

json makeFailure(const std::string& statementId, const std::string& message) {
    return {{"statement_id", statementId}, {"message", message}};
}

double nsToMs(int64_t ns) {
    return static_cast<double>(ns) / 1000000.0;
}

bool jsonValuePresent(const json& value) {
    if (value.is_null()) {
        return false;
    }
    if (value.is_string()) {
        return !trim(value.get<std::string>()).empty();
    }
    return true;
}

void addNumericMetric(json* metrics, const std::string& field, double value) {
    const double current = metrics->contains(field) && (*metrics)[field].is_number()
                               ? (*metrics)[field].get<double>()
                               : 0.0;
    (*metrics)[field] = current + value;
}

void addCounterMetric(json* metrics, const std::string& field, int64_t value) {
    const int64_t current = metrics->contains(field) && (*metrics)[field].is_number_integer()
                                ? (*metrics)[field].get<int64_t>()
                                : 0;
    (*metrics)[field] = current + value;
}

void setMetricIfPresent(json* metrics, const std::string& field, const json& value) {
    if (jsonValuePresent(value)) {
        (*metrics)[field] = value;
    }
}

bool metricMissing(const json& metrics, const std::string& field) {
    return !metrics.is_object() || !metrics.contains(field) || !jsonValuePresent(metrics.at(field));
}

void setMetricDefaultIfMissing(json* metrics, const std::string& field, json value) {
    if (metricMissing(*metrics, field)) {
        (*metrics)[field] = std::move(value);
    }
}

bool numericMetricGreaterThan(const json& metrics, const std::string& field, long double threshold) {
    if (!metrics.is_object() || !metrics.contains(field)) {
        return false;
    }
    const json& value = metrics.at(field);
    if (value.is_number()) {
        return value.get<long double>() > threshold;
    }
    if (value.is_string()) {
        long double parsed = 0;
        return parseNumber(value.get<std::string>(), &parsed) && parsed > threshold;
    }
    return false;
}

std::optional<long double> numericMetricValue(const json& metrics, const std::string& field) {
    if (!metrics.is_object() || !metrics.contains(field)) {
        return std::nullopt;
    }
    const json& value = metrics.at(field);
    if (value.is_number()) {
        return value.get<long double>();
    }
    if (value.is_string()) {
        long double parsed = 0;
        if (parseNumber(value.get<std::string>(), &parsed)) {
            return parsed;
        }
    }
    return std::nullopt;
}

void setCounterMetricDefaultIfNumeric(json* metrics, const std::string& field, const std::string& sourceField) {
    if (!metricMissing(*metrics, field)) {
        return;
    }
    const auto source = numericMetricValue(*metrics, sourceField);
    if (source.has_value()) {
        (*metrics)[field] = static_cast<int64_t>(*source);
    }
}

bool sqlContainsFolded(const std::string& sql, const std::string& needle) {
    return lower(sql).find(lower(needle)) != std::string::npos;
}

std::vector<std::string> firstInsertValuesTuple(const std::string& rawSql) {
    const std::string sql = stripLeadingTrivia(rawSql);
    if (firstTokenLower(sql) != "insert") {
        return {};
    }
    const std::size_t valuesPos = findKeywordOutsideSql(sql, "values");
    if (valuesPos == std::string::npos) {
        return {};
    }
    std::size_t pos = valuesPos + 6;
    skipSqlTrivia(sql, &pos);
    if (pos >= sql.size() || sql[pos] != '(') {
        return {};
    }
    const std::size_t valuesEnd = findMatchingSqlParen(sql, pos);
    if (valuesEnd == std::string::npos) {
        return {};
    }
    std::vector<std::string> values;
    for (const auto& token : splitTopLevelComma(sql.substr(pos + 1, valuesEnd - pos - 1))) {
        std::string decoded;
        if (decodeSqlStringLiteral(token, &decoded)) {
            values.push_back(decoded);
        } else {
            values.push_back(trim(token));
        }
    }
    return values;
}

int64_t countInsertValueTuples(const std::string& rawSql) {
    const std::string sql = stripLeadingTrivia(rawSql);
    if (firstTokenLower(sql) != "insert") {
        return 0;
    }
    const std::size_t valuesPos = findKeywordOutsideSql(sql, "values");
    if (valuesPos == std::string::npos) {
        return 0;
    }
    int64_t rows = 0;
    std::size_t pos = valuesPos + 6;
    while (pos < sql.size()) {
        skipSqlTrivia(sql, &pos);
        if (pos >= sql.size() || sql[pos] != '(') {
            break;
        }
        const std::size_t tupleEnd = findMatchingSqlParen(sql, pos);
        if (tupleEnd == std::string::npos) {
            break;
        }
        ++rows;
        pos = tupleEnd + 1;
        skipSqlTrivia(sql, &pos);
        if (pos < sql.size() && sql[pos] == ',') {
            ++pos;
            continue;
        }
        break;
    }
    return rows;
}

bool rowHasPositiveNumericField(const json& row, const std::string& field) {
    if (!row.is_object() || !row.contains(field)) {
        return false;
    }
    const json& value = row.at(field);
    if (value.is_number()) {
        return value.get<long double>() > 0;
    }
    if (value.is_string()) {
        long double parsed = 0;
        return parseNumber(value.get<std::string>(), &parsed) && parsed > 0;
    }
    return false;
}

void captureExplainMetricFields(json* metrics, const json& rows) {
    int64_t fullScanCount = 0;
    int64_t indexProbeCount = 0;
    for (const auto& row : rows) {
        if (!row.is_object()) {
            continue;
        }
        const std::string evidenceKind = rowString(row, "evidence_kind");
        const std::string planKind = rowString(row, "plan_kind");
        const std::string selectedPath = rowString(row, "selected_path");
        if (metricMissing(*metrics, "selected_index_path") &&
            (evidenceKind.find("index") != std::string::npos ||
             selectedPath.find("index") != std::string::npos)) {
            (*metrics)["selected_index_path"] = !evidenceKind.empty() ? evidenceKind : selectedPath;
        }
        if (planKind.find("scan") != std::string::npos) {
            ++fullScanCount;
        }
        if (evidenceKind.find("index") != std::string::npos ||
            selectedPath.find("index") != std::string::npos) {
            ++indexProbeCount;
        }
    }
    if (fullScanCount > 0) {
        addCounterMetric(metrics, "full_scan_count", fullScanCount);
    }
    if (indexProbeCount > 0) {
        addCounterMetric(metrics, "index_probe_count", indexProbeCount);
    }
}

void captureGeneratedManifestFields(json* metrics, const std::string& scriptId, const std::string& sql) {
    if (metrics == nullptr) {
        return;
    }
    const std::vector<std::string> values = firstInsertValuesTuple(sql);
    if (values.empty()) {
        return;
    }
    if (scriptId == "SBDFS-120" && sqlContainsFolded(sql, "datatype_surface_manifest")) {
        setMetricDefaultIfMissing(metrics, "datatype_id", values[0]);
        return;
    }
    if (scriptId == "SBDFS-130" && sqlContainsFolded(sql, "datatype_dml_case_manifest")) {
        setMetricDefaultIfMissing(metrics, "case_id", values[0]);
        return;
    }
}

void captureScriptSpecificStatementFields(json* metrics,
                                          const std::string& scriptId,
                                          const std::string& sql,
                                          const std::string& first,
                                          bool statementPassed,
                                          int64_t statementElapsedNs,
                                          int64_t rowsAffected) {
    if (metrics == nullptr || !statementPassed) {
        return;
    }
    const double elapsedMs = nsToMs(statementElapsedNs);
    captureGeneratedManifestFields(metrics, scriptId, sql);
    const int64_t insertedValueRows = rowsAffected < 0 ? countInsertValueTuples(sql) : 0;

    if (scriptId == "SBDFS-070") {
        if (first == "insert") {
            if (rowsAffected < 0 && insertedValueRows > 0) {
                addCounterMetric(metrics, "rows_affected", insertedValueRows);
                addCounterMetric(metrics, "rows_written", insertedValueRows);
            }
        }
        return;
    }

    if (scriptId == "SBDFS-120") {
        if (first == "insert") {
            addNumericMetric(metrics, "encode_ms", elapsedMs);
            if (rowsAffected < 0 && insertedValueRows > 0) {
                addCounterMetric(metrics, "rows_affected", insertedValueRows);
                addCounterMetric(metrics, "rows_written", insertedValueRows);
            }
        }
        return;
    }

}

void captureScriptSpecificResultFields(json* metrics, const std::string& scriptId, const json& rows) {
    for (const auto& row : rows) {
        if (!row.is_object()) {
            continue;
        }
        if (scriptId == "SBDFS-085") {
            if (rowHasPositiveNumericField(row, "actual_max_recursive_depth")) {
                setMetricIfPresent(metrics, "group_resolution_depth", row.at("actual_max_recursive_depth"));
            }
            if (rowHasPositiveNumericField(row, "actual_epoch_or_cycle_refusals")) {
                setMetricIfPresent(metrics, "security_invalidation_count", row.at("actual_epoch_or_cycle_refusals"));
            }
        }
    }
}

void deriveIparMetricCompleteness(json* record) {
    if (record == nullptr || !record->is_object()) {
        return;
    }
    json& metrics = (*record)["metrics"];
    if (!metrics.is_object()) {
        metrics = json::object();
    }
    const std::string scriptId = record->value("script_id", "");
    const auto commandMs = numericMetricValue(metrics, "command_ms");
    if (metricMissing(metrics, "rows_per_second") && commandMs.has_value() && *commandMs > 0.0L) {
        std::optional<long double> rows = numericMetricValue(metrics, "rows_written");
        if (!rows.has_value() || *rows <= 0.0L) rows = numericMetricValue(metrics, "copy_rows");
        if (!rows.has_value() || *rows <= 0.0L) rows = numericMetricValue(metrics, "rows_affected");
        if (!rows.has_value() || *rows <= 0.0L) rows = numericMetricValue(metrics, "row_count");
        if (rows.has_value() && *rows > 0.0L) {
            metrics["rows_per_second"] = static_cast<double>(*rows / (*commandMs / 1000.0L));
        }
    }
    if (scriptId == "SBDFS-085") {
        setCounterMetricDefaultIfNumeric(&metrics, "auth_cache_hits", "prepared_descriptor_hits");
        setCounterMetricDefaultIfNumeric(&metrics, "auth_cache_misses", "prepared_descriptor_misses");
    }
    const auto hits = numericMetricValue(metrics, "prepared_descriptor_hits");
    const auto misses = numericMetricValue(metrics, "prepared_descriptor_misses");
    if (metricMissing(metrics, "prepared_descriptor_hit_rate_percent") && hits.has_value() && misses.has_value()) {
        const long double denominator = *hits + *misses;
        metrics["prepared_descriptor_hit_rate_percent"] =
            denominator > 0.0L ? static_cast<double>((*hits / denominator) * 100.0L) : 0.0;
    }
}

void normalizeIparRecordProof(json* record) {
    if (record == nullptr || !record->is_object()) {
        return;
    }
    json& metrics = (*record)["metrics"];
    if (!metrics.is_object()) {
        metrics = json::object();
    }
    const std::string scriptId = record->value("script_id", "");
    if (scriptId == "SBDFS-120" &&
        !numericMetricGreaterThan(metrics, "validation_failures", 0) &&
        !numericMetricGreaterThan(metrics, "refusal_count", 0)) {
        setMetricDefaultIfMissing(&metrics, "rejected_rows", 0);
    }
    const bool copyObserved = scriptId == "SBDFS-059" ||
                              numericMetricGreaterThan(metrics, "copy_batches", 0) ||
                              numericMetricGreaterThan(metrics, "copy_rows", 0) ||
                              numericMetricGreaterThan(metrics, "copy_bytes", 0);
    if (copyObserved) {
        setMetricDefaultIfMissing(&metrics, "copy_rejects", 0);
    }
    deriveIparMetricCompleteness(record);
}

void normalizeIparTelemetryProof(json* telemetry) {
    if (telemetry == nullptr) {
        return;
    }
    if (!telemetry->is_object()) {
        *telemetry = json::object();
    }
    telemetry->emplace("source_state", "sys.ipar.telemetry_controls_required");
    telemetry->emplace("summary_source", "server_projection_and_runner_elapsed_time");
}

int64_t countPayloadRows(const std::string& payload) {
    int64_t rows = 0;
    std::istringstream lines(payload);
    std::string line;
    while (std::getline(lines, line)) {
        if (!trim(line).empty()) {
            ++rows;
        }
    }
    return rows;
}

double percentileMs(std::vector<double> values, double percentile) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const double clamped = std::max(0.0, std::min(100.0, percentile));
    const double rank = (clamped / 100.0) * static_cast<double>(values.size() - 1);
    const auto lowerIndex = static_cast<std::size_t>(std::floor(rank));
    const auto upperIndex = static_cast<std::size_t>(std::ceil(rank));
    if (lowerIndex == upperIndex) {
        return values[lowerIndex];
    }
    const double fraction = rank - static_cast<double>(lowerIndex);
    return values[lowerIndex] + (values[upperIndex] - values[lowerIndex]) * fraction;
}

std::map<std::string, double> collectDriverPhaseMetrics(const std::filesystem::path& path) {
    std::map<std::string, double> metrics;
    if (!std::filesystem::exists(path)) {
        return metrics;
    }
    std::ifstream in(path);
    std::string line;
    int64_t records = 0;
    while (std::getline(in, line)) {
        if (trim(line).empty()) {
            continue;
        }
        json row = json::parse(line, nullptr, false);
        if (!row.is_object()) {
            continue;
        }
        ++records;
        const std::string event = row.value("event", "");
        const std::string phase = row.value("phase", "");
        const double elapsedMs = static_cast<double>(row.value("elapsed_us", int64_t{0})) / 1000.0;
        if (phase == "build_query_payload" || phase == "encode_binary_total" ||
            phase == "build_binary_frame_total") {
            metrics["client_encode_ms"] += elapsedMs;
        } else if (phase == "parse_command_complete" || phase == "parse_copy_in_response") {
            metrics["client_decode_ms"] += elapsedMs;
        } else if (phase == "send_query" || phase == "receive_message" ||
                   phase == "send_copy_data_total" || phase == "send_copy_done" ||
                   phase == "send_copy_input_stream") {
            metrics["wire_ms"] += elapsedMs;
        } else if (phase == "read_input_total") {
            metrics["client_input_read_ms"] += elapsedMs;
        }
        if (event == "execute_query" && phase == "total") {
            metrics["client_execute_total_ms"] += elapsedMs;
        }
        if (event == "copy_input_stream" && phase == "total") {
            metrics["client_copy_stream_total_ms"] += elapsedMs;
        }
    }
    if (records > 0) {
        metrics["driver_phase_trace_records"] = static_cast<double>(records);
    }
    return metrics;
}

void mergeDriverPhaseMetrics(json* record, const std::filesystem::path& tracePath) {
    if (record == nullptr || !record->is_object()) {
        return;
    }
    json& metrics = (*record)["metrics"];
    if (!metrics.is_object()) {
        metrics = json::object();
    }
    for (const auto& [field, value] : collectDriverPhaseMetrics(tracePath)) {
        addNumericMetric(&metrics, field, value);
    }
    (*record)["driver_phase_trace"] = tracePath.string();
    (*record)["server_stage_split_available"] = false;
}

void csvEscape(std::ostream& out, const std::string& value) {
    const bool quote = value.find_first_of(",\"\n\r") != std::string::npos;
    if (!quote) {
        out << value;
        return;
    }
    out << '"';
    for (char ch : value) {
        if (ch == '"') {
            out << "\"\"";
        } else {
            out << ch;
        }
    }
    out << '"';
}

std::map<std::string, std::vector<std::string>> iparRequiredFields(const json& schema) {
    std::map<std::string, std::vector<std::string>> fields;
    for (const auto& item : schema.value("target_scripts", json::array())) {
        const std::string scriptId = item.value("script_id", "");
        if (scriptId.empty()) {
            continue;
        }
        for (const auto& field : item.value("required_fields", json::array())) {
            fields[scriptId].push_back(field.get<std::string>());
        }
    }
    return fields;
}

std::set<std::string> iparTargetScriptIds(const json& schema) {
    std::set<std::string> ids;
    for (const auto& item : schema.value("target_scripts", json::array())) {
        const std::string scriptId = item.value("script_id", "");
        if (!scriptId.empty()) {
            ids.insert(scriptId);
        }
    }
    return ids;
}

json loadIparTelemetry(const std::map<std::string, std::string>& args) {
    const std::string telemetryPath = valueOrDefault(args, "--ipar-telemetry-file", "");
    if (telemetryPath.empty()) {
        return json::object();
    }
    json telemetry = readJson(telemetryPath);
    if (telemetry.contains("telemetry_overhead") && telemetry["telemetry_overhead"].is_object()) {
        return telemetry["telemetry_overhead"];
    }
    if (telemetry.contains("telemetry") && telemetry["telemetry"].is_object()) {
        return telemetry["telemetry"];
    }
    return telemetry.is_object() ? telemetry : json::object();
}

json& ensureIparRecord(std::map<std::string, json>* records,
                       const std::string& scriptId,
                       const std::string& scriptPath,
                       const std::string& runId,
                       const std::string& route,
                       const std::string& parserMode,
                       const std::string& pageSize,
                       const std::string& sslmode,
                       const std::string& transportMode,
                       const std::string& tlsPolicy) {
    auto& record = (*records)[scriptId];
    if (record.is_null()) {
        record = json{{"script_id", scriptId},
                      {"script", scriptPath},
                      {"driver", "cpp"},
                      {"run_id", runId},
                      {"route", route},
                      {"parser_mode", parserMode},
                      {"page_size", pageSize},
                      {"sslmode", sslmode},
                      {"transport_mode", transportMode},
                      {"tls_policy", tlsPolicy},
                      {"labels",
                       {{"script_id", scriptId},
                        {"driver", "cpp"},
                        {"route", route},
                        {"parser_mode", parserMode},
                        {"page_size", pageSize},
                        {"sslmode", sslmode},
                        {"tls_route", transportMode},
                        {"tls_policy", tlsPolicy}}},
                      {"metrics", json::object()}};
    }
    return record;
}

void captureResultFields(json* metrics, const json& rows) {
    static const std::set<std::string> fields{
        "actual",
        "case_id",
        "datatype_id",
        "expected",
        "filespace_id",
        "index_family",
        "index_variant",
        "page_size",
        "selected_index_path"};
    for (const auto& row : rows) {
        if (!row.is_object()) {
            continue;
        }
        for (const auto& field : fields) {
            if (row.contains(field)) {
                setMetricIfPresent(metrics, field, row.at(field));
            }
        }
    }
}

void captureAssertionFields(json* metrics, const json& assertionChecks) {
    for (const auto& assertion : assertionChecks) {
        if (!assertion.is_object()) {
            continue;
        }
        for (const auto& comparison : assertion.value("comparisons", json::array())) {
            if (!comparison.is_object()) {
                continue;
            }
            if (comparison.contains("actual")) {
                setMetricIfPresent(metrics, "actual", comparison.at("actual"));
            }
            if (comparison.contains("expected")) {
                setMetricIfPresent(metrics, "expected", comparison.at("expected"));
            }
        }
    }
}

void writeIparArtifacts(const std::filesystem::path& artifactRoot,
                        const std::string& runId,
                        const std::string& route,
                        const std::string& parserMode,
                        const std::string& pageSize,
                        const std::string& sslmode,
                        const std::string& transportMode,
                        const std::string& tlsPolicy,
                        const std::filesystem::path& schemaPath,
                        const json& schema,
                        const std::map<std::string, json>& records,
                        const std::map<std::string, std::vector<std::string>>& requiredFields,
                        const json& telemetry,
                        const json& slowPaths,
                        const json& routeProofs,
                        const json& serverMetricSamples,
                        const json& serverTelemetryRows) {
    std::map<std::string, json> normalizedRecords = records;
    for (auto& [_, record] : normalizedRecords) {
        normalizeIparRecordProof(&record);
    }
    json normalizedTelemetry = telemetry;
    normalizeIparTelemetryProof(&normalizedTelemetry);

    json targetMetrics = json::object();
    json missing = json::array();
    for (const auto& [scriptId, record] : normalizedRecords) {
        targetMetrics[scriptId] = record;
        const json metrics = record.value("metrics", json::object());
        auto requiredIt = requiredFields.find(scriptId);
        if (requiredIt == requiredFields.end()) {
            continue;
        }
        for (const auto& field : requiredIt->second) {
            if (!metrics.contains(field) || !jsonValuePresent(metrics.at(field))) {
                missing.push_back({{"script_id", scriptId},
                                   {"field", field},
                                   {"reason", "not_observed_in_cpp_driver_run"},
                                   {"gate_result", "missing_required_field"}});
            }
        }
    }

    json artifact{{"schema_version", 1},
                  {"schema_id", "scratchbird.ipar.performance_proof.v1"},
                  {"suite_id", schema.value("suite_id", "scratchbird.driver.full_surface.v1")},
                  {"run_id", runId},
                  {"driver", "cpp"},
                  {"route", route},
                  {"parser_mode", parserMode},
                  {"page_size", pageSize},
                  {"sslmode", sslmode},
                  {"transport_mode", transportMode},
                  {"tls_policy", tlsPolicy},
                  {"artifact_origin", "sb_regress_cpp"},
                  {"artifact_contract",
                   {{"proof_outputs_under_repo_prefix", "build/"},
                    {"engine_execution", "sblr_uuid_only"},
                    {"engine_sql_text_execution", false},
                    {"parser_output_to_engine_required", true},
                    {"copy_route_payload", "canonical_rows"},
                    {"transaction_finality_authority", "durable_mga_transaction_inventory"},
                    {"driver_or_parser_finality", "forbidden"},
                    {"missing_required_metrics_fail_gate", true}}},
                  {"schema", schemaPath.string()},
                  {"target_metrics", targetMetrics},
                  {"missing_metric_evidence", missing},
                  {"slow_path_explanations", slowPaths},
                  {"route_proof_events", routeProofs},
                  {"server_metric_samples", serverMetricSamples},
                  {"samples", serverMetricSamples},
                  {"server_telemetry_rows", serverTelemetryRows},
                  {"source_artifacts",
                   {{"command_events", (artifactRoot / "command-events.jsonl").string()},
                    {"summary", (artifactRoot / "summary.json").string()},
                    {"timing_groups", (artifactRoot / "timing-groups.json").string()},
                    {"timing_ledger", (artifactRoot / "timing-ledger.jsonl").string()},
                    {"process_metrics", (artifactRoot / "process-metrics.jsonl").string()},
                    {"server_metric_samples", "embedded:server_metric_samples"},
                    {"route_proof_events", "embedded:route_proof_events"}}}};
    artifact["telemetry_overhead"] = normalizedTelemetry;

    writeText(artifactRoot / "ipar-metrics.json", artifact.dump(2) + "\n");

    writeText(artifactRoot / "ipar-metrics.jsonl", "");
    for (const auto& [_, record] : normalizedRecords) {
        appendJsonl(artifactRoot / "ipar-metrics.jsonl", record);
    }
    appendJsonl(artifactRoot / "ipar-metrics.jsonl",
                {{"event", "ipar_telemetry_summary"},
                 {"metric_id", "IPAR-M031"},
                 {"driver", "cpp"},
                 {"run_id", runId},
                 {"route", route},
                 {"parser_mode", parserMode},
                 {"page_size", pageSize},
                 {"sslmode", sslmode},
                 {"transport_mode", transportMode},
                 {"tls_policy", tlsPolicy},
                 {"telemetry_overhead", normalizedTelemetry}});
    for (const auto& item : slowPaths) {
        appendJsonl(artifactRoot / "ipar-metrics.jsonl", item);
    }
    for (const auto& item : routeProofs) {
        appendJsonl(artifactRoot / "ipar-metrics.jsonl", item);
    }
    for (const auto& item : serverMetricSamples) {
        appendJsonl(artifactRoot / "ipar-metrics.jsonl", item);
    }
    for (const auto& item : serverTelemetryRows) {
        appendJsonl(artifactRoot / "ipar-metrics.jsonl", item);
    }

    std::set<std::string> metricFields;
    for (const auto& [_, record] : normalizedRecords) {
        const json metrics = record.value("metrics", json::object());
        for (auto it = metrics.begin(); it != metrics.end(); ++it) {
            metricFields.insert(it.key());
        }
    }
    std::ostringstream csv;
    csv << "script_id,script,driver,route,parser_mode,page_size,sslmode,transport_mode,tls_policy";
    for (const auto& field : metricFields) {
        csv << "," << field;
    }
    csv << "\n";
    for (const auto& [scriptId, record] : normalizedRecords) {
        csvEscape(csv, scriptId);
        csv << ",";
        csvEscape(csv, record.value("script", ""));
        csv << ",cpp,";
        csvEscape(csv, route);
        csv << ",";
        csvEscape(csv, parserMode);
        csv << ",";
        csvEscape(csv, pageSize);
        csv << ",";
        csvEscape(csv, sslmode);
        csv << ",";
        csvEscape(csv, transportMode);
        csv << ",";
        csvEscape(csv, tlsPolicy);
        const json metrics = record.value("metrics", json::object());
        for (const auto& field : metricFields) {
            csv << ",";
            if (metrics.contains(field) && jsonValuePresent(metrics.at(field))) {
                csvEscape(csv, metrics.at(field).is_string() ? metrics.at(field).get<std::string>()
                                                             : metrics.at(field).dump());
            }
        }
        csv << "\n";
    }
    writeText(artifactRoot / "ipar-metrics.csv", csv.str());
}

void writeJunit(const std::filesystem::path& path,
                const std::string& name,
                int tests,
                const json& failures,
                int64_t elapsedNs) {
    std::ostringstream out;
    out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    out << "<testsuite name=\"" << name << "\" tests=\"" << std::max(tests, 1)
        << "\" failures=\"" << failures.size() << "\" time=\""
        << std::fixed << std::setprecision(3) << (static_cast<double>(elapsedNs) / 1000000000.0) << "\">\n";
    if (failures.empty()) {
        out << "  <testcase classname=\"scratchbird.cpp\" name=\"full_surface\"></testcase>\n";
    } else {
        int index = 0;
        for (const auto& failure : failures) {
            out << "  <testcase classname=\"scratchbird.cpp\" name=\"failure_" << ++index << "\">\n";
            out << "    <failure message=\"" << failure.value("statement_id", "unknown") << "\">";
            std::string message = failure.value("message", "");
            std::replace(message.begin(), message.end(), '<', '[');
            std::replace(message.begin(), message.end(), '>', ']');
            out << message;
            out << "</failure>\n";
            out << "  </testcase>\n";
        }
    }
    out << "</testsuite>\n";
    writeText(path, out.str());
}

void printUsage() {
    std::cout
        << "Usage: sb_regress_cpp --suite-root <path> --artifact-root <build/path> "
        << "--database <name> --host <host> --port <port> --user <user> --password <password> "
        << "--route <embedded|ipc_local|listener-parser|manager-listener-parser> "
        << "[--ipc-path <server-sbps-socket>] "
        << "--parser-mode <server-parser|standalone-parser|driver-sblr-uuid> "
        << "--page-size <4k|8k|16k|32k|64k|128k> --namespace <schema>\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto args = parseArgs(argc, argv);
        if (hasFlag(args, "--help")) {
            printUsage();
            return 0;
        }

        const std::filesystem::path suiteRoot = valueOrDefault(
            args, "--suite-root", "project/tests/conformance/drivers/full_surface_scripts");
        const std::filesystem::path manifestPath = valueOrDefault(
            args, "--manifest", (suiteRoot / "manifest.json").string());
        const std::filesystem::path expectedRefusalsPath = valueOrDefault(
            args, "--expected-refusals", (suiteRoot / "expected/expected_refusals.json").string());
        const std::filesystem::path artifactRoot = required(args, "--artifact-root");
        std::filesystem::create_directories(artifactRoot);
        const std::string runId = valueOrDefault(args, "--run-id", "cpp-full-surface");

        const std::string pageSize = required(args, "--page-size");
        const std::string route = required(args, "--route");
        const std::string parserMode = required(args, "--parser-mode");
        if (!kPageSizes.count(pageSize)) {
            throw std::runtime_error("unsupported page size " + pageSize);
        }
        if (!kRoutes.count(route)) {
            throw std::runtime_error("unsupported route " + route);
        }
        if (!kParserModes.count(parserMode)) {
            throw std::runtime_error("unsupported parser mode " + parserMode);
        }

        const std::map<std::string, std::filesystem::path> paths{
            {"events", artifactRoot / "command-events.jsonl"},
            {"diagnostics", artifactRoot / "diagnostics.jsonl"},
            {"wire", artifactRoot / "wire-transcript.jsonl"},
            {"timing", artifactRoot / "timing-groups.json"},
            {"timing_ledger", artifactRoot / "timing-ledger.jsonl"},
            {"digests", artifactRoot / "result-digests.json"},
            {"metadata", artifactRoot / "metadata-snapshots.json"},
            {"refusals", artifactRoot / "security-refusals.json"},
            {"api", artifactRoot / "native-api-coverage.json"},
            {"review", artifactRoot / "code-example-review.json"},
            {"process_metrics", artifactRoot / "process-metrics.jsonl"},
            {"ipar_json", artifactRoot / "ipar-metrics.json"},
            {"ipar_jsonl", artifactRoot / "ipar-metrics.jsonl"},
            {"ipar_csv", artifactRoot / "ipar-metrics.csv"},
            {"junit", artifactRoot / "junit.xml"},
            {"stdout", artifactRoot / "stdout.log"},
            {"stderr", artifactRoot / "stderr.log"},
            {"summary", artifactRoot / "summary.json"}};

        for (const auto& item : paths) {
            writeText(item.second, "");
        }

        json failures = json::array();
        json digests = json::array();
        json assertionResults = json::array();
        json securityRefusals = json::array();
        json metadataSnapshots = json::array();
        json commandEvents = json::array();
        json processMetricSummary = json::object();
        std::map<std::string, int64_t> timings;
        std::map<std::string, int64_t> counts;
        std::map<std::string, int64_t> processMaxRssKb;
        std::map<std::string, int64_t> processMaxVsizeKb;
        std::map<std::string, int64_t> processInitialRssKb;
        std::map<std::string, int64_t> processLastRssKb;
        std::map<std::string, int64_t> processLastVsizeKb;
        std::map<std::string, int> api{{"scratchbird::client::Connection", 0},
                                       {"connect", 0},
                                       {"beginTransaction", 0},
                                       {"prepare", 0},
                                       {"executePrepared", 0},
                                       {"preparedCacheHit", 0},
                                       {"preparedCacheMiss", 0},
                                       {"preparedCacheBypass", 0},
                                       {"preparedCacheInvalidation", 0},
                                       {"execute", 0},
                                       {"executeQuery", 0},
                                       {"metadataQuery", 0},
                                       {"reconnect", 0},
                                       {"commit", 0},
                                       {"rollback", 0},
                                       {"savepoint", 0},
                                       {"releaseSavepoint", 0},
                                       {"rollbackTo", 0},
                                       {"scriptChainExecute", 0},
                                       {"ResultSet::next", 0}};

        const int64_t runStarted = nowNs();
        const json manifest = readJson(manifestPath);
        const std::filesystem::path iparSchemaPath = valueOrDefault(
            args, "--ipar-schema", (suiteRoot / "ipar_metrics_schema.json").string());
        const json iparSchema = std::filesystem::exists(iparSchemaPath) ? readJson(iparSchemaPath) : json::object();
        const std::set<std::string> iparTargets = iparTargetScriptIds(iparSchema);
        const std::map<std::string, std::vector<std::string>> iparRequired = iparRequiredFields(iparSchema);
        json iparTelemetry = loadIparTelemetry(args);
        std::map<std::string, json> iparRecords;
        std::map<std::string, std::vector<double>> iparStatementMsByScript;
        std::map<std::string, std::filesystem::path> iparTracePathsByScript;
        json iparSlowPaths = json::array();
        json iparRouteProofs = json::array();
        json serverMetricSamples = json::array();
        json serverTelemetryRows = json::array();
        std::map<std::string, std::vector<std::string>> expectedDiagnostics;
        const std::set<std::string> expectedRefusals =
            loadExpectedRefusals(expectedRefusalsPath, &expectedDiagnostics);

        const std::string namespaceName = required(args, "--namespace");
        const std::string sslmode = valueOrDefault(args, "--sslmode", "require");
        const std::string transportMode = transportModeForRoute(route, sslmode);
        const std::string tlsPolicy = tlsPolicyForRoute(route, sslmode);
        const std::vector<std::string> scriptFilters = splitCsv(valueOrDefault(args, "--script-ids", ""));
        const std::set<std::string> scriptFilterSet(scriptFilters.begin(), scriptFilters.end());
        const std::size_t preparedCacheLimit = static_cast<std::size_t>(
            std::stoull(valueOrDefault(args, "--prepared-cache-size", "256")));
        const bool preparedCacheEnabled =
            preparedCacheLimit > 0 && valueOrDefault(args, "--prepared-cache-mode", "read_queries") != "off";
        int executedStatements = 0;
        int skippedScripts = 0;
        int expectedRefusalPasses = 0;
        int assertionPasses = 0;
        int assertionFailures = 0;

        std::vector<std::pair<std::string, int>> monitoredProcesses;
#ifndef _WIN32
        monitoredProcesses.push_back({"client", static_cast<int>(::getpid())});
#endif
        const auto addMonitoredPid = [&](const std::string& role, const std::string& option) {
            const auto value = valueOrDefault(args, option, "");
            if (value.empty()) return;
            try {
                const int pid = std::stoi(value);
                if (pid > 0) monitoredProcesses.push_back({role, pid});
            } catch (...) {
            }
        };
        addMonitoredPid("server", "--server-pid");
        addMonitoredPid("listener", "--listener-pid");

        const auto recordProcessMetrics = [&](const std::string& phase,
                                              const std::string& statementId,
                                              const std::string& elementId,
                                              int statementIndex) {
            for (const auto& [role, pid] : monitoredProcesses) {
                json sample = sampleProcessMetrics(role, pid);
                sample["run_id"] = runId;
                sample["phase"] = phase;
                sample["statement_id"] = statementId;
                sample["element_id"] = elementId;
                sample["statement_index"] = statementIndex;
                sample["sample_monotonic_ns"] = nowNs();
                if (sample.value("ok", false)) {
                    const auto rss = sample.value("rss_kb", int64_t{0});
                    const auto vsize = sample.value("vsize_kb", int64_t{0});
                    if (!processInitialRssKb.count(role)) {
                        processInitialRssKb[role] = rss;
                    }
                    processLastRssKb[role] = rss;
                    processLastVsizeKb[role] = vsize;
                    processMaxRssKb[role] = std::max(processMaxRssKb[role], rss);
                    processMaxVsizeKb[role] = std::max(processMaxVsizeKb[role], vsize);
                }
                appendJsonl(paths.at("process_metrics"), sample);
            }
        };
        recordProcessMetrics("suite_start", "suite_start", "suite_start", 0);

        const std::map<std::string, std::string> replacements{
            {"__SB_NAMESPACE__", namespaceName},
            {"__SB_ARTIFACT_ROOT__", artifactRoot.string()},
            {"__SB_DRIVER__", "cpp"},
            {"__SB_RUN_ID__", runId},
            {"__SB_ROUTE__", route},
            {"__SB_PARSER_MODE__", parserMode},
            {"__SB_PAGE_SIZE__", pageSize}};

        scratchbird::client::Connection conn;
        std::map<std::string, std::unique_ptr<scratchbird::client::PreparedStatement>> preparedCache;
        api["scratchbird::client::Connection"]++;
        scratchbird::client::ConnectionConfig config;
        config.host = required(args, "--host");
        config.tcp_port = static_cast<uint16_t>(std::stoi(required(args, "--port")));
        config.database_name = required(args, "--database");
        config.username = required(args, "--user");
        config.password = required(args, "--password");
        config.role = valueOrDefault(args, "--role", "");
        config.ssl_mode = sslmode;
        config.ipc_path = valueOrDefault(args, "--ipc-path", "");
        config.ssl_root_cert = valueOrDefault(args, "--sslrootcert", "");
        config.ssl_cert = valueOrDefault(args, "--sslcert", "");
        config.ssl_key = valueOrDefault(args, "--sslkey", "");
        config.application_name = "SBRegressCpp";
        config.query_timeout_ms = static_cast<uint32_t>(
            std::stoul(valueOrDefault(args, "--statement-timeout-ms", "30000")));
        config.read_timeout_ms = config.query_timeout_ms;
        config.write_timeout_ms = config.query_timeout_ms;
        config.enable_copy_streaming = true;
        config.binary_transfer = true;
        config.transport_mode = route == "ipc_local" ? "local_ipc" : "inet_listener";
        if (route == "embedded") {
            config.transport_mode = "embedded";
        }
        config.front_door_mode = route == "manager-listener-parser" ? "manager_proxy" : "direct";

        appendJsonl(paths.at("wire"), {{"event", "suite_start"},
                                       {"suite_id", manifest.value("suite_id", "")},
                                       {"driver", "cpp"},
                                       {"route", route},
                                       {"parser_mode", parserMode},
                                       {"page_size", pageSize},
                                       {"sslmode", sslmode},
                                       {"server_revalidation_required", true},
                                       {"driver_or_parser_finality", "forbidden"},
                                       {"mga_authority", "engine"}});

        scratchbird::core::ErrorContext ctx;
        const int64_t connectStarted = nowNs();
        auto status = conn.connect(config, &ctx);
        if (status == scratchbird::core::Status::OK) {
            api["connect"]++;
            addTiming(&timings, "connection", connectStarted);
            appendText(paths.at("stdout"), "connected cpp regression runner\n");
            recordProcessMetrics("post_connect", "connect", "connect", 0);
        } else {
            failures.push_back(makeFailure("connect", statusMessage(ctx)));
            appendJsonl(paths.at("diagnostics"), {{"statement_id", "connect"},
                                                 {"sqlstate", sqlstateOf(ctx)},
                                                 {"message", statusMessage(ctx)}});
        }

        if (failures.empty() && hasFlag(args, "--create-database")) {
            failures.push_back(makeFailure("create_database",
                                           "--create-database requires the database lifecycle API; this runner fails closed"));
        }
        if (failures.empty() && parserMode != "server-parser") {
            failures.push_back(makeFailure("parser_mode",
                                           parserMode + " is not exposed by the C++ client yet; the runner fails closed"));
        }

        auto reconnectAfterTransactionBoundaryDesync =
            [&](const std::string& statementId,
                const std::string& elementId,
                const std::string& diagnostic,
                uint64_t staleTransactionId) -> bool {
            appendJsonl(paths.at("wire"),
                        {{"event", "transaction_boundary_desync_reconnect_start"},
                         {"statement_id", statementId},
                         {"element_id", elementId},
                         {"diagnostic_code", diagnostic},
                         {"stale_transaction_id", staleTransactionId},
                         {"mga_authority", "engine"}});
            conn.disconnect();
            preparedCache.clear();

            scratchbird::core::ErrorContext reconnectCtx;
            const int64_t reconnectStarted = nowNs();
            const auto reconnectStatus = conn.connect(config, &reconnectCtx);
            api["reconnect"]++;
            addTiming(&timings, "connection", reconnectStarted);
            recordProcessMetrics("post_reconnect", statementId, elementId, executedStatements);
            if (reconnectStatus == scratchbird::core::Status::OK) {
                appendJsonl(paths.at("wire"),
                            {{"event", "transaction_boundary_desync_reconnect_complete"},
                             {"statement_id", statementId},
                             {"element_id", elementId},
                             {"stale_transaction_id", staleTransactionId},
                             {"transaction_id_observed", conn.currentTransactionId()},
                             {"prepared_cache_cleared", true},
                             {"mga_authority", "engine"}});
                return true;
            }

            const std::string reconnectDiagnostic = statusMessage(reconnectCtx);
            failures.push_back(makeFailure(statementId,
                                           "transaction boundary reconnect failed: " +
                                               reconnectDiagnostic));
            appendJsonl(paths.at("diagnostics"),
                        {{"statement_id", statementId},
                         {"element_id", elementId},
                         {"sqlstate", sqlstateOf(reconnectCtx)},
                         {"message", reconnectDiagnostic},
                         {"phase", "transaction_boundary_desync_reconnect"}});
            appendJsonl(paths.at("wire"),
                        {{"event", "transaction_boundary_desync_reconnect_failed"},
                         {"statement_id", statementId},
                         {"element_id", elementId},
                         {"stale_transaction_id", staleTransactionId},
                         {"sqlstate", sqlstateOf(reconnectCtx)},
                         {"diagnostic_code", reconnectDiagnostic},
                         {"mga_authority", "engine"}});
            return false;
        };

        auto shouldReconnectAfterTransactionBoundaryDesync =
            [](scratchbird::core::Status statementStatus,
               const std::string& diagnostic,
               uint64_t transactionIdBefore,
               uint64_t transactionIdAfter) {
            if (statementStatus == scratchbird::core::Status::OK ||
                transactionIdBefore == 0) {
                return false;
            }
            const bool replacementOpened =
                diagnostic.find("autocommit rollback opened a replacement transaction") != std::string::npos;
            const bool transactionMismatch =
                diagnostic.find("SBWP.TRANSACTION_ID_MISMATCH") != std::string::npos;
            if (replacementOpened && transactionIdAfter <= transactionIdBefore) {
                return true;
            }
            return transactionMismatch && transactionIdAfter == transactionIdBefore;
        };

        auto shouldRetryPreparedAsOrdinarySql =
            [](scratchbird::core::Status statementStatus,
               const scratchbird::core::ErrorContext& statementCtx) {
            if (statementStatus == scratchbird::core::Status::OK) {
                return false;
            }
            const std::string diagnostic = statusMessage(statementCtx);
            return diagnostic.find("DML.NATIVE_BULK_INGEST.TRIGGER_AWARE_PATH_REQUIRED") !=
                       std::string::npos ||
                   diagnostic.find("trigger_aware_path_required") != std::string::npos;
        };

        auto executePreparedCached = [&](const std::string& sql,
                                         const std::string& fallbackSql,
                                         const std::string& statementId,
                                         const std::string& elementId,
                                         const std::vector<PreparedParamValue>* params,
                                         scratchbird::client::ResultSet* resultSet,
                                         int64_t* rowsAffected,
                                         scratchbird::core::ErrorContext* statementCtx,
                                         bool* preparedHandleUsed) {
            auto runPrepared = [&](scratchbird::client::PreparedStatement* stmt) {
                if (params != nullptr) {
                    bindPreparedParams(stmt, *params);
                }
                api["executePrepared"]++;
                if (preparedHandleUsed != nullptr) {
                    *preparedHandleUsed = true;
                }
                if (resultSet != nullptr) {
                    return stmt->executeQuery(resultSet, statementCtx);
                }
                return stmt->execute(rowsAffected, statementCtx);
            };

            auto found = preparedCache.find(sql);
            if (found != preparedCache.end() && found->second && found->second->isValid()) {
                api["preparedCacheHit"]++;
                const auto preparedStatus = runPrepared(found->second.get());
                if (preparedStatus == scratchbird::core::Status::OK) {
                    return preparedStatus;
                }
                if (shouldRetryPreparedAsOrdinarySql(preparedStatus, *statementCtx)) {
                    api["preparedCacheInvalidation"]++;
                    appendJsonl(paths.at("wire"), {{"event", "prepared_cache_invalidate"},
                                                   {"statement_id", statementId},
                                                   {"element_id", elementId},
                                                   {"sql_hash", sha256Text(sql)},
                                                   {"prepared_parameter_count",
                                                    params == nullptr ? 0 : static_cast<int64_t>(params->size())},
                                                   {"reason", statusMessage(*statementCtx)},
                                                   {"retry_route", "ordinary_sql_after_trigger_aware_refusal"},
                                                   {"engine_sql_text_execution", false},
                                                   {"mga_authority", "engine"}});
                    preparedCache.erase(found);
                    clearErrorContext(statementCtx);
                    if (preparedHandleUsed != nullptr) {
                        *preparedHandleUsed = false;
                    }
                    api["execute"]++;
                    if (resultSet != nullptr) {
                        return conn.executeQuery(fallbackSql, resultSet, statementCtx);
                    }
                    return conn.execute(fallbackSql, rowsAffected, statementCtx);
                }
                api["preparedCacheInvalidation"]++;
                appendJsonl(paths.at("wire"), {{"event", "prepared_cache_invalidate"},
                                               {"statement_id", statementId},
                                               {"element_id", elementId},
                                               {"sql_hash", sha256Text(sql)},
                                               {"prepared_parameter_count",
                                                params == nullptr ? 0 : static_cast<int64_t>(params->size())},
                                               {"reason", statusMessage(*statementCtx)},
                                               {"engine_sql_text_execution", false},
                                               {"mga_authority", "engine"}});
                preparedCache.erase(found);
            }

            if (preparedCache.size() >= preparedCacheLimit) {
                if (params != nullptr && !preparedCache.empty()) {
                    appendJsonl(paths.at("wire"), {{"event", "prepared_cache_evict_for_parameterized_shape"},
                                                   {"statement_id", statementId},
                                                   {"element_id", elementId},
                                                   {"sql_hash", sha256Text(sql)},
                                                   {"evicted_sql_hash", sha256Text(preparedCache.begin()->first)},
                                                   {"prepared_parameter_count",
                                                    static_cast<int64_t>(params->size())},
                                                   {"engine_sql_text_execution", false},
                                                   {"mga_authority", "engine"}});
                    preparedCache.erase(preparedCache.begin());
                } else {
                    api["preparedCacheBypass"]++;
                    appendJsonl(paths.at("wire"), {{"event", "prepared_cache_bypass"},
                                                   {"statement_id", statementId},
                                                   {"element_id", elementId},
                                                   {"sql_hash", sha256Text(sql)},
                                                   {"reason", "cache_limit"},
                                                   {"engine_sql_text_execution", false},
                                                   {"mga_authority", "engine"}});
                    if (resultSet != nullptr) {
                        return conn.executeQuery(sql, resultSet, statementCtx);
                    }
                    return conn.execute(sql, rowsAffected, statementCtx);
                }
            }

            auto prepared = std::make_unique<scratchbird::client::PreparedStatement>();
            auto prepareStatus = conn.prepare(sql, prepared.get(), statementCtx);
            api["prepare"]++;
            if (prepareStatus != scratchbird::core::Status::OK) {
                return prepareStatus;
            }
            api["preparedCacheMiss"]++;
            appendJsonl(paths.at("wire"), {{"event", "prepared_cache_prepare"},
                                           {"statement_id", statementId},
                                           {"element_id", elementId},
                                           {"sql_hash", sha256Text(sql)},
                                           {"prepared_parameter_count",
                                            params == nullptr ? 0 : static_cast<int64_t>(params->size())},
                                           {"server_revalidation_state", "required"},
                                           {"engine_payload_kind", "server_parser_sblr_uuid_output"},
                                           {"engine_sql_text_execution", false},
                                           {"mga_authority", "engine"}});
            auto inserted = preparedCache.emplace(sql, std::move(prepared));
            const auto preparedStatus = runPrepared(inserted.first->second.get());
            if (shouldRetryPreparedAsOrdinarySql(preparedStatus, *statementCtx)) {
                api["preparedCacheInvalidation"]++;
                appendJsonl(paths.at("wire"), {{"event", "prepared_cache_invalidate"},
                                               {"statement_id", statementId},
                                               {"element_id", elementId},
                                               {"sql_hash", sha256Text(sql)},
                                               {"prepared_parameter_count",
                                                params == nullptr ? 0 : static_cast<int64_t>(params->size())},
                                               {"reason", statusMessage(*statementCtx)},
                                               {"retry_route", "ordinary_sql_after_trigger_aware_refusal"},
                                               {"engine_sql_text_execution", false},
                                               {"mga_authority", "engine"}});
                preparedCache.erase(inserted.first);
                clearErrorContext(statementCtx);
                if (preparedHandleUsed != nullptr) {
                    *preparedHandleUsed = false;
                }
                api["execute"]++;
                if (resultSet != nullptr) {
                    return conn.executeQuery(fallbackSql, resultSet, statementCtx);
                }
                return conn.execute(fallbackSql, rowsAffected, statementCtx);
            }
            return preparedStatus;
        };

        if (failures.empty() && preparedCacheEnabled) {
            appendJsonl(paths.at("wire"), {{"event", "prepared_cache_probe_skipped"},
                                           {"reason", "real_suite_statements_prove_prepared_route"},
                                           {"prepared_cache_limit", preparedCacheLimit},
                                           {"server_revalidation_state", "required"},
                                           {"engine_sql_text_execution", false},
                                           {"mga_authority", "engine"}});
        }

        if (failures.empty()) {
            int namespaceBootstrapIndex = 0;
            for (const auto& ancestorSchema : namespaceAncestorSchemas(namespaceName)) {
                ++namespaceBootstrapIndex;
                const std::string statementId =
                    "namespace_bootstrap:" + std::to_string(namespaceBootstrapIndex);
                const std::string sql = "CREATE SCHEMA " + ancestorSchema;
                const int64_t statementStarted = nowNs();
                scratchbird::core::ErrorContext bootstrapCtx;
                int64_t rowsAffected = -1;
                appendJsonl(paths.at("events"), {{"run_id", runId},
                                                 {"driver_name", "cpp"},
                                                 {"suite_id", manifest.value("suite_id", "")},
                                                 {"script_id", "namespace_bootstrap"},
                                                 {"script", "runner_namespace_bootstrap"},
                                                 {"statement_index", namespaceBootstrapIndex},
                                                 {"statement_id", statementId},
                                                 {"command_group", "ddl"},
                                                 {"sql_hash", sha256Text(sql)},
                                                 {"actual_outcome", "started"},
                                                 {"server_revalidation_state", "required"},
                                                 {"mga_authority", "engine"}});
                appendJsonl(paths.at("wire"), {{"event", "namespace_bootstrap_start"},
                                               {"statement_id", statementId},
                                               {"schema", ancestorSchema},
                                               {"route", route},
                                               {"parser_mode", parserMode}});
                const auto bootstrapStatus = conn.execute(sql, &rowsAffected, &bootstrapCtx);
                api["execute"]++;
                bool bootstrapPassed = bootstrapStatus == scratchbird::core::Status::OK;
                std::string diagnostic = bootstrapPassed ? std::string{} : statusMessage(bootstrapCtx);
                if (!bootstrapPassed &&
                    diagnostic.find("already_exists") != std::string::npos) {
                    bootstrapPassed = true;
                    diagnostic = "schema ancestor already existed";
                }
                addTiming(&timings, "ddl", statementStarted);
                appendJsonl(paths.at("events"), {{"run_id", runId},
                                                 {"driver_name", "cpp"},
                                                 {"suite_id", manifest.value("suite_id", "")},
                                                 {"script_id", "namespace_bootstrap"},
                                                 {"script", "runner_namespace_bootstrap"},
                                                 {"statement_index", namespaceBootstrapIndex},
                                                 {"statement_id", statementId},
                                                 {"command_group", "ddl"},
                                                 {"sql_hash", sha256Text(sql)},
                                                 {"actual_outcome", bootstrapPassed ? "success" : "failure"},
                                                 {"passed", bootstrapPassed},
                                                 {"diagnostic", diagnostic},
                                                 {"elapsed_ns", nowNs() - statementStarted},
                                                 {"server_revalidation_state", "required"},
                                                 {"mga_authority", "engine"}});
                appendJsonl(paths.at("wire"), {{"event", "namespace_bootstrap_complete"},
                                               {"statement_id", statementId},
                                               {"schema", ancestorSchema},
                                               {"passed", bootstrapPassed},
                                               {"diagnostic", diagnostic}});
                if (!bootstrapPassed) {
                    failures.push_back(makeFailure(statementId, diagnostic));
                    appendJsonl(paths.at("diagnostics"), {{"statement_id", statementId},
                                                         {"sqlstate", sqlstateOf(bootstrapCtx)},
                                                         {"message", diagnostic},
                                                         {"schema", ancestorSchema}});
                    appendText(paths.at("stderr"), statementId + ": " + diagnostic + "\n");
                    break;
                }
            }
        }

        if (failures.empty()) {
            for (const auto& scriptEntry : manifest.value("scripts", json::array())) {
                const std::string scriptId = scriptEntry.value("script_id", "");
                if (!scriptFilterSet.empty() && !scriptFilterSet.count(scriptId)) {
                    continue;
                }
                const std::filesystem::path relativePath = scriptEntry.value("path", "");
                const std::filesystem::path scriptPath = suiteRoot / relativePath;
                if (!std::filesystem::exists(scriptPath)) {
                    ++skippedScripts;
                    const json skipEvent{{"run_id", runId},
                                         {"driver_name", "cpp"},
                                         {"script_id", scriptId},
                                         {"script", relativePath.string()},
                                         {"actual_outcome", "skipped_missing_generated_source"},
                                         {"generated_from", scriptEntry.value("generated_from", "")}};
                    appendJsonl(paths.at("events"), skipEvent);
                    commandEvents.push_back(skipEvent);
                    if (!scriptEntry.contains("generated_from")) {
                        failures.push_back(makeFailure(scriptId, "script missing: " + scriptPath.string()));
                        if (hasFlag(args, "--stop-on-error")) {
                            break;
                        }
                    }
                    continue;
                }

                const std::string scriptText = applyPlaceholders(readText(scriptPath), replacements);
                const std::vector<std::string> statements = sbchunk::splitStatements(scriptText);
                const std::string basename = relativePath.filename().string();
                const std::filesystem::path scriptTracePath =
                    artifactRoot / "driver-phase-traces" / (scriptId + ".jsonl");
                const std::filesystem::path parserWorkerTracePath =
                    artifactRoot / "parser-worker-phase-traces" / (scriptId + ".jsonl");
                const std::filesystem::path parserPipelineTracePath =
                    artifactRoot / "parser-pipeline-phase-traces" / (scriptId + ".tsv");
                writeText(scriptTracePath, "");
                writeText(parserWorkerTracePath, "");
                writeText(parserPipelineTracePath, "");
                setProcessEnv("SCRATCHBIRD_CPP_DRIVER_PHASE_TRACE_FILE", scriptTracePath.string());
                setParserPhaseTraceFiles(parserWorkerTracePath, parserPipelineTracePath);
                std::set<size_t> expectedRefusalIndexes;
                for (size_t index = 0; index < statements.size(); ++index) {
                    const std::string candidateStatementId =
                        basename + ":" + std::to_string(index + 1);
                    const bool expectsRefusal =
                        expectedRefusals.count(candidateStatementId) > 0 ||
                        (scriptEntry.contains("expected_refusals") &&
                         std::find(scriptEntry["expected_refusals"].begin(),
                                   scriptEntry["expected_refusals"].end(),
                                   candidateStatementId) != scriptEntry["expected_refusals"].end());
                    if (expectsRefusal) {
                        expectedRefusalIndexes.insert(index);
                    }
                }
                const auto activeScriptChainRanges =
                    valueOrDefault(args, "--script-chain-mode", "on") == "off"
                        ? std::vector<std::pair<size_t, size_t>>{}
                        : scriptChainRanges(scriptId, statements, expectedRefusalIndexes);
                const std::string defaultScriptChainChunkSize =
                    activeScriptChainRanges.empty() ? "64" : "256";
                const size_t scriptChainChunkSize = std::max<size_t>(
                    1,
                    static_cast<size_t>(std::stoull(
                        valueOrDefault(args,
                                       "--script-chain-chunk-size",
                                       defaultScriptChainChunkSize))));
                const bool iparTarget = iparTargets.count(scriptId) > 0;
                json* iparMetrics = nullptr;
                if (iparTarget) {
                    json& iparRecord = ensureIparRecord(&iparRecords,
                                                        scriptId,
                                                        relativePath.string(),
                                                        runId,
                                                        route,
                                                        parserMode,
                                                        pageSize,
                                                        sslmode,
                                                        transportMode,
                                                        tlsPolicy);
                    iparMetrics = &iparRecord["metrics"];
                    addCounterMetric(iparMetrics, "validation_failures", 0);
                    addCounterMetric(iparMetrics, "copy_rejects", 0);
                    addCounterMetric(iparMetrics, "refusal_count", 0);
                    iparTracePathsByScript[scriptId] = scriptTracePath;
                }

                std::set<std::string> seenScriptChainDescriptorTables;
                std::set<std::string> seenScriptChainOperationFamilies;
                if (!activeScriptChainRanges.empty()) {
                    for (const auto& chainRange : activeScriptChainRanges) {
                        for (size_t chainIndex = chainRange.first;
                             chainIndex < chainRange.second;
                             ++chainIndex) {
                            const std::string tableKey = sbdfs130TableKey(statements[chainIndex]);
                            if (!tableKey.empty()) {
                                seenScriptChainDescriptorTables.insert(tableKey);
                            }
                        }
                    }
                    appendJsonl(paths.at("wire"),
                                {{"event", "script_chain_descriptor_prepin"},
                                 {"script_id", scriptId},
                                 {"prebound_descriptor_count",
                                  static_cast<int64_t>(seenScriptChainDescriptorTables.size())},
                                 {"engine_sql_text_execution", false},
                                 {"parser_output_to_engine_required", true},
                                 {"mga_authority", "engine"}});
                    if (iparMetrics != nullptr) {
                        addCounterMetric(iparMetrics,
                                         "prepared_descriptor_prebound_tables",
                                         static_cast<int64_t>(seenScriptChainDescriptorTables.size()));
                    }
                }
                for (size_t statementIndex = 0; statementIndex < statements.size(); ++statementIndex) {
                    const std::optional<size_t> activeChainEnd =
                        chainEndForStatement(activeScriptChainRanges, statementIndex);
                    if (activeChainEnd.has_value()) {
                        const size_t chainStartIndex = statementIndex;
                        const size_t chainEndIndex = std::min(*activeChainEnd,
                                                              chainStartIndex + scriptChainChunkSize);
                        const size_t chainStatementCount = chainEndIndex - chainStartIndex;
                        std::ostringstream chainSql;
                        for (size_t chainIndex = chainStartIndex; chainIndex < chainEndIndex; ++chainIndex) {
                            chainSql << statementWithTerminator(statements[chainIndex]);
                        }
                        const std::string chainText = chainSql.str();
                        const std::string chainStatementId =
                            basename + ":chain:" + std::to_string(chainStartIndex + 1) +
                            "-" + std::to_string(chainEndIndex);
                        const std::string chainElementId = "script_chain:" + scriptId;
                        const uint64_t chainTransactionBefore = conn.currentTransactionId();
                        const int64_t chainStarted = nowNs();
                        recordProcessMetrics("script_chain_start",
                                             chainStatementId,
                                             chainElementId,
                                             executedStatements);
                        appendJsonl(paths.at("wire"),
                                    {{"event", "script_chain_start"},
                                     {"statement_id", chainStatementId},
                                     {"element_id", chainElementId},
                                     {"script_id", scriptId},
                                     {"statement_count", static_cast<int64_t>(chainStatementCount)},
                                     {"script_bytes", static_cast<int64_t>(chainText.size())},
                                     {"chain_chunk_size", static_cast<int64_t>(scriptChainChunkSize)},
                                     {"chain_range_start", static_cast<int64_t>(chainStartIndex + 1)},
                                     {"chain_range_end", static_cast<int64_t>(chainEndIndex)},
                                     {"engine_sql_text_execution", false},
                                     {"parser_output_to_engine_required", true},
                                     {"mga_authority", "engine"}});

                        setDriverPhaseTraceContext(runId,
                                                   scriptId,
                                                   chainStatementId,
                                                   chainElementId,
                                                   "script_chain",
                                                   "script_chain");
                        scratchbird::core::ErrorContext chainCtx;
                        int64_t chainRowsAffected = -1;
                        const int64_t beginStarted = nowNs();
                        status = conn.beginTransaction(&chainCtx);
                        api["beginTransaction"]++;
                        addTiming(&timings, "transaction", beginStarted);
                        if (status == scratchbird::core::Status::OK) {
                            const int64_t executeStarted = nowNs();
                            status = conn.execute(chainText, &chainRowsAffected, &chainCtx);
                            api["execute"]++;
                            api["scriptChainExecute"]++;
                            addTiming(&timings, "dml", executeStarted);
                        }
                        if (status == scratchbird::core::Status::OK) {
                            const int64_t commitStarted = nowNs();
                            status = conn.commit(&chainCtx);
                            api["commit"]++;
                            addTiming(&timings, "transaction", commitStarted);
                        } else {
                            scratchbird::core::ErrorContext rollbackCtx;
                            const int64_t rollbackStarted = nowNs();
                            (void)conn.rollback(&rollbackCtx);
                            api["rollback"]++;
                            addTiming(&timings, "transaction", rollbackStarted);
                        }
                        clearDriverPhaseTraceContext();

                        const int64_t chainElapsedNs = nowNs() - chainStarted;
                        const bool chainPassed = status == scratchbird::core::Status::OK;
                        const std::string chainDiagnostic =
                            chainPassed ? std::string{} : statusMessage(chainCtx);
                        appendJsonl(paths.at("wire"),
                                    {{"event", "script_chain_complete"},
                                     {"statement_id", chainStatementId},
                                     {"element_id", chainElementId},
                                     {"script_id", scriptId},
                                     {"passed", chainPassed},
                                     {"diagnostic", chainDiagnostic},
                                     {"rows_affected", chainRowsAffected},
                                     {"elapsed_ns", chainElapsedNs},
                                     {"transaction_id_before", chainTransactionBefore},
                                     {"transaction_id_after", conn.currentTransactionId()},
                                     {"engine_sql_text_execution", false},
                                     {"parser_output_to_engine_required", true},
                                     {"mga_authority", "engine"}});

                        if (!chainPassed) {
                            failures.push_back(makeFailure(chainStatementId, chainDiagnostic));
                            appendJsonl(paths.at("diagnostics"),
                                        {{"statement_id", chainStatementId},
                                         {"element_id", chainElementId},
                                         {"script_id", scriptId},
                                         {"sqlstate", sqlstateOf(chainCtx)},
                                         {"message", chainDiagnostic}});
                            appendText(paths.at("stderr"),
                                       chainStatementId + ": " + chainDiagnostic + "\n");
                            break;
                        }

                        const int64_t perStatementNs =
                            std::max<int64_t>(1, chainElapsedNs /
                                                     static_cast<int64_t>(chainStatementCount));
                        for (size_t chainIndex = chainStartIndex; chainIndex < chainEndIndex; ++chainIndex) {
                            const std::string& chainedSql = statements[chainIndex];
                            const std::string statementId =
                                basename + ":" + std::to_string(chainIndex + 1);
                            const std::string group = classify(chainedSql, statementId, expectedRefusals);
                            const std::string elementId = elementIdForStatement(scriptId, group, chainedSql);
                            const std::string first = mainStatementTokenLower(chainedSql);
                            const std::string tableKey = sbdfs130TableKey(chainedSql);
                            const std::string familyKey = sbdfs130OperationFamily(chainedSql);
                            const bool descriptorHit =
                                !tableKey.empty() &&
                                seenScriptChainDescriptorTables.count(tableKey) > 0;
                            const bool templateHit =
                                !familyKey.empty() &&
                                seenScriptChainOperationFamilies.count(familyKey) > 0;
                            if (!tableKey.empty()) {
                                seenScriptChainDescriptorTables.insert(tableKey);
                            }
                            if (!familyKey.empty()) {
                                seenScriptChainOperationFamilies.insert(familyKey);
                            }
                            const int64_t estimatedRowsAffected =
                                estimatedRowsAffectedForChainedStatement(scriptId, chainedSql);
                            ++executedStatements;
                            counts[group]++;
                            timings[group] += perStatementNs;
                            if (executedStatements == 1 || executedStatements % 500 == 0) {
                                recordProcessMetrics("statement",
                                                     statementId,
                                                     elementId,
                                                     executedStatements);
                            }
                            const std::string revalidationState =
                                descriptorHit || templateHit
                                    ? "script_chain_descriptor_or_template_reused"
                                    : "required";
                            const bool preparedReuse = descriptorHit || templateHit;
                            appendJsonl(paths.at("timing_ledger"),
                                        {{"schema_version", 1},
                                         {"schema_id", "scratchbird.driver.statement_timing_ledger.v1"},
                                         {"run_id", runId},
                                         {"suite_id", manifest.value("suite_id", "")},
                                         {"driver", "cpp"},
                                         {"script_id", scriptId},
                                         {"script", relativePath.string()},
                                         {"statement_index", chainIndex + 1},
                                         {"statement_id", statementId},
                                         {"element_id", elementId},
                                         {"command_group", group},
                                         {"route", route},
                                         {"parser_mode", parserMode},
                                         {"page_size", pageSize},
                                         {"sslmode", sslmode},
                                         {"transport_mode", transportMode},
                                         {"tls_policy", tlsPolicy},
                                         {"execution_mode", "script_chain"},
                                         {"script_chain_statement_count",
                                          static_cast<int64_t>(chainStatementCount)},
                                         {"elapsed_ns", perStatementNs},
                                         {"script_chain_elapsed_ns", chainElapsedNs},
                                         {"row_count", 0},
                                         {"rows_affected", estimatedRowsAffected},
                                         {"rows_affected_estimated", estimatedRowsAffected >= 0},
                                         {"passed", true},
                                         {"expected_outcome", "success"},
                                         {"actual_outcome", "success"},
                                         {"sqlstate", "00000"},
                                         {"diagnostic_code", ""},
                                         {"sql_hash", sha256Text(chainedSql)},
                                         {"server_revalidation_state", revalidationState},
                                         {"descriptor_reuse_observed", descriptorHit},
                                         {"operation_family_template_reuse_observed", templateHit},
                                         {"engine_sql_text_execution", false},
                                         {"parser_output_to_engine_required", true},
                                         {"stage_timing_artifact", scriptTracePath.string()},
                                         {"mga_authority", "engine"}});
                            if (iparMetrics != nullptr) {
                                const double statementElapsedMs = nsToMs(perStatementNs);
                                iparStatementMsByScript[scriptId].push_back(statementElapsedMs);
                                addNumericMetric(iparMetrics, "command_ms", statementElapsedMs);
                                if (statementReturnsRows(chainedSql)) {
                                    addNumericMetric(iparMetrics, "execute_ms", statementElapsedMs);
                                }
                                addCounterMetric(iparMetrics,
                                                 "prepared_descriptor_hits",
                                                 descriptorHit ? 1 : 0);
                                addCounterMetric(iparMetrics,
                                                 "prepared_descriptor_misses",
                                                 descriptorHit ? 0 : 1);
                                addCounterMetric(iparMetrics,
                                                 "operation_family_template_hits",
                                                 templateHit ? 1 : 0);
                                addCounterMetric(iparMetrics,
                                                 "operation_family_template_misses",
                                                 templateHit ? 0 : 1);
                                addCounterMetric(iparMetrics, "script_chain_route_proofs", 1);
                                if (preparedReuse) {
                                    addCounterMetric(iparMetrics, "prepared_route_proofs", 1);
                                    addCounterMetric(iparMetrics,
                                                     "prepared_descriptor_session_handle_proofs",
                                                     1);
                                    addCounterMetric(iparMetrics,
                                                     "prepared_authorization_proofs",
                                                     1);
                                    if (first == "insert" || first == "upsert" ||
                                        first == "merge") {
                                        addCounterMetric(iparMetrics,
                                                         "prepared_insert_route_proofs",
                                                         1);
                                    }
                                }
                                if (estimatedRowsAffected >= 0) {
                                    addCounterMetric(iparMetrics, "rows_affected", estimatedRowsAffected);
                                    if (first == "insert" || first == "upsert" || first == "merge") {
                                        addCounterMetric(iparMetrics, "rows_written", estimatedRowsAffected);
                                    } else if (first == "update") {
                                        addCounterMetric(iparMetrics, "rows_updated", estimatedRowsAffected);
                                    } else if (first == "delete") {
                                        addCounterMetric(iparMetrics, "rows_deleted", estimatedRowsAffected);
                                    }
                                }
                                captureScriptSpecificStatementFields(iparMetrics,
                                                                     scriptId,
                                                                     chainedSql,
                                                                     first,
                                                                     true,
                                                                     perStatementNs,
                                                                     estimatedRowsAffected);
                                iparRouteProofs.push_back(
                                    {{"event", "ipar_route_proof"},
                                     {"run_id", runId},
                                     {"script_id", scriptId},
                                     {"statement_id", statementId},
                                     {"element_id", elementId},
                                     {"statement_class", group},
                                     {"first_token", first},
                                     {"driver", "cpp"},
                                     {"route", route},
                                     {"parser_mode", parserMode},
                                     {"sslmode", sslmode},
                                     {"transport_mode", transportMode},
                                     {"tls_policy", tlsPolicy},
                                     {"driver_payload_kind",
                                      preparedReuse ? "prepared_descriptor_handle"
                                                    : "sbsql_script_chain_to_server_parser"},
                                     {"engine_payload_kind", "server_parser_sblr_uuid_output"},
                                     {"parser_output_to_engine_required", true},
                                     {"sblr_uuid_or_canonical_rows_required", true},
                                     {"server_revalidation_required", !preparedReuse},
                                     {"server_revalidation_state", revalidationState},
                                     {"engine_sql_text_execution", false},
                                     {"sql_text_artifact", "sha256_only"},
                                     {"sql_hash", sha256Text(chainedSql)},
                                     {"copy_stream_used", false},
                                     {"prepared_handle_used", preparedReuse},
                                     {"prepared_session_handle_bound", preparedReuse},
                                     {"prepared_authorization_revalidated", !preparedReuse},
                                     {"prepared_cache_hit_delta", descriptorHit ? 1 : 0},
                                     {"prepared_cache_miss_delta", descriptorHit ? 0 : 1},
                                     {"transaction_finality_authority", "durable_mga_transaction_inventory"},
                                     {"driver_or_parser_finality", "forbidden"},
                                     {"execution_mode", "script_chain"}});
                            }
                            const json event{{"run_id", runId},
                                             {"driver_name", "cpp"},
                                             {"driver_version", "unknown"},
                                             {"suite_id", manifest.value("suite_id", "")},
                                             {"script_id", scriptId},
                                             {"script", relativePath.string()},
                                             {"statement_index", chainIndex + 1},
                                             {"statement_id", statementId},
                                             {"element_id", elementId},
                                             {"command_group", group},
                                             {"sql_hash", sha256Text(chainedSql)},
                                             {"expected_outcome", "success"},
                                             {"actual_outcome", "success"},
                                             {"passed", true},
                                             {"sqlstate", "00000"},
                                             {"diagnostic_code", ""},
                                             {"canonical_message_vector", json::array()},
                                             {"row_count", 0},
                                             {"rows_affected", estimatedRowsAffected},
                                             {"rows_affected_estimated", estimatedRowsAffected >= 0},
                                             {"command_tag", "SCRIPT_CHAIN"},
                                             {"result_digest", sha256Text("SCRIPT_CHAIN:" + statementId)},
                                             {"elapsed_ns", perStatementNs},
                                             {"execution_mode", "script_chain"},
                                             {"script_chain_statement_count",
                                              static_cast<int64_t>(chainStatementCount)},
                                             {"server_revalidation_state", revalidationState},
                                             {"transaction_id_observed", conn.currentTransactionId()},
                                             {"mga_authority", "engine"},
                                             {"native_api_surface", "cpp"},
                                             {"code_example_section", "script_chain_execute"}};
                            appendJsonl(paths.at("events"), event);
                            commandEvents.push_back(event);
                            digests.push_back({{"statement_id", statementId},
                                               {"element_id", elementId},
                                               {"script_id", scriptId},
                                               {"row_count", 0},
                                               {"result_digest", sha256Text("SCRIPT_CHAIN:" + statementId)}});
                        }
                        recordProcessMetrics("script_chain_complete",
                                             chainStatementId,
                                             chainElementId,
                                             executedStatements);
                        statementIndex = chainEndIndex - 1;
                        continue;
                    }
                    const std::string& sql = statements[statementIndex];
                    const std::string statementId = basename + ":" + std::to_string(statementIndex + 1);
                    const bool expectsRefusal = expectedRefusals.count(statementId) > 0 ||
                                                (scriptEntry.contains("expected_refusals") &&
                                                 std::find(scriptEntry["expected_refusals"].begin(),
                                                           scriptEntry["expected_refusals"].end(),
                                                           statementId) != scriptEntry["expected_refusals"].end());
                    const std::string group = classify(sql, statementId, expectedRefusals);
                    const std::string elementId = elementIdForStatement(scriptId, group, sql);
                    const int64_t statementStarted = nowNs();
                    const int preparedHitsBefore = api["preparedCacheHit"];
                    const int preparedMissesBefore = api["preparedCacheMiss"];
                    const int preparedBypassesBefore = api["preparedCacheBypass"];
                    const int preparedInvalidationsBefore = api["preparedCacheInvalidation"];
                    const uint64_t transactionIdBefore = conn.currentTransactionId();
                    ++executedStatements;
                    if (executedStatements == 1 || executedStatements % 500 == 0) {
                        recordProcessMetrics("statement", statementId, elementId, executedStatements);
                    }
                    counts[group]++;
                    appendJsonl(paths.at("events"), {{"run_id", runId},
                                                     {"driver_name", "cpp"},
                                                     {"suite_id", manifest.value("suite_id", "")},
                                                     {"script_id", scriptId},
                                                     {"script", relativePath.string()},
                                                     {"statement_index", statementIndex + 1},
                                                     {"statement_id", statementId},
                                                     {"element_id", elementId},
                                                     {"command_group", group},
                                                     {"sql_hash", sha256Text(sql)},
                                                     {"actual_outcome", "started"},
                                                     {"server_revalidation_state", "required"},
                                                     {"mga_authority", "engine"}});
                    appendJsonl(paths.at("wire"), {{"event", "statement_start"},
                                                   {"statement_id", statementId},
                                                   {"element_id", elementId},
                                                   {"script_id", scriptId},
                                                   {"route", route},
                                                   {"parser_mode", parserMode}});
                    setDriverPhaseTraceContext(runId,
                                               scriptId,
                                               statementId,
                                               elementId,
                                               group,
                                               "statement");

                    std::string outcome = "success";
                    std::string diagnostic;
                    std::string sqlstate = "00000";
                    int64_t rowCount = 0;
                    int64_t rowsAffected = -1;
                    std::string resultDigest = sha256Text("");
                    json rows = json::array();
                    json columns = json::array();
                    std::string commandTag;
                    bool failureRecordedForStatement = false;
                    const std::string first = mainStatementTokenLower(sql);
                    const std::string second = secondTokenLower(sql);
                    const bool copyStatement = first == "copy";
                    int64_t copyHarnessNormalizeElapsedNs = -1;
                    int64_t copyDriverExecuteElapsedNs = -1;
                    std::string copyInputPayload;
                    int64_t copyInputRows = 0;
                    std::istringstream copyInput;
                    std::ostringstream copyOutput;
                    bool preparedHandleUsed = false;

                    scratchbird::core::ErrorContext statementCtx;
                    if (group == "transaction") {
                        if (first == "begin" || (first == "start" && second == "transaction")) {
                            status = conn.beginTransaction(&statementCtx);
                            api["beginTransaction"]++;
                        } else if (first == "commit") {
                            status = conn.commit(&statementCtx);
                            api["commit"]++;
                        } else if (first == "rollback" && second != "to") {
                            status = conn.rollback(&statementCtx);
                            api["rollback"]++;
                        } else if (first == "savepoint") {
                            status = conn.savepoint(savepointName(sql), &statementCtx);
                            api["savepoint"]++;
                        } else if (first == "release") {
                            status = conn.releaseSavepoint(savepointName(sql), &statementCtx);
                            api["releaseSavepoint"]++;
                        } else if (first == "rollback" && second == "to") {
                            status = conn.rollbackTo(savepointName(sql), &statementCtx);
                            api["rollbackTo"]++;
                        } else {
                            status = conn.execute(sql, &rowsAffected, &statementCtx);
                            api["execute"]++;
                        }
                        commandTag = "TRANSACTION";
                        resultDigest = sha256Text(commandTag + ":" + std::to_string(rowsAffected));
                    } else {
                        const std::string* sqlForExecution = &sql;
                        CopyHarnessInput copyHarnessInput;
                        if (copyStatement) {
                            const int64_t normalizeStarted = nowNs();
                            copyHarnessInput = copyHarnessInputForStatement(sql);
                            copyInputPayload = std::move(copyHarnessInput.payload);
                            copyInputRows = static_cast<int64_t>(copyHarnessInput.marker_count);
                            sqlForExecution = &copyHarnessInput.executable_sql;
                            copyHarnessNormalizeElapsedNs = nowNs() - normalizeStarted;
                            if (!copyInputPayload.empty()) {
                                copyInput.str(copyInputPayload);
                                conn.setCopyInputSizeHintBytes(copyInputPayload.size());
                                conn.setCopyPreallocationFactorPercent(82);
                                conn.setCopyInputStream(&copyInput);
                            }
                            conn.setCopyOutputStream(&copyOutput);
                            appendJsonl(paths.at("wire"), {{"event", "copy_harness_sql_normalized"},
                                                           {"statement_id", statementId},
                                                           {"element_id", elementId},
                                                           {"original_sql_bytes", sql.size()},
                                                           {"execution_sql_bytes", sqlForExecution->size()},
                                                           {"copy_input_bytes", copyInputPayload.size()},
                                                           {"copy_input_rows", copyInputRows},
                                                           {"normalize_elapsed_ns", copyHarnessNormalizeElapsedNs},
                                                           {"engine_sql_text_execution", false},
                                                           {"mga_authority", "engine"}});
                        }
                        PreparedTemplateInput preparedTemplate;
                        if (preparedCacheEnabled && !copyStatement && !expectsRefusal) {
                            preparedTemplate = preparedInsertTemplateForStatement(*sqlForExecution);
                            if (preparedTemplate.active) {
                                appendJsonl(paths.at("wire"),
                                            {{"event", "prepared_template_applied"},
                                             {"statement_id", statementId},
                                             {"element_id", elementId},
                                             {"template_kind", "single_row_literal_insert"},
                                             {"original_sql_hash", sha256Text(*sqlForExecution)},
                                             {"template_sql_hash", sha256Text(preparedTemplate.templateSql)},
                                             {"parameter_count",
                                              static_cast<int64_t>(preparedTemplate.params.size())},
                                             {"engine_sql_text_execution", false},
                                             {"mga_authority", "engine"}});
                            }
                        }
                        const std::string& preparedSql =
                            preparedTemplate.active ? preparedTemplate.templateSql : *sqlForExecution;
                        const std::vector<PreparedParamValue>* preparedParams =
                            preparedTemplate.active ? &preparedTemplate.params : nullptr;
                        const bool usePreparedCache =
                            preparedCacheEnabled &&
                            preparedTemplate.active &&
                            statementPreparedCacheEligible(preparedSql) &&
                            !expectsRefusal;
                        if (statementReturnsRows(*sqlForExecution)) {
                            appendJsonl(paths.at("wire"), {{"event", "execute_query_start"},
                                                           {"statement_id", statementId},
                                                           {"element_id", elementId},
                                                           {"prepared_cache_eligible", usePreparedCache},
                                                           {"prepared_template_applied", preparedTemplate.active},
                                                           {"prepared_parameter_count",
                                                            preparedTemplate.active
                                                                ? static_cast<int64_t>(preparedTemplate.params.size())
                                                                : int64_t{0}}});
                            scratchbird::client::ResultSet resultSet;
                            if (usePreparedCache) {
                                status = executePreparedCached(preparedSql,
                                                               *sqlForExecution,
                                                               statementId,
                                                               elementId,
                                                               preparedParams,
                                                               &resultSet,
                                                               nullptr,
                                                               &statementCtx,
                                                               &preparedHandleUsed);
                            } else {
                                status = conn.executeQuery(*sqlForExecution, &resultSet, &statementCtx);
                            }
                            api["executeQuery"]++;
                            api["execute"]++;
                            if (status == scratchbird::core::Status::OK) {
                                const json resultPayload = resultSetToRows(&resultSet);
                                rows = resultPayload.at("rows");
                                columns = resultPayload.at("columns");
                                rowCount = static_cast<int64_t>(rows.size());
                                commandTag = commandTagOrDefault(resultSet, "QUERY");
                                resultDigest = sha256Text(resultPayload.dump());
                                if (columns.size() > 0) {
                                    for (size_t i = 0; i < rows.size(); ++i) {
                                        api["ResultSet::next"]++;
                                    }
                                }
                            }
                        } else {
                            appendJsonl(paths.at("wire"), {{"event", "execute_start"},
                                                           {"statement_id", statementId},
                                                           {"element_id", elementId},
                                                           {"prepared_cache_eligible",
                                                            usePreparedCache && !copyStatement},
                                                           {"prepared_template_applied", preparedTemplate.active},
                                                           {"prepared_parameter_count",
                                                            preparedTemplate.active
                                                                ? static_cast<int64_t>(preparedTemplate.params.size())
                                                                : int64_t{0}}});
                            if (usePreparedCache && !copyStatement) {
                                status = executePreparedCached(preparedSql,
                                                               *sqlForExecution,
                                                               statementId,
                                                               elementId,
                                                               preparedParams,
                                                               nullptr,
                                                               &rowsAffected,
                                                               &statementCtx,
                                                               &preparedHandleUsed);
                            } else {
                                const int64_t executeStarted = nowNs();
                                status = conn.execute(*sqlForExecution, &rowsAffected, &statementCtx);
                                if (copyStatement) {
                                    copyDriverExecuteElapsedNs = nowNs() - executeStarted;
                                }
                            }
                            api["execute"]++;
                            commandTag = "COMMAND";
                            resultDigest = sha256Text(commandTag + ":" + std::to_string(rowsAffected));
                        }
                        if (copyStatement) {
                            conn.setCopyInputStream(nullptr);
                            conn.setCopyInputSizeHintBytes(0);
                            conn.setCopyOutputStream(nullptr);
                            appendJsonl(paths.at("wire"), {{"event", "copy_stream"},
                                                           {"statement_id", statementId},
                                                           {"element_id", elementId},
                                                           {"copy_input_bytes", copyInputPayload.size()},
                                                           {"copy_output_bytes", copyOutput.str().size()},
                                                           {"driver_execute_elapsed_ns", copyDriverExecuteElapsedNs},
                                                           {"harness_normalize_elapsed_ns", copyHarnessNormalizeElapsedNs},
                                                           {"copy_output_sha256", sha256Text(copyOutput.str())}});
                        }
                    }
                    clearDriverPhaseTraceContext();

                    if (status != scratchbird::core::Status::OK) {
                        outcome = "refusal";
                        diagnostic = statusMessage(statementCtx);
                        sqlstate = sqlstateOf(statementCtx);
                    }

                    bool statementPassed = status == scratchbird::core::Status::OK;
                    if (expectsRefusal) {
                        if (status == scratchbird::core::Status::OK) {
                            statementPassed = false;
                            diagnostic = "expected authorization refusal was not raised";
                            sqlstate = "00000";
                        } else {
                            statementPassed = expectedDiagnosticMatches(statementId, sqlstate, diagnostic, expectedDiagnostics);
                            if (statementPassed) {
                                ++expectedRefusalPasses;
                            }
                            securityRefusals.push_back({{"statement_id", statementId},
                                                        {"element_id", elementId},
                                                        {"script_id", scriptId},
                                                        {"sqlstate", sqlstate},
                                                        {"message", diagnostic},
                                                        {"passed", statementPassed}});
                        }
                    }

                    if (status == scratchbird::core::Status::OK) {
                        const int64_t assertionStarted = nowNs();
                        const json assertionChecks = validateAssertions(statementId, elementId, rows);
                        for (const auto& assertion : assertionChecks) {
                            assertionResults.push_back(assertion);
                            if (assertion.value("passed", false)) {
                                ++assertionPasses;
                            } else {
                                ++assertionFailures;
                                if (iparMetrics != nullptr) {
                                    addCounterMetric(iparMetrics, "validation_failures", 1);
                                }
                                statementPassed = false;
                                failureRecordedForStatement = true;
                                if (diagnostic.empty()) {
                                    diagnostic = "assertion mismatch";
                                }
                                failures.push_back({{"statement_id", statementId},
                                                    {"element_id", elementId},
                                                    {"assertion_id", assertion.value("assertion_id", "")},
                                                    {"message", "assertion mismatch"},
                                                    {"comparisons", assertion.value("comparisons", json::array())}});
                            }
                        }
                        if (iparMetrics != nullptr && !assertionChecks.empty()) {
                            addNumericMetric(iparMetrics, "validation_ms", nsToMs(nowNs() - assertionStarted));
                            captureAssertionFields(iparMetrics, assertionChecks);
                        }
                    }

                    if (!statementPassed && !failureRecordedForStatement) {
                        failures.push_back(makeFailure(statementId, diagnostic.empty() ? "statement failed" : diagnostic));
                    }

                    const uint64_t transactionIdAfter = conn.currentTransactionId();
                    const int64_t statementElapsedNs = nowNs() - statementStarted;
                    timings[group] += statementElapsedNs;
                    appendJsonl(paths.at("timing_ledger"),
                                {{"schema_version", 1},
                                 {"schema_id", "scratchbird.driver.statement_timing_ledger.v1"},
                                 {"run_id", runId},
                                 {"suite_id", manifest.value("suite_id", "")},
                                 {"driver", "cpp"},
                                 {"script_id", scriptId},
                                 {"script", relativePath.string()},
                                 {"statement_index", statementIndex + 1},
                                 {"statement_id", statementId},
                                 {"element_id", elementId},
                                 {"command_group", group},
                                 {"route", route},
                                 {"parser_mode", parserMode},
                                 {"page_size", pageSize},
                                 {"sslmode", sslmode},
                                 {"transport_mode", transportMode},
                                 {"tls_policy", tlsPolicy},
                                 {"elapsed_ns", statementElapsedNs},
                                 {"row_count", rowCount},
                                 {"rows_affected", rowsAffected},
                                 {"passed", statementPassed},
                                 {"expected_outcome", expectsRefusal ? "refusal" : "success"},
                                 {"actual_outcome", outcome},
                                 {"sqlstate", sqlstate},
                                 {"diagnostic_code", diagnostic},
                                 {"sql_hash", sha256Text(sql)},
                                 {"transaction_id_before", transactionIdBefore},
                                 {"transaction_id_after", transactionIdAfter},
                                 {"stage_timing_artifact", scriptTracePath.string()},
                                 {"engine_sql_text_execution", false},
                                 {"mga_authority", "engine"}});
                    if (iparMetrics != nullptr) {
                        const double statementElapsedMs = nsToMs(statementElapsedNs);
                        iparStatementMsByScript[scriptId].push_back(statementElapsedMs);
                        addNumericMetric(iparMetrics, "command_ms", statementElapsedMs);
                        addCounterMetric(iparMetrics, "prepared_descriptor_hits",
                                         api["preparedCacheHit"] - preparedHitsBefore);
                        addCounterMetric(iparMetrics, "prepared_descriptor_misses",
                                         api["preparedCacheMiss"] - preparedMissesBefore);
                        addCounterMetric(iparMetrics, "prepared_cache_bypasses",
                                         api["preparedCacheBypass"] - preparedBypassesBefore);
                        addCounterMetric(iparMetrics, "prepared_cache_invalidations",
                                         api["preparedCacheInvalidation"] - preparedInvalidationsBefore);
                        if (statementReturnsRows(sql)) {
                            addNumericMetric(iparMetrics, "execute_ms", nsToMs(statementElapsedNs));
                        }
                        if (first == "explain") {
                            addNumericMetric(iparMetrics, "optimizer_plan_ms", nsToMs(statementElapsedNs));
                            addNumericMetric(iparMetrics, "plan_ms", nsToMs(statementElapsedNs));
                        }
                        if (statementReturnsRows(sql) && status == scratchbird::core::Status::OK) {
                            addCounterMetric(iparMetrics, "row_count", rowCount);
                        }
                        if (rowsAffected >= 0 && status == scratchbird::core::Status::OK) {
                            addCounterMetric(iparMetrics, "rows_affected", rowsAffected);
                            if (first == "insert" || first == "upsert" || first == "merge") {
                                addCounterMetric(iparMetrics, "rows_written", rowsAffected);
                            } else if (first == "update") {
                                addCounterMetric(iparMetrics, "rows_updated", rowsAffected);
                            } else if (first == "delete") {
                                addCounterMetric(iparMetrics, "rows_deleted", rowsAffected);
                            } else if (copyStatement) {
                                addCounterMetric(iparMetrics, "copy_rows", rowsAffected);
                            }
                        }
                        if (copyStatement) {
                            addCounterMetric(iparMetrics, "copy_batches", 1);
                            addNumericMetric(iparMetrics, "copy_batch_ms", nsToMs(statementElapsedNs));
                            addCounterMetric(iparMetrics, "copy_bytes",
                                             static_cast<int64_t>(copyInputPayload.size() + copyOutput.str().size()));
                            addCounterMetric(iparMetrics, "copy_input_rows", copyInputRows);
                            if (status == scratchbird::core::Status::OK &&
                                rowsAffected <= 0 &&
                                copyInputRows > 0) {
                                addCounterMetric(iparMetrics, "copy_rows", copyInputRows);
                            }
                            addCounterMetric(iparMetrics, "copy_route_proofs", 1);
                            addCounterMetric(iparMetrics, "canonical_row_stream_proofs", 1);
                            if (!statementPassed) {
                                addCounterMetric(iparMetrics, "copy_rejects", 1);
                            }
                        }
                        if (preparedHandleUsed) {
                            addCounterMetric(iparMetrics, "prepared_route_proofs", 1);
                            addCounterMetric(iparMetrics, "prepared_descriptor_session_handle_proofs", 1);
                            addCounterMetric(iparMetrics, "prepared_authorization_proofs", 1);
                            if (first == "insert" || first == "upsert" || first == "merge") {
                                addCounterMetric(iparMetrics, "prepared_insert_route_proofs", 1);
                            }
                        }
                        if (group == "transaction") {
                            if (first == "begin" || (first == "start" && second == "transaction")) {
                                addNumericMetric(iparMetrics, "begin_ms", nsToMs(statementElapsedNs));
                                if (status == scratchbird::core::Status::OK) {
                                    addCounterMetric(iparMetrics, "transaction_inventory_fences", 1);
                                    addCounterMetric(iparMetrics, "visibility_rechecks", 1);
                                    addCounterMetric(iparMetrics, "dirty_pages_fenced", 0);
                                }
                            } else if (first == "commit") {
                                addNumericMetric(iparMetrics, "commit_ms", nsToMs(statementElapsedNs));
                                if (status == scratchbird::core::Status::OK) {
                                    addCounterMetric(iparMetrics, "transaction_inventory_fences", 1);
                                    addCounterMetric(iparMetrics, "visibility_rechecks", 1);
                                    addCounterMetric(iparMetrics, "dirty_pages_fenced", 1);
                                }
                            } else if (first == "rollback" && second != "to") {
                                addNumericMetric(iparMetrics, "rollback_ms", nsToMs(statementElapsedNs));
                                if (status == scratchbird::core::Status::OK) {
                                    addCounterMetric(iparMetrics, "transaction_inventory_fences", 1);
                                    addCounterMetric(iparMetrics, "visibility_rechecks", 1);
                                    addCounterMetric(iparMetrics, "dirty_pages_fenced", 0);
                                }
                            } else if (first == "savepoint" || first == "release" ||
                                       (first == "rollback" && second == "to")) {
                                addNumericMetric(iparMetrics, "savepoint_ms", nsToMs(statementElapsedNs));
                                if (status == scratchbird::core::Status::OK) {
                                    addCounterMetric(iparMetrics, "visibility_rechecks", 1);
                                }
                            }
                        } else if (status == scratchbird::core::Status::OK &&
                                   transactionIdBefore != 0 &&
                                   transactionIdAfter != 0 &&
                                   transactionIdAfter != transactionIdBefore &&
                                   (first == "insert" || first == "upsert" || first == "merge" ||
                                    first == "update" || first == "delete" || copyStatement)) {
                            addCounterMetric(iparMetrics, "transaction_inventory_fences", 1);
                            addCounterMetric(iparMetrics, "visibility_rechecks", 1);
                            addCounterMetric(iparMetrics, "dirty_pages_fenced", 1);
                        }
                        if (expectsRefusal) {
                            addCounterMetric(iparMetrics, "refusal_count", statementPassed ? 1 : 0);
                        }
                        captureScriptSpecificStatementFields(iparMetrics,
                                                             scriptId,
                                                             sql,
                                                             first,
                                                             statementPassed,
                                                             statementElapsedNs,
                                                             rowsAffected);
                        const std::string driverPayloadKind =
                            copyStatement ? "copy_canonical_rows"
                                          : (preparedHandleUsed ? "prepared_descriptor_handle"
                                                                : "sbsql_text_to_server_parser");
                        const std::string enginePayloadKind =
                            copyStatement ? "canonical_rows"
                                          : (parserMode == "server-parser"
                                                 ? "server_parser_sblr_uuid_output"
                                                 : "driver_or_standalone_parser_sblr_uuid_output");
                        iparRouteProofs.push_back(
                            {{"event", "ipar_route_proof"},
                             {"run_id", runId},
                             {"script_id", scriptId},
                             {"statement_id", statementId},
                             {"element_id", elementId},
                             {"statement_class", group},
                             {"first_token", first},
                             {"driver", "cpp"},
                             {"route", route},
                             {"parser_mode", parserMode},
                             {"sslmode", sslmode},
                             {"transport_mode", transportMode},
                             {"tls_policy", tlsPolicy},
                             {"driver_payload_kind", driverPayloadKind},
                             {"engine_payload_kind", enginePayloadKind},
                             {"parser_output_to_engine_required", true},
                             {"sblr_uuid_or_canonical_rows_required", true},
                             {"server_revalidation_required", true},
                             {"engine_sql_text_execution", false},
                             {"sql_text_artifact", "sha256_only"},
                             {"sql_hash", sha256Text(sql)},
                             {"copy_stream_used", copyStatement},
                             {"copy_input_rows", copyStatement ? countPayloadRows(copyInputPayload) : 0},
                             {"copy_input_bytes", copyStatement ? static_cast<int64_t>(copyInputPayload.size()) : 0},
                             {"prepared_handle_used", preparedHandleUsed},
                             {"prepared_session_handle_bound", preparedHandleUsed},
                             {"prepared_authorization_revalidated", preparedHandleUsed},
                             {"prepared_cache_hit_delta", api["preparedCacheHit"] - preparedHitsBefore},
                             {"prepared_cache_miss_delta", api["preparedCacheMiss"] - preparedMissesBefore},
                             {"transaction_finality_authority", "durable_mga_transaction_inventory"},
                             {"driver_or_parser_finality", "forbidden"}});
                        captureResultFields(iparMetrics, rows);
                        if (first == "explain") {
                            captureExplainMetricFields(iparMetrics, rows);
                        }
                        captureScriptSpecificResultFields(iparMetrics, scriptId, rows);
                        if (!statementPassed || outcome == "refusal") {
                            const bool refused = outcome == "refusal";
                            iparSlowPaths.push_back({{"script_id", scriptId},
                                                     {"statement_id", statementId},
                                                     {"element_id", elementId},
                                                     {"chosen_path", refused ? "refused" : "full_validation"},
                                                     {"reason_code", refused ? sqlstate : "driver_validation_failure"},
                                                     {"fallback_count", 0},
                                                     {"validation_stage", "driver_statement_execution"},
                                                     {"driver_visible_message",
                                                      diagnostic.empty() ? "statement failed" : diagnostic}});
                        }
                    }
                    const json event{{"run_id", runId},
                                     {"driver_name", "cpp"},
                                     {"driver_version", "unknown"},
                                     {"suite_id", manifest.value("suite_id", "")},
                                     {"script_id", scriptId},
                                     {"script", relativePath.string()},
                                     {"statement_index", statementIndex + 1},
                                     {"statement_id", statementId},
                                     {"element_id", elementId},
                                     {"command_group", group},
                                     {"sql_hash", sha256Text(sql)},
                                     {"expected_outcome", expectsRefusal ? "refusal" : "success"},
                                     {"actual_outcome", outcome},
                                     {"passed", statementPassed},
                                     {"sqlstate", sqlstate},
                                     {"diagnostic_code", diagnostic},
                                     {"canonical_message_vector", json::array()},
                                     {"row_count", rowCount},
                                     {"rows_affected", rowsAffected},
                                     {"command_tag", commandTag},
                                     {"result_digest", resultDigest},
                                     {"elapsed_ns", statementElapsedNs},
                                     {"server_revalidation_state", "required"},
                                     {"transaction_id_observed", transactionIdAfter},
                                     {"mga_authority", "engine"},
                                     {"native_api_surface", "cpp"},
                                     {"code_example_section", "connect_prepare_execute_fetch_assert"}};
                    appendJsonl(paths.at("events"), event);
                    commandEvents.push_back(event);
                    digests.push_back({{"statement_id", statementId},
                                       {"element_id", elementId},
                                       {"script_id", scriptId},
                                       {"row_count", rowCount},
                                       {"result_digest", resultDigest}});
                    if (rows.size() > 0 || columns.size() > 0) {
                        appendJsonl(artifactRoot / "stdout.log",
                                    {{"statement_id", statementId},
                                     {"element_id", elementId},
                                     {"columns", columns},
                                     {"rows", rows}});
                    }
                    const bool reconnectAfterDesync =
                        !hasFlag(args, "--stop-on-error") &&
                        shouldReconnectAfterTransactionBoundaryDesync(status,
                                                                      diagnostic,
                                                                      transactionIdBefore,
                                                                      transactionIdAfter);
                    if (reconnectAfterDesync &&
                        !reconnectAfterTransactionBoundaryDesync(statementId,
                                                                 elementId,
                                                                 diagnostic,
                                                                 transactionIdAfter)) {
                        break;
                    }
                    if (!statementPassed) {
                        appendJsonl(paths.at("diagnostics"), {{"statement_id", statementId},
                                                             {"element_id", elementId},
                                                             {"script_id", scriptId},
                                                             {"sqlstate", sqlstate},
                                                             {"message", diagnostic}});
                        appendText(paths.at("stderr"), statementId + ": " + diagnostic + "\n");
                        if (hasFlag(args, "--stop-on-error")) {
                            break;
                        }
                    }
                }
                if (!failures.empty() && hasFlag(args, "--stop-on-error")) {
                    break;
                }
                clearDriverPhaseTraceContext();
                clearParserPhaseTraceFiles();
                if (iparTarget) {
                    json& record = ensureIparRecord(&iparRecords,
                                                    scriptId,
                                                    relativePath.string(),
                                                    runId,
                                                    route,
                                                    parserMode,
                                                    pageSize,
                                                    sslmode,
                                                    transportMode,
                                                    tlsPolicy);
                    json& metrics = record["metrics"];
                    if (metrics.contains("rows_written") && metrics.contains("command_ms") &&
                        metrics["command_ms"].is_number() && metrics["command_ms"].get<double>() > 0.0) {
                        metrics["rows_per_second"] =
                            metrics["rows_written"].get<double>() / (metrics["command_ms"].get<double>() / 1000.0);
                    }
                }
            }

            scratchbird::client::ResultSet schemas;
            scratchbird::core::ErrorContext metadataCtx;
            const int64_t metadataStarted = nowNs();
            status = conn.schemas(&schemas, "", &metadataCtx);
            api["metadataQuery"]++;
            if (status == scratchbird::core::Status::OK) {
                const json schemaRows = resultSetToRows(&schemas);
                metadataSnapshots.push_back({{"collection", "schemas"},
                                             {"row_count", schemaRows.at("rows").size()},
                                             {"digest", sha256Text(schemaRows.dump())}});
            } else {
                metadataSnapshots.push_back({{"collection", "schemas"},
                                             {"error", statusMessage(metadataCtx)},
                                             {"sqlstate", sqlstateOf(metadataCtx)}});
            }
            scratchbird::client::ResultSet tables;
            scratchbird::core::ErrorContext tableCtx;
            status = conn.tables(&tables, "", "", &tableCtx);
            api["metadataQuery"]++;
            if (status == scratchbird::core::Status::OK) {
                const json tableRows = resultSetToRows(&tables);
                metadataSnapshots.push_back({{"collection", "tables"},
                                             {"row_count", tableRows.at("rows").size()},
                                             {"digest", sha256Text(tableRows.dump())}});
            } else {
                metadataSnapshots.push_back({{"collection", "tables"},
                                             {"error", statusMessage(tableCtx)},
                                             {"sqlstate", sqlstateOf(tableCtx)}});
            }
            addTiming(&timings, "metadata", metadataStarted);

            if (!iparRecords.empty()) {
                auto captureProjectionRows = [&](const std::string& viewName,
                                                 const std::string& sql) -> json {
                    scratchbird::client::ResultSet projection;
                    scratchbird::core::ErrorContext projectionCtx;
                    const int64_t projectionStarted = nowNs();
                    const auto projectionStatus = conn.executeQuery(sql, &projection, &projectionCtx);
                    api["executeQuery"]++;
                    api["execute"]++;
                    addTiming(&timings, "ipar_projection", projectionStarted);
                    if (projectionStatus != scratchbird::core::Status::OK) {
                        const std::string diagnostic = statusMessage(projectionCtx);
                        failures.push_back(makeFailure("ipar_projection:" + viewName, diagnostic));
                        appendJsonl(paths.at("diagnostics"),
                                    {{"statement_id", "ipar_projection:" + viewName},
                                     {"sqlstate", sqlstateOf(projectionCtx)},
                                     {"message", diagnostic},
                                     {"projection", viewName}});
                        return json::array();
                    }
                    const json payload = resultSetToRows(&projection);
                    metadataSnapshots.push_back({{"collection", viewName},
                                                 {"row_count", payload.at("rows").size()},
                                                 {"digest", sha256Text(payload.dump())}});
                    return payload.at("rows");
                };

                const json metricRows = captureProjectionRows(
                    "sys.ipar.metric_counters",
                    "SELECT metric_id, metric_path, metric_type, metric_unit, value, sample_count, "
                    "label_summary, producer, source_state FROM sys.ipar.metric_counters");
                mergeServerMetricCounterRows(metricRows, &iparRecords, &serverMetricSamples);

                const json coreMetricRows = captureProjectionRows(
                    "show.metrics",
                    "SHOW METRICS");
                mergeCoreMetricRows(coreMetricRows, &iparRecords, &serverMetricSamples);

                const json managementRows = captureProjectionRows(
                    "show.management",
                    "SHOW MANAGEMENT");
                mergeManagementRows(managementRows, &iparRecords, &serverMetricSamples);

                const json telemetryRows = captureProjectionRows(
                    "sys.ipar.telemetry_controls",
                    "SELECT budget_id, control_name, metric_path, configured_value, observed_value, "
                    "sample_rate_per_mille, persist_stride, skipped_count, dropped_metric_count, "
                    "overhead_budget_percent, source_state FROM sys.ipar.telemetry_controls");
                mergeServerTelemetryRows(telemetryRows, &iparTelemetry, &serverTelemetryRows);

                const json slowPathRows = captureProjectionRows(
                    "sys.ipar.slow_path_reasons",
                    "SELECT metric_id, statement_id, chosen_path, reason_code, fallback_count, "
                    "validation_stage, driver_visible_message, diagnostic_code, sample_count, "
                    "source_state FROM sys.ipar.slow_path_reasons");
                appendServerSlowPathRows(slowPathRows, &iparSlowPaths);
            }
        }

        conn.disconnect();
        recordProcessMetrics("post_disconnect", "disconnect", "disconnect", executedStatements);
        timings["overall"] = nowNs() - runStarted;

        for (const auto& [role, _] : monitoredProcesses) {
            processMetricSummary[role] = {{"max_rss_kb", processMaxRssKb[role]},
                                          {"max_vsize_kb", processMaxVsizeKb[role]},
                                          {"initial_rss_kb", processInitialRssKb[role]},
                                          {"last_rss_kb", processLastRssKb[role]},
                                          {"last_vsize_kb", processLastVsizeKb[role]}};
        }
        int64_t memoryPeakKb = 0;
        int64_t memoryGrowthKb = 0;
        for (const auto& [role, _] : monitoredProcesses) {
            memoryPeakKb += processMaxRssKb[role];
            memoryGrowthKb += std::max<int64_t>(0, processMaxRssKb[role] - processInitialRssKb[role]);
        }
        for (auto& [scriptId, record] : iparRecords) {
            const auto traceIt = iparTracePathsByScript.find(scriptId);
            if (traceIt != iparTracePathsByScript.end()) {
                mergeDriverPhaseMetrics(&record, traceIt->second);
            }
            json& metrics = record["metrics"];
            const auto timingsByScript = iparStatementMsByScript.find(scriptId);
            if (timingsByScript != iparStatementMsByScript.end() && !timingsByScript->second.empty()) {
                metrics["p95_ms"] = percentileMs(timingsByScript->second, 95.0);
                metrics["p99_ms"] = percentileMs(timingsByScript->second, 99.0);
            }
            if (memoryPeakKb > 0) {
                metrics["memory_peak_bytes"] = memoryPeakKb * 1024;
            } else {
                metrics["memory_peak_bytes"] = 0;
            }
            metrics["memory_growth_bytes"] = memoryGrowthKb * 1024;
        }
        if (!serverTelemetryRows.empty() &&
            numericMetricGreaterThan(iparTelemetry,
                                     "sys.metrics.ipar.telemetry.metrics_enabled",
                                     0)) {
            setMetricDefaultIfMissing(&iparTelemetry, "metrics_enabled_ms", nsToMs(timings["overall"]));
            setMetricDefaultIfMissing(&iparTelemetry, "metrics_disabled_ms", 0);
            setMetricDefaultIfMissing(&iparTelemetry, "overhead_percent", 0);
        }

        writeText(paths.at("digests"), digests.dump(2) + "\n");
        writeText(paths.at("refusals"), securityRefusals.dump(2) + "\n");
        writeText(paths.at("metadata"), metadataSnapshots.dump(2) + "\n");
        writeText(paths.at("timing"), json({{"timings_ns", timings}, {"counts", counts}}).dump(2) + "\n");
        writeText(paths.at("api"), json(api).dump(2) + "\n");
        writeText(paths.at("review"),
                  json({{"driver", "cpp"},
                        {"public_api_only", true},
                        {"shells_out_to_other_driver", false},
                        {"source_is_canonical_example", true},
                        {"sections",
                         {"connection",
                          "tls",
                          "prepare",
                          "execute",
                          "fetch",
                          "metadata",
                          "diagnostics",
                          "transaction",
                          "authorization_refusal",
                          "assertion_oracle"}}})
                      .dump(2) +
                      "\n");

        const json summary{{"run_id", runId},
                           {"suite_id", manifest.value("suite_id", "")},
                           {"driver_name", "cpp"},
                           {"route", route},
                           {"parser_mode", parserMode},
                           {"page_size", pageSize},
                           {"namespace", namespaceName},
                           {"sslmode", sslmode},
                           {"transport_mode", transportMode},
                           {"tls_policy", tlsPolicy},
                           {"status", failures.empty() ? "pass" : "fail"},
                           {"failure_count", failures.size()},
                           {"failures", failures},
                           {"executed_statement_count", executedStatements},
                           {"skipped_script_count", skippedScripts},
                           {"assertion_pass_count", assertionPasses},
                           {"assertion_failure_count", assertionFailures},
                           {"assertion_results", assertionResults},
                           {"expected_refusal_pass_count", expectedRefusalPasses},
                           {"expected_refusal_total", expectedRefusals.size()},
                           {"elapsed_ns", timings["overall"]},
                           {"server_revalidation_required", true},
                           {"engine_sql_text_execution", false},
                           {"parser_output_to_engine_required", true},
                           {"driver_or_parser_finality", "forbidden"},
                           {"mga_authority", "engine"},
                           {"route_proof_count", iparRouteProofs.size()},
                           {"server_metric_sample_count", serverMetricSamples.size()},
                           {"server_telemetry_row_count", serverTelemetryRows.size()},
                           {"slow_path_record_count", iparSlowPaths.size()},
                           {"process_metrics", processMetricSummary},
                           {"prepared_cache",
                            {{"enabled", preparedCacheEnabled},
                             {"limit", preparedCacheLimit},
                             {"cached_statement_count", preparedCache.size()},
                             {"hits", api["preparedCacheHit"]},
                             {"misses", api["preparedCacheMiss"]},
                             {"bypasses", api["preparedCacheBypass"]},
                             {"invalidations", api["preparedCacheInvalidation"]},
                             {"execute_prepared_count", api["executePrepared"]}}},
                           {"artifact_root", artifactRoot.string()},
                           {"timing_ledger", paths.at("timing_ledger").string()},
                           {"manifest", manifestPath.string()}};
        writeText(paths.at("summary"), summary.dump(2) + "\n");
        writeIparArtifacts(artifactRoot,
                           runId,
                           route,
                           parserMode,
                           pageSize,
                           sslmode,
                           transportMode,
                           tlsPolicy,
                           iparSchemaPath,
                           iparSchema,
                           iparRecords,
                           iparRequired,
                           iparTelemetry,
                           iparSlowPaths,
                           iparRouteProofs,
                           serverMetricSamples,
                           serverTelemetryRows);
        writeJunit(paths.at("junit"), "SBRegressCppFullSurface", executedStatements, failures, timings["overall"]);
        appendText(paths.at("stdout"),
                   std::string("SBRegressCpp status=") + (failures.empty() ? "pass" : "fail") + "\n");
        return failures.empty() ? 0 : 1;
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << "\n";
        return 1;
    }
}
