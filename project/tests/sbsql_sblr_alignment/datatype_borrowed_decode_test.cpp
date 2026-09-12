// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#define main ExistingTypedResultFixtureMain
#include "typed_result_transport_codec_test.cpp"
#undef main
#include "datatype_binary_view.hpp"
#include "sbl_numeric.hpp"
#include <limits>
#include <array>
#include <cstring>
#include <cstdlib>
#include <memory_resource>
#include <new>
#include <optional>
#include <type_traits>

namespace buffer_fault {
thread_local long remaining = -1;
thread_local bool hit = false, watch = false;
thread_local std::size_t exact = 0, count = 0, reject_size = 0;
void Arm(long point) { remaining = point; hit = false; }
void Off() { remaining = -1; watch = false; reject_size = 0; }
}
void* operator new(std::size_t bytes) {
  if (buffer_fault::watch && bytes == buffer_fault::exact) ++buffer_fault::count;
  if ((buffer_fault::remaining >= 0 && buffer_fault::remaining-- == 0) ||
      (buffer_fault::reject_size && bytes >= buffer_fault::reject_size)) {
    buffer_fault::remaining = 0; buffer_fault::hit = true; throw std::bad_alloc();
  }
  if (auto* p = std::malloc(bytes ? bytes : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept {
  try { return ::operator new(bytes); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t bytes, const std::nothrow_t&) noexcept {
  return ::operator new(bytes, std::nothrow);
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }

namespace {
unsigned checks = 0, failures = 0, injected = 0;
void Check(bool good, const char* why) {
  ++checks;
  if (!good) { ++failures; std::cerr << "FAIL " << why << '\n'; }
}

void ExistingDecodeCanonicality() {
  const auto encoded = datatypes::EncodeDatatypeBinaryValue(
      {datatypes::CanonicalTypeId::character, false, false, Bytes("abc")});
  Check(encoded.ok(), "scalar fixture");
  for (unsigned bit = 2; bit != 16; ++bit) {
    auto changed = encoded.encoded;
    platform::StoreLittle16(changed.data() + 12, static_cast<platform::u16>(1u << bit));
    const auto rejected = datatypes::DecodeDatatypeBinaryValue(changed);
    Check(!rejected.ok() && rejected.value.payload.empty() && rejected.encoded.empty(),
          "general scalar decoder accepted unknown flags");
  }
  for (unsigned bit = 0; bit != 32; ++bit) {
    auto changed = encoded.encoded;
    platform::StoreLittle32(changed.data() + 20, 1u << bit);
    const auto rejected = datatypes::DecodeDatatypeBinaryValue(changed);
    Check(!rejected.ok() && rejected.value.payload.empty() && rejected.encoded.empty(),
          "general scalar decoder accepted nonzero reserved bits");
  }
}
void ActualDecodeCopies() {
  const auto descriptor = Descriptor({TextColumn(0, "value", 0, 0x11)});
  const auto batch = Batch(descriptor, {Row(0, {Present(0, 0, std::vector<byte>(65537, 'x'))})});
  const auto binding = ExecuteBinding(batch);
  const auto encoded = wire::EncodeTypedResultBatch(batch, descriptor, binding);
  buffer_fault::exact = 32 + 65537; buffer_fault::count = 0; buffer_fault::watch = true;
  const auto decoded = wire::DecodeTypedResultBatch(encoded.encoded, descriptor, binding);
  buffer_fault::Off();
  Check(decoded.ok() && decoded.batch.rows[0].cells[0].canonical_payload == batch.rows[0].cells[0].canonical_payload,
        "actual typed decoder changed values");
  Check(buffer_fault::count == 0, "actual typed decoder still materializes scalar envelopes");
}
}
namespace {

std::vector<byte> IndependentAbcEnvelope() {
  std::vector<byte> bytes(35, 0);
  std::memcpy(bytes.data(), "SBDVAL01", 8);
  platform::StoreLittle32(bytes.data() + 8, 300); // Core character type code.
  platform::StoreLittle16(bytes.data() + 14, 32);
  platform::StoreLittle32(bytes.data() + 16, 3);
  platform::StoreLittle64(bytes.data() + 24, 0xe16801510db89efdULL);
  bytes[32] = 'a'; bytes[33] = 'b'; bytes[34] = 'c';
  return bytes;
}
void BorrowedGoldenAndCorruption() {
  const auto golden = IndependentAbcEnvelope();
  const auto decoded = datatypes::DecodeDatatypeBinaryValueView(golden.data(), golden.size());
  Check(decoded.ok() && decoded.value.type_id == datatypes::CanonicalTypeId::character &&
        decoded.value.payload_data == golden.data() + 32 && decoded.value.payload_bytes == 3 &&
        !decoded.value.is_null && !decoded.value.payload_is_toast_reference,
        "independent golden borrowed decode");
  for (std::size_t size = 0; size < golden.size(); ++size) {
    const auto bad = datatypes::DecodeDatatypeBinaryValueView(golden.data(), size);
    Check(!bad.ok() && bad.value.payload_data == nullptr && bad.value.payload_bytes == 0,
          "truncated scalar exposed partial borrowed payload");
  }
  for (std::size_t size : {0u, 31u, 32u, 35u}) {
    const auto bad = datatypes::DecodeDatatypeBinaryValueView(nullptr, size);
    Check(!bad.ok() && bad.value.payload_data == nullptr, "null scalar pointer admitted");
  }
  if (std::numeric_limits<std::size_t>::max() > std::numeric_limits<platform::u32>::max()) {
    const auto bad = datatypes::DecodeDatatypeBinaryValueView(golden.data(), std::numeric_limits<std::size_t>::max());
    Check(!bad.ok() && bad.value.payload_data == nullptr, "overflow extent read nonexistent payload");
  }
  for (std::size_t offset = 0; offset != golden.size(); ++offset) {
    // A changed canonical type can legitimately describe the same raw payload;
    // that mismatch belongs to the consuming descriptor, not this generic API.
    if (offset >= 8 && offset < 14) continue;
    for (unsigned bit = 0; bit != 8; ++bit) {
      auto changed = golden; changed[offset] ^= static_cast<byte>(1u << bit);
      const auto before = changed;
      const auto bad = datatypes::DecodeDatatypeBinaryValueView(changed.data(), changed.size());
      Check(!bad.ok() && bad.value.payload_data == nullptr && changed == before,
            "independent corrupt scalar admitted or input mutated");
      if (offset >= 20 && offset < 24)
        Check(bad.diagnostic.diagnostic_code == "DATATYPE.DESCRIPTOR.INVALID",
              "reserved field refusal invented diagnostic spelling");
    }
  }
  for (unsigned bit = 2; bit != 16; ++bit) {
    auto changed = golden;
    platform::StoreLittle16(changed.data() + 12, static_cast<platform::u16>(1u << bit));
    const auto bad = datatypes::DecodeDatatypeBinaryValueView(changed.data(), changed.size());
    Check(!bad.ok() && bad.value.payload_data == nullptr &&
          bad.diagnostic.diagnostic_code == "DATATYPE.DESCRIPTOR.INVALID", "unknown flags not canonically refused");
  }
  for (unsigned extra = 1; extra != 5; ++extra) {
    auto changed = golden; changed.resize(changed.size() + extra);
    const auto bad = datatypes::DecodeDatatypeBinaryValueView(changed.data(), changed.size());
    Check(!bad.ok() && bad.value.payload_data == nullptr, "trailing bytes admitted");
  }
}
void BorrowedValueProfiles() {
  using Type = datatypes::CanonicalTypeId;
  std::vector<datatypes::DatatypeBinaryValue> values{
      {Type::character, false, false, {}}, {Type::character, true, false, {}},
      {Type::character, false, false, {0, 'a', ';', '=', 0}},
      {Type::binary, false, false, {0xff, 0, 0x80}},
      {Type::boolean, false, false, {0}}, {Type::boolean, false, false, {1}},
      {Type::int32, false, false, {0xff, 0xff, 0xff, 0x7f}},
      {Type::int64, true, false, {}},
      {Type::character, false, true, std::vector<byte>(16, 0xab)}};
  for (unsigned version = 1; version <= 7; ++version) {
    std::vector<byte> uuid(16, 0xab); uuid[6] = static_cast<byte>(version << 4); uuid[8] = 0x80;
    values.push_back({Type::uuid, false, false, uuid});
  }
  std::vector<byte> wide(16, 0xff); wide.back() = 0x7f;
  values.push_back({Type::int128, false, false, wide});
  std::fill(wide.begin(), wide.end(), 0); wide.back() = 0x80;
  values.push_back({Type::int128, false, false, wide});
  for (const char* text : {"0", "12.5", "-12345678901234567890123456789012345678"}) {
    const auto decimal = scratchbird::libraries::sbl_numeric::EncodeExactDecimalLittleEndian(text);
    Check(decimal.ok, "exact decimal setup");
    values.push_back({Type::decimal, false, false,
                     {decimal.canonical_bytes.begin(), decimal.canonical_bytes.end()}});
  }
  for (const auto& value : values) {
    const auto encoded = datatypes::EncodeDatatypeBinaryValue(value);
    Check(encoded.ok(), "profile encode setup");
    if (!encoded.ok()) continue;
    const auto view = datatypes::DecodeDatatypeBinaryValueView(encoded.encoded.data(), encoded.encoded.size());
    const auto owned = datatypes::DecodeDatatypeBinaryValue(encoded.encoded);
    Check(view.ok() && owned.ok() && view.value.type_id == value.type_id &&
          view.value.is_null == value.is_null && view.value.payload_is_toast_reference == value.payload_is_toast_reference &&
          view.value.payload_data == encoded.encoded.data() + 32 && view.value.payload_bytes == value.payload.size() &&
          std::equal(value.payload.begin(), value.payload.end(), view.value.payload_data),
          "borrowed decoding changed NULL/TOAST/decimal/int128/user UUID value role or bytes");
    std::vector<byte> reencoded(encoded.encoded.size());
    const auto encoded_again = datatypes::EncodeDatatypeBinaryValueInto(view.value, reencoded.data(), reencoded.size());
    Check(encoded_again.ok() && reencoded == encoded.encoded && owned.encoded == encoded.encoded &&
          owned.value.payload == value.payload, "borrowed canonical header/payload re-encode mismatch");
  }
  const auto large = datatypes::EncodeDatatypeBinaryValue({Type::character, false, false, std::vector<byte>(65537, 'x')});
  buffer_fault::reject_size = 1024;
  bool escaped = false; datatypes::DatatypeBinaryDecodedViewResult view;
  try { view = datatypes::DecodeDatatypeBinaryValueView(large.encoded.data(), large.encoded.size()); }
  catch (...) { escaped = true; }
  buffer_fault::Off();
  Check(!escaped && view.ok() && view.value.payload_data == large.encoded.data() + 32,
        "borrowed scalar decoder materialized a proportional payload copy");
  buffer_fault::exact = 65537; buffer_fault::count = 0; buffer_fault::watch = true;
  const auto owned = datatypes::DecodeDatatypeBinaryValue(large.encoded);
  buffer_fault::Off();
  Check(owned.ok() && buffer_fault::count == 1, "owning scalar decoder copied payload more than once");
}
void DecodeAllocationFailures() {
  for (unsigned mode = 0; mode != 3; ++mode) {
    auto encoded = IndependentAbcEnvelope();
    if (mode == 1) encoded[20] = 1;
    if (mode == 2) encoded[32] ^= 1;
    const auto original = encoded;
    bool complete = false;
    for (long point = 0; point != 512 && !complete; ++point) {
      std::optional<datatypes::DatatypeBinaryDecodedViewResult> result;
      buffer_fault::Arm(point);
      try { result.emplace(datatypes::DecodeDatatypeBinaryValueView(encoded.data(), encoded.size())); }
      catch (const std::bad_alloc&) {}
      const bool hit = buffer_fault::hit; buffer_fault::Off();
      if (hit) ++injected;
      else { complete = true; Check(result && result->ok() == (mode == 0), "unfaulted decode result mismatch"); }
      Check(encoded == original, "faulted decode mutated input");
      if (result && !result->ok())
        Check(result->value.payload_data == nullptr && result->value.payload_bytes == 0, "faulted refusal exposed payload");
      const auto good = IndependentAbcEnvelope();
      const auto retry = datatypes::DecodeDatatypeBinaryValueView(good.data(), good.size());
      Check(retry.ok(), "faulted decoder poisoned retry");
    }
    Check(complete, "borrowed decoder allocation sweep incomplete");
  }
}

}
int main() {
  Check(ExistingTypedResultFixtureMain() == 0, "existing independent typed fixtures");
  ExistingDecodeCanonicality(); ActualDecodeCopies();
  BorrowedGoldenAndCorruption(); BorrowedValueProfiles(); DecodeAllocationFailures();
  std::cout << "borrowed decode checks=" << checks << " faults=" << injected
            << " failures=" << failures << '\n';
  return failures ? 1 : 0;
}
