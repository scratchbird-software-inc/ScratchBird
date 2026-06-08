// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/archive_manager.hpp"
#include "agents/backup_manager.hpp"
#include "agents/export_adapter_manager.hpp"
#include "agents/identity_manager.hpp"
#include "agents/job_control_manager.hpp"
#include "agents/pitr_manager.hpp"
#include "agents/restore_drill_manager.hpp"
#include "agents/session_control_manager.hpp"
#include "agent_production_classification.hpp"

#include <cstdlib>
#include <iostream>
#include <map>
#include <string>

namespace {

namespace agents = scratchbird::core::agents;
namespace impl = scratchbird::core::agents::implemented_agents;

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) { Fail(message); }
}

std::map<std::string, agents::AgentProductionExposureRecord> ExposureByAgent() {
  std::map<std::string, agents::AgentProductionExposureRecord> by_agent;
  for (const auto& record : agents::ClassifyAllCanonicalAgentProductionExposures()) {
    by_agent.emplace(record.agent_type_id, record);
  }
  return by_agent;
}

agents::DurableAgentCatalogImage DurableWorkflowCatalog() {
  agents::DurableAgentCatalogImage image;
  image.source = agents::AgentCatalogStateSource::durable_catalog_image;
  image.schema_version = 1;
  image.authority.durable_catalog_authority = true;
  image.authority.mga_transaction_evidence = true;
  image.authority.mga_transaction_uuid = "019f0900-0000-7000-8000-000000000003";
  image.authority.transaction_generation = 9;
  image.authority.evidence_uuid = "019f0900-0000-7000-8000-000000000004";
  image.authority.database_uuid = "019f0900-0000-7000-8000-000000000001";
  image.authority.catalog_storage_uuid = "019f0900-0000-7000-8000-000000000011";
  image.authority.storage_commit_evidence_uuid = image.authority.evidence_uuid;
  image.authority.catalog_generation = 9;
  image.authority.local_transaction_id = 9001;
  image.authority.storage_catalog_record_evidence = true;
  image.authority.transaction_inventory_bound = true;
  image.authority.fsync_or_checkpoint_evidence = true;
  const auto refresh =
      agents::RefreshDurableAgentCatalogAuthorityDigest(
          &image, image.authority.evidence_uuid);
  Require(refresh.ok, "durable workflow catalog root refresh failed");
  return image;
}

void SetStorageWorkflowAuthority(impl::BackupManagerRequest* request,
                                 std::string subject) {
  request->database_uuid = "019f0900-0000-7000-8000-000000000001";
  request->principal_uuid = "019f0900-0000-7000-8000-000000000002";
  request->mga_transaction_uuid = "019f0900-0000-7000-8000-000000000003";
  request->evidence_uuid = "019f0900-0000-7000-8000-000000000004";
  request->idempotency_key = "idem:" + std::move(subject);
  request->local_transaction_id = 9001;
  request->catalog_generation = 9;
  request->durable_catalog_bound = true;
  request->transaction_inventory_bound = true;
  request->storage_snapshot_authoritative = true;
  request->mga_hold_authoritative = true;
  request->metadata_authoritative = true;
  request->intended_state_observed = true;
}

template <typename TRequest>
void SetGenericWorkflowAuthority(TRequest* request, std::string subject) {
  request->database_uuid = "019f0900-0000-7000-8000-000000000001";
  request->principal_uuid = "019f0900-0000-7000-8000-000000000002";
  request->mga_transaction_uuid = "019f0900-0000-7000-8000-000000000003";
  request->evidence_uuid = "019f0900-0000-7000-8000-000000000004";
  request->idempotency_key = "idem:" + std::move(subject);
  request->local_transaction_id = 9001;
  request->catalog_generation = 9;
  request->durable_catalog_bound = true;
  request->transaction_inventory_bound = true;
  request->intended_state_observed = true;
}

