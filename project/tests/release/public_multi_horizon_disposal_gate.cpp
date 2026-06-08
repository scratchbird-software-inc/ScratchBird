// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/cleanup_archive_manager.hpp"
#include "backup_archive/backup_archive_api.hpp"
#include "public_release_authz_fixture.hpp"
#include "uuid.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace agents = scratchbird::core::agents::implemented_agents;
namespace api = scratchbird::engine::internal_api;
namespace uuid = scratchbird::core::uuid;

using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::u64;

constexpr u64 kBaseMillis = 1770700000000ull;

bool Expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
}

TypedUuid MakeUuid(UuidKind kind, u64 offset) {
  const auto generated =
      uuid::GenerateEngineIdentityV7(kind, kBaseMillis + offset);
  return generated.ok() ? generated.value : TypedUuid{};
}

std::string UuidText(UuidKind kind, u64 offset) {
  return uuid::UuidToString(MakeUuid(kind, offset).value);
}

api::EngineRequestContext Context(const std::filesystem::path& work_dir) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = "pcr086-multi-horizon-disposal";
  context.database_path = (work_dir / "pcr086.sbdb").string();
  context.database_uuid.canonical = UuidText(UuidKind::database, 1);
  context.principal_uuid.canonical = UuidText(UuidKind::principal, 2);
  context.session_uuid.canonical = UuidText(UuidKind::object, 3);
  context.transaction_uuid.canonical = UuidText(UuidKind::transaction, 4);
  context.local_transaction_id = 30;
  context.snapshot_visible_through_local_transaction_id = 30;
  context.security_context_present = true;
  context.trace_tags.push_back("right:BACKUP_CREATE");
  context.identifier_profile_uuid = "sbsql_v3";
  context.language_context.language_tag = "en";
  context.language_context.default_language_tag = "en";
  scratchbird::tests::release::GrantMaterializedRight(
      &context, "BACKUP_CREATE");
  return context;
}

api::EngineEvaluateHistoryDisposalMultiHorizonRequest BaseRequest(
    const std::filesystem::path& work_dir) {
  api::EngineEvaluateHistoryDisposalMultiHorizonRequest request;
  request.context = Context(work_dir);
  request.disposable_start_transaction_id = 10;
  request.disposable_end_transaction_id = 20;
  request.filespace_uuid = UuidText(UuidKind::filespace, 5);
  request.archive_manifest_uri =
      (work_dir / "archive-before-delete.manifest").string();
  request.write_after_segment_uri =
      (work_dir / "write-after-before-truncate.delta").string();
  request.physical_reclaim_requested = true;
  request.archive_deletion_requested = true;
  request.write_after_truncation_requested = true;
  request.reader_horizon_authoritative = true;
  request.reader_horizon_transaction_id = 21;
  request.writer_horizon_authoritative = true;
  request.writer_horizon_transaction_id = 21;
  request.parser_snapshot_horizon_authoritative = true;
  request.parser_snapshot_horizon_transaction_id = 21;
  request.backup_forward_horizon_authoritative = true;
  request.backup_forward_horizon_transaction_id = 21;
  request.archive_horizon_authoritative = true;
  request.archive_horizon_transaction_id = 21;
  request.legal_hold_horizon_authoritative = true;
  request.legal_hold_horizon_transaction_id = 21;
  request.detached_filespace_horizon_authoritative = true;
  request.detached_filespace_horizon_transaction_id = 21;
  request.stable_checkpoint_horizon_authoritative = true;
  request.stable_checkpoint_horizon_transaction_id = 21;
  request.local_durable_horizon_authoritative = true;
  request.local_durable_horizon_transaction_id = 21;
  request.restore_reachability_horizon_authoritative = true;
  request.restore_reachability_horizon_transaction_id = 21;
  return request;
}

bool HasDiagnosticDetail(const api::EngineApiResult& result,
                         std::string_view detail) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.detail.find(detail) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind &&
        evidence.evidence_id.find(id) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool HasAgentEvidence(const agents::CleanupArchiveManagerResult& result,
                      std::string_view key,
                      std::string_view value) {
  for (const auto& evidence : result.evidence) {
    if (evidence.key == key && evidence.value == value) {
      return true;
    }
  }
  return false;
}

