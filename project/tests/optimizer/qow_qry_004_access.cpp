// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "selected_index_storage_access.hpp"
#include "indexed_physical_operator.hpp"

#include "index_key_encoding.hpp"
#include "uuid.hpp"

#include <cstdint>
#include <functional>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace exec = scratchbird::engine::executor;
namespace api = scratchbird::engine::internal_api;

namespace {

constexpr std::uint64_t kOwner = 0xffff'ffff'ffff'ff00ULL;
constexpr std::uint64_t kOldestActive = 0xffff'ffff'ffff'fee8ULL;
constexpr std::uint64_t kHorizon = 0xffff'ffff'ffff'fed0ULL;
constexpr std::uint64_t kInDoubt = 0xffff'ffff'ffff'fef0ULL;
constexpr std::uint64_t kCommittedHighWater =
    0xffff'ffff'ffff'fec8ULL;
constexpr std::uint64_t kInventoryNext = 0xffff'ffff'ffff'fff0ULL;

std::size_t checks = 0;
bool Require(const bool condition, const std::string_view detail) {
  ++checks;
  if (!condition) {
    std::cerr << "QOW-TEST-QRY-004-ACCESS-V1: " << detail << '\n';
  }
  return condition;
}

api::EngineUuid Uuid(const std::uint64_t suffix) {
  api::EngineUuid value;
  value.bytes[0] = 1;
  value.bytes[6] = 0x74;
  value.bytes[8] = 0x80;
  for (unsigned i = 0; i != 6; ++i) value.bytes[15-i] = suffix >> (i*8);
  return value;
}

exec::CanonicalExecutionMgaAuthority ClosureAuthority(
    const exec::TypedPhysicalNodeDag& dag) {
  exec::CanonicalExecutionMgaAuthority authority;
  authority.statement_context = dag.mga_statement_context;
  authority.origin = exec::CanonicalMgaAuthorityOrigin::kClosureTestSeam;
  const auto current = authority.statement_context;
  authority.resolve_current = [current] {
    exec::CanonicalMgaCurrentResolution resolution;
    resolution.statement_context = current;
    return resolution;
  };
  return authority;
}

void BindContext(exec::CanonicalScanAccessRequest* request,
                 const exec::PhysicalMgaStatementContext& context) {
  request->physical_dag.statement_snapshot_id =
      context.visible_committed_high_watermark;
  request->physical_dag.mga_statement_context = context;
  for (auto& node : request->physical_dag.nodes) {
    node.mga_statement_context = context;
  }
  request->mga_authority = ClosureAuthority(request->physical_dag);
}

exec::CanonicalScanAccessRequest Request(const bool index_scan = true) {
  exec::CanonicalScanAccessRequest request;
  request.physical_dag.abi_version = 2;
  request.physical_dag.selected_plan_uuid = Uuid(401);
  request.physical_dag.root_physical_node_id = 41;
  request.physical_dag.local_transaction_id = kOwner;
  request.physical_dag.statement_snapshot_id = 0;
  request.physical_dag.mga_statement_context = {
      Uuid(440), Uuid(441), Uuid(442), Uuid(443), kOwner, 0,
      kOldestActive, kHorizon, kHorizon, kHorizon,
      {kOldestActive, kOwner}, {kInDoubt}, "statement_stable",
      kInventoryNext, true, true, true};
  request.physical_dag.bound_sblr_tree_uuid = Uuid(451);
  request.physical_dag.catalog_epoch_uuid = Uuid(452);
  request.physical_dag.security_context_uuid = Uuid(453);
  request.physical_dag.capability_snapshot_uuid = Uuid(455);
  request.physical_dag.resource_snapshot_uuid = Uuid(456);
  request.physical_dag.statistics_snapshot_uuid = Uuid(457);
  request.physical_dag.route_snapshot_uuid = Uuid(458);
  request.physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       request.physical_dag.bound_sblr_tree_uuid},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       request.physical_dag.catalog_epoch_uuid},
      {exec::PhysicalAdmissionStage::kSecurity,
       request.physical_dag.security_context_uuid},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       request.physical_dag.mga_statement_context.statement_snapshot_uuid},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       request.physical_dag.capability_snapshot_uuid},
      {exec::PhysicalAdmissionStage::kResource,
       request.physical_dag.resource_snapshot_uuid},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       request.physical_dag.statistics_snapshot_uuid},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       request.physical_dag.route_snapshot_uuid},
  };
  request.physical_dag.nodes = {
      {.physical_node_id = 41,
       .relational_node_id = 41,
       .node_kind = exec::PhysicalNodeKind::kScan,
       .implementation_id = index_scan ? "scan.index.v1" : "scan.heap.v1",
       .output_descriptor_ids = {411},
       .causal_counter_id = 4101,
       .selected_alternative_uuid = Uuid(461),
       .executor_capability_uuid = Uuid(462),
       .executor_capability_abi_version = 1,
       .cost_vector_uuid = Uuid(463),
       .memory_bytes_required = 1024,
       .engine_capability_validated = true,
       .mga_statement_context = request.physical_dag.mga_statement_context},
  };
  request.physical_dag.catalog_generation = 1;
  request.physical_dag.security_epoch = 1;
  request.physical_dag.policy_epoch = 1;
  request.physical_dag.resource_epoch = 1;
  request.physical_dag.statistics_generation = 1;
  request.physical_dag.route_epoch = 1;
  request.physical_dag.route_generation = 1;
  request.physical_dag.memory_budget_bytes = 1 << 20;
  request.physical_dag.optimizer_published = true;
  request.physical_dag.immutable_node_identity_validated = true;
  request.physical_dag.capability_validated_before_access = true;
  request.selected_physical_node_id = 41;
  request.available_implementation_id =
      request.physical_dag.nodes.front().implementation_id;
  request.relation_uuid = Uuid(430);
  request.mga_authority = ClosureAuthority(request.physical_dag);
  request.selected_descriptor_generation = 7;
  request.current_descriptor_generation = 7;
  return request;
}

