// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "mga_relation_store/mga_update_durable_frame_store_internal.hpp"

#include "hash_digest.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace scratchbird::engine::internal_api::mga_update_durable_detail {

// SEARCH_KEY: SB_ENGINE_MGA_UPDATE_DURABLE_FRAME_STORE_AUTHORITY
// Owns authenticated durable UPDATE frame encoding, bounded frame loading,
// directory durability and file locking. Frame presence cannot decide
// transaction finality.

namespace {

constexpr std::array<std::uint8_t, 8> kDmlUpdateDurableFrameMagic{{
    'S', 'B', 'M', 'D', 'U', 'O', 'P', '1'}};
constexpr std::uint16_t kDmlUpdateDurableFrameVersion = 1;
constexpr std::uint32_t kDmlUpdateDurableFrameHeaderBytes = 352;
constexpr std::uint64_t kDmlUpdateDurableMaximumFrameBytes =
    static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max());
constexpr std::string_view kDmlUpdateDurableFrameEvidenceDomain =
    "ScratchBird.MgaDmlUpdateDurableOperationFrame.V1";

}  // namespace

std::string DmlUpdateDurableOperationStorePath(
    const EngineRequestContext& context) {
  return context.database_path + ".sb.mga_durable_operations";
}

std::string DmlUpdateStatementSavepointBinaryStorePath(
    const EngineRequestContext& context) {
  return context.database_path + ".sb.mga_update_statement_savepoints.v1";
}

void DmlUpdateDurablePutU16(std::vector<std::uint8_t>* bytes,
                            std::size_t offset, std::uint16_t value) {
  (*bytes)[offset] = static_cast<std::uint8_t>(value & 0xffu);
  (*bytes)[offset + 1] =
      static_cast<std::uint8_t>((value >> 8u) & 0xffu);
}

void DmlUpdateDurablePutU32(std::vector<std::uint8_t>* bytes,
                            std::size_t offset, std::uint32_t value) {
  for (std::size_t index = 0; index < 4; ++index) {
    (*bytes)[offset + index] =
        static_cast<std::uint8_t>((value >> (index * 8u)) & 0xffu);
  }
}

void DmlUpdateDurablePutU64(std::vector<std::uint8_t>* bytes,
                            std::size_t offset, std::uint64_t value) {
  for (std::size_t index = 0; index < 8; ++index) {
    (*bytes)[offset + index] =
        static_cast<std::uint8_t>((value >> (index * 8u)) & 0xffu);
  }
}

bool DmlUpdateDurableReadU16(std::span<const std::uint8_t> bytes,
                             std::size_t offset, std::uint16_t* value) {
  if (value == nullptr || offset > bytes.size() ||
      bytes.size() - offset < 2) {
    return false;
  }
  *value = static_cast<std::uint16_t>(bytes[offset]) |
           (static_cast<std::uint16_t>(bytes[offset + 1]) << 8u);
  return true;
}

bool DmlUpdateDurableReadU32(std::span<const std::uint8_t> bytes,
                             std::size_t offset, std::uint32_t* value) {
  if (value == nullptr || offset > bytes.size() ||
      bytes.size() - offset < 4) {
    return false;
  }
  std::uint32_t parsed = 0;
  for (std::size_t index = 0; index < 4; ++index) {
    parsed |= static_cast<std::uint32_t>(bytes[offset + index])
              << (index * 8u);
  }
  *value = parsed;
  return true;
}

bool DmlUpdateDurableReadU64(std::span<const std::uint8_t> bytes,
                             std::size_t offset, std::uint64_t* value) {
  if (value == nullptr || offset > bytes.size() ||
      bytes.size() - offset < 8) {
    return false;
  }
  std::uint64_t parsed = 0;
  for (std::size_t index = 0; index < 8; ++index) {
    parsed |= static_cast<std::uint64_t>(bytes[offset + index])
              << (index * 8u);
  }
  *value = parsed;
  return true;
}

