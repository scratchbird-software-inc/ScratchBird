#pragma once

#include "api_types.hpp"
#include "sblr_bulk_import_stream_registry.hpp"

#include <cstdint>
#include <string>

namespace scratchbird::engine::internal_api {

// Bridge-neutral immutable authority copied from the engine-owned statement
// receipt.  Server/session/receipt handle types deliberately do not cross this
// boundary.
struct SblrBulkImportStreamAuthorityInputV1 {
  engine::sblr::BulkImportUuid authenticated_receipt_uuid{};
  std::string admitted_command_surface_id;
  engine::sblr::BulkImportUuid binding_uuid{};
  std::uint64_t binding_generation = 0;
  std::uint64_t structural_occurrence = 0;
  std::uint32_t import_occurrence = 0;
  engine::sblr::BulkImportSha syntax_demand_sha256{};
  engine::sblr::BulkImportSha binding_evidence_sha256{};

  engine::sblr::BulkImportUuid target_relation_uuid{};
  std::uint64_t target_relation_generation = 0;
  engine::sblr::BulkImportUuid owning_transaction_uuid{};
  std::uint64_t owning_local_transaction_id = 0;
  engine::sblr::BulkImportUuid statement_snapshot_uuid{};
  engine::sblr::BulkImportUuid catalog_epoch_uuid{};
  std::uint64_t catalog_generation = 0;
  engine::sblr::BulkImportUuid security_context_uuid{};
  std::uint64_t security_epoch = 0;
  engine::sblr::BulkImportUuid policy_snapshot_uuid{};
  std::uint64_t policy_generation = 0;
  engine::sblr::BulkImportSha import_policy_bundle_sha256{};
  engine::sblr::BulkImportUuid route_snapshot_uuid{};
  std::uint64_t route_generation = 0;
  engine::sblr::BulkImportUuid row_shape_uuid{};
  std::uint64_t row_shape_generation = 0;
  engine::sblr::BulkImportSha column_descriptor_set_sha256{};
  engine::sblr::BulkImportUuid resource_grant_uuid{};
  std::uint64_t resource_grant_generation = 0;
  bool cluster_bound = false;
  std::uint64_t cluster_epoch = 0;
  engine::sblr::BulkImportUuid cluster_fence_uuid{};
  std::uint64_t executor_availability_generation = 0;
  std::uint64_t effective_maximum_stream_bytes = 0;
  std::uint64_t effective_maximum_chunk_count = 0;
  std::uint32_t effective_maximum_chunk_bytes = 0;
  std::uint64_t effective_maximum_rows = 0;
  std::uint32_t effective_maximum_target_columns = 0;
};

struct SblrBulkImportStreamCoordinationResult {
  bool ok = false;
  bool replayed = false;
  engine::sblr::SblrBulkImportStreamDescriptorV1 descriptor{};
  BulkImportStreamAllocation allocation{};
  EngineApiDiagnostic diagnostic;
};

SblrBulkImportStreamCoordinationResult
CoordinateDurableSblrBulkImportStreamDescriptorV1(
    const EngineRequestContext&,
    SblrBulkImportStreamRegistry&,
    const SblrBulkImportStreamAuthorityInputV1&);

// Legacy private coordinator entry points remain temporarily declared while
// the terminal opcode-775 executor is migrated to the durable registry.  New
// BIRQ coordination must use CoordinateDurable... above.
SblrBulkImportStreamCoordinationResult CompileSblrBulkImportStreamDescriptor(
    const EngineRequestContext&, const std::string&, std::uint64_t,
    std::uint32_t, std::uint64_t);
SblrBulkImportStreamCoordinationResult ConsumeSblrBulkImportStreamDescriptor(
    const EngineRequestContext&,
    const engine::sblr::SblrBulkImportStreamDescriptorV1&);

}  // namespace scratchbird::engine::internal_api
