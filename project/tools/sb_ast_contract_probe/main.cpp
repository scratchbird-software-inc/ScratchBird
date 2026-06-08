// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ast_model.hpp"
#include "bound_ast_model.hpp"
#include "sblr_envelope.hpp"

#include <iostream>
#include <string>

int main() {
  namespace ast = scratchbird::parser::ast;
  namespace bound = scratchbird::parser::bound_ast;
  namespace lowering = scratchbird::parser::lowering;

  const auto ast_node = ast::MakeShowIdentityAst(ast::ShowIdentityKind::kVersion,
                                                 "show version",
                                                 "sbsql.show.version",
                                                 {0, 12});
  bound::BindingContext context;
  context.database_uuid = "018f0000-0000-7000-8000-000000000001";
  context.principal_uuid = "018f0000-0000-7000-8000-000000000002";
  context.catalog_epoch = "epoch-1";
  context.registry_snapshot_uuid = "018f0000-0000-7000-8000-000000000003";
  const auto bound_result = bound::BindShowIdentityAst(ast_node, context);
  if (!bound_result.ok()) {
    std::cout << "{\"ok\":false,\"phase\":\"bind\"}\n";
    return 1;
  }
  const auto* bound_node = std::get_if<bound::BoundShowIdentity>(&bound_result.value);
  if (bound_node == nullptr || bound_node->header.sblr_operation_key != "op.show.version") {
    std::cout << "{\"ok\":false,\"phase\":\"bound_contract\"}\n";
    return 1;
  }
  const auto lowered = lowering::LowerBoundShowIdentity(*bound_node);
  if (!lowered.ok()) {
    std::cout << "{\"ok\":false,\"phase\":\"lower\"}\n";
    return 1;
  }
  const auto* envelope = std::get_if<lowering::LogicalEnvelope>(&lowered.value);
  const bool ok = envelope != nullptr && envelope->operation_key == "op.show.version" &&
                  envelope->database_uuid == context.database_uuid &&
                  envelope->principal_uuid == context.principal_uuid;
  std::cout << "{\"ok\":" << (ok ? "true" : "false")
            << ",\"ast_family\":\"" << ast::ToString(ast_node.header.family)
            << "\",\"operation_key\":\"" << (envelope ? envelope->operation_key : "") << "\"}\n";
  return ok ? 0 : 1;
}
