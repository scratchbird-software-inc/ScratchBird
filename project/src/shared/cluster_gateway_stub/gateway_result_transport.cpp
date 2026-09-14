// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "gateway_result_transport.hpp"

#include <cstdint>
#include <cstring>
#include <limits>

namespace scratchbird::shared::cluster_gateway {
namespace {

// The public declaration block alone does not assert all natural alignments.
// A gateway translation unit must not silently compile under active packing.
static_assert(alignof(SbCgAbiHeaderV1) == 4);
static_assert(alignof(SbCgByteViewV1) == 8);
static_assert(alignof(SbCgMutableBufferV1) == 8);
static_assert(alignof(SbCgDescriptorV1) == 8);
static_assert(alignof(SbCgHostCallRequestV1) == 8);
static_assert(alignof(SbCgHostV1) == 8);
static_assert(alignof(SbCgOpenParamsV1) == 8);
static_assert(alignof(SbCgRequestV1) == 8);
static_assert(alignof(SbCgResultV1) == 8);
static_assert(sizeof(std::uintptr_t) == 8);

bool ExactHeader(const SbCgAbiHeaderV1& header, std::uint32_t size) noexcept {
  return header.struct_bytes == size &&
         header.abi_major == SB_CG_ABI_MAJOR_V1 &&
         header.abi_minor == SB_CG_ABI_MINOR_V1;
}

bool RepresentableExtent(const std::uint8_t* data, std::uint64_t bytes) noexcept {
  if (data == nullptr) return bytes == 0;
  const auto address = reinterpret_cast<std::uintptr_t>(data);
  return bytes <= std::numeric_limits<std::uintptr_t>::max() - address;
}

bool BorrowedView(const SbCgByteViewV1& view,
                  const SbCgMutableBufferV1& storage) noexcept {
  if (view.data == nullptr) return view.bytes == 0;
  if (storage.data == nullptr) return false;
  const auto origin = reinterpret_cast<std::uintptr_t>(storage.data);
  const auto address = reinterpret_cast<std::uintptr_t>(view.data);
  if (address < origin) return false;
  const auto offset = address - origin;
  // Subtraction after the comparison avoids overflowing address+length and
  // accepts an empty view at the end of the written allocation only.
  return offset <= storage.written_bytes &&
         view.bytes <= storage.written_bytes - offset;
}

}  // namespace

SbCgStatusV1 ValidateReturnedStorage(
    const SbCgMutableBufferV1& supplied_storage,
    const SbCgMutableBufferV1& returned_storage,
    const SbCgResultV1& returned_result) noexcept {
  if (supplied_storage.written_bytes != 0 ||
      !RepresentableExtent(supplied_storage.data, supplied_storage.capacity_bytes)) {
    return SB_CG_STATUS_ABI_INCOMPATIBLE;
  }
  if (!ExactHeader(returned_result.header, sizeof(SbCgResultV1)) ||
      returned_result.reserved0 != 0 ||
      returned_storage.data != supplied_storage.data ||
      returned_storage.capacity_bytes != supplied_storage.capacity_bytes ||
      returned_storage.written_bytes > supplied_storage.capacity_bytes ||
      !BorrowedView(returned_result.typed_result, returned_storage) ||
      !BorrowedView(returned_result.evidence_proposal, returned_storage)) {
    return SB_CG_STATUS_RESULT_INVALID;
  }
  return SB_CG_STATUS_OK;
}

SbCgStatusV1 ValidateResultCorrelation(
    const SbCgRequestV1& retained_request,
    const SbCgResultV1& returned_result) noexcept {
  if (!ExactHeader(retained_request.header, sizeof(SbCgRequestV1)) ||
      retained_request.reserved0 != 0) {
    return SB_CG_STATUS_ABI_INCOMPATIBLE;
  }
  if (!ExactHeader(returned_result.header, sizeof(SbCgResultV1)) ||
      returned_result.reserved0 != 0 ||
      std::memcmp(retained_request.request_uuid.bytes,
                  returned_result.request_uuid.bytes, SB_CG_UUID_BYTES) != 0 ||
      std::memcmp(retained_request.canonical_envelope_digest.bytes,
                  returned_result.canonical_envelope_digest.bytes, SB_CG_SHA256_BYTES) != 0 ||
      std::memcmp(retained_request.session_nonce.bytes,
                  returned_result.session_nonce.bytes, SB_CG_UUID_BYTES) != 0) {
    return SB_CG_STATUS_RESULT_INVALID;
  }
  return SB_CG_STATUS_OK;
}

}  // namespace scratchbird::shared::cluster_gateway
