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

// Engine-owned procedural/general operation surface. The parser supplies
// SBLR/procedural IR descriptors only; this API never executes source SQL text.
struct EngineGeneralProceduralOperationRequest : EngineApiRequest {};
struct EngineGeneralProceduralOperationResult : EngineApiResult {};
EngineGeneralProceduralOperationResult EngineGeneralProceduralOperation(
    const EngineGeneralProceduralOperationRequest& request);

struct EngineSignalDiagnosticRequest : EngineApiRequest {};
struct EngineSignalDiagnosticResult : EngineApiResult {};
EngineSignalDiagnosticResult EngineSignalDiagnostic(
    const EngineSignalDiagnosticRequest& request);

struct EngineRaiseDiagnosticRequest : EngineApiRequest {};
struct EngineRaiseDiagnosticResult : EngineApiResult {};
EngineRaiseDiagnosticResult EngineRaiseDiagnostic(
    const EngineRaiseDiagnosticRequest& request);

struct EngineResignalDiagnosticRequest : EngineApiRequest {};
struct EngineResignalDiagnosticResult : EngineApiResult {};
EngineResignalDiagnosticResult EngineResignalDiagnostic(
    const EngineResignalDiagnosticRequest& request);

}  // namespace scratchbird::engine::internal_api
