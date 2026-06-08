// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "savepoint.hpp"

#include "metric_producer.hpp"

#include <algorithm>
#include <utility>
#include <vector>

namespace scratchbird::transaction::mga {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status SavepointOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::transaction_mga};
}

Status SavepointErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::transaction_mga};
}

SavepointResult SavepointError(std::string diagnostic_code, std::string message_key, std::string detail = {}) {
  SavepointResult result;
  result.status = SavepointErrorStatus();
  result.diagnostic = MakeSavepointDiagnostic(result.status,
                                              std::move(diagnostic_code),
                                              std::move(message_key),
                                              std::move(detail));
  return result;
}

SavepointMutationResult SavepointMutationError(std::string diagnostic_code, std::string message_key, std::string detail = {}) {
  SavepointMutationResult result;
  result.status = SavepointErrorStatus();
  result.diagnostic = MakeSavepointDiagnostic(result.status,
                                              std::move(diagnostic_code),
                                              std::move(message_key),
                                              std::move(detail));
  return result;
}

SavepointRollbackPlan SavepointRollbackError(SavepointRollbackDecision decision,
                                             std::string diagnostic_code,
                                             std::string message_key,
                                             std::string detail = {}) {
  SavepointRollbackPlan plan;
  plan.status = SavepointErrorStatus();
  plan.decision = decision;
  plan.diagnostic = MakeSavepointDiagnostic(plan.status,
                                            std::move(diagnostic_code),
                                            std::move(message_key),
                                            std::move(detail));
  return plan;
}

SavepointRollbackExecutionResult SavepointExecutionError(SavepointRollbackDecision decision,
                                                         std::string diagnostic_code,
                                                         std::string message_key,
                                                         std::string detail = {}) {
  SavepointRollbackExecutionResult result;
  result.status = SavepointErrorStatus();
  result.decision = decision;
  result.diagnostic = MakeSavepointDiagnostic(result.status,
                                              std::move(diagnostic_code),
                                              std::move(message_key),
                                              std::move(detail));
  return result;
}

void PublishSavepointDecisionMetric(const char* action) {
  (void)scratchbird::core::metrics::IncrementCounter(
      "sb_tx_savepoint_operations_total",
      scratchbird::core::metrics::Labels({{"component", "transaction.mga.savepoint"}, {"action", action}}),
      1.0,
      "transaction_mga_savepoint");
}

const SavepointRecord* FindSavepoint(const std::vector<SavepointRecord>& savepoints,
                                     LocalTransactionId local_id,
                                     const std::string& name) {
  for (auto it = savepoints.rbegin(); it != savepoints.rend(); ++it) {
    if (it->local_id.value == local_id.value && it->name == name) { return &(*it); }
  }
  return nullptr;
}

}  // namespace

const char* SavepointMutationKindName(SavepointMutationKind kind) {
  switch (kind) {
    case SavepointMutationKind::data_page: return "data_page";
    case SavepointMutationKind::catalog: return "catalog";
    case SavepointMutationKind::index: return "index";
    case SavepointMutationKind::overflow: return "overflow";
    case SavepointMutationKind::metrics: return "metrics";
    case SavepointMutationKind::unknown: return "unknown";
  }
  return "unknown";
}

const char* SavepointRollbackDecisionName(SavepointRollbackDecision decision) {
  switch (decision) {
    case SavepointRollbackDecision::rollback_actions_ready: return "rollback_actions_ready";
    case SavepointRollbackDecision::rollback_refused_missing_undo: return "rollback_refused_missing_undo";
    case SavepointRollbackDecision::rollback_refused_executor_missing: return "rollback_refused_executor_missing";
    case SavepointRollbackDecision::rollback_refused_executor_unavailable: return "rollback_refused_executor_unavailable";
    case SavepointRollbackDecision::rollback_refused_executor_failed: return "rollback_refused_executor_failed";
    case SavepointRollbackDecision::savepoint_not_found: return "savepoint_not_found";
    case SavepointRollbackDecision::invalid_request: return "invalid_request";
  }
  return "invalid_request";
}

SavepointResult SavepointStack::Create(LocalTransactionId local_id, std::string name, u64 mutation_sequence) {
  if (!local_id.valid() || name.empty()) {
    PublishSavepointDecisionMetric("create_invalid");
    return SavepointError("SB-SNTXN-SAVEPOINT-UNSUPPORTED",
                          "transaction.savepoint.invalid_request");
  }
  for (auto it = savepoints_.begin(); it != savepoints_.end();) {
    if (it->local_id.value == local_id.value && it->name == name) {
      it = savepoints_.erase(it);
    } else {
      ++it;
    }
  }
  SavepointRecord record;
  record.local_id = local_id;
  record.name = std::move(name);
  record.ordinal = next_ordinal_++;
  record.mutation_sequence = mutation_sequence;
  savepoints_.push_back(record);
  SavepointResult result;
  result.status = SavepointOkStatus();
  result.savepoint = record;
  PublishSavepointDecisionMetric("create");
  return result;
}

