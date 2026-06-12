// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "storage/storage_management_api.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

namespace {

namespace engine = scratchbird::engine::internal_api;
namespace disk = scratchbird::storage::disk;
namespace filespace = scratchbird::storage::filespace;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool HasEvidence(const engine::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && evidence.evidence_id == id) {
      return true;
    }
  }
  return false;
}

engine::EngineRequestContext Context(bool mutation_right = false) {
  engine::EngineRequestContext context;
  context.trust_mode = engine::EngineTrustMode::embedded_in_process;
  context.security_context_present = true;
  context.database_uuid.canonical = "019e3000-0000-7000-8000-000000000001";
  context.transaction_uuid.canonical = "019e3000-0000-7000-8000-000000000002";
  context.local_transaction_id = mutation_right ? 7001 : 0;
  context.catalog_generation_id = 12;
  context.resource_epoch = 34;
  context.trace_tags.push_back("security.fixture_trace_authority");
  context.trace_tags.push_back("right:OBS_CONFIG_INSPECT");
  if (mutation_right) {
    context.trace_tags.push_back("right:FILESPACE_LIFECYCLE_CONTROL");
  }
  return context;
}

engine::EngineStorageTierMigrationDescriptor Descriptor() {
  engine::EngineStorageTierMigrationDescriptor descriptor;
  descriptor.storage_tier_policy_uuid.canonical = "019e3000-0000-7000-8000-000000000010";
  descriptor.source_tier_uuid.canonical = "019e3000-0000-7000-8000-000000000011";
  descriptor.target_tier_uuid.canonical = "019e3000-0000-7000-8000-000000000012";
  descriptor.source_tier_class = engine::EngineStorageTierClass::hot;
  descriptor.target_tier_class = engine::EngineStorageTierClass::cold;
  descriptor.target_filespace_role = filespace::FilespaceRole::secondary_data;
  descriptor.page_types = {disk::PageType::row_data, disk::PageType::blob};
  descriptor.expected_catalog_generation = 12;
  descriptor.observed_catalog_generation = 12;
  descriptor.expected_policy_generation = 34;
  descriptor.observed_policy_generation = 34;
  descriptor.storage_tier_policy_resolved = true;
  descriptor.filespace_role_known = true;
  descriptor.page_family_eligibility_validated = true;
  descriptor.typed_dependency_manifest_validated = false;
  return descriptor;
}

engine::EngineStorageTierMigrationRequest Request(
    engine::EngineStorageTierMigrationOperation operation =
        engine::EngineStorageTierMigrationOperation::plan_migration) {
  engine::EngineStorageTierMigrationRequest request;
  request.context = Context(operation == engine::EngineStorageTierMigrationOperation::stage_migration ||
                            operation == engine::EngineStorageTierMigrationOperation::commit_migration ||
                            operation == engine::EngineStorageTierMigrationOperation::rollback_migration);
  request.operation_id = engine::EngineStorageTierMigrationOperationName(operation);
  request.tier_operation = operation;
  request.target_object.object_kind = "filespace";
  request.target_object.uuid.canonical = "019e3000-0000-7000-8000-000000000020";
  request.descriptor = Descriptor();
  return request;
}

void WriteFile(const std::filesystem::path& path, const std::string& payload) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  Require(out.is_open(), "storage tier test could not create fixture file");
  out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  Require(in.is_open(), "storage tier test could not read fixture file");
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

void AuthorizePhysicalRelocation(engine::EngineStorageTierMigrationRequest* request,
                                 const std::filesystem::path& source,
                                 const std::filesystem::path& target) {
  request->descriptor.physical_data_movement_requested = true;
  request->descriptor.physical_data_movement_authorized = true;
  request->descriptor.physical_rewrite_plan_validated = true;
  request->descriptor.backup_export_repair_profile_validated = true;
  request->descriptor.allow_physical_target_overwrite = true;
  request->descriptor.source_physical_path = source.string();
  request->descriptor.target_physical_path = target.string();
}

