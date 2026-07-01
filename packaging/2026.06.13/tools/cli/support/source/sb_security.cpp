// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

/**
 * sb_security - ScratchBird Security Administration Tool
 *
 * CLI Tools - Manage users, roles, and security settings.
 *
 * Usage:
 *   sb_security <command> <database> [options]
 *
 * User Management Commands:
 *   user list                        List all users
 *   user create <username>           Create a new user
 *   user delete <username>           Delete a user
 *   user password <username>         Change user password
 *   user enable <username>           Enable a disabled user
 *   user disable <username>          Disable a user
 *   user info <username>             Show user details
 *   user unlock <username>           Unlock a locked user account
 *
 * Role Management Commands:
 *   role list                        List all roles
 *   role create <rolename>           Create a new role
 *   role delete <rolename>           Delete a role
 *   role grant <role> <user>         Grant role to user
 *   role revoke <role> <user>        Revoke role from user
 *   role members <rolename>          List role members
 *   role grants <rolename>           List role privileges
 *
 * Permission Commands:
 *   grant <privilege> on <object> to <user/role>
 *   revoke <privilege> on <object> from <user/role>
 *   show grants for <user/role>
 *   show grants on <object>
 *
 * Audit Commands:
 *   audit status                     Show audit configuration
 *   audit enable [category]          Enable auditing
 *   audit disable [category]         Disable auditing
 *   audit log [filter]               View audit log
 *
 * Security Checks:
 *   check                            Run security assessment
 *   check passwords                  Check password strength
 *   check permissions                Check permission issues
 *   check audit                      Check audit configuration
 *
 * Options:
 *   -U, --user=<username>    Admin username
 *   -P, --password=<pass>    Admin password
 *   -p, --port=<n>           TCP port (default: 3092)
 *   -v, --verbose            Verbose output
 *   -q, --quiet              Only show errors
 *   --json                   JSON output format
 *   -h, --help               Show this help
 *   --version                Show version
 */

#include <iostream>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <iomanip>
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

#include "cli_auth_bootstrap.h"
#include "scratchbird/client/connection.h"
#include "scratchbird/core/status.h"
#include "scratchbird/core/error_context.h"

using namespace scratchbird;
using namespace scratchbird::client;

// =============================================================================
// Configuration
// =============================================================================

enum class SecurityCommand {
    NONE,
    // User commands
    USER_LIST,
    USER_CREATE,
    USER_DELETE,
    USER_PASSWORD,
    USER_ENABLE,
    USER_DISABLE,
    USER_INFO,
    USER_UNLOCK,
    // Role commands
    ROLE_LIST,
    ROLE_CREATE,
    ROLE_DELETE,
    ROLE_GRANT,
    ROLE_REVOKE,
    ROLE_MEMBERS,
    ROLE_GRANTS,
    // Permission commands
    GRANT,
    REVOKE_PERM,
    SHOW_GRANTS_USER,
    SHOW_GRANTS_OBJECT,
    // Audit commands
    AUDIT_STATUS,
    AUDIT_ENABLE,
    AUDIT_DISABLE,
    AUDIT_LOG,
    // Security checks
    CHECK_ALL,
    CHECK_PASSWORDS,
    CHECK_PERMISSIONS,
    CHECK_AUDIT
};

struct SecurityConfig {
    SecurityCommand command = SecurityCommand::NONE;
    std::string database_path;
    std::string admin_user;
    std::string admin_password;
    std::string host = "localhost";
    std::string connection_string;
    std::string mode = "inet_listener";  // embedded|local_ipc|inet_listener|managed
    std::string ipc_method = "auto";     // auto|unix|pipe|tcp
    std::string ipc_path;
    std::string front_door_mode = "direct";
    std::string manager_auth_token;
    std::string manager_username;
    std::string manager_database;
    std::string manager_connection_profile = "SBsql";
    std::string manager_client_intent = "SBsql";
    std::string manager_client_flags;
    std::string manager_auth_fast_path;
    std::string connect_client_flags = "256";
    std::string auth_method_id;
    std::string auth_token;
    std::string auth_method_payload;
    std::string auth_payload_json;
    std::string auth_payload_b64;
    std::string auth_provider_profile;
    std::string auth_required_methods;
    std::string auth_forbidden_methods;
    std::string auth_require_channel_binding;
    std::string workload_identity_token;
    std::string proxy_principal_assertion;
    std::string ssl_mode;
    std::vector<std::pair<std::string, std::string>> conn_options;
    uint16_t port = 3092;

    // Command arguments
    std::string username;
    std::string rolename;
    std::string privilege;
    std::string object;
    std::string target;  // user/role for grant/revoke

    // Options
    bool verbose = false;
    bool quiet = false;
    bool json = false;
    bool probe_auth_surface = false;
    bool show_auth_context = false;
};

// =============================================================================
// Global state
// =============================================================================

static SecurityConfig g_config;
static std::unique_ptr<Connection> g_connection;

// =============================================================================
// Output helpers
// =============================================================================

void log(const std::string& msg) {
    if (!g_config.quiet) {
        std::cout << msg << "\n";
    }
}

void logVerbose(const std::string& msg) {
    if (g_config.verbose && !g_config.quiet) {
        std::cout << "  " << msg << "\n";
    }
}

void printError(const std::string& msg) {
    std::cerr << "Error: " << msg << "\n";
}

