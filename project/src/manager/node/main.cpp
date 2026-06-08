// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SBMI_NODE_MANAGER_MAIN

#include "manager_runtime.hpp"

#include <iostream>

namespace {

int Emit(const std::vector<scratchbird::manager::protocol::Diagnostic>& diagnostics) {
  for (const auto& diagnostic : diagnostics) {
    std::cerr << scratchbird::manager::protocol::ToMessageVectorJsonLine(diagnostic) << '\n';
  }
  return diagnostics.empty() ? 0 : 2;
}

}  // namespace

int main(int argc, char** argv) {
  const auto parsed = scratchbird::manager::node::ParseManagerCli(argc, argv);
  if (!parsed.ok()) return Emit(parsed.diagnostics);
  if (parsed.config.help) {
    std::cout << scratchbird::manager::node::HelpText();
    return 0;
  }
  if (parsed.config.version) {
    std::cout << scratchbird::manager::node::ProductVersionLine() << '\n';
    return 0;
  }
  const auto result = scratchbird::manager::node::RunManager(parsed.config);
  if (!result.diagnostics.empty()) Emit(result.diagnostics);
  return result.exit_code;
}
