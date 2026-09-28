// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "server/diagnostic_rendering/diagnostic_rendering.hpp"
#include "engine/internal_api/api_types.hpp"

namespace scratchbird::server_engine_bridge {
// This private adapter copies only the legacy projection's supported fields.
// It does not admit a canonical result or authorize parser publication.
server::legacy_rendering::EngineRenderedResultEnvelope RenderLegacyEngineResult(
    const engine::internal_api::EngineApiResult& source,
    server::legacy_rendering::EngineParserPackageRenderOptions options);
} // namespace scratchbird::server_engine_bridge
