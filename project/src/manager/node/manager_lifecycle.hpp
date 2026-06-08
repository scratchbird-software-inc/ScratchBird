// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SBMI_NODE_MANAGER_LIFECYCLE

#pragma once

#include "manager_protocol.hpp"

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace scratchbird::manager::node {

enum class ManagerLifecycleState {
  kCreated,
  kArgsParsed,
  kConfigLoading,
  kConfigValidating,
  kRuntimePreparing,
  kOwnerAcquiring,
  kDaemonizing,
  kServerEndpointResolving,
  kServerSupervisionStarting,
  kListenerEndpointResolving,
  kListenerSupervisionStarting,
  kProxyBinding,
  kManagementBinding,
  kServerHeartbeatStarting,
  kReady,
  kRestricted,
  kDraining,
  kStopping,
  kStopped,
  kStartupFailed,
  kFailedTerminal,
  kQuarantined,
};

std::string ManagerLifecycleStateName(ManagerLifecycleState state);

class ManagerLifecycle {
 public:
  explicit ManagerLifecycle(std::filesystem::path control_dir);

  ManagerLifecycleState current() const;
  std::string CurrentName() const;

  bool Transition(ManagerLifecycleState next,
                  const std::string& detail,
                  std::vector<scratchbird::manager::protocol::Diagnostic>* diagnostics);

  bool Evidence(const std::string& state, const std::string& detail);

 private:
  bool IsLegalTransition(ManagerLifecycleState from, ManagerLifecycleState to) const;
  bool WriteStateLocked(ManagerLifecycleState state,
                        std::vector<scratchbird::manager::protocol::Diagnostic>* diagnostics);
  bool AppendJournalLocked(const std::string& state,
                           const std::string& detail,
                           std::vector<scratchbird::manager::protocol::Diagnostic>* diagnostics);
  bool CleanupStaleStateTempsLocked(
      std::vector<scratchbird::manager::protocol::Diagnostic>* diagnostics);

  std::filesystem::path control_dir_;
  mutable std::mutex mutex_;
  ManagerLifecycleState current_ = ManagerLifecycleState::kCreated;
};

}  // namespace scratchbird::manager::node