exec::CanonicalScanCandidateEvidence Candidate(
    const std::uint64_t ordinal,
    const exec::CanonicalMgaVisibilityDecision visibility,
    const exec::CanonicalMgaSecurityDecision security,
    const api::EngineSqlTruthValue residual,
    const bool locator_matches = true,
    const std::uint64_t creator_local_transaction_id = kOwner) {
  exec::CanonicalScanCandidateEvidence candidate;
  candidate.candidate_uuid = Uuid(500 + ordinal);
  candidate.record_uuid = Uuid(600 + ordinal);
  candidate.relation_uuid = Uuid(430);
  candidate.visibility_decision_uuid = Uuid(700 + ordinal);
  candidate.creator_local_transaction_id = creator_local_transaction_id;
  candidate.row_version_id = 800 + ordinal;
  candidate.candidate_generation = 9;
  candidate.observed_generation = locator_matches ? 9 : 10;
  candidate.source = exec::CanonicalScanCandidateSource::kIndexEntry;
  candidate.visibility = visibility;
  candidate.security_decision = security;
  candidate.residual_truth = residual;
  candidate.locator_identity_matches = locator_matches;
  return candidate;
}

bool EmptyFailureResult(
    const exec::CanonicalScanAccessResult& result) {
  return !result.diagnostic.ok && result.accepted_record_uuids.empty() &&
         result.accepted_row_version_ids.empty() &&
         result.counters.emitted_count == 0 &&
         result.selected_plan_uuid.is_nil() &&
         result.executed_physical_node_id == 0 &&
         !exec::PhysicalMgaStatementContextValid(
             result.mga_statement_context);
}

// QOW-TEST-QRY-004-ACCESS-V1
bool ValidateIndexAccessRechecks() {
  auto request = Request();
  request.candidates = {
      Candidate(1, exec::CanonicalMgaVisibilityDecision::kVisible,
                exec::CanonicalMgaSecurityDecision::kAllowed,
                api::EngineSqlTruthValue::true_value),
      Candidate(2, exec::CanonicalMgaVisibilityDecision::kInvisible,
                exec::CanonicalMgaSecurityDecision::kAllowed,
                api::EngineSqlTruthValue::true_value),
      Candidate(3, exec::CanonicalMgaVisibilityDecision::kVisible,
                exec::CanonicalMgaSecurityDecision::kAllowed,
                api::EngineSqlTruthValue::true_value, false),
      Candidate(4, exec::CanonicalMgaVisibilityDecision::kVisible,
                exec::CanonicalMgaSecurityDecision::kDenied,
                api::EngineSqlTruthValue::true_value),
      Candidate(5, exec::CanonicalMgaVisibilityDecision::kVisible,
                exec::CanonicalMgaSecurityDecision::kAllowed,
                api::EngineSqlTruthValue::false_value),
      Candidate(6, exec::CanonicalMgaVisibilityDecision::kVisible,
                exec::CanonicalMgaSecurityDecision::kAllowed,
                api::EngineSqlTruthValue::unknown),
  };

  const auto result = exec::ExecuteCanonicalSelectedScanAccess(request);
  bool passed = true;
  passed &= Require(result.diagnostic.ok && !result.replan_required,
                    "valid selected index access was refused");
  passed &= Require(result.accepted_record_uuids ==
                            std::vector<api::EngineUuid>{Uuid(601)} &&
                        result.accepted_row_version_ids ==
                            std::vector<std::uint64_t>{801},
                    "index access published a candidate before all rechecks");
  passed &= Require(result.counters.candidate_count == 6 &&
                        result.counters.visibility_recheck_count == 6 &&
                        result.counters.invisible_filtered_count == 1 &&
                        result.counters.stale_index_filtered_count == 1 &&
                        result.counters.security_filtered_count == 1 &&
                        result.counters.residual_filtered_count == 2 &&
                        result.counters.emitted_count == 1,
                    "scan causal counters do not match recheck outcomes");
  passed &= Require(result.selected_plan_uuid == Uuid(401) &&
                        result.executed_physical_node_id == 41 &&
                        result.causal_counter_id == 4101 &&
                        result.mga_statement_context
                                .owning_local_transaction_id == kOwner &&
                        result.mga_statement_context
                                .visible_committed_high_watermark == 0,
                    "selected plan/node/causal identity was not retained");
  passed &= Require(result.authority.engine_mga_snapshot_bound &&
                        result.authority.visibility_rechecks_complete &&
                        !result.authority.owns_transaction_finality &&
                        !result.authority.owns_recovery &&
                        !result.authority.owns_parser_execution &&
                        !result.authority.index_or_cache_is_visibility_authority &&
                        !result.authority.wal_is_visibility_or_recovery_authority,
                    "scan access acquired forbidden authority");
  return passed;
}

