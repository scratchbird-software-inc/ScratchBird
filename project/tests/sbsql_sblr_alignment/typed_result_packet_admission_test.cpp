// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Actual canonical encoder, independent fixed-layout sizes and allocator
// observation. Component coverage, not an SQL/IPC or physical-grant oracle.
#define main ExistingTypedResultCodecFixtureMain
#include "typed_result_transport_codec_test.cpp"
#undef main
#include <cstdlib>
#include <new>

namespace packet_alloc {
thread_local bool watch = false;
thread_local std::size_t largest = 0;
thread_local std::size_t reject_at = 0;
thread_local long remaining = -1;
thread_local unsigned faults = 0;
}
void* operator new(std::size_t bytes) {
  if (packet_alloc::watch) {
    packet_alloc::largest = std::max(packet_alloc::largest, bytes);
    if ((packet_alloc::reject_at && bytes >= packet_alloc::reject_at) ||
        packet_alloc::remaining == 0) {
      ++packet_alloc::faults;
      throw std::bad_alloc();
    }
    if (packet_alloc::remaining > 0) --packet_alloc::remaining;
  }
  if (void* pointer = std::malloc(bytes ? bytes : 1)) return pointer;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
  try { return ::operator new(n); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
  try { return ::operator new(n); } catch (...) { return nullptr; }
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }

#ifdef SB_PACKET_ORIGINAL_CODEC
namespace scratchbird::wire {
TypedResultBatchCodecResult EncodeTypedResultBatch(
    const TypedResultBatch&, const TypedResultRowDescriptor&,
    const TypedResultCarrierBinding&);
TypedResultBatchCodecResult EncodeTypedResultBatch(
    const TypedResultBatch& b, const TypedResultRowDescriptor& d,
    const TypedResultCarrierBinding& c, u64) {
  using Fn = TypedResultBatchCodecResult (*)(const TypedResultBatch&,
      const TypedResultRowDescriptor&, const TypedResultCarrierBinding&);
  const Fn original = &EncodeTypedResultBatch;
  return original(b, d, c);
}
}
#endif

namespace {
unsigned packet_checks = 0;
void CheckPacket(bool ok, const char* detail) {
  ++packet_checks;
  Require(ok, detail);
}
wire::TypedResultBatchCodecResult EncodePacket(
    const wire::TypedResultBatch& batch,
    const wire::TypedResultRowDescriptor& descriptor, std::uint64_t ceiling) {
  const auto binding = ExecuteBinding(batch);
#ifdef SB_PACKET_ORIGINAL_CODEC
  using Fn = wire::TypedResultBatchCodecResult (*)(
      const wire::TypedResultBatch&, const wire::TypedResultRowDescriptor&,
      const wire::TypedResultCarrierBinding&);
  const Fn original = &wire::EncodeTypedResultBatch;
  return original(batch, descriptor, binding);
#else
  return wire::EncodeTypedResultBatch(batch, descriptor, binding, ceiling);
#endif
}
void RefusesBeforeMaterialization(const wire::TypedResultBatch& batch,
                                 const wire::TypedResultRowDescriptor& descriptor,
                                 std::uint64_t ceiling) {
  packet_alloc::largest = 0;
  packet_alloc::reject_at = 1024;
  packet_alloc::watch = true;
  bool escaped = false;
  wire::TypedResultBatchCodecResult refused;
  try { refused = EncodePacket(batch, descriptor, ceiling); }
  catch (...) { escaped = true; }
  packet_alloc::watch = false;
  packet_alloc::reject_at = 0;
  CheckPacket(!escaped, "oversize packet attempted allocation before admission");
  CheckPacket(packet_alloc::largest < 1024,
              "oversize packet copied descriptor, row or cell storage");
  CheckPacket(refused.status == wire::TypedResultCodecStatus::resource_limit_exceeded &&
                  refused.encoded.empty() && refused.batch.rows.empty(),
              "packet ceiling returned success or a partial packet");
}
void PacketCases(bool global_only) {
  constexpr std::uint64_t max_packet = 16ull * 1024ull * 1024ull;
  // Core fixed header224 + row16 + cell20 + datatype envelope32.
  constexpr std::uint64_t one_row_overhead = 292;
  auto descriptor = Descriptor({TextColumn(0, "value", 0, 0x11)});
  auto oversized = Batch(descriptor, {Row(0, {Present(0, 0,
      std::vector<byte>(max_packet - one_row_overhead + 1, 'x'))})});
  RefusesBeforeMaterialization(oversized, descriptor, UINT64_MAX);
  // A huge descriptor must not be copied before the packet byte refusal.
  descriptor.columns[0].name.assign(32768, 'n');
  RefusesBeforeMaterialization(oversized, descriptor, max_packet);
  if (global_only) return;
  descriptor.columns[0].name = "value";
  auto batch = Batch(descriptor, {Row(0, {Present(0, 0, Bytes("hello"))}),
                                 Row(1, {Null(0, 0)})});
  // Complete two-row packet including an empty SQL NULL value envelope.
  constexpr std::uint64_t exact = 224 + 2 * (16 + 20 + 32) + 5;
  auto golden = EncodePacket(batch, descriptor, exact);
  CheckPacket(golden.ok() && golden.encoded.size() == exact,
              "exact independent frame size not admitted");
  CheckPacket(platform::LoadLittle64(golden.encoded.data() + 16) == exact &&
                  platform::LoadLittle64(golden.encoded.data() + 144) == exact - 224,
              "canonical header omitted part of the charged bytes");
  for (std::uint64_t ceiling : std::array<std::uint64_t, 4>{0, 1, 223, exact - 1})
    RefusesBeforeMaterialization(batch, descriptor, ceiling);
  auto default_ceiling = EncodePacket(batch, descriptor, UINT64_MAX);
  CheckPacket(default_ceiling.ok() && default_ceiling.encoded == golden.encoded,
              "ceiling changed admitted canonical bytes");
  auto decoded = wire::DecodeTypedResultBatch(
      golden.encoded, descriptor, ExecuteBinding(batch));
  CheckPacket(decoded.ok() && decoded.batch.rows.size() == 2 &&
                  decoded.batch.rows[0].cells[0].canonical_payload == Bytes("hello") &&
                  decoded.batch.rows[1].cells[0].state == wire::TypedResultValueState::sql_null,
              "admitted bytes changed values or SQL NULL state");
  // Exactly the transport limit must still work, without a rounded-up reserve.
  oversized.rows[0].cells[0].canonical_payload.pop_back();
  auto maximum = EncodePacket(oversized, descriptor, max_packet);
  CheckPacket(maximum.ok() && maximum.encoded.size() == max_packet,
              "exact transport maximum falsely refused");
  CheckPacket(maximum.encoded.capacity() == max_packet,
              "canonical output growth exceeds admitted frame capacity");
  // Failure anywhere in normal encoding must preserve the inputs and allow
  // exact retry. The caller owns exception conversion, not this codec.
  bool completed = false;
  for (long fail = 0; fail != 2048; ++fail) {
    packet_alloc::remaining = fail;
    packet_alloc::watch = true;
    bool failed = false;
    wire::TypedResultBatchCodecResult candidate;
    try { candidate = EncodePacket(batch, descriptor, exact); }
    catch (const std::bad_alloc&) { failed = true; }
    catch (...) { packet_alloc::watch = false; throw; }
    packet_alloc::watch = false;
    packet_alloc::remaining = -1;
    CheckPacket(batch.rows.size() == 2 && batch.rows[0].row_ordinal == 0 &&
                    batch.rows[0].cells[0].canonical_payload == Bytes("hello") &&
                    batch.batch_evidence_sha256 == wire::TypedResultEvidenceHash{},
                "failed codec modified its input ownership or ordinals");
    if (!failed && candidate.ok()) {
      CheckPacket(candidate.encoded == golden.encoded, "fault sweep changed bytes");
      completed = true;
      break;
    }
    CheckPacket(candidate.encoded.empty(), "allocation failure exposed a partial packet");
    auto retry = EncodePacket(batch, descriptor, exact);
    CheckPacket(retry.ok() && retry.encoded == golden.encoded,
                "allocation failure prevented exact retry");
  }
  CheckPacket(completed && packet_alloc::faults != 0,
              "allocation fault sweep did not reach real encoding completion");
}
}
int main(int argc, char** argv) {
  try {
    PacketCases(argc == 2 && std::string_view(argv[1]) == "--global-only");
    std::cout << "PASS typed packet checks=" << packet_checks
              << " faults=" << packet_alloc::faults << '\n';
    return 0;
  } catch (const std::exception& e) {
    packet_alloc::watch = false;
    std::cerr << "FAIL " << e.what() << '\n';
    return 1;
  }
}