void TestDescriptorPlanSucceedsWithoutMutation() {
  const auto result = engine::EnginePlanStorageTierMigrationOperation(Request());
  Require(result.ok, "storage tier descriptor plan failed");
  Require(result.result_shape.result_kind == "rs.storage_tier.descriptor_plan.v1",
          "storage tier descriptor result shape mismatch");
  Require(!result.durable_state_changed, "storage tier descriptor plan mutated durable state");
  Require(!result.physical_data_movement_dispatched,
          "storage tier descriptor plan dispatched physical movement");
  Require(!result.parser_storage_authority,
          "storage tier descriptor plan gave parser storage authority");
  Require(!result.private_provider_dispatch,
          "storage tier descriptor plan dispatched a private provider");
  Require(HasEvidence(result, "mga_visibility_authority", "durable_transaction_inventory"),
          "storage tier descriptor plan omitted MGA authority evidence");
  Require(HasEvidence(result, "parser_storage_authority", "false"),
          "storage tier descriptor plan omitted parser authority refusal evidence");
}

void TestRefusals() {
  auto missing_policy = Request();
  missing_policy.descriptor.storage_tier_policy_resolved = false;
  const auto missing_policy_result =
      engine::EnginePlanStorageTierMigrationOperation(missing_policy);
  Require(!missing_policy_result.ok, "storage tier accepted missing policy");

  auto same_tier = Request();
  same_tier.descriptor.target_tier_uuid = same_tier.descriptor.source_tier_uuid;
  const auto same_tier_result = engine::EnginePlanStorageTierMigrationOperation(same_tier);
  Require(!same_tier_result.ok, "storage tier accepted same source and target tier");

  auto stale_policy = Request();
  stale_policy.descriptor.observed_policy_generation = 33;
  const auto stale_policy_result =
      engine::EnginePlanStorageTierMigrationOperation(stale_policy);
  Require(!stale_policy_result.ok, "storage tier accepted stale policy generation");

  auto forbidden_role = Request();
  forbidden_role.descriptor.target_filespace_role = filespace::FilespaceRole::archive_history;
  const auto forbidden_role_result =
      engine::EnginePlanStorageTierMigrationOperation(forbidden_role);
  Require(!forbidden_role_result.ok, "storage tier accepted forbidden filespace role");
  Require(!forbidden_role_result.diagnostics.empty() &&
              forbidden_role_result.diagnostics.front().code ==
                  "SB_DIAG_PAGE_FILESPACE_ROLE_FORBIDDEN",
          "storage tier forbidden role diagnostic mismatch");

  auto typed_dependency = Request();
  typed_dependency.descriptor.page_types = {disk::PageType::index_btree_leaf};
  typed_dependency.descriptor.typed_dependency_manifest_validated = false;
  const auto typed_dependency_result =
      engine::EnginePlanStorageTierMigrationOperation(typed_dependency);
  Require(!typed_dependency_result.ok, "storage tier accepted missing typed dependency manifest");
  Require(!typed_dependency_result.diagnostics.empty() &&
              typed_dependency_result.diagnostics.front().code ==
                  "STORAGE_TIER.TYPED_DEPENDENCY_MANIFEST_REQUIRED",
          "storage tier typed dependency diagnostic mismatch");

  auto cluster_scoped = Request();
  cluster_scoped.descriptor.cluster_scoped = true;
  const auto cluster_result = engine::EnginePlanStorageTierMigrationOperation(cluster_scoped);
  Require(!cluster_result.ok, "storage tier accepted missing cluster authority");
  Require(!cluster_result.diagnostics.empty() &&
              cluster_result.diagnostics.front().code ==
                  "STORAGE_TIER.CLUSTER_AUTHORITY_REQUIRED",
          "storage tier cluster authority diagnostic mismatch");

  auto physical = Request();
  physical.descriptor.physical_data_movement_requested = true;
  const auto physical_result = engine::EnginePlanStorageTierMigrationOperation(physical);
  Require(!physical_result.ok, "storage tier descriptor planner accepted physical movement");
  Require(!physical_result.diagnostics.empty() &&
              physical_result.diagnostics.front().code ==
                  "STORAGE_TIER.PHYSICAL_RELOCATION_OPERATION_UNSUPPORTED",
          "storage tier physical movement diagnostic mismatch");

  auto mutation_without_tx = Request(engine::EngineStorageTierMigrationOperation::stage_migration);
  mutation_without_tx.context.local_transaction_id = 0;
  const auto mutation_result =
      engine::EnginePlanStorageTierMigrationOperation(mutation_without_tx);
  Require(!mutation_result.ok, "storage tier mutation accepted missing local transaction");
}

