// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::parser::sbsql {

struct ReferenceVariableCompatibilityDescriptor {
  std::string_view surface_id;
  std::string_view reference_spelling;
  std::string_view reference_family;
  std::string_view canonical_variable_id;
  std::string_view canonical_function_id;
  std::string_view canonical_surface_id;
  std::string_view canonical_sbsql_name;
  std::string_view sblr_operation_id;
  std::string_view sblr_opcode;
  std::string_view sblr_operation_family;
  std::string_view diagnostic_id;
  bool native_sbsql_surface = false;
  bool reference_parser_only = true;
  bool exact_refusal = false;
};

struct ReferenceVariableSblrOperand {
  std::string type;
  std::string name;
  std::string value;
};

struct ReferenceVariableSblrBinding {
  std::string surface_id;
  std::string reference_spelling;
  std::string sblr_operation_id;
  std::string sblr_opcode;
  std::string canonical_variable_id;
  std::string diagnostic_id;
  bool exact_refusal = false;
  std::vector<ReferenceVariableSblrOperand> operands;
};

std::span<const ReferenceVariableCompatibilityDescriptor>
BuiltinReferenceVariableCompatibilityDescriptors();
const ReferenceVariableCompatibilityDescriptor*
FindReferenceVariableCompatibilityBySurfaceId(std::string_view surface_id);
const ReferenceVariableCompatibilityDescriptor*
FindReferenceVariableCompatibilityBySpelling(std::string_view reference_spelling);
bool IsReferenceVariableCompatibilitySurface(std::string_view surface_id);
bool IsReferenceVariableCompatibilitySpelling(std::string_view reference_spelling);
ReferenceVariableSblrBinding LowerReferenceVariableCompatibilityBySpelling(
    std::string_view reference_spelling);

}  // namespace scratchbird::parser::sbsql
