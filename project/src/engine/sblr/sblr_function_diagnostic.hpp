// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

namespace scratchbird::engine::internal_api { struct EngineApiDiagnostic; }
namespace scratchbird::engine::sblr {
struct SblrRuntimeDiagnostic;
// Trusted in-process bridge, not a parser-facing MessageVector renderer.
internal_api::EngineApiDiagnostic FunctionDiagnosticToApi(const SblrRuntimeDiagnostic& diagnostic);
}
