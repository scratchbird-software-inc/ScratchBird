// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "meta/meta_command_surface.hpp"

#include <array>
#include <string>

namespace scratchbird::parser::sbsql {
namespace {

std::string NormalizeMetaCommand(std::string_view raw) {
  std::string normalized;
  normalized.reserve(raw.size());
  bool seen_nonspace = false;
  for (const char ch : raw) {
    const bool space = ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
    if (space) {
      if (seen_nonspace) break;
      continue;
    }
    seen_nonspace = true;
    normalized.push_back(ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch);
  }
  return normalized;
}

constexpr std::array<MetaCommandSurfaceRecord, 16> kMetaCommandRecords{{
    {"meta.psql.help", "019e17c0-0000-7000-8000-000000000001", "psql", "\\?",
     MetaCommandSurfaceClass::kToolLocal, MetaCommandDisposition::kToolLocalOnly,
     "tool.meta.psql.help", "", "tool_renders_help_without_server_execution",
     "tool_local_no_engine_authority", "render.meta.help",
     "SBSQL.META.TOOL_LOCAL_ONLY", "meta.psql.help.tool_local", false, false},
    {"meta.psql.sql_help", "019e17c0-0000-7000-8000-000000000002", "psql", "\\h",
     MetaCommandSurfaceClass::kToolLocal, MetaCommandDisposition::kToolLocalOnly,
     "tool.meta.psql.sql_help", "", "tool_renders_sql_help_without_server_execution",
     "tool_local_no_engine_authority", "render.meta.sql_help",
     "SBSQL.META.TOOL_LOCAL_ONLY", "meta.psql.sql_help.tool_local", false, false},
    {"meta.psql.describe", "019e17c0-0000-7000-8000-000000000003", "psql", "\\d",
     MetaCommandSurfaceClass::kMetadataReport, MetaCommandDisposition::kExactRefusal,
     "tool.meta.psql.describe", "sblr.management.show.v3",
     "metadata_report_requires_profile_specific_lowering", "engine_authority_required",
     "render.meta.metadata_table", "SBSQL.META.PROFILE_REQUIRED",
     "meta.psql.describe.refusal", false, false},
    {"meta.psql.describe_tables", "019e17c0-0000-7000-8000-000000000004", "psql", "\\dt",
     MetaCommandSurfaceClass::kMetadataReport, MetaCommandDisposition::kExactRefusal,
     "tool.meta.psql.describe_tables", "sblr.management.show.v3",
     "metadata_report_requires_profile_specific_lowering", "engine_authority_required",
     "render.meta.metadata_table", "SBSQL.META.PROFILE_REQUIRED",
     "meta.psql.describe_tables.refusal", false, false},
    {"meta.psql.describe_schemas", "019e17c0-0000-7000-8000-000000000005", "psql", "\\dn",
     MetaCommandSurfaceClass::kMetadataReport, MetaCommandDisposition::kExactRefusal,
     "tool.meta.psql.describe_schemas", "sblr.management.show.v3",
     "metadata_report_requires_profile_specific_lowering", "engine_authority_required",
     "render.meta.metadata_table", "SBSQL.META.PROFILE_REQUIRED",
     "meta.psql.describe_schemas.refusal", false, false},
    {"meta.psql.describe_views", "019e17c0-0000-7000-8000-000000000006", "psql", "\\dv",
     MetaCommandSurfaceClass::kMetadataReport, MetaCommandDisposition::kExactRefusal,
     "tool.meta.psql.describe_views", "sblr.management.show.v3",
     "metadata_report_requires_profile_specific_lowering", "engine_authority_required",
     "render.meta.metadata_table", "SBSQL.META.PROFILE_REQUIRED",
     "meta.psql.describe_views.refusal", false, false},
    {"meta.psql.list_databases", "019e17c0-0000-7000-8000-000000000007", "psql", "\\l",
     MetaCommandSurfaceClass::kMetadataReport, MetaCommandDisposition::kExactRefusal,
     "tool.meta.psql.list_databases", "sblr.management.show.v3",
     "metadata_report_requires_profile_specific_lowering", "engine_authority_required",
     "render.meta.metadata_table", "SBSQL.META.PROFILE_REQUIRED",
     "meta.psql.list_databases.refusal", false, false},
    {"meta.psql.connect", "019e17c0-0000-7000-8000-000000000008", "psql", "\\c",
     MetaCommandSurfaceClass::kConnectionSession, MetaCommandDisposition::kExactRefusal,
     "tool.meta.psql.connect", "sblr.session.connection.v3",
     "connection_switch_is_client_driver_route", "engine_authentication_required",
     "render.meta.connection_status", "SBSQL.META.CLIENT_ROUTE_REQUIRED",
     "meta.psql.connect.refusal", false, false},
    {"meta.psql.conninfo", "019e17c0-0000-7000-8000-000000000009", "psql", "\\conninfo",
     MetaCommandSurfaceClass::kConnectionSession, MetaCommandDisposition::kExactRefusal,
     "tool.meta.psql.conninfo", "sblr.session.connection.v3",
     "connection_report_requires_authenticated_driver_context", "engine_authentication_required",
     "render.meta.connection_status", "SBSQL.META.CLIENT_ROUTE_REQUIRED",
     "meta.psql.conninfo.refusal", false, false},
    {"meta.psql.gexec", "019e17c0-0000-7000-8000-00000000000a", "psql", "\\gexec",
     MetaCommandSurfaceClass::kQueryExecution, MetaCommandDisposition::kExactRefusal,
     "tool.meta.psql.gexec", "sblr.udr.operation.v3",
     "dynamic_sql_requires_trusted_parser_support_udr", "engine_sblr_udr_invoke_required",
     "render.meta.command_status", "SBSQL.META.DYNAMIC_SQL_ENGINE_ROUTE_REQUIRED",
     "meta.psql.gexec.refusal", false, false},
    {"meta.psql.copy", "019e17c0-0000-7000-8000-00000000000b", "psql", "\\copy",
     MetaCommandSurfaceClass::kBulkIo, MetaCommandDisposition::kExactRefusal,
     "tool.meta.psql.copy", "sblr.bulk.import_export.v3",
     "client_file_copy_must_use_declared_driver_streaming_route", "engine_authority_required",
     "render.meta.copy_status", "SBSQL.META.CLIENT_FILE_IO_REFUSED",
     "meta.psql.copy.refusal", false, true},
    {"meta.psql.include", "019e17c0-0000-7000-8000-00000000000c", "psql", "\\i",
     MetaCommandSurfaceClass::kUnsafeLocalShell, MetaCommandDisposition::kExactRefusal,
     "tool.meta.psql.include", "", "client_file_include_is_tool_local",
     "tool_local_no_engine_authority", "render.meta.refusal",
     "SBSQL.META.CLIENT_FILE_IO_REFUSED", "meta.psql.include.refusal", false, true},
    {"meta.psql.output", "019e17c0-0000-7000-8000-00000000000d", "psql", "\\o",
     MetaCommandSurfaceClass::kUnsafeLocalShell, MetaCommandDisposition::kExactRefusal,
     "tool.meta.psql.output", "", "client_output_redirect_is_tool_local",
     "tool_local_no_engine_authority", "render.meta.refusal",
     "SBSQL.META.CLIENT_FILE_IO_REFUSED", "meta.psql.output.refusal", false, true},
    {"meta.psql.watch", "019e17c0-0000-7000-8000-00000000000e", "psql", "\\watch",
     MetaCommandSurfaceClass::kAdminControl, MetaCommandDisposition::kExactRefusal,
     "tool.meta.psql.watch", "", "client_repetition_loop_is_tool_local",
     "tool_local_no_engine_authority", "render.meta.refusal",
     "SBSQL.META.TOOL_LOOP_REFUSED", "meta.psql.watch.refusal", false, false},
    {"meta.psql.shell", "019e17c0-0000-7000-8000-00000000000f", "psql", "\\!",
     MetaCommandSurfaceClass::kUnsafeLocalShell, MetaCommandDisposition::kExactRefusal,
     "tool.meta.psql.shell", "", "local_shell_is_forbidden_in_parser_route",
     "never_execute_local_process", "render.meta.refusal",
     "SBSQL.META.LOCAL_SHELL_REFUSED", "meta.psql.shell.refusal", true, false},
    {"meta.unknown", "019e17c0-0000-7000-8000-000000000010", "unknown", "\\",
     MetaCommandSurfaceClass::kUnknown, MetaCommandDisposition::kExactRefusal,
     "tool.meta.unknown", "sblr.diagnostic.refusal.v3",
     "unknown_meta_command_is_refused", "engine_authority_required",
     "render.meta.refusal", "SBSQL.META.UNKNOWN_COMMAND",
     "meta.unknown.refusal", false, false},
}};

} // namespace

std::span<const MetaCommandSurfaceRecord> BuiltinMetaCommandSurfaceRecords() {
  return {kMetaCommandRecords.data(), kMetaCommandRecords.size()};
}

const MetaCommandSurfaceRecord* ResolveMetaCommandSurface(std::string_view raw_meta_command) {
  const auto normalized = NormalizeMetaCommand(raw_meta_command);
  for (const auto& record : kMetaCommandRecords) {
    if (record.command == std::string_view(normalized) && record.surface_id != "meta.unknown") return &record;
  }
  return &UnknownMetaCommandSurface();
}

const MetaCommandSurfaceRecord& UnknownMetaCommandSurface() {
  return kMetaCommandRecords.back();
}

std::string_view MetaCommandSurfaceClassName(MetaCommandSurfaceClass surface_class) {
  switch (surface_class) {
    case MetaCommandSurfaceClass::kToolLocal: return "tool_local";
    case MetaCommandSurfaceClass::kConnectionSession: return "connection_session";
    case MetaCommandSurfaceClass::kMetadataReport: return "metadata_report";
    case MetaCommandSurfaceClass::kQueryExecution: return "query_execution";
    case MetaCommandSurfaceClass::kBulkIo: return "bulk_io";
    case MetaCommandSurfaceClass::kAdminControl: return "admin_control";
    case MetaCommandSurfaceClass::kUnsafeLocalShell: return "unsafe_local_shell";
    case MetaCommandSurfaceClass::kUnknown: return "unknown";
  }
  return "unknown";
}

std::string_view MetaCommandDispositionName(MetaCommandDisposition disposition) {
  switch (disposition) {
    case MetaCommandDisposition::kToolLocalOnly: return "tool_local_only";
    case MetaCommandDisposition::kLowerThroughEngine: return "lower_through_engine";
    case MetaCommandDisposition::kExactRefusal: return "exact_refusal";
  }
  return "exact_refusal";
}

Diagnostic MetaCommandRefusalDiagnostic(const MetaCommandSurfaceRecord& record,
                                        std::string_view raw_meta_command) {
  std::string message = "meta-command is not executable in the SBSQL parser route";
  if (record.disposition == MetaCommandDisposition::kToolLocalOnly) {
    message = "meta-command is tool-local and must not be sent to the engine parser route";
  } else if (record.surface_class == MetaCommandSurfaceClass::kUnsafeLocalShell) {
    message = "meta-command would require client local file or shell effects and is refused";
  } else if (record.refusal_diagnostic == "SBSQL.META.PROFILE_REQUIRED") {
    message = "meta-command requires a donor/tool parser profile and is refused by SBSQL";
  }
  return MakeDiagnostic(
      std::string(record.refusal_diagnostic),
      "ERROR",
      std::move(message),
      "sbp_sbsql.meta",
      {{"surface_id", std::string(record.surface_id)},
       {"tool_family", std::string(record.tool_family)},
       {"meta_command", std::string(raw_meta_command)},
       {"surface_class", std::string(MetaCommandSurfaceClassName(record.surface_class))},
       {"disposition", std::string(MetaCommandDispositionName(record.disposition))},
       {"parser_executes_local_process", record.parser_executes_local_process ? "true" : "false"},
       {"parser_reads_or_writes_client_files",
        record.parser_reads_or_writes_client_files ? "true" : "false"}});
}

} // namespace scratchbird::parser::sbsql
