// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "hash_digest.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "query/narrow_query_binding_authority.hpp"
#include "query/narrow_query_profile_source.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"
#include "wire/narrow_query_binding_demand_codec.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace hash = scratchbird::core::hash;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;
namespace wire = scratchbird::wire;

constexpr std::string_view kDatatypeCatalogSnapshotUuid =
    "019d0000-0000-7000-8000-00000000d701";
constexpr std::string_view kInt32DescriptorUuid =
    "019d0000-0000-7000-8000-00000000d716";
constexpr std::string_view kInt32TypeUuid =
    "019d0000-0000-7000-8000-00000000d717";
constexpr std::string_view kTextDescriptorUuid =
    "019d0000-0000-7000-8000-00000000d718";
constexpr std::string_view kTextTypeUuid =
    "019d0000-0000-7000-8000-00000000d719";
constexpr std::uint64_t kLargeMemoryGrant = 64ull * 1024ull * 1024ull;
constexpr std::uint64_t kLargeDecodedGrant = 64ull * 1024ull * 1024ull;

[[noreturn]] void Fail(std::string_view detail) {
  std::cerr << "sbsql_narrow_query_profile_source_conformance: " << detail
            << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view detail) {
  if (!condition) Fail(detail);
}

template <typename Result>
void RequireOk(const Result& result, std::string_view detail) {
  if (!result.ok) {
    if constexpr (requires { result.diagnostic; }) {
      std::cerr << result.diagnostic.code << ':'
                << result.diagnostic.message_key << ':'
                << result.diagnostic.detail << '\n';
    } else if constexpr (requires { result.diagnostics; }) {
      if (!result.diagnostics.empty()) {
        std::cerr << result.diagnostics.front().code << ':'
                  << result.diagnostics.front().message_key << ':'
                  << result.diagnostics.front().detail << '\n';
      }
    }
    Fail(detail);
  }
}

std::uint64_t NowMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::string NewUuid(platform::UuidKind kind) {
  static std::atomic<std::uint64_t> sequence{1};
  const auto generated = uuid::GenerateEngineIdentityV7(
      kind, NowMillis() + sequence.fetch_add(1));
  Require(generated.ok(), "engine UUID generation failed");
  return uuid::UuidToString(generated.value.value);
}

wire::NarrowQueryUuid WireUuid(std::string_view text) {
  const auto parsed = uuid::ParseUuid(std::string(text));
  Require(parsed.ok(), "wire UUID parse failed");
  wire::NarrowQueryUuid result{};
  std::copy(parsed.value.bytes.begin(), parsed.value.bytes.end(),
            result.begin());
  return result;
}

std::string Int32Descriptor(bool nullable) {
  return "type=int32;datatype_descriptor_uuid=" +
         std::string(kInt32DescriptorUuid) + ";type_uuid=" +
         std::string(kInt32TypeUuid) + ";nullable=" +
         (nullable ? "true" : "false");
}

std::string TextDescriptor(bool nullable) {
  return "type=text;datatype_descriptor_uuid=" +
         std::string(kTextDescriptorUuid) + ";type_uuid=" +
         std::string(kTextTypeUuid) + ";nullable=" +
         (nullable ? "true" : "false");
}

struct Fixture {
  std::filesystem::path directory;
  std::filesystem::path database_path;
  std::string database_uuid;
  std::string filespace_uuid;
  std::string schema_uuid;
  std::string relation_uuid;
  std::string principal_uuid;
  std::string session_uuid;
  api::EngineRequestContext transaction;
  std::shared_ptr<std::atomic_bool> cancelled =
      std::make_shared<std::atomic_bool>(false);
  std::vector<std::int32_t> ids{10, 20, 30, 40, 50};

  ~Fixture() {
    std::error_code ignored;
    if (!directory.empty()) std::filesystem::remove_all(directory, ignored);
  }
};

api::EngineRequestContext BaseContext(const Fixture& fixture) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = "narrow-profile-source";
  context.database_path = fixture.database_path.string();
  context.database_uuid.canonical = fixture.database_uuid;
  context.database_page_size_bytes = 16384;
  context.default_root_uuid.canonical = fixture.filespace_uuid;
  context.current_schema_uuid.canonical = fixture.schema_uuid;
  context.principal_uuid.canonical = fixture.principal_uuid;
  context.session_uuid.canonical = fixture.session_uuid;
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.datatype_catalog_snapshot_uuid.canonical =
      std::string(kDatatypeCatalogSnapshotUuid);
  context.datatype_catalog_generation = 1;
  context.datatype_registry_generation = 1;
  context.maximum_mga_relation_decoded_bytes_per_pass =
      kLargeDecodedGrant;
  context.catalog_epoch_uuid.canonical = NewUuid(platform::UuidKind::object);
  context.query_cancellation_requested = [flag = fixture.cancelled]() {
    return flag->load();
  };
  return context;
}

api::EngineRequestContext Begin(Fixture* fixture) {
  Require(fixture != nullptr, "fixture is absent");
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(*fixture);
  request.isolation_level = "repeatable_read";
  const auto begun = api::EngineBeginTransaction(request);
  RequireOk(begun, "fixture transaction begin failed");
  auto context = request.context;
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = begun.isolation_level;
  return context;
}

void Rollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  const auto result = api::EngineRollbackTransaction(request);
  RequireOk(result, "fixture rollback failed");
}

