// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>

namespace scratchbird::cli {

// This is deliberately a one-shot, pre-connection operation.  It has no
// client, parser, listener, manager, or network fields because first-principal
// creation is an embedded engine lifecycle action, not a SQL connection mode.
enum class FirstPrincipalBootstrapPasswordSource {
    kProtectedPrompt,
    kProtectedStdin,
};

struct FirstPrincipalBootstrapRequest {
    bool explicit_embedded_mode = false;
    std::string principal_name;
    std::string database_path;
    std::string platform_profile_path;
    std::string resource_seed_pack_root;
    std::string policy_seed_pack_root;
    FirstPrincipalBootstrapPasswordSource password_source =
        FirstPrincipalBootstrapPasswordSource::kProtectedPrompt;
};

struct FirstPrincipalBootstrapResult {
    bool ok = false;
    std::string diagnostic_code;
    std::string principal_uuid;
    std::uint64_t policy_generation = 0;
    bool committed_with_warning = false;
};

struct FirstPrincipalBootstrapParseResult {
    bool ok = false;
    bool help_requested = false;
    FirstPrincipalBootstrapRequest request;
    std::string diagnostic_code;
    std::string safe_message;
};

// Parse only the arguments after the literal `bootstrap` subcommand.  The
// grammar is intentionally closed: it rejects every ordinary connection,
// parser, query, manager, and delegated-authentication option before any
// filesystem or engine action can occur.
FirstPrincipalBootstrapParseResult ParseFirstPrincipalBootstrapArgs(
    int argc, char* const argv[]);

// Execute the approved first-principal bootstrap path.  The caller supplies
// streams so command grammar can be tested without attempting OS authority or
// creating a database.  This function obtains the password only from protected
// stdin or a no-echo prompt; it never accepts one from argv or the environment.
FirstPrincipalBootstrapResult RunFirstPrincipalBootstrap(
    const FirstPrincipalBootstrapRequest& request,
    std::istream& input,
    std::ostream& output);

void PrintFirstPrincipalBootstrapUsage(const char* program, std::ostream& output);

// CLI adapter shared by SBsql and SBsec.  `argv` contains only the tokens after
// `bootstrap`, not the executable name or the subcommand itself.
int RunFirstPrincipalBootstrapCli(
    const char* program,
    int argc,
    char* const argv[],
    std::istream& input,
    std::ostream& output,
    std::ostream& error);

}  // namespace scratchbird::cli
