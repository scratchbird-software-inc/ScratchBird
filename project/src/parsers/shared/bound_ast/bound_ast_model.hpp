// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "ast_model.hpp"

#include <string>
#include <string_view>
#include <variant>

namespace scratchbird::parser::bound_ast {

// Bound-AST is validated operation input. It is not execution authority until
// lowered into a SBLR/internal envelope and accepted by engine-side gates.

struct BindingContext {
  std::string database_uuid;
  std::string principal_uuid;
  std::string catalog_epoch;
  std::string registry_snapshot_uuid;
  std::string package_profile = "public_node";
};

struct BindingDiagnostic {
  std::string code;
  std::string message;
  scratchbird::parser::ast::SourceRange source_range;
};

struct BoundHeader {
  std::uint32_t bound_ast_format_version = 1;
  std::string surface_key;
  std::string command_family;
  std::string database_uuid;
  std::string principal_uuid;
  std::string catalog_epoch;
  std::string registry_snapshot_uuid;
  std::string required_right;
  std::string scope_mode;
  std::string edition_gate_result;
  std::string profile_gate_result;
  std::string sblr_operation_key;
  std::string result_shape;
  std::string diagnostic_shape;
  std::string trace_key;
};

struct BoundShowIdentity {
  BoundHeader header;
  scratchbird::parser::ast::ShowIdentityKind show_kind =
      scratchbird::parser::ast::ShowIdentityKind::kVersion;
};

struct BindResult {
  std::variant<BoundShowIdentity, BindingDiagnostic> value;
  bool ok() const;
};

BindResult BindShowIdentityAst(const scratchbird::parser::ast::ShowIdentityAst& ast,
                               const BindingContext& context);

std::string SerializeToJson(const BoundShowIdentity& bound);
std::string SerializeDiagnosticToJson(const BindingDiagnostic& diagnostic);
std::string SerializeBindResultToJson(const BindResult& result);

}  // namespace scratchbird::parser::bound_ast
