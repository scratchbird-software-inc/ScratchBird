// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <string_view>
#include <span>

namespace scratchbird::parser::sbsql {

struct SurfaceRegistryRow {
  std::string_view surface_id;
  std::string_view statement_family;
  std::string_view sblr_operation_family;
  std::uint32_t version;
};

std::span<const SurfaceRegistryRow> BuiltinSurfaceRegistryRows();
const SurfaceRegistryRow* FindSurfaceRegistryRow(std::string_view statement_family);

} // namespace scratchbird::parser::sbsql
