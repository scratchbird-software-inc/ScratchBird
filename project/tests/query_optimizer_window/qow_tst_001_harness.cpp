// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#define SCRATCHBIRD_QOW_HARNESS_NO_MAIN
#include "qow_native_harness.cpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

[[nodiscard]] bool Require(const bool condition, const std::string_view message)
{
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}

}  // namespace

// QOW-TEST-TST-001-HARNESS-V1
// QOW_TEST_TST_001_HARNESS
int main()
{
    bool passed = true;

    passed &= Require(QOW_BUILD_QOW_CLOSURE_TESTS == 1,
                      "isolated QOW switch is not enabled");
    passed &= Require(QOW_BUILD_TESTS == 0,
                      "broad SB_BUILD_TESTS switch leaked into QOW enrollment");
    passed &= Require(QOW_BUILD_SBSQL_PARSER_WORKER_TESTS == 0,
                      "broad parser-worker test switch leaked into QOW enrollment");

    constexpr std::array<std::string_view, 1> self_test{"--self-test"};
    const auto success = scratchbird::qow::ExecuteHarness(self_test);
    passed &= Require(success.exit_code == 0, "self-test did not succeed");
    passed &= Require(
        success.output_json ==
            "{\"execution_mode\":\"non_semantic_self_test\","
            "\"schema_id\":\"qow-native-harness-bootstrap-v1\","
            "\"semantic_execution\":false,\"status\":\"success\"}\n",
        "self-test output is not the locked structured record");
    passed &= Require(success.diagnostic_json.empty(),
                      "successful self-test emitted a diagnostic");

    const auto missing = scratchbird::qow::ExecuteHarness({});
    passed &= Require(missing.exit_code == 64,
                      "missing mode did not fail closed");
    passed &= Require(missing.output_json.empty(),
                      "missing mode emitted a successful output record");
    passed &= Require(
        missing.diagnostic_json ==
            "{\"diagnostic_id\":\"QOW-HARNESS-MODE-REFUSED\","
            "\"reason\":\"missing_mode\","
            "\"schema_id\":\"qow-native-harness-diagnostic-v1\"}\n",
        "missing mode diagnostic differs");

    constexpr std::array<std::string_view, 1> unsupported{"--execute-sql"};
    const auto refusal = scratchbird::qow::ExecuteHarness(unsupported);
    passed &= Require(refusal.exit_code == 64,
                      "unsupported semantic mode did not fail closed");
    passed &= Require(refusal.output_json.empty(),
                      "unsupported semantic mode emitted successful output");
    passed &= Require(
        refusal.diagnostic_json.find("unsupported_mode") != std::string::npos,
        "unsupported semantic mode diagnostic differs");

    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
