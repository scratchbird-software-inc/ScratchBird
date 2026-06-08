// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/page_allocation_runtime_bridge.hpp"

#include "agent_runtime.hpp"
#include "agents/filespace_capacity_manager.hpp"
#include "agents/page_allocation_manager.hpp"
#include "api_diagnostics.hpp"
#include "page_allocation_lifecycle.hpp"
#include "strict_bulk_load_lifecycle.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

namespace agents = scratchbird::core::agents::implemented_agents;
namespace agent_runtime = scratchbird::core::agents;
namespace bulk = scratchbird::core::bulk_load;
namespace page = scratchbird::storage::page;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

constexpr const char* kRuntimeEnabledOption = "page_allocation.runtime=enabled";
constexpr const char* kFreePagesOption = "page_allocation.free_pages=";
constexpr const char* kDataPreallocatePagesOption = "page_allocation.preallocate_data_pages=";
constexpr const char* kIndexPreallocatePagesOption = "page_allocation.preallocate_index_pages=";
constexpr const char* kRowPagesOption = "page_allocation.row_pages_per_mutation=";
constexpr const char* kIndexPagesOption = "page_allocation.index_pages_per_mutation=";
constexpr const char* kDemandHintsOption = "dml_demand_hints";
constexpr const char* kDemandMaxPagesOption = "dml_demand_hints.max_pages=";
constexpr const char* kDemandCapacityPagesOption = "dml_demand_hints.available_capacity_pages=";
constexpr const char* kDemandMinimumFreePagesOption = "dml_demand_hints.minimum_free_pages=";
constexpr const char* kDemandTargetFreePagesOption = "dml_demand_hints.target_free_pages=";

struct RuntimeLedger {
  page::PageAllocationLedger ledger;
  page::PageFilespaceAgentRequestQueue agent_queue;
  platform::TypedUuid filespace_uuid;
  platform::TypedUuid policy_uuid;
  platform::TypedUuid capacity_evidence_uuid;
  platform::u64 next_start_page = 1000;
  platform::u64 next_demand_sequence = 1;
  std::vector<std::string> applied_capacity_tokens;
};

struct DemandRuntimeOutcome {
  bool requested = false;
  bool granted = false;
  bool capped = false;
  bool refused = false;
  platform::u64 requested_pages = 0;
  platform::u64 granted_pages = 0;
};

std::mutex& RuntimeMutex() {
  static std::mutex mutex;
  return mutex;
}

std::map<std::string, RuntimeLedger>& RuntimeLedgers() {
  static std::map<std::string, RuntimeLedger> ledgers;
  return ledgers;
}

bool HasOption(const std::vector<std::string>& options, const std::string& option) {
  const auto equals = option.find('=');
  const std::string colon_option = equals == std::string::npos
                                       ? std::string{}
                                       : option.substr(0, equals) + ":" + option.substr(equals + 1);
  for (const auto& candidate : options) {
    if (candidate == option || (!colon_option.empty() && candidate == colon_option)) {
      return true;
    }
  }
  return false;
}

std::string OptionValue(const std::vector<std::string>& options, const std::string& prefix) {
  for (const auto& option : options) {
    if (option.rfind(prefix, 0) == 0) {
      return option.substr(prefix.size());
    }
    if (!prefix.empty() && prefix.back() == '=') {
      const std::string colon_prefix = prefix.substr(0, prefix.size() - 1) + ":";
      if (option.rfind(colon_prefix, 0) == 0) {
        return option.substr(colon_prefix.size());
      }
    }
  }
  return {};
}

std::string OptionAssignmentValue(const std::vector<std::string>& options,
                                  const std::string& key) {
  const std::string equals_prefix = key + "=";
  const std::string colon_prefix = key + ":";
  for (const auto& option : options) {
    if (option.rfind(equals_prefix, 0) == 0) {
      return option.substr(equals_prefix.size());
    }
    if (option.rfind(colon_prefix, 0) == 0) {
      return option.substr(colon_prefix.size());
    }
  }
  return {};
}

bool IsTruthyOptionValue(std::string value) {
  for (char& ch : value) {
    if (ch >= 'A' && ch <= 'Z') {
      ch = static_cast<char>(ch - 'A' + 'a');
    }
  }
  return value == "1" || value == "true" || value == "enabled" || value == "on";
}

bool IsFalsyOptionValue(std::string value) {
  for (char& ch : value) {
    if (ch >= 'A' && ch <= 'Z') {
      ch = static_cast<char>(ch - 'A' + 'a');
    }
  }
  return value == "0" || value == "false" || value == "disabled" || value == "off";
}

platform::u64 ParseU64Option(const std::vector<std::string>& options,
                             const std::string& prefix,
                             platform::u64 fallback) {
  const auto value = OptionValue(options, prefix);
  if (value.empty()) {
    return fallback;
  }
  try {
    return static_cast<platform::u64>(std::stoull(value));
  } catch (...) {
    return fallback;
  }
}

bool DemandHintOptionPresent(const std::vector<std::string>& option_envelopes) {
  return !OptionAssignmentValue(option_envelopes, kDemandHintsOption).empty() ||
         !OptionValue(option_envelopes, kDemandMaxPagesOption).empty() ||
         !OptionValue(option_envelopes, kDemandCapacityPagesOption).empty();
}

bool DemandHintsEnabled(const std::vector<std::string>& option_envelopes) {
  const std::string configured = OptionAssignmentValue(option_envelopes, kDemandHintsOption);
  if (IsFalsyOptionValue(configured)) {
    return false;
  }
  if (IsTruthyOptionValue(configured)) {
    return true;
  }
  return DemandHintOptionPresent(option_envelopes);
}

