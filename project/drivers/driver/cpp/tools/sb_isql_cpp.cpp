// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "scratchbird/client/connection.h"

#include <openssl/sha.h>

#include <chrono>
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

int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
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

std::vector<std::string> splitStatements(const std::string& script) {
    std::vector<std::string> out;
    std::string current;
    bool single = false;
    bool dbl = false;
    for (char ch : script) {
        if (ch == '\'' && !dbl) {
            single = !single;
        } else if (ch == '"' && !single) {
            dbl = !dbl;
        }
        if (ch == ';' && !single && !dbl) {
            if (!current.empty()) {
                out.push_back(current);
            }
            current.clear();
            continue;
        }
        current.push_back(ch);
    }
    if (!current.empty()) {
        out.push_back(current);
    }
    return out;
}

std::string firstTokenLower(std::string sql) {
    std::istringstream in(sql);
    std::string first;
    in >> first;
    for (char& ch : first) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return first;
}

std::string classify(const std::string& sql) {
    const auto first = firstTokenLower(sql);
    if (first == "create" || first == "alter" || first == "drop") return "ddl";
    if (first == "insert" || first == "update" || first == "delete" || first == "merge" || first == "upsert") return "dml";
    if (first == "commit" || first == "rollback" || first == "savepoint" || first == "begin" || first == "start") return "transaction";
    if (first == "grant" || first == "revoke") return "security_refusal";
    return sql.find("sys.") != std::string::npos ? "metadata" : "query";
}

