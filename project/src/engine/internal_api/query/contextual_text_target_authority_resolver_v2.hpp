// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "engine/internal_api/mga_relation_store/mga_contextual_text_sidecar_set_v2.hpp"
#include "engine/internal_api/query/contextual_text_literal_authority.hpp"
#include "engine/internal_api/query/contextual_text_policy_registry_v2.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_CONTEXTUAL_TEXT_TARGET_AUTHORITY_RESOLVER_V2
//
// This is the narrow engine-owned selection seam needed by the contextual
// TEXT resolver.  An implementation may source it from an MGA catalog/store,
// but a parser, an SBPS frame, or an SBLR operand can never implement or
// populate it.  The adapter below independently validates the selected
// projection and the complete sealed sidecar set before issuing authority.

struct EngineContextualTextCoordinationPolicyV2 {
  sblr::ContextualTextUuidV2 literal_budget_policy_uuid{};
  std::uint64_t literal_budget_policy_generation = 0;
  sblr::ContextualTextUuidV2 descriptor_format_uuid{};
  std::uint64_t descriptor_format_generation = 0;
  sblr::ContextualTextUuidV2 profile_set_namespace_uuid{};
  std::uint64_t profile_set_namespace_generation = 0;
  sblr::ContextualTextUuidV2 source_occurrence_namespace_uuid{};
  std::uint64_t source_occurrence_namespace_generation = 0;
  sblr::ContextualTextUuidV2 budget_grant_namespace_uuid{};
  std::uint64_t budget_grant_namespace_generation = 0;
  sblr::ContextualTextUuidV2 raw_token_codec_uuid{};
  std::uint64_t raw_token_codec_generation = 0;

  // These fields are resolved from the non-SBTLTD coordination rows. They are
  // separate from EngineContextualTextPolicyRowSetV2's five exact descriptor
  // rows and never constitute a policy snapshot identity.
  bool literal_budget_is_private_receipt_policy = false;
  bool descriptor_format_is_sbtltd02_v2 = false;
  bool profile_set_namespace_is_uuidv7_v2 = false;
  bool source_occurrence_namespace_is_uuidv7_v2 = false;
  bool budget_grant_namespace_is_uuidv7_v2 = false;
  bool raw_token_codec_is_sbsql_single_quoted_v2 = false;

  std::uint64_t minimum_literal_negotiation_byte_grant = 0;
  std::uint64_t maximum_literal_negotiation_byte_grant = 0;
  std::uint64_t default_literal_negotiation_byte_grant = 0;
  std::uint64_t minimum_canonical_body_aggregate_grant = 0;
  std::uint64_t maximum_canonical_body_aggregate_grant = 0;
  std::uint64_t default_canonical_body_aggregate_grant = 0;
};

bool ValidateEngineContextualTextCoordinationPolicyV2(
    const EngineContextualTextCoordinationPolicyV2& policy,
    EngineApiDiagnostic* diagnostic);

// Exact public_relation_projection_v3 codec shared by the engine resolver and
// MGA publication/selection. It preserves the frozen field order and exact
// extent. canonical_value_width is the exact datatype-registry value; d718
// generation 1 therefore carries the authoritative zero variable-width
// marker.
struct EnginePublicRelationProjectionColumnV3 {
  sblr::ContextualTextUuidV2 column_uuid{};
  std::uint32_t ordinal = 0;
  std::string canonical_name;
  sblr::ContextualTextUuidV2 descriptor_uuid{};
  std::string descriptor_kind;
  std::string canonical_type_name;
  std::string encoded_type_descriptor;
  std::uint8_t attributes = 0;
  sblr::ContextualTextUuidV2 charset_uuid{};
  std::string charset_name;
  sblr::ContextualTextUuidV2 collation_uuid{};
  std::string collation_name;
  std::uint32_t character_length = 0;
  std::uint32_t charset_min_bytes = 0;
  std::uint32_t charset_max_bytes = 0;
  bool identity_present = false;
  std::uint64_t descriptor_generation = 0;
  sblr::ContextualTextUuidV2 type_uuid{};
  std::uint64_t type_generation = 0;
  std::string codec_id;
  std::uint16_t codec_version = 0;
  std::uint64_t codec_generation = 0;
  std::uint32_t canonical_value_width = 0;
  std::uint8_t null_encoding = 0;
};