std::string LedgerKey(const EngineRequestContext& context) {
  const std::string identity = context.database_uuid.canonical.empty()
                                   ? std::string("database_uuid_absent")
                                   : context.database_uuid.canonical;
  if (!context.database_path.empty()) {
    return context.database_path + "|" + identity;
  }
  return identity;
}

platform::u64 StableSeed(const std::string& value, platform::u64 salt) {
  platform::u64 hash = 1469598103934665603ull ^ salt;
  for (const unsigned char c : value) {
    hash ^= static_cast<platform::u64>(c);
    hash *= 1099511628211ull;
  }
  return 1800000000000ull + (hash % 1000000000ull);
}

platform::TypedUuid GeneratedIdentity(platform::UuidKind kind,
                                      const std::string& key,
                                      platform::u64 salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, StableSeed(key, salt));
  return generated.ok() ? generated.value : platform::TypedUuid{};
}

bool IsEngineIdentity(const platform::TypedUuid& typed, platform::UuidKind kind) {
  return typed.kind == kind && typed.valid() && uuid::IsEngineIdentityUuid(typed.value);
}

platform::TypedUuid ParseEngineIdentity(platform::UuidKind kind, const std::string& text) {
  const auto parsed = uuid::ParseTypedUuid(kind, text);
  if (!parsed.ok() || !IsEngineIdentity(parsed.value, kind)) {
    return {};
  }
  return parsed.value;
}

bool RuntimeCanActivate(const EngineRequestContext& context,
                        const std::vector<std::string>& option_envelopes,
                        const std::string& owner_object_uuid,
                        platform::TypedUuid* database_uuid,
                        platform::TypedUuid* transaction_uuid,
                        platform::TypedUuid* owner_uuid) {
  if (!HasOption(option_envelopes, kRuntimeEnabledOption) ||
      context.local_transaction_id == 0 ||
      LedgerKey(context).empty() ||
      database_uuid == nullptr ||
      transaction_uuid == nullptr ||
      owner_uuid == nullptr) {
    return false;
  }
  *database_uuid = ParseEngineIdentity(platform::UuidKind::database,
                                       context.database_uuid.canonical);
  *transaction_uuid = ParseEngineIdentity(platform::UuidKind::transaction,
                                          context.transaction_uuid.canonical);
  *owner_uuid = ParseEngineIdentity(platform::UuidKind::object, owner_object_uuid);
  return IsEngineIdentity(*database_uuid, platform::UuidKind::database) &&
         IsEngineIdentity(*transaction_uuid, platform::UuidKind::transaction) &&
         IsEngineIdentity(*owner_uuid, platform::UuidKind::object);
}

std::string UuidText(const platform::TypedUuid& typed) {
  return typed.valid() ? uuid::UuidToString(typed.value) : std::string{};
}

void AddEvidence(std::vector<EngineEvidenceReference>* evidence,
                 std::string kind,
                 std::string id) {
  if (evidence != nullptr) {
    evidence->push_back({std::move(kind), std::move(id)});
  }
}

const agent_runtime::AgentWorkerCapacityAssignment* FindWorkerAssignment(
    const agent_runtime::AgentWorkerCapacitySnapshot& snapshot,
    const std::string& agent_type_id) {
  for (const auto& assignment : snapshot.assignments) {
    if (assignment.agent_type_id == agent_type_id) {
      return &assignment;
    }
  }
  return nullptr;
}

agent_runtime::AgentRuntimeContext WorkerCapacityContext(
    const EngineRequestContext& context) {
  agent_runtime::AgentRuntimeContext runtime_context;
  runtime_context.security_context_present = context.security_context_present;
  runtime_context.private_features_available = true;
  runtime_context.standalone_edition = true;
  runtime_context.cluster_authority_available = false;
  runtime_context.database_uuid = context.database_uuid.canonical;
  runtime_context.principal_uuid = context.principal_uuid.canonical;
  runtime_context.rights = {
      "OBS_AGENT_STATE_READ",
      "OBS_AGENT_CONTROL",
      "OBS_AGENT_EVIDENCE_READ"};
  runtime_context.monotonic_now_microseconds =
      context.local_transaction_id == 0
          ? 1
          : context.local_transaction_id * 1000;
  return runtime_context;
}

