// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_WINDOWS_SCM_RUNTIME

#include "windows_service_runtime.hpp"

#include "ipc_server.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <string>
#include <thread>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace scratchbird::server {

namespace {

constexpr std::uint32_t kServiceStartWaitHintMs = 30000;
constexpr std::uint32_t kServiceStopWaitHintMs = 30000;
constexpr auto kPendingStatusHeartbeat = std::chrono::seconds(5);

ServerDiagnostic ServiceDiagnostic(std::string code,
                                   std::string message,
                                   std::uint32_t platform_error = 0) {
  std::vector<ServerDiagnosticField> fields;
  if (platform_error != 0) {
    fields.push_back({"platform_error", std::to_string(platform_error)});
  }
  return {std::move(code),
          "server.service.scm_runtime_failed",
          ServerDiagnosticSeverity::kError,
          std::move(message),
          std::move(fields)};
}

#if defined(_WIN32)

class PlatformWindowsServiceHost final {
 public:
  WindowsServiceDispatchResult Dispatch(const WindowsServiceWorker& worker) {
    WindowsServiceDispatchResult result;
    if (!worker) {
      result.exit_code = 2;
      result.diagnostics.push_back(ServiceDiagnostic(
          "SERVER.SERVICE.WORKER_REQUIRED",
          "The Windows service runtime requires a server worker."));
      return result;
    }

    PlatformWindowsServiceHost* expected = nullptr;
    if (!active_host_.compare_exchange_strong(expected, this,
                                              std::memory_order_acq_rel)) {
      result.exit_code = 2;
      result.diagnostics.push_back(ServiceDiagnostic(
          "SERVER.SERVICE.DISPATCH_ALREADY_ACTIVE",
          "A Windows service dispatcher is already active in this process."));
      return result;
    }

    worker_ = worker;
    worker_exit_code_.store(2, std::memory_order_release);
    status_publish_error_.store(ERROR_SUCCESS, std::memory_order_release);

    SERVICE_TABLE_ENTRYW dispatch_table[] = {
        {const_cast<LPWSTR>(kScratchBirdWindowsServiceName),
         &PlatformWindowsServiceHost::ServiceMainThunk},
        {nullptr, nullptr},
    };
    const BOOL dispatched = ::StartServiceCtrlDispatcherW(dispatch_table);
    const DWORD dispatch_error = dispatched ? ERROR_SUCCESS : ::GetLastError();
    active_host_.store(nullptr, std::memory_order_release);

    if (!dispatched) {
      result.exit_code = 2;
      if (dispatch_error == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
        result.diagnostics.push_back(ServiceDiagnostic(
            "SERVER.SERVICE.SCM_REQUIRED",
            "Windows service mode was requested outside the Service Control Manager.",
            dispatch_error));
      } else {
        result.diagnostics.push_back(ServiceDiagnostic(
            "SERVER.SERVICE.DISPATCH_FAILED",
            "The Windows Service Control Manager dispatcher could not be started.",
            dispatch_error));
      }
      return result;
    }

    const DWORD status_error = status_publish_error_.load(std::memory_order_acquire);
    if (status_error != ERROR_SUCCESS) {
      result.exit_code = 2;
      result.diagnostics.push_back(ServiceDiagnostic(
          "SERVER.SERVICE.STATUS_PUBLISH_FAILED",
          "The Windows service status could not be published to the Service Control Manager.",
          status_error));
      return result;
    }

    result.exit_code = worker_exit_code_.load(std::memory_order_acquire);
    return result;
  }

 private:
  static void WINAPI ServiceMainThunk(DWORD argc, LPWSTR* argv) {
    (void)argc;
    (void)argv;
    if (auto* host = active_host_.load(std::memory_order_acquire)) {
      host->ServiceMain();
    }
  }

  static DWORD WINAPI ServiceControlThunk(DWORD control,
                                          DWORD event_type,
                                          LPVOID event_data,
                                          LPVOID context) {
    (void)event_type;
    (void)event_data;
    auto* host = static_cast<PlatformWindowsServiceHost*>(context);
    return host == nullptr ? ERROR_INVALID_HANDLE : host->HandleControl(control);
  }

