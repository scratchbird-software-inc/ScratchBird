// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "careful_chain_support.hpp"

#include <limits>

namespace scratchbird::storage::page::detail {
namespace {
namespace disk = scratchbird::storage::disk;
using E = TransactionWriterError;
using disk::CarefulWriteError;
}  // namespace

bool ReadChainPage(disk::CarefulWriteSink& sink, u32 page_size, u64 number, std::vector<byte>& out,
                   TransactionWriterOutcome& result) {
  if (number == 0 || number > std::numeric_limits<u64>::max() / page_size) {
    result.error = E::invalid_inventory;
    result.page_error = TipPageError::invalid_link;
    return false;
  }
  const auto at = out.size();
  out.resize(at + page_size);
  const auto io = sink.ReadAt(number * page_size, std::span<byte>(out).subspan(at, page_size));
  if (!io.ok() || io.bytes_transferred != page_size) {
    result.error = E::io_failure;
    result.io = io;
    return false;
  }
  return true;
}

disk::CarefulWriteAddResult AddChainUnit(disk::CarefulWritePlan& plan, u64 page_number, u32 page_size,
                                         std::span<const byte> image, const TipPageEdit& edit) {
  return plan.Add(page_number * page_size + edit.unit_offset,
                  image.subspan(edit.unit_offset, kTransactionStateWriteUnitBytes),
                  std::span<const byte>(edit.preimage));
}

bool RunChainPlan(disk::CarefulWritePlan& plan, disk::CarefulWriteSink& sink, disk::CarefulWriteBarrierPolicy policy,
                  TransactionWriterOutcome& result, bool& failed, disk::CarefulWriteNodeId* failed_node) {
  const auto r = plan.Execute(sink, policy);
  if (failed_node) *failed_node = r.failed_node;
  result.write_error = r.error;
  result.io = r.io;
  result.writes = r.written;
  result.syncs = r.syncs;
  if (r.ok()) {
    result.error = E::none;
    return true;
  }
  const bool attempted = r.written > 0 || r.syncs > 0 || r.error == CarefulWriteError::write_failed ||
                         r.error == CarefulWriteError::barrier_sync_failed ||
                         r.error == CarefulWriteError::final_sync_failed ||
                         r.error == CarefulWriteError::sink_exception;
  if (attempted) {
    failed = true;
    result.error = E::write_failed;
  } else if (r.error == CarefulWriteError::preimage_read_failed) {
    result.error = E::io_failure;
  } else if (r.error == CarefulWriteError::preimage_mismatch) {
    result.error = E::invalid_inventory;  // the file changed after it was read
  } else {
    result.error = E::resource_exhausted;
  }
  return false;
}

bool GrowChainByOnePage(disk::CarefulWriteSink& sink, const ChainGrowthSpec& spec, ChainGrowthOutcome& result,
                        bool& failed) {
  const u32 page_size = spec.page_size;
  if (!spec.issuer || !spec.format || !spec.link) {
    result.error = E::invalid_request;
    return false;
  }
  auto loaded = AllocationMapChain::Load(sink, {spec.database_uuid, spec.allocation_root});
  if (!loaded.ok()) {
    result.allocation_error = loaded.error;
    result.error = loaded.error == AllocationWriteError::io_failure ? E::io_failure : E::invalid_inventory;
    result.io = loaded.io;
    return false;
  }
  const auto page_id = spec.issuer->Issue(scratchbird::core::platform::UuidKind::page);
  const auto allocation_id = spec.issuer->Issue(scratchbird::core::platform::UuidKind::object);
  if (!page_id.ok() || !allocation_id.ok()) {
    result.error = E::resource_exhausted;
    return false;
  }
  disk::CarefulWritePlan plan;
  const PageAllocationRequest request{spec.page_type, spec.owner_object, page_id.value->value,
                                      allocation_id.value->value, {}, 0};
  const auto planned = loaded.chain->PlanAllocate(plan, request);
  if (!planned.ok()) {
    result.allocation_error = planned.error;
    result.error = planned.error == AllocationWriteError::filespace_full ? E::chain_full : E::invalid_inventory;
    if (planned.error == AllocationWriteError::filespace_full) result.page_error = TipPageError::invalid_slot;
    return false;
  }
  // The new page, formatted empty; nothing reaches it until the link.
  std::vector<byte> image(page_size);
  const disk::NativeCommonPageHeader header{page_size, spec.page_type, spec.database_uuid,
                                            spec.filespace_uuid, page_id.value->value,
                                            planned.page_number, 1, 0, spec.page_size_profile_uuid};
  const TipPageError formatted = spec.format(header, image);
  if (formatted != TipPageError::none) {
    result.error = E::invalid_inventory;
    result.page_error = formatted;
    return false;
  }
  const auto link = spec.link(spec.last_image, planned.page_number);
  if (!link.ok() || !link.changed) {
    result.error = E::invalid_inventory;
    result.page_error = link.ok() ? TipPageError::invalid_link : link.error;
    return false;
  }
  const auto page_node = plan.Add(planned.page_number * page_size, image);
  const auto link_node = AddChainUnit(plan, spec.last_page_number, page_size, spec.last_image, link);
  if (!page_node.ok() || !link_node.ok() || plan.Before(planned.last_node, page_node.node) != CarefulWriteError::none ||
      plan.Before(page_node.node, link_node.node) != CarefulWriteError::none) {
    result.error = E::resource_exhausted;
    return false;
  }
  // Owner Q76 = 1: a record sharing the state's map unit is written, durable,
  // by its own plan first.
  bool record_written = false;
  if (planned.record_first()) {
    disk::CarefulWritePlan first;
    if (!PlanAllocationRecordFirst(first, planned).ok()) {
      result.error = E::resource_exhausted;
      return false;
    }
    TransactionWriterOutcome record;
    const bool wrote = RunChainPlan(first, sink, spec.barrier_policy, record, failed);
    result.growth_writes += record.writes;
    result.growth_syncs += record.syncs;
    record_written = record.writes > 0;
    if (!wrote) {
      result.error = record.error;
      result.write_error = record.write_error;
      result.io = record.io;
      return false;
    }
  }
  TransactionWriterOutcome growth;
  bool ran = RunChainPlan(plan, sink, spec.barrier_policy, growth, failed);
  if (!ran && record_written && !failed) {
    // The record is already on the file: the allocation was attempted.
    failed = true;
    growth.error = E::write_failed;
  }
  result.growth_writes += growth.writes;
  result.growth_syncs += growth.syncs;
  if (!ran) {
    result.error = growth.error;
    result.write_error = growth.write_error;
    result.io = growth.io;
    return false;
  }
  return true;
}

}  // namespace scratchbird::storage::page::detail
