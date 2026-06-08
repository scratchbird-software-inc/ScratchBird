// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cpu_cache_layout_observability.hpp"

#include "datatype_binary.hpp"
#include "executor_batching.hpp"
#include "index_hash_page.hpp"
#include "index_key_encoding.hpp"
#include "row_data_page.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace dt = scratchbird::core::datatypes;
namespace exec = scratchbird::engine::executor;
namespace idx = scratchbird::core::index;
namespace page = scratchbird::storage::page;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << "ORH-289 gate failure: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(std::string(message));
  }
}

platform::TypedUuid StableUuid(platform::UuidKind kind, platform::byte seed) {
  platform::TypedUuid out;
  out.kind = kind;
  for (std::size_t i = 0; i < out.value.bytes.size(); ++i) {
    out.value.bytes[i] =
        static_cast<platform::byte>(seed + static_cast<platform::byte>(i));
  }
  out.value.bytes[6] =
      static_cast<platform::byte>((out.value.bytes[6] & 0x0fu) | 0x70u);
  out.value.bytes[8] =
      static_cast<platform::byte>((out.value.bytes[8] & 0x3fu) | 0x80u);
  return out;
}

dt::DatatypeBinaryValue TextValue(std::string text) {
  dt::DatatypeBinaryValue value;
  value.type_id = dt::CanonicalTypeId::character;
  value.payload.assign(text.begin(), text.end());
  return value;
}

page::RowDataPageBody BuildRowPage() {
  page::RowDataPageBody body;
  body.relation_uuid = StableUuid(platform::UuidKind::object, 0x10);
  body.segment_id = 1;
  body.segment_generation = 289;
  body.page_number = 7;
  body.page_generation = 289;
  for (platform::u32 i = 0; i < 5; ++i) {
    page::RowDataRecord row;
    row.row_uuid =
        StableUuid(platform::UuidKind::row,
                   static_cast<platform::byte>(0x30 + i));
    row.transaction_uuid =
        StableUuid(platform::UuidKind::transaction,
                   static_cast<platform::byte>(0x50 + i));
    row.local_transaction_id = 1000 + i;
    row.row_version = 1;
    row.deleted = false;
    row.cells.push_back({1, TextValue("tenant-alpha-" + std::to_string(i))});
    row.cells.push_back({2, TextValue(std::string(48 + i, 'x'))});
    body.rows.push_back(std::move(row));
  }
  page::AssignDenseInternalRowOrdinals(&body);
  const auto built = page::BuildRowDataPageBody(body, 4096);
  Require(built.ok(), "row data page build failed");
  const auto parsed = page::ParseRowDataPageBody(built.serialized,
                                                 body.page_number);
  Require(parsed.ok(), "row data page parse failed");
  return parsed.body;
}

std::vector<platform::byte> Bytes(std::string_view text) {
  return std::vector<platform::byte>(text.begin(), text.end());
}

std::vector<platform::byte> EncodedKey(
    const platform::TypedUuid& index_uuid,
    std::string_view key) {
  idx::IndexKeyEncodingComponent component;
  component.kind = idx::IndexKeyComponentKind::scalar;
  component.ordinal = 0;
  component.type_descriptor_uuid = index_uuid;
  component.payload = Bytes(key);
  const auto encoded = idx::EncodeIndexKey({component}, {});
  Require(encoded.ok(), "index key encoding failed");
  return encoded.encoded;
}

page::IndexBtreePhysicalScanBound Bound(
    const platform::TypedUuid& index_uuid,
    std::string_view key) {
  page::IndexBtreePhysicalScanBound bound;
  bound.unbounded = false;
  bound.inclusive = true;
  bound.encoded_key = EncodedKey(index_uuid, key);
  return bound;
}