Fixture MakeFixture() {
  Fixture fixture;
  fixture.directory = std::filesystem::temp_directory_path() /
                      ("scratchbird_narrow_query_source_" +
                       std::to_string(NowMillis()));
  std::filesystem::create_directories(fixture.directory);
  fixture.database_path = fixture.directory / "narrow_query.sbdb";
  const auto database = uuid::GenerateDurableEngineIdentityV7(
      platform::UuidKind::database, NowMillis() + 100);
  const auto filespace = uuid::GenerateDurableEngineIdentityV7(
      platform::UuidKind::filespace, NowMillis() + 101);
  Require(database.ok() && filespace.ok(),
          "database identity generation failed");
  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = database.value;
  create.filespace_uuid = filespace.value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = NowMillis();
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "fixture database creation failed");

  fixture.database_uuid = uuid::UuidToString(database.value.value);
  fixture.filespace_uuid = uuid::UuidToString(filespace.value.value);
  fixture.schema_uuid = NewUuid(platform::UuidKind::object);
  fixture.relation_uuid = NewUuid(platform::UuidKind::object);
  fixture.principal_uuid = NewUuid(platform::UuidKind::principal);
  fixture.session_uuid = NewUuid(platform::UuidKind::object);
  fixture.transaction = Begin(&fixture);

  api::CrudTableRecord table;
  table.creator_tx = fixture.transaction.local_transaction_id;
  table.table_uuid = fixture.relation_uuid;
  table.default_name = "narrow_profile_values";
  table.columns = {{"id", Int32Descriptor(false)},
                   {"k1", Int32Descriptor(true)},
                   {"k2", Int32Descriptor(true)},
                   {"payload", TextDescriptor(true)}};
  Require(!api::AppendMgaTableMetadata(fixture.transaction, table).error,
          "fixture table metadata append failed");
  api::MgaRelationStorageDescriptor descriptor;
  const auto ensured = api::EnsureMgaRelationStorageDescriptor(
      fixture.transaction, table, {}, &descriptor);
  Require(!ensured.error && descriptor.columns.size() == 4,
          "fixture relation descriptor creation failed");

  const std::vector<std::optional<std::int32_t>> k1 = {1, 1, 1,
                                                       std::nullopt, 2};
  const std::vector<std::optional<std::int32_t>> k2 = {5, 5,
                                                       std::nullopt, 7, 9};
  const std::vector<std::optional<std::string>> payload = {
      "semi;colon", "key=value", "", std::nullopt, "plain"};
  std::vector<api::CrudRowVersionRecord> rows;
  for (std::size_t index = 0; index < fixture.ids.size(); ++index) {
    api::CrudRowVersionRecord row;
    row.creator_tx = fixture.transaction.local_transaction_id;
    row.table_uuid = fixture.relation_uuid;
    row.row_uuid = NewUuid(platform::UuidKind::object);
    row.version_uuid = NewUuid(platform::UuidKind::object);
    row.values = {{"id", std::to_string(fixture.ids[index])},
                  {"k1", k1[index].has_value()
                             ? std::to_string(*k1[index])
                             : std::string("<NULL>")},
                  {"k2", k2[index].has_value()
                             ? std::to_string(*k2[index])
                             : std::string("<NULL>")},
                  {"payload", payload[index].has_value()
                                  ? *payload[index]
                                  : std::string("<NULL>")}};
    rows.push_back(std::move(row));
  }
  std::vector<std::uint64_t> sequences;
  const auto appended = api::AppendMgaRowVersions(
      fixture.transaction, &rows, &sequences);
  Require(!appended.error && sequences.size() == rows.size(),
          "fixture row append failed");
  return fixture;
}

api::EngineRequestContext QueryContext(Fixture* fixture) {
  Require(fixture != nullptr, "fixture is absent");
  auto context = fixture->transaction;
  context.statement_uuid.canonical = NewUuid(platform::UuidKind::object);
  context.statement_receipt_uuid.canonical =
      NewUuid(platform::UuidKind::object);
  api::EnginePublishStatementSnapshotRequest publish;
  publish.context = context;
  const auto snapshot = api::EnginePublishStatementSnapshot(publish);
  RequireOk(snapshot, "statement snapshot publication failed");
  context.statement_snapshot_uuid = snapshot.statement_snapshot_uuid;
  context.snapshot_visible_through_local_transaction_id =
      snapshot.snapshot_vector.visible_committed_high_watermark;
  context.statement_metadata_snapshot_engine_owned = true;
  context.statement_metadata_snapshot_uuid.canonical =
      NewUuid(platform::UuidKind::object);

  context.authorization_context.present = true;
  context.authorization_context.authority_uuid.canonical =
      NewUuid(platform::UuidKind::object);
  context.authorization_context.security_context_generation = 1;
  context.authorization_context.principal_uuid = context.principal_uuid;
  context.authorization_context.security_epoch = context.security_epoch;
  context.authorization_context.policy_epoch = 1;
  context.authorization_context.catalog_generation_id =
      context.catalog_generation_id;
  api::EngineAuthorizationSubject subject;
  subject.subject_uuid = context.principal_uuid;
  subject.subject_kind = "principal";
  context.authorization_context.effective_subjects.push_back(subject);
  api::EngineMaterializedAuthorizationGrant grant;
  grant.grant_uuid.canonical = NewUuid(platform::UuidKind::object);
  grant.subject_uuid = context.principal_uuid;
  grant.subject_kind = "principal";
  grant.target_uuid.canonical = fixture->relation_uuid;
  grant.right = "SELECT";
  grant.security_epoch = context.security_epoch;
  context.authorization_context.grants.push_back(std::move(grant));
  context.trace_tags = {"private_narrow_query_binding_binder"};
  return context;
}