bool DmlUpdateDurableZero(std::span<const std::uint8_t> bytes) {
  return std::all_of(bytes.begin(), bytes.end(),
                     [](std::uint8_t value) { return value == 0; });
}

bool DmlUpdateDurableUuidBytes(
    std::string_view uuid, std::array<std::uint8_t, 16>* bytes) {
  if (bytes == nullptr || uuid.empty()) return false;
  const auto parsed = scratchbird::core::uuid::ParseUuid(std::string(uuid));
  if (!parsed.ok() || scratchbird::core::uuid::IsNilUuid(parsed.value) ||
      scratchbird::core::uuid::UuidToString(parsed.value) != uuid) {
    return false;
  }
  std::copy(parsed.value.bytes.begin(), parsed.value.bytes.end(),
            bytes->begin());
  return true;
}

bool DmlUpdateDurableTypedUuid(
    std::string_view uuid, scratchbird::wire::TypedUpdateUuid* bytes) {
  if (bytes == nullptr) return false;
  std::array<std::uint8_t, 16> parsed{};
  if (!DmlUpdateDurableUuidBytes(uuid, &parsed)) return false;
  std::copy(parsed.begin(), parsed.end(), bytes->begin());
  return true;
}

std::string DmlUpdateDurableUuidText(
    std::span<const std::uint8_t> bytes) {
  if (bytes.size() != 16 || DmlUpdateDurableZero(bytes)) return {};
  scratchbird::core::platform::Uuid value{};
  std::copy(bytes.begin(), bytes.end(), value.bytes.begin());
  return scratchbird::core::uuid::UuidToString(value);
}

std::string DmlUpdateDurableTypedUuidText(
    const scratchbird::wire::TypedUpdateUuid& bytes) {
  return DmlUpdateDurableUuidText(
      std::span<const std::uint8_t>(bytes.data(), bytes.size()));
}

bool DmlUpdateDurablePutUuid(std::vector<std::uint8_t>* bytes,
                             std::size_t offset, std::string_view uuid) {
  if (bytes == nullptr || offset > bytes->size() ||
      bytes->size() - offset < 16) {
    return false;
  }
  std::array<std::uint8_t, 16> parsed{};
  if (!DmlUpdateDurableUuidBytes(uuid, &parsed)) return false;
  std::copy(parsed.begin(), parsed.end(), bytes->begin() + offset);
  return true;
}

bool DmlUpdateDurableBaseIdentityValid(
    const MgaDmlUpdateDurableOperationIdentityV1& identity) {
  std::array<std::uint8_t, 16> ignored{};
  return DmlUpdateDurableUuidBytes(identity.database_uuid, &ignored) &&
         DmlUpdateDurableUuidBytes(identity.owning_transaction_uuid,
                                   &ignored) &&
         identity.owning_local_transaction_id != 0 &&
         DmlUpdateDurableUuidBytes(
             identity.authenticated_statement_receipt_uuid, &ignored) &&
         DmlUpdateDurableUuidBytes(identity.operation_uuid, &ignored) &&
         DmlUpdateDurableUuidBytes(identity.descriptor_uuid, &ignored) &&
         identity.descriptor_generation != 0 &&
         DmlUpdateDurableUuidBytes(identity.recovery_token_uuid, &ignored) &&
         identity.recovery_generation != 0;
}

bool DmlUpdateDurableIdentityValid(
    const MgaDmlUpdateDurableOperationIdentityV1& identity) {
  std::array<std::uint8_t, 16> ignored{};
  return DmlUpdateDurableBaseIdentityValid(identity) &&
         identity.operation_generation != 0 &&
         DmlUpdateDurableUuidBytes(identity.validated_durable_handle_uuid,
                                   &ignored) &&
         identity.validated_durable_handle_generation != 0 &&
         DmlUpdateDurableUuidBytes(identity.reserved_statement_barrier_uuid,
                                   &ignored) &&
         identity.reserved_statement_barrier_generation != 0;
}