page::IndexBtreePhysicalTree BuildBtree(platform::TypedUuid index_uuid) {
  auto initialized = page::InitializeIndexBtreePhysicalTree(index_uuid, 768);
  Require(initialized.ok(), "btree init failed");
  auto tree = std::move(initialized.tree);
  const std::vector<std::string> keys = {"alpha", "bravo", "charlie"};
  for (std::size_t i = 0; i < keys.size(); ++i) {
    page::IndexBtreeCell cell;
    cell.key_ordinal = 0;
    cell.encoded_key = EncodedKey(index_uuid, keys[i]);
    cell.row_uuid =
        StableUuid(platform::UuidKind::row,
                   static_cast<platform::byte>(0x80 + i));
    cell.version_uuid =
        StableUuid(platform::UuidKind::row,
                   static_cast<platform::byte>(0xa0 + i));
    page::IndexBtreePhysicalInsertRequest insert;
    insert.cell = cell;
    const auto inserted = page::InsertIndexBtreeCell(&tree, insert);
    Require(inserted.ok(), "btree insert failed");
  }
  return tree;
}

exec::IndexedPhysicalOperatorResult RangeScan(
    const page::IndexBtreePhysicalTree& tree,
    platform::TypedUuid index_uuid) {
  exec::IndexedPhysicalOperatorRequest request;
  request.kind = exec::IndexedPhysicalOperatorKind::range_scan;
  request.physical_tree = &tree;
  request.lower_bound = Bound(index_uuid, "alpha");
  request.upper_bound = Bound(index_uuid, "charlie");
  request.plan_safe = true;
  request.physical_tree_available = true;
  request.encoded_bounds_proof = true;
  request.durable_mga_inventory_proof = true;
  request.mga_visibility_recheck_planned = true;
  request.security_recheck_planned = true;
  const auto result = exec::ExecuteIndexedPhysicalOperator(request);
  Require(result.ok, "indexed physical range scan failed");
  return result;
}

struct HashFixture {
  page::IndexHashPhysicalIndex index;
  page::IndexHashPhysicalProbeResult probe;
};

HashFixture BuildHash(platform::TypedUuid index_uuid,
                      platform::TypedUuid row_uuid,
                      platform::TypedUuid version_uuid) {
  auto initialized = page::InitializeIndexHashPhysicalIndex(
      index_uuid,
      768,
      0x289289u,
      page::kIndexHashProductionDefaultAlgorithmVersion,
      2,
      true);
  Require(initialized.ok(), "hash index init failed");
  auto index = std::move(initialized.index);
  const auto key = EncodedKey(index_uuid, "bravo");
  auto located = page::LocateIndexHashBucket(index, key);
  Require(located.ok(), "hash bucket locate failed");
  {
    auto latch = page::AcquireIndexHashBucketExclusiveLatch(
        &index, located.bucket_page_number);
    page::IndexHashPhysicalInsertRequest insert;
    insert.encoded_key = key;
    insert.row_uuid = row_uuid;
    insert.version_uuid = version_uuid;
    insert.latch_evidence = latch.evidence();
    const auto inserted = page::InsertIndexHashEntry(&index, insert);
    Require(inserted.ok(), "hash insert failed");
  }
  auto shared = page::AcquireIndexHashBucketSharedLatch(
      index, located.bucket_page_number);
  page::IndexHashPhysicalProbeRequest probe_request;
  probe_request.encoded_key = key;
  probe_request.latch_evidence = shared.evidence();
  auto probe = page::ProbeIndexHashBucket(index, probe_request);
  Require(probe.ok(), "hash probe failed");
  return {std::move(index), std::move(probe)};
}

exec::Batch BuildBatch() {
  return exec::MakeBatch("orh289.batch.layout",
                         {{{1, 10, 100}},
                          {{2, 20, 200}},
                          {{3, 30, 300}},
                          {{4, 40, 400}}});
}