bool ValidateRelationAccessOrder() {
  auto request = Request(false);
  auto first = Candidate(
      11, exec::CanonicalMgaVisibilityDecision::kVisible,
      exec::CanonicalMgaSecurityDecision::kAllowed,
      api::EngineSqlTruthValue::true_value);
  auto second = Candidate(
      12, exec::CanonicalMgaVisibilityDecision::kVisible,
      exec::CanonicalMgaSecurityDecision::kAllowed,
      api::EngineSqlTruthValue::true_value);
  first.source = exec::CanonicalScanCandidateSource::kRelationPage;
  second.source = exec::CanonicalScanCandidateSource::kRelationPage;
  request.candidates = {first, second};

  const auto result = exec::ExecuteCanonicalSelectedScanAccess(request);
  return Require(result.diagnostic.ok &&
                     result.accepted_row_version_ids ==
                         std::vector<std::uint64_t>{811, 812} &&
                     result.counters.emitted_count == 2,
                 "relation scan did not preserve candidate order");
}

bool ValidateAuthorityAndReplanRefusals() {
  bool passed = true;
  auto request = Request();
  request.candidates = {
      Candidate(21, exec::CanonicalMgaVisibilityDecision::kVisible,
                exec::CanonicalMgaSecurityDecision::kAllowed,
                api::EngineSqlTruthValue::true_value),
  };
  auto drifted_current = request.mga_authority.statement_context;
  drifted_current.statement_snapshot_uuid = Uuid(999);
  request.mga_authority.resolve_current = [drifted_current] {
    exec::CanonicalMgaCurrentResolution resolution;
    resolution.statement_context = drifted_current;
    return resolution;
  };
  auto result = exec::ExecuteCanonicalSelectedScanAccess(request);
  passed &= Require(!result.diagnostic.ok &&
                        result.diagnostic.diagnostic_code ==
                            "QOW-DIAG-MGA-RUNTIME-CURRENT-V1" &&
                        result.accepted_record_uuids.empty() &&
                        result.executed_physical_node_id == 0,
                    "mismatched MGA snapshot produced scan output");

  request = Request();
  request.current_descriptor_generation = 8;
  result = exec::ExecuteCanonicalSelectedScanAccess(request);
  passed &= Require(!result.diagnostic.ok && result.replan_required &&
                        result.diagnostic.diagnostic_code ==
                            "SB_DIAG_MGA_READ_INDEX_DESCRIPTOR_INVALID",
                    "stale selected index generation did not require replan");

  request = Request();
  request.available_implementation_id = "scan.heap.v1";
  result = exec::ExecuteCanonicalSelectedScanAccess(request);
  passed &= Require(!result.diagnostic.ok && result.replan_required &&
                        result.diagnostic.diagnostic_code ==
                            "QOW-DIAG-QRY-004-SCAN-IMPLEMENTATION-UNAVAILABLE-V1",
                    "unavailable selected implementation silently defaulted");

  request = Request();
  request.physical_dag.mga_statement_context.current = false;
  result = exec::ExecuteCanonicalSelectedScanAccess(request);
  passed &= Require(
      !result.diagnostic.ok &&
          result.diagnostic.diagnostic_code ==
              "QOW-DIAG-PHYSICAL-NODE-ABI-PUBLICATION",
      "non-current physical MGA vector reached selected scan access");
  return passed;
}

bool ValidateCapturedCreatorVisibility() {
  bool passed = true;

  auto excluded_context = Request().physical_dag.mga_statement_context;
  excluded_context.visible_committed_high_watermark = kCommittedHighWater;
  excluded_context.oldest_active_transaction_id = kOldestActive;
  excluded_context.oldest_interesting_transaction_id = kHorizon;
  excluded_context.oldest_snapshot_transaction_id = kHorizon;
  excluded_context.retention_horizon_transaction_id = kHorizon;
  excluded_context.active_excluded_local_transaction_ids =
      {kOldestActive, kOwner};
  excluded_context.in_doubt_excluded_local_transaction_ids = {kInDoubt};

  auto expect_creator_refusal = [&](const std::uint64_t ordinal,
                                    const std::uint64_t creator,
                                    const std::string_view detail) {
    auto request = Request();
    BindContext(&request, excluded_context);
    request.candidates = {
        Candidate(ordinal, exec::CanonicalMgaVisibilityDecision::kVisible,
                  exec::CanonicalMgaSecurityDecision::kAllowed,
                  api::EngineSqlTruthValue::true_value, true, creator),
    };
    const auto result = exec::ExecuteCanonicalSelectedScanAccess(request);
    return Require(
        EmptyFailureResult(result) &&
            result.diagnostic.diagnostic_code ==
                "SB_DIAG_MGA_READ_VISIBILITY_DECISION_INVALID",
        detail);
  };
  passed &= expect_creator_refusal(
      71, kOldestActive,
      "visible verdict bypassed the captured active exclusion");
  passed &= expect_creator_refusal(
      72, kInDoubt,
      "visible verdict bypassed the captured in-doubt exclusion");

  auto committed_request = Request();
  BindContext(&committed_request, excluded_context);
  committed_request.candidates = {
      Candidate(73, exec::CanonicalMgaVisibilityDecision::kVisible,
                exec::CanonicalMgaSecurityDecision::kAllowed,
                api::EngineSqlTruthValue::true_value, true,
                kCommittedHighWater - 1),
  };
  auto result =
      exec::ExecuteCanonicalSelectedScanAccess(committed_request);
  passed &= Require(
      result.diagnostic.ok &&
          result.accepted_row_version_ids ==
              std::vector<std::uint64_t>{873} &&
          exec::PhysicalMgaStatementContextEqual(
              result.mga_statement_context, excluded_context),
      "committed non-excluded creator at or below high-water was refused");

  auto zero_request = Request();
  auto zero_context = zero_request.physical_dag.mga_statement_context;
  zero_context.visible_committed_high_watermark = 0;
  BindContext(&zero_request, zero_context);
  zero_request.candidates = {
      Candidate(74, exec::CanonicalMgaVisibilityDecision::kVisible,
                exec::CanonicalMgaSecurityDecision::kAllowed,
                api::EngineSqlTruthValue::true_value, true,
                kCommittedHighWater - 1),
  };
  result = exec::ExecuteCanonicalSelectedScanAccess(zero_request);
  passed &= Require(
      EmptyFailureResult(result) &&
          result.diagnostic.diagnostic_code ==
              "SB_DIAG_MGA_READ_VISIBILITY_DECISION_INVALID",
      "zero high-water admitted a non-owner creator or leaked scan output");
  return passed;
}