bool DmlUpdateDurableIdentityMatchesContext(
    const EngineRequestContext& context,
    const MgaDmlUpdateDurableOperationIdentityV1& identity) {
  return !context.database_path.empty() &&
         DmlUpdateDurableIdentityValid(identity) &&
         context.database_uuid.canonical == identity.database_uuid &&
         context.transaction_uuid.canonical ==
             identity.owning_transaction_uuid &&
         context.local_transaction_id == identity.owning_local_transaction_id &&
         context.statement_receipt_uuid.canonical ==
             identity.authenticated_statement_receipt_uuid;
}

std::string DmlUpdateDurableDescriptorPath(
    const EngineRequestContext& context,
    const MgaDmlUpdateDurableOperationIdentityV1& identity) {
  return DmlUpdateDurableOperationStorePath(context) + "/" +
         identity.descriptor_uuid + ".duop";
}

std::string DmlUpdateDurableSavepointPath(
    const EngineRequestContext& context, std::string_view savepoint_uuid) {
  return DmlUpdateStatementSavepointBinaryStorePath(context) + "/" +
         std::string(savepoint_uuid) + ".dups";
}

MgaDmlUpdateDurableSha256V1 DmlUpdateDurableSha256(
    std::span<const std::uint8_t> bytes) {
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(
      bytes.data(), bytes.size());
  return digest.ok() ? digest.digest : MgaDmlUpdateDurableSha256V1{};
}

MgaDmlUpdateDurableSha256V1 DmlUpdateDurableFrameSha256(
    std::span<const std::uint8_t> header_without_checksum) {
  std::vector<std::uint8_t> material;
  material.reserve(kDmlUpdateDurableFrameEvidenceDomain.size() +
                   header_without_checksum.size());
  material.insert(material.end(),
                  kDmlUpdateDurableFrameEvidenceDomain.begin(),
                  kDmlUpdateDurableFrameEvidenceDomain.end());
  material.insert(material.end(), header_without_checksum.begin(),
                  header_without_checksum.end());
  return DmlUpdateDurableSha256(material);
}