exec::ExecutorBatchResult ExecuteBatch(const exec::Batch& batch) {
  exec::ExecutorBatchRequest request;
  request.requested_mode = exec::ExecutorBatchRequestMode::kPreferBatch;
  request.node_supports_batch = true;
  request.preserve_input_order = true;
  request.limits.max_batch_rows = 16;
  return exec::ExecuteScopedExecutorBatch(
      batch,
      request,
      [](const exec::Tuple& row, std::size_t) {
        exec::ExecutorRowStepResult result;
        result.ok = true;
        result.emit_row = true;
        result.row = row;
        return result;
      });
}

struct Fixture {
  page::RowDataPageBody row_page = BuildRowPage();
  platform::TypedUuid btree_uuid = StableUuid(platform::UuidKind::object, 0x20);
  page::IndexBtreePhysicalTree btree = BuildBtree(btree_uuid);
  exec::IndexedPhysicalOperatorResult physical = RangeScan(btree, btree_uuid);
  platform::TypedUuid hash_uuid = StableUuid(platform::UuidKind::object, 0x21);
  HashFixture hash =
      BuildHash(hash_uuid,
                StableUuid(platform::UuidKind::row, 0x82),
                StableUuid(platform::UuidKind::row, 0xa2));
  exec::Batch batch = BuildBatch();
  exec::ExecutorBatchResult batch_result = ExecuteBatch(batch);
};

bool Has(const std::vector<std::string>& evidence, std::string_view needle) {
  return std::any_of(evidence.begin(), evidence.end(), [&](const auto& item) {
    return item.find(needle) != std::string::npos;
  });
}

exec::CpuCacheLayoutObservationRequest Request(const Fixture& fixture) {
  exec::CpuCacheLayoutObservationRequest request;
  request.route_label = "orh289.sql_select.btree.layout_observability";
  request.route = idx::IndexRouteKind::sql_select;
  request.expected_family = idx::IndexFamily::btree;
  request.observed_family = idx::IndexFamily::btree;
  request.row_page = &fixture.row_page;
  request.btree_tree = &fixture.btree;
  request.hash_index = &fixture.hash.index;
  request.hash_probe = &fixture.hash.probe;
  request.physical_result = &fixture.physical;
  request.executor_batch = &fixture.batch;
  request.executor_batch_result = &fixture.batch_result;
  request.authority.engine_mga_visibility_authority = true;
  request.authority.security_recheck_authority = true;
  request.layout_generation = 289;
  request.expected_layout_generation = 289;
  request.route_capability_generation = 289;
  request.expected_route_capability_generation = 289;
  request.runtime_consumed = true;
  request.exact_fallback_available = true;
  return request;
}

void RequireAccepted(const exec::CpuCacheLayoutObservationResult& result) {
  if (!result.ok || !result.benchmark_clean) {
    Fail("layout observation did not pass: " + result.diagnostic_code);
  }
  Require(result.storage_layout_observed, "storage layout evidence missing");
  Require(result.btree_layout_observed, "btree identity evidence missing");
  Require(result.hash_layout_observed, "hash identity evidence missing");
  Require(result.executor_layout_observed, "executor layout evidence missing");
  Require(result.row_width_bytes > 0, "row width metric missing");
  Require(result.tuple_decode_cost_units > 0, "tuple decode metric missing");
  Require(result.visibility_branch_proxy > 0,
          "visibility branch proxy missing");
  Require(result.cache_miss_proxy > 0, "cache-miss proxy missing");
  Require(result.batch_width == 4, "batch width evidence mismatch");
  Require(!result.metrics_authority, "layout metrics became authority");
  Require(result.mga_security_evidence, "MGA/security evidence missing");
  Require(Has(result.evidence, "orh289.route_label="),
          "route label evidence missing");
  Require(Has(result.evidence, "orh289.runtime_consumed=true"),
          "runtime-consumed evidence missing");
  Require(Has(result.evidence, "orh289.physical_index_family=btree"),
          "physical family evidence missing");
  Require(Has(result.evidence, "orh289.physical_btree_identity="),
          "btree identity missing");
  Require(Has(result.evidence, "orh289.physical_hash_identity="),
          "hash identity missing");
  Require(Has(result.evidence,
              "orh289.mga_visibility_authority=engine_transaction_inventory"),
          "MGA authority evidence missing");
  Require(Has(result.evidence,
              "orh289.layout_metrics_finality_authority=false"),
          "finality non-authority evidence missing");
}

