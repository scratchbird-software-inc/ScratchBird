// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_runtime_principal_credential.hpp"
#include "core/catalog/catalog_native_agent_record.hpp"
#include "core/catalog/catalog_object_constants.hpp"
#include <algorithm>
#include <array>
#include <span>

namespace scratchbird::server {
// SB_CATALOG_NATIVE_AGENT_RECORDS (3c-3): native startup selects the typed
// agent records native create writes (schemas 65640..65677) and nothing else;
// no text is parsed. A legacy text record (65824) in a native catalog is not
// an agent definition here and is refused (invalid_definition).
// config_missing .. orphan_config are 3c-3 additions (one typed config record
// per policy).
enum class NativeLocalServiceAgentConfigurationError {
  none, credential_unavailable, absent, capacity, invalid_definition, inactive,
  database_mismatch, generation_mismatch, origin_not_committed, duplicate_identity,
  invalid_properties, policy_missing, policy_mismatch, duplicate_registration,
  duplicate_policy_family, orphan_policy, missing_lifecycle_agent, invalid_lifecycle_agent,
  config_missing, config_mismatch, duplicate_config, orphan_config
};
enum class NativeStartupAgentKind { policy, registration };
// One selected policy (with its family config record) or registration. The
// payload views borrow the credential's retained rows; the selection has
// already decoded and validated every one, so Policy()/Registration() succeed
// for the matching kind. The decoded fields kept here are the ones the cohort
// checks compare.
struct NativeStartupAgentDefinition {
  NativeStartupAgentKind kind = NativeStartupAgentKind::policy;
  core::catalog::CatalogAgentType agent_type = core::catalog::CatalogAgentType::none;
  core::platform::Uuid definition_uuid, database_uuid, policy_uuid;
  core::platform::TypedUuid origin_transaction_uuid;
  core::platform::u64 origin_local_transaction_id = 0, generation = 0;
  // Policy only.
  core::catalog::CatalogAgentPolicyFamily policy_family = core::catalog::CatalogAgentPolicyFamily::none;
  bool enabled = false;
  std::string_view config_payload;
  core::platform::Uuid config_uuid, config_version_uuid;
  // Registration only.
  core::catalog::NativeAgentPolicyFamilyList policy_families;
  core::catalog::CatalogAgentRegistrationState state = core::catalog::CatalogAgentRegistrationState::none;
  core::catalog::CatalogAgentManagedBy managed_by = core::catalog::CatalogAgentManagedBy::none;
  core::catalog::CatalogAgentLaunchTrigger launch_trigger = core::catalog::CatalogAgentLaunchTrigger::none;
  bool starts_during_create = false;
  std::string_view payload;
  core::platform::Uuid version_uuid;
  bool is_policy() const noexcept { return kind == NativeStartupAgentKind::policy; }
  core::catalog::NativeAgentDecodeResult<core::catalog::NativeAgentPolicyRecord> Policy() const {
    return core::catalog::DecodeNativeAgentPolicy(payload);
  }
  core::catalog::NativeAgentDecodeResult<core::catalog::NativeAgentRegistrationRecord> Registration() const {
    return core::catalog::DecodeNativeAgentRegistration(payload);
  }
  // The family config record, decoded into its typed record (policy only).
  template <class R>
  core::catalog::NativeAgentDecodeResult<R> Config() const {
    return core::catalog::DecodeNativeAgentConfig<R>(config_payload);
  }
};
// Bounded borrowed catalog selection. Policy bodies remain configuration data;
// no worker assignment, readiness, provider activation or BEGIN is issued here.
// The private credential owner must outlive all returned payload views.
// Complete policy/descriptor/metric/dependency admission follows outside I/O
// guards (ValidateAgentPolicies).
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

// Select only from the same actual retained checkpoint used for the protected
// service credential comparison. There is no public row-array/path overload.
// The whole cohort is checked before any view is exposed; no allocations or
// physical writes occur under the selected source guards.
inline NativeLocalServiceAgentConfigurationResult SelectNativeLocalServiceAgentConfiguration(
    const NativeRuntimePrincipalCredentialLease& credential) noexcept {
  namespace c = core::catalog;
  namespace mga = transaction::mga;
  using E = NativeLocalServiceAgentConfigurationError;
  using K = NativeStartupAgentKind;
  using T = c::CatalogAgentType;
  using Uuid = core::platform::Uuid;
  NativeLocalServiceAgentConfigurationResult out;
  const auto fail = [&](E error, Uuid identity = {}) {
    out.error = error; out.failing_object_uuid = identity;
    out.definitions = {}; out.definition_count = out.registration_count = out.policy_count = 0;
    return out;
  };
  if (!credential.valid() || !credential.has_selected_source()) return out;
  const auto& selected = credential.selected_catalog();
  const auto inventory = credential.SourceInventory();
  // Config records, paired with their policy after the scan.
  struct Config {
    Uuid object_uuid, version_uuid, policy_uuid;
    c::CatalogAgentPolicyFamily family = c::CatalogAgentPolicyFamily::none;
    std::string_view payload;
    bool attached = false;
  };
  std::array<Config, NativeLocalServiceAgentConfigurationResult::maximum_definitions> configs{};
  std::size_t config_count = 0;
  for (const auto& row : credential.configuration_) {
    const auto& m = row.metadata;
    const bool legacy = c::IsCatalogAgentBootstrapPayload(m.record.payload);
    const bool typed = c::IsNativeAgentPayload(m.record.payload);
    const bool agent_subtype = m.object_subtype == c::CatalogObjectSubtype::agent_policy ||
        m.object_subtype == c::CatalogObjectSubtype::agent_registration ||
        m.object_subtype == c::CatalogObjectSubtype::agent_policy_config;
    if (!typed && !legacy && !agent_subtype) continue;
    const auto id = m.record.header.object_uuid.value;
    // A native catalog carries only typed agent records; the legacy text
    // record belongs to legacy-created databases (drift; not read here).
    if (!typed || !c::NativeAgentRecordMatchesHeader(m.record)) {
      out.record_error = c::CatalogValueError::invalid_value;
      return fail(E::invalid_definition, id);
    }
    const auto schema = c::NativeAgentPayloadSchemaId(m.record.payload);
    const auto expected_subtype = schema == c::kNativeAgentPolicySchemaId ? c::CatalogObjectSubtype::agent_policy
        : schema == c::kNativeAgentRegistrationSchemaId ? c::CatalogObjectSubtype::agent_registration
        : c::CatalogObjectSubtype::agent_policy_config;
    if (m.object_subtype != expected_subtype) return fail(E::invalid_definition, id);
    if (row.provisional || m.record.header.deleted || m.authority_scope != c::CatalogAuthorityScope::local ||
        row.effective_lifecycle != c::CatalogObjectLifecycle::active ||
        row.effective_status != c::CatalogObjectStatus::active) return fail(E::inactive, id);
    if (!m.definition_version || !m.catalog_generation || !m.security_epoch || !m.schema_epoch ||
        m.catalog_generation > selected.catalog_generation || m.security_epoch > selected.security_epoch ||
        m.schema_epoch > selected.schema_epoch || m.resource_epoch > selected.resource_epoch)
      return fail(E::generation_mismatch, id);
    const auto committed = [&](const core::platform::TypedUuid& tx, core::platform::u64 local) {
      const auto origin = std::lower_bound(inventory.begin(), inventory.end(), local,
          [](const auto& entry, auto value) { return entry.identity.local_id.value < value; });
      return origin != inventory.end() && origin->identity.local_id.value == local &&
          origin->identity.transaction_uuid.value == tx.value &&
          origin->identity.scope == mga::TransactionScope::local_node && mga::HasCommittedInventoryOutcome(*origin);
    };
    if (schema != c::kNativeAgentPolicySchemaId && schema != c::kNativeAgentRegistrationSchemaId) {
      const auto head = c::DecodeNativeAgentConfigHead(m.record.payload);
      out.record_error = head.error;
      if (!head.ok()) return fail(E::invalid_definition, id);
      if (!committed(head.record->origin_transaction_uuid, head.record->origin_local_transaction_id))
        return fail(E::origin_not_committed, id);
      for (std::size_t i = 0; i < config_count; ++i)
        if (configs[i].object_uuid == id) return fail(E::duplicate_identity, id);
      if (config_count == configs.size()) return fail(E::capacity, id);
      configs[config_count++] = {id, row.version_uuid, head.record->definition_uuid, head.record->family,
                                 m.record.payload, false};
      continue;
    }
    NativeStartupAgentDefinition d;
    d.payload = m.record.payload;d.version_uuid = row.version_uuid;
    if (schema == c::kNativeAgentPolicySchemaId) {
      const auto decoded = c::DecodeNativeAgentPolicy(m.record.payload);
      out.record_error = decoded.error;
      if (!decoded.ok()) return fail(E::invalid_definition, id);
      const auto& p = *decoded.record;
      d.kind = K::policy;d.agent_type = p.agent_type;d.definition_uuid = p.definition_uuid;
      d.database_uuid = p.database_uuid;d.policy_uuid = p.policy_uuid;d.origin_transaction_uuid = p.origin_transaction_uuid;
      d.origin_local_transaction_id = p.origin_local_transaction_id;d.generation = p.generation;
      d.policy_family = p.policy_family;d.enabled = p.enabled;
    } else {
      const auto decoded = c::DecodeNativeAgentRegistration(m.record.payload);
      out.record_error = decoded.error;
      if (!decoded.ok()) return fail(E::invalid_definition, id);
      const auto& r = *decoded.record;
      d.kind = K::registration;d.agent_type = r.agent_type;d.definition_uuid = r.definition_uuid;
      d.database_uuid = r.database_uuid;d.policy_uuid = r.policy_uuid;d.origin_transaction_uuid = r.origin_transaction_uuid;
      d.origin_local_transaction_id = r.origin_local_transaction_id;d.generation = r.generation;
      d.policy_families = r.policy_families;d.state = r.state;d.managed_by = r.managed_by;
      d.launch_trigger = r.launch_trigger;d.starts_during_create = r.starts_during_create;
    }
    if (d.database_uuid != credential.binding().database_uuid) return fail(E::database_mismatch, id);
    if (!committed(d.origin_transaction_uuid, d.origin_local_transaction_id)) return fail(E::origin_not_committed, id);
    if (d.kind == K::registration) {
      // state is registered or disabled (the codec admits no other value).
      if (d.starts_during_create ||
          d.managed_by != (d.agent_type == T::agent_coordinator ? c::CatalogAgentManagedBy::ipc_server
                                                                : c::CatalogAgentManagedBy::agent_coordinator))
        return fail(E::invalid_properties, id);
    }
    for (std::size_t i = 0; i < out.definition_count; ++i) {
      const auto& prior = out.definitions[i];
      if (prior.definition_uuid == id) return fail(E::duplicate_identity, id);
      if (prior.kind == d.kind && prior.agent_type == d.agent_type) {
        if (d.kind == K::registration) return fail(E::duplicate_registration, id);
        if (prior.policy_family == d.policy_family) return fail(E::duplicate_policy_family, id);
      }
    }
    for (std::size_t i = 0; i < config_count; ++i)
      if (configs[i].object_uuid == id) return fail(E::duplicate_identity, id);
    if (out.definition_count == out.maximum_definitions) return fail(E::capacity, id);
    out.definitions[out.definition_count++] = d;
    if (d.kind == K::policy) ++out.policy_count; else ++out.registration_count;
  }
  if (!out.definition_count) return fail(E::absent);
  // Each config record configures exactly one selected policy of its family,
  // and every policy has exactly one.
  for (std::size_t i = 0; i < config_count; ++i) {
    auto& config = configs[i];
    for (std::size_t j = 0; j < out.definition_count; ++j)
      if (out.definitions[j].definition_uuid == config.object_uuid) return fail(E::duplicate_identity, config.object_uuid);
    NativeStartupAgentDefinition* policy = nullptr;
    for (std::size_t j = 0; j < out.definition_count; ++j)
      if (out.definitions[j].kind == K::policy && out.definitions[j].definition_uuid == config.policy_uuid)
        policy = &out.definitions[j];
    if (!policy) return fail(E::orphan_config, config.object_uuid);
    if (!policy->config_payload.empty()) return fail(E::duplicate_config, config.object_uuid);
    if (config.family != policy->policy_family) return fail(E::config_mismatch, config.object_uuid);
    policy->config_payload = config.payload;policy->config_uuid = config.object_uuid;
    policy->config_version_uuid = config.version_uuid;
  }
  unsigned lifecycle = 0;
  for (std::size_t i = 0; i < out.definition_count; ++i) {
    const auto& d = out.definitions[i];
    if (d.kind == K::policy) {
      if (d.config_payload.empty()) return fail(E::config_missing, d.definition_uuid);
      bool registered = false;
      for (std::size_t j = 0; j < out.definition_count; ++j) {
        const auto& candidate = out.definitions[j];
        if (candidate.kind == K::registration && candidate.agent_type == d.agent_type &&
            std::ranges::find(candidate.policy_families.values(), d.policy_family) !=
                candidate.policy_families.values().end()) registered = true;
      }
      if (!registered) return fail(E::orphan_policy, d.definition_uuid);
      continue;
    }
    // Admit every listed family, including secondary families. The codec
    // already refuses an empty or repeated family list.
    if (!d.policy_families.count) return fail(E::policy_mismatch, d.definition_uuid);
    for (const auto family : d.policy_families.values()) {
      bool present = false;
      for (std::size_t j = 0; j < out.definition_count; ++j) {
        const auto& candidate = out.definitions[j];
        if (candidate.kind == K::policy && candidate.agent_type == d.agent_type && candidate.policy_family == family)
          present = true;
      }
      if (!present) return fail(E::policy_missing, d.definition_uuid);
    }
    const NativeStartupAgentDefinition* policy = nullptr;
    for (std::size_t j = 0; j < out.definition_count; ++j)
      if (out.definitions[j].definition_uuid == d.policy_uuid) policy = &out.definitions[j];
    if (!policy) return fail(E::policy_missing, d.definition_uuid);
    if (policy->kind != K::policy || policy->agent_type != d.agent_type)
      return fail(E::policy_mismatch, d.definition_uuid);
    if ((d.state == c::CatalogAgentRegistrationState::registered) != policy->enabled)
      return fail(E::policy_mismatch, d.definition_uuid);
    const auto bit = d.agent_type == T::agent_coordinator ? 1u : d.agent_type == T::startup_agent ? 2u :
        d.agent_type == T::shutdown_agent ? 4u : 0u;
    if (bit) {
      using L = c::CatalogAgentLaunchTrigger;using F = c::CatalogAgentPolicyFamily;
      const auto trigger = bit == 1 ? L::server_open : bit == 2 ? L::coordinator_startup : L::coordinator_shutdown;
      const auto family = bit == 1 ? F::agent_coordinator_policy : bit == 2 ? F::agent_startup_policy : F::agent_shutdown_policy;
      if (!policy->enabled || d.launch_trigger != trigger || policy->policy_family != family)
        return fail(E::invalid_lifecycle_agent, d.definition_uuid);
      lifecycle |= bit;
    } else if (d.launch_trigger != c::CatalogAgentLaunchTrigger::policy_schedule)
      return fail(E::invalid_properties, d.definition_uuid);
  }
  if (lifecycle != 7) return fail(E::missing_lifecycle_agent);
  out.error = E::none;
  return out;
}
} // namespace scratchbird::server
