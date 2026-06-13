// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_AGENT_THREAD_RUNTIME

#include "server_agent_runtime.hpp"

#include "agent_runtime.hpp"
#include "agents/agent_action_hooks_api.hpp"
#include "agents/agent_durable_catalog_store_api.hpp"
#include "uuid.hpp"
#include "transaction/transaction_api.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>

#ifndef _WIN32
#include <pthread.h>
#endif

namespace scratchbird::server {
namespace {

namespace engine_api = scratchbird::engine::internal_api;
namespace agents = scratchbird::core::agents;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

std::uint64_t CurrentUnixMillis() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

std::uint64_t CurrentMonotonicNs() {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

constexpr auto kInitialAgentSchedulerDelay = std::chrono::milliseconds(750);
constexpr auto kWarmAgentSchedulerInterval = std::chrono::seconds(1);
constexpr auto kIdleAgentSchedulerInterval = std::chrono::seconds(5);
constexpr std::uint32_t kIdleResidentAgentSlots = 5;
constexpr std::uint64_t kWorkerLeaseDurationMicroseconds = 7200000000;
constexpr std::uint64_t kHeartbeatEveryGenerations = 600;
constexpr std::uint64_t kActionEveryWorkerTicks = 20;

bool WorkerRunsGeneration(std::size_t worker_index,
                          std::size_t worker_count,
                          std::uint64_t generation) {
  if (worker_count == 0 || generation == 0) {
    return false;
  }
  return worker_index == ((generation - 1) % worker_count);
}

std::string JsonEscape(const std::string& value) {
  std::ostringstream out;
  for (const unsigned char ch : value) {
    switch (ch) {
      case '\\': out << "\\\\"; break;
      case '"': out << "\\\""; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default:
        if (ch < 0x20) {
          out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
              << static_cast<int>(ch) << std::dec;
        } else {
          out << static_cast<char>(ch);
        }
    }
  }
  return out.str();
}

void SetCurrentThreadName(const std::string& name) {
#ifndef _WIN32
  std::string bounded = name.substr(0, 15);
  (void)::pthread_setname_np(::pthread_self(), bounded.c_str());
#else
  (void)name;
#endif
}

std::string ThreadIdText(std::thread::id id) {
  std::ostringstream out;
  out << id;
  return out.str();
}

std::string NewTypedUuidText(platform::UuidKind kind,
                             const std::string& key,
                             std::uint64_t salt) {
  std::uint64_t folded = salt + CurrentUnixMillis();
  for (const unsigned char ch : key) {
    folded ^= static_cast<std::uint64_t>(ch);
    folded *= 1099511628211ull;
  }
  const auto generated = uuid::GenerateEngineIdentityV7(kind, folded);
  return generated.ok() ? uuid::UuidToString(generated.value.value) : std::string{};
}

std::string ServerAgentRuntimeUuid(const std::string& database_uuid,
                                   const std::string& purpose,
                                   std::uint64_t salt) {
  return NewTypedUuidText(platform::UuidKind::object,
                          database_uuid + "|server_agent_runtime|" + purpose,
                          salt);
}

std::string ServerAgentInstanceUuid(const std::string& database_uuid,
                                    const std::string& agent_type_id) {
  return ServerAgentRuntimeUuid(database_uuid,
                                "instance|" + agent_type_id,
                                1000);
}

agents::DurableAgentCatalogImage BuildServerAgentRuntimeCatalog(
    const std::string& database_uuid,
    const std::string& filespace_uuid,
    const std::vector<std::string>& assignments) {
  agents::DurableAgentCatalogImage image;
  image.schema_version = 1;

  std::set<std::string> added;
  for (const auto& agent_type_id : assignments) {
    if (!added.insert(agent_type_id).second) { continue; }
    agents::AgentInstanceRecord instance;
    instance.instance_uuid = ServerAgentInstanceUuid(database_uuid, agent_type_id);
    instance.agent_type_id = agent_type_id;
    instance.policy_uuid =
        ServerAgentRuntimeUuid(database_uuid, "policy|" + agent_type_id, 1400);
    instance.scope = "database/" + database_uuid + "/filespace/" + filespace_uuid;
    instance.state = agents::AgentLifecycleState::registered;
    instance.run_generation = 1;
    instance.policy_generation = 1;
    instance.instance_generation = 1;
    image.instances.push_back(std::move(instance));
  }
  return image;
}

std::vector<std::string> DefaultAgentAssignments() {
  return {
      "page_allocation_manager",
      "filespace_capacity_manager",
      "storage_health_manager",
      "transaction_pressure_manager",
      "storage_version_cleanup_agent",
      "metrics_registry_manager",
      "memory_governor",
      "index_health_manager",
      "admission_control_manager",
      "cleanup_archive_manager",
      "policy_recommendation_manager",
      "runtime_learning_agent",
      "support_bundle_triage_agent",
      "backup_manager",
      "archive_manager",
      "pitr_manager",
      "alert_manager",
  };
}

std::vector<std::string> PrioritizeDmlPreworkAssignments(std::vector<std::string> assignments) {
  const std::vector<std::string> priority = {
      "page_allocation_manager",
      "filespace_capacity_manager",
      "storage_health_manager",
      "transaction_pressure_manager",
      "storage_version_cleanup_agent",
  };
  std::vector<std::string> prioritized;
  prioritized.reserve(assignments.size() + priority.size());
  for (const auto& agent_type_id : priority) {
    if (std::find(assignments.begin(), assignments.end(), agent_type_id) != assignments.end()) {
      prioritized.push_back(agent_type_id);
    }
  }
  for (auto& agent_type_id : assignments) {
    if (std::find(prioritized.begin(), prioritized.end(), agent_type_id) == prioritized.end()) {
      prioritized.push_back(std::move(agent_type_id));
    }
  }
  return prioritized;
}

std::optional<HostedDatabaseSnapshot> FirstOpenDatabase(const HostedEngineState& state) {
  for (const auto& database : state.databases) {
    if (database.database_open && database.state == HostedDatabaseState::kOpen) {
      return database;
    }
  }
  for (const auto& database : state.databases) {
    if (database.database_open) {
      return database;
    }
  }
  return std::nullopt;
}

ServerDiagnostic RuntimeDiagnostic(std::string code,
                                   std::string message,
                                   std::vector<ServerDiagnosticField> fields = {},
                                   ServerDiagnosticSeverity severity =
                                       ServerDiagnosticSeverity::kError) {
  return ServerDiagnostic{std::move(code),
                          std::move(code),
                          severity,
                          std::move(message),
                          std::move(fields)};
}

struct ServerAgentAuthorityEpochs {
  std::uint64_t catalog_generation_id = 1;
  std::uint64_t security_epoch = 1;
  std::uint64_t resource_epoch = 1;
  std::uint64_t name_resolution_epoch = 1;
};

ServerAgentAuthorityEpochs AuthorityEpochsFromDatabase(
    const HostedDatabaseSnapshot& database) {
  ServerAgentAuthorityEpochs epochs;
  epochs.catalog_generation_id =
      std::max<std::uint64_t>(1, database.policy_generation);
  epochs.security_epoch = std::max<std::uint64_t>(1, database.security_epoch);
  epochs.resource_epoch =
      std::max<std::uint64_t>(1, database.cache_invalidation_epoch);
  epochs.name_resolution_epoch =
      std::max<std::uint64_t>(1, database.cache_invalidation_epoch);
  return epochs;
}

const char* AgentRuntimeHostedStateName(HostedDatabaseState state) {
  switch (state) {
    case HostedDatabaseState::kNotConfigured: return "not_configured";
    case HostedDatabaseState::kOpening: return "opening";
    case HostedDatabaseState::kOpen: return "open";
    case HostedDatabaseState::kReadOnly: return "read_only";
    case HostedDatabaseState::kRestrictedOpen: return "restricted_open";
    case HostedDatabaseState::kMaintenance: return "maintenance";
    case HostedDatabaseState::kFailed: return "failed";
    case HostedDatabaseState::kDetached: return "detached";
    case HostedDatabaseState::kQuarantined: return "quarantined";
  }
  return "unknown";
}

std::optional<ServerDiagnostic> ServerAgentRuntimeBlocker(
    const HostedDatabaseSnapshot& database) {
  std::string reason;
  if (!database.database_open || database.state != HostedDatabaseState::kOpen) {
    reason = "database_not_open_for_background_agent_runtime";
  } else if (database.read_only) {
    reason = "read_only_database";
  } else if (database.write_admission_fenced) {
    reason = "write_admission_fenced";
  } else if (database.cluster_structures_present ||
             database.cluster_authority_required) {
    reason = "cluster_authority_required";
  } else if (!database.config_policy_security_lifecycle_present) {
    reason = "config_policy_security_lifecycle_missing";
  }
  if (reason.empty()) { return std::nullopt; }
  return RuntimeDiagnostic(
      "SERVER.AGENT_RUNTIME.PRODUCTION_GATE_CLOSED",
      "The server agent runtime did not start because this database is not a writable normal hosted database.",
      {{"database_path", database.database_path},
       {"database_state", AgentRuntimeHostedStateName(database.state)},
       {"reason", reason}},
      ServerDiagnosticSeverity::kWarning);
}

std::string FirstDiagnosticCode(const engine_api::EngineApiResult& result,
                                const std::string& fallback) {
  return result.diagnostics.empty() ? fallback : result.diagnostics.front().code;
}

std::string FirstDiagnosticDetail(const engine_api::EngineApiResult& result,
                                  const std::string& fallback) {
  if (result.diagnostics.empty()) { return fallback; }
  if (!result.diagnostics.front().detail.empty()) {
    return result.diagnostics.front().detail;
  }
  if (!result.diagnostics.front().message_key.empty()) {
    return result.diagnostics.front().message_key;
  }
  return result.diagnostics.front().code;
}

std::string EvidenceValue(const engine_api::EngineApiResult& result,
                          std::string_view evidence_kind) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == evidence_kind) {
      return evidence.evidence_id;
    }
  }
  return {};
}