// =============================================================================
// Password input (no echo)
// =============================================================================

std::string readPassword(const std::string& prompt) {
    std::cout << prompt;
    std::cout.flush();

#ifdef _WIN32
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD old_mode = 0;
    const bool restore_mode =
        input != INVALID_HANDLE_VALUE && GetConsoleMode(input, &old_mode);
    if (restore_mode) {
        SetConsoleMode(input, old_mode & ~ENABLE_ECHO_INPUT);
    }

    std::string password;
    std::getline(std::cin, password);

    if (restore_mode) {
        SetConsoleMode(input, old_mode);
    }
#else
    struct termios old_term, new_term;
    tcgetattr(STDIN_FILENO, &old_term);
    new_term = old_term;
    new_term.c_lflag &= ~(ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &new_term);

    std::string password;
    std::getline(std::cin, password);

    tcsetattr(STDIN_FILENO, TCSANOW, &old_term);
#endif
    std::cout << "\n";

    return password;
}

std::string normalizeConnectionMode(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    if (value == "embedded" || value == "inproc" || value == "in-process" || value == "in_process") {
        return "embedded";
    }
    if (value == "local" || value == "ipc" || value == "local-ipc" || value == "local_ipc" ||
        value == "unix" || value == "pipe") {
        return "local_ipc";
    }
    if (value == "inet" || value == "listener" || value == "inet_listener" ||
        value == "tcp" || value == "network" || value.empty()) {
        return "inet_listener";
    }
    if (value == "managed" || value == "manager" || value == "manager_proxy" || value == "manager-proxy") {
        return "managed";
    }
    return {};
}

bool splitConnOption(const std::string& value, std::string& key, std::string& out_value) {
    size_t eq = value.find('=');
    if (eq == std::string::npos || eq == 0 || eq + 1 >= value.size()) {
        return false;
    }
    key = value.substr(0, eq);
    out_value = value.substr(eq + 1);
    return !key.empty();
}

std::string encodeConnectionValue(const std::string& value) {
    if (value.find(';') != std::string::npos || value.find(' ') != std::string::npos) {
        return "{" + value + "}";
    }
    return value;
}

void appendConnParam(std::vector<std::pair<std::string, std::string>>& params,
                     const std::string& key,
                     const std::string& value) {
    if (!key.empty() && !value.empty()) {
        params.emplace_back(key, value);
    }
}

bool isLikelyConnectionString(const std::string& value) {
    if (value.rfind("scratchbird://", 0) == 0) {
        return true;
    }
    return value.find('=') != std::string::npos && value.find(';') != std::string::npos;
}

std::string buildConnectionTarget() {
    scratchbird::cli::ConnectionBootstrapOptions options;
    options.database_path = g_config.database_path;
    options.connection_string = g_config.connection_string;
    options.mode = g_config.mode;
    options.ipc_method = g_config.ipc_method;
    options.ipc_path = g_config.ipc_path;
    options.front_door_mode = g_config.front_door_mode;
    options.host = g_config.host;
    options.port = g_config.port;
    options.manager_auth_token = g_config.manager_auth_token;
    options.manager_username = g_config.manager_username;
    options.manager_database = g_config.manager_database;
    options.manager_connection_profile = g_config.manager_connection_profile;
    options.manager_client_intent = g_config.manager_client_intent;
    options.manager_client_flags = g_config.manager_client_flags;
    options.manager_auth_fast_path = g_config.manager_auth_fast_path;
    options.connect_client_flags = g_config.connect_client_flags;
    options.auth_method_id = g_config.auth_method_id;
    options.auth_token = g_config.auth_token;
    options.auth_method_payload = g_config.auth_method_payload;
    options.auth_payload_json = g_config.auth_payload_json;
    options.auth_payload_b64 = g_config.auth_payload_b64;
    options.auth_provider_profile = g_config.auth_provider_profile;
    options.auth_required_methods = g_config.auth_required_methods;
    options.auth_forbidden_methods = g_config.auth_forbidden_methods;
    options.auth_require_channel_binding = g_config.auth_require_channel_binding;
    options.workload_identity_token = g_config.workload_identity_token;
    options.proxy_principal_assertion = g_config.proxy_principal_assertion;
    options.ssl_mode = g_config.ssl_mode;
    options.conn_options = g_config.conn_options;
    return scratchbird::cli::buildConnectionTarget(options);
}

// =============================================================================
// SQL execution helper
// =============================================================================

bool executeSQL(const std::string& sql, ResultSet* results = nullptr) {
    core::ErrorContext ctx;

    if (results) {
        core::Status status = g_connection->executeQuery(sql, results, &ctx);
        if (status != core::Status::OK) {
            printError(ctx.message);
            return false;
        }
    } else {
        int64_t affected;
        core::Status status = g_connection->execute(sql, &affected, &ctx);
        if (status != core::Status::OK) {
            printError(ctx.message);
            return false;
        }
    }
    return true;
}

// =============================================================================
// Result display
// =============================================================================