void AddWorkerCapacityPlanningEvidence(
    const EngineRequestContext& context,
    const std::vector<std::string>& option_envelopes,
    std::vector<EngineEvidenceReference>* evidence) {
  agent_runtime::AgentWorkerCapacityConfig config;
  config.observed_cpu_count = ParseU64Option(
      option_envelopes,
      "page_allocation.agent_worker_observed_cpu_count=",
      5);
  config.configured_cpu_count = ParseU64Option(
      option_envelopes,
      "page_allocation.agent_worker_configured_cpu_count=",
      config.observed_cpu_count);
  config.foreground_reserved_capacity = ParseU64Option(
      option_envelopes,
      "page_allocation.agent_worker_foreground_reserved=",
      1);
  config.max_background_worker_slots = ParseU64Option(
      option_envelopes,
      "page_allocation.agent_worker_max_background_slots=",
      4);
  config.foreground_database_work_active = false;
  config.standalone_edition = true;
  config.cluster_authority_available = false;

  const auto snapshot = agent_runtime::PlanAgentWorkerCapacity(
      config,
      WorkerCapacityContext(context),
      agent_runtime::DefaultDmlPreworkAgentWorkerCandidates(1));
  AddEvidence(evidence,
              "agent_worker_capacity_planning_snapshot",
              snapshot.status.ok ? "ok" : snapshot.status.diagnostic_code);
  AddEvidence(evidence,
              "agent_worker_capacity_planning_background_slots",
              std::to_string(snapshot.background_worker_slots));
  const auto* page =
      FindWorkerAssignment(snapshot, "page_allocation_manager");
  const auto* filespace =
      FindWorkerAssignment(snapshot, "filespace_capacity_manager");
  if (page != nullptr) {
    AddEvidence(evidence,
                "page_allocator_worker_capacity_planned_diagnostic",
                page->diagnostic_code);
    AddEvidence(evidence,
                "page_allocator_worker_capacity_planned_slot",
                std::to_string(page->worker_slot_index));
    AddEvidence(evidence,
                "page_allocator_worker_capacity_planned_can_precede_foreground",
                page->can_run_before_foreground_demand ? "true" : "false");
  }
  if (filespace != nullptr) {
    AddEvidence(evidence,
                "filespace_capacity_worker_capacity_planned_diagnostic",
                filespace->diagnostic_code);
    AddEvidence(evidence,
                "filespace_capacity_worker_capacity_planned_slot",
                std::to_string(filespace->worker_slot_index));
    AddEvidence(evidence,
                "filespace_capacity_worker_capacity_planned_can_precede_foreground",
                filespace->can_run_before_foreground_demand ? "true" : "false");
  }
  const bool separate_workers =
      page != nullptr && filespace != nullptr && page->assigned &&
      filespace->assigned &&
      page->worker_slot_index != filespace->worker_slot_index;
  const bool ahead_of_need =
      separate_workers && page->can_run_before_foreground_demand &&
      filespace->can_run_before_foreground_demand;
  AddEvidence(evidence,
              "page_filespace_worker_capacity_planned_separate_slots",
              separate_workers ? "true" : "false");
  AddEvidence(evidence,
              "page_allocator_worker_capacity_planned_ahead_of_foreground",
              ahead_of_need ? "true" : "false");
  AddEvidence(evidence,
              "page_allocator_worker_runtime_proven",
              "false");
  AddEvidence(evidence,
              "page_allocator_worker_runtime_mode",
              "inline_tick_not_worker_runtime");
  AddEvidence(evidence,
              "filespace_capacity_worker_runtime_mode",
              "inline_tick_not_worker_runtime");
  AddEvidence(evidence,
              "page_allocator_worker_runtime_blocker",
              "SB_ORH_AHEAD_OF_NEED_PAGE_ALLOCATOR.WORKER_RUNTIME_UNPROVEN");
  AddEvidence(evidence,
              "page_allocator_ahead_of_need_runtime",
              "blocked");
  if (!separate_workers || !ahead_of_need) {
    AddEvidence(evidence,
                "page_allocator_worker_capacity_planned_blocker",
                "SB_ORH_AHEAD_OF_NEED_PAGE_ALLOCATOR.WORKER_CAPACITY_UNAVAILABLE");
  }
}

platform::u64 SaturatingAdd(platform::u64 left, platform::u64 right) {
  const platform::u64 max = ~platform::u64{0};
  return max - left < right ? max : left + right;
}

platform::u64 FreeExtentPages(const page::PageAllocationLedger& ledger) {
  platform::u64 pages = 0;
  for (const auto& extent : ledger.free_extents) {
    pages = SaturatingAdd(pages, extent.page_count);
  }
  return pages;
}

platform::u64 AllocationPagesByState(const page::PageAllocationLedger& ledger,
                                     const std::string& page_family,
                                     page::PageAllocationLifecycleState state) {
  platform::u64 pages = 0;
  for (const auto& allocation : ledger.allocations) {
    if (allocation.page_family == page_family && allocation.state == state) {
      pages = SaturatingAdd(pages, allocation.page_count);
    }
  }
  return pages;
}

platform::u64 EffectiveDemandTargetPages(const page::PageAllocationLedger& ledger,
                                         const std::string& page_family,
                                         platform::u64 granted_pages) {
  const platform::u64 preallocated = AllocationPagesByState(
      ledger,
      page_family,
      page::PageAllocationLifecycleState::preallocated);
  return granted_pages > preallocated ? granted_pages - preallocated : 0;
}

EngineApiDiagnostic DiagnosticFromAllocation(const page::PageAllocationResult& result,
                                             const std::string& mutation_phase) {
  std::string detail = mutation_phase;
  for (const auto& argument : result.diagnostic.arguments) {
    if (argument.key == "detail" && !argument.value.empty()) {
      detail += ":" + argument.value;
    }
  }
  return MakeEngineApiDiagnostic(
      result.diagnostic.diagnostic_code.empty()
          ? "SB-STORAGE-PAGE-ALLOCATION-RUNTIME-FAILED"
          : result.diagnostic.diagnostic_code,
      result.diagnostic.message_key.empty()
          ? "storage.page_allocation.runtime_failed"
          : result.diagnostic.message_key,
      std::move(detail),
      !result.ok());
}

std::string CapacityToken(const EngineRequestContext& context,
                          const std::vector<std::string>& option_envelopes) {
  std::ostringstream token;
  token << context.request_id << '|'
        << context.local_transaction_id << '|'
        << OptionValue(option_envelopes, kFreePagesOption) << '|'
        << OptionValue(option_envelopes, kDataPreallocatePagesOption) << '|'
        << OptionValue(option_envelopes, kIndexPreallocatePagesOption);
  return token.str();
}

void AddFreeExtent(RuntimeLedger* runtime, platform::u64 page_count) {
  if (runtime == nullptr || page_count == 0) {
    return;
  }
  runtime->ledger.free_extents.push_back({runtime->next_start_page, page_count});
  runtime->next_start_page += page_count + 16;
}