engine_api::EngineRequestContext ServerAgentBaseContext(
    const std::string& database_path,
    const std::string& database_uuid,
    const ServerAgentAuthorityEpochs& epochs,
    std::uint64_t generation,
    std::string request_id) {
  engine_api::EngineRequestContext context;
  context.trust_mode = engine_api::EngineTrustMode::server_isolated;
  context.request_id = std::move(request_id);
  context.database_path = database_path;
  context.database_uuid.canonical = database_uuid;
  context.principal_uuid.canonical =
      agents::DeterministicAgentRuntimePrincipalUuidFromKey(
          database_uuid + "|server_agent_runtime");
  context.session_uuid.canonical =
      ServerAgentRuntimeUuid(database_uuid,
                             "session|" + std::to_string(generation),
                             102 + generation);
  context.security_context_present = true;
  context.catalog_generation_id = epochs.catalog_generation_id;
  context.security_epoch = epochs.security_epoch;
  context.resource_epoch = epochs.resource_epoch;
  context.name_resolution_epoch = epochs.name_resolution_epoch;
  context.current_monotonic_ns = std::to_string(CurrentMonotonicNs());
  context.trace_tags.push_back("security.bootstrap");
  context.trace_tags.push_back("group:OPS");
  context.trace_tags.push_back("right:OBS_AGENT_STATE_READ");
  context.trace_tags.push_back("right:OBS_AGENT_EVIDENCE_READ");
  context.trace_tags.push_back("right:OBS_AGENT_CONTROL");
  return context;
}

struct ServerAgentTransactionContext {
  bool ok = false;
  engine_api::EngineRequestContext context;
  std::string diagnostic_code;
  std::string diagnostic_detail;
};

ServerAgentTransactionContext BeginServerAgentTransaction(
    const std::string& database_path,
    const std::string& database_uuid,
    const ServerAgentAuthorityEpochs& epochs,
    std::uint64_t generation,
    const std::string& purpose) {
  ServerAgentTransactionContext out;
  engine_api::EngineBeginTransactionRequest begin;
  begin.context = ServerAgentBaseContext(
      database_path,
      database_uuid,
      epochs,
      generation,
      "server-agent-" + purpose + "-" + std::to_string(generation));
  begin.context.local_transaction_id = 0;
  begin.context.transaction_uuid.canonical.clear();
  begin.context.snapshot_visible_through_local_transaction_id = 0;
  begin.context.read_only_mode = false;
  begin.isolation_level = "read_committed";
  begin.transaction_policy_profile.encoded_profiles.push_back("fail_closed:true");
  begin.transaction_policy_profile.encoded_profiles.push_back(
      "transaction_read_only:false");
  begin.transaction_policy_profile.encoded_profiles.push_back(
      "transaction_read_mode:read_write");
  const auto begun = engine_api::EngineBeginTransaction(begin);
  if (!begun.ok || begun.local_transaction_id == 0 ||
      begun.transaction_uuid.canonical.empty()) {
    out.diagnostic_code =
        FirstDiagnosticCode(begun, "SERVER.AGENT_RUNTIME.MGA_BEGIN_FAILED");
    out.diagnostic_detail =
        FirstDiagnosticDetail(begun, "server_agent_transaction_begin_failed");
    return out;
  }
  out.context = begin.context;
  out.context.transaction_uuid = begun.transaction_uuid;
  out.context.local_transaction_id = begun.local_transaction_id;
  out.context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  out.context.transaction_isolation_level = begun.isolation_level;
  out.context.read_only_mode = begun.read_only;
  out.context.transaction_timestamp = EvidenceValue(begun, "transaction_timestamp");
  out.ok = true;
  return out;
}

bool CommitServerAgentTransaction(const engine_api::EngineRequestContext& context,
                                  std::string* diagnostic_code,
                                  std::string* diagnostic_detail) {
  engine_api::EngineCommitTransactionRequest commit;
  commit.context = context;
  const auto committed = engine_api::EngineCommitTransaction(commit);
  if (committed.ok) { return true; }
  if (diagnostic_code != nullptr) {
    *diagnostic_code =
        FirstDiagnosticCode(committed, "SERVER.AGENT_RUNTIME.MGA_COMMIT_FAILED");
  }
  if (diagnostic_detail != nullptr) {
    *diagnostic_detail =
        FirstDiagnosticDetail(committed, "server_agent_transaction_commit_failed");
  }
  return false;
}

