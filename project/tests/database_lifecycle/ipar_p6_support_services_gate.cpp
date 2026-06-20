// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/ipar_fast_path_support.hpp"
#include "diagnostics/diagnostic_template_cache.hpp"
#include "ipar_memory_resource_services.hpp"
#include "memory.hpp"
#include "query_memory_arena.hpp"
#include "resource_residency_cache.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace scratchbird::core::memory;
using namespace scratchbird::core::resources;
using namespace scratchbird::engine::internal_api;

void Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "ipar_p6_support_services_gate failure: " << message << '\n';
    std::exit(1);
  }
}

IparResidencyEpoch ResidencyEpoch(std::uint64_t value) {
  return {value, value, value, value, value};
}

ResourceResidencyEpochVector ResourceEpoch(std::uint64_t value) {
  return {value, value, value, value, value, value, value, value};
}

IparCompressedEpochVector FastEpoch(std::uint64_t value) {
  return CompressIparFastPathEpochVector(
      {value, value, value, value, value, value, value});
}

EngineDescriptor Descriptor(std::string uuid,
                            std::string kind,
                            std::string type,
                            std::string encoded) {
  EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = std::move(uuid);
  descriptor.descriptor_kind = std::move(kind);
  descriptor.canonical_type_name = std::move(type);
  descriptor.encoded_descriptor = std::move(encoded);
  return descriptor;
}

void ProveHotResidency() {
  std::vector<IparResidencyEntry> entries = {
      {"table-desc", "table-uuid", IparResidencyKind::table_descriptor,
       ResidencyEpoch(1), 4096, 100, 80, 20, 8, true, true},
      {"index-root", "index-uuid", IparResidencyKind::index_root,
       ResidencyEpoch(1), 4096, 90, 60, 15, 7, false, true},
      {"row-encoder", "table-uuid", IparResidencyKind::row_encoder,
       ResidencyEpoch(1), 4096, 88, 50, 30, 7, false, true},
      {"cold-page", "page-uuid", IparResidencyKind::page_buffer,
       ResidencyEpoch(1), 32768, 1, 0, 1, 1, false, true}};
  IparHotResidencyPolicy policy;
  policy.max_resident_bytes = 49152;
  policy.pressure_target_bytes = 12288;
  policy.max_entry_bytes = 65536;
  auto decision = PlanIparHotResidency(entries,
                                       policy,
                                       MemoryPressureState::high_pressure);
  Require(decision.ok(), "hot residency decision should pass");
  Require(decision.admitted_bytes <= policy.pressure_target_bytes + 4096,
          "pressure target should bound admitted bytes except pinned entry");
  bool evicted_cold = false;
  bool kept_descriptor = false;
  for (const auto& entry : decision.entries) {
    if (entry.entry.entry_id == "cold-page") {
      evicted_cold = entry.action != IparResidencyAction::keep_hot &&
                     entry.action != IparResidencyAction::keep_warm;
    }
    if (entry.entry.entry_id == "table-desc") {
      kept_descriptor = entry.action == IparResidencyAction::keep_hot;
    }
  }
  Require(evicted_cold, "cold page buffer should not remain resident under pressure");
  Require(kept_descriptor, "hot table descriptor should stay resident");
}

void ProveGovernance() {
  IparResourceGovernanceRequest request;
  request.work_kind = IparGovernedWorkKind::dml;
  request.operation_id = "insert.batch.1";
  request.database_id = "db";
  request.session_id = "session";
  request.filespace_id = "fs";
  request.requested_memory_bytes = 8192;
  request.requested_dirty_pages = 2;
  request.requested_queue_slots = 2;
  request.total_governed_limit_bytes = 65536;
  request.total_governed_used_bytes = 56000;
  request.support_agent_reserved_bytes = 8192;
  request.support_agent_used_bytes = 1024;
  request.scopes.push_back({"session", "session", 32768, 4096, 8, 2, 16, 2});
  request.scopes.push_back({"database", "db", 65536, 56000, 16, 8, 32, 12});
  auto decision = PlanIparResourceGovernance(request);
  Require(decision.ok(), "governance should throttle without fail-closing");
  Require(decision.action == IparGovernanceAction::throttle,
          "support-agent reserve should throttle foreground work");
  Require(!decision.throttle_reasons.empty(), "throttle reason should be visible");
}