bool DmlUpdateDurableEncodeFrame(
    const DmlUpdateDurableFrameV1& frame,
    std::vector<std::uint8_t>* encoded) {
  const bool savepoint_frame =
      frame.kind == DmlUpdateDurableFrameKindV1::statement_savepoint;
  if (encoded == nullptr ||
      !(savepoint_frame
            ? DmlUpdateDurableBaseIdentityValid(frame.identity)
            : DmlUpdateDurableIdentityValid(frame.identity)) ||
      frame.payload.size() > kDmlUpdateDurableMaximumFrameBytes ||
      static_cast<std::uint64_t>(frame.payload.size()) >
          std::numeric_limits<std::uint64_t>::max() -
              kDmlUpdateDurableFrameHeaderBytes) {
    return false;
  }
  std::vector<std::uint8_t> header(kDmlUpdateDurableFrameHeaderBytes, 0);
  std::copy(kDmlUpdateDurableFrameMagic.begin(),
            kDmlUpdateDurableFrameMagic.end(), header.begin());
  DmlUpdateDurablePutU16(&header, 8, kDmlUpdateDurableFrameVersion);
  DmlUpdateDurablePutU16(
      &header, 10, static_cast<std::uint16_t>(frame.kind));
  DmlUpdateDurablePutU32(&header, 12,
                         kDmlUpdateDurableFrameHeaderBytes);
  DmlUpdateDurablePutU64(
      &header, 16,
      kDmlUpdateDurableFrameHeaderBytes + frame.payload.size());
  DmlUpdateDurablePutU64(&header, 24, frame.payload.size());
  if (!DmlUpdateDurablePutUuid(&header, 32, frame.identity.database_uuid) ||
      !DmlUpdateDurablePutUuid(
          &header, 48, frame.identity.owning_transaction_uuid) ||
      !DmlUpdateDurablePutUuid(
          &header, 72,
          frame.identity.authenticated_statement_receipt_uuid) ||
      !DmlUpdateDurablePutUuid(&header, 88, frame.identity.operation_uuid) ||
      !DmlUpdateDurablePutUuid(&header, 112,
                               frame.identity.descriptor_uuid) ||
      !DmlUpdateDurablePutUuid(&header, 136,
                               frame.identity.recovery_token_uuid) ||
      (!savepoint_frame &&
       !DmlUpdateDurablePutUuid(
           &header, 160,
           frame.identity.validated_durable_handle_uuid)) ||
      (!savepoint_frame &&
       !DmlUpdateDurablePutUuid(
           &header, 184,
           frame.identity.reserved_statement_barrier_uuid))) {
    return false;
  }
  DmlUpdateDurablePutU64(
      &header, 64, frame.identity.owning_local_transaction_id);
  DmlUpdateDurablePutU64(&header, 104,
                         frame.identity.operation_generation);
  DmlUpdateDurablePutU64(&header, 128,
                         frame.identity.descriptor_generation);
  DmlUpdateDurablePutU64(&header, 152,
                         frame.identity.recovery_generation);
  DmlUpdateDurablePutU64(
      &header, 176, frame.identity.validated_durable_handle_generation);
  DmlUpdateDurablePutU64(
      &header, 200, frame.identity.reserved_statement_barrier_generation);
  DmlUpdateDurablePutU64(&header, 208, frame.sequence);
  header[216] = frame.state;
  header[217] = frame.flags;
  std::copy(frame.prior_record_sha256.begin(),
            frame.prior_record_sha256.end(), header.begin() + 224);
  std::copy(frame.record_evidence_sha256.begin(),
            frame.record_evidence_sha256.end(), header.begin() + 256);
  const auto payload_sha = DmlUpdateDurableSha256(frame.payload);
  std::copy(payload_sha.begin(), payload_sha.end(), header.begin() + 288);
  const auto frame_sha = DmlUpdateDurableFrameSha256(
      std::span<const std::uint8_t>(header).first(320));
  std::copy(frame_sha.begin(), frame_sha.end(), header.begin() + 320);
  if (DmlUpdateDurableZero(payload_sha) || DmlUpdateDurableZero(frame_sha)) {
    return false;
  }
  encoded->clear();
  encoded->reserve(header.size() + frame.payload.size());
  encoded->insert(encoded->end(), header.begin(), header.end());
  encoded->insert(encoded->end(), frame.payload.begin(), frame.payload.end());
  return true;
}

