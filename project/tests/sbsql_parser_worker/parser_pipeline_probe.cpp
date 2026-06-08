// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ast/ast.hpp"
#include "binder/binder.hpp"
#include "cst/cst.hpp"
#include "lowering/lowering.hpp"

#include <cstdlib>
#include <iostream>

int main() {
  using namespace scratchbird::parser::sbsql;
  ParserConfig config;
  config.probe_mode = true;
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "00000000-0000-7000-8000-000000000001";
  session.catalog_epoch = 1;
  session.security_policy_epoch = 1;
  session.descriptor_epoch = 1;
  auto cst = BuildCst("select 1");
  auto ast = BuildAst(cst);
  auto bound = BindAst(ast, cst, config, session);
  auto lowered = LowerToSblr(bound, cst, session);
  if (lowered.payload.empty() || lowered.messages.has_errors()) {
    std::cerr << "parser pipeline probe failed\n";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
