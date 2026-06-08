// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "disk_device.hpp"
#include "savepoint.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace disk = scratchbird::storage::disk;
namespace txn = scratchbird::transaction::mga;

using scratchbird::core::platform::Severity;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::u64;

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::transaction_mga};
}

Status ErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::transaction_mga};
}

bool Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
}

void PrintDiagnostic(const scratchbird::core::platform::DiagnosticRecord& diagnostic) {
  if (diagnostic.diagnostic_code.empty()) {
    return;
  }
  std::cerr << diagnostic.diagnostic_code << ':' << diagnostic.message_key;
  for (const auto& argument : diagnostic.arguments) {
    std::cerr << ' ' << argument.key << '=' << argument.value;
  }
  std::cerr << '\n';
}

std::vector<std::string> SplitTabs(const std::string& text) {
  std::vector<std::string> parts;
  std::string part;
  std::istringstream in(text);
  while (std::getline(in, part, '\t')) {
    parts.push_back(part);
  }
  return parts;
}

std::string StableOperationId(txn::SavepointMutationKind kind,
                              std::string_view resource_id) {
  return std::string(txn::SavepointMutationKindName(kind)) + ":" +
         std::string(resource_id);
}

struct ParsedStableOperationId {
  bool ok = false;
  txn::SavepointMutationKind kind = txn::SavepointMutationKind::unknown;
  std::string resource_id;
};

ParsedStableOperationId ParseStableOperationId(const std::string& stable_operation_id) {
  ParsedStableOperationId parsed;
  const auto colon = stable_operation_id.find(':');
  if (colon == std::string::npos || colon == 0 || colon + 1 >= stable_operation_id.size()) {
    return parsed;
  }
  const std::string kind_name = stable_operation_id.substr(0, colon);
  parsed.resource_id = stable_operation_id.substr(colon + 1);
  constexpr std::array<txn::SavepointMutationKind, 5> kinds = {
      txn::SavepointMutationKind::data_page,
      txn::SavepointMutationKind::catalog,
      txn::SavepointMutationKind::index,
      txn::SavepointMutationKind::overflow,
      txn::SavepointMutationKind::metrics,
  };
  for (const auto kind : kinds) {
    if (kind_name == txn::SavepointMutationKindName(kind)) {
      parsed.kind = kind;
      parsed.ok = true;
      return parsed;
    }
  }
  return parsed;
}

struct TempDir {
  std::filesystem::path path;

  TempDir() {
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    path = std::filesystem::temp_directory_path() /
           ("scratchbird_savepoint_physical_undo_" + std::to_string(tick));
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
    std::filesystem::create_directories(path);
  }

  ~TempDir() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

class DurableUndoLedgerExecutor final : public txn::SavepointPhysicalUndoExecutor {
 public:
  DurableUndoLedgerExecutor(std::filesystem::path ledger_path,
                            std::set<txn::SavepointMutationKind> supported,
                            std::string fail_operation_id = {})
      : ledger_path_(std::move(ledger_path)),
        supported_(std::move(supported)),
        fail_operation_id_(std::move(fail_operation_id)) {
    std::filesystem::create_directories(ledger_path_.parent_path());
  }

  bool Supports(txn::SavepointMutationKind kind) const override {
    return supported_.find(kind) != supported_.end();
  }