wire::NarrowQuerySourceDemand SourceDemand(
    std::uint32_t ordinal,
    std::string alias,
    const Fixture& fixture) {
  wire::NarrowQuerySourceDemand source;
  source.source_ordinal = ordinal;
  source.relation_object_hint_present = true;
  source.relation_object_uuid_hint = WireUuid(fixture.relation_uuid);
  source.explicit_alias = true;
  source.alias_spelling = std::move(alias);
  return source;
}

wire::NarrowQueryOutputDemand OutputDemand(std::uint32_t ordinal,
                                           std::uint32_t source,
                                           std::string column,
                                           std::string name = {}) {
  wire::NarrowQueryOutputDemand output;
  output.output_ordinal = ordinal;
  output.source_ordinal = source;
  output.source_column_spelling = std::move(column);
  output.output_name_present = !name.empty();
  output.output_name_spelling = std::move(name);
  return output;
}

wire::NarrowQueryOrderingDemand OrderingDemand(
    std::uint32_t ordinal,
    std::string column,
    wire::NarrowQueryDirection direction,
    wire::NarrowQueryNullPlacement null_placement) {
  wire::NarrowQueryOrderingDemand ordering;
  ordering.term_ordinal = ordinal;
  ordering.source_ordinal = 0;
  ordering.source_column_spelling = std::move(column);
  ordering.direction = direction;
  ordering.null_placement = null_placement;
  return ordering;
}

wire::NarrowQueryBindingDemand DecodeDemand(
    wire::NarrowQueryBindingDemand demand) {
  std::vector<std::uint8_t> bytes;
  wire::NarrowQueryBindingDemandError error;
  Require(wire::EncodeNarrowQueryBindingDemand(demand, &bytes, &error),
          "narrow demand encode failed");
  wire::NarrowQueryBindingDemand decoded;
  wire::NarrowQueryBindingDemandValidationContext context;
  context.authenticated_statement_receipt_uuid =
      demand.statement_receipt_uuid;
  context.maximum_mga_relation_decoded_bytes_per_pass =
      demand.maximum_mga_relation_decoded_bytes_per_pass;
  Require(wire::DecodeAndValidateNarrowQueryBindingDemand(
              bytes, context, &decoded, &error),
          "narrow demand decode failed");
  Require(decoded.exact_bytes == bytes,
          "narrow demand exact bytes were not retained");
  return decoded;
}

struct Grant {
  std::uint64_t source_rows_per_occurrence = 100;
  std::uint64_t cumulative_source_rows = 1000;
  std::uint64_t result_rows = 1'048'576;
  std::uint64_t join_combinations = 1'048'576;
  std::uint64_t sort_memory_bytes = kLargeMemoryGrant;
  std::uint64_t decoded_bytes_per_pass = kLargeDecodedGrant;
  std::uint64_t typed_result_transport_bytes_per_packet =
      api::kDefaultTypedResultTransportBytesPerPacket;
  std::uint64_t batch_rows = 64;
};

struct OpenedProfile {
  api::EngineRequestContext context;
  wire::NarrowQueryBinding binding;
  std::unique_ptr<api::NarrowQueryTypedResultOccurrenceSourceV1> source;
};

OpenedProfile OpenProfile(api::EngineRequestContext binder_context,
                          wire::NarrowQueryBindingDemand demand,
                          const Grant& grant = {}) {
  binder_context.maximum_mga_relation_decoded_bytes_per_pass =
      grant.decoded_bytes_per_pass;
  binder_context.maximum_typed_result_transport_bytes_per_packet =
      grant.typed_result_transport_bytes_per_packet;
  demand.statement_receipt_uuid =
      WireUuid(binder_context.statement_receipt_uuid.canonical);
  demand.maximum_mga_relation_decoded_bytes_per_pass =
      grant.decoded_bytes_per_pass;
  demand = DecodeDemand(std::move(demand));
  api::EngineNarrowQueryBindingAuthorityIssueRequestV1 issue;
  issue.context = binder_context;
  issue.demand = std::move(demand);
  issue.policy_snapshot_uuid.canonical = NewUuid(platform::UuidKind::object);
  issue.policy_generation = binder_context.authorization_context.policy_epoch;
  issue.maximum_source_rows_per_occurrence =
      grant.source_rows_per_occurrence;
  issue.maximum_cumulative_source_rows = grant.cumulative_source_rows;
  issue.maximum_result_rows = grant.result_rows;
  issue.maximum_join_combinations = grant.join_combinations;
  issue.maximum_sort_memory_bytes = grant.sort_memory_bytes;
  issue.maximum_batch_rows = grant.batch_rows;
  issue.maximum_mga_relation_decoded_bytes_per_pass =
      grant.decoded_bytes_per_pass;
  issue.maximum_typed_result_transport_bytes_per_packet =
      grant.typed_result_transport_bytes_per_packet;
  const auto issued = api::IssueNarrowQueryBindingAuthorityV1(issue);
  RequireOk(issued, "narrow binding authority issue failed");
  Require(!issued.exact_binding_bytes.empty(),
          "narrow binding authority omitted exact bytes");

  auto consumer_context = binder_context;
  consumer_context.trace_tags = {"private_narrow_query_binding_consumer"};
  api::EngineNarrowQueryBindingAuthorityConsumeRequestV1 consume;
  consume.context = consumer_context;
  consume.exact_binding_bytes = issued.exact_binding_bytes;
  auto consumed = api::ConsumeNarrowQueryBindingAuthorityV1(consume);
  RequireOk(consumed, "narrow binding authority consume failed");

  OpenedProfile opened;
  opened.context = consumer_context;
  opened.binding = consumed.binding;
  api::EngineNarrowQueryProfileSourceOpenRequestV1 open;
  open.context = consumer_context;
  open.binding_authority = std::move(consumed.authority);
  auto result = api::OpenNarrowQueryProfileSourceV1(std::move(open));
  Require(result.ok() && !result.generic_fallback_permitted(),
          "exact narrow profile source open failed");
  opened.source = std::move(result.source);
  return opened;
}

