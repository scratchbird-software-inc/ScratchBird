// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "listener_config.hpp"
#include "dbbt_lpreface.hpp"

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

bool HasDiagnostic(const scratchbird::listener::DbbtGateResult& result,
                   std::string_view code) {
  for (const auto& diagnostic : result.messages.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

void SetDbbtEnvironment(const char* value) {
#ifdef _WIN32
  _putenv_s("SCRATCHBIRD_LISTENER_DBBT_KEY_HEX", value);
#else
  if (*value == '\0') {
    unsetenv("SCRATCHBIRD_LISTENER_DBBT_KEY_HEX");
  } else {
    setenv("SCRATCHBIRD_LISTENER_DBBT_KEY_HEX", value, 1);
  }
#endif
}

} // namespace

int main() {
  auto explicit_endpoint = Load({"listener_explicit_config_probe",
                                 "--validate-config",
                                 "--protocol-family=Vendor.Future-Wire",
                                 "--parser-package=vendor.future.package",
                                 "--parser-executable=/opt/vendor/parser-worker-v7",
                                 "--dialect=vendor.wire.v7",
                                 "--database-selector=test.db",
                                 "--server-endpoint=unix:/tmp/sb",
                                 "--bind-address=192.0.2.44",
                                 "--port=45678"});
  if (!Expect(explicit_endpoint.ok,
              "fully explicit opaque listener endpoint should validate")) {
    return EXIT_FAILURE;
  }
  if (!Expect(explicit_endpoint.config.protocol_family == "Vendor.Future-Wire",
              "opaque protocol family was interpreted or normalized")) {
    return EXIT_FAILURE;
  }
  if (!Expect(explicit_endpoint.config.parser_package == "vendor.future.package",
              "explicit parser package was changed")) {
    return EXIT_FAILURE;
  }
  if (!Expect(explicit_endpoint.config.parser_executable ==
                  "/opt/vendor/parser-worker-v7",
              "explicit parser executable was changed")) {
    return EXIT_FAILURE;
  }
  if (!Expect(explicit_endpoint.config.dialect == "vendor.wire.v7",
              "explicit dialect was changed")) {
    return EXIT_FAILURE;
  }
  if (!Expect(explicit_endpoint.config.bind_address == "192.0.2.44",
              "explicit bind address was changed")) {
    return EXIT_FAILURE;
  }
  if (!Expect(explicit_endpoint.config.port == 45678,
              "explicit listener port was changed")) {
    return EXIT_FAILURE;
  }

  auto missing_endpoint_identity = Load({"listener_explicit_config_probe",
                                         "--validate-config",
                                         "--database-selector=test.db",
                                         "--server-endpoint=unix:/tmp/sb"});
  if (!Expect(!missing_endpoint_identity.ok,
              "listener endpoint identity must not come from compiled defaults")) {
    return EXIT_FAILURE;
  }
  if (!Expect(HasDiagnostic(missing_endpoint_identity,
                            "LISTENER.CONFIG.MISSING_PROTOCOL_FAMILY"),
              "missing protocol family diagnostic mismatch") ||
      !Expect(HasDiagnostic(missing_endpoint_identity,
                            "LISTENER.CONFIG.MISSING_PARSER_EXECUTABLE"),
              "missing parser executable diagnostic mismatch") ||
      !Expect(HasDiagnostic(missing_endpoint_identity,
                            "LISTENER.CONFIG.MISSING_BIND_ADDRESS"),
              "missing bind address diagnostic mismatch") ||
      !Expect(HasDiagnostic(missing_endpoint_identity,
                            "LISTENER.CONFIG.MISSING_PORT"),
              "missing port diagnostic mismatch")) {
    return EXIT_FAILURE;
  }
  if (!Expect(missing_endpoint_identity.config.protocol_family.empty(),
              "protocol family received a compiled default") ||
      !Expect(missing_endpoint_identity.config.parser_package.empty(),
              "parser package received a compiled default") ||
      !Expect(missing_endpoint_identity.config.parser_executable.empty(),
              "parser executable received a compiled default") ||
      !Expect(missing_endpoint_identity.config.dialect.empty(),
              "dialect received a compiled default") ||
      !Expect(missing_endpoint_identity.config.bind_address.empty(),
              "bind address received a compiled default") ||
      !Expect(missing_endpoint_identity.config.port == 0,
              "port received a compiled default")) {
    return EXIT_FAILURE;
  }

  scratchbird::listener::ListenerConfig keyring_config;
  keyring_config.dbbt_key_source = scratchbird::listener::DbbtKeySource::kKeyring;
  scratchbird::listener::DbbtKeyMaterial key_material;
  SetDbbtEnvironment("0011223344556677");
  const auto short_key =
      scratchbird::listener::LoadDbbtKeyMaterial(keyring_config, &key_material);
  if (!Expect(!short_key.ok, "short DBBT keyring material must fail closed") ||
      !Expect(HasDiagnostic(short_key, "LISTENER.DBBT.KEYRING_KEY_INVALID"),
              "short DBBT keyring material diagnostic mismatch")) {
    SetDbbtEnvironment("");
    return EXIT_FAILURE;
  }
  SetDbbtEnvironment(
      "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
  const auto valid_key =
      scratchbird::listener::LoadDbbtKeyMaterial(keyring_config, &key_material);
  SetDbbtEnvironment("");
  if (!Expect(valid_key.ok && key_material.bytes.size() == 32,
              "32-byte DBBT keyring material must be accepted")) {
    return EXIT_FAILURE;
  }

  auto unknown_key = Load({"listener_explicit_config_probe",
                           "--validate-config",
                           "--protocol-family=vendor-wire",
                           "--parser-executable=/opt/vendor/parser-worker",
                           "--database-selector=test.db",
                           "--server-endpoint=unix:/tmp/sb",
                           "--bind-address=127.0.0.1",
                           "--port=45001",
                           "--unknown-public-key=value"});
  if (!Expect(!unknown_key.ok, "Unknown listener configuration keys should fail closed") ||
      !Expect(HasDiagnostic(unknown_key, "LISTENER.CONFIG.UNKNOWN_KEY"),
              "Unknown listener configuration key diagnostic mismatch")) {
    return EXIT_FAILURE;
  }

  auto malformed_cli = Load({"listener_explicit_config_probe",
                             "--validate-config",
                             "--protocol-family"});
  if (!Expect(!malformed_cli.ok, "Malformed listener CLI option should fail closed") ||
      !Expect(HasDiagnostic(malformed_cli, "LISTENER.CLI.INVALID_ARGUMENT"),
              "Malformed listener CLI diagnostic mismatch")) {
    return EXIT_FAILURE;
  }

  auto positional_arg = Load({"listener_explicit_config_probe", "positional"});
  if (!Expect(!positional_arg.ok, "Unknown listener positional argument should fail closed") ||
      !Expect(HasDiagnostic(positional_arg, "LISTENER.CLI.UNKNOWN_ARGUMENT"),
              "Unknown listener positional argument diagnostic mismatch")) {
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