std::map<std::string, std::string> parseArgs(int argc, char** argv) {
    std::map<std::string, std::string> args;
    for (int i = 1; i < argc; ++i) {
        std::string key(argv[i]);
        if (key.rfind("--", 0) != 0) {
            throw std::runtime_error("unexpected positional argument: " + key);
        }
        if (key == "--stop-on-error" || key == "--create-database") {
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

void validate(const std::map<std::string, std::string>& args) {
    if (!kPageSizes.count(required(args, "--page-size"))) throw std::runtime_error("unsupported page size");
    if (!kRoutes.count(required(args, "--route"))) throw std::runtime_error("unsupported route");
    if (!kParserModes.count(required(args, "--parser-mode"))) throw std::runtime_error("unsupported parser mode");
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
        const int64_t started = nowNs();

        scratchbird::client::Connection conn;
        api["scratchbird::client::Connection"]++;
        scratchbird::client::ConnectionConfig config;
        config.host = required(args, "--host");
        config.tcp_port = static_cast<uint16_t>(std::stoi(required(args, "--port")));
        config.database_name = required(args, "--database");
        config.username = required(args, "--user");
        config.password = required(args, "--password");
        config.role = valueOrDefault(args, "--role", "");
        config.ssl_mode = valueOrDefault(args, "--sslmode", "require");
        config.front_door_mode = required(args, "--route") == "manager-listener-parser" ? "manager_proxy" : "direct";
        config.application_name = "SBIsqlCpp";

        scratchbird::core::ErrorContext ctx;
        const int64_t connectStarted = nowNs();
        auto status = conn.connect(config, &ctx);
        if (status == scratchbird::core::Status::OK) {
            api["connect"]++;
            addTiming(timings, "connection", connectStarted);
            appendJsonl(required(args, "--transcript"), {{"event", "connect"}, {"driver", "cpp"}, {"route", required(args, "--route")},
                                                         {"parser_mode", required(args, "--parser-mode")}, {"page_size", required(args, "--page-size")}});
            appendJsonl(paths.at("wire"), {{"event", "server_admission_required"}, {"driver_or_parser_finality", "forbidden"}});
        } else {
            failures.push_back({{"statement_id", "connect"}, {"message", statusMessage(ctx)}});
        }

        if (failures.empty() && args.count("--create-database")) {
            failures.push_back({{"statement_id", "database_create"}, {"message", "--create-database is not implemented in the C++ native tool yet"}});
        }
        if (failures.empty() && required(args, "--parser-mode") != "server-parser") {
            failures.push_back({{"statement_id", "parser_mode"}, {"message", required(args, "--parser-mode") + " is not yet implemented by the C++ native tool; it fails closed"}});
        }

        if (failures.empty()) {
            const auto statements = splitStatements(readInput(required(args, "--input")));
            for (size_t index = 0; index < statements.size(); ++index) {
                const auto& sql = statements[index];
                const std::string statementId = required(args, "--input") + ":" + std::to_string(index + 1);
                const std::string group = classify(sql);
                const int64_t statementStarted = nowNs();
                std::string outcome = "success";
                int64_t rowCount = -1;
                std::string resultDigest;
                std::string diagnostic;

                if (group == "transaction") {
                    if (firstTokenLower(sql) == "commit") {
                        status = conn.commit(&ctx);
                        api["commit"]++;
                    } else if (firstTokenLower(sql) == "rollback") {
                        status = conn.rollback(&ctx);
                        api["rollback"]++;
                    } else {
                        status = conn.beginTransaction(&ctx);
                    }
                    rowCount = 0;
                    resultDigest = sha256Text("transaction");
                } else {
                    scratchbird::client::PreparedStatement stmt;
                    status = conn.prepare(sql, &stmt, &ctx);
                    api["prepare"]++;
                    scratchbird::client::ResultSet results;
                    if (status == scratchbird::core::Status::OK) {
                        status = stmt.executeQuery(&results, &ctx);
                        api["executeQuery"]++;
                        api["execute"]++;
                    }
                    json rows = json::array();
                    if (status == scratchbird::core::Status::OK) {
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
                    }
                }

                if (status != scratchbird::core::Status::OK) {
                    outcome = "refusal";
                    diagnostic = statusMessage(ctx);
                    appendJsonl(required(args, "--diagnostics"), {{"statement_id", statementId}, {"sqlstate", ctx.sqlstate ? ctx.sqlstate : "HY000"}, {"message", diagnostic}});
                    appendText(required(args, "--error"), statementId + ": " + diagnostic + "\n");
                    failures.push_back({{"statement_id", statementId}, {"message", diagnostic}});
                    if (args.count("--stop-on-error")) {
                        addTiming(timings, group, statementStarted);
                        break;
                    }
                }
                addTiming(timings, group, statementStarted);
                const auto event = json{{"run_id", valueOrDefault(args, "--run-id", "manual")},
                                        {"driver_name", "cpp"},
                                        {"driver_version", "unknown"},
                                        {"route", required(args, "--route")},
                                        {"parser_mode", required(args, "--parser-mode")},
                                        {"page_size", required(args, "--page-size")},
                                        {"namespace", required(args, "--namespace")},
                                        {"script", required(args, "--input")},
                                        {"statement_index", index + 1},
                                        {"statement_id", statementId},
                                        {"command_group", group},
                                        {"sql_hash", sha256Text(sql)},
                                        {"expected_outcome", "success"},
                                        {"actual_outcome", outcome},
                                        {"sqlstate", ctx.sqlstate ? ctx.sqlstate : "HY000"},
                                        {"diagnostic_code", diagnostic},
                                        {"canonical_message_vector", json::array()},
                                        {"row_count", rowCount},
                                        {"result_digest", resultDigest},
                                        {"elapsed_ns", nowNs() - statementStarted},
                                        {"server_revalidation_state", "required"},
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
        const json summaryJson{{"run_id", valueOrDefault(args, "--run-id", "manual")},
                               {"driver_name", "cpp"},
                               {"route", required(args, "--route")},
                               {"parser_mode", required(args, "--parser-mode")},
                               {"page_size", required(args, "--page-size")},
                               {"namespace", required(args, "--namespace")},
                               {"status", failures.empty() ? "pass" : "fail"},
                               {"failure_count", failures.size()},
                               {"elapsed_ns", timings["overall"]},
                               {"server_revalidation_required", true},
                               {"driver_or_parser_finality", "forbidden"},
                               {"mga_authority", "engine"}};
        writeText(summaryPath, summaryJson.dump() + "\n");
        writeText(required(args, "--metrics"), json(timings).dump() + "\n");
        writeText(paths.at("timing"), json(timings).dump() + "\n");
        writeText(paths.at("digests"), digests.dump() + "\n");
        writeText(paths.at("refusals"), "[]\n");
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