void printResults(ResultSet& results) {
    size_t col_count = results.getColumnCount();
    if (col_count == 0) return;

    // Collect column names
    std::vector<std::string> columns(col_count);
    std::vector<size_t> widths(col_count);
    for (size_t i = 0; i < col_count; ++i) {
        columns[i] = results.getColumnName(i);
        widths[i] = columns[i].size();
    }

    // Collect rows
    std::vector<std::vector<std::string>> rows;
    while (results.next()) {
        std::vector<std::string> row(col_count);
        for (size_t i = 0; i < col_count; ++i) {
            row[i] = results.isNull(i) ? "(null)" : results.getString(i);
            widths[i] = std::max(widths[i], row[i].size());
        }
        rows.push_back(std::move(row));
    }

    // Print header
    if (!g_config.json) {
        for (size_t i = 0; i < col_count; ++i) {
            std::cout << std::left << std::setw(static_cast<int>(widths[i] + 2)) << columns[i];
        }
        std::cout << "\n";

        for (size_t i = 0; i < col_count; ++i) {
            for (size_t j = 0; j < widths[i]; ++j) std::cout << "-";
            std::cout << "  ";
        }
        std::cout << "\n";

        // Print rows
        for (const auto& row : rows) {
            for (size_t i = 0; i < col_count; ++i) {
                std::cout << std::left << std::setw(static_cast<int>(widths[i] + 2)) << row[i];
            }
            std::cout << "\n";
        }

        std::cout << "\n(" << rows.size() << " row" << (rows.size() == 1 ? "" : "s") << ")\n";
    } else {
        // JSON output
        std::cout << "[\n";
        for (size_t r = 0; r < rows.size(); ++r) {
            std::cout << "  {";
            for (size_t i = 0; i < col_count; ++i) {
                if (i > 0) std::cout << ", ";
                std::cout << "\"" << columns[i] << "\": \"" << rows[r][i] << "\"";
            }
            std::cout << "}" << (r < rows.size() - 1 ? "," : "") << "\n";
        }
        std::cout << "]\n";
    }
}

// =============================================================================
// User management
// =============================================================================

bool userList() {
    ResultSet results;
    if (!executeSQL("SELECT name, is_admin, enabled, locked, created_at FROM sys_users ORDER BY name", &results)) {
        return false;
    }
    printResults(results);
    return true;
}

bool userCreate() {
    std::string password = readPassword("Enter password for " + g_config.username + ": ");
    std::string confirm = readPassword("Confirm password: ");

    if (password != confirm) {
        printError("Passwords do not match");
        return false;
    }

    std::string sql = "CREATE USER '" + g_config.username + "' WITH PASSWORD '" + password + "'";
    if (!executeSQL(sql)) {
        return false;
    }

    log("User '" + g_config.username + "' created successfully");
    return true;
}

bool userDelete() {
    std::cout << "Are you sure you want to delete user '" << g_config.username << "'? (y/N): ";
    std::string response;
    std::getline(std::cin, response);

    if (response != "y" && response != "Y") {
        log("Cancelled");
        return true;
    }

    std::string sql = "DROP USER '" + g_config.username + "'";
    if (!executeSQL(sql)) {
        return false;
    }

    log("User '" + g_config.username + "' deleted");
    return true;
}

bool userPassword() {
    std::string password = readPassword("Enter new password: ");
    std::string confirm = readPassword("Confirm password: ");

    if (password != confirm) {
        printError("Passwords do not match");
        return false;
    }

    std::string sql = "ALTER USER '" + g_config.username + "' SET PASSWORD '" + password + "'";
    if (!executeSQL(sql)) {
        return false;
    }

    log("Password changed for user '" + g_config.username + "'");
    return true;
}

bool userEnable() {
    std::string sql = "ALTER USER '" + g_config.username + "' ENABLE";
    if (!executeSQL(sql)) {
        return false;
    }
    log("User '" + g_config.username + "' enabled");
    return true;
}

bool userDisable() {
    std::string sql = "ALTER USER '" + g_config.username + "' DISABLE";
    if (!executeSQL(sql)) {
        return false;
    }
    log("User '" + g_config.username + "' disabled");
    return true;
}

bool userInfo() {
    ResultSet results;
    std::string sql = "SELECT * FROM sys_users WHERE name = '" + g_config.username + "'";
    if (!executeSQL(sql, &results)) {
        return false;
    }

    if (!results.next()) {
        printError("User not found: " + g_config.username);
        return false;
    }

    std::cout << "User Information:\n";
    std::cout << "  Name:       " << results.getString("name") << "\n";
    std::cout << "  Admin:      " << (results.getBool("is_admin") ? "Yes" : "No") << "\n";
    std::cout << "  Enabled:    " << (results.getBool("enabled") ? "Yes" : "No") << "\n";
    std::cout << "  Locked:     " << (results.getBool("locked") ? "Yes" : "No") << "\n";
    std::cout << "  Created:    " << results.getString("created_at") << "\n";

    return true;
}

bool userUnlock() {
    std::string sql = "ALTER USER '" + g_config.username + "' UNLOCK";
    if (!executeSQL(sql)) {
        return false;
    }
    log("User '" + g_config.username + "' unlocked");
    return true;
}

// =============================================================================
// Role management
// =============================================================================

bool roleList() {
    ResultSet results;
    if (!executeSQL("SELECT name, created_at FROM sys_roles ORDER BY name", &results)) {
        return false;
    }
    printResults(results);
    return true;
}

bool roleCreate() {
    std::string sql = "CREATE ROLE '" + g_config.rolename + "'";
    if (!executeSQL(sql)) {
        return false;
    }
    log("Role '" + g_config.rolename + "' created");
    return true;
}