bool ValidateCompleteAuthorityRefusalMatrix() {
  const auto expect_refusal = [](auto mutation,
                                 const std::string_view diagnostic,
                                 const std::string_view detail) {
    auto request = Request();
    request.candidates = {
        Candidate(91, exec::CanonicalMgaVisibilityDecision::kVisible,
                  exec::CanonicalMgaSecurityDecision::kAllowed,
                  api::EngineSqlTruthValue::true_value),
    };
    mutation(request);
    const auto result = exec::ExecuteCanonicalSelectedScanAccess(request);
    return Require(EmptyFailureResult(result) &&
                       result.diagnostic.diagnostic_code == diagnostic &&
                       !result.replan_required,
                   detail);
  };
  bool passed = true;
  passed &= expect_refusal(
      [](auto& request) {
        request.mga_authority.origin =
            exec::CanonicalMgaAuthorityOrigin::kMissing;
      },
      "QOW-DIAG-MGA-RUNTIME-AUTHORITY-V1",
      "missing MGA authority reached scan access");
  passed &= expect_refusal(
      [](auto& request) { request.mga_authority.resolve_current = {}; },
      "QOW-DIAG-MGA-RUNTIME-AUTHORITY-V1",
      "missing current resolver reached scan access");
  passed &= expect_refusal(
      [](auto& request) {
        request.mga_authority.statement_context.statement_uuid = {};
      },
      "QOW-DIAG-MGA-RUNTIME-AUTHORITY-V1",
      "malformed carried context reached scan access");
  passed &= expect_refusal(
      [](auto& request) {
        request.mga_authority.statement_context.owning_local_transaction_id =
            static_cast<std::uint32_t>(kOwner);
      },
      "QOW-DIAG-MGA-RUNTIME-AUTHORITY-V1",
      "narrowed carried transaction identity reached scan access");
  const auto expect_current_refusal = [&](auto mutation,
                                          const std::string_view detail) {
    return expect_refusal(
        [mutation](auto& request) {
          auto current = request.mga_authority.statement_context;
          mutation(current);
          request.mga_authority.resolve_current = [current] {
            exec::CanonicalMgaCurrentResolution resolution;
            resolution.statement_context = current;
            return resolution;
          };
        },
        "QOW-DIAG-MGA-RUNTIME-CURRENT-V1", detail);
  };
  passed &= expect_current_refusal(
      [](auto& current) { current.statement_snapshot_uuid = Uuid(999); },
      "swapped current snapshot reached scan access");
  passed &= expect_current_refusal(
      [](auto& current) {
        current.active_excluded_local_transaction_ids =
            {kOldestActive, kOwner, kOwner};
      },
      "duplicate current exclusion reached scan access");
  passed &= expect_current_refusal(
      [](auto& current) {
        current.publication_inventory_next_local_transaction_id =
            static_cast<std::uint32_t>(kInventoryNext);
      },
      "truncated current inventory reached scan access");
  passed &= expect_current_refusal(
      [](auto& current) { current.current = false; },
      "stale current vector reached scan access");
  passed &= expect_refusal(
      [](auto& request) {
        request.physical_dag.nodes.front().mga_statement_context.current =
            false;
      },
      "QOW-DIAG-PHYSICAL-NODE-ABI-CAPABILITY",
      "node-swapped context reached scan access");
  passed &= expect_refusal(
      [](auto& request) {
        request.physical_dag.catalog_epoch_uuid =
            request.physical_dag.mga_statement_context
                .statement_metadata_snapshot_uuid;
        request.physical_dag.admission_evidence[1].evidence_uuid =
            request.physical_dag.catalog_epoch_uuid;
      },
      "QOW-DIAG-PHYSICAL-NODE-ABI-PUBLICATION",
      "metadata/catalog conflation reached scan access");
  return passed;
}

bool ValidateAllOrNothingRefusal() {
  auto request = Request();
  request.candidates = {
      Candidate(31, exec::CanonicalMgaVisibilityDecision::kVisible,
                exec::CanonicalMgaSecurityDecision::kAllowed,
                api::EngineSqlTruthValue::true_value),
      Candidate(32, exec::CanonicalMgaVisibilityDecision::kIndeterminate,
                exec::CanonicalMgaSecurityDecision::kAllowed,
                api::EngineSqlTruthValue::true_value),
  };
  auto result = exec::ExecuteCanonicalSelectedScanAccess(request);
  bool passed = true;
  passed &= Require(!result.diagnostic.ok &&
                        result.diagnostic.diagnostic_code ==
                            "SB_DIAG_MGA_READ_VISIBILITY_DECISION_INVALID" &&
                        result.accepted_record_uuids.empty() &&
                        result.accepted_row_version_ids.empty() &&
                        result.counters.emitted_count == 0,
                    "indeterminate visibility leaked partial scan output");

  request = Request();
  request.maximum_candidate_count = 1;
  request.candidates = {
      Candidate(33, exec::CanonicalMgaVisibilityDecision::kVisible,
                exec::CanonicalMgaSecurityDecision::kAllowed,
                api::EngineSqlTruthValue::true_value),
      Candidate(34, exec::CanonicalMgaVisibilityDecision::kVisible,
                exec::CanonicalMgaSecurityDecision::kAllowed,
                api::EngineSqlTruthValue::true_value),
  };
  result = exec::ExecuteCanonicalSelectedScanAccess(request);
  passed &= Require(!result.diagnostic.ok &&
                        result.diagnostic.diagnostic_code ==
                            "SBLR.PLAN_TREE.RESOURCE_LIMIT" &&
                        result.accepted_record_uuids.empty(),
                    "scan candidate resource bound was ignored");
  return passed;
}

