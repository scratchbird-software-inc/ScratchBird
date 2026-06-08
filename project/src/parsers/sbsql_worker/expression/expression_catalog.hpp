// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace scratchbird::parser::sbsql {

enum class ExpressionSurfaceKind {
  kFunction,
  kOperator,
  kVariable,
};

enum class ExpressionAssociativity {
  kNone,
  kLeft,
  kRight,
  kPrefix,
  kPostfix,
};

struct ExpressionSurfaceDescriptor {
  std::string_view surface_id;
  std::string_view fixed_uuid_v7;
  std::string_view canonical_name;
  ExpressionSurfaceKind kind{ExpressionSurfaceKind::kFunction};
  std::string_view source_status;
  std::string_view cluster_scope;
  std::string_view parser_handler_key;
  std::string_view binder_descriptor_key;
  std::string_view lowering_descriptor_key;
  std::string_view engine_rule_key;
  std::string_view diagnostic_key;
  std::string_view final_acceptance_rule;
  std::string_view behavior_descriptor_key;
  std::string_view expression_class;
  std::string_view arity_class;
  std::uint16_t precedence{0};
  ExpressionAssociativity associativity{ExpressionAssociativity::kNone};
  bool exact_refusal_required{false};
};

std::span<const ExpressionSurfaceDescriptor> BuiltinExpressionSurfaceDescriptors();

const ExpressionSurfaceDescriptor* FindExpressionSurfaceById(std::string_view surface_id);
const ExpressionSurfaceDescriptor* FindExpressionSurfaceByName(std::string_view canonical_name);

std::optional<ExpressionSurfaceKind> ParseExpressionSurfaceKind(std::string_view kind);
std::string_view ExpressionSurfaceKindName(ExpressionSurfaceKind kind);
std::string_view ExpressionAssociativityName(ExpressionAssociativity associativity);

} // namespace scratchbird::parser::sbsql
