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
// SB_ENGINE_INTERNAL_API_DML_IMPORT_API_BEHAVIOR
// SB_PID007_IMPORT_SURFACE

#include "dml/import_api.hpp"

#include <initializer_list>
#include <string>

#include "api_diagnostics.hpp"

namespace scratchbird::engine::internal_api {
namespace {

bool OneOf(const std::string& value, const std::initializer_list<const char*> allowed) {
    for (const char* item : allowed) {
        if (value == item) {
            return true;
        }
    }
    return false;
}

EnginePlanImportRowsResult ImportFailure(const std::string& code,
                                         const std::string&,
                                         const std::string& detail) {
    EnginePlanImportRowsResult result;
    result.ok = false;
    result.operation_id = "dml.plan_import_rows";
    result.surface_accepted = false;
    result.planning_only = true;
    result.execution_requires_execute_import_rows = false;
    result.row_execution_completed = false;
    result.diagnostics.push_back(MakeInvalidRequestDiagnostic("dml.plan_import_rows", code + ":" + detail));
    return result;
}

bool SupportedSourceKind(const std::string& value) {
    return OneOf(value, {
        "native_sbsql_import",
        "csv_stream",
        "delimited_text",
        "fixed_width_text",
        "jsonl_stream",
        "document_stream",
        "binary_typed_rows",
        "donor_dump_replay",
        "donor_bulk_api",
        "live_ingest_stream",
    });
}

bool SupportedFormatFamily(const std::string& value) {
    return OneOf(value, {
        "csv",
        "delimited_text",
        "fixed_width",
        "jsonl",
        "document",
        "binary_typed_rows",
        "donor_dump",
        "donor_bulk",
        "live_ingest",
    });
}

bool SourceFormatPairAllowed(const std::string& source_kind, const std::string& format_family) {
    if (source_kind == "native_sbsql_import") {
        return OneOf(format_family, {"csv", "delimited_text", "fixed_width", "jsonl", "document", "binary_typed_rows"});
    }
    if (source_kind == "csv_stream") {
        return format_family == "csv";
    }
    if (source_kind == "delimited_text") {
        return OneOf(format_family, {"csv", "delimited_text"});
    }
    if (source_kind == "fixed_width_text") {
        return format_family == "fixed_width";
    }
    if (source_kind == "jsonl_stream") {
        return format_family == "jsonl";
    }
    if (source_kind == "document_stream") {
        return OneOf(format_family, {"document", "jsonl"});
    }
    if (source_kind == "binary_typed_rows") {
        return format_family == "binary_typed_rows";
    }
    if (source_kind == "donor_dump_replay") {
        return format_family == "donor_dump";
    }
    if (source_kind == "donor_bulk_api") {
        return format_family == "donor_bulk";
    }
    if (source_kind == "live_ingest_stream") {
        return format_family == "live_ingest";
    }
    return false;
}

std::string InsertModeForSource(const std::string& source_kind) {
    if (source_kind == "donor_bulk_api") {
        return "donor_bulk";
    }
    if (source_kind == "binary_typed_rows") {
        return "native_bulk";
    }
    if (source_kind == "live_ingest_stream") {
        return "copy_import";
    }
    return "copy_import";
}

} // namespace

EnginePlanImportRowsResult EnginePlanImportRows(const EnginePlanImportRowsRequest& request) {
    if (request.context.local_transaction_id == 0) {
        return ImportFailure(
            "import_transaction_context_required",
            "Import planning requires an engine-owned transaction context.",
            "Parsers cannot plan import execution outside an accepted engine transaction.");
    }

    if (!request.localized_names.empty()) {
        return ImportFailure(
            "import_names_not_allowed_engine_boundary",
            "Import planning does not accept names as authority.",
            "Parser name resolution must return target UUIDs before calling dml.plan_import_rows.");
    }

    if (request.target_table.uuid.canonical.empty()) {
        return ImportFailure(
            "import_target_uuid_required",
            "Import planning requires a target table UUID.",
            "The target table must be UUID-bound before engine import planning begins.");
    }

    if (request.source.source_kind.empty() || !SupportedSourceKind(request.source.source_kind)) {
        return ImportFailure(
            "import_source_kind_unsupported",
            "Import planning received an unsupported source kind.",
            request.source.source_kind.empty() ? "source_kind is empty" : request.source.source_kind);
    }

    if (request.format.format_family.empty() || !SupportedFormatFamily(request.format.format_family)) {
        return ImportFailure(
            "import_format_family_unsupported",
            "Import planning received an unsupported format family.",
            request.format.format_family.empty() ? "format_family is empty" : request.format.format_family);
    }

    if (!SourceFormatPairAllowed(request.source.source_kind, request.format.format_family)) {
        return ImportFailure(
            "import_source_format_mismatch",
            "Import source kind and format family are not compatible.",
            request.source.source_kind + " -> " + request.format.format_family);
    }

    if (request.source.source_kind == "sql_text" || request.format.format_family == "sql_text") {
        return ImportFailure(
            "import_sql_text_rejected",
            "The engine import API does not accept SQL text.",
            "Client dialect input must be lowered by the parser before the engine boundary.");
    }

    if (request.import_policy.donor_relaxed_semantics_requested &&
        !request.import_policy.strict_bulk_load_requested) {
        return ImportFailure(
            "import_donor_relaxed_semantics_requires_policy",
            "Donor-relaxed import semantics require an explicit strict or policy-authorized profile.",
            "The default engine import surface fails closed rather than inheriting donor shortcuts.");
    }

    if (request.import_policy.reject_limit_percent < 0.0 ||
        request.import_policy.reject_limit_percent > 100.0) {
        return ImportFailure(
            "import_reject_limit_percent_invalid",
            "Import reject percentage must be between 0 and 100.",
            "reject_limit_percent is outside the accepted range.");
    }

    for (const auto& mapping : request.column_mappings) {
        if (mapping.target_column.empty()) {
            return ImportFailure(
                "import_target_column_required",
                "Every import column mapping must include a target column identifier.",
                "Parser/source field labels are not sufficient engine authority.");
        }
    }

    EnginePlanImportRowsResult result;
    result.ok = true;
    result.operation_id = "dml.plan_import_rows";
    result.surface_accepted = true;
    result.planning_only = true;
    result.execution_requires_execute_import_rows = true;
    result.row_execution_completed = false;
    result.normalized_source_kind = request.source.source_kind;
    result.normalized_format_family = request.format.format_family;
    result.normalized_insert_mode = InsertModeForSource(request.source.source_kind);
    result.mapped_column_count = static_cast<EngineApiU64>(request.column_mappings.size());
    result.evidence.push_back({"import_surface", "dml.plan_import_rows"});
    result.evidence.push_back({"import_plan_contract", "complete_planning_only"});
    result.evidence.push_back({"import_plan_planning_only", "true"});
    result.evidence.push_back({"import_execution_entrypoint", "dml.execute_import_rows"});
    result.evidence.push_back({"import_plan_requires_canonical_rows", "true"});
    result.evidence.push_back({"import_plan_row_persistence_claimed", "false"});
    result.evidence.push_back({"import_plan_row_execution_completed", "false"});
    result.evidence.push_back({"parser_boundary", "parser_decodes_bytes_engine_validates_uuid_rows"});
    result.evidence.push_back({"target_object_uuid", request.target_table.uuid.canonical});
    result.evidence.push_back({"import_source_kind", result.normalized_source_kind});
    result.evidence.push_back({"import_format_family", result.normalized_format_family});
    result.evidence.push_back({"insert_mode", result.normalized_insert_mode});
    if (request.import_policy.strict_bulk_load_requested) {
        result.evidence.push_back({"strict_bulk_load_requested", "true"});
    }
    return result;
}

} // namespace scratchbird::engine::internal_api
