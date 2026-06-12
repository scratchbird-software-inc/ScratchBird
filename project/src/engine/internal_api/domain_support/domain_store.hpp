// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "api_diagnostics.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_API_DOMAIN_FOUNDATION_STORE
// Domain metadata support for the current engine API/SBLR readiness slice.

inline constexpr const char* kDomainFoundationEventMagic = "SBDOMAIN1";

struct DomainRecord {
  std::uint64_t creator_tx = 0;
  std::string domain_uuid;
  std::string catalog_row_uuid;
  std::string schema_uuid;
  // Migration/display cache only. SQL object name authority is SBNAME1 name registry.
  std::string default_name;
  std::string base_descriptor_uuid;
  std::string base_descriptor_kind;
  std::string base_canonical_type_name;
  std::string base_encoded_descriptor;
  bool nullable = true;
  std::string default_expression_envelope;
  std::string check_constraint_envelope;
  std::string charset_or_collation_ref;
  std::string numeric_metadata;
  std::string cast_policy_envelope;
  std::string mutation_policy_envelope;
  std::string masking_policy_envelope;
  std::string visibility_policy_envelope;
  std::string encryption_policy_ref;
  std::string driver_metadata_envelope;
  std::string wire_metadata_envelope;
  std::string element_path_envelope;
  std::string method_binding_envelope;
  // Parser/request compatibility cache. Durable authority is SBNAME1 name registry.
  std::string localized_names_envelope;
  std::string comment_envelope;
  std::string reference_alias_envelope;
  std::string validation_hook_status;
  bool dropped = false;
};

struct DomainStoreResult {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  std::vector<DomainRecord> domains;
};

struct DomainValueValidationResult {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  EngineTypedValue value;
  std::vector<EngineEvidenceReference> evidence;
};

struct DomainRowValidationResult {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  std::vector<std::pair<std::string, std::string>> values;
  std::vector<EngineEvidenceReference> evidence;
};

struct DomainReadPolicyResult {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  std::vector<std::pair<std::string, std::string>> values;
  std::vector<EngineEvidenceReference> evidence;
};

struct ConstraintDmlValidationCache;

DomainStoreResult LoadDomainState(const EngineRequestContext& context);
EngineApiDiagnostic AppendDomainEvent(const EngineRequestContext& context, const std::string& event);
std::string MakeDomainCreateEvent(const DomainRecord& record);
std::string MakeDomainAlterEvent(const DomainRecord& record);
std::string MakeDomainDropEvent(std::uint64_t creator_tx, const std::string& domain_uuid);
std::optional<DomainRecord> FindVisibleDomain(const EngineRequestContext& context,
                                              const std::string& domain_uuid,
                                              std::uint64_t observer_tx);
EngineDescriptor DomainDescriptor(const DomainRecord& record);
std::string DomainUuidFromDescriptor(const EngineDescriptor& descriptor);
std::string DomainUuidFromColumnDescriptor(const std::string& column_descriptor);
bool IsSupportedDomainCheckEnvelope(const std::string& envelope);
DomainValueValidationResult ValidateDomainTypedValue(const EngineRequestContext& context,
                                                     const EngineDescriptor& domain_descriptor,
                                                     const EngineTypedValue& input_value,
                                                     std::uint64_t observer_tx);
DomainRowValidationResult ApplyDomainRulesToCrudValues(
    const EngineRequestContext& context,
    const std::vector<std::pair<std::string, std::string>>& table_columns,
    const std::vector<std::pair<std::string, std::string>>& input_values,
    std::uint64_t observer_tx,
    ConstraintDmlValidationCache* cache = nullptr);
DomainReadPolicyResult ApplyDomainReadPoliciesToCrudValues(
    const EngineRequestContext& context,
    const std::vector<std::pair<std::string, std::string>>& table_columns,
    const std::vector<std::pair<std::string, std::string>>& input_values,
    std::uint64_t observer_tx);
bool DomainHasCrudDependencies(const EngineRequestContext& context,
                               const std::string& domain_uuid,
                               std::uint64_t observer_tx);
bool DomainChainContainsUuid(const EngineRequestContext& context,
                             const std::string& start_domain_uuid,
                             const std::string& searched_domain_uuid,
                             std::uint64_t observer_tx);

}  // namespace scratchbird::engine::internal_api
