// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "engine_internal_api.hpp"
#include "resource_seed_pack.hpp"

#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::u32;

enum class EngineResultValueKind : u32 {
  null_value,
  string_value,
  uint32_value,
  boolean_value,
  uuid_value,
  unknown
};

struct EngineResultValue {
  EngineResultValueKind kind = EngineResultValueKind::unknown;
  std::string string_value;
  u32 uint32_value = 0;
  bool boolean_value = false;
  TypedUuid uuid_value;
};

struct EngineResultCell {
  std::string stable_column_name;
  CanonicalTypeId type_id = CanonicalTypeId::unknown;
  EngineResultValue value;
};

struct EngineResultRow {
  std::vector<EngineResultCell> cells;
};

struct EngineOperationResult {
  Status status;
  EngineOperationCode operation_code = EngineOperationCode::unknown;
  EngineResultShape result_shape;
  std::vector<EngineResultRow> rows;
  std::vector<DiagnosticRecord> diagnostics;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok() && diagnostics.empty();
  }
};

struct EngineVersionInfo {
  std::string product_name = "ScratchBird";
  std::string component_name = "sb_engine";
  u32 version_major = 0;
  u32 version_minor = 1;
  u32 version_patch = 0;
  u32 internal_api_major = kEngineInternalApiMajor;
  u32 internal_api_minor = kEngineInternalApiMinor;
  std::string build_label = "private-runtime-foundation";
};

struct EngineDatabaseInfo {
  TypedUuid database_uuid;
  std::string database_label;
  bool cluster_authority_active = false;
};

const char* EngineResultValueKindName(EngineResultValueKind kind);
BoundEngineOperation MakeShowVersionOperationDescriptor();
BoundEngineOperation MakeShowDatabaseOperationDescriptor();
BoundEngineOperation MakeShowDatabaseResourcesOperationDescriptor();
EngineOperationResult ExecuteShowVersionOperation(const EngineContext& context, const EngineVersionInfo& version);
EngineOperationResult ExecuteShowDatabaseOperation(const EngineContext& context, const EngineDatabaseInfo& database);
EngineOperationResult ExecuteShowDatabaseResourcesOperation(
    const EngineContext& context,
    const scratchbird::core::resources::ResourceSeedCatalogImage& resources);
DiagnosticRecord MakeEngineBuiltinOperationDiagnostic(Status status,
                                                      std::string diagnostic_code,
                                                      std::string message_key,
                                                      std::string detail = {});

}  // namespace scratchbird::engine::internal_api
