// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "row_version.hpp"
#include "transaction_inventory.hpp"
#include <array>
#include <optional>
#include <vector>

namespace scratchbird::transaction::mga {
using scratchbird::core::platform::u32;
using ContentGenerationUuid = std::array<scratchbird::core::platform::byte, 16>;

// Native catalog projection, not a durable format or executable descriptor.
// Producers must authenticate complete catalog batches and rollback evidence.
struct TableContentGenerationVersion {
  ContentGenerationUuid database_uuid{}, schema_uuid{}, table_uuid{}, catalog_row_uuid{};
  ContentGenerationUuid generation_uuid{}, predecessor_generation_uuid{};
  ContentGenerationUuid root_set_uuid{}, statistics_generation_uuid{}, batch_uuid{};
  ContentGenerationUuid creator_transaction_uuid{};
  u64 creator_local_transaction_id = 0;
  u64 publication_effect_sequence = 0;
  u64 descriptor_generation = 0;
  u32 batch_ordinal = 0;
  u32 batch_target_count = 0;
};
struct TableContentGenerationRollbackInterval {
  ContentGenerationUuid rollback_record_uuid{}, creator_transaction_uuid{};
  u64 creator_local_transaction_id = 0;
  u64 effect_sequence_lower_exclusive = 0;
  u64 effect_sequence_upper_inclusive = 0;
};
struct TableContentGenerationReadContext {
  ContentGenerationUuid database_uuid{}, table_uuid{}, reader_transaction_uuid{};
  VisibilitySnapshot snapshot;
};
enum class TableContentGenerationSelectionStatus {
  selected,
  not_visible,
  invalid_request,
  corrupt_history,
  inventory_required,
  recovery_required,
  cluster_authority_required,
  resource_exhausted
};
struct TableContentGenerationSelection {
  TableContentGenerationSelectionStatus status =
      TableContentGenerationSelectionStatus::invalid_request;
  std::optional<TableContentGenerationVersion> binding;
  // An older readable generation is not permission for a conflicting write.
  std::vector<TransactionIdentity> pending_creators;
};

// Visibility is decided by actual MGA inventory, never UUID or sequence order.
// Failure returns no partial binding or pending-creator list. Root resolution,
// durable publication, authorization and mutation barriers belong to consumers.
TableContentGenerationSelection ResolveTableContentGeneration(
    const TableContentGenerationReadContext& context,
    const std::vector<TableContentGenerationVersion>& history,
    const std::vector<TableContentGenerationRollbackInterval>& rollbacks,
    const LocalTransactionInventory& inventory) noexcept;
}  // namespace scratchbird::transaction::mga
