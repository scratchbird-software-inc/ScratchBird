// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sbps_preauth.hpp"

#include <array>
#include <sstream>

namespace scratchbird::listener {
namespace {
constexpr std::array<std::string_view, 7> kAllowed = {
    "HELLO", "DBBT_VALIDATE", "AUTH_START", "AUTH_CONTINUE", "AUTH_CANCEL", "PROTOCOL_NEGOTIATE", "PING"};
}

PreauthCheckResult CheckPreauthOperation(std::string_view operation_name) {
  for (auto allowed : kAllowed) {
    if (operation_name == allowed) {
      return {PreauthDecision::kAllowed, MakeMessageVectorSet({})};
    }
  }
  std::vector<proto::Diagnostic> diagnostics;
  diagnostics.push_back(MakeDiagnostic("LISTENER.PREAUTH.OPERATION_DENIED", "ERROR",
                                       "pre-auth parser channel may only perform authentication and protocol negotiation operations",
                                       "sb_listener.preauth", {{"operation", std::string(operation_name)}}));
  return {PreauthDecision::kDenied, MakeMessageVectorSet(std::move(diagnostics))};
}

std::string PreauthAllowedOperationsJson() {
  std::ostringstream out;
  out << '[';
  for (std::size_t i = 0; i < kAllowed.size(); ++i) {
    if (i != 0) out << ',';
    out << '"' << kAllowed[i] << '"';
  }
  out << ']';
  return out.str();
}

} // namespace scratchbird::listener
