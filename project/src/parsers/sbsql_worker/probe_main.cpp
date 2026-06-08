// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "runtime/parser_runtime.hpp"

int main(int argc, char** argv) {
  auto config = scratchbird::parser::sbsql::ConfigFromArgs(argc, argv, true);
  config.probe_mode = true;
  config.allow_probe_auth = true;
  config.bundle_contract_id = "sbp_probe@1";
  return scratchbird::parser::sbsql::RunParserWorker(std::move(config));
}
