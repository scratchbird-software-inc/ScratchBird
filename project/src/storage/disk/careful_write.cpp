// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "careful_write.hpp"

#include <algorithm>

namespace scratchbird::storage::disk {

// SEARCH_KEY: SB_CAREFUL_WRITE_PRECEDENCE
// Owner ruling 2026-11-05: careful write, no intent records. Reference:
// FirebirdSQL 5.0.4 src/jrd/cch.cpp (CCH_precedence, check_precedence,
// write_buffer). Firebird resolves a precedence cycle by writing the dependent
// page early, which is safe there because that buffer still holds the old
// image; every image in a plan is post-change, so a cycle is refused instead.

BoundedIoResult FileDeviceCarefulWriteSink::WriteAt(u64 offset, std::span<const byte> bytes) {
  return batch_.WriteAt(offset, bytes.data(), bytes.size());
}

BoundedIoResult FileDeviceCarefulWriteSink::ReadAt(u64 offset, std::span<byte> bytes) {
  return batch_.ReadAt(offset, bytes.data(), bytes.size());
}

BoundedIoResult FileDeviceCarefulWriteSink::Sync() {
  return batch_.Sync();
}

const char* CarefulWriteErrorName(CarefulWriteError error) noexcept {
  switch (error) {
    case CarefulWriteError::none: return "none";
    case CarefulWriteError::empty_image: return "empty_image";
    case CarefulWriteError::preimage_size_mismatch: return "preimage_size_mismatch";
    case CarefulWriteError::extent_overflow: return "extent_overflow";
    case CarefulWriteError::duplicate_offset: return "duplicate_offset";
    case CarefulWriteError::overlapping_extent: return "overlapping_extent";
    case CarefulWriteError::unknown_node: return "unknown_node";
    case CarefulWriteError::self_edge: return "self_edge";
    case CarefulWriteError::too_many_nodes: return "too_many_nodes";
    case CarefulWriteError::too_many_edges: return "too_many_edges";
    case CarefulWriteError::resource_exhausted: return "resource_exhausted";
    case CarefulWriteError::empty_plan: return "empty_plan";
    case CarefulWriteError::cycle: return "cycle";
    case CarefulWriteError::preimage_read_failed: return "preimage_read_failed";
    case CarefulWriteError::preimage_mismatch: return "preimage_mismatch";
    case CarefulWriteError::write_failed: return "write_failed";
    case CarefulWriteError::barrier_sync_failed: return "barrier_sync_failed";
    case CarefulWriteError::final_sync_failed: return "final_sync_failed";
    case CarefulWriteError::sink_exception: return "sink_exception";
  }
  return "unknown";
}

CarefulWriteError CarefulWritePlan::Refuse(CarefulWriteError error) noexcept {
  if (build_error_ == CarefulWriteError::none) build_error_ = error;
  return error;
}

void CarefulWritePlan::Truncate(usize nodes, usize arena, usize edges) noexcept {
  // Shrinking vectors of trivially copyable elements never allocates.
  nodes_.resize(nodes);
  arena_.resize(arena);
  edges_.resize(edges);
  edge_targets_.resize(edges);
  edge_begin_.resize(nodes == 0 ? 0 : nodes + 1);
  edge_cursor_.resize(nodes);
  pending_.resize(nodes);
  ready_.resize(nodes);
  order_.resize(nodes);
}

CarefulWriteAddResult CarefulWritePlan::Add(u64 byte_offset,
                                            std::span<const byte> image,
                                            std::optional<std::span<const byte>> preimage) {
  CarefulWriteAddResult result;
  const auto refuse = [&](CarefulWriteError error) {
    result.error = Refuse(error);
    return result;
  };
  if (image.empty()) return refuse(CarefulWriteError::empty_image);
  if (preimage && preimage->size() != image.size())
    return refuse(CarefulWriteError::preimage_size_mismatch);
  if (CheckFileDeviceExtentNative(byte_offset, image.size()) != BoundedIoError::none)
    return refuse(CarefulWriteError::extent_overflow);
  if (nodes_.size() >= kCarefulWriteNoNode) return refuse(CarefulWriteError::too_many_nodes);
  const u64 end = byte_offset + image.size();
  for (const auto& node : nodes_) {
    if (node.offset == byte_offset) return refuse(CarefulWriteError::duplicate_offset);
    if (byte_offset < node.offset + node.size && node.offset < end)
      return refuse(CarefulWriteError::overlapping_extent);
  }
  const usize old_nodes = nodes_.size();
  const usize old_arena = arena_.size();
  const usize old_edges = edges_.size();
  try {
    Node node;
    node.offset = byte_offset;
    node.size = image.size();
    node.image_at = arena_.size();
    arena_.insert(arena_.end(), image.begin(), image.end());
    if (preimage) {
      node.has_preimage = true;
      node.preimage_at = arena_.size();
      arena_.insert(arena_.end(), preimage->begin(), preimage->end());
      if (scratch_.size() < image.size()) scratch_.resize(image.size());
    }
    nodes_.push_back(node);
    const usize count = nodes_.size();
    edge_begin_.resize(count + 1);
    edge_cursor_.resize(count);
    pending_.resize(count);
    ready_.resize(count);
    order_.resize(count);
  } catch (...) {
    Truncate(old_nodes, old_arena, old_edges);
    return refuse(CarefulWriteError::resource_exhausted);
  }
  result.node = static_cast<CarefulWriteNodeId>(old_nodes);
  return result;
}

CarefulWriteError CarefulWritePlan::Before(CarefulWriteNodeId first, CarefulWriteNodeId then) {
  if (first >= nodes_.size() || then >= nodes_.size()) return Refuse(CarefulWriteError::unknown_node);
  if (first == then) return Refuse(CarefulWriteError::self_edge);
  // Edge indices are u32 in the validation scratch.
  if (edges_.size() >= 0xFFFFFFFFu) return Refuse(CarefulWriteError::too_many_edges);
  const usize old_edges = edges_.size();
  try {
    edges_.push_back({first, then});
    edge_targets_.resize(edges_.size());
  } catch (...) {
    Truncate(nodes_.size(), arena_.size(), old_edges);
    return Refuse(CarefulWriteError::resource_exhausted);
  }
  ++nodes_[first].successors;
  ++nodes_[then].predecessors;
  return CarefulWriteError::none;
}

CarefulWriteError CarefulWritePlan::MarkSyncBarrier(CarefulWriteNodeId node) {
  if (node >= nodes_.size()) return Refuse(CarefulWriteError::unknown_node);
  nodes_[node].sync_barrier = true;
  return CarefulWriteError::none;
}

bool CarefulWritePlan::Reserve(usize nodes, usize edges, usize image_bytes) noexcept {
  try {
    nodes_.reserve(nodes);
    edges_.reserve(edges);
    edge_targets_.reserve(edges);
    arena_.reserve(image_bytes);
    edge_begin_.reserve(nodes + 1);
    edge_cursor_.reserve(nodes);
    pending_.reserve(nodes);
    ready_.reserve(nodes);
    order_.reserve(nodes);
    return true;
  } catch (...) {
    return false;
  }
}

void CarefulWritePlan::Clear() noexcept {
  nodes_.clear();
  edges_.clear();
  arena_.clear();
  scratch_.clear();
  edge_begin_.clear();
  edge_cursor_.clear();
  edge_targets_.clear();
  pending_.clear();
  ready_.clear();
  order_.clear();
  build_error_ = CarefulWriteError::none;
}

u64 CarefulWritePlan::offset(CarefulWriteNodeId node) const noexcept {
  return node < nodes_.size() ? nodes_[node].offset : ~u64{0};
}

CarefulWriteOrderResult CarefulWritePlan::Validate() noexcept {
  CarefulWriteOrderResult result;
  if (build_error_ != CarefulWriteError::none) {
    result.error = build_error_;
    return result;
  }
  const usize count = nodes_.size();
  if (count == 0) {
    result.error = CarefulWriteError::empty_plan;
    return result;
  }
  // Successor lists (compressed rows) from the edge list.
  std::fill(edge_begin_.begin(), edge_begin_.end(), 0u);
  for (const auto& edge : edges_) ++edge_begin_[edge.first + 1];
  for (usize i = 0; i < count; ++i) edge_begin_[i + 1] += edge_begin_[i];
  std::copy(edge_begin_.begin(), edge_begin_.begin() + static_cast<std::ptrdiff_t>(count),
            edge_cursor_.begin());
  for (const auto& edge : edges_) edge_targets_[edge_cursor_[edge.first]++] = edge.then;
  // Kahn's algorithm; the ready set is a heap ordered by ascending offset, so
  // ties among ready nodes always issue in offset order.
  const auto later = [this](CarefulWriteNodeId a, CarefulWriteNodeId b) {
    return nodes_[a].offset > nodes_[b].offset;
  };
  CarefulWriteNodeId* const heap = ready_.data();
  usize ready = 0;
  for (usize i = 0; i < count; ++i) {
    pending_[i] = nodes_[i].predecessors;
    if (pending_[i] == 0) {
      heap[ready++] = static_cast<CarefulWriteNodeId>(i);
      std::push_heap(heap, heap + ready, later);
    }
  }
  usize emitted = 0;
  while (ready != 0) {
    std::pop_heap(heap, heap + ready, later);
    const CarefulWriteNodeId id = heap[--ready];
    order_[emitted++] = id;
    for (u32 at = edge_begin_[id]; at < edge_begin_[id + 1]; ++at) {
      const CarefulWriteNodeId next = edge_targets_[at];
      if (--pending_[next] == 0) {
        heap[ready++] = next;
        std::push_heap(heap, heap + ready, later);
      }
    }
  }
  if (emitted != count) {
    result.error = CarefulWriteError::cycle;
    return result;
  }
  result.order = std::span<const CarefulWriteNodeId>(order_.data(), count);
  return result;
}

CarefulWriteResult CarefulWritePlan::Execute(CarefulWriteSink& sink,
                                             CarefulWriteBarrierPolicy policy) noexcept {
  CarefulWriteResult result;
  const auto validated = Validate();
  if (!validated.ok()) {
    result.error = validated.error;
    return result;
  }
  const usize count = nodes_.size();
  usize completed = 0;
  CarefulWriteNodeId current = kCarefulWriteNoNode;
  const auto stop = [&](CarefulWriteError error, const BoundedIoResult& io) {
    result.error = error;
    result.failed_node = current;
    result.io = io;
  };
  try {
    const bool elide = policy == CarefulWriteBarrierPolicy::trust_forced_writes &&
                       sink.forced_ordered();
    for (usize i = 0; i < count; ++i) {
      current = order_[i];
      const Node& node = nodes_[current];
      const std::span<const byte> image(arena_.data() + node.image_at, node.size);
      bool skip = false;
      if (node.has_preimage) {
        const std::span<byte> found(scratch_.data(), node.size);
        const auto io = sink.ReadAt(node.offset, found);
        if (!io.ok() || io.bytes_transferred != node.size) {
          stop(CarefulWriteError::preimage_read_failed, io);
          break;
        }
        if (std::equal(found.begin(), found.end(), image.begin())) {
          skip = true;
        } else if (!std::equal(found.begin(), found.end(), arena_.data() + node.preimage_at)) {
          stop(CarefulWriteError::preimage_mismatch, io);
          break;
        }
      }
      if (skip) {
        ++result.skipped;
      } else {
        const auto io = sink.WriteAt(node.offset, image);
        if (!io.ok() || io.bytes_transferred != node.size) {
          stop(CarefulWriteError::write_failed, io);
          break;
        }
        ++result.written;
      }
      // A skipped node's bytes may come from an earlier attempt that never
      // reached a Sync, so its successors wait for one whatever the policy.
      if (node.successors != 0 && (!elide || skip || node.sync_barrier)) {
        ++result.syncs;
        const auto io = sink.Sync();
        if (!io.ok()) {
          stop(CarefulWriteError::barrier_sync_failed, io);
          break;
        }
      }
      completed = i + 1;
    }
    if (result.ok()) {
      current = kCarefulWriteNoNode;
      ++result.syncs;
      const auto io = sink.Sync();
      if (!io.ok()) stop(CarefulWriteError::final_sync_failed, io);
    }
  } catch (...) {
    result.error = CarefulWriteError::sink_exception;
    result.failed_node = current;
  }
  result.completed = std::span<const CarefulWriteNodeId>(order_.data(), completed);
  return result;
}

}  // namespace scratchbird::storage::disk