std::vector<api::NarrowQueryTypedResultOccurrenceRowV1> Collect(
    OpenedProfile* opened,
    std::uint64_t maximum_rows = 64) {
  Require(opened != nullptr && opened->source != nullptr,
          "opened profile source is absent");
  std::vector<api::NarrowQueryTypedResultOccurrenceRowV1> rows;
  std::uint64_t batch_ordinal = 0;
  while (true) {
    api::TypedResultProducerStageRequestV1 stage;
    stage.next_batch_ordinal = batch_ordinal;
    stage.row_position = rows.size();
    stage.maximum_rows = maximum_rows;
    stage.maximum_bytes = 16ull * 1024ull * 1024ull;
    stage.cancellation_requested = [] { return false; };
    auto result = opened->source->Stage(stage);
    if (result.outcome == api::TypedResultProducerStageOutcomeV1::empty_eos) {
      Require(result.end_of_cursor && result.rows.empty(),
              "empty EOS carried rows or remained open");
      Require(result.lease.Commit() ==
                  api::TypedResultProducerStageCommitStatusV1::committed,
              "empty EOS source lease did not commit");
      break;
    }
    if (result.outcome != api::TypedResultProducerStageOutcomeV1::batch) {
      std::cerr << result.detail << '\n';
      Fail("narrow source staging refused");
    }
    Require(!result.rows.empty(), "narrow source emitted an empty batch");
    Require(result.lease.Commit() ==
                api::TypedResultProducerStageCommitStatusV1::committed,
            "narrow source batch lease did not commit");
    for (std::size_t index = 0; index < result.rows.size(); ++index) {
      Require(result.rows[index].row_ordinal == index,
              "batch-local row ordinal is not dense");
      rows.push_back(std::move(result.rows[index]));
    }
    ++batch_ordinal;
    if (result.end_of_cursor) break;
  }
  return rows;
}

api::EngineNarrowQueryProfileSourceExecutionMetricsV1 TerminalMetrics(
    const OpenedProfile& opened) {
  Require(opened.source != nullptr, "narrow metrics source is absent");
  api::EngineNarrowQueryProfileSourceExecutionMetricsV1 metrics;
  Require(api::InspectNarrowQueryProfileSourceExecutionMetricsV1(
              opened.source.get(), &metrics),
          "terminal narrow source metrics were unavailable");
  return metrics;
}

void CloseProfile(OpenedProfile* opened) {
  Require(opened != nullptr && opened->source != nullptr,
          "narrow source close target is absent");
  opened->source->Close(api::TypedResultProducerReleaseReasonV1::eos);
}

const api::NarrowQueryTypedResultOccurrenceCellV1& Cell(
    const api::NarrowQueryTypedResultOccurrenceRowV1& row,
    const wire::NarrowQueryOutputOccurrence& output) {
  const auto found = std::find_if(
      row.cells.begin(), row.cells.end(), [&](const auto& cell) {
        return cell.output_occurrence_uuid == output.output_occurrence_uuid &&
               cell.output_occurrence_generation ==
                   output.output_occurrence_generation;
      });
  Require(found != row.cells.end(), "output occurrence cell is missing");
  return *found;
}

std::int32_t Int32(const api::NarrowQueryTypedResultOccurrenceCellV1& cell) {
  Require(cell.state == wire::TypedResultValueState::value_present &&
              cell.canonical_payload.size() == 4,
          "int32 output cell is not canonical");
  const auto bits = static_cast<std::uint32_t>(cell.canonical_payload[0]) |
                    (static_cast<std::uint32_t>(cell.canonical_payload[1])
                     << 8u) |
                    (static_cast<std::uint32_t>(cell.canonical_payload[2])
                     << 16u) |
                    (static_cast<std::uint32_t>(cell.canonical_payload[3])
                     << 24u);
  return static_cast<std::int32_t>(bits);
}

std::optional<std::string> Text(
    const api::NarrowQueryTypedResultOccurrenceCellV1& cell) {
  if (cell.state == wire::TypedResultValueState::sql_null) {
    Require(cell.canonical_payload.empty(),
            "SQL NULL output carried a payload");
    return std::nullopt;
  }
  Require(cell.state == wire::TypedResultValueState::value_present,
          "text output has an invalid value state");
  return std::string(cell.canonical_payload.begin(),
                     cell.canonical_payload.end());
}

wire::NarrowQueryBindingDemand OrderedDemand(const Fixture& fixture) {
  wire::NarrowQueryBindingDemand demand;
  demand.requested_profile = wire::NarrowQueryProfile::ordered_projection;
  demand.sources.push_back(SourceDemand(0, "ordered", fixture));
  demand.outputs.push_back(OutputDemand(0, 0, "id", "id"));
  demand.ordering_terms.push_back(OrderingDemand(
      0, "k1", wire::NarrowQueryDirection::ascending,
      wire::NarrowQueryNullPlacement::last));
  demand.ordering_terms.push_back(OrderingDemand(
      1, "k2", wire::NarrowQueryDirection::descending,
      wire::NarrowQueryNullPlacement::first));
  return demand;
}

