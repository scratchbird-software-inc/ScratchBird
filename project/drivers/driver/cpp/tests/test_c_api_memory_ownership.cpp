// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "scratchbird/client/scratchbird_client.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string_view>

namespace {

[[noreturn]] void Fail(std::string_view message) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
    if (!condition) {
        Fail(message);
    }
}

void DriverOwnedCStringLifecycle() {
    sb_error err{};
    Require(sb_test_driver_owned_allocation_count() == 0,
            "MMCH-016 expected no driver-owned ABI allocations at test start");

    char* payload = sb_test_allocate_owned_memory("owned-payload", &err);
    Require(payload != nullptr, "MMCH-016 test allocation failed");
    Require(err.code == SB_OK, "MMCH-016 test allocation returned non-OK error");
    Require(std::strcmp(payload, "owned-payload") == 0,
            "MMCH-016 allocated payload contents changed");
    Require(sb_test_driver_owned_allocation_count() == 1,
            "MMCH-016 driver-owned allocation count was not tracked");

    sb_memory_ownership_info info{};
    const int describe_status = sb_memory_describe(payload, &info, &err);
    Require(describe_status == SB_OK, "MMCH-016 owned pointer describe failed");
    Require(err.code == SB_OK, "MMCH-016 owned pointer describe set non-OK error");
    Require(info.abi_version == 1, "MMCH-016 ownership ABI version changed");
    Require(info.ownership_kind == SB_MEMORY_OWNERSHIP_DRIVER_ALLOCATED,
            "MMCH-016 owned pointer kind changed");
    Require(info.bytes == std::strlen("owned-payload") + 1,
            "MMCH-016 owned pointer byte count changed");
    Require(std::string_view(info.purpose) == "test_owned_memory",
            "MMCH-016 owned pointer purpose changed");
    Require(std::string_view(info.release_function) == "sb_memory_release",
            "MMCH-016 owned pointer release function changed");
    Require(std::string_view(info.authority_scope) ==
                "abi_memory_ownership_evidence_only_not_transaction_finality_visibility_security_recovery_parser_reference_or_benchmark_authority",
            "MMCH-016 ownership authority scope changed");

    const int release_status = sb_memory_release(payload, &err);
    Require(release_status == SB_OK, "MMCH-016 owned pointer release failed");
    Require(err.code == SB_OK, "MMCH-016 owned pointer release set non-OK error");
    Require(sb_test_driver_owned_allocation_count() == 0,
            "MMCH-016 owned pointer release leaked allocation tracking");

    const int double_release_status = sb_memory_release(payload, &err);
    Require(double_release_status == SB_ERR_INVALID_PARAM,
            "MMCH-016 double release did not fail closed");
    Require(err.code == SB_ERR_INVALID_PARAM,
            "MMCH-016 double release did not report invalid param");
}

void LegacyFreeDelegatesSafely() {
    sb_error err{};
    char* payload = sb_test_allocate_owned_memory("legacy-free-payload", &err);
    Require(payload != nullptr && err.code == SB_OK,
            "MMCH-016 legacy-free allocation failed");
    Require(sb_test_driver_owned_allocation_count() == 1,
            "MMCH-016 legacy-free allocation was not tracked");
    sb_memory_free(payload);
    Require(sb_test_driver_owned_allocation_count() == 0,
            "MMCH-016 legacy sb_memory_free did not release tracking");

    int stack_value = 7;
    sb_memory_ownership_info info{};
    const int describe_status = sb_memory_describe(&stack_value, &info, &err);
    Require(describe_status == SB_ERR_INVALID_PARAM,
            "MMCH-016 stack pointer describe did not fail closed");
    Require(info.ownership_kind == SB_MEMORY_OWNERSHIP_UNKNOWN,
            "MMCH-016 stack pointer ownership kind was not unknown");
    Require(std::string_view(info.authority_scope) ==
                "abi_memory_ownership_evidence_only_not_transaction_finality_visibility_security_recovery_parser_reference_or_benchmark_authority",
            "MMCH-016 unknown pointer authority scope missing");

    const int release_status = sb_memory_release(&stack_value, &err);
    Require(release_status == SB_ERR_INVALID_PARAM,
            "MMCH-016 foreign pointer release did not fail closed");
    sb_memory_free(&stack_value);
    Require(sb_test_driver_owned_allocation_count() == 0,
            "MMCH-016 foreign pointer free changed allocation tracking");
}

void NullAndOutputValidation() {
    sb_error err{};
    Require(sb_memory_release(nullptr, &err) == SB_OK,
            "MMCH-016 null memory release should be OK");
    Require(err.code == SB_OK, "MMCH-016 null memory release set non-OK error");
    Require(sb_memory_describe(nullptr, nullptr, &err) == SB_ERR_NULL_POINTER,
            "MMCH-016 missing ownership output was not rejected");

    sb_memory_ownership_info info{};
    Require(sb_memory_describe(nullptr, &info, &err) == SB_ERR_NULL_POINTER,
            "MMCH-016 null pointer describe was not rejected");
    Require(info.ownership_kind == SB_MEMORY_OWNERSHIP_UNKNOWN,
            "MMCH-016 null pointer ownership kind was not unknown");
}

}  // namespace

int main() {
    std::cout << "MMCH-016 authority_note=abi_memory_ownership_evidence_only;"
                 "not_transaction_finality_visibility_security_recovery_parser_reference_or_benchmark_authority"
              << '\n';
    DriverOwnedCStringLifecycle();
    LegacyFreeDelegatesSafely();
    NullAndOutputValidation();
    return EXIT_SUCCESS;
}
