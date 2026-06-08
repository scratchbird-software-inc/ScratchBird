// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "res_lifecycle_parity.h"

#include <cctype>
#include <set>
#include <string>

namespace scratchbird::cli::parity {
namespace {

std::string trimCopy(const std::string& value) {
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }
    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return value.substr(begin, end - begin);
}

std::string bestError(const ResourceLifecycleClient& client, const core::ErrorContext* ctx) {
    if (ctx != nullptr && !ctx->message.empty()) {
        return ctx->message;
    }
    std::string last_error = client.lastError();
    if (!last_error.empty()) {
        return last_error;
    }
    return "Operation failed";
}

void addError(nlohmann::json& result, bool* had_error, const std::string& message) {
    result["status"] = "error";
    result["errors"].push_back(message);
    if (had_error != nullptr) {
        *had_error = true;
    }
}

bool readOptionalInt64(const nlohmann::json& test,
                       const char* key,
                       int64_t* out,
                       bool* has_value,
                       std::string* error) {
    if (has_value != nullptr) {
        *has_value = false;
    }
    auto it = test.find(key);
    if (it == test.end()) {
        return true;
    }
    if (!it->is_number_integer()) {
        if (error != nullptr) {
            *error = std::string("Field '") + key + "' must be an integer";
        }
        return false;
    }
    if (out != nullptr) {
        *out = it->get<int64_t>();
    }
    if (has_value != nullptr) {
        *has_value = true;
    }
    return true;
}

}  // namespace

const std::vector<AdminLifecycleRoute>& adminLifecycleRoutes() {
    static const std::vector<AdminLifecycleRoute> routes = {
        {"health", "health_database", "SHOW SERVER LIFECYCLE", "OBS_MANAGEMENT_INSPECT", false, false},
        {"status", "status_database", "SHOW SERVER LIFECYCLE", "OBS_MANAGEMENT_INSPECT", false, false},
        {"create", "create_database", "ADMIN LIFECYCLE CREATE DATABASE", "OBS_MANAGEMENT_CONTROL", true, true},
        {"open", "open_database", "ADMIN LIFECYCLE OPEN DATABASE", "OBS_MANAGEMENT_CONTROL", true, false},
        {"attach", "attach_database", "ADMIN LIFECYCLE ATTACH DATABASE", "OBS_MANAGEMENT_CONTROL", true, false},
        {"detach", "detach_database", "ADMIN LIFECYCLE DETACH DATABASE", "OBS_MANAGEMENT_CONTROL", true, false},
        {"inspect", "inspect_database", "INSPECT DATABASE", "OBS_MANAGEMENT_CONTROL", false, false},
        {"verify", "verify_database", "VERIFY DATABASE", "OBS_MANAGEMENT_CONTROL", true, false},
        {"repair", "repair_database", "ADMIN LIFECYCLE REPAIR DATABASE", "OBS_MANAGEMENT_CONTROL", true, true},
        {"shutdown", "shutdown_database", "SHUTDOWN DATABASE", "OBS_MANAGEMENT_CONTROL", true, true},
        {"shutdown-force", "shutdown_database_force", "SHUTDOWN DATABASE FORCE", "OBS_MANAGEMENT_CONTROL", true, true},
        {"drop", "drop_database", "DROP DATABASE", "OBS_MANAGEMENT_CONTROL", true, true},
    };
    return routes;
}

bool hasCompleteAdminLifecycleRouteCoverage(std::vector<std::string>* missing_operations) {
    static const char* required[] = {
        "health", "status", "create", "open", "attach", "detach", "inspect", "verify",
        "repair", "shutdown", "shutdown-force", "drop"};
    std::set<std::string> present;
    for (const auto& route : adminLifecycleRoutes()) {
        if (!route.operation.empty() &&
            !route.management_operation_key.empty() &&
            !route.client_statement.empty() &&
            !route.required_right.empty()) {
            present.insert(route.operation);
        }
    }
    bool complete = true;
    for (const char* operation : required) {
        if (present.count(operation) != 0) continue;
        complete = false;
        if (missing_operations != nullptr) {
            missing_operations->push_back(operation);
        }
    }
    return complete;
}

void runResourceLifecycleLoopCase(ResourceLifecycleClient& client,
                                  const nlohmann::json& test,
                                  nlohmann::json& result,
                                  bool* had_error,
                                  core::ErrorContext* ctx) {
    const std::string sql = trimCopy(test.value("sql", ""));
    if (sql.empty()) {
        addError(result, had_error, "res_loop_exec requires sql");
        return;
    }

    int64_t loop_iterations = 1;
    int64_t expect_total_rows_affected = 0;
    int64_t expect_total_rows = 0;
    bool has_expect_total_rows_affected = false;
    bool has_expect_total_rows = false;
    std::string parse_error;

    if (!readOptionalInt64(test, "loop_iterations", &loop_iterations, nullptr, &parse_error)) {
        addError(result, had_error, parse_error);
        return;
    }
    if (!readOptionalInt64(test, "expect_total_rows_affected", &expect_total_rows_affected,
                           &has_expect_total_rows_affected, &parse_error)) {
        addError(result, had_error, parse_error);
        return;
    }
    if (!readOptionalInt64(test, "expect_total_rows", &expect_total_rows,
                           &has_expect_total_rows, &parse_error)) {
        addError(result, had_error, parse_error);
        return;
    }

    if (loop_iterations <= 0) {
        addError(result, had_error, "loop_iterations must be greater than zero");
        return;
    }

    int64_t total_rows_affected = 0;
    int64_t total_rows = 0;
    for (int64_t iteration = 1; iteration <= loop_iterations; ++iteration) {
        bool connected = false;
        core::Status status = client.connect(ctx);
        if (status != core::Status::OK) {
            addError(result,
                     had_error,
                     "connect failed at iteration " + std::to_string(iteration) + ": " +
                         bestError(client, ctx));
            return;
        }
        connected = true;

        LifecycleObservation observation;
        status = client.executeStatement(sql, &observation, ctx);
        if (status != core::Status::OK) {
            if (connected) {
                client.disconnect();
            }
            addError(result,
                     had_error,
                     "execute failed at iteration " + std::to_string(iteration) + ": " +
                         bestError(client, ctx));
            return;
        }

        total_rows_affected += observation.rows_affected;
        total_rows += observation.rows_returned;

        if (connected) {
            client.disconnect();
        }
    }

    result["loop_iterations"] = loop_iterations;
    result["total_rows_affected"] = total_rows_affected;
    result["total_rows"] = total_rows;

    if (has_expect_total_rows_affected &&
        total_rows_affected != expect_total_rows_affected) {
        addError(result,
                 had_error,
                 "total_rows_affected mismatch (expected " +
                     std::to_string(expect_total_rows_affected) +
                     ", got " + std::to_string(total_rows_affected) + ")");
        return;
    }
    if (has_expect_total_rows && total_rows != expect_total_rows) {
        addError(result,
                 had_error,
                 "total_rows mismatch (expected " + std::to_string(expect_total_rows) +
                     ", got " + std::to_string(total_rows) + ")");
    }
}

}  // namespace scratchbird::cli::parity
