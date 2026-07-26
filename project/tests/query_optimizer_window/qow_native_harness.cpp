// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::qow {

// QOW-SOURCE-TST-001-HARNESS-V1
// QOW_NATIVE_HARNESS_MAIN
struct HarnessOutcome {
    int exit_code{0};
    std::string output_json;
    std::string diagnostic_json;
};

[[nodiscard]] HarnessOutcome ExecuteHarness(
    const std::span<const std::string_view> arguments)
{
    if (arguments.size() == 1 && arguments.front() == "--self-test") {
        return HarnessOutcome{
            0,
            "{\"execution_mode\":\"non_semantic_self_test\","
            "\"schema_id\":\"qow-native-harness-bootstrap-v1\","
            "\"semantic_execution\":false,\"status\":\"success\"}\n",
            {},
        };
    }

    const std::string_view reason = arguments.empty()
        ? "missing_mode"
        : "unsupported_mode";
    return HarnessOutcome{
        64,
        {},
        std::string{"{\"diagnostic_id\":\"QOW-HARNESS-MODE-REFUSED\","
                    "\"reason\":\""} + std::string{reason} +
            "\",\"schema_id\":\"qow-native-harness-diagnostic-v1\"}\n",
    };
}

}  // namespace scratchbird::qow

#ifndef SCRATCHBIRD_QOW_HARNESS_NO_MAIN
int main(int argc, char** argv)
{
    std::vector<std::string_view> arguments;
    arguments.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0U);
    for (int index = 1; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }

    const auto outcome = scratchbird::qow::ExecuteHarness(arguments);
    if (!outcome.output_json.empty()) {
        std::cout << outcome.output_json;
    }
    if (!outcome.diagnostic_json.empty()) {
        std::cerr << outcome.diagnostic_json;
    }
    return outcome.exit_code;
}
#endif