void RollbackServerAgentTransaction(const engine_api::EngineRequestContext& context) {
  engine_api::EngineRollbackTransactionRequest rollback;
  rollback.context = context;
  (void)engine_api::EngineRollbackTransaction(rollback);
}

agents::AgentRuntimeContext CapacityPlanningContext(const std::string& database_uuid) {
  agents::AgentRuntimeContext context;
  context.security_context_present = true;
  context.private_features_available = true;
  context.standalone_edition = true;
  context.cluster_authority_available = false;
  context.database_uuid = database_uuid;
  context.principal_uuid =
      agents::DeterministicAgentRuntimePrincipalUuidFromKey(database_uuid + "|server_agent_runtime");
  context.monotonic_now_microseconds = CurrentMonotonicNs() / 1000;
  context.wall_now_microseconds = CurrentUnixMillis() * 1000;
  context.groups.push_back("OPS");
  context.rights.push_back("OBS_AGENT_STATE_READ");
  context.rights.push_back("OBS_AGENT_EVIDENCE_READ");
  context.rights.push_back("OBS_AGENT_CONTROL");
  return context;
}

void AddCommonActionFields(engine_api::EngineAgentActionHookRequest* request,
                           const std::string& database_uuid,
                           const std::string& filespace_uuid,
                           std::uint64_t generation,
                           const std::string& agent_type,
                           const std::string& action_class) {
  request->agent_type = agent_type;
  request->action_class = action_class;
  request->agent_uuid.canonical = NewTypedUuidText(
      platform::UuidKind::object, database_uuid + "|" + agent_type + "|instance", 201 + generation);
  request->policy_snapshot_uuid.canonical = NewTypedUuidText(
      platform::UuidKind::object, database_uuid + "|" + agent_type + "|policy", 301 + generation);
  request->target_filespace.uuid.canonical = filespace_uuid;
  request->target_filespace.object_kind = "filespace";
  request->safety_fence_result = "passed";
  request->policy_authorized = true;
  request->evidence_sink_available = true;
  request->metrics_fresh = true;
  const auto wall_now_us = CurrentUnixMillis() * 1000;
  request->option_envelopes.push_back("wall_now_us:" + std::to_string(wall_now_us));
  request->option_envelopes.push_back("monotonic_now_us:" + std::to_string(CurrentMonotonicNs() / 1000));
  request->option_envelopes.push_back("agent_action_class:direct_bounded_action");
  request->option_envelopes.push_back("agent_metric_snapshot_observed:true");
  request->option_envelopes.push_back("agent_metric_snapshot_trusted:true");
  request->option_envelopes.push_back("agent_metric_snapshot_source_quality:trusted");
  request->option_envelopes.push_back("agent_metric_snapshot_trust_provenance:server_agent_runtime");
  request->option_envelopes.push_back("agent_metric_snapshot_scope_uuid:" + database_uuid);
  request->option_envelopes.push_back("agent_metric_snapshot_generation:" + std::to_string(generation));
  request->option_envelopes.push_back("agent_metric_snapshot_observed_wall_us:" + std::to_string(wall_now_us));
  request->option_envelopes.push_back(
      "agent_metric_snapshot_digest:sha256:server-agent-runtime:" +
      agent_type + ":" + std::to_string(generation));
  request->option_envelopes.push_back(
      "agent_metric_snapshot_id:server-agent-runtime:" +
      agent_type + ":" + std::to_string(generation));
  request->option_envelopes.push_back(
      "agent_metric_snapshot_evidence_uuid:" +
      NewTypedUuidText(platform::UuidKind::object,
                       database_uuid + "|" + agent_type + "|metric-evidence",
                       401 + generation));
}

}  // namespace

ServerAgentRuntime::~ServerAgentRuntime() {
  Stop();
}

