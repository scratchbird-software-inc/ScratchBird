// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "txn_exec_parity.h"

#include <algorithm>
#include <cctype>
#include <limits>
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

std::string toLowerCopy(const std::string& value) {
    std::string out = value;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return out;
}

std::string quoteSqlLiteral(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('\'');
    for (char ch : value) {
        out.push_back(ch);
        if (ch == '\'') {
            out.push_back('\'');
        }
    }
    out.push_back('\'');
    return out;
}

std::string bestError(const TxnExecClient& client, const core::ErrorContext* ctx) {
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

bool readOptionalBool(const nlohmann::json& test,
                      const char* key,
                      bool* out,
                      bool* has_value,
                      std::string* error) {
    if (has_value != nullptr) {
        *has_value = false;
    }
    auto it = test.find(key);
    if (it == test.end()) {
        return true;
    }
    if (!it->is_boolean()) {
        if (error != nullptr) {
            *error = std::string("Field '") + key + "' must be a boolean";
        }
        return false;
    }
    if (out != nullptr) {
        *out = it->get<bool>();
    }
    if (has_value != nullptr) {
        *has_value = true;
    }
    return true;
}

bool isSimpleIdentifier(const std::string& value) {
    if (value.empty()) {
        return false;
    }
    for (char ch : value) {
        const unsigned char uch = static_cast<unsigned char>(ch);
        if (std::isalnum(uch) == 0 && ch != '_') {
            return false;
        }
    }
    return true;
}

}  // namespace

bool supportsPreparedTransactions() {
    return true;
}

bool supportsDormantReattach() {
    return false;
}

bool supportsPortalResume() {
    return false;
}

core::Status buildPreparedTransactionSql(const std::string& verb,
                                         const std::string& global_transaction_id,
                                         std::string* sql,
                                         core::ErrorContext* ctx) {
    if (sql == nullptr) {
        if (ctx != nullptr) {
            ctx->set(core::Status::INVALID_ARGUMENT,
                     "SQL output buffer is required",
                     __FILE__,
                     __LINE__,
                     __func__);
        }
        return core::Status::INVALID_ARGUMENT;
    }
    if (trimCopy(verb).empty()) {
        if (ctx != nullptr) {
            ctx->set(core::Status::INVALID_ARGUMENT,
                     "Prepared-transaction verb is required",
                     __FILE__,
                     __LINE__,
                     __func__);
        }
        return core::Status::INVALID_ARGUMENT;
    }
    const std::string gid = trimCopy(global_transaction_id);
    if (gid.empty()) {
        if (ctx != nullptr) {
            ctx->set(core::Status::SYNTAX_ERROR,
                     "Global transaction id is required",
                     __FILE__,
                     __LINE__,
                     __func__);
            ctx->setSQLState(core::SQLSTATE_SYNTAX_ERROR);
        }
        sql->clear();
        return core::Status::SYNTAX_ERROR;
    }
    *sql = trimCopy(verb) + " " + quoteSqlLiteral(gid);
    return core::Status::OK;
}

core::Status rejectDormantReattach(const char* operation,
                                   core::ErrorContext* ctx) {
    const std::string op = trimCopy(operation != nullptr ? operation : "");
    const std::string message =
        op.empty()
            ? "dormant detach/reattach is not exposed by the CLI front door"
            : "dormant " + op + " is not exposed by the CLI front door";
    if (ctx != nullptr) {
        ctx->set(core::Status::NOT_IMPLEMENTED,
                 message.c_str(),
                 __FILE__,
                 __LINE__,
                 __func__);
        ctx->setSQLState(core::SQLSTATE_FEATURE_NOT_SUPPORTED);
    }
    return core::Status::NOT_IMPLEMENTED;
}

