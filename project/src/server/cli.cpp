// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_PRODUCT_SKELETON_CLI

#include "cli.hpp"

#include "manager_control.hpp"
#include "product_identity.hpp"

#include <sstream>
#include <string_view>

namespace scratchbird::server {

namespace {

bool IsKnownLogLevel(std::string_view level) {
  return level == "trace" || level == "debug" || level == "info" || level == "warn" ||
         level == "error" || level == "fatal";
}

ServerDiagnostic CliDiagnostic(std::string code,
                               std::string key,
                               std::string safe_message,
                               std::vector<ServerDiagnosticField> fields = {}) {
  return ServerDiagnostic{std::move(code),
                          std::move(key),
                          ServerDiagnosticSeverity::kError,
                          std::move(safe_message),
                          std::move(fields)};
}

bool NeedsValue(std::string_view option) {
  return option == "--config" || option == "--control-dir" || option == "--runtime-dir" ||
         option == "--database" || option == "--sbps-endpoint" || option == "--log" ||
         option == "--log-level" || option == "--lifecycle-command" ||
         option == "--lifecycle-mode" || option == "--lifecycle-target-uuid" ||
         option == "--lifecycle-audit-reason";
}

std::string CanonicalLifecycleOperation(std::string operation) {
  if (operation == "health") return "health_database";
  if (operation == "status") return "status_database";
  if (operation == "create") return "create_database";
  if (operation == "open") return "open_database";
  if (operation == "attach") return "attach_database";
  if (operation == "detach") return "detach_database";
  if (operation == "inspect") return "inspect_database";
  if (operation == "verify") return "verify_database";
  if (operation == "repair") return "repair_database";
  if (operation == "shutdown") return "shutdown_database";
  if (operation == "shutdown-force" || operation == "shutdown_force") {
    return "shutdown_database_force";
  }
  if (operation == "drop") return "drop_database";
  return operation;
}

bool IsKnownLifecycleOperation(const std::string& operation) {
  return operation == "health_database" ||
         operation == "status_database" ||
         operation == "create_database" ||
         operation == "open_database" ||
         operation == "attach_database" ||
         operation == "detach_database" ||
         operation == "inspect_database" ||
         operation == "verify_database" ||
         operation == "repair_database" ||
         operation == "shutdown_database" ||
         operation == "shutdown_database_force" ||
         operation == "drop_database";
}

}  // namespace

ServerCliParseResult ParseServerCli(int argc, char** argv) {
  ServerCliParseResult result;
  for (int index = 1; index < argc; ++index) {
    const std::string arg = argv[index] == nullptr ? std::string{} : std::string(argv[index]);
    auto read_value = [&](const std::string& option) -> std::string {
      if (index + 1 >= argc || argv[index + 1] == nullptr) {
        result.diagnostics.push_back(CliDiagnostic(
            "SERVER.CLI.MISSING_VALUE",
            "server.cli.missing_value",
            "A server command line option requires a value.",
            {{"option", option}}));
        return {};
      }
      ++index;
      return std::string(argv[index]);
    };

    if (arg == "--help" || arg == "-h") {
      result.options.help = true;
    } else if (arg == "--version") {
      result.options.version = true;
    } else if (arg == "--foreground" || arg == "-F") {
      result.options.foreground = true;
    } else if (arg == "--service") {
      result.options.service = true;
    } else if (arg == "--validate-config") {
      result.options.validate_config = true;
    } else if (arg == "--validate-endpoints") {
      result.options.validate_endpoints = true;
    } else if (arg == "--read-only") {
      result.options.read_only = true;
    } else if (arg == "--maintenance") {
      result.options.maintenance = true;
    } else if (arg == "--restricted-open") {
      result.options.restricted_open = true;
    } else if (arg == "--no-listeners") {
      result.options.no_listeners = true;
    } else if (arg == "--create-if-missing") {
      result.options.create_if_missing = true;
    } else if (arg == "--lifecycle-command") {
      result.options.lifecycle_request = true;
      result.options.lifecycle_operation = CanonicalLifecycleOperation(read_value(arg));
    } else if (arg == "--lifecycle-mode") {
      result.options.lifecycle_mode = read_value(arg);
    } else if (arg == "--lifecycle-target-uuid") {
      result.options.lifecycle_target_uuid = read_value(arg);
    } else if (arg == "--lifecycle-audit-reason") {
      result.options.lifecycle_audit_reason = read_value(arg);
    } else if (arg == "--config") {
      result.options.config_path = read_value(arg);
    } else if (arg == "--control-dir") {
      result.options.control_dir = read_value(arg);
    } else if (arg == "--runtime-dir") {
      result.options.runtime_dir = read_value(arg);
    } else if (arg == "--database") {
      result.options.database_ref = read_value(arg);
    } else if (arg == "--sbps-endpoint") {
      result.options.sbps_endpoint = read_value(arg);
    } else if (arg == "--log") {
      result.options.log_path = read_value(arg);
    } else if (arg == "--log-level") {
      result.options.log_level = read_value(arg);
    } else if (NeedsValue(arg)) {
      result.diagnostics.push_back(CliDiagnostic(
          "SERVER.CLI.MISSING_VALUE",
          "server.cli.missing_value",
          "A server command line option requires a value.",
          {{"option", arg}}));
    } else {
      result.diagnostics.push_back(CliDiagnostic(
          "SERVER.CLI.UNKNOWN_OPTION",
          "server.cli.unknown_option",
          "The server command line contains an unknown option.",
          {{"option", arg}}));
    }
  }

  if (result.options.foreground && result.options.service) {
    result.diagnostics.push_back(CliDiagnostic(
        "SERVER.CLI.MODE_CONFLICT",
        "server.cli.mode_conflict",
        "Foreground and service modes are mutually exclusive.",
        {{"mode_a", "foreground"}, {"mode_b", "service"}}));
  }

  if (!IsKnownLogLevel(result.options.log_level)) {
    result.diagnostics.push_back(CliDiagnostic(
        "SERVER.LOG.LEVEL_INVALID",
        "server.log.level_invalid",
        "The server log level is not recognized.",
        {{"log_level", result.options.log_level}}));
  }

  if (result.options.lifecycle_request &&
      !IsKnownLifecycleOperation(result.options.lifecycle_operation)) {
    result.diagnostics.push_back(CliDiagnostic(
        "SERVER.CLI.LIFECYCLE_OPERATION_INVALID",
        "server.cli.lifecycle_operation_invalid",
        "The server lifecycle command is not recognized.",
        {{"operation", result.options.lifecycle_operation}}));
  }

  return result;
}

std::string ServerHelpText() {
  std::ostringstream out;
  out << ProductVersionLine() << "\\n"
      << "Usage: SBsrv [options]\\n\\n"
      << "Options:\\n"
      << "  --help, -h                 Show this help and exit.\\n"
      << "  --version                  Show product version and exit.\\n"
      << "  --config PATH              Load SBCD1 bootstrap configuration.\\n"
      << "  --foreground, -F           Run in foreground mode.\\n"
      << "  --service                  Run in service mode.\\n"
      << "  --validate-config          Validate configuration and exit.\\n"
      << "  --validate-endpoints       Validate endpoint feasibility and exit.\\n"
      << "  --control-dir PATH         Override control directory.\\n"
      << "  --runtime-dir PATH         Override runtime directory.\\n"
      << "  --database PATH_OR_ALIAS   Select configured database path or alias.\\n"
      << "  --create-if-missing        Permit create-if-missing only when policy allows.\\n"
      << "  --read-only                Open databases read-only.\\n"
      << "  --maintenance              Open in maintenance mode.\\n"
      << "  --restricted-open          Permit restricted open after unsafe classification.\\n"
      << "  --no-listeners             Do not start listener profiles.\\n"
      << "  --sbps-endpoint ENDPOINT   Override parser-server SBPS endpoint.\\n"
      << "  --log PATH                 Write structured log to PATH or stderr marker.\\n"
      << "  --log-level LEVEL          trace, debug, info, warn, error, or fatal.\\n"
      << "  --lifecycle-command NAME   Build an authenticated management request for health,\\n"
      << "                             status, create, open, attach, detach, inspect, verify,\\n"
      << "                             repair, shutdown, shutdown-force, or drop.\\n"
      << "  --lifecycle-mode KVS       Semicolon-delimited lifecycle policy evidence.\\n"
      << "  --lifecycle-target-uuid ID Target database UUID for exact-scope operations.\\n"
      << "  --lifecycle-audit-reason R Safe audit reason recorded with the request.\\n";
  return out.str();
}

bool HasServerCliLifecycleRequest(const ServerCliOptions& options) {
  return options.lifecycle_request;
}

ServerManagementRequest BuildServerCliLifecycleRequest(const ServerCliOptions& options) {
  ServerManagementRequest request;
  request.operation_key = CanonicalLifecycleOperation(options.lifecycle_operation);
  request.target_uuid = options.lifecycle_target_uuid;
  request.mode = options.lifecycle_mode;
  request.audit_reason = options.lifecycle_audit_reason;
  request.include_history = request.operation_key == "status_database" ||
                            request.operation_key == "health_database";
  return request;
}

}  // namespace scratchbird::server
