// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "txn_exec_parity.h"

namespace {

using json = nlohmann::json;
using scratchbird::cli::parity::ExecObservation;
using scratchbird::cli::parity::TxnExecClient;
using scratchbird::core::ErrorContext;
using scratchbird::core::Status;

enum class CallType {
    kExecute,
    kBegin,
    kCommit,
    kRollback
};

struct ScriptedCall {
    CallType type{CallType::kExecute};
    std::string sql;
    Status status{Status::OK};
    ExecObservation observation{};
    std::string message;
};

class FakeTxnExecClient final : public TxnExecClient {
public:
    std::vector<ScriptedCall> script;
    size_t offset{0};
    std::string last_error;

    explicit FakeTxnExecClient(std::vector<ScriptedCall> scripted_calls)
        : script(std::move(scripted_calls)) {}

    Status executeStatement(const std::string& sql,
                            ExecObservation* observation,
                            ErrorContext* ctx) override {
        return consume(CallType::kExecute, sql, observation, ctx);
    }

    Status beginTransaction(ErrorContext* ctx) override {
        return consume(CallType::kBegin, "", nullptr, ctx);
    }

    Status commit(ErrorContext* ctx) override {
        return consume(CallType::kCommit, "", nullptr, ctx);
    }

    Status rollback(ErrorContext* ctx) override {
        return consume(CallType::kRollback, "", nullptr, ctx);
    }

    std::string lastError() const override {
        return last_error;
    }

    bool consumedAll() const {
        return offset == script.size();
    }

private:
    Status consume(CallType expected_type,
                   const std::string& sql,
                   ExecObservation* observation,
                   ErrorContext* ctx) {
        if (offset >= script.size()) {
            return fail(Status::INTERNAL_ERROR, "Unexpected call", ctx);
        }
        const ScriptedCall& next = script[offset++];
        if (next.type != expected_type) {
            return fail(Status::INTERNAL_ERROR, "Call type mismatch", ctx);
        }
        if (expected_type == CallType::kExecute && next.sql != sql) {
            return fail(Status::INTERNAL_ERROR,
                        "SQL mismatch (expected '" + next.sql + "', got '" + sql + "')",
                        ctx);
        }

        if (next.status != Status::OK) {
            return fail(next.status, next.message.empty() ? "Scripted failure" : next.message, ctx);
        }

        last_error.clear();
        if (observation != nullptr) {
            *observation = next.observation;
        }
        if (ctx != nullptr) {
            ctx->code = Status::OK;
            ctx->message.clear();
        }
        return Status::OK;
    }

