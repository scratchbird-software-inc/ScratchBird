// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_family_registry.hpp"

#include <array>
#include <utility>

namespace scratchbird::core::index {
namespace {
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::UuidKind;

TypedUuid FixedIndexUuid(unsigned seed) {
  TypedUuid uuid;
  uuid.kind = UuidKind::object;
  for (std::size_t i = 0; i < uuid.value.bytes.size(); ++i) {
    uuid.value.bytes[i] = static_cast<scratchbird::core::platform::byte>((seed * 37u + i * 17u + 0x71u) & 0xffu);
  }
  uuid.value.bytes[6] = static_cast<scratchbird::core::platform::byte>((uuid.value.bytes[6] & 0x0fu) | 0x70u);
  uuid.value.bytes[8] = static_cast<scratchbird::core::platform::byte>((uuid.value.bytes[8] & 0x3fu) | 0x80u);
  return uuid;
}

Status OkStatus() { return Status{StatusCode::ok, Severity::info, Subsystem::engine}; }
Status InvalidStatus() { return Status{StatusCode::platform_required_feature_missing, Severity::error, Subsystem::engine}; }

IndexFamilyDescriptor D(IndexFamily family, const char* id, IndexPersistenceClass persistence,
                        IndexKeyModel key_model, const char* native, const char* profile,
                        bool baseline, bool ordering, bool unique, bool approximate = false) {
  const auto ordinal = static_cast<unsigned>(family) + 1u;
  return IndexFamilyDescriptor{family,
                               id,
                               id,
                               FixedIndexUuid(ordinal),
                               persistence,
                               key_model,
                               IndexCompletionStatus::accepted_requires_full_implementation,
                               native,
                               profile,
                               std::string("sys.metrics.index.") + id,
                               std::string("INDEX.") + id,
                               std::string("index_family_implementation_packet:") + id,
                               baseline,
                               persistence == IndexPersistenceClass::persistent,
                               true,
                               ordering,
                               unique,
                               approximate};
}

IndexFamilyDescriptor P(IndexFamily family, const char* id, const char* policy) {
  auto desc = D(family, id, IndexPersistenceClass::policy_blocked, IndexKeyModel::donor_defined,
                "policy_blocked", policy, false, false, false, true);
  desc.completion = IndexCompletionStatus::policy_blocked_alpha;
  desc.persistent = false;
  desc.metrics_prefix = std::string("sys.metrics.index.policy_blocked.") + id;
  desc.diagnostics_prefix = std::string("INDEX.POLICY_BLOCKED.") + id;
  return desc;
}

IndexFamilyPhysicalCapabilityState Capability(
    IndexFamily family,
    IndexFamilyPhysicalCapabilityBlocker blocker,
    const char* diagnostic_code,
    const char* message_key,
    const char* detail,
    bool planner_contract_capability = true) {
  IndexFamilyPhysicalCapabilityState state;
  state.family = family;
  state.declared_capability = FindBuiltinIndexFamily(family) != nullptr;
  state.planner_contract_capability = planner_contract_capability;
  state.static_contract = state.declared_capability;
  state.provider_present = false;
  state.evidence_required = state.static_contract;
  state.provider_admitted = false;
  state.durable_closure_admitted = false;
  state.implemented = false;
  state.physical_reader = false;
  state.physical_writer = false;
  state.maintenance = false;
  state.validate = false;
  state.repair = false;
  state.recovery_reopen = false;
  state.rebuild = false;
  state.runtime_available = false;
  state.benchmark_clean = false;
  state.blocker = blocker;
  state.blocker_diagnostic_code = diagnostic_code;
  state.blocker_message_key = message_key;
  state.blocker_detail = detail;
  return state;
}

IndexFamilyPhysicalCapabilityState CompleteCapability(IndexFamily family) {
  IndexFamilyPhysicalCapabilityState state;
  state.family = family;
  state.declared_capability = FindBuiltinIndexFamily(family) != nullptr;
  state.planner_contract_capability = true;
  state.static_contract = state.declared_capability;
  state.provider_present = false;
  state.evidence_required = state.static_contract;
  state.provider_admitted = false;
  state.durable_closure_admitted = false;
  state.implemented = true;
  state.physical_reader = true;
  state.physical_writer = true;
  state.maintenance = true;
  state.validate = true;
  state.repair = true;
  state.recovery_reopen = true;
  state.rebuild = true;
  state.runtime_available = true;
  state.benchmark_clean = true;
  state.blocker = IndexFamilyPhysicalCapabilityBlocker::none;
  return state;
}

IndexFamilyPhysicalCapabilityState UnknownCapability(IndexFamily family) {
  return Capability(family,
                    IndexFamilyPhysicalCapabilityBlocker::unknown_family,
                    "INDEX.CAPABILITY.UNKNOWN_FAMILY",
                    "index.capability.unknown_family",
                    "family is not declared in the built-in index family registry",
                    false);
}
}  // namespace

const std::vector<IndexFamilyDescriptor>& BuiltinIndexFamilyDescriptors() {
  static const std::vector<IndexFamilyDescriptor> descriptors = {
      D(IndexFamily::btree, "btree", IndexPersistenceClass::persistent, IndexKeyModel::ordered_key, "btree", "native_btree", true, true, true),
      D(IndexFamily::unique_btree, "unique_btree", IndexPersistenceClass::persistent, IndexKeyModel::ordered_key, "btree", "native_unique_btree", false, true, true),
      D(IndexFamily::expression, "expression", IndexPersistenceClass::persistent, IndexKeyModel::expression_key, "btree", "native_expression_index", false, true, false),
      D(IndexFamily::partial, "partial", IndexPersistenceClass::persistent, IndexKeyModel::predicate_filtered_key, "btree", "native_partial_index", false, true, false),
      D(IndexFamily::covering, "covering", IndexPersistenceClass::persistent, IndexKeyModel::covering_payload, "btree", "native_covering_index", false, true, false),
      D(IndexFamily::hash, "hash", IndexPersistenceClass::persistent, IndexKeyModel::hashed_key, "hash", "native_hash", false, false, true),
      D(IndexFamily::bitmap, "bitmap", IndexPersistenceClass::persistent, IndexKeyModel::zone_summary, "bitmap", "native_bitmap", false, false, false),
      D(IndexFamily::brin_zone, "brin_zone", IndexPersistenceClass::persistent, IndexKeyModel::zone_summary, "columnar_zone", "native_zone_summary", false, false, false),
      D(IndexFamily::bloom, "bloom", IndexPersistenceClass::persistent, IndexKeyModel::zone_summary, "columnar_zone", "native_bloom_summary", false, false, false),
      D(IndexFamily::full_text, "full_text", IndexPersistenceClass::persistent, IndexKeyModel::token_key, "full_text", "native_full_text", false, false, false),
      D(IndexFamily::gin, "gin", IndexPersistenceClass::persistent, IndexKeyModel::token_key, "full_text", "postgresql_gin_profile", false, false, false),
      D(IndexFamily::inverted, "inverted", IndexPersistenceClass::persistent, IndexKeyModel::token_key, "full_text", "native_inverted", false, false, false),
      D(IndexFamily::ngram, "ngram", IndexPersistenceClass::persistent, IndexKeyModel::token_key, "full_text", "native_ngram", false, false, false),
      D(IndexFamily::sparse_wand, "sparse_wand", IndexPersistenceClass::persistent, IndexKeyModel::token_key, "full_text", "native_sparse_wand", false, false, false),
      D(IndexFamily::spatial, "spatial", IndexPersistenceClass::persistent, IndexKeyModel::spatial_key, "spatial", "native_spatial", false, false, false),
      D(IndexFamily::rtree, "rtree", IndexPersistenceClass::persistent, IndexKeyModel::spatial_key, "spatial", "native_rtree", false, false, false),
      D(IndexFamily::gist, "gist", IndexPersistenceClass::persistent, IndexKeyModel::spatial_key, "spatial", "postgresql_gist_profile", false, false, false),
      D(IndexFamily::spgist, "spgist", IndexPersistenceClass::persistent, IndexKeyModel::spatial_key, "spatial", "postgresql_spgist_profile", false, false, false),
      D(IndexFamily::vector_exact, "vector_exact", IndexPersistenceClass::persistent, IndexKeyModel::vector_key, "vector_exact", "native_vector_exact", false, true, false),
      D(IndexFamily::vector_hnsw, "vector_hnsw", IndexPersistenceClass::persistent, IndexKeyModel::vector_key, "vector_hnsw", "native_hnsw", false, true, false, true),
      D(IndexFamily::vector_ivf, "vector_ivf", IndexPersistenceClass::persistent, IndexKeyModel::vector_key, "vector_ivf", "native_ivf", false, true, false, true),
      D(IndexFamily::columnar_zone, "columnar_zone", IndexPersistenceClass::persistent, IndexKeyModel::zone_summary, "columnar_zone", "native_columnar_zone", false, false, false),
      D(IndexFamily::document_path, "document_path", IndexPersistenceClass::persistent, IndexKeyModel::token_key, "full_text", "native_document_path", false, false, false),
      D(IndexFamily::graph, "graph", IndexPersistenceClass::persistent, IndexKeyModel::donor_defined, "graph", "native_graph_lookup", false, false, false),
      D(IndexFamily::temporary_work, "temporary_work", IndexPersistenceClass::memory_only, IndexKeyModel::donor_defined, "in_memory", "native_temporary_work", false, false, false),
      D(IndexFamily::in_memory, "in_memory", IndexPersistenceClass::memory_primary_persisted_cold_start, IndexKeyModel::donor_defined, "in_memory", "native_in_memory", false, false, false),
      D(IndexFamily::donor_emulated, "donor_emulated", IndexPersistenceClass::donor_emulated, IndexKeyModel::donor_defined, "donor_emulated", "native_donor_emulated", false, false, false),
      P(IndexFamily::policy_blocked, "advanced_vector_policy_blocked", "SB_POLICY_INDEX_ADVANCED_VECTOR_NOT_ACCEPTED_ALPHA")};
  return descriptors;
}

const std::vector<IndexFamilyPhysicalCapabilityState>&
BuiltinIndexFamilyPhysicalCapabilityStates() {
  static const std::vector<IndexFamilyPhysicalCapabilityState> states = {
      CompleteCapability(IndexFamily::btree),
      CompleteCapability(IndexFamily::unique_btree),
      CompleteCapability(IndexFamily::expression),
      CompleteCapability(IndexFamily::partial),
      CompleteCapability(IndexFamily::covering),
      CompleteCapability(IndexFamily::hash),
      CompleteCapability(IndexFamily::bitmap),
      CompleteCapability(IndexFamily::brin_zone),
      CompleteCapability(IndexFamily::bloom),
      CompleteCapability(IndexFamily::full_text),
      CompleteCapability(IndexFamily::gin),
      CompleteCapability(IndexFamily::inverted),
      CompleteCapability(IndexFamily::ngram),
      CompleteCapability(IndexFamily::sparse_wand),
      CompleteCapability(IndexFamily::spatial),
      CompleteCapability(IndexFamily::rtree),
      CompleteCapability(IndexFamily::gist),
      CompleteCapability(IndexFamily::spgist),
      CompleteCapability(IndexFamily::vector_exact),
      CompleteCapability(IndexFamily::vector_hnsw),
      CompleteCapability(IndexFamily::vector_ivf),
      CompleteCapability(IndexFamily::columnar_zone),
      CompleteCapability(IndexFamily::document_path),
      CompleteCapability(IndexFamily::graph),
      CompleteCapability(IndexFamily::temporary_work),
      CompleteCapability(IndexFamily::in_memory),
      Capability(IndexFamily::donor_emulated,
                 IndexFamilyPhysicalCapabilityBlocker::contract_only,
                 "INDEX.CAPABILITY.DONOR_EMULATED.CONTRACT_ONLY_NON_AUTHORITY_MAPPING",
                 "index.capability.donor_emulated.contract_only_non_authority_mapping",
                 "donor-emulated indexes expose semantic mapping only; they must map to native ScratchBird physical providers and cannot own visibility, finality, or recovery"),
      Capability(IndexFamily::policy_blocked,
                 IndexFamilyPhysicalCapabilityBlocker::policy_blocked,
                 "INDEX.CAPABILITY.POLICY_BLOCKED.NOT_ACCEPTED_ALPHA",
                 "index.capability.policy_blocked.not_accepted_alpha",
                 "policy-blocked index family is not an accepted physical Alpha capability and cannot be a runtime or benchmark-clean family",
                 false)};
  return states;
}

const IndexFamilyDescriptor* FindBuiltinIndexFamily(IndexFamily family) {
  for (const auto& descriptor : BuiltinIndexFamilyDescriptors()) {
    if (descriptor.family == family) return &descriptor;
  }
  return nullptr;
}

const IndexFamilyPhysicalCapabilityState*
FindBuiltinIndexFamilyPhysicalCapabilityState(IndexFamily family) {
  for (const auto& state : BuiltinIndexFamilyPhysicalCapabilityStates()) {
    if (state.family == family) return &state;
  }
  return nullptr;
}

IndexFamilyLookupResult FindBuiltinIndexFamilyById(std::string_view id) {
  for (const auto& descriptor : BuiltinIndexFamilyDescriptors()) {
    if (descriptor.id == id) return IndexFamilyLookupResult{OkStatus(), &descriptor, {}};
  }
  return IndexFamilyLookupResult{InvalidStatus(), nullptr,
                                 MakeIndexFamilyDiagnostic(InvalidStatus(), "INDEX.FAMILY_UNSUPPORTED", "index.family.unsupported", std::string(id))};
}

const char* IndexFamilyName(IndexFamily family) {
  const auto* descriptor = FindBuiltinIndexFamily(family);
  return descriptor ? descriptor->id.c_str() : "unknown";
}

const char* IndexPersistenceClassName(IndexPersistenceClass persistence) {
  switch (persistence) {
    case IndexPersistenceClass::persistent: return "persistent";
    case IndexPersistenceClass::memory_primary_persisted_cold_start: return "memory_primary_persisted_cold_start";
    case IndexPersistenceClass::memory_only: return "memory_only";
    case IndexPersistenceClass::virtual_catalog: return "virtual_catalog";
    case IndexPersistenceClass::donor_emulated: return "donor_emulated";
    case IndexPersistenceClass::policy_blocked: return "policy_blocked";
  }
  return "unknown";
}

const char* IndexKeyModelName(IndexKeyModel key_model) {
  switch (key_model) {
    case IndexKeyModel::ordered_key: return "ordered_key";
    case IndexKeyModel::hashed_key: return "hashed_key";
    case IndexKeyModel::token_key: return "token_key";
    case IndexKeyModel::spatial_key: return "spatial_key";
    case IndexKeyModel::vector_key: return "vector_key";
    case IndexKeyModel::zone_summary: return "zone_summary";
    case IndexKeyModel::expression_key: return "expression_key";
    case IndexKeyModel::predicate_filtered_key: return "predicate_filtered_key";
    case IndexKeyModel::covering_payload: return "covering_payload";
    case IndexKeyModel::donor_defined: return "donor_defined";
  }
  return "unknown";
}

const char* IndexCompletionStatusName(IndexCompletionStatus completion) {
  switch (completion) {
    case IndexCompletionStatus::accepted_requires_full_implementation: return "accepted_requires_full_implementation";
    case IndexCompletionStatus::policy_blocked_alpha: return "policy_blocked_alpha";
    case IndexCompletionStatus::rejected: return "rejected";
  }
  return "unknown";
}

const char* IndexFamilyPhysicalCapabilityBlockerName(
    IndexFamilyPhysicalCapabilityBlocker blocker) {
  switch (blocker) {
    case IndexFamilyPhysicalCapabilityBlocker::none: return "none";
    case IndexFamilyPhysicalCapabilityBlocker::contract_only: return "contract_only";
    case IndexFamilyPhysicalCapabilityBlocker::planner_only: return "planner_only";
    case IndexFamilyPhysicalCapabilityBlocker::provider_only: return "provider_only";
    case IndexFamilyPhysicalCapabilityBlocker::lifecycle_only: return "lifecycle_only";
    case IndexFamilyPhysicalCapabilityBlocker::policy_blocked: return "policy_blocked";
    case IndexFamilyPhysicalCapabilityBlocker::rejected: return "rejected";
    case IndexFamilyPhysicalCapabilityBlocker::unknown_family: return "unknown_family";
  }
  return "unknown_family";
}

bool IsPolicyBlockedIndexFamily(IndexFamily family) {
  const auto* descriptor = FindBuiltinIndexFamily(family);
  return descriptor != nullptr && descriptor->completion == IndexCompletionStatus::policy_blocked_alpha;
}

DiagnosticRecord MakeIndexFamilyDiagnostic(Status status, std::string diagnostic_code,
                                           std::string message_key, std::string detail) {
  DiagnosticRecord record;
  record.status = status;
  record.diagnostic_code = std::move(diagnostic_code);
  record.message_key = std::move(message_key);
  if (!detail.empty()) record.arguments.push_back({"detail", std::move(detail)});
  record.source_component = "sb_core_index.family_registry";
  return record;
}

DiagnosticRecord MakeIndexFamilyCapabilityDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string family_id,
    std::string detail,
    IndexFamilyPhysicalCapabilityBlocker blocker) {
  DiagnosticRecord record;
  record.status = status;
  record.diagnostic_code = std::move(diagnostic_code);
  record.message_key = std::move(message_key);
  if (!family_id.empty()) record.arguments.push_back({"family", std::move(family_id)});
  if (blocker != IndexFamilyPhysicalCapabilityBlocker::none) {
    record.arguments.push_back(
        {"blocker", IndexFamilyPhysicalCapabilityBlockerName(blocker)});
  }
  if (!detail.empty()) record.arguments.push_back({"detail", std::move(detail)});
  record.source_component = "sb_core_index.family_capability";
  return record;
}

DiagnosticRecord MakeIndexFamilyCapabilityBlockerDiagnostic(
    Status status,
    const IndexFamilyPhysicalCapabilityState& state) {
  const auto* descriptor = FindBuiltinIndexFamily(state.family);
  return MakeIndexFamilyCapabilityDiagnostic(
      status,
      state.blocker_diagnostic_code.empty()
          ? UnknownCapability(state.family).blocker_diagnostic_code
          : state.blocker_diagnostic_code,
      state.blocker_message_key.empty()
          ? UnknownCapability(state.family).blocker_message_key
          : state.blocker_message_key,
      descriptor ? descriptor->id : IndexFamilyName(state.family),
      state.blocker_detail,
      state.blocker);
}

}  // namespace scratchbird::core::index