bool ServerAgentRuntime::Start(const ServerBootstrapConfig& config,
                               const HostedEngineState& engine_state,
                               std::vector<ServerDiagnostic>* diagnostics) {
  const auto database = FirstOpenDatabase(engine_state);
  if (!database.has_value()) {
    return true;
  }
  if (config.embedded_direct_mode) {
    return true;
  }
  if (database->database_uuid.empty() || database->filespace_uuid.empty()) {
    if (diagnostics != nullptr) {
      diagnostics->push_back(RuntimeDiagnostic(
          "SERVER.AGENT_RUNTIME.IDENTITY_MISSING",
          "The server agent runtime requires database and filespace UUIDs before workers start.",
          {{"database_path", database->database_path}}));
    }
    return false;
  }
  if (auto blocked = ServerAgentRuntimeBlocker(*database)) {
    if (diagnostics != nullptr) {
      diagnostics->push_back(std::move(*blocked));
    }
    return true;
  }
  const ServerAgentAuthorityEpochs authority_epochs =
      AuthorityEpochsFromDatabase(*database);

  std::vector<std::string> assignments = database->selected_agent_type_ids;
  if (assignments.empty()) {
    assignments = DefaultAgentAssignments();
  }
  assignments = PrioritizeDmlPreworkAssignments(std::move(assignments));

  const auto hardware = std::thread::hardware_concurrency();
  const std::uint32_t observed_cpu_count = hardware == 0 ? 4 : hardware;
  agents::AgentWorkerCapacityConfig capacity_config;
  capacity_config.observed_cpu_count = observed_cpu_count;
  capacity_config.configured_cpu_count = std::max<std::uint32_t>(3, observed_cpu_count);
  capacity_config.foreground_reserved_capacity = 1;
  capacity_config.max_background_worker_slots = 31;
  capacity_config.foreground_database_work_active = false;
  capacity_config.standalone_edition = true;
  capacity_config.cluster_authority_available = false;
  const auto capacity = agents::PlanAgentWorkerCapacity(
      capacity_config,
      CapacityPlanningContext(database->database_uuid),
      agents::DefaultDmlPreworkAgentWorkerCandidates(1));
  const std::uint32_t planned_worker_count = std::max<std::uint32_t>(
      2, static_cast<std::uint32_t>(capacity.background_worker_slots));
  const std::uint32_t assignment_worker_count =
      assignments.empty() ? planned_worker_count
                          : static_cast<std::uint32_t>(assignments.size());
  const std::uint32_t worker_count =
      std::max<std::uint32_t>(1,
                              std::min(planned_worker_count,
                                       std::min(assignment_worker_count,
                                                kIdleResidentAgentSlots)));
  const std::uint32_t bounded_worker_count = std::min<std::uint32_t>(worker_count, 31);

  {
    std::lock_guard<std::mutex> guard(state_mutex_);
    if (started_) {
      return true;
    }
    database_path_ = database->database_path;
    database_uuid_ = database->database_uuid;
    filespace_uuid_ = database->filespace_uuid;
    catalog_generation_id_ = authority_epochs.catalog_generation_id;
    security_epoch_ = authority_epochs.security_epoch;
    resource_epoch_ = authority_epochs.resource_epoch;
    name_resolution_epoch_ = authority_epochs.name_resolution_epoch;
    status_path_ = config.control_dir / "sb_server.agent_runtime.json";
    hardware_concurrency_ = hardware;
    effective_cpu_count_ = static_cast<std::uint32_t>(capacity.effective_cpu_count);
    foreground_reserved_capacity_ =
        static_cast<std::uint32_t>(capacity.foreground_reserved_capacity);
    background_worker_slots_ = bounded_worker_count;
    worker_wake_policy_ = "staggered_worker_per_scheduler_tick";
    selected_agents_ = std::move(assignments);
    {
      std::lock_guard<std::mutex> schedule_guard(schedule_mutex_);
      scheduled_generation_ = 0;
      worker_completed_generations_.assign(bounded_worker_count, 0);
    }
    worker_evidence_.clear();
    worker_evidence_.reserve(bounded_worker_count);
    for (std::uint32_t i = 0; i < bounded_worker_count; ++i) {
      WorkerEvidence evidence;
      std::ostringstream name;
      name << "sb-agent-w" << std::setw(2) << std::setfill('0') << i;
      evidence.name = name.str();
      evidence.role = "database_local_agent_worker";
      evidence.agent_type_id = selected_agents_[i % selected_agents_.size()];
      evidence.instance_uuid =
          ServerAgentInstanceUuid(database->database_uuid, evidence.agent_type_id);
      evidence.lease_uuid = ServerAgentRuntimeUuid(
          database->database_uuid,
          "lease|" + evidence.name + "|" + evidence.agent_type_id,
          1500 + i);
      evidence.lease_owner_uuid = ServerAgentRuntimeUuid(
          database->database_uuid,
          "lease_owner|" + evidence.name,
          1600 + i);
      worker_evidence_.push_back(std::move(evidence));
    }
    stopping_.store(false);
  }

  std::error_code ec;
  std::filesystem::create_directories(status_path_.parent_path(), ec);
  if (ec) {
    if (diagnostics != nullptr) {
      diagnostics->push_back(RuntimeDiagnostic(
          "SERVER.AGENT_RUNTIME.STATUS_PATH_FAILED",
          "The server agent runtime status directory could not be created.",
          {{"status_path", status_path_.string()}}));
    }
    std::lock_guard<std::mutex> guard(state_mutex_);
    started_ = false;
    return false;
  }

  {
    auto seed_tx = BeginServerAgentTransaction(database_path_,
                                               database_uuid_,
                                               authority_epochs,
                                               1,
                                               "catalog-seed");
    if (!seed_tx.ok) {
      if (diagnostics != nullptr) {
        diagnostics->push_back(RuntimeDiagnostic(
            seed_tx.diagnostic_code,
            "The server agent runtime could not begin an MGA transaction for catalog seeding.",
            {{"detail", seed_tx.diagnostic_detail}}));
      }
      return false;
    }
    auto loaded = engine_api::LoadAgentDurableCatalogImage(seed_tx.context, true);
    if (!loaded.ok) {
      engine_api::AgentDurableCatalogStoreRequest seed_request;
      seed_request.context = seed_tx.context;
      seed_request.image = BuildServerAgentRuntimeCatalog(database_uuid_,
                                                          filespace_uuid_,
                                                          selected_agents_);
      seed_request.evidence_uuid =
          ServerAgentRuntimeUuid(database_uuid_, "catalog_seed", 1750);
      seed_request.production_live_path = true;
      seed_request.fsync_or_checkpoint_evidence = true;
      const auto seeded =
          engine_api::PersistAgentDurableCatalogImage(seed_request);
      if (!seeded.ok) {
        RollbackServerAgentTransaction(seed_tx.context);
        if (diagnostics != nullptr) {
          diagnostics->push_back(RuntimeDiagnostic(
              seeded.diagnostic.code,
              "The server agent runtime durable catalog seed could not be persisted.",
              {{"detail", seeded.diagnostic.detail}}));
        }
        return false;
      }
    }
    std::string tx_diagnostic;
    std::string tx_detail;
    if (!CommitServerAgentTransaction(seed_tx.context,
                                      &tx_diagnostic,
                                      &tx_detail)) {
      if (diagnostics != nullptr) {
        diagnostics->push_back(RuntimeDiagnostic(
            tx_diagnostic,
            "The server agent runtime durable catalog seed could not be committed.",
            {{"detail", tx_detail}}));
      }
      return false;
    }
  }

  agents::AgentRuntimeServiceResult service_started;
  {
    std::lock_guard<std::mutex> service_guard(runtime_service_mutex_);
    auto open_tx = BeginServerAgentTransaction(database_path_,
                                               database_uuid_,
                                               authority_epochs,
                                               2,
                                               "service-open");
    if (!open_tx.ok) {
      if (diagnostics != nullptr) {
        diagnostics->push_back(RuntimeDiagnostic(
            open_tx.diagnostic_code,
            "The server agent runtime could not begin an MGA transaction for service open.",
            {{"detail", open_tx.diagnostic_detail}}));
      }
      return false;
    }
    engine_api::AgentRuntimeServiceStoreOpenRequest open_request;
    open_request.context = open_tx.context;
    open_request.manifest = agents::CanonicalAgentManifest();
    open_request.production_live_path = true;
    open_request.worker_foreground_protection_enabled = true;
    open_request.crash_recovery_mode = true;
    open_request.service_owner_uuid =
        ServerAgentRuntimeUuid(database_uuid_, "service_owner", 1700);
    open_request.evidence_uuid =
        ServerAgentRuntimeUuid(database_uuid_, "service_open", 1800);
    open_request.fsync_or_checkpoint_evidence = true;
    auto opened = runtime_service_.Open(std::move(open_request));
    if (!opened.status.ok) {
      RollbackServerAgentTransaction(open_tx.context);
      if (diagnostics != nullptr) {
        diagnostics->push_back(RuntimeDiagnostic(
            opened.status.diagnostic_code,
            "The server agent runtime durable service could not open.",
            {{"detail", opened.status.detail}}));
      }
      return false;
    }
    std::string tx_diagnostic;
    std::string tx_detail;
    if (!CommitServerAgentTransaction(open_tx.context,
                                      &tx_diagnostic,
                                      &tx_detail)) {
      if (diagnostics != nullptr) {
        diagnostics->push_back(RuntimeDiagnostic(
            tx_diagnostic,
            "The server agent runtime durable service open could not be committed.",
            {{"detail", tx_detail}}));
      }
      return false;
    }

    auto recover_tx = BeginServerAgentTransaction(database_path_,
                                                  database_uuid_,
                                                  authority_epochs,
                                                  4,
                                                  "service-recover");
    if (!recover_tx.ok) {
      if (diagnostics != nullptr) {
        diagnostics->push_back(RuntimeDiagnostic(
            recover_tx.diagnostic_code,
            "The server agent runtime could not begin an MGA transaction for service recovery.",
            {{"detail", recover_tx.diagnostic_detail}}));
      }
      return false;
    }
    runtime_service_.SetContext(recover_tx.context);
    auto service_recovered = runtime_service_.Recover(
        ServerAgentRuntimeUuid(database_uuid_, "service_recover", 1850),
        CurrentUnixMillis() * 1000,
        true);
    if (!service_recovered.status.ok) {
      RollbackServerAgentTransaction(recover_tx.context);
      if (diagnostics != nullptr) {
        diagnostics->push_back(RuntimeDiagnostic(
            service_recovered.status.diagnostic_code,
            "The server agent runtime durable service recovery could not complete.",
            {{"detail", service_recovered.status.detail}}));
      }
      return false;
    }
    if (!CommitServerAgentTransaction(recover_tx.context,
                                      &tx_diagnostic,
                                      &tx_detail)) {
      if (diagnostics != nullptr) {
        diagnostics->push_back(RuntimeDiagnostic(
            tx_diagnostic,
            "The server agent runtime durable service recovery could not be committed.",
            {{"detail", tx_detail}}));
      }
      return false;
    }
    {
      std::lock_guard<std::mutex> guard(state_mutex_);
      UpdateRuntimeCatalogSnapshotLocked(service_recovered.catalog);
      try {
        last_recovery_replayed_count_ =
            service_recovered.status.detail.empty()
                ? 0
                : std::stoull(service_recovered.status.detail);
      } catch (...) {
        last_recovery_replayed_count_ = std::numeric_limits<std::uint64_t>::max();
      }
    }

    auto start_tx = BeginServerAgentTransaction(database_path_,
                                                database_uuid_,
                                                authority_epochs,
                                                5,
                                                "service-start");
    if (!start_tx.ok) {
      if (diagnostics != nullptr) {
        diagnostics->push_back(RuntimeDiagnostic(
            start_tx.diagnostic_code,
            "The server agent runtime could not begin an MGA transaction for service start.",
            {{"detail", start_tx.diagnostic_detail}}));
      }
      return false;
    }
    runtime_service_.SetContext(start_tx.context);
    service_started = runtime_service_.Start(
        ServerAgentRuntimeUuid(database_uuid_, "service_start", 1900), true);
    if (!service_started.status.ok) {
      RollbackServerAgentTransaction(start_tx.context);
      if (diagnostics != nullptr) {
        diagnostics->push_back(RuntimeDiagnostic(
            service_started.status.diagnostic_code,
            "The server agent runtime durable service could not start.",
            {{"detail", service_started.status.detail}}));
      }
      return false;
    }
    if (!CommitServerAgentTransaction(start_tx.context,
                                      &tx_diagnostic,
                                      &tx_detail)) {
      if (diagnostics != nullptr) {
        diagnostics->push_back(RuntimeDiagnostic(
            tx_diagnostic,
            "The server agent runtime durable service start could not be committed.",
            {{"detail", tx_detail}}));
      }
      return false;
    }
  }
  {
    std::lock_guard<std::mutex> guard(state_mutex_);
    started_ = true;
    UpdateRuntimeCatalogSnapshotLocked(service_started.catalog);
  }

  scheduler_thread_ = std::thread(&ServerAgentRuntime::SchedulerLoop, this);
  for (std::size_t i = 0; i < bounded_worker_count; ++i) {
    worker_threads_.emplace_back(&ServerAgentRuntime::WorkerLoop, this, i);
  }
  WriteStatusSnapshot();
  return true;
}

