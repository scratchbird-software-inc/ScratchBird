// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "first_principal_bootstrap.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string_view>
#include <vector>

#include <openssl/crypto.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

#include "bootstrap_os_authority.hpp"
#include "bootstrap_password_verifier.hpp"
#include "database_lifecycle.hpp"
#include "memory.hpp"
#include "uuid.hpp"

namespace scratchbird::cli {
namespace {

constexpr std::string_view kBootstrapInputInvalid =
    "BOOTSTRAP.AUTH_DB_INPUT_INVALID";
constexpr std::string_view kBootstrapEmbeddedModeRequired =
    "BOOTSTRAP.MODE_EMBEDDED_REQUIRED";
constexpr std::string_view kBootstrapOptionNotAllowed =
    "BOOTSTRAP.OPTION_NOT_ALLOWED";

FirstPrincipalBootstrapParseResult parseFailure(
    std::string_view diagnostic_code,
    std::string_view safe_message = {}) {
    FirstPrincipalBootstrapParseResult result;
    result.diagnostic_code = diagnostic_code;
    result.safe_message = safe_message;
    return result;
}

FirstPrincipalBootstrapResult bootstrapFailure(std::string_view diagnostic_code) {
    FirstPrincipalBootstrapResult result;
    result.diagnostic_code = diagnostic_code;
    return result;
}

bool validBootstrapPrincipalName(const std::string& value) {
    if (value.empty() || value.size() > 128) {
        return false;
    }
    const auto first = static_cast<unsigned char>(value.front());
    if (!(std::isalpha(first) || first == '_')) {
        return false;
    }
    return std::all_of(value.begin() + 1, value.end(), [](unsigned char ch) {
        return std::isalnum(ch) || ch == '_' || ch == '-' || ch == '.';
    });
}

void clearSensitive(std::string& value) {
    if (!value.empty()) {
        OPENSSL_cleanse(value.data(), value.size());
        value.clear();
    }
}

std::string readProtectedPassword(std::istream& input,
                                  std::ostream& output,
                                  const std::string& prompt) {
    output << prompt;
    output.flush();

#ifdef _WIN32
    HANDLE console_input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD old_mode = 0;
    const bool restore_mode =
        console_input != INVALID_HANDLE_VALUE &&
        GetConsoleMode(console_input, &old_mode) != 0;
    if (restore_mode) {
        (void)SetConsoleMode(console_input, old_mode & ~ENABLE_ECHO_INPUT);
    }
#else
    struct termios old_term {};
    struct termios new_term {};
    const bool restore_term = ::isatty(STDIN_FILENO) != 0 &&
                              tcgetattr(STDIN_FILENO, &old_term) == 0;
    if (restore_term) {
        new_term = old_term;
        new_term.c_lflag &= ~(ECHO);
        (void)tcsetattr(STDIN_FILENO, TCSANOW, &new_term);
    }
#endif

    std::string password;
    (void)std::getline(input, password);

#ifdef _WIN32
    if (restore_mode) {
        (void)SetConsoleMode(console_input, old_mode);
    }
#else
    if (restore_term) {
        (void)tcsetattr(STDIN_FILENO, TCSANOW, &old_term);
    }
#endif
    output << "\n";
    return password;
}

bool isExactOption(std::string_view token, std::string_view option) {
    return token == option ||
           (token.size() > option.size() &&
            token.substr(0, option.size()) == option &&
            token[option.size()] == '=');
}

bool readRequiredOptionValue(int& index,
                             int argc,
                             char* const argv[],
                             std::string_view token,
                             std::string_view option,
                             std::string* value) {
    if (token == option) {
        if (index + 1 >= argc || argv[index + 1] == nullptr ||
            argv[index + 1][0] == '\0' || argv[index + 1][0] == '-') {
            return false;
        }
        *value = argv[++index];
        return true;
    }
    const std::string prefix = std::string(option) + "=";
    if (token.substr(0, prefix.size()) != prefix) {
        return false;
    }
    const std::string_view assigned = token.substr(prefix.size());
    if (assigned.empty()) {
        return false;
    }
    value->assign(assigned);
    return true;
}

bool isForbiddenBootstrapOption(std::string_view token) {
    // The parser is closed, so this list exists only to attach a precise,
    // stable diagnostic to the main option families instead of silently
    // treating a forbidden routing option as a positional database path.
    constexpr std::string_view exact_options[] = {
        "-U", "--user", "-P", "--password", "-H", "--host", "-p",
        "--port", "--connection", "--ipc-method", "--ipc-path",
        "--front-door-mode", "--client-flags", "--sslmode", "--conn-opt",
        "-par", "--parser", "-c", "--command", "-f", "--file", "-i",
        "--input", "-o", "--output", "-t", "--tuples-only", "-A",
        "--no-align", "-F", "--field-separator", "-q", "--quiet", "-e",
        "--echo", "-b", "--bail", "-a", "--extract-all", "-x",
        "--extract", "-ex", "--extract-db", "-s", "--dialect",
        "--schema-tree", "--probe-auth-surface", "--show-auth-context",
        "--workload-identity-token", "--proxy-principal-assertion",
    };
    for (const auto option : exact_options) {
        if (isExactOption(token, option)) {
            return true;
        }
    }
    return token.rfind("--manager-", 0) == 0 ||
           token.rfind("--auth-", 0) == 0;
}

}  // namespace

void PrintFirstPrincipalBootstrapUsage(const char* program, std::ostream& output) {
    output << "Create a new local ScratchBird database and its first security "
              "principal.\n\n";
    output << "Usage:\n  " << program
           << " bootstrap <first-principal> <database-path> --mode=embedded"
              " --platform-profile <file> --resource-seed-pack-root <dir>"
              " --policy-seed-pack-root <dir> [--password-stdin]\n\n";
    output << "This is an explicit embedded, root/Administrator-authorized "
              "bootstrap operation. It does not connect to a listener, manager, "
              "or parser and it never auto-creates a database during an ordinary "
              "SQL client session.\n";
}

FirstPrincipalBootstrapParseResult ParseFirstPrincipalBootstrapArgs(
    int argc, char* const argv[]) {
    if (argc == 1 && argv != nullptr && argv[0] != nullptr &&
        std::string_view(argv[0]) == "--help") {
        FirstPrincipalBootstrapParseResult result;
        result.ok = true;
        result.help_requested = true;
        return result;
    }

    FirstPrincipalBootstrapParseResult result;
    std::vector<std::string> positional_arguments;
    bool mode_seen = false;
    bool platform_profile_seen = false;
    bool resource_seed_pack_seen = false;
    bool policy_seed_pack_seen = false;
    bool password_stdin_seen = false;
    bool option_seen = false;

    for (int index = 0; index < argc; ++index) {
        if (argv[index] == nullptr) {
            return parseFailure(kBootstrapInputInvalid);
        }
        const std::string_view token(argv[index]);
        if (token == "--password-stdin") {
            if (password_stdin_seen) {
                return parseFailure(kBootstrapInputInvalid);
            }
            password_stdin_seen = true;
            option_seen = true;
            result.request.password_source =
                FirstPrincipalBootstrapPasswordSource::kProtectedStdin;
            continue;
        }
        if (isExactOption(token, "--mode")) {
            if (mode_seen) {
                return parseFailure(kBootstrapInputInvalid);
            }
            std::string mode;
            if (!readRequiredOptionValue(index, argc, argv, token, "--mode", &mode) ||
                mode != "embedded") {
                return parseFailure(kBootstrapEmbeddedModeRequired);
            }
            mode_seen = true;
            result.request.explicit_embedded_mode = true;
            option_seen = true;
            continue;
        }
        if (isExactOption(token, "--platform-profile")) {
            if (platform_profile_seen ||
                !readRequiredOptionValue(index, argc, argv, token,
                                         "--platform-profile",
                                         &result.request.platform_profile_path)) {
                return parseFailure(kBootstrapInputInvalid);
            }
            platform_profile_seen = true;
            option_seen = true;
            continue;
        }
        if (isExactOption(token, "--resource-seed-pack-root")) {
            if (resource_seed_pack_seen ||
                !readRequiredOptionValue(index, argc, argv, token,
                                         "--resource-seed-pack-root",
                                         &result.request.resource_seed_pack_root)) {
                return parseFailure(kBootstrapInputInvalid);
            }
            resource_seed_pack_seen = true;
            option_seen = true;
            continue;
        }
        if (isExactOption(token, "--policy-seed-pack-root")) {
            if (policy_seed_pack_seen ||
                !readRequiredOptionValue(index, argc, argv, token,
                                         "--policy-seed-pack-root",
                                         &result.request.policy_seed_pack_root)) {
                return parseFailure(kBootstrapInputInvalid);
            }
            policy_seed_pack_seen = true;
            option_seen = true;
            continue;
        }
        if (!token.empty() && token.front() == '-') {
            if (token == "-P" || isExactOption(token, "--password")) {
                return parseFailure(
                    kBootstrapOptionNotAllowed,
                    "Bootstrap passwords are forbidden in command-line arguments; "
                    "use the protected prompt or --password-stdin");
            }
            if (isForbiddenBootstrapOption(token)) {
                return parseFailure(kBootstrapOptionNotAllowed);
            }
            return parseFailure(kBootstrapOptionNotAllowed);
        }
        if (option_seen) {
            return parseFailure(kBootstrapInputInvalid);
        }
        positional_arguments.emplace_back(token);
        if (positional_arguments.size() > 2) {
            return parseFailure(kBootstrapInputInvalid);
        }
    }

    if (!mode_seen) {
        return parseFailure(kBootstrapEmbeddedModeRequired);
    }
    if (!platform_profile_seen || !resource_seed_pack_seen ||
        !policy_seed_pack_seen || positional_arguments.size() != 2) {
        return parseFailure(kBootstrapInputInvalid);
    }

    result.request.principal_name = std::move(positional_arguments[0]);
    result.request.database_path = std::move(positional_arguments[1]);
    if (!validBootstrapPrincipalName(result.request.principal_name)) {
        return parseFailure(kBootstrapInputInvalid);
    }
    result.ok = true;
    return result;
}

FirstPrincipalBootstrapResult RunFirstPrincipalBootstrap(
    const FirstPrincipalBootstrapRequest& request,
    std::istream& input,
    std::ostream& output) {
    if (!request.explicit_embedded_mode) {
        return bootstrapFailure(kBootstrapEmbeddedModeRequired);
    }
    if (!validBootstrapPrincipalName(request.principal_name) ||
        request.database_path.empty() || request.platform_profile_path.empty() ||
        request.resource_seed_pack_root.empty() || request.policy_seed_pack_root.empty()) {
        return bootstrapFailure(kBootstrapInputInvalid);
    }
    if (std::filesystem::exists(request.database_path)) {
        return bootstrapFailure("BOOTSTRAP.AUTH_DB_ALREADY_OWNED");
    }

    const auto loaded_profile = LoadBootstrapPlatformProfile(
        request.platform_profile_path);
    if (!loaded_profile.ok) {
        return bootstrapFailure(kBootstrapDeniedDiagnostic);
    }
    const auto os_authority = VerifyAndAssumeBootstrapServiceIdentity(
        loaded_profile.profile);
    if (!os_authority.ok || !os_authority.ownership_handoff_ready) {
        return bootstrapFailure(kBootstrapDeniedDiagnostic);
    }
    const auto prepared_access = EnsureBootstrapServiceIdentityAccess(
        loaded_profile.profile, request.database_path);
    if (!prepared_access.ok || !prepared_access.ownership_handoff_ready) {
        return bootstrapFailure("BOOTSTRAP.DIRECTORY_PERMISSION_INVALID");
    }

    std::string password;
    if (request.password_source ==
        FirstPrincipalBootstrapPasswordSource::kProtectedStdin) {
        if (!std::getline(input, password)) {
            return bootstrapFailure(kBootstrapInputInvalid);
        }
    } else {
        password = readProtectedPassword(
            input, output, "Enter password for " + request.principal_name + ": ");
        std::string confirmation = readProtectedPassword(input, output, "Confirm password: ");
        const bool matches = password == confirmation;
        clearSensitive(confirmation);
        if (!matches) {
            clearSensitive(password);
            return bootstrapFailure("BOOTSTRAP.SYSARCH_PASSWORD_POLICY_INVALID");
        }
    }
    if (!BootstrapPasswordSecretValid(password)) {
        clearSensitive(password);
        return bootstrapFailure("BOOTSTRAP.SYSARCH_PASSWORD_POLICY_INVALID");
    }

    std::string credential_fingerprint;
    if (!DeriveBootstrapPasswordVerifier(password, &credential_fingerprint)) {
        clearSensitive(password);
        return bootstrapFailure("BOOTSTRAP.SYSARCH_PASSWORD_POLICY_INVALID");
    }
    clearSensitive(password);

    namespace db = scratchbird::storage::database;
    namespace memory = scratchbird::core::memory;
    namespace uuid = scratchbird::core::uuid;
    using scratchbird::core::platform::UuidKind;
    const auto configured_memory = memory::ConfigureDefaultMemoryManager(
        memory::DefaultLocalEngineMemoryPolicy(), "sbcli.first_principal_bootstrap");
    if (!configured_memory.ok()) {
        clearSensitive(credential_fingerprint);
        return bootstrapFailure(kBootstrapInputInvalid);
    }
    const auto now = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    const auto database_uuid = uuid::GenerateEngineIdentityV7(UuidKind::database, now);
    const auto filespace_uuid = uuid::GenerateEngineIdentityV7(UuidKind::filespace, now + 1);
    if (!database_uuid.ok() || !filespace_uuid.ok()) {
        clearSensitive(credential_fingerprint);
        return bootstrapFailure(kBootstrapInputInvalid);
    }

    db::DatabaseCreateConfig create_request;
    create_request.path = request.database_path;
    create_request.database_uuid = database_uuid.value;
    create_request.filespace_uuid = filespace_uuid.value;
    create_request.page_size = 16u * 1024u;
    create_request.creation_unix_epoch_millis = now;
    create_request.resource_seed_pack_root = request.resource_seed_pack_root;
    create_request.require_resource_seed_pack = true;
    create_request.policy_seed_pack_root = request.policy_seed_pack_root;
    create_request.require_policy_seed_pack = true;
    create_request.bootstrap_principal_name = request.principal_name;
    create_request.bootstrap_credential_fingerprint = credential_fingerprint;
    create_request.require_bootstrap_principal = true;
    create_request.allow_uncredentialed_bootstrap = false;
    create_request.allow_overwrite = false;
    const auto create_result = db::CreateDatabaseFile(create_request);
    clearSensitive(create_request.bootstrap_credential_fingerprint);
    clearSensitive(credential_fingerprint);
    if (!create_result.ok()) {
        return bootstrapFailure(
            create_result.diagnostic.diagnostic_code.empty()
                ? kBootstrapInputInvalid
                : create_result.diagnostic.diagnostic_code);
    }

    const auto finalized_access = EnsureBootstrapServiceIdentityAccess(
        loaded_profile.profile, request.database_path);
    if (!finalized_access.ok || !finalized_access.ownership_handoff_ready) {
        return bootstrapFailure("BOOTSTRAP.DIRECTORY_PERMISSION_INVALID");
    }

    const auto committed_catalog = db::ReadDatabaseBootstrapSecurityCatalog(
        request.database_path);
    if (!committed_catalog.ok() || !committed_catalog.state.present ||
        !committed_catalog.state.committed_by_inventory) {
        return bootstrapFailure("BOOTSTRAP.SECURITY_DATABASE_UNAVAILABLE");
    }
    const std::string principal_uuid =
        uuid::UuidToString(committed_catalog.state.principal_uuid.value);
    const std::string sysarch_role_uuid =
        uuid::UuidToString(committed_catalog.state.sysarch_role_uuid.value);
    if (sysarch_role_uuid != db::kCanonicalSysarchRoleObjectUuid ||
        committed_catalog.state.principal_name != request.principal_name ||
        principal_uuid.empty()) {
        return bootstrapFailure("BOOTSTRAP.SECURITY_DATABASE_UNAVAILABLE");
    }

    FirstPrincipalBootstrapResult result;
    result.ok = true;
    result.principal_uuid = principal_uuid;
    result.policy_generation = committed_catalog.state.policy_generation;
    result.committed_with_warning =
        create_result.create_finality == db::DatabaseCreateFinalityClass::committed_with_warning;
    return result;
}

int RunFirstPrincipalBootstrapCli(
    const char* program,
    int argc,
    char* const argv[],
    std::istream& input,
    std::ostream& output,
    std::ostream& error) {
    const auto parsed = ParseFirstPrincipalBootstrapArgs(argc, argv);
    if (!parsed.ok) {
        error << "Error: " << parsed.diagnostic_code;
        if (!parsed.safe_message.empty()) {
            error << ": " << parsed.safe_message;
        }
        error << "\n";
        return 1;
    }
    if (parsed.help_requested) {
        PrintFirstPrincipalBootstrapUsage(program, output);
        return 0;
    }

    const auto result = RunFirstPrincipalBootstrap(parsed.request, input, output);
    if (!result.ok) {
        error << "Error: " << result.diagnostic_code << "\n";
        return 1;
    }

    output << "First security principal created and committed by transaction-1 "
              "catalog authority\n";
    output << "bootstrap_principal_uuid=" << result.principal_uuid << "\n";
    output << "bootstrap_scope=initial_local_embedded_database_security_tree\n";
    output << "policy_generation=" << result.policy_generation << "\n";
    if (result.committed_with_warning) {
        output << "Create finality: committed_with_warning\n";
    }
    return 0;
}

}  // namespace scratchbird::cli
