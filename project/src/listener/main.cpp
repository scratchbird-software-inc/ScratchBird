// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include <iostream>

#include "listener_runtime.hpp"

int main(int argc, char** argv) {
  auto result = scratchbird::listener::RunListenerFromArgs(argc, argv);
  if (!result.messages.diagnostics.empty()) {
    std::ostream& out = result.exit_code == 0 ? std::cout : std::cerr;
    out << scratchbird::listener::MessageVectorSetJson(result.messages) << '\n';
  }
  return result.exit_code;
}
