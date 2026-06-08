// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

#include "metadata_shaping.h"

namespace {

using scratchbird::cli::metadata::SchemaTreeRow;
using scratchbird::cli::metadata::SchemaTreeRowKind;

bool expect(bool condition, const std::string& message, int* failures) {
    if (condition) {
        return true;
    }
    std::cerr << "FAIL: " << message << "\n";
    if (failures != nullptr) {
        ++(*failures);
    }
    return false;
}

const SchemaTreeRow* findRowByPath(const std::vector<SchemaTreeRow>& rows,
                                   const std::string& path) {
    for (const auto& row : rows) {
        if (row.path == path) {
            return &row;
        }
    }
    return nullptr;
}

void testDatabaseRowDefaultsAndTopBranch(int* failures) {
    const std::vector<SchemaTreeRow> rows =
        scratchbird::cli::metadata::buildSchemaTreeRows(
            {"users.alice.dev"},
            "",
            false);

    expect(!rows.empty(), "rows should not be empty", failures);
    expect(rows[0].kind == SchemaTreeRowKind::kDatabase,
           "first row should be database row",
           failures);
    expect(rows[0].name == "default",
           "database row should default to 'default'",
           failures);

    const SchemaTreeRow* users = findRowByPath(rows, "users");
    expect(users != nullptr, "users branch should exist", failures);
    if (users != nullptr) {
        expect(users->parent_path == "default",
               "top-level schema parent should point at database row",
               failures);
        expect(users->top_level_branch,
               "top-level schema should be marked as top-level branch",
               failures);
    }
}

void testDottedParentExpansion(int* failures) {
    const std::vector<std::string> expanded =
        scratchbird::cli::metadata::schemaPathsForNavigation(
            {"users.alice.dev", "users.bob.dev", "users.bob.dev"},
            true);

    const std::vector<std::string> expected{
        "users",
        "users.alice",
        "users.alice.dev",
        "users.bob",
        "users.bob.dev",
    };
    expect(expanded == expected,
           "dotted parent expansion should add unique ancestors in order",
           failures);
}

void testPerParentUniqueness(int* failures) {
    const std::vector<SchemaTreeRow> rows =
        scratchbird::cli::metadata::buildSchemaTreeRows(
            {"users.bob.dev", "users.bob.dev"},
            "main",
            false);

    const int copies = static_cast<int>(std::count_if(
        rows.begin(), rows.end(), [](const SchemaTreeRow& row) {
            return row.path == "users.bob.dev";
        }));
    expect(copies == 1,
           "duplicate schema leaf under same parent should be emitted once",
           failures);
}

void testSameLeafUnderDifferentParents(int* failures) {
    const std::vector<SchemaTreeRow> rows =
        scratchbird::cli::metadata::buildSchemaTreeRows(
            {"users.alice.orders", "users.bob.orders"},
            "main",
            false);

    const SchemaTreeRow* alice_orders = findRowByPath(rows, "users.alice.orders");
    const SchemaTreeRow* bob_orders = findRowByPath(rows, "users.bob.orders");
    expect(alice_orders != nullptr, "users.alice.orders should exist", failures);
    expect(bob_orders != nullptr, "users.bob.orders should exist", failures);
    if (alice_orders != nullptr && bob_orders != nullptr) {
        expect(alice_orders->name == "orders",
               "alice leaf should preserve orders name",
               failures);
        expect(bob_orders->name == "orders",
               "bob leaf should preserve orders name",
               failures);
        expect(alice_orders->parent_path != bob_orders->parent_path,
               "same leaf name under different parents should remain distinct",
               failures);
    }
}

}  // namespace

int main() {
    int failures = 0;
    testDatabaseRowDefaultsAndTopBranch(&failures);
    testDottedParentExpansion(&failures);
    testPerParentUniqueness(&failures);
    testSameLeafUnderDifferentParents(&failures);

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }

    std::cout << "metadata_shaping_test: PASS\n";
    return 0;
}