    Status fail(Status status, const std::string& message, ErrorContext* ctx) {
        last_error = message;
        if (ctx != nullptr) {
            ctx->code = status;
            ctx->message = message;
        }
        return status;
    }
};

bool expect(bool condition, const std::string& message, int* failures) {
    if (condition) {
        return true;
    }
    std::cerr << "FAIL: " << message << "\n";
    if (failures != nullptr) {
        ++(*failures);
    }
    return false;
}

json makeBaseResult(const std::string& id) {
    json result;
    result["test_id"] = id;
    result["status"] = "ok";
    result["errors"] = json::array();
    result["rows"] = json::array();
    result["columns"] = json::array();
    return result;
}

void testNativeExecPass(int* failures) {
    FakeTxnExecClient client({
        {CallType::kExecute, "DELETE FROM basic_table WHERE id = 1", Status::OK, {1, 0}, ""}
    });
    json test = {
        {"id", "native_exec_pass"},
        {"sql", "DELETE FROM basic_table WHERE id = 1"},
        {"expect_rows_affected", 1},
        {"expect_rows", 0}
    };
    json result = makeBaseResult("native_exec_pass");
    ErrorContext ctx;
    bool had_error = false;
    scratchbird::cli::parity::runNativeExecCase(client, test, result, &had_error, &ctx);

    expect(result.value("status", "") == "ok", "native_exec should pass", failures);
    expect(!had_error, "native_exec should not set had_error", failures);
    expect(result.value("rows_affected", -1) == 1, "native_exec rows_affected", failures);
    expect(result.value("observed_rows", -1) == 0, "native_exec observed_rows", failures);
    expect(client.consumedAll(), "native_exec should consume script", failures);
}

void testNativeExecMismatch(int* failures) {
    FakeTxnExecClient client({
        {CallType::kExecute, "DELETE FROM basic_table WHERE id = 2", Status::OK, {1, 0}, ""}
    });
    json test = {
        {"id", "native_exec_mismatch"},
        {"sql", "DELETE FROM basic_table WHERE id = 2"},
        {"expect_rows_affected", 3}
    };
    json result = makeBaseResult("native_exec_mismatch");
    ErrorContext ctx;
    bool had_error = false;
    scratchbird::cli::parity::runNativeExecCase(client, test, result, &had_error, &ctx);

    expect(result.value("status", "") == "error", "native_exec mismatch should fail", failures);
    expect(had_error, "native_exec mismatch should set had_error", failures);
    expect(client.consumedAll(), "native_exec mismatch should consume script", failures);
}

void testTxnExecCommitAndVerify(int* failures) {
    FakeTxnExecClient client({
        {CallType::kBegin, "", Status::OK, {}, ""},
        {CallType::kExecute, "INSERT INTO basic_table (id) VALUES (7001)", Status::OK, {1, 0}, ""},
        {CallType::kCommit, "", Status::OK, {}, ""},
        {CallType::kExecute, "SELECT id FROM basic_table WHERE id = 7001", Status::OK, {0, 1}, ""},
        {CallType::kExecute, "DELETE FROM basic_table WHERE id = 7001", Status::OK, {1, 0}, ""}
    });
    json test = {
        {"id", "txn_exec_commit"},
        {"sql", "INSERT INTO basic_table (id) VALUES (7001)"},
        {"txn_end", "commit"},
        {"expect_rows_affected", 1},
        {"verify_sql", "SELECT id FROM basic_table WHERE id = 7001"},
        {"verify_expect_rows", 1},
        {"cleanup_sql", "DELETE FROM basic_table WHERE id = 7001"}
    };
    json result = makeBaseResult("txn_exec_commit");
    ErrorContext ctx;
    bool had_error = false;
    scratchbird::cli::parity::runTxnExecCase(client, test, result, &had_error, &ctx);

    expect(result.value("status", "") == "ok", "txn_exec commit should pass", failures);
    expect(!had_error, "txn_exec commit should not set had_error", failures);
    expect(result.value("observed_rows", -1) == 1, "txn_exec verify row count", failures);
    expect(client.consumedAll(), "txn_exec commit should consume script", failures);
}

void testTxnExecRollbackAndVerify(int* failures) {
    FakeTxnExecClient client({
        {CallType::kBegin, "", Status::OK, {}, ""},
        {CallType::kExecute, "INSERT INTO basic_table (id) VALUES (7002)", Status::OK, {1, 0}, ""},
        {CallType::kRollback, "", Status::OK, {}, ""},
        {CallType::kExecute, "SELECT id FROM basic_table WHERE id = 7002", Status::OK, {0, 0}, ""}
    });
    json test = {
        {"id", "txn_exec_rollback"},
        {"sql", "INSERT INTO basic_table (id) VALUES (7002)"},
        {"txn_end", "rollback"},
        {"expect_rows_affected", 1},
        {"verify_sql", "SELECT id FROM basic_table WHERE id = 7002"},
        {"verify_expect_rows", 0}
    };
    json result = makeBaseResult("txn_exec_rollback");
    ErrorContext ctx;
    bool had_error = false;
    scratchbird::cli::parity::runTxnExecCase(client, test, result, &had_error, &ctx);

    expect(result.value("status", "") == "ok", "txn_exec rollback should pass", failures);
    expect(!had_error, "txn_exec rollback should not set had_error", failures);
    expect(result.value("observed_rows", -1) == 0, "txn_exec rollback verify row count", failures);
    expect(client.consumedAll(), "txn_exec rollback should consume script", failures);
}

void testTxnExecFailureRollsBack(int* failures) {
    FakeTxnExecClient client({
        {CallType::kBegin, "", Status::OK, {}, ""},
        {CallType::kExecute, "INSERT INTO basic_table (id) VALUES (7003)", Status::SYNTAX_ERROR, {}, "synthetic failure"},
        {CallType::kRollback, "", Status::OK, {}, ""}
    });
    json test = {
        {"id", "txn_exec_failure"},
        {"sql", "INSERT INTO basic_table (id) VALUES (7003)"},
        {"txn_end", "commit"}
    };
    json result = makeBaseResult("txn_exec_failure");
    ErrorContext ctx;
    bool had_error = false;
    scratchbird::cli::parity::runTxnExecCase(client, test, result, &had_error, &ctx);

    expect(result.value("status", "") == "error", "txn_exec failure should fail", failures);
    expect(had_error, "txn_exec failure should set had_error", failures);
    expect(client.consumedAll(), "txn_exec failure should rollback and consume script", failures);
}

void testTxnExecSavepointReleaseCommit(int* failures) {
    FakeTxnExecClient client({
        {CallType::kBegin, "", Status::OK, {}, ""},
        {CallType::kExecute, "SAVEPOINT sp_cli", Status::OK, {0, 0}, ""},
        {CallType::kExecute, "INSERT INTO basic_table (id) VALUES (7010)", Status::OK, {1, 0}, ""},
        {CallType::kExecute, "RELEASE SAVEPOINT sp_cli", Status::OK, {0, 0}, ""},
        {CallType::kCommit, "", Status::OK, {}, ""},
        {CallType::kExecute, "SELECT id FROM basic_table WHERE id = 7010", Status::OK, {0, 1}, ""},
        {CallType::kExecute, "DELETE FROM basic_table WHERE id = 7010", Status::OK, {1, 0}, ""}
    });
    json test = {
        {"id", "txn_exec_savepoint_release"},
        {"sql", "INSERT INTO basic_table (id) VALUES (7010)"},
        {"savepoint_name", "sp_cli"},
        {"release_savepoint", true},
        {"txn_end", "commit"},
        {"expect_rows_affected", 1},
        {"verify_sql", "SELECT id FROM basic_table WHERE id = 7010"},
        {"verify_expect_rows", 1},
        {"cleanup_sql", "DELETE FROM basic_table WHERE id = 7010"}
    };
    json result = makeBaseResult("txn_exec_savepoint_release");
    ErrorContext ctx;
    bool had_error = false;
    scratchbird::cli::parity::runTxnExecCase(client, test, result, &had_error, &ctx);

    expect(result.value("status", "") == "ok", "txn_exec savepoint/release should pass", failures);
    expect(!had_error, "txn_exec savepoint/release should not set had_error", failures);
    expect(result.value("observed_rows", -1) == 1, "txn_exec savepoint/release verify row count", failures);
    expect(client.consumedAll(), "txn_exec savepoint/release should consume script", failures);
}

void testTxnExecSavepointRollbackToCommit(int* failures) {
    FakeTxnExecClient client({
        {CallType::kBegin, "", Status::OK, {}, ""},
        {CallType::kExecute, "SAVEPOINT sp_cli2", Status::OK, {0, 0}, ""},
        {CallType::kExecute, "INSERT INTO basic_table (id) VALUES (7011)", Status::OK, {1, 0}, ""},
        {CallType::kExecute, "ROLLBACK TO SAVEPOINT sp_cli2", Status::OK, {0, 0}, ""},
        {CallType::kCommit, "", Status::OK, {}, ""},
        {CallType::kExecute, "SELECT id FROM basic_table WHERE id = 7011", Status::OK, {0, 0}, ""}
    });
    json test = {
        {"id", "txn_exec_savepoint_rollback_to"},
        {"sql", "INSERT INTO basic_table (id) VALUES (7011)"},
        {"savepoint_name", "sp_cli2"},
        {"rollback_to_savepoint", true},
        {"txn_end", "commit"},
        {"expect_rows_affected", 1},
        {"verify_sql", "SELECT id FROM basic_table WHERE id = 7011"},
        {"verify_expect_rows", 0}
    };
    json result = makeBaseResult("txn_exec_savepoint_rollback_to");
    ErrorContext ctx;
    bool had_error = false;
    scratchbird::cli::parity::runTxnExecCase(client, test, result, &had_error, &ctx);

    expect(result.value("status", "") == "ok", "txn_exec rollback-to-savepoint should pass", failures);
    expect(!had_error, "txn_exec rollback-to-savepoint should not set had_error", failures);
    expect(result.value("observed_rows", -1) == 0, "txn_exec rollback-to-savepoint verify row count", failures);
    expect(client.consumedAll(), "txn_exec rollback-to-savepoint should consume script", failures);
}

void testTxnExecSavepointFlagsRequireName(int* failures) {
    FakeTxnExecClient client({});
    json test = {
        {"id", "txn_exec_savepoint_requires_name"},
        {"sql", "INSERT INTO basic_table (id) VALUES (7012)"},
        {"rollback_to_savepoint", true}
    };
    json result = makeBaseResult("txn_exec_savepoint_requires_name");
    ErrorContext ctx;
    bool had_error = false;
    scratchbird::cli::parity::runTxnExecCase(client, test, result, &had_error, &ctx);

    expect(result.value("status", "") == "error", "rollback_to_savepoint without name should fail", failures);
    expect(had_error, "rollback_to_savepoint without name should set had_error", failures);
    expect(client.consumedAll(), "rollback_to_savepoint without name should not consume script", failures);
}

void testPreparedDormantAndPortalCapabilitiesStayExplicit(int* failures) {
    expect(scratchbird::cli::parity::supportsPreparedTransactions(),
           "CLI lane should expose prepared-transaction support explicitly",
           failures);
    expect(!scratchbird::cli::parity::supportsDormantReattach(),
           "CLI lane should keep dormant reattach explicitly unsupported",
           failures);
    expect(!scratchbird::cli::parity::supportsPortalResume(),
           "CLI lane should keep standalone portal resume explicitly unsupported",
           failures);

    std::string sql;
    ErrorContext ctx;
    Status status = scratchbird::cli::parity::buildPreparedTransactionSql(
        " PREPARE TRANSACTION ", " gid'one ", &sql, &ctx);
    expect(status == Status::OK, "prepared transaction SQL builder should succeed", failures);
    expect(sql == "PREPARE TRANSACTION 'gid''one'",
           "prepared transaction SQL should be canonical and quoted",
           failures);

    sql.clear();
    ErrorContext blank_ctx;
    status = scratchbird::cli::parity::buildPreparedTransactionSql(
        "COMMIT PREPARED", "   ", &sql, &blank_ctx);
    expect(status == Status::SYNTAX_ERROR,
           "blank global transaction id should fail with syntax error",
           failures);
    expect(std::string(blank_ctx.sqlstate) == "42601",
           "blank global transaction id should expose SQLSTATE 42601",
           failures);

    ErrorContext dormant_ctx;
    status = scratchbird::cli::parity::rejectDormantReattach("reattach", &dormant_ctx);
    expect(status == Status::NOT_IMPLEMENTED,
           "CLI dormant reattach should fail closed as not implemented",
           failures);
    expect(std::string(dormant_ctx.sqlstate) == "0A000",
           "CLI dormant reattach should expose SQLSTATE 0A000",
           failures);
}

}  // namespace

int main() {
    int failures = 0;
    testNativeExecPass(&failures);
    testNativeExecMismatch(&failures);
    testTxnExecCommitAndVerify(&failures);
    testTxnExecRollbackAndVerify(&failures);
    testTxnExecFailureRollsBack(&failures);
    testTxnExecSavepointReleaseCommit(&failures);
    testTxnExecSavepointRollbackToCommit(&failures);
    testTxnExecSavepointFlagsRequireName(&failures);
    testPreparedDormantAndPortalCapabilitiesStayExplicit(&failures);

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }

    std::cout << "txn_exec_parity_test: PASS\n";
    return 0;
}