  txn::SavepointUndoResult ApplyUndo(const txn::SavepointMutationRecord& mutation) override {
    if (!Supports(mutation.kind)) {
      return UndoFailure(mutation,
                         "SB-SNTXN-SAVEPOINT-UNDO-EXECUTOR-UNAVAILABLE",
                         "transaction.savepoint.undo_executor_unavailable",
                         txn::SavepointMutationKindName(mutation.kind));
    }
    const auto parsed = ParseStableOperationId(mutation.stable_operation_id);
    if (!parsed.ok || parsed.kind != mutation.kind) {
      return UndoFailure(mutation,
                         "SB-SNTXN-SAVEPOINT-UNDO-RESOURCE-BINDING-INVALID",
                         "transaction.savepoint.undo_resource_binding_invalid",
                         mutation.stable_operation_id);
    }
    if (!Replay()) {
      return UndoFailure(mutation,
                         "SB-SNTXN-SAVEPOINT-UNDO-LEDGER-REPLAY-FAILED",
                         "transaction.savepoint.undo_ledger_replay_failed",
                         ledger_path_.string());
    }

    const std::string resource_key = mutation.stable_operation_id;
    const auto visible_it = resource_visible_.find(resource_key);
    if (undone_operations_.find(resource_key) != undone_operations_.end() ||
        (visible_it != resource_visible_.end() && !visible_it->second)) {
      return UndoSuccess(mutation, false);
    }
    if (visible_it == resource_visible_.end()) {
      return UndoFailure(mutation,
                         "SB-SNTXN-SAVEPOINT-UNDO-RESOURCE-MISSING",
                         "transaction.savepoint.undo_resource_missing",
                         resource_key);
    }
    if (fail_operation_id_ == mutation.stable_operation_id) {
      return UndoFailure(mutation,
                         "SB-SNTXN-SAVEPOINT-UNDO-EXECUTOR-FAILED",
                         "transaction.savepoint.undo_executor_failed",
                         resource_key);
    }

    if (!AppendLine("UNDO\t" + resource_key + "\t" +
                    std::to_string(mutation.mutation_sequence) + "\t" +
                    mutation.stable_operation_id + "\n")) {
      return UndoFailure(mutation,
                         "SB-SNTXN-SAVEPOINT-UNDO-LEDGER-WRITE-FAILED",
                         "transaction.savepoint.undo_ledger_write_failed",
                         resource_key);
    }
    if (!Replay()) {
      return UndoFailure(mutation,
                         "SB-SNTXN-SAVEPOINT-UNDO-LEDGER-REPLAY-FAILED",
                         "transaction.savepoint.undo_ledger_replay_failed",
                         ledger_path_.string());
    }
    return UndoSuccess(mutation, true);
  }

  bool CreateResource(txn::SavepointMutationKind kind,
                      std::string_view resource_id,
                      std::string_view value) {
    const std::string resource_key = StableOperationId(kind, resource_id);
    return AppendLine("CREATE\t" + resource_key + "\t" + std::string(value) + "\n") &&
           Replay();
  }

  bool Visible(txn::SavepointMutationKind kind, std::string_view resource_id) {
    if (!Replay()) {
      return false;
    }
    const std::string resource_key = StableOperationId(kind, resource_id);
    const auto it = resource_visible_.find(resource_key);
    return it != resource_visible_.end() && it->second;
  }

  u64 UndoCount() {
    if (!Replay()) {
      return 0;
    }
    return static_cast<u64>(undone_operations_.size());
  }

 private:
  std::filesystem::path ledger_path_;
  std::set<txn::SavepointMutationKind> supported_;
  std::string fail_operation_id_;
  std::map<std::string, bool> resource_visible_;
  std::set<std::string> undone_operations_;

  bool AppendLine(const std::string& line) {
    std::ofstream out(ledger_path_, std::ios::binary | std::ios::app);
    if (!out) {
      return false;
    }
    out << line;
    out.close();
    if (!out) {
      return false;
    }
    const auto synced_file = disk::SyncFilesystemPath(ledger_path_.string(), true);
    if (!synced_file.ok()) {
      PrintDiagnostic(synced_file.diagnostic);
      return false;
    }
    const auto synced_parent = disk::SyncParentDirectoryPath(ledger_path_.string());
    if (!synced_parent.ok()) {
      PrintDiagnostic(synced_parent.diagnostic);
      return false;
    }
    return true;
  }

