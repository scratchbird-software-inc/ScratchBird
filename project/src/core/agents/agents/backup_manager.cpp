// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/backup_manager.hpp"

// CanonicalAgentRegistry/CanonicalAgentManifest owns production exposure;
// this file provides the local backup workflow handler.

#include <utility>

namespace scratchbird::core::agents::implemented_agents {
namespace {
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
Status OkStatus() { return {StatusCode::ok, Severity::info, Subsystem::engine}; }
Status ErrorStatus() { return {StatusCode::memory_invalid_request, Severity::error, Subsystem::engine}; }
void Add(BackupManagerResult* r, std::string k, std::string v) { r->evidence.push_back({std::move(k), std::move(v)}); }
BackupManagerResult Finish(BackupManagerDecisionKind d, Status s, std::string c, std::string k, std::string detail, bool f) {
  BackupManagerResult r; r.status=s; r.decision=d; r.fail_closed=f;
  r.diagnostic=MakeBackupManagerDiagnostic(r.status,std::move(c),std::move(k),std::move(detail));
  Add(&r,"decision",BackupManagerDecisionKindName(r.decision)); Add(&r,"local_workflow","true"); return r;
}
bool BadAuthority(const BackupManagerRequest& r) { return r.cluster_route_requested || r.parser_authority || !r.storage_snapshot_authoritative || !r.mga_hold_authoritative || !r.metadata_authoritative; }
AgentLocalWorkflowRequest WorkflowRequest(const BackupManagerRequest& r,
                                          BackupManagerDecisionKind decision) {
  AgentLocalWorkflowRequest request;
  request.domain = AgentLocalWorkflowDomain::backup;
  request.operation_id = BackupManagerDecisionKindName(decision);
  request.idempotency_key = r.idempotency_key.empty() ? r.backup_uuid + ":" + request.operation_id : r.idempotency_key;
  request.authority.database_uuid = r.database_uuid;
  request.authority.principal_uuid = r.principal_uuid;
  request.authority.subject_uuid = r.backup_uuid;
  request.authority.mga_transaction_uuid = r.mga_transaction_uuid;
  request.authority.evidence_uuid = r.evidence_uuid;
  request.authority.local_transaction_id = r.local_transaction_id;
  request.authority.catalog_generation = r.catalog_generation;
  request.authority.durable_catalog_bound = r.durable_catalog_bound;
  request.authority.transaction_inventory_bound = r.transaction_inventory_bound;
  request.authority.storage_snapshot_authoritative = r.storage_snapshot_authoritative;
  request.authority.metadata_authoritative = r.metadata_authoritative;
  request.authority.parser_authority = r.parser_authority;
  request.authority.cluster_route_requested = r.cluster_route_requested;
  request.subsystem_precondition_satisfied = true;
  request.intended_state_observed = r.intended_state_observed;
  return request;
}
BackupManagerResult ApplyWorkflow(AgentLocalWorkflowLedger* ledger,
                                  BackupManagerResult result,
                                  const BackupManagerRequest& request) {
  if (ledger == nullptr || result.fail_closed ||
      result.decision == BackupManagerDecisionKind::no_action ||
      result.decision == BackupManagerDecisionKind::refused) {
    return result;
  }
  const auto workflow = ledger->Apply(WorkflowRequest(request, result.decision));
  result.workflow_record = workflow.record;
  result.workflow_record_written = workflow.ok && !workflow.idempotent;
  result.outcome_verified = workflow.record.outcome_verified;
  Add(&result, "workflow_uuid", workflow.record.workflow_uuid);
  Add(&result, "workflow_state", AgentLocalWorkflowStateName(workflow.record.state));
  Add(&result, "workflow_verification_evidence_uuid",
      workflow.record.verification_evidence_uuid);
  if (!workflow.ok) {
    return Finish(BackupManagerDecisionKind::refused, ErrorStatus(),
                  workflow.status.diagnostic_code, "agents.backup.workflow_refused",
                  workflow.status.detail, true);
  }
  return result;
}
}
const char* BackupManagerDecisionKindName(BackupManagerDecisionKind d) {
  switch (d) { case BackupManagerDecisionKind::no_action: return "no_action"; case BackupManagerDecisionKind::start_backup: return "start_backup"; case BackupManagerDecisionKind::cancel_backup: return "cancel_backup"; case BackupManagerDecisionKind::verify_backup: return "verify_backup"; case BackupManagerDecisionKind::refused: return "refused"; } return "refused";
}
DiagnosticRecord MakeBackupManagerDiagnostic(Status s, std::string c, std::string k, std::string d) {
  return scratchbird::core::platform::MakeDiagnostic(s.code,s.severity,s.subsystem,std::move(c),std::move(k),{{"detail",std::move(d)}},{},"backup_manager",{});
}
BackupManagerResult EvaluateBackupManagerRequest(const BackupManagerRequest& r) {
  return EvaluateBackupManagerRequest(nullptr, r);
}
BackupManagerResult EvaluateBackupManagerRequest(AgentLocalWorkflowLedger* ledger, const BackupManagerRequest& r) {
  BackupManagerResult result;
  if (r.backup_uuid.empty()) result = Finish(BackupManagerDecisionKind::refused,ErrorStatus(),"SB_AGENT_BACKUP_ID_REQUIRED","agents.backup.id_required","backup identity required",true);
  else if (BadAuthority(r)) result = Finish(BackupManagerDecisionKind::refused,ErrorStatus(),"SB_AGENT_BACKUP_AUTHORITY_UNTRUSTED","agents.backup.untrusted_authority","backup requires local storage/MGA/metadata evidence and external cluster routing",true);
  else if (r.start_requested && r.blockers_clear) result = Finish(BackupManagerDecisionKind::start_backup,OkStatus(),"SB_AGENT_BACKUP_START_READY","agents.backup.start_ready","local backup blockers clear",false);
  else if (r.cancel_requested && r.backup_cancellable) result = Finish(BackupManagerDecisionKind::cancel_backup,OkStatus(),"SB_AGENT_BACKUP_CANCEL_READY","agents.backup.cancel_ready","backup is cancellable",false);
  else if (r.verify_requested && r.manifest_available) result = Finish(BackupManagerDecisionKind::verify_backup,OkStatus(),"SB_AGENT_BACKUP_VERIFY_READY","agents.backup.verify_ready","backup manifest available",false);
  else result = Finish(BackupManagerDecisionKind::no_action,OkStatus(),"SB_AGENT_BACKUP_NO_ACTION","agents.backup.no_action","backup request not actionable",false);
  return ApplyWorkflow(ledger, std::move(result), r);
}
const char* backup_manager_implementation_anchor() { return "backup_manager"; }
}  // namespace scratchbird::core::agents::implemented_agents