page::PageAllocationResult Preallocate(RuntimeLedger* runtime,
                                       const platform::TypedUuid& database_uuid,
                                       const platform::TypedUuid& transaction_uuid,
                                       platform::u64 local_transaction_id,
                                       const std::string& page_family,
                                       platform::u64 page_count) {
  AddFreeExtent(runtime, page_count);
  page::PagePreallocationRequest request;
  request.database_uuid = database_uuid;
  request.filespace_uuid = runtime->filespace_uuid;
  request.policy_uuid = runtime->policy_uuid;
  request.capacity_evidence_uuid = runtime->capacity_evidence_uuid;
  request.creator_transaction_uuid = transaction_uuid;
  request.creator_local_transaction_id = local_transaction_id;
  request.page_family = page_family;
  request.page_count = page_count;
  request.page_generation = 1;
  request.engine_authoritative = true;
  request.capacity_evidence_accepted = true;
  request.durability_fence_satisfied = true;
  return page::PreallocatePageFamilyPool(&runtime->ledger, request);
}

page::PageAllocationResult SeedCapacityIfRequested(RuntimeLedger* runtime,
                                                   const EngineRequestContext& context,
                                                   const std::vector<std::string>& option_envelopes,
                                                   const platform::TypedUuid& database_uuid,
                                                   const platform::TypedUuid& transaction_uuid) {
  const auto token = CapacityToken(context, option_envelopes);
  if (std::find(runtime->applied_capacity_tokens.begin(),
                runtime->applied_capacity_tokens.end(),
                token) != runtime->applied_capacity_tokens.end()) {
    page::PageAllocationResult ok;
    ok.admitted = true;
    return ok;
  }
  AddFreeExtent(runtime, ParseU64Option(option_envelopes, kFreePagesOption, 0));

  const auto data_pages = ParseU64Option(option_envelopes, kDataPreallocatePagesOption, 0);
  if (data_pages != 0) {
    auto preallocated = Preallocate(runtime,
                                    database_uuid,
                                    transaction_uuid,
                                    context.local_transaction_id,
                                    "data",
                                    data_pages);
    if (!preallocated.ok()) {
      return preallocated;
    }
  }

  const auto index_pages = ParseU64Option(option_envelopes, kIndexPreallocatePagesOption, 0);
  if (index_pages != 0) {
    auto preallocated = Preallocate(runtime,
                                    database_uuid,
                                    transaction_uuid,
                                    context.local_transaction_id,
                                    "index",
                                    index_pages);
    if (!preallocated.ok()) {
      return preallocated;
    }
  }

  runtime->applied_capacity_tokens.push_back(token);
  page::PageAllocationResult ok;
  ok.admitted = true;
  return ok;
}

RuntimeLedger& EnsureRuntimeLedger(const EngineRequestContext& context,
                                   const platform::TypedUuid& database_uuid) {
  auto& runtime = RuntimeLedgers()[LedgerKey(context)];
  if (!runtime.filespace_uuid.valid()) {
    const auto key = LedgerKey(context);
    runtime.filespace_uuid = GeneratedIdentity(platform::UuidKind::filespace, key, 11);
    runtime.policy_uuid = GeneratedIdentity(platform::UuidKind::object, key, 17);
    runtime.capacity_evidence_uuid = GeneratedIdentity(platform::UuidKind::object, key, 23);
    runtime.ledger.database_uuid = database_uuid;
    runtime.ledger.filespace_uuid = runtime.filespace_uuid;
  }
  return runtime;
}

std::string PageFamilyName(DmlPageAllocationRuntimeFamily family) {
  return family == DmlPageAllocationRuntimeFamily::index ? "index" : "data";
}

bulk::DmlPageFilespaceDemandHintRequest DemandHintRequest(
    RuntimeLedger* runtime,
    const EngineRequestContext& context,
    const platform::TypedUuid& database_uuid,
    const platform::TypedUuid& transaction_uuid,
    const platform::TypedUuid& owner_uuid,
    const std::vector<std::string>& option_envelopes,
    platform::u64 requested_pages,
    const std::string& mutation_phase) {
  bulk::DmlPageFilespaceDemandHintRequest request;
  request.database_uuid = database_uuid;
  request.object_uuid = owner_uuid;
  request.filespace_uuid = runtime == nullptr ? platform::TypedUuid{} : runtime->filespace_uuid;
  request.transaction_uuid = transaction_uuid;
  request.local_transaction_id = context.local_transaction_id;
  request.batch_sequence = runtime == nullptr ? 1 : runtime->next_demand_sequence++;
  request.batch_row_count = requested_pages == 0 ? 1 : requested_pages;
  request.requested_page_count = requested_pages == 0 ? 1 : requested_pages;
  request.max_preallocation_pages = ParseU64Option(option_envelopes, kDemandMaxPagesOption, 0);
  request.enabled = DemandHintsEnabled(option_envelopes);
  request.source = mutation_phase;
  return request;
}

void AddDemandHintRecordEvidence(const bulk::DmlPageFilespaceDemandHintResult& hint,
                                 const std::string& mutation_phase,
                                 std::vector<EngineEvidenceReference>* evidence,
                                 DemandRuntimeOutcome* outcome) {
  if (outcome != nullptr) {
    outcome->requested = true;
    outcome->requested_pages = hint.record.requested_page_count;
    outcome->granted_pages = hint.record.granted_page_count;
    outcome->granted = hint.record.granted_page_count != 0;
    outcome->capped =
        hint.record.decision == bulk::DmlPageFilespaceDemandHintDecision::capped;
    outcome->refused =
        hint.record.decision == bulk::DmlPageFilespaceDemandHintDecision::refused;
  }
  AddEvidence(evidence,
              "dml_demand_hint_decision",
              bulk::DmlPageFilespaceDemandHintDecisionName(hint.record.decision));
  AddEvidence(evidence, "dml_demand_hint_phase", mutation_phase);
  AddEvidence(evidence,
              "dml_demand_hint_requested_pages",
              std::to_string(hint.record.requested_page_count));
  AddEvidence(evidence,
              "dml_demand_hint_granted_pages",
              std::to_string(hint.record.granted_page_count));
  AddEvidence(evidence,
              "dml_demand_hint_max_pages",
              std::to_string(hint.record.max_preallocation_pages));
  if (!hint.diagnostic.diagnostic_code.empty()) {
    AddEvidence(evidence, "dml_demand_hint_diagnostic", hint.diagnostic.diagnostic_code);
  }
}

