#include "sblr_dml_counter_add_runtime.hpp"

#include <cstring>

namespace scratchbird::engine::sblr {
namespace {
void PutU64(std::vector<std::uint8_t>& b, std::size_t o, std::uint64_t v) {
  for (unsigned i = 0; i != 8; ++i) b[o + i] = static_cast<std::uint8_t>(v >> (8 * i));
}
std::uint64_t GetU64(const std::uint8_t* p, std::size_t o) {
  std::uint64_t v = 0;
  for (unsigned i = 0; i != 8; ++i) v |= static_cast<std::uint64_t>(p[o + i]) << (8 * i);
  return v;
}
bool NonZero(const std::uint8_t* p, std::size_t n) {
  for (std::size_t i = 0; i != n; ++i) if (p[i] != 0) return true;
  return false;
}
bool Header(const std::uint8_t* p, std::size_t n, const char* magic,
            std::size_t expected, std::string* d) {
  if (!p || n != expected || std::memcmp(p, magic, 4) != 0) {
    if (d) *d = "counter_add_wire_invalid";
    return false;
  }
  if (p[4] != 1 || p[5] || p[6] || p[7]) {
    if (d) *d = "counter_add_header_invalid";
    return false;
  }
  return true;
}
}

std::vector<std::uint8_t> EncodeSblrDmlCounterAddRequestV1(const SblrDmlCounterAddRequestV1& v) {
  std::vector<std::uint8_t> b(64, 0); std::memcpy(b.data(), "CARQ", 4); b[4] = 1;
  std::memcpy(b.data() + 16, v.receipt.data(), 16); PutU64(b, 32, v.occurrence); PutU64(b, 40, v.counter_occurrence); return b;
}
bool DecodeSblrDmlCounterAddRequestV1(const std::uint8_t* p, std::size_t n,
                                      SblrDmlCounterAddRequestV1* o, std::string* d) {
  if (!o || !Header(p, n, "CARQ", 64, d) || !NonZero(p + 16, 16) || !GetU64(p, 32) || !GetU64(p, 40)) {
    if (d && p && n == 64 && std::memcmp(p, "CARQ", 4) == 0) *d = "counter_add_request_identity_invalid";
    return false;
  }
  std::memcpy(o->receipt.data(), p + 16, 16); o->occurrence = GetU64(p, 32); o->counter_occurrence = GetU64(p, 40); return true;
}

std::vector<std::uint8_t> EncodeSblrDmlCounterAddDescriptorV1(const SblrDmlCounterAddDescriptorV1& v, bool operand) {
  std::vector<std::uint8_t> b(488, 0); std::memcpy(b.data(), operand ? "CAO" : "CARD", 4); b[4] = 1;
  std::memcpy(b.data() + 16, v.body.data(), 400); std::memcpy(b.data() + 416, v.evidence.data(), 32); PutU64(b, 448, v.availability); return b;
}
bool DecodeSblrDmlCounterAddDescriptorV1(const std::uint8_t* p, std::size_t n,
                                         SblrDmlCounterAddDescriptorV1* o, std::string* d, bool operand) {
  if (!o || !Header(p, n, operand ? "CAO" : "CARD", 488, d)) return false;
  if (!GetU64(p, 448) || !NonZero(p + 416, 32)) { if (d) *d = "counter_add_descriptor_evidence_invalid"; return false; }
  std::memcpy(o->body.data(), p + 16, 400); std::memcpy(o->evidence.data(), p + 416, 32); o->availability = GetU64(p, 448); return true;
}

std::vector<std::uint8_t> EncodeSblrDmlCounterAddResultV1(const SblrDmlCounterAddResultV1& v) {
  std::vector<std::uint8_t> b(320, 0); std::memcpy(b.data(), "CAR", 4); b[4] = 1;
  std::memcpy(b.data() + 16, v.body.data(), 240); std::memcpy(b.data() + 256, v.evidence.data(), 32); PutU64(b, 288, v.availability); std::memcpy(b.data() + 296, v.publication_barrier.data(), 16); return b;
}
bool DecodeSblrDmlCounterAddResultV1(const std::uint8_t* p, std::size_t n,
                                     SblrDmlCounterAddResultV1* o, std::string* d) {
  if (!o || !Header(p, n, "CAR", 320, d)) return false;
  if (!GetU64(p, 288) || !NonZero(p + 256, 32) || !NonZero(p + 296, 16)) { if (d) *d = "counter_add_result_publication_invalid"; return false; }
  std::memcpy(o->body.data(), p + 16, 240); std::memcpy(o->evidence.data(), p + 256, 32); o->availability = GetU64(p, 288); std::memcpy(o->publication_barrier.data(), p + 296, 16); return true;
}
}  // namespace scratchbird::engine::sblr
