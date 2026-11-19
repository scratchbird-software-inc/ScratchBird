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
        "--policy-seed-pack-root", "/tmp/policy-pack",
        // SB_CLI_NATIVE_BOOTSTRAP (owner Q254 = 1, 2026-11-16): the native
        // create's inputs are part of the valid grammar.
        "--total-pages", "512",
        "--service-principal=node_service",
        "--database-name", "qa database",
        "--language=en-CA",
        "--password-stdin",
    });
    require(valid.ok && !valid.help_requested, "valid bootstrap grammar must parse");
    require(valid.request.total_pages == 512 &&
                valid.request.service_principal_name == "node_service" &&
                valid.request.database_name == "qa database" &&
                valid.request.language_tag == "en-CA",
            "bootstrap must preserve the native create inputs");
    const auto defaults = parse({
        "qa_admin", "/tmp/qa.sbdb", "--mode=embedded",
        "--platform-profile=/tmp/SBbootstrap.profile",
        "--policy-seed-pack-root=/tmp/policy-pack",
        "--total-pages=512", "--service-principal=node_service",
    });
    require(defaults.ok && defaults.request.database_name.empty() &&
                defaults.request.language_tag == "en",
            "database name defaults to the file stem at run time; language defaults to en");
    for (const char* missing : {"--total-pages=512", "--service-principal=node_service"}) {
        const auto incomplete = parse({
            "qa_admin", "/tmp/qa.sbdb", "--mode=embedded",
            "--platform-profile=/tmp/SBbootstrap.profile",
                "--policy-seed-pack-root=/tmp/policy-pack",
        "--total-pages=512", "--service-principal=node_service",
            missing,
        });
        require(!incomplete.ok && incomplete.diagnostic_code == "BOOTSTRAP.AUTH_DB_INPUT_INVALID",
                "native create requires both --total-pages and --service-principal");
    }
    for (const char* bad_pages : {"--total-pages=0", "--total-pages=12x", "--total-pages=-5"}) {
        const auto refused = parse({
            "qa_admin", "/tmp/qa.sbdb", "--mode=embedded",
            "--platform-profile=/tmp/SBbootstrap.profile",
                "--policy-seed-pack-root=/tmp/policy-pack",
        "--total-pages=512", "--service-principal=node_service",
            bad_pages, "--service-principal=node_service",
        });
        require(!refused.ok, "total pages must be a positive decimal count");
    }
    require(valid.request.explicit_embedded_mode,
            "bootstrap must preserve the explicit embedded mode");
    require(valid.request.principal_name == "qa_admin",
            "bootstrap must preserve the principal positional");
    require(valid.request.database_path == "/tmp/qa.sbdb",
            "bootstrap must preserve the database positional");
    require(valid.request.password_source ==
                FirstPrincipalBootstrapPasswordSource::kProtectedStdin,
            "--password-stdin must select protected stdin");
    const auto missing_service_profile = parse({
        "qa_admin", "/tmp/qa.sbdb", "--mode=embedded",
        "--policy-seed-pack-root=/tmp/policy-pack",
        "--total-pages=512", "--service-principal=node_service",
    });
    require(!missing_service_profile.ok,
            "installer bootstrap still requires its explicit service profile");

    const auto missing_mode = parse({
        "qa_admin", "/tmp/qa.sbdb",
        "--platform-profile=/tmp/SBbootstrap.profile",
        "--policy-seed-pack-root=/tmp/policy-pack",
        "--total-pages=512", "--service-principal=node_service",
    });
    require(!missing_mode.ok &&
                missing_mode.diagnostic_code == "BOOTSTRAP.MODE_EMBEDDED_REQUIRED",
            "bootstrap must require an explicit embedded mode");

    const auto network_mode = parse({
        "qa_admin", "/tmp/qa.sbdb", "--mode=inet",
        "--platform-profile=/tmp/SBbootstrap.profile",
        "--policy-seed-pack-root=/tmp/policy-pack",
        "--total-pages=512", "--service-principal=node_service",
    });
    require(!network_mode.ok &&
                network_mode.diagnostic_code == "BOOTSTRAP.MODE_EMBEDDED_REQUIRED",
            "bootstrap must reject a network mode");

    const auto password_argv = parse({
        "qa_admin", "/tmp/qa.sbdb", "--mode=embedded",
        "--platform-profile=/tmp/SBbootstrap.profile",
        "--policy-seed-pack-root=/tmp/policy-pack",
        "--total-pages=512", "--service-principal=node_service",
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
                "--policy-seed-pack-root=/tmp/policy-pack",
        "--total-pages=512", "--service-principal=node_service",
            forbidden_option,
        });
        require(!forbidden.ok &&
                    forbidden.diagnostic_code == "BOOTSTRAP.OPTION_NOT_ALLOWED",
                "bootstrap must reject ordinary route, parser, query, and auth options");
    }

    // SB_CLI_NATIVE_BOOTSTRAP_GRAMMAR (owner Q268 = 1, 2026-11-18): the
    // duplicate-option case now uses --policy-seed-pack-root. (Was: two
    // --resource-seed-pack-root values, an option no longer in the grammar.)
    const auto duplicate_seed_root = parse({
        "qa_admin", "/tmp/qa.sbdb", "--mode=embedded",
        "--platform-profile=/tmp/SBbootstrap.profile",
        "--policy-seed-pack-root=/tmp/policy-pack-a",
        "--policy-seed-pack-root=/tmp/policy-pack-b",
        "--total-pages=512", "--service-principal=node_service",
    });
    require(!duplicate_seed_root.ok &&
                duplicate_seed_root.diagnostic_code == "BOOTSTRAP.AUTH_DB_INPUT_INVALID",
            "bootstrap must reject duplicate required options");
    // Q268 = 1: the dropped options are refused like any unknown option.
    // (Was: each accepted once and unused; --resource-seed-pack-root required.)
    for (const char* dropped_option :
         {"--resource-seed-pack-root=/tmp/resource-pack", "--metric-policy-defaults=/tmp/defaults.conf",
          "--metric-policy-families=/tmp/families.conf"}) {
        const auto dropped = parse({
            "qa_admin", "/tmp/qa.sbdb", "--mode=embedded",
            "--platform-profile=/tmp/SBbootstrap.profile",
            "--policy-seed-pack-root=/tmp/policy-pack",
            "--total-pages=512", "--service-principal=node_service",
            dropped_option,
        });
        require(!dropped.ok && dropped.diagnostic_code == "BOOTSTRAP.OPTION_NOT_ALLOWED",
                "bootstrap must refuse the resource-seed and metric-policy options dropped by Q268");
    }

    const auto extra_positional = parse({
        "qa_admin", "/tmp/qa.sbdb", "extra", "--mode=embedded",
        "--platform-profile=/tmp/SBbootstrap.profile",
        "--policy-seed-pack-root=/tmp/policy-pack",
        "--total-pages=512", "--service-principal=node_service",
    });
    require(!extra_positional.ok &&
                extra_positional.diagnostic_code == "BOOTSTRAP.AUTH_DB_INPUT_INVALID",
            "bootstrap must reject extra positional arguments");

    const auto positional_after_option = parse({
        "qa_admin", "--mode=embedded", "/tmp/qa.sbdb",
        "--platform-profile=/tmp/SBbootstrap.profile",
        "--policy-seed-pack-root=/tmp/policy-pack",
        "--total-pages=512", "--service-principal=node_service",
    });
    require(!positional_after_option.ok &&
                positional_after_option.diagnostic_code ==
                    "BOOTSTRAP.AUTH_DB_INPUT_INVALID",
            "bootstrap must retain its fixed positional-before-options grammar");

    const auto malformed_principal = parse({
        "1not_a_principal", "/tmp/qa.sbdb", "--mode=embedded",
        "--platform-profile=/tmp/SBbootstrap.profile",
        "--policy-seed-pack-root=/tmp/policy-pack",
        "--total-pages=512", "--service-principal=node_service",
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