namespace idx = scratchbird::core::index;
namespace page = scratchbird::storage::page;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

idx::IndexKeyEncodingComponent IndexKeyComponent(
    const api::EngineUuid& descriptor_uuid,
    const std::string_view payload) {
  idx::IndexKeyEncodingComponent component;
  component.type_descriptor_uuid = {platform::UuidKind::object, descriptor_uuid};
  component.kind = idx::IndexKeyComponentKind::scalar;
  component.ordinal = 0;
  component.payload.assign(payload.begin(), payload.end());
  return component;
}

std::vector<platform::byte> EncodedIndexKey(
    const api::EngineUuid& descriptor_uuid,
    const std::string_view payload) {
  const auto encoded =
      idx::EncodeIndexKey({IndexKeyComponent(descriptor_uuid, payload)}, {});
  return encoded.ok() ? encoded.encoded : std::vector<platform::byte>{};
}

page::IndexBtreePhysicalTree PhysicalTree(const api::EngineUuid& index_uuid) {
  auto initialized = page::InitializeIndexBtreePhysicalTree(
      {platform::UuidKind::object, index_uuid}, 768);
  return initialized.ok() ? std::move(initialized.tree)
                          : page::IndexBtreePhysicalTree{};
}

void AddIndexCell(page::IndexBtreePhysicalTree* tree,
                  const api::EngineUuid& descriptor_uuid,
                  const std::string_view key,
                  const api::EngineUuid& row_uuid,
                  const api::EngineUuid& version_uuid) {
  page::IndexBtreeCell cell;
  cell.key_ordinal = 0;
  cell.encoded_key = EncodedIndexKey(descriptor_uuid, key);
  cell.row_uuid = {platform::UuidKind::row, row_uuid};
  cell.version_uuid = {platform::UuidKind::row, version_uuid};
  page::IndexBtreePhysicalInsertRequest insert;
  insert.cell = std::move(cell);
  (void)page::InsertIndexBtreeCell(tree, insert);
}

struct IndexStorageFixture {
  api::EngineUuid index_uuid = Uuid(1201);
  api::EngineUuid key_descriptor_uuid = Uuid(1202);
  api::EngineUuid row_visible = Uuid(1211);
  api::EngineUuid row_invisible = Uuid(1212);
  api::EngineUuid row_residual = Uuid(1213);
  api::EngineUuid version_visible = Uuid(1221);
  api::EngineUuid version_invisible = Uuid(1222);
  api::EngineUuid version_residual = Uuid(1223);
  page::IndexBtreePhysicalTree tree = PhysicalTree(index_uuid);

  IndexStorageFixture() {
    AddIndexCell(&tree, key_descriptor_uuid, "bravo", row_visible,
                 version_visible);
    AddIndexCell(&tree, key_descriptor_uuid, "bravo", row_invisible,
                 version_invisible);
    AddIndexCell(&tree, key_descriptor_uuid, "bravo", row_residual,
                 version_residual);
  }
};

