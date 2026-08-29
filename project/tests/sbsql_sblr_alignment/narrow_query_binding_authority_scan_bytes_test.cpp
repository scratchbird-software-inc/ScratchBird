// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "query/narrow_query_binding_authority.hpp"
#include "runtime_platform.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"
#include "wire/narrow_query_binding_demand_codec.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;
namespace wire = scratchbird::wire;

constexpr std::string_view kDatatypeCatalogSnapshotUuid =
    "019d0000-0000-7000-8000-00000000d701";
constexpr std::string_view kInt32DescriptorUuid =
    "019d0000-0000-7000-8000-00000000d716";
constexpr std::string_view kInt32TypeUuid =
    "019d0000-0000-7000-8000-00000000d717";
constexpr std::uint64_t kScanBytes = 64ull * 1024ull * 1024ull;
constexpr std::uint64_t kTransportBytes = 64ull * 1024ull;

[[noreturn]] void Fail(std::string_view detail) {
  std::cerr << "narrow_query_binding_authority_scan_bytes: " << detail
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

std::string Int32Descriptor() {
  return "type=int32;datatype_descriptor_uuid=" +
         std::string(kInt32DescriptorUuid) + ";type_uuid=" +
         std::string(kInt32TypeUuid) + ";nullable=false";
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
  api::MgaRelationStorageDescriptor descriptor;
  std::shared_ptr<std::atomic_bool> cancelled =
      std::make_shared<std::atomic_bool>(false);

  ~Fixture() {
    std::error_code ignored;
    if (!directory.empty()) std::filesystem::remove_all(directory, ignored);
  }
};

api::EngineRequestContext BaseContext(const Fixture& fixture) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = "narrow-authority-scan-bytes";
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
  context.maximum_mga_relation_decoded_bytes_per_pass = kScanBytes;
  context.maximum_typed_result_transport_bytes_per_packet = kTransportBytes;
  context.name_resolution_epoch = 1;
  context.datatype_catalog_snapshot_uuid.canonical =
      std::string(kDatatypeCatalogSnapshotUuid);
  context.datatype_catalog_generation = 1;
  context.datatype_registry_generation = 1;
  context.catalog_epoch_uuid.canonical = NewUuid(platform::UuidKind::object);
  context.query_cancellation_requested = [flag = fixture.cancelled]() {
    return flag->load();
  };
  return context;
}

Fixture MakeFixture() {
  Fixture fixture;
  fixture.directory = std::filesystem::temp_directory_path() /
                      ("scratchbird_narrow_authority_scan_" +
                       std::to_string(NowMillis()));
  std::filesystem::create_directories(fixture.directory);
  fixture.database_path = fixture.directory / "authority.sbdb";
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
  Require(db::CreateDatabaseFile(create).ok(),
          "fixture database creation failed");

  fixture.database_uuid = uuid::UuidToString(database.value.value);
  fixture.filespace_uuid = uuid::UuidToString(filespace.value.value);
  fixture.schema_uuid = NewUuid(platform::UuidKind::object);
  fixture.relation_uuid = NewUuid(platform::UuidKind::object);
  fixture.principal_uuid = NewUuid(platform::UuidKind::principal);
  fixture.session_uuid = NewUuid(platform::UuidKind::object);

  api::EngineBeginTransactionRequest begin;
  begin.context = BaseContext(fixture);
  begin.isolation_level = "repeatable_read";
  const auto begun = api::EngineBeginTransaction(begin);
  RequireOk(begun, "fixture transaction begin failed");
  fixture.transaction = begin.context;
  fixture.transaction.local_transaction_id = begun.local_transaction_id;
  fixture.transaction.transaction_uuid = begun.transaction_uuid;
  fixture.transaction.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  fixture.transaction.transaction_isolation_level = begun.isolation_level;

  api::CrudTableRecord table;
  table.creator_tx = fixture.transaction.local_transaction_id;
  table.table_uuid = fixture.relation_uuid;
  table.default_name = "authority_scan_values";
  table.columns = {{"id", Int32Descriptor()}};
  Require(!api::AppendMgaTableMetadata(fixture.transaction, table).error,
          "fixture table metadata append failed");
  const auto ensured = api::EnsureMgaRelationStorageDescriptor(
      fixture.transaction, table, {}, &fixture.descriptor);
  Require(!ensured.error && fixture.descriptor.columns.size() == 1,
          "fixture relation descriptor creation failed");
  return fixture;
}

