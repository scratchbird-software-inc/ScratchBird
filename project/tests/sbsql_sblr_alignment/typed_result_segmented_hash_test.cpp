// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#define main ExistingTypedResultFixtureMain
#include "typed_result_transport_codec_test.cpp"
#undef main
#include "hash_digest_parts.hpp"
#include <cstdlib>
#include <limits>
#include <new>
#ifdef SB_SEGMENTED_HASH_BACKEND_FAULTS
#include <openssl/evp.h>
namespace backend_fault {
enum class Point { none, create, initialize, update, finalize };
thread_local Point point = Point::none;
thread_local unsigned update_target = 1, update_calls = 0, active_contexts = 0;
}
extern "C" EVP_MD_CTX* __real_EVP_MD_CTX_new();
extern "C" void __real_EVP_MD_CTX_free(EVP_MD_CTX*);
extern "C" int __real_EVP_DigestInit_ex(EVP_MD_CTX*, const EVP_MD*, ENGINE*);
extern "C" int __real_EVP_DigestUpdate(EVP_MD_CTX*, const void*, std::size_t);
extern "C" int __real_EVP_DigestFinal_ex(EVP_MD_CTX*, unsigned char*, unsigned int*);
extern "C" EVP_MD_CTX* __wrap_EVP_MD_CTX_new() {
  if (backend_fault::point == backend_fault::Point::create) return nullptr;
  auto* result = __real_EVP_MD_CTX_new();
  if (result) ++backend_fault::active_contexts;
  return result;
}
extern "C" void __wrap_EVP_MD_CTX_free(EVP_MD_CTX* context) {
  if (context) --backend_fault::active_contexts;
  __real_EVP_MD_CTX_free(context);
}
extern "C" int __wrap_EVP_DigestInit_ex(EVP_MD_CTX* context, const EVP_MD* digest, ENGINE* engine) {
  if (backend_fault::point == backend_fault::Point::initialize) return 0;
  return __real_EVP_DigestInit_ex(context, digest, engine);
}
extern "C" int __wrap_EVP_DigestUpdate(EVP_MD_CTX* context, const void* data, std::size_t size) {
  if (backend_fault::point == backend_fault::Point::update &&
      ++backend_fault::update_calls == backend_fault::update_target) return 0;
  return __real_EVP_DigestUpdate(context, data, size);
}
extern "C" int __wrap_EVP_DigestFinal_ex(EVP_MD_CTX* context, unsigned char* output, unsigned int* size) {
  if (backend_fault::point == backend_fault::Point::finalize) {
    std::fill_n(output, 32, 0xa5); *size = 32;
    return 0; // Partial backend output must never escape as a valid digest.
  }
  return __real_EVP_DigestFinal_ex(context, output, size);
}
#endif