SavepointResult SavepointStack::Release(LocalTransactionId local_id, std::string name) {
  for (auto it = savepoints_.begin(); it != savepoints_.end(); ++it) {
    if (it->local_id.value == local_id.value && it->name == name) {
      SavepointRecord record = *it;
      savepoints_.erase(it, savepoints_.end());
      SavepointResult result;
      result.status = SavepointOkStatus();
      result.savepoint = record;
      PublishSavepointDecisionMetric("release");
      return result;
    }
  }
  PublishSavepointDecisionMetric("release_not_found");
  return SavepointError("SB-SNTXN-SAVEPOINT-UNSUPPORTED",
                        "transaction.savepoint.not_found",
                        name);
}

SavepointResult SavepointStack::RollbackTo(LocalTransactionId local_id, std::string name) {
  for (auto it = savepoints_.begin(); it != savepoints_.end(); ++it) {
    if (it->local_id.value == local_id.value && it->name == name) {
      SavepointRecord record = *it;
      savepoints_.erase(it + 1, savepoints_.end());
      SavepointResult result;
      result.status = SavepointOkStatus();
      result.savepoint = record;
      PublishSavepointDecisionMetric("rollback_marker");
      return result;
    }
  }
  PublishSavepointDecisionMetric("rollback_marker_not_found");
  return SavepointError("SB-SNTXN-SAVEPOINT-UNSUPPORTED",
                        "transaction.savepoint.not_found",
                        name);
}

SavepointMutationResult SavepointStack::RecordMutation(SavepointMutationRecord mutation) {
  if (!mutation.local_id.valid() || mutation.mutation_sequence == 0 ||
      mutation.kind == SavepointMutationKind::unknown || mutation.stable_operation_id.empty() ||
      !mutation.durable_evidence_written) {
    PublishSavepointDecisionMetric("mutation_invalid");
    return SavepointMutationError("SB-SNTXN-SAVEPOINT-MUTATION-INVALID",
                                  "transaction.savepoint.mutation_invalid",
                                  mutation.stable_operation_id);
  }
  mutations_.push_back(std::move(mutation));
  SavepointMutationResult result;
  result.status = SavepointOkStatus();
  result.mutation = mutations_.back();
  PublishSavepointDecisionMetric("mutation_recorded");
  return result;
}

SavepointRollbackPlan SavepointStack::PlanRollbackTo(LocalTransactionId local_id, std::string name) const {
  if (!local_id.valid() || name.empty()) {
    PublishSavepointDecisionMetric("rollback_plan_invalid");
    return SavepointRollbackError(SavepointRollbackDecision::invalid_request,
                                  "SB-SNTXN-SAVEPOINT-ROLLBACK-INVALID",
                                  "transaction.savepoint.rollback_invalid");
  }
  const SavepointRecord* marker = FindSavepoint(savepoints_, local_id, name);
  if (marker == nullptr) {
    PublishSavepointDecisionMetric("rollback_plan_not_found");
    return SavepointRollbackError(SavepointRollbackDecision::savepoint_not_found,
                                  "SB-SNTXN-SAVEPOINT-NOT-FOUND",
                                  "transaction.savepoint.not_found",
                                  name);
  }
  SavepointRollbackPlan plan;
  plan.status = SavepointOkStatus();
  plan.decision = SavepointRollbackDecision::rollback_actions_ready;
  plan.savepoint = *marker;
  for (const SavepointMutationRecord& mutation : mutations_) {
    if (mutation.local_id.value != local_id.value || mutation.mutation_sequence <= marker->mutation_sequence ||
        mutation.rolled_back) {
      continue;
    }
    if (!mutation.undo_evidence_available) {
      PublishSavepointDecisionMetric("rollback_plan_missing_undo");
      return SavepointRollbackError(SavepointRollbackDecision::rollback_refused_missing_undo,
                                    "SB-SNTXN-SAVEPOINT-ROLLBACK-MISSING-UNDO",
                                    "transaction.savepoint.rollback_missing_undo",
                                    mutation.stable_operation_id);
    }
    plan.rollback_actions.push_back(mutation);
  }
  std::sort(plan.rollback_actions.begin(), plan.rollback_actions.end(), [](const auto& lhs, const auto& rhs) {
    return lhs.mutation_sequence > rhs.mutation_sequence;
  });
  plan.affected_mutation_count = static_cast<u64>(plan.rollback_actions.size());
  PublishSavepointDecisionMetric("rollback_plan_ready");
  return plan;
}

