// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "bound_ast_model.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace scratchbird::parser::lowering {

// Logical envelope emitted by parser lowering. This is not executor acceptance;
// engine-side capability, rights, profile, and operation gates still apply.

struct Operand {
  std::string type;
  std::string name;
  std::string value;
};

struct LogicalEnvelope {
  std::uint32_t envelope_format_version = 1;
  std::string envelope_kind = "sblr_show_self_or_baseline";
  std::string operation_key;
  std::uint32_t operation_version = 1;
  std::string database_uuid;
  std::string principal_uuid;
  std::string registry_snapshot_uuid;
  std::string result_shape;
  std::string diagnostic_shape;
  std::string trace_key;
  std::vector<Operand> operands;
};

struct LoweringDiagnostic {
  std::string code;
  std::string message;
};

struct LoweringResult {
  std::variant<LogicalEnvelope, LoweringDiagnostic> value;
  bool ok() const;
};

LoweringResult LowerBoundShowIdentity(
    const scratchbird::parser::bound_ast::BoundShowIdentity& bound);

std::string SerializeToJson(const LogicalEnvelope& envelope);
std::string SerializeDiagnosticToJson(const LoweringDiagnostic& diagnostic);
std::string SerializeLoweringResultToJson(const LoweringResult& result);

}  // namespace scratchbird::parser::lowering