bool roleDelete() {
    std::string sql = "DROP ROLE '" + g_config.rolename + "'";
    if (!executeSQL(sql)) {
        return false;
    }
    log("Role '" + g_config.rolename + "' deleted");
    return true;
}

bool roleGrant() {
    std::string sql = "GRANT ROLE '" + g_config.rolename + "' TO '" + g_config.username + "'";
    if (!executeSQL(sql)) {
        return false;
    }
    log("Role '" + g_config.rolename + "' granted to '" + g_config.username + "'");
    return true;
}

bool roleRevoke() {
    std::string sql = "REVOKE ROLE '" + g_config.rolename + "' FROM '" + g_config.username + "'";
    if (!executeSQL(sql)) {
        return false;
    }
    log("Role '" + g_config.rolename + "' revoked from '" + g_config.username + "'");
    return true;
}

bool roleMembers() {
    ResultSet results;
    std::string sql = "SELECT user_name FROM sys_role_members WHERE role_name = '" + g_config.rolename + "'";
    if (!executeSQL(sql, &results)) {
        return false;
    }
    printResults(results);
    return true;
}

bool roleGrants() {
    ResultSet results;
    std::string sql = "SELECT privilege, object_type, object_name FROM sys_privileges WHERE grantee = '" + g_config.rolename + "'";
    if (!executeSQL(sql, &results)) {
        return false;
    }
    printResults(results);
    return true;
}

// =============================================================================
// Audit management
// =============================================================================

bool auditStatus() {
    log("Audit Configuration:");

    ResultSet results;
    if (!executeSQL("SELECT * FROM sys_audit_config", &results)) {
        return false;
    }
    printResults(results);
    return true;
}

bool auditEnable() {
    std::string sql = "ALTER SYSTEM SET audit_enabled = true";
    if (!executeSQL(sql)) {
        return false;
    }
    log("Auditing enabled");
    return true;
}

bool auditDisable() {
    std::string sql = "ALTER SYSTEM SET audit_enabled = false";
    if (!executeSQL(sql)) {
        return false;
    }
    log("Auditing disabled");
    return true;
}

bool auditLog() {
    ResultSet results;
    std::string sql = "SELECT timestamp, username, action, object, result FROM sys_audit_log ORDER BY timestamp DESC LIMIT 100";
    if (!executeSQL(sql, &results)) {
        return false;
    }
    printResults(results);
    return true;
}

// =============================================================================
// Security checks
// =============================================================================

bool checkAll() {
    log("Running security assessment...\n");

    int warnings = 0;
    int errors = 0;

    // Check for users without passwords
    log("Checking user accounts...");
    ResultSet users;
    if (executeSQL("SELECT name FROM sys_users WHERE password_hash IS NULL OR password_hash = ''", &users)) {
        while (users.next()) {
            std::cout << "  WARNING: User '" << users.getString(0) << "' has no password\n";
            warnings++;
        }
    }

    // Check for disabled admin accounts
    ResultSet admins;
    if (executeSQL("SELECT name FROM sys_users WHERE is_admin = true AND enabled = false", &admins)) {
        while (admins.next()) {
            std::cout << "  INFO: Admin account '" << admins.getString(0) << "' is disabled\n";
        }
    }

    // Check for locked accounts
    ResultSet locked;
    if (executeSQL("SELECT name FROM sys_users WHERE locked = true", &locked)) {
        while (locked.next()) {
            std::cout << "  INFO: Account '" << locked.getString(0) << "' is locked\n";
        }
    }

    // Check audit configuration
    log("\nChecking audit configuration...");
    ResultSet audit;
    if (executeSQL("SELECT value FROM sys_settings WHERE name = 'audit_enabled'", &audit)) {
        if (audit.next()) {
            if (audit.getString(0) != "true") {
                std::cout << "  WARNING: Auditing is disabled\n";
                warnings++;
            } else {
                std::cout << "  OK: Auditing is enabled\n";
            }
        }
    }

    // Summary
    log("\n========================================");
    log("Security Assessment Summary:");
    log("  Warnings: " + std::to_string(warnings));
    log("  Errors:   " + std::to_string(errors));

    if (errors > 0) {
        log("\nStatus: FAILED - Security issues require immediate attention");
        return false;
    } else if (warnings > 0) {
        log("\nStatus: WARNINGS - Review recommended");
    } else {
        log("\nStatus: PASSED - No security issues found");
    }
    log("========================================");

    return true;
}

// =============================================================================
// Connection management
// =============================================================================

bool connectToDatabase() {
    auto candidate = std::make_unique<Connection>();

    std::string conn_target = buildConnectionTarget();
    core::ErrorContext ctx;
    core::Status status = candidate->connect(conn_target, g_config.admin_user, g_config.admin_password, &ctx);

    if (status != core::Status::OK) {
        printError("Connection failed: " + ctx.message);
        return false;
    }
    if (g_config.show_auth_context) {
        const std::string context =
            g_config.json
                ? scratchbird::cli::renderResolvedAuthContextJson(candidate->getResolvedAuthContext())
                : scratchbird::cli::renderResolvedAuthContextText(candidate->getResolvedAuthContext());
        std::cerr << context << "\n";
    }
    g_connection = std::move(candidate);

    logVerbose("Connected to " + g_config.database_path);
    return true;
}

