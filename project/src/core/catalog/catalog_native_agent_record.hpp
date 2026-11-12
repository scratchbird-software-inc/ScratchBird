// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
// SEARCH_KEY: SB_CATALOG_NATIVE_AGENT_RECORDS (owner Q68 = 1, Q69 = 1, Q70 = 2,
// 2026-11-12). Typed native agent catalog records, each a fixed-schema SBCV
// value block with no text field:
//  - NativeAgentPolicyRecord (schema 65640): one agent policy (replaces the
//    text bootstrap row of kind policy, schema 65824).
//  - NativeAgentRegistrationRecord (schema 65641): one agent registration
//    (replaces the text row of kind registration).
//  - NativeAgentConfig_<family> (schema 65641 + family code, 65642..65677):
//    the per-family config.* fields of one policy (Q70 = 2: one typed record
//    per policy family), keyed by the policy's definition_uuid.
// Every identifier is a binary(16) engine identity or a coding-time constant
// (catalog_agent_constants.hpp, or the generated enums below). A policy's name
// lives only in the names table; the English text of invalid_policy_behavior
// is not stored. Code lists (metric families, policy families, agent types,
// layers) are opaque bytes of little-endian u16 codes, in order, no
// duplicates, each a listed nonzero code. Enumerations must be a listed
// nonzero value; a wire value wider than u16 is refused, never truncated.
//
// Codec only (commit 3c-2): nothing writes or reads these yet; the text codec
// (catalog_agent_bootstrap.hpp) is unchanged. The *FromTags functions are the
// boundary adapters from the text the current code writes
// (agent_bootstrap_configuration.hpp); engine code compares the constants.
#include "catalog_agent_bootstrap.hpp"
#include "catalog_agent_constants.hpp"
#include "catalog_record_codec.hpp"
#include "catalog_value_codec.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>

