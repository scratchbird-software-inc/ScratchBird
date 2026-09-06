// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "mga_relation_store/mga_relation_store.hpp"
#include "mga_relation_store/mga_contextual_text_sidecar_set_v2.hpp"
#include "query/contextual_text_target_authority_resolver_v2.hpp"

#include <initializer_list>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_MGA_CONTEXTUAL_TEXT_DESCRIPTOR_INTERFACE
// Descriptor parsing and canonical contextual-text projection are metadata
// interpretation only. They do not determine transaction visibility/finality.
struct MgaContextualTextProjectionMaterialV2 {
  EnginePublicRelationProjectionV3 public_projection;
  std::vector<MgaContextualTextProjectedColumnV2> projected_columns;
};

struct MgaSealedContextualTextDescriptorMaterialV2 {
  MgaRelationStorageDescriptor relation_descriptor;
  std::vector<MgaContextualTextDescriptorFieldPairV2> base_fields;
  MgaContextualTextProjectionMaterialV2 projection;
  MgaContextualTextSidecarSetV2 sealed_set;
};

std::string EncodeStringListAsCrudPairs(
    const std::vector<std::string>& values);
std::map<std::string, std::string> RelationDescriptorFields(
    const std::string& descriptor);
std::optional<std::map<std::string, std::string>>
StrictRelationDescriptorFields(
    const std::string& descriptor);
bool ReplaceExactRelationDescriptorIdentities(
    std::string* descriptor,
    const std::map<std::string,
                   std::pair<std::string_view, std::string_view>>& replacements);
bool CanonicalNonNilMigrationUuid(std::string_view value);
bool ExactCanonicalTextIdentityAuthorityAvailable(
    const EngineRequestContext& context);
bool ExactCanonicalMigratedTextDescriptor(
    const EngineRequestContext& context,
    std::string_view descriptor,
    std::string_view column_uuid);
bool RewriteLegacyTextDescriptor(
    const EngineRequestContext& context,
    std::string* descriptor,
    std::string_view column_uuid);
EngineApiDiagnostic ContextualTextMgaDiagnostic(std::string detail);
bool CopyContextualUuidV2(std::string_view text,
                          MgaContextualTextUuidV2* output,
                          bool allow_nil = false);
std::string ContextualUuidTextV2(const MgaContextualTextUuidV2& value);
std::vector<MgaContextualTextDescriptorFieldPairV2>
RawContextualDescriptorFieldsV2(
    const std::vector<std::pair<std::string, std::string>>& fields);
bool BuildMgaContextualTextProjectionMaterialV2(
    const EngineRequestContext& context,
    const MgaRelationStorageDescriptor& relation,
    const EngineContextualTextPolicyRowSetV2& exact_policy_rows,
    MgaContextualTextProjectionMaterialV2* output,
    EngineApiDiagnostic* diagnostic);
bool BindFreshCanonicalTextColumnIdentitiesV2(
    CrudTableRecord* table,
    MgaRelationStorageDescriptor* relation,
    EngineApiDiagnostic* diagnostic);
bool BuildMgaSealedContextualTextDescriptorMaterialV2(
    const EngineRequestContext& context,
    const CrudTableRecord& table,
    MgaRelationStorageDescriptor relation,
    const EngineContextualTextPolicyRowSetV2& exact_policy_rows,
    MgaSealedContextualTextDescriptorMaterialV2* output,
    EngineApiDiagnostic* diagnostic);
std::string RelationDescriptorFieldOrEmpty(
    const std::map<std::string, std::string>& fields,
    std::initializer_list<const char*> keys);
bool RelationDescriptorBoolField(
    const std::map<std::string, std::string>& fields,
    std::initializer_list<const char*> keys);
bool RelationDescriptorRequiresDeferredStore(
    const std::map<std::string, std::string>& fields);
std::optional<std::string> ParentTableUuidFromRelationDescriptor(
    const std::string& descriptor);
std::set<std::string> InsertTargetRelationScope(
    const EngineRequestContext& context,
    const RelationReadSnapshot& metadata,
    const std::string& table_uuid);

}  // namespace scratchbird::engine::internal_api
