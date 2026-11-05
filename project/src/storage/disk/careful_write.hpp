// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "disk_device.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace scratchbird::storage::disk {

using scratchbird::core::platform::byte;

// SEARCH_KEY: SB_CAREFUL_WRITE_PRECEDENCE
// Owner ruling 2026-11-05: careful write, no intent records. A page is durable
// before any page that points to it (FirebirdSQL CCH_precedence semantics),
// so after a crash the durable pages are a dependency-closed prefix of the
// plan: the old state, the new state, or the old state plus orphans.
//
// This is the generic ordering primitive only. It orders write issue and
// durability barriers; it does not decide which in-place page updates are
// admissible, and it does not detect a write torn inside one page.

// Where a careful-write plan sends its I/O. Every read/write receipt reports
// the full transfer in bytes_transferred on success; a short success is an
// error.
class CarefulWriteSink {
 public:
  virtual ~CarefulWriteSink() = default;
  virtual BoundedIoResult WriteAt(u64 offset, std::span<const byte> bytes) = 0;
  virtual BoundedIoResult ReadAt(u64 offset, std::span<byte> bytes) = 0;
  virtual BoundedIoResult Sync() = 0;
  // True only when each WriteAt is durable on return, so issue order is
  // durable order without an intervening Sync.
  virtual bool forced_ordered() const noexcept = 0;
};

// A sink over an open FileDevice. It uses the device's fixed-receipt batch, so
// no diagnostic rendering, heap allocation or telemetry publication happens
// during I/O; latency observations publish when the sink is destroyed.
// Lifetime rule of FileDevice::BoundedIoBatch: declare the sink before ALL
// enclosing device/source guards, and let the device outlive it. Each sink
// call takes the device operation guard only for that call; a caller that
// needs the whole plan exclusive holds device.AcquireOperationGuard(),
// declared after the sink, across Execute. The batch keeps 16 latency
// observations: a plan of more sink calls (about 8 nodes with preimages)
// loses the excess, which the device counts in
// rejected_io_latency_observations(); physical effects are unchanged.
class FileDeviceCarefulWriteSink final : public CarefulWriteSink {
 public:
  explicit FileDeviceCarefulWriteSink(FileDevice& device) noexcept
      : device_(device), batch_(device) {}
  FileDeviceCarefulWriteSink(const FileDeviceCarefulWriteSink&) = delete;
  FileDeviceCarefulWriteSink& operator=(const FileDeviceCarefulWriteSink&) = delete;
  BoundedIoResult WriteAt(u64 offset, std::span<const byte> bytes) override;
  BoundedIoResult ReadAt(u64 offset, std::span<byte> bytes) override;
  BoundedIoResult Sync() override;
  bool forced_ordered() const noexcept override { return device_.forced_ordered_writes(); }
  FileDevice& device() const noexcept { return device_; }

 private:
  FileDevice& device_;
  FileDevice::BoundedIoBatch batch_;
};

using CarefulWriteNodeId = u32;
inline constexpr CarefulWriteNodeId kCarefulWriteNoNode = 0xFFFFFFFFu;

enum class CarefulWriteError : std::uint8_t {
  none,
  // Refused while building or validating the plan; no I/O was issued.
  empty_image,
  preimage_size_mismatch,
  extent_overflow,
  duplicate_offset,
  overlapping_extent,
  unknown_node,
  self_edge,
  too_many_nodes,
  too_many_edges,
  resource_exhausted,
  empty_plan,
  cycle,
  // Stopped during execution; no node after failed_node was issued.
  preimage_read_failed,
  preimage_mismatch,
  write_failed,
  barrier_sync_failed,
  final_sync_failed,
  sink_exception,
};
const char* CarefulWriteErrorName(CarefulWriteError error) noexcept;

enum class CarefulWriteBarrierPolicy : std::uint8_t {
  // When the sink reports forced ordered writes, issue no Sync between nodes
  // (except after a sync-barrier node or a skipped node that has successors).
  trust_forced_writes,
  // Sync after every node that has successors, whatever the sink reports.
  sync_every_dependency,
};

struct CarefulWriteAddResult {
  CarefulWriteError error = CarefulWriteError::none;
  CarefulWriteNodeId node = kCarefulWriteNoNode;
  bool ok() const noexcept { return error == CarefulWriteError::none; }
};

struct CarefulWriteOrderResult {
  CarefulWriteError error = CarefulWriteError::none;
  // Borrowed from the plan; valid until the plan is next changed or run.
  std::span<const CarefulWriteNodeId> order;
  bool ok() const noexcept { return error == CarefulWriteError::none; }
};