bool AuthorizedProof(const std::filesystem::path& work_dir) {
  bool ok = true;
  const auto result =
      api::EngineEvaluateHistoryDisposalMultiHorizon(BaseRequest(work_dir));
  ok = Expect(result.ok, "PCR-086 all-clear disposal guard should pass") && ok;
  ok = Expect(result.disposal_authorized,
              "PCR-086 disposal should be authorized") &&
       ok;
  ok = Expect(result.physical_reclaim_authorized,
              "PCR-086 physical reclaim should be authorized") &&
       ok;
  ok = Expect(result.archive_deletion_authorized,
              "PCR-086 archive deletion should be authorized") &&
       ok;
  ok = Expect(result.write_after_truncation_authorized,
              "PCR-086 write-after truncation should be authorized") &&
       ok;
  ok = Expect(!result.fail_closed,
              "PCR-086 all-clear result should not fail closed") &&
       ok;
  ok = Expect(!result.mutation_performed,
              "PCR-086 guard should not mutate storage directly") &&
       ok;
  ok = Expect(!result.transaction_finality_authority,
              "PCR-086 guard must not become transaction finality authority") &&
       ok;
  ok = Expect(!result.write_after_recovery_authority,
              "PCR-086 write-after evidence must not become recovery authority") &&
       ok;
  ok = Expect(!result.cluster_recovery_authority,
              "PCR-086 cluster recovery authority must stay external") &&
       ok;
  ok = Expect(HasEvidence(result,
                          "finality_source",
                          "local_mga_transaction_inventory"),
              "PCR-086 finality source evidence missing") &&
       ok;
  ok = Expect(HasEvidence(result, "mutation_performed", "false"),
              "PCR-086 non-mutating guard evidence missing") &&
       ok;
  return ok;
}

bool BlockerProof(const std::filesystem::path& work_dir) {
  struct BlockerCase {
    std::string name;
    std::string expected_detail;
    void (*mutate)(api::EngineEvaluateHistoryDisposalMultiHorizonRequest*);
  };
  const std::vector<BlockerCase> cases = {
      {"reader",
       "MULTI_HORIZON_RANGE_HELD:reader",
       [](api::EngineEvaluateHistoryDisposalMultiHorizonRequest* request) {
         request->reader_horizon_transaction_id = 20;
       }},
      {"writer",
       "MULTI_HORIZON_RANGE_HELD:writer",
       [](api::EngineEvaluateHistoryDisposalMultiHorizonRequest* request) {
         request->writer_horizon_transaction_id = 20;
       }},
      {"parser_snapshot",
       "MULTI_HORIZON_RANGE_HELD:parser_snapshot",
       [](api::EngineEvaluateHistoryDisposalMultiHorizonRequest* request) {
         request->parser_snapshot_horizon_transaction_id = 20;
       }},
      {"backup_forward",
       "MULTI_HORIZON_RANGE_HELD:backup_forward",
       [](api::EngineEvaluateHistoryDisposalMultiHorizonRequest* request) {
         request->backup_forward_horizon_transaction_id = 20;
       }},
      {"archive",
       "MULTI_HORIZON_RANGE_HELD:archive",
       [](api::EngineEvaluateHistoryDisposalMultiHorizonRequest* request) {
         request->archive_horizon_transaction_id = 20;
       }},
      {"legal_hold",
       "MULTI_HORIZON_HOLD_ACTIVE:legal_hold",
       [](api::EngineEvaluateHistoryDisposalMultiHorizonRequest* request) {
         request->legal_hold_active = true;
       }},
      {"detached_filespace",
       "MULTI_HORIZON_HOLD_ACTIVE:detached_filespace",
       [](api::EngineEvaluateHistoryDisposalMultiHorizonRequest* request) {
         request->detached_filespace_hold_active = true;
       }},
      {"stable_checkpoint",
       "MULTI_HORIZON_RANGE_HELD:stable_checkpoint",
       [](api::EngineEvaluateHistoryDisposalMultiHorizonRequest* request) {
         request->stable_checkpoint_horizon_transaction_id = 20;
       }},
      {"local_durable",
       "MULTI_HORIZON_RANGE_HELD:local_durable",
       [](api::EngineEvaluateHistoryDisposalMultiHorizonRequest* request) {
         request->local_durable_horizon_transaction_id = 20;
       }},
      {"restore_reachability",
       "MULTI_HORIZON_RANGE_HELD:restore_reachability",
       [](api::EngineEvaluateHistoryDisposalMultiHorizonRequest* request) {
         request->restore_reachability_horizon_transaction_id = 20;
       }},
  };

  bool ok = true;
  for (const auto& item : cases) {
    auto request = BaseRequest(work_dir);
    item.mutate(&request);
    const auto result =
        api::EngineEvaluateHistoryDisposalMultiHorizon(request);
    ok = Expect(!result.ok,
                "PCR-086 blocker should fail closed: " + item.name) &&
         ok;
    ok = Expect(result.fail_closed,
                "PCR-086 blocker fail_closed flag missing: " + item.name) &&
         ok;
    ok = Expect(result.blocking_horizon_kind == item.name,
                "PCR-086 blocker kind mismatch: " + item.name) &&
         ok;
    ok = Expect(!result.physical_reclaim_authorized &&
                    !result.archive_deletion_authorized &&
                    !result.write_after_truncation_authorized,
                "PCR-086 blocker authorized a disposal action: " + item.name) &&
         ok;
    ok = Expect(HasDiagnosticDetail(result, item.expected_detail),
                "PCR-086 blocker diagnostic mismatch: " + item.name) &&
         ok;
    ok = Expect(!result.mutation_performed,
                "PCR-086 blocker should not mutate: " + item.name) &&
         ok;
  }
  return ok;
}

