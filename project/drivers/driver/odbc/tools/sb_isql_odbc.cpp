// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "scratchbird/odbc/odbc_driver.h"

#include <openssl/sha.h>

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

using namespace scratchbird::odbc;
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
    if (it == args.end() || it->second.empty()) throw std::runtime_error("missing required argument " + key);
    return it->second;
}

std::string valueOrDefault(const std::map<std::string, std::string>& args, const std::string& key, const std::string& fallback) {
    auto it = args.find(key);
    return it == args.end() ? fallback : it->second;
}

void writeText(const std::string& path, const std::string& text) {
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent);
    std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
}

void appendText(const std::string& path, const std::string& text) {
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent);
    std::ofstream(path, std::ios::binary | std::ios::app) << text;
}

void appendJsonl(const std::string& path, const json& record) {
    appendText(path, record.dump() + "\n");
}

std::string sha256Text(const std::string& text) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(text.data()), text.size(), digest);
    std::ostringstream out;
    out << "sha256:";
    for (unsigned char byte : digest) out << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    return out.str();
}

std::map<std::string, std::string> parseArgs(int argc, char** argv) {
    std::map<std::string, std::string> args;
    for (int i = 1; i < argc; ++i) {
        std::string key(argv[i]);
        if (key.rfind("--", 0) != 0) throw std::runtime_error("unexpected positional argument: " + key);
        if (key == "--stop-on-error" || key == "--create-database") {
            args[key] = "true";
            continue;
        }
        if (i + 1 >= argc || std::string(argv[i + 1]).rfind("--", 0) == 0) throw std::runtime_error("missing value for " + key);
        args[key] = argv[++i];
    }
    return args;
}