void disconnectFromDatabase() {
    if (g_connection) {
        g_connection->disconnect();
        g_connection.reset();
    }
}

// =============================================================================
// Command dispatch
// =============================================================================

bool executeCommand() {
    switch (g_config.command) {
        case SecurityCommand::USER_LIST:      return userList();
        case SecurityCommand::USER_CREATE:    return userCreate();
        case SecurityCommand::USER_DELETE:    return userDelete();
        case SecurityCommand::USER_PASSWORD:  return userPassword();
        case SecurityCommand::USER_ENABLE:    return userEnable();
        case SecurityCommand::USER_DISABLE:   return userDisable();
        case SecurityCommand::USER_INFO:      return userInfo();
        case SecurityCommand::USER_UNLOCK:    return userUnlock();

        case SecurityCommand::ROLE_LIST:      return roleList();
        case SecurityCommand::ROLE_CREATE:    return roleCreate();
        case SecurityCommand::ROLE_DELETE:    return roleDelete();
        case SecurityCommand::ROLE_GRANT:     return roleGrant();
        case SecurityCommand::ROLE_REVOKE:    return roleRevoke();
        case SecurityCommand::ROLE_MEMBERS:   return roleMembers();
        case SecurityCommand::ROLE_GRANTS:    return roleGrants();

        case SecurityCommand::AUDIT_STATUS:   return auditStatus();
        case SecurityCommand::AUDIT_ENABLE:   return auditEnable();
        case SecurityCommand::AUDIT_DISABLE:  return auditDisable();
        case SecurityCommand::AUDIT_LOG:      return auditLog();

        case SecurityCommand::CHECK_ALL:
        case SecurityCommand::CHECK_PASSWORDS:
        case SecurityCommand::CHECK_PERMISSIONS:
        case SecurityCommand::CHECK_AUDIT:
            return checkAll();

        default:
            printError("Unknown command");
            return false;
    }
}

// =============================================================================
// Argument parsing
// =============================================================================

void printUsage(const char* program) {
    std::cout << "ScratchBird Security Administration Tool\n\n";
    std::cout << "Usage:\n";
    std::cout << "  " << program << " <command> <database> [options]\n\n";
    std::cout << "User Commands:\n";
    std::cout << "  user list                    List all users\n";
    std::cout << "  user create <username>       Create a new user\n";
    std::cout << "  user delete <username>       Delete a user\n";
    std::cout << "  user password <username>     Change user password\n";
    std::cout << "  user enable <username>       Enable a user\n";
    std::cout << "  user disable <username>      Disable a user\n";
    std::cout << "  user info <username>         Show user details\n";
    std::cout << "  user unlock <username>       Unlock a locked account\n\n";
    std::cout << "Role Commands:\n";
    std::cout << "  role list                    List all roles\n";
    std::cout << "  role create <rolename>       Create a new role\n";
    std::cout << "  role delete <rolename>       Delete a role\n";
    std::cout << "  role grant <role> <user>     Grant role to user\n";
    std::cout << "  role revoke <role> <user>    Revoke role from user\n";
    std::cout << "  role members <rolename>      List role members\n";
    std::cout << "  role grants <rolename>       List role privileges\n\n";
    std::cout << "Audit Commands:\n";
    std::cout << "  audit status                 Show audit configuration\n";
    std::cout << "  audit enable                 Enable auditing\n";
    std::cout << "  audit disable                Disable auditing\n";
    std::cout << "  audit log                    View audit log\n\n";
    std::cout << "Security Checks:\n";
    std::cout << "  check                        Run security assessment\n\n";
    std::cout << "Options:\n";
    std::cout << "  -U, --user=<username>  Admin username\n";
    std::cout << "  -P, --password=<pass>  Admin password\n";
    std::cout << "  -H, --host=<host>      Host (default: localhost)\n";
    std::cout << "  -p, --port=<n>         TCP port (default: 3092)\n";
    std::cout << "      --connection=<str> Full driver connection string\n";
    std::cout << "      --mode=<m>         embedded|local-ipc|inet|managed\n";
    std::cout << "      --ipc-method=<m>   auto|unix|pipe|tcp\n";
    std::cout << "      --ipc-path=<path>  Local IPC socket/pipe path\n";
    std::cout << "      --front-door-mode=<m> direct|manager_proxy\n";
    std::cout << "      --manager-auth-token=<t> Manager auth token\n";
    std::cout << "      --manager-user=<name> Manager user\n";
    std::cout << "      --manager-db=<name> Manager target database\n";
    std::cout << "      --client-flags=<n> Startup client flags (default: 256)\n";
    std::cout << "      --auth-method-id=<id> Auth plugin method id\n";
    std::cout << "      --auth-token=<tok> Generic token-auth payload\n";
    std::cout << "      --auth-method-payload=<v> Auth plugin opaque payload\n";
    std::cout << "      --auth-payload-json=<j> Auth plugin JSON payload\n";
    std::cout << "      --auth-payload-b64=<b64> Auth plugin base64 payload\n";
    std::cout << "      --auth-provider-profile=<p> Auth provider profile\n";
    std::cout << "      --auth-required-methods=<csv> Required auth methods\n";
    std::cout << "      --auth-forbidden-methods=<csv> Forbidden auth methods\n";
    std::cout << "      --auth-require-channel-binding=<bool> Require channel binding\n";
    std::cout << "      --workload-identity-token=<tok> Workload identity token\n";
    std::cout << "      --proxy-principal-assertion=<tok> Proxy principal assertion\n";
    std::cout << "      --probe-auth-surface Probe staged auth/bootstrap and exit\n";
    std::cout << "      --show-auth-context Print resolved auth context after connect\n";
    std::cout << "      --sslmode=<mode>   disable|allow|prefer|require|verify-ca|verify-full\n";
    std::cout << "      --conn-opt key=value Extra connection option (repeatable)\n";
    std::cout << "  -v, --verbose          Verbose output\n";
    std::cout << "  -q, --quiet            Only show errors\n";
    std::cout << "      --json             JSON output format\n";
    std::cout << "  -h, --help             Show this help\n";
    std::cout << "      --version          Show version\n\n";
    std::cout << "Examples:\n";
    std::cout << "  " << program << " user list mydb.sbdb\n";
    std::cout << "  " << program << " user create admin mydb.sbdb -U root\n";
    std::cout << "  " << program << " check mydb.sbdb -v\n";
}

