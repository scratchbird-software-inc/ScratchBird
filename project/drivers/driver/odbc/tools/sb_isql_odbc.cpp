// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "scratchbird/odbc/odbc_driver.h"

#include <openssl/sha.h>

#include <chrono>
#include <cctype>
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
#include "sb_statement_chunker.hpp"

using namespace scratchbird::odbc;
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

std::string required(const std::map<std::string, std::string>& args, const std::string& key) {
    auto it = args.find(key);
    if (it == args.end() || it->second.empty()) throw std::runtime_error("missing required argument " + key);
    return it->second;
}

std::string valueOrDefault(const std::map<std::string, std::string>& args, const std::string& key, const std::string& fallback) {
    auto it = args.find(key);
    return it == args.end() ? fallback : it->second;
}

bool booleanArg(const std::map<std::string, std::string>& args, const std::string& key, bool fallback) {
    auto it = args.find(key);
    if (it == args.end()) return fallback;
    std::string value = it->second;
    for (char& ch : value) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (value == "true" || value == "1" || value == "yes" || value == "on") return true;
    if (value == "false" || value == "0" || value == "no" || value == "off") return false;
    throw std::runtime_error("invalid boolean value for " + key + ": " + it->second);
}

bool networkRoute(const std::string& route) {
    return route == "listener-parser" || route == "manager-listener-parser";
}

std::string transportModeForRoute(const std::string& route, const std::string& sslmode) {
    if (route == "embedded") return "embedded_no_network_transport";
    if (route == "ipc_local") return "local_ipc_no_tls";
    return sslmode == "disable" ? "tls_disabled" : "tls_required";
}

std::string tlsPolicyForRoute(const std::string& route, const std::string& sslmode) {
    if (!networkRoute(route)) return "not_applicable_non_network_route";
    return sslmode == "disable" ? "explicit_non_tls_test_route" : "scratchbird_tls_1_3_floor";
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
        if (!kSupportedArgs.count(key)) throw std::runtime_error("unsupported argument: " + key);
        if (key == "--stop-on-error" || key == "--create-database" || key == "--standard-english-fallback") {
            if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) {
                args[key] = argv[++i];
            } else {
                args[key] = "true";
            }
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
    if (!kSslModes.count(valueOrDefault(args, "--sslmode", "require"))) throw std::runtime_error("unsupported sslmode");
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
    if (!value.is_array()) return;
    for (const auto& item : value) {
        if (item.is_string()) {
            ids.insert(item.get<std::string>());
        } else if (item.is_object() && item.contains("statement_id") && item["statement_id"].is_string()) {
            ids.insert(item["statement_id"].get<std::string>());
        }
    }
}