  bool Replay() {
    resource_visible_.clear();
    undone_operations_.clear();
    if (!std::filesystem::exists(ledger_path_)) {
      return true;
    }
    std::ifstream in(ledger_path_, std::ios::binary);
    if (!in) {
      return false;
    }
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty()) {
        continue;
      }
      const auto parts = SplitTabs(line);
      if (parts.size() < 2) {
        return false;
      }
      if (parts[0] == "CREATE") {
        resource_visible_[parts[1]] = true;
      } else if (parts[0] == "UNDO") {
        resource_visible_[parts[1]] = false;
        undone_operations_.insert(parts[1]);
      } else {
        return false;
      }
    }
    return true;
  }

  txn::SavepointUndoResult UndoSuccess(const txn::SavepointMutationRecord& mutation,
                                       bool applied) const {
    txn::SavepointUndoResult result;
    result.status = OkStatus();
    result.mutation = mutation;
    result.applied = applied;
    result.already_applied = !applied;
    result.executor_id = "durable_savepoint_undo_ledger_v1";
    result.durable_evidence_id = ledger_path_.string() + "#" + mutation.stable_operation_id;
    return result;
  }

  txn::SavepointUndoResult UndoFailure(const txn::SavepointMutationRecord& mutation,
                                       std::string diagnostic_code,
                                       std::string message_key,
                                       std::string detail) const {
    txn::SavepointUndoResult result;
    result.status = ErrorStatus();
    result.mutation = mutation;
    result.executor_id = "durable_savepoint_undo_ledger_v1";
    result.durable_evidence_id = ledger_path_.string() + "#" + mutation.stable_operation_id;
    result.diagnostic = txn::MakeSavepointDiagnostic(result.status,
                                                     std::move(diagnostic_code),
                                                     std::move(message_key),
                                                     std::move(detail));
    return result;
  }
};

std::set<txn::SavepointMutationKind> AllSupportedKinds() {
  return {
      txn::SavepointMutationKind::data_page,
      txn::SavepointMutationKind::catalog,
      txn::SavepointMutationKind::index,
      txn::SavepointMutationKind::overflow,
      txn::SavepointMutationKind::metrics,
  };
}

bool RecordMutation(txn::SavepointStack* stack,
                    txn::LocalTransactionId tx,
                    u64 sequence,
                    txn::SavepointMutationKind kind,
                    std::string stable_operation_id,
                    bool undo_evidence_available = true) {
  return Require(stack->RecordMutation({tx,
                                        sequence,
                                        kind,
                                        std::move(stable_operation_id),
                                        true,
                                        undo_evidence_available,
                                        false})
                     .ok(),
                 "ELER-023 mutation should record");
}

bool BuildStackForAllKinds(txn::SavepointStack* stack,
                           txn::LocalTransactionId tx) {
  bool ok = Require(stack->Create(tx, "sp", 10).ok(),
                    "ELER-023 savepoint should create");
  constexpr std::array<txn::SavepointMutationKind, 5> kinds = {
      txn::SavepointMutationKind::data_page,
      txn::SavepointMutationKind::catalog,
      txn::SavepointMutationKind::index,
      txn::SavepointMutationKind::overflow,
      txn::SavepointMutationKind::metrics,
  };
  u64 sequence = 11;
  for (const auto kind : kinds) {
    ok = RecordMutation(stack,
                        tx,
                        sequence++,
                        kind,
                        StableOperationId(kind, "after")) && ok;
  }
  return ok;
}

