// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_runtime_principal_credential.hpp"
#include "core/catalog/catalog_agent_bootstrap.hpp"
#include <algorithm>
#include <array>
#include <span>

namespace scratchbird::server {
enum class NativeLocalServiceAgentConfigurationError {
  none, credential_unavailable, absent, capacity, invalid_definition, inactive,
  database_mismatch, generation_mismatch, origin_not_committed, duplicate_identity,
  invalid_properties, policy_missing, policy_mismatch, duplicate_registration,
  duplicate_policy_family, orphan_policy, missing_lifecycle_agent, invalid_lifecycle_agent
};
struct NativeStartupAgentDefinition {
  core::catalog::CatalogAgentBootstrapDefinition definition;
  core::platform::Uuid version_uuid;
};
// Bounded borrowed catalog selection. Policy bodies remain configuration data;
// no worker assignment, readiness, provider activation or BEGIN is issued here.
// The private credential owner must outlive all returned text views. Complete
// policy-schema/resource/metric/dependency admission follows outside I/O guards.
struct NativeLocalServiceAgentConfigurationResult {
  static constexpr std::size_t maximum_definitions = 128;
  NativeLocalServiceAgentConfigurationError error = NativeLocalServiceAgentConfigurationError::credential_unavailable;
  core::platform::Uuid failing_object_uuid;
  core::catalog::CatalogValueError record_error = core::catalog::CatalogValueError::none;
  std::array<NativeStartupAgentDefinition, maximum_definitions> definitions{};
  std::size_t definition_count = 0, registration_count = 0, policy_count = 0;
  bool ok() const noexcept { return error == NativeLocalServiceAgentConfigurationError::none; }
  std::span<const NativeStartupAgentDefinition> entries() const noexcept {
    return ok() ? std::span<const NativeStartupAgentDefinition>{definitions}.first(definition_count)
                : std::span<const NativeStartupAgentDefinition>{};
  }
};
namespace native_agent_configuration_detail {
inline std::string_view Property(std::string_view body, std::string_view key) noexcept {
  while (!body.empty()) {
    const auto end = body.find('\n');
    if (end == body.npos) return {};
    const auto line = body.substr(0, end);
    const auto equal = line.find('=');
    if (equal != line.npos && line.substr(0, equal) == key) return line.substr(equal + 1);
    body.remove_prefix(end + 1);
  }
  return {};
}
inline bool Properties(std::string_view body) noexcept {
  if (body.empty() || body.back() != '\n' || body.find('\0') != body.npos || body.find('\r') != body.npos)
    return false;
  std::size_t start = 0, count = 0;
  while (start < body.size()) {
    if (++count > 128) return false;
    const auto end = body.find('\n', start);
    const auto line = body.substr(start, end - start);
    const auto equal = line.find('=');
    if (equal == 0 || equal == line.npos) return false;
    const auto key = line.substr(0, equal);
    // An earlier empty value is still a duplicate declaration.
    std::size_t prior = 0;
    while (prior < start) {
      const auto stop = body.find('\n', prior);
      const auto old = body.substr(prior, stop - prior);
      if (old.substr(0, old.find('=')) == key) return false;
      prior = stop + 1;
    }
    start = end + 1;
  }
  return true;
}
inline bool ContainsFamily(std::string_view families, std::string_view family) noexcept {
  while (!families.empty()) {
    const auto end = families.find(',');
    if (families.substr(0, end) == family) return true;
    if (end == families.npos) break;
    families.remove_prefix(end + 1);
  }
  return false;
}
} // namespace native_agent_configuration_detail

// Select only from the same actual retained checkpoint used for the protected
// service credential comparison. There is no public row-array/path overload.
// The whole cohort is checked before any view is exposed; no allocations or
// physical writes occur under the selected source guards.
inline NativeLocalServiceAgentConfigurationResult SelectNativeLocalServiceAgentConfiguration(
    const NativeRuntimePrincipalCredentialLease& credential) noexcept {
  namespace c = core::catalog;
  namespace mga = transaction::mga;
  using E = NativeLocalServiceAgentConfigurationError;
  using K = c::AgentBootstrapDefinitionKind;
  using native_agent_configuration_detail::Property;
  NativeLocalServiceAgentConfigurationResult out;
  const auto fail = [&](E error, core::platform::Uuid identity = {}) {
    out.error = error; out.failing_object_uuid = identity;
    out.definitions = {}; out.definition_count = out.registration_count = out.policy_count = 0;
    return out;
  };
  if (!credential.valid() || !credential.has_selected_source()) return out;
  const auto& selected = credential.selected_catalog();
  const auto inventory = credential.SourceInventory();
  for (const auto& row : credential.configuration_) {
    const auto& m = row.metadata;
    if (!c::IsCatalogAgentBootstrapPayload(m.record.payload)) continue;
    const auto id = m.record.header.object_uuid.value;
    const auto decoded = c::DecodeCatalogAgentBootstrapDefinition(m.record.payload);
    out.record_error = decoded.error;
    if (!decoded.ok() || !c::CatalogAgentBootstrapMatchesHeader(m.record))
      return fail(E::invalid_definition, id);
    const auto& d = *decoded.record;
    if (d.database_uuid != credential.binding().database_uuid) return fail(E::database_mismatch, id);
    if (row.provisional || m.record.header.deleted || m.authority_scope != c::CatalogAuthorityScope::local ||
        row.effective_lifecycle != c::CatalogObjectLifecycle::active ||
        row.effective_status != c::CatalogObjectStatus::active) return fail(E::inactive, id);
    if (!m.definition_version || !m.catalog_generation || !m.security_epoch || !m.schema_epoch ||
        m.catalog_generation > selected.catalog_generation || m.security_epoch > selected.security_epoch ||
        m.schema_epoch > selected.schema_epoch || m.resource_epoch > selected.resource_epoch)
      return fail(E::generation_mismatch, id);
    const auto origin = std::lower_bound(inventory.begin(), inventory.end(), d.origin_local_transaction_id,
        [](const auto& entry, auto local) { return entry.identity.local_id.value < local; });
    if (origin == inventory.end() || origin->identity.local_id.value != d.origin_local_transaction_id ||
        origin->identity.transaction_uuid.value != d.origin_transaction_uuid.value ||
        origin->identity.scope != mga::TransactionScope::local_node || !mga::HasCommittedInventoryOutcome(*origin))
      return fail(E::origin_not_committed, id);
    if (!native_agent_configuration_detail::Properties(d.properties) ||
        Property(d.properties, "agent_type") != d.agent_type) return fail(E::invalid_properties, id);
    if (d.kind == K::policy) {
      const auto enabled = Property(d.properties, "enabled");
      if ((enabled != "0" && enabled != "1") || Property(d.properties, "policy_family").empty())
        return fail(E::invalid_properties, id);
    } else {
      const auto state = Property(d.properties, "state");
      if ((state != "registered" && state != "disabled") ||
          Property(d.properties, "starts_during_create") != "0" ||
          Property(d.properties, "managed_by") != (d.agent_type == "agent_coordinator" ? "ipc_server" : "agent_coordinator"))
        return fail(E::invalid_properties, id);
    }
    for (std::size_t i = 0; i < out.definition_count; ++i) {
      const auto& prior = out.definitions[i].definition;
      if (prior.definition_uuid == id) return fail(E::duplicate_identity, id);
      if (prior.kind == d.kind && prior.agent_type == d.agent_type) {
        if (d.kind == K::registration) return fail(E::duplicate_registration, id);
        if (Property(prior.properties, "policy_family") == Property(d.properties, "policy_family"))
          return fail(E::duplicate_policy_family, id);
      }
    }
    if (out.definition_count == out.maximum_definitions) return fail(E::capacity, id);
    out.definitions[out.definition_count++] = {d, row.version_uuid};
    if (d.kind == K::policy) ++out.policy_count; else ++out.registration_count;
  }
  if (!out.definition_count) return fail(E::absent);
  unsigned lifecycle = 0;
  for (std::size_t i = 0; i < out.definition_count; ++i) {
    const auto& d = out.definitions[i].definition;
    if (d.kind == K::policy) {
      bool registered = false;
      for (std::size_t j = 0; j < out.definition_count; ++j) {
        const auto& candidate = out.definitions[j].definition;
        if (candidate.kind == K::registration && candidate.agent_type == d.agent_type &&
            native_agent_configuration_detail::ContainsFamily(Property(candidate.properties, "policy_families"),
                Property(d.properties, "policy_family"))) registered = true;
      }
      if (!registered) return fail(E::orphan_policy, d.definition_uuid);
      continue;
    }
    // Admit every listed family, including secondary families. No missing,
    // empty or repeated family can silently disappear from the cohort.
    const auto families = Property(d.properties, "policy_families");
    if (families.empty() || families.back() == ',') return fail(E::policy_mismatch, d.definition_uuid);
    std::size_t start = 0;
    while (start < families.size()) {
      const auto end = families.find(',', start);
      const auto family = families.substr(start, end == families.npos ? end : end - start);
      if (family.empty() || native_agent_configuration_detail::ContainsFamily(families.substr(0, start), family))
        return fail(E::policy_mismatch, d.definition_uuid);
      bool present = false;
      for (std::size_t j = 0; j < out.definition_count; ++j) {
        const auto& candidate = out.definitions[j].definition;
        if (candidate.kind == K::policy && candidate.agent_type == d.agent_type &&
            Property(candidate.properties, "policy_family") == family) present = true;
      }
      if (!present) return fail(E::policy_missing, d.definition_uuid);
      if (end == families.npos) break;
      start = end + 1;
    }
    const c::CatalogAgentBootstrapDefinition* policy = nullptr;
    for (std::size_t j = 0; j < out.definition_count; ++j)
      if (out.definitions[j].definition.definition_uuid == d.policy_uuid) policy = &out.definitions[j].definition;
    if (!policy) return fail(E::policy_missing, d.definition_uuid);
    if (policy->kind != K::policy || policy->agent_type != d.agent_type)
      return fail(E::policy_mismatch, d.definition_uuid);
    const bool enabled = Property(policy->properties, "enabled") == "1";
    if ((Property(d.properties, "state") == "registered") != enabled)
      return fail(E::policy_mismatch, d.definition_uuid);
    const auto bit = d.agent_type == "agent_coordinator" ? 1u : d.agent_type == "startup_agent" ? 2u :
        d.agent_type == "shutdown_agent" ? 4u : 0u;
    if (bit) {
      const auto trigger = bit == 1 ? "server_open" : bit == 2 ? "coordinator_startup" : "coordinator_shutdown";
      const auto family = bit == 1 ? "agent_coordinator_policy" : bit == 2 ? "agent_startup_policy" : "agent_shutdown_policy";
      if (!enabled || Property(d.properties, "launch_trigger") != trigger || Property(policy->properties, "policy_family") != family)
        return fail(E::invalid_lifecycle_agent, d.definition_uuid);
      lifecycle |= bit;
    } else if (Property(d.properties, "launch_trigger") != "policy_schedule") return fail(E::invalid_properties, d.definition_uuid);
  }
  if (lifecycle != 7) return fail(E::missing_lifecycle_agent);
  out.error = E::none;
  return out;
}
} // namespace scratchbird::server