void ProveResourceResidency() {
  ResourceResidencyCache cache(32768);
  ResourceResidencyEntry entry;
  entry.family = ResourceSeedFamily::charset;
  entry.resource_name = "utf8";
  entry.version = "v1";
  entry.content_hash = "hash:utf8";
  entry.epoch = ResourceEpoch(1);
  entry.bytes = 4096;
  auto put = cache.Put(entry);
  Require(put.ok(), "resource residency put should pass");
  auto hit = cache.Lookup(ResourceSeedFamily::charset, "utf8", "v1", ResourceEpoch(1));
  Require(hit.ok() && hit.cache_hit, "resource residency should hit");
  auto stale = cache.Lookup(ResourceSeedFamily::charset, "utf8", "v1", ResourceEpoch(2));
  Require(!stale.ok() && stale.stale, "resource residency stale epoch should refuse");
  auto invalidated = cache.InvalidateForEpoch(ResourceEpoch(2));
  Require(invalidated.invalidated_count == 1, "resource epoch invalidation should remove stale entry");
}

void ProvePrewarm() {
  IparPrewarmRequest request;
  request.database_id = "db";
  request.session_id = "session";
  request.budget_bytes = 12288;
  request.candidates.push_back({"descriptor", "table",
                                IparResidencyKind::table_descriptor,
                                ResidencyEpoch(1), ResidencyEpoch(1),
                                4096, 100, 10, 80, true, true, true});
  request.candidates.push_back({"index-root", "index",
                                IparResidencyKind::index_root,
                                ResidencyEpoch(1), ResidencyEpoch(1),
                                4096, 90, 9, 60, true, true, true});
  request.candidates.push_back({"security", "mask",
                                IparResidencyKind::security_mask,
                                ResidencyEpoch(1), ResidencyEpoch(2),
                                4096, 80, 8, 70, true, true, true});
  auto plan = PlanIparWorkingSetPrewarm(request);
  Require(plan.ok(), "prewarm plan should pass");
  Require(plan.selected.size() == 2, "prewarm should select exact-epoch candidates");
  Require(plan.cold_start_cost_after < plan.cold_start_cost_before,
          "prewarm should reduce modeled cold-start cost");
}

void ProveFastPathDescriptors() {
  const auto epoch = FastEpoch(5);
  Require(epoch.complete, "compressed epoch should be complete");
  const auto same = FastEpoch(5);
  const auto changed = FastEpoch(6);
  Require(IparCompressedEpochVectorMatches(epoch, same),
          "compressed epoch should match equal vector");
  Require(!IparCompressedEpochVectorMatches(epoch, changed),
          "compressed epoch should reject changed vector");

  IparCapabilityInput input;
  input.object_uuid = "table";
  input.epoch = epoch;
  input.has_unique_indexes = true;
  input.has_defaults = true;
  auto capabilities = BuildIparObjectCapabilitySet(input);
  Require(IparObjectCapabilityHas(capabilities, IparCapability::unique_indexes),
          "unique index capability should be set");
  Require(!IparObjectCapabilityHas(capabilities, IparCapability::triggers),
          "absent trigger capability should stay clear");
  auto branches = BuildIparNoopBranchTable(capabilities);
  bool triggers_skipped = false;
  bool defaults_required = false;
  for (const auto& branch : branches) {
    if (branch.branch_name == "triggers") {
      triggers_skipped = branch.skip_fast_path;
    }
    if (branch.branch_name == "defaults") {
      defaults_required = branch.required && !branch.skip_fast_path;
    }
  }
  Require(triggers_skipped, "no-op branch table should skip absent triggers");
  Require(defaults_required, "no-op branch table should retain defaults");

  std::vector<IparRowLayoutColumn> columns;
  columns.push_back({"col-id", Descriptor("desc-i64", "scalar", "int64", "i64"),
                     0, 8, 0, false, false, false, false, false});
  columns.push_back({"col-name", Descriptor("desc-text", "scalar", "text", "text"),
                     1, 0, 128, true, true, true, false, true});
  columns.push_back({"col-gen", Descriptor("desc-i64-gen", "scalar", "int64", "i64"),
                     2, 8, 0, false, false, false, true, false});
  auto layout = BuildIparRowLayoutDescriptor("table", "stmt", epoch, columns);
  Require(layout.ok, "row layout should build");
  Require(layout.layout.null_bitmap_bytes == 1, "null bitmap should cover columns");
  Require(layout.layout.variable_column_count == 1,
          "layout should track variable column");
  Require(!layout.layout.encoder_digest.empty(),
          "layout should produce encoder digest");
  IparParameterEncoderCache cache;
  Require(cache.Put(layout.layout).ok, "encoder cache put should pass");
  auto hit = cache.Lookup("table", "stmt", epoch, layout.layout.encoder_digest);
  Require(hit.ok && hit.cache_hit, "encoder cache should hit");
  Require(cache.InvalidateStale(changed) == 1,
          "encoder cache should invalidate changed epoch");
}

