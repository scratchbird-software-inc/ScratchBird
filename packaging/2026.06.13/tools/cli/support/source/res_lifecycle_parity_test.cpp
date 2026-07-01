// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include "res_lifecycle_parity.h"

namespace {

using json = nlohmann::json;
using scratchbird::cli::parity::LifecycleObservation;
using scratchbird::cli::parity::ResourceLifecycleClient;
using scratchbird::core::ErrorContext;
using scratchbird::core::Status;

class FakeResourceLifecycleClient final : public ResourceLifecycleClient {
public:
    int connect_calls{0};
    int execute_calls{0};
    int disconnect_calls{0};
    int fail_connect_iteration{-1};
    int fail_execute_iteration{-1};
    LifecycleObservation fixed_observation{1, 0};
    std::string last_error;

    Status connect(ErrorContext* ctx) override {
        ++connect_calls;
        if (connect_calls == fail_connect_iteration) {
            return fail(Status::CONNECTION_FAILURE, "synthetic connect failure", ctx);
        }
        if (ctx != nullptr) {
            ctx->code = Status::OK;
            ctx->message.clear();
        }
        last_error.clear();
        return Status::OK;
    }

    Status executeStatement(const std::string&,
                            LifecycleObservation* observation,
                            ErrorContext* ctx) override {
        ++execute_calls;
        if (execute_calls == fail_execute_iteration) {
            return fail(Status::INTERNAL_ERROR, "synthetic execute failure", ctx);
        }
        if (observation != nullptr) {
            *observation = fixed_observation;
        }
        if (ctx != nullptr) {
            ctx->code = Status::OK;
            ctx->message.clear();
        }
        last_error.clear();
        return Status::OK;
    }

    void disconnect() override {
        ++disconnect_calls;
    }

    std::string lastError() const override {
        return last_error;
    }

private:
    Status fail(Status code, const std::string& message, ErrorContext* ctx) {
        last_error = message;
        if (ctx != nullptr) {
            ctx->code = code;
            ctx->message = message;
        }
        return code;
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

void testResourceLoopSuccess(int* failures) {
    FakeResourceLifecycleClient client;
    client.fixed_observation = LifecycleObservation{1, 0};

    json test = {
        {"id", "res_loop_success"},
        {"sql", "SELECT 1"},
        {"loop_iterations", 50},
        {"expect_total_rows_affected", 50},
        {"expect_total_rows", 0}
    };
    json result = makeBaseResult("res_loop_success");
    ErrorContext ctx;
    bool had_error = false;
    scratchbird::cli::parity::runResourceLifecycleLoopCase(client, test, result, &had_error, &ctx);

    expect(result.value("status", "") == "ok", "res loop success should pass", failures);
    expect(!had_error, "res loop success should not set had_error", failures);
    expect(client.connect_calls == 50, "res loop success connect count", failures);
    expect(client.execute_calls == 50, "res loop success execute count", failures);
    expect(client.disconnect_calls == 50, "res loop success disconnect count", failures);
    expect(result.value("total_rows_affected", -1) == 50, "res loop total_rows_affected", failures);
}

void testResourceLoopExecuteFailureStillDisconnects(int* failures) {
    FakeResourceLifecycleClient client;
    client.fixed_observation = LifecycleObservation{2, 0};
    client.fail_execute_iteration = 4;

    json test = {
        {"id", "res_loop_execute_failure"},
        {"sql", "UPDATE t SET v = 1"},
        {"loop_iterations", 10}
    };
    json result = makeBaseResult("res_loop_execute_failure");
    ErrorContext ctx;
    bool had_error = false;
    scratchbird::cli::parity::runResourceLifecycleLoopCase(client, test, result, &had_error, &ctx);

    expect(result.value("status", "") == "error", "res loop execute failure should fail", failures);
    expect(had_error, "res loop execute failure should set had_error", failures);
    expect(client.connect_calls == 4, "execute failure connect count", failures);
    expect(client.execute_calls == 4, "execute failure execute count", failures);
    expect(client.disconnect_calls == 4, "execute failure should disconnect failed iteration", failures);
}

void testResourceLoopConnectFailureStopsWithoutExtraDisconnect(int* failures) {
    FakeResourceLifecycleClient client;
    client.fail_connect_iteration = 3;

    json test = {
        {"id", "res_loop_connect_failure"},
        {"sql", "SELECT 1"},
        {"loop_iterations", 10}
    };
    json result = makeBaseResult("res_loop_connect_failure");
    ErrorContext ctx;
    bool had_error = false;
    scratchbird::cli::parity::runResourceLifecycleLoopCase(client, test, result, &had_error, &ctx);

    expect(result.value("status", "") == "error", "res loop connect failure should fail", failures);
    expect(had_error, "res loop connect failure should set had_error", failures);
    expect(client.connect_calls == 3, "connect failure connect count", failures);
    expect(client.execute_calls == 2, "connect failure execute count", failures);
    expect(client.disconnect_calls == 2, "connect failure disconnect count", failures);
}

void testResourceLoopRejectsInvalidIterations(int* failures) {
    FakeResourceLifecycleClient client;

    json test = {
        {"id", "res_loop_invalid_iterations"},
        {"sql", "SELECT 1"},
        {"loop_iterations", 0}
    };
    json result = makeBaseResult("res_loop_invalid_iterations");
    ErrorContext ctx;
    bool had_error = false;
    scratchbird::cli::parity::runResourceLifecycleLoopCase(client, test, result, &had_error, &ctx);

    expect(result.value("status", "") == "error", "invalid loop_iterations should fail", failures);
    expect(had_error, "invalid loop_iterations should set had_error", failures);
    expect(client.connect_calls == 0, "invalid loop_iterations should not connect", failures);
    expect(client.execute_calls == 0, "invalid loop_iterations should not execute", failures);
    expect(client.disconnect_calls == 0, "invalid loop_iterations should not disconnect", failures);
}

}  // namespace

int main() {
    int failures = 0;
    testResourceLoopSuccess(&failures);
    testResourceLoopExecuteFailureStillDisconnects(&failures);
    testResourceLoopConnectFailureStopsWithoutExtraDisconnect(&failures);
    testResourceLoopRejectsInvalidIterations(&failures);

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }

    std::cout << "res_lifecycle_parity_test: PASS\n";
    return 0;
}
