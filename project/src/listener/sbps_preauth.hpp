// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <string>
#include <string_view>

#include "listener_diagnostics.hpp"

namespace scratchbird::listener {

enum class PreauthDecision {
  kAllowed,
  kDenied,
};

struct PreauthCheckResult {
  PreauthDecision decision{PreauthDecision::kDenied};
  proto::MessageVectorSet messages;
};

PreauthCheckResult CheckPreauthOperation(std::string_view operation_name);
std::string PreauthAllowedOperationsJson();

} // namespace scratchbird::listener
