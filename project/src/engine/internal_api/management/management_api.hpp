// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_MANAGEMENT_MANAGEMENT_API
struct EngineInspectManagementRuntimeRequest : EngineApiRequest {};
struct EngineInspectManagementRuntimeResult : EngineApiResult {};
EngineInspectManagementRuntimeResult EngineInspectManagementRuntime(const EngineInspectManagementRuntimeRequest& request);

struct EngineControlManagementRuntimeRequest : EngineApiRequest {};
struct EngineControlManagementRuntimeResult : EngineApiResult {};
EngineControlManagementRuntimeResult EngineControlManagementRuntime(const EngineControlManagementRuntimeRequest& request);

struct EngineBeginMigrationRequest : EngineApiRequest {};
struct EngineBeginMigrationResult : EngineApiResult {};
EngineBeginMigrationResult EngineBeginMigration(const EngineBeginMigrationRequest& request);

struct EngineAlterMigrationRequest : EngineApiRequest {};
struct EngineAlterMigrationResult : EngineApiResult {};
EngineAlterMigrationResult EngineAlterMigration(const EngineAlterMigrationRequest& request);

struct EngineShowMigrationRequest : EngineApiRequest {};
struct EngineShowMigrationResult : EngineApiResult {};
EngineShowMigrationResult EngineShowMigration(const EngineShowMigrationRequest& request);

struct EngineShowMigrationsRequest : EngineApiRequest {};
struct EngineShowMigrationsResult : EngineApiResult {};
EngineShowMigrationsResult EngineShowMigrations(const EngineShowMigrationsRequest& request);

}  // namespace scratchbird::engine::internal_api