void TestBackupArchiveRestorePitrExport() {
  auto catalog = DurableWorkflowCatalog();
  const auto initial_root = catalog.authority.catalog_root_digest;
  agents::AgentLocalWorkflowLedger ledger(&catalog);
  impl::BackupManagerRequest backup;
  backup.backup_uuid = "backup-1";
  SetStorageWorkflowAuthority(&backup, backup.backup_uuid);
  backup.start_requested = true;
  backup.blockers_clear = true;
  const auto backup_result = impl::EvaluateBackupManagerRequest(&ledger, backup);
  Require(backup_result.decision ==
              impl::BackupManagerDecisionKind::start_backup,
          "backup manager did not start local backup");
  Require(backup_result.workflow_record_written && backup_result.outcome_verified,
          "backup manager did not write verified workflow record");
  backup.cluster_route_requested = true;
  Require(!impl::EvaluateBackupManagerRequest(&ledger, backup).ok(),
          "backup manager accepted cluster route in core");

  impl::ArchiveManagerRequest archive;
  archive.slice_uuid = "archive-slice-1";
  SetGenericWorkflowAuthority(&archive, archive.slice_uuid);
  archive.seal_requested = true;
  archive.slice_complete = true;
  archive.metadata_authoritative = true;
  const auto archive_result = impl::EvaluateArchiveManagerRequest(&ledger, archive);
  Require(archive_result.decision ==
              impl::ArchiveManagerDecisionKind::seal_archive_slice,
          "archive manager did not seal complete slice");
  Require(archive_result.workflow_record_written && archive_result.outcome_verified,
          "archive manager did not write verified workflow record");

  impl::RestoreDrillManagerRequest drill;
  drill.drill_uuid = "drill-1";
  SetGenericWorkflowAuthority(&drill, drill.drill_uuid);
  drill.run_requested = true;
  drill.target_isolated = true;
  drill.resources_available = true;
  drill.backup_manifest_available = true;
  drill.restore_inspection_open = true;
  drill.metadata_authoritative = true;
  drill.storage_snapshot_authoritative = true;
  const auto drill_result =
      impl::EvaluateRestoreDrillManagerRequest(&ledger, drill);
  Require(drill_result.decision ==
              impl::RestoreDrillManagerDecisionKind::run_restore_drill,
          "restore drill manager did not accept isolated restore route");
  Require(drill_result.workflow_record_written && drill_result.outcome_verified,
          "restore drill manager did not write verified workflow record");

  impl::PitrManagerRequest pitr;
  pitr.target_uuid = "pitr-target-1";
  SetGenericWorkflowAuthority(&pitr, pitr.target_uuid);
  pitr.restore_plan_requested = true;
  pitr.target_reachable = true;
  pitr.archive_window_authoritative = true;
  pitr.metadata_authoritative = true;
  pitr.storage_snapshot_authoritative = true;
  const auto pitr_result = impl::EvaluatePitrManagerRequest(&ledger, pitr);
  Require(pitr_result.decision ==
              impl::PitrManagerDecisionKind::request_restore_plan,
          "PITR manager did not request restore plan");
  Require(pitr_result.workflow_record_written && pitr_result.outcome_verified,
          "PITR manager did not write verified workflow record");

  impl::ExportAdapterManagerRequest export_request;
  export_request.adapter_uuid = "export-1";
  SetGenericWorkflowAuthority(&export_request, export_request.adapter_uuid);
  export_request.enable_requested = true;
  export_request.adapter_visible = true;
  export_request.config_valid = true;
  export_request.redaction_policy_valid = true;
  export_request.residency_policy_valid = true;
  export_request.metadata_authoritative = true;
  const auto export_result =
      impl::EvaluateExportAdapterManagerRequest(&ledger, export_request);
  Require(export_result.decision ==
              impl::ExportAdapterManagerDecisionKind::enable_export,
          "export manager did not enable valid adapter");
  Require(export_result.workflow_record_written && export_result.outcome_verified,
          "export manager did not write verified workflow record");
  Require(ledger.records().size() == 5,
          "operational storage workflow ledger did not retain all records");
  Require(catalog.evidence.size() == 5 && catalog.actions.size() == 5 &&
              catalog.retained_history.size() == 5,
          "operational storage workflow did not write durable catalog records");
  Require(catalog.authority.catalog_root_digest != initial_root,
          "operational storage workflow did not advance durable root digest");
  Require(agents::ValidateDurableAgentCatalogForProduction(catalog).ok,
          "operational storage workflow catalog failed production validation");
}

