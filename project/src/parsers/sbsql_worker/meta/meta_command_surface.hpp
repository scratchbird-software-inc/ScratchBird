// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "common/common.hpp"

#include <span>
#include <string_view>

namespace scratchbird::parser::sbsql {

enum class MetaCommandSurfaceClass {
  kToolLocal,
  kConnectionSession,
  kMetadataReport,
  kQueryExecution,
  kBulkIo,
  kAdminControl,
  kUnsafeLocalShell,
  kUnknown,
};

enum class MetaCommandDisposition {
  kToolLocalOnly,
  kLowerThroughEngine,
  kExactRefusal,
};

struct MetaCommandSurfaceRecord {
  std::string_view surface_id;
  std::string_view fixed_uuid_v7;
  std::string_view tool_family;
  std::string_view command;
  MetaCommandSurfaceClass surface_class{MetaCommandSurfaceClass::kUnknown};
  MetaCommandDisposition disposition{MetaCommandDisposition::kExactRefusal};
  std::string_view registry_ref;
  std::string_view sblr_operation_family;
  std::string_view tool_local_behavior;
  std::string_view security_policy;
  std::string_view render_contract;
  std::string_view refusal_diagnostic;
  std::string_view conformance_case;
  bool parser_executes_local_process{false};
  bool parser_reads_or_writes_client_files{false};
};

std::span<const MetaCommandSurfaceRecord> BuiltinMetaCommandSurfaceRecords();
const MetaCommandSurfaceRecord* ResolveMetaCommandSurface(std::string_view raw_meta_command);
const MetaCommandSurfaceRecord& UnknownMetaCommandSurface();
std::string_view MetaCommandSurfaceClassName(MetaCommandSurfaceClass surface_class);
std::string_view MetaCommandDispositionName(MetaCommandDisposition disposition);
Diagnostic MetaCommandRefusalDiagnostic(const MetaCommandSurfaceRecord& record,
                                        std::string_view raw_meta_command);

} // namespace scratchbird::parser::sbsql
