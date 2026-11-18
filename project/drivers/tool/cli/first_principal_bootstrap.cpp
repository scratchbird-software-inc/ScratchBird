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
#include "catalog_language.hpp"
#include "database_lifecycle.hpp"
#include "lifecycle/engine_lifecycle_api.hpp"
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

bool parseTotalPages(const std::string& text, std::uint64_t* out) {
    if (text.empty() || text.size() > 19) return false;
    std::uint64_t value = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') return false;
        value = value * 10u + static_cast<std::uint64_t>(ch - '0');
    }
    if (value == 0) return false;
    *out = value;
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
              " --policy-seed-pack-root <dir> --metric-policy-defaults <file>"
              " --metric-policy-families <file> --total-pages <n>"
              " --service-principal <name> [--database-name <name>]"
              " [--language <tag>] [--password-stdin]\n\n";
    output << "The database is created in the native single-file format: --total-pages"
              " sizes the file, --database-name is the database's name (data; default:"
              " the file stem), --language its catalog language (default en), and"
              " --service-principal names the internal service principal, whose"
              " password is read after the first principal's (second line with"
              " --password-stdin).\n";
    output << "This is an explicit embedded, root/Administrator-authorized "
              "bootstrap operation. It does not connect to a listener, manager, "
              "or parser and it never auto-creates a database during an ordinary "
              "SQL client session.\n";
}

