// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>

namespace scratchbird::core::catalog {

inline constexpr std::uint64_t kBootstrapCatalogTransactionId = 1;

// SEARCH_KEY: SB_BOOTSTRAP_SCHEMA_ROOT_PATHS
// Database-create assigns fresh UUIDv7 object IDs for these paths. Do not add
// hard-coded bootstrap object UUIDs here; name/UUID association must come from
// the catalog rows returned or read after creation.
inline constexpr const char* kLocalSysSchemaPath = "sys";
inline constexpr const char* kLocalSysCatalogSchemaPath = "sys.catalog";
inline constexpr const char* kLocalSysMetricsSchemaPath = "sys.metrics";
inline constexpr const char* kLocalSysAgentsSchemaPath = "sys.agents";
inline constexpr const char* kLocalSysSecuritySchemaPath = "sys.security";
inline constexpr const char* kLocalSysConfigurationSchemaPath = "sys.configuration";
inline constexpr const char* kLocalSysManagementSchemaPath = "sys.management";
inline constexpr const char* kLocalSysFnSchemaPath = "sys.fn";
inline constexpr const char* kLocalSysUdrSchemaPath = "sys.udr";
inline constexpr const char* kLocalSysParserSchemaPath = "sys.parser";
inline constexpr const char* kLocalSysStorageSchemaPath = "sys.storage";
inline constexpr const char* kLocalSysMgaSchemaPath = "sys.mga";
inline constexpr const char* kLocalSysAuditSchemaPath = "sys.audit";
inline constexpr const char* kLocalSysCompatibilitySchemaPath = "sys.compatibility";
inline constexpr const char* kLocalSysInformationTrueSchemaPath = "sys.information";
inline constexpr const char* kLocalSysInformationSchemaPath = "sys.information_schema";
inline constexpr const char* kLocalSysCatalogReadableSchemaPath = "sys.catalog_readable";
inline constexpr const char* kLocalSysDiagnosticsSchemaPath = "sys.diagnostics";
inline constexpr const char* kLocalUsersSchemaPath = "users";
inline constexpr const char* kLocalUsersPublicSchemaPath = "users.public";
inline constexpr const char* kLocalRemoteSchemaPath = "remote";
inline constexpr const char* kLocalEmulatedSchemaPath = "emulated";
inline constexpr const char* kLocalUserHomePolicyName = "default_local_user_home_schema_policy";
inline constexpr const char* kLocalUserHomePolicyRoot = "users";
inline constexpr const char* kLocalUserHomePublicPath = kLocalUsersPublicSchemaPath;
inline constexpr const char* kClusterUserHomePolicyRoot = "cluster.user";

}  // namespace scratchbird::core::catalog
