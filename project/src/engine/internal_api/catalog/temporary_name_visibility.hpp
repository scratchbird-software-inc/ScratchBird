// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "api_types.hpp"
namespace scratchbird::engine::internal_api {
struct MgaTemporaryTableVisibilityResult;
enum class TemporaryNameCandidateKind { kNotVisible, kCatalog, kOwnedPrivate };
struct TemporaryNameCandidateClassification {
  bool ok=false;
  EngineApiDiagnostic diagnostic;
  TemporaryNameCandidateKind kind=TemporaryNameCandidateKind::kNotVisible;
};
// Classifies an engine visibility observation, not a parser-supplied claim.
// The caller remains responsible for acquiring the exact MGA metadata view.
TemporaryNameCandidateClassification ClassifyTemporaryNameCandidate(
    const EngineRequestContext&, const EngineUuid& object_uuid,
    const MgaTemporaryTableVisibilityResult&);
}
