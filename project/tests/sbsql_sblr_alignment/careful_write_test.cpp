// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
//
// SEARCH_KEY: SB_CAREFUL_WRITE_PRECEDENCE
// Owner ruling 2026-11-05: careful write, no intent records. Unit coverage of
// the generic careful-write primitive: plan refusals, deterministic order,
// barrier placement under both policies, preimage write/skip/refuse, and
// first-error stop. A recording sink holds an in-memory medium and logs every
// sink call, so issue order and barrier placement are asserted exactly.
#include "careful_write.hpp"
#include "disk_device.hpp"
#include "../support/owned_temp_directory.hpp"

#include <cerrno>
#include <cstring>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace disk = scratchbird::storage::disk;
using disk::BoundedIoError;
using disk::BoundedIoOperation;
using disk::BoundedIoResult;
using disk::CarefulWriteBarrierPolicy;
using disk::CarefulWriteError;
using disk::CarefulWriteNodeId;
using disk::CarefulWritePlan;
using disk::kCarefulWriteNoNode;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::u64;
using scratchbird::core::platform::usize;

namespace {

unsigned checks = 0;
void Check(bool condition, const std::string& message) {
  ++checks;
  if (!condition) throw std::runtime_error(message);
}
void CheckError(CarefulWriteError actual, CarefulWriteError expected, const std::string& message) {
  Check(actual == expected, message + ": expected " + disk::CarefulWriteErrorName(expected) +
                                ", got " + disk::CarefulWriteErrorName(actual));
}

std::vector<byte> Bytes(usize size, byte fill) { return std::vector<byte>(size, fill); }
std::span<const byte> View(const std::vector<byte>& bytes) { return bytes; }

// In-memory medium plus an exact log of sink calls ("R<offset>", "W<offset>", "S").
class RecordingSink final : public disk::CarefulWriteSink {
 public:
  explicit RecordingSink(bool forced) : forced_(forced) {}
  enum class Fault { none, error, short_transfer, exception };

  BoundedIoResult WriteAt(u64 offset, std::span<const byte> bytes) override {
    const auto fault = Next("W" + std::to_string(offset));
    BoundedIoResult r = Receipt(BoundedIoOperation::write, offset, bytes.size());
    if (Fail(fault, r)) return r;
    if (medium.size() < offset + bytes.size()) medium.resize(offset + bytes.size(), 0);
    std::memcpy(medium.data() + offset, bytes.data(), bytes.size());
    r.bytes_transferred = bytes.size();
    return r;
  }
  BoundedIoResult ReadAt(u64 offset, std::span<byte> bytes) override {
    const auto fault = Next("R" + std::to_string(offset));
    BoundedIoResult r = Receipt(BoundedIoOperation::read, offset, bytes.size());
    if (Fail(fault, r)) return r;
    if (medium.size() < offset + bytes.size()) {
      r.error = BoundedIoError::short_transfer;
      return r;
    }
    std::memcpy(bytes.data(), medium.data() + offset, bytes.size());
    r.bytes_transferred = bytes.size();
    return r;
  }
  BoundedIoResult Sync() override {
    const auto fault = Next("S");
    BoundedIoResult r = Receipt(BoundedIoOperation::sync, 0, 0);
    if (Fail(fault, r)) return r;
    r.synchronized = true;
    return r;
  }
  bool forced_ordered() const noexcept override { return forced_; }

  // Fail the call with this zero-based index.
  void FailCall(usize index, Fault fault) {
    fail_index_ = index;
    fault_ = fault;
  }
  std::string Log() const {
    std::string out;
    for (const auto& op : ops) out += (out.empty() ? "" : " ") + op;
    return out;
  }
  void Place(u64 offset, const std::vector<byte>& bytes) {
    if (medium.size() < offset + bytes.size()) medium.resize(offset + bytes.size(), 0);
    std::memcpy(medium.data() + offset, bytes.data(), bytes.size());
  }
  std::vector<byte> At(u64 offset, usize size) const {
    return {medium.begin() + static_cast<std::ptrdiff_t>(offset),
            medium.begin() + static_cast<std::ptrdiff_t>(offset + size)};
  }
  void ResetLog() {
    ops.clear();
    fault_ = Fault::none;
  }