agents::PageAllocationManagerPolicy DemandPagePolicy(
    const RuntimeLedger& runtime,
    const platform::TypedUuid& database_uuid,
    const std::vector<std::string>& option_envelopes,
    platform::u64 granted_pages,
    bool require_capacity_evidence,
    double low_water_ratio) {
  agents::PageAllocationManagerPolicy policy;
  policy.database_uuid = database_uuid;
  policy.filespace_uuid = runtime.filespace_uuid;
  policy.policy_uuid = runtime.policy_uuid;
  policy.capacity_request_allowed = true;
  policy.capacity_request_policy_explicit = true;
  policy.live_preallocation_allowed = true;
  policy.live_preallocation_policy_explicit = true;
  policy.minimum_free_pages = ParseU64Option(option_envelopes,
                                             kDemandMinimumFreePagesOption,
                                             1);
  policy.target_free_pages = ParseU64Option(option_envelopes,
                                            kDemandTargetFreePagesOption,
                                            std::max<platform::u64>(policy.minimum_free_pages,
                                                                    granted_pages));
  policy.target_free_pages = std::max(policy.minimum_free_pages,
                                      policy.target_free_pages);
  policy.low_water_notify_ratio = low_water_ratio;
  policy.capacity_evidence_required = require_capacity_evidence;
  policy.allowed_page_families = {"data", "index", "overflow", "toast"};
  return policy;
}

agents::PageAllocationManagerMetricSnapshot DemandPageSnapshot(
    const RuntimeLedger& runtime,
    const platform::TypedUuid& database_uuid,
    const std::string& page_family,
    platform::u64 granted_pages,
    platform::u64 free_pages_override,
    bool request_capacity,
    bool request_preallocation) {
  agents::PageAllocationManagerMetricSnapshot snapshot;
  snapshot.database_uuid = database_uuid;
  snapshot.filespace_uuid = runtime.filespace_uuid;
  snapshot.policy_uuid = runtime.policy_uuid;
  snapshot.page_family = page_family;
  snapshot.free_pages = free_pages_override;
  snapshot.preallocated_pages = AllocationPagesByState(
      runtime.ledger,
      page_family,
      page::PageAllocationLifecycleState::preallocated);
  snapshot.allocated_pages = AllocationPagesByState(
      runtime.ledger,
      page_family,
      page::PageAllocationLifecycleState::allocated);
  if (request_capacity) {
    snapshot.target_free_deficit_pages =
        granted_pages > free_pages_override ? granted_pages - free_pages_override : 0;
  }
  if (request_preallocation) {
    snapshot.preallocation_target_pages = granted_pages;
    snapshot.preallocation_deficit_pages =
        EffectiveDemandTargetPages(runtime.ledger, page_family, granted_pages);
  }
  return snapshot;
}

agents::PageAllocationManagerActionContext DemandPageActionContext(
    const EngineRequestContext& context,
    const platform::TypedUuid& transaction_uuid,
    const platform::TypedUuid& capacity_evidence_uuid,
    platform::u64 capacity_evidence_free_pages,
    bool capacity_evidence_present) {
  agents::PageAllocationManagerActionContext action;
  action.present = true;
  action.engine_authoritative = true;
  action.transaction_uuid = transaction_uuid;
  action.local_transaction_id = context.local_transaction_id;
  action.page_generation = 1;
  action.durability_fence_satisfied = true;
  action.capacity_evidence_present = capacity_evidence_present;
  action.capacity_evidence_fresh = capacity_evidence_present;
  action.capacity_evidence_scope_compatible = capacity_evidence_present;
  action.capacity_evidence_uuid = capacity_evidence_uuid;
  action.capacity_evidence_free_pages = capacity_evidence_free_pages;
  action.cluster_route_requested = false;
  return action;
}

agents::FilespaceCapacityManagerMetricSnapshot DemandFilespaceSnapshot(
    const RuntimeLedger& runtime,
    const platform::TypedUuid& database_uuid,
    platform::u64 available_capacity_pages) {
  agents::FilespaceCapacityManagerMetricSnapshot snapshot;
  snapshot.database_uuid = database_uuid;
  snapshot.filespace_uuid = runtime.filespace_uuid;
  snapshot.policy_uuid = runtime.policy_uuid;
  snapshot.free_pages = FreeExtentPages(runtime.ledger);
  snapshot.available_capacity_window_pages = available_capacity_pages;
  snapshot.total_pages = SaturatingAdd(snapshot.free_pages,
                                       available_capacity_pages);
  return snapshot;
}

agents::FilespaceCapacityManagerPolicy DemandFilespacePolicy(
    const RuntimeLedger& runtime,
    const platform::TypedUuid& database_uuid,
    platform::u64 granted_pages,
    platform::u64 available_capacity_pages) {
  agents::FilespaceCapacityManagerPolicy policy;
  policy.database_uuid = database_uuid;
  policy.filespace_uuid = runtime.filespace_uuid;
  policy.policy_uuid = runtime.policy_uuid;
  policy.minimum_free_pages = 1;
  policy.target_free_pages = std::max<platform::u64>(1, granted_pages);
  policy.max_capacity_window_pages = std::max<platform::u64>(1, available_capacity_pages);
  policy.capacity_window_allowed = true;
  policy.capacity_processing_policy_explicit = true;
  policy.expand_allowed = true;
  policy.expand_request_policy_explicit = true;
  return policy;
}