api::EngineRequestContext BindingContext(Fixture* fixture) {
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

wire::NarrowQueryBindingDemand MakeDemand(
    const api::EngineRequestContext& context,
    const Fixture& fixture) {
  wire::NarrowQueryBindingDemand demand;
  demand.statement_receipt_uuid =
      WireUuid(context.statement_receipt_uuid.canonical);
  demand.requested_profile = wire::NarrowQueryProfile::projection_occurrence;
  demand.maximum_mga_relation_decoded_bytes_per_pass = kScanBytes;
  wire::NarrowQuerySourceDemand source;
  source.source_ordinal = 0;
  source.relation_object_hint_present = true;
  source.relation_object_uuid_hint = WireUuid(fixture.relation_uuid);
  source.explicit_alias = true;
  source.alias_spelling = "authority_source";
  demand.sources.push_back(std::move(source));
  for (std::uint32_t ordinal = 0; ordinal < 2; ++ordinal) {
    wire::NarrowQueryOutputDemand output;
    output.output_ordinal = ordinal;
    output.source_ordinal = 0;
    output.source_column_spelling = "id";
    output.output_name_present = true;
    output.output_name_spelling = ordinal == 0 ? "left_id" : "right_id";
    demand.outputs.push_back(std::move(output));
  }
  std::vector<std::uint8_t> bytes;
  wire::NarrowQueryBindingDemandError error;
  Require(wire::EncodeNarrowQueryBindingDemand(demand, &bytes, &error),
          "demand encode failed");
  wire::NarrowQueryBindingDemand decoded;
  wire::NarrowQueryBindingDemandValidationContext validation;
  validation.authenticated_statement_receipt_uuid =
      demand.statement_receipt_uuid;
  validation.maximum_mga_relation_decoded_bytes_per_pass = kScanBytes;
  Require(wire::DecodeAndValidateNarrowQueryBindingDemand(
              bytes, validation, &decoded, &error),
          "demand decode failed");
  return decoded;
}

void Rollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  RequireOk(api::EngineRollbackTransaction(request),
            "fixture rollback failed");
}