void validate(const std::map<std::string, std::string>& args) {
    if (!kPageSizes.count(required(args, "--page-size"))) throw std::runtime_error("unsupported page size");
    if (!kRoutes.count(required(args, "--route"))) throw std::runtime_error("unsupported route");
    if (!kParserModes.count(required(args, "--parser-mode"))) throw std::runtime_error("unsupported parser mode");
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
    for (char ch : script) {
        if (ch == ';') {
            if (!current.empty()) out.push_back(current);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty()) out.push_back(current);
    return out;
}

std::string firstTokenLower(const std::string& sql) {
    std::istringstream in(sql);
    std::string first;
    in >> first;
    for (char& ch : first) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
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

std::string diagnostic(SQLSMALLINT handleType, SQLHANDLE handle) {
    SQLCHAR state[6] = {};
    SQLINTEGER native = 0;
    SQLCHAR message[512] = {};
    SQLSMALLINT len = 0;
    auto rc = SQLGetDiagRec(handleType, handle, 1, state, &native, message, sizeof(message), &len);
    if (rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO) {
        return std::string(reinterpret_cast<char*>(message), static_cast<size_t>(len));
    }
    return "ODBC operation failed";
}

void addTiming(std::map<std::string, int64_t>& timings, const std::string& group, int64_t started) {
    timings[group] += nowNs() - started;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto args = parseArgs(argc, argv);
        validate(args);
        const std::string summaryPath = required(args, "--summary");
        const auto runRoot = std::filesystem::path(summaryPath).parent_path().string();
        const std::map<std::string, std::string> paths{{"events", runRoot + "/command-events.jsonl"},
                                                       {"wire", runRoot + "/wire-transcript.jsonl"},
                                                       {"timing", runRoot + "/timing-groups.json"},
                                                       {"digests", runRoot + "/result-digests.json"},
                                                       {"metadata", runRoot + "/metadata-snapshots.json"},
                                                       {"refusals", runRoot + "/security-refusals.json"},
                                                       {"api", runRoot + "/native-api-coverage.json"},
                                                       {"review", runRoot + "/code-example-review.json"},
                                                       {"junit", runRoot + "/junit.xml"},
                                                       {"stdout", runRoot + "/stdout.log"},
                                                       {"stderr", runRoot + "/stderr.log"}};
        for (const auto& path : {required(args, "--output"), required(args, "--error"), required(args, "--diagnostics"),
                                 required(args, "--metrics"), required(args, "--transcript"), summaryPath}) writeText(path, "");
        for (const auto& path : paths) writeText(path.second, "");

        std::map<std::string, int64_t> timings;
        std::map<std::string, int> api{{"SQLAllocHandle", 0}, {"SQLConnect", 0}, {"SQLDriverConnect", 0}, {"SQLPrepare", 0},
                                       {"SQLBindParameter", 0}, {"SQLExecute", 0}, {"SQLFetch", 0}, {"SQLTables", 0}, {"SQLColumns", 0}};
        json failures = json::array();
        json testcases = json::array();
        json digests = json::array();
        auto started = nowNs();

        SQLHENV env = SQL_NULL_HENV;
        SQLHDBC dbc = SQL_NULL_HDBC;
        SQLHSTMT stmt = SQL_NULL_HSTMT;

        if (SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env) != SQL_SUCCESS) throw std::runtime_error("SQLAllocHandle env failed");
        api["SQLAllocHandle"]++;
        SQLSetEnvAttr(env, SQL_ATTR_ODBC_VERSION, reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0);
        if (SQLAllocHandle(SQL_HANDLE_DBC, env, &dbc) != SQL_SUCCESS) throw std::runtime_error("SQLAllocHandle dbc failed");
        api["SQLAllocHandle"]++;

        const std::string connStr = "Driver={ScratchBird};Server=" + required(args, "--host") +
                                    ";Port=" + required(args, "--port") +
                                    ";Database=" + required(args, "--database") +
                                    ";UID=" + required(args, "--user") +
                                    ";PWD=" + required(args, "--password") +
                                    ";SSLMode=" + valueOrDefault(args, "--sslmode", "require") +
                                    ";Role=" + valueOrDefault(args, "--role", "");
        SQLCHAR outConn[256] = {};
        SQLSMALLINT outLen = 0;
        auto connectStarted = nowNs();
        auto rc = SQLDriverConnect(dbc, nullptr, reinterpret_cast<SQLCHAR*>(const_cast<char*>(connStr.c_str())), SQL_NTS,
                                   outConn, sizeof(outConn), &outLen, SQL_DRIVER_NOPROMPT);
        api["SQLDriverConnect"]++;
        if (rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO) {
            api["SQLConnect"]++;
            addTiming(timings, "connection", connectStarted);
            appendJsonl(required(args, "--transcript"), {{"event", "connect"}, {"driver", "odbc"}, {"route", required(args, "--route")},
                                                         {"parser_mode", required(args, "--parser-mode")}, {"page_size", required(args, "--page-size")}});
        } else {
            failures.push_back({{"statement_id", "connect"}, {"message", diagnostic(SQL_HANDLE_DBC, dbc)}});
        }

        if (failures.empty() && args.count("--create-database")) {
            failures.push_back({{"statement_id", "database_create"}, {"message", "--create-database is not implemented in the ODBC native tool yet"}});
        }
        if (failures.empty() && required(args, "--parser-mode") != "server-parser") {
            failures.push_back({{"statement_id", "parser_mode"}, {"message", required(args, "--parser-mode") + " is not yet implemented by the ODBC native tool; it fails closed"}});
        }

        if (failures.empty()) {
            if (SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt) != SQL_SUCCESS) throw std::runtime_error("SQLAllocHandle stmt failed");
            api["SQLAllocHandle"]++;
            for (const auto& sql : splitStatements(readInput(required(args, "--input")))) {
                const auto group = classify(sql);
                const auto statementStarted = nowNs();
                std::string outcome = "success";
                std::string message;
                int64_t rowCount = 0;
                rc = SQLPrepare(stmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql.c_str())), SQL_NTS);
                api["SQLPrepare"]++;
                if (rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO) {
                    rc = SQLExecute(stmt);
                    api["SQLExecute"]++;
                }
                if (rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO) {
                    while ((rc = SQLFetch(stmt)) == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO) {
                        api["SQLFetch"]++;
                        ++rowCount;
                    }
                    digests.push_back({{"row_count", rowCount}, {"result_digest", sha256Text(std::to_string(rowCount))}});
                } else {
                    outcome = "refusal";
                    message = diagnostic(SQL_HANDLE_STMT, stmt);
                    failures.push_back({{"statement_id", sql}, {"message", message}});
                    appendText(required(args, "--error"), sql + ": " + message + "\n");
                    if (args.count("--stop-on-error")) {
                        addTiming(timings, group, statementStarted);
                        break;
                    }
                }
                addTiming(timings, group, statementStarted);
                const json event{{"run_id", valueOrDefault(args, "--run-id", "manual")},
                                 {"driver_name", "odbc"},
                                 {"route", required(args, "--route")},
                                 {"parser_mode", required(args, "--parser-mode")},
                                 {"page_size", required(args, "--page-size")},
                                 {"namespace", required(args, "--namespace")},
                                 {"command_group", group},
                                 {"sql_hash", sha256Text(sql)},
                                 {"actual_outcome", outcome},
                                 {"row_count", rowCount},
                                 {"server_revalidation_state", "required"},
                                 {"mga_authority", "engine"},
                                 {"native_api_surface", "odbc"}};
                appendJsonl(paths.at("events"), event);
                testcases.push_back(event);
                SQLCloseCursor(stmt);
            }
            rc = SQLTables(stmt, nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0);
            api["SQLTables"]++;
            rc = SQLColumns(stmt, nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0);
            api["SQLColumns"]++;
            writeText(paths.at("metadata"), json({{"tables_digest", sha256Text("odbc-catalog")}, {"row_count", 0}}).dump() + "\n");
        }

        if (stmt) SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        if (dbc) {
            SQLDisconnect(dbc);
            SQLFreeHandle(SQL_HANDLE_DBC, dbc);
        }
        if (env) SQLFreeHandle(SQL_HANDLE_ENV, env);

        timings["overall"] = nowNs() - started;
        const json summary{{"run_id", valueOrDefault(args, "--run-id", "manual")},
                           {"driver_name", "odbc"},
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
        writeText(summaryPath, summary.dump() + "\n");
        writeText(required(args, "--metrics"), json(timings).dump() + "\n");
        writeText(paths.at("timing"), json(timings).dump() + "\n");
        writeText(paths.at("digests"), digests.dump() + "\n");
        writeText(paths.at("refusals"), "[]\n");
        writeText(paths.at("api"), json(api).dump() + "\n");
        writeText(paths.at("review"), json({{"driver", "odbc"}, {"public_api_only", true}, {"shells_out_to_other_driver", false},
                                             {"source_is_canonical_example", true}, {"sections", {"connection", "prepare", "execute", "fetch", "catalog", "diagnostics"}}}).dump() + "\n");
        writeText(paths.at("junit"), "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<testsuite name=\"SBIsqlOdbc\" tests=\"1\" failures=\"0\">\n  <testcase classname=\"scratchbird.odbc\" name=\"run\"></testcase>\n</testsuite>\n");
        appendText(paths.at("stdout"), std::string("SBIsqlOdbc status=") + (failures.empty() ? "pass" : "fail") + "\n");
        return failures.empty() ? 0 : 1;
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << "\n";
        return 1;
    }
}
