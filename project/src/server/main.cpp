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
#include "standalone_shutdown_deadline.hpp"
#include "windows_service_runtime.hpp"

#include <iostream>
#include <algorithm>
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
  if (!scratchbird::server::WriteServerStartupDiagnostics(startup, std::cerr)) return 2;
  if (startup.exit_code != 0 ||
      startup.effective_config.mode == scratchbird::server::ServerMode::kValidationOnly) {
    return startup.exit_code;
  }
  // Declared before engine/IPC owners: it outlives their actual destruction.
  scratchbird::server::StandaloneShutdownDeadline shutdown_deadline(
      startup.effective_config.shutdown_drain_timeout_ms,
      scratchbird::server::ParserServerStopRequested);
  const auto engine_host =
      scratchbird::server::StartHostedEngine(startup.effective_config,
                                            scratchbird::server::RequestParserServerStop);
  struct ShutdownArm {
    ~ShutdownArm() { scratchbird::server::RequestParserServerStop(); }
  } shutdown_arm;
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
    scratchbird::server::ServerIpcEndpointOwner owner(engine_host.state);
    scratchbird::server::ServerIpcEndpointResult ipc;
    try {
      ipc = scratchbird::server::RunParserServerIpcEndpoint(
          startup.effective_config, startup.lifecycle_artifacts, owner, ipc_callbacks);
    } catch (...) {
      if (!owner.first_failure) owner.first_failure = std::current_exception();
      ipc.exit_code = 2;
    }
    scratchbird::server::RequestParserServerStop();
    // On persistent failure the independent deadline exits without destroying
    // this cohort. Embedded callers instead keep their own owner for retry.
    auto retry_delay = std::chrono::milliseconds(10);
    for (;;) {
      try {
        const auto drained = scratchbird::server::DrainServerIpcEndpoint(owner);
        if (drained.complete) break;
      } catch (...) {
        if (!owner.first_failure) owner.first_failure = std::current_exception();
      }
      ipc.exit_code = 2;
      std::this_thread::sleep_for(retry_delay);
      retry_delay = std::min(retry_delay * 2, std::chrono::milliseconds(250));
    }
    if (!ipc.diagnostics.empty()) EmitDiagnostics(ipc.diagnostics);
    if (owner.first_failure) ipc.exit_code = 2;
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