  void ServiceMain() {
    status_handle_ = ::RegisterServiceCtrlHandlerExW(
        kScratchBirdWindowsServiceName,
        &PlatformWindowsServiceHost::ServiceControlThunk,
        this);
    if (status_handle_ == nullptr) {
      worker_exit_code_.store(2, std::memory_order_release);
      return;
    }

    if (!PublishStatus(lifecycle_.BeginStart())) {
      worker_exit_code_.store(2, std::memory_order_release);
      RequestParserServerStop();
      return;
    }

    WindowsServiceWorkerCallbacks callbacks;
    callbacks.report_ready = [this]() {
      const auto status = lifecycle_.MarkRunning();
      if (status && !PublishStatus(*status)) {
        RequestParserServerStop();
      }
    };
    callbacks.report_stopping = [this]() {
      const auto status = lifecycle_.MarkStopPending();
      if (status && !PublishStatus(*status)) {
        RequestParserServerStop();
      }
    };

    {
      std::lock_guard<std::mutex> guard(worker_done_mutex_);
      worker_done_ = false;
    }
    std::thread worker_thread([this, callbacks]() {
      int exit_code = 2;
      try {
        exit_code = worker_(callbacks);
      } catch (...) {
        RequestParserServerStop();
        exit_code = 2;
      }
      worker_exit_code_.store(exit_code, std::memory_order_release);
      {
        std::lock_guard<std::mutex> guard(worker_done_mutex_);
        worker_done_ = true;
      }
      worker_done_cv_.notify_one();
    });

    for (;;) {
      std::unique_lock<std::mutex> lock(worker_done_mutex_);
      if (worker_done_cv_.wait_for(lock, kPendingStatusHeartbeat,
                                   [this] { return worker_done_; })) {
        break;
      }
      lock.unlock();
      if (const auto pending = lifecycle_.AdvancePendingCheckpoint()) {
        if (!PublishStatus(*pending)) {
          RequestParserServerStop();
        }
      }
    }
    worker_thread.join();

    if (const auto stopping = lifecycle_.MarkStopPending()) {
      PublishStatus(*stopping);
    }
    const int exit_code = worker_exit_code_.load(std::memory_order_acquire);
    PublishStatus(lifecycle_.MarkStopped(exit_code));
  }

  DWORD HandleControl(DWORD control) {
    WindowsServiceControl mapped = WindowsServiceControl::kUnsupported;
    switch (control) {
      case SERVICE_CONTROL_STOP:
        mapped = WindowsServiceControl::kStop;
        break;
      case SERVICE_CONTROL_SHUTDOWN:
        mapped = WindowsServiceControl::kShutdown;
        break;
      case SERVICE_CONTROL_INTERROGATE:
        mapped = WindowsServiceControl::kInterrogate;
        break;
      default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }

    const auto result = lifecycle_.HandleControl(mapped);
    if (!result.handled) {
      return ERROR_CALL_NOT_IMPLEMENTED;
    }
    if (result.publish_status && !PublishStatus(result.status)) {
      RequestParserServerStop();
      return status_publish_error_.load(std::memory_order_acquire);
    }
    if (result.request_stop) {
      RequestParserServerStop();
    }
    return ERROR_SUCCESS;
  }

  bool PublishStatus(const WindowsServiceStatusSnapshot& snapshot) {
    std::lock_guard<std::mutex> guard(status_publish_mutex_);
    const auto current = lifecycle_.CurrentStatus();
    if (current.state != snapshot.state ||
        current.checkpoint != snapshot.checkpoint ||
        current.process_exit_code != snapshot.process_exit_code) {
      return true;
    }

    SERVICE_STATUS status{};
    status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    status.dwCurrentState = ToPlatformState(snapshot.state);
    if (snapshot.accepts_stop) {
      status.dwControlsAccepted |= SERVICE_ACCEPT_STOP;
    }
    if (snapshot.accepts_shutdown) {
      status.dwControlsAccepted |= SERVICE_ACCEPT_SHUTDOWN;
    }
    status.dwCheckPoint = snapshot.checkpoint;
    status.dwWaitHint = snapshot.wait_hint_ms;
    if (snapshot.state == WindowsServiceLifecycleState::kStopped &&
        snapshot.process_exit_code != 0) {
      status.dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR;
      const auto positive_exit = snapshot.process_exit_code < 0
                                     ? 1u
                                     : static_cast<std::uint64_t>(snapshot.process_exit_code);
      status.dwServiceSpecificExitCode = static_cast<DWORD>(
          std::min<std::uint64_t>(positive_exit,
                                  std::numeric_limits<DWORD>::max()));
    } else {
      status.dwWin32ExitCode = ERROR_SUCCESS;
      status.dwServiceSpecificExitCode = 0;
    }

    if (::SetServiceStatus(status_handle_, &status) == FALSE) {
      const DWORD error = ::GetLastError();
      DWORD expected = ERROR_SUCCESS;
      status_publish_error_.compare_exchange_strong(expected, error,
                                                    std::memory_order_acq_rel);
      return false;
    }
    return true;
  }

  static DWORD ToPlatformState(WindowsServiceLifecycleState state) {
    switch (state) {
      case WindowsServiceLifecycleState::kStartPending:
        return SERVICE_START_PENDING;
      case WindowsServiceLifecycleState::kRunning:
        return SERVICE_RUNNING;
      case WindowsServiceLifecycleState::kStopPending:
        return SERVICE_STOP_PENDING;
      case WindowsServiceLifecycleState::kStopped:
        return SERVICE_STOPPED;
    }
    return SERVICE_STOPPED;
  }

  static std::atomic<PlatformWindowsServiceHost*> active_host_;

