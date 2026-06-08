// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "transaction/transaction_api.hpp"

#include "api_diagnostics.hpp"
#include "behavior_support/api_behavior_store.hpp"
#include "crud_support/crud_store.hpp"
#include "dml/constraint_enforcement.hpp"
#include "local_transaction_store.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "transaction_inventory.hpp"
#include "transaction_lock.hpp"
#include "transaction_policy.hpp"
#include "transaction_prepare.hpp"
#include "transaction_state.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cctype>
#include <algorithm>
#include <ctime>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

// SEARCH_KEY: SB_MGA_TXN_API_BRIDGE_AUTHORITY

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::uuid::GenerateDurableEngineIdentityV7;
using scratchbird::core::uuid::UuidToString;
using scratchbird::storage::database::LoadLocalTransactionInventoryFromDatabase;
using scratchbird::storage::database::PersistLocalTransactionInventoryToDatabase;
using scratchbird::transaction::mga::CommitLocalTransaction;
using scratchbird::transaction::mga::CompletePreparedLocalTransactionCommit;
using scratchbird::transaction::mga::CompletePreparedLocalTransactionRollback;
using scratchbird::transaction::mga::LocalTransactionInventory;
using scratchbird::transaction::mga::LocalTransactionId;
using scratchbird::transaction::mga::MakeLocalTransactionId;
using scratchbird::transaction::mga::PrepareLocalTransactionDurable;
using scratchbird::transaction::mga::LocalPreparePolicy;
using scratchbird::transaction::mga::BeginLocalTransaction;
using scratchbird::transaction::mga::BeginLocalReadOnlyTransaction;
using scratchbird::transaction::mga::DefaultLocalTransactionRuntimePolicy;
using scratchbird::transaction::mga::EvaluateTransactionRuntimePolicy;
using scratchbird::transaction::mga::RollbackLocalTransaction;
using scratchbird::transaction::mga::TransactionInventoryEntry;
using scratchbird::transaction::mga::TransactionRuntimePolicy;
using scratchbird::transaction::mga::TransactionState;
using scratchbird::transaction::mga::LocalTransactionLockTable;
using scratchbird::transaction::mga::TransactionLockDecisionName;
using scratchbird::transaction::mga::TransactionLockMode;
using scratchbird::transaction::mga::TransactionLockRequest;
using scratchbird::transaction::mga::TransactionWaitPolicy;

std::uint64_t CurrentUnixMillis() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

std::mutex& TransactionInventoryGuardRegistryMutex() {
  static std::mutex mutex;
  return mutex;
}

std::map<std::string, std::shared_ptr<std::recursive_mutex>>&
TransactionInventoryGuardRegistry() {
  static std::map<std::string, std::shared_ptr<std::recursive_mutex>> registry;
  return registry;
}

std::unique_lock<std::recursive_mutex> AcquireTransactionInventoryGuard(
    const std::string& database_path) {
  std::shared_ptr<std::recursive_mutex> mutex;
  {
    std::lock_guard<std::mutex> guard(TransactionInventoryGuardRegistryMutex());
    auto& slot = TransactionInventoryGuardRegistry()[database_path];
    if (!slot) {
      slot = std::make_shared<std::recursive_mutex>();
    }
    mutex = slot;
  }
  return std::unique_lock<std::recursive_mutex>(*mutex);
}

std::string CurrentUtcTimestampText() {
  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &time);
#else
  gmtime_r(&time, &tm);
#endif
  std::ostringstream out;
  out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
  return out.str();
}

bool StartsWith(const std::string& value, const std::string& prefix) { return value.rfind(prefix, 0) == 0; }

bool IsDigits(const std::string& value) {
  if (value.empty()) { return false; }
  for (const unsigned char ch : value) {
    if (!std::isdigit(ch)) { return false; }
  }
  return true;
}

std::uint64_t ParseU64(const std::string& value) {
  std::uint64_t parsed = 0;
  for (const unsigned char ch : value) {
    parsed = parsed * 10 + static_cast<std::uint64_t>(ch - '0');
  }
  return parsed;
}