namespace {
FirstPrincipalBootstrapParseResult ParseCreationArgs(
    int argc, char* const argv[], bool service_profile_required) {
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
    bool metric_defaults_seen = false;
    bool metric_families_seen = false;
    bool password_stdin_seen = false;
    bool option_seen = false;
    bool database_name_seen = false;
    bool language_seen = false;
    bool total_pages_seen = false;
    bool service_principal_seen = false;

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
        if (isExactOption(token, "--metric-policy-defaults")) {
            if (metric_defaults_seen ||
                !readRequiredOptionValue(index, argc, argv, token,
                                         "--metric-policy-defaults",
                                         &result.request.metric_policy_defaults_path)) {
                return parseFailure(kBootstrapInputInvalid);
            }
            metric_defaults_seen = true;
            option_seen = true;
            continue;
        }
        if (isExactOption(token, "--metric-policy-families")) {
            if (metric_families_seen ||
                !readRequiredOptionValue(index, argc, argv, token,
                                         "--metric-policy-families",
                                         &result.request.metric_policy_families_path)) {
                return parseFailure(kBootstrapInputInvalid);
            }
            metric_families_seen = true;
            option_seen = true;
            continue;
        }
        // SB_CLI_NATIVE_BOOTSTRAP (owner Q254 = 1, 2026-11-16): the native
        // create's inputs. Each at most once; values never start with '-'.
        if (isExactOption(token, "--database-name")) {
            if (database_name_seen ||
                !readRequiredOptionValue(index, argc, argv, token, "--database-name",
                                         &result.request.database_name)) {
                return parseFailure(kBootstrapInputInvalid);
            }
            database_name_seen = true;
            option_seen = true;
            continue;
        }
        if (isExactOption(token, "--language")) {
            if (language_seen ||
                !readRequiredOptionValue(index, argc, argv, token, "--language",
                                         &result.request.language_tag)) {
                return parseFailure(kBootstrapInputInvalid);
            }
            language_seen = true;
            option_seen = true;
            continue;
        }
        if (isExactOption(token, "--total-pages")) {
            std::string pages;
            if (total_pages_seen ||
                !readRequiredOptionValue(index, argc, argv, token, "--total-pages", &pages) ||
                !parseTotalPages(pages, &result.request.total_pages)) {
                return parseFailure(kBootstrapInputInvalid);
            }
            total_pages_seen = true;
            option_seen = true;
            continue;
        }
        if (isExactOption(token, "--service-principal")) {
            if (service_principal_seen ||
                !readRequiredOptionValue(index, argc, argv, token, "--service-principal",
                                         &result.request.service_principal_name)) {
                return parseFailure(kBootstrapInputInvalid);
            }
            service_principal_seen = true;
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
    if ((service_profile_required && !platform_profile_seen) || !resource_seed_pack_seen ||
        !policy_seed_pack_seen || positional_arguments.size() != 2) {
        return parseFailure(kBootstrapInputInvalid);
    }
    // SB_CLI_NATIVE_BOOTSTRAP: the native create has no default for the file's
    // size (owner Q105 = 1) and requires a service principal.
    if (!total_pages_seen || !service_principal_seen ||
        !validBootstrapPrincipalName(result.request.service_principal_name) ||
        result.request.language_tag.empty()) {
        return parseFailure(kBootstrapInputInvalid);
    }

    result.request.principal_name = std::move(positional_arguments[0]);
    result.request.database_path = std::move(positional_arguments[1]);
    result.request.ownership = platform_profile_seen
        ? EmbeddedCreationOwnership::kServiceIdentity
        : EmbeddedCreationOwnership::kCurrentProcess;
    if (!validBootstrapPrincipalName(result.request.principal_name)) {
        return parseFailure(kBootstrapInputInvalid);
    }
    result.ok = true;
    return result;
}
}  // namespace

FirstPrincipalBootstrapParseResult ParseFirstPrincipalBootstrapArgs(
    int argc, char* const argv[]) {
    return ParseCreationArgs(argc, argv, true);
}

FirstPrincipalBootstrapParseResult ParseEmbeddedDatabaseCreateArgs(
    int argc, char* const argv[]) {
    return ParseCreationArgs(argc, argv, false);
}

FirstPrincipalBootstrapResult RunFirstPrincipalBootstrap(
    const FirstPrincipalBootstrapRequest& request,
    std::istream& input,
    std::ostream& output) {
    if (!request.explicit_embedded_mode) {
        return bootstrapFailure(kBootstrapEmbeddedModeRequired);
    }
    const bool service_handoff = request.ownership == EmbeddedCreationOwnership::kServiceIdentity;
    const bool current_process = request.ownership == EmbeddedCreationOwnership::kCurrentProcess;
    if ((!service_handoff && !current_process) ||
        (service_handoff && request.platform_profile_path.empty()) ||
        (current_process && !request.platform_profile_path.empty()) ||
        !validBootstrapPrincipalName(request.principal_name) ||
        request.database_path.empty() ||
        request.resource_seed_pack_root.empty() || request.policy_seed_pack_root.empty() ||
        request.metric_policy_defaults_path.empty() ||
        request.metric_policy_families_path.empty()) {
        return bootstrapFailure(kBootstrapInputInvalid);
    }
    if (std::filesystem::exists(request.database_path)) {
        return bootstrapFailure("BOOTSTRAP.AUTH_DB_ALREADY_OWNED");
    }

    BootstrapPlatformProfileLoadResult loaded_profile;
    if (service_handoff) {
        loaded_profile = LoadBootstrapPlatformProfile(request.platform_profile_path);
        if (!loaded_profile.ok) {
            return bootstrapFailure(kBootstrapDeniedDiagnostic);
        }
        const auto os_authority = VerifyAndAssumeBootstrapServiceIdentity(loaded_profile.profile);
        if (!os_authority.ok || !os_authority.ownership_handoff_ready) {
            return bootstrapFailure(kBootstrapDeniedDiagnostic);
        }
        const auto prepared_access = EnsureBootstrapServiceIdentityAccess(
            loaded_profile.profile, request.database_path);
        if (!prepared_access.ok || !prepared_access.ownership_handoff_ready) {
            return bootstrapFailure("BOOTSTRAP.DIRECTORY_PERMISSION_INVALID");
        }
    }
    // Current-process creation relies on the actual exclusive file create and
    // the OS access checks it performs. It never changes credentials, groups,
    // directory ownership or existing permissions to manufacture file access.

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

    // SEARCH_KEY: SB_CLI_NATIVE_BOOTSTRAP (owner Q254 = 1, 2026-11-16). The
    // internal service principal's password, by the same protected path (never
    // argv or the environment): the second stdin line, or a second prompt.
    std::string service_password;
    if (request.password_source == FirstPrincipalBootstrapPasswordSource::kProtectedStdin) {
        if (!std::getline(input, service_password)) {
            clearSensitive(credential_fingerprint);
            return bootstrapFailure(kBootstrapInputInvalid);
        }
    } else {
        service_password = readProtectedPassword(
            input, output, "Enter password for service principal " + request.service_principal_name + ": ");
        std::string confirmation = readProtectedPassword(input, output, "Confirm service password: ");
        const bool matches = service_password == confirmation;
        clearSensitive(confirmation);
        if (!matches) {
            clearSensitive(service_password);
            clearSensitive(credential_fingerprint);
            return bootstrapFailure("BOOTSTRAP.SERVICE_PASSWORD_POLICY_INVALID");
        }
    }
    std::string service_fingerprint;
    if (!BootstrapPasswordSecretValid(service_password) ||
        !DeriveBootstrapPasswordVerifier(service_password, &service_fingerprint)) {
        clearSensitive(service_password);
        clearSensitive(credential_fingerprint);
        return bootstrapFailure("BOOTSTRAP.SERVICE_PASSWORD_POLICY_INVALID");
    }
    clearSensitive(service_password);

    namespace api = scratchbird::engine::internal_api;
    namespace memory = scratchbird::core::memory;
    namespace uuid = scratchbird::core::uuid;
    const auto configured_memory = memory::ConfigureDefaultMemoryManager(
        memory::DefaultLocalEngineMemoryPolicy(), "sbcli.first_principal_bootstrap");
    if (!configured_memory.ok()) {
        clearSensitive(credential_fingerprint);
        clearSensitive(service_fingerprint);
        return bootstrapFailure(kBootstrapInputInvalid);
    }
    // The language tag is adapted to its catalog_language constant here, at
    // the command-line boundary; the engine sees only the constant.
    const auto language = scratchbird::core::catalog::CatalogLanguageForTag(request.language_tag);
    if (!language) {
        clearSensitive(credential_fingerprint);
        clearSensitive(service_fingerprint);
        return bootstrapFailure("BOOTSTRAP.LANGUAGE_UNKNOWN");
    }

    // The native create through the engine API. This command stands in for the
    // hosting server: the policy pack root it was given is the server's
    // configuration (owner Q104 = 2), and its own invocation is the security
    // context (Q95). The engine mints every identity. --resource-seed-pack-root
    // and the metric policy files have no native counterpart and are not
    // passed. (Was: CreateDatabaseFile, legacy SBDBV001, with those inputs.)
    api::EngineCreateLifecycleRequest create_request;
    create_request.context.request_id = "sbcli.first_principal_bootstrap";
    create_request.context.database_path = request.database_path;
    create_request.context.security_context_present = true;
    create_request.context.server_policy_seed_pack_root = request.policy_seed_pack_root;
    create_request.database_format = api::EngineDatabaseFormat::native;
    create_request.database_name = request.database_name.empty()
        ? std::filesystem::path(request.database_path).stem().string()
        : request.database_name;
    create_request.database_language = *language;
    create_request.option_envelopes.push_back("page_size:16384");
    create_request.option_envelopes.push_back("total_pages:" + std::to_string(request.total_pages));
    create_request.option_envelopes.push_back("bootstrap_principal_name:" + request.principal_name);
    create_request.option_envelopes.push_back("bootstrap_credential_fingerprint:" + credential_fingerprint);
    create_request.option_envelopes.push_back("internal_service_principal_name:" + request.service_principal_name);
    create_request.option_envelopes.push_back("internal_service_credential_fingerprint:" + service_fingerprint);
    const auto created = api::EngineCreateLifecycle(create_request);
    for (auto& option : create_request.option_envelopes) clearSensitive(option);
    clearSensitive(credential_fingerprint);
    clearSensitive(service_fingerprint);
    if (!created.ok) {
        return bootstrapFailure(created.diagnostics.empty() || created.diagnostics.front().code.empty()
                                    ? std::string(kBootstrapInputInvalid)
                                    : created.diagnostics.front().code);
    }

    if (service_handoff) {
        const auto finalized_access = EnsureBootstrapServiceIdentityAccess(
            loaded_profile.profile, request.database_path);
        if (!finalized_access.ok || !finalized_access.ownership_handoff_ready) {
            auto result = bootstrapFailure("BOOTSTRAP.DIRECTORY_PERMISSION_INVALID");
            result.committed_with_warning = true;
            return result;
        }
    }

    // The receipt's identities (binary on the wire; rendered here, at the
    // command-line boundary). What the native receipt does NOT carry, and this
    // command no longer reports or checks: the sysarch role UUID, a policy
    // generation, a "verified_and_closed" handoff state. The native create
    // writes, verifies by read-back and closes inside the engine; a complete
    // receipt is the handoff.
    const auto receipt_uuid = [&](std::string_view key, std::string* out) {
        for (const auto& row : created.result_shape.rows) {
            for (const auto& field : row.fields) {
                if (field.first != key) continue;
                if (field.second.isSqlNull() || field.second.binary_value.size() != 16) return false;
                scratchbird::core::platform::Uuid id;
                std::copy(field.second.binary_value.begin(), field.second.binary_value.end(), id.bytes.begin());
                if (!uuid::IsEngineIdentityUuid(id)) return false;
                *out = uuid::UuidToString(id);
                return true;
            }
        }
        return false;
    };
    FirstPrincipalBootstrapResult result;
    if (!receipt_uuid("database_uuid", &result.database_uuid) ||
        !receipt_uuid("filespace_uuid", &result.filespace_uuid) ||
        !receipt_uuid("bootstrap_principal_uuid", &result.principal_uuid) ||
        !receipt_uuid("service_principal_uuid", &result.service_principal_uuid) ||
        !receipt_uuid("construction_transaction_uuid", &result.construction_transaction_uuid)) {
        return bootstrapFailure("BOOTSTRAP.CREATE_RECEIPT_INCOMPLETE");
    }
    result.ok = true;
    result.creation_handoff_ready = true;
    result.committed_with_warning = false;
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
        if (result.committed_with_warning) {
            error << "The database committed but requires verification; it was preserved.\n";
        }
        return 1;
    }

    output << "First security principal created and committed by the native "
              "construction transaction\n";
    output << "database_uuid=" << result.database_uuid << "\n";
    output << "filespace_uuid=" << result.filespace_uuid << "\n";
    output << "bootstrap_principal_uuid=" << result.principal_uuid << "\n";
    output << "service_principal_uuid=" << result.service_principal_uuid << "\n";
    output << "construction_transaction_uuid=" << result.construction_transaction_uuid << "\n";
    output << "bootstrap_scope=initial_local_embedded_database_security_tree\n";
    output << "database_format=native\n"
              "database_state=created\n"
              "creation_handoff=constructed_and_closed\n"
              "server_ready=false\n";
    if (result.committed_with_warning) {
        output << "Create finality: committed_with_warning\n";
    }
    return 0;
}

}  // namespace scratchbird::cli
