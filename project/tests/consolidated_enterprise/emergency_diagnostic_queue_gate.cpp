// SPDX-License-Identifier: MPL-2.0
// Reuse the process allocation-fault harness, not a diagnostic producer mock.
#define main LegacyMemoryFaultHarnessMain
#include "../sbsql_sblr_alignment/reservation_backed_resource_ownership_test.cpp"
#undef main
#include "emergency_diagnostic_queue.hpp"
#if defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace {
using Capture = mem::EmergencyCaptureStatus;
mem::MemoryTag QueueTag() {
  mem::MemoryTag tag;
  for (auto kind : {mem::MemoryBinaryScopeKind::owner, mem::MemoryBinaryScopeKind::context}) {
    auto& id = tag.binary_ownership[kind];
    id[0] = 1; id[6] = 0x70; id[8] = 0x80; id[15] = static_cast<unsigned char>(kind) + 1;
  }
  return tag;
}

void BoundariesAndRetention() {
  mem::MemoryManager manager(Policy());
  {
    mem::EmergencyDiagnosticQueue queue(manager, QueueTag());
    std::array<std::byte, 16> first{}, second{}, output{};
    first.fill(std::byte{0xa5}); second.fill(std::byte{0x5a});
    Check(queue.TryCapture(first).status == Capture::uninitialized, "uninitialized capture");
    Check(queue.Initialize(0, 16) == Capture::invalid_request, "zero slot count");
    Check(queue.Initialize(1, std::numeric_limits<std::size_t>::max()) == Capture::invalid_request,
          "slot size overflow");
    Check(queue.Initialize(std::numeric_limits<std::size_t>::max(), 16) == Capture::invalid_request,
          "capacity multiplication overflow");
    fault::Arm(0);
    const auto refused = queue.Initialize(2, 16);
    fault::Off();
    Check(refused == Capture::allocation_failed && manager.Snapshot().current_bytes == 0,
          "failed initialization leaked a charge");
    Check(queue.Initialize(2, 16) == Capture::ok, "bounded backing initialization");
    const auto backing = queue.Snapshot().backing_bytes;
    Check(backing == 2 * (sizeof(mem::u64) + sizeof(mem::usize) + 16) &&
              manager.Snapshot().current_bytes == backing,
          "payload and slot headers must have real governed backing");
    fault::Arm(0);
    const auto a = queue.TryCapture(first);
    const auto b = queue.TryCapture(second);
    const auto full = queue.TryCapture(first);
    const auto invalid = queue.TryCapture({});
    std::array<std::byte, 17> oversized{};
    const auto too_large = queue.TryCapture(oversized);
    const auto short_read = queue.TryRead(std::span(output).first(15));
    const auto read = queue.TryRead(output);
    const bool first_preserved = output == first;
    const auto repeated_read = queue.TryRead(output); // failed sink: no ack
    const auto wrong = queue.AcknowledgeDelivery(b.sequence);
    const auto ack = queue.AcknowledgeDelivery(a.sequence);
    const auto stale = queue.AcknowledgeDelivery(a.sequence);
    const auto second_read = queue.TryRead(output);
    const bool second_preserved = output == second;
    const auto snapshot = queue.Snapshot();
    const bool allocated = fault::hit;
    fault::Off();
    Check(!allocated && a.ok() && b.ok(), "capture/read/ack allocated under exhaustion");
    Check(full.status == Capture::full && invalid.status == Capture::invalid_request &&
              too_large.status == Capture::invalid_request && snapshot.dropped == 3,
          "reject-new overflow accounting");
    Check(short_read.status == Capture::destination_too_small && short_read.bytes == 16 &&
              read.sequence == a.sequence && repeated_read.sequence == a.sequence && first_preserved,
          "short destination or failed delivery consumed first evidence");
    Check(wrong == Capture::stale_receipt && ack == Capture::ok && stale == Capture::stale_receipt &&
              second_read.sequence == b.sequence && second_preserved && snapshot.queued == 1 &&
              snapshot.captured == 2 && snapshot.acknowledged == 1,
          "FIFO or exact acknowledgement failed");
    Check(queue.Initialize(4, 16) == Capture::already_initialized,
          "initialization overwrote captured records");
    Check(queue.TryCapture(std::span(first).first(1)).ok(), "wrapped slot reuse");
    Check(queue.CloseCapture() == Capture::ok && queue.CloseCapture() == Capture::ok &&
              queue.TryCapture(first).status == Capture::closed && !queue.Snapshot().accepting,
          "close did not stop capture or was not idempotent");
    Check(queue.AcknowledgeDelivery(b.sequence) == Capture::ok, "second acknowledgement");
    output.fill(std::byte{0x33});
    const auto tiny = queue.TryRead(output);
    Check(tiny.ok() && tiny.bytes == 1 && output[0] == first[0] && output[1] == std::byte{0x33},
          "short payload copied stale tail bytes");
    Check(queue.AcknowledgeDelivery(tiny.sequence) == Capture::ok &&
              queue.TryRead(output).status == Capture::empty &&
              manager.Snapshot().current_bytes == backing,
          "drain refunded still-owned physical backing");
    fault::Arm(0);
  }
  const bool teardown_allocated = fault::hit;
  fault::Off();
  Check(!teardown_allocated, "quiescent queue teardown allocated under exhaustion");
  Check(manager.Snapshot().current_bytes == 0, "queue destruction leaked backing");
  mem::EmergencyDiagnosticQueue invalid_owner(manager, {});
  Check(invalid_owner.Initialize(1, 16) == Capture::invalid_request, "missing binary ownership admitted");
}

