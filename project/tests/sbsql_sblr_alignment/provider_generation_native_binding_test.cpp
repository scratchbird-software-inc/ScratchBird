// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/engine/internal_api/nosql/nosql_provider_generation_store.cpp"
#include "../support/binary_uuid_fixture.hpp"
#include <cstdlib>
#include <iostream>
#include <source_location>
#include <unistd.h>
namespace a = scratchbird::engine::internal_api;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  if (!ok) { std::cerr << "failure at " << at.line() << '\n'; std::abort(); }
}
int main() {
  using scratchbird::tests::FixtureUuid;
  auto identity = FixtureUuid(1134, 1);
  identity.bytes[9] = 0; identity.bytes[15] = 0xff;
  a::EngineRequestContext context;
  context.database_uuid = identity;
  char path[] = "/tmp/sb_provider_native_XXXXXX";
  const auto directory = ::mkdtemp(path); Check(directory != nullptr);
  context.database_path = std::string(directory) + "/database";
  a::EngineNoSqlProviderGenerationMetadata metadata;
  metadata.family = a::EngineNoSqlProviderFamily::kTimeSeries;
  metadata.provider_id = "rollup";
  metadata.provider_uuid = FixtureUuid(1134, 2);
  metadata.database_identity = context.database_path;
  metadata.database_uuid = identity;
  metadata.collection_uuid = FixtureUuid(1134, 3);
  metadata.generation_uuid = FixtureUuid(1134, 4);
  metadata.generation_id = 1;
  metadata.descriptor_epoch = metadata.security_epoch = metadata.redaction_epoch = metadata.catalog_epoch = 1;
  metadata.time_series_rollup_candidate_present = true;
  metadata.time_series_rollup_generation = 1;
  metadata.time_series_visible_late_arrival_generation = 1;
  metadata.time_series_rollup_interval_ns = 60000000000;
  metadata.time_series_rollup_exactness_attestation_state = "TIME_SERIES_ROLLUP_SECTION_8_EXACT_V1";
  metadata.time_series_rollup_statement_snapshot_uuid = FixtureUuid(1134, 5);
  metadata.time_series_rollup_statement_metadata_snapshot_uuid = FixtureUuid(1134, 6);
  metadata.time_series_rollup_owning_transaction_uuid = FixtureUuid(1134, 7);
  metadata.time_series_rollup_local_transaction_id = 1;
  metadata.time_series_rollup_snapshot_visible_through_local_transaction_id = 1;
  metadata.time_series_rollup_security_context_uuid = FixtureUuid(1134, 8);
  metadata.time_series_rollup_catalog_epoch_uuid = FixtureUuid(1134, 9);
  metadata.time_series_rollup_exact_residual_recheck_required = true;
  metadata.time_series_rollup_base_row_mga_recheck_required = true;
  metadata.time_series_rollup_security_recheck_required = true;
  Check(a::SealTimeSeriesRollupCapabilityV2(&metadata));
  Check(a::IsNativeIdentity(metadata.time_series_rollup_capability_uuid));
  Check(metadata.time_series_rollup_binding_digest.size() == 32);
  Check(a::ValidateTimeSeriesRollupCapabilityBindingV1(metadata));
  const auto original = metadata;
  metadata.collection_uuid.bytes[15] ^= 1;
  Check(!a::ValidateTimeSeriesRollupCapabilityBindingV1(metadata)); metadata = original;
  metadata.time_series_rollup_capability_uuid.bytes[15] ^= 1;
  Check(!a::ValidateTimeSeriesRollupCapabilityBindingV1(metadata)); metadata = original;
  metadata.time_series_rollup_binding_digest[31] ^= 1;
  Check(!a::ValidateTimeSeriesRollupCapabilityBindingV1(metadata)); metadata = original;
  Check(a::RewriteLocked(context, {metadata}));
  auto loaded = a::LoadLocked(context);
  Check(loaded.size() == 1 && loaded[0].persistence_valid);
  Check(loaded[0].database_uuid == identity);
  Check(loaded[0].time_series_rollup_capability_uuid == metadata.time_series_rollup_capability_uuid);
  Check(a::ValidateTimeSeriesRollupCapabilityBindingV1(loaded[0]));
  a::GenerationCache().clear();
  // A second database cannot borrow this database's native identity or cached records.
  auto other = context; other.database_uuid.bytes[15] ^= 1;
  loaded = a::LoadLocked(other);
  Check(loaded.size() == 1 && !loaded[0].persistence_valid);
  // Duplicates remain visible to admission rather than being normalized away.
  Check(a::RewriteLocked(context, {metadata, metadata}));
  loaded = a::LoadLocked(context); Check(loaded.size() == 2);
  a::GenerationCache().clear();
  auto filename = a::GenerationPath(context);
  auto size = std::filesystem::file_size(filename);
  std::filesystem::resize_file(filename, size - 1);
  loaded = a::LoadLocked(context);
  Check(loaded.size() == 1 && !loaded[0].persistence_valid);
  { std::ofstream out(filename, std::ios::binary | std::ios::trunc);
    out << "SBNOSQLPG1\tGENERATION\tlegacy\n"; }
  loaded = a::LoadLocked(context);
  Check(loaded.size() == 1 && !loaded[0].persistence_valid);
  std::filesystem::remove(filename);
  std::filesystem::remove(directory);
}
