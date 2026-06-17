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
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
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

std::string firstTokenLower(const std::string& sql) {
    std::istringstream in(trim(sql));
    std::string first;
    in >> first;
    return lower(first);
}

std::string secondTokenLower(const std::string& sql) {
    std::istringstream in(trim(sql));
    std::string first;
    std::string second;
    in >> first >> second;
    return lower(second);
}

std::string savepointName(const std::string& sql) {
    std::istringstream in(trim(sql));
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
    const auto first = firstTokenLower(sql);
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

bool statementReturnsRows(const std::string& sql) {
    const std::string first = firstTokenLower(sql);
    if (first == "select" || first == "with" || first == "values" || first == "show" ||
        first == "explain" || first == "sbsql_surface_replay") {
        return true;
    }
    const std::string text = " " + lower(sql) + " ";
    return text.find(" returning ") != std::string::npos;
}

bool statementPreparedCacheEligible(const std::string& sql) {
    const std::string first = firstTokenLower(sql);
    return first == "select" || first == "with" || first == "values";
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

json validateAssertions(const std::string& statementId, const json& rows) {
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
    for (const auto& expected : it->second) {
        if (sqlstate.find(expected) != std::string::npos || message.find(expected) != std::string::npos) {
            return true;
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
            {"digests", artifactRoot / "result-digests.json"},
            {"metadata", artifactRoot / "metadata-snapshots.json"},
            {"refusals", artifactRoot / "security-refusals.json"},
            {"api", artifactRoot / "native-api-coverage.json"},
            {"review", artifactRoot / "code-example-review.json"},
            {"process_metrics", artifactRoot / "process-metrics.jsonl"},
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
        std::map<std::string, int64_t> processLastRssKb;
        std::map<std::string, int64_t> processLastVsizeKb;
        std::map<std::string, int> api{{"scratchbird::client::Connection", 0},
                                       {"connect", 0},
                                       {"prepare", 0},
                                       {"executePrepared", 0},
                                       {"preparedCacheHit", 0},
                                       {"preparedCacheMiss", 0},
                                       {"preparedCacheBypass", 0},
                                       {"preparedCacheInvalidation", 0},
                                       {"execute", 0},
                                       {"executeQuery", 0},
                                       {"metadataQuery", 0},
                                       {"commit", 0},
                                       {"rollback", 0},
                                       {"savepoint", 0},
                                       {"releaseSavepoint", 0},
                                       {"rollbackTo", 0},
                                       {"ResultSet::next", 0}};

        const int64_t runStarted = nowNs();
        const json manifest = readJson(manifestPath);
        std::map<std::string, std::vector<std::string>> expectedDiagnostics;
        const std::set<std::string> expectedRefusals =
            loadExpectedRefusals(expectedRefusalsPath, &expectedDiagnostics);

        const std::string namespaceName = required(args, "--namespace");
        const std::string sslmode = valueOrDefault(args, "--sslmode", "require");
        const std::string transportMode = sslmode == "disable" ? "tls_disabled" : "tls_required";
        const std::vector<std::string> scriptFilters = splitCsv(valueOrDefault(args, "--script-ids", ""));
        const std::set<std::string> scriptFilterSet(scriptFilters.begin(), scriptFilters.end());
        const std::size_t preparedCacheLimit = static_cast<std::size_t>(
            std::stoull(valueOrDefault(args, "--prepared-cache-size", "256")));
        const bool preparedCacheEnabled =
            preparedCacheLimit > 0 && valueOrDefault(args, "--prepared-cache-mode", "read_queries") != "off";

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
                                              int statementIndex) {
            for (const auto& [role, pid] : monitoredProcesses) {
                json sample = sampleProcessMetrics(role, pid);
                sample["run_id"] = runId;
                sample["phase"] = phase;
                sample["statement_id"] = statementId;
                sample["statement_index"] = statementIndex;
                sample["sample_monotonic_ns"] = nowNs();
                if (sample.value("ok", false)) {
                    const auto rss = sample.value("rss_kb", int64_t{0});
                    const auto vsize = sample.value("vsize_kb", int64_t{0});
                    processLastRssKb[role] = rss;
                    processLastVsizeKb[role] = vsize;
                    processMaxRssKb[role] = std::max(processMaxRssKb[role], rss);
                    processMaxVsizeKb[role] = std::max(processMaxVsizeKb[role], vsize);
                }
                appendJsonl(paths.at("process_metrics"), sample);
            }
        };
        recordProcessMetrics("suite_start", "suite_start", 0);

        const std::map<std::string, std::string> replacements{
            {"__SB_NAMESPACE__", namespaceName},
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
            recordProcessMetrics("post_connect", "connect", 0);
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

        auto executePreparedCached = [&](const std::string& sql,
                                         const std::string& statementId,
                                         scratchbird::client::ResultSet* resultSet,
                                         scratchbird::core::ErrorContext* statementCtx) {
            auto runPrepared = [&](scratchbird::client::PreparedStatement* stmt) {
                api["executePrepared"]++;
                return stmt->executeQuery(resultSet, statementCtx);
            };

            auto found = preparedCache.find(sql);
            if (found != preparedCache.end() && found->second && found->second->isValid()) {
                api["preparedCacheHit"]++;
                const auto preparedStatus = runPrepared(found->second.get());
                if (preparedStatus == scratchbird::core::Status::OK) {
                    return preparedStatus;
                }
                api["preparedCacheInvalidation"]++;
                appendJsonl(paths.at("wire"), {{"event", "prepared_cache_invalidate"},
                                               {"statement_id", statementId},
                                               {"sql_hash", sha256Text(sql)},
                                               {"reason", statusMessage(*statementCtx)}});
                preparedCache.erase(found);
            }

            if (preparedCache.size() >= preparedCacheLimit) {
                api["preparedCacheBypass"]++;
                appendJsonl(paths.at("wire"), {{"event", "prepared_cache_bypass"},
                                               {"statement_id", statementId},
                                               {"sql_hash", sha256Text(sql)},
                                               {"reason", "cache_limit"}});
                return conn.executeQuery(sql, resultSet, statementCtx);
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
                                           {"sql_hash", sha256Text(sql)},
                                           {"server_revalidation_state", "required"},
                                           {"mga_authority", "engine"}});
            auto inserted = preparedCache.emplace(sql, std::move(prepared));
            return runPrepared(inserted.first->second.get());
        };

        if (failures.empty() && preparedCacheEnabled) {
            scratchbird::client::ResultSet probeFirst;
            scratchbird::client::ResultSet probeSecond;
            scratchbird::core::ErrorContext probeCtx;
            const std::string probeSql = "SELECT 1";
            auto probeStatus =
                executePreparedCached(probeSql, "prepared_cache_probe:1", &probeFirst, &probeCtx);
            if (probeStatus == scratchbird::core::Status::OK) {
                probeStatus =
                    executePreparedCached(probeSql, "prepared_cache_probe:2", &probeSecond, &probeCtx);
            }
            if (probeStatus != scratchbird::core::Status::OK) {
                failures.push_back(makeFailure("prepared_cache_probe", statusMessage(probeCtx)));
                appendJsonl(paths.at("diagnostics"), {{"statement_id", "prepared_cache_probe"},
                                                     {"sqlstate", sqlstateOf(probeCtx)},
                                                     {"message", statusMessage(probeCtx)}});
            } else {
                appendJsonl(paths.at("wire"), {{"event", "prepared_cache_probe_passed"},
                                               {"sql_hash", sha256Text(probeSql)},
                                               {"prepared_cache_limit", preparedCacheLimit},
                                               {"server_revalidation_state", "required"},
                                               {"mga_authority", "engine"}});
            }
        }

        int executedStatements = 0;
        int skippedScripts = 0;
        int expectedRefusalPasses = 0;
        int assertionPasses = 0;
        int assertionFailures = 0;

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

                const std::string scriptText = applyPlaceholders(stripComments(readText(scriptPath)), replacements);
                const std::vector<std::string> statements = sbchunk::splitStatements(scriptText);
                const std::string basename = relativePath.filename().string();

                for (size_t statementIndex = 0; statementIndex < statements.size(); ++statementIndex) {
                    const std::string& sql = statements[statementIndex];
                    const std::string statementId = basename + ":" + std::to_string(statementIndex + 1);
                    const bool expectsRefusal = expectedRefusals.count(statementId) > 0 ||
                                                (scriptEntry.contains("expected_refusals") &&
                                                 std::find(scriptEntry["expected_refusals"].begin(),
                                                           scriptEntry["expected_refusals"].end(),
                                                           statementId) != scriptEntry["expected_refusals"].end());
                    const std::string group = classify(sql, statementId, expectedRefusals);
                    const int64_t statementStarted = nowNs();
                    ++executedStatements;
                    if (executedStatements == 1 || executedStatements % 500 == 0) {
                        recordProcessMetrics("statement", statementId, executedStatements);
                    }
                    counts[group]++;
                    appendJsonl(paths.at("events"), {{"run_id", runId},
                                                     {"driver_name", "cpp"},
                                                     {"suite_id", manifest.value("suite_id", "")},
                                                     {"script_id", scriptId},
                                                     {"script", relativePath.string()},
                                                     {"statement_index", statementIndex + 1},
                                                     {"statement_id", statementId},
                                                     {"command_group", group},
                                                     {"sql_hash", sha256Text(sql)},
                                                     {"actual_outcome", "started"},
                                                     {"server_revalidation_state", "required"},
                                                     {"mga_authority", "engine"}});
                    appendJsonl(paths.at("wire"), {{"event", "statement_start"},
                                                   {"statement_id", statementId},
                                                   {"script_id", scriptId},
                                                   {"route", route},
                                                   {"parser_mode", parserMode}});

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

                    scratchbird::core::ErrorContext statementCtx;
                    const std::string first = firstTokenLower(sql);
                    const std::string second = secondTokenLower(sql);
                    if (group == "transaction") {
                        if (first == "begin" || (first == "start" && second == "transaction")) {
                            status = conn.beginTransaction(&statementCtx);
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
                        if (statementReturnsRows(sql)) {
                            appendJsonl(paths.at("wire"), {{"event", "execute_query_start"},
                                                           {"statement_id", statementId},
                                                           {"prepared_cache_eligible",
                                                            preparedCacheEnabled &&
                                                                statementPreparedCacheEligible(sql) &&
                                                                !expectsRefusal}});
                            scratchbird::client::ResultSet resultSet;
                            if (preparedCacheEnabled &&
                                statementPreparedCacheEligible(sql) &&
                                !expectsRefusal) {
                                status = executePreparedCached(sql,
                                                               statementId,
                                                               &resultSet,
                                                               &statementCtx);
                            } else {
                                status = conn.executeQuery(sql, &resultSet, &statementCtx);
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
                                                           {"statement_id", statementId}});
                            status = conn.execute(sql, &rowsAffected, &statementCtx);
                            api["execute"]++;
                            commandTag = "COMMAND";
                            resultDigest = sha256Text(commandTag + ":" + std::to_string(rowsAffected));
                        }
                    }

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
                                                        {"script_id", scriptId},
                                                        {"sqlstate", sqlstate},
                                                        {"message", diagnostic},
                                                        {"passed", statementPassed}});
                        }
                    }

                    if (status == scratchbird::core::Status::OK) {
                        const json assertionChecks = validateAssertions(statementId, rows);
                        for (const auto& assertion : assertionChecks) {
                            assertionResults.push_back(assertion);
                            if (assertion.value("passed", false)) {
                                ++assertionPasses;
                            } else {
                                ++assertionFailures;
                                statementPassed = false;
                                failureRecordedForStatement = true;
                                failures.push_back({{"statement_id", statementId},
                                                    {"assertion_id", assertion.value("assertion_id", "")},
                                                    {"message", "assertion mismatch"},
                                                    {"comparisons", assertion.value("comparisons", json::array())}});
                            }
                        }
                    }

                    if (!statementPassed && !failureRecordedForStatement) {
                        failures.push_back(makeFailure(statementId, diagnostic.empty() ? "statement failed" : diagnostic));
                    }

                    addTiming(&timings, group, statementStarted);
                    const json event{{"run_id", runId},
                                     {"driver_name", "cpp"},
                                     {"driver_version", "unknown"},
                                     {"suite_id", manifest.value("suite_id", "")},
                                     {"script_id", scriptId},
                                     {"script", relativePath.string()},
                                     {"statement_index", statementIndex + 1},
                                     {"statement_id", statementId},
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
                                     {"elapsed_ns", nowNs() - statementStarted},
                                     {"server_revalidation_state", "required"},
                                     {"transaction_id_observed", conn.currentTransactionId()},
                                     {"mga_authority", "engine"},
                                     {"native_api_surface", "cpp"},
                                     {"code_example_section", "connect_prepare_execute_fetch_assert"}};
                    appendJsonl(paths.at("events"), event);
                    commandEvents.push_back(event);
                    digests.push_back({{"statement_id", statementId},
                                       {"script_id", scriptId},
                                       {"row_count", rowCount},
                                       {"result_digest", resultDigest}});
                    if (rows.size() > 0 || columns.size() > 0) {
                        appendJsonl(artifactRoot / "stdout.log",
                                    {{"statement_id", statementId}, {"columns", columns}, {"rows", rows}});
                    }
                    if (!statementPassed) {
                        appendJsonl(paths.at("diagnostics"), {{"statement_id", statementId},
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
        }

        conn.disconnect();
        recordProcessMetrics("post_disconnect", "disconnect", executedStatements);
        timings["overall"] = nowNs() - runStarted;

        for (const auto& [role, _] : monitoredProcesses) {
            processMetricSummary[role] = {{"max_rss_kb", processMaxRssKb[role]},
                                          {"max_vsize_kb", processMaxVsizeKb[role]},
                                          {"last_rss_kb", processLastRssKb[role]},
                                          {"last_vsize_kb", processLastVsizeKb[role]}};
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
                           {"driver_or_parser_finality", "forbidden"},
                           {"mga_authority", "engine"},
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
                           {"manifest", manifestPath.string()}};
        writeText(paths.at("summary"), summary.dump(2) + "\n");
        writeJunit(paths.at("junit"), "SBRegressCppFullSurface", executedStatements, failures, timings["overall"]);
        appendText(paths.at("stdout"),
                   std::string("SBRegressCpp status=") + (failures.empty() ? "pass" : "fail") + "\n");
        return failures.empty() ? 0 : 1;
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << "\n";
        return 1;
    }
}