void ExhaustedReceiptStillDrains() {
  mem::MemoryManager manager(Policy());
  mem::EmergencyDiagnosticQueue queue(manager, QueueTag());
  Check(queue.Initialize(2, 1, 2) == Capture::ok, "bounded receipt range setup");
  const std::array<std::byte, 1> input{std::byte{1}};
  const auto first = queue.TryCapture(input);
  Check(first.sequence == 1 && queue.AcknowledgeDelivery(first.sequence) == Capture::ok,
        "first bounded receipt");
  const auto last = queue.TryCapture(input);
  Check(last.sequence == 2 && queue.TryCapture(input).status == Capture::receipt_exhausted,
        "receipt range silently rolled over");
  Check(queue.AcknowledgeDelivery(last.sequence) == Capture::ok &&
            queue.TryCapture(input).status == Capture::receipt_exhausted &&
            queue.Snapshot().queued == 0, "receipt exhaustion prevented drain or renewed identities");
}

void InitializationFailureSweep() {
  bool completed = false;
  for (long boundary = 0; boundary != 128 && !completed; ++boundary) {
    mem::MemoryManager manager(Policy());
    {
      mem::EmergencyDiagnosticQueue queue(manager, QueueTag());
      fault::Arm(boundary);
      const auto status = queue.Initialize(4, 32);
      const bool injected_failure = fault::hit;
      fault::Off();
      Check(status == Capture::ok || status == Capture::allocation_failed,
            "initialization fault escaped its typed failure boundary");
      if (status == Capture::ok) {
        Check(manager.Snapshot().current_bytes == queue.Snapshot().backing_bytes,
              "successful faulted initialization lost physical charge");
      } else {
        Check(manager.Snapshot().current_bytes == 0 &&
                  queue.Snapshot().status == Capture::uninitialized,
              "failed initialization published partial backing");
        Check(queue.Initialize(4, 32) == Capture::ok, "failed initialization was not retryable");
      }
      completed = !injected_failure;
    }
    Check(manager.Snapshot().current_bytes == 0 && manager.Snapshot().active_allocation_count == 0,
          "initialization fault sweep leaked ownership");
  }
  Check(completed, "initialization fault sweep did not reach unfaulted completion");
}

void ConcurrentCaptureAndDrain() {
  mem::MemoryManager manager(Policy());
  mem::EmergencyDiagnosticQueue queue(manager, QueueTag());
  using Payload = std::array<mem::u64, 2>;
  constexpr unsigned producers = 4, each = 128;
  Check(queue.Initialize(8, sizeof(Payload)) == Capture::ok, "concurrent backing setup");
  std::atomic<unsigned> errors{0};
  std::array<std::thread, producers> threads;
  for (unsigned producer = 0; producer < producers; ++producer) {
    threads[producer] = std::thread([&, producer] {
      fault::Arm(0);
      for (unsigned row = 0; row < each; ++row) {
        const Payload input{producer, row};
        for (;;) {
          const auto receipt = queue.TryCapture(std::as_bytes(std::span(input)));
          if (receipt.ok()) break;
          if (receipt.status != Capture::full && receipt.status != Capture::busy) { ++errors; break; }
          std::this_thread::yield();
        }
      }
      if (fault::hit) ++errors;
      fault::Off();
    });
  }
  std::array<unsigned, producers> next{};
  fault::Arm(0);
  for (unsigned received = 0; received < producers * each;) {
    Payload output{};
    const auto receipt = queue.TryRead(std::as_writable_bytes(std::span(output)));
    if (receipt.status == Capture::busy || receipt.status == Capture::empty) {
      std::this_thread::yield(); continue;
    }
    if (!receipt.ok() || receipt.sequence != received + 1 || output[0] >= producers ||
        output[1] != next[output[0]]++) ++errors;
    Capture ack;
    do { ack = queue.AcknowledgeDelivery(receipt.sequence); } while (ack == Capture::busy);
    if (ack != Capture::ok) ++errors;
    ++received;
  }
  if (fault::hit) ++errors;
  fault::Off();
  for (auto& thread : threads) thread.join();
  const auto snapshot = queue.Snapshot();
  Check(errors == 0 && snapshot.queued == 0 && snapshot.captured == producers * each &&
            snapshot.acknowledged == snapshot.captured, "concurrent FIFO/ownership/no-allocation failure");
}

