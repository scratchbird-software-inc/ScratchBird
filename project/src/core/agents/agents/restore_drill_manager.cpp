// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/restore_drill_manager.hpp"

// CanonicalAgentRegistry/CanonicalAgentManifest owns production exposure;
// this file provides the local restore-drill workflow handler.

#include <utility>

namespace scratchbird::core::agents::implemented_agents {
namespace {
using scratchbird::core::platform::Severity; using scratchbird::core::platform::StatusCode; using scratchbird::core::platform::Subsystem;
Status OkStatus(){return {StatusCode::ok,Severity::info,Subsystem::engine};}
Status ErrorStatus(){return {StatusCode::memory_invalid_request,Severity::error,Subsystem::engine};}
RestoreDrillManagerResult Finish(RestoreDrillManagerDecisionKind d, Status s, std::string c, std::string k, std::string detail, bool f){RestoreDrillManagerResult r; r.status=s; r.decision=d; r.fail_closed=f; r.diagnostic=MakeRestoreDrillManagerDiagnostic(r.status,std::move(c),std::move(k),std::move(detail)); r.evidence.push_back({"decision",RestoreDrillManagerDecisionKindName(r.decision)}); r.evidence.push_back({"restore_inspection_required","true"}); return r;}
AgentLocalWorkflowRequest WorkflowRequest(const RestoreDrillManagerRequest& r){AgentLocalWorkflowRequest request;request.domain=AgentLocalWorkflowDomain::restore_drill;request.operation_id="run_restore_drill";request.idempotency_key=r.idempotency_key.empty()?r.drill_uuid+":run_restore_drill":r.idempotency_key;request.authority.database_uuid=r.database_uuid;request.authority.principal_uuid=r.principal_uuid;request.authority.subject_uuid=r.drill_uuid;request.authority.mga_transaction_uuid=r.mga_transaction_uuid;request.authority.evidence_uuid=r.evidence_uuid;request.authority.local_transaction_id=r.local_transaction_id;request.authority.catalog_generation=r.catalog_generation;request.authority.durable_catalog_bound=r.durable_catalog_bound;request.authority.transaction_inventory_bound=r.transaction_inventory_bound;request.authority.storage_snapshot_authoritative=r.storage_snapshot_authoritative;request.authority.metadata_authoritative=r.metadata_authoritative;request.authority.parser_authority=r.parser_authority;request.authority.cluster_route_requested=r.cluster_route_requested;request.subsystem_precondition_satisfied=true;request.intended_state_observed=r.intended_state_observed;return request;}
RestoreDrillManagerResult ApplyWorkflow(AgentLocalWorkflowLedger* ledger,RestoreDrillManagerResult result,const RestoreDrillManagerRequest& request){if(ledger==nullptr||result.fail_closed||result.decision!=RestoreDrillManagerDecisionKind::run_restore_drill)return result;const auto workflow=ledger->Apply(WorkflowRequest(request));result.workflow_record=workflow.record;result.workflow_record_written=workflow.ok&&!workflow.idempotent;result.outcome_verified=workflow.record.outcome_verified;result.evidence.push_back({"workflow_uuid",workflow.record.workflow_uuid});result.evidence.push_back({"workflow_state",AgentLocalWorkflowStateName(workflow.record.state)});if(!workflow.ok)return Finish(RestoreDrillManagerDecisionKind::refused,ErrorStatus(),workflow.status.diagnostic_code,"agents.restore_drill.workflow_refused",workflow.status.detail,true);return result;}
}
const char* RestoreDrillManagerDecisionKindName(RestoreDrillManagerDecisionKind d){switch(d){case RestoreDrillManagerDecisionKind::no_action:return "no_action";case RestoreDrillManagerDecisionKind::run_restore_drill:return "run_restore_drill";case RestoreDrillManagerDecisionKind::refused:return "refused";}return "refused";}
DiagnosticRecord MakeRestoreDrillManagerDiagnostic(Status s,std::string c,std::string k,std::string d){return scratchbird::core::platform::MakeDiagnostic(s.code,s.severity,s.subsystem,std::move(c),std::move(k),{{"detail",std::move(d)}},{},"restore_drill_manager",{});}
RestoreDrillManagerResult EvaluateRestoreDrillManagerRequest(const RestoreDrillManagerRequest& r){return EvaluateRestoreDrillManagerRequest(nullptr,r);}
RestoreDrillManagerResult EvaluateRestoreDrillManagerRequest(AgentLocalWorkflowLedger* ledger,const RestoreDrillManagerRequest& r){RestoreDrillManagerResult result;if(r.drill_uuid.empty())result=Finish(RestoreDrillManagerDecisionKind::refused,ErrorStatus(),"SB_AGENT_RESTORE_DRILL_ID_REQUIRED","agents.restore_drill.id_required","drill identity required",true);else if(r.cluster_route_requested||r.parser_authority)result=Finish(RestoreDrillManagerDecisionKind::refused,ErrorStatus(),"SB_AGENT_RESTORE_DRILL_AUTHORITY_UNTRUSTED","agents.restore_drill.untrusted_authority","restore drill must be local and parser non-authoritative",true);else if(r.run_requested&&r.target_isolated&&r.resources_available&&r.backup_manifest_available&&r.restore_inspection_open)result=Finish(RestoreDrillManagerDecisionKind::run_restore_drill,OkStatus(),"SB_AGENT_RESTORE_DRILL_READY","agents.restore_drill.ready","isolated restore drill route is ready",false);else result=Finish(RestoreDrillManagerDecisionKind::no_action,OkStatus(),"SB_AGENT_RESTORE_DRILL_NO_ACTION","agents.restore_drill.no_action","restore drill blockers remain",false);return ApplyWorkflow(ledger,std::move(result),r);}
const char* restore_drill_manager_implementation_anchor(){return "restore_drill_manager";}
}  // namespace scratchbird::core::agents::implemented_agents