void ServerAgentRuntime::Stop() {
  {
    std::lock_guard<std::mutex> guard(state_mutex_);
    if (!started_ || stopping_.load()) {
      return;
    }
    stopping_.store(true);
  }
  schedule_cv_.notify_all();
  if (scheduler_thread_.joinable()) {
    scheduler_thread_.join();
  }
  for (auto& thread : worker_threads_) {
    if (thread.joinable()) {
      thread.join();
    }
  }
  worker_threads_.clear();
  {
    std::lock_guard<std::mutex> service_guard(runtime_service_mutex_);
    const auto now = CurrentUnixMillis() * 1000;
    const ServerAgentAuthorityEpochs authority_epochs{
        catalog_generation_id_,
        security_epoch_,
        resource_epoch_,
        name_resolution_epoch_};
    agents::AgentRuntimeServiceResult drained;
    auto drain_tx = BeginServerAgentTransaction(database_path_,
                                                database_uuid_,
                                                authority_epochs,
                                                now,
                                                "service-drain");
    if (drain_tx.ok) {
      runtime_service_.SetContext(drain_tx.context);
      drained = runtime_service_.Drain(
          ServerAgentRuntimeUuid(database_uuid_, "service_drain", 2000),
          now,
          true);
      if (drained.status.ok) {
        std::string ignored_code;
        std::string ignored_detail;
        if (!CommitServerAgentTransaction(drain_tx.context,
                                          &ignored_code,
                                          &ignored_detail)) {
          drained.status =
              agents::AgentError(ignored_code.empty()
                                     ? "SERVER.AGENT_RUNTIME.MGA_COMMIT_FAILED"
                                     : ignored_code,
                                 ignored_detail);
        }
      } else {
        RollbackServerAgentTransaction(drain_tx.context);
      }
    }
    agents::AgentRuntimeServiceResult shutdown;
    auto shutdown_tx = BeginServerAgentTransaction(database_path_,
                                                   database_uuid_,
                                                   authority_epochs,
                                                   now + 1,
                                                   "service-shutdown");
    if (shutdown_tx.ok) {
      runtime_service_.SetContext(shutdown_tx.context);
      shutdown = runtime_service_.Shutdown(
          ServerAgentRuntimeUuid(database_uuid_, "service_shutdown", 2100),
          now + 1,
          true);
      if (shutdown.status.ok) {
        std::string ignored_code;
        std::string ignored_detail;
        if (!CommitServerAgentTransaction(shutdown_tx.context,
                                          &ignored_code,
                                          &ignored_detail)) {
          shutdown.status =
              agents::AgentError(ignored_code.empty()
                                     ? "SERVER.AGENT_RUNTIME.MGA_COMMIT_FAILED"
                                     : ignored_code,
                                 ignored_detail);
        }
      } else {
        RollbackServerAgentTransaction(shutdown_tx.context);
      }
    }
    std::lock_guard<std::mutex> guard(state_mutex_);
    if (shutdown.status.ok) {
      UpdateRuntimeCatalogSnapshotLocked(shutdown.catalog);
    } else if (drained.status.ok) {
      UpdateRuntimeCatalogSnapshotLocked(drained.catalog);
    }
  }
  {
    std::lock_guard<std::mutex> guard(state_mutex_);
    started_ = false;
  }
  WriteStatusSnapshot();
}

