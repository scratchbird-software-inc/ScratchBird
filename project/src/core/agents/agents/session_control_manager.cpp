// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/session_control_manager.hpp"

// CanonicalAgentRegistry/CanonicalAgentManifest owns production exposure;
// this file provides the local session-control workflow handler.

#include <utility>

namespace scratchbird::core::agents::implemented_agents {
namespace {
using scratchbird::core::platform::Severity; using scratchbird::core::platform::StatusCode; using scratchbird::core::platform::Subsystem;
Status OkStatus(){return {StatusCode::ok,Severity::info,Subsystem::engine};}
Status ErrorStatus(){return {StatusCode::memory_invalid_request,Severity::error,Subsystem::engine};}
SessionControlManagerResult Finish(SessionControlManagerDecisionKind d, Status s, std::string c, std::string k, std::string detail, bool f){SessionControlManagerResult r; r.status=s; r.decision=d; r.fail_closed=f; r.diagnostic=MakeSessionControlManagerDiagnostic(r.status,std::move(c),std::move(k),std::move(detail)); r.evidence.push_back({"decision",SessionControlManagerDecisionKindName(r.decision)}); r.evidence.push_back({"client_authority","false"}); return r;}
bool Clean(const SessionControlManagerRequest& r){return !r.session_uuid.empty()&&r.session_visible&&r.security_metrics_authoritative&&!r.cluster_route_requested&&!r.client_authority;}
AgentLocalWorkflowRequest WorkflowRequest(const SessionControlManagerRequest& r,SessionControlManagerDecisionKind decision){AgentLocalWorkflowRequest request;request.domain=AgentLocalWorkflowDomain::session_control;request.operation_id=SessionControlManagerDecisionKindName(decision);request.idempotency_key=r.idempotency_key.empty()?r.session_uuid+":"+request.operation_id:r.idempotency_key;request.authority.database_uuid=r.database_uuid;request.authority.principal_uuid=r.principal_uuid;request.authority.subject_uuid=r.session_uuid;request.authority.mga_transaction_uuid=r.mga_transaction_uuid;request.authority.evidence_uuid=r.evidence_uuid;request.authority.local_transaction_id=r.local_transaction_id;request.authority.catalog_generation=r.catalog_generation;request.authority.durable_catalog_bound=r.durable_catalog_bound;request.authority.transaction_inventory_bound=r.transaction_inventory_bound;request.authority.session_registry_authoritative=r.session_visible;request.authority.security_catalog_authoritative=r.security_metrics_authoritative;request.authority.client_authority=r.client_authority;request.authority.cluster_route_requested=r.cluster_route_requested;request.subsystem_precondition_satisfied=true;request.intended_state_observed=r.intended_state_observed;return request;}
SessionControlManagerResult ApplyWorkflow(AgentLocalWorkflowLedger* ledger,SessionControlManagerResult result,const SessionControlManagerRequest& request){if(ledger==nullptr||result.fail_closed||result.decision==SessionControlManagerDecisionKind::refused)return result;const auto workflow=ledger->Apply(WorkflowRequest(request,result.decision));result.workflow_record=workflow.record;result.workflow_record_written=workflow.ok&&!workflow.idempotent;result.outcome_verified=workflow.record.outcome_verified;result.evidence.push_back({"workflow_uuid",workflow.record.workflow_uuid});result.evidence.push_back({"workflow_state",AgentLocalWorkflowStateName(workflow.record.state)});if(!workflow.ok)return Finish(SessionControlManagerDecisionKind::refused,ErrorStatus(),workflow.status.diagnostic_code,"agents.session_control.workflow_refused",workflow.status.detail,true);return result;}
}
const char* SessionControlManagerDecisionKindName(SessionControlManagerDecisionKind d){switch(d){case SessionControlManagerDecisionKind::force_disconnect:return "force_disconnect";case SessionControlManagerDecisionKind::require_reauth:return "require_reauth";case SessionControlManagerDecisionKind::revoke_session:return "revoke_session";case SessionControlManagerDecisionKind::refused:return "refused";}return "refused";}
DiagnosticRecord MakeSessionControlManagerDiagnostic(Status s,std::string c,std::string k,std::string d){return scratchbird::core::platform::MakeDiagnostic(s.code,s.severity,s.subsystem,std::move(c),std::move(k),{{"detail",std::move(d)}},{},"session_control_manager",{});}
SessionControlManagerResult EvaluateSessionControlManagerRequest(const SessionControlManagerRequest& r){return EvaluateSessionControlManagerRequest(nullptr,r);}
SessionControlManagerResult EvaluateSessionControlManagerRequest(AgentLocalWorkflowLedger* ledger,const SessionControlManagerRequest& r){SessionControlManagerResult result;if(!Clean(r))result=Finish(SessionControlManagerDecisionKind::refused,ErrorStatus(),"SB_AGENT_SESSION_CONTROL_AUTHORITY_UNTRUSTED","agents.session_control.untrusted_authority","session action requires visible session and trusted security metrics",true);else if(r.disconnect_requested&&r.disconnect_allowed)result=Finish(SessionControlManagerDecisionKind::force_disconnect,OkStatus(),"SB_AGENT_SESSION_CONTROL_DISCONNECT_READY","agents.session_control.disconnect_ready","session can be disconnected",false);else if(r.reauth_requested)result=Finish(SessionControlManagerDecisionKind::require_reauth,OkStatus(),"SB_AGENT_SESSION_CONTROL_REAUTH_READY","agents.session_control.reauth_ready","session can be forced to reauthenticate",false);else if(r.revoke_requested&&r.token_visible)result=Finish(SessionControlManagerDecisionKind::revoke_session,OkStatus(),"SB_AGENT_SESSION_CONTROL_REVOKE_READY","agents.session_control.revoke_ready","token/session visible for revocation",false);else result=Finish(SessionControlManagerDecisionKind::refused,ErrorStatus(),"SB_AGENT_SESSION_CONTROL_ACTION_REFUSED","agents.session_control.action_refused","requested session action is not allowed",true);return ApplyWorkflow(ledger,std::move(result),r);}
const char* session_control_manager_implementation_anchor(){return "session_control_manager";}
}  // namespace scratchbird::core::agents::implemented_agents