void runNativeExecCase(TxnExecClient& client,
                       const nlohmann::json& test,
                       nlohmann::json& result,
                       bool* had_error,
                       core::ErrorContext* ctx) {
    const std::string sql = trimCopy(test.value("sql", ""));
    if (sql.empty()) {
        addError(result, had_error, "native_exec requires sql");
        return;
    }

    int64_t expect_rows_affected = 0;
    int64_t expect_rows = 0;
    bool has_expect_rows_affected = false;
    bool has_expect_rows = false;
    std::string parse_error;
    if (!readOptionalInt64(test, "expect_rows_affected", &expect_rows_affected,
                           &has_expect_rows_affected, &parse_error) ||
        !readOptionalInt64(test, "expect_rows", &expect_rows, &has_expect_rows, &parse_error)) {
        addError(result, had_error, parse_error);
        return;
    }

    ExecObservation observation;
    core::Status status = client.executeStatement(sql, &observation, ctx);
    if (status != core::Status::OK) {
        addError(result, had_error, bestError(client, ctx));
        return;
    }

    result["rows_affected"] = observation.rows_affected;
    result["observed_rows"] = observation.rows_returned;

    if (has_expect_rows_affected && observation.rows_affected != expect_rows_affected) {
        addError(result,
                 had_error,
                 "rows_affected mismatch (expected " + std::to_string(expect_rows_affected) +
                     ", got " + std::to_string(observation.rows_affected) + ")");
        return;
    }

    if (has_expect_rows && observation.rows_returned != expect_rows) {
        addError(result,
                 had_error,
                 "Row count mismatch (expected " + std::to_string(expect_rows) +
                     ", got " + std::to_string(observation.rows_returned) + ")");
    }
}