std::string NormalizedIsolation(std::string level) {
  if (level.empty()) { return "read_committed"; }
  for (char& c : level) {
    if (c == '-') { c = '_'; }
    else { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
  }
  return level;
}

std::uint64_t MaxCommittedLocalTransactionId(const scratchbird::transaction::mga::LocalTransactionInventory& inventory) {
  std::uint64_t max_committed = 0;
  for (const auto& entry : inventory.entries) {
    if ((entry.state == scratchbird::transaction::mga::TransactionState::committed ||
         entry.state == scratchbird::transaction::mga::TransactionState::archived) &&
        entry.identity.local_id.valid()) {
      max_committed = std::max(max_committed, entry.identity.local_id.value);
    }
  }
  return max_committed;
}

bool IsSupportedIsolation(const std::string& level) {
  return level == "read_committed" || level == "snapshot" || level == "repeatable_read" ||
         level == "serializable" || level == "read_consistency";
}

EngineApiDiagnostic DiagnosticFromMGA(const DiagnosticRecord& diagnostic, const std::string& fallback_code,
                                      const std::string& fallback_key) {
  std::string detail = diagnostic.remediation_hint;
  for (const auto& argument : diagnostic.arguments) {
    if (!detail.empty()) { detail += ";"; }
    detail += argument.key + "=" + argument.value;
  }
  return MakeEngineApiDiagnostic(diagnostic.diagnostic_code.empty() ? fallback_code : diagnostic.diagnostic_code,
                                 diagnostic.message_key.empty() ? fallback_key : diagnostic.message_key,
                                 detail,
                                 true);
}

template <typename TResult>
TResult MakeTxnError(const EngineRequestContext& context,
                     const std::string& operation_id,
                     EngineApiDiagnostic diagnostic) {
  TResult result;
  result.ok = false;
  result.operation_id = operation_id;
  result.diagnostics.push_back(std::move(diagnostic));
  if (context.trust_mode == EngineTrustMode::embedded_in_process) {
    result.embedded_trust_mode_observed = true;
    result.diagnostics.push_back(MakeEmbeddedTrustModeDiagnostic(operation_id));
  }
  return result;
}

template <typename TResult>
TResult MakeTxnOk(const EngineRequestContext& context, const std::string& operation_id) {
  TResult result;
  result.ok = true;
  result.operation_id = operation_id;
  result.embedded_trust_mode_observed = context.trust_mode == EngineTrustMode::embedded_in_process;
  result.evidence.push_back({"mga_authority", "durable_transaction_inventory"});
  result.evidence.push_back({"sync_policy", "strict"});
  return result;
}

std::string NormalizedOptionText(std::string value);
bool IsReadOnlyTrueText(const std::string& value);
bool IsReadOnlyFalseText(const std::string& value);

EngineApiDiagnostic ValidateTransactionPolicy(const EngineBeginTransactionRequest& request) {
  for (const auto& profile : request.transaction_policy_profile.encoded_profiles) {
    if (StartsWith(profile, "idle_timeout_ms:") && IsDigits(profile.substr(16))) { continue; }
    if (StartsWith(profile, "max_age_ms:") && IsDigits(profile.substr(11))) { continue; }
    if (StartsWith(profile, "long_running_warning_ms:") && IsDigits(profile.substr(24))) { continue; }
    if (profile == "fail_closed:true") { continue; }
    if (profile == "fail_closed:false") {
      return MakeInvalidRequestDiagnostic("transaction.begin", "transaction_policy_must_fail_closed");
    }
    if (StartsWith(profile, "dormant_") || StartsWith(profile, "dormant:")) {
      return MakeEngineApiDiagnostic("SB-SNTXN-DORMANT-UNSUPPORTED",
                                     "transaction.policy.dormant_unsupported",
                                     profile,
                                     true);
    }
    if (StartsWith(profile, "abandoned_") || StartsWith(profile, "abandoned:")) {
      return MakeEngineApiDiagnostic("SB-SNTXN-ABANDONED-UNSUPPORTED",
                                     "transaction.policy.abandoned_unsupported",
                                     profile,
                                     true);
    }
    if (StartsWith(profile, "read_only:")) {
      const std::string value = NormalizedOptionText(profile.substr(10));
      if (IsReadOnlyTrueText(value) || IsReadOnlyFalseText(value)) { continue; }
      return MakeInvalidRequestDiagnostic("transaction.begin", "unsupported_transaction_read_only_value");
    }
    if (StartsWith(profile, "transaction_read_only:")) {
      const std::string value = NormalizedOptionText(profile.substr(22));
      if (IsReadOnlyTrueText(value) || IsReadOnlyFalseText(value)) { continue; }
      return MakeInvalidRequestDiagnostic("transaction.begin", "unsupported_transaction_read_only_value");
    }
    if (StartsWith(profile, "transaction_read_mode:")) {
      const std::string value = NormalizedOptionText(profile.substr(22));
      if (value == "read_write" || value == "read_only") { continue; }
      return MakeInvalidRequestDiagnostic("transaction.begin", "unsupported_transaction_read_mode");
    }
    return MakeInvalidRequestDiagnostic("transaction.begin", "unsupported_transaction_policy_profile");
  }
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

EngineApiDiagnostic ApplyTransactionPolicyProfile(const std::string& profile,
                                                  const std::string& operation_id,
                                                  TransactionRuntimePolicy* policy) {
  if (StartsWith(profile, "idle_timeout_ms:") && IsDigits(profile.substr(16))) {
    policy->max_idle_millis = ParseU64(profile.substr(16));
    return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  }
  if (StartsWith(profile, "max_age_ms:") && IsDigits(profile.substr(11))) {
    policy->max_active_millis = ParseU64(profile.substr(11));
    return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  }
  if (StartsWith(profile, "long_running_warning_ms:") && IsDigits(profile.substr(24))) {
    return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  }
  if (profile == "fail_closed:true") {
    return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  }
  if (StartsWith(profile, "read_only:")) {
    const std::string value = NormalizedOptionText(profile.substr(10));
    if (IsReadOnlyTrueText(value) || IsReadOnlyFalseText(value)) {
      return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
    }
    return MakeInvalidRequestDiagnostic(operation_id, "unsupported_transaction_read_only_value");
  }
  if (StartsWith(profile, "transaction_read_only:")) {
    const std::string value = NormalizedOptionText(profile.substr(22));
    if (IsReadOnlyTrueText(value) || IsReadOnlyFalseText(value)) {
      return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
    }
    return MakeInvalidRequestDiagnostic(operation_id, "unsupported_transaction_read_only_value");
  }
  if (StartsWith(profile, "transaction_read_mode:")) {
    const std::string value = NormalizedOptionText(profile.substr(22));
    if (value == "read_write" || value == "read_only") {
      return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
    }
    return MakeInvalidRequestDiagnostic(operation_id, "unsupported_transaction_read_mode");
  }
  if (profile == "fail_closed:false") {
    return MakeInvalidRequestDiagnostic(operation_id, "transaction_policy_must_fail_closed");
  }
  if (StartsWith(profile, "dormant_") || StartsWith(profile, "dormant:")) {
    return MakeEngineApiDiagnostic("SB-SNTXN-DORMANT-UNSUPPORTED",
                                   "transaction.policy.dormant_unsupported",
                                   profile,
                                   true);
  }
  if (StartsWith(profile, "abandoned_") || StartsWith(profile, "abandoned:")) {
    return MakeEngineApiDiagnostic("SB-SNTXN-ABANDONED-UNSUPPORTED",
                                   "transaction.policy.abandoned_unsupported",
                                   profile,
                                   true);
  }
  return MakeInvalidRequestDiagnostic(operation_id, "unsupported_transaction_policy_profile");
}

EngineApiDiagnostic BuildRuntimePolicy(const EngineProfileSet& profiles,
                                       const std::string& operation_id,
                                       TransactionRuntimePolicy* policy) {
  *policy = DefaultLocalTransactionRuntimePolicy();
  for (const auto& profile : profiles.encoded_profiles) {
    const auto status = ApplyTransactionPolicyProfile(profile, operation_id, policy);
    if (status.error) { return status; }
  }
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

std::optional<TransactionInventoryEntry> FindTransaction(const scratchbird::transaction::mga::LocalTransactionInventory& inventory,
                                                         LocalTransactionId local_id);

template <typename TResult>
std::optional<TResult> EnforceRuntimePolicyForExistingTransaction(const EngineApiRequest& request,
                                                                 const std::string& operation_id,
                                                                 const scratchbird::transaction::mga::LocalTransactionInventory& inventory,
                                                                 bool allow_rollback_resolution) {
  TransactionRuntimePolicy policy;
  const auto profile_status = BuildRuntimePolicy(request.policy_profile, operation_id, &policy);
  if (profile_status.error) {
    return MakeTxnError<TResult>(request.context, operation_id, profile_status);
  }
  const auto entry = FindTransaction(inventory, MakeLocalTransactionId(request.context.local_transaction_id));
  if (!entry.has_value()) {
    return MakeTxnError<TResult>(
        request.context,
        operation_id,
        MakeInvalidRequestDiagnostic(operation_id, "local_transaction_id_not_found"));
  }
  const auto evaluated = EvaluateTransactionRuntimePolicy(*entry,
                                                          policy,
                                                          CurrentUnixMillis(),
                                                          entry->begin_unix_epoch_millis);
  if (!evaluated.ok() && !allow_rollback_resolution) {
    return MakeTxnError<TResult>(
        request.context,
        operation_id,
        DiagnosticFromMGA(evaluated.diagnostic,
                          "SB-SNTXN-RUNTIME-POLICY-VIOLATION",
                          "transaction.policy.runtime_violation"));
  }
  return std::nullopt;
}

std::optional<TransactionInventoryEntry> FindTransaction(const scratchbird::transaction::mga::LocalTransactionInventory& inventory,
                                                         LocalTransactionId local_id) {
  for (const auto& entry : inventory.entries) {
    if (entry.identity.local_id.value == local_id.value) { return entry; }
  }
  return std::nullopt;
}

bool ContainsTransactionUuid(const LocalTransactionInventory& inventory, const TypedUuid& transaction_uuid) {
  for (const auto& entry : inventory.entries) {
    if (entry.identity.transaction_uuid.value == transaction_uuid.value) { return true; }
  }
  return false;
}

EngineApiDiagnostic ValidateDatabasePath(const EngineRequestContext& context, const std::string& operation_id) {
  if (context.database_path.empty()) {
    return MakeInvalidRequestDiagnostic(operation_id, "database_path_required_for_durable_mga_transaction_authority");
  }
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

bool ContextHasTraceTag(const EngineRequestContext& context, const std::string& tag) {
  for (const auto& candidate : context.trace_tags) {
    if (candidate == tag) return true;
  }
  return false;
}

bool RequestOptionPresent(const EngineApiRequest& request, const std::string& option) {
  for (const auto& candidate : request.option_envelopes) {
    if (candidate == option) return true;
  }
  return false;
}

std::string RequestOptionValue(const EngineApiRequest& request, const std::string& prefix) {
  for (const auto& candidate : request.option_envelopes) {
    if (StartsWith(candidate, prefix)) return candidate.substr(prefix.size());
  }
  return {};
}

bool RequestOptionBool(const EngineApiRequest& request, const std::string& prefix, bool fallback = false) {
  for (const auto& candidate : request.option_envelopes) {
    if (!StartsWith(candidate, prefix)) continue;
    std::string value = candidate.substr(prefix.size());
    for (char& ch : value) {
      ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    if (value == "1" || value == "true" || value == "yes" || value == "on") return true;
    if (value == "0" || value == "false" || value == "no" || value == "off") return false;
  }
  return fallback;
}

std::string ProfileValue(const EngineProfileSet& profiles, const std::string& prefix) {
  for (const auto& candidate : profiles.encoded_profiles) {
    if (StartsWith(candidate, prefix)) return candidate.substr(prefix.size());
  }
  return {};
}

std::string NormalizedOptionText(std::string value) {
  for (char& ch : value) {
    if (ch == '-') {
      ch = '_';
    } else {
      ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
  }
  return value;
}

bool IsReadOnlyTrueText(const std::string& value) {
  return value == "1" || value == "true" || value == "yes" || value == "on";
}

bool IsReadOnlyFalseText(const std::string& value) {
  return value == "0" || value == "false" || value == "no" || value == "off";
}

struct TransactionReadModeSettings {
  std::string read_mode = "read_write";
  bool read_only = false;
  EngineApiDiagnostic diagnostic =
      MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
};

TransactionReadModeSettings ResolveTransactionReadModeSettings(
    const EngineApiRequest& request,
    const EngineProfileSet* profiles,
    const std::string& operation_id) {
  TransactionReadModeSettings settings;
  std::string read_mode =
      NormalizedOptionText(RequestOptionValue(request, "transaction_read_mode:"));
  if (read_mode.empty() && profiles != nullptr) {
    read_mode = NormalizedOptionText(ProfileValue(*profiles, "transaction_read_mode:"));
  }

  std::string read_only_text =
      NormalizedOptionText(RequestOptionValue(request, "transaction_read_only:"));
  if (read_only_text.empty() && profiles != nullptr) {
    read_only_text = NormalizedOptionText(ProfileValue(*profiles, "transaction_read_only:"));
  }
  if (read_only_text.empty() && profiles != nullptr) {
    read_only_text = NormalizedOptionText(ProfileValue(*profiles, "read_only:"));
  }

  bool read_only = request.context.read_only_mode;
  if (!read_only_text.empty()) {
    const bool explicit_read_only = IsReadOnlyTrueText(read_only_text);
    const bool explicit_read_write = IsReadOnlyFalseText(read_only_text);
    if (!explicit_read_only && !explicit_read_write) {
      settings.diagnostic = MakeInvalidRequestDiagnostic(operation_id, "unsupported_transaction_read_only_value");
      return settings;
    }
    read_only = explicit_read_only;
    if (read_mode.empty()) {
      read_mode = read_only ? "read_only" : "read_write";
    }
    if ((read_mode == "read_only" && !explicit_read_only) ||
        (read_mode == "read_write" && !explicit_read_write)) {
      settings.diagnostic = MakeInvalidRequestDiagnostic(operation_id, "transaction_read_mode_conflict");
      return settings;
    }
  }

  if (read_mode.empty()) {
    read_mode = read_only ? "read_only" : "read_write";
  }
  if (read_mode != "read_write" && read_mode != "read_only") {
    settings.diagnostic = MakeInvalidRequestDiagnostic(operation_id, "unsupported_transaction_read_mode");
    return settings;
  }
  if (request.context.read_only_mode && (read_mode == "read_write" || !read_only)) {
    settings.diagnostic = MakeInvalidRequestDiagnostic(operation_id, "transaction_read_mode_conflict");
    return settings;
  }

  if (read_only_text.empty()) {
    read_only = read_mode == "read_only";
  } else {
    read_only = read_only || read_mode == "read_only";
  }

  settings.read_mode = read_mode;
  settings.read_only = read_only;
  return settings;
}

LocalTransactionLockTable& NamedAdvisoryLockTable() {
  static LocalTransactionLockTable table;
  return table;
}

EngineApiDiagnostic DblcTransactionAdmissionDenied(std::string detail);

EngineApiDiagnostic LockNotAvailableDiagnostic(const std::string& detail) {
  return MakeEngineApiDiagnostic("TCL.LOCK_NOT_AVAILABLE",
                                 "transaction.lock.not_available",
                                 detail,
                                 true);
}

EngineApiDiagnostic LockTimeoutDiagnostic(const std::string& detail) {
  return MakeEngineApiDiagnostic("TCL.LOCK_TIMEOUT",
                                 "transaction.lock.timeout",
                                 detail,
                                 true);
}

EngineApiDiagnostic ClusterAuthorityRequiredDiagnostic(const std::string& detail) {
  return MakeEngineApiDiagnostic("CLUSTER.AUTHORITY_REQUIRED",
                                 "cluster.authority_required",
                                 detail,
                                 true);
}

bool LockContextRequestsClusterScope(const EngineApiRequest& request) {
  const std::string scope = NormalizedOptionText(RequestOptionValue(request, "lock_scope:"));
  return scope == "cluster" ||
         RequestOptionBool(request, "cluster_scope_requested:", false) ||
         RequestOptionBool(request, "requires_cluster_authority:", false) ||
         ContextHasTraceTag(request.context, "cluster_scope_requested");
}

EngineApiDiagnostic ValidateLockControlContext(const EngineApiRequest& request,
                                               const std::string& operation_id) {
  if (!request.context.security_context_present) {
    return DblcTransactionAdmissionDenied("security_context_required");
  }
  if (request.context.session_uuid.canonical.empty() ||
      request.context.principal_uuid.canonical.empty() ||
      request.context.database_uuid.canonical.empty()) {
    return DblcTransactionAdmissionDenied("session_principal_database_identity_required");
  }
  if (request.context.local_transaction_id == 0) {
    return MakeInvalidRequestDiagnostic(operation_id, "local_transaction_id_required");
  }
  if (LockContextRequestsClusterScope(request) &&
      !request.context.cluster_authority_available) {
    return ClusterAuthorityRequiredDiagnostic(
        "cluster_scope_named_locks_require_cluster_authority");
  }
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

template <typename TResult>
void AddLockInvariantEvidence(TResult* result) {
  result->evidence.push_back({"mga_visibility_impact", "false"});
  result->evidence.push_back({"transaction_finality_impact", "false"});
  result->evidence.push_back({"cleanup_horizon_pinned", "false"});
  result->evidence.push_back({"parser_finality", "false"});
  result->evidence.push_back({"parser_executes_sql", "false"});
  result->evidence.push_back({"lock_policy_matrix", "LOCK_SURFACE_MGA_POLICY_MATRIX.csv"});
}

template <typename TResult>
TResult MakeLockError(const EngineRequestContext& context,
                      const std::string& operation_id,
                      EngineApiDiagnostic diagnostic,
                      std::string lock_surface,
                      std::string policy) {
  TResult result;
  const bool cluster_authority_required =
      diagnostic.code == "CLUSTER.AUTHORITY_REQUIRED";
  result.ok = false;
  result.operation_id = operation_id;
  result.diagnostics.push_back(std::move(diagnostic));
  result.embedded_trust_mode_observed =
      context.trust_mode == EngineTrustMode::embedded_in_process;
  result.cluster_authority_required = cluster_authority_required;
  AddLockInvariantEvidence(&result);
  result.evidence.push_back({"lock_surface", std::move(lock_surface)});
  result.evidence.push_back({"lock_policy", std::move(policy)});
  return result;
}

template <typename TResult>
void AddLockBehaviorRow(TResult* result,
                        std::string operation_id,
                        std::string lock_surface,
                        std::string lock_policy,
                        std::string outcome,
                        std::string resource_key = {}) {
  AddApiBehaviorRow(result,
                    {{"operation_id", std::move(operation_id)},
                     {"lock_surface", std::move(lock_surface)},
                     {"lock_policy", std::move(lock_policy)},
                     {"outcome", std::move(outcome)},
                     {"resource_key", std::move(resource_key)},
                     {"mga_visibility_impact", "false"},
                     {"transaction_finality_impact", "false"},
                     {"parser_finality", "false"}});
}

std::uint64_t ParseU64OrZero(const std::string& value) {
  if (!IsDigits(value)) return 0;
  return ParseU64(value);
}

std::string DecodeSbsqlStringLiteral(std::string value) {
  if (value.size() < 2 || value.front() != '\'' || value.back() != '\'') {
    return value;
  }
  std::string decoded;
  decoded.reserve(value.size() - 2);
  for (std::size_t index = 1; index + 1 < value.size(); ++index) {
    if (value[index] == '\'' && index + 1 < value.size() - 1 &&
        value[index + 1] == '\'') {
      decoded.push_back('\'');
      ++index;
    } else {
      decoded.push_back(value[index]);
    }
  }
  return decoded;
}

std::string NamedLockResourceKey(const EngineApiRequest& request) {
  std::string descriptor = RequestOptionValue(request, "lock_descriptor:");
  if (descriptor.empty()) descriptor = RequestOptionValue(request, "lock_target:");
  if (descriptor.empty()) descriptor = RequestOptionValue(request, "lock_name:");
  descriptor = DecodeSbsqlStringLiteral(std::move(descriptor));
  if (descriptor.empty()) return {};
  std::string database = request.context.database_uuid.canonical;
  if (database.empty()) database = "database:unknown";
  return "named:" + database + ":" + descriptor;
}

std::string TableLockModeForRequest(const EngineApiRequest& request) {
  std::string mode = NormalizedOptionText(RequestOptionValue(request, "lock_mode:"));
  if (mode.empty()) mode = NormalizedOptionText(RequestOptionValue(request, "table_lock_mode:"));
  if (mode.empty()) mode = "read_or_share";
  if (mode == "read" || mode == "share" || mode == "shared" ||
      mode == "read_or_share") {
    return "read_or_share";
  }
  if (mode == "write" || mode == "exclusive" || mode == "write_or_exclusive") {
    return "write_or_exclusive";
  }
  return mode;
}

TransactionWaitPolicy WaitPolicyForRequest(const EngineApiRequest& request) {
  TransactionWaitPolicy policy;
  policy.timeout_millis =
      ParseU64OrZero(RequestOptionValue(request, "lock_timeout_millis:"));
  policy.no_wait = RequestOptionBool(request, "nowait:", policy.timeout_millis == 0);
  const std::uint64_t now = CurrentUnixMillis();
  policy.wait_start_unix_epoch_millis = now;
  policy.now_unix_epoch_millis = now;
  return policy;
}

std::string LockSurfaceOption(const EngineApiRequest& request,
                              const std::string& fallback) {
  std::string surface = RequestOptionValue(request, "lock_surface:");
  if (surface.empty()) surface = fallback;
  return surface;
}

bool IsWriteOrExclusiveTableMode(const std::string& mode) {
  return mode == "write_or_exclusive" || mode == "write" || mode == "exclusive";
}

bool EngineOwnedTableFenceAuthorized(const EngineLockTableRequest& request) {
  const bool requested =
      RequestOptionBool(request, "engine_owned_admission_fence:", false) ||
      RequestOptionBool(request, "ddl_admin_admission_fence:", false) ||
      RequestOptionBool(request, "migration_admission_fence:", false) ||
      ContextHasTraceTag(request.context, "engine_owned_admission_fence");
  if (!requested) return false;
  return ContextHasTraceTag(request.context, "right:CATALOG_MUTATE") ||
         ContextHasTraceTag(request.context, "right:DDL_ADMIN") ||
         ContextHasTraceTag(request.context, "right:MIGRATION_ADMIN") ||
         ContextHasTraceTag(request.context,
                            "engine_owned_ddl_admission_fence_authorized") ||
         ContextHasTraceTag(request.context,
                            "engine_owned_migration_admission_fence_authorized");
}

template <typename TResult>
void AddNamedLockDecisionEvidence(TResult* result,
                                  const std::string& resource_key,
                                  const std::string& decision,
                                  std::uint64_t blocking_transaction = 0) {
  result->evidence.push_back({"lock_decision", decision});
  result->evidence.push_back({"lock_resource_key", resource_key});
  if (blocking_transaction != 0) {
    result->evidence.push_back({"blocking_local_transaction_id",
                                std::to_string(blocking_transaction)});
  }
}

std::string NamedLockOutcomeForDecision(const std::string& decision) {
  if (decision == "granted" || decision == "already_owned") return "advisory_lock_acquired";
  if (decision == "timeout" || decision == "wait_required") return "advisory_lock_wait_refused";
  if (decision == "admission_refused") return "advisory_lock_admission_refused";
  if (decision == "deadlock_detected") return "advisory_lock_deadlock_detected";
  return "advisory_lock_invalid_request";
}

EngineApiDiagnostic DblcTransactionAdmissionDenied(std::string detail) {
  return MakeEngineApiDiagnostic("ENGINE.DBLC_TRANSACTION_ADMISSION_DENIED",
                                 "engine.dblc.transaction_admission_denied",
                                 std::move(detail),
                                 true);
}

EngineApiDiagnostic DblcStandaloneClusterFailClosed(std::string detail) {
  return MakeEngineApiDiagnostic("ENGINE.DBLC_STANDALONE_CLUSTER_FAIL_CLOSED",
                                 "engine.dblc.standalone_cluster_fail_closed",
                                 std::move(detail),
                                 true);
}

EngineApiDiagnostic ValidateBeginTransactionAdmission(const EngineBeginTransactionRequest& request) {
  const auto& context = request.context;
  if (!context.security_context_present) {
    return DblcTransactionAdmissionDenied("security_context_required");
  }
  if (context.session_uuid.canonical.empty() ||
      context.principal_uuid.canonical.empty() ||
      context.database_uuid.canonical.empty()) {
    return DblcTransactionAdmissionDenied("session_principal_database_identity_required");
  }
  if (context.local_transaction_id != 0 || !context.transaction_uuid.canonical.empty()) {
    return DblcTransactionAdmissionDenied("active_transaction_already_bound");
  }
  if (context.catalog_generation_id == 0 ||
      context.security_epoch == 0 ||
      context.resource_epoch == 0 ||
      context.name_resolution_epoch == 0) {
    return DblcTransactionAdmissionDenied("authority_generation_required");
  }
  if ((RequestOptionBool(request, "requires_cluster_authority:", false) ||
       RequestOptionBool(request, "cluster_structures_present:", false) ||
       RequestOptionPresent(request, "cluster_transaction_mapping_required") ||
       ContextHasTraceTag(context, "cluster_structures_present")) &&
      !context.cluster_authority_available) {
    return DblcStandaloneClusterFailClosed("cluster_authority_unavailable");
  }
  if (RequestOptionBool(request, "maintenance_mode:", false) ||
      ContextHasTraceTag(context, "maintenance_mode")) {
    return DblcTransactionAdmissionDenied("maintenance_transaction_admission_fenced");
  }
  if (RequestOptionBool(request, "restricted_open:", false) ||
      ContextHasTraceTag(context, "restricted_open")) {
    return DblcTransactionAdmissionDenied("restricted_open_transaction_admission_fenced");
  }
  if (RequestOptionBool(request, "write_admission_fenced:", false) ||
      ContextHasTraceTag(context, "write_admission_fenced")) {
    return DblcTransactionAdmissionDenied("write_admission_fenced");
  }
  if (RequestOptionBool(request, "filespace_available:", true) == false ||
      ContextHasTraceTag(context, "filespace_unavailable")) {
    return DblcTransactionAdmissionDenied("filespace_unavailable");
  }
  if (RequestOptionBool(request, "memory_admission:", true) == false ||
      ContextHasTraceTag(context, "memory_admission_denied")) {
    return DblcTransactionAdmissionDenied("memory_admission_denied");
  }
  if (RequestOptionBool(request, "lock_admission:", true) == false ||
      ContextHasTraceTag(context, "lock_admission_denied")) {
    return DblcTransactionAdmissionDenied("lock_admission_denied");
  }
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

}  // namespace

EngineSetTransactionCharacteristicsResult EngineSetTransactionCharacteristics(
    const EngineSetTransactionCharacteristicsRequest& request) {
  const std::string operation_id = "transaction.set_characteristics";
  const auto path_status = ValidateDatabasePath(request.context, operation_id);
  if (path_status.error) {
    return MakeTxnError<EngineSetTransactionCharacteristicsResult>(
        request.context, operation_id, path_status);
  }
  if (!request.context.security_context_present) {
    return MakeTxnError<EngineSetTransactionCharacteristicsResult>(
        request.context,
        operation_id,
        DblcTransactionAdmissionDenied("security_context_required"));
  }
  if (request.context.session_uuid.canonical.empty() ||
      request.context.principal_uuid.canonical.empty() ||
      request.context.database_uuid.canonical.empty()) {
    return MakeTxnError<EngineSetTransactionCharacteristicsResult>(
        request.context,
        operation_id,
        DblcTransactionAdmissionDenied("session_principal_database_identity_required"));
  }
  if (request.context.local_transaction_id != 0 ||
      !request.context.transaction_uuid.canonical.empty()) {
    return MakeTxnError<EngineSetTransactionCharacteristicsResult>(
        request.context,
        operation_id,
        DblcTransactionAdmissionDenied("active_transaction_already_bound"));
  }
  const auto read_settings = ResolveTransactionReadModeSettings(
      request, nullptr, operation_id);
  if (read_settings.diagnostic.error) {
    return MakeTxnError<EngineSetTransactionCharacteristicsResult>(
        request.context,
        operation_id,
        read_settings.diagnostic);
  }

  std::string isolation = RequestOptionValue(request, "transaction_isolation_level:");
  if (isolation.empty()) isolation = request.context.transaction_isolation_level;
  isolation = NormalizedIsolation(isolation);
  if (!IsSupportedIsolation(isolation)) {
    return MakeTxnError<EngineSetTransactionCharacteristicsResult>(
        request.context,
        operation_id,
        MakeInvalidRequestDiagnostic(operation_id, "unsupported_isolation_level"));
  }

  auto result = MakeTxnOk<EngineSetTransactionCharacteristicsResult>(
      request.context, operation_id);
  AddApiBehaviorRow(&result,
                     {{"transaction_characteristics", "session_defaults"},
                     {"transaction_read_mode", read_settings.read_mode},
                     {"transaction_read_only", read_settings.read_only ? "true" : "false"},
                     {"transaction_isolation_level", isolation}});
  result.evidence.push_back({"transaction_characteristics", "session_defaults_applied"});
  result.evidence.push_back({"transaction_read_mode", read_settings.read_mode});
  result.evidence.push_back({"transaction_read_only", read_settings.read_only ? "true" : "false"});
  result.evidence.push_back({"transaction_isolation_level", isolation});
  result.evidence.push_back({"mga_authority", "session_default_only_no_finality"});
  result.evidence.push_back({"parser_finality", "false"});
  if (request.context.local_transaction_id != 0 ||
      !request.context.transaction_uuid.canonical.empty()) {
    result.evidence.push_back({"active_transaction_context", "preserved"});
  }
  if (const auto surface_id = RequestOptionValue(request, "sbsfc080_surface_id:");
      !surface_id.empty()) {
    const std::string evidence_kind =
        RequestOptionValue(request, "sbsfc080_runtime_evidence_kind:");
    const std::string evidence_id =
        RequestOptionValue(request, "sbsfc080_runtime_evidence_id:");
    result.evidence.push_back(
        {evidence_kind.empty() ? "mga_transaction_authority_route" : evidence_kind,
         evidence_id.empty() ? surface_id : evidence_id});
    result.evidence.push_back({"sbsfc080_surface", surface_id});
    result.evidence.push_back({"parser_executes_sql", "false"});
    result.evidence.push_back({"cluster_provider_dispatch", "false"});
    result.evidence.push_back({"private_cluster_execution", "false"});
    result.evidence.push_back({"wal_recovery_authority", "false"});
  }
  return result;
}

EngineBeginTransactionResult EngineBeginTransaction(const EngineBeginTransactionRequest& request) {
  const std::string operation_id = "transaction.begin";
  const auto path_status = ValidateDatabasePath(request.context, operation_id);
  if (path_status.error) {
    return MakeTxnError<EngineBeginTransactionResult>(request.context, operation_id, path_status);
  }
  const auto admission_status = ValidateBeginTransactionAdmission(request);
  if (admission_status.error) {
    return MakeTxnError<EngineBeginTransactionResult>(request.context, operation_id, admission_status);
  }
  const std::string isolation = NormalizedIsolation(request.isolation_level);
  if (!IsSupportedIsolation(isolation)) {
    return MakeTxnError<EngineBeginTransactionResult>(
        request.context,
        operation_id,
        MakeInvalidRequestDiagnostic(operation_id, "unsupported_isolation_level"));
  }
  const auto read_settings = ResolveTransactionReadModeSettings(
      request, &request.transaction_policy_profile, operation_id);
  if (read_settings.diagnostic.error) {
    return MakeTxnError<EngineBeginTransactionResult>(
        request.context, operation_id, read_settings.diagnostic);
  }
  const auto policy_status = ValidateTransactionPolicy(request);
  if (policy_status.error) {
    return MakeTxnError<EngineBeginTransactionResult>(request.context, operation_id, policy_status);
  }
  TransactionRuntimePolicy begin_policy;
  const auto begin_policy_status = BuildRuntimePolicy(request.transaction_policy_profile, operation_id, &begin_policy);
  if (begin_policy_status.error) {
    return MakeTxnError<EngineBeginTransactionResult>(request.context, operation_id, begin_policy_status);
  }

  const auto inventory_guard =
      AcquireTransactionInventoryGuard(request.context.database_path);
  const auto loaded = LoadLocalTransactionInventoryFromDatabase(request.context.database_path);
  if (!loaded.ok()) {
    return MakeTxnError<EngineBeginTransactionResult>(
        request.context,
        operation_id,
        DiagnosticFromMGA(loaded.diagnostic,
                          "SB-MGA-TXN-INV-LOAD-FAILED",
                          "mga.transaction_inventory.load_failed"));
  }

  const std::uint64_t begin_unix_epoch_millis = CurrentUnixMillis();
  std::optional<TypedUuid> generated_transaction_uuid;
  DiagnosticRecord generation_diagnostic;
  const std::uint64_t generation_attempts =
      static_cast<std::uint64_t>(loaded.inventory.entries.size()) + 8U;
  for (std::uint64_t attempt = 0; attempt < generation_attempts; ++attempt) {
    const auto generated = GenerateDurableEngineIdentityV7(
        UuidKind::transaction,
        begin_unix_epoch_millis + loaded.inventory.next_local_transaction_id + attempt);
    if (!generated.ok()) {
      generation_diagnostic = generated.diagnostic;
      break;
    }
    if (!ContainsTransactionUuid(loaded.inventory, generated.value)) {
      generated_transaction_uuid = generated.value;
      break;
    }
  }
  if (!generated_transaction_uuid.has_value()) {
    if (generation_diagnostic.diagnostic_code.empty()) {
      return MakeTxnError<EngineBeginTransactionResult>(
          request.context,
          operation_id,
          MakeInvalidRequestDiagnostic(operation_id, "transaction_uuid_generation_collision"));
    }
    return MakeTxnError<EngineBeginTransactionResult>(
        request.context,
        operation_id,
        DiagnosticFromMGA(generation_diagnostic,
                          "SB-MGA-TXN-LIFE-UUID-FAILED",
                          "mga.transaction_lifecycle.uuid_generation_failed"));
  }

  const auto transaction_timestamp = CurrentUtcTimestampText();
  const auto begun = read_settings.read_only
                         ? BeginLocalReadOnlyTransaction(loaded.inventory,
                                                         *generated_transaction_uuid,
                                                         begin_unix_epoch_millis)
                         : BeginLocalTransaction(loaded.inventory,
                                                 *generated_transaction_uuid,
                                                 begin_unix_epoch_millis);
  if (!begun.ok()) {
    return MakeTxnError<EngineBeginTransactionResult>(
        request.context,
        operation_id,
        DiagnosticFromMGA(begun.diagnostic,
                          "SB-MGA-TXN-LIFE-BEGIN-FAILED",
                          "mga.transaction_lifecycle.begin_failed"));
  }

  const auto persisted = PersistLocalTransactionInventoryToDatabase(request.context.database_path, begun.inventory);
  if (!persisted.ok()) {
    return MakeTxnError<EngineBeginTransactionResult>(
        request.context,
        operation_id,
        DiagnosticFromMGA(persisted.diagnostic,
                          "SB-MGA-TXN-INV-PERSIST-FAILED",
                          "mga.transaction_inventory.persist_failed"));
  }

  auto result = MakeTxnOk<EngineBeginTransactionResult>(request.context, operation_id);
  result.transaction_uuid.canonical = UuidToString(begun.entry.identity.transaction_uuid.value);
  result.local_transaction_id = begun.entry.identity.local_id.value;
  static_cast<EngineApiResult&>(result).transaction_uuid = result.transaction_uuid;
  static_cast<EngineApiResult&>(result).local_transaction_id = result.local_transaction_id;
  result.isolation_level = isolation;
  result.read_mode = read_settings.read_mode;
  result.read_only = read_settings.read_only;
  result.snapshot_visible_through_local_transaction_id = MaxCommittedLocalTransactionId(loaded.inventory);
  result.evidence.push_back({"transaction_state", read_settings.read_only ? "read_only_active" : "active"});
  result.evidence.push_back({"transaction_timestamp", transaction_timestamp});
  result.evidence.push_back({"isolation_level", isolation});
  result.evidence.push_back({"transaction_read_mode", read_settings.read_mode});
  result.evidence.push_back({"transaction_read_only", read_settings.read_only ? "true" : "false"});
  result.evidence.push_back({"snapshot_visible_through_local_transaction_id",
                             std::to_string(result.snapshot_visible_through_local_transaction_id)});
  result.evidence.push_back({"runtime_policy", "fail_closed"});
  result.evidence.push_back({"transaction_admission", "engine_mga_admitted"});
  result.evidence.push_back({"parser_finality", "false"});
  if (read_settings.read_only) {
    result.evidence.push_back({"read_only_write_guard", "mga_transaction_state"});
  }
  result.evidence.push_back({"max_active_millis", std::to_string(begin_policy.max_active_millis)});
  result.evidence.push_back({"max_idle_millis", std::to_string(begin_policy.max_idle_millis)});
  return result;
}

EngineCommitTransactionResult EngineCommitTransaction(const EngineCommitTransactionRequest& request) {
  const std::string operation_id = "transaction.commit";
  const auto path_status = ValidateDatabasePath(request.context, operation_id);
  if (path_status.error) {
    return MakeTxnError<EngineCommitTransactionResult>(request.context, operation_id, path_status);
  }
  if (request.context.local_transaction_id == 0) {
    return MakeTxnError<EngineCommitTransactionResult>(
        request.context,
        operation_id,
        MakeInvalidRequestDiagnostic(operation_id, "local_transaction_id_required"));
  }
  const auto inventory_guard =
      AcquireTransactionInventoryGuard(request.context.database_path);
  const auto loaded = LoadLocalTransactionInventoryFromDatabase(request.context.database_path);
  if (!loaded.ok()) {
    return MakeTxnError<EngineCommitTransactionResult>(request.context, operation_id,
        DiagnosticFromMGA(loaded.diagnostic, "SB-MGA-TXN-INV-LOAD-FAILED", "mga.transaction_inventory.load_failed"));
  }
  if (auto policy_error = EnforceRuntimePolicyForExistingTransaction<EngineCommitTransactionResult>(
          request, operation_id, loaded.inventory, false)) {
    return *policy_error;
  }
  const auto committing_entry = FindTransaction(loaded.inventory,
                                                MakeLocalTransactionId(request.context.local_transaction_id));
  if (!committing_entry.has_value()) {
    return MakeTxnError<EngineCommitTransactionResult>(
        request.context,
        operation_id,
        MakeInvalidRequestDiagnostic(operation_id, "local_transaction_id_not_found"));
  }
  const bool read_only_commit = committing_entry->state == TransactionState::read_only_active;
  std::uint64_t temporary_deleted_rows = 0;
  if (!read_only_commit) {
    const auto deferred_constraints = ValidateDeferredTransactionConstraints(request.context);
    if (deferred_constraints.error) {
      return MakeTxnError<EngineCommitTransactionResult>(request.context, operation_id, deferred_constraints);
    }
    const auto temporary_cleanup = ApplyMgaTemporaryOnCommitActions(request.context,
                                                                   request.context.local_transaction_id,
                                                                   &temporary_deleted_rows);
    if (temporary_cleanup.error) {
      return MakeTxnError<EngineCommitTransactionResult>(request.context, operation_id, temporary_cleanup);
    }
  }
  const auto committed = CommitLocalTransaction(loaded.inventory, MakeLocalTransactionId(request.context.local_transaction_id), CurrentUnixMillis());
  if (!committed.ok()) {
    return MakeTxnError<EngineCommitTransactionResult>(request.context, operation_id,
        DiagnosticFromMGA(committed.diagnostic, "SB-MGA-TXN-LIFE-COMMIT-FAILED", "mga.transaction_lifecycle.commit_failed"));
  }
  const auto persisted = PersistLocalTransactionInventoryToDatabase(request.context.database_path, committed.inventory);
  if (!persisted.ok()) {
    return MakeTxnError<EngineCommitTransactionResult>(request.context, operation_id,
        DiagnosticFromMGA(persisted.diagnostic, "SB-MGA-TXN-INV-PERSIST-FAILED", "mga.transaction_inventory.persist_failed"));
  }
  // DPC_DEFERRED_INDEX_WRITE_PATH
  const auto committed_deltas = CommitMgaSecondaryIndexDeltaLedgerTransaction(
      request.context,
      request.context.local_transaction_id);
  if (committed_deltas.error) {
    return MakeTxnError<EngineCommitTransactionResult>(
        request.context,
        operation_id,
        committed_deltas);
  }
  auto result = MakeTxnOk<EngineCommitTransactionResult>(request.context, operation_id);
  result.local_transaction_id = committed.entry.identity.local_id.value;
  result.transaction_uuid.canonical = UuidToString(committed.entry.identity.transaction_uuid.value);
  static_cast<EngineApiResult&>(result).local_transaction_id = result.local_transaction_id;
  static_cast<EngineApiResult&>(result).transaction_uuid = result.transaction_uuid;
  result.evidence.push_back({"transaction_state", "committed"});
  result.evidence.push_back({"transaction_read_only", read_only_commit ? "true" : "false"});
  result.evidence.push_back({"temporary_on_commit_deleted_rows", std::to_string(temporary_deleted_rows)});
  const auto released_locks =
      NamedAdvisoryLockTable().ReleaseAll(MakeLocalTransactionId(request.context.local_transaction_id));
  result.evidence.push_back({"transaction_advisory_locks_released",
                             std::to_string(released_locks)});
  if (read_only_commit) {
    result.evidence.push_back({"read_only_commit_cleanup", "skipped_no_mutation_authority"});
  }
  return result;
}

EngineRollbackTransactionResult EngineRollbackTransaction(const EngineRollbackTransactionRequest& request) {
  const std::string operation_id = "transaction.rollback";
  const auto path_status = ValidateDatabasePath(request.context, operation_id);
  if (path_status.error) {
    return MakeTxnError<EngineRollbackTransactionResult>(request.context, operation_id, path_status);
  }
  if (request.context.local_transaction_id == 0) {
    return MakeTxnError<EngineRollbackTransactionResult>(
        request.context,
        operation_id,
        MakeInvalidRequestDiagnostic(operation_id, "local_transaction_id_required"));
  }
  const auto inventory_guard =
      AcquireTransactionInventoryGuard(request.context.database_path);
  const auto loaded = LoadLocalTransactionInventoryFromDatabase(request.context.database_path);
  if (!loaded.ok()) {
    return MakeTxnError<EngineRollbackTransactionResult>(request.context, operation_id,
        DiagnosticFromMGA(loaded.diagnostic, "SB-MGA-TXN-INV-LOAD-FAILED", "mga.transaction_inventory.load_failed"));
  }
  if (auto policy_error = EnforceRuntimePolicyForExistingTransaction<EngineRollbackTransactionResult>(
          request, operation_id, loaded.inventory, true)) {
    return *policy_error;
  }
  const auto rollback_entry = FindTransaction(loaded.inventory,
                                              MakeLocalTransactionId(request.context.local_transaction_id));
  if (!rollback_entry.has_value()) {
    return MakeTxnError<EngineRollbackTransactionResult>(
        request.context,
        operation_id,
        MakeInvalidRequestDiagnostic(operation_id, "local_transaction_id_not_found"));
  }
  const bool read_only_rollback = rollback_entry->state == TransactionState::read_only_active;
  const auto rolled_back = RollbackLocalTransaction(loaded.inventory, MakeLocalTransactionId(request.context.local_transaction_id), CurrentUnixMillis());
  if (!rolled_back.ok()) {
    return MakeTxnError<EngineRollbackTransactionResult>(request.context, operation_id,
        DiagnosticFromMGA(rolled_back.diagnostic, "SB-MGA-TXN-LIFE-ROLLBACK-FAILED", "mga.transaction_lifecycle.rollback_failed"));
  }
  const auto persisted = PersistLocalTransactionInventoryToDatabase(request.context.database_path, rolled_back.inventory);
  if (!persisted.ok()) {
    return MakeTxnError<EngineRollbackTransactionResult>(request.context, operation_id,
        DiagnosticFromMGA(persisted.diagnostic, "SB-MGA-TXN-INV-PERSIST-FAILED", "mga.transaction_inventory.persist_failed"));
  }
  // DPC_DEFERRED_INDEX_WRITE_PATH
  if (!read_only_rollback) {
    const auto rolled_back_deltas = RollbackMgaSecondaryIndexDeltaLedgerTransaction(
        request.context,
        request.context.local_transaction_id);
    if (rolled_back_deltas.error) {
      return MakeTxnError<EngineRollbackTransactionResult>(
          request.context,
          operation_id,
          rolled_back_deltas);
    }
  }
  auto result = MakeTxnOk<EngineRollbackTransactionResult>(request.context, operation_id);
  result.local_transaction_id = rolled_back.entry.identity.local_id.value;
  result.transaction_uuid.canonical = UuidToString(rolled_back.entry.identity.transaction_uuid.value);
  static_cast<EngineApiResult&>(result).local_transaction_id = result.local_transaction_id;
  static_cast<EngineApiResult&>(result).transaction_uuid = result.transaction_uuid;
  result.evidence.push_back({"transaction_state", "rolled_back"});
  result.evidence.push_back({"transaction_read_only", read_only_rollback ? "true" : "false"});
  const auto released_locks =
      NamedAdvisoryLockTable().ReleaseAll(MakeLocalTransactionId(request.context.local_transaction_id));
  result.evidence.push_back({"transaction_advisory_locks_released",
                             std::to_string(released_locks)});
  if (read_only_rollback) {
    result.evidence.push_back({"read_only_rollback_delta_cleanup",
                               "skipped_no_mutation_authority"});
  }
  return result;
}

EnginePrepareTransactionResult EnginePrepareTransaction(const EnginePrepareTransactionRequest& request) {
  const std::string operation_id = "transaction.prepare";
  const auto path_status = ValidateDatabasePath(request.context, operation_id);
  if (path_status.error) {
    return MakeTxnError<EnginePrepareTransactionResult>(request.context, operation_id, path_status);
  }
  if (request.context.local_transaction_id == 0) {
    return MakeTxnError<EnginePrepareTransactionResult>(
        request.context,
        operation_id,
        MakeInvalidRequestDiagnostic(operation_id, "local_transaction_id_required"));
  }
  const auto inventory_guard =
      AcquireTransactionInventoryGuard(request.context.database_path);
  const auto loaded = LoadLocalTransactionInventoryFromDatabase(request.context.database_path);
  if (!loaded.ok()) {
    return MakeTxnError<EnginePrepareTransactionResult>(request.context, operation_id,
        DiagnosticFromMGA(loaded.diagnostic, "SB-MGA-TXN-INV-LOAD-FAILED", "mga.transaction_inventory.load_failed"));
  }
  if (auto policy_error = EnforceRuntimePolicyForExistingTransaction<EnginePrepareTransactionResult>(
          request, operation_id, loaded.inventory, false)) {
    return *policy_error;
  }
  LocalPreparePolicy policy;
  policy.require_evidence_before_prepared_success = true;
  const auto prepared = PrepareLocalTransactionDurable(loaded.inventory, MakeLocalTransactionId(request.context.local_transaction_id), policy);
  if (!prepared.ok()) {
    return MakeTxnError<EnginePrepareTransactionResult>(request.context, operation_id,
        DiagnosticFromMGA(prepared.diagnostic, "SB-MGA-TXN-LIFE-PREPARE-FAILED", "mga.transaction_lifecycle.prepare_failed"));
  }
  const auto persisted = PersistLocalTransactionInventoryToDatabase(request.context.database_path, prepared.inventory);
  if (!persisted.ok()) {
    return MakeTxnError<EnginePrepareTransactionResult>(request.context, operation_id,
        DiagnosticFromMGA(persisted.diagnostic, "SB-MGA-TXN-INV-PERSIST-FAILED", "mga.transaction_inventory.persist_failed"));
  }
  auto result = MakeTxnOk<EnginePrepareTransactionResult>(request.context, operation_id);
  result.local_transaction_id = prepared.entry.identity.local_id.value;
  result.transaction_uuid.canonical = UuidToString(prepared.entry.identity.transaction_uuid.value);
  static_cast<EngineApiResult&>(result).local_transaction_id = result.local_transaction_id;
  static_cast<EngineApiResult&>(result).transaction_uuid = result.transaction_uuid;
  result.evidence.push_back({"transaction_state", "prepared"});
  return result;
}

EngineExecuteTransactionBlockResult EngineExecuteTransactionBlock(
    const EngineExecuteTransactionBlockRequest& request) {
  const std::string operation_id = "transaction.execute_block";
  const auto path_status = ValidateDatabasePath(request.context, operation_id);
  if (path_status.error) {
    return MakeTxnError<EngineExecuteTransactionBlockResult>(
        request.context, operation_id, path_status);
  }
  if (!request.context.security_context_present) {
    return MakeTxnError<EngineExecuteTransactionBlockResult>(
        request.context,
        operation_id,
        DblcTransactionAdmissionDenied("security_context_required"));
  }
  if (request.context.local_transaction_id == 0) {
    return MakeTxnError<EngineExecuteTransactionBlockResult>(
        request.context,
        operation_id,
        MakeInvalidRequestDiagnostic(operation_id, "local_transaction_id_required"));
  }
  const auto loaded = LoadLocalTransactionInventoryFromDatabase(request.context.database_path);
  if (!loaded.ok()) {
    return MakeTxnError<EngineExecuteTransactionBlockResult>(
        request.context,
        operation_id,
        DiagnosticFromMGA(loaded.diagnostic,
                          "SB-MGA-TXN-INV-LOAD-FAILED",
                          "mga.transaction_inventory.load_failed"));
  }
  if (auto policy_error =
          EnforceRuntimePolicyForExistingTransaction<EngineExecuteTransactionBlockResult>(
              request, operation_id, loaded.inventory, false)) {
    return *policy_error;
  }
  auto result = MakeTxnOk<EngineExecuteTransactionBlockResult>(
      request.context, operation_id);
  const std::string surface_id = RequestOptionValue(request, "sbsfc077_surface_id:");
  const std::string block_kind = RequestOptionValue(request, "transaction_block_kind:").empty()
                                     ? "internal_procedure_block"
                                     : RequestOptionValue(request, "transaction_block_kind:");
  AddApiBehaviorRow(&result,
                    {{"transaction_block_kind", block_kind},
                     {"surface_id", surface_id},
                     {"local_transaction_id", std::to_string(request.context.local_transaction_id)},
                     {"execution_authority", "engine_internal_procedure"},
                     {"parser_executes_sql", "false"}});
  result.evidence.push_back({"transaction_internal_procedure_block", block_kind});
  result.evidence.push_back({"parser_finality", "false"});
  if (!surface_id.empty()) {
    result.evidence.push_back({"sbsfc077_surface", surface_id});
  }
  return result;
}

EngineLockTableResult EngineLockTable(const EngineLockTableRequest& request) {
  const std::string operation_id = "transaction.lock_table";
  if (const auto path_status = ValidateDatabasePath(request.context, operation_id);
      path_status.error) {
    return MakeLockError<EngineLockTableResult>(
        request.context, operation_id, path_status, "LOCK TABLE", "path_validation");
  }
  const auto admission = ValidateLockControlContext(request, operation_id);
  if (admission.error) {
    return MakeLockError<EngineLockTableResult>(
        request.context, operation_id, admission, "LOCK TABLE", "admission_refused");
  }
  const std::string mode = TableLockModeForRequest(request);
  const std::string surface = LockSurfaceOption(request, "LOCK TABLE");
  if (IsWriteOrExclusiveTableMode(mode)) {
    if (EngineOwnedTableFenceAuthorized(request)) {
      auto result = MakeTxnOk<EngineLockTableResult>(request.context, operation_id);
      result.lock_surface = surface;
      result.lock_mode = "write_or_exclusive";
      result.lock_policy = "engine_owned_admission_fence";
      result.compatibility_noop = false;
      result.admission_fence = true;
      AddLockInvariantEvidence(&result);
      result.evidence.push_back({"lock_surface", surface});
      result.evidence.push_back({"table_lock_mode", "write_or_exclusive"});
      result.evidence.push_back({"lock_decision", "admitted_fence"});
      result.evidence.push_back({"lock_policy", result.lock_policy});
      result.evidence.push_back({"admission_fence", "engine_owned_ddl_admin_migration"});
      result.evidence.push_back({"ordinary_reader_blocking", "false"});
      result.evidence.push_back({"mga_cleanup_horizon_impact", "false"});
      AddLockBehaviorRow(&result,
                         operation_id,
                         surface,
                         result.lock_policy,
                         "engine_owned_admission_fence");
      return result;
    }
    auto result = MakeLockError<EngineLockTableResult>(
        request.context,
        operation_id,
        LockNotAvailableDiagnostic("exclusive_table_lock_refused_by_mga_policy"),
        surface,
        "default_refusal");
    result.lock_surface = surface;
    result.lock_mode = "write_or_exclusive";
    result.lock_policy = "default_refusal";
    result.compatibility_noop = false;
    result.evidence.push_back({"table_lock_mode", "write_or_exclusive"});
    result.evidence.push_back({"lock_decision", "refused"});
    AddLockBehaviorRow(&result,
                       operation_id,
                       surface,
                       "default_refusal",
                       "exclusive_table_lock_refused");
    return result;
  }

  auto result = MakeTxnOk<EngineLockTableResult>(request.context, operation_id);
  result.lock_surface = surface;
  result.lock_mode = "read_or_share";
  result.lock_policy = "compatibility_noop";
  result.compatibility_noop = true;
  AddLockInvariantEvidence(&result);
  result.evidence.push_back({"lock_surface", surface});
  result.evidence.push_back({"table_lock_mode", "read_or_share"});
  result.evidence.push_back({"lock_decision", "granted_noop"});
  result.evidence.push_back({"lock_policy", result.lock_policy});
  AddLockBehaviorRow(&result,
                     operation_id,
                     surface,
                     result.lock_policy,
                     "read_or_share_noop");
  return result;
}

EngineUnlockTableResult EngineUnlockTable(const EngineUnlockTableRequest& request) {
  const std::string operation_id = "transaction.unlock_table";
  if (const auto path_status = ValidateDatabasePath(request.context, operation_id);
      path_status.error) {
    return MakeLockError<EngineUnlockTableResult>(
        request.context, operation_id, path_status, "UNLOCK TABLE", "path_validation");
  }
  const auto admission = ValidateLockControlContext(request, operation_id);
  if (admission.error) {
    return MakeLockError<EngineUnlockTableResult>(
        request.context, operation_id, admission, "UNLOCK TABLE", "admission_refused");
  }
  const std::string surface = LockSurfaceOption(request, "UNLOCK TABLE");
  auto result = MakeTxnOk<EngineUnlockTableResult>(request.context, operation_id);
  result.lock_surface = surface;
  result.release_outcome = "noop_no_table_lock_held";
  result.compatibility_noop = true;
  AddLockInvariantEvidence(&result);
  result.evidence.push_back({"lock_surface", surface});
  result.evidence.push_back({"lock_policy", "compatibility_noop"});
  result.evidence.push_back({"release_outcome", result.release_outcome});
  AddLockBehaviorRow(&result,
                     operation_id,
                     surface,
                     "compatibility_noop",
                     result.release_outcome);
  return result;
}

EngineLockNamedResult EngineLockNamed(const EngineLockNamedRequest& request) {
  const std::string operation_id = "transaction.lock_named";
  if (const auto path_status = ValidateDatabasePath(request.context, operation_id);
      path_status.error) {
    return MakeLockError<EngineLockNamedResult>(
        request.context, operation_id, path_status, "LOCK NAMED", "path_validation");
  }
  const auto admission = ValidateLockControlContext(request, operation_id);
  if (admission.error) {
    return MakeLockError<EngineLockNamedResult>(
        request.context, operation_id, admission, "LOCK NAMED", "admission_refused");
  }
  const std::string surface = LockSurfaceOption(request, "LOCK NAMED");
  const std::string resource_key = NamedLockResourceKey(request);
  if (resource_key.empty()) {
    return MakeLockError<EngineLockNamedResult>(
        request.context,
        operation_id,
        MakeInvalidRequestDiagnostic(operation_id, "lock_descriptor_required"),
        surface,
        "advisory_lock");
  }

  TransactionLockRequest lock_request;
  lock_request.requester = MakeLocalTransactionId(request.context.local_transaction_id);
  lock_request.resource_key = resource_key;
  lock_request.mode = TransactionLockMode::exclusive;
  lock_request.wait_policy = WaitPolicyForRequest(request);
  lock_request.transaction_active = true;
  const auto lock = NamedAdvisoryLockTable().Acquire(std::move(lock_request));
  const std::string decision = TransactionLockDecisionName(lock.decision);

  if (!lock.ok()) {
    EngineApiDiagnostic diagnostic = LockNotAvailableDiagnostic(resource_key);
    if (decision == "timeout" || decision == "wait_required") {
      diagnostic = LockTimeoutDiagnostic(resource_key);
    } else if (decision == "invalid_request") {
      diagnostic = MakeInvalidRequestDiagnostic(operation_id, "named_lock_invalid_request");
    }
    auto result = MakeLockError<EngineLockNamedResult>(
        request.context, operation_id, diagnostic, surface, "advisory_lock");
    result.lock_surface = surface;
    result.lock_decision = decision;
    result.resource_key = resource_key;
    result.acquired = false;
    AddNamedLockDecisionEvidence(&result,
                                 resource_key,
                                 decision,
                                 lock.blocking_transaction.value);
    AddLockBehaviorRow(&result,
                       operation_id,
                       surface,
                       "advisory_lock",
                       NamedLockOutcomeForDecision(decision),
                       resource_key);
    return result;
  }

  auto result = MakeTxnOk<EngineLockNamedResult>(request.context, operation_id);
  result.lock_surface = surface;
  result.lock_decision = decision;
  result.resource_key = resource_key;
  result.acquired = true;
  AddLockInvariantEvidence(&result);
  result.evidence.push_back({"lock_surface", surface});
  result.evidence.push_back({"lock_policy", "advisory_lock"});
  AddNamedLockDecisionEvidence(&result, resource_key, decision);
  AddLockBehaviorRow(&result,
                     operation_id,
                     surface,
                     "advisory_lock",
                     NamedLockOutcomeForDecision(decision),
                     resource_key);
  return result;
}

EngineUnlockNamedResult EngineUnlockNamed(const EngineUnlockNamedRequest& request) {
  const std::string operation_id = "transaction.unlock_named";
  if (const auto path_status = ValidateDatabasePath(request.context, operation_id);
      path_status.error) {
    return MakeLockError<EngineUnlockNamedResult>(
        request.context, operation_id, path_status, "UNLOCK NAMED", "path_validation");
  }
  const auto admission = ValidateLockControlContext(request, operation_id);
  if (admission.error) {
    return MakeLockError<EngineUnlockNamedResult>(
        request.context, operation_id, admission, "UNLOCK NAMED", "admission_refused");
  }
  const std::string surface = LockSurfaceOption(request, "UNLOCK NAMED");
  const std::string resource_key = NamedLockResourceKey(request);
  if (resource_key.empty()) {
    return MakeLockError<EngineUnlockNamedResult>(
        request.context,
        operation_id,
        MakeInvalidRequestDiagnostic(operation_id, "lock_descriptor_required"),
        surface,
        "advisory_lock_release");
  }

  const auto release = NamedAdvisoryLockTable().Release(
      MakeLocalTransactionId(request.context.local_transaction_id),
      resource_key);
  auto result = MakeTxnOk<EngineUnlockNamedResult>(request.context, operation_id);
  result.lock_surface = surface;
  result.resource_key = resource_key;
  result.released = release.ok();
  result.release_outcome = release.ok() ? "released" : "noop_not_owned";
  AddLockInvariantEvidence(&result);
  result.evidence.push_back({"lock_surface", surface});
  result.evidence.push_back({"lock_policy", "advisory_lock_release"});
  result.evidence.push_back({"release_outcome", result.release_outcome});
  result.evidence.push_back({"lock_resource_key", resource_key});
  AddLockBehaviorRow(&result,
                     operation_id,
                     surface,
                     "advisory_lock_release",
                     result.release_outcome,
                     resource_key);
  return result;
}

}  // namespace scratchbird::engine::internal_api