wire::NarrowQueryBindingDemand ProjectionDemand(const Fixture& fixture) {
  wire::NarrowQueryBindingDemand demand;
  demand.requested_profile =
      wire::NarrowQueryProfile::projection_occurrence;
  demand.sources.push_back(SourceDemand(0, "projected", fixture));
  demand.outputs.push_back(OutputDemand(0, 0, "payload"));
  demand.outputs.push_back(OutputDemand(1, 0, "payload"));
  return demand;
}

wire::NarrowQueryBindingDemand JoinDemand(const Fixture& fixture,
                                          std::uint32_t source_count,
                                          std::uint64_t offset,
                                          std::uint64_t limit) {
  wire::NarrowQueryBindingDemand demand;
  demand.requested_profile =
      wire::NarrowQueryProfile::alias_distinct_self_join;
  demand.row_offset_present = offset != 0;
  demand.row_offset = offset;
  demand.row_limit_present = true;
  demand.row_limit = limit;
  for (std::uint32_t index = 0; index < source_count; ++index) {
    demand.sources.push_back(
        SourceDemand(index, "self_" + std::to_string(index), fixture));
    demand.outputs.push_back(OutputDemand(
        index, index, "id", "id_" + std::to_string(index)));
  }
  return demand;
}

bool SameOccurrenceRows(
    const std::vector<api::NarrowQueryTypedResultOccurrenceRowV1>& left,
    const std::vector<api::NarrowQueryTypedResultOccurrenceRowV1>& right) {
  if (left.size() != right.size()) return false;
  for (std::size_t row = 0; row < left.size(); ++row) {
    if (left[row].row_ordinal != right[row].row_ordinal ||
        left[row].cells.size() != right[row].cells.size()) {
      return false;
    }
    for (std::size_t cell = 0; cell < left[row].cells.size(); ++cell) {
      const auto& a = left[row].cells[cell];
      const auto& b = right[row].cells[cell];
      if (a.output_occurrence_uuid != b.output_occurrence_uuid ||
          a.output_occurrence_generation != b.output_occurrence_generation ||
          a.state != b.state ||
          a.canonical_payload != b.canonical_payload) {
        return false;
      }
    }
  }
  return true;
}

void TestStageLeaseAbortRetry(Fixture* fixture) {
  {
    auto opened = OpenProfile(QueryContext(fixture), ProjectionDemand(*fixture));
    api::TypedResultProducerStageRequestV1 stage;
    stage.maximum_rows = 5;
    // One duplicate-projection row fits; the next becomes provisional pending
    // state, exercising rollback of both traversal and the pending-row cache.
    stage.maximum_bytes = 500;
    stage.cancellation_requested = [] { return false; };
    auto first = opened.source->Stage(stage);
    Require(first.outcome == api::TypedResultProducerStageOutcomeV1::batch &&
                first.rows.size() == 1 && !first.end_of_cursor,
            "pending-row rollback fixture did not stage one bounded row");
    const auto expected = first.rows;
    first.lease.Abort();
    auto retried = opened.source->Stage(stage);
    Require(retried.outcome == api::TypedResultProducerStageOutcomeV1::batch &&
                SameOccurrenceRows(expected, retried.rows) &&
                retried.lease.Commit() ==
                    api::TypedResultProducerStageCommitStatusV1::committed,
            "projection abort did not preserve the identical pending batch");
    CloseProfile(&opened);
  }
  {
    Grant exact;
    // The retained result grant also admits the exact OFFSET + LIMIT demand
    // extent.  Nine combinations are inspected to publish seven rows; keeping
    // the grant at nine still detects an accidental second seven-row commit.
    exact.result_rows = 9;
    exact.batch_rows = 7;
    exact.join_combinations = 9;
    auto opened = OpenProfile(QueryContext(fixture),
                              JoinDemand(*fixture, 3, 2, 7), exact);
    api::TypedResultProducerStageRequestV1 stage;
    stage.maximum_rows = 7;
    stage.maximum_bytes = 16ull * 1024ull * 1024ull;
    stage.cancellation_requested = [] { return false; };
    auto first = opened.source->Stage(stage);
    Require(first.outcome == api::TypedResultProducerStageOutcomeV1::batch &&
                first.rows.size() == 7 && first.end_of_cursor,
            "join rollback fixture did not stage the exact terminal batch");
    const auto expected = first.rows;
    first.lease.Abort();
    auto retried = opened.source->Stage(stage);
    Require(retried.outcome == api::TypedResultProducerStageOutcomeV1::batch &&
                retried.end_of_cursor &&
                SameOccurrenceRows(expected, retried.rows) &&
                retried.lease.Commit() ==
                    api::TypedResultProducerStageCommitStatusV1::committed,
            "join abort recharged work or changed the logical batch");
    const auto metrics = TerminalMetrics(opened);
    Require(metrics.sources.size() == 3,
            "terminal join metrics were unavailable after lease commit");
    CloseProfile(&opened);
  }
}

void TestOrderedProjection(Fixture* fixture) {
  auto opened = OpenProfile(QueryContext(fixture), OrderedDemand(*fixture));
  const auto rows = Collect(&opened, 64);
  const std::vector<std::int32_t> expected{30, 10, 20, 50, 40};
  Require(rows.size() == expected.size(),
          "ordered profile returned the wrong cardinality");
  for (std::size_t index = 0; index < rows.size(); ++index) {
    Require(Int32(Cell(rows[index], opened.binding.outputs[0])) ==
                expected[index],
            "multi-term ordering or stable equal-key order drifted");
  }
}

