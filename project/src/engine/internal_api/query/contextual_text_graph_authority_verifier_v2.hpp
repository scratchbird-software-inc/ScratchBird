// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "engine/internal_api/query/contextual_text_literal_authority.hpp"
#include "engine/sblr/sblr_literal_runtime.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_CONTEXTUAL_TEXT_GRAPH_AUTHORITY_VERIFIER_V2
//
// Core defines the pre-contextual top-level SBOP operand vector and the SBXN
// bytes, but no second relational-DAG byte carrier.  This selector is the
// engine-only bridge from the already canonical query decoder to a structured
// immutable snapshot.  The verifier below consumes that snapshot and performs
// the contextual profile/graph/descriptor/equality closure.  No parser claim
// can populate this structure directly.

struct EngineContextualTextGraphSourceV2 {
  std::uint32_t node_id = 0;
  std::uint32_t top_level_operand_ordinal = 0;
  std::uint32_t source_ordinal = 0;
  bool node_kind_is_scan = false;
  bool semantic_variant_is_catalog_or_model_source = false;
  sblr::ContextualTextUuidV2 required_relation_uuid{};
};

struct EngineContextualTextGraphDescriptorV2 {
  std::uint32_t descriptor_handle = 0;
  std::array<std::string, 17> exact_relational_descriptor_v2_fields{};
  std::string canonical_type_name;
  bool element_profile_empty = false;
};

struct EngineContextualTextGraphOccurrenceV2 {
  std::uint64_t literal_occurrence = 0;
  std::uint64_t node_id = 0;
  sblr::ContextualTextUuidV2 literal_binding_uuid{};
  std::uint64_t literal_binding_generation = 0;

  EngineContextualTextGraphSourceV2 source;

  std::uint32_t comparison_expression_id = 0;
  bool comparison_kind_is_binary = false;
  std::array<std::uint32_t, 2> comparison_child_expression_ids{};
  std::uint32_t comparison_child_count = 0;
  std::string canonical_operator_name;
  bool function_uuid_present = false;
  bool bound_name_uuid_present = false;
  bool collation_override_present = false;
  sblr::ContextualTextUuidV2 resolved_equality_operation_uuid{};
  std::uint64_t resolved_equality_operation_generation = 0;

  std::uint32_t target_expression_id = 0;
  bool target_is_simple_bound_column = false;
  std::uint32_t target_source_node_id = 0;
  sblr::ContextualTextUuidV2 target_relation_uuid{};
  sblr::ContextualTextUuidV2 target_column_uuid{};
  std::uint32_t target_column_ordinal = 0;
  std::uint32_t target_descriptor_handle = 0;
  EngineContextualTextGraphDescriptorV2 target_descriptor;

  std::uint32_t literal_expression_id = 0;
  sblr::SblrExpressionNodeReferenceV1 literal_reference;
  sblr::SblrExpressionLiteralNodeV1 literal_node;
  std::uint32_t literal_incoming_use_count = 0;
  std::uint32_t literal_sole_parent_expression_id = 0;
  std::uint32_t literal_descriptor_handle = 0;
  EngineContextualTextGraphDescriptorV2 literal_descriptor;
};

struct EngineContextualTextCanonicalGraphSnapshotV2 {
  std::vector<std::uint8_t> exact_pre_contextual_operand_records;
  std::uint32_t pre_contextual_operand_count = 0;
  std::vector<std::uint8_t> exact_sbxn;
  bool one_global_sbxn_table = false;
  bool every_sbxn_node_classified_once = false;
  std::uint32_t total_sbxn_node_count = 0;
  std::uint32_t numeric_v1_sbxn_node_count = 0;
  std::vector<EngineContextualTextGraphSourceV2> sources;
  std::vector<EngineContextualTextGraphOccurrenceV2>
      contextual_occurrences;
};

class EngineContextualTextCanonicalGraphSelectorV2 {
 public:
  virtual ~EngineContextualTextCanonicalGraphSelectorV2() = default;

  virtual bool SelectCanonicalGraph(
      const EngineRequestContext& exact_live_context,
      const std::vector<std::uint8_t>& exact_sbel_v1,
      const std::vector<std::uint8_t>& exact_canonical_sbos,
      const EngineContextualTextComposedTransferRecordV2& composed_transfer,
      const sblr::ContextualTextLiteralExecuteV2& execute,
      const std::vector<std::uint8_t>& exact_pre_contextual_operand_records,
      std::uint32_t pre_contextual_operand_count,
      const std::vector<std::uint8_t>& exact_sbxn,
      EngineContextualTextCanonicalGraphSnapshotV2* out,
      EngineApiDiagnostic* diagnostic) const = 0;
};

class EngineContextualTextCanonicalGraphSelectorProviderV2 {
 public:
  virtual ~EngineContextualTextCanonicalGraphSelectorProviderV2() = default;
  virtual std::shared_ptr<const EngineContextualTextCanonicalGraphSelectorV2>
  SelectForReceipt(const EngineRequestContext& exact_live_context) const = 0;
};

bool InstallEngineContextualTextCanonicalGraphSelectorProviderV2(
    std::shared_ptr<const EngineContextualTextCanonicalGraphSelectorProviderV2>
        provider,
    EngineApiDiagnostic* diagnostic);

std::unique_ptr<EngineContextualTextGraphAuthorityVerifierV2>
CreateEngineContextualTextGraphAuthorityVerifierForReceiptV2(
    const EngineRequestContext& exact_live_context);

}  // namespace scratchbird::engine::internal_api
