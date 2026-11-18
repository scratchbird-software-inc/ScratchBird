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

enum class EmbeddedCreationOwnership {
    kServiceIdentity,
    kCurrentProcess,
};

struct FirstPrincipalBootstrapRequest {
    bool explicit_embedded_mode = false;
    EmbeddedCreationOwnership ownership = EmbeddedCreationOwnership::kServiceIdentity;
    std::string principal_name;
    std::string database_path;
    std::string platform_profile_path;
    std::string resource_seed_pack_root;
    std::string policy_seed_pack_root;
    // METRIC-BOOTSTRAP-CONFIG-DEFAULTS-001: both installed metric policy files,
    // always explicit. Create publishes every selected family.
    std::string metric_policy_defaults_path;
    std::string metric_policy_families_path;
    FirstPrincipalBootstrapPasswordSource password_source =
        FirstPrincipalBootstrapPasswordSource::kProtectedPrompt;
    // SEARCH_KEY: SB_CLI_NATIVE_BOOTSTRAP (owner Q254 = 1, 2026-11-16). The
    // native create's inputs that the legacy bootstrap never had:
    //   --database-name   the database's name, as DATA (default: the file stem)
    //   --language        a catalog language tag (default "en"); the parser-side
    //                     adapter turns it into the catalog_language constant
    //   --total-pages     the file's size in pages, required (owner Q105 = 1)
    //   --service-principal  the internal service principal's name, required;
    //                     its password is read after the first principal's, by
    //                     the same protected path (second stdin line or prompt)
    // --resource-seed-pack-root and the metric policy files stay in the
    // grammar for the callers but have no native counterpart and are not
    // consumed by the native create.
    std::string database_name;
    std::string language_tag = "en";
    std::uint64_t total_pages = 0;
    std::string service_principal_name;
};

struct FirstPrincipalBootstrapResult {
    bool ok = false;
    std::string diagnostic_code;
    std::string principal_uuid;
    // SB_CLI_NATIVE_BOOTSTRAP: the native receipt carries no policy generation
    // (stays 0, not printed) and no sysarch role UUID; it does carry the
    // service principal and the construction transaction, reported below.
    std::uint64_t policy_generation = 0;
    bool committed_with_warning = false;
    bool creation_handoff_ready = false;
    std::string database_uuid;
    std::string filespace_uuid;
    std::string service_principal_uuid;
    std::string construction_transaction_uuid;
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

// Local CREATE uses the caller's OS file access by default. An explicit
// platform profile selects the administrator/service-account handoff instead.
// Neither choice authenticates against the database being constructed.
FirstPrincipalBootstrapParseResult ParseEmbeddedDatabaseCreateArgs(
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