void AddPageAgentTickEvidence(const agents::PageAllocationManagerTickResult& tick,
                              std::vector<EngineEvidenceReference>* evidence) {
  AddEvidence(evidence,
              "page_agent_demand_decision",
              agents::PageAllocationManagerDecisionKindName(tick.decision));
  AddEvidence(evidence,
              "page_agent_demand_requested_pages",
              std::to_string(tick.requested_pages));
  if (!tick.diagnostic.diagnostic_code.empty()) {
    AddEvidence(evidence, "page_agent_demand_diagnostic", tick.diagnostic.diagnostic_code);
  }
  if (tick.capacity_request_enqueued) {
    AddEvidence(evidence,
                "filespace_agent_demand_request",
                UuidText(tick.handoff.request_uuid.valid()
                             ? tick.handoff.request_uuid
                             : tick.handoff.evidence.request_uuid));
    AddEvidence(evidence,
                "filespace_agent_queue_state",
                page::PageFilespaceAgentRequestStateName(
                    tick.handoff.queue_record.request.state));
  }
  if (tick.preallocation_uuid.valid()) {
    AddEvidence(evidence,
                "page_agent_preallocation",
                UuidText(tick.preallocation_uuid));
    AddEvidence(evidence,
                "page_agent_preallocated_pages",
                std::to_string(tick.preallocated_pages));
    AddEvidence(evidence,
                "page_agent_preallocation_source",
                tick.preallocation_evidence.diagnostic_code);
  }
}

void AddFilespaceAgentTickEvidence(
    const agents::FilespaceCapacityManagerTickResult& tick,
    std::vector<EngineEvidenceReference>* evidence) {
  AddEvidence(evidence,
              "filespace_agent_demand_decision",
              agents::FilespaceCapacityManagerDecisionKindName(tick.decision));
  AddEvidence(evidence,
              "filespace_agent_requested_pages",
              std::to_string(tick.requested_pages));
  AddEvidence(evidence,
              "filespace_agent_granted_pages",
              std::to_string(tick.granted_pages));
  if (tick.evidence.evidence_uuid.valid()) {
    AddEvidence(evidence,
                "filespace_agent_capacity_evidence",
                UuidText(tick.evidence.evidence_uuid));
  }
  if (!tick.diagnostic.diagnostic_code.empty()) {
    AddEvidence(evidence, "filespace_agent_demand_diagnostic", tick.diagnostic.diagnostic_code);
  }
  if (!tick.queue_record.request.request_uuid.valid()) {
    return;
  }
  AddEvidence(evidence,
              "filespace_agent_queue_state",
              page::PageFilespaceAgentRequestStateName(tick.queue_record.request.state));
}

