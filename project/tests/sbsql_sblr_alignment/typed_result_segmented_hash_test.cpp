// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#define main ExistingTypedResultFixtureMain
#include "typed_result_transport_codec_test.cpp"
#undef main
#include "hash_digest_parts.hpp"
#include <cstdlib>
#include <limits>
#include <new>
#include <sstream>
#include <stdexcept>
#include <fstream>
#include <cstdio>
#ifdef __linux__
#include <unistd.h>
#endif
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
void TimingSafeEqualityCases() {
  for (const std::size_t left : {0u, 1u, 31u, 32u, 255u, 256u, 257u, 511u, 512u, 1024u}) {
    for (const std::size_t right : {0u, 1u, 31u, 32u, 255u, 256u, 257u, 511u, 512u, 1024u}) {
      const std::string a(left, '\0'), b(right, '\0');
      const std::vector<byte> av(left, 0), bv(right, 0);
      Check(core_hash::ConstantTimeEqual(a, b) == (left == right), "text-view equality truncated length difference");
      Check(core_hash::ConstantTimeEqual(av, bv) == (left == right), "binary equality truncated length difference");
    }
  }
  const std::string value(513, '\0');
  const std::vector<byte> binary(513, 0);
  for (std::size_t i = 0; i < value.size(); ++i) {
    auto changed = value; changed[i] = static_cast<char>(0xff);
    auto changed_binary = binary; changed_binary[i] = 0xff;
    Check(!core_hash::ConstantTimeEqual(value, changed) && !core_hash::ConstantTimeEqual(changed, value),
        "text-view equality omitted a content byte");
    Check(!core_hash::ConstantTimeEqual(binary, changed_binary) && !core_hash::ConstantTimeEqual(changed_binary, binary),
        "binary equality omitted a content byte");
  }
}
void RawDigestExtentCases() {
  const byte one = 1;
  const auto zero_output = [](const core_hash::HashDigestResult& result) {
    return !result.ok() && result.digest_bytes == 0 &&
        std::all_of(result.digest.begin(), result.digest.end(), [](byte b) { return b == 0; });
  };
  Check(zero_output(core_hash::ComputeSha256Digest(nullptr, 1)), "raw SHA256 accessed missing payload");
  Check(zero_output(core_hash::ComputeHmacSha256Digest(&one, 1, nullptr, 1)), "HMAC accessed missing payload");
  if (std::numeric_limits<std::size_t>::max() > std::numeric_limits<std::uint64_t>::max() / 8) {
    const auto excessive = static_cast<std::size_t>(std::numeric_limits<std::uint64_t>::max() / 8 + 1);
    Check(zero_output(core_hash::ComputeSha256Digest(&one, excessive)), "raw SHA256 admitted bit length overflow");
    Check(zero_output(core_hash::ComputeHmacSha256Digest(&one, 1, &one, excessive)), "HMAC admitted bit length overflow");
    Check(zero_output(core_hash::ComputeHmacSha256Digest(&one, excessive, nullptr, 0)), "HMAC admitted oversized key digest extent");
    Check(zero_output(core_hash::ComputeHmacSha256Digest(&one, 1, &one, excessive - 64)), "HMAC omitted inner pad from bit length bound");
  }
  const std::vector<byte> key(20, 0x0b);
  const auto hmac = core_hash::ComputeHmacSha256Digest(key, Bytes("Hi There"));
  Check(hmac.ok() && core_hash::HexLower(hmac.digest) ==
      "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7",
      "independent RFC4231 HMAC vector drifted");
  const auto large_key = core_hash::ComputeHmacSha256Digest(std::vector<byte>(131, 0xaa),
      Bytes("Test Using Larger Than Block-Size Key - Hash Key First"));
  Check(large_key.ok() && core_hash::HexLower(large_key.digest) ==
      "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54",
      "independent RFC4231 large-key prehash vector drifted");
}
class FailingHashStream final : public std::stringbuf {
 public:
  FailingHashStream(const std::string& bytes, unsigned read, bool end)
      : std::stringbuf(bytes), fail_read(read), fail_end(end) {}
  unsigned read_calls = 0;
 private:
  unsigned fail_read;
  bool fail_end;
  std::streamsize xsgetn(char* output, std::streamsize size) override {
    if (++read_calls == fail_read) throw std::runtime_error("injected read failure");
    return std::stringbuf::xsgetn(output, size);
  }
  int_type underflow() override {
    if (fail_end) throw std::runtime_error("injected EOF probe failure");
    return std::stringbuf::underflow();
  }
};
void DigestStreamCases() {
  const auto refused = [](const core_hash::HashDigestResult& result) {
    return !result.ok() && result.digest_bytes == 0 &&
        result.diagnostic.diagnostic_code == "SB-CORE-HASH-SHA256-FAILED" &&
        std::all_of(result.digest.begin(), result.digest.end(), [](byte b) { return b == 0; });
  };
  for (const auto& [message, expected] : std::vector<std::pair<std::string, std::string>>{
       {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
       {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
       {std::string(1000000, 'a'), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"}}) {
    for (bool exceptions : {false, true}) {
      std::istringstream input(message);
      if (exceptions) input.exceptions(std::ios::badbit | std::ios::failbit | std::ios::eofbit);
      const auto mask = input.exceptions();
      hash_alloc::watch = true; hash_alloc::reject_at = 1024;
      bool escaped = false; core_hash::HashDigestResult result;
      try { result = core_hash::ComputeSha256Stream(input, message.size()); } catch (...) { escaped = true; }
      hash_alloc::watch = false; hash_alloc::reject_at = 0;
      Check(!escaped && result.ok() && result.digest_bytes == 32 && core_hash::HexLower(result.digest) == expected &&
          input.eof() && input.exceptions() == mask, "stream standard vector, bounded allocation or exception-mask contract");
    }
  }
  for (const std::size_t size : {1u, 63u, 64u, 65u, 4096u, 8193u, 65535u, 65536u, 65537u, 196609u}) {
    std::string bytes(size, '\0');
    for (std::size_t i = 0; i < size; ++i) bytes[i] = static_cast<char>((i * 17) & 255);
    const auto expected = core_hash::ComputeSha256Digest(reinterpret_cast<const byte*>(bytes.data()), bytes.size());
    std::istringstream input(bytes);
    const auto actual = core_hash::ComputeSha256Stream(input, size);
    Check(expected.ok() && actual.ok() && expected.digest == actual.digest, "stream buffer boundary differs from independent one-shot path");
    bytes[size / 2] ^= 1;
    std::istringstream changed(bytes);
    const auto middle = core_hash::ComputeSha256Stream(changed, size);
    Check(middle.ok() && middle.digest != expected.digest, "middle-byte mutation escaped full artifact digest");
    for (bool exceptions : {false, true}) {
      for (const auto extent : {size - 1, size + 1}) {
        std::istringstream wrong(bytes);
        if (exceptions) wrong.exceptions(std::ios::badbit | std::ios::failbit | std::ios::eofbit);
        Check(refused(core_hash::ComputeSha256Stream(wrong, extent)), "short or trailing stream admitted partial digest");
      }
    }
  }
  const std::string long_input(196609, 'x');
  for (unsigned read = 0; read <= 4; ++read) {
    for (bool exceptions : {false, true}) {
      FailingHashStream buffer(long_input, read, read == 0);
      std::istream input(&buffer);
      if (exceptions) input.exceptions(std::ios::badbit | std::ios::failbit | std::ios::eofbit);
      Check(refused(core_hash::ComputeSha256Stream(input, long_input.size())), "stream read/EOF failure exposed partial digest");
    }
  }
  FailingHashStream unused("abc", 1, false);
  std::istream excessive(&unused);
  Check(refused(core_hash::ComputeSha256Stream(excessive, std::numeric_limits<std::uint64_t>::max() / 8 + 1)) &&
      unused.read_calls == 0, "excessive SHA256 extent read untrusted input");
  std::istringstream bad("abc"); bad.setstate(std::ios::badbit);
  Check(refused(core_hash::ComputeSha256Stream(bad, 3)), "preexisting stream failure admitted digest");
  std::istringstream offset("skipabc"); offset.seekg(4);
  Check(core_hash::HexLower(core_hash::ComputeSha256Stream(offset, 3).digest) ==
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "stream primitive rewound caller position");
#ifdef SB_SEGMENTED_HASH_BACKEND_FAULTS
  for (const auto point : {backend_fault::Point::create, backend_fault::Point::initialize,
                          backend_fault::Point::update, backend_fault::Point::finalize}) {
    for (unsigned target = 1; target <= (point == backend_fault::Point::update ? 4u : 1u); ++target) {
      std::istringstream input(long_input);
      backend_fault::point = point; backend_fault::update_target = target; backend_fault::update_calls = 0;
      const auto result = core_hash::ComputeSha256Stream(input, long_input.size());
      backend_fault::point = backend_fault::Point::none;
      Check(refused(result) && backend_fault::active_contexts == 0, "stream backend failure leaked output/context");
      std::istringstream retry("abc");
      Check(core_hash::ComputeSha256Stream(retry, 3).ok(), "stream backend failure poisoned retry");
    }
  }
#endif
}
void ActualFileDigestCases() {
#ifdef __linux__
  char path[] = "/tmp/sb-full-artifact-hash.XXXXXX";
  const int fd = ::mkstemp(path);
  Check(fd >= 0, "create exclusive full artifact digest fixture");
  if (fd < 0) return;
  ::close(fd);
  struct Cleanup {
    const char* path;
    ~Cleanup() { Check(std::remove(path) == 0, "remove generated full artifact fixture"); }
  } cleanup{path};
  const std::string bytes(262145, 'x');
  {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), bytes.size()); output.close();
    Check(!output.fail(), "write actual full artifact digest fixture");
  }
  const auto expected = core_hash::ComputeSha256Digest(reinterpret_cast<const byte*>(bytes.data()), bytes.size());
  {
    std::ifstream input(path, std::ios::binary);
    const auto actual = core_hash::ComputeSha256Stream(input, bytes.size());
    Check(expected.ok() && actual.ok() && expected.digest == actual.digest, "reopened physical artifact digest mismatch");
  }
  {
    std::fstream mutation(path, std::ios::binary | std::ios::in | std::ios::out);
    mutation.seekp(131072); mutation.put('y'); mutation.close();
    Check(!mutation.fail(), "mutate physical artifact middle byte");
  }
  {
    std::ifstream input(path, std::ios::binary);
    const auto actual = core_hash::ComputeSha256Stream(input, bytes.size());
    Check(actual.ok() && expected.digest != actual.digest, "physical middle corruption escaped full digest");
  }
  {
    std::ofstream append(path, std::ios::binary | std::ios::app);
    append.put('z'); append.close();
    Check(!append.fail(), "extend physical artifact fixture");
    std::ifstream input(path, std::ios::binary);
    const auto actual = core_hash::ComputeSha256Stream(input, bytes.size());
    Check(!actual.ok() && actual.digest_bytes == 0, "physical artifact extension passed captured extent");
  }
  {
    std::ofstream truncate(path, std::ios::binary | std::ios::trunc);
    truncate.put('x'); truncate.close();
    Check(!truncate.fail(), "truncate physical artifact fixture");
    std::ifstream input(path, std::ios::binary);
    const auto actual = core_hash::ComputeSha256Stream(input, bytes.size());
    Check(!actual.ok() && actual.digest_bytes == 0, "physical artifact truncation passed captured extent");
  }
#endif
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
  TimingSafeEqualityCases();
  RawDigestExtentCases();
  DigestStreamCases();
  ActualFileDigestCases();
  Check(ExistingTypedResultFixtureMain() == 0,
        "independent descriptor and batch evidence fixture");
  DigestVectorsAndBoundaries(); ActualCodecHashCopies(); BackendFailureOwnership();
  std::cout << "segmented evidence checks=" << checks << " failures=" << failures << '\n';
  return failures ? 1 : 0;
}