bool AllMutationKindsExecuteDurableUndoAndReplay() {
  TempDir temp;
  const auto ledger_path = temp.path / "all_kinds.savepoint_undo_ledger";
  DurableUndoLedgerExecutor executor(ledger_path, AllSupportedKinds());
  constexpr std::array<txn::SavepointMutationKind, 5> kinds = {
      txn::SavepointMutationKind::data_page,
      txn::SavepointMutationKind::catalog,
      txn::SavepointMutationKind::index,
      txn::SavepointMutationKind::overflow,
      txn::SavepointMutationKind::metrics,
  };
  bool ok = true;
  for (const auto kind : kinds) {
    ok = Require(executor.CreateResource(kind, "before", "visible-before"),
                 "ELER-023 before resource should be durably created") && ok;
    ok = Require(executor.CreateResource(kind, "after", "visible-after"),
                 "ELER-023 after resource should be durably created") && ok;
  }

  txn::SavepointStack stack;
  const auto tx = txn::MakeLocalTransactionId(44);
  ok = BuildStackForAllKinds(&stack, tx) && ok;
  const auto executed = stack.ExecuteRollbackTo(tx, "sp", &executor);
  if (!executed.ok()) {
    PrintDiagnostic(executed.diagnostic);
  }
  ok = Require(executed.ok(), "ELER-023 rollback execution should succeed") && ok;
  ok = Require(executed.undo_results.size() == kinds.size(),
               "ELER-023 should execute one undo per mutation kind") && ok;
  ok = Require(executed.affected_mutation_count == kinds.size(),
               "ELER-023 affected mutation count should include every kind") && ok;
  for (std::size_t i = 0; i < kinds.size() && i < executed.undo_results.size(); ++i) {
    const auto expected_kind = kinds[kinds.size() - i - 1];
    ok = Require(executed.undo_results[i].mutation.kind == expected_kind &&
                     executed.undo_results[i].applied,
                 "ELER-023 undo should run in reverse mutation order") && ok;
  }
  for (const auto kind : kinds) {
    ok = Require(executor.Visible(kind, "before"),
                 "ELER-023 before resource should remain visible") && ok;
    ok = Require(!executor.Visible(kind, "after"),
                 "ELER-023 after resource should be hidden by undo") && ok;
  }

  txn::SavepointStack replayed_stack;
  DurableUndoLedgerExecutor replayed_executor(ledger_path, AllSupportedKinds());
  ok = BuildStackForAllKinds(&replayed_stack, tx) && ok;
  const auto replayed = replayed_stack.ExecuteRollbackTo(tx, "sp", &replayed_executor);
  if (!replayed.ok()) {
    PrintDiagnostic(replayed.diagnostic);
  }
  ok = Require(replayed.ok(), "ELER-023 crash replay execution should succeed") && ok;
  ok = Require(replayed.undo_results.size() == kinds.size(),
               "ELER-023 crash replay should revisit every mutation kind") && ok;
  for (const auto& undo : replayed.undo_results) {
    ok = Require(undo.already_applied,
                 "ELER-023 crash replay undo should be idempotent") && ok;
  }
  ok = Require(replayed_executor.UndoCount() == kinds.size(),
               "ELER-023 crash replay should not duplicate undo evidence") && ok;
  for (const auto kind : kinds) {
    ok = Require(replayed_executor.Visible(kind, "before"),
                 "ELER-023 replay before resource should remain visible") && ok;
    ok = Require(!replayed_executor.Visible(kind, "after"),
                 "ELER-023 replay after resource should remain hidden") && ok;
  }
  return ok;
}

bool MissingUndoEvidenceFailsClosed() {
  TempDir temp;
  DurableUndoLedgerExecutor executor(temp.path / "missing_undo.savepoint_undo_ledger",
                                     AllSupportedKinds());
  bool ok = Require(executor.CreateResource(txn::SavepointMutationKind::catalog,
                                            "after",
                                            "catalog-after"),
                    "ELER-023 missing-undo fixture resource should create");
  txn::SavepointStack stack;
  const auto tx = txn::MakeLocalTransactionId(45);
  ok = Require(stack.Create(tx, "sp", 20).ok(),
               "ELER-023 missing-undo savepoint should create") && ok;
  ok = RecordMutation(&stack,
                      tx,
                      21,
                      txn::SavepointMutationKind::catalog,
                      StableOperationId(txn::SavepointMutationKind::catalog, "after"),
                      false) && ok;
  const auto executed = stack.ExecuteRollbackTo(tx, "sp", &executor);
  ok = Require(!executed.ok() &&
                   executed.decision ==
                       txn::SavepointRollbackDecision::rollback_refused_missing_undo &&
                   executed.diagnostic.diagnostic_code ==
                       "SB-SNTXN-SAVEPOINT-ROLLBACK-MISSING-UNDO",
               "ELER-023 missing undo evidence should fail closed before executor") && ok;
  ok = Require(executor.Visible(txn::SavepointMutationKind::catalog, "after"),
               "ELER-023 missing-undo failure must not hide the resource") && ok;
  ok = Require(executor.UndoCount() == 0,
               "ELER-023 missing-undo failure must not write undo evidence") && ok;
  return ok;
}