SavepointRollbackPlan SavepointStack::ApplyRollbackTo(LocalTransactionId local_id, std::string name) {
  SavepointRollbackPlan plan = PlanRollbackTo(local_id, name);
  if (!plan.ok()) { return plan; }
  if (!plan.rollback_actions.empty()) {
    PublishSavepointDecisionMetric("rollback_apply_executor_missing");
    return SavepointRollbackError(SavepointRollbackDecision::rollback_refused_executor_missing,
                                  "SB-SNTXN-SAVEPOINT-UNDO-EXECUTOR-MISSING",
                                  "transaction.savepoint.undo_executor_missing",
                                  plan.savepoint.name);
  }
  MarkRollbackApplied(local_id, plan.savepoint.mutation_sequence);
  PublishSavepointDecisionMetric("rollback_applied");
  return plan;
}

SavepointRollbackExecutionResult SavepointStack::ExecuteRollbackTo(LocalTransactionId local_id,
                                                                   std::string name,
                                                                   SavepointPhysicalUndoExecutor* executor) {
  SavepointRollbackPlan plan = PlanRollbackTo(local_id, std::move(name));
  if (!plan.ok()) {
    SavepointRollbackExecutionResult result;
    result.status = plan.status;
    result.decision = plan.decision;
    result.savepoint = plan.savepoint;
    result.affected_mutation_count = plan.affected_mutation_count;
    result.diagnostic = plan.diagnostic;
    return result;
  }
  if (executor == nullptr) {
    PublishSavepointDecisionMetric("rollback_execute_executor_missing");
    SavepointRollbackExecutionResult result =
        SavepointExecutionError(SavepointRollbackDecision::rollback_refused_executor_missing,
                                "SB-SNTXN-SAVEPOINT-UNDO-EXECUTOR-MISSING",
                                "transaction.savepoint.undo_executor_missing",
                                plan.savepoint.name);
    result.savepoint = plan.savepoint;
    return result;
  }

  SavepointRollbackExecutionResult result;
  result.status = SavepointOkStatus();
  result.decision = SavepointRollbackDecision::rollback_actions_ready;
  result.savepoint = plan.savepoint;
  result.affected_mutation_count = plan.affected_mutation_count;
  for (const SavepointMutationRecord& mutation : plan.rollback_actions) {
    if (!executor->Supports(mutation.kind)) {
      PublishSavepointDecisionMetric("rollback_execute_executor_unavailable");
      result.status = SavepointErrorStatus();
      result.decision = SavepointRollbackDecision::rollback_refused_executor_unavailable;
      result.diagnostic = MakeSavepointDiagnostic(
          result.status,
          "SB-SNTXN-SAVEPOINT-UNDO-EXECUTOR-UNAVAILABLE",
          "transaction.savepoint.undo_executor_unavailable",
          std::string(SavepointMutationKindName(mutation.kind)) + ":" + mutation.stable_operation_id);
      return result;
    }
    SavepointUndoResult undo = executor->ApplyUndo(mutation);
    result.undo_results.push_back(undo);
    if (!undo.ok()) {
      PublishSavepointDecisionMetric("rollback_execute_executor_failed");
      result.status = undo.status.ok() ? SavepointErrorStatus() : undo.status;
      result.decision = SavepointRollbackDecision::rollback_refused_executor_failed;
      result.diagnostic = undo.diagnostic.diagnostic_code.empty()
                              ? MakeSavepointDiagnostic(result.status,
                                                        "SB-SNTXN-SAVEPOINT-UNDO-EXECUTOR-FAILED",
                                                        "transaction.savepoint.undo_executor_failed",
                                                        mutation.stable_operation_id)
                              : undo.diagnostic;
      return result;
    }
  }
  MarkRollbackApplied(local_id, plan.savepoint.mutation_sequence);
  PublishSavepointDecisionMetric("rollback_execute_applied");
  return result;
}

void SavepointStack::MarkRollbackApplied(LocalTransactionId local_id, u64 savepoint_mutation_sequence) {
  for (SavepointMutationRecord& mutation : mutations_) {
    if (mutation.local_id.value == local_id.value &&
        mutation.mutation_sequence > savepoint_mutation_sequence) {
      mutation.rolled_back = true;
    }
  }
  for (auto it = savepoints_.begin(); it != savepoints_.end();) {
    if (it->local_id.value == local_id.value && it->mutation_sequence > savepoint_mutation_sequence) {
      it = savepoints_.erase(it);
    } else {
      ++it;
    }
  }
}

u64 SavepointStack::size() const {
  return static_cast<u64>(savepoints_.size());
}

u64 SavepointStack::mutation_count() const {
  return static_cast<u64>(mutations_.size());
}

DiagnosticRecord MakeSavepointDiagnostic(Status status,
                                         std::string diagnostic_code,
                                         std::string message_key,
                                         std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", detail});
  }
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "transaction.mga.savepoint");
}

}  // namespace scratchbird::transaction::mga
