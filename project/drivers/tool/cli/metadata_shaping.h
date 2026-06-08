// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::cli::metadata {

enum class SchemaTreeRowKind : uint8_t {
    kDatabase = 0,
    kSchema = 1,
};

struct SchemaTreeRow {
    SchemaTreeRowKind kind{SchemaTreeRowKind::kSchema};
    std::string database;
    std::string parent_path;
    std::string path;
    std::string name;
    bool terminal{false};
    bool top_level_branch{false};
};

struct ObjectResolverEntry {
    std::string object_type;
    std::string schema_path;
    std::string object_name;
};

// Returns normalized schema paths. When expand_schema_parents is true,
// each dotted path contributes all dotted ancestors in insertion order.
std::vector<std::string> schemaPathsForNavigation(
    const std::vector<std::string>& schema_names,
    bool expand_schema_parents);

// Builds database + schema rows for recursive tree navigation.
std::vector<SchemaTreeRow> buildSchemaTreeRows(
    const std::vector<std::string>& schema_names,
    const std::string& database,
    bool expand_schema_parents);

// Extracts schema paths from object_resolver rows in metadata-only mode.
std::vector<std::string> schemaPathsFromObjectResolver(
    const std::vector<ObjectResolverEntry>& entries);

}  // namespace scratchbird::cli::metadata
