// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_PRODUCT_MAIN

#include "cli.hpp"
#include "diagnostics.hpp"
#include "engine_host.hpp"
#include "ipc_server.hpp"
#include "product_identity.hpp"
#include "server_daemon_lifecycle.hpp"
#include "startup.hpp"
#include "windows_service_runtime.hpp"

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

int RunServerProduct(
    const scratchbird::server::ServerCliOptions& options,
    const scratchbird::server::ParserServerIpcLifecycleCallbacks& ipc_callbacks = {}) {
  if (options.help) {
    std::cout << scratchbird::server::ServerHelpText();
    return 0;
  }

  if (options.version) {
    std::cout << scratchbird::server::ProductVersionLine() << '\n';
    return 0;
  }

  const auto startup = scratchbird::server::RunServerStartup(options);
  if (!startup.stdout_text.empty()) {
    std::cout << startup.stdout_text;
  }
  if (!startup.diagnostics.empty()) {
    EmitDiagnostics(startup.diagnostics);
  }
  if (startup.exit_code != 0 ||
      startup.effective_config.mode == scratchbird::server::ServerMode::kValidationOnly) {
    return startup.exit_code;
  }
  const auto engine_host =
      scratchbird::server::StartHostedEngine(startup.effective_config);
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
        startup.effective_config,
        startup.lifecycle_artifacts,
        engine_host.state,
        ipc_callbacks);
    if (!ipc.diagnostics.empty()) {
      EmitDiagnostics(ipc.diagnostics);
    }
    return ipc.exit_code;
  }
  return startup.exit_code;
}

}  // namespace

int main(int argc, char** argv) {
  const auto parse = scratchbird::server::ParseServerCli(argc, argv);
  if (!parse.ok()) {
    return EmitDiagnostics(parse.diagnostics);
  }

  scratchbird::server::ResetParserServerStopRequest();
#if defined(_WIN32)
  if (parse.options.service) {
    const auto service = scratchbird::server::DispatchWindowsServerService(
        [&options = parse.options](
            const scratchbird::server::WindowsServiceWorkerCallbacks& callbacks) {
          scratchbird::server::ParserServerIpcLifecycleCallbacks ipc_callbacks;
          ipc_callbacks.on_ready = callbacks.report_ready;
          ipc_callbacks.on_stopping = callbacks.report_stopping;
          return RunServerProduct(options, ipc_callbacks);
        });
    if (!service.diagnostics.empty()) {
      EmitDiagnostics(service.diagnostics);
    }
    return service.exit_code;
  }
#endif
  return RunServerProduct(parse.options);
}
