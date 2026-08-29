// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "engine/internal_api/api_types.hpp"
#include "engine/sblr/contextual_text_literal_v2_codec.hpp"

#include <cstdint>
#include <string_view>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_CONTEXTUAL_TEXT_POLICY_REGISTRY_V2
//
// Engine-owned implementation of the five accepted manifest rows consumed by
// SBTLTD02.  The exact rows below are the retained evidence: Core defines no
// independent contextual-policy snapshot UUID, generation, or row-set hash,
// and this API deliberately does not invent one.

enum class EngineContextualTextPolicyKindV2 : std::uint8_t {
  normalization_policy = 1,
  render_policy = 2,
  canonicalization_profile = 3,
  comparison_contract = 4,
  equality_operation_binding = 5,
};

struct EngineContextualTextPolicyRowV2 {
  std::string_view row_id;
  scratchbird::engine::sblr::ContextualTextUuidV2 identity_uuid{};
  std::uint64_t generation = 0;
  EngineContextualTextPolicyKindV2 kind =
      EngineContextualTextPolicyKindV2::normalization_policy;
  std::string_view status;
  std::string_view exact_semantic_contract;

  bool operator==(const EngineContextualTextPolicyRowV2&) const = default;
};

struct EngineContextualTextPolicyRowSetV2 {
  EngineContextualTextPolicyRowV2 normalization;
  EngineContextualTextPolicyRowV2 render;
  EngineContextualTextPolicyRowV2 canonicalization;
  EngineContextualTextPolicyRowV2 comparison;
  EngineContextualTextPolicyRowV2 equality;

  // These two fields are part of the equality row's exact manifest binding,
  // not a contextual-policy snapshot abstraction.
  scratchbird::engine::sblr::ContextualTextUuidV2
      equality_operator_snapshot_uuid{};
  std::uint64_t equality_operator_registry_generation = 0;

  bool operator==(const EngineContextualTextPolicyRowSetV2&) const = default;
};

struct EngineContextualTextPolicyLookupResultV2 {
  bool ok = false;
  EngineContextualTextPolicyRowSetV2 rows;
  EngineApiDiagnostic diagnostic;
};

EngineContextualTextPolicyLookupResultV2
LookupEngineContextualTextPolicyRowSetV2(
    const EngineRequestContext& exact_live_context);

// Engine DDL/MGA publication has no statement receipt or guaranteed datatype
// tuple. This contextless manifest loader returns only the same immutable five
// rows after rechecking the live builtin-equality snapshot. It supplies no
// statement, receipt, catalog, resource, or sidecar authority.
EngineContextualTextPolicyLookupResultV2
LoadCurrentEngineContextualTextPolicyRowSetForPublicationV2();

// Re-resolves the engine registry and requires byte-identical UUID/generation,
// kind, status, semantic-contract, and equality snapshot evidence.
bool RevalidateEngineContextualTextPolicyRowSetV2(
    const EngineRequestContext& exact_live_context,
    const EngineContextualTextPolicyRowSetV2& retained,
    EngineApiDiagnostic* diagnostic);

const EngineContextualTextPolicyRowV2* FindEngineContextualTextPolicyRowV2(
    const EngineContextualTextPolicyRowSetV2& rows,
    EngineContextualTextPolicyKindV2 kind);

}  // namespace scratchbird::engine::internal_api