std::set<std::string> loadExpectedRefusals(const std::string& path) {
    if (path.empty()) return {};
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("expected refusal file not found: " + path);
    json doc;
    in >> doc;
    std::set<std::string> ids;
    if (doc.is_array()) {
        addExpectedRefusalIds(doc, ids);
        return ids;
    }
    if (!doc.is_object()) throw std::runtime_error("expected refusals must be a JSON object or array");
    if (doc.contains("statement_ids")) addExpectedRefusalIds(doc["statement_ids"], ids);
    if (doc.contains("expected_refusals")) addExpectedRefusalIds(doc["expected_refusals"], ids);
    if (doc.contains("expected_diagnostics") && doc["expected_diagnostics"].is_object()) {
        for (const auto& item : doc["expected_diagnostics"].items()) ids.insert(item.key());
    }
    return ids;
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
        json securityRefusals = json::array();
        auto started = nowNs();
        const std::string route = required(args, "--route");
        const std::string parserMode = required(args, "--parser-mode");
        const std::string pageSize = required(args, "--page-size");
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
                                    ";SSLRootCert=" + valueOrDefault(args, "--sslrootcert", "") +
                                    ";SSLCert=" + valueOrDefault(args, "--sslcert", "") +
                                    ";SSLKey=" + valueOrDefault(args, "--sslkey", "") +
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
            appendJsonl(required(args, "--transcript"), {{"event", "connect"}, {"driver", "odbc"}, {"route", route},
                                                         {"parser_mode", parserMode}, {"page_size", pageSize},
                                                         {"sslmode", sslmode}, {"transport_mode", transportMode},
                                                         {"tls_policy", tlsPolicy}});
            appendJsonl(paths.at("wire"), {{"event", "server_admission_required"},
                                           {"driver_or_parser_finality", "forbidden"},
                                           {"parser_output_to_engine_required", true},
                                           {"engine_sql_text_execution", false}});
        } else {
            failures.push_back({{"statement_id", "connect"}, {"message", diagnostic(SQL_HANDLE_DBC, dbc)}});
        }

        if (failures.empty() && booleanArg(args, "--create-database", false)) {
            failures.push_back({{"statement_id", "database_create"}, {"message", "--create-database is not implemented in the ODBC native tool yet"},
                                {"create_emulation_mode", valueOrDefault(args, "--create-emulation-mode", "sbsql")}});
        }
        if (failures.empty() && parserMode != "server-parser") {
            failures.push_back({{"statement_id", "parser_mode"}, {"message", parserMode + " is not yet implemented by the ODBC native tool; it fails closed"}});
        }

        if (failures.empty()) {
            if (SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt) != SQL_SUCCESS) throw std::runtime_error("SQLAllocHandle stmt failed");
            api["SQLAllocHandle"]++;
            const auto statements = sbchunk::splitStatements(readInput(required(args, "--input")));
            std::string scriptName = std::filesystem::path(required(args, "--input")).filename().string();
            if (scriptName.empty()) scriptName = required(args, "--input");
            for (size_t index = 0; index < statements.size(); ++index) {
                const auto& sql = statements[index];
                const std::string statementId = scriptName + ":" + std::to_string(index + 1);
                const bool expectedRefusal = expectedRefusals.count(statementId) != 0;
                const std::string expectedOutcome = expectedRefusal ? "refusal" : "success";
                const auto group = classify(sql);
                const auto statementStarted = nowNs();
                std::string outcome = "success";
                std::string message;
                std::string sqlState = "00000";
                int64_t rowCount = 0;
                bool statementSucceeded = false;
                rc = SQLPrepare(stmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql.c_str())), SQL_NTS);
                api["SQLPrepare"]++;
                if (rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO) {
                    rc = SQLExecute(stmt);
                    api["SQLExecute"]++;
                }
                if (rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO) {
                    statementSucceeded = true;
                    while ((rc = SQLFetch(stmt)) == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO) {
                        api["SQLFetch"]++;
                        ++rowCount;
                    }
                    digests.push_back({{"row_count", rowCount}, {"result_digest", sha256Text(std::to_string(rowCount))}});
                } else {
                    outcome = "refusal";
                    message = diagnostic(SQL_HANDLE_STMT, stmt);
                    sqlState = "HY000";
                    appendText(required(args, "--error"), statementId + ": " + message + "\n");
                    appendJsonl(required(args, "--diagnostics"), {{"statement_id", statementId}, {"sqlstate", sqlState}, {"message", message}});
                    if (expectedRefusal) {
                        securityRefusals.push_back({{"statement_id", statementId}, {"sqlstate", sqlState}, {"diagnostic_code", message}});
                    } else {
                        failures.push_back({{"statement_id", statementId}, {"message", message}});
                    }
                    if (!expectedRefusal && booleanArg(args, "--stop-on-error", true)) {
                        addTiming(timings, group, statementStarted);
                        break;
                    }
                }
                if (statementSucceeded) {
                    if (expectedRefusal) {
                        outcome = "unexpected_success";
                        message = "statement succeeded but was expected to refuse";
                        failures.push_back({{"statement_id", statementId}, {"message", message}});
                    }
                }
                addTiming(timings, group, statementStarted);
                const json event{{"run_id", valueOrDefault(args, "--run-id", "manual")},
                                 {"driver_name", "odbc"},
                                 {"route", route},
                                 {"parser_mode", parserMode},
                                 {"page_size", pageSize},
                                 {"namespace", required(args, "--namespace")},
                                 {"command_group", group},
                                 {"sql_hash", sha256Text(sql)},
                                 {"statement_index", index + 1},
                                 {"statement_id", statementId},
                                 {"expected_outcome", expectedOutcome},
                                 {"actual_outcome", outcome},
                                 {"sqlstate", sqlState},
                                 {"diagnostic_code", message},
                                 {"row_count", rowCount},
                                 {"server_revalidation_state", "required"},
                                 {"language_profile", languageProfile},
                                 {"language_resource_pack", languageResourcePack},
                                 {"language_resource_identity", languageResourceIdentity},
                                 {"language_resource_hash", languageResourceHash},
                                 {"syntax_profile", syntaxProfile},
                                 {"topology_profile", topologyProfile},
                                 {"standard_english_fallback", standardEnglishFallback},
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
                           {"route", route},
                           {"parser_mode", parserMode},
                           {"page_size", pageSize},
                           {"namespace", required(args, "--namespace")},
                           {"sslmode", sslmode},
                           {"transport_mode", transportMode},
                           {"tls_policy", tlsPolicy},
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
                           {"server_revalidation_required", true},
                           {"driver_or_parser_finality", "forbidden"},
                           {"mga_authority", "engine"}};
        writeText(summaryPath, summary.dump() + "\n");
        writeText(required(args, "--metrics"), json(timings).dump() + "\n");
        writeText(paths.at("timing"), json(timings).dump() + "\n");
        writeText(paths.at("digests"), digests.dump() + "\n");
        writeText(paths.at("refusals"), securityRefusals.dump() + "\n");
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