namespace scratchbird::core::catalog {
inline constexpr u32 kNativeAgentPolicySchemaId = 65640;
inline constexpr u32 kNativeAgentRegistrationSchemaId = 65641;
// A config record's schema id is this base + its policy family code.
inline constexpr u32 kNativeAgentConfigSchemaIdBase = 65641;
inline constexpr u32 kNativeAgentFirstSchemaId = 65640, kNativeAgentLastSchemaId = 65677;
inline constexpr u16 kNativeAgentSchemaVersion = 1;
constexpr u32 NativeAgentConfigSchemaIdFor(CatalogAgentPolicyFamily family) noexcept {
  return IsKnownCatalogAgentPolicyFamily(family) ? kNativeAgentConfigSchemaIdBase + static_cast<u32>(family) : 0;
}

// An ordered list of u16 codes with fixed capacity (the size of the code's
// universe: no duplicates, so it can never need more). Allocation-free.
template <class E, std::size_t N>
struct NativeAgentCodeList {
  static constexpr std::size_t capacity = N;
  std::array<E, N> codes{};
  std::size_t count = 0;
  std::span<const E> values() const noexcept { return std::span<const E>(codes).first(count <= N ? count : N); }
  bool push(E value) noexcept {
    if (count >= N) return false;
    codes[count++] = value;
    return true;
  }
  bool operator==(const NativeAgentCodeList& other) const noexcept {
    return count == other.count && std::equal(values().begin(), values().end(), other.values().begin());
  }
};
using NativeAgentMetricFamilyList = NativeAgentCodeList<CatalogMetricFamily, kCatalogMetricFamilies.size()>;
using NativeAgentPolicyFamilyList = NativeAgentCodeList<CatalogAgentPolicyFamily, kCatalogAgentPolicyFamilies.size()>;
using NativeAgentTypeList = NativeAgentCodeList<CatalogAgentType, kCatalogAgentTypes.size()>;
using NativeAgentLayerList = NativeAgentCodeList<CatalogAgentRuntimeLayer, kCatalogAgentRuntimeLayerTags.size()>;

// Boundary spelling of a layer in agent_coordinator_policy config.managed_layers
// ("L1,L2,L3"; agent_policy_schema.cpp, checked by the native policy reader).
// It differs from the layer tags of catalog_agent_constants.hpp.
inline constexpr std::array<CatalogAgentTagEntry<CatalogAgentRuntimeLayer>, 5> kNativeAgentManagedLayerTags{{
    {CatalogAgentRuntimeLayer::l1_observation, "L1"},
    {CatalogAgentRuntimeLayer::l2_recorder, "L2"},
    {CatalogAgentRuntimeLayer::l3_dispatcher, "L3"},
    {CatalogAgentRuntimeLayer::l4_worker, "L4"},
    {CatalogAgentRuntimeLayer::l5_coordinator, "L5"}}};
constexpr std::optional<CatalogAgentRuntimeLayer> NativeAgentManagedLayerForTag(std::string_view tag) noexcept {
  return CatalogAgentTagLookup(kNativeAgentManagedLayerTags, tag);
}

// ---- policy (schema 65640) ----
// policy_uuid = definition_uuid (as the text codec requires). The startup
// verification selection is allowed only for agent_type agent_coordinator:
// verification_levels nonzero within 1023; the four policy identities engine
// identities, distinct, and none equal to definition, database, policy or
// origin transaction (same rules as catalog_agent_bootstrap.hpp).
struct NativeAgentPolicyRecord {
  Uuid definition_uuid, database_uuid, policy_uuid;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id = 0, generation = 0;
  CatalogAgentType agent_type = CatalogAgentType::none;
  CatalogAgentPolicyFamily policy_family = CatalogAgentPolicyFamily::none;
  CatalogAgentPolicyClass policy_class = CatalogAgentPolicyClass::none;
  u64 policy_generation = 0;
  CatalogAgentRuntimeLayer layer = CatalogAgentRuntimeLayer::none;
  CatalogAgentDeployment deployment = CatalogAgentDeployment::none;
  CatalogAgentScope scope = CatalogAgentScope::none;  // nonzero bit set of listed flags
  CatalogAgentAuthorityClass authority = CatalogAgentAuthorityClass::none;
  bool enabled = false;
  CatalogAgentActivationProfile activation = CatalogAgentActivationProfile::none;
  CatalogAgentActionMode action_mode = CatalogAgentActionMode::none;
  CatalogAgentInvalidPolicyBehavior invalid_policy_behavior = CatalogAgentInvalidPolicyBehavior::none;
  bool allow_live_action = false, require_manual_approval = false, require_dry_run_before_live = false,
       evidence_required = false, explainability_required = false, editable_seed_policy = false;
  u64 run_interval_microseconds = 0, jitter_microseconds = 0, lease_microseconds = 0, cooldown_microseconds = 0,
      max_runtime_microseconds = 0, max_restart_attempts = 0, initial_backoff_microseconds = 0,
      max_backoff_microseconds = 0, max_history_query_rows = 0, max_evidence_fanout = 0, max_label_cardinality = 0,
      action_budget_per_window = 0;
  NativeAgentMetricFamilyList required_metric_families;  // may be empty
  NativeAgentPolicyFamilyList policy_dependencies;       // may be empty
  std::optional<CatalogAgentStartupVerificationSelection> startup_verification;
  bool operator==(const NativeAgentPolicyRecord& o) const noexcept;
};
// ---- registration (schema 65641) ----
// policy_uuid is the primary policy. policy_families is nonempty.
struct NativeAgentRegistrationRecord {
  Uuid definition_uuid, database_uuid, policy_uuid;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id = 0, generation = 0;
  CatalogAgentType agent_type = CatalogAgentType::none;
  NativeAgentPolicyFamilyList policy_families;
  CatalogAgentRegistrationState state = CatalogAgentRegistrationState::none;
  CatalogAgentManagedBy managed_by = CatalogAgentManagedBy::none;
  CatalogAgentLaunchTrigger launch_trigger = CatalogAgentLaunchTrigger::none;
  bool starts_during_create = false;
  bool operator==(const NativeAgentRegistrationRecord& o) const noexcept;
};
inline constexpr std::size_t kNativeAgentPolicyMaxBytes = 1635;
inline constexpr std::size_t kNativeAgentRegistrationMaxBytes = 305;

template <class Record>
struct NativeAgentDecodeResult {
  CatalogValueError error = CatalogValueError::invalid_value;
  std::optional<Record> record;
  bool ok() const noexcept { return error == CatalogValueError::none && record.has_value(); }
};

// EncodedBytes/EncodeInto/Decode are allocation-free; Encode owns its bytes.
// Encoding alone is not authorization or publication.
std::optional<std::size_t> NativeAgentPolicyEncodedBytes(const NativeAgentPolicyRecord&);
CatalogValueEncodeIntoResult EncodeNativeAgentPolicyInto(const NativeAgentPolicyRecord&, std::span<byte>);
CatalogValueEncodeResult EncodeNativeAgentPolicy(const NativeAgentPolicyRecord&);
NativeAgentDecodeResult<NativeAgentPolicyRecord> DecodeNativeAgentPolicy(std::string_view);

std::optional<std::size_t> NativeAgentRegistrationEncodedBytes(const NativeAgentRegistrationRecord&);
CatalogValueEncodeIntoResult EncodeNativeAgentRegistrationInto(const NativeAgentRegistrationRecord&, std::span<byte>);
CatalogValueEncodeResult EncodeNativeAgentRegistration(const NativeAgentRegistrationRecord&);
NativeAgentDecodeResult<NativeAgentRegistrationRecord> DecodeNativeAgentRegistration(std::string_view);

// Header binding, as CatalogAgentBootstrapMatchesHeader: a policy record under
// header kind policy, a registration under config_profile; header object (kind
// object) = definition_uuid. Config records have no header binding yet (their
// catalog record kind is a wiring decision).
bool NativeAgentPolicyMatchesHeader(const CatalogTypedRecordView&);
bool NativeAgentRegistrationMatchesHeader(const CatalogTypedRecordView&);
// True when the payload is an SBCV block under one of 65640..65677. Selection
// only; the payload is not validated.
bool IsNativeAgentPayload(std::string_view payload) noexcept;

// ---- boundary adapters from the current text (key=value properties) ----
struct NativeAgentPropertyTag {
  std::string_view key, value;
};
template <class Record>
struct NativeAgentTagResult {
  std::optional<Record> record;
  std::string_view bad_key;  // the refused key (or the missing one)
  bool ok() const noexcept { return record.has_value(); }
};
// Policy: every non-config key the builder writes is required exactly once.
// policy_name is accepted and NOT carried (the names table holds it);
// agent_type maps to its constant; config.* keys are skipped (they belong to
// the family config record). Flags are "0"/"1". Identity, origin and
// generation fields are left for the caller; the result is not validated.
NativeAgentTagResult<NativeAgentPolicyRecord> NativeAgentPolicyFromTags(std::span<const NativeAgentPropertyTag>);
// Registration: profile_name is accepted and NOT carried (names table);
// profile_class must be "agent_registration" (the record kind says it).
NativeAgentTagResult<NativeAgentRegistrationRecord> NativeAgentRegistrationFromTags(
    std::span<const NativeAgentPropertyTag>);

// ---- per-family config records ----
// Wire layout of every config record: tag 1 definition_uuid (the policy record
// it configures, kind object), tag 2 origin transaction (kind transaction),
// tag 3 origin local id (nonzero); tags 4.. the family's fields in schema-map
// order (a field that is not representable keeps its tag, unused); tags
// 64..76 the optional common resource-budget fields.
enum class NativeAgentConfigSourceType : u8 {  // mirrors AgentPolicyFieldType
  boolean = 1, unsigned_integer, decimal, duration_seconds, duration_microseconds, byte_count, percent, token,
  token_list };
enum class NativeAgentConfigValueKind : u8 {
  unsigned_integer = 1,  // u64
  boolean,               // bool; boundary "true"/"false"
  decimal,               // IEEE-754 binary64, 8 opaque little-endian bytes; finite, not negative
  enumeration,           // a generated u16 enum below
  agent_type,            // CatalogAgentType
  launch_trigger,        // CatalogAgentLaunchTrigger
  agent_type_list,       // NativeAgentTypeList
  layer_list,            // NativeAgentLayerList (boundary "L1".."L5")
  unrepresented,         // free text; NOT carried (no tag on the wire)
};
struct NativeAgentConfigFieldDescriptor {
  std::string_view key;  // boundary spelling (without "config.")
  u16 tag = 0;
  NativeAgentConfigSourceType source_type = NativeAgentConfigSourceType::boolean;
  NativeAgentConfigValueKind kind = NativeAgentConfigValueKind::unsigned_integer;
  bool required = false;
  bool has_minimum = false, has_maximum = false;
  double minimum = 0, maximum = 0;
};

// ---- GENERATED: header (gen_native_agent_config.py); never edit by hand ----
// Inputs: agent_policy_schema.cpp sha256 0ff5471aefedc938047b42a0b0b5ac97d5238dbeeda2be89a0016c756e4bef28
//         catalog_agent_constants.hpp sha256 60641899040a89bfa385a6da5a52cd83e85fea7fe7713083cfcb48c7efdd4763
// The optional common resource-budget fields (CommonResourceBudgetFields,
// appended to every family by AgentPolicySchemaForFamily). Tags 64..76.
struct NativeAgentConfigResourceBudget {
  std::optional<bool> protect_foreground_work;
  std::optional<u64> max_cpu_time_microseconds;
  std::optional<u64> max_memory_bytes;
  std::optional<u64> max_io_bytes;
  std::optional<u64> max_io_ops;
  std::optional<u64> max_thread_slots;
  std::optional<u64> max_queue_depth;
  std::optional<u64> min_run_interval_microseconds;
  std::optional<u64> retry_backoff_microseconds;
  std::optional<u64> watchdog_timeout_microseconds;
  std::optional<u64> max_history_query_rows;
  std::optional<u64> max_evidence_fanout;
  std::optional<u64> max_label_cardinality;
  bool operator==(const NativeAgentConfigResourceBudget&) const noexcept = default;
};
// node_resource_policy (code 1): schema 65642, max 337 bytes.
struct NativeAgentConfig_node_resource_policy {
  static constexpr CatalogAgentPolicyFamily family = CatalogAgentPolicyFamily::node_resource_policy;
  static constexpr u32 schema_id = 65642;
  static constexpr std::size_t max_bytes = 337;
  Uuid definition_uuid;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id = 0;
  u64 capability_probe_interval = 0;  // tag 4
  u64 suitability_score_window = 0;  // tag 5
  u64 stale_after = 0;  // tag 6
  NativeAgentConfigResourceBudget common;  // tags 64..76
  bool operator==(const NativeAgentConfig_node_resource_policy& o) const noexcept {
    return definition_uuid == o.definition_uuid &&
           origin_transaction_uuid.kind == o.origin_transaction_uuid.kind &&
           origin_transaction_uuid.value == o.origin_transaction_uuid.value &&
           origin_local_transaction_id == o.origin_local_transaction_id &&
           capability_probe_interval == o.capability_probe_interval &&
           suitability_score_window == o.suitability_score_window &&
           stale_after == o.stale_after &&
           common == o.common;
  }
};
// metric_registry_policy (code 2): schema 65643, max 362 bytes.
struct NativeAgentConfig_metric_registry_policy {
  static constexpr CatalogAgentPolicyFamily family = CatalogAgentPolicyFamily::metric_registry_policy;
  static constexpr u32 schema_id = 65643;
  static constexpr std::size_t max_bytes = 362;
  Uuid definition_uuid;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id = 0;
  u64 schema_version = 0;  // tag 4
  u64 cardinality_limit = 0;  // tag 5
  u64 retention_window = 0;  // tag 6
  u64 rollup_interval = 0;  // tag 7
  bool reject_on_schema_drift = false;  // tag 8
  NativeAgentConfigResourceBudget common;  // tags 64..76
  bool operator==(const NativeAgentConfig_metric_registry_policy& o) const noexcept {
    return definition_uuid == o.definition_uuid &&
           origin_transaction_uuid.kind == o.origin_transaction_uuid.kind &&
           origin_transaction_uuid.value == o.origin_transaction_uuid.value &&
           origin_local_transaction_id == o.origin_local_transaction_id &&
           schema_version == o.schema_version &&
           cardinality_limit == o.cardinality_limit &&
           retention_window == o.retention_window &&
           rollup_interval == o.rollup_interval &&
           reject_on_schema_drift == o.reject_on_schema_drift &&
           common == o.common;
  }
};
// storage_health_policy (code 3): schema 65644, max 353 bytes.
struct NativeAgentConfig_storage_health_policy {
  static constexpr CatalogAgentPolicyFamily family = CatalogAgentPolicyFamily::storage_health_policy;
  static constexpr u32 schema_id = 65644;
  static constexpr std::size_t max_bytes = 353;
  Uuid definition_uuid;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id = 0;
  u64 fsync_p99_critical_us = 0;  // tag 4
  u64 unknown_page_quarantine_threshold = 0;  // tag 5
  u64 checksum_failure_threshold = 0;  // tag 6
  u64 optimizer_cost_update_window = 0;  // tag 7
  NativeAgentConfigResourceBudget common;  // tags 64..76
  bool operator==(const NativeAgentConfig_storage_health_policy& o) const noexcept {
    return definition_uuid == o.definition_uuid &&
           origin_transaction_uuid.kind == o.origin_transaction_uuid.kind &&
           origin_transaction_uuid.value == o.origin_transaction_uuid.value &&
           origin_local_transaction_id == o.origin_local_transaction_id &&
           fsync_p99_critical_us == o.fsync_p99_critical_us &&
           unknown_page_quarantine_threshold == o.unknown_page_quarantine_threshold &&
           checksum_failure_threshold == o.checksum_failure_threshold &&
           optimizer_cost_update_window == o.optimizer_cost_update_window &&
           common == o.common;
  }
};
// memory_governor_policy (code 4): schema 65645, max 353 bytes.
struct NativeAgentConfig_memory_governor_policy {
  static constexpr CatalogAgentPolicyFamily family = CatalogAgentPolicyFamily::memory_governor_policy;
  static constexpr u32 schema_id = 65645;
  static constexpr std::size_t max_bytes = 353;
  Uuid definition_uuid;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id = 0;
  u64 emergency_reserve_percent = 0;  // tag 4
  u64 allocation_failure_pressure = 0;  // tag 5
  u64 spill_threshold = 0;  // tag 6
  u64 cache_shrink_limit = 0;  // tag 7
  NativeAgentConfigResourceBudget common;  // tags 64..76
  bool operator==(const NativeAgentConfig_memory_governor_policy& o) const noexcept {
    return definition_uuid == o.definition_uuid &&
           origin_transaction_uuid.kind == o.origin_transaction_uuid.kind &&
           origin_transaction_uuid.value == o.origin_transaction_uuid.value &&
           origin_local_transaction_id == o.origin_local_transaction_id &&
           emergency_reserve_percent == o.emergency_reserve_percent &&
           allocation_failure_pressure == o.allocation_failure_pressure &&
           spill_threshold == o.spill_threshold &&
           cache_shrink_limit == o.cache_shrink_limit &&
           common == o.common;
  }
};
// index_health_policy (code 5): schema 65646, max 353 bytes.
struct NativeAgentConfig_index_health_policy {
  static constexpr CatalogAgentPolicyFamily family = CatalogAgentPolicyFamily::index_health_policy;
  static constexpr u32 schema_id = 65646;
  static constexpr std::size_t max_bytes = 353;
  Uuid definition_uuid;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id = 0;
  u64 rebuild_threshold = 0;  // tag 4
  u64 split_pressure_threshold = 0;  // tag 5
  u64 unused_index_window = 0;  // tag 6
  u64 recommendation_cooldown = 0;  // tag 7
  NativeAgentConfigResourceBudget common;  // tags 64..76
  bool operator==(const NativeAgentConfig_index_health_policy& o) const noexcept {
    return definition_uuid == o.definition_uuid &&
           origin_transaction_uuid.kind == o.origin_transaction_uuid.kind &&
           origin_transaction_uuid.value == o.origin_transaction_uuid.value &&
           origin_local_transaction_id == o.origin_local_transaction_id &&
           rebuild_threshold == o.rebuild_threshold &&
           split_pressure_threshold == o.split_pressure_threshold &&
           unused_index_window == o.unused_index_window &&
           recommendation_cooldown == o.recommendation_cooldown &&
           common == o.common;
  }
};
// cluster_autoscale_policy (code 6): schema 65647, max 346 bytes.
struct NativeAgentConfig_cluster_autoscale_policy {
  static constexpr CatalogAgentPolicyFamily family = CatalogAgentPolicyFamily::cluster_autoscale_policy;
  static constexpr u32 schema_id = 65647;
  static constexpr std::size_t max_bytes = 346;
  Uuid definition_uuid;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id = 0;
  u64 scale_up_threshold = 0;  // tag 4
  u64 scale_down_threshold = 0;  // tag 5
  u64 cooldown_seconds = 0;  // tag 6
  bool no_scale_down_when_limbo = false;  // tag 7
  NativeAgentConfigResourceBudget common;  // tags 64..76
  bool operator==(const NativeAgentConfig_cluster_autoscale_policy& o) const noexcept {
    return definition_uuid == o.definition_uuid &&
           origin_transaction_uuid.kind == o.origin_transaction_uuid.kind &&
           origin_transaction_uuid.value == o.origin_transaction_uuid.value &&
           origin_local_transaction_id == o.origin_local_transaction_id &&
           scale_up_threshold == o.scale_up_threshold &&
           scale_down_threshold == o.scale_down_threshold &&
           cooldown_seconds == o.cooldown_seconds &&
           no_scale_down_when_limbo == o.no_scale_down_when_limbo &&
           common == o.common;
  }
};
// admission_control_policy (code 7): schema 65648, max 337 bytes.
struct NativeAgentConfig_admission_control_policy {
  static constexpr CatalogAgentPolicyFamily family = CatalogAgentPolicyFamily::admission_control_policy;
  static constexpr u32 schema_id = 65648;
  static constexpr std::size_t max_bytes = 337;
  Uuid definition_uuid;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id = 0;
  u64 listener_queue_threshold = 0;  // tag 4
  u64 scheduler_queue_threshold = 0;  // tag 5
  u64 hard_limit_threshold = 0;  // tag 6
  // tag 7 workload_class_rules (token_list): free text, NOT carried (unrepresented).
  NativeAgentConfigResourceBudget common;  // tags 64..76
  bool operator==(const NativeAgentConfig_admission_control_policy& o) const noexcept {
    return definition_uuid == o.definition_uuid &&
           origin_transaction_uuid.kind == o.origin_transaction_uuid.kind &&
           origin_transaction_uuid.value == o.origin_transaction_uuid.value &&
           origin_local_transaction_id == o.origin_local_transaction_id &&
           listener_queue_threshold == o.listener_queue_threshold &&
           scheduler_queue_threshold == o.scheduler_queue_threshold &&
           hard_limit_threshold == o.hard_limit_threshold &&
           common == o.common;
  }
};
// parser_health_policy (code 8): schema 65649, max 337 bytes.
struct NativeAgentConfig_parser_health_policy {
  static constexpr CatalogAgentPolicyFamily family = CatalogAgentPolicyFamily::parser_health_policy;
  static constexpr u32 schema_id = 65649;
  static constexpr std::size_t max_bytes = 337;
  Uuid definition_uuid;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id = 0;
  u64 parser_crash_quarantine_threshold = 0;  // tag 4
  u64 drain_cooldown = 0;  // tag 5
  // tag 6 package_quarantine_rules (token_list): free text, NOT carried (unrepresented).
  u64 policy_attach_latency_warn_us = 0;  // tag 7
  NativeAgentConfigResourceBudget common;  // tags 64..76
  bool operator==(const NativeAgentConfig_parser_health_policy& o) const noexcept {
    return definition_uuid == o.definition_uuid &&
           origin_transaction_uuid.kind == o.origin_transaction_uuid.kind &&
           origin_transaction_uuid.value == o.origin_transaction_uuid.value &&
           origin_local_transaction_id == o.origin_local_transaction_id &&
           parser_crash_quarantine_threshold == o.parser_crash_quarantine_threshold &&
           drain_cooldown == o.drain_cooldown &&
           policy_attach_latency_warn_us == o.policy_attach_latency_warn_us &&
           common == o.common;
  }
};
// long_transaction_policy (code 9): schema 65650, max 346 bytes.
struct NativeAgentConfig_long_transaction_policy {
  static constexpr CatalogAgentPolicyFamily family = CatalogAgentPolicyFamily::long_transaction_policy;
  static constexpr u32 schema_id = 65650;
  static constexpr std::size_t max_bytes = 346;
  Uuid definition_uuid;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id = 0;
  u64 oldest_snapshot_pressure_seconds = 0;  // tag 4
  u64 idle_warning_seconds = 0;  // tag 5
  u64 blocker_escalation_seconds = 0;  // tag 6
  bool cancel_requires_approval = false;  // tag 7
  NativeAgentConfigResourceBudget common;  // tags 64..76
  bool operator==(const NativeAgentConfig_long_transaction_policy& o) const noexcept {
    return definition_uuid == o.definition_uuid &&
           origin_transaction_uuid.kind == o.origin_transaction_uuid.kind &&
           origin_transaction_uuid.value == o.origin_transaction_uuid.value &&
           origin_local_transaction_id == o.origin_local_transaction_id &&
           oldest_snapshot_pressure_seconds == o.oldest_snapshot_pressure_seconds &&
           idle_warning_seconds == o.idle_warning_seconds &&
           blocker_escalation_seconds == o.blocker_escalation_seconds &&
           cancel_requires_approval == o.cancel_requires_approval &&
           common == o.common;
  }
};
// storage_version_cleanup_policy (code 10): schema 65651, max 362 bytes.
struct NativeAgentConfig_storage_version_cleanup_policy {
  static constexpr CatalogAgentPolicyFamily family = CatalogAgentPolicyFamily::storage_version_cleanup_policy;
  static constexpr u32 schema_id = 65651;
  static constexpr std::size_t max_bytes = 362;
  Uuid definition_uuid;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id = 0;
  bool authoritative_cleanup_horizon_required = false;  // tag 4
  u64 cleanup_horizon = 0;  // tag 5
  u64 max_versions_per_batch = 0;  // tag 6
  u64 retained_row_pressure = 0;  // tag 7
  u64 active_blocker_threshold = 0;  // tag 8
  NativeAgentConfigResourceBudget common;  // tags 64..76
  bool operator==(const NativeAgentConfig_storage_version_cleanup_policy& o) const noexcept {
    return definition_uuid == o.definition_uuid &&
           origin_transaction_uuid.kind == o.origin_transaction_uuid.kind &&
           origin_transaction_uuid.value == o.origin_transaction_uuid.value &&
           origin_local_transaction_id == o.origin_local_transaction_id &&
           authoritative_cleanup_horizon_required == o.authoritative_cleanup_horizon_required &&
           cleanup_horizon == o.cleanup_horizon &&
           max_versions_per_batch == o.max_versions_per_batch &&
           retained_row_pressure == o.retained_row_pressure &&
           active_blocker_threshold == o.active_blocker_threshold &&
           common == o.common;
  }
};
// cleanup_archive_policy (code 11): schema 65652, max 339 bytes.
struct NativeAgentConfig_cleanup_archive_policy {
  static constexpr CatalogAgentPolicyFamily family = CatalogAgentPolicyFamily::cleanup_archive_policy;
  static constexpr u32 schema_id = 65652;
  static constexpr std::size_t max_bytes = 339;
  Uuid definition_uuid;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id = 0;
  u64 cleanup_blocked_threshold = 0;  // tag 4
  u64 archive_slice_max_age_us = 0;  // tag 5
  bool authoritative_lwm_required = false;  // tag 6
  bool verify_before_release = false;  // tag 7
  NativeAgentConfigResourceBudget common;  // tags 64..76
  bool operator==(const NativeAgentConfig_cleanup_archive_policy& o) const noexcept {
    return definition_uuid == o.definition_uuid &&
           origin_transaction_uuid.kind == o.origin_transaction_uuid.kind &&
           origin_transaction_uuid.value == o.origin_transaction_uuid.value &&
           origin_local_transaction_id == o.origin_local_transaction_id &&
           cleanup_blocked_threshold == o.cleanup_blocked_threshold &&
           archive_slice_max_age_us == o.archive_slice_max_age_us &&
           authoritative_lwm_required == o.authoritative_lwm_required &&
           verify_before_release == o.verify_before_release &&
           common == o.common;
  }
};
// policy_recommendation_policy (code 12): schema 65653, max 346 bytes.
struct NativeAgentConfig_policy_recommendation_policy {
  static constexpr CatalogAgentPolicyFamily family = CatalogAgentPolicyFamily::policy_recommendation_policy;
  static constexpr u32 schema_id = 65653;
  static constexpr std::size_t max_bytes = 346;
  Uuid definition_uuid;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id = 0;
  double recommendation_burn_rate = 0;  // tag 4
  double confidence_threshold = 0;  // tag 5
  u64 cooldown_seconds = 0;  // tag 6
  bool no_auto_apply = false;  // tag 7
  NativeAgentConfigResourceBudget common;  // tags 64..76
  bool operator==(const NativeAgentConfig_policy_recommendation_policy& o) const noexcept {
    return definition_uuid == o.definition_uuid &&
           origin_transaction_uuid.kind == o.origin_transaction_uuid.kind &&
           origin_transaction_uuid.value == o.origin_transaction_uuid.value &&
           origin_local_transaction_id == o.origin_local_transaction_id &&
           recommendation_burn_rate == o.recommendation_burn_rate &&
           confidence_threshold == o.confidence_threshold &&
           cooldown_seconds == o.cooldown_seconds &&
           no_auto_apply == o.no_auto_apply &&
           common == o.common;
  }
};
// distributed_query_policy (code 13): schema 65654, max 337 bytes.
struct NativeAgentConfig_distributed_query_policy {
  static constexpr CatalogAgentPolicyFamily family = CatalogAgentPolicyFamily::distributed_query_policy;
  static constexpr u32 schema_id = 65654;
  static constexpr std::size_t max_bytes = 337;
  Uuid definition_uuid;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id = 0;
  u64 fragment_delay_warn_us = 0;  // tag 4
  u64 queue_sample_interval = 0;  // tag 5
  u64 remote_fragment_trace_level = 0;  // tag 6
  NativeAgentConfigResourceBudget common;  // tags 64..76
  bool operator==(const NativeAgentConfig_distributed_query_policy& o) const noexcept {
    return definition_uuid == o.definition_uuid &&
           origin_transaction_uuid.kind == o.origin_transaction_uuid.kind &&
           origin_transaction_uuid.value == o.origin_transaction_uuid.value &&
           origin_local_transaction_id == o.origin_local_transaction_id &&
           fragment_delay_warn_us == o.fragment_delay_warn_us &&
           queue_sample_interval == o.queue_sample_interval &&
           remote_fragment_trace_level == o.remote_fragment_trace_level &&
           common == o.common;
  }
};
// remote_routing_policy (code 14): schema 65655, max 332 bytes.
struct NativeAgentConfig_remote_routing_policy {
  static constexpr CatalogAgentPolicyFamily family = CatalogAgentPolicyFamily::remote_routing_policy;
  static constexpr u32 schema_id = 65655;
  static constexpr std::size_t max_bytes = 332;
  Uuid definition_uuid;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id = 0;
  double remote_route_latency_weight = 0;  // tag 4
  bool stale_route_refusal = false;  // tag 5
  bool local_fallback_required = false;  // tag 6
  bool fence_required = false;  // tag 7
  NativeAgentConfigResourceBudget common;  // tags 64..76
  bool operator==(const NativeAgentConfig_remote_routing_policy& o) const noexcept {
    return definition_uuid == o.definition_uuid &&
           origin_transaction_uuid.kind == o.origin_transaction_uuid.kind &&
           origin_transaction_uuid.value == o.origin_transaction_uuid.value &&
           origin_local_transaction_id == o.origin_local_transaction_id &&
           remote_route_latency_weight == o.remote_route_latency_weight &&
           stale_route_refusal == o.stale_route_refusal &&
           local_fallback_required == o.local_fallback_required &&
           fence_required == o.fence_required &&
           common == o.common;
  }
};
// optimizer_learning_policy (code 15): schema 65656, max 346 bytes.
struct NativeAgentConfig_optimizer_learning_policy {
  static constexpr CatalogAgentPolicyFamily family = CatalogAgentPolicyFamily::optimizer_learning_policy;
  static constexpr u32 schema_id = 65656;
  static constexpr std::size_t max_bytes = 346;
  Uuid definition_uuid;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id = 0;
  double correction_threshold = 0;  // tag 4
  double confidence_floor = 0;  // tag 5
  u64 max_correction_age = 0;  // tag 6
  bool no_semantic_override = false;  // tag 7
  NativeAgentConfigResourceBudget common;  // tags 64..76
  bool operator==(const NativeAgentConfig_optimizer_learning_policy& o) const noexcept {
    return definition_uuid == o.definition_uuid &&
           origin_transaction_uuid.kind == o.origin_transaction_uuid.kind &&
           origin_transaction_uuid.value == o.origin_transaction_uuid.value &&
           origin_local_transaction_id == o.origin_local_transaction_id &&
           correction_threshold == o.correction_threshold &&
           confidence_floor == o.confidence_floor &&
           max_correction_age == o.max_correction_age &&
           no_semantic_override == o.no_semantic_override &&
           common == o.common;
  }
};
// support_bundle_policy (code 16): schema 65657, max 332 bytes.
struct NativeAgentConfig_support_bundle_policy {
  static constexpr CatalogAgentPolicyFamily family = CatalogAgentPolicyFamily::support_bundle_policy;
  static constexpr u32 schema_id = 65657;
  static constexpr std::size_t max_bytes = 332;
  Uuid definition_uuid;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id = 0;
  bool completeness_required = false;  // tag 4
  bool redaction_required = false;  // tag 5
  u64 bundle_expiry = 0;  // tag 6
  bool secret_classes_forbidden = false;  // tag 7
  NativeAgentConfigResourceBudget common;  // tags 64..76
  bool operator==(const NativeAgentConfig_support_bundle_policy& o) const noexcept {
    return definition_uuid == o.definition_uuid &&
           origin_transaction_uuid.kind == o.origin_transaction_uuid.kind &&
           origin_transaction_uuid.value == o.origin_transaction_uuid.value &&
           origin_local_transaction_id == o.origin_local_transaction_id &&
           completeness_required == o.completeness_required &&
           redaction_required == o.redaction_required &&
           bundle_expiry == o.bundle_expiry &&
           secret_classes_forbidden == o.secret_classes_forbidden &&
           common == o.common;
  }
};
// cluster_scheduler_policy (code 17): schema 65658, max 339 bytes.
struct NativeAgentConfig_cluster_scheduler_policy {
  static constexpr CatalogAgentPolicyFamily family = CatalogAgentPolicyFamily::cluster_scheduler_policy;
  static constexpr u32 schema_id = 65658;
  static constexpr std::size_t max_bytes = 339;
  Uuid definition_uuid;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id = 0;
  u64 scheduler_queue_threshold = 0;  // tag 4
  double placement_score_floor = 0;  // tag 5
  bool fence_required = false;  // tag 6
  bool keep_queued_on_uncertainty = false;  // tag 7
  NativeAgentConfigResourceBudget common;  // tags 64..76
  bool operator==(const NativeAgentConfig_cluster_scheduler_policy& o) const noexcept {
    return definition_uuid == o.definition_uuid &&
           origin_transaction_uuid.kind == o.origin_transaction_uuid.kind &&
           origin_transaction_uuid.value == o.origin_transaction_uuid.value &&
           origin_local_transaction_id == o.origin_local_transaction_id &&
           scheduler_queue_threshold == o.scheduler_queue_threshold &&
           placement_score_floor == o.placement_score_floor &&
           fence_required == o.fence_required &&
           keep_queued_on_uncertainty == o.keep_queued_on_uncertainty &&
           common == o.common;
  }
};
// job_control_policy (code 18): schema 65659, max 339 bytes.
struct NativeAgentConfig_job_control_policy {
  static constexpr CatalogAgentPolicyFamily family = CatalogAgentPolicyFamily::job_control_policy;
  static constexpr u32 schema_id = 65659;
  static constexpr std::size_t max_bytes = 339;
  Uuid definition_uuid;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id = 0;
  bool cancel_requires_owner_or_admin = false;  // tag 4
  u64 retry_cooldown = 0;  // tag 5
  u64 suppression_max_duration = 0;  // tag 6
  bool audit_required = false;  // tag 7
  NativeAgentConfigResourceBudget common;  // tags 64..76
  bool operator==(const NativeAgentConfig_job_control_policy& o) const noexcept {
    return definition_uuid == o.definition_uuid &&
           origin_transaction_uuid.kind == o.origin_transaction_uuid.kind &&
           origin_transaction_uuid.value == o.origin_transaction_uuid.value &&
           origin_local_transaction_id == o.origin_local_transaction_id &&
           cancel_requires_owner_or_admin == o.cancel_requires_owner_or_admin &&
           retry_cooldown == o.retry_cooldown &&
           suppression_max_duration == o.suppression_max_duration &&
           audit_required == o.audit_required &&
           common == o.common;
  }
};
// backup_policy (code 19): schema 65660, max 323 bytes.
struct NativeAgentConfig_backup_policy {
  static constexpr CatalogAgentPolicyFamily family = CatalogAgentPolicyFamily::backup_policy;
  static constexpr u32 schema_id = 65660;
  static constexpr std::size_t max_bytes = 323;
  Uuid definition_uuid;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id = 0;
  u64 stuck_progress_window = 0;  // tag 4
  bool verification_required = false;  // tag 5
  // tag 6 blocker_policy (token): free text, NOT carried (unrepresented).
  bool cancel_requires_approval = false;  // tag 7
  NativeAgentConfigResourceBudget common;  // tags 64..76
  bool operator==(const NativeAgentConfig_backup_policy& o) const noexcept {
    return definition_uuid == o.definition_uuid &&
           origin_transaction_uuid.kind == o.origin_transaction_uuid.kind &&
           origin_transaction_uuid.value == o.origin_transaction_uuid.value &&
           origin_local_transaction_id == o.origin_local_transaction_id &&
           stuck_progress_window == o.stuck_progress_window &&
           verification_required == o.verification_required &&
           cancel_requires_approval == o.cancel_requires_approval &&
           common == o.common;
  }
};
// archive_policy (code 20): schema 65661, max 339 bytes.
struct NativeAgentConfig_archive_policy {
  static constexpr CatalogAgentPolicyFamily family = CatalogAgentPolicyFamily::archive_policy;
  static constexpr u32 schema_id = 65661;
  static constexpr std::size_t max_bytes = 339;
  Uuid definition_uuid;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id = 0;
  u64 archive_queue_pressure = 0;  // tag 4
  u64 slice_max_age = 0;  // tag 5
  bool verification_required = false;  // tag 6
  bool protected_slice_delete_forbidden = false;  // tag 7
  NativeAgentConfigResourceBudget common;  // tags 64..76
  bool operator==(const NativeAgentConfig_archive_policy& o) const noexcept {
    return definition_uuid == o.definition_uuid &&
           origin_transaction_uuid.kind == o.origin_transaction_uuid.kind &&
           origin_transaction_uuid.value == o.origin_transaction_uuid.value &&
           origin_local_transaction_id == o.origin_local_transaction_id &&
           archive_queue_pressure == o.archive_queue_pressure &&
           slice_max_age == o.slice_max_age &&
           verification_required == o.verification_required &&
           protected_slice_delete_forbidden == o.protected_slice_delete_forbidden &&
           common == o.common;
  }
};
// restore_drill_policy (code 21): schema 65662, max 332 bytes.
struct NativeAgentConfig_restore_drill_policy {
  static constexpr CatalogAgentPolicyFamily family = CatalogAgentPolicyFamily::restore_drill_policy;
  static constexpr u32 schema_id = 65662;
  static constexpr std::size_t max_bytes = 332;
  Uuid definition_uuid;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id = 0;
  bool isolated_target_required = false;  // tag 4
  u64 max_duration_us = 0;  // tag 5
  bool verification_required = false;  // tag 6
  bool source_mutation_forbidden = false;  // tag 7
  NativeAgentConfigResourceBudget common;  // tags 64..76
  bool operator==(const NativeAgentConfig_restore_drill_policy& o) const noexcept {
    return definition_uuid == o.definition_uuid &&
           origin_transaction_uuid.kind == o.origin_transaction_uuid.kind &&
           origin_transaction_uuid.value == o.origin_transaction_uuid.value &&
           origin_local_transaction_id == o.origin_local_transaction_id &&
           isolated_target_required == o.isolated_target_required &&
           max_duration_us == o.max_duration_us &&
           verification_required == o.verification_required &&
           source_mutation_forbidden == o.source_mutation_forbidden &&
           common == o.common;
  }
};
// pitr_policy (code 22): schema 65663, max 346 bytes.
struct NativeAgentConfig_pitr_policy {
  static constexpr CatalogAgentPolicyFamily family = CatalogAgentPolicyFamily::pitr_policy;
  static constexpr u32 schema_id = 65663;
  static constexpr std::size_t max_bytes = 346;
  Uuid definition_uuid;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id = 0;
  u64 minimum_pitr_window_seconds = 0;  // tag 4
  u64 estimate_refresh_interval = 0;  // tag 5
  bool restore_plan_requires_approval = false;  // tag 6
  u64 archive_freshness_limit = 0;  // tag 7
  NativeAgentConfigResourceBudget common;  // tags 64..76
  bool operator==(const NativeAgentConfig_pitr_policy& o) const noexcept {
    return definition_uuid == o.definition_uuid &&
           origin_transaction_uuid.kind == o.origin_transaction_uuid.kind &&
           origin_transaction_uuid.value == o.origin_transaction_uuid.value &&
           origin_local_transaction_id == o.origin_local_transaction_id &&
           minimum_pitr_window_seconds == o.minimum_pitr_window_seconds &&
           estimate_refresh_interval == o.estimate_refresh_interval &&
           restore_plan_requires_approval == o.restore_plan_requires_approval &&
           archive_freshness_limit == o.archive_freshness_limit &&
           common == o.common;
  }
};
// identity_lifecycle_policy (code 23): schema 65664, max 307 bytes.
struct NativeAgentConfig_identity_lifecycle_policy {
  static constexpr CatalogAgentPolicyFamily family = CatalogAgentPolicyFamily::identity_lifecycle_policy;
  static constexpr u32 schema_id = 65664;
  static constexpr std::size_t max_bytes = 307;
  Uuid definition_uuid;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id = 0;
  // tag 4 auth_anomaly_policy (token): free text, NOT carried (unrepresented).
  bool lock_requires_sec_right = false;  // tag 5
  bool evidence_required = false;  // tag 6
  // tag 7 redaction_class (token): free text, NOT carried (unrepresented).
  NativeAgentConfigResourceBudget common;  // tags 64..76
  bool operator==(const NativeAgentConfig_identity_lifecycle_policy& o) const noexcept {
    return definition_uuid == o.definition_uuid &&
           origin_transaction_uuid.kind == o.origin_transaction_uuid.kind &&
           origin_transaction_uuid.value == o.origin_transaction_uuid.value &&
           origin_local_transaction_id == o.origin_local_transaction_id &&
           lock_requires_sec_right == o.lock_requires_sec_right &&
           evidence_required == o.evidence_required &&
           common == o.common;
  }
};
// session_control_policy (code 24): schema 65665, max 323 bytes.
struct NativeAgentConfig_session_control_policy {
  static constexpr CatalogAgentPolicyFamily family = CatalogAgentPolicyFamily::session_control_policy;
  static constexpr u32 schema_id = 65665;
  static constexpr std::size_t max_bytes = 323;
  Uuid definition_uuid;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id = 0;
  bool disconnect_requires_right = false;  // tag 4
  u64 reauth_cooldown = 0;  // tag 5
  bool revoke_requires_sec_right = false;  // tag 6
  // tag 7 self_view_redaction (token): free text, NOT carried (unrepresented).
  NativeAgentConfigResourceBudget common;  // tags 64..76
  bool operator==(const NativeAgentConfig_session_control_policy& o) const noexcept {
    return definition_uuid == o.definition_uuid &&
           origin_transaction_uuid.kind == o.origin_transaction_uuid.kind &&
           origin_transaction_uuid.value == o.origin_transaction_uuid.value &&
           origin_local_transaction_id == o.origin_local_transaction_id &&
           disconnect_requires_right == o.disconnect_requires_right &&
           reauth_cooldown == o.reauth_cooldown &&
           revoke_requires_sec_right == o.revoke_requires_sec_right &&
           common == o.common;
  }
};
// alerting_baseline (code 25): schema 65666, max 330 bytes.
struct NativeAgentConfig_alerting_baseline {
  static constexpr CatalogAgentPolicyFamily family = CatalogAgentPolicyFamily::alerting_baseline;
  static constexpr u32 schema_id = 65666;
  static constexpr std::size_t max_bytes = 330;
  Uuid definition_uuid;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id = 0;
  u64 dedupe_window = 0;  // tag 4
  // tag 5 grouping_rules (token_list): free text, NOT carried (unrepresented).
  u64 silence_max_duration = 0;  // tag 6
  bool critical_silence_requires_override = false;  // tag 7
  NativeAgentConfigResourceBudget common;  // tags 64..76
  bool operator==(const NativeAgentConfig_alerting_baseline& o) const noexcept {
    return definition_uuid == o.definition_uuid &&
           origin_transaction_uuid.kind == o.origin_transaction_uuid.kind &&
           origin_transaction_uuid.value == o.origin_transaction_uuid.value &&
           origin_local_transaction_id == o.origin_local_transaction_id &&
           dedupe_window == o.dedupe_window &&
           silence_max_duration == o.silence_max_duration &&
           critical_silence_requires_override == o.critical_silence_requires_override &&
           common == o.common;
  }
};
// export_default_baseline (code 26): schema 65667, max 332 bytes.
struct NativeAgentConfig_export_default_baseline {
  static constexpr CatalogAgentPolicyFamily family = CatalogAgentPolicyFamily::export_default_baseline;
  static constexpr u32 schema_id = 65667;
  static constexpr std::size_t max_bytes = 332;
  Uuid definition_uuid;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id = 0;
  bool residency_required = false;  // tag 4
  bool redaction_required = false;  // tag 5
  u64 queue_backpressure = 0;  // tag 6
  bool adapter_health_required = false;  // tag 7
  NativeAgentConfigResourceBudget common;  // tags 64..76
  bool operator==(const NativeAgentConfig_export_default_baseline& o) const noexcept {
    return definition_uuid == o.definition_uuid &&
           origin_transaction_uuid.kind == o.origin_transaction_uuid.kind &&
           origin_transaction_uuid.value == o.origin_transaction_uuid.value &&
           origin_local_transaction_id == o.origin_local_transaction_id &&
           residency_required == o.residency_required &&
           redaction_required == o.redaction_required &&
           queue_backpressure == o.queue_backpressure &&
           adapter_health_required == o.adapter_health_required &&
           common == o.common;
  }
};
// upgrade_policy (code 27): schema 65668, max 316 bytes.
struct NativeAgentConfig_upgrade_policy {
  static constexpr CatalogAgentPolicyFamily family = CatalogAgentPolicyFamily::upgrade_policy;
  static constexpr u32 schema_id = 65668;
  static constexpr std::size_t max_bytes = 316;
  Uuid definition_uuid;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id = 0;
  bool readiness_required = false;  // tag 4
  bool fence_required = false;  // tag 5
  // tag 6 rolling_step_order (token_list): free text, NOT carried (unrepresented).
  // tag 7 rollback_gate (token): free text, NOT carried (unrepresented).
  bool version_compatibility_required = false;  // tag 8
  NativeAgentConfigResourceBudget common;  // tags 64..76
  bool operator==(const NativeAgentConfig_upgrade_policy& o) const noexcept {
    return definition_uuid == o.definition_uuid &&
           origin_transaction_uuid.kind == o.origin_transaction_uuid.kind &&
           origin_transaction_uuid.value == o.origin_transaction_uuid.value &&
           origin_local_transaction_id == o.origin_local_transaction_id &&
           readiness_required == o.readiness_required &&
           fence_required == o.fence_required &&
           version_compatibility_required == o.version_compatibility_required &&
           common == o.common;
  }
};
// agent_coordinator_policy config.startup_verification_class: allowed values in schema-map order.
enum class NativeAgentConfigEnum_agent_coordinator_policy_startup_verification_class : u16 {
  none = 0,
  startup_light_verify = 1,
  startup_full_verify = 2,
};
inline constexpr std::array<CatalogAgentTagEntry<NativeAgentConfigEnum_agent_coordinator_policy_startup_verification_class>, 2> kNativeAgentConfigEnum_agent_coordinator_policy_startup_verification_classTags{{
    {NativeAgentConfigEnum_agent_coordinator_policy_startup_verification_class::startup_light_verify, "startup_light_verify"},
    {NativeAgentConfigEnum_agent_coordinator_policy_startup_verification_class::startup_full_verify, "startup_full_verify"}}};
constexpr std::optional<NativeAgentConfigEnum_agent_coordinator_policy_startup_verification_class> NativeAgentConfigEnum_agent_coordinator_policy_startup_verification_classForTag(std::string_view tag) noexcept {
  return CatalogAgentTagLookup(kNativeAgentConfigEnum_agent_coordinator_policy_startup_verification_classTags, tag);
}
// agent_coordinator_policy (code 28): schema 65669, max 530 bytes.
struct NativeAgentConfig_agent_coordinator_policy {
  static constexpr CatalogAgentPolicyFamily family = CatalogAgentPolicyFamily::agent_coordinator_policy;
  static constexpr u32 schema_id = 65669;
  static constexpr std::size_t max_bytes = 530;
  Uuid definition_uuid;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id = 0;
  bool startup_launch_enabled = false;  // tag 4
  NativeAgentConfigEnum_agent_coordinator_policy_startup_verification_class startup_verification_class = NativeAgentConfigEnum_agent_coordinator_policy_startup_verification_class::none;  // tag 5
  u64 startup_clock_max_backward_ns = 0;  // tag 6
  u64 startup_clock_max_forward_jump_ns = 0;  // tag 7
  bool startup_clock_fail_on_rollback = false;  // tag 8
  bool startup_clock_fail_on_monotonic_regression = false;  // tag 9
  bool startup_clock_fail_on_forward_jump = false;  // tag 10
  u64 startup_uuid_max_regression_ms = 0;  // tag 11
  u64 startup_uuid_same_ms_wait_timeout_ms = 0;  // tag 12
  bool go_no_go_required = false;  // tag 13
  NativeAgentLayerList managed_layers;  // tag 14
  bool worker_launch_forbidden = false;  // tag 15
  u64 max_agents_per_cycle = 0;  // tag 16
  u64 max_schedule_queue_depth = 0;  // tag 17
  u64 schedule_interval_microseconds = 0;  // tag 18
  CatalogAgentType startup_agent = CatalogAgentType::none;  // tag 19
  CatalogAgentType shutdown_agent = CatalogAgentType::none;  // tag 20
  bool coordinator_stops_last = false;  // tag 21
  NativeAgentConfigResourceBudget common;  // tags 64..76
  bool operator==(const NativeAgentConfig_agent_coordinator_policy& o) const noexcept {
    return definition_uuid == o.definition_uuid &&
           origin_transaction_uuid.kind == o.origin_transaction_uuid.kind &&
           origin_transaction_uuid.value == o.origin_transaction_uuid.value &&
           origin_local_transaction_id == o.origin_local_transaction_id &&
           startup_launch_enabled == o.startup_launch_enabled &&
           startup_verification_class == o.startup_verification_class &&
           startup_clock_max_backward_ns == o.startup_clock_max_backward_ns &&
           startup_clock_max_forward_jump_ns == o.startup_clock_max_forward_jump_ns &&
           startup_clock_fail_on_rollback == o.startup_clock_fail_on_rollback &&
           startup_clock_fail_on_monotonic_regression == o.startup_clock_fail_on_monotonic_regression &&
           startup_clock_fail_on_forward_jump == o.startup_clock_fail_on_forward_jump &&
           startup_uuid_max_regression_ms == o.startup_uuid_max_regression_ms &&
           startup_uuid_same_ms_wait_timeout_ms == o.startup_uuid_same_ms_wait_timeout_ms &&
           go_no_go_required == o.go_no_go_required &&
           managed_layers == o.managed_layers &&
           worker_launch_forbidden == o.worker_launch_forbidden &&
           max_agents_per_cycle == o.max_agents_per_cycle &&
           max_schedule_queue_depth == o.max_schedule_queue_depth &&
           schedule_interval_microseconds == o.schedule_interval_microseconds &&
           startup_agent == o.startup_agent &&
           shutdown_agent == o.shutdown_agent &&
           coordinator_stops_last == o.coordinator_stops_last &&
           common == o.common;
  }
};
// agent_startup_policy (code 29): schema 65670, max 445 bytes.
struct NativeAgentConfig_agent_startup_policy {
  static constexpr CatalogAgentPolicyFamily family = CatalogAgentPolicyFamily::agent_startup_policy;
  static constexpr u32 schema_id = 65670;
  static constexpr std::size_t max_bytes = 445;
  Uuid definition_uuid;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id = 0;
  CatalogAgentLaunchTrigger launch_trigger = CatalogAgentLaunchTrigger::none;  // tag 4
  bool engine_verification_required = false;  // tag 5
  bool committed_policy_required = false;  // tag 6
  bool child_readiness_required = false;  // tag 7
  bool configured_components_only = false;  // tag 8
  NativeAgentTypeList dependency_order;  // tag 9
  u64 readiness_timeout_microseconds = 0;  // tag 10
  u64 max_retry_attempts = 0;  // tag 11
  NativeAgentConfigResourceBudget common;  // tags 64..76
  bool operator==(const NativeAgentConfig_agent_startup_policy& o) const noexcept {
    return definition_uuid == o.definition_uuid &&
           origin_transaction_uuid.kind == o.origin_transaction_uuid.kind &&
           origin_transaction_uuid.value == o.origin_transaction_uuid.value &&
           origin_local_transaction_id == o.origin_local_transaction_id &&
           launch_trigger == o.launch_trigger &&
           engine_verification_required == o.engine_verification_required &&
           committed_policy_required == o.committed_policy_required &&
           child_readiness_required == o.child_readiness_required &&
           configured_components_only == o.configured_components_only &&
           dependency_order == o.dependency_order &&
           readiness_timeout_microseconds == o.readiness_timeout_microseconds &&
           max_retry_attempts == o.max_retry_attempts &&
           common == o.common;
  }
};
// agent_shutdown_policy (code 30): schema 65671, max 366 bytes.
struct NativeAgentConfig_agent_shutdown_policy {
  static constexpr CatalogAgentPolicyFamily family = CatalogAgentPolicyFamily::agent_shutdown_policy;
  static constexpr u32 schema_id = 65671;
  static constexpr std::size_t max_bytes = 366;
  Uuid definition_uuid;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id = 0;
  CatalogAgentLaunchTrigger launch_trigger = CatalogAgentLaunchTrigger::none;  // tag 4
  bool withdraw_admission_first = false;  // tag 5
  bool reverse_dependency_drain = false;  // tag 6
  bool drain_observations_before_recorders = false;  // tag 7
  bool engine_final_receipt_required = false;  // tag 8
  bool cooperative_cancellation_only = false;  // tag 9
  u64 drain_timeout_microseconds = 0;  // tag 10
  NativeAgentConfigResourceBudget common;  // tags 64..76
  bool operator==(const NativeAgentConfig_agent_shutdown_policy& o) const noexcept {
    return definition_uuid == o.definition_uuid &&
           origin_transaction_uuid.kind == o.origin_transaction_uuid.kind &&
           origin_transaction_uuid.value == o.origin_transaction_uuid.value &&
           origin_local_transaction_id == o.origin_local_transaction_id &&
           launch_trigger == o.launch_trigger &&
           withdraw_admission_first == o.withdraw_admission_first &&
           reverse_dependency_drain == o.reverse_dependency_drain &&
           drain_observations_before_recorders == o.drain_observations_before_recorders &&
           engine_final_receipt_required == o.engine_final_receipt_required &&
           cooperative_cancellation_only == o.cooperative_cancellation_only &&
           drain_timeout_microseconds == o.drain_timeout_microseconds &&
           common == o.common;
  }
};
// coordinator_policy (code 31): schema 65672, max 394 bytes.
struct NativeAgentConfig_coordinator_policy {
  static constexpr CatalogAgentPolicyFamily family = CatalogAgentPolicyFamily::coordinator_policy;
  static constexpr u32 schema_id = 65672;
  static constexpr std::size_t max_bytes = 394;
  Uuid definition_uuid;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id = 0;
  u64 heartbeat_interval_microseconds = 0;  // tag 4
  u64 coordinator_log_poll_interval_microseconds = 0;  // tag 5
  bool zero_row_short_circuit = false;  // tag 6
  u64 cardinality_surprise_threshold_pct = 0;  // tag 7
  u64 cardinality_surprise_repeat_window_microseconds = 0;  // tag 8
  u64 max_observation_queue_depth = 0;  // tag 9
  u64 max_active_leg_workers = 0;  // tag 10
  NativeAgentConfigResourceBudget common;  // tags 64..76
  bool operator==(const NativeAgentConfig_coordinator_policy& o) const noexcept {
    return definition_uuid == o.definition_uuid &&
           origin_transaction_uuid.kind == o.origin_transaction_uuid.kind &&
           origin_transaction_uuid.value == o.origin_transaction_uuid.value &&
           origin_local_transaction_id == o.origin_local_transaction_id &&
           heartbeat_interval_microseconds == o.heartbeat_interval_microseconds &&
           coordinator_log_poll_interval_microseconds == o.coordinator_log_poll_interval_microseconds &&
           zero_row_short_circuit == o.zero_row_short_circuit &&
           cardinality_surprise_threshold_pct == o.cardinality_surprise_threshold_pct &&
           cardinality_surprise_repeat_window_microseconds == o.cardinality_surprise_repeat_window_microseconds &&
           max_observation_queue_depth == o.max_observation_queue_depth &&
           max_active_leg_workers == o.max_active_leg_workers &&
           common == o.common;
  }
};
// cleanup_cycle_policy (code 32): schema 65673, max 355 bytes.
struct NativeAgentConfig_cleanup_cycle_policy {
  static constexpr CatalogAgentPolicyFamily family = CatalogAgentPolicyFamily::cleanup_cycle_policy;
  static constexpr u32 schema_id = 65673;
  static constexpr std::size_t max_bytes = 355;
  Uuid definition_uuid;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id = 0;
  u64 coordinator_liveness_window_microseconds = 0;  // tag 4
  u64 max_absences_per_sweep = 0;  // tag 5
  u64 orphaned_leg_kill_timeout_microseconds = 0;  // tag 6
  bool write_absence_evidence_required = false;  // tag 7
  bool degrade_on_stale_log = false;  // tag 8
  NativeAgentConfigResourceBudget common;  // tags 64..76
  bool operator==(const NativeAgentConfig_cleanup_cycle_policy& o) const noexcept {
    return definition_uuid == o.definition_uuid &&
           origin_transaction_uuid.kind == o.origin_transaction_uuid.kind &&
           origin_transaction_uuid.value == o.origin_transaction_uuid.value &&
           origin_local_transaction_id == o.origin_local_transaction_id &&
           coordinator_liveness_window_microseconds == o.coordinator_liveness_window_microseconds &&
           max_absences_per_sweep == o.max_absences_per_sweep &&
           orphaned_leg_kill_timeout_microseconds == o.orphaned_leg_kill_timeout_microseconds &&
           write_absence_evidence_required == o.write_absence_evidence_required &&
           degrade_on_stale_log == o.degrade_on_stale_log &&
           common == o.common;
  }
};
// filespace_capacity_policy (code 33): schema 65674, max 437 bytes.
struct NativeAgentConfig_filespace_capacity_policy {
  static constexpr CatalogAgentPolicyFamily family = CatalogAgentPolicyFamily::filespace_capacity_policy;
  static constexpr u32 schema_id = 65674;
  static constexpr std::size_t max_bytes = 437;
  Uuid definition_uuid;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id = 0;
  u64 minimum_free_bytes = 0;  // tag 4
  u64 minimum_free_percent = 0;  // tag 5
  u64 growth_rate_window_seconds = 0;  // tag 6
  u64 depletion_eta_warning_seconds = 0;  // tag 7
  u64 depletion_eta_action_seconds = 0;  // tag 8
  bool expand_allowed = false;  // tag 9
  bool move_allowed = false;  // tag 10
  bool shrink_allowed = false;  // tag 11
  bool truncate_allowed = false;  // tag 12
  u64 critical_device_error_threshold = 0;  // tag 13
  u64 fsync_p99_critical_us = 0;  // tag 14
  // tag 15 required_approval_class (token): free text, NOT carried (unrepresented).
  NativeAgentConfigResourceBudget common;  // tags 64..76
  bool operator==(const NativeAgentConfig_filespace_capacity_policy& o) const noexcept {
    return definition_uuid == o.definition_uuid &&
           origin_transaction_uuid.kind == o.origin_transaction_uuid.kind &&
           origin_transaction_uuid.value == o.origin_transaction_uuid.value &&
           origin_local_transaction_id == o.origin_local_transaction_id &&
           minimum_free_bytes == o.minimum_free_bytes &&
           minimum_free_percent == o.minimum_free_percent &&
           growth_rate_window_seconds == o.growth_rate_window_seconds &&
           depletion_eta_warning_seconds == o.depletion_eta_warning_seconds &&
           depletion_eta_action_seconds == o.depletion_eta_action_seconds &&
           expand_allowed == o.expand_allowed &&
           move_allowed == o.move_allowed &&
           shrink_allowed == o.shrink_allowed &&
           truncate_allowed == o.truncate_allowed &&
           critical_device_error_threshold == o.critical_device_error_threshold &&
           fsync_p99_critical_us == o.fsync_p99_critical_us &&
           common == o.common;
  }
};
// filespace_shadow_promotion_policy (code 34): schema 65675, max 332 bytes.
struct NativeAgentConfig_filespace_shadow_promotion_policy {
  static constexpr CatalogAgentPolicyFamily family = CatalogAgentPolicyFamily::filespace_shadow_promotion_policy;
  static constexpr u32 schema_id = 65675;
  static constexpr std::size_t max_bytes = 332;
  Uuid definition_uuid;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id = 0;
  u64 primary_degradation_threshold = 0;  // tag 4
  bool candidate_readiness_required = false;  // tag 5
  bool catalog_persistence_move_required = false;  // tag 6
  bool operator_approval_required = false;  // tag 7
  NativeAgentConfigResourceBudget common;  // tags 64..76
  bool operator==(const NativeAgentConfig_filespace_shadow_promotion_policy& o) const noexcept {
    return definition_uuid == o.definition_uuid &&
           origin_transaction_uuid.kind == o.origin_transaction_uuid.kind &&
           origin_transaction_uuid.value == o.origin_transaction_uuid.value &&
           origin_local_transaction_id == o.origin_local_transaction_id &&
           primary_degradation_threshold == o.primary_degradation_threshold &&
           candidate_readiness_required == o.candidate_readiness_required &&
           catalog_persistence_move_required == o.catalog_persistence_move_required &&
           operator_approval_required == o.operator_approval_required &&
           common == o.common;
  }
};
// page_preallocation_policy (code 35): schema 65676, max 362 bytes.
struct NativeAgentConfig_page_preallocation_policy {
  static constexpr CatalogAgentPolicyFamily family = CatalogAgentPolicyFamily::page_preallocation_policy;
  static constexpr u32 schema_id = 65676;
  static constexpr std::size_t max_bytes = 362;
  Uuid definition_uuid;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id = 0;
  bool preallocation_allowed = false;  // tag 4
  u64 forecast_window_seconds = 0;  // tag 5
  double history_confidence_threshold = 0;  // tag 6
  u64 max_reserved_bytes_per_filespace = 0;  // tag 7
  // tag 8 allowed_page_families (token_list): free text, NOT carried (unrepresented).
  // tag 9 workload_calendar (token_list): free text, NOT carried (unrepresented).
  u64 allocation_throttle_pages_per_second = 0;  // tag 10
  NativeAgentConfigResourceBudget common;  // tags 64..76
  bool operator==(const NativeAgentConfig_page_preallocation_policy& o) const noexcept {
    return definition_uuid == o.definition_uuid &&
           origin_transaction_uuid.kind == o.origin_transaction_uuid.kind &&
           origin_transaction_uuid.value == o.origin_transaction_uuid.value &&
           origin_local_transaction_id == o.origin_local_transaction_id &&
           preallocation_allowed == o.preallocation_allowed &&
           forecast_window_seconds == o.forecast_window_seconds &&
           history_confidence_threshold == o.history_confidence_threshold &&
           max_reserved_bytes_per_filespace == o.max_reserved_bytes_per_filespace &&
           allocation_throttle_pages_per_second == o.allocation_throttle_pages_per_second &&
           common == o.common;
  }
};
// page_relocation_policy (code 36): schema 65677, max 375 bytes.
struct NativeAgentConfig_page_relocation_policy {
  static constexpr CatalogAgentPolicyFamily family = CatalogAgentPolicyFamily::page_relocation_policy;
  static constexpr u32 schema_id = 65677;
  static constexpr std::size_t max_bytes = 375;
  Uuid definition_uuid;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id = 0;
  bool relocation_allowed = false;  // tag 4
  bool defragment_allowed = false;  // tag 5
  bool publish_shrink_ready_allowed = false;  // tag 6
  u64 max_pages_per_interval = 0;  // tag 7
  u64 interval_seconds = 0;  // tag 8
  // tag 9 unmovable_blocker_classes (token_list): free text, NOT carried (unrepresented).
  bool require_checksum_valid = false;  // tag 10
  bool require_transaction_holds_clear = false;  // tag 11
  bool require_backup_archive_holds_clear = false;  // tag 12
  NativeAgentConfigResourceBudget common;  // tags 64..76
  bool operator==(const NativeAgentConfig_page_relocation_policy& o) const noexcept {
    return definition_uuid == o.definition_uuid &&
           origin_transaction_uuid.kind == o.origin_transaction_uuid.kind &&
           origin_transaction_uuid.value == o.origin_transaction_uuid.value &&
           origin_local_transaction_id == o.origin_local_transaction_id &&
           relocation_allowed == o.relocation_allowed &&
           defragment_allowed == o.defragment_allowed &&
           publish_shrink_ready_allowed == o.publish_shrink_ready_allowed &&
           max_pages_per_interval == o.max_pages_per_interval &&
           interval_seconds == o.interval_seconds &&
           require_checksum_valid == o.require_checksum_valid &&
           require_transaction_holds_clear == o.require_transaction_holds_clear &&
           require_backup_archive_holds_clear == o.require_backup_archive_holds_clear &&
           common == o.common;
  }
};
// Calls f(std::type_identity<NativeAgentConfig_<family>>{}) for a listed
// family; false for any other value.
template <class F>
bool WithNativeAgentConfigType(CatalogAgentPolicyFamily family, F&& f) {
  switch (family) {
    case CatalogAgentPolicyFamily::node_resource_policy: f(std::type_identity<NativeAgentConfig_node_resource_policy>{}); return true;
    case CatalogAgentPolicyFamily::metric_registry_policy: f(std::type_identity<NativeAgentConfig_metric_registry_policy>{}); return true;
    case CatalogAgentPolicyFamily::storage_health_policy: f(std::type_identity<NativeAgentConfig_storage_health_policy>{}); return true;
    case CatalogAgentPolicyFamily::memory_governor_policy: f(std::type_identity<NativeAgentConfig_memory_governor_policy>{}); return true;
    case CatalogAgentPolicyFamily::index_health_policy: f(std::type_identity<NativeAgentConfig_index_health_policy>{}); return true;
    case CatalogAgentPolicyFamily::cluster_autoscale_policy: f(std::type_identity<NativeAgentConfig_cluster_autoscale_policy>{}); return true;
    case CatalogAgentPolicyFamily::admission_control_policy: f(std::type_identity<NativeAgentConfig_admission_control_policy>{}); return true;
    case CatalogAgentPolicyFamily::parser_health_policy: f(std::type_identity<NativeAgentConfig_parser_health_policy>{}); return true;
    case CatalogAgentPolicyFamily::long_transaction_policy: f(std::type_identity<NativeAgentConfig_long_transaction_policy>{}); return true;
    case CatalogAgentPolicyFamily::storage_version_cleanup_policy: f(std::type_identity<NativeAgentConfig_storage_version_cleanup_policy>{}); return true;
    case CatalogAgentPolicyFamily::cleanup_archive_policy: f(std::type_identity<NativeAgentConfig_cleanup_archive_policy>{}); return true;
    case CatalogAgentPolicyFamily::policy_recommendation_policy: f(std::type_identity<NativeAgentConfig_policy_recommendation_policy>{}); return true;
    case CatalogAgentPolicyFamily::distributed_query_policy: f(std::type_identity<NativeAgentConfig_distributed_query_policy>{}); return true;
    case CatalogAgentPolicyFamily::remote_routing_policy: f(std::type_identity<NativeAgentConfig_remote_routing_policy>{}); return true;
    case CatalogAgentPolicyFamily::optimizer_learning_policy: f(std::type_identity<NativeAgentConfig_optimizer_learning_policy>{}); return true;
    case CatalogAgentPolicyFamily::support_bundle_policy: f(std::type_identity<NativeAgentConfig_support_bundle_policy>{}); return true;
    case CatalogAgentPolicyFamily::cluster_scheduler_policy: f(std::type_identity<NativeAgentConfig_cluster_scheduler_policy>{}); return true;
    case CatalogAgentPolicyFamily::job_control_policy: f(std::type_identity<NativeAgentConfig_job_control_policy>{}); return true;
    case CatalogAgentPolicyFamily::backup_policy: f(std::type_identity<NativeAgentConfig_backup_policy>{}); return true;
    case CatalogAgentPolicyFamily::archive_policy: f(std::type_identity<NativeAgentConfig_archive_policy>{}); return true;
    case CatalogAgentPolicyFamily::restore_drill_policy: f(std::type_identity<NativeAgentConfig_restore_drill_policy>{}); return true;
    case CatalogAgentPolicyFamily::pitr_policy: f(std::type_identity<NativeAgentConfig_pitr_policy>{}); return true;
    case CatalogAgentPolicyFamily::identity_lifecycle_policy: f(std::type_identity<NativeAgentConfig_identity_lifecycle_policy>{}); return true;
    case CatalogAgentPolicyFamily::session_control_policy: f(std::type_identity<NativeAgentConfig_session_control_policy>{}); return true;
    case CatalogAgentPolicyFamily::alerting_baseline: f(std::type_identity<NativeAgentConfig_alerting_baseline>{}); return true;
    case CatalogAgentPolicyFamily::export_default_baseline: f(std::type_identity<NativeAgentConfig_export_default_baseline>{}); return true;
    case CatalogAgentPolicyFamily::upgrade_policy: f(std::type_identity<NativeAgentConfig_upgrade_policy>{}); return true;
    case CatalogAgentPolicyFamily::agent_coordinator_policy: f(std::type_identity<NativeAgentConfig_agent_coordinator_policy>{}); return true;
    case CatalogAgentPolicyFamily::agent_startup_policy: f(std::type_identity<NativeAgentConfig_agent_startup_policy>{}); return true;
    case CatalogAgentPolicyFamily::agent_shutdown_policy: f(std::type_identity<NativeAgentConfig_agent_shutdown_policy>{}); return true;
    case CatalogAgentPolicyFamily::coordinator_policy: f(std::type_identity<NativeAgentConfig_coordinator_policy>{}); return true;
    case CatalogAgentPolicyFamily::cleanup_cycle_policy: f(std::type_identity<NativeAgentConfig_cleanup_cycle_policy>{}); return true;
    case CatalogAgentPolicyFamily::filespace_capacity_policy: f(std::type_identity<NativeAgentConfig_filespace_capacity_policy>{}); return true;
    case CatalogAgentPolicyFamily::filespace_shadow_promotion_policy: f(std::type_identity<NativeAgentConfig_filespace_shadow_promotion_policy>{}); return true;
    case CatalogAgentPolicyFamily::page_preallocation_policy: f(std::type_identity<NativeAgentConfig_page_preallocation_policy>{}); return true;
    case CatalogAgentPolicyFamily::page_relocation_policy: f(std::type_identity<NativeAgentConfig_page_relocation_policy>{}); return true;
    default: return false;
  }
}
// ---- end GENERATED: header ----

// The family fields (schema-map order, unrepresented ones included) and the
// common optional fields.
std::span<const NativeAgentConfigFieldDescriptor> NativeAgentConfigFieldsFor(CatalogAgentPolicyFamily) noexcept;
std::span<const NativeAgentConfigFieldDescriptor> NativeAgentConfigCommonFields() noexcept;
// Keys a family declares whose value is free text with no enumeration or
// identifier meaning: accepted by the boundary adapter, NOT carried. Pending
// an owner ruling; see the 3c-2 report.
std::span<const std::string_view> NativeAgentConfigUnrepresentedKeys(CatalogAgentPolicyFamily) noexcept;

// Config codec, one instantiation per NativeAgentConfig_<family>.
template <class R> std::optional<std::size_t> NativeAgentConfigEncodedBytes(const R&);
template <class R> CatalogValueEncodeIntoResult EncodeNativeAgentConfigInto(const R&, std::span<byte>);
template <class R> CatalogValueEncodeResult EncodeNativeAgentConfig(const R&);
template <class R> NativeAgentDecodeResult<R> DecodeNativeAgentConfig(std::string_view);
// Config boundary adapter: keys without the "config." prefix. Every family
// field is required exactly once; common fields are optional; unknown or
// repeated keys are refused. Unrepresented keys are accepted and dropped.
// Identity and origin fields are left for the caller; not validated.
template <class R> NativeAgentTagResult<R> NativeAgentConfigFromTags(std::span<const NativeAgentPropertyTag>);
}  // namespace scratchbird::core::catalog
