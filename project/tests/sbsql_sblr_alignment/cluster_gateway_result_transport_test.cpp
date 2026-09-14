// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "gateway_result_transport.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sys/mman.h>
#include <unistd.h>

namespace gateway = scratchbird::shared::cluster_gateway;
namespace {
std::size_t checks = 0;
void Check(bool condition, const char* message) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "FAIL check=%zu %s\n", checks, message);
    std::exit(1);
  }
}
SbCgAbiHeaderV1 Header(std::uint32_t bytes) { return {bytes, 1, 0}; }
SbCgRequestV1 Request() {
  // Only the projection owned by this memory/correlation checker. This fixture
  // is not a dispatchable envelope or an artifact/security admission proof.
  SbCgRequestV1 request{};
  request.header = Header(224);
  for (std::size_t i = 0; i < 16; ++i) {
    request.request_uuid.bytes[i] = static_cast<std::uint8_t>(i + 1);
    request.session_nonce.bytes[i] = static_cast<std::uint8_t>(i + 65);
  }
  for (std::size_t i = 0; i < 32; ++i)
    request.canonical_envelope_digest.bytes[i] = static_cast<std::uint8_t>(i + 129);
  request.request_uuid.bytes[6] = 0x70;
  request.request_uuid.bytes[8] = 0x80;
  request.session_nonce.bytes[6] = 0x70;
  request.session_nonce.bytes[8] = 0x80;
  return request;
}
SbCgResultV1 Result(const SbCgRequestV1& request) {
  SbCgResultV1 result{};
  result.header = Header(216);
  result.request_uuid = request.request_uuid;
  result.session_nonce = request.session_nonce;
  result.canonical_envelope_digest = request.canonical_envelope_digest;
  return result;
}
void Correlation() {
  const auto request = Request();
  const auto base = Result(request);
  Check(gateway::ValidateResultCorrelation(request, base) == SB_CG_STATUS_OK, "exact correlation");
  for (std::size_t i = 0; i < 64; ++i) {
    auto result = base;
    if (i < 16) result.request_uuid.bytes[i] ^= 0x80;
    else if (i < 48) result.canonical_envelope_digest.bytes[i - 16] ^= 0x80;
    else result.session_nonce.bytes[i - 48] ^= 0x80;
    Check(gateway::ValidateResultCorrelation(request, result) == SB_CG_STATUS_RESULT_INVALID,
          "each correlation byte must match");
  }
  for (std::uint32_t size : {0U, 215U, 217U, 224U, UINT32_MAX}) {
    auto result = base;
    result.header.struct_bytes = size;
    Check(gateway::ValidateResultCorrelation(request, result) == SB_CG_STATUS_RESULT_INVALID,
          "result exact size");
  }
  for (std::uint32_t size : {0U, 216U, 223U, 225U, UINT32_MAX}) {
    auto bad_request = request;
    bad_request.header.struct_bytes = size;
    Check(gateway::ValidateResultCorrelation(bad_request, base) == SB_CG_STATUS_ABI_INCOMPATIBLE,
          "host request exact size");
  }
  for (std::uint16_t version : {0U, 2U, 65535U}) {
    auto result = base;
    result.header.abi_major = version;
    Check(gateway::ValidateResultCorrelation(request, result) == SB_CG_STATUS_RESULT_INVALID,
          "result major");
    auto bad_request = request;
    bad_request.header.abi_major = version;
    Check(gateway::ValidateResultCorrelation(bad_request, base) == SB_CG_STATUS_ABI_INCOMPATIBLE,
          "request major");
  }
  for (std::uint16_t version : {1U, 2U, 65535U}) {
    auto result = base;
    result.header.abi_minor = version;
    Check(gateway::ValidateResultCorrelation(request, result) == SB_CG_STATUS_RESULT_INVALID,
          "result minor");
    auto bad_request = request;
    bad_request.header.abi_minor = version;
    Check(gateway::ValidateResultCorrelation(bad_request, base) == SB_CG_STATUS_ABI_INCOMPATIBLE,
          "request minor");
  }
  for (std::uint32_t bits : {1U, 0x80000000U, UINT32_MAX}) {
    auto result = base;
    result.reserved0 = bits;
    Check(gateway::ValidateResultCorrelation(request, result) == SB_CG_STATUS_RESULT_INVALID,
          "result reserved");
    auto bad_request = request;
    bad_request.reserved0 = bits;
    Check(gateway::ValidateResultCorrelation(bad_request, base) == SB_CG_STATUS_ABI_INCOMPATIBLE,
          "request reserved");
  }
}
void BufferRanges() {
  std::array<std::uint8_t, 96> allocation{};
  for (std::size_t i = 0; i < allocation.size(); ++i)
    allocation[i] = static_cast<std::uint8_t>(i + 11);
  const auto untouched = allocation;
  const auto base = Result(Request());
  for (std::uint64_t capacity = 0; capacity <= 32; ++capacity) {
    const SbCgMutableBufferV1 supplied{allocation.data() + 16, capacity, 0};
    for (std::uint64_t written = 0; written <= capacity; ++written) {
      const SbCgMutableBufferV1 returned{supplied.data, capacity, written};
      for (int offset = -1; offset <= 33; ++offset) {
        for (std::uint64_t length = 0; length <= 33; ++length) {
          const auto begin = static_cast<std::int64_t>(offset);
          const auto end = begin + static_cast<std::int64_t>(length);
          const bool inside = begin >= 0 && begin <= static_cast<std::int64_t>(written) &&
                              end <= static_cast<std::int64_t>(written);
          for (bool evidence : {false, true}) {
            auto result = base;
            auto& view = evidence ? result.evidence_proposal : result.typed_result;
            view = {supplied.data + offset, length};
            Check(gateway::ValidateReturnedStorage(supplied, returned, result) ==
                    (inside ? SB_CG_STATUS_OK : SB_CG_STATUS_RESULT_INVALID),
                  "view must be within written bytes, not spare capacity");
          }
        }
      }
    }
  }
  Check(allocation == untouched, "validation must not modify caller allocation");
  const SbCgMutableBufferV1 supplied{allocation.data(), allocation.size(), 0};
  SbCgMutableBufferV1 returned{supplied.data, supplied.capacity_bytes, supplied.capacity_bytes};
  auto result = base;
  result.typed_result = {allocation.data(), 48};
  result.evidence_proposal = {allocation.data() + 48, 48};
  Check(gateway::ValidateReturnedStorage(supplied, returned, result) == SB_CG_STATUS_OK,
        "two adjacent complete views");
  result.evidence_proposal = result.typed_result;
  Check(gateway::ValidateReturnedStorage(supplied, returned, result) == SB_CG_STATUS_OK,
        "overlap is a memory contract, not payload semantic admission");
  for (auto bytes : {UINT64_C(1), UINT64_MAX}) {
    result = base;
    result.typed_result = {nullptr, bytes};
    Check(gateway::ValidateReturnedStorage(supplied, returned, result) == SB_CG_STATUS_RESULT_INVALID,
          "null data cannot name bytes");
    result = base;
    result.evidence_proposal = {nullptr, bytes};
    Check(gateway::ValidateReturnedStorage(supplied, returned, result) == SB_CG_STATUS_RESULT_INVALID,
          "null evidence cannot name bytes");
  }
  result = base;
  for (auto bytes : {UINT64_C(97), UINT64_MAX}) {
    returned.written_bytes = bytes;
    Check(gateway::ValidateReturnedStorage(supplied, returned, result) == SB_CG_STATUS_RESULT_INVALID,
          "written length cannot exceed retained capacity");
  }
  returned = supplied;
  returned.capacity_bytes = supplied.capacity_bytes + 1;
  Check(gateway::ValidateReturnedStorage(supplied, returned, result) == SB_CG_STATUS_RESULT_INVALID,
        "provider cannot change capacity");
  returned = supplied;
  returned.data++;
  Check(gateway::ValidateReturnedStorage(supplied, returned, result) == SB_CG_STATUS_RESULT_INVALID,
        "provider cannot substitute allocation");
  auto invalid_supplied = supplied;
  invalid_supplied.written_bytes = 1;
  Check(gateway::ValidateReturnedStorage(invalid_supplied, returned, result) == SB_CG_STATUS_ABI_INCOMPATIBLE,
        "caller initial written bytes must be zero");
  invalid_supplied = {nullptr, 1, 0};
  Check(gateway::ValidateReturnedStorage(invalid_supplied, invalid_supplied, result) ==
          SB_CG_STATUS_ABI_INCOMPATIBLE, "nonempty allocation cannot be null");
  invalid_supplied = {nullptr, 0, 0};
  Check(gateway::ValidateReturnedStorage(invalid_supplied, invalid_supplied, result) == SB_CG_STATUS_OK,
        "empty storage and absent payloads");
  result.typed_result = {allocation.data(), 0};
  Check(gateway::ValidateReturnedStorage(invalid_supplied, invalid_supplied, result) ==
          SB_CG_STATUS_RESULT_INVALID, "foreign empty view is not borrowed storage");
  result = base;
  result.header.struct_bytes++;
  Check(gateway::ValidateReturnedStorage(supplied, supplied, result) == SB_CG_STATUS_RESULT_INVALID,
        "storage check rejects wrong result shape");
  result = base;
  result.reserved0 = 1;
  Check(gateway::ValidateReturnedStorage(supplied, supplied, result) == SB_CG_STATUS_RESULT_INVALID,
        "storage check rejects reserved bits");
}
void InaccessibleAndOverflow() {
  const auto page_size = sysconf(_SC_PAGESIZE);
  Check(page_size > 0, "page size");
  void* inaccessible = mmap(nullptr, static_cast<std::size_t>(page_size), PROT_NONE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  Check(inaccessible != MAP_FAILED, "create inaccessible guard mapping");
  std::array<std::uint8_t, 8> allocation{};
  const SbCgMutableBufferV1 supplied{allocation.data(), 8, 0};
  const SbCgMutableBufferV1 returned{allocation.data(), 8, 8};
  auto result = Result(Request());
  result.typed_result = {static_cast<const std::uint8_t*>(inaccessible), 1};
  Check(gateway::ValidateReturnedStorage(supplied, returned, result) == SB_CG_STATUS_RESULT_INVALID,
        "inaccessible view must be rejected without dereference");
  result.typed_result = {};
  result.evidence_proposal = {static_cast<const std::uint8_t*>(inaccessible), UINT64_MAX};
  Check(gateway::ValidateReturnedStorage(supplied, returned, result) == SB_CG_STATUS_RESULT_INVALID,
        "inaccessible overflowing evidence must not be read");
  Check(munmap(inaccessible, static_cast<std::size_t>(page_size)) == 0, "release guard mapping");
  result = Result(Request());
  const auto maximum = std::numeric_limits<std::uintptr_t>::max();
  for (std::uintptr_t suffix : {0U, 1U, 7U, 31U}) {
    auto* origin = reinterpret_cast<std::uint8_t*>(maximum - suffix);
    const SbCgMutableBufferV1 wrapped{origin, suffix + 1, 0};
    Check(gateway::ValidateReturnedStorage(wrapped, wrapped, result) == SB_CG_STATUS_ABI_INCOMPATIBLE,
          "address extent overflow");
  }
  result.typed_result = {allocation.data(), UINT64_MAX};
  Check(gateway::ValidateReturnedStorage(supplied, returned, result) == SB_CG_STATUS_RESULT_INVALID,
        "view length overflow");
}
}  // namespace
int main() {
  Correlation();
  BufferRanges();
  InaccessibleAndOverflow();
  std::printf("PASS gateway result memory/correlation contract checks=%zu\n", checks);
}
