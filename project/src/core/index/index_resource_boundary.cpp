// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_resource_boundary.hpp"

namespace scratchbird::core::index {
namespace {
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
Status OkStatus() { return Status{StatusCode::ok, Severity::info, Subsystem::engine}; }
Status RefuseStatus() { return Status{StatusCode::platform_required_feature_missing, Severity::error, Subsystem::engine}; }
}

const char* IndexResourceKindName(IndexResourceKind kind) {
  switch (kind) {
    case IndexResourceKind::collation: return "collation";
    case IndexResourceKind::charset: return "charset";
    case IndexResourceKind::tokenizer: return "tokenizer";
    case IndexResourceKind::analyzer: return "analyzer";
    case IndexResourceKind::spatial: return "spatial";
    case IndexResourceKind::vector_metric: return "vector_metric";
    case IndexResourceKind::quantizer: return "quantizer";
    case IndexResourceKind::trusted_udr: return "trusted_udr";
    case IndexResourceKind::llvm_profile: return "llvm_profile";
  }
  return "unknown";
}

const char* IndexResourceActionName(IndexResourceAction action) {
  switch (action) {
    case IndexResourceAction::usable: return "usable";
    case IndexResourceAction::mark_stale: return "mark_stale";
    case IndexResourceAction::rebuild_required: return "rebuild_required";
    case IndexResourceAction::refuse: return "refuse";
  }
  return "unknown";
}

IndexResourceDecision EvaluateIndexResource(IndexResourceKind, bool registered, bool epoch_current, bool deterministic) {
  if (!registered || !deterministic) {
    return IndexResourceDecision{RefuseStatus(), IndexResourceAction::refuse,
                                 MakeIndexFamilyDiagnostic(RefuseStatus(), "INDEX.RESOURCE.REFUSED", "index.resource.refused")};
  }
  if (!epoch_current) {
    return IndexResourceDecision{RefuseStatus(), IndexResourceAction::rebuild_required,
                                 MakeIndexFamilyDiagnostic(RefuseStatus(), "INDEX.RESOURCE.EPOCH_STALE", "index.resource.epoch_stale")};
  }
  return IndexResourceDecision{OkStatus(), IndexResourceAction::usable, {}};
}

}  // namespace scratchbird::core::index
