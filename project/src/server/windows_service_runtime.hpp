// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_WINDOWS_SCM_RUNTIME

#pragma once

#include "diagnostics.hpp"

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>

namespace scratchbird::server {

inline constexpr const wchar_t* kScratchBirdWindowsServiceName = L"scratchbird";

enum class WindowsServiceLifecycleState {
  kStartPending,
  kRunning,
  kStopPending,
  kStopped,
};

enum class WindowsServiceControl {
  kStop,
  kShutdown,
  kInterrogate,
  kUnsupported,
};

struct WindowsServiceStatusSnapshot {
  WindowsServiceLifecycleState state = WindowsServiceLifecycleState::kStopped;
  bool accepts_stop = false;
  bool accepts_shutdown = false;
  std::uint32_t checkpoint = 0;
  std::uint32_t wait_hint_ms = 0;
  int process_exit_code = 0;
};

struct WindowsServiceControlResult {
  bool handled = false;
  bool publish_status = false;
  bool request_stop = false;
  WindowsServiceStatusSnapshot status;
};

// Portable, thread-safe SCM lifecycle model. The Windows host maps these
// snapshots to SERVICE_STATUS; tests exercise the state/control contract
// without registering or mutating a service on the host machine.
class WindowsServiceLifecycleModel {
 public:
  WindowsServiceStatusSnapshot BeginStart();
  std::optional<WindowsServiceStatusSnapshot> MarkRunning();
  std::optional<WindowsServiceStatusSnapshot> MarkStopPending();
  std::optional<WindowsServiceStatusSnapshot> AdvancePendingCheckpoint();
  WindowsServiceStatusSnapshot MarkStopped(int process_exit_code);
  WindowsServiceControlResult HandleControl(WindowsServiceControl control);
  WindowsServiceStatusSnapshot CurrentStatus() const;

 private:
  WindowsServiceStatusSnapshot MarkStopPendingLocked();

  mutable std::mutex mutex_;
  WindowsServiceStatusSnapshot status_;
  bool stop_requested_ = false;
};

struct WindowsServiceWorkerCallbacks {
  std::function<void()> report_ready;
  std::function<void()> report_stopping;
};

using WindowsServiceWorker =
    std::function<int(const WindowsServiceWorkerCallbacks& callbacks)>;

struct WindowsServiceDispatchResult {
  int exit_code = 0;
  std::vector<ServerDiagnostic> diagnostics;

  bool ok() const { return exit_code == 0 && diagnostics.empty(); }
};

// On Windows this call must be made for --service before server startup. It
// blocks in StartServiceCtrlDispatcher until ServiceMain has stopped. On other
// platforms it fails closed and never invokes the worker.
WindowsServiceDispatchResult DispatchWindowsServerService(
    const WindowsServiceWorker& worker);

}  // namespace scratchbird::server