  WindowsServiceWorker worker_;
  WindowsServiceLifecycleModel lifecycle_;
  SERVICE_STATUS_HANDLE status_handle_ = nullptr;
  std::mutex status_publish_mutex_;
  std::mutex worker_done_mutex_;
  std::condition_variable worker_done_cv_;
  bool worker_done_ = false;
  std::atomic<int> worker_exit_code_{2};
  std::atomic<DWORD> status_publish_error_{ERROR_SUCCESS};
};

std::atomic<PlatformWindowsServiceHost*> PlatformWindowsServiceHost::active_host_{nullptr};

#endif

}  // namespace

WindowsServiceStatusSnapshot WindowsServiceLifecycleModel::BeginStart() {
  std::lock_guard<std::mutex> guard(mutex_);
  stop_requested_ = false;
  status_ = {};
  status_.state = WindowsServiceLifecycleState::kStartPending;
  status_.checkpoint = 1;
  status_.wait_hint_ms = kServiceStartWaitHintMs;
  return status_;
}

std::optional<WindowsServiceStatusSnapshot>
WindowsServiceLifecycleModel::MarkRunning() {
  std::lock_guard<std::mutex> guard(mutex_);
  if (status_.state != WindowsServiceLifecycleState::kStartPending ||
      stop_requested_) {
    return std::nullopt;
  }
  status_.state = WindowsServiceLifecycleState::kRunning;
  status_.accepts_stop = true;
  status_.accepts_shutdown = true;
  status_.checkpoint = 0;
  status_.wait_hint_ms = 0;
  return status_;
}

WindowsServiceStatusSnapshot WindowsServiceLifecycleModel::MarkStopPendingLocked() {
  status_.state = WindowsServiceLifecycleState::kStopPending;
  status_.accepts_stop = false;
  status_.accepts_shutdown = false;
  status_.checkpoint = 1;
  status_.wait_hint_ms = kServiceStopWaitHintMs;
  return status_;
}

std::optional<WindowsServiceStatusSnapshot>
WindowsServiceLifecycleModel::MarkStopPending() {
  std::lock_guard<std::mutex> guard(mutex_);
  if (status_.state == WindowsServiceLifecycleState::kStopped ||
      status_.state == WindowsServiceLifecycleState::kStopPending) {
    return std::nullopt;
  }
  return MarkStopPendingLocked();
}

std::optional<WindowsServiceStatusSnapshot>
WindowsServiceLifecycleModel::AdvancePendingCheckpoint() {
  std::lock_guard<std::mutex> guard(mutex_);
  if (status_.state != WindowsServiceLifecycleState::kStartPending &&
      status_.state != WindowsServiceLifecycleState::kStopPending) {
    return std::nullopt;
  }
  if (status_.checkpoint < std::numeric_limits<std::uint32_t>::max()) {
    ++status_.checkpoint;
  }
  return status_;
}

WindowsServiceStatusSnapshot WindowsServiceLifecycleModel::MarkStopped(
    int process_exit_code) {
  std::lock_guard<std::mutex> guard(mutex_);
  status_.state = WindowsServiceLifecycleState::kStopped;
  status_.accepts_stop = false;
  status_.accepts_shutdown = false;
  status_.checkpoint = 0;
  status_.wait_hint_ms = 0;
  status_.process_exit_code = process_exit_code;
  return status_;
}

WindowsServiceControlResult WindowsServiceLifecycleModel::HandleControl(
    WindowsServiceControl control) {
  std::lock_guard<std::mutex> guard(mutex_);
  WindowsServiceControlResult result;
  result.status = status_;
  if (control == WindowsServiceControl::kInterrogate) {
    result.handled = true;
    result.publish_status = true;
    return result;
  }
  if (control != WindowsServiceControl::kStop &&
      control != WindowsServiceControl::kShutdown) {
    return result;
  }

  result.handled = true;
  if (status_.state == WindowsServiceLifecycleState::kStopped) {
    return result;
  }
  if (!stop_requested_) {
    stop_requested_ = true;
    result.request_stop = true;
  }
  if (status_.state != WindowsServiceLifecycleState::kStopPending) {
    result.status = MarkStopPendingLocked();
    result.publish_status = true;
  } else {
    result.status = status_;
  }
  return result;
}

WindowsServiceStatusSnapshot WindowsServiceLifecycleModel::CurrentStatus() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return status_;
}

WindowsServiceDispatchResult DispatchWindowsServerService(
    const WindowsServiceWorker& worker) {
#if defined(_WIN32)
  PlatformWindowsServiceHost host;
  return host.Dispatch(worker);
#else
  (void)worker;
  WindowsServiceDispatchResult result;
  result.exit_code = 2;
  result.diagnostics.push_back(ServiceDiagnostic(
      "SERVER.SERVICE.PLATFORM_UNSUPPORTED",
      "Windows service mode is unavailable on this platform."));
  return result;
#endif
}

}  // namespace scratchbird::server
