// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "dml/dml_target_access_plan.hpp"
#include "uuid.hpp"
#include <optional>
#include <stdexcept>

namespace scratchbird::engine::internal_api {
// Exact companion fields for the existing UTF-8 plan observations.
// Native identifiers remain typed evidence; JSON presence flags are not IDs.
inline void AppendDmlTargetPlanIdentityEvidence(
    const DmlTargetAccessPlan& plan, std::string_view prefix,
    std::vector<EngineEvidenceReference>* output,
    std::optional<std::uint64_t> source_ordinal = std::nullopt) {
  if (!output || (prefix != "dml_target_access_plan" && prefix != "merge_target_access_plan") ||
      source_ordinal.has_value() != (prefix == "merge_target_access_plan"))
    throw std::invalid_argument("dml_target_plan_evidence_prefix_invalid");
  const std::string scope = std::string(prefix) +
      (source_ordinal ? "." + std::to_string(*source_ordinal) : "");
  std::vector<EngineEvidenceReference> staged;
  const auto append = [&](std::string_view field, const EngineUuid& identity, bool required) {
    if (identity.is_nil() && !required) return;
    if (!scratchbird::core::uuid::IsEngineIdentityUuid(identity))
      throw std::invalid_argument("dml_target_plan_evidence_identity_invalid");
    staged.push_back({scope + "." + std::string(field), identity});
  };
  append("database_uuid", plan.database_uuid, false);
  append("relation_uuid", plan.relation_uuid, false);
  append("row_uuid", plan.row_uuid, false);
  append("index_uuid", plan.index_uuid, false);
  for (std::size_t n = 0; n < plan.row_uuids.size(); ++n)
    append("row_uuids." + std::to_string(n), plan.row_uuids[n], true);
  output->insert(output->end(), staged.begin(), staged.end());
}
}  // namespace scratchbird::engine::internal_api
