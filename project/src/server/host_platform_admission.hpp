// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string>
#include <string_view>

namespace scratchbird::server {

// Local capability observations only: not a peer, package, or launch receipt.
// Native OS descriptors never escape this admission interface.
struct HostPlatformAdmission {
  bool supported = false;
  std::string platform;
  std::string kernel_release;
  std::string failed_capability;
  int native_error = 0;
};

bool MeetsLinuxServerKernelMinimum(std::string_view release);
HostPlatformAdmission ProbeServerHostPlatform();

}  // namespace scratchbird::server