void RealNonblockingSinkRetention() {
#if defined(__linux__)
  int descriptors[2];
  const bool opened = ::pipe2(descriptors, O_NONBLOCK | O_CLOEXEC) == 0;
  Check(opened, "nonblocking diagnostic pipe");
  if (!opened) return;
  mem::MemoryManager manager(Policy());
  mem::EmergencyDiagnosticQueue queue(manager, QueueTag());
  Check(queue.Initialize(2, 16) == Capture::ok, "sink fixture queue");
  std::array<std::byte, 16> input{};
  input.fill(std::byte{0x7b});
  const auto captured = queue.TryCapture(input);
  std::array<std::byte, 4096> filler{};
  while (::write(descriptors[1], filler.data(), filler.size()) > 0) {}
  Check(errno == EAGAIN || errno == EWOULDBLOCK, "pipe did not reach backpressure");
  struct Sink { int fd; mem::EmergencyDiagnosticQueue* queue; bool unlocked = false; } sink{descriptors[1], &queue};
  const auto write_sink = +[](void* context, std::span<const std::byte> bytes) noexcept {
    auto& state = *static_cast<Sink*>(context);
    state.unlocked = state.queue->Snapshot().status == Capture::ok;
    const auto written = ::write(state.fd, bytes.data(), bytes.size());
    return mem::EmergencyDiagnosticSinkReceipt{
        written < 0 ? 0 : static_cast<mem::usize>(written),
        written >= 0 && static_cast<mem::usize>(written) == bytes.size()};
  };
  std::array<std::byte, 16> scratch{};
  fault::Arm(0);
  const auto blocked = mem::DeliverEmergencyDiagnostic(queue, scratch, write_sink, &sink);
  const bool blocked_allocated = fault::hit;
  fault::Off();
  Check(!blocked_allocated && !blocked.delivered && !blocked.retired &&
            blocked.record.status == Capture::sink_failed && queue.Snapshot().queued == 1,
        "sink backpressure lost capture or allocated");
  while (::read(descriptors[0], filler.data(), filler.size()) > 0) {}
  fault::Arm(0);
  const auto delivered = mem::DeliverEmergencyDiagnostic(queue, scratch, write_sink, &sink);
  const bool delivered_allocated = fault::hit;
  fault::Off();
  std::array<std::byte, 16> actual{};
  Check(::read(descriptors[0], actual.data(), actual.size()) == 16 && actual == input &&
            delivered.delivered && delivered.retired && delivered.record.sequence == captured.sequence &&
            delivered.delivered_bytes == input.size() && !delivered_allocated && sink.unlocked &&
            queue.Snapshot().queued == 0,
        "real sink bytes, delivery acknowledgement or lock boundary failed");
  Check(queue.TryCapture(input).ok(), "partial-delivery setup");
  const auto partial_sink = +[](void*, std::span<const std::byte> bytes) noexcept {
    return mem::EmergencyDiagnosticSinkReceipt{bytes.size() / 2, false};
  };
  const auto partial = mem::DeliverEmergencyDiagnostic(queue, scratch, partial_sink, nullptr);
  Check(!partial.delivered && !partial.retired && partial.record.status == Capture::partial_delivery &&
            queue.Snapshot().queued == 1, "partial sink receipt retired captured evidence");
  const auto invalid_sink = +[](void*, std::span<const std::byte> bytes) noexcept {
    return mem::EmergencyDiagnosticSinkReceipt{bytes.size() + 1, true};
  };
  Check(mem::DeliverEmergencyDiagnostic(queue, scratch, invalid_sink, nullptr).record.status ==
            Capture::invalid_sink_receipt && queue.Snapshot().queued == 1,
        "invalid sink receipt retired captured evidence");
  ::close(descriptors[0]);
  ::close(descriptors[1]);
#endif
}
}

int main() {
  WarmMetrics();
  BoundariesAndRetention();
  ExhaustedReceiptStillDrains();
  InitializationFailureSweep();
  ConcurrentCaptureAndDrain();
  RealNonblockingSinkRetention();
  // Payloads here prove byte custody only, not canonical-vector validation,
  // authorization, production sink delivery or full OOM-safe producer wiring.
  return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