ServerAgentRuntimeSnapshot ServerAgentRuntime::Snapshot() const {
  std::lock_guard<std::mutex> guard(state_mutex_);
  ServerAgentRuntimeSnapshot snapshot;
  snapshot.started = started_;
  snapshot.stopping = stopping_.load();
  snapshot.database_path = database_path_;
  snapshot.database_uuid = database_uuid_;
  snapshot.filespace_uuid = filespace_uuid_;
  snapshot.status_path = status_path_;
  snapshot.hardware_concurrency = hardware_concurrency_;
  snapshot.effective_cpu_count = effective_cpu_count_;
  snapshot.foreground_reserved_capacity = foreground_reserved_capacity_;
  snapshot.background_worker_slots = background_worker_slots_;
  snapshot.worker_thread_count = static_cast<std::uint32_t>(worker_evidence_.size());
  snapshot.worker_wake_policy = worker_wake_policy_;
  snapshot.scheduler_ticks = scheduler_ticks_;
  for (const auto& worker : worker_evidence_) {
    snapshot.total_worker_ticks += worker.ticks;
    snapshot.total_actions_accepted += worker.actions_accepted;
    snapshot.total_actions_refused += worker.actions_refused;
  }
  snapshot.durable_catalog_generation = durable_catalog_generation_;
  snapshot.durable_lease_count = durable_lease_count_;
  snapshot.durable_replay_pending_lease_count =
      durable_replay_pending_lease_count_;
  snapshot.durable_action_backlog_count = durable_action_backlog_count_;
  snapshot.durable_replay_pending_action_count =
      durable_replay_pending_action_count_;
  snapshot.durable_service_evidence_count = durable_service_evidence_count_;
  snapshot.last_recovery_replayed_count = last_recovery_replayed_count_;
  snapshot.durable_catalog_root_digest = durable_catalog_root_digest_;
  return snapshot;
}

void ServerAgentRuntime::SchedulerLoop() {
  SetCurrentThreadName("sb-agent-sch");
  {
    std::lock_guard<std::mutex> guard(state_mutex_);
    scheduler_thread_id_ = ThreadIdText(std::this_thread::get_id());
  }
  {
    std::unique_lock<std::mutex> lock(schedule_mutex_);
    if (schedule_cv_.wait_for(lock, kInitialAgentSchedulerDelay, [&] {
          return stopping_.load();
        })) {
      return;
    }
  }
  while (true) {
    std::uint64_t generation = 0;
    {
      std::unique_lock<std::mutex> lock(schedule_mutex_);
      const auto interval =
          scheduled_generation_ < worker_completed_generations_.size()
              ? kWarmAgentSchedulerInterval
              : kIdleAgentSchedulerInterval;
      if (schedule_cv_.wait_for(lock, interval, [&] {
            return stopping_.load();
          })) {
        break;
      }
      ++scheduled_generation_;
      generation = scheduled_generation_;
    }
    {
      std::lock_guard<std::mutex> guard(state_mutex_);
      ++scheduler_ticks_;
    }
    schedule_cv_.notify_all();
    {
      std::unique_lock<std::mutex> lock(schedule_mutex_);
      schedule_cv_.wait(lock, [&] {
        return stopping_.load() ||
               std::all_of(worker_completed_generations_.begin(),
                           worker_completed_generations_.end(),
                           [generation](std::uint64_t completed) {
                             return completed >= generation;
                           });
      });
      if (stopping_.load()) {
        break;
      }
    }
    WriteStatusSnapshot();
  }
}

void ServerAgentRuntime::WorkerLoop(std::size_t worker_index) {
  std::string thread_name;
  std::size_t worker_count = 0;
  {
    std::lock_guard<std::mutex> guard(state_mutex_);
    worker_count = worker_evidence_.size();
    if (worker_index < worker_evidence_.size()) {
      thread_name = worker_evidence_[worker_index].name;
      worker_evidence_[worker_index].native_thread_id = ThreadIdText(std::this_thread::get_id());
    }
  }
  SetCurrentThreadName(thread_name.empty() ? "sb-agent-w" : thread_name);
  std::uint64_t observed_generation = 0;
  while (true) {
    std::uint64_t generation = 0;
    {
      std::unique_lock<std::mutex> lock(schedule_mutex_);
      schedule_cv_.wait(lock, [&] {
        return stopping_.load() ||
               (scheduled_generation_ > observed_generation && worker_count != 0);
      });
      if (stopping_.load()) {
        break;
      }
      generation = scheduled_generation_;
      observed_generation = generation;
    }
    if (WorkerRunsGeneration(worker_index, worker_count, generation)) {
      RunWorkerTick(worker_index, generation);
    }
    {
      std::lock_guard<std::mutex> lock(schedule_mutex_);
      if (worker_index < worker_completed_generations_.size()) {
        worker_completed_generations_[worker_index] = generation;
      }
    }
    schedule_cv_.notify_all();
  }
}