void ProveStatementPools() {
  IparStatementPoolRequest request;
  request.context.query_id = "query";
  request.context.statement_id = "statement";
  request.context.session_id = "session";
  request.context.transaction_id = "transaction";
  request.context.database_id = "database";
  request.context.engine_id = "engine";
  request.context.operation_id = "insert";
  request.statement_limit_bytes = 65536;
  request.batch_limit_bytes = 32768;
  request.row_version_bytes = 96;
  request.key_buffer_bytes = 48;
  request.diagnostic_bytes = 128;
  request.coercion_temp_bytes = 64;
  request.scratch_bytes = 128;
  request.max_batch_rows = 32;
  auto plan = PlanIparStatementMemoryPools(request);
  Require(plan.ok(), "statement pool plan should pass");
  Require(!plan.slab_size_classes.empty(), "statement pool should produce slab classes");

  auto policy = DefaultLocalEngineMemoryPolicy();
  policy.byte_limit = 131072;
  policy.hard_limit_bytes = 131072;
  policy.soft_limit_bytes = 131072;
  policy.per_context_limit_bytes = 131072;
  MemoryManager manager(policy);
  QueryMemoryArena arena(request.context, plan.arena_limits, manager.allocator());
  auto grant = arena.Grant(plan.arena_grant);
  Require(grant.ok(), "statement arena grant should allocate");
  auto reset = arena.Reset();
  Require(reset.ok(), "statement arena should reset at boundary");
}

void ProveDiagnosticTemplateCache() {
  IparDiagnosticTemplateCache cache;
  IparDiagnosticTemplate entry;
  entry.diagnostic_code = "SB_IPAR.TEST_FAILURE";
  entry.message_key = "ipar.test.failure.{object}";
  entry.severity = "error";
  entry.language_tag = "en";
  entry.localization_hook = "builtin.en";
  entry.variable_names = {"object"};
  entry.epoch = {1, 1, 1};
  Require(cache.Put(entry).ok, "diagnostic template put should pass");
  IparDiagnosticRenderRequest success;
  success.diagnostic_code = entry.diagnostic_code;
  success.language_tag = "en";
  success.epoch = entry.epoch;
  success.failure_path = false;
  auto skipped = cache.Render(success);
  Require(skipped.ok && skipped.formatting_skipped,
          "success path should skip diagnostic formatting");
  IparDiagnosticRenderRequest failure = success;
  failure.failure_path = true;
  failure.variables["object"] = "table";
  auto rendered = cache.Render(failure);
  Require(rendered.ok, "failure path should render diagnostic");
  Require(rendered.rendered_message == "ipar.test.failure.table",
          "diagnostic variable should be filled");
  Require(cache.InvalidateStale({2, 1, 1}) == 1,
          "diagnostic template should invalidate on epoch change");
}

void ProveWarmOpenProfile() {
  const auto epoch = FastEpoch(9);
  IparWarmProfileRequest request;
  request.database_uuid = "database";
  request.open_epoch = epoch;
  request.budget_bytes = 16384;
  request.items.push_back({"policy", "default_policy", "policy", epoch, 2048, 100, true, true});
  request.items.push_back({"sys-desc", "sys_descriptor", "sys", epoch, 4096, 90, true, true});
  request.items.push_back({"resource", "resource_pack", "resource", epoch, 4096, 80, true, true});
  request.items.push_back({"page-map", "page_map", "page", FastEpoch(10), 4096, 70, true, true});
  auto plan = PlanIparDatabaseOpenWarmProfile(request);
  Require(plan.ok, "warm open profile should pass");
  Require(plan.selected.size() == 3, "warm open should select authorized current-epoch items");
  Require(plan.selected_bytes <= request.budget_bytes, "warm open should obey budget");
}

}  // namespace

int main() {
  ProveHotResidency();
  ProveGovernance();
  ProveResourceResidency();
  ProvePrewarm();
  ProveFastPathDescriptors();
  ProveStatementPools();
  ProveDiagnosticTemplateCache();
  ProveWarmOpenProfile();
  std::cout << "ipar_p6_support_services_gate=passed\n";
  return 0;
}
