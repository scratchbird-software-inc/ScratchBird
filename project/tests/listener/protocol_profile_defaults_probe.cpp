// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "listener_config.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

scratchbird::listener::ConfigResult Load(std::vector<std::string> args) {
  std::vector<char*> argv;
  argv.reserve(args.size());
  for (auto& arg : args) argv.push_back(arg.data());
  return scratchbird::listener::LoadListenerConfigFromArgs(
      static_cast<int>(argv.size()), argv.data());
}

bool Expect(bool condition, std::string_view message) {
  if (condition) return true;
  std::cerr << message << '\n';
  return false;
}

bool HasDiagnostic(const scratchbird::listener::ConfigResult& result, std::string_view code) {
  for (const auto& diagnostic : result.messages.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

} // namespace

int main() {
  auto sbsql = Load({"listener_profile_defaults_probe",
                     "--validate-config",
                     "--protocol-family=sbsql",
                     "--database-selector=test.db",
                     "--server-endpoint=unix:/tmp/sb"});
  if (!Expect(sbsql.ok, "SBSQL listener profile defaults should validate")) {
    return EXIT_FAILURE;
  }
  if (!Expect(sbsql.config.parser_package == "sbp_sbsql",
              "SBSQL default parser package mismatch")) {
    return EXIT_FAILURE;
  }
  if (!Expect(sbsql.config.dialect == "sbsql.v3", "SBSQL default dialect mismatch")) {
    return EXIT_FAILURE;
  }

  auto native = Load({"listener_profile_defaults_probe",
                      "--validate-config",
                      "--protocol-family=native",
                      "--database-selector=test.db",
                      "--server-endpoint=unix:/tmp/sb"});
  if (!Expect(native.ok, "Native SBWP listener profile defaults should validate")) {
    return EXIT_FAILURE;
  }
  if (!Expect(native.config.parser_package == "sbp_native",
              "Native default parser package mismatch")) {
    return EXIT_FAILURE;
  }
  if (!Expect(native.config.dialect == "sbwp.v1", "Native default dialect mismatch")) {
    return EXIT_FAILURE;
  }

  auto firebird = Load({"listener_profile_defaults_probe",
                        "--validate-config",
                        "--protocol-family=firebird",
                        "--database-selector=test.db",
                        "--server-endpoint=unix:/tmp/sb"});
  if (!Expect(firebird.ok, "Firebird listener profile defaults should validate")) {
    return EXIT_FAILURE;
  }
  if (!Expect(firebird.config.parser_package == "sbp_firebird",
              "Firebird default parser package mismatch")) {
    return EXIT_FAILURE;
  }
  if (!Expect(firebird.config.dialect == "firebird.wire.v13",
              "Firebird default dialect mismatch")) {
    return EXIT_FAILURE;
  }

  auto explicit_package = Load({"listener_profile_defaults_probe",
                                "--validate-config",
                                "--protocol-family=firebird",
                                "--parser-package=custom_firebird_parser",
                                "--database-selector=test.db",
                                "--server-endpoint=unix:/tmp/sb"});
  if (!Expect(explicit_package.ok, "Explicit parser package config should validate")) {
    return EXIT_FAILURE;
  }
  if (!Expect(explicit_package.config.parser_package == "custom_firebird_parser",
              "Explicit parser package was overwritten")) {
    return EXIT_FAILURE;
  }
  if (!Expect(explicit_package.config.dialect == "firebird.wire.v13",
              "Explicit package should still use Firebird dialect default")) {
    return EXIT_FAILURE;
  }

  auto unknown_key = Load({"listener_profile_defaults_probe",
                           "--validate-config",
                           "--protocol-family=sbsql",
                           "--database-selector=test.db",
                           "--server-endpoint=unix:/tmp/sb",
                           "--unknown-public-key=value"});
  if (!Expect(!unknown_key.ok, "Unknown listener configuration keys should fail closed") ||
      !Expect(HasDiagnostic(unknown_key, "LISTENER.CONFIG.UNKNOWN_KEY"),
              "Unknown listener configuration key diagnostic mismatch")) {
    return EXIT_FAILURE;
  }

  auto malformed_cli = Load({"listener_profile_defaults_probe",
                             "--validate-config",
                             "--protocol-family"});
  if (!Expect(!malformed_cli.ok, "Malformed listener CLI option should fail closed") ||
      !Expect(HasDiagnostic(malformed_cli, "LISTENER.CLI.INVALID_ARGUMENT"),
              "Malformed listener CLI diagnostic mismatch")) {
    return EXIT_FAILURE;
  }

  auto positional_arg = Load({"listener_profile_defaults_probe", "positional"});
  if (!Expect(!positional_arg.ok, "Unknown listener positional argument should fail closed") ||
      !Expect(HasDiagnostic(positional_arg, "LISTENER.CLI.UNKNOWN_ARGUMENT"),
              "Unknown listener positional argument diagnostic mismatch")) {
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