void TestDuplicateProjection(Fixture* fixture) {
  auto opened = OpenProfile(QueryContext(fixture), ProjectionDemand(*fixture));
  Require(opened.binding.outputs.size() == 2 &&
              opened.binding.outputs[0].output_occurrence_uuid !=
                  opened.binding.outputs[1].output_occurrence_uuid,
          "duplicate projection reused an output occurrence identity");
  api::EngineNarrowQueryProfileSourceExecutionMetricsV1 premature_metrics;
  Require(!api::InspectNarrowQueryProfileSourceExecutionMetricsV1(
              opened.source.get(), &premature_metrics),
          "narrow metrics were exposed before terminal EOS");
  const auto rows = Collect(&opened, 2);
  const std::vector<std::optional<std::string>> expected = {
      "semi;colon", "key=value", "", std::nullopt, "plain"};
  Require(rows.size() == expected.size(),
          "duplicate projection returned the wrong cardinality");
  for (std::size_t index = 0; index < rows.size(); ++index) {
    const auto left = Text(Cell(rows[index], opened.binding.outputs[0]));
    const auto right = Text(Cell(rows[index], opened.binding.outputs[1]));
    Require(left == expected[index] && right == expected[index] &&
                left == right,
            "duplicate occurrence lost semicolon/equal/empty/NULL payload");
  }
  const auto metrics = TerminalMetrics(opened);
  Require(metrics.sources.size() == 1 &&
              metrics.sources[0].source_ordinal == 0 &&
              metrics.sources[0].visible_row_count == expected.size() &&
              metrics.sources[0].delivered_row_count == expected.size() &&
              metrics.sources[0].second_pass_scanned_row_version_count ==
                  expected.size() &&
              metrics.sources[0].second_pass_decoded_byte_count != 0 &&
              metrics.sources[0].second_pass_storage_bytes_read != 0 &&
              metrics.sources[0].complete_value_delivery &&
              !metrics.sources[0].delivery_stopped_by_bound,
          "full-delivery terminal metrics are not exact");
  CloseProfile(&opened);
  api::EngineNarrowQueryProfileSourceExecutionMetricsV1 closed_metrics;
  Require(!api::InspectNarrowQueryProfileSourceExecutionMetricsV1(
              opened.source.get(), &closed_metrics),
          "narrow metrics remained visible after Close");
}

void TestThreeWayJoinAndBounds(Fixture* fixture) {
  auto opened = OpenProfile(QueryContext(fixture),
                            JoinDemand(*fixture, 3, 2, 7));
  const auto rows = Collect(&opened, 64);
  Require(rows.size() == 7,
          "one large batch overran or under-ran post-project LIMIT");
  std::vector<std::vector<std::int32_t>> expected;
  for (const auto first : fixture->ids) {
    for (const auto second : fixture->ids) {
      for (const auto third : fixture->ids) {
        expected.push_back({first, second, third});
      }
    }
  }
  expected.erase(expected.begin(), expected.begin() + 2);
  expected.resize(7);
  for (std::size_t row = 0; row < rows.size(); ++row) {
    Require(rows[row].cells.size() == 3,
            "three-way join output lost a source occurrence");
    for (std::size_t output = 0; output < 3; ++output) {
      Require(Int32(Cell(rows[row], opened.binding.outputs[output])) ==
                  expected[row][output],
              "left-deep CROSS offset/limit semantics drifted");
    }
  }
}

void TestNineWayLimitOne(Fixture* fixture) {
  Grant bounded_source_rows;
  bounded_source_rows.source_rows_per_occurrence = 1;
  bounded_source_rows.cumulative_source_rows = 9;
  auto opened = OpenProfile(QueryContext(fixture),
                            JoinDemand(*fixture, 9, 0, 1),
                            bounded_source_rows);
  const auto rows = Collect(&opened, 64);
  Require(rows.size() == 1 && rows.front().cells.size() == 9,
          "nine-way LIMIT 1 did not short-circuit to one tuple");
  const std::set<std::int32_t> members(fixture->ids.begin(),
                                       fixture->ids.end());
  for (std::size_t output = 0; output < 9; ++output) {
    Require(members.contains(
                Int32(Cell(rows.front(), opened.binding.outputs[output]))),
            "nine-way unordered tuple contains a non-source member");
  }
  const auto metrics = TerminalMetrics(opened);
  Require(metrics.sources.size() == 9,
          "nine-way terminal metrics lost a source occurrence");
  for (std::size_t index = 0; index < metrics.sources.size(); ++index) {
    const auto& source = metrics.sources[index];
    Require(source.source_ordinal == index &&
                source.visible_row_count == fixture->ids.size() &&
                source.delivered_row_count == 1 &&
                source.second_pass_scanned_row_version_count == 1 &&
                source.second_pass_decoded_byte_count != 0 &&
                source.second_pass_storage_bytes_read != 0 &&
                !source.complete_value_delivery &&
                source.delivery_stopped_by_bound,
            "nine-way LIMIT 1 did not stop every source at one row");
  }
  CloseProfile(&opened);
}

void TestZeroDeliveryBound(Fixture* fixture) {
  auto demand = ProjectionDemand(*fixture);
  demand.row_limit_present = true;
  demand.row_limit = 0;
  auto opened = OpenProfile(QueryContext(fixture), std::move(demand));
  const auto rows = Collect(&opened, 64);
  Require(rows.empty(), "LIMIT 0 profile published a value row");
  const auto metrics = TerminalMetrics(opened);
  Require(metrics.sources.size() == 1 &&
              metrics.sources[0].source_ordinal == 0 &&
              metrics.sources[0].visible_row_count == fixture->ids.size() &&
              metrics.sources[0].delivered_row_count == 0 &&
              metrics.sources[0].second_pass_scanned_row_version_count == 0 &&
              metrics.sources[0].second_pass_decoded_byte_count == 0 &&
              metrics.sources[0].second_pass_storage_bytes_read == 0 &&
              !metrics.sources[0].complete_value_delivery &&
              metrics.sources[0].delivery_stopped_by_bound,
          "LIMIT 0 did not retain full first-pass cardinality with zero delivery");
  CloseProfile(&opened);
}