bool FailClosedPolicyProof(const std::filesystem::path& work_dir) {
  bool ok = true;
  auto missing = BaseRequest(work_dir);
  missing.reader_horizon_authoritative = false;
  const auto missing_result =
      api::EngineEvaluateHistoryDisposalMultiHorizon(missing);
  ok = Expect(!missing_result.ok && missing_result.fail_closed,
              "PCR-086 missing horizon proof should fail closed") &&
       ok;
  ok = Expect(HasDiagnosticDetail(
                  missing_result,
                  "MULTI_HORIZON_PROOF_NOT_AUTHORITATIVE:reader"),
              "PCR-086 missing proof diagnostic mismatch") &&
       ok;

  auto cluster = BaseRequest(work_dir);
  cluster.option_envelopes.push_back("scope:cluster");
  const auto cluster_result =
      api::EngineEvaluateHistoryDisposalMultiHorizon(cluster);
  ok = Expect(!cluster_result.ok && cluster_result.cluster_authority_required,
              "PCR-086 cluster disposal should require external provider") &&
       ok;
  ok = Expect(HasDiagnosticDetail(cluster_result,
                                  "MULTI_HORIZON_CLUSTER_PROVIDER_REQUIRED"),
              "PCR-086 cluster diagnostic mismatch") &&
       ok;

  auto wal = BaseRequest(work_dir);
  wal.option_envelopes.push_back("authoritative_wal:true");
  const auto wal_result =
      api::EngineEvaluateHistoryDisposalMultiHorizon(wal);
  ok = Expect(!wal_result.ok && wal_result.fail_closed,
              "PCR-086 WAL authority should fail closed") &&
       ok;
  ok = Expect(HasDiagnosticDetail(wal_result,
                                  "MULTI_HORIZON_AUTHORITATIVE_WAL_FORBIDDEN"),
              "PCR-086 WAL diagnostic mismatch") &&
       ok;
  return ok;
}

agents::CleanupArchiveManagerSnapshot AgentSnapshot() {
  agents::CleanupArchiveManagerSnapshot snapshot;
  snapshot.authoritative_cleanup_horizon = 30;
  snapshot.current_cleanup_lwm = 20;
  snapshot.cleanup_horizon_authoritative = true;
  snapshot.archive_metadata_authoritative = true;
  snapshot.disposal_guard_requested = true;
  return snapshot;
}

bool AgentDisposalGuardProof() {
  bool ok = true;
  auto held = AgentSnapshot();
  held.restore_reachability_hold_active = true;
  const auto held_result = agents::EvaluateCleanupArchiveManager(held);
  ok = Expect(!held_result.ok() && held_result.fail_closed,
              "PCR-086 agent disposal guard should fail closed") &&
       ok;
  ok = Expect(held_result.decision ==
                  agents::CleanupArchiveManagerDecisionKind::refused,
              "PCR-086 agent disposal guard should refuse") &&
       ok;
  ok = Expect(held_result.diagnostic.diagnostic_code ==
                  "SB_AGENT_CLEANUP_ARCHIVE_MULTI_HORIZON_HOLD_ACTIVE",
              "PCR-086 agent diagnostic mismatch") &&
       ok;
  ok = Expect(HasAgentEvidence(held_result,
                               "restore_reachability_hold_active",
                               "true"),
              "PCR-086 agent restore reachability evidence missing") &&
       ok;

  const auto clear_result =
      agents::EvaluateCleanupArchiveManager(AgentSnapshot());
  ok = Expect(clear_result.ok(),
              "PCR-086 agent all-clear cleanup/archive decision should pass") &&
       ok;
  ok = Expect(clear_result.decision ==
                  agents::CleanupArchiveManagerDecisionKind::advance_cleanup_lwm,
              "PCR-086 agent all-clear decision mismatch") &&
       ok;
  ok = Expect(HasAgentEvidence(clear_result,
                               "disposal_guard_requested",
                               "true"),
              "PCR-086 agent disposal guard evidence missing") &&
       ok;
  return ok;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: public_multi_horizon_disposal_gate <work-dir>\n";
    return 2;
  }
  const std::filesystem::path work_dir = argv[1];
  std::filesystem::create_directories(work_dir);

  bool ok = true;
  ok = AuthorizedProof(work_dir) && ok;
  ok = BlockerProof(work_dir) && ok;
  ok = FailClosedPolicyProof(work_dir) && ok;
  ok = AgentDisposalGuardProof() && ok;
  return ok ? 0 : 1;
}