struct EnginePublicRelationProjectionV3 {
  sblr::ContextualTextUuidV2 relation_descriptor_uuid{};
  sblr::ContextualTextUuidV2 relation_uuid{};
  sblr::ContextualTextUuidV2 schema_uuid{};
  std::uint64_t relation_descriptor_generation = 0;
  std::uint64_t resource_epoch = 0;
  sblr::ContextualTextUuidV2 catalog_snapshot_uuid{};
  std::uint64_t catalog_generation = 0;
  std::uint64_t registry_generation = 0;
  std::vector<EnginePublicRelationProjectionColumnV3> columns;
};

bool DecodeEnginePublicRelationProjectionV3(
    const std::vector<std::uint8_t>& exact,
    EnginePublicRelationProjectionV3* out,
    EngineApiDiagnostic* diagnostic);
bool EncodeEnginePublicRelationProjectionV3(
    const EnginePublicRelationProjectionV3& projection,
    std::vector<std::uint8_t>* exact,
    EngineApiDiagnostic* diagnostic);

struct EngineContextualTextTargetSelectionV2 {
  std::vector<std::uint8_t> exact_public_relation_projection_v3;
  MgaContextualTextSidecarSetOwnerV2 sidecar_owner;
  std::vector<MgaContextualTextDescriptorFieldPairV2>
      base_descriptor_fields;
  std::vector<MgaContextualTextProjectedColumnV2> projected_columns;
  MgaContextualTextSidecarSetV2 sealed_sidecar_set;
};

class EngineContextualTextTargetAuthoritySelectorV2 {
 public:
  virtual ~EngineContextualTextTargetAuthoritySelectorV2() = default;

  virtual bool SelectCoordinationPolicy(
      const EngineRequestContext& exact_live_context,
      EngineContextualTextCoordinationPolicyV2* out,
      EngineApiDiagnostic* diagnostic) const = 0;

  virtual bool SelectTarget(
      const EngineRequestContext& exact_live_context,
      const sblr::ContextualTextLiteralDemandV2& structural_claim,
      const EngineContextualTextPolicyRowSetV2& exact_policy_rows,
      EngineContextualTextTargetSelectionV2* out,
      EngineApiDiagnostic* diagnostic) const = 0;
};

class EngineContextualTextTargetAuthoritySelectorProviderV2 {
 public:
  virtual ~EngineContextualTextTargetAuthoritySelectorProviderV2() = default;

  // Selectors are engine-owned immutable services.  The returned shared
  // ownership is retained by one receipt adapter and never exposed to a
  // parser/server caller.
  virtual std::shared_ptr<const EngineContextualTextTargetAuthoritySelectorV2>
  SelectForReceipt(const EngineRequestContext& exact_live_context) const = 0;
};

// One process may install its engine-owned provider before issuing receipts.
// Registration is one-shot (or idempotent for the same provider); replacement
// while receipts exist is forbidden. In its absence the adapter uses only the
// engine-owned visible-MGA selector; missing/ambiguous/unsealed selection still
// fails closed and is never replaced by an implicit or caller-supplied row.
bool InstallEngineContextualTextTargetAuthoritySelectorProviderV2(
    std::shared_ptr<
        const EngineContextualTextTargetAuthoritySelectorProviderV2> provider,
    EngineApiDiagnostic* diagnostic);

std::unique_ptr<EngineContextualTextTargetAuthorityResolverV2>
CreateEngineContextualTextTargetAuthorityResolverForReceiptV2(
    const EngineRequestContext& exact_live_context);

}  // namespace scratchbird::engine::internal_api