class NonNarrowOccurrenceSource final
    : public api::NarrowQueryTypedResultOccurrenceSourceV1 {
 public:
  api::NarrowQueryTypedResultOccurrenceStageResultV1 Stage(
      const api::TypedResultProducerStageRequestV1&) override {
    api::NarrowQueryTypedResultOccurrenceStageResultV1 result;
    result.outcome = api::TypedResultProducerStageOutcomeV1::empty_eos;
    result.end_of_cursor = true;
    return result;
  }

  void Close(api::TypedResultProducerReleaseReasonV1) noexcept override {}
};

void TestMetricsRejectNonNarrowSource() {
  NonNarrowOccurrenceSource source;
  api::EngineNarrowQueryProfileSourceExecutionMetricsV1 metrics;
  Require(!api::InspectNarrowQueryProfileSourceExecutionMetricsV1(
              &source, &metrics),
          "terminal metrics admitted a non-narrow source");
}

void TestResourceAndCancellation(Fixture* fixture) {
  Grant tiny_memory;
  // Exercise the retained/source-sort grant itself.  This must not rely on
  // the independent MGA decoded-bytes-per-pass authority being aliased to
  // sort memory.
  tiny_memory.sort_memory_bytes = 1;
  auto memory = OpenProfile(QueryContext(fixture), OrderedDemand(*fixture),
                            tiny_memory);
  api::TypedResultProducerStageRequestV1 stage;
  stage.maximum_rows = 8;
  stage.maximum_bytes = 4096;
  stage.cancellation_requested = [] { return false; };
  const auto refused = memory.source->Stage(stage);
  Require(refused.outcome ==
              api::TypedResultProducerStageOutcomeV1::refused &&
              refused.detail.starts_with("RESOURCE.BUDGET_EXCEEDED:"),
          "sort/source memory exhaustion did not fail closed");

  Grant tiny_join;
  tiny_join.join_combinations = 3;
  auto join = OpenProfile(QueryContext(fixture),
                          JoinDemand(*fixture, 3, 2, 7), tiny_join);
  const auto join_refused = join.source->Stage(stage);
  Require(join_refused.outcome ==
              api::TypedResultProducerStageOutcomeV1::refused &&
              join_refused.detail.starts_with("RESOURCE.BUDGET_EXCEEDED:"),
          "join-combination exhaustion did not fail closed");

  fixture->cancelled->store(false);
  auto cancelled = OpenProfile(QueryContext(fixture),
                               ProjectionDemand(*fixture));
  fixture->cancelled->store(true);
  const auto cancellation = cancelled.source->Stage(stage);
  Require(cancellation.outcome ==
              api::TypedResultProducerStageOutcomeV1::cancelled &&
              cancellation.rows.empty(),
          "pre-source cancellation was not deterministic");
  fixture->cancelled->store(false);
}

void TestIndependentDecodedPassCeiling() {
  auto fixture = MakeFixture();
  api::CrudRowVersionRecord row;
  row.creator_tx = fixture.transaction.local_transaction_id;
  row.table_uuid = fixture.relation_uuid;
  row.row_uuid = NewUuid(platform::UuidKind::object);
  row.version_uuid = NewUuid(platform::UuidKind::object);
  row.values = {{"id", "60"},
                {"k1", "3"},
                {"k2", "11"},
                {"payload", std::string(80ull * 1024ull, 'x')}};
  std::uint64_t sequence = 0;
  Require(!api::AppendMgaRowVersion(fixture.transaction, row, &sequence).error &&
              sequence != 0,
          "large decoded-pass fixture append failed");

  Grant decoded_bound;
  decoded_bound.decoded_bytes_per_pass = 64ull * 1024ull;
  auto opened = OpenProfile(QueryContext(&fixture),
                            ProjectionDemand(fixture), decoded_bound);
  api::TypedResultProducerStageRequestV1 stage;
  stage.maximum_rows = 8;
  stage.maximum_bytes = 1ull * 1024ull * 1024ull;
  stage.cancellation_requested = [] { return false; };
  const auto refused = opened.source->Stage(stage);
  Require(refused.outcome ==
              api::TypedResultProducerStageOutcomeV1::refused &&
              refused.detail.starts_with("RESOURCE.BUDGET_EXCEEDED:") &&
              refused.detail.find("maximum_decoded_bytes") !=
                  std::string::npos,
          "independent MGA decoded-pass ceiling was ignored or aliased");
  api::EngineNarrowQueryProfileSourceExecutionMetricsV1 metrics;
  Require(!api::InspectNarrowQueryProfileSourceExecutionMetricsV1(
              opened.source.get(), &metrics),
          "refused decoded-pass source exposed terminal metrics");
  Rollback(fixture.transaction);
}

struct RelationPostStateObservation {
  bool count_ok = false;
  std::uint64_t visible_row_count = 0;
  std::uint64_t scanned_row_version_count = 0;
  std::uint64_t decoded_byte_count = 0;
  bool memory_receipt_complete = false;
  api::EngineApiDiagnostic diagnostic;
  std::array<std::pair<std::string, std::string>, 3> files;
};