bool UnsupportedExecutorFailsClosedWithoutPartialUndo() {
  TempDir temp;
  DurableUndoLedgerExecutor executor(temp.path / "unsupported.savepoint_undo_ledger",
                                     {txn::SavepointMutationKind::data_page});
  bool ok = Require(executor.CreateResource(txn::SavepointMutationKind::metrics,
                                            "after",
                                            "metrics-after"),
                    "ELER-023 unsupported fixture resource should create");
  txn::SavepointStack stack;
  const auto tx = txn::MakeLocalTransactionId(46);
  ok = Require(stack.Create(tx, "sp", 30).ok(),
               "ELER-023 unsupported savepoint should create") && ok;
  ok = RecordMutation(&stack,
                      tx,
                      31,
                      txn::SavepointMutationKind::metrics,
                      StableOperationId(txn::SavepointMutationKind::metrics, "after")) && ok;
  const auto executed = stack.ExecuteRollbackTo(tx, "sp", &executor);
  ok = Require(!executed.ok() &&
                   executed.decision ==
                       txn::SavepointRollbackDecision::rollback_refused_executor_unavailable &&
                   executed.diagnostic.diagnostic_code ==
                       "SB-SNTXN-SAVEPOINT-UNDO-EXECUTOR-UNAVAILABLE",
               "ELER-023 unsupported executor should fail closed") && ok;
  ok = Require(executed.undo_results.empty(),
               "ELER-023 unsupported executor must not apply partial undo") && ok;
  ok = Require(executor.Visible(txn::SavepointMutationKind::metrics, "after"),
               "ELER-023 unsupported executor must leave resource visible") && ok;
  ok = Require(executor.UndoCount() == 0,
               "ELER-023 unsupported executor must not write undo evidence") && ok;
  return ok;
}

bool NullExecutorFailsClosed() {
  txn::SavepointStack stack;
  const auto tx = txn::MakeLocalTransactionId(47);
  bool ok = Require(stack.Create(tx, "sp", 40).ok(),
                    "ELER-023 null executor savepoint should create");
  ok = RecordMutation(&stack,
                      tx,
                      41,
                      txn::SavepointMutationKind::data_page,
                      StableOperationId(txn::SavepointMutationKind::data_page, "after")) && ok;
  const auto executed = stack.ExecuteRollbackTo(tx, "sp", nullptr);
  ok = Require(!executed.ok() &&
                   executed.decision ==
                       txn::SavepointRollbackDecision::rollback_refused_executor_missing &&
                   executed.diagnostic.diagnostic_code ==
                       "SB-SNTXN-SAVEPOINT-UNDO-EXECUTOR-MISSING",
               "ELER-023 null executor should fail closed") && ok;
  return ok;
}

bool LegacyApplyRequiresPhysicalExecutorForMutationActions() {
  TempDir temp;
  DurableUndoLedgerExecutor executor(temp.path / "legacy_apply.savepoint_undo_ledger",
                                     AllSupportedKinds());
  bool ok = Require(executor.CreateResource(txn::SavepointMutationKind::data_page,
                                            "after",
                                            "data-after"),
                    "ELER-023 legacy-apply fixture resource should create");
  txn::SavepointStack stack;
  const auto tx = txn::MakeLocalTransactionId(49);
  ok = Require(stack.Create(tx, "sp", 60).ok(),
               "ELER-023 legacy-apply savepoint should create") && ok;
  ok = RecordMutation(&stack,
                      tx,
                      61,
                      txn::SavepointMutationKind::data_page,
                      StableOperationId(txn::SavepointMutationKind::data_page, "after")) && ok;

  const auto refused = stack.ApplyRollbackTo(tx, "sp");
  ok = Require(!refused.ok() &&
                   refused.decision ==
                       txn::SavepointRollbackDecision::rollback_refused_executor_missing &&
                   refused.diagnostic.diagnostic_code ==
                       "SB-SNTXN-SAVEPOINT-UNDO-EXECUTOR-MISSING",
               "ELER-023 legacy apply must not mark physical mutations rolled back") && ok;
  ok = Require(executor.Visible(txn::SavepointMutationKind::data_page, "after"),
               "ELER-023 legacy apply refusal must leave resource visible") && ok;

  const auto executed = stack.ExecuteRollbackTo(tx, "sp", &executor);
  ok = Require(executed.ok() && executed.affected_mutation_count == 1,
               "ELER-023 executor path should complete after legacy apply refusal") && ok;
  const auto idempotent_apply = stack.ApplyRollbackTo(tx, "sp");
  ok = Require(idempotent_apply.ok() && idempotent_apply.affected_mutation_count == 0,
               "ELER-023 legacy apply may only succeed after physical undo is complete") && ok;
  ok = Require(!executor.Visible(txn::SavepointMutationKind::data_page, "after"),
               "ELER-023 executor path should hide legacy-apply resource") && ok;
  return ok;
}

