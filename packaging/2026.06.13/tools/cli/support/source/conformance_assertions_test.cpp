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

#include "conformance_assertions.h"

namespace {

using nlohmann::json;
using scratchbird::cli::conformance::applyManifestExpectations;

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

json makeOkResult() {
    return json{
        {"test_id", "typed_assert"},
        {"status", "ok"},
        {"errors", json::array()},
        {"columns", json::array({"id", "label", "enabled"})},
        {"column_type_oids", json::array({23, 25, 16})},
        {"rows", json::array({json::array({1, "alpha", true})})}
    };
}

void testExpectationPass(int* failures) {
    json test{
        {"expect_row_count", 1},
        {"expect_columns", json::array({"id", "label", "enabled"})},
        {"expect_column_type_oids", json::array({23, 25, 16})},
        {"expect_first_row_json", json::array({1, "alpha", true})},
        {"expect_first_row_types", json::array({"integer", "string", "boolean"})},
        {"expect_rows_json", json::array({json::array({1, "alpha", true})})}
    };
    json result = makeOkResult();
    std::string summary;

    const bool ok = applyManifestExpectations(test, result, &summary);
    expect(ok, "Expected manifest assertions to pass", failures);
    expect(result.value("status", "") == "ok", "Result status should stay ok", failures);
    expect(summary.empty(), "Summary should be empty on success", failures);
}

void testExpectationFailure(int* failures) {
    json test{
        {"expect_columns", json::array({"id", "name"})}
    };
    json result = makeOkResult();
    std::string summary;

    const bool ok = applyManifestExpectations(test, result, &summary);
    expect(!ok, "Expected manifest assertions to fail", failures);
    expect(result.value("status", "") == "error", "Result status should be error on mismatch", failures);
    expect(!result["errors"].empty(), "Failure should append error messages", failures);
    expect(summary.find("Column name mismatch") != std::string::npos,
           "Summary should include mismatch reason",
           failures);
}

void testNumericCoercion(int* failures) {
    json test{
        {"expect_first_row_json", json::array({1.0, "alpha", true})}
    };
    json result = makeOkResult();

    const bool ok = applyManifestExpectations(test, result, nullptr);
    expect(ok, "Numeric expectations should tolerate int/float representation differences", failures);
}

void testTypeMismatchFailure(int* failures) {
    json test{
        {"expect_first_row_types", json::array({"string", "string", "boolean"})}
    };
    json result = makeOkResult();

    const bool ok = applyManifestExpectations(test, result, nullptr);
    expect(!ok, "Type mismatch should fail", failures);
}

}  // namespace

int main() {
    int failures = 0;

    testExpectationPass(&failures);
    testExpectationFailure(&failures);
    testNumericCoercion(&failures);
    testTypeMismatchFailure(&failures);

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }

    std::cout << "conformance_assertions_test: PASS\n";
    return 0;
}