void runTxnExecCase(TxnExecClient& client,
                    const nlohmann::json& test,
                    nlohmann::json& result,
                    bool* had_error,
                    core::ErrorContext* ctx) {
    const std::string sql = trimCopy(test.value("sql", ""));
    if (sql.empty()) {
        addError(result, had_error, "txn_exec requires sql");
        return;
    }

    const std::string txn_end = toLowerCopy(trimCopy(test.value("txn_end", "commit")));
    if (txn_end != "commit" && txn_end != "rollback") {
        addError(result, had_error, "txn_end must be commit or rollback");
        return;
    }
    const std::string savepoint_name = trimCopy(test.value("savepoint_name", ""));

    int64_t expect_rows_affected = std::numeric_limits<int64_t>::min();
    int64_t verify_expect_rows = std::numeric_limits<int64_t>::min();
    bool has_expect_rows_affected = false;
    bool has_verify_expect_rows = false;
    bool rollback_to_savepoint = false;
    bool has_rollback_to_savepoint = false;
    bool release_savepoint = false;
    bool has_release_savepoint = false;
    std::string parse_error;

    if (!readOptionalInt64(test, "expect_rows_affected", &expect_rows_affected,
                           &has_expect_rows_affected, &parse_error)) {
        addError(result, had_error, parse_error);
        return;
    }
    if (!readOptionalInt64(test, "verify_expect_rows", &verify_expect_rows,
                           &has_verify_expect_rows, &parse_error)) {
        addError(result, had_error, parse_error);
        return;
    }
    if (!has_verify_expect_rows) {
        if (!readOptionalInt64(test, "expect_rows", &verify_expect_rows,
                               &has_verify_expect_rows, &parse_error)) {
            addError(result, had_error, parse_error);
            return;
        }
    }
    if (!readOptionalBool(test, "rollback_to_savepoint", &rollback_to_savepoint,
                          &has_rollback_to_savepoint, &parse_error)) {
        addError(result, had_error, parse_error);
        return;
    }
    if (!readOptionalBool(test, "release_savepoint", &release_savepoint,
                          &has_release_savepoint, &parse_error)) {
        addError(result, had_error, parse_error);
        return;
    }

    const bool has_savepoint = !savepoint_name.empty();
    if (has_savepoint && !isSimpleIdentifier(savepoint_name)) {
        addError(result, had_error, "savepoint_name must use [A-Za-z0-9_]+");
        return;
    }
    if ((has_rollback_to_savepoint && rollback_to_savepoint) && !has_savepoint) {
        addError(result, had_error, "rollback_to_savepoint requires savepoint_name");
        return;
    }
    if ((has_release_savepoint && release_savepoint) && !has_savepoint) {
        addError(result, had_error, "release_savepoint requires savepoint_name");
        return;
    }

    bool transaction_open = false;
    auto rollback_if_open = [&]() {
        if (!transaction_open) {
            return;
        }
        core::ErrorContext rollback_ctx;
        client.rollback(&rollback_ctx);
        transaction_open = false;
    };

    core::Status status = client.beginTransaction(ctx);
    if (status != core::Status::OK) {
        addError(result, had_error, bestError(client, ctx));
        return;
    }
    transaction_open = true;

    if (has_savepoint) {
        ExecObservation savepoint_observation;
        const std::string savepoint_sql = "SAVEPOINT " + savepoint_name;
        status = client.executeStatement(savepoint_sql, &savepoint_observation, ctx);
        if (status != core::Status::OK) {
            rollback_if_open();
            addError(result, had_error, bestError(client, ctx));
            return;
        }
    }

    ExecObservation txn_observation;
    status = client.executeStatement(sql, &txn_observation, ctx);
    if (status != core::Status::OK) {
        rollback_if_open();
        addError(result, had_error, bestError(client, ctx));
        return;
    }

    if (has_savepoint && rollback_to_savepoint) {
        ExecObservation rollback_to_savepoint_observation;
        const std::string rollback_to_savepoint_sql = "ROLLBACK TO SAVEPOINT " + savepoint_name;
        status = client.executeStatement(rollback_to_savepoint_sql, &rollback_to_savepoint_observation, ctx);
        if (status != core::Status::OK) {
            rollback_if_open();
            addError(result, had_error, bestError(client, ctx));
            return;
        }
    }

    if (has_savepoint && release_savepoint) {
        ExecObservation release_savepoint_observation;
        const std::string release_savepoint_sql = "RELEASE SAVEPOINT " + savepoint_name;
        status = client.executeStatement(release_savepoint_sql, &release_savepoint_observation, ctx);
        if (status != core::Status::OK) {
            rollback_if_open();
            addError(result, had_error, bestError(client, ctx));
            return;
        }
    }

    if (has_expect_rows_affected && txn_observation.rows_affected != expect_rows_affected) {
        rollback_if_open();
        addError(result,
                 had_error,
                 "rows_affected mismatch (expected " + std::to_string(expect_rows_affected) +
                     ", got " + std::to_string(txn_observation.rows_affected) + ")");
        return;
    }

    if (txn_end == "rollback") {
        status = client.rollback(ctx);
    } else {
        status = client.commit(ctx);
    }
    transaction_open = false;
    if (status != core::Status::OK) {
        addError(result, had_error, bestError(client, ctx));
        return;
    }

    ExecObservation final_observation = txn_observation;
    const std::string verify_sql = trimCopy(test.value("verify_sql", ""));
    if (!verify_sql.empty()) {
        status = client.executeStatement(verify_sql, &final_observation, ctx);
        if (status != core::Status::OK) {
            addError(result, had_error, bestError(client, ctx));
            return;
        }
        if (has_verify_expect_rows && final_observation.rows_returned != verify_expect_rows) {
            addError(result,
                     had_error,
                     "Verify row count mismatch (expected " + std::to_string(verify_expect_rows) +
                         ", got " + std::to_string(final_observation.rows_returned) + ")");
            return;
        }
    }

    const std::string cleanup_sql = trimCopy(test.value("cleanup_sql", ""));
    if (!cleanup_sql.empty()) {
        ExecObservation cleanup_observation;
        status = client.executeStatement(cleanup_sql, &cleanup_observation, ctx);
        if (status != core::Status::OK) {
            addError(result, had_error, bestError(client, ctx));
            return;
        }
    }

    result["rows_affected"] = final_observation.rows_affected;
    result["observed_rows"] = final_observation.rows_returned;
}

}  // namespace scratchbird::cli::parity