void ServerAgentRuntime::RunWorkerTick(std::size_t worker_index,
                                       std::uint64_t generation) {
  std::string agent_type;
  std::string database_path;
  std::string database_uuid;
  std::string filespace_uuid;
  std::string instance_uuid;
  std::string lease_uuid;
  std::string lease_owner_uuid;
  bool durable_lease_acquired = false;
  std::uint64_t last_lease_heartbeat_generation = 0;
  std::uint64_t worker_ticks_so_far = 0;
  ServerAgentAuthorityEpochs authority_epochs;
  {
    std::lock_guard<std::mutex> guard(state_mutex_);
    if (worker_index >= worker_evidence_.size()) {
      return;
    }
    agent_type = worker_evidence_[worker_index].agent_type_id;
    database_path = database_path_;
    database_uuid = database_uuid_;
    filespace_uuid = filespace_uuid_;
    instance_uuid = worker_evidence_[worker_index].instance_uuid;
    lease_uuid = worker_evidence_[worker_index].lease_uuid;
    lease_owner_uuid = worker_evidence_[worker_index].lease_owner_uuid;
    durable_lease_acquired = worker_evidence_[worker_index].durable_lease_acquired;
    last_lease_heartbeat_generation =
        worker_evidence_[worker_index].last_lease_heartbeat_generation;
    worker_ticks_so_far = worker_evidence_[worker_index].ticks;
    authority_epochs.catalog_generation_id = catalog_generation_id_;
    authority_epochs.security_epoch = security_epoch_;
    authority_epochs.resource_epoch = resource_epoch_;
    authority_epochs.name_resolution_epoch = name_resolution_epoch_;
  }

  bool action_attempted = false;
  bool action_accepted = false;
  std::string last_action = "tick_health";
  std::string diagnostic = "SB_AGENT_THREAD_TICK_OK";

  agents::DurableLeaseRequest lease;
  lease.lease_uuid = lease_uuid;
  lease.instance_uuid = instance_uuid;
  lease.owner_uuid = lease_owner_uuid;
  lease.now_microseconds = CurrentUnixMillis() * 1000;
  lease.lease_duration_microseconds = kWorkerLeaseDurationMicroseconds;
  lease.evidence_uuid = ServerAgentRuntimeUuid(
      database_uuid,
      "worker_lease|" + std::to_string(worker_index) + "|" +
          std::to_string(generation),
      2200 + worker_index + generation);
  if (!durable_lease_acquired) {
    std::lock_guard<std::mutex> transaction_guard(database_transaction_mutex_);
    auto lease_tx = BeginServerAgentTransaction(
        database_path,
        database_uuid,
        authority_epochs,
        generation,
        "worker-lease-" + std::to_string(worker_index));
    if (!lease_tx.ok) {
      RecordWorkerTick(worker_index,
                       "lease_acquire",
                       true,
                       false,
                       lease_tx.diagnostic_code);
      return;
    }
    std::lock_guard<std::mutex> service_guard(runtime_service_mutex_);
    runtime_service_.SetContext(lease_tx.context);
    auto lease_result = runtime_service_.AcquireLease(lease, true);
    {
      std::lock_guard<std::mutex> guard(state_mutex_);
      UpdateRuntimeCatalogSnapshotLocked(lease_result.catalog);
      if (lease_result.status.ok && worker_index < worker_evidence_.size()) {
        worker_evidence_[worker_index].durable_lease_acquired = true;
        worker_evidence_[worker_index].last_lease_heartbeat_generation =
            generation;
      }
    }
    if (!lease_result.status.ok) {
      RollbackServerAgentTransaction(lease_tx.context);
      RecordWorkerTick(worker_index,
                       "lease_acquire",
                       true,
                       false,
                       lease_result.status.diagnostic_code);
      return;
    }
    std::string tx_diagnostic;
    std::string tx_detail;
    if (!CommitServerAgentTransaction(lease_tx.context,
                                      &tx_diagnostic,
                                      &tx_detail)) {
      RecordWorkerTick(worker_index,
                       "lease_acquire",
                       true,
                       false,
                       tx_diagnostic);
      return;
    }
    durable_lease_acquired = true;
    last_lease_heartbeat_generation = generation;
  }

  const bool action_cycle =
      worker_ticks_so_far == 0 ||
      ((worker_ticks_so_far + 1) % kActionEveryWorkerTicks == 0);
  if (action_cycle && IsPrimaryWorkerForAgent(worker_index, "page_allocation_manager") &&
      agent_type == "page_allocation_manager") {
    action_attempted = true;
    last_action = "page_preallocation_request";
    std::lock_guard<std::mutex> transaction_guard(database_transaction_mutex_);
    auto action_tx = BeginServerAgentTransaction(
        database_path,
        database_uuid,
        authority_epochs,
        generation,
        "page-preallocation");
    if (!action_tx.ok) {
      diagnostic = action_tx.diagnostic_code;
    } else {
      engine_api::EngineRequestPagePreallocationRequest request;
      request.context = action_tx.context;
      AddCommonActionFields(&request,
                            database_uuid,
                            filespace_uuid,
                            generation,
                            "page_allocation_manager",
                            "page_preallocation_request");
      request.page_family = "data";
      request.page_type = "relation";
      request.requested_pages = 8;
      const auto result = engine_api::EngineRequestPagePreallocation(request);
      action_accepted = result.ok && result.action_accepted;
      diagnostic = result.diagnostics.empty()
          ? (action_accepted ? "SB_AGENT_THREAD_PAGE_PREALLOCATION_ACCEPTED"
                             : "SB_AGENT_THREAD_PAGE_PREALLOCATION_REFUSED")
          : result.diagnostics.front().code;
      if (result.ok) {
        std::string tx_diagnostic;
        std::string tx_detail;
        if (!CommitServerAgentTransaction(action_tx.context,
                                          &tx_diagnostic,
                                          &tx_detail)) {
          action_accepted = false;
          diagnostic = tx_diagnostic;
        }
      } else {
        RollbackServerAgentTransaction(action_tx.context);
      }
    }
  } else if (action_cycle && IsPrimaryWorkerForAgent(worker_index, "filespace_capacity_manager") &&
             agent_type == "filespace_capacity_manager") {
    action_attempted = true;
    last_action = "filespace_growth_request";
    std::lock_guard<std::mutex> transaction_guard(database_transaction_mutex_);
    auto action_tx = BeginServerAgentTransaction(
        database_path,
        database_uuid,
        authority_epochs,
        generation,
        "filespace-growth");
    if (!action_tx.ok) {
      diagnostic = action_tx.diagnostic_code;
    } else {
      engine_api::EngineRequestFilespaceGrowthRequest request;
      request.context = action_tx.context;
      AddCommonActionFields(&request,
                            database_uuid,
                            filespace_uuid,
                            generation,
                            "filespace_capacity_manager",
                            "filespace_growth_request");
      request.requested_bytes = 8 * 16384;
      request.option_envelopes.push_back("filespace.page_size_bytes:16384");
      request.option_envelopes.push_back("filespace.current_pages:64");
      request.option_envelopes.push_back("filespace.preallocated_pages:4");
      request.option_envelopes.push_back("filespace.maximum_pages:4096");
      request.option_envelopes.push_back("filespace.reserve_growth_as_preallocated:true");
      const auto result = engine_api::EngineRequestFilespaceGrowth(request);
      action_accepted = result.ok && result.action_accepted;
      diagnostic = result.diagnostics.empty()
          ? (action_accepted ? "SB_AGENT_THREAD_FILESPACE_GROWTH_ACCEPTED"
                             : "SB_AGENT_THREAD_FILESPACE_GROWTH_REFUSED")
          : result.diagnostics.front().code;
      if (result.ok) {
        std::string tx_diagnostic;
        std::string tx_detail;
        if (!CommitServerAgentTransaction(action_tx.context,
                                          &tx_diagnostic,
                                          &tx_detail)) {
          action_accepted = false;
          diagnostic = tx_diagnostic;
        }
      } else {
        RollbackServerAgentTransaction(action_tx.context);
      }
    }
  }

  const bool heartbeat_due =
      !durable_lease_acquired ||
      last_lease_heartbeat_generation == 0 ||
      (generation > last_lease_heartbeat_generation &&
       generation - last_lease_heartbeat_generation >= kHeartbeatEveryGenerations);
  if (heartbeat_due) {
    lease.now_microseconds = CurrentUnixMillis() * 1000;
    lease.evidence_uuid = ServerAgentRuntimeUuid(
        database_uuid,
        "worker_heartbeat|" + std::to_string(worker_index) + "|" +
            std::to_string(generation),
        2300 + worker_index + generation);
    std::lock_guard<std::mutex> transaction_guard(database_transaction_mutex_);
    auto heartbeat_tx = BeginServerAgentTransaction(
        database_path,
        database_uuid,
        authority_epochs,
        generation,
        "worker-heartbeat-" + std::to_string(worker_index));
    if (!heartbeat_tx.ok) {
      if (diagnostic == "SB_AGENT_THREAD_TICK_OK") {
        diagnostic = heartbeat_tx.diagnostic_code;
      }
    } else {
      std::lock_guard<std::mutex> service_guard(runtime_service_mutex_);
      runtime_service_.SetContext(heartbeat_tx.context);
      auto heartbeat = runtime_service_.HeartbeatLease(lease, true);
      {
        std::lock_guard<std::mutex> guard(state_mutex_);
        UpdateRuntimeCatalogSnapshotLocked(heartbeat.catalog);
        if (heartbeat.status.ok && worker_index < worker_evidence_.size()) {
          worker_evidence_[worker_index].last_lease_heartbeat_generation =
              generation;
        }
      }
      if (!heartbeat.status.ok && diagnostic == "SB_AGENT_THREAD_TICK_OK") {
        RollbackServerAgentTransaction(heartbeat_tx.context);
        diagnostic = heartbeat.status.diagnostic_code;
        if (heartbeat.status.diagnostic_code == "SB_AGENT_LEASE.EXPIRED") {
          std::lock_guard<std::mutex> guard(state_mutex_);
          if (worker_index < worker_evidence_.size()) {
            worker_evidence_[worker_index].durable_lease_acquired = false;
            worker_evidence_[worker_index].last_lease_heartbeat_generation = 0;
          }
        }
      } else if (heartbeat.status.ok) {
        std::string tx_diagnostic;
        std::string tx_detail;
        if (!CommitServerAgentTransaction(heartbeat_tx.context,
                                          &tx_diagnostic,
                                          &tx_detail) &&
            diagnostic == "SB_AGENT_THREAD_TICK_OK") {
          diagnostic = tx_diagnostic;
        }
      }
    }
  }

  RecordWorkerTick(worker_index,
                   std::move(last_action),
                   action_attempted,
                   action_accepted,
                   std::move(diagnostic));
}