void TestAuthorityScanByteAndProjectionContract() {
  auto fixture = MakeFixture();
  const auto binder_context = BindingContext(&fixture);
  api::EngineNarrowQueryBindingAuthorityIssueRequestV1 issue;
  issue.context = binder_context;
  issue.demand = MakeDemand(binder_context, fixture);
  issue.policy_snapshot_uuid.canonical = NewUuid(platform::UuidKind::object);
  issue.policy_generation = binder_context.authorization_context.policy_epoch;
  issue.maximum_source_rows_per_occurrence = 64;
  issue.maximum_cumulative_source_rows = 64;
  issue.maximum_result_rows = 64;
  issue.maximum_join_combinations = 64;
  issue.maximum_sort_memory_bytes = 1;  // Deliberately not the scan-byte grant.
  issue.maximum_batch_rows = 16;
  issue.maximum_mga_relation_decoded_bytes_per_pass = kScanBytes;
  issue.maximum_typed_result_transport_bytes_per_packet = kTransportBytes;

  auto mismatched_transport = issue;
  ++mismatched_transport.maximum_typed_result_transport_bytes_per_packet;
  const auto mismatch_refusal =
      api::IssueNarrowQueryBindingAuthorityV1(mismatched_transport);
  Require(!mismatch_refusal.ok &&
              mismatch_refusal.diagnostic.code ==
                  "RESOURCE.BUDGET_EXCEEDED",
          "cross-context typed-result packet ceiling was accepted");

  const auto issued = api::IssueNarrowQueryBindingAuthorityV1(issue);
  RequireOk(issued, "binding authority issue failed");
  Require(issued.binding.maximum_mga_relation_decoded_bytes_per_pass ==
              kScanBytes &&
              issued.exact_binding_bytes.size() > 480u &&
              platform::LoadLittle64(issued.exact_binding_bytes.data() + 472u) ==
                  kScanBytes,
          "issued binding lost the exact scan-byte grant");

  auto consumer_context = binder_context;
  consumer_context.trace_tags = {"private_narrow_query_binding_consumer"};
  api::EngineNarrowQueryBindingAuthorityConsumeRequestV1 consume;
  consume.context = consumer_context;
  consume.exact_binding_bytes = issued.exact_binding_bytes;
  auto consumed = api::ConsumeNarrowQueryBindingAuthorityV1(consume);
  RequireOk(consumed, "binding authority consume failed");

  api::EngineNarrowQueryBindingAuthoritySnapshotV1 snapshot;
  api::EngineApiDiagnostic diagnostic;
  Require(api::CopyNarrowQueryBindingAuthoritySnapshotV1(
              consumed.authority, &snapshot, &diagnostic) &&
              snapshot.binding.maximum_mga_relation_decoded_bytes_per_pass ==
                  kScanBytes &&
              snapshot.resource_grant
                      .maximum_mga_relation_decoded_bytes_per_pass ==
                  kScanBytes &&
              snapshot.resource_grant
                      .maximum_typed_result_transport_bytes_per_packet ==
                  kTransportBytes &&
              snapshot.pinned_context
                      .maximum_typed_result_transport_bytes_per_packet ==
                  kTransportBytes &&
              snapshot.resource_grant.maximum_sort_memory_bytes == 1 &&
              snapshot.resource_grant.maximum_batch_rows == 16 &&
              snapshot.resource_grant.maximum_result_rows == 64 &&
              snapshot.resource_grant.grant_generation ==
                  snapshot.binding.resource_grant_generation,
          "retained snapshot aliased or omitted an independent grant");

  auto wrong_retention_context = consumer_context;
  ++wrong_retention_context.resource_epoch;
  const auto wrong_epoch_retention =
      api::RetainNarrowQueryTypedResultResourceGrantReceiptV1(
          consumed.authority, wrong_retention_context);
  Require(!wrong_epoch_retention.ok,
          "cross-resource-epoch typed-result grant was retained");
  wrong_retention_context = consumer_context;
  ++wrong_retention_context
        .maximum_typed_result_transport_bytes_per_packet;
  const auto wrong_ceiling_retention =
      api::RetainNarrowQueryTypedResultResourceGrantReceiptV1(
          consumed.authority, wrong_retention_context);
  Require(!wrong_ceiling_retention.ok,
          "cross-context typed-result ceiling was retained");

  auto retained = api::RetainNarrowQueryTypedResultResourceGrantReceiptV1(
      consumed.authority, consumer_context);
  RequireOk(retained, "typed-result resource grant retention failed");
  Require(retained.receipt_handle != nullptr &&
              retained.grant_receipt_uuid ==
                  snapshot.binding.resource_grant_receipt_uuid &&
              retained.grant_generation ==
                  snapshot.binding.resource_grant_generation &&
              retained.maximum_typed_result_transport_bytes_per_packet ==
                  kTransportBytes,
          "typed-result resource grant was not pinned to the binding");
  const auto second_retention =
      api::RetainNarrowQueryTypedResultResourceGrantReceiptV1(
          consumed.authority, consumer_context);
  Require(!second_retention.ok,
          "typed-result resource grant was retained more than once");

  using PublicationCharge =
      api::EngineNarrowQueryPublicationChargeStatusV1;
  fixture.cancelled->store(true);
  Require(api::CommitNarrowQueryPublicationChargeV1(
              &consumed.authority, consumer_context, 48, 16) ==
              PublicationCharge::cancelled,
          "cancelled publication charge was committed");
  fixture.cancelled->store(false);
  auto stale_charge_context = consumer_context;
  ++stale_charge_context.resource_epoch;
  Require(api::CommitNarrowQueryPublicationChargeV1(
              &consumed.authority, stale_charge_context, 48, 16) ==
              PublicationCharge::stale,
          "cross-context publication charge was committed");

  Require(api::CommitNarrowQueryPublicationChargeV1(
              &consumed.authority, consumer_context, 48, 16) ==
              PublicationCharge::committed,
          "first paired publication charge was refused");
  Require(api::CommitNarrowQueryPublicationChargeV1(
              &consumed.authority, consumer_context, 17, 1) ==
              PublicationCharge::resource_budget_exceeded,
          "cumulative result-row overflow was committed");
  Require(api::CommitNarrowQueryPublicationChargeV1(
              &consumed.authority, consumer_context, 16, 17) ==
              PublicationCharge::resource_budget_exceeded,
          "per-batch overflow was committed");
  Require(api::CommitNarrowQueryPublicationChargeV1(
              &consumed.authority, consumer_context, 16, 16) ==
              PublicationCharge::committed,
          "refused paired charge partially consumed result-row authority");
  Require(api::CommitNarrowQueryPublicationChargeV1(
              &consumed.authority, consumer_context, 1, 1) ==
              PublicationCharge::resource_budget_exceeded,
          "successful publication charge was not consumed exactly once");

  const auto descriptor_packet = retained.receipt_handle->ObserveGrant(
      retained.grant_receipt_uuid, retained.grant_generation,
      retained.maximum_typed_result_transport_bytes_per_packet,
      kTransportBytes);
  const auto row_packet = retained.receipt_handle->ObserveGrant(
      retained.grant_receipt_uuid, retained.grant_generation,
      retained.maximum_typed_result_transport_bytes_per_packet,
      kTransportBytes);
  Require(descriptor_packet == api::TypedResultProducerGrantObservationV1::live &&
              row_packet == api::TypedResultProducerGrantObservationV1::live,
          "typed-result packets were cumulatively charged");
  Require(retained.receipt_handle->ObserveGrant(
              retained.grant_receipt_uuid, retained.grant_generation,
              retained.maximum_typed_result_transport_bytes_per_packet, 1) ==
              api::TypedResultProducerGrantObservationV1::live &&
              retained.receipt_handle->ObserveGrant(
                  retained.grant_receipt_uuid, retained.grant_generation,
                  retained.maximum_typed_result_transport_bytes_per_packet,
                  0) ==
                  api::TypedResultProducerGrantObservationV1::exhausted &&
              retained.receipt_handle->ObserveGrant(
                  retained.grant_receipt_uuid, retained.grant_generation,
                  retained.maximum_typed_result_transport_bytes_per_packet,
                  kTransportBytes + 1) ==
                  api::TypedResultProducerGrantObservationV1::exhausted,
          "typed-result packet ceiling boundary was not exact");
  auto wrong_grant_uuid = retained.grant_receipt_uuid;
  wrong_grant_uuid.front() ^= 0x01u;
  Require(retained.receipt_handle->ObserveGrant(
              wrong_grant_uuid, retained.grant_generation,
              retained.maximum_typed_result_transport_bytes_per_packet, 1) ==
              api::TypedResultProducerGrantObservationV1::stale_or_released &&
              retained.receipt_handle->ObserveGrant(
                  retained.grant_receipt_uuid,
                  retained.grant_generation + 1,
                  retained.maximum_typed_result_transport_bytes_per_packet,
                  1) ==
              api::TypedResultProducerGrantObservationV1::stale_or_released &&
              retained.receipt_handle->ObserveGrant(
                  retained.grant_receipt_uuid, retained.grant_generation,
                  kTransportBytes + 1, 1) ==
              api::TypedResultProducerGrantObservationV1::stale_or_released,
          "typed-result grant identity drift was not stale");
  retained.receipt_handle->Release(
      api::TypedResultProducerReleaseReasonV1::explicit_close);
  Require(retained.receipt_handle->ObserveGrant(
              retained.grant_receipt_uuid, retained.grant_generation,
              retained.maximum_typed_result_transport_bytes_per_packet, 1) ==
              api::TypedResultProducerGrantObservationV1::stale_or_released,
          "released typed-result grant remained live");

  const auto loaded = api::LoadMgaRelationStorageDescriptor(
      consumer_context, fixture.relation_uuid);
  RequireOk(loaded, "live relation descriptor load failed");
  const auto revalidated =
      api::RevalidateNarrowQuerySourceOccurrenceAuthorityV1(
          consumed.authority, consumer_context, 0, loaded.descriptor);
  RequireOk(revalidated, "current source projection revalidation failed");

  auto wrong_context = consumer_context;
  wrong_context.maximum_mga_relation_decoded_bytes_per_pass = kScanBytes * 2u;
  const auto crossed =
      api::RevalidateNarrowQuerySourceOccurrenceAuthorityV1(
          consumed.authority, wrong_context, 0, loaded.descriptor);
  Require(!crossed.ok && crossed.stale,
          "cross-context scan-byte policy was accepted");
  const auto wrong_ordinal =
      api::RevalidateNarrowQuerySourceOccurrenceAuthorityV1(
          consumed.authority, consumer_context, 1, loaded.descriptor);
  Require(!wrong_ordinal.ok && wrong_ordinal.stale,
          "source ordinal drift was accepted");

  const auto first_full_pass = api::ObserveNarrowQueryBindingLivenessV1(
      &consumed.authority, consumer_context,
      api::EngineNarrowQueryWorkClassV1::
          mga_relation_decoded_bytes_per_pass,
      0, kScanBytes);
  RequireOk(first_full_pass, "first full decoded-byte pass was refused");
  const auto second_full_pass = api::ObserveNarrowQueryBindingLivenessV1(
      &consumed.authority, consumer_context,
      api::EngineNarrowQueryWorkClassV1::
          mga_relation_decoded_bytes_per_pass,
      0, kScanBytes);
  RequireOk(second_full_pass,
            "second full decoded-byte pass was cumulatively charged");
  const auto exceeded = api::ObserveNarrowQueryBindingLivenessV1(
      &consumed.authority, consumer_context,
      api::EngineNarrowQueryWorkClassV1::
          mga_relation_decoded_bytes_per_pass,
      0, kScanBytes + 1u);
  Require(!exceeded.ok && exceeded.resource_exhausted &&
              exceeded.diagnostic.code == "RESOURCE.BUDGET_EXCEEDED",
          "maximum plus one decoded byte did not refuse exactly");

  auto mutated = loaded.descriptor;
  ++mutated.descriptor_generation;
  const auto projection_stale =
      api::RevalidateNarrowQuerySourceOccurrenceAuthorityV1(
          consumed.authority, consumer_context, 0, mutated);
  Require(!projection_stale.ok && projection_stale.stale,
          "mutated relation projection was accepted");

  Require(!api::ReleaseNarrowQueryBindingAuthorityV1(&consumed.authority).error,
          "binding authority release failed");
  const auto released =
      api::RevalidateNarrowQuerySourceOccurrenceAuthorityV1(
          consumed.authority, consumer_context, 0, loaded.descriptor);
  Require(!released.ok && released.stale,
          "released binding handle revalidated a source");
  Rollback(fixture.transaction);
}

}  // namespace

int main() {
  TestAuthorityScanByteAndProjectionContract();
  std::cout << "PASS narrow_query_binding_authority scan_bytes=1"
            << " independent_full_passes=2 projection_revalidation=1"
            << " typed_result_packets=2 retain_once=1"
            << " atomic_publication_charge=1"
            << " released_handle_refusal=1\n";
  return EXIT_SUCCESS;
}