void printVersion() {
    std::cout << "sb_security (ScratchBird Security Administration) 0.1.0\n";
}

bool parseArgs(int argc, char* argv[]) {
    if (argc < 3) {
        return false;
    }

    int arg_index = 1;
    std::string category = argv[arg_index++];

    // Parse command category
    if (category == "user") {
        if (arg_index >= argc) {
            printError("Missing user subcommand");
            return false;
        }
        std::string subcmd = argv[arg_index++];

        if (subcmd == "list") {
            g_config.command = SecurityCommand::USER_LIST;
        } else if (subcmd == "create" || subcmd == "delete" || subcmd == "password" ||
                   subcmd == "enable" || subcmd == "disable" || subcmd == "info" || subcmd == "unlock") {
            if (arg_index >= argc) {
                printError("Missing username");
                return false;
            }
            g_config.username = argv[arg_index++];

            if (subcmd == "create") g_config.command = SecurityCommand::USER_CREATE;
            else if (subcmd == "delete") g_config.command = SecurityCommand::USER_DELETE;
            else if (subcmd == "password") g_config.command = SecurityCommand::USER_PASSWORD;
            else if (subcmd == "enable") g_config.command = SecurityCommand::USER_ENABLE;
            else if (subcmd == "disable") g_config.command = SecurityCommand::USER_DISABLE;
            else if (subcmd == "info") g_config.command = SecurityCommand::USER_INFO;
            else if (subcmd == "unlock") g_config.command = SecurityCommand::USER_UNLOCK;
        } else {
            printError("Unknown user subcommand: " + subcmd);
            return false;
        }
    } else if (category == "role") {
        if (arg_index >= argc) {
            printError("Missing role subcommand");
            return false;
        }
        std::string subcmd = argv[arg_index++];

        if (subcmd == "list") {
            g_config.command = SecurityCommand::ROLE_LIST;
        } else if (subcmd == "create" || subcmd == "delete" || subcmd == "members" || subcmd == "grants") {
            if (arg_index >= argc) {
                printError("Missing rolename");
                return false;
            }
            g_config.rolename = argv[arg_index++];

            if (subcmd == "create") g_config.command = SecurityCommand::ROLE_CREATE;
            else if (subcmd == "delete") g_config.command = SecurityCommand::ROLE_DELETE;
            else if (subcmd == "members") g_config.command = SecurityCommand::ROLE_MEMBERS;
            else if (subcmd == "grants") g_config.command = SecurityCommand::ROLE_GRANTS;
        } else if (subcmd == "grant" || subcmd == "revoke") {
            if (arg_index + 1 >= argc) {
                printError("Usage: role " + subcmd + " <role> <user>");
                return false;
            }
            g_config.rolename = argv[arg_index++];
            g_config.username = argv[arg_index++];

            if (subcmd == "grant") g_config.command = SecurityCommand::ROLE_GRANT;
            else g_config.command = SecurityCommand::ROLE_REVOKE;
        } else {
            printError("Unknown role subcommand: " + subcmd);
            return false;
        }
    } else if (category == "audit") {
        if (arg_index >= argc) {
            printError("Missing audit subcommand");
            return false;
        }
        std::string subcmd = argv[arg_index++];

        if (subcmd == "status") g_config.command = SecurityCommand::AUDIT_STATUS;
        else if (subcmd == "enable") g_config.command = SecurityCommand::AUDIT_ENABLE;
        else if (subcmd == "disable") g_config.command = SecurityCommand::AUDIT_DISABLE;
        else if (subcmd == "log") g_config.command = SecurityCommand::AUDIT_LOG;
        else {
            printError("Unknown audit subcommand: " + subcmd);
            return false;
        }
    } else if (category == "check") {
        g_config.command = SecurityCommand::CHECK_ALL;
    } else if (category == "-h" || category == "--help") {
        printUsage(argv[0]);
        std::exit(0);
    } else if (category == "--version") {
        printVersion();
        std::exit(0);
    } else {
        printError("Unknown command category: " + category);
        return false;
    }

    // Next argument is database path unless options start immediately.
    if (arg_index < argc && argv[arg_index][0] != '-') {
        g_config.database_path = argv[arg_index++];
    }

    // Parse remaining options
    for (; arg_index < argc; ++arg_index) {
        std::string arg = argv[arg_index];

        if (arg == "-U" && arg_index + 1 < argc) {
            g_config.admin_user = argv[++arg_index];
        } else if (arg.find("--user=") == 0) {
            g_config.admin_user = arg.substr(7);
        } else if (arg == "-P" && arg_index + 1 < argc) {
            g_config.admin_password = argv[++arg_index];
        } else if (arg.find("--password=") == 0) {
            g_config.admin_password = arg.substr(11);
        } else if (arg == "-H" && arg_index + 1 < argc) {
            g_config.host = argv[++arg_index];
        } else if (arg.find("--host=") == 0) {
            g_config.host = arg.substr(7);
        } else if (arg == "-p" && arg_index + 1 < argc) {
            g_config.port = static_cast<uint16_t>(std::stoul(argv[++arg_index]));
        } else if (arg.find("--port=") == 0) {
            g_config.port = static_cast<uint16_t>(std::stoul(arg.substr(7)));
        } else if (arg == "--connection" && arg_index + 1 < argc) {
            g_config.connection_string = argv[++arg_index];
        } else if (arg.rfind("--connection=", 0) == 0) {
            g_config.connection_string = arg.substr(13);
        } else if (arg == "--mode" && arg_index + 1 < argc) {
            g_config.mode = argv[++arg_index];
        } else if (arg.rfind("--mode=", 0) == 0) {
            g_config.mode = arg.substr(7);
        } else if (arg == "--ipc-method" && arg_index + 1 < argc) {
            g_config.ipc_method = argv[++arg_index];
        } else if (arg.rfind("--ipc-method=", 0) == 0) {
            g_config.ipc_method = arg.substr(13);
        } else if (arg == "--ipc-path" && arg_index + 1 < argc) {
            g_config.ipc_path = argv[++arg_index];
        } else if (arg.rfind("--ipc-path=", 0) == 0) {
            g_config.ipc_path = arg.substr(11);
        } else if (arg == "--front-door-mode" && arg_index + 1 < argc) {
            g_config.front_door_mode = argv[++arg_index];
        } else if (arg.rfind("--front-door-mode=", 0) == 0) {
            g_config.front_door_mode = arg.substr(18);
        } else if (arg == "--manager-auth-token" && arg_index + 1 < argc) {
            g_config.manager_auth_token = argv[++arg_index];
        } else if (arg.rfind("--manager-auth-token=", 0) == 0) {
            g_config.manager_auth_token = arg.substr(21);
        } else if (arg == "--manager-user" && arg_index + 1 < argc) {
            g_config.manager_username = argv[++arg_index];
        } else if (arg.rfind("--manager-user=", 0) == 0) {
            g_config.manager_username = arg.substr(15);
        } else if (arg == "--manager-db" && arg_index + 1 < argc) {
            g_config.manager_database = argv[++arg_index];
        } else if (arg.rfind("--manager-db=", 0) == 0) {
            g_config.manager_database = arg.substr(13);
        } else if (arg == "--client-flags" && arg_index + 1 < argc) {
            g_config.connect_client_flags = argv[++arg_index];
        } else if (arg.rfind("--client-flags=", 0) == 0) {
            g_config.connect_client_flags = arg.substr(15);
        } else if (arg == "--auth-method-id" && arg_index + 1 < argc) {
            g_config.auth_method_id = argv[++arg_index];
        } else if (arg.rfind("--auth-method-id=", 0) == 0) {
            g_config.auth_method_id = arg.substr(17);
        } else if (arg == "--auth-token" && arg_index + 1 < argc) {
            g_config.auth_token = argv[++arg_index];
        } else if (arg.rfind("--auth-token=", 0) == 0) {
            g_config.auth_token = arg.substr(13);
        } else if (arg == "--auth-method-payload" && arg_index + 1 < argc) {
            g_config.auth_method_payload = argv[++arg_index];
        } else if (arg.rfind("--auth-method-payload=", 0) == 0) {
            g_config.auth_method_payload = arg.substr(22);
        } else if (arg == "--auth-payload-json" && arg_index + 1 < argc) {
            g_config.auth_payload_json = argv[++arg_index];
        } else if (arg.rfind("--auth-payload-json=", 0) == 0) {
            g_config.auth_payload_json = arg.substr(20);
        } else if (arg == "--auth-payload-b64" && arg_index + 1 < argc) {
            g_config.auth_payload_b64 = argv[++arg_index];
        } else if (arg.rfind("--auth-payload-b64=", 0) == 0) {
            g_config.auth_payload_b64 = arg.substr(19);
        } else if (arg == "--auth-provider-profile" && arg_index + 1 < argc) {
            g_config.auth_provider_profile = argv[++arg_index];
        } else if (arg.rfind("--auth-provider-profile=", 0) == 0) {
            g_config.auth_provider_profile = arg.substr(24);
        } else if (arg == "--auth-required-methods" && arg_index + 1 < argc) {
            g_config.auth_required_methods = argv[++arg_index];
        } else if (arg.rfind("--auth-required-methods=", 0) == 0) {
            g_config.auth_required_methods = arg.substr(24);
        } else if (arg == "--auth-forbidden-methods" && arg_index + 1 < argc) {
            g_config.auth_forbidden_methods = argv[++arg_index];
        } else if (arg.rfind("--auth-forbidden-methods=", 0) == 0) {
            g_config.auth_forbidden_methods = arg.substr(25);
        } else if (arg == "--auth-require-channel-binding" && arg_index + 1 < argc) {
            g_config.auth_require_channel_binding = argv[++arg_index];
        } else if (arg.rfind("--auth-require-channel-binding=", 0) == 0) {
            g_config.auth_require_channel_binding = arg.substr(31);
        } else if (arg == "--workload-identity-token" && arg_index + 1 < argc) {
            g_config.workload_identity_token = argv[++arg_index];
        } else if (arg.rfind("--workload-identity-token=", 0) == 0) {
            g_config.workload_identity_token = arg.substr(26);
        } else if (arg == "--proxy-principal-assertion" && arg_index + 1 < argc) {
            g_config.proxy_principal_assertion = argv[++arg_index];
        } else if (arg.rfind("--proxy-principal-assertion=", 0) == 0) {
            g_config.proxy_principal_assertion = arg.substr(28);
        } else if (arg == "--manager-profile" && arg_index + 1 < argc) {
            g_config.manager_connection_profile = argv[++arg_index];
        } else if (arg.rfind("--manager-profile=", 0) == 0) {
            g_config.manager_connection_profile = arg.substr(18);
        } else if (arg == "--manager-intent" && arg_index + 1 < argc) {
            g_config.manager_client_intent = argv[++arg_index];
        } else if (arg.rfind("--manager-intent=", 0) == 0) {
            g_config.manager_client_intent = arg.substr(17);
        } else if (arg == "--manager-client-flags" && arg_index + 1 < argc) {
            g_config.manager_client_flags = argv[++arg_index];
        } else if (arg.rfind("--manager-client-flags=", 0) == 0) {
            g_config.manager_client_flags = arg.substr(23);
        } else if (arg == "--manager-auth-fast-path" && arg_index + 1 < argc) {
            g_config.manager_auth_fast_path = argv[++arg_index];
        } else if (arg.rfind("--manager-auth-fast-path=", 0) == 0) {
            g_config.manager_auth_fast_path = arg.substr(25);
        } else if (arg == "--sslmode" && arg_index + 1 < argc) {
            g_config.ssl_mode = argv[++arg_index];
        } else if (arg.rfind("--sslmode=", 0) == 0) {
            g_config.ssl_mode = arg.substr(10);
        } else if (arg == "--conn-opt" && arg_index + 1 < argc) {
            std::string key;
            std::string value;
            if (!splitConnOption(argv[++arg_index], key, value)) {
                printError("--conn-opt expects key=value");
                return false;
            }
            g_config.conn_options.emplace_back(key, value);
        } else if (arg.rfind("--conn-opt=", 0) == 0) {
            std::string key;
            std::string value;
            if (!splitConnOption(arg.substr(11), key, value)) {
                printError("--conn-opt expects key=value");
                return false;
            }
            g_config.conn_options.emplace_back(key, value);
        } else if (arg == "-v" || arg == "--verbose") {
            g_config.verbose = true;
        } else if (arg == "-q" || arg == "--quiet") {
            g_config.quiet = true;
        } else if (arg == "--json") {
            g_config.json = true;
        } else if (arg == "--probe-auth-surface") {
            g_config.probe_auth_surface = true;
        } else if (arg == "--show-auth-context") {
            g_config.show_auth_context = true;
        } else if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            std::exit(0);
        } else if (arg[0] == '-') {
            printError("Unknown option: " + arg);
            return false;
        }
    }

    std::string normalized_mode = normalizeConnectionMode(g_config.mode);
    if (normalized_mode.empty()) {
        printError("Invalid --mode value (expected embedded|local-ipc|inet|managed)");
        return false;
    }
    g_config.mode = normalized_mode;

    if (g_config.database_path.empty() && g_config.connection_string.empty()) {
        printError("Missing database path (or provide --connection)");
        return false;
    }

    // Prompt for admin password if user given but no password
    if (!g_config.admin_user.empty() && g_config.admin_password.empty() &&
        !g_config.probe_auth_surface) {
        g_config.admin_password = readPassword("Admin password: ");
    }

    return true;
}

// =============================================================================
// Main
// =============================================================================

int main(int argc, char* argv[]) {
    if (argc < 3) {
        printUsage(argv[0]);
        return 1;
    }

    if (!parseArgs(argc, argv)) {
        return 1;
    }

    if (g_config.probe_auth_surface) {
        const std::string conn_target = buildConnectionTarget();
        scratchbird::client::AuthProbeResult probe;
        core::ErrorContext probe_ctx;
        core::Status probe_status =
            scratchbird::client::probeAuthSurface(conn_target, &probe, &probe_ctx);
        if (probe_status != core::Status::OK) {
            printError("Auth probe failed: " + probe_ctx.message);
            return 1;
        }
        std::cout << (g_config.json
                          ? scratchbird::cli::renderAuthProbeJson(probe)
                          : scratchbird::cli::renderAuthProbeText(probe))
                  << "\n";
        return 0;
    }

    // Connect to database
    if (!connectToDatabase()) {
        return 1;
    }

    // Execute command
    bool success = executeCommand();

    // Cleanup
    disconnectFromDatabase();

    return success ? 0 : 1;
}