  std::vector<byte> medium;
  std::vector<std::string> ops;

 private:
  static BoundedIoResult Receipt(BoundedIoOperation operation, u64 offset, usize bytes) {
    BoundedIoResult r;
    r.operation = operation;
    r.offset = offset;
    r.requested_bytes = bytes;
    r.native_attempted = true;
    return r;
  }
  Fault Next(std::string op) {
    const usize index = ops.size();
    ops.push_back(std::move(op));
    return index == fail_index_ ? fault_ : Fault::none;
  }
  static bool Fail(Fault fault, BoundedIoResult& r) {
    switch (fault) {
      case Fault::none: return false;
      case Fault::error:
        r.error = BoundedIoError::native_failure;
        r.native_error = EIO;
        return true;
      case Fault::short_transfer:
        // A "successful" receipt that moved fewer bytes than requested.
        r.bytes_transferred = r.requested_bytes ? r.requested_bytes - 1 : 0;
        return true;
      case Fault::exception: throw std::runtime_error("recording sink fault");
    }
    return false;
  }
  bool forced_;
  usize fail_index_ = ~usize{0};
  Fault fault_ = Fault::none;
};

std::vector<u64> Offsets(const CarefulWritePlan& plan, std::span<const CarefulWriteNodeId> ids) {
  std::vector<u64> out;
  for (const auto id : ids) out.push_back(plan.offset(id));
  return out;
}
std::string Render(const std::vector<u64>& offsets) {
  std::string out;
  for (const auto o : offsets) out += (out.empty() ? "" : ",") + std::to_string(o);
  return out;
}

constexpr usize kPage = 100;

// ---------------------------------------------------------------------------
void PlanValidationAndRefusals() {
  const auto a = Bytes(kPage, 0xA1);
  const auto b = Bytes(kPage, 0xB2);
  {
    CarefulWritePlan plan;
    RecordingSink sink(true);
    CheckError(plan.Validate().error, CarefulWriteError::empty_plan, "empty plan validates as refused");
    const auto r = plan.Execute(sink);
    CheckError(r.error, CarefulWriteError::empty_plan, "empty plan execute refused");
    Check(sink.ops.empty() && r.completed.empty() && r.failed_node == kCarefulWriteNoNode,
          "empty plan issues no sink call");
  }
  {
    CarefulWritePlan plan;
    RecordingSink sink(true);
    const auto added = plan.Add(0, std::span<const byte>{});
    CheckError(added.error, CarefulWriteError::empty_image, "zero-length image refused");
    Check(added.node == kCarefulWriteNoNode && plan.node_count() == 0, "refused image adds no node");
    Check(plan.Add(kPage, View(a)).ok(), "later valid add itself accepted");
    CheckError(plan.build_error(), CarefulWriteError::empty_image, "refusal poisons the plan");
    CheckError(plan.Execute(sink).error, CarefulWriteError::empty_image, "poisoned plan refused at execute");
    Check(sink.ops.empty(), "poisoned plan issues no sink call");
  }
  {
    CarefulWritePlan plan;
    RecordingSink sink(true);
    const auto short_preimage = Bytes(kPage - 1, 0);
    CheckError(plan.Add(0, View(a), View(short_preimage)).error,
               CarefulWriteError::preimage_size_mismatch, "preimage size must equal image size");
    CheckError(plan.Execute(sink).error, CarefulWriteError::preimage_size_mismatch,
               "preimage mismatch poisons");
    Check(sink.ops.empty(), "preimage size refusal issues no sink call");
  }
  {
    CarefulWritePlan plan;
    CheckError(plan.Add(~u64{0} - 1, View(a)).error, CarefulWriteError::extent_overflow,
               "extent past the native offset limit refused");
  }
  {
    CarefulWritePlan plan;
    RecordingSink sink(true);
    const auto first = plan.Add(0, View(a));
    Check(first.ok() && first.node == 0, "first node id is 0");
    const auto duplicate = plan.Add(0, View(b));
    CheckError(duplicate.error, CarefulWriteError::duplicate_offset, "one node per offset");
    Check(plan.node_count() == 1, "duplicate adds no node");
    CheckError(plan.Execute(sink).error, CarefulWriteError::duplicate_offset, "duplicate poisons");
    Check(sink.ops.empty() && sink.medium.empty(), "duplicate refusal issues no I/O");
    plan.Clear();
    Check(plan.build_error() == CarefulWriteError::none && plan.node_count() == 0, "Clear resets poison");
    Check(plan.Add(0, View(b)).ok(), "cleared plan accepts the offset again");
    const auto run = plan.Execute(sink);
    Check(run.ok() && sink.Log() == "W0 S" && sink.At(0, kPage) == b, "cleared plan executes");
  }
  {
    CarefulWritePlan plan;
    Check(plan.Add(0, View(a)).ok(), "base extent");
    CheckError(plan.Add(kPage / 2, View(b)).error, CarefulWriteError::overlapping_extent,
               "overlapping extent at a different offset refused");
    CarefulWritePlan adjacent;
    Check(adjacent.Add(0, View(a)).ok() && adjacent.Add(kPage, View(b)).ok() &&
              adjacent.Add(2 * kPage, View(a)).ok(),
          "adjacent extents are not overlaps");
    CarefulWritePlan below;
    Check(below.Add(kPage, View(a)).ok(), "upper extent");
    CheckError(below.Add(kPage / 2, View(b)).error, CarefulWriteError::overlapping_extent,
               "extent overlapping from below refused");
  }
  {
    CarefulWritePlan plan;
    RecordingSink sink(true);
    const auto x = plan.Add(0, View(a)).node;
    CheckError(plan.Before(x, 7), CarefulWriteError::unknown_node, "edge to unknown node refused");
    CheckError(plan.Execute(sink).error, CarefulWriteError::unknown_node, "unknown node poisons");
    Check(sink.ops.empty(), "unknown node refusal issues no I/O");
    CarefulWritePlan marked;
    Check(marked.Add(0, View(a)).ok(), "barrier target");
    CheckError(marked.MarkSyncBarrier(3), CarefulWriteError::unknown_node, "barrier on unknown node refused");
    CheckError(marked.build_error(), CarefulWriteError::unknown_node, "barrier refusal poisons");
  }
  {
    CarefulWritePlan plan;
    RecordingSink sink(true);
    const auto x = plan.Add(0, View(a)).node;
    CheckError(plan.Before(x, x), CarefulWriteError::self_edge, "self-edge refused");
    CheckError(plan.Validate().error, CarefulWriteError::self_edge, "self-edge poisons validate");
    CheckError(plan.Execute(sink).error, CarefulWriteError::self_edge, "self-edge poisons execute");
    Check(sink.ops.empty(), "self-edge refusal issues no I/O");
  }
  {
    CarefulWritePlan plan;
    RecordingSink sink(true);
    const auto x = plan.Add(0, View(a)).node;
    const auto y = plan.Add(kPage, View(b)).node;
    const auto z = plan.Add(2 * kPage, View(a)).node;
    Check(plan.Before(x, y) == CarefulWriteError::none && plan.Before(y, z) == CarefulWriteError::none &&
              plan.Before(z, x) == CarefulWriteError::none,
          "cycle edges are individually acceptable");
    CheckError(plan.Validate().error, CarefulWriteError::cycle, "three-node cycle refused");
    const auto r = plan.Execute(sink);
    CheckError(r.error, CarefulWriteError::cycle, "cycle refused at execute");
    Check(sink.ops.empty() && r.completed.empty(), "cycle refusal issues no I/O");
    CarefulWritePlan two;
    const auto p = two.Add(0, View(a)).node;
    const auto q = two.Add(kPage, View(b)).node;
    Check(two.Before(p, q) == CarefulWriteError::none && two.Before(q, p) == CarefulWriteError::none,
          "two-node cycle edges");
    CheckError(two.Validate().error, CarefulWriteError::cycle, "two-node cycle refused");
  }
  {
    // A repeated edge is harmless: same order, same barriers.
    CarefulWritePlan once;
    CarefulWritePlan twice;
    for (auto* plan : {&once, &twice}) {
      const auto x = plan->Add(kPage, View(a)).node;
      const auto y = plan->Add(0, View(b)).node;
      Check(plan->Before(x, y) == CarefulWriteError::none, "edge");
      if (plan == &twice) Check(plan->Before(x, y) == CarefulWriteError::none, "repeated edge accepted");
    }
    RecordingSink s1(false);
    RecordingSink s2(false);
    Check(once.Execute(s1).ok() && twice.Execute(s2).ok(), "both plans execute");
    Check(s1.Log() == "W100 S W0 S" && s2.Log() == s1.Log(), "repeated edge changes nothing: " + s2.Log());
  }
}

// ---------------------------------------------------------------------------
void DeterministicOrder() {
  const auto img = Bytes(kPage, 0x5A);
  {
    CarefulWritePlan plan;
    for (const u64 offset : {300u, 0u, 200u, 100u}) Check(plan.Add(offset, View(img)).ok(), "add");
    const auto order = plan.Validate();
    Check(order.ok() && Render(Offsets(plan, order.order)) == "0,100,200,300",
          "no edges: ascending offset regardless of add order");
  }
  {
    CarefulWritePlan plan;
    const auto n0 = plan.Add(0, View(img)).node;
    plan.Add(100, View(img));
    plan.Add(200, View(img));
    const auto n300 = plan.Add(300, View(img)).node;
    Check(plan.Before(n300, n0) == CarefulWriteError::none, "high offset first");
    const auto order = plan.Validate();
    Check(order.ok() && Render(Offsets(plan, order.order)) == "100,200,300,0",
          "edge overrides offset; ready ties by offset: " + Render(Offsets(plan, order.order)));
  }
  // The same diamond built in two different add orders issues identically.
  std::string first_render;
  for (int variant = 0; variant < 2; ++variant) {
    CarefulWritePlan plan;
    CarefulWriteNodeId map, a, b, root, leaf;
    if (variant == 0) {
      root = plan.Add(0, View(img)).node;
      a = plan.Add(100, View(img)).node;
      b = plan.Add(200, View(img)).node;
      map = plan.Add(300, View(img)).node;
      leaf = plan.Add(400, View(img)).node;
    } else {
      leaf = plan.Add(400, View(img)).node;
      map = plan.Add(300, View(img)).node;
      b = plan.Add(200, View(img)).node;
      a = plan.Add(100, View(img)).node;
      root = plan.Add(0, View(img)).node;
    }
    (void)leaf;
    Check(plan.Before(map, b) == CarefulWriteError::none && plan.Before(map, a) == CarefulWriteError::none &&
              plan.Before(b, root) == CarefulWriteError::none && plan.Before(a, root) == CarefulWriteError::none,
          "diamond edges");
    const auto order = plan.Validate();
    Check(order.ok(), "diamond validates");
    const auto rendered = Render(Offsets(plan, order.order));
    Check(rendered == "300,100,200,0,400", "diamond order: " + rendered);
    if (variant == 0) first_render = rendered;
    else Check(rendered == first_render, "add order does not change issue order");
    // Validate is repeatable.
    Check(Render(Offsets(plan, plan.Validate().order)) == rendered, "repeat validate identical");
  }
}

// map(300) -> a(100), map -> b(200), a -> root(0), b -> root, independent leaf(400).
struct Diamond {
  CarefulWritePlan plan;
  CarefulWriteNodeId map = 0, a = 0, b = 0, root = 0, leaf = 0;
  std::vector<byte> img_root = Bytes(kPage, 0x10), img_a = Bytes(kPage, 0x11), img_b = Bytes(kPage, 0x12),
                    img_map = Bytes(kPage, 0x13), img_leaf = Bytes(kPage, 0x14);
  Diamond() {
    root = plan.Add(0, View(img_root)).node;
    a = plan.Add(100, View(img_a)).node;
    b = plan.Add(200, View(img_b)).node;
    map = plan.Add(300, View(img_map)).node;
    leaf = plan.Add(400, View(img_leaf)).node;
    Check(plan.Before(map, a) == CarefulWriteError::none && plan.Before(map, b) == CarefulWriteError::none &&
              plan.Before(a, root) == CarefulWriteError::none && plan.Before(b, root) == CarefulWriteError::none,
          "diamond edges");
  }
  bool Landed(const RecordingSink& sink) const {
    return sink.At(0, kPage) == img_root && sink.At(100, kPage) == img_a && sink.At(200, kPage) == img_b &&
           sink.At(300, kPage) == img_map && sink.At(400, kPage) == img_leaf;
  }
};

void BarrierPlacement() {
  const std::string forced_log = "W300 W100 W200 W0 W400 S";
  const std::string synced_log = "W300 S W100 S W200 S W0 W400 S";
  struct Case {
    CarefulWriteBarrierPolicy policy;
    bool forced;
    const std::string* expected;
    unsigned syncs;
    const char* name;
  } cases[] = {
      {CarefulWriteBarrierPolicy::trust_forced_writes, true, &forced_log, 1, "trust+forced"},
      {CarefulWriteBarrierPolicy::trust_forced_writes, false, &synced_log, 4, "trust+unforced"},
      {CarefulWriteBarrierPolicy::sync_every_dependency, true, &synced_log, 4, "sync_every+forced"},
      {CarefulWriteBarrierPolicy::sync_every_dependency, false, &synced_log, 4, "sync_every+unforced"},
  };
  for (const auto& c : cases) {
    Diamond d;
    RecordingSink sink(c.forced);
    const auto r = d.plan.Execute(sink, c.policy);
    Check(r.ok(), std::string(c.name) + " executes");
    Check(sink.Log() == *c.expected, std::string(c.name) + " issue/barrier sequence: " + sink.Log());
    Check(r.syncs == c.syncs && r.written == 5 && r.skipped == 0, std::string(c.name) + " counts");
    Check(r.completed.size() == 5 && Render(Offsets(d.plan, r.completed)) == "300,100,200,0,400",
          std::string(c.name) + " completed in issue order");
    Check(d.Landed(sink), std::string(c.name) + " every image landed");
  }
  {
    Diamond d;
    Check(d.plan.MarkSyncBarrier(d.map) == CarefulWriteError::none, "mark barrier");
    RecordingSink sink(true);
    const auto r = d.plan.Execute(sink, CarefulWriteBarrierPolicy::trust_forced_writes);
    Check(r.ok() && sink.Log() == "W300 S W100 W200 W0 W400 S" && r.syncs == 2,
          "sync-barrier node forces a Sync before its successors under forced writes: " + sink.Log());
  }
  {
    Diamond d;
    Check(d.plan.MarkSyncBarrier(d.leaf) == CarefulWriteError::none, "mark leaf barrier");
    RecordingSink sink(true);
    const auto r = d.plan.Execute(sink, CarefulWriteBarrierPolicy::trust_forced_writes);
    Check(r.ok() && sink.Log() == forced_log && r.syncs == 1,
          "barrier on a node without successors adds nothing beyond the final Sync: " + sink.Log());
  }
  {
    // Single node: exactly one write and one final Sync under every policy.
    for (const bool forced : {true, false}) {
      CarefulWritePlan plan;
      const auto img = Bytes(kPage, 7);
      Check(plan.Add(0, View(img)).ok(), "single");
      RecordingSink sink(forced);
      const auto r = plan.Execute(sink, CarefulWriteBarrierPolicy::sync_every_dependency);
      Check(r.ok() && sink.Log() == "W0 S" && r.syncs == 1, "single node: one write, one final Sync");
    }
  }
}

// ---------------------------------------------------------------------------
void PreimageCases() {
  const auto old_x = Bytes(kPage, 0x01), new_x = Bytes(kPage, 0x02), other = Bytes(kPage, 0x03);
  const auto old_y = Bytes(kPage, 0x04), new_y = Bytes(kPage, 0x05);
  {
    CarefulWritePlan plan;
    Check(plan.Add(0, View(new_x), View(old_x)).ok(), "add with preimage");
    RecordingSink sink(true);
    sink.Place(0, old_x);
    const auto r = plan.Execute(sink);
    Check(r.ok() && sink.Log() == "R0 W0 S" && r.written == 1 && r.skipped == 0 && sink.At(0, kPage) == new_x,
          "on-disk equals preimage: write");
  }
  {
    CarefulWritePlan plan;
    const auto x = plan.Add(0, View(new_x), View(old_x)).node;
    const auto y = plan.Add(100, View(new_y)).node;
    Check(plan.Before(x, y) == CarefulWriteError::none, "x before y");
    RecordingSink sink(true);
    sink.Place(0, new_x);
    sink.Place(100, old_y);
    const auto r = plan.Execute(sink, CarefulWriteBarrierPolicy::trust_forced_writes);
    Check(r.ok() && r.written == 1 && r.skipped == 1, "on-disk equals image: skip");
    Check(sink.Log() == "R0 S W100 S",
          "a skipped node with successors gets a Sync even under forced writes: " + sink.Log());
    Check(r.completed.size() == 2 && r.completed[0] == x && r.completed[1] == y, "skip counts as completed");
  }
  {
    CarefulWritePlan plan;
    Check(plan.Add(0, View(new_x), View(new_x)).ok(), "preimage equal to image");
    RecordingSink sink(true);
    sink.Place(0, new_x);
    const auto r = plan.Execute(sink);
    Check(r.ok() && r.skipped == 1 && r.written == 0 && sink.Log() == "R0 S", "unchanged page is skipped");
  }
  {
    CarefulWritePlan plan;
    const auto x = plan.Add(0, View(new_x), View(old_x)).node;
    const auto y = plan.Add(100, View(new_y)).node;
    Check(plan.Before(x, y) == CarefulWriteError::none, "x before y");
    RecordingSink sink(true);
    sink.Place(0, other);
    sink.Place(100, old_y);
    const auto r = plan.Execute(sink);
    CheckError(r.error, CarefulWriteError::preimage_mismatch, "unexpected on-disk bytes refused");
    Check(r.failed_node == x && r.completed.empty() && r.written == 0 && r.syncs == 0, "refusal names node");
    Check(sink.Log() == "R0", "nothing issued after the refused read: " + sink.Log());
    Check(sink.At(0, kPage) == other && sink.At(100, kPage) == old_y, "medium untouched");
  }
  {
    // Mismatch at the second node: the first is complete, nothing follows.
    for (const bool forced : {true, false}) {
      CarefulWritePlan plan;
      const auto x = plan.Add(0, View(new_x)).node;
      const auto y = plan.Add(100, View(new_y), View(old_y)).node;
      Check(plan.Before(x, y) == CarefulWriteError::none, "x before y");
      RecordingSink sink(forced);
      sink.Place(100, other);
      const auto r = plan.Execute(sink);
      CheckError(r.error, CarefulWriteError::preimage_mismatch, "second node mismatch");
      Check(r.failed_node == y && r.completed.size() == 1 && r.completed[0] == x, "completed prefix");
      Check(sink.Log() == (forced ? "W0 R100" : "W0 S R100"), "no final Sync after refusal: " + sink.Log());
    }
  }
  {
    CarefulWritePlan plan;
    const auto x = plan.Add(0, View(new_x), View(old_x)).node;
    Check(plan.Add(100, View(new_y)).ok(), "y");
    RecordingSink sink(true);
    sink.Place(0, old_x);
    sink.FailCall(0, RecordingSink::Fault::error);
    const auto r = plan.Execute(sink);
    CheckError(r.error, CarefulWriteError::preimage_read_failed, "preimage read failure stops");
    Check(r.failed_node == x && r.io.native_error == EIO && sink.Log() == "R0" && r.completed.empty(),
          "read failure: no write, no later node");
  }
  {
    // Retry safety: a failed run, then the same plan again; already-written
    // nodes are skipped and the rest are written.
    CarefulWritePlan plan;
    const auto x = plan.Add(0, View(new_x), View(old_x)).node;
    const auto y = plan.Add(100, View(new_y), View(old_y)).node;
    Check(plan.Before(x, y) == CarefulWriteError::none, "x before y");
    RecordingSink sink(true);
    sink.Place(0, old_x);
    sink.Place(100, old_y);
    sink.FailCall(3, RecordingSink::Fault::error);  // R0 W0 R100 [W100 fails]
    const auto failed = plan.Execute(sink);
    CheckError(failed.error, CarefulWriteError::write_failed, "first attempt fails at y");
    Check(failed.failed_node == y && sink.Log() == "R0 W0 R100 W100" && sink.At(0, kPage) == new_x &&
              sink.At(100, kPage) == old_y,
          "first attempt wrote x only");
    sink.ResetLog();
    const auto retry = plan.Execute(sink);
    Check(retry.ok() && retry.skipped == 1 && retry.written == 1 && sink.Log() == "R0 S R100 W100 S",
          "retry skips x, writes y: " + sink.Log());
    Check(sink.At(0, kPage) == new_x && sink.At(100, kPage) == new_y, "retry reaches new state");
  }
}

// ---------------------------------------------------------------------------
void FirstErrorStop() {
  const auto img = Bytes(kPage, 0x66);
  {
    CarefulWritePlan plan;
    for (const u64 offset : {0u, 100u, 200u, 300u}) Check(plan.Add(offset, View(img)).ok(), "add");
    RecordingSink sink(true);
    sink.FailCall(2, RecordingSink::Fault::error);
    const auto r = plan.Execute(sink);
    CheckError(r.error, CarefulWriteError::write_failed, "write failure stops");
    Check(plan.offset(r.failed_node) == 200 && Render(Offsets(plan, r.completed)) == "0,100",
          "failed node and completed prefix named");
    Check(sink.Log() == "W0 W100 W200" && r.written == 2 && r.syncs == 0,
          "no later node and no Sync after the failed write: " + sink.Log());
    Check(r.io.operation == BoundedIoOperation::write && r.io.offset == 200 && r.io.native_error == EIO,
          "failing receipt returned");
  }
  {
    CarefulWritePlan plan;
    const auto n0 = plan.Add(0, View(img)).node;
    const auto n1 = plan.Add(100, View(img)).node;
    const auto n2 = plan.Add(200, View(img)).node;
    Check(plan.Before(n0, n1) == CarefulWriteError::none && plan.Before(n1, n2) == CarefulWriteError::none,
          "chain");
    RecordingSink sink(false);
    sink.FailCall(3, RecordingSink::Fault::error);  // W0 S W100 [S fails]
    const auto r = plan.Execute(sink);
    CheckError(r.error, CarefulWriteError::barrier_sync_failed, "barrier sync failure stops");
    Check(r.failed_node == n1 && r.completed.size() == 1 && r.completed[0] == n0 && r.syncs == 2,
          "barrier failure names the node whose successors it guards");
    Check(sink.Log() == "W0 S W100 S", "successor never issued: " + sink.Log());
  }
  {
    CarefulWritePlan plan;
    Check(plan.Add(0, View(img)).ok() && plan.Add(100, View(img)).ok(), "two leaves");
    RecordingSink sink(true);
    sink.FailCall(2, RecordingSink::Fault::error);  // W0 W100 [S fails]
    const auto r = plan.Execute(sink);
    CheckError(r.error, CarefulWriteError::final_sync_failed, "final sync failure reported");
    Check(r.failed_node == kCarefulWriteNoNode && r.completed.size() == 2 && r.written == 2 && r.syncs == 1,
          "final sync failure: no node named, every node issued");
    Check(sink.Log() == "W0 W100 S", "exactly one final Sync attempted");
  }
  {
    CarefulWritePlan plan;
    Check(plan.Add(0, View(img)).ok() && plan.Add(100, View(img)).ok(), "two leaves");
    RecordingSink sink(true);
    sink.FailCall(0, RecordingSink::Fault::short_transfer);
    const auto r = plan.Execute(sink);
    CheckError(r.error, CarefulWriteError::write_failed, "short write is a failure");
    Check(plan.offset(r.failed_node) == 0 && r.completed.empty() && sink.Log() == "W0",
          "short write stops before any later node");
  }
  {
    CarefulWritePlan plan;
    Check(plan.Add(0, View(img)).ok() && plan.Add(100, View(img)).ok(), "two leaves");
    RecordingSink sink(true);
    sink.FailCall(1, RecordingSink::Fault::exception);
    const auto r = plan.Execute(sink);
    CheckError(r.error, CarefulWriteError::sink_exception, "sink exception contained");
    Check(plan.offset(r.failed_node) == 100 && r.completed.size() == 1 && sink.Log() == "W0 W100",
          "exception stops at its node");
  }
}

// ---------------------------------------------------------------------------
void FileDeviceSink() {
  scratchbird::tests::OwnedTempDirectory directory;
  const auto path = (directory.path() / "careful-write.db").string();
  constexpr usize kFilePage = 512;
  const auto zeros = Bytes(4 * kFilePage, 0);
  {
    disk::FileDevice device;
    Check(device.Open(path, disk::FileOpenMode::create_new).ok(), "create file");
    Check(device.WriteAt(0, zeros.data(), zeros.size()).ok(), "pre-size file");
#if defined(__linux__)
    Check(device.forced_ordered_writes(), "writable Linux device has forced ordered writes");
#endif
    std::vector<std::vector<byte>> images;
    for (byte i = 0; i < 4; ++i) images.push_back(Bytes(kFilePage, static_cast<byte>(0x40 + i)));
    const auto old_page = Bytes(kFilePage, 0);
    CarefulWritePlan plan;
    std::vector<CarefulWriteNodeId> ids;
    for (usize i = 0; i < 4; ++i)
      ids.push_back(plan.Add(i * kFilePage, View(images[i]), View(old_page)).node);
    Check(plan.Before(ids[1], ids[0]) == CarefulWriteError::none &&
              plan.Before(ids[2], ids[0]) == CarefulWriteError::none &&
              plan.Before(ids[3], ids[2]) == CarefulWriteError::none,
          "file plan edges");
    {
      disk::FileDeviceCarefulWriteSink sink(device);
      Check(sink.forced_ordered() == device.forced_ordered_writes(), "sink mirrors device flag");
      const auto r = plan.Execute(sink);
      Check(r.ok() && r.written == 4 && r.skipped == 0, "file plan executes");
      Check(Render(Offsets(plan, r.completed)) == "512,1536,1024,0", "file plan order");
    }
    for (usize i = 0; i < 4; ++i) {
      std::vector<byte> found(kFilePage);
      Check(device.ReadAt(i * kFilePage, found.data(), found.size()).ok() && found == images[i],
            "file page " + std::to_string(i) + " holds the new image");
    }
    {
      disk::FileDeviceCarefulWriteSink sink(device);
      const auto again = plan.Execute(sink);
      Check(again.ok() && again.written == 0 && again.skipped == 4, "re-execution skips every page");
    }
    if (disk::ArmTestDeviceWriteFault(path)) {
      CarefulWritePlan fresh;
      const auto replacement = Bytes(kFilePage, 0x77);
      Check(fresh.Add(0, View(replacement), View(images[0])).ok(), "fault plan");
      disk::FileDeviceCarefulWriteSink sink(device);
      const auto r = fresh.Execute(sink);
      disk::DisarmTestDeviceWriteFault();
      CheckError(r.error, CarefulWriteError::write_failed, "device write fault stops the plan");
      Check(r.io.native_error == EIO && r.syncs == 0, "fault receipt; no Sync after a failed write");
      std::vector<byte> found(kFilePage);
      Check(device.ReadAt(0, found.data(), found.size()).ok() && found == images[0], "faulted page unchanged");
    } else {
      std::cout << "careful_write: device write fault injection not compiled in; fault case not run\n";
    }
    Check(device.Close().ok(), "close");
    Check(!device.forced_ordered_writes(), "closed device reports no forced writes");
  }
  {
    disk::FileDevice device;
    Check(device.Open(path, disk::FileOpenMode::open_existing_read_only).ok(), "read-only open");
    Check(!device.forced_ordered_writes(), "read-only device reports no forced writes");
    CarefulWritePlan plan;
    const auto img = Bytes(kFilePage, 0x99);
    Check(plan.Add(0, View(img)).ok(), "read-only plan");
    {
      disk::FileDeviceCarefulWriteSink sink(device);
      Check(!sink.forced_ordered(), "read-only sink not forced");
      const auto r = plan.Execute(sink);
      CheckError(r.error, CarefulWriteError::write_failed, "read-only device refuses the write");
      Check(r.io.error == BoundedIoError::read_only && r.syncs == 0, "read-only receipt");
    }
    Check(device.Close().ok(), "close read-only");
  }
  directory.Cleanup();
}

}  // namespace

int main() {
  try {
    PlanValidationAndRefusals();
    DeterministicOrder();
    BarrierPlacement();
    PreimageCases();
    FirstErrorStop();
    FileDeviceSink();
  } catch (const std::exception& error) {
    std::cerr << "careful_write_test FAILED after " << checks << " checks: " << error.what() << '\n';
    return 1;
  }
  std::cout << "careful_write_test passed checks=" << checks << '\n';
  return 0;
}