struct CarefulWriteResult {
  CarefulWriteError error = CarefulWriteError::none;
  // The node being processed when execution stopped; kCarefulWriteNoNode for
  // a validation refusal or a failed final Sync.
  CarefulWriteNodeId failed_node = kCarefulWriteNoNode;
  // Nodes whose image is on the sink (written, or found already current) and
  // whose required barrier completed, in issue order. Borrowed from the plan;
  // valid until the plan is next changed or run. After a failed final Sync it
  // lists every node, but durability after the last successful Sync is unknown.
  std::span<const CarefulWriteNodeId> completed;
  u32 written = 0;   // WriteAt calls that succeeded
  u32 skipped = 0;   // nodes whose on-sink bytes already equalled the image
  u32 syncs = 0;     // Sync calls issued, including a failed one
  BoundedIoResult io{};  // receipt of the failing sink call, if any
  bool ok() const noexcept { return error == CarefulWriteError::none; }
};

// A set of page images and the order in which they must become durable.
// Nodes are byte extents (offset, image), so any page size works. Images and
// preimages are copied into the plan; every buffer Execute needs is sized
// while the plan is built, so Validate and Execute do not allocate.
//
// Any refused Add/Before/MarkSyncBarrier poisons the plan: Validate and
// Execute then return that first refusal without I/O. Clear() resets it.
// Not thread-safe; one thread builds and runs a plan.
class CarefulWritePlan {
 public:
  CarefulWritePlan() = default;
  CarefulWritePlan(const CarefulWritePlan&) = delete;
  CarefulWritePlan& operator=(const CarefulWritePlan&) = delete;

  // One node per offset; extents may not overlap. A preimage, when given, is
  // the expected on-sink bytes before this plan and must match the image size.
  CarefulWriteAddResult Add(u64 byte_offset,
                            std::span<const byte> image,
                            std::optional<std::span<const byte>> preimage = std::nullopt);
  // `first` must be durable before `then` is issued. Repeating an edge is
  // harmless (the order constraint is the same).
  CarefulWriteError Before(CarefulWriteNodeId first, CarefulWriteNodeId then);
  // Force a Sync after `node` before any of its successors, even with forced
  // ordered writes (for example an extent that O_DSYNC does not cover).
  CarefulWriteError MarkSyncBarrier(CarefulWriteNodeId node);
  // Optional capacity hint (image_bytes counts images plus preimages).
  // False if the reservation could not be made; the plan is unchanged.
  bool Reserve(usize nodes, usize edges, usize image_bytes) noexcept;
  void Clear() noexcept;

  // Topological order, ties broken by ascending offset. Refuses a poisoned
  // plan, an empty plan or a cycle. No I/O.
  CarefulWriteOrderResult Validate() noexcept;
  // Validate, then issue each node in order:
  //  - with a preimage, read the extent: equal to the image -> skip (a retry
  //    is safe); equal to the preimage -> write; otherwise refuse;
  //  - Sync after a node that has successors unless the policy is
  //    trust_forced_writes and the sink is forced; a sync-barrier node or a
  //    skipped node (its bytes may come from an unsynced earlier attempt)
  //    always gets that Sync;
  //  - one final Sync after the last node.
  // Stops at the first error: nothing later is issued, including the final
  // Sync, and the result names the failed node.
  CarefulWriteResult Execute(CarefulWriteSink& sink,
                             CarefulWriteBarrierPolicy policy =
                                 CarefulWriteBarrierPolicy::trust_forced_writes) noexcept;

  usize node_count() const noexcept { return nodes_.size(); }
  usize edge_count() const noexcept { return edges_.size(); }
  CarefulWriteError build_error() const noexcept { return build_error_; }
  // Byte offset of a node, or u64 max for an unknown id.
  u64 offset(CarefulWriteNodeId node) const noexcept;

 private:
  struct Node {
    u64 offset = 0;
    usize size = 0;
    usize image_at = 0;
    usize preimage_at = 0;
    u32 predecessors = 0;
    u32 successors = 0;
    bool has_preimage = false;
    bool sync_barrier = false;
  };
  struct Edge {
    CarefulWriteNodeId first = 0;
    CarefulWriteNodeId then = 0;
  };
  CarefulWriteError Refuse(CarefulWriteError error) noexcept;
  void Truncate(usize nodes, usize arena, usize edges) noexcept;

  std::vector<Node> nodes_;
  std::vector<Edge> edges_;
  std::vector<byte> arena_;       // images and preimages
  std::vector<byte> scratch_;     // preimage read buffer (largest image with one)
  // Validation scratch, sized while building.
  std::vector<u32> edge_begin_;   // nodes + 1
  std::vector<u32> edge_cursor_;  // nodes
  std::vector<CarefulWriteNodeId> edge_targets_;  // edges
  std::vector<u32> pending_;      // nodes
  std::vector<CarefulWriteNodeId> ready_;  // nodes (binary heap by offset)
  std::vector<CarefulWriteNodeId> order_;  // nodes
  CarefulWriteError build_error_ = CarefulWriteError::none;
};

}  // namespace scratchbird::storage::disk
