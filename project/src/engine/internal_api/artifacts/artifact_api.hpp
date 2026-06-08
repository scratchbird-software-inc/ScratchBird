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

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_ARTIFACT_API
struct EngineExportCatalogArtifactsRequest : EngineApiRequest {};
struct EngineExportCatalogArtifactsResult : EngineApiResult {};
EngineExportCatalogArtifactsResult EngineExportCatalogArtifacts(const EngineExportCatalogArtifactsRequest& request);

struct EngineImportCatalogArtifactsRequest : EngineApiRequest {};
struct EngineImportCatalogArtifactsResult : EngineApiResult {};
EngineImportCatalogArtifactsResult EngineImportCatalogArtifacts(const EngineImportCatalogArtifactsRequest& request);

}  // namespace scratchbird::engine::internal_api