exec::CanonicalSelectedIndexStorageRequestV1 IndexStorageRequest(
    const IndexStorageFixture& fixture) {
  exec::CanonicalSelectedIndexStorageRequestV1 request;
  auto scan = Request();
  scan.physical_dag.nodes.front().implementation_id = "scan.index.btree.v1";
  scan.available_implementation_id = "scan.index.btree.v1";
  request.physical_dag = scan.physical_dag;
  request.selected_physical_node_id = scan.selected_physical_node_id;
  request.selected_alternative_uuid =
      scan.physical_dag.nodes.front().selected_alternative_uuid;
  request.selected_index_uuid = fixture.index_uuid;
  request.available_implementation_id = scan.available_implementation_id;
  request.relation_uuid = scan.relation_uuid;
  request.mga_authority = scan.mga_authority;
  request.selected_descriptor_generation = 7;
  request.current_descriptor_generation = 7;
  request.selected_key_descriptor_uuids = {fixture.key_descriptor_uuid};
  request.selected_key_profile_id = "sb_native_default";
  request.point_key_components = {
      IndexKeyComponent(fixture.key_descriptor_uuid, "bravo")};
  request.physical_tree = &fixture.tree;
  request.maximum_candidate_count = 16;
  request.cancellation_requested = [] { return false; };
  request.heap_fallback_alternative_uuid = Uuid(1299);
  request.physical_tree_engine_owned = true;
  request.resolver_engine_owned = true;
  request.resolve_engine_row_version =
      [&fixture](const exec::IndexedPhysicalOperatorLocator& locator) {
        exec::CanonicalIndexStorageResolvedRowV1 resolved;
        resolved.candidate.candidate_uuid =
            locator.row_uuid == fixture.row_visible
                ? Uuid(1231)
                : (locator.row_uuid == fixture.row_invisible ? Uuid(1232)
                                                              : Uuid(1233));
        resolved.candidate.record_uuid = locator.row_uuid;
        resolved.candidate.relation_uuid = Uuid(430);
        resolved.candidate.visibility_decision_uuid =
            locator.row_uuid == fixture.row_visible
                ? Uuid(1241)
                : (locator.row_uuid == fixture.row_invisible ? Uuid(1242)
                                                              : Uuid(1243));
        resolved.candidate.row_version_id =
            locator.row_uuid == fixture.row_visible
                ? 1251
                : (locator.row_uuid == fixture.row_invisible ? 1252 : 1253);
        resolved.candidate.candidate_generation = 7;
        resolved.candidate.observed_generation = 7;
        resolved.candidate.creator_local_transaction_id = kOwner;
        resolved.candidate.source =
            exec::CanonicalScanCandidateSource::kIndexEntry;
        resolved.candidate.visibility =
            locator.row_uuid == fixture.row_invisible
                ? exec::CanonicalMgaVisibilityDecision::kInvisible
                : exec::CanonicalMgaVisibilityDecision::kVisible;
        resolved.candidate.security_decision =
            exec::CanonicalMgaSecurityDecision::kAllowed;
        resolved.candidate.residual_truth =
            locator.row_uuid == fixture.row_residual
                ? api::EngineSqlTruthValue::false_value
                : api::EngineSqlTruthValue::true_value;
        resolved.candidate.locator_identity_matches = true;
        resolved.version_uuid = locator.version_uuid;
        resolved.engine_mga_visibility_rechecked = true;
        resolved.engine_security_rechecked = true;
        resolved.engine_residual_rechecked = true;
        return resolved;
      };
  return request;
}

// QOW-TEST-QRY-004-INDEX-STORAGE-V1
bool ValidatePhysicalIndexStorageAcquisition() {
  IndexStorageFixture fixture;
  auto request = IndexStorageRequest(fixture);
  auto result = exec::ExecuteCanonicalSelectedIndexStorageAccessV1(request);
  bool passed = true;
  passed &= Require(result.diagnostic.ok &&
                        result.scan_result.diagnostic.ok &&
                        result.exact_key_encoded &&
                        result.exact_selected_index_bound &&
                        result.data_access_observation_known &&
                        result.data_access_observed &&
                        !result.governed_heap_replan_required,
                    "selected physical index point lookup was refused");
  passed &= Require(result.selected_alternative_uuid ==
                            request.selected_alternative_uuid &&
                        result.selected_index_uuid == fixture.index_uuid &&
                        result.encoded_point_key ==
                            EncodedIndexKey(fixture.key_descriptor_uuid,
                                            "bravo"),
                    "selected index or exact encoded key identity drifted");
  passed &= Require(result.physical_locator_count == 3 &&
                        result.resolved_row_version_count == 3 &&
                        result.scan_result.counters.candidate_count == 3 &&
                        result.scan_result.counters
                                .visibility_recheck_count == 3 &&
                        result.scan_result.counters
                                .invisible_filtered_count == 1 &&
                        result.scan_result.counters.residual_filtered_count ==
                            1 &&
                        result.scan_result.counters.emitted_count == 1 &&
                        result.scan_result.accepted_record_uuids ==
                            std::vector<api::EngineUuid>{fixture.row_visible},
                    "physical locators bypassed MGA or residual rechecks");

  auto empty = request;
  empty.point_key_components = {
      IndexKeyComponent(fixture.key_descriptor_uuid, "missing")};
  result = exec::ExecuteCanonicalSelectedIndexStorageAccessV1(empty);
  passed &= Require(result.diagnostic.ok &&
                        result.data_access_observation_known &&
                        result.data_access_observed &&
                        result.physical_locator_count == 0 &&
                        result.scan_result.counters.emitted_count == 0,
                    "completed empty physical read was not observed exactly");

  auto approximate = request;
  approximate.selected_index_is_approximate = true;
  approximate.exact_fallback_recheck_authorized = true;
  result = exec::ExecuteCanonicalSelectedIndexStorageAccessV1(approximate);
  passed &= Require(result.diagnostic.ok &&
                        result.exact_fallback_recheck_applied &&
                        result.scan_result.counters.emitted_count == 1,
                    "authorized approximate route skipped exact recheck");
  return passed;
}

