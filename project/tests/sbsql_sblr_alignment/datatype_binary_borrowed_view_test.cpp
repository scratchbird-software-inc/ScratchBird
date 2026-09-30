// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#define main ExistingTypedResultFixtureMain
#include "typed_result_transport_codec_test.cpp"
#undef main
#include "datatype_binary_view.hpp"
#include "sbl_numeric.hpp"
#include <atomic>
#include <cstdlib>
#include <new>
#include <cstring>
#include <limits>
#include <thread>

namespace borrowed_alloc {
thread_local bool watch = false;
thread_local std::size_t exact_size = 0, exact_count = 0, reject_at = 0;
thread_local long remaining = -1;
thread_local bool fault = false;
}
void* operator new(std::size_t bytes) {
  if (borrowed_alloc::watch) {
    if (bytes == borrowed_alloc::exact_size) ++borrowed_alloc::exact_count;
    if ((borrowed_alloc::reject_at && bytes >= borrowed_alloc::reject_at) ||
        borrowed_alloc::remaining == 0) {
      borrowed_alloc::fault = true;
      throw std::bad_alloc();
    }
    if (borrowed_alloc::remaining > 0) --borrowed_alloc::remaining;
  }
  if (void* p = std::malloc(bytes ? bytes : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept {
  try { return ::operator new(bytes); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t bytes, const std::nothrow_t&) noexcept {
  try { return ::operator new(bytes); } catch (...) { return nullptr; }
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }

namespace {
unsigned checks = 0, failures = 0, allocation_faults = 0;
void Check(bool pass, const char* why) {
  ++checks;
  if (!pass) { ++failures; std::cerr << "FAIL " << why << '\n'; }
}
namespace dt = scratchbird::core::datatypes;
dt::DatatypeBinaryValueView View(const dt::DatatypeBinaryValue& value) {
  return {value.type_id, value.is_null, value.payload_is_toast_reference,
          value.payload.data(), value.payload.size()};
}
void SharedCodecParity() {
  std::vector<dt::DatatypeBinaryValue> values{
      {dt::CanonicalTypeId::boolean, false, false, {0}},
      {dt::CanonicalTypeId::boolean, false, false, {1}},
      {dt::CanonicalTypeId::int32, false, false, {0xff, 0xff, 0xff, 0xff}},
      {dt::CanonicalTypeId::int64, false, false, {1,2,3,4,5,6,7,8}},
      {dt::CanonicalTypeId::character, false, false, Bytes("abc")},
      {dt::CanonicalTypeId::character, false, false, {0, 'a', 0}},
      {dt::CanonicalTypeId::character, false, false, {}},
      {dt::CanonicalTypeId::character, true, false, {}},
      {dt::CanonicalTypeId::binary, false, false, {0, 0xff, 0x80}},
      {dt::CanonicalTypeId::int64, true, false, {}}};
  std::vector<byte> wide(16, 0xff); wide.back() = 0x7f;
  values.push_back({dt::CanonicalTypeId::int128, false, false, wide});
  std::fill(wide.begin(), wide.end(), 0); wide.back() = 0x80;
  values.push_back({dt::CanonicalTypeId::int128, false, false, wide});
  for (const auto text : {"0", "12.5", "-12345678901234567890123456789012345678"}) {
    const auto decimal = scratchbird::libraries::sbl_numeric::EncodeExactDecimalLittleEndian(text);
    Check(decimal.ok, "canonical decimal fixture");
    values.push_back({dt::CanonicalTypeId::decimal, false, false,
                     {decimal.canonical_bytes.begin(), decimal.canonical_bytes.end()}});
  }
  for (unsigned version = 1; version != 8; ++version) {
    std::vector<byte> uuid(16, 0xab); uuid[6] = version << 4; uuid[8] = 0x80;
    values.push_back({dt::CanonicalTypeId::uuid, false, false, std::move(uuid)});
  }
  for (const auto& value : values) {
    const auto owned = dt::EncodeDatatypeBinaryValue(value);
    Check(owned.ok(), "owning codec fixture refused");
    std::vector<byte> destination(32 + value.payload.size() + 11, 0xa5);
    const auto result = dt::EncodeDatatypeBinaryValueInto(View(value), destination.data(), destination.size());
    Check(result.ok() && result.bytes_written == 32 + value.payload.size(), "borrowed envelope size");
    Check(std::equal(owned.encoded.begin(), owned.encoded.end(), destination.begin()),
          "borrowed encoder changed existing canonical bytes");
    Check(std::all_of(destination.begin() + result.bytes_written, destination.end(),
                     [](byte b) { return b == 0xa5; }), "borrowed encoder wrote beyond exact envelope");
    Check(platform::LoadLittle16(destination.data() + 14) == 32 &&
          platform::LoadLittle32(destination.data() + 16) == value.payload.size() &&
          platform::LoadLittle32(destination.data() + 20) == 0,
          "Core fixed header/payload/reserved fields");
    Check(std::equal(value.payload.begin(), value.payload.end(), destination.begin() + 32),
          "user payload bytes changed during borrowed encoding");
  }
  const auto text = Bytes("abc");
  std::array<byte, 35> golden{};
  Check(dt::EncodeDatatypeBinaryValueInto({dt::CanonicalTypeId::character, false, false,
      text.data(), text.size()}, golden.data(), golden.size()).ok(), "fixed golden text");
  Check(std::memcmp(golden.data(), "SBDVAL01", 8) == 0 &&
        platform::LoadLittle16(golden.data() + 12) == 0 &&
        platform::LoadLittle64(golden.data() + 24) == 0xe16801510db89efdULL,
        "independent existing-envelope checksum/magic/flags golden");
}

void DestinationAtomicityAndAlias() {
  auto payload = Bytes("abc");
  const dt::DatatypeBinaryValueView valid{dt::CanonicalTypeId::character, false, false,
                                         payload.data(), payload.size()};
  for (std::size_t capacity = 0; capacity != 35; ++capacity) {
    std::array<byte, 64> destination; destination.fill(0x5a);
    const auto result = dt::EncodeDatatypeBinaryValueInto(valid, destination.data(), capacity);
    Check(!result.ok() && result.bytes_written == 0, "short destination published envelope");
    Check(std::all_of(destination.begin(), destination.end(), [](byte b){ return b == 0x5a; }),
          "short destination changed bytes before refusal");
  }
  Check(!dt::EncodeDatatypeBinaryValueInto(valid, nullptr, 35).ok(), "null destination admitted");
  for (std::size_t source : {0u, 20u, 30u, 32u, 50u, 52u, 54u, 70u}) {
    std::array<byte, 128> buffer; buffer.fill(0xa5);
    std::copy(payload.begin(), payload.end(), buffer.begin() + source);
    const auto result = dt::EncodeDatatypeBinaryValueInto(
        {dt::CanonicalTypeId::character, false, false, buffer.data() + source, 3},
        buffer.data() + 20, 35);
    Check(result.ok() && result.bytes_written == 35, "aliased payload refused");
    Check(platform::LoadLittle64(buffer.data() + 44) == 0xe16801510db89efdULL &&
          std::equal(payload.begin(), payload.end(), buffer.begin() + 52),
          "header write corrupted aliased payload or checksum");
  }
  byte invalid_boolean = 2;
  const std::array<byte, 2> invalid_utf8{0xc0, 0x80};
  const std::array<byte, 24> invalid_decimal{};
  const std::array<dt::DatatypeBinaryValueView, 10> invalid{{
      {dt::CanonicalTypeId::boolean, false, false, &invalid_boolean, 1},
      {dt::CanonicalTypeId::int64, false, false, payload.data(), 3},
      {dt::CanonicalTypeId::character, true, false, payload.data(), 3},
      {dt::CanonicalTypeId::character, false, false, nullptr, 3},
      {dt::CanonicalTypeId::unknown, false, false, nullptr, 0},
      {dt::CanonicalTypeId::character, false, false, payload.data(), std::numeric_limits<std::size_t>::max()},
      {dt::CanonicalTypeId::null_type, false, false, nullptr, 0},
      {dt::CanonicalTypeId::character, false, false, invalid_utf8.data(), invalid_utf8.size()},
      {dt::CanonicalTypeId::decimal, false, false, invalid_decimal.data(), invalid_decimal.size()},
      {dt::CanonicalTypeId::int64, false, true, invalid_decimal.data(), 8}
  }};
  for (const auto& value : invalid) {
    std::array<byte, 64> destination; destination.fill(0x7e);
    const auto result = dt::EncodeDatatypeBinaryValueInto(value, destination.data(), destination.size());
    Check(!result.ok() && result.bytes_written == 0, "malformed borrowed payload accepted");
    Check(std::all_of(destination.begin(), destination.end(), [](byte b){return b == 0x7e;}),
          "invalid payload partially changed destination");
  }
}

void TypedNullBoundsAllocationAndConcurrency() {
  const dt::DatatypeBinaryValueView typed_null{
      dt::CanonicalTypeId::int64, true, false, nullptr, 0};
  std::array<byte, dt::kDatatypeBinaryEnvelopeHeaderBytes> encoded{};

  // The borrowed typed-NULL path needs framing storage only. It neither owns
  // nor allocates a value payload on encode or decode.
  borrowed_alloc::remaining = 0;
  borrowed_alloc::fault = false;
  borrowed_alloc::watch = true;
  bool escaped = false;
  dt::DatatypeBinaryViewResult encoded_result;
  dt::DatatypeBinaryDecodedViewResult decoded_result;
  try {
    encoded_result = dt::EncodeDatatypeBinaryValueInto(
        typed_null, encoded.data(), encoded.size());
    decoded_result = dt::DecodeDatatypeBinaryValueView(
        encoded.data(), encoded.size());
  } catch (const std::bad_alloc&) {
    escaped = true;
  }
  borrowed_alloc::watch = false;
  borrowed_alloc::remaining = -1;
  Check(!escaped && !borrowed_alloc::fault && encoded_result.ok() &&
            encoded_result.bytes_written == encoded.size() &&
            decoded_result.ok() &&
            decoded_result.value.type_id == dt::CanonicalTypeId::int64 &&
            decoded_result.value.is_null &&
            decoded_result.value.payload_bytes == 0,
        "typed NULL borrowed codec allocated or lost state");

  for (std::size_t size = 0; size < encoded.size(); ++size) {
    const auto truncated = dt::DecodeDatatypeBinaryValueView(
        size == 0 ? nullptr : encoded.data(), size);
    Check(!truncated.ok() &&
              truncated.value.type_id == dt::CanonicalTypeId::unknown &&
              truncated.value.payload_data == nullptr &&
              truncated.value.payload_bytes == 0,
          "truncated typed NULL exposed a decoded payload view");
  }

  auto impossible_length = encoded;
  platform::StoreLittle32(impossible_length.data() + 16,
                          std::numeric_limits<std::uint32_t>::max());
  const auto oversized = dt::DecodeDatatypeBinaryValueView(
      impossible_length.data(), impossible_length.size());
  Check(!oversized.ok() && oversized.value.payload_data == nullptr,
        "impossible typed NULL length exposed a value");

  const auto one = Bytes("x");
  std::array<byte, dt::kDatatypeBinaryEnvelopeHeaderBytes + 1> payload_null{};
  Check(dt::EncodeDatatypeBinaryValueInto(
            {dt::CanonicalTypeId::character, false, false, one.data(), one.size()},
            payload_null.data(), payload_null.size()).ok(),
        "payload-bearing NULL corruption fixture did not encode");
  platform::StoreLittle16(payload_null.data() + 12, 1);
  const auto invalid_null = dt::DecodeDatatypeBinaryValueView(
      payload_null.data(), payload_null.size());
  Check(!invalid_null.ok() &&
            invalid_null.diagnostic.diagnostic_code == "DATATYPE.NULL_STATE.INVALID" &&
            invalid_null.value.payload_data == nullptr,
        "payload-bearing typed NULL was admitted or exposed");

  const auto toast_null_encode = dt::ValidateDatatypeBinaryValueView(
      {dt::CanonicalTypeId::int64, true, true, nullptr, 0});
  Check(!toast_null_encode.ok() &&
            toast_null_encode.diagnostic.diagnostic_code ==
                "DATATYPE.NULL_STATE.INVALID",
        "typed NULL with a TOAST-reference flag was admitted");

  auto toast_null = encoded;
  platform::StoreLittle16(toast_null.data() + 12, 3);
  const auto toast_null_decode = dt::DecodeDatatypeBinaryValueView(
      toast_null.data(), toast_null.size());
  Check(!toast_null_decode.ok() &&
            toast_null_decode.diagnostic.diagnostic_code ==
                "DATATYPE.NULL_STATE.INVALID" &&
            toast_null_decode.value.type_id == dt::CanonicalTypeId::unknown &&
            toast_null_decode.value.payload_data == nullptr,
        "decoded typed NULL retained an impossible TOAST-reference state");

  constexpr unsigned kThreads = 8;
  constexpr unsigned kIterations = 1000;
  std::atomic<unsigned> thread_failures{0};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (unsigned thread = 0; thread < kThreads; ++thread) {
    threads.emplace_back([&] {
      for (unsigned iteration = 0; iteration < kIterations; ++iteration) {
        std::array<byte, dt::kDatatypeBinaryEnvelopeHeaderBytes> output{};
        const auto write = dt::EncodeDatatypeBinaryValueInto(
            typed_null, output.data(), output.size());
        const auto read = dt::DecodeDatatypeBinaryValueView(
            output.data(), output.size());
        if (!write.ok() || write.bytes_written != output.size() || !read.ok() ||
            read.value.type_id != dt::CanonicalTypeId::int64 ||
            !read.value.is_null || read.value.payload_bytes != 0) {
          ++thread_failures;
        }
      }
    });
  }
  for (auto& thread : threads) thread.join();
  Check(thread_failures.load() == 0,
        "concurrent read-only typed NULL codec use was nondeterministic");
}

void ProportionalAllocationAndFaults() {
  std::vector<byte> payload(262144, 'x'), destination(payload.size() + 32, 0x5a);
  const dt::DatatypeBinaryValueView view{dt::CanonicalTypeId::character, false, false,
                                        payload.data(), payload.size()};
  borrowed_alloc::reject_at = 1024; borrowed_alloc::watch = true;
  bool escaped = false; dt::DatatypeBinaryViewResult result;
  try { result = dt::EncodeDatatypeBinaryValueInto(view, destination.data(), destination.size()); }
  catch (...) { escaped = true; }
  borrowed_alloc::watch = false; borrowed_alloc::reject_at = 0;
  Check(!escaped && result.ok(), "borrowed encoder allocated proportional payload temporary");
  bool exhausted = false;
  for (long index = 0; index != 1024; ++index) {
    std::fill(destination.begin(), destination.end(), 0x5a);
    borrowed_alloc::remaining = index; borrowed_alloc::fault = false; borrowed_alloc::watch = true;
    escaped = false; result = {};
    try { result = dt::EncodeDatatypeBinaryValueInto(view, destination.data(), destination.size()); }
    catch (const std::bad_alloc&) { escaped = true; }
    borrowed_alloc::watch = false; borrowed_alloc::remaining = -1;
    const bool hit = borrowed_alloc::fault; allocation_faults += hit;
    Check((result.ok() && !escaped) ||
          std::all_of(destination.begin(), destination.end(), [](byte b){return b == 0x5a;}),
          "allocation failure partially modified borrowed destination");
    if (!hit) { exhausted = true; break; }
  }
  Check(exhausted, "borrowed allocation sweep never completed");

  // Numeric canonicalization and rich refusal construction may allocate bounded
  // metadata. Every sustained failure must still leave destination untouched.
  const auto decimal = scratchbird::libraries::sbl_numeric::EncodeExactDecimalLittleEndian(
      "-12345678901234567890123456789012345678");
  const std::array<dt::DatatypeBinaryValueView, 3> allocating_views{{
      {dt::CanonicalTypeId::decimal, false, false, decimal.canonical_bytes.data(), decimal.canonical_bytes.size()},
      {dt::CanonicalTypeId::character, true, false, payload.data(), 10},
      {dt::CanonicalTypeId::character, false, false, payload.data(), 10}
  }};
  for (unsigned mode = 0; mode != allocating_views.size(); ++mode) {
    bool complete = false;
    for (long index = 0; index != 1024; ++index) {
      std::array<byte, 64> output; output.fill(0x5a);
      result = {}; escaped = false;
      borrowed_alloc::remaining = index; borrowed_alloc::fault = false; borrowed_alloc::watch = true;
      try { result = dt::EncodeDatatypeBinaryValueInto(allocating_views[mode], output.data(), mode == 2 ? 1 : output.size()); }
      catch (const std::bad_alloc&) { escaped = true; }
      borrowed_alloc::watch = false; borrowed_alloc::remaining = -1;
      const bool hit = borrowed_alloc::fault; allocation_faults += hit;
      if (escaped || !result.ok())
        Check(std::all_of(output.begin(), output.end(), [](byte b){return b == 0x5a;}),
              "fallible numeric/error preparation changed destination");
      if (!hit) {
        Check(!escaped && result.ok() == (mode == 0), "terminal borrowed encode outcome");
        complete = true; break;
      }
    }
    Check(complete, "numeric/error allocation sweep never completed");
  }
}

void ActualTypedCellCopies() {
  const auto descriptor = Descriptor({TextColumn(0, "value", 0, 0x11)});
  const auto batch = Batch(descriptor, {Row(0, {Present(0, 0, std::vector<byte>(262144, 'x'))})});
  const auto binding = ExecuteBinding(batch);
  borrowed_alloc::exact_size = 262144; borrowed_alloc::exact_count = 0; borrowed_alloc::watch = true;
  const auto result = wire::EncodeTypedResultBatch(batch, descriptor, binding);
  borrowed_alloc::watch = false;
  Check(result.ok(), "actual typed cell serialization failed");
  // The owning compatibility result still returns one copy of the input batch.
  // Scalar validation and envelope encoding must not create additional copies.
  Check(borrowed_alloc::exact_count <= 1, "typed cell encoding still copies intermediate datatype payloads");
}
}
int main() {
  SharedCodecParity(); DestinationAtomicityAndAlias(); TypedNullBoundsAllocationAndConcurrency();
  ProportionalAllocationAndFaults(); ActualTypedCellCopies();
  std::cout << "borrowed datatype checks=" << checks << " allocation_faults=" << allocation_faults
            << " failures=" << failures << '\n';
  return failures ? 1 : 0;
}
