// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "first_principal_bootstrap.hpp"

#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <string>
#include <vector>

namespace {

using scratchbird::cli::FirstPrincipalBootstrapParseResult;
using scratchbird::cli::FirstPrincipalBootstrapPasswordSource;

FirstPrincipalBootstrapParseResult parse(
    std::initializer_list<const char*> arguments) {
    std::vector<char*> argv;
    argv.reserve(arguments.size());
    for (const char* argument : arguments) {
        argv.push_back(const_cast<char*>(argument));
    }
    return scratchbird::cli::ParseFirstPrincipalBootstrapArgs(
        static_cast<int>(argv.size()), argv.data());
}

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "first-principal bootstrap argument test failed: " << message
                  << "\n";
        std::exit(1);
    }
}

}  // namespace

int main() {
    const auto valid = parse({
        "qa_admin",
        "/tmp/qa.sbdb",
        "--mode=embedded",
        "--platform-profile", "/tmp/SBbootstrap.profile",
        "--resource-seed-pack-root=/tmp/resource-pack",
        "--policy-seed-pack-root", "/tmp/policy-pack",
        "--password-stdin",
    });
    require(valid.ok && !valid.help_requested, "valid bootstrap grammar must parse");
    require(valid.request.explicit_embedded_mode,
            "bootstrap must preserve the explicit embedded mode");
    require(valid.request.principal_name == "qa_admin",
            "bootstrap must preserve the principal positional");
    require(valid.request.database_path == "/tmp/qa.sbdb",
            "bootstrap must preserve the database positional");
    require(valid.request.password_source ==
                FirstPrincipalBootstrapPasswordSource::kProtectedStdin,
            "--password-stdin must select protected stdin");

    const auto missing_mode = parse({
        "qa_admin", "/tmp/qa.sbdb",
        "--platform-profile=/tmp/SBbootstrap.profile",
        "--resource-seed-pack-root=/tmp/resource-pack",
        "--policy-seed-pack-root=/tmp/policy-pack",
    });
    require(!missing_mode.ok &&
                missing_mode.diagnostic_code == "BOOTSTRAP.MODE_EMBEDDED_REQUIRED",
            "bootstrap must require an explicit embedded mode");

    const auto network_mode = parse({
        "qa_admin", "/tmp/qa.sbdb", "--mode=inet",
        "--platform-profile=/tmp/SBbootstrap.profile",
        "--resource-seed-pack-root=/tmp/resource-pack",
        "--policy-seed-pack-root=/tmp/policy-pack",
    });
    require(!network_mode.ok &&
                network_mode.diagnostic_code == "BOOTSTRAP.MODE_EMBEDDED_REQUIRED",
            "bootstrap must reject a network mode");

    const auto password_argv = parse({
        "qa_admin", "/tmp/qa.sbdb", "--mode=embedded",
        "--platform-profile=/tmp/SBbootstrap.profile",
        "--resource-seed-pack-root=/tmp/resource-pack",
        "--policy-seed-pack-root=/tmp/policy-pack",
        "--password=secret",
    });
    require(!password_argv.ok &&
                password_argv.diagnostic_code == "BOOTSTRAP.OPTION_NOT_ALLOWED" &&
                password_argv.safe_message.find("forbidden") != std::string::npos,
            "bootstrap must reject passwords supplied in argv");

    for (const char* forbidden_option :
         {"-U", "--connection=ignored", "--manager-auth-token=ignored",
          "--auth-token=ignored", "--parser=native", "-c", "--sslmode=disable"}) {
        const auto forbidden = parse({
            "qa_admin", "/tmp/qa.sbdb", "--mode=embedded",
            "--platform-profile=/tmp/SBbootstrap.profile",
            "--resource-seed-pack-root=/tmp/resource-pack",
            "--policy-seed-pack-root=/tmp/policy-pack",
            forbidden_option,
        });
        require(!forbidden.ok &&
                    forbidden.diagnostic_code == "BOOTSTRAP.OPTION_NOT_ALLOWED",
                "bootstrap must reject ordinary route, parser, query, and auth options");
    }

    const auto duplicate_seed_root = parse({
        "qa_admin", "/tmp/qa.sbdb", "--mode=embedded",
        "--platform-profile=/tmp/SBbootstrap.profile",
        "--resource-seed-pack-root=/tmp/resource-pack-a",
        "--resource-seed-pack-root=/tmp/resource-pack-b",
        "--policy-seed-pack-root=/tmp/policy-pack",
    });
    require(!duplicate_seed_root.ok &&
                duplicate_seed_root.diagnostic_code == "BOOTSTRAP.AUTH_DB_INPUT_INVALID",
            "bootstrap must reject duplicate required options");

    const auto extra_positional = parse({
        "qa_admin", "/tmp/qa.sbdb", "extra", "--mode=embedded",
        "--platform-profile=/tmp/SBbootstrap.profile",
        "--resource-seed-pack-root=/tmp/resource-pack",
        "--policy-seed-pack-root=/tmp/policy-pack",
    });
    require(!extra_positional.ok &&
                extra_positional.diagnostic_code == "BOOTSTRAP.AUTH_DB_INPUT_INVALID",
            "bootstrap must reject extra positional arguments");

    const auto positional_after_option = parse({
        "qa_admin", "--mode=embedded", "/tmp/qa.sbdb",
        "--platform-profile=/tmp/SBbootstrap.profile",
        "--resource-seed-pack-root=/tmp/resource-pack",
        "--policy-seed-pack-root=/tmp/policy-pack",
    });
    require(!positional_after_option.ok &&
                positional_after_option.diagnostic_code ==
                    "BOOTSTRAP.AUTH_DB_INPUT_INVALID",
            "bootstrap must retain its fixed positional-before-options grammar");

    const auto malformed_principal = parse({
        "1not_a_principal", "/tmp/qa.sbdb", "--mode=embedded",
        "--platform-profile=/tmp/SBbootstrap.profile",
        "--resource-seed-pack-root=/tmp/resource-pack",
        "--policy-seed-pack-root=/tmp/policy-pack",
    });
    require(!malformed_principal.ok &&
                malformed_principal.diagnostic_code ==
                    "BOOTSTRAP.AUTH_DB_INPUT_INVALID",
            "bootstrap must reject an invalid first-principal name before OS mutation");

    const auto help = parse({"--help"});
    require(help.ok && help.help_requested,
            "bootstrap must permit only its one-shot help form");
    return 0;
}