void ServerAgentRuntime::RecordWorkerTick(std::size_t worker_index,
                                          std::string last_action,
                                          bool action_attempted,
                                          bool action_accepted,
                                          std::string diagnostic_code) {
  std::lock_guard<std::mutex> guard(state_mutex_);
  if (worker_index >= worker_evidence_.size()) {
    return;
  }
  auto& evidence = worker_evidence_[worker_index];
  ++evidence.ticks;
  if (action_attempted) {
    if (action_accepted) {
      ++evidence.actions_accepted;
    } else {
      ++evidence.actions_refused;
    }
  }
  evidence.last_action = std::move(last_action);
  evidence.last_diagnostic_code = std::move(diagnostic_code);
}

void ServerAgentRuntime::UpdateRuntimeCatalogSnapshotLocked(
    const agents::DurableAgentCatalogImage& catalog) {
  durable_catalog_generation_ = catalog.authority.catalog_generation;
  durable_catalog_root_digest_ = catalog.authority.catalog_root_digest;
  durable_lease_count_ = static_cast<std::uint64_t>(catalog.leases.size());
  durable_replay_pending_lease_count_ = 0;
  for (const auto& lease : catalog.leases) {
    if (lease.state == agents::DurableAgentLeaseState::replay_pending) {
      ++durable_replay_pending_lease_count_;
    }
  }
  durable_action_backlog_count_ = 0;
  durable_replay_pending_action_count_ = 0;
  for (const auto& action : catalog.actions) {
    if (action.state == agents::DurableAgentActionState::pending ||
        action.state == agents::DurableAgentActionState::running ||
        action.state == agents::DurableAgentActionState::replay_pending) {
      ++durable_action_backlog_count_;
    }
    if (action.state == agents::DurableAgentActionState::replay_pending) {
      ++durable_replay_pending_action_count_;
    }
  }
  durable_service_evidence_count_ =
      static_cast<std::uint64_t>(catalog.retained_history.size());
}

bool ServerAgentRuntime::IsPrimaryWorkerForAgent(std::size_t worker_index,
                                                 const std::string& agent_type_id) const {
  std::lock_guard<std::mutex> guard(state_mutex_);
  for (std::size_t i = 0; i < worker_evidence_.size(); ++i) {
    if (worker_evidence_[i].agent_type_id == agent_type_id) {
      return i == worker_index;
    }
  }
  return false;
}

void ServerAgentRuntime::WriteStatusSnapshot() const {
  std::filesystem::path path;
  {
    std::lock_guard<std::mutex> guard(state_mutex_);
    path = status_path_;
  }
  if (path.empty()) {
    return;
  }
  const std::lock_guard<std::mutex> file_guard(file_mutex_);
  const auto json = StatusJson();
  const auto tmp = path.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) {
      return;
    }
    out << json;
  }
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    std::remove(tmp.c_str());
  }
}

std::string ServerAgentRuntime::StatusJson() const {
  std::lock_guard<std::mutex> guard(state_mutex_);
  std::uint64_t total_ticks = 0;
  std::uint64_t total_accepted = 0;
  std::uint64_t total_refused = 0;
  for (const auto& worker : worker_evidence_) {
    total_ticks += worker.ticks;
    total_accepted += worker.actions_accepted;
    total_refused += worker.actions_refused;
  }

  std::ostringstream out;
  out << "{\"server_agent_runtime\":{"
      << "\"started\":" << (started_ ? "true" : "false") << ','
      << "\"stopping\":" << (stopping_.load() ? "true" : "false") << ','
      << "\"database_path\":\"" << JsonEscape(database_path_) << "\","
      << "\"database_uuid\":\"" << JsonEscape(database_uuid_) << "\","
      << "\"filespace_uuid\":\"" << JsonEscape(filespace_uuid_) << "\","
      << "\"hardware_concurrency\":" << hardware_concurrency_ << ','
      << "\"effective_cpu_count\":" << effective_cpu_count_ << ','
      << "\"foreground_reserved_capacity\":" << foreground_reserved_capacity_ << ','
      << "\"background_worker_slots\":" << background_worker_slots_ << ','
      << "\"scheduler_thread_name\":\"sb-agent-sch\","
      << "\"scheduler_thread_id\":\"" << JsonEscape(scheduler_thread_id_) << "\","
      << "\"worker_wake_policy\":\"" << JsonEscape(worker_wake_policy_) << "\","
      << "\"scheduler_ticks\":" << scheduler_ticks_ << ','
      << "\"durable_catalog_generation\":" << durable_catalog_generation_ << ','
      << "\"durable_lease_count\":" << durable_lease_count_ << ','
      << "\"durable_replay_pending_lease_count\":"
      << durable_replay_pending_lease_count_ << ','
      << "\"durable_action_backlog_count\":"
      << durable_action_backlog_count_ << ','
      << "\"durable_replay_pending_action_count\":"
      << durable_replay_pending_action_count_ << ','
      << "\"durable_service_evidence_count\":" << durable_service_evidence_count_ << ','
      << "\"last_recovery_replayed_count\":"
      << last_recovery_replayed_count_ << ','
      << "\"durable_catalog_root_digest\":\"" << JsonEscape(durable_catalog_root_digest_) << "\","
      << "\"worker_thread_count\":" << worker_evidence_.size() << ','
      << "\"total_worker_ticks\":" << total_ticks << ','
      << "\"total_actions_accepted\":" << total_accepted << ','
      << "\"total_actions_refused\":" << total_refused << ','
      << "\"threads\":[";
  for (std::size_t i = 0; i < worker_evidence_.size(); ++i) {
    const auto& worker = worker_evidence_[i];
    if (i != 0) {
      out << ',';
    }
    out << "{\"name\":\"" << JsonEscape(worker.name) << "\","
        << "\"role\":\"" << JsonEscape(worker.role) << "\","
        << "\"agent_type_id\":\"" << JsonEscape(worker.agent_type_id) << "\","
        << "\"instance_uuid\":\"" << JsonEscape(worker.instance_uuid) << "\","
        << "\"lease_uuid\":\"" << JsonEscape(worker.lease_uuid) << "\","
        << "\"native_thread_id\":\"" << JsonEscape(worker.native_thread_id) << "\","
        << "\"ticks\":" << worker.ticks << ','
        << "\"actions_accepted\":" << worker.actions_accepted << ','
        << "\"actions_refused\":" << worker.actions_refused << ','
        << "\"last_action\":\"" << JsonEscape(worker.last_action) << "\","
        << "\"last_diagnostic_code\":\"" << JsonEscape(worker.last_diagnostic_code) << "\"}";
  }
  out << "]}}\n";
  return out.str();
}

}  // namespace scratchbird::server