namespace hash_alloc {
thread_local bool watch = false;
thread_local std::size_t exact_size = 0, exact_count = 0, reject_at = 0;
}
void* operator new(std::size_t bytes) {
  if (hash_alloc::watch) {
    if (bytes == hash_alloc::exact_size) ++hash_alloc::exact_count;
    if (hash_alloc::reject_at && bytes >= hash_alloc::reject_at) throw std::bad_alloc();
  }
  if (auto* pointer = std::malloc(bytes ? bytes : 1)) return pointer;
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
unsigned checks = 0, failures = 0;
void Check(bool good, const char* why) {
  ++checks; if (!good) { ++failures; std::cerr << "FAIL " << why << '\n'; }
}
void DigestVectorsAndBoundaries() {
  const auto empty = core_hash::ComputeSha256DigestParts(nullptr, 0);
  Check(empty.ok() && core_hash::HexLower(empty.digest) ==
      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
      "independent empty SHA256 vector");
  const auto abc = Bytes("abc");
  for (std::size_t first = 0; first <= abc.size(); ++first)
    for (std::size_t second = first; second <= abc.size(); ++second) {
      const core_hash::HashDigestSegment segments[] = {
          {abc.data(), first}, {nullptr, 0}, {abc.data() + first, second - first},
          {abc.data() + second, abc.size() - second}};
      const auto result = core_hash::ComputeSha256DigestParts(segments, 4);
      Check(result.ok() && result.digest_bytes == 32 && core_hash::HexLower(result.digest) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "independent abc SHA256 partition vector");
    }
  std::vector<byte> message(257);
  for (std::size_t i = 0; i != message.size(); ++i) message[i] = static_cast<byte>(i);
  const auto reference = core_hash::ComputeSha256Digest(message);
  for (std::size_t split = 0; split <= message.size(); ++split) {
    const core_hash::HashDigestSegment segments[] = {
        {message.data(), split}, {message.data() + split, message.size() - split}};
    const auto result = core_hash::ComputeSha256DigestParts(segments, 2);
    Check(result.ok() && result.digest == reference.digest, "SHA256 block boundary partition changed digest");
  }
  std::vector<byte> million(1000000, 'a');
  const core_hash::HashDigestSegment large[] = {{million.data(), 1},
      {million.data() + 1, 65535}, {million.data() + 65536, million.size() - 65536}};
  hash_alloc::watch = true; hash_alloc::reject_at = 1024;
  bool escaped = false; core_hash::HashDigestResult result;
  try { result = core_hash::ComputeSha256DigestParts(large, 3); } catch (...) { escaped = true; }
  hash_alloc::watch = false; hash_alloc::reject_at = 0;
  Check(!escaped && result.ok() && core_hash::HexLower(result.digest) ==
      "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
      "million-byte independent digest or proportional allocation");
  Check(!core_hash::ComputeSha256DigestParts(nullptr, 1).ok(), "missing segment array accepted");
  const core_hash::HashDigestSegment missing{nullptr, 1};
  Check(!core_hash::ComputeSha256DigestParts(&missing, 1).ok(), "nonempty null segment accepted");
  if (std::numeric_limits<std::size_t>::max() > std::numeric_limits<std::uint64_t>::max() / 8) {
    const core_hash::HashDigestSegment overflow[] = {
        {abc.data(), static_cast<std::size_t>(std::numeric_limits<std::uint64_t>::max() / 8)},
        {abc.data(), 1}};
    Check(!core_hash::ComputeSha256DigestParts(overflow, 2).ok(),
          "SHA256 bit-length overflow not refused before borrowed payload access");
  }
}

void ActualCodecHashCopies() {
  const auto descriptor = Descriptor({TextColumn(0, std::string(997, 'n'), 0, 0x11)});
  const auto descriptor_bytes = wire::EncodeTypedResultRowDescriptor(descriptor);
  Check(descriptor_bytes.ok(), "descriptor setup");
  if (!descriptor_bytes.ok()) return;
  hash_alloc::exact_size = descriptor_bytes.encoded.size() + kDescriptorDomain.size();
  hash_alloc::exact_count = 0; hash_alloc::watch = true;
  const auto encoded_descriptor = wire::EncodeTypedResultRowDescriptor(descriptor);
  const auto decoded_descriptor = wire::DecodeTypedResultRowDescriptor(descriptor_bytes.encoded);
  hash_alloc::watch = false;
  Check(encoded_descriptor.ok() && decoded_descriptor.ok(), "descriptor segmented evidence roundtrip");
  Check(hash_alloc::exact_count == 0, "descriptor evidence concatenation still allocated");
  Check(encoded_descriptor.encoded == descriptor_bytes.encoded, "descriptor evidence bytes changed");

  const auto batch = Batch(descriptor, {Row(0, {Present(0, 0, std::vector<byte>(262144, 'x'))})});
  const auto binding = ExecuteBinding(batch);
  const auto bytes = wire::EncodeTypedResultBatch(batch, descriptor, binding);
  Check(bytes.ok(), "batch setup");
  if (!bytes.ok()) return;
  const auto before = bytes.encoded;
  hash_alloc::exact_size = bytes.encoded.size() + kBatchDomain.size();
  hash_alloc::exact_count = 0; hash_alloc::watch = true;
  const auto encoded = wire::EncodeTypedResultBatch(batch, descriptor, binding);
  const auto decoded = wire::DecodeTypedResultBatch(bytes.encoded, descriptor, binding);
  hash_alloc::watch = false;
  Check(encoded.ok() && decoded.ok(), "batch segmented evidence roundtrip");
  Check(hash_alloc::exact_count == 0, "batch evidence concatenation still allocated");
  Check(encoded.encoded == before && bytes.encoded == before, "hash verification changed packet bytes");

  auto corrupt = bytes.encoded; corrupt[192] ^= 1;
  const auto corrupt_before = corrupt;
  hash_alloc::reject_at = 1024; hash_alloc::watch = true;
  bool escaped = false; wire::TypedResultBatchCodecResult refused;
  try { refused = wire::DecodeTypedResultBatch(corrupt, descriptor, binding); }
  catch (...) { escaped = true; }
  hash_alloc::watch = false; hash_alloc::reject_at = 0;
  Check(!escaped, "corrupt packet hash validation made a whole-packet allocation");
  Check(refused.status == wire::TypedResultCodecStatus::evidence_mismatch &&
        refused.encoded.empty() && refused.batch.rows.empty(), "corrupt hash did not refuse before rows");
  Check(corrupt == corrupt_before, "corrupt borrowed packet mutated during validation");
}

void BackendFailureOwnership() {
#ifdef SB_SEGMENTED_HASH_BACKEND_FAULTS
  const auto abc = Bytes("abc");
  const core_hash::HashDigestSegment segments[] = {{abc.data(), 1},
      {abc.data() + 1, 1}, {abc.data() + 2, 1}};
  const auto descriptor = Descriptor({TextColumn(0, "value", 0, 0x11)});
  const auto batch = Batch(descriptor, {Row(0, {Present(0, 0, Bytes("abc"))})});
  const auto binding = ExecuteBinding(batch);
  for (const auto point : {backend_fault::Point::create, backend_fault::Point::initialize,
                          backend_fault::Point::update, backend_fault::Point::finalize}) {
    for (unsigned target = 1; target <= (point == backend_fault::Point::update ? 3u : 1u); ++target) {
      backend_fault::point = point; backend_fault::update_target = target; backend_fault::update_calls = 0;
      const auto failed = core_hash::ComputeSha256DigestParts(segments, 3);
      backend_fault::point = backend_fault::Point::none;
      Check(!failed.ok() && failed.digest_bytes == 0 &&
            std::all_of(failed.digest.begin(), failed.digest.end(), [](byte b){return b == 0;}),
            "backend failure exposed partial or successful digest");
      Check(backend_fault::active_contexts == 0, "failed digest leaked its real backend context");
      const auto retry = core_hash::ComputeSha256DigestParts(segments, 3);
      Check(retry.ok() && core_hash::HexLower(retry.digest) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "backend refusal poisoned subsequent digest");
    }
    backend_fault::point = point; backend_fault::update_target = 2; backend_fault::update_calls = 0;
    const auto refused = wire::EncodeTypedResultBatch(batch, descriptor, binding);
    backend_fault::point = backend_fault::Point::none;
    Check(!refused.ok() && refused.encoded.empty() && refused.batch.rows.empty(),
          "typed serializer published despite actual hash-boundary refusal");
    Check(backend_fault::active_contexts == 0, "typed hash refusal leaked backend context");
  }
#endif
}
}
int main() {
  Check(ExistingTypedResultFixtureMain() == 0,
        "independent descriptor and batch evidence fixture");
  DigestVectorsAndBoundaries(); ActualCodecHashCopies(); BackendFailureOwnership();
  std::cout << "segmented evidence checks=" << checks << " failures=" << failures << '\n';
  return failures ? 1 : 0;
}