void TestBoundedPhysicalRelocationWorkflow() {
  const auto base = std::filesystem::temp_directory_path() /
                    "scratchbird-storage-tier-physical";
  const auto source = base / "hot" / "member.fsp";
  const auto target = base / "cold" / "member.fsp";
  std::filesystem::remove(target);
  std::filesystem::remove(source);
  WriteFile(source, "scratchbird storage tier relocation fixture\npage-family-bytes\n");

  auto missing_authority =
      Request(engine::EngineStorageTierMigrationOperation::stage_migration);
  missing_authority.descriptor.physical_data_movement_requested = true;
  missing_authority.descriptor.source_physical_path = source.string();
  missing_authority.descriptor.target_physical_path = target.string();
  const auto refused =
      engine::EnginePlanStorageTierMigrationOperation(missing_authority);
  Require(!refused.ok, "storage tier physical relocation accepted missing authority");
  Require(!refused.diagnostics.empty() &&
              refused.diagnostics.front().code ==
                  "STORAGE_TIER.PHYSICAL_MOVEMENT_AUTHORITY_REQUIRED",
          "storage tier physical authority diagnostic mismatch");

  auto stage = Request(engine::EngineStorageTierMigrationOperation::stage_migration);
  AuthorizePhysicalRelocation(&stage, source, target);
  const auto staged = engine::EnginePlanStorageTierMigrationOperation(stage);
  Require(staged.ok, "storage tier physical relocation stage failed");
  Require(staged.result_shape.result_kind == "rs.storage_tier.physical_relocation.v1",
          "storage tier physical relocation result shape mismatch");
  Require(!staged.durable_state_changed,
          "storage tier stage should not claim durable catalog mutation");
  Require(staged.physical_data_movement_dispatched,
          "storage tier stage did not dispatch physical movement");
  Require(staged.physical_digest_verified,
          "storage tier stage did not verify digest");
  Require(staged.physical_data_movement_bytes > 0,
          "storage tier stage did not report moved bytes");
  Require(!staged.parser_storage_authority,
          "storage tier stage gave parser storage authority");
  Require(!staged.private_provider_dispatch,
          "storage tier stage dispatched a private provider");
  Require(ReadFile(source) == ReadFile(target),
          "storage tier physical relocation did not copy source bytes");
  Require(HasEvidence(staged, "storage_tier_physical_relocation",
                      "storage_tier.stage_migration"),
          "storage tier stage relocation evidence missing");
  Require(HasEvidence(staged, "physical_data_movement_dispatched", "true"),
          "storage tier stage physical dispatch evidence missing");
  Require(HasEvidence(staged, "physical_digest_verified", "true"),
          "storage tier stage digest evidence missing");

  auto commit = Request(engine::EngineStorageTierMigrationOperation::commit_migration);
  AuthorizePhysicalRelocation(&commit, source, target);
  const auto committed = engine::EnginePlanStorageTierMigrationOperation(commit);
  Require(committed.ok, "storage tier physical relocation commit failed");
  Require(committed.durable_state_changed,
          "storage tier commit did not mark durable state changed");
  Require(committed.cache_invalidation_required,
          "storage tier commit did not require cache invalidation");
  Require(committed.physical_data_movement_dispatched,
          "storage tier commit did not dispatch physical relocation verification");
  Require(committed.physical_digest_verified,
          "storage tier commit did not verify physical relocation digest");
  Require(HasEvidence(committed, "durable_state_changed", "true"),
          "storage tier commit durable evidence missing");
  Require(HasEvidence(committed, "parser_storage_authority", "false"),
          "storage tier commit parser authority evidence missing");
  Require(HasEvidence(committed, "private_provider_dispatch", "false"),
          "storage tier commit private provider evidence missing");
  Require(HasEvidence(committed,
                      "mga_visibility_authority",
                      "durable_transaction_inventory"),
          "storage tier commit MGA authority evidence missing");
}

}  // namespace

int main() {
  TestDescriptorPlanSucceedsWithoutMutation();
  TestRefusals();
  TestBoundedPhysicalRelocationWorkflow();
  return EXIT_SUCCESS;
}