bool DmlUpdateDurableDecodeFrame(
    std::span<const std::uint8_t> encoded,
    DmlUpdateDurableFrameV1* frame, std::string* detail) {
  const auto fail = [&](std::string reason) {
    if (detail != nullptr) *detail = std::move(reason);
    return false;
  };
  if (frame == nullptr || encoded.size() < kDmlUpdateDurableFrameHeaderBytes) {
    return fail("durable_frame_header_truncated");
  }
  if (!std::equal(kDmlUpdateDurableFrameMagic.begin(),
                  kDmlUpdateDurableFrameMagic.end(), encoded.begin())) {
    return fail("durable_frame_magic_invalid");
  }
  std::uint16_t version = 0;
  std::uint16_t kind = 0;
  std::uint32_t header_bytes = 0;
  std::uint64_t total_bytes = 0;
  std::uint64_t payload_bytes = 0;
  if (!DmlUpdateDurableReadU16(encoded, 8, &version) ||
      !DmlUpdateDurableReadU16(encoded, 10, &kind) ||
      !DmlUpdateDurableReadU32(encoded, 12, &header_bytes) ||
      !DmlUpdateDurableReadU64(encoded, 16, &total_bytes) ||
      !DmlUpdateDurableReadU64(encoded, 24, &payload_bytes) ||
      version != kDmlUpdateDurableFrameVersion ||
      header_bytes != kDmlUpdateDurableFrameHeaderBytes ||
      total_bytes != encoded.size() ||
      payload_bytes != encoded.size() - header_bytes ||
      payload_bytes > kDmlUpdateDurableMaximumFrameBytes ||
      (kind < static_cast<std::uint16_t>(
                  DmlUpdateDurableFrameKindV1::authority_snapshot) ||
       kind > static_cast<std::uint16_t>(
                  DmlUpdateDurableFrameKindV1::prepared_successor_invalidated)) ||
      !DmlUpdateDurableZero(encoded.subspan(218, 6))) {
    return fail("durable_frame_extent_or_header_invalid");
  }
  frame->kind = static_cast<DmlUpdateDurableFrameKindV1>(kind);
  frame->identity.database_uuid =
      DmlUpdateDurableUuidText(encoded.subspan(32, 16));
  frame->identity.owning_transaction_uuid =
      DmlUpdateDurableUuidText(encoded.subspan(48, 16));
  frame->identity.authenticated_statement_receipt_uuid =
      DmlUpdateDurableUuidText(encoded.subspan(72, 16));
  frame->identity.operation_uuid =
      DmlUpdateDurableUuidText(encoded.subspan(88, 16));
  frame->identity.descriptor_uuid =
      DmlUpdateDurableUuidText(encoded.subspan(112, 16));
  frame->identity.recovery_token_uuid =
      DmlUpdateDurableUuidText(encoded.subspan(136, 16));
  frame->identity.validated_durable_handle_uuid =
      DmlUpdateDurableUuidText(encoded.subspan(160, 16));
  frame->identity.reserved_statement_barrier_uuid =
      DmlUpdateDurableUuidText(encoded.subspan(184, 16));
  if (!DmlUpdateDurableReadU64(
          encoded, 64, &frame->identity.owning_local_transaction_id) ||
      !DmlUpdateDurableReadU64(
          encoded, 104, &frame->identity.operation_generation) ||
      !DmlUpdateDurableReadU64(
          encoded, 128, &frame->identity.descriptor_generation) ||
      !DmlUpdateDurableReadU64(
          encoded, 152, &frame->identity.recovery_generation) ||
      !DmlUpdateDurableReadU64(
          encoded, 176,
          &frame->identity.validated_durable_handle_generation) ||
      !DmlUpdateDurableReadU64(
          encoded, 200,
          &frame->identity.reserved_statement_barrier_generation) ||
      !DmlUpdateDurableReadU64(encoded, 208, &frame->sequence) ||
      !(static_cast<DmlUpdateDurableFrameKindV1>(kind) ==
                DmlUpdateDurableFrameKindV1::statement_savepoint
            ? DmlUpdateDurableBaseIdentityValid(frame->identity)
            : DmlUpdateDurableIdentityValid(frame->identity))) {
    return fail("durable_frame_identity_invalid");
  }
  frame->state = encoded[216];
  frame->flags = encoded[217];
  std::copy_n(encoded.begin() + 224, 32,
              frame->prior_record_sha256.begin());
  std::copy_n(encoded.begin() + 256, 32,
              frame->record_evidence_sha256.begin());
  MgaDmlUpdateDurableSha256V1 payload_sha{};
  MgaDmlUpdateDurableSha256V1 frame_sha{};
  std::copy_n(encoded.begin() + 288, 32, payload_sha.begin());
  std::copy_n(encoded.begin() + 320, 32, frame_sha.begin());
  const auto payload = encoded.subspan(header_bytes, payload_bytes);
  if (DmlUpdateDurableSha256(payload) != payload_sha ||
      DmlUpdateDurableFrameSha256(encoded.first(320)) != frame_sha) {
    return fail("durable_frame_checksum_invalid");
  }
  frame->payload.assign(payload.begin(), payload.end());
  return true;
}