std::vector<EngineEvidenceReference> PublishDmlDemandHintLocked(
    RuntimeLedger* runtime,
    const EngineRequestContext& context,
    const platform::TypedUuid& database_uuid,
    const platform::TypedUuid& transaction_uuid,
    const platform::TypedUuid& owner_uuid,
    const std::vector<std::string>& option_envelopes,
    DmlPageAllocationRuntimeFamily family,
    platform::u64 requested_pages,
    const std::string& mutation_phase,
    DemandRuntimeOutcome* outcome) {
  std::vector<EngineEvidenceReference> evidence;
  if (runtime == nullptr || !DemandHintOptionPresent(option_envelopes)) {
    return evidence;
  }
  AddWorkerCapacityPlanningEvidence(context, option_envelopes, &evidence);

  const auto hint = bulk::MakeDmlPageFilespaceDemandHint(
      DemandHintRequest(runtime,
                        context,
                        database_uuid,
                        transaction_uuid,
                        owner_uuid,
                        option_envelopes,
                        requested_pages,
                        mutation_phase));
  AddDemandHintRecordEvidence(hint, mutation_phase, &evidence, outcome);
  if (!hint.ok()) {
    if (outcome != nullptr) {
      outcome->refused = true;
    }
    AddEvidence(&evidence, "dml_demand_runtime_outcome", "not_accepted");
    return evidence;
  }

  const std::string page_family = PageFamilyName(family);
  const platform::u64 granted_pages = hint.record.granted_page_count;
  const platform::u64 free_pages = FreeExtentPages(runtime->ledger);
  const bool capacity_needed = free_pages < granted_pages;
  platform::TypedUuid capacity_evidence_uuid;
  platform::u64 capacity_evidence_pages = 0;

  if (capacity_needed) {
    const platform::u64 available_capacity_pages =
        ParseU64Option(option_envelopes, kDemandCapacityPagesOption, 0);
    const auto capacity_policy = DemandPagePolicy(*runtime,
                                                  database_uuid,
                                                  option_envelopes,
                                                  granted_pages,
                                                  true,
                                                  1.0);
    const auto capacity_snapshot = DemandPageSnapshot(*runtime,
                                                      database_uuid,
                                                      page_family,
                                                      granted_pages,
                                                      free_pages,
                                                      true,
                                                      false);
    const auto queued = agents::EvaluatePageAllocationManagerTick(
        &runtime->agent_queue,
        capacity_snapshot,
        capacity_policy);
    AddEvidence(&evidence,
                "page_allocator_demand_evaluation_mode",
                "inline_tick");
    AddPageAgentTickEvidence(queued, &evidence);
    if (!queued.capacity_request_enqueued) {
      if (outcome != nullptr) {
        outcome->refused = true;
      }
      AddEvidence(&evidence, "dml_demand_runtime_outcome", "capacity_request_refused");
      return evidence;
    }

    const auto filespace = agents::EvaluateFilespaceCapacityManagerTick(
        &runtime->agent_queue,
        DemandFilespaceSnapshot(*runtime, database_uuid, available_capacity_pages),
        DemandFilespacePolicy(*runtime, database_uuid, granted_pages, available_capacity_pages),
        agents::FilespaceCapacityManagerSafetyState{});
    AddEvidence(&evidence,
                "filespace_capacity_demand_evaluation_mode",
                "inline_tick");
    AddFilespaceAgentTickEvidence(filespace, &evidence);
    if (!filespace.approved || filespace.granted_pages == 0) {
      if (outcome != nullptr) {
        outcome->refused = true;
      }
      AddEvidence(&evidence, "dml_demand_runtime_outcome", "capacity_refused");
      return evidence;
    }
    AddFreeExtent(runtime, filespace.granted_pages);
    AddEvidence(&evidence,
                "filespace_runtime_capacity_window_materialized",
                std::to_string(filespace.granted_pages));
    capacity_evidence_uuid = filespace.evidence.evidence_uuid;
    capacity_evidence_pages = filespace.granted_pages;
  }

  const platform::u64 free_pages_after_capacity = FreeExtentPages(runtime->ledger);
  const bool require_capacity_evidence = capacity_needed;
  const auto preallocation_policy = DemandPagePolicy(*runtime,
                                                     database_uuid,
                                                     option_envelopes,
                                                     granted_pages,
                                                     require_capacity_evidence,
                                                     0.0);
  const auto preallocation_snapshot = DemandPageSnapshot(*runtime,
                                                        database_uuid,
                                                        page_family,
                                                        granted_pages,
                                                        free_pages_after_capacity,
                                                        false,
                                                        true);
  const auto preallocated = agents::EvaluatePageAllocationManagerTick(
      &runtime->agent_queue,
      &runtime->ledger,
      preallocation_snapshot,
      preallocation_policy,
      DemandPageActionContext(context,
                              transaction_uuid,
                              capacity_evidence_uuid,
                              capacity_evidence_pages,
                              capacity_needed));
  AddEvidence(&evidence,
              "page_allocator_preallocation_evaluation_mode",
              "inline_tick");
  AddPageAgentTickEvidence(preallocated, &evidence);
  if (outcome != nullptr) {
    outcome->granted = preallocated.preallocated_pages != 0;
    outcome->granted_pages = std::max(outcome->granted_pages,
                                      preallocated.preallocated_pages);
    outcome->refused = outcome->refused ||
                       !(preallocated.preallocation_recommended &&
                         preallocated.accepted_evidence);
  }
  AddEvidence(&evidence,
              "dml_demand_runtime_outcome",
              preallocated.preallocation_recommended && preallocated.accepted_evidence
                  ? "preallocated"
                  : agents::PageAllocationManagerDecisionKindName(preallocated.decision));
  if (preallocated.preallocation_recommended && preallocated.accepted_evidence) {
    AddEvidence(&evidence,
                "page_allocator_inline_tick_consumed_demand",
                "page_allocation_manager");
    AddEvidence(&evidence,
                "filespace_capacity_inline_tick_consumed_demand",
                capacity_needed ? "filespace_capacity_manager"
                                : "capacity_window_not_needed");
    AddEvidence(&evidence,
                "page_allocator_inline_preallocation_before_append",
                "true");
  }
  return evidence;
}

DmlPageAllocationRuntimeResult ReserveRuntimeLocked(
    RuntimeLedger* runtime,
    const EngineRequestContext& context,
    const platform::TypedUuid& database_uuid,
    const platform::TypedUuid& transaction_uuid,
    const platform::TypedUuid& owner_uuid,
    DmlPageAllocationRuntimeFamily family,
    platform::u64 requested_pages,
    const std::string& mutation_phase) {
  DmlPageAllocationRuntimeResult result;
  result.active = true;
  result.requested_pages = requested_pages == 0 ? 1 : requested_pages;
  page::PageAllocationRequest request;
  request.database_uuid = database_uuid;
  request.filespace_uuid = runtime->filespace_uuid;
  request.owner_object_uuid = owner_uuid;
  request.creator_transaction_uuid = transaction_uuid;
  request.creator_local_transaction_id = context.local_transaction_id;
  request.page_family = family == DmlPageAllocationRuntimeFamily::index ? "index" : "data";
  request.page_count = requested_pages == 0 ? 1 : requested_pages;
  request.page_generation = 1;
  request.engine_authoritative = true;
  request.durability_fence_satisfied = true;

  const auto reserved = page::ReservePageAllocation(&runtime->ledger, request);
  result.diagnostic = DiagnosticFromAllocation(reserved, mutation_phase);
  result.evidence.push_back({"page_allocation_runtime", "active"});
  result.evidence.push_back({"page_allocation_action", reserved.evidence.action});
  result.evidence.push_back({"page_allocation_source", reserved.evidence.diagnostic_code});
  result.evidence.push_back({"page_allocation_diagnostic", reserved.evidence.diagnostic_code});
  result.evidence.push_back({"page_allocation", UuidText(reserved.allocation.allocation_uuid)});
  result.evidence.push_back({family == DmlPageAllocationRuntimeFamily::index
                                 ? "index_page_allocation"
                                 : "row_page_allocation",
                             UuidText(reserved.allocation.allocation_uuid)});
  result.evidence.push_back({family == DmlPageAllocationRuntimeFamily::index
                                 ? "index_page_allocation_source"
                                 : "row_page_allocation_source",
                             reserved.evidence.diagnostic_code});
  result.evidence.push_back({"page_allocation_runtime_phase", mutation_phase});
  return result;
}