bool ExecutorFailureRequiresReplayBeforeStackMarksRolledBack() {
  TempDir temp;
  const auto ledger_path = temp.path / "partial_failure.savepoint_undo_ledger";
  DurableUndoLedgerExecutor failing_executor(
      ledger_path,
      AllSupportedKinds(),
      StableOperationId(txn::SavepointMutationKind::metrics, "metrics-fail"));
  bool ok = Require(failing_executor.CreateResource(txn::SavepointMutationKind::data_page,
                                                    "data-partial",
                                                    "data-after"),
                    "ELER-023 partial data resource should create");
  ok = Require(failing_executor.CreateResource(txn::SavepointMutationKind::metrics,
                                               "metrics-fail",
                                               "metrics-after"),
               "ELER-023 partial metrics resource should create") && ok;
  txn::SavepointStack stack;
  const auto tx = txn::MakeLocalTransactionId(48);
  ok = Require(stack.Create(tx, "sp", 50).ok(),
               "ELER-023 partial-failure savepoint should create") && ok;
  ok = RecordMutation(&stack,
                      tx,
                      51,
                      txn::SavepointMutationKind::metrics,
                      StableOperationId(txn::SavepointMutationKind::metrics, "metrics-fail")) && ok;
  ok = RecordMutation(&stack,
                      tx,
                      52,
                      txn::SavepointMutationKind::data_page,
                      StableOperationId(txn::SavepointMutationKind::data_page, "data-partial")) && ok;
  const auto failed = stack.ExecuteRollbackTo(tx, "sp", &failing_executor);
  ok = Require(!failed.ok() &&
                   failed.decision ==
                       txn::SavepointRollbackDecision::rollback_refused_executor_failed &&
                   failed.undo_results.size() == 2 &&
                   failed.undo_results.front().applied,
               "ELER-023 executor failure should report partial durable undo") && ok;
  ok = Require(!failing_executor.Visible(txn::SavepointMutationKind::data_page, "data-partial"),
               "ELER-023 partial data undo should be durable") && ok;
  ok = Require(failing_executor.Visible(txn::SavepointMutationKind::metrics, "metrics-fail"),
               "ELER-023 failed metrics undo should remain visible") && ok;

  DurableUndoLedgerExecutor replay_executor(ledger_path, AllSupportedKinds());
  const auto replayed = stack.ExecuteRollbackTo(tx, "sp", &replay_executor);
  if (!replayed.ok()) {
    PrintDiagnostic(replayed.diagnostic);
  }
  ok = Require(replayed.ok() &&
                   replayed.undo_results.size() == 2 &&
                   replayed.undo_results[0].already_applied &&
                   replayed.undo_results[1].applied,
               "ELER-023 stack must require replay before marking rollback applied") && ok;
  ok = Require(!replay_executor.Visible(txn::SavepointMutationKind::data_page, "data-partial"),
               "ELER-023 replay data resource should remain hidden") && ok;
  ok = Require(!replay_executor.Visible(txn::SavepointMutationKind::metrics, "metrics-fail"),
               "ELER-023 replay metrics resource should become hidden") && ok;
  return ok;
}

}  // namespace

int main() {
  bool ok = true;
  ok = AllMutationKindsExecuteDurableUndoAndReplay() && ok;
  ok = MissingUndoEvidenceFailsClosed() && ok;
  ok = UnsupportedExecutorFailsClosedWithoutPartialUndo() && ok;
  ok = NullExecutorFailsClosed() && ok;
  ok = LegacyApplyRequiresPhysicalExecutorForMutationActions() && ok;
  ok = ExecutorFailureRequiresReplayBeforeStackMarksRolledBack() && ok;
  if (!ok) {
    return 1;
  }
  std::cout << "engine_listener_savepoint_physical_undo_conformance=passed\n";
  return 0;
}