DmlUpdateDurableFrameLoadV1 DmlUpdateDurableLoadFrames(
    const std::string& path) {
  DmlUpdateDurableFrameLoadV1 result;
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    std::error_code ignored;
    result.missing = !std::filesystem::exists(path, ignored);
    result.ok = result.missing;
    result.detail = result.missing ? std::string{}
                                   : "durable_store_open_failed";
    return result;
  }
  while (true) {
    std::vector<std::uint8_t> header(kDmlUpdateDurableFrameHeaderBytes, 0);
    input.read(reinterpret_cast<char*>(header.data()),
               static_cast<std::streamsize>(header.size()));
    const auto header_read = input.gcount();
    if (header_read == 0 && input.eof()) break;
    if (header_read != static_cast<std::streamsize>(header.size())) {
      result.detail = "durable_store_partial_frame_header";
      return result;
    }
    std::uint64_t total_bytes = 0;
    std::uint64_t payload_bytes = 0;
    if (!DmlUpdateDurableReadU64(header, 16, &total_bytes) ||
        !DmlUpdateDurableReadU64(header, 24, &payload_bytes) ||
        total_bytes != kDmlUpdateDurableFrameHeaderBytes + payload_bytes ||
        payload_bytes > kDmlUpdateDurableMaximumFrameBytes ||
        payload_bytes >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::size_t>::max())) {
      result.detail = "durable_store_frame_extent_invalid";
      return result;
    }
    std::vector<std::uint8_t> encoded;
    try {
      encoded.reserve(static_cast<std::size_t>(total_bytes));
      encoded.insert(encoded.end(), header.begin(), header.end());
      const auto old_size = encoded.size();
      encoded.resize(old_size + static_cast<std::size_t>(payload_bytes));
    } catch (const std::bad_alloc&) {
      result.detail = "durable_store_frame_allocation_failed";
      return result;
    }
    input.read(
        reinterpret_cast<char*>(encoded.data() + header.size()),
        static_cast<std::streamsize>(payload_bytes));
    if (input.gcount() != static_cast<std::streamsize>(payload_bytes)) {
      result.detail = "durable_store_partial_frame_payload";
      return result;
    }
    DmlUpdateDurableFrameV1 decoded;
    if (!DmlUpdateDurableDecodeFrame(encoded, &decoded, &result.detail)) {
      return result;
    }
    result.frames.push_back(std::move(decoded));
  }
  if (input.bad()) {
    result.detail = "durable_store_read_failed";
    return result;
  }
  result.ok = true;
  return result;
}

bool DmlUpdateDurableEnsureDirectory(const std::string& directory) {
  std::error_code error;
  if (!std::filesystem::create_directories(directory, error) && error) {
    return false;
  }
#if defined(_WIN32)
  return true;
#else
  const int fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) return false;
  const bool ok = ::fsync(fd) == 0;
  ::close(fd);
  return ok;
#endif
}

DmlUpdateDurableFileLock::DmlUpdateDurableFileLock(
    const std::string& data_path) {
  const std::string path = data_path + ".lock";
#if defined(_WIN32)
  handle_ = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle_ == INVALID_HANDLE_VALUE) return;
  OVERLAPPED overlapped{};
  if (LockFileEx(handle_, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD,
                 &overlapped) == 0) {
    CloseHandle(handle_);
    handle_ = INVALID_HANDLE_VALUE;
    return;
  }
  ok_ = true;
#else
  fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (fd_ < 0) return;
  if (::flock(fd_, LOCK_EX) != 0) {
    ::close(fd_);
    fd_ = -1;
    return;
  }
  ok_ = true;
#endif
}

DmlUpdateDurableFileLock::~DmlUpdateDurableFileLock() {
#if defined(_WIN32)
  if (handle_ != INVALID_HANDLE_VALUE) {
    OVERLAPPED overlapped{};
    UnlockFileEx(handle_, 0, MAXDWORD, MAXDWORD, &overlapped);
    CloseHandle(handle_);
  }
#else
  if (fd_ >= 0) {
    (void)::flock(fd_, LOCK_UN);
    ::close(fd_);
  }
#endif
}

bool DmlUpdateDurableFileLock::ok() const { return ok_; }

}  // namespace scratchbird::engine::internal_api::mga_update_durable_detail
