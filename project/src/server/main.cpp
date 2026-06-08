// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_PRODUCT_SKELETON_MAIN

#include "cli.hpp"
#include "diagnostics.hpp"
#include "engine_host.hpp"
#include "ipc_server.hpp"
#include "product_identity.hpp"
#include "server_daemon_lifecycle.hpp"
#include "startup.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

int EmitDiagnostics(const std::vector<scratchbird::server::ServerDiagnostic>& diagnostics) {
  for (const auto& diagnostic : diagnostics) {
    std::cerr << scratchbird::server::ToMessageVectorJsonLine(diagnostic) << '\n';
  }
  return diagnostics.empty() ? 0 : 2;
}

}  // namespace

int main(int argc, char** argv) {
  const auto parse = scratchbird::server::ParseServerCli(argc, argv);
  if (!parse.ok()) {
    return EmitDiagnostics(parse.diagnostics);
  }

  if (parse.options.help) {
    std::cout << scratchbird::server::ServerHelpText();
    return 0;
  }

  if (parse.options.version) {
    std::cout << scratchbird::server::ProductVersionLine() << '\n';
    return 0;
  }

  const auto startup = scratchbird::server::RunServerStartup(parse.options);
  if (!startup.stdout_text.empty()) {
    std::cout << startup.stdout_text;
  }
  if (!startup.diagnostics.empty()) {
    EmitDiagnostics(startup.diagnostics);
  }
  if (startup.exit_code != 0 || startup.effective_config.mode == scratchbird::server::ServerMode::kValidationOnly) {
    return startup.exit_code;
  }
  const auto engine_host = scratchbird::server::StartHostedEngine(startup.effective_config);
  if (!engine_host.diagnostics.empty()) {
    EmitDiagnostics(engine_host.diagnostics);
    return 2;
  }
  const auto daemon_lifecycle = scratchbird::server::EvaluateServerDaemonLifecycle(
      startup.effective_config, startup.lifecycle_artifacts, engine_host.state);
  if (!daemon_lifecycle.diagnostics.empty()) {
    EmitDiagnostics(daemon_lifecycle.diagnostics);
    return 2;
  }
  if (startup.exit_code == 0 && startup.serving_requested) {
    std::cout.flush();
    const auto ipc = scratchbird::server::RunParserServerIpcEndpoint(
        startup.effective_config, startup.lifecycle_artifacts, engine_host.state);
    if (!ipc.diagnostics.empty()) {
      EmitDiagnostics(ipc.diagnostics);
    }
    return ipc.exit_code;
  }
  return startup.exit_code;
}
