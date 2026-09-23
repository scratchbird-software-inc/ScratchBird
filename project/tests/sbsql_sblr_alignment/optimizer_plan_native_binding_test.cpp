// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/engine/internal_api/query/optimizer_plan_lifecycle.cpp"
#include "../support/binary_uuid_fixture.hpp"
#include <cstdlib>
#include <iostream>
#include <source_location>
#include <unistd.h>
namespace api = scratchbird::engine::internal_api;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  if (!ok) { std::cerr << "failure at " << at.line() << '\n'; std::abort(); }
}
auto Id(unsigned n) { return scratchbird::tests::FixtureUuid(1167,n); }
int main() {
  api::EngineRequestContext context;
  context.database_uuid = Id(1);
  context.database_path = (std::filesystem::temp_directory_path()/
      ("sb_optimizer_native_"+std::to_string(getpid()))).string();
  auto event = Id(2); event.bytes[10] = 0; event.bytes[15] = 255;
  api::PlanFields fields{{"record_schema", "optimizer_plan_metadata_v3"},
      {"metadata_only", "1"}, {"invalidated", "0"}, {"event_uuid", event},
      {"plan_uuid", Id(3)}, {"relation_uuid", Id(4)}, {"index_uuid", Id(5)},
      {"query_fingerprint", std::string("query\0fingerprint",17)},
      {"catalog_physical_profile_key", "btree"}, {"plan_shape_digest", "shape"},
      {"object_dependency_uuids", std::vector<api::EngineUuid>{Id(4),Id(5)}}};
  unsigned next = 6;
  for (const auto* key : {"bound_sblr_tree_uuid", "catalog_epoch_uuid", "security_context_uuid",
      "capability_snapshot_uuid", "resource_snapshot_uuid", "statistics_snapshot_uuid", "route_snapshot_uuid"})
    fields.emplace(key, Id(next++));
  for (const auto* key : {"plan_cache_epoch", "index_generation", "statistics_generation",
      "catalog_generation_id", "resource_epoch", "charset_epoch", "collation_epoch",
      "dependency_catalog_generation_id", "dependency_security_epoch", "dependency_policy_epoch",
      "dependency_resource_epoch", "dependency_statistics_generation", "dependency_route_epoch", "dependency_route_generation"})
    fields.emplace(key, "1");
  const auto bytes = api::MakeEvent(context, "CACHE_PLAN", fields);
  Check(!bytes.empty());
  Check(bytes.find(std::string(reinterpret_cast<const char*>(event.bytes.data()),16)) != std::string::npos);
  api::RawPlanEvent raw;
  Check(api::DecodeEvent(bytes, context, &raw) && api::CachePlanEventSchemaValid(raw));
  const auto entry = api::EntryFromFields(raw);
  Check(entry && entry->event_uuid == event && entry->plan_uuid == Id(3) &&
      entry->dependencies.object_dependency_uuids == std::vector<api::EngineUuid>({Id(4),Id(5)}));
  Check(entry->metadata_only && !entry->invalidated);
  api::EngineApiResult rows; api::FillEntryResult(&rows,*entry);
  const auto& value = rows.result_shape.rows[0].fields[0].second;
  Check(value.binary_value.size() == 16 && value.encoded_value.empty());
  auto wrong = context; wrong.database_uuid = Id(99);
  api::RawPlanEvent bad;
  Check(!api::DecodeEvent(bytes, wrong, &bad));
  for (std::size_t n=0; n<bytes.size(); ++n) {
    bad = {}; Check(!api::DecodeEvent(std::string_view(bytes).substr(0,n), context, &bad));
  }
  for (const auto& replacement : {api::PlanField(std::string("uuid-text")), api::PlanField(api::EngineUuid{})}) {
    auto changed = fields; changed["event_uuid"] = replacement; bad = {};
    const auto encoded = api::MakeEvent(context,"CACHE_PLAN",changed);
    Check(!api::DecodeEvent(encoded,context,&bad) || !api::CachePlanEventSchemaValid(bad));
  }
  auto changed = fields; changed["plan_cache_epoch"] = "01"; bad = {};
  Check(api::DecodeEvent(api::MakeEvent(context,"CACHE_PLAN",changed),context,&bad));
  Check(!api::CachePlanEventSchemaValid(bad));
  changed = fields; changed["unknown"] = "field"; bad = {};
  Check(api::DecodeEvent(api::MakeEvent(context,"CACHE_PLAN",changed),context,&bad));
  Check(!api::CachePlanEventSchemaValid(bad));
  const auto path = api::EventPath(context);
  Check(!std::filesystem::exists(path));
  Check(!api::AppendEvent(context,"CACHE_PLAN",fields).error);
  auto state = api::LoadOptimizerPlanLifecycleState(context);
  Check(state.ok && state.state.entries.size() == 1 && state.state.entries[0].plan_uuid == Id(3));
  Check(!api::LoadOptimizerPlanLifecycleState(wrong).ok);
  api::PlanFields invalidation{{"record_schema","optimizer_plan_invalidation_v3"},
      {"event_uuid",Id(20)},{"metadata_only","1"},{"plan_cache_epoch","2"},
      {"index_uuid",Id(5)},{"reason","changed"},{"invalidate_all","0"}};
  for (const auto* key : {"new_index_generation","new_statistics_generation","new_catalog_generation_id",
      "new_resource_epoch","new_charset_epoch","new_collation_epoch"}) invalidation.emplace(key,"2");
  Check(!api::AppendEvent(context,"INVALIDATE",invalidation).error);
  state = api::LoadOptimizerPlanLifecycleState(context);
  Check(state.ok && state.state.entries[0].invalidated && state.state.invalidation_events == 1);
  api::PlanFields recovery{{"record_schema","optimizer_plan_recovery_v3"},{"event_uuid",Id(21)},
      {"metadata_only","1"},{"plan_cache_epoch","3"},{"recovery_snapshot_uuid",Id(22)}};
  Check(!api::AppendEvent(context,"RECOVERY_SNAPSHOT",recovery).error);
  state = api::LoadOptimizerPlanLifecycleState(context);
  Check(state.ok && state.state.recovery_snapshot_uuid == Id(22) && state.state.max_event_sequence == 3);
  std::filesystem::resize_file(path,std::filesystem::file_size(path)-1);
  state = api::LoadOptimizerPlanLifecycleState(context);
  Check(!state.ok && state.state.entries.empty());
  Check(api::AppendEvent(context,"CACHE_PLAN",fields).error);
  { std::ofstream out(path,std::ios::binary|std::ios::trunc); out << "SBPLANL2\t2\tCACHE_PLAN\told-text"; }
  state = api::LoadOptimizerPlanLifecycleState(context);
  Check(!state.ok && state.state.legacy_event_count == 1);
  Check(api::AppendEvent(context,"CACHE_PLAN",fields).error);
  std::filesystem::remove(path);
}
