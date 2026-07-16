// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ipc_server.hpp"
#include "windows_service_runtime.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using scratchbird::server::WindowsServiceControl;
using scratchbird::server::WindowsServiceLifecycleModel;
using scratchbird::server::WindowsServiceLifecycleState;

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

void TestThreadSafeParserServerStopRequest() {
  scratchbird::server::ResetParserServerStopRequest();
  Require(!scratchbird::server::ParserServerStopRequested(),
          "parser-server stop state did not reset");

  std::vector<std::thread> requesters;
  for (int index = 0; index < 8; ++index) {
    requesters.emplace_back([] {
      scratchbird::server::RequestParserServerStop();
    });
  }
  for (auto& requester : requesters) {
    requester.join();
  }
  Require(scratchbird::server::ParserServerStopRequested(),
          "concurrent parser-server stop request was lost");
  scratchbird::server::ResetParserServerStopRequest();
}

void TestExactServiceIdentityContract() {
  Require(std::wstring_view(scratchbird::server::kScratchBirdWindowsServiceName) ==
              L"scratchbird",
          "Windows SCM service name must match NT SERVICE\\scratchbird");
}

void TestReadyThenGracefulStopLifecycle() {
  WindowsServiceLifecycleModel lifecycle;
  const auto starting = lifecycle.BeginStart();
  Require(starting.state == WindowsServiceLifecycleState::kStartPending &&
              !starting.accepts_stop && !starting.accepts_shutdown &&
              starting.checkpoint != 0 && starting.wait_hint_ms != 0,
          "SCM start-pending status contract mismatch");
  const auto starting_heartbeat = lifecycle.AdvancePendingCheckpoint();
  Require(starting_heartbeat.has_value() &&
              starting_heartbeat->state ==
                  WindowsServiceLifecycleState::kStartPending &&
              starting_heartbeat->checkpoint > starting.checkpoint,
          "SCM start-pending checkpoint did not advance");

  const auto running = lifecycle.MarkRunning();
  Require(running.has_value() &&
              running->state == WindowsServiceLifecycleState::kRunning &&
              running->accepts_stop && running->accepts_shutdown &&
              running->checkpoint == 0 && running->wait_hint_ms == 0,
          "SCM running status contract mismatch");

  const auto interrogate = lifecycle.HandleControl(WindowsServiceControl::kInterrogate);
  Require(interrogate.handled && interrogate.publish_status &&
              !interrogate.request_stop &&
              interrogate.status.state == WindowsServiceLifecycleState::kRunning,
          "SCM interrogate control contract mismatch");

  const auto shutdown = lifecycle.HandleControl(WindowsServiceControl::kShutdown);
  Require(shutdown.handled && shutdown.publish_status && shutdown.request_stop &&
              shutdown.status.state == WindowsServiceLifecycleState::kStopPending &&
              !shutdown.status.accepts_stop && !shutdown.status.accepts_shutdown &&
              shutdown.status.checkpoint != 0 && shutdown.status.wait_hint_ms != 0,
          "SCM shutdown control did not enter stop-pending state");
  const auto stopping_heartbeat = lifecycle.AdvancePendingCheckpoint();
  Require(stopping_heartbeat.has_value() &&
              stopping_heartbeat->state ==
                  WindowsServiceLifecycleState::kStopPending &&
              stopping_heartbeat->checkpoint > shutdown.status.checkpoint,
          "SCM stop-pending checkpoint did not advance");

  const auto duplicate = lifecycle.HandleControl(WindowsServiceControl::kStop);
  Require(duplicate.handled && !duplicate.publish_status &&
              !duplicate.request_stop &&
              duplicate.status.state == WindowsServiceLifecycleState::kStopPending,
          "duplicate SCM stop control was not idempotent");
  Require(!lifecycle.MarkRunning().has_value(),
          "SCM lifecycle returned to running after a stop request");

  const auto stopped = lifecycle.MarkStopped(0);
  Require(stopped.state == WindowsServiceLifecycleState::kStopped &&
              stopped.process_exit_code == 0 && stopped.checkpoint == 0 &&
              stopped.wait_hint_ms == 0,
          "SCM stopped status contract mismatch");
}

void TestStartupStopAndFailureLifecycle() {
  WindowsServiceLifecycleModel lifecycle;
  lifecycle.BeginStart();
  const auto stop = lifecycle.HandleControl(WindowsServiceControl::kStop);
  Require(stop.handled && stop.publish_status && stop.request_stop &&
              stop.status.state == WindowsServiceLifecycleState::kStopPending,
          "SCM startup stop did not fail closed into stop-pending");
  Require(!lifecycle.MarkRunning().has_value(),
          "SCM startup stop allowed a late running transition");
  const auto stopped = lifecycle.MarkStopped(2);
  Require(stopped.state == WindowsServiceLifecycleState::kStopped &&
              stopped.process_exit_code == 2,
          "SCM failed worker exit code was not retained");

  const auto unsupported = lifecycle.HandleControl(WindowsServiceControl::kUnsupported);
  Require(!unsupported.handled && !unsupported.publish_status &&
              !unsupported.request_stop,
          "unsupported SCM control was accepted");
}

void TestServiceDispatchFailsClosedOutsideScm() {
  bool worker_called = false;
  const auto dispatched = scratchbird::server::DispatchWindowsServerService(
      [&worker_called](const scratchbird::server::WindowsServiceWorkerCallbacks&) {
        worker_called = true;
        return 0;
      });
  Require(!dispatched.ok() && dispatched.exit_code != 0 &&
              !dispatched.diagnostics.empty() && !worker_called,
          "service dispatcher executed the worker outside SCM authority");
#if defined(_WIN32)
  Require(dispatched.diagnostics.front().code == "SERVER.SERVICE.SCM_REQUIRED",
          "Windows non-SCM service dispatch diagnostic mismatch");
#else
  Require(dispatched.diagnostics.front().code ==
              "SERVER.SERVICE.PLATFORM_UNSUPPORTED",
          "non-Windows service dispatch diagnostic mismatch");
#endif
}

}  // namespace

int main() {
  TestExactServiceIdentityContract();
  TestThreadSafeParserServerStopRequest();
  TestReadyThenGracefulStopLifecycle();
  TestStartupStopAndFailureLifecycle();
  TestServiceDispatchFailsClosedOutsideScm();
  return EXIT_SUCCESS;
}