bool ValidatePhysicalIndexRefusalAndFallbackTruth() {
  IndexStorageFixture fixture;
  bool passed = true;

  auto unavailable = IndexStorageRequest(fixture);
  unavailable.physical_tree = nullptr;
  auto result =
      exec::ExecuteCanonicalSelectedIndexStorageAccessV1(unavailable);
  passed &= Require(!result.diagnostic.ok &&
                        result.governed_heap_replan_required &&
                        result.governed_heap_fallback_alternative_uuid ==
                            unavailable.heap_fallback_alternative_uuid &&
                        !result.data_access_observation_known &&
                        !result.data_access_observed &&
                        result.scan_result.accepted_record_uuids.empty(),
                    "unavailable selected index did not request governed "
                    "pre-read heap replan");

  IndexStorageFixture other;
  other.index_uuid = Uuid(1301);
  other.tree = PhysicalTree(other.index_uuid);
  auto swapped = IndexStorageRequest(fixture);
  swapped.physical_tree = &other.tree;
  result = exec::ExecuteCanonicalSelectedIndexStorageAccessV1(swapped);
  passed &= Require(!result.diagnostic.ok &&
                        result.governed_heap_replan_required &&
                        !result.data_access_observed,
                    "swapped index tree reached physical access");

  auto approximate = IndexStorageRequest(fixture);
  approximate.selected_index_is_approximate = true;
  result = exec::ExecuteCanonicalSelectedIndexStorageAccessV1(approximate);
  passed &= Require(!result.diagnostic.ok &&
                        result.governed_heap_replan_required &&
                        !result.data_access_observed,
                    "approximate index without exact fallback was consumed");

  auto cancelled = IndexStorageRequest(fixture);
  cancelled.cancellation_requested = [] { return true; };
  result = exec::ExecuteCanonicalSelectedIndexStorageAccessV1(cancelled);
  passed &= Require(!result.diagnostic.ok &&
                        result.diagnostic.diagnostic_code ==
                            "QOW-DIAG-QRY-004-INDEX-CANCELLED-V1" &&
                        !result.data_access_observed,
                    "pre-read cancellation crossed the index boundary");

  auto bounded = IndexStorageRequest(fixture);
  bounded.maximum_candidate_count = 2;
  result = exec::ExecuteCanonicalSelectedIndexStorageAccessV1(bounded);
  passed &= Require(!result.diagnostic.ok &&
                        result.diagnostic.diagnostic_code ==
                            "SBLR.PLAN_TREE.RESOURCE_LIMIT" &&
                        result.data_access_observation_known &&
                        result.data_access_observed &&
                        result.physical_locator_count == 3 &&
                        result.scan_result.accepted_record_uuids.empty(),
                    "post-read locator bound leaked rows or hid access");

  auto mid_read_cancelled = IndexStorageRequest(fixture);
  mid_read_cancelled.cancellation_requested = [calls = 0]() mutable {
    return ++calls == 3;
  };
  result = exec::ExecuteCanonicalSelectedIndexStorageAccessV1(
      mid_read_cancelled);
  passed &= Require(!result.diagnostic.ok &&
                        result.diagnostic.diagnostic_code ==
                            "QOW-DIAG-QRY-004-INDEX-CANCELLED-V1" &&
                        result.data_access_observation_known &&
                        result.data_access_observed &&
                        result.scan_result.accepted_record_uuids.empty(),
                    "mid-recheck cancellation leaked rows or hid access");

  auto swapped_key_descriptor = IndexStorageRequest(fixture);
  swapped_key_descriptor.selected_key_descriptor_uuids = {Uuid(1302)};
  result = exec::ExecuteCanonicalSelectedIndexStorageAccessV1(
      swapped_key_descriptor);
  passed &= Require(!result.diagnostic.ok &&
                        result.governed_heap_replan_required &&
                        !result.data_access_observed,
                    "swapped key descriptor reached physical index access");

  auto swapped_version = IndexStorageRequest(fixture);
  swapped_version.resolve_engine_row_version =
      [](const exec::IndexedPhysicalOperatorLocator& locator) {
        exec::CanonicalIndexStorageResolvedRowV1 resolved;
        resolved.candidate.candidate_uuid = Uuid(1311);
        resolved.candidate.record_uuid = locator.row_uuid;
        resolved.candidate.relation_uuid = Uuid(430);
        resolved.candidate.source =
            exec::CanonicalScanCandidateSource::kIndexEntry;
        resolved.candidate.locator_identity_matches = true;
        resolved.version_uuid = Uuid(1312);
        resolved.engine_mga_visibility_rechecked = true;
        resolved.engine_security_rechecked = true;
        resolved.engine_residual_rechecked = true;
        return resolved;
      };
  result =
      exec::ExecuteCanonicalSelectedIndexStorageAccessV1(swapped_version);
  passed &= Require(!result.diagnostic.ok &&
                        result.data_access_observation_known &&
                        result.data_access_observed &&
                        result.scan_result.accepted_record_uuids.empty(),
                    "post-read locator swap leaked output or hid access");

  auto unowned_resolver = IndexStorageRequest(fixture);
  unowned_resolver.resolver_engine_owned = false;
  result =
      exec::ExecuteCanonicalSelectedIndexStorageAccessV1(unowned_resolver);
  passed &= Require(!result.diagnostic.ok && !result.data_access_observed,
                    "non-engine row resolver reached index storage");
  return passed;
}

