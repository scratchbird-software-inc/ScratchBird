// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "common/common.hpp"

#include <cctype>

namespace scratchbird::parser::sbsql {

std::string StateName(ParserState state) {
  switch (state) {
    case ParserState::kSpawned: return "spawned";
    case ParserState::kPackageAdmitted: return "package_admitted";
    case ParserState::kInitializing: return "initializing";
    case ParserState::kWireReady: return "wire_ready";
    case ParserState::kIdlePreAuth: return "idle_preauth";
    case ParserState::kClientConnected: return "client_connected";
    case ParserState::kAuthenticating: return "authenticating";
    case ParserState::kAuthenticated: return "authenticated";
    case ParserState::kActive: return "active";
    case ParserState::kDraining: return "draining";
    case ParserState::kRecycled: return "recycled";
    case ParserState::kDisconnected: return "disconnected";
    case ParserState::kTerminating: return "terminating";
    case ParserState::kFailed: return "failed";
    case ParserState::kQuarantined: return "quarantined";
  }
  return "failed";
}

std::string TrimAscii(std::string_view text) {
  std::size_t begin = 0;
  while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
  std::size_t end = text.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
  return std::string(text.substr(begin, end - begin));
}

std::uint64_t Fnv1a64(std::string_view text) {
  std::uint64_t hash = 14695981039346656037ull;
  for (const unsigned char c : text) {
    hash ^= static_cast<std::uint64_t>(c);
    hash *= 1099511628211ull;
  }
  return hash;
}

} // namespace scratchbird::parser::sbsql