std::string FileFingerprint(const std::filesystem::path& path) {
  std::error_code error;
  if (!std::filesystem::exists(path, error)) {
    return error ? "stat_error:" + error.message() : "absent";
  }
  const auto extent = std::filesystem::file_size(path, error);
  if (error || extent > std::numeric_limits<std::size_t>::max()) {
    return "extent_error:" + error.message();
  }
  std::vector<platform::byte> bytes(static_cast<std::size_t>(extent));
  std::ifstream input(path, std::ios::binary);
  if (!input ||
      (!bytes.empty() &&
       !input.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size())))) {
    return "read_error";
  }
  const auto digest = hash::ComputeSha256Digest(bytes);
  return digest.ok()
             ? std::to_string(extent) + ":sha256:" +
                   hash::HexLower(digest.digest)
             : "hash_error";
}

RelationPostStateObservation ObserveRelationPostState(
    Fixture* fixture,
    const api::EngineRequestContext& read_context) {
  RelationPostStateObservation observation;
  api::MgaVisibleHeapRelationCountRequest request;
  request.relation_uuid = fixture->relation_uuid;
  request.maximum_decoded_bytes = kLargeDecodedGrant;
  request.maximum_memory_bytes = kLargeMemoryGrant;
  request.cancellation_requested = [] { return false; };
  const auto counted = api::CountVisibleMgaHeapRelation(read_context, request);
  observation.count_ok = counted.ok;
  observation.visible_row_count = counted.visible_row_count;
  observation.scanned_row_version_count =
      counted.scanned_row_version_count;
  observation.decoded_byte_count = counted.decoded_byte_count;
  observation.memory_receipt_complete = counted.memory_receipt_complete;
  observation.diagnostic = counted.diagnostic;
  const auto root = fixture->database_path.string() +
                    ".sb.mga_relation_scope/" + fixture->relation_uuid;
  const std::array<std::string, 3> paths{
      root + ".rows", root + ".rows.sbnr", root + ".summary"};
  for (std::size_t index = 0; index < paths.size(); ++index) {
    observation.files[index] =
        {paths[index], FileFingerprint(paths[index])};
  }
  return observation;
}

void PrintRelationPostState(std::string_view label,
                            const RelationPostStateObservation& observation) {
  std::cerr << label << "_count=" << (observation.count_ok ? 1 : 0)
            << ',' << observation.visible_row_count << ','
            << observation.scanned_row_version_count << ','
            << observation.decoded_byte_count << ','
            << (observation.memory_receipt_complete ? 1 : 0) << ','
            << observation.diagnostic.code << ':'
            << observation.diagnostic.message_key << ':'
            << observation.diagnostic.detail << '\n';
  for (const auto& [path, fingerprint] : observation.files) {
    std::cerr << label << "_file=" << path
              << ";fingerprint=" << fingerprint << '\n';
  }
}

void RequireUnchangedPostState(
    Fixture* fixture,
    const api::EngineRequestContext& read_context,
    const RelationPostStateObservation& before) {
  const auto after = ObserveRelationPostState(fixture, read_context);
  const bool unchanged =
      before.count_ok && after.count_ok &&
      before.visible_row_count == fixture->ids.size() &&
      after.visible_row_count == before.visible_row_count &&
      before.scanned_row_version_count == after.scanned_row_version_count &&
      before.decoded_byte_count == after.decoded_byte_count &&
      before.memory_receipt_complete && after.memory_receipt_complete &&
      before.files == after.files;
  if (!unchanged) {
    PrintRelationPostState("post_state_before", before);
    PrintRelationPostState("post_state_after", after);
    for (std::size_t index = 0; index < before.files.size(); ++index) {
      std::cerr << "post_state_file=" << before.files[index].first
                << ";before=" << before.files[index].second
                << ";after=" << after.files[index].second << '\n';
    }
  }
  Require(unchanged,
          "read-only profile execution changed independent MGA post-state");
}

}  // namespace

int main() {
  try {
    auto fixture = MakeFixture();
    const auto post_state_context = QueryContext(&fixture);
    const auto initial_post_state =
        ObserveRelationPostState(&fixture, post_state_context);
    if (!initial_post_state.count_ok ||
        initial_post_state.visible_row_count != fixture.ids.size() ||
        !initial_post_state.memory_receipt_complete) {
      PrintRelationPostState("initial_post_state", initial_post_state);
    }
    Require(initial_post_state.count_ok &&
                initial_post_state.visible_row_count == fixture.ids.size() &&
                initial_post_state.memory_receipt_complete,
            "initial independent MGA post-state oracle is invalid");
    TestOrderedProjection(&fixture);
    TestStageLeaseAbortRetry(&fixture);
    TestDuplicateProjection(&fixture);
    TestThreeWayJoinAndBounds(&fixture);
    TestNineWayLimitOne(&fixture);
    TestZeroDeliveryBound(&fixture);
    TestMetricsRejectNonNarrowSource();
    TestResourceAndCancellation(&fixture);
    RequireUnchangedPostState(&fixture, post_state_context,
                              initial_post_state);
    Rollback(fixture.transaction);
    TestIndependentDecodedPassCeiling();
    std::cout << "PASS narrow query profile source profiles=3 "
                 "stable_order duplicate_occurrence cross_2_to_9 "
                 "limit_offset budgets cancellation stage_lease_retry "
                 "unchanged_post_state\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& exception) {
    std::cerr << "sbsql_narrow_query_profile_source_conformance: FAIL: "
              << exception.what() << '\n';
    return EXIT_FAILURE;
  }
}
