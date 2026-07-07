// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "conformance_assertions.h"

#include <cmath>
#include <sstream>
#include <string>
#include <vector>

namespace scratchbird::cli::conformance {
namespace {

bool asInt64(const nlohmann::json& value, int64_t* out) {
    if (!value.is_number_integer() && !value.is_number_unsigned()) {
        return false;
    }
    if (out != nullptr) {
        *out = value.get<int64_t>();
    }
    return true;
}

std::string typeTag(const nlohmann::json& value) {
    if (value.is_null()) {
        return "null";
    }
    if (value.is_boolean()) {
        return "boolean";
    }
    if (value.is_number_integer() || value.is_number_unsigned()) {
        return "integer";
    }
    if (value.is_number_float()) {
        return "number";
    }
    if (value.is_string()) {
        return "string";
    }
    if (value.is_array()) {
        return "array";
    }
    return "object";
}

bool numbersEquivalent(const nlohmann::json& expected, const nlohmann::json& actual) {
    if (!expected.is_number() || !actual.is_number()) {
        return false;
    }
    const long double lhs = expected.get<long double>();
    const long double rhs = actual.get<long double>();
    const long double delta = std::fabs(lhs - rhs);
    return delta <= 1e-9L;
}

bool jsonEquivalent(const nlohmann::json& expected, const nlohmann::json& actual) {
    if (expected.type() == actual.type()) {
        if (expected.is_array()) {
            if (expected.size() != actual.size()) {
                return false;
            }
            for (size_t i = 0; i < expected.size(); ++i) {
                if (!jsonEquivalent(expected[i], actual[i])) {
                    return false;
                }
            }
            return true;
        }
        if (expected.is_object()) {
            if (expected.size() != actual.size()) {
                return false;
            }
            for (auto it = expected.begin(); it != expected.end(); ++it) {
                auto jt = actual.find(it.key());
                if (jt == actual.end() || !jsonEquivalent(it.value(), *jt)) {
                    return false;
                }
            }
            return true;
        }
        return expected == actual;
    }
    if (expected.is_number() && actual.is_number()) {
        return numbersEquivalent(expected, actual);
    }
    return false;
}

void addFailure(std::vector<std::string>& failures, const std::string& message) {
    failures.push_back(message);
}

std::string joinFailures(const std::vector<std::string>& failures) {
    std::ostringstream out;
    for (size_t i = 0; i < failures.size(); ++i) {
        if (i > 0) {
            out << "; ";
        }
        out << failures[i];
    }
    return out.str();
}

}  // namespace

bool applyManifestExpectations(const nlohmann::json& test,
                               nlohmann::json& result,
                               std::string* summary) {
    std::vector<std::string> failures;

    if (!result.contains("errors") || !result["errors"].is_array()) {
        result["errors"] = nlohmann::json::array();
    }
    if (!result.contains("rows") || !result["rows"].is_array()) {
        result["rows"] = nlohmann::json::array();
    }
    if (!result.contains("columns") || !result["columns"].is_array()) {
        result["columns"] = nlohmann::json::array();
    }
    if (!result.contains("column_type_oids") || !result["column_type_oids"].is_array()) {
        result["column_type_oids"] = nlohmann::json::array();
    }

    if (test.contains("expect_row_count")) {
        int64_t expected = 0;
        if (!asInt64(test["expect_row_count"], &expected)) {
            addFailure(failures, "expect_row_count must be an integer");
        } else {
            const int64_t actual = static_cast<int64_t>(result["rows"].size());
            if (actual != expected) {
                addFailure(
                    failures,
                    "Row count mismatch (expected " + std::to_string(expected) +
                        ", got " + std::to_string(actual) + ")");
            }
        }
    }

    if (test.contains("expect_columns")) {
        if (!test["expect_columns"].is_array()) {
            addFailure(failures, "expect_columns must be an array");
        } else if (result["columns"] != test["expect_columns"]) {
            addFailure(failures,
                       "Column name mismatch (expected " + test["expect_columns"].dump() +
                           ", got " + result["columns"].dump() + ")");
        }
    }

    if (test.contains("expect_column_type_oids")) {
        if (!test["expect_column_type_oids"].is_array()) {
            addFailure(failures, "expect_column_type_oids must be an array");
        } else if (result["column_type_oids"] != test["expect_column_type_oids"]) {
            addFailure(failures,
                       "Column type OID mismatch (expected " + test["expect_column_type_oids"].dump() +
                           ", got " + result["column_type_oids"].dump() + ")");
        }
    }

    const nlohmann::json* expected_rows = nullptr;
    if (test.contains("expect_rows_json")) {
        expected_rows = &test["expect_rows_json"];
    } else if (test.contains("expect_rows_exact")) {
        expected_rows = &test["expect_rows_exact"];
    }
    if (expected_rows != nullptr) {
        if (!expected_rows->is_array()) {
            addFailure(failures, "expect_rows_json/expect_rows_exact must be an array");
        } else if (!jsonEquivalent(*expected_rows, result["rows"])) {
            addFailure(failures,
                       "Row payload mismatch (expected " + expected_rows->dump() +
                           ", got " + result["rows"].dump() + ")");
        }
    }

    if (test.contains("expect_first_row_json")) {
        if (!test["expect_first_row_json"].is_array()) {
            addFailure(failures, "expect_first_row_json must be an array");
        } else if (result["rows"].empty()) {
            addFailure(failures, "expect_first_row_json requires at least one returned row");
        } else if (!jsonEquivalent(test["expect_first_row_json"], result["rows"][0])) {
            addFailure(failures,
                       "First-row mismatch (expected " + test["expect_first_row_json"].dump() +
                           ", got " + result["rows"][0].dump() + ")");
        }
    }

    if (test.contains("expect_first_row_types")) {
        if (!test["expect_first_row_types"].is_array()) {
            addFailure(failures, "expect_first_row_types must be an array");
        } else if (result["rows"].empty()) {
            addFailure(failures, "expect_first_row_types requires at least one returned row");
        } else if (!result["rows"][0].is_array()) {
            addFailure(failures, "Returned first row is not an array");
        } else {
            const auto& expected_types = test["expect_first_row_types"];
            const auto& first_row = result["rows"][0];
            if (expected_types.size() != first_row.size()) {
                addFailure(
                    failures,
                    "First-row type arity mismatch (expected " +
                        std::to_string(expected_types.size()) + ", got " +
                        std::to_string(first_row.size()) + ")");
            } else {
                for (size_t i = 0; i < expected_types.size(); ++i) {
                    if (!expected_types[i].is_string()) {
                        addFailure(failures, "expect_first_row_types entries must be strings");
                        break;
                    }
                    const std::string expected_tag = expected_types[i].get<std::string>();
                    const std::string actual_tag = typeTag(first_row[i]);
                    if (expected_tag != actual_tag) {
                        addFailure(
                            failures,
                            "First-row type mismatch at column " + std::to_string(i) +
                                " (expected " + expected_tag + ", got " + actual_tag + ")");
                    }
                }
            }
        }
    }

    if (!failures.empty()) {
        result["status"] = "error";
        for (const auto& failure : failures) {
            result["errors"].push_back("Expectation failed: " + failure);
        }
        if (summary != nullptr) {
            *summary = joinFailures(failures);
        }
        return false;
    }

    if (summary != nullptr) {
        summary->clear();
    }
    return true;
}

}  // namespace scratchbird::cli::conformance
