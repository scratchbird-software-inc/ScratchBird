// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "api_types.hpp"
#include <array>
#include <cstdint>
#include <vector>
namespace scratchbird::engine::internal_api {
// SEARCH_KEY: SB_MGA_BULK_IMPORT_PUBLICATION_V1
// Transaction-local durable publication authority for an opcode-775 mutation.
// This record proves statement publication inside the owning MGA transaction;
// it is not transaction commit or cross-session visibility evidence.
using MgaBulkImportSha256V1 = std::array<std::uint8_t, 32>;

enum class MgaBulkImportPublicationLifecycleV1 : std::uint8_t {
  prepared = 1,
  published_uncommitted = 2,
  aborted = 3,
};

struct MgaBulkImportPublicationRecordV1 {
  MgaBulkImportPublicationLifecycleV1 lifecycle =
      MgaBulkImportPublicationLifecycleV1::prepared;
  EngineUuid durable_publication_uuid;
  std::uint64_t durable_publication_generation = 0;
  MgaBulkImportSha256V1 recovery_idempotency_key{};
  EngineUuid stream_uuid;
  std::uint64_t stream_generation = 0;
  MgaBulkImportSha256V1 descriptor_evidence{};
  EngineUuid target_relation_uuid;
  std::uint64_t target_relation_generation = 0;
  EngineUuid owning_transaction_uuid;
  std::uint64_t owning_local_transaction_id = 0;
  EngineUuid authenticated_receipt_uuid;
  EngineUuid statement_uuid;
  std::uint64_t savepoint_ordinal = 0;
  EngineUuid mutation_uuid;
  EngineUuid bulk_batch_uuid;
  MgaBulkImportSha256V1 content_sha256{};
  std::uint64_t total_stream_bytes = 0;
  std::uint64_t chunk_count = 0;
  std::uint64_t input_row_count = 0;
  std::uint64_t affected_rows = 0;
  std::uint64_t rejected_rows = 0;
  std::uint64_t imported_row_postcondition_count = 0;
  MgaBulkImportSha256V1 imported_row_postcondition_sha256{};
  MgaBulkImportSha256V1 normalized_statement_effect_sha256{};
  MgaBulkImportSha256V1 column_descriptor_set_sha256{};
  MgaBulkImportSha256V1 import_policy_bundle_sha256{};
  MgaBulkImportSha256V1 default_descriptor_set_sha256{};
  MgaBulkImportSha256V1 constraint_set_sha256{};
  MgaBulkImportSha256V1 trigger_set_sha256{};
  MgaBulkImportSha256V1 index_set_sha256{};
  std::uint64_t executor_availability_generation = 0;
  MgaBulkImportSha256V1 record_evidence_sha256{};

  bool operator==(const MgaBulkImportPublicationRecordV1&) const = default;
};

struct MgaBulkImportImportedRowEventV1 {
  EngineUuid durable_publication_uuid;
  std::uint64_t durable_publication_generation = 0;
  MgaBulkImportSha256V1 recovery_idempotency_key{};
  EngineUuid mutation_uuid;
  EngineUuid bulk_batch_uuid;
  EngineUuid owning_transaction_uuid;
  std::uint64_t owning_local_transaction_id = 0;
  EngineUuid statement_uuid;
  std::uint64_t savepoint_ordinal = 0;
  EngineUuid target_relation_uuid;
  std::uint64_t target_relation_generation = 0;
  std::uint64_t import_ordinal = 0;
  EngineUuid row_uuid;
  EngineUuid row_version_uuid;
  EngineUuid row_image_uuid;
  std::uint64_t row_image_metadata_generation = 0;
  MgaBulkImportSha256V1 row_image_domain_hash{};
  MgaBulkImportSha256V1 row_image_value_hash{};
  MgaBulkImportSha256V1 column_descriptor_set_sha256{};
  MgaBulkImportSha256V1 canonical_typed_field_vector_sha256{};
  MgaBulkImportSha256V1 event_evidence_sha256{};

  bool operator==(const MgaBulkImportImportedRowEventV1&) const = default;
};

struct MgaBulkImportImportedRowEventResultV1 {
  bool ok = false;
  bool replayed = false;
  EngineApiDiagnostic diagnostic;
  std::vector<MgaBulkImportImportedRowEventV1> events;
};

struct MgaBulkImportPublicationResultV1 {
  bool ok = false;
  bool found = false;
  bool replayed = false;
  EngineApiDiagnostic diagnostic;
  MgaBulkImportPublicationRecordV1 record;
};
MgaBulkImportPublicationResultV1 PrepareMgaBulkImportPublicationV1(
    const EngineRequestContext&, const MgaBulkImportPublicationRecordV1&);
MgaBulkImportPublicationResultV1 PublishMgaBulkImportPublicationV1(
    const EngineRequestContext&, const MgaBulkImportPublicationRecordV1&);
MgaBulkImportPublicationResultV1 AbortMgaBulkImportPublicationV1(
    const EngineRequestContext&, const MgaBulkImportPublicationRecordV1&);
MgaBulkImportPublicationResultV1 RecoverMgaBulkImportPublicationV1(
    const EngineRequestContext&, const MgaBulkImportSha256V1&);
MgaBulkImportImportedRowEventResultV1 StoreMgaBulkImportImportedRowEventsV1(
    const EngineRequestContext&, const std::vector<MgaBulkImportImportedRowEventV1>&);
MgaBulkImportImportedRowEventResultV1 RecoverMgaBulkImportImportedRowEventsV1(
    const EngineRequestContext&, const MgaBulkImportSha256V1&);
} // namespace scratchbird::engine::internal_api
