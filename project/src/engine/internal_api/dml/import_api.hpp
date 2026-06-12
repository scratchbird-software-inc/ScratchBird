// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SPDX-License-Identifier: Apache-2.0
// Copyright (c) ScratchBird contributors
//
// SB_ENGINE_INTERNAL_API_DML_IMPORT_API
// SB_PID007_IMPORT_SURFACE
//
// Import planning is an engine-owned API surface. Parsers may decode client
// syntax, client protocol packets, reference bulk APIs, and client-side file
// handles, but the engine only accepts canonical UUID/descriptors/policy
// envelopes and never executes SQL text.

#pragma once

#include <string>
#include <utility>
#include <vector>

#include "api_types.hpp"
#include "dml/import_reject_model.hpp"

namespace scratchbird::engine::internal_api {

struct EngineImportSourceEnvelope {
    std::string source_kind;
    EngineUuid source_uuid;
    std::string source_fingerprint;
    std::string source_position;
    std::string redacted_source_handle;
    bool source_handle_sensitive = true;
};

struct EngineImportFormatEnvelope {
    std::string format_family;
    std::string encoding;
    std::string line_ending;
    std::string delimiter;
    std::string quote;
    std::string escape;
    std::string header_policy;
    std::vector<std::string> null_markers;
    std::string date_time_profile;
    std::string timezone_profile;
    std::vector<std::pair<std::string, std::string>> format_options;
};

struct EngineImportColumnMapping {
    std::string source_field;
    std::string target_column;
    EngineDescriptor target_descriptor;
    bool required = false;
};

struct EngineImportPolicyEnvelope : public EngineImportRejectPolicyEnvelope {
    bool strict_bulk_load_requested = false;
    bool reference_relaxed_semantics_requested = false;
};

struct EnginePlanImportRowsRequest : public EngineApiRequest {
    EngineObjectReference target_table;
    EngineImportSourceEnvelope source;
    EngineImportFormatEnvelope format;
    std::vector<EngineImportColumnMapping> column_mappings;
    EngineImportPolicyEnvelope import_policy;
};

struct EnginePlanImportRowsResult : public EngineApiResult {
    bool surface_accepted = false;
    bool planning_only = true;
    bool execution_requires_execute_import_rows = false;
    bool row_execution_completed = false;
    std::string normalized_insert_mode;
    std::string normalized_source_kind;
    std::string normalized_format_family;
    EngineApiU64 mapped_column_count = 0;
};

EnginePlanImportRowsResult EnginePlanImportRows(const EnginePlanImportRowsRequest& request);

} // namespace scratchbird::engine::internal_api
