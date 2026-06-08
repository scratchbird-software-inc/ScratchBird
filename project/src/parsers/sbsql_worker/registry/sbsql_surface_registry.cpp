// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "registry/sbsql_surface_registry.hpp"

#include "common/common.hpp"

#include <array>

namespace scratchbird::parser::sbsql {
namespace {

constexpr std::array<SurfaceRegistryRow, 12> kRows{{
    {"sbsql.query.relational.v3", "query", "sblr.query.relational.v3", 3},
    {"sbsql.query.values.v3", "values", "sblr.query.values.v3", 3},
    {"sbsql.dml.insert.v3", "insert", "sblr.dml.insert.v3", 3},
    {"sbsql.dml.update.v3", "update", "sblr.dml.update.v3", 3},
    {"sbsql.dml.delete.v3", "delete", "sblr.dml.delete.v3", 3},
    {"sbsql.dml.merge.v3", "merge", "sblr.dml.merge.v3", 3},
    {"sbsql.dml.upsert.v3", "upsert", "sblr.dml.operation.v3", 3},
    {"sbsql.catalog.mutation.v3", "catalog", "sblr.catalog.mutation.v3", 3},
    {"sbsql.management.show.v3", "show", "sblr.management.show.v3", 3},
    {"sbsql.session.setting.v3", "session", "sblr.session.setting.v3", 3},
    {"sbsql.transaction.control.v3", "transaction", "sblr.transaction.control.v3", 3},
    {"sbsql.routine.call.v3", "call", "sblr.routine.call.v3", 3},
}};

} // namespace

std::span<const SurfaceRegistryRow> BuiltinSurfaceRegistryRows() { return kRows; }

const SurfaceRegistryRow* FindSurfaceRegistryRow(std::string_view statement_family) {
  const auto wanted = ToUpperAscii(statement_family);
  for (const auto& row : kRows) {
    if (ToUpperAscii(row.statement_family) == wanted) return &row;
  }
  return nullptr;
}

} // namespace scratchbird::parser::sbsql