std::uint64_t IndexPagesForValues(const CrudState& state,
                                  const EngineRequestContext& context,
                                  const std::string& table_uuid,
                                  const std::vector<std::pair<std::string, std::string>>& values,
                                  std::string* first_index_uuid) {
  std::uint64_t pages = 0;
  for (const auto& index : VisibleCrudIndexesForTable(state, table_uuid, context.local_transaction_id)) {
    if (CrudIndexKeysForValues(index, values).empty()) {
      continue;
    }
    if (first_index_uuid != nullptr && first_index_uuid->empty()) {
      *first_index_uuid = index.index_uuid;
    }
    ++pages;
  }
  return pages;
}

std::uint64_t IndexPagesForValueBatch(
    const CrudState& state,
    const EngineRequestContext& context,
    const std::string& table_uuid,
    const std::vector<std::vector<std::pair<std::string, std::string>>>& row_values,
    std::string* first_index_uuid) {
  std::uint64_t pages = 0;
  for (const auto& values : row_values) {
    pages += IndexPagesForValues(state, context, table_uuid, values, first_index_uuid);
  }
  return pages;
}

}  // namespace

DmlPageAllocationRuntimeResult ReserveDmlPageAllocationRuntime(
    const EngineRequestContext& context,
    const std::vector<std::string>& option_envelopes,
    const std::string& owner_object_uuid,
    DmlPageAllocationRuntimeFamily family,
    std::uint64_t requested_pages,
    std::string mutation_phase) {
  DmlPageAllocationRuntimeResult result;
  platform::TypedUuid database_uuid;
  platform::TypedUuid transaction_uuid;
  platform::TypedUuid owner_uuid;
  if (!RuntimeCanActivate(context,
                          option_envelopes,
                          owner_object_uuid,
                          &database_uuid,
                          &transaction_uuid,
                          &owner_uuid)) {
    return result;
  }

  const std::lock_guard<std::mutex> guard(RuntimeMutex());
  RuntimeLedger& runtime = EnsureRuntimeLedger(context, database_uuid);
  const auto seeded = SeedCapacityIfRequested(&runtime,
                                              context,
                                              option_envelopes,
                                              database_uuid,
                                              transaction_uuid);
  if (!seeded.ok()) {
    result.active = true;
    result.diagnostic = DiagnosticFromAllocation(seeded, mutation_phase);
    result.evidence.push_back({"page_allocation_runtime", "active"});
    result.evidence.push_back({"page_allocation_diagnostic", seeded.diagnostic.diagnostic_code});
    result.evidence.push_back({"page_allocation_runtime_phase", mutation_phase});
    return result;
  }

  const auto override_pages =
      family == DmlPageAllocationRuntimeFamily::index
          ? ParseU64Option(option_envelopes, kIndexPagesOption, requested_pages)
          : ParseU64Option(option_envelopes, kRowPagesOption, requested_pages);
  DemandRuntimeOutcome outcome;
  std::vector<EngineEvidenceReference> demand_evidence =
      PublishDmlDemandHintLocked(&runtime,
                                 context,
                                 database_uuid,
                                 transaction_uuid,
                                 owner_uuid,
                                 option_envelopes,
                                 family,
                                 override_pages,
                                 mutation_phase,
                                 &outcome);
  auto reserved = ReserveRuntimeLocked(&runtime,
                                       context,
                                       database_uuid,
                                       transaction_uuid,
                                       owner_uuid,
                                       family,
                                       override_pages,
                                       mutation_phase);
  if (!demand_evidence.empty()) {
    reserved.evidence.insert(reserved.evidence.begin(),
                             demand_evidence.begin(),
                             demand_evidence.end());
  }
  reserved.preallocation_requested = outcome.requested;
  reserved.preallocation_granted = outcome.granted;
  reserved.preallocation_capped = outcome.capped;
  reserved.preallocation_refused = outcome.refused;
  reserved.granted_preallocation_pages = outcome.granted_pages;
  return reserved;
}

DmlPageAllocationRuntimeResult ReserveDmlIndexPageAllocationRuntime(
    const EngineRequestContext& context,
    const std::vector<std::string>& option_envelopes,
    const CrudState& state,
    const std::string& table_uuid,
    const std::vector<std::pair<std::string, std::string>>& values,
    std::string mutation_phase) {
  std::string index_uuid;
  const auto pages = IndexPagesForValues(state, context, table_uuid, values, &index_uuid);
  if (pages == 0 || index_uuid.empty()) {
    return {};
  }
  return ReserveDmlPageAllocationRuntime(context,
                                         option_envelopes,
                                         index_uuid,
                                         DmlPageAllocationRuntimeFamily::index,
                                         pages,
                                         std::move(mutation_phase));
}

DmlPageAllocationRuntimeResult ReserveDmlIndexPageAllocationRuntimeForRows(
    const EngineRequestContext& context,
    const std::vector<std::string>& option_envelopes,
    const CrudState& state,
    const std::string& table_uuid,
    const std::vector<std::vector<std::pair<std::string, std::string>>>& row_values,
    std::string mutation_phase) {
  std::string index_uuid;
  const auto pages = IndexPagesForValueBatch(state, context, table_uuid, row_values, &index_uuid);
  if (pages == 0 || index_uuid.empty()) {
    return {};
  }
  return ReserveDmlPageAllocationRuntime(context,
                                         option_envelopes,
                                         index_uuid,
                                         DmlPageAllocationRuntimeFamily::index,
                                         pages,
                                         std::move(mutation_phase));
}

void AddDmlPageAllocationRuntimeEvidence(const DmlPageAllocationRuntimeResult& allocation,
                                         EngineApiResult* result) {
  if (result == nullptr || !allocation.active) {
    return;
  }
  for (const auto& evidence : allocation.evidence) {
    result->evidence.push_back(evidence);
  }
}

}  // namespace scratchbird::engine::internal_api
