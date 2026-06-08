// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_lineage.hpp"

namespace scratchbird::core::index {
namespace {
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
Status OkStatus() { return Status{StatusCode::ok, Severity::info, Subsystem::engine}; }
Status RefuseStatus() { return Status{StatusCode::platform_required_feature_missing, Severity::error, Subsystem::engine}; }
}

const char* IndexLineageBehaviorName(IndexLineageBehavior behavior) {
  switch (behavior) {
    case IndexLineageBehavior::persist: return "persist";
    case IndexLineageBehavior::rebuild: return "rebuild";
    case IndexLineageBehavior::skip: return "skip";
    case IndexLineageBehavior::transform: return "transform";
    case IndexLineageBehavior::verify: return "verify";
    case IndexLineageBehavior::refuse: return "refuse";
  }
  return "unknown";
}

IndexLineageDecision DecideIndexLineage(IndexFamily family, bool resource_available, bool finality_proven) {
  if (!finality_proven) {
    return IndexLineageDecision{RefuseStatus(), IndexLineageBehavior::refuse, IndexLineageBehavior::refuse,
                                IndexLineageBehavior::refuse, "transaction_finality_required",
                                MakeIndexFamilyDiagnostic(RefuseStatus(), "INDEX.LINEAGE.FINALITY_REQUIRED", "index.lineage.finality_required")};
  }
  if (!resource_available && (family == IndexFamily::full_text || family == IndexFamily::vector_ivf ||
                              family == IndexFamily::spatial || family == IndexFamily::document_path)) {
    return IndexLineageDecision{RefuseStatus(), IndexLineageBehavior::refuse, IndexLineageBehavior::refuse,
                                IndexLineageBehavior::refuse, "resource_epoch_required",
                                MakeIndexFamilyDiagnostic(RefuseStatus(), "INDEX.LINEAGE.RESOURCE_REQUIRED", "index.lineage.resource_required")};
  }
  if (family == IndexFamily::columnar_zone || family == IndexFamily::brin_zone || family == IndexFamily::bloom) {
    return IndexLineageDecision{OkStatus(), IndexLineageBehavior::rebuild, IndexLineageBehavior::rebuild,
                                IndexLineageBehavior::verify, "summary_recheck_verify", {}};
  }
  return IndexLineageDecision{OkStatus(), IndexLineageBehavior::persist, IndexLineageBehavior::rebuild,
                              IndexLineageBehavior::verify, "row_count_and_key_visibility_verify", {}};
}

}  // namespace scratchbird::core::index