void TestIdentitySessionJob() {
  auto catalog = DurableWorkflowCatalog();
  const auto initial_root = catalog.authority.catalog_root_digest;
  agents::AgentLocalWorkflowLedger ledger(&catalog);
  impl::IdentityManagerRequest identity;
  identity.principal_uuid = "principal-1";
  SetGenericWorkflowAuthority(&identity, identity.principal_uuid);
  identity.operator_principal_uuid = "operator-1";
  identity.lock_requested = true;
  identity.identity_metrics_authoritative = true;
  identity.explicit_admin_request = true;
  identity.redaction_policy_valid = true;
  const auto identity_result =
      impl::EvaluateIdentityManagerRequest(&ledger, identity);
  Require(identity_result.decision ==
              impl::IdentityManagerDecisionKind::lock_user,
          "identity manager did not lock user with admin request");
  Require(identity_result.workflow_record_written && identity_result.outcome_verified,
          "identity manager did not write verified workflow record");

  impl::SessionControlManagerRequest session;
  session.session_uuid = "session-1";
  SetGenericWorkflowAuthority(&session, session.session_uuid);
  session.disconnect_requested = true;
  session.session_visible = true;
  session.disconnect_allowed = true;
  session.security_metrics_authoritative = true;
  const auto session_result =
      impl::EvaluateSessionControlManagerRequest(&ledger, session);
  Require(session_result.decision ==
              impl::SessionControlManagerDecisionKind::force_disconnect,
          "session control manager did not disconnect eligible session");
  Require(session_result.workflow_record_written && session_result.outcome_verified,
          "session control manager did not write verified workflow record");

  impl::JobControlManagerRequest job;
  job.job_uuid = "job-1";
  SetGenericWorkflowAuthority(&job, job.job_uuid);
  job.retry_requested = true;
  job.job_visible = true;
  job.retry_policy_valid = true;
  job.job_metrics_authoritative = true;
  const auto job_result = impl::EvaluateJobControlManagerRequest(&ledger, job);
  Require(job_result.decision ==
              impl::JobControlManagerDecisionKind::retry_job,
          "job control manager did not retry eligible job");
  Require(job_result.workflow_record_written && job_result.outcome_verified,
          "job control manager did not write verified workflow record");
  Require(ledger.records().size() == 3,
          "identity/session/job workflow ledger did not retain all records");
  Require(catalog.evidence.size() == 3 && catalog.actions.size() == 3 &&
              catalog.retained_history.size() == 3,
          "identity/session/job workflow did not write durable catalog records");
  Require(catalog.authority.catalog_root_digest != initial_root,
          "identity/session/job workflow did not advance durable root digest");
  Require(agents::ValidateDurableAgentCatalogForProduction(catalog).ok,
          "identity/session/job workflow catalog failed production validation");
}

void TestProductionClassificationNoLongerAnchorOnly() {
  const auto by_agent = ExposureByAgent();
  for (const std::string agent : {
           "backup_manager",
           "archive_manager",
           "restore_drill_manager",
           "pitr_manager",
           "export_adapter_manager",
           "identity_manager",
           "session_control_manager",
           "job_control_manager"}) {
    const auto found = by_agent.find(agent);
    Require(found != by_agent.end(), "missing exposure record: " + agent);
    Require(!found->second.implementation_anchor_only,
            "operational workflow still classified anchor-only: " + agent);
    Require(!found->second.route_evidence_kind.empty(),
            "operational workflow lacks route evidence classification: " + agent);
    Require(!found->second.production_live_route_available,
            "operational workflow exposed live mutation without proof: " + agent);
    Require(found->second.workflow_route_proven,
            "operational workflow lacks workflow-only proof: " + agent);
    Require(found->second.exposure ==
                agents::AgentProductionExposureClass::workflow_only,
            "operational workflow is not explicitly workflow-only: " + agent);
  }
}

}  // namespace

int main() {
  TestBackupArchiveRestorePitrExport();
  TestIdentitySessionJob();
  TestProductionClassificationNoLongerAnchorOnly();
  return EXIT_SUCCESS;
}