bool ValidateBinaryScanIdentity() {
  bool passed = true;
  const auto candidate = Candidate(
      1, exec::CanonicalMgaVisibilityDecision::kVisible,
      exec::CanonicalMgaSecurityDecision::kAllowed,
      api::EngineSqlTruthValue::true_value);
  const auto request_with_candidate = [&] {
    auto request = Request();
    request.maximum_candidate_count = 2;
    request.candidates = {candidate};
    return request;
  };
  // The fixture's shape oracle is independent of the production validator.
  for (const auto member : {
           &exec::CanonicalScanCandidateEvidence::candidate_uuid,
           &exec::CanonicalScanCandidateEvidence::record_uuid,
           &exec::CanonicalScanCandidateEvidence::relation_uuid,
           &exec::CanonicalScanCandidateEvidence::visibility_decision_uuid}) {
    for (unsigned version = 0; version != 16; ++version) {
      for (unsigned variant = 0; variant != 4; ++variant) {
        auto request = request_with_candidate();
        auto& id = request.candidates.front().*member;
        id.bytes[6] = (id.bytes[6] & 15) | (version << 4);
        id.bytes[8] = (id.bytes[8] & 63) | (variant << 6);
        const auto result = exec::ExecuteCanonicalSelectedScanAccess(request);
        const bool valid = version == 7 && variant == 2;
        passed &= Require(valid ? result.diagnostic.ok : EmptyFailureResult(result),
                          "candidate system UUID version/variant admission");
      }
    }
    auto nil = request_with_candidate();
    nil.candidates.front().*member = {};
    passed &= Require(EmptyFailureResult(
        exec::ExecuteCanonicalSelectedScanAccess(nil)), "nil scan identity admitted");
  }
  auto duplicate = request_with_candidate();
  duplicate.candidates.push_back(candidate);
  passed &= Require(EmptyFailureResult(exec::ExecuteCanonicalSelectedScanAccess(duplicate)),
                    "duplicate binary candidate admitted");
  for (unsigned bit = 0; bit != 128; ++bit) {
    auto distinct = duplicate;
    auto& id = distinct.candidates.back().candidate_uuid;
    id.bytes[bit / 8] ^= 1u << (bit % 8);
    const bool identity_shape = (id.bytes[6] >> 4) == 7 &&
                               (id.bytes[8] >> 6) == 2;
    const auto result = exec::ExecuteCanonicalSelectedScanAccess(distinct);
    passed &= Require(identity_shape
                          ? result.diagnostic.ok && result.counters.emitted_count == 2
                          : EmptyFailureResult(result),
                      "binary candidate equality lost a UUID bit");
    auto swapped_relation = request_with_candidate();
    swapped_relation.candidates.front().relation_uuid.bytes[bit / 8] ^=
        1u << (bit % 8);
    passed &= Require(EmptyFailureResult(
        exec::ExecuteCanonicalSelectedScanAccess(swapped_relation)),
        "candidate relation substitution lost a UUID bit");
  }
  IndexStorageFixture fixture;
  for (unsigned version = 0; version != 16; ++version) {
    for (unsigned variant = 0; variant != 4; ++variant) {
      auto request = IndexStorageRequest(fixture);
      request.relation_uuid.bytes[6] = 4 | (version << 4);
      request.relation_uuid.bytes[8] = variant << 6;
      const auto result = exec::ExecuteCanonicalSelectedIndexStorageAccessV1(request);
      const bool valid = version == 7 && variant == 2;
      passed &= Require(valid ? result.diagnostic.ok :
          !result.diagnostic.ok && !result.data_access_observed,
          "invalid selected relation reached physical read");
    }
  }
  for (unsigned bit = 0; bit != 128; ++bit) {
    auto request = IndexStorageRequest(fixture);
    request.selected_index_uuid.bytes[bit / 8] ^= 1u << (bit % 8);
    auto result = exec::ExecuteCanonicalSelectedIndexStorageAccessV1(request);
    passed &= Require(!result.diagnostic.ok && !result.data_access_observed,
                      "selected index substitution lost a UUID bit");
    request = IndexStorageRequest(fixture);
    request.selected_key_descriptor_uuids.front().bytes[bit / 8] ^=
        1u << (bit % 8);
    result = exec::ExecuteCanonicalSelectedIndexStorageAccessV1(request);
    passed &= Require(!result.diagnostic.ok && !result.data_access_observed,
                      "key descriptor substitution lost a UUID bit");
    for (bool mutate_version : {false, true}) {
      request = IndexStorageRequest(fixture);
      const auto resolve = request.resolve_engine_row_version;
      request.resolve_engine_row_version = [resolve, bit, mutate_version](const auto& locator) {
        auto resolved = resolve(locator);
        auto& id = mutate_version ? resolved.version_uuid : resolved.candidate.record_uuid;
        id.bytes[bit / 8] ^= 1u << (bit % 8);
        return resolved;
      };
      result = exec::ExecuteCanonicalSelectedIndexStorageAccessV1(request);
      passed &= Require(!result.diagnostic.ok && result.data_access_observed &&
                            result.scan_result.accepted_record_uuids.empty(),
                        "resolver row/version substitution lost a UUID bit");
    }
  }
  for (const auto kind : {platform::UuidKind::row, platform::UuidKind::unknown,
                         platform::UuidKind::database}) {
    auto request = IndexStorageRequest(fixture);
    request.point_key_components.front().type_descriptor_uuid.kind = kind;
    auto result = exec::ExecuteCanonicalSelectedIndexStorageAccessV1(request);
    passed &= Require(!result.diagnostic.ok && !result.data_access_observed,
                      "wrong key descriptor kind reached access");
    auto tree = fixture.tree;
    tree.index_uuid.kind = kind;
    request = IndexStorageRequest(fixture);
    request.physical_tree = &tree;
    result = exec::ExecuteCanonicalSelectedIndexStorageAccessV1(request);
    passed &= Require(!result.diagnostic.ok && !result.data_access_observed,
                      "wrong physical index kind reached access");
  }
  return passed;
}

}  // namespace

int main() {
  if (!ValidateIndexAccessRechecks() || !ValidateRelationAccessOrder() ||
      !ValidateAuthorityAndReplanRefusals() ||
      !ValidateCapturedCreatorVisibility() ||
      !ValidateCompleteAuthorityRefusalMatrix() ||
      !ValidateAllOrNothingRefusal() ||
      !ValidatePhysicalIndexStorageAcquisition() ||
      !ValidatePhysicalIndexRefusalAndFallbackTruth() ||
      !ValidateBinaryScanIdentity()) {
    return 1;
  }
  std::cout << "QOW-TEST-QRY-004-ACCESS-V1: PASS checks=" << checks << '\n';
  return 0;
}
