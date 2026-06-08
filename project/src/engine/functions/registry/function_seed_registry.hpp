// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "registry/function_registry.hpp"

#include <string>
#include <vector>

namespace scratchbird::engine::functions {

struct FunctionNameSeedRow {
  std::string function_uuid;
  std::string language;
  std::string canonical_name;
  std::string localized_name;
  std::string name_class;
  std::string name_lookup_seed_id;
  std::string canonical_function_id;
  std::string name_namespace;
  std::string target_kind;
  std::string parser_profile;
  std::string notes;
};

struct FunctionSeedPackage {
  // Active runtime registry used by SBLR function dispatch. This includes
  // canonical builtin IDs such as sb.scalar.*, sb.vector.*, and sb.aggregate.*.
  FunctionRegistry registry;
  // Database-create catalog seed registry. This contains only fixed catalog
  // function objects that have name seed rows in the canonical function packet.
  FunctionRegistry catalog_registry;
  std::vector<FunctionNameSeedRow> name_rows;
};

FunctionSeedPackage BuildStandardFunctionSeedPackage();

}  // namespace scratchbird::engine::functions