void ExpectRejected(exec::CpuCacheLayoutObservationRequest request,
                    std::string_view expected_code) {
  const auto result = exec::ObserveCpuCacheLayoutHotPath(request);
  Require(!result.ok, "negative layout case was accepted");
  Require(result.fail_closed, "negative layout case did not fail closed");
  Require(!result.benchmark_clean,
          "negative layout case claimed benchmark-clean");
  if (result.diagnostic_code != expected_code) {
    Fail("diagnostic mismatch expected " + std::string(expected_code) +
         " got " + result.diagnostic_code);
  }
  Require(Has(result.evidence, "orh289.fail_closed=true"),
          "fail closed evidence missing");
  Require(Has(result.evidence, "orh289.diagnostic="),
          "diagnostic evidence missing");
}

void NegativeCases(const Fixture& fixture) {
  auto request = Request(fixture);
  request.authority.parser_client_or_donor_layout_authority = true;
  ExpectRejected(request, "ORH_CPU_CACHE_LAYOUT.UNSAFE_LAYOUT_AUTHORITY");

  request = Request(fixture);
  request.authority.layout_metrics_finality_authority = true;
  ExpectRejected(request, "ORH_CPU_CACHE_LAYOUT.METRICS_AUTHORITY_UNSAFE");

  request = Request(fixture);
  request.runtime_consumed = false;
  ExpectRejected(request, "ORH_CPU_CACHE_LAYOUT.NO_RUNTIME_CONSUMPTION");

  request = Request(fixture);
  request.contract_only_evidence = true;
  ExpectRejected(request, "ORH_CPU_CACHE_LAYOUT.NO_RUNTIME_CONSUMPTION");

  request = Request(fixture);
  request.layout_generation = 288;
  ExpectRejected(request, "ORH_CPU_CACHE_LAYOUT.STALE_LAYOUT_GENERATION");

  request = Request(fixture);
  request.observed_family = idx::IndexFamily::hash;
  ExpectRejected(request,
                 "ORH_CPU_CACHE_LAYOUT.ROUTE_FAMILY_IDENTITY_MISMATCH");

  request = Request(fixture);
  request.authority.engine_mga_visibility_authority = false;
  ExpectRejected(request, "ORH_CPU_CACHE_LAYOUT.MISSING_MGA_SECURITY_PROOF");

  request = Request(fixture);
  request.exact_fallback_available = false;
  ExpectRejected(request, "ORH_CPU_CACHE_LAYOUT.NO_EXACT_FALLBACK");

  request = Request(fixture);
  request.benchmark_clean_claim = true;
  ExpectRejected(request, "ORH_CPU_CACHE_LAYOUT.BENCHMARK_OVERCLAIM");

  request = Request(fixture);
  request.donor_dominance_claim = true;
  ExpectRejected(request, "ORH_CPU_CACHE_LAYOUT.BENCHMARK_OVERCLAIM");

  request = Request(fixture);
  request.expected_family = idx::IndexFamily::donor_emulated;
  request.observed_family = idx::IndexFamily::donor_emulated;
  ExpectRejected(request,
                 "ORH_CPU_CACHE_LAYOUT.UNSUPPORTED_PHYSICAL_FAMILY_IDENTITY");

  request = Request(fixture);
  request.route_capability_generation = 288;
  ExpectRejected(request, "ORH_CPU_CACHE_LAYOUT.STALE_ROUTE_CAPABILITY");
}

}  // namespace

int main() {
  const Fixture fixture;
  const auto result = exec::ObserveCpuCacheLayoutHotPath(Request(fixture));
  RequireAccepted(result);
  NegativeCases(fixture);
  std::cout << "ORH-289 CPU/cache/layout observability gate passed\n";
  return EXIT_SUCCESS;
}
