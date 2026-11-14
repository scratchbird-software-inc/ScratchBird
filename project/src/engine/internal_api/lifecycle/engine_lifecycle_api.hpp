// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_LIFECYCLE_API
// Database creation is an engine-owned lifecycle mutation. The caller supplies
// authenticated management context and policy options; the engine assigns the
// database/filespace identities and owns storage/catalog bootstrap finality.

struct EngineOpenLifecycleRequest : EngineApiRequest {};
struct EngineOpenLifecycleResult : EngineApiResult {};
EngineOpenLifecycleResult EngineOpenLifecycle(const EngineOpenLifecycleRequest& request);

// SEARCH_KEY: SB_ENGINE_CREATE_DATABASE_FORMAT (owner Q92/Q93/Q95 = 1,
// 2026-11-13; plan 31 step S2). The database format is an explicit choice on
// the request. legacy (the default) is the existing create, unchanged until
// workstream C; native writes the single-file database (one file, no side
// files, no lock files) through CreateNativeDatabaseOnOpenDevice. The native
// format takes the database name as DATA and its language as the
// catalog_language constant (16 binary bytes, never a text tag); the engine
// mints every identity and returns them. The SBLR create descriptor v2
// (SB_LIFECYCLE_CREATE_DATABASE_DESCRIPTOR_V2) fills these fields.
enum class EngineDatabaseFormat : std::uint8_t { legacy = 0, native = 1 };
struct EngineCreateLifecycleRequest : EngineApiRequest {
  EngineDatabaseFormat database_format = EngineDatabaseFormat::legacy;
  std::string database_name;      // native: data, written as the name row
  EngineUuid database_language;   // native: a catalog_language constant
};
struct EngineCreateLifecycleResult : EngineApiResult {};
EngineCreateLifecycleResult EngineCreateLifecycle(const EngineCreateLifecycleRequest& request);

struct EngineAttachLifecycleRequest : EngineApiRequest {};
struct EngineAttachLifecycleResult : EngineApiResult {};
EngineAttachLifecycleResult EngineAttachLifecycle(const EngineAttachLifecycleRequest& request);

struct EngineDetachLifecycleRequest : EngineApiRequest {};
struct EngineDetachLifecycleResult : EngineApiResult {};
EngineDetachLifecycleResult EngineDetachLifecycle(const EngineDetachLifecycleRequest& request);

struct EngineShutdownLifecycleRequest : EngineApiRequest {};
struct EngineShutdownLifecycleResult : EngineApiResult {};
EngineShutdownLifecycleResult EngineShutdownLifecycle(const EngineShutdownLifecycleRequest& request);

struct EngineEnterMaintenanceLifecycleRequest : EngineApiRequest {};
struct EngineEnterMaintenanceLifecycleResult : EngineApiResult {};
EngineEnterMaintenanceLifecycleResult EngineEnterMaintenanceLifecycle(const EngineEnterMaintenanceLifecycleRequest& request);

struct EngineExitMaintenanceLifecycleRequest : EngineApiRequest {};
struct EngineExitMaintenanceLifecycleResult : EngineApiResult {};
EngineExitMaintenanceLifecycleResult EngineExitMaintenanceLifecycle(const EngineExitMaintenanceLifecycleRequest& request);

struct EngineEnterRestrictedOpenLifecycleRequest : EngineApiRequest {};
struct EngineEnterRestrictedOpenLifecycleResult : EngineApiResult {};
EngineEnterRestrictedOpenLifecycleResult EngineEnterRestrictedOpenLifecycle(const EngineEnterRestrictedOpenLifecycleRequest& request);

struct EngineExitRestrictedOpenLifecycleRequest : EngineApiRequest {};
struct EngineExitRestrictedOpenLifecycleResult : EngineApiResult {};
EngineExitRestrictedOpenLifecycleResult EngineExitRestrictedOpenLifecycle(const EngineExitRestrictedOpenLifecycleRequest& request);

struct EngineInspectLifecycleRequest : EngineApiRequest {};
struct EngineInspectLifecycleResult : EngineApiResult {};
EngineInspectLifecycleResult EngineInspectLifecycle(const EngineInspectLifecycleRequest& request);

struct EngineVerifyLifecycleRequest : EngineApiRequest {};
struct EngineVerifyLifecycleResult : EngineApiResult {};
EngineVerifyLifecycleResult EngineVerifyLifecycle(const EngineVerifyLifecycleRequest& request);

struct EngineRepairLifecycleRequest : EngineApiRequest {};
struct EngineRepairLifecycleResult : EngineApiResult {};
EngineRepairLifecycleResult EngineRepairLifecycle(const EngineRepairLifecycleRequest& request);

struct EngineForceShutdownLifecycleRequest : EngineApiRequest {};
struct EngineForceShutdownLifecycleResult : EngineApiResult {};
EngineForceShutdownLifecycleResult EngineForceShutdownLifecycle(const EngineForceShutdownLifecycleRequest& request);

struct EngineAcknowledgeShutdownLifecycleRequest : EngineApiRequest {};
struct EngineAcknowledgeShutdownLifecycleResult : EngineApiResult {};
EngineAcknowledgeShutdownLifecycleResult EngineAcknowledgeShutdownLifecycle(
    const EngineAcknowledgeShutdownLifecycleRequest& request);

struct EngineDropLifecycleRequest : EngineApiRequest {};
struct EngineDropLifecycleResult : EngineApiResult {};
EngineDropLifecycleResult EngineDropLifecycle(const EngineDropLifecycleRequest& request);

}  // namespace scratchbird::engine::internal_api
