// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "mga_relation_store/mga_update_durable_store.hpp"
#include "mga_relation_store/mga_event_sequence_allocator.hpp"
#include "mga_relation_store/mga_savepoint_store.hpp"

#include "api_diagnostics.hpp"
#include "crud_support/crud_store.hpp"
#include "ipar_fault_injection.hpp"
#include "local_transaction_store.hpp"
#include "transaction_inventory.hpp"
#include "transaction_state.hpp"
#include "typed_update_carrier_codec.hpp"
#include "uuid.hpp"
#include "hash_digest.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_MGA_UPDATE_DURABLE_STORE_IMPLEMENTATION_AUTHORITY
// Owns exact durable UPDATE operation frames, statement-barrier records,
// authenticated recovery inspection, and append/fsync sequencing. These
// records are mutation/recovery evidence subordinate to durable transaction
// inventory finality.

namespace {

using scratchbird::storage::database::LoadLocalTransactionInventoryFromDatabase;
using scratchbird::transaction::mga::LookupLocalTransaction;
using scratchbird::transaction::mga::MakeLocalTransactionId;
using scratchbird::transaction::mga::TransactionState;

constexpr const char* kRowStoreMagic = "SBMGA1";
constexpr std::string_view kDmlUpdateStatementSavepointCreateKind =
    "DML_UPDATE_STATEMENT_SAVEPOINT_CREATE_V1";
constexpr std::string_view kDmlUpdateStatementSavepointRollbackKind =
    "DML_UPDATE_STATEMENT_SAVEPOINT_ROLLBACK_V1";
constexpr std::string_view kDmlUpdateStatementSavepointReleaseKind =
    "DML_UPDATE_STATEMENT_SAVEPOINT_RELEASE_V1";
constexpr std::string_view kDmlUpdateStatementSavepointEvidenceDomain =
    "ScratchBird.MgaDmlUpdateStatementSavepointAuthority.V1";

std::string DmlUpdateDurableOperationStorePath(
    const EngineRequestContext& context) {
  return context.database_path + ".sb.mga_durable_operations";
}

std::string DmlUpdateStatementSavepointBinaryStorePath(
    const EngineRequestContext& context) {
  return context.database_path + ".sb.mga_update_statement_savepoints.v1";
}

EngineApiDiagnostic OkDiagnostic() {
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

int HexValue(const char c) {
  if (c >= '0' && c <= '9') { return c - '0'; }
  if (c >= 'a' && c <= 'f') { return 10 + c - 'a'; }
  if (c >= 'A' && c <= 'F') { return 10 + c - 'A'; }
  return -1;
}

std::string DecodeCrudTextLocal(const std::string& encoded) {
  if ((encoded.size() % 2) != 0) { return {}; }
  std::string decoded;
  decoded.reserve(encoded.size() / 2);
  for (std::size_t index = 0; index < encoded.size(); index += 2) {
    const int high = HexValue(encoded[index]);
    const int low = HexValue(encoded[index + 1]);
    if (high < 0 || low < 0) { return {}; }
    decoded.push_back(static_cast<char>((high << 4) | low));
  }
  return decoded;
}

std::string JoinLine(const std::vector<std::string>& fields) {
  std::string line;
  for (std::size_t index = 0; index < fields.size(); ++index) {
    if (index != 0) { line.push_back('\t'); }
    line.append(fields[index]);
  }
  return line;
}

}  // namespace

namespace {

constexpr std::array<std::uint8_t, 8> kDmlUpdateDurableFrameMagic{{
    'S', 'B', 'M', 'D', 'U', 'O', 'P', '1'}};
constexpr std::uint16_t kDmlUpdateDurableFrameVersion = 1;
constexpr std::uint32_t kDmlUpdateDurableFrameHeaderBytes = 352;
constexpr std::uint64_t kDmlUpdateDurableMaximumFrameBytes =
    static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max());
constexpr std::string_view kDmlUpdateDurableFrameEvidenceDomain =
    "ScratchBird.MgaDmlUpdateDurableOperationFrame.V1";

enum class DmlUpdateDurableFrameKindV1 : std::uint16_t {
  authority_snapshot = 1,
  journal = 2,
  statement_savepoint = 3,
  recovery_observation = 4,
  authority_reservation = 5,
  // Contains the already encoded canonical journal frame that may be
  // published after the statement barrier without any further
  // encode/hash/allocation step.
  prepared_successor = 6,
  // Durable tombstone for a prepared publication successor that was
  // cancelled before the publication barrier.  The tombstone names the exact
  // staged successor in its fixed frame fields; recovery clears that staged
  // successor before considering any later aborted successor.
  prepared_successor_invalidated = 7,
};

struct DmlUpdateDurableFrameV1 {
  DmlUpdateDurableFrameKindV1 kind =
      DmlUpdateDurableFrameKindV1::authority_snapshot;
  MgaDmlUpdateDurableOperationIdentityV1 identity;
  std::uint64_t sequence = 0;
  std::uint8_t state = 0;
  std::uint8_t flags = 0;
  MgaDmlUpdateDurableSha256V1 prior_record_sha256{};
  MgaDmlUpdateDurableSha256V1 record_evidence_sha256{};
  std::vector<std::uint8_t> payload;
};

struct DmlUpdateDurableFrameLoadV1 {
  bool ok = false;
  bool missing = false;
  std::string detail;
  std::vector<DmlUpdateDurableFrameV1> frames;
};

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

class DmlUpdateDurableFileLock final {
 public:
  explicit DmlUpdateDurableFileLock(const std::string& data_path) {
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

  ~DmlUpdateDurableFileLock() {
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

  bool ok() const { return ok_; }

 private:
  bool ok_ = false;
#if defined(_WIN32)
  HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
  int fd_ = -1;
#endif
};

std::atomic<std::uint64_t> g_dml_update_durable_prepare_calls{0};
std::atomic<std::uint64_t> g_dml_update_durable_frame_encode_calls{0};
std::atomic<std::uint64_t> g_dml_update_durable_checksum_calls{0};
std::atomic<std::uint64_t> g_dml_update_durable_commit_calls{0};
std::atomic<std::uint64_t> g_dml_update_durable_commit_write_calls{0};
std::atomic<std::uint64_t> g_dml_update_durable_commit_fsync_calls{0};
std::atomic<std::uint64_t> g_dml_update_durable_recovery_calls{0};
std::atomic<std::uint64_t> g_dml_update_durable_observation_encode_calls{0};
std::atomic<MgaDmlUpdateDurableFaultCutpointV1>
    g_dml_update_durable_fault_cutpoint{
        MgaDmlUpdateDurableFaultCutpointV1::none};

bool DmlUpdateDurableFault(MgaDmlUpdateDurableFaultCutpointV1 cutpoint) {
  return g_dml_update_durable_fault_cutpoint.load(
             std::memory_order_acquire) == cutpoint;
}

enum class DmlUpdateDurableRawAppendResultV1 : std::uint8_t {
  ok,
  write_failed,
  fsync_failed,
  after_fsync_ack_lost,
};

DmlUpdateDurableRawAppendResultV1 DmlUpdateDurableAppendEncodedFrame(
    const std::string& path, std::span<const std::uint8_t> encoded,
    bool successor_commit) {
  if (encoded.empty()) return DmlUpdateDurableRawAppendResultV1::write_failed;
#if defined(_WIN32)
  HANDLE handle = CreateFileA(path.c_str(), FILE_APPEND_DATA,
                              FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return DmlUpdateDurableRawAppendResultV1::write_failed;
  }
  std::size_t offset = 0;
  bool write_ok = true;
  while (offset < encoded.size()) {
    DWORD written = 0;
    const DWORD request = static_cast<DWORD>(std::min<std::size_t>(
        encoded.size() - offset, std::numeric_limits<DWORD>::max()));
    if (WriteFile(handle, encoded.data() + offset, request, &written,
                  nullptr) == 0 || written == 0) {
      write_ok = false;
      break;
    }
    offset += written;
  }
  if (successor_commit) {
    g_dml_update_durable_commit_write_calls.fetch_add(
        1, std::memory_order_relaxed);
  }
  if (!write_ok ||
      (successor_commit &&
       DmlUpdateDurableFault(
           MgaDmlUpdateDurableFaultCutpointV1::after_successor_write_before_fsync))) {
    CloseHandle(handle);
    return DmlUpdateDurableRawAppendResultV1::write_failed;
  }
  const bool fsync_ok = FlushFileBuffers(handle) != 0;
  if (successor_commit) {
    g_dml_update_durable_commit_fsync_calls.fetch_add(
        1, std::memory_order_relaxed);
  }
  CloseHandle(handle);
#else
  const int fd = ::open(path.c_str(),
                        O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
  if (fd < 0) return DmlUpdateDurableRawAppendResultV1::write_failed;
  std::size_t offset = 0;
  bool write_ok = true;
  while (offset < encoded.size()) {
    const ssize_t written =
        ::write(fd, encoded.data() + offset, encoded.size() - offset);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) {
      write_ok = false;
      break;
    }
    offset += static_cast<std::size_t>(written);
  }
  if (successor_commit) {
    g_dml_update_durable_commit_write_calls.fetch_add(
        1, std::memory_order_relaxed);
  }
  if (!write_ok ||
      (successor_commit &&
       DmlUpdateDurableFault(
           MgaDmlUpdateDurableFaultCutpointV1::after_successor_write_before_fsync))) {
    ::close(fd);
    return DmlUpdateDurableRawAppendResultV1::write_failed;
  }
  const bool fsync_ok = ::fsync(fd) == 0;
  if (successor_commit) {
    g_dml_update_durable_commit_fsync_calls.fetch_add(
        1, std::memory_order_relaxed);
  }
  ::close(fd);
#endif
  if (!fsync_ok) return DmlUpdateDurableRawAppendResultV1::fsync_failed;
  if (successor_commit &&
      DmlUpdateDurableFault(
          MgaDmlUpdateDurableFaultCutpointV1::after_successor_fsync_before_ack)) {
    return DmlUpdateDurableRawAppendResultV1::after_fsync_ack_lost;
  }
  return DmlUpdateDurableRawAppendResultV1::ok;
}

bool DmlUpdateDurableAppendFrame(const std::string& path,
                                 const DmlUpdateDurableFrameV1& frame) {
  std::vector<std::uint8_t> encoded;
  if (!DmlUpdateDurableEncodeFrame(frame, &encoded)) return false;
  return DmlUpdateDurableAppendEncodedFrame(path, encoded, false) ==
         DmlUpdateDurableRawAppendResultV1::ok;
}

// Replace the complete descriptor extent only after its new contents are
// durable. PublishBound uses this path because the authority snapshot and the
// root DUJR are one admission decision: recovery may observe the old
// reservation or the complete bound operation, never a snapshot without its
// root journal extent.
bool DmlUpdateDurableReplaceFileAtomically(
    const std::string& path, std::span<const std::uint8_t> bytes) {
  if (bytes.empty()) return false;
  const std::string temporary = path + ".publish.tmp";
  std::error_code ignored;
  std::filesystem::remove(temporary, ignored);
#if defined(_WIN32)
  HANDLE handle = CreateFileA(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  std::size_t offset = 0;
  bool write_ok = true;
  while (offset < bytes.size()) {
    DWORD written = 0;
    const DWORD request = static_cast<DWORD>(std::min<std::size_t>(
        bytes.size() - offset, std::numeric_limits<DWORD>::max()));
    if (WriteFile(handle, bytes.data() + offset, request, &written, nullptr) ==
            0 ||
        written == 0) {
      write_ok = false;
      break;
    }
    offset += written;
  }
  const bool durable = write_ok && FlushFileBuffers(handle) != 0;
  CloseHandle(handle);
  if (!durable ||
      MoveFileExA(temporary.c_str(), path.c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    std::filesystem::remove(temporary, ignored);
    return false;
  }
  return true;
#else
  const int fd = ::open(temporary.c_str(),
                        O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) return false;
  std::size_t offset = 0;
  bool write_ok = true;
  while (offset < bytes.size()) {
    const ssize_t written =
        ::write(fd, bytes.data() + offset, bytes.size() - offset);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) {
      write_ok = false;
      break;
    }
    offset += static_cast<std::size_t>(written);
  }
  const bool durable = write_ok && ::fsync(fd) == 0;
  ::close(fd);
  if (!durable || ::rename(temporary.c_str(), path.c_str()) != 0) {
    std::filesystem::remove(temporary, ignored);
    return false;
  }
  const std::filesystem::path parent =
      std::filesystem::path(path).parent_path();
  const int directory_fd =
      ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (directory_fd < 0) return false;
  const bool directory_durable = ::fsync(directory_fd) == 0;
  ::close(directory_fd);
  return directory_durable;
#endif
}

bool DmlUpdateDurableBytesEqualUuid(
    std::span<const std::uint8_t> bytes, std::size_t offset,
    std::string_view uuid) {
  std::array<std::uint8_t, 16> expected{};
  return DmlUpdateDurableUuidBytes(uuid, &expected) &&
         offset <= bytes.size() && bytes.size() - offset >= expected.size() &&
         std::equal(expected.begin(), expected.end(), bytes.begin() + offset);
}

bool DmlUpdateDurableBytesEqual(
    std::span<const std::uint8_t> left, std::size_t left_offset,
    std::span<const std::uint8_t> right, std::size_t right_offset,
    std::size_t count) {
  return left_offset <= left.size() && left.size() - left_offset >= count &&
         right_offset <= right.size() && right.size() - right_offset >= count &&
         std::equal(left.begin() + left_offset,
                    left.begin() + left_offset + count,
                    right.begin() + right_offset);
}

bool DmlUpdateDurableCarrierHeader(
    std::span<const std::uint8_t> bytes, std::string_view magic,
    std::uint16_t header_bytes, bool exact_total) {
  std::uint16_t version = 0;
  std::uint16_t parsed_header = 0;
  std::uint32_t total = 0;
  std::uint32_t flags = 0;
  return magic.size() == 4 && bytes.size() >= 16 &&
         std::equal(magic.begin(), magic.end(), bytes.begin()) &&
         DmlUpdateDurableReadU16(bytes, 4, &version) && version == 1 &&
         DmlUpdateDurableReadU16(bytes, 6, &parsed_header) &&
         parsed_header == header_bytes &&
         DmlUpdateDurableReadU32(bytes, 8, &total) &&
         total == bytes.size() &&
         (!exact_total || total == header_bytes) &&
         DmlUpdateDurableReadU32(bytes, 12, &flags) && flags == 0;
}

bool DmlUpdateDurableVectorCarrier(
    std::span<const std::uint8_t> bytes, std::string_view magic,
    std::uint32_t minimum_count, std::uint32_t maximum_count,
    std::uint32_t fixed_record_bytes, std::uint32_t* record_count = nullptr) {
  std::uint32_t count = 0;
  std::uint32_t exact_records = 0;
  if (!DmlUpdateDurableCarrierHeader(bytes, magic, 104, false) ||
      !DmlUpdateDurableReadU32(bytes, 64, &count) ||
      !DmlUpdateDurableReadU32(bytes, 68, &exact_records) ||
      count < minimum_count || count > maximum_count ||
      exact_records != bytes.size() - 104) {
    return false;
  }
  if (fixed_record_bytes != 0 &&
      (count > std::numeric_limits<std::uint32_t>::max() /
                   fixed_record_bytes ||
       exact_records != count * fixed_record_bytes)) {
    return false;
  }
  if (record_count != nullptr) *record_count = count;
  return true;
}

bool DmlUpdateDurableSnapshotShallowValid(
    const MgaDmlUpdateDurableOperationIdentityV1& identity,
    const MgaDmlUpdateDurableAuthoritySnapshotV1& snapshot,
    std::uint64_t* structural_occurrence_id,
    std::string* detail) {
  const auto fail = [&](std::string reason) {
    if (detail != nullptr) *detail = std::move(reason);
    return false;
  };
  const auto descriptor = std::span<const std::uint8_t>(
      snapshot.descriptor_dudc);
  if (!DmlUpdateDurableCarrierHeader(descriptor, "DUDC", 712, true) ||
      !DmlUpdateDurableBytesEqualUuid(descriptor, 16,
                                      identity.descriptor_uuid) ||
      !DmlUpdateDurableBytesEqualUuid(
          descriptor, 40, identity.authenticated_statement_receipt_uuid) ||
      !DmlUpdateDurableBytesEqualUuid(descriptor, 64,
                                      identity.operation_uuid) ||
      !DmlUpdateDurableBytesEqualUuid(
          descriptor, 88, identity.owning_transaction_uuid) ||
      !DmlUpdateDurableBytesEqualUuid(descriptor, 624,
                                      identity.recovery_token_uuid)) {
    return fail("durable_snapshot_descriptor_identity_invalid");
  }
  std::uint64_t descriptor_generation = 0;
  std::uint64_t operation_generation = 0;
  std::uint64_t local_transaction_id = 0;
  std::uint64_t recovery_generation = 0;
  std::uint64_t structural_occurrence = 0;
  if (!DmlUpdateDurableReadU64(descriptor, 32, &descriptor_generation) ||
      !DmlUpdateDurableReadU64(descriptor, 56, &structural_occurrence) ||
      !DmlUpdateDurableReadU64(descriptor, 80, &operation_generation) ||
      !DmlUpdateDurableReadU64(descriptor, 104, &local_transaction_id) ||
      !DmlUpdateDurableReadU64(descriptor, 640, &recovery_generation) ||
      descriptor_generation != identity.descriptor_generation ||
      operation_generation != identity.operation_generation ||
      structural_occurrence == 0 ||
      local_transaction_id != identity.owning_local_transaction_id ||
      recovery_generation != identity.recovery_generation) {
    return fail("durable_snapshot_descriptor_generation_invalid");
  }
  if (structural_occurrence_id != nullptr) {
    *structural_occurrence_id = structural_occurrence;
  }

  struct VectorRule {
    const std::vector<std::uint8_t>* bytes;
    std::string_view magic;
    std::size_t descriptor_reference_offset;
    std::uint32_t minimum_count;
    std::uint32_t maximum_count;
    std::uint32_t fixed_record_bytes;
  };
  const std::array<VectorRule, 5> vectors{{
      {&snapshot.assignment_vector_duav, "DUAV", 248, 1, 1024, 0},
      {&snapshot.predicate_vector_duev, "DUEV", 312, 1, 3, 0},
      {&snapshot.row_policy_vector_dupv, "DUPV", 384, 0, 2, 176},
      {&snapshot.constraint_vector_ducv, "DUCV", 448, 0, 1048576, 160},
      {&snapshot.trigger_vector_dutv, "DUTV", 512, 0, 1048576, 192},
  }};
  for (const auto& vector : vectors) {
    const auto bytes = std::span<const std::uint8_t>(*vector.bytes);
    std::uint32_t count = 0;
    std::uint64_t vector_generation = 0;
    std::uint64_t referenced_generation = 0;
    if (!DmlUpdateDurableVectorCarrier(
            bytes, vector.magic, vector.minimum_count, vector.maximum_count,
            vector.fixed_record_bytes, &count) ||
        (vector.magic == "DUEV" && count != 1 && count != 3) ||
        !DmlUpdateDurableBytesEqual(bytes, 16, descriptor,
                                    vector.descriptor_reference_offset, 16) ||
        !DmlUpdateDurableReadU64(bytes, 32, &vector_generation) ||
        !DmlUpdateDurableReadU64(
            descriptor, vector.descriptor_reference_offset + 16,
            &referenced_generation) ||
        vector_generation != referenced_generation ||
        !DmlUpdateDurableBytesEqualUuid(bytes, 40,
                                        identity.descriptor_uuid) ||
        !DmlUpdateDurableReadU64(bytes, 56, &vector_generation) ||
        vector_generation != identity.descriptor_generation) {
      return fail("durable_snapshot_vector_identity_invalid");
    }
  }

  const auto order =
      std::span<const std::uint8_t>(snapshot.target_order_duor);
  const auto budget =
      std::span<const std::uint8_t>(snapshot.resource_budget_dubr);
  const auto recovery =
      std::span<const std::uint8_t>(snapshot.recovery_token_durc);
  const auto source_policies =
      std::span<const std::uint8_t>(snapshot.source_policy_vector_dusv);
  const auto security_proof =
      std::span<const std::uint8_t>(snapshot.security_snapshot_proof_dusp);
  if (!DmlUpdateDurableCarrierHeader(order, "DUOR", 160, true) ||
      !DmlUpdateDurableBytesEqual(order, 16, descriptor, 576, 24) ||
      !DmlUpdateDurableBytesEqualUuid(
          order, 40, identity.authenticated_statement_receipt_uuid) ||
      !DmlUpdateDurableCarrierHeader(budget, "DUBR", 208, true) ||
      !DmlUpdateDurableBytesEqual(budget, 16, descriptor, 600, 24) ||
      !DmlUpdateDurableBytesEqualUuid(
          budget, 40, identity.authenticated_statement_receipt_uuid) ||
      !DmlUpdateDurableBytesEqualUuid(
          budget, 56, identity.owning_transaction_uuid) ||
      !DmlUpdateDurableCarrierHeader(recovery, "DURC", 208, true) ||
      !DmlUpdateDurableBytesEqual(recovery, 16, descriptor, 624, 24) ||
      !DmlUpdateDurableBytesEqualUuid(
          recovery, 16, identity.recovery_token_uuid) ||
      !DmlUpdateDurableBytesEqualUuid(
          recovery, 40, identity.authenticated_statement_receipt_uuid) ||
      !DmlUpdateDurableBytesEqualUuid(
          recovery, 56, identity.owning_transaction_uuid) ||
      !DmlUpdateDurableBytesEqualUuid(recovery, 72,
                                      identity.operation_uuid) ||
      !DmlUpdateDurableBytesEqualUuid(recovery, 88,
                                      identity.descriptor_uuid) ||
      !DmlUpdateDurableBytesEqualUuid(
          recovery, 136, identity.validated_durable_handle_uuid)) {
    return fail("durable_snapshot_scalar_authority_invalid");
  }
  std::uint64_t scalar_generation = 0;
  if (!DmlUpdateDurableReadU64(recovery, 32, &scalar_generation) ||
      scalar_generation != identity.recovery_generation ||
      !DmlUpdateDurableReadU64(recovery, 104, &scalar_generation) ||
      scalar_generation != identity.descriptor_generation ||
      !DmlUpdateDurableReadU64(recovery, 152, &scalar_generation) ||
      scalar_generation != identity.validated_durable_handle_generation) {
    return fail("durable_snapshot_recovery_generation_invalid");
  }

  std::uint32_t source_policy_count = 0;
  std::uint32_t effective_policy_count = 0;
  std::uint32_t proof_effective_count = 0;
  std::uint32_t proof_source_count = 0;
  if (!DmlUpdateDurableVectorCarrier(source_policies, "DUSV", 0, 1048576,
                                     256, &source_policy_count) ||
      !DmlUpdateDurableVectorCarrier(
          snapshot.row_policy_vector_dupv, "DUPV", 0, 2, 176,
          &effective_policy_count) ||
      !DmlUpdateDurableCarrierHeader(security_proof, "DUSP", 576, true) ||
      !DmlUpdateDurableReadU32(security_proof, 352,
                               &proof_effective_count) ||
      !DmlUpdateDurableReadU32(security_proof, 356,
                               &proof_source_count) ||
      proof_effective_count != effective_policy_count ||
      proof_source_count != source_policy_count ||
      ((effective_policy_count == 0) != (source_policy_count == 0)) ||
      security_proof[360] != 1 ||
      !DmlUpdateDurableZero(security_proof.subspan(361, 7)) ||
      !DmlUpdateDurableZero(security_proof.subspan(560, 16)) ||
      !DmlUpdateDurableBytesEqualUuid(
          security_proof, 80, identity.database_uuid) ||
      !DmlUpdateDurableBytesEqualUuid(
          security_proof, 96,
          identity.authenticated_statement_receipt_uuid) ||
      !DmlUpdateDurableBytesEqualUuid(
          security_proof, 112, identity.owning_transaction_uuid) ||
      !DmlUpdateDurableBytesEqualUuid(
          security_proof, 136, identity.operation_uuid) ||
      !DmlUpdateDurableBytesEqualUuid(
          security_proof, 160, identity.recovery_token_uuid) ||
      !DmlUpdateDurableBytesEqualUuid(
          security_proof, 280, identity.descriptor_uuid) ||
      !DmlUpdateDurableBytesEqual(
          security_proof, 304, descriptor, 384, 24) ||
      !DmlUpdateDurableBytesEqual(
          security_proof, 328, source_policies, 16, 24) ||
      !DmlUpdateDurableBytesEqual(
          security_proof, 368, descriptor, 680, 32) ||
      !DmlUpdateDurableBytesEqual(
          security_proof, 400, descriptor, 416, 32) ||
      !DmlUpdateDurableBytesEqual(
          security_proof, 496, source_policies, 72, 32) ||
      DmlUpdateDurableSha256(descriptor) !=
          [&] {
            MgaDmlUpdateDurableSha256V1 value{};
            std::copy_n(security_proof.begin() + 432, 32, value.begin());
            return value;
          }() ||
      DmlUpdateDurableSha256(snapshot.row_policy_vector_dupv) !=
          [&] {
            MgaDmlUpdateDurableSha256V1 value{};
            std::copy_n(security_proof.begin() + 464, 32, value.begin());
            return value;
          }()) {
    return fail("durable_snapshot_security_authority_invalid");
  }
  std::uint64_t scalar = 0;
  const std::array<std::pair<std::size_t, std::uint64_t>, 6>
      security_generations{{
          {128, identity.owning_local_transaction_id},
          {152, identity.operation_generation},
          {176, identity.recovery_generation},
          {296, identity.descriptor_generation},
          {344, [&] {
             std::uint64_t value = 0;
             (void)DmlUpdateDurableReadU64(source_policies, 32, &value);
             return value;
           }()},
          {32, [&] {
             std::uint64_t value = 0;
             (void)DmlUpdateDurableReadU64(security_proof, 32, &value);
             return value;
           }()},
      }};
  for (const auto& [offset, expected] : security_generations) {
    if (!DmlUpdateDurableReadU64(security_proof, offset, &scalar) ||
        scalar == 0 || scalar != expected) {
      return fail("durable_snapshot_security_generation_invalid");
    }
  }

  // The MGA store does not merely preserve carrier-shaped bytes.  It accepts
  // a bound snapshot only after the canonical carrier codec has validated the
  // complete set and the two recovery-only security carriers byte-for-byte.
  // This remains a storage admission check; the UPDATE consumer performs the
  // live datatype/operator/security/catalog revalidation before publication.
  scratchbird::wire::TypedUpdateCarrierSet carriers;
  scratchbird::wire::TypedUpdateSecurityPolicySourceVector typed_sources;
  scratchbird::wire::TypedUpdateSecuritySnapshotProof typed_proof;
  scratchbird::wire::TypedUpdateDatatypeAuthorityVector typed_datatypes;
  scratchbird::wire::TypedUpdateBuiltinOperatorAuthorityVector typed_operators;
  scratchbird::wire::TypedUpdateCarrierError carrier_error;
  if (!scratchbird::wire::DecodeAndValidateTypedUpdateDescriptor(
          snapshot.descriptor_dudc, &carriers.descriptor, &carrier_error) ||
      !scratchbird::wire::DecodeAndValidateTypedUpdateAssignmentVector(
          snapshot.assignment_vector_duav, &carriers.assignments,
          &carrier_error) ||
      !scratchbird::wire::DecodeAndValidateTypedUpdatePredicateVector(
          snapshot.predicate_vector_duev, &carriers.predicate,
          &carrier_error) ||
      !scratchbird::wire::DecodeAndValidateTypedUpdateRowPolicyVector(
          snapshot.row_policy_vector_dupv, &carriers.row_policies,
          &carrier_error) ||
      !scratchbird::wire::DecodeAndValidateTypedUpdateConstraintVector(
          snapshot.constraint_vector_ducv, &carriers.constraints,
          &carrier_error) ||
      !scratchbird::wire::DecodeAndValidateTypedUpdateTriggerVector(
          snapshot.trigger_vector_dutv, &carriers.triggers,
          &carrier_error) ||
      !scratchbird::wire::DecodeAndValidateTypedUpdateTargetOrder(
          snapshot.target_order_duor, &carriers.target_order,
          &carrier_error) ||
      !scratchbird::wire::DecodeAndValidateTypedUpdateResourceBudget(
          snapshot.resource_budget_dubr, &carriers.resource_budget,
          &carrier_error) ||
      !scratchbird::wire::DecodeAndValidateTypedUpdateRecoveryToken(
          snapshot.recovery_token_durc, &carriers.recovery_token,
          &carrier_error) ||
      !scratchbird::wire::ValidateTypedUpdateCarrierSet(carriers,
                                                         &carrier_error) ||
      !scratchbird::wire::DecodeAndValidateTypedUpdateSecurityPolicySourceVector(
          snapshot.source_policy_vector_dusv, &typed_sources,
          &carrier_error) ||
      !scratchbird::wire::DecodeAndValidateTypedUpdateSecuritySnapshotProof(
          snapshot.security_snapshot_proof_dusp, &typed_proof,
          &carrier_error) ||
      !scratchbird::wire::DecodeAndValidateTypedUpdateDatatypeAuthorityVector(
          snapshot.datatype_authority_vector_dudv, &typed_datatypes,
          &carrier_error) ||
      !scratchbird::wire::DecodeAndValidateTypedUpdateBuiltinOperatorAuthorityVector(
          snapshot.builtin_operator_authority_vector_duov, &typed_operators,
          &carrier_error) ||
      !scratchbird::wire::ValidateTypedUpdateDatatypeOperatorAuthority(
          carriers.descriptor, carriers.assignments, carriers.predicate,
          typed_datatypes, typed_operators, &carrier_error)) {
    return fail("durable_snapshot_canonical_carrier_invalid:" +
                carrier_error.field + ":" + carrier_error.detail);
  }
  if (typed_sources.identity.owner_descriptor_uuid !=
          carriers.descriptor.descriptor_uuid ||
      typed_sources.identity.owner_descriptor_generation !=
          carriers.descriptor.descriptor_generation ||
      typed_proof.descriptor_uuid != carriers.descriptor.descriptor_uuid ||
      typed_proof.descriptor_generation !=
          carriers.descriptor.descriptor_generation ||
      typed_proof.source_policy_vector_uuid !=
          typed_sources.identity.vector_uuid ||
      typed_proof.source_policy_vector_generation !=
          typed_sources.identity.vector_generation ||
      typed_proof.source_policy_count != typed_sources.records.size() ||
      carriers.recovery_token.durable_registry_uuid !=
          [&] {
            scratchbird::wire::TypedUpdateUuid value{};
            (void)DmlUpdateDurableTypedUuid(
                identity.validated_durable_handle_uuid, &value);
            return value;
          }() ||
      carriers.recovery_token.durable_registry_generation !=
          identity.validated_durable_handle_generation) {
    return fail("durable_snapshot_typed_recovery_authority_mismatch");
  }
  return true;
}

constexpr std::array<std::uint8_t, 8> kDmlUpdateDurableSnapshotMagic{{
    'S', 'B', 'M', 'D', 'U', 'A', 'S', '1'}};
constexpr std::uint16_t kDmlUpdateDurableSnapshotVersion = 1;
constexpr std::uint16_t kDmlUpdateDurableSnapshotHeaderBytes = 128;
constexpr std::size_t kDmlUpdateDurableSnapshotCarrierCount = 13;
constexpr std::uint64_t kDmlUpdateDurableMaximumSnapshotBytes =
    64ULL * 1024ULL * 1024ULL;

std::array<const std::vector<std::uint8_t>*,
           kDmlUpdateDurableSnapshotCarrierCount>
DmlUpdateDurableSnapshotCarriers(
    const MgaDmlUpdateDurableAuthoritySnapshotV1& snapshot) {
  return {{&snapshot.assignment_vector_duav,
           &snapshot.predicate_vector_duev,
           &snapshot.row_policy_vector_dupv,
           &snapshot.constraint_vector_ducv,
           &snapshot.trigger_vector_dutv,
           &snapshot.target_order_duor,
           &snapshot.resource_budget_dubr,
           &snapshot.recovery_token_durc,
           &snapshot.source_policy_vector_dusv,
           &snapshot.security_snapshot_proof_dusp,
           &snapshot.datatype_authority_vector_dudv,
           &snapshot.builtin_operator_authority_vector_duov,
           &snapshot.descriptor_dudc}};
}

std::array<std::vector<std::uint8_t>*,
           kDmlUpdateDurableSnapshotCarrierCount>
DmlUpdateDurableMutableSnapshotCarriers(
    MgaDmlUpdateDurableAuthoritySnapshotV1* snapshot) {
  return {{&snapshot->assignment_vector_duav,
           &snapshot->predicate_vector_duev,
           &snapshot->row_policy_vector_dupv,
           &snapshot->constraint_vector_ducv,
           &snapshot->trigger_vector_dutv,
           &snapshot->target_order_duor,
           &snapshot->resource_budget_dubr,
           &snapshot->recovery_token_durc,
           &snapshot->source_policy_vector_dusv,
           &snapshot->security_snapshot_proof_dusp,
           &snapshot->datatype_authority_vector_dudv,
           &snapshot->builtin_operator_authority_vector_duov,
           &snapshot->descriptor_dudc}};
}

bool DmlUpdateDurableAddSize(std::uint64_t* total, std::uint64_t value) {
  if (total == nullptr || value > kDmlUpdateDurableMaximumSnapshotBytes ||
      *total > kDmlUpdateDurableMaximumSnapshotBytes - value) {
    return false;
  }
  *total += value;
  return true;
}

bool DmlUpdateDurableEncodeSnapshot(
    const MgaDmlUpdateDurableAuthoritySnapshotV1& snapshot,
    std::vector<std::uint8_t>* payload) {
  if (payload == nullptr) return false;
  const auto carriers = DmlUpdateDurableSnapshotCarriers(snapshot);
  std::uint64_t total = kDmlUpdateDurableSnapshotHeaderBytes;
  for (const auto* carrier : carriers) {
    if (carrier == nullptr || carrier->empty() ||
        !DmlUpdateDurableAddSize(&total, carrier->size())) {
      return false;
    }
  }
  if (total > std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }
  try {
    payload->assign(static_cast<std::size_t>(total), 0);
  } catch (const std::bad_alloc&) {
    return false;
  }
  std::copy(kDmlUpdateDurableSnapshotMagic.begin(),
            kDmlUpdateDurableSnapshotMagic.end(), payload->begin());
  DmlUpdateDurablePutU16(payload, 8, kDmlUpdateDurableSnapshotVersion);
  DmlUpdateDurablePutU16(payload, 10,
                         kDmlUpdateDurableSnapshotHeaderBytes);
  DmlUpdateDurablePutU32(payload, 12, static_cast<std::uint32_t>(total));
  for (std::size_t index = 0; index < carriers.size(); ++index) {
    DmlUpdateDurablePutU64(payload, 16 + index * 8,
                           carriers[index]->size());
  }
  std::size_t cursor = kDmlUpdateDurableSnapshotHeaderBytes;
  for (const auto* carrier : carriers) {
    std::copy(carrier->begin(), carrier->end(), payload->begin() + cursor);
    cursor += carrier->size();
  }
  return cursor == payload->size();
}

bool DmlUpdateDurableDecodeSnapshot(
    std::span<const std::uint8_t> payload,
    MgaDmlUpdateDurableAuthoritySnapshotV1* snapshot,
    std::string* detail) {
  const auto fail = [&](std::string reason) {
    if (detail != nullptr) *detail = std::move(reason);
    return false;
  };
  std::uint16_t version = 0;
  std::uint16_t header = 0;
  std::uint32_t total = 0;
  if (snapshot == nullptr || payload.size() < kDmlUpdateDurableSnapshotHeaderBytes ||
      payload.size() > kDmlUpdateDurableMaximumSnapshotBytes ||
      !std::equal(kDmlUpdateDurableSnapshotMagic.begin(),
                  kDmlUpdateDurableSnapshotMagic.end(), payload.begin()) ||
      !DmlUpdateDurableReadU16(payload, 8, &version) ||
      !DmlUpdateDurableReadU16(payload, 10, &header) ||
      !DmlUpdateDurableReadU32(payload, 12, &total) ||
      version != kDmlUpdateDurableSnapshotVersion ||
      header != kDmlUpdateDurableSnapshotHeaderBytes ||
      total != payload.size() ||
      !DmlUpdateDurableZero(payload.subspan(120, 8))) {
    return fail("durable_snapshot_payload_header_invalid");
  }
  MgaDmlUpdateDurableAuthoritySnapshotV1 decoded;
  const auto carriers = DmlUpdateDurableMutableSnapshotCarriers(&decoded);
  std::size_t cursor = kDmlUpdateDurableSnapshotHeaderBytes;
  for (std::size_t index = 0; index < carriers.size(); ++index) {
    std::uint64_t bytes = 0;
    if (!DmlUpdateDurableReadU64(payload, 16 + index * 8, &bytes) ||
        bytes == 0 || bytes > payload.size() - cursor) {
      return fail("durable_snapshot_payload_carrier_extent_invalid");
    }
    carriers[index]->assign(payload.begin() + cursor,
                            payload.begin() + cursor + bytes);
    cursor += static_cast<std::size_t>(bytes);
  }
  if (cursor != payload.size()) {
    return fail("durable_snapshot_payload_authority_extent_invalid");
  }
  *snapshot = std::move(decoded);
  return true;
}

struct DmlUpdateDurableParsedJournalV1 {
  std::uint64_t sequence = 0;
  MgaDmlUpdateDurableJournalStateV1 state =
      MgaDmlUpdateDurableJournalStateV1::bound;
  MgaDmlUpdateDurableSha256V1 prior{};
  MgaDmlUpdateDurableSha256V1 evidence{};
};

bool DmlUpdateDurableJournalShallowValid(
    const MgaDmlUpdateDurableOperationIdentityV1& identity,
    const MgaDmlUpdateDurableAuthoritySnapshotV1& snapshot,
    std::span<const std::uint8_t> bytes,
    DmlUpdateDurableParsedJournalV1* parsed, std::string* detail) {
  const auto fail = [&](std::string reason) {
    if (detail != nullptr) *detail = std::move(reason);
    return false;
  };
  if (parsed == nullptr ||
      (bytes.size() != 968 && bytes.size() != 1224) ||
      !DmlUpdateDurableCarrierHeader(bytes, "DUJR", 256, false)) {
    return fail("durable_journal_extent_invalid");
  }
  const std::uint8_t state = bytes[16];
  if (state < static_cast<std::uint8_t>(
                  MgaDmlUpdateDurableJournalStateV1::bound) ||
      state > static_cast<std::uint8_t>(
                  MgaDmlUpdateDurableJournalStateV1::aborted) ||
      !DmlUpdateDurableZero(bytes.subspan(17, 7)) ||
      !DmlUpdateDurableZero(bytes.subspan(188, 4)) ||
      !DmlUpdateDurableBytesEqualUuid(bytes, 32, identity.database_uuid) ||
      !DmlUpdateDurableBytesEqualUuid(bytes, 48,
                                      identity.descriptor_uuid) ||
      !DmlUpdateDurableBytesEqualUuid(
          bytes, 72, identity.authenticated_statement_receipt_uuid) ||
      !DmlUpdateDurableBytesEqualUuid(
          bytes, 88, identity.owning_transaction_uuid) ||
      !DmlUpdateDurableBytesEqualUuid(bytes, 112,
                                      identity.operation_uuid) ||
      !DmlUpdateDurableBytesEqualUuid(bytes, 128,
                                      identity.recovery_token_uuid)) {
    return fail("durable_journal_identity_invalid");
  }
  std::uint64_t descriptor_generation = 0;
  std::uint64_t local_transaction_id = 0;
  std::uint64_t recovery_generation = 0;
  std::uint32_t descriptor_bytes = 0;
  std::uint32_t result_bytes = 0;
  std::uint32_t payload_bytes = 0;
  if (!DmlUpdateDurableReadU64(bytes, 24, &parsed->sequence) ||
      !DmlUpdateDurableReadU64(bytes, 64, &descriptor_generation) ||
      !DmlUpdateDurableReadU64(bytes, 104, &local_transaction_id) ||
      !DmlUpdateDurableReadU64(bytes, 144, &recovery_generation) ||
      !DmlUpdateDurableReadU32(bytes, 176, &descriptor_bytes) ||
      !DmlUpdateDurableReadU32(bytes, 180, &result_bytes) ||
      !DmlUpdateDurableReadU32(bytes, 184, &payload_bytes) ||
      descriptor_generation != identity.descriptor_generation ||
      local_transaction_id != identity.owning_local_transaction_id ||
      recovery_generation != identity.recovery_generation ||
      descriptor_bytes != 712 || payload_bytes != 712 + result_bytes ||
      bytes.size() != 256 + payload_bytes ||
      !DmlUpdateDurableBytesEqual(
          bytes, 256, snapshot.descriptor_dudc, 0, 712)) {
    return fail("durable_journal_payload_or_generation_invalid");
  }
  const auto typed_state =
      static_cast<MgaDmlUpdateDurableJournalStateV1>(state);
  const bool requires_result =
      typed_state == MgaDmlUpdateDurableJournalStateV1::prepared ||
      typed_state == MgaDmlUpdateDurableJournalStateV1::published;
  if ((requires_result && result_bytes != 256) ||
      (!requires_result && result_bytes != 0)) {
    return fail("durable_journal_state_extent_invalid");
  }
  parsed->state = typed_state;
  std::copy_n(bytes.begin() + 192, 32, parsed->prior.begin());
  std::copy_n(bytes.begin() + 224, 32, parsed->evidence.begin());
  if (DmlUpdateDurableZero(parsed->evidence)) {
    return fail("durable_journal_evidence_missing");
  }
  return true;
}

bool DmlUpdateDurableJournalExtentMatchesBytes(
    const MgaDmlUpdateDurableOperationIdentityV1& identity,
    const MgaDmlUpdateDurableAuthoritySnapshotV1& snapshot,
    const MgaDmlUpdateDurableJournalExtentV1& extent,
    std::string* detail) {
  DmlUpdateDurableParsedJournalV1 parsed;
  if (!DmlUpdateDurableJournalShallowValid(
          identity, snapshot, extent.exact_dujr_bytes, &parsed, detail)) {
    return false;
  }
  if (parsed.sequence != extent.journal_sequence ||
      parsed.state != extent.lifecycle_state ||
      parsed.prior != extent.prior_record_sha256 ||
      parsed.evidence != extent.record_evidence_sha256) {
    if (detail != nullptr) {
      *detail = "durable_journal_supplied_metadata_mismatch";
    }
    return false;
  }
  return true;
}

bool DmlUpdateDurableLegalTransition(
    MgaDmlUpdateDurableJournalStateV1 prior,
    MgaDmlUpdateDurableJournalStateV1 next) {
  switch (prior) {
    case MgaDmlUpdateDurableJournalStateV1::bound:
      return next == MgaDmlUpdateDurableJournalStateV1::intent ||
             next == MgaDmlUpdateDurableJournalStateV1::aborted;
    case MgaDmlUpdateDurableJournalStateV1::intent:
      return next == MgaDmlUpdateDurableJournalStateV1::prepared ||
             next == MgaDmlUpdateDurableJournalStateV1::aborted;
    case MgaDmlUpdateDurableJournalStateV1::prepared:
      return next == MgaDmlUpdateDurableJournalStateV1::published ||
             next == MgaDmlUpdateDurableJournalStateV1::aborted;
    case MgaDmlUpdateDurableJournalStateV1::published:
    case MgaDmlUpdateDurableJournalStateV1::aborted:
      return false;
  }
  return false;
}

EngineApiDiagnostic DmlUpdateDurableDiagnostic(
    std::string code, std::string key, std::string detail = {}) {
  return MakeEngineApiDiagnostic(std::move(code), std::move(key),
                                 std::move(detail), true);
}

MgaDmlUpdateDurableOperationMutationResultV1 DmlUpdateDurableMutation(
    MgaDmlUpdateDurableOperationOutcomeV1 outcome, std::string detail = {}) {
  MgaDmlUpdateDurableOperationMutationResultV1 result;
  result.outcome = outcome;
  if (result.ok()) {
    result.diagnostic = OkDiagnostic();
  } else {
    const bool stale = outcome == MgaDmlUpdateDurableOperationOutcomeV1::stale;
    const bool denied =
        outcome == MgaDmlUpdateDurableOperationOutcomeV1::access_denied;
    result.diagnostic = DmlUpdateDurableDiagnostic(
        denied ? "SECURITY.ACCESS_DENIED"
               : stale ? "MGA.TRANSACTION.STALE" : "DML.UPDATE_FAILED",
        denied ? "sblr.dml_update_rows.durable_operation_denied"
               : stale ? "sblr.dml_update_rows.durable_operation_stale"
                       : "sblr.dml_update_rows.durable_operation_failed",
        std::move(detail));
  }
  return result;
}

std::string DmlUpdateDurablePathForLookup(
    const EngineRequestContext& context,
    const MgaDmlUpdateDurableOperationLookupV1& lookup) {
  std::array<std::uint8_t, 16> ignored{};
  if (context.database_path.empty() ||
      !DmlUpdateDurableUuidBytes(lookup.descriptor_uuid, &ignored) ||
      lookup.descriptor_generation == 0 ||
      lookup.structural_occurrence_id == 0) {
    return {};
  }
  return DmlUpdateDurableOperationStorePath(context) + "/" +
         lookup.descriptor_uuid + ".duop";
}

bool DmlUpdateDurableSameReservationRequest(
    const EngineRequestContext& context,
    const MgaDmlUpdateDurableAuthorityReservationRequestV1& request,
    const MgaDmlUpdateDurableOperationIdentityV1& identity) {
  return identity.database_uuid == context.database_uuid.canonical &&
         identity.owning_transaction_uuid ==
             context.transaction_uuid.canonical &&
         identity.owning_local_transaction_id ==
             context.local_transaction_id &&
         identity.authenticated_statement_receipt_uuid ==
             context.statement_receipt_uuid.canonical &&
         identity.operation_uuid == request.operation_uuid &&
         identity.operation_generation == request.operation_generation &&
         identity.descriptor_uuid == request.descriptor_uuid &&
         identity.descriptor_generation == request.descriptor_generation &&
         identity.recovery_token_uuid == request.recovery_token_uuid &&
         identity.recovery_generation == request.recovery_generation;
}

std::string DmlUpdateDurableFreshIdentity(
    const MgaDmlUpdateDurableOperationIdentityV1& identity,
    std::string_view other = {}) {
  for (std::size_t attempt = 0; attempt < 16; ++attempt) {
    const std::string candidate = GenerateCrudEngineUuid("object");
    std::array<std::uint8_t, 16> ignored{};
    if (DmlUpdateDurableUuidBytes(candidate, &ignored) &&
        candidate != identity.database_uuid &&
        candidate != identity.owning_transaction_uuid &&
        candidate != identity.authenticated_statement_receipt_uuid &&
        candidate != identity.operation_uuid &&
        candidate != identity.descriptor_uuid &&
        candidate != identity.recovery_token_uuid && candidate != other) {
      return candidate;
    }
  }
  return {};
}

std::string DmlUpdateDurableQuarantinePath(const std::string& path) {
  return path + ".quarantine";
}

bool DmlUpdateDurableIsQuarantined(const std::string& path) {
  std::error_code error;
  return std::filesystem::exists(DmlUpdateDurableQuarantinePath(path), error) &&
         !error;
}

bool DmlUpdateDurableWriteQuarantine(const std::string& path) {
  const std::array<std::uint8_t, 16> marker{{
      'S', 'B', 'M', 'D', 'U', 'Q', '1', 0, 1, 0, 0, 0, 0, 0, 0, 0}};
  const std::string quarantine = DmlUpdateDurableQuarantinePath(path);
#if defined(_WIN32)
  HANDLE handle = CreateFileA(quarantine.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  const bool ok = WriteFile(handle, marker.data(), marker.size(), &written,
                            nullptr) != 0 &&
                  written == marker.size() && FlushFileBuffers(handle) != 0;
  CloseHandle(handle);
  return ok;
#else
  const int fd = ::open(quarantine.c_str(),
                        O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) return false;
  std::size_t offset = 0;
  while (offset < marker.size()) {
    const ssize_t written =
        ::write(fd, marker.data() + offset, marker.size() - offset);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) {
      ::close(fd);
      return false;
    }
    offset += static_cast<std::size_t>(written);
  }
  const bool ok = ::fsync(fd) == 0;
  ::close(fd);
  return ok;
#endif
}

struct DmlUpdateDurableStoredOperationV1 {
  bool ok = false;
  bool missing = false;
  bool reservation_only = false;
  bool snapshot_present = false;
  bool quarantined = false;
  std::string detail;
  MgaDmlUpdateDurableOperationIdentityV1 identity;
  MgaDmlUpdateDurableAuthoritySnapshotV1 snapshot;
  std::vector<MgaDmlUpdateDurableJournalExtentV1> journal;
  bool staged_successor_present = false;
  MgaDmlUpdateDurableJournalExtentV1 staged_successor;
  std::vector<std::uint8_t> staged_encoded_journal_frame;
  std::vector<std::uint8_t> latest_dumo;
  std::uint64_t structural_occurrence_id = 0;
};

DmlUpdateDurableStoredOperationV1 DmlUpdateDurableLoadOperation(
    const std::string& path, bool quarantine_on_corruption) {
  DmlUpdateDurableStoredOperationV1 result;
  if (DmlUpdateDurableIsQuarantined(path)) {
    result.quarantined = true;
    result.detail = "durable_operation_quarantined";
    return result;
  }
  const auto loaded = DmlUpdateDurableLoadFrames(path);
  if (!loaded.ok) {
    result.missing = loaded.missing;
    result.detail = loaded.detail;
    if (!loaded.missing && quarantine_on_corruption) {
      result.quarantined = DmlUpdateDurableWriteQuarantine(path);
    }
    return result;
  }
  if (loaded.missing || loaded.frames.empty()) {
    result.missing = true;
    return result;
  }
  auto corrupt = [&](std::string detail) {
    result.detail = std::move(detail);
    if (quarantine_on_corruption) {
      result.quarantined = DmlUpdateDurableWriteQuarantine(path);
    }
    return result;
  };
  const auto& reservation = loaded.frames.front();
  if (reservation.kind !=
          DmlUpdateDurableFrameKindV1::authority_reservation ||
      reservation.sequence != 0 || reservation.state != 0 ||
      reservation.flags != 0 || !reservation.payload.empty() ||
      !DmlUpdateDurableZero(reservation.prior_record_sha256) ||
      !DmlUpdateDurableZero(reservation.record_evidence_sha256)) {
    return corrupt("durable_reservation_frame_invalid");
  }
  result.identity = reservation.identity;
  if (loaded.frames.size() == 1) {
    result.ok = true;
    result.reservation_only = true;
    return result;
  }
  const auto& snapshot = loaded.frames[1];
  if (snapshot.kind != DmlUpdateDurableFrameKindV1::authority_snapshot ||
      snapshot.identity != result.identity || snapshot.sequence != 0 ||
      snapshot.state != 0 || snapshot.flags != 0 ||
      !DmlUpdateDurableZero(snapshot.prior_record_sha256) ||
      !DmlUpdateDurableZero(snapshot.record_evidence_sha256) ||
      !DmlUpdateDurableDecodeSnapshot(snapshot.payload, &result.snapshot,
                                      &result.detail) ||
      !DmlUpdateDurableSnapshotShallowValid(
          result.identity, result.snapshot,
          &result.structural_occurrence_id, &result.detail)) {
    return corrupt(result.detail.empty() ? "durable_snapshot_frame_invalid"
                                         : result.detail);
  }
  result.snapshot_present = true;
  std::size_t cursor = 2;
  for (; cursor < loaded.frames.size(); ++cursor) {
    const auto& frame = loaded.frames[cursor];
    if (frame.identity != result.identity || frame.flags != 0) {
      return corrupt("durable_frame_cross_identity");
    }
    if (frame.kind == DmlUpdateDurableFrameKindV1::recovery_observation) {
      if (frame.payload.size() != 416 ||
          !std::equal(frame.payload.begin(), frame.payload.begin() + 4,
                      "DUMO")) {
        return corrupt("durable_observation_extent_invalid");
      }
      result.latest_dumo = frame.payload;
      continue;
    }
    if (frame.kind ==
        DmlUpdateDurableFrameKindV1::prepared_successor_invalidated) {
      if (!result.staged_successor_present || !frame.payload.empty() ||
          frame.sequence != result.staged_successor.journal_sequence ||
          frame.state != static_cast<std::uint8_t>(
                             result.staged_successor.lifecycle_state) ||
          frame.prior_record_sha256 !=
              result.staged_successor.prior_record_sha256 ||
          frame.record_evidence_sha256 !=
              result.staged_successor.record_evidence_sha256) {
        return corrupt("durable_prepared_successor_invalidation_invalid");
      }
      result.staged_successor_present = false;
      result.staged_successor = {};
      result.staged_encoded_journal_frame.clear();
      continue;
    }
    if (frame.kind == DmlUpdateDurableFrameKindV1::prepared_successor) {
      if (result.journal.empty() || result.staged_successor_present ||
          frame.flags != 0 || frame.payload.empty()) {
        return corrupt("durable_prepared_successor_position_invalid");
      }
      DmlUpdateDurableFrameV1 staged_frame;
      if (!DmlUpdateDurableDecodeFrame(frame.payload, &staged_frame,
                                       &result.detail) ||
          staged_frame.kind != DmlUpdateDurableFrameKindV1::journal ||
          staged_frame.identity != result.identity ||
          staged_frame.flags != 0 ||
          frame.sequence != staged_frame.sequence ||
          frame.state != staged_frame.state ||
          frame.prior_record_sha256 !=
              staged_frame.prior_record_sha256 ||
          frame.record_evidence_sha256 !=
              staged_frame.record_evidence_sha256) {
        return corrupt(result.detail.empty()
                           ? "durable_prepared_successor_frame_invalid"
                           : result.detail);
      }
      MgaDmlUpdateDurableJournalExtentV1 staged_extent;
      staged_extent.journal_sequence = staged_frame.sequence;
      staged_extent.lifecycle_state =
          static_cast<MgaDmlUpdateDurableJournalStateV1>(staged_frame.state);
      staged_extent.prior_record_sha256 =
          staged_frame.prior_record_sha256;
      staged_extent.record_evidence_sha256 =
          staged_frame.record_evidence_sha256;
      staged_extent.exact_dujr_bytes = staged_frame.payload;
      const auto& prior = result.journal.back();
      if (!DmlUpdateDurableJournalExtentMatchesBytes(
              result.identity, result.snapshot, staged_extent,
              &result.detail) ||
          staged_extent.journal_sequence != prior.journal_sequence + 1 ||
          staged_extent.prior_record_sha256 !=
              prior.record_evidence_sha256 ||
          !DmlUpdateDurableLegalTransition(
              prior.lifecycle_state, staged_extent.lifecycle_state)) {
        return corrupt(result.detail.empty()
                           ? "durable_prepared_successor_cas_invalid"
                           : result.detail);
      }
      result.staged_successor_present = true;
      result.staged_successor = std::move(staged_extent);
      result.staged_encoded_journal_frame = frame.payload;
      continue;
    }
    if (frame.kind != DmlUpdateDurableFrameKindV1::journal) {
      return corrupt("durable_frame_kind_invalid");
    }
    MgaDmlUpdateDurableJournalExtentV1 extent;
    extent.journal_sequence = frame.sequence;
    extent.lifecycle_state =
        static_cast<MgaDmlUpdateDurableJournalStateV1>(frame.state);
    extent.prior_record_sha256 = frame.prior_record_sha256;
    extent.record_evidence_sha256 = frame.record_evidence_sha256;
    extent.exact_dujr_bytes = frame.payload;
    if (!DmlUpdateDurableJournalExtentMatchesBytes(
            result.identity, result.snapshot, extent, &result.detail)) {
      return corrupt(result.detail);
    }
    if (result.journal.empty()) {
      if (extent.journal_sequence != 1 ||
          extent.lifecycle_state !=
              MgaDmlUpdateDurableJournalStateV1::bound ||
          !DmlUpdateDurableZero(extent.prior_record_sha256)) {
        return corrupt("durable_journal_root_invalid");
      }
    } else {
      const auto& prior = result.journal.back();
      if (extent.journal_sequence != prior.journal_sequence + 1 ||
          extent.prior_record_sha256 != prior.record_evidence_sha256 ||
          !DmlUpdateDurableLegalTransition(prior.lifecycle_state,
                                           extent.lifecycle_state)) {
        return corrupt("durable_journal_chain_forked");
      }
    }
    if (result.staged_successor_present) {
      if (extent != result.staged_successor) {
        return corrupt("durable_prepared_successor_commit_mismatch");
      }
      result.staged_successor_present = false;
      result.staged_successor = {};
      result.staged_encoded_journal_frame.clear();
    }
    result.journal.push_back(std::move(extent));
  }
  if (result.journal.empty()) {
    result.reservation_only = false;
  }
  result.ok = true;
  return result;
}

DmlUpdateDurableFrameV1 DmlUpdateDurableJournalFrame(
    const MgaDmlUpdateDurableOperationIdentityV1& identity,
    const MgaDmlUpdateDurableJournalExtentV1& extent) {
  DmlUpdateDurableFrameV1 frame;
  frame.kind = DmlUpdateDurableFrameKindV1::journal;
  frame.identity = identity;
  frame.sequence = extent.journal_sequence;
  frame.state = static_cast<std::uint8_t>(extent.lifecycle_state);
  frame.prior_record_sha256 = extent.prior_record_sha256;
  frame.record_evidence_sha256 = extent.record_evidence_sha256;
  frame.payload = extent.exact_dujr_bytes;
  return frame;
}

bool DmlUpdateDurableEncodePreparedInvalidation(
    const MgaDmlUpdateDurableOperationIdentityV1& identity,
    const MgaDmlUpdateDurableJournalExtentV1& staged,
    std::vector<std::uint8_t>* encoded) {
  DmlUpdateDurableFrameV1 invalidation;
  invalidation.kind =
      DmlUpdateDurableFrameKindV1::prepared_successor_invalidated;
  invalidation.identity = identity;
  invalidation.sequence = staged.journal_sequence;
  invalidation.state =
      static_cast<std::uint8_t>(staged.lifecycle_state);
  invalidation.prior_record_sha256 = staged.prior_record_sha256;
  invalidation.record_evidence_sha256 = staged.record_evidence_sha256;
  g_dml_update_durable_frame_encode_calls.fetch_add(
      1, std::memory_order_relaxed);
  g_dml_update_durable_checksum_calls.fetch_add(
      2, std::memory_order_relaxed);
  return DmlUpdateDurableEncodeFrame(invalidation, encoded);
}

constexpr std::size_t kDmlUpdateStatementSavepointJournalFields = 27;

struct DmlUpdateStatementSavepointJournalRecordV1 {
  MgaDmlUpdateStatementSavepointAuthorityV1 authority;
  std::string private_marker;
  std::uint64_t journal_sequence = 0;
  SavepointCutoffs cutoffs;
  std::uint64_t row_upper_event_sequence = 0;
  std::uint64_t metadata_upper_event_sequence = 0;
  std::uint64_t index_upper_event_sequence = 0;
  MgaDmlUpdateStatementAuthoritySha256V1 prior_record_sha256{};
};

std::mutex& DmlUpdateStatementSavepointJournalMutex() {
  static std::mutex mutex;
  return mutex;
}

EngineApiDiagnostic DmlUpdateStatementSavepointDiagnostic(
    std::string code, std::string key, std::string detail = {}) {
  return MakeEngineApiDiagnostic(std::move(code), std::move(key),
                                 std::move(detail), true);
}

MgaDmlUpdateStatementSavepointAuthorityResultV1
DmlUpdateStatementSavepointFailure(std::string code, std::string key,
                                   std::string detail = {}) {
  MgaDmlUpdateStatementSavepointAuthorityResultV1 result;
  result.diagnostic = DmlUpdateStatementSavepointDiagnostic(
      std::move(code), std::move(key), std::move(detail));
  return result;
}

bool DmlUpdateStatementParseU64(std::string_view text,
                                std::uint64_t* value) {
  if (value == nullptr || text.empty()) return false;
  std::uint64_t parsed = 0;
  const auto converted =
      std::from_chars(text.data(), text.data() + text.size(), parsed, 10);
  if (converted.ec != std::errc{} ||
      converted.ptr != text.data() + text.size()) {
    return false;
  }
  *value = parsed;
  return true;
}

bool DmlUpdateStatementParseUuid(
    std::string_view text, std::array<std::uint8_t, 16>* bytes = nullptr) {
  if (text.empty()) return false;
  const auto parsed = scratchbird::core::uuid::ParseUuid(std::string(text));
  if (!parsed.ok() || scratchbird::core::uuid::IsNilUuid(parsed.value) ||
      scratchbird::core::uuid::UuidToString(parsed.value) != text) {
    return false;
  }
  if (bytes != nullptr) {
    std::copy(parsed.value.bytes.begin(), parsed.value.bytes.end(),
              bytes->begin());
  }
  return true;
}

bool DmlUpdateStatementShaNonzero(
    const MgaDmlUpdateStatementAuthoritySha256V1& value) {
  return std::any_of(value.begin(), value.end(),
                     [](std::uint8_t byte) { return byte != 0; });
}

std::string DmlUpdateStatementShaHex(
    const MgaDmlUpdateStatementAuthoritySha256V1& value) {
  return scratchbird::core::hash::HexLower(value);
}

bool DmlUpdateStatementParseSha(
    std::string_view text, MgaDmlUpdateStatementAuthoritySha256V1* value) {
  if (value == nullptr || text.size() != value->size() * 2) return false;
  MgaDmlUpdateStatementAuthoritySha256V1 parsed{};
  for (std::size_t index = 0; index < parsed.size(); ++index) {
    const int high = HexValue(text[index * 2]);
    const int low = HexValue(text[index * 2 + 1]);
    if (high < 0 || low < 0) return false;
    parsed[index] = static_cast<std::uint8_t>((high << 4) | low);
  }
  *value = parsed;
  return true;
}

void DmlUpdateStatementAppendU64(std::vector<std::uint8_t>* bytes,
                                 std::uint64_t value) {
  for (std::size_t offset = 0; offset < 8; ++offset) {
    bytes->push_back(
        static_cast<std::uint8_t>((value >> (offset * 8)) & 0xffu));
  }
}

bool DmlUpdateStatementAppendUuid(std::vector<std::uint8_t>* bytes,
                                  std::string_view uuid,
                                  bool optional = false) {
  if (optional && uuid.empty()) {
    bytes->insert(bytes->end(), 16, 0);
    return true;
  }
  std::array<std::uint8_t, 16> parsed{};
  if (!DmlUpdateStatementParseUuid(uuid, &parsed)) return false;
  bytes->insert(bytes->end(), parsed.begin(), parsed.end());
  return true;
}

MgaDmlUpdateStatementAuthoritySha256V1
DmlUpdateStatementSavepointRecordSha256(
    const DmlUpdateStatementSavepointJournalRecordV1& record) {
  std::vector<std::uint8_t> material;
  material.reserve(kDmlUpdateStatementSavepointEvidenceDomain.size() + 257);
  material.insert(material.end(),
                  kDmlUpdateStatementSavepointEvidenceDomain.begin(),
                  kDmlUpdateStatementSavepointEvidenceDomain.end());
  material.push_back(1);
  material.push_back(static_cast<std::uint8_t>(record.authority.lifecycle));
  material.push_back(record.authority.publication_barrier_present ? 1 : 0);
  DmlUpdateStatementAppendU64(&material, record.journal_sequence);
  DmlUpdateStatementAppendU64(&material,
                              record.cutoffs.row_event_sequence);
  DmlUpdateStatementAppendU64(&material,
                              record.cutoffs.metadata_event_sequence);
  DmlUpdateStatementAppendU64(&material,
                              record.cutoffs.index_event_sequence);
  DmlUpdateStatementAppendU64(&material, record.row_upper_event_sequence);
  DmlUpdateStatementAppendU64(&material,
                              record.metadata_upper_event_sequence);
  DmlUpdateStatementAppendU64(&material, record.index_upper_event_sequence);
  const auto& binding = record.authority.binding;
  if (!DmlUpdateStatementAppendUuid(&material, binding.database_uuid) ||
      !DmlUpdateStatementAppendUuid(&material,
                                    binding.owning_transaction_uuid) ||
      !DmlUpdateStatementAppendUuid(
          &material, binding.authenticated_statement_receipt_uuid) ||
      !DmlUpdateStatementAppendUuid(&material, binding.operation_uuid) ||
      !DmlUpdateStatementAppendUuid(&material, binding.descriptor_uuid) ||
      !DmlUpdateStatementAppendUuid(&material, binding.recovery_token_uuid) ||
      !DmlUpdateStatementAppendUuid(&material,
                                    record.authority.savepoint_uuid) ||
      !DmlUpdateStatementAppendUuid(
          &material, record.authority.publication_barrier_uuid, true)) {
    return {};
  }
  DmlUpdateStatementAppendU64(&material,
                              binding.owning_local_transaction_id);
  DmlUpdateStatementAppendU64(&material, binding.descriptor_generation);
  DmlUpdateStatementAppendU64(&material, binding.recovery_generation);
  DmlUpdateStatementAppendU64(&material,
                              record.authority.savepoint_generation);
  DmlUpdateStatementAppendU64(
      &material, record.authority.publication_barrier_generation);
  material.insert(material.end(), record.prior_record_sha256.begin(),
                  record.prior_record_sha256.end());
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(material);
  return digest.ok() ? digest.digest
                     : MgaDmlUpdateStatementAuthoritySha256V1{};
}

std::string DmlUpdateStatementPrivateSavepointMarker(
    std::string_view savepoint_uuid) {
  if (!DmlUpdateStatementParseUuid(savepoint_uuid)) return {};
  std::string marker = "__sblr_dml_update_rows_";
  marker.reserve(marker.size() + 32);
  for (const char value : savepoint_uuid) {
    if (value != '-') marker.push_back(value);
  }
  return marker;
}

MgaDmlUpdateDurableOperationIdentityV1
DmlUpdateStatementDurableIdentity(
    const MgaDmlUpdateStatementSavepointBindingV1& binding) {
  MgaDmlUpdateDurableOperationIdentityV1 identity;
  identity.database_uuid = binding.database_uuid;
  identity.owning_transaction_uuid = binding.owning_transaction_uuid;
  identity.owning_local_transaction_id =
      binding.owning_local_transaction_id;
  identity.authenticated_statement_receipt_uuid =
      binding.authenticated_statement_receipt_uuid;
  identity.operation_uuid = binding.operation_uuid;
  identity.descriptor_uuid = binding.descriptor_uuid;
  identity.descriptor_generation = binding.descriptor_generation;
  identity.recovery_token_uuid = binding.recovery_token_uuid;
  identity.recovery_generation = binding.recovery_generation;
  return identity;
}

bool DmlUpdateStatementEncodeBinaryPayload(
    const DmlUpdateStatementSavepointJournalRecordV1& record,
    std::vector<std::uint8_t>* payload) {
  if (payload == nullptr) return false;
  payload->assign(136, 0);
  if (!DmlUpdateDurablePutUuid(payload, 0,
                               record.authority.savepoint_uuid) ||
      !DmlUpdateDurablePutUuid(
          payload, 24, record.authority.publication_barrier_uuid)) {
    return false;
  }
  DmlUpdateDurablePutU64(payload, 16,
                         record.authority.savepoint_generation);
  DmlUpdateDurablePutU64(
      payload, 40, record.authority.publication_barrier_generation);
  (*payload)[48] = record.authority.publication_barrier_present ? 1 : 0;
  (*payload)[49] =
      static_cast<std::uint8_t>(record.authority.lifecycle);
  DmlUpdateDurablePutU64(payload, 56,
                         record.cutoffs.row_event_sequence);
  DmlUpdateDurablePutU64(payload, 64,
                         record.cutoffs.metadata_event_sequence);
  DmlUpdateDurablePutU64(payload, 72,
                         record.cutoffs.index_event_sequence);
  DmlUpdateDurablePutU64(payload, 80, record.row_upper_event_sequence);
  DmlUpdateDurablePutU64(payload, 88,
                         record.metadata_upper_event_sequence);
  DmlUpdateDurablePutU64(payload, 96,
                         record.index_upper_event_sequence);
  std::copy(record.authority.durable_presence_sha256.begin(),
            record.authority.durable_presence_sha256.end(),
            payload->begin() + 104);
  return true;
}

bool DmlUpdateStatementDecodeBinaryFrame(
    const DmlUpdateDurableFrameV1& frame,
    DmlUpdateStatementSavepointJournalRecordV1* record) {
  if (record == nullptr ||
      frame.kind != DmlUpdateDurableFrameKindV1::statement_savepoint ||
      frame.flags != 0 || frame.payload.size() != 136 ||
      frame.sequence < 1 || frame.sequence > 2 ||
      frame.state < static_cast<std::uint8_t>(
                        MgaDmlUpdateStatementSavepointLifecycleV1::active) ||
      frame.state > static_cast<std::uint8_t>(
                        MgaDmlUpdateStatementSavepointLifecycleV1::released) ||
      !DmlUpdateDurableZero(
          std::span<const std::uint8_t>(frame.payload).subspan(50, 6))) {
    return false;
  }
  record->authority.binding.database_uuid = frame.identity.database_uuid;
  record->authority.binding.owning_transaction_uuid =
      frame.identity.owning_transaction_uuid;
  record->authority.binding.owning_local_transaction_id =
      frame.identity.owning_local_transaction_id;
  record->authority.binding.authenticated_statement_receipt_uuid =
      frame.identity.authenticated_statement_receipt_uuid;
  record->authority.binding.operation_uuid = frame.identity.operation_uuid;
  record->authority.binding.descriptor_uuid = frame.identity.descriptor_uuid;
  record->authority.binding.descriptor_generation =
      frame.identity.descriptor_generation;
  record->authority.binding.recovery_token_uuid =
      frame.identity.recovery_token_uuid;
  record->authority.binding.recovery_generation =
      frame.identity.recovery_generation;
  record->authority.savepoint_uuid = DmlUpdateDurableUuidText(
      std::span<const std::uint8_t>(frame.payload).subspan(0, 16));
  record->authority.publication_barrier_uuid = DmlUpdateDurableUuidText(
      std::span<const std::uint8_t>(frame.payload).subspan(24, 16));
  if (!DmlUpdateDurableReadU64(frame.payload, 16,
                               &record->authority.savepoint_generation) ||
      !DmlUpdateDurableReadU64(
          frame.payload, 40,
          &record->authority.publication_barrier_generation) ||
      !DmlUpdateDurableReadU64(frame.payload, 56,
                               &record->cutoffs.row_event_sequence) ||
      !DmlUpdateDurableReadU64(frame.payload, 64,
                               &record->cutoffs.metadata_event_sequence) ||
      !DmlUpdateDurableReadU64(frame.payload, 72,
                               &record->cutoffs.index_event_sequence) ||
      !DmlUpdateDurableReadU64(frame.payload, 80,
                               &record->row_upper_event_sequence) ||
      !DmlUpdateDurableReadU64(frame.payload, 88,
                               &record->metadata_upper_event_sequence) ||
      !DmlUpdateDurableReadU64(frame.payload, 96,
                               &record->index_upper_event_sequence)) {
    return false;
  }
  record->authority.publication_barrier_present = frame.payload[48] != 0;
  record->authority.lifecycle =
      static_cast<MgaDmlUpdateStatementSavepointLifecycleV1>(
          frame.payload[49]);
  std::copy_n(frame.payload.begin() + 104, 32,
              record->authority.durable_presence_sha256.begin());
  record->journal_sequence = frame.sequence;
  record->prior_record_sha256 = frame.prior_record_sha256;
  record->private_marker = DmlUpdateStatementPrivateSavepointMarker(
      record->authority.savepoint_uuid);
  const bool terminal =
      record->authority.lifecycle !=
      MgaDmlUpdateStatementSavepointLifecycleV1::active;
  const bool barrier_shape =
      DmlUpdateStatementParseUuid(
          record->authority.publication_barrier_uuid) &&
      record->authority.publication_barrier_generation == 1 &&
      record->authority.publication_barrier_uuid !=
          record->authority.savepoint_uuid &&
      record->authority.publication_barrier_present ==
          (record->authority.lifecycle ==
           MgaDmlUpdateStatementSavepointLifecycleV1::released);
  const bool active_shape =
      record->authority.lifecycle !=
          MgaDmlUpdateStatementSavepointLifecycleV1::active ||
      (record->row_upper_event_sequence == 0 &&
       record->metadata_upper_event_sequence == 0 &&
       record->index_upper_event_sequence == 0 &&
       !DmlUpdateStatementShaNonzero(record->prior_record_sha256));
  const bool release_shape =
      record->authority.lifecycle !=
          MgaDmlUpdateStatementSavepointLifecycleV1::released ||
      (record->row_upper_event_sequence == 0 &&
       record->metadata_upper_event_sequence == 0 &&
       record->index_upper_event_sequence == 0);
  const bool rollback_shape =
      record->authority.lifecycle !=
          MgaDmlUpdateStatementSavepointLifecycleV1::rolled_back ||
      (record->row_upper_event_sequence >=
           record->cutoffs.row_event_sequence &&
       record->metadata_upper_event_sequence >=
           record->cutoffs.metadata_event_sequence &&
       record->index_upper_event_sequence >=
           record->cutoffs.index_event_sequence);
  const auto expected = DmlUpdateStatementSavepointRecordSha256(*record);
  return frame.state == frame.payload[49] &&
         record->authority.savepoint_generation == 1 &&
         DmlUpdateStatementParseUuid(record->authority.savepoint_uuid) &&
         !record->private_marker.empty() &&
         record->journal_sequence == (terminal ? 2 : 1) && barrier_shape &&
         active_shape && release_shape && rollback_shape &&
         frame.record_evidence_sha256 ==
             record->authority.durable_presence_sha256 &&
         DmlUpdateStatementShaNonzero(expected) &&
         expected == record->authority.durable_presence_sha256;
}

bool DmlUpdateStatementLoadBinaryChain(
    const EngineRequestContext& context, std::string_view savepoint_uuid,
    std::vector<DmlUpdateStatementSavepointJournalRecordV1>* records,
    std::string* detail) {
  if (records == nullptr || !DmlUpdateStatementParseUuid(savepoint_uuid)) {
    if (detail != nullptr) *detail = "savepoint_identity_invalid";
    return false;
  }
  const std::string path =
      DmlUpdateDurableSavepointPath(context, savepoint_uuid);
  DmlUpdateDurableFileLock lock(path);
  if (!lock.ok()) {
    if (detail != nullptr) *detail = "savepoint_store_lock_failed";
    return false;
  }
  const auto loaded = DmlUpdateDurableLoadFrames(path);
  if (!loaded.ok || loaded.missing || loaded.frames.empty() ||
      loaded.frames.size() > 2) {
    if (detail != nullptr) {
      *detail = loaded.detail.empty() ? "savepoint_identity_unknown"
                                     : loaded.detail;
    }
    return false;
  }
  records->clear();
  records->reserve(loaded.frames.size());
  for (const auto& frame : loaded.frames) {
    DmlUpdateStatementSavepointJournalRecordV1 record;
    if (!DmlUpdateStatementDecodeBinaryFrame(frame, &record) ||
        record.authority.savepoint_uuid != savepoint_uuid) {
      if (detail != nullptr) *detail = "savepoint_binary_record_invalid";
      return false;
    }
    records->push_back(std::move(record));
  }
  const auto& first = records->front();
  if (first.journal_sequence != 1 ||
      first.authority.lifecycle !=
          MgaDmlUpdateStatementSavepointLifecycleV1::active ||
      DmlUpdateStatementShaNonzero(first.prior_record_sha256)) {
    if (detail != nullptr) *detail = "savepoint_binary_chain_root_invalid";
    return false;
  }
  if (records->size() == 2) {
    const auto& terminal = records->back();
    if (terminal.journal_sequence != 2 ||
        terminal.authority.lifecycle ==
            MgaDmlUpdateStatementSavepointLifecycleV1::active ||
        terminal.authority.binding != first.authority.binding ||
        terminal.authority.publication_barrier_uuid !=
            first.authority.publication_barrier_uuid ||
        terminal.authority.publication_barrier_generation !=
            first.authority.publication_barrier_generation ||
        terminal.prior_record_sha256 !=
            first.authority.durable_presence_sha256) {
      if (detail != nullptr) *detail = "savepoint_binary_chain_forked";
      return false;
    }
  }
  return true;
}

bool DmlUpdateStatementAppendBinaryRecord(
    const EngineRequestContext& context,
    const DmlUpdateStatementSavepointJournalRecordV1& record) {
  const std::string directory =
      DmlUpdateStatementSavepointBinaryStorePath(context);
  if (!DmlUpdateDurableEnsureDirectory(directory)) return false;
  const std::string path =
      DmlUpdateDurableSavepointPath(context,
                                    record.authority.savepoint_uuid);
  DmlUpdateDurableFileLock lock(path);
  if (!lock.ok()) return false;
  const auto loaded = DmlUpdateDurableLoadFrames(path);
  if (!loaded.ok) return false;
  if ((record.journal_sequence == 1 && !loaded.frames.empty()) ||
      (record.journal_sequence == 2 && loaded.frames.size() != 1)) {
    return false;
  }
  DmlUpdateDurableFrameV1 frame;
  frame.kind = DmlUpdateDurableFrameKindV1::statement_savepoint;
  frame.identity =
      DmlUpdateStatementDurableIdentity(record.authority.binding);
  frame.sequence = record.journal_sequence;
  frame.state = static_cast<std::uint8_t>(record.authority.lifecycle);
  frame.prior_record_sha256 = record.prior_record_sha256;
  frame.record_evidence_sha256 =
      record.authority.durable_presence_sha256;
  if (!DmlUpdateStatementEncodeBinaryPayload(record, &frame.payload)) {
    return false;
  }
  return DmlUpdateDurableAppendFrame(path, frame);
}

bool ApplyDmlUpdateBinarySavepointRecords(
    const EngineRequestContext& context, SavepointParsedState* state,
    std::string* refusal_detail) {
  if (state == nullptr) {
    if (refusal_detail != nullptr) *refusal_detail = "savepoint_state_required";
    return false;
  }
  const std::string directory =
      DmlUpdateStatementSavepointBinaryStorePath(context);
  std::error_code error;
  if (!std::filesystem::exists(directory, error)) return !error;
  if (error || !std::filesystem::is_directory(directory, error) || error) {
    if (refusal_detail != nullptr) {
      *refusal_detail = "update_savepoint_store_directory_invalid";
    }
    return false;
  }
  std::vector<std::filesystem::path> paths;
  for (std::filesystem::directory_iterator iterator(directory, error), end;
       !error && iterator != end; iterator.increment(error)) {
    if (!iterator->is_regular_file(error) || error) break;
    const auto path = iterator->path();
    if (path.extension() == ".dups") paths.push_back(path);
  }
  if (error) {
    if (refusal_detail != nullptr) {
      *refusal_detail = "update_savepoint_store_enumeration_failed";
    }
    return false;
  }
  std::ranges::sort(paths);
  constexpr std::size_t kMaximumDurableSavepointFiles = 1048576;
  if (paths.size() > kMaximumDurableSavepointFiles) {
    if (refusal_detail != nullptr) {
      *refusal_detail = "update_savepoint_store_count_exceeded";
    }
    return false;
  }
  for (const auto& path : paths) {
    const std::string filename = path.stem().string();
    std::vector<DmlUpdateStatementSavepointJournalRecordV1> records;
    std::string detail;
    if (!DmlUpdateStatementLoadBinaryChain(context, filename, &records,
                                            &detail)) {
      if (refusal_detail != nullptr) {
        *refusal_detail = detail.empty()
                              ? "update_savepoint_store_record_invalid"
                              : detail;
      }
      return false;
    }
    const auto& first = records.front();
    const auto tx_id = first.authority.binding.owning_local_transaction_id;
    const auto marker = first.private_marker;
    const auto preexisting_tx = state->active_savepoints.find(tx_id);
    if (preexisting_tx != state->active_savepoints.end() &&
        preexisting_tx->second.find(marker) !=
            preexisting_tx->second.end()) {
      if (refusal_detail != nullptr) {
        *refusal_detail = "update_savepoint_text_binary_contradiction";
      }
      return false;
    }
    state->active_savepoints[tx_id][marker] = first.cutoffs;
    if (records.size() == 1) continue;
    const auto& terminal = records.back();
    if (terminal.authority.lifecycle ==
        MgaDmlUpdateStatementSavepointLifecycleV1::rolled_back) {
      SavepointRollbackRange range;
      range.cutoffs = terminal.cutoffs;
      range.row_upper_event_sequence = terminal.row_upper_event_sequence;
      range.metadata_upper_event_sequence =
          terminal.metadata_upper_event_sequence;
      range.index_upper_event_sequence = terminal.index_upper_event_sequence;
      state->rollback_ranges[tx_id].push_back(range);
    }
    state->active_savepoints[tx_id].erase(marker);
  }
  return true;
}

bool DmlUpdateStatementBindingMatchesContext(
    const EngineRequestContext& context,
    const MgaDmlUpdateStatementSavepointBindingV1& binding) {
  return !context.database_path.empty() &&
         DmlUpdateStatementParseUuid(binding.database_uuid) &&
         binding.database_uuid == context.database_uuid.canonical &&
         DmlUpdateStatementParseUuid(binding.owning_transaction_uuid) &&
         binding.owning_transaction_uuid == context.transaction_uuid.canonical &&
         binding.owning_local_transaction_id != 0 &&
         binding.owning_local_transaction_id == context.local_transaction_id &&
         DmlUpdateStatementParseUuid(
             binding.authenticated_statement_receipt_uuid) &&
         binding.authenticated_statement_receipt_uuid ==
             context.statement_receipt_uuid.canonical &&
         DmlUpdateStatementParseUuid(binding.operation_uuid) &&
         DmlUpdateStatementParseUuid(binding.descriptor_uuid) &&
         binding.descriptor_generation != 0 &&
         DmlUpdateStatementParseUuid(binding.recovery_token_uuid) &&
         binding.recovery_generation != 0;
}

bool DmlUpdateStatementRecordKind(
    std::string_view kind,
    MgaDmlUpdateStatementSavepointLifecycleV1* lifecycle) {
  if (lifecycle == nullptr) return false;
  if (kind == kDmlUpdateStatementSavepointCreateKind) {
    *lifecycle = MgaDmlUpdateStatementSavepointLifecycleV1::active;
    return true;
  }
  if (kind == kDmlUpdateStatementSavepointRollbackKind) {
    *lifecycle = MgaDmlUpdateStatementSavepointLifecycleV1::rolled_back;
    return true;
  }
  if (kind == kDmlUpdateStatementSavepointReleaseKind) {
    *lifecycle = MgaDmlUpdateStatementSavepointLifecycleV1::released;
    return true;
  }
  return false;
}

bool DmlUpdateStatementParseJournalRecord(
    const std::vector<std::string>& fields,
    DmlUpdateStatementSavepointJournalRecordV1* record) {
  if (record == nullptr ||
      fields.size() != kDmlUpdateStatementSavepointJournalFields ||
      fields[0] != kRowStoreMagic ||
      !DmlUpdateStatementRecordKind(fields[1],
                                    &record->authority.lifecycle) ||
      !DmlUpdateStatementParseU64(
          fields[2], &record->authority.binding.owning_local_transaction_id) ||
      record->authority.binding.owning_local_transaction_id == 0 ||
      !DmlUpdateStatementParseU64(fields[4],
                                  &record->cutoffs.row_event_sequence) ||
      !DmlUpdateStatementParseU64(fields[5],
                                  &record->cutoffs.metadata_event_sequence) ||
      !DmlUpdateStatementParseU64(fields[6],
                                  &record->cutoffs.index_event_sequence) ||
      !DmlUpdateStatementParseU64(fields[7],
                                  &record->row_upper_event_sequence) ||
      !DmlUpdateStatementParseU64(fields[8],
                                  &record->metadata_upper_event_sequence) ||
      !DmlUpdateStatementParseU64(fields[9],
                                  &record->index_upper_event_sequence)) {
    return false;
  }
  std::uint64_t format_version = 0;
  if (!DmlUpdateStatementParseU64(fields[10], &format_version) ||
      format_version != 1 ||
      !DmlUpdateStatementParseU64(fields[11], &record->journal_sequence)) {
    return false;
  }
  auto& binding = record->authority.binding;
  binding.database_uuid = fields[12];
  binding.owning_transaction_uuid = fields[13];
  binding.authenticated_statement_receipt_uuid = fields[14];
  binding.operation_uuid = fields[15];
  binding.descriptor_uuid = fields[16];
  if (!DmlUpdateStatementParseU64(fields[17],
                                  &binding.descriptor_generation)) {
    return false;
  }
  binding.recovery_token_uuid = fields[18];
  if (!DmlUpdateStatementParseU64(fields[19],
                                  &binding.recovery_generation)) {
    return false;
  }
  record->authority.savepoint_uuid = fields[20];
  if (!DmlUpdateStatementParseU64(
          fields[21], &record->authority.savepoint_generation)) {
    return false;
  }
  record->authority.publication_barrier_uuid = fields[22];
  if (!DmlUpdateStatementParseU64(
          fields[23], &record->authority.publication_barrier_generation)) {
    return false;
  }
  std::uint64_t barrier_present = 0;
  if (!DmlUpdateStatementParseU64(fields[24], &barrier_present) ||
      barrier_present > 1 ||
      !DmlUpdateStatementParseSha(fields[25],
                                  &record->prior_record_sha256) ||
      !DmlUpdateStatementParseSha(
          fields[26], &record->authority.durable_presence_sha256)) {
    return false;
  }
  record->authority.publication_barrier_present = barrier_present == 1;
  record->private_marker = DecodeCrudTextLocal(fields[3]);
  const bool terminal =
      record->authority.lifecycle !=
      MgaDmlUpdateStatementSavepointLifecycleV1::active;
  if (!DmlUpdateStatementParseUuid(binding.database_uuid) ||
      !DmlUpdateStatementParseUuid(binding.owning_transaction_uuid) ||
      !DmlUpdateStatementParseUuid(
          binding.authenticated_statement_receipt_uuid) ||
      !DmlUpdateStatementParseUuid(binding.operation_uuid) ||
      !DmlUpdateStatementParseUuid(binding.descriptor_uuid) ||
      binding.descriptor_generation == 0 ||
      !DmlUpdateStatementParseUuid(binding.recovery_token_uuid) ||
      binding.recovery_generation == 0 ||
      !DmlUpdateStatementParseUuid(record->authority.savepoint_uuid) ||
      record->authority.savepoint_generation != 1 ||
      record->private_marker != DmlUpdateStatementPrivateSavepointMarker(
                                    record->authority.savepoint_uuid) ||
      record->journal_sequence != (terminal ? 2 : 1)) {
    return false;
  }
  if (!DmlUpdateStatementParseUuid(
          record->authority.publication_barrier_uuid) ||
      record->authority.publication_barrier_generation != 1 ||
      record->authority.publication_barrier_uuid ==
          record->authority.savepoint_uuid ||
      record->authority.publication_barrier_uuid == binding.database_uuid ||
      record->authority.publication_barrier_uuid ==
          binding.owning_transaction_uuid ||
      record->authority.publication_barrier_uuid ==
          binding.authenticated_statement_receipt_uuid ||
      record->authority.publication_barrier_uuid == binding.operation_uuid ||
      record->authority.publication_barrier_uuid == binding.descriptor_uuid ||
      record->authority.publication_barrier_uuid ==
          binding.recovery_token_uuid ||
      record->authority.publication_barrier_present !=
          (record->authority.lifecycle ==
           MgaDmlUpdateStatementSavepointLifecycleV1::released)) {
    return false;
  }
  if (record->authority.lifecycle ==
          MgaDmlUpdateStatementSavepointLifecycleV1::active &&
      (record->row_upper_event_sequence != 0 ||
       record->metadata_upper_event_sequence != 0 ||
       record->index_upper_event_sequence != 0 ||
       DmlUpdateStatementShaNonzero(record->prior_record_sha256))) {
    return false;
  }
  if (record->authority.lifecycle ==
          MgaDmlUpdateStatementSavepointLifecycleV1::released &&
      (record->row_upper_event_sequence != 0 ||
       record->metadata_upper_event_sequence != 0 ||
       record->index_upper_event_sequence != 0)) {
    return false;
  }
  if (record->authority.lifecycle ==
          MgaDmlUpdateStatementSavepointLifecycleV1::rolled_back &&
      (record->row_upper_event_sequence <
           record->cutoffs.row_event_sequence ||
       record->metadata_upper_event_sequence <
           record->cutoffs.metadata_event_sequence ||
       record->index_upper_event_sequence <
           record->cutoffs.index_event_sequence)) {
    return false;
  }
  const auto expected = DmlUpdateStatementSavepointRecordSha256(*record);
  return DmlUpdateStatementShaNonzero(expected) &&
         expected == record->authority.durable_presence_sha256;
}

std::string DmlUpdateStatementEncodeJournalRecord(
    const DmlUpdateStatementSavepointJournalRecordV1& record) {
  std::string kind;
  switch (record.authority.lifecycle) {
    case MgaDmlUpdateStatementSavepointLifecycleV1::active:
      kind = std::string(kDmlUpdateStatementSavepointCreateKind);
      break;
    case MgaDmlUpdateStatementSavepointLifecycleV1::rolled_back:
      kind = std::string(kDmlUpdateStatementSavepointRollbackKind);
      break;
    case MgaDmlUpdateStatementSavepointLifecycleV1::released:
      kind = std::string(kDmlUpdateStatementSavepointReleaseKind);
      break;
  }
  const auto& authority = record.authority;
  const auto& binding = authority.binding;
  return JoinLine(
      {kRowStoreMagic,
       kind,
       std::to_string(binding.owning_local_transaction_id),
       EncodeCrudText(record.private_marker),
       std::to_string(record.cutoffs.row_event_sequence),
       std::to_string(record.cutoffs.metadata_event_sequence),
       std::to_string(record.cutoffs.index_event_sequence),
       std::to_string(record.row_upper_event_sequence),
       std::to_string(record.metadata_upper_event_sequence),
       std::to_string(record.index_upper_event_sequence),
       "1",
       std::to_string(record.journal_sequence),
       binding.database_uuid,
       binding.owning_transaction_uuid,
       binding.authenticated_statement_receipt_uuid,
       binding.operation_uuid,
       binding.descriptor_uuid,
       std::to_string(binding.descriptor_generation),
       binding.recovery_token_uuid,
       std::to_string(binding.recovery_generation),
       authority.savepoint_uuid,
       std::to_string(authority.savepoint_generation),
       authority.publication_barrier_uuid,
       std::to_string(authority.publication_barrier_generation),
       authority.publication_barrier_present ? "1" : "0",
       DmlUpdateStatementShaHex(record.prior_record_sha256),
       DmlUpdateStatementShaHex(authority.durable_presence_sha256)});
}

MgaDmlUpdateStatementSavepointAuthorityResultV1
DmlUpdateStatementLoadSavepointAuthority(
    const EngineRequestContext& context,
    const MgaDmlUpdateStatementSavepointBindingV1& binding,
    const std::string& savepoint_uuid, std::uint64_t savepoint_generation) {
  if (!DmlUpdateStatementBindingMatchesContext(context, binding) ||
      !DmlUpdateStatementParseUuid(savepoint_uuid) ||
      savepoint_generation != 1) {
    return DmlUpdateStatementSavepointFailure(
        "MGA.TRANSACTION.STALE",
        "sblr.dml_update_rows.statement_savepoint_stale",
        "savepoint_binding_or_generation_mismatch");
  }

  std::vector<DmlUpdateStatementSavepointJournalRecordV1> chain;
  std::string binary_detail;
  if (!DmlUpdateStatementLoadBinaryChain(
          context, savepoint_uuid, &chain, &binary_detail)) {
    return DmlUpdateStatementSavepointFailure(
        binary_detail == "savepoint_identity_unknown"
            ? "MGA.TRANSACTION.STALE"
            : "DML.UPDATE_FAILED",
        binary_detail == "savepoint_identity_unknown"
            ? "sblr.dml_update_rows.statement_savepoint_stale"
            : "sblr.dml_update_rows.statement_savepoint_corrupt",
        binary_detail);
  }
  if (chain.empty() || chain.size() > 2 ||
      chain.front().authority.lifecycle !=
          MgaDmlUpdateStatementSavepointLifecycleV1::active ||
      chain.front().journal_sequence != 1 ||
      DmlUpdateStatementShaNonzero(chain.front().prior_record_sha256)) {
    return DmlUpdateStatementSavepointFailure(
        "DML.UPDATE_FAILED",
        "sblr.dml_update_rows.statement_savepoint_corrupt",
        "savepoint_journal_chain_invalid");
  }
  if (chain.front().authority.binding != binding) {
    return DmlUpdateStatementSavepointFailure(
        "MGA.TRANSACTION.STALE",
        "sblr.dml_update_rows.statement_savepoint_stale",
        "savepoint_cross_authority_replay");
  }
  if (chain.size() == 2 &&
      (chain.back().authority.lifecycle ==
           MgaDmlUpdateStatementSavepointLifecycleV1::active ||
       chain.back().authority.binding != chain.front().authority.binding ||
       chain.back().cutoffs.row_event_sequence !=
           chain.front().cutoffs.row_event_sequence ||
       chain.back().cutoffs.metadata_event_sequence !=
           chain.front().cutoffs.metadata_event_sequence ||
       chain.back().cutoffs.index_event_sequence !=
           chain.front().cutoffs.index_event_sequence ||
       chain.back().authority.publication_barrier_uuid !=
           chain.front().authority.publication_barrier_uuid ||
       chain.back().authority.publication_barrier_generation !=
           chain.front().authority.publication_barrier_generation ||
       chain.back().prior_record_sha256 !=
           chain.front().authority.durable_presence_sha256)) {
    return DmlUpdateStatementSavepointFailure(
        "DML.UPDATE_FAILED",
        "sblr.dml_update_rows.statement_savepoint_corrupt",
        "savepoint_terminal_chain_invalid");
  }

  const auto& latest = chain.back();
  const auto parsed_savepoints = ParseSavepoints(context);
  if (parsed_savepoints.update_statement_authority_corrupt) {
    return DmlUpdateStatementSavepointFailure(
        "DML.UPDATE_FAILED",
        "sblr.dml_update_rows.statement_savepoint_corrupt",
        "text_binary_savepoint_authority_contradiction");
  }
  bool marker_active = false;
  const auto tx = parsed_savepoints.active_savepoints.find(
      binding.owning_local_transaction_id);
  if (tx != parsed_savepoints.active_savepoints.end()) {
    marker_active =
        tx->second.find(latest.private_marker) != tx->second.end();
  }
  const bool expected_active =
      latest.authority.lifecycle ==
      MgaDmlUpdateStatementSavepointLifecycleV1::active;
  if (marker_active != expected_active) {
    return DmlUpdateStatementSavepointFailure(
        "DML.UPDATE_FAILED",
        "sblr.dml_update_rows.statement_savepoint_corrupt",
        expected_active ? "active_savepoint_presence_missing"
                        : "terminal_savepoint_and_active_marker_contradictory");
  }

  MgaDmlUpdateStatementSavepointAuthorityResultV1 result;
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  result.authority = latest.authority;
  return result;
}

bool DmlUpdateStatementAuthorityExact(
    const MgaDmlUpdateStatementSavepointAuthorityV1& left,
    const MgaDmlUpdateStatementSavepointAuthorityV1& right) {
  return left == right;
}

std::string DmlUpdateStatementFreshDistinctUuid(
    const MgaDmlUpdateStatementSavepointBindingV1& binding,
    std::string_view other = {}) {
  for (std::size_t attempt = 0; attempt < 8; ++attempt) {
    const std::string candidate = GenerateCrudEngineUuid("object");
    if (DmlUpdateStatementParseUuid(candidate) &&
        candidate != binding.database_uuid &&
        candidate != binding.owning_transaction_uuid &&
        candidate != binding.authenticated_statement_receipt_uuid &&
        candidate != binding.operation_uuid && candidate != binding.descriptor_uuid &&
        candidate != binding.recovery_token_uuid && candidate != other) {
      return candidate;
    }
  }
  return {};
}

MgaDmlUpdateStatementSavepointBindingV1
DmlUpdateDurableStatementBinding(
    const MgaDmlUpdateDurableOperationIdentityV1& identity) {
  MgaDmlUpdateStatementSavepointBindingV1 binding;
  binding.database_uuid = identity.database_uuid;
  binding.owning_transaction_uuid = identity.owning_transaction_uuid;
  binding.owning_local_transaction_id = identity.owning_local_transaction_id;
  binding.authenticated_statement_receipt_uuid =
      identity.authenticated_statement_receipt_uuid;
  binding.operation_uuid = identity.operation_uuid;
  binding.descriptor_uuid = identity.descriptor_uuid;
  binding.descriptor_generation = identity.descriptor_generation;
  binding.recovery_token_uuid = identity.recovery_token_uuid;
  binding.recovery_generation = identity.recovery_generation;
  return binding;
}

enum class DmlUpdateDurableSavepointLookupStateV1 : std::uint8_t {
  absent = 0,
  present = 1,
  corrupt = 2,
};

struct DmlUpdateDurableSavepointLookupV1 {
  DmlUpdateDurableSavepointLookupStateV1 state =
      DmlUpdateDurableSavepointLookupStateV1::absent;
  MgaDmlUpdateStatementSavepointAuthorityV1 authority;
  std::string detail;
};

DmlUpdateDurableSavepointLookupV1 DmlUpdateDurableFindStatementSavepoint(
    const EngineRequestContext& context,
    const MgaDmlUpdateDurableOperationIdentityV1& identity,
    std::string_view required_savepoint_uuid = {}) {
  DmlUpdateDurableSavepointLookupV1 result;
  const auto expected_binding = DmlUpdateDurableStatementBinding(identity);
  const std::string directory =
      DmlUpdateStatementSavepointBinaryStorePath(context);
  std::error_code error;
  if (!std::filesystem::exists(directory, error)) {
    if (error) {
      result.state = DmlUpdateDurableSavepointLookupStateV1::corrupt;
      result.detail = "savepoint_store_presence_failed";
    }
    return result;
  }
  if (!std::filesystem::is_directory(directory, error) || error) {
    result.state = DmlUpdateDurableSavepointLookupStateV1::corrupt;
    result.detail = "savepoint_store_directory_invalid";
    return result;
  }

  std::vector<std::filesystem::path> paths;
  if (!required_savepoint_uuid.empty()) {
    if (!DmlUpdateStatementParseUuid(required_savepoint_uuid)) {
      result.state = DmlUpdateDurableSavepointLookupStateV1::corrupt;
      result.detail = "required_savepoint_identity_invalid";
      return result;
    }
    paths.emplace_back(DmlUpdateDurableSavepointPath(
        context, required_savepoint_uuid));
  } else {
    for (std::filesystem::directory_iterator iterator(directory, error), end;
         !error && iterator != end; iterator.increment(error)) {
      if (!iterator->is_regular_file(error) || error) break;
      if (iterator->path().extension() == ".dups") {
        paths.push_back(iterator->path());
      }
    }
    if (error || paths.size() > 1048576) {
      result.state = DmlUpdateDurableSavepointLookupStateV1::corrupt;
      result.detail = error ? "savepoint_store_enumeration_failed"
                            : "savepoint_store_count_exceeded";
      return result;
    }
    std::ranges::sort(paths);
  }

  for (const auto& path : paths) {
    if (!std::filesystem::exists(path, error)) {
      if (error) {
        result.state = DmlUpdateDurableSavepointLookupStateV1::corrupt;
        result.detail = "savepoint_store_presence_failed";
      }
      continue;
    }
    const std::string uuid = path.stem().string();
    std::vector<DmlUpdateStatementSavepointJournalRecordV1> chain;
    std::string detail;
    if (!DmlUpdateStatementLoadBinaryChain(context, uuid, &chain, &detail)) {
      result.state = DmlUpdateDurableSavepointLookupStateV1::corrupt;
      result.detail = detail.empty() ? "savepoint_chain_invalid" : detail;
      return result;
    }
    if (chain.empty() || chain.front().authority.binding != expected_binding) {
      continue;
    }
    if (result.state == DmlUpdateDurableSavepointLookupStateV1::present) {
      result.state = DmlUpdateDurableSavepointLookupStateV1::corrupt;
      result.detail = "multiple_statement_savepoints_for_operation";
      return result;
    }
    result.state = DmlUpdateDurableSavepointLookupStateV1::present;
    result.authority = chain.back().authority;
  }
  if (!required_savepoint_uuid.empty() &&
      result.state == DmlUpdateDurableSavepointLookupStateV1::absent) {
    result.detail = "required_savepoint_identity_unknown";
  }
  return result;
}

bool DmlUpdateDurableDecodeTypedJournalChain(
    const EngineRequestContext& context,
    const MgaDmlUpdateDurableOperationIdentityV1& identity,
    const MgaDmlUpdateDurableAuthoritySnapshotV1& snapshot,
    std::span<const MgaDmlUpdateDurableJournalExtentV1> extents,
    std::vector<scratchbird::wire::TypedUpdateJournalRecord>* records,
    std::string* detail) {
  const auto fail = [&](std::string reason) {
    if (detail != nullptr) *detail = std::move(reason);
    return false;
  };
  if (records == nullptr || extents.empty() ||
      snapshot.descriptor_dudc.size() !=
          scratchbird::wire::kTypedUpdateDescriptorBytes) {
    return fail("durable_journal_chain_required");
  }
  records->clear();
  records->reserve(extents.size());
  for (std::size_t index = 0; index < extents.size(); ++index) {
    scratchbird::wire::TypedUpdateJournalChainContext chain;
    chain.first_record = index == 0;
    if (!chain.first_record) {
      const auto& prior = records->back();
      chain.prior_sequence = prior.journal_sequence;
      chain.prior_state = prior.lifecycle_state;
      chain.prior_record_evidence_sha256 = prior.record_evidence_sha256;
      chain.prior_savepoint_uuid = prior.statement_savepoint_uuid;
      chain.prior_savepoint_generation = prior.statement_savepoint_generation;
      chain.require_same_descriptor = true;
      std::copy(snapshot.descriptor_dudc.begin(),
                snapshot.descriptor_dudc.end(),
                chain.expected_descriptor_bytes.begin());
      if (prior.lifecycle_state ==
              scratchbird::wire::TypedUpdateJournalState::prepared) {
        if (!prior.embedded_result_bytes.has_value() ||
            prior.embedded_result_bytes->size() !=
                scratchbird::wire::kTypedUpdateResultBytes) {
          return fail("prepared_journal_result_missing");
        }
        std::array<scratchbird::core::platform::byte,
                   scratchbird::wire::kTypedUpdateResultBytes> exact{};
        std::copy(prior.embedded_result_bytes->begin(),
                  prior.embedded_result_bytes->end(), exact.begin());
        chain.expected_prepared_result_bytes = exact;
      }
      // A crash may leave a provider-owned savepoint after bound but before
      // intent.  The bound-to-aborted codec edge can trust a nonnil savepoint
      // only after the MGA savepoint store authenticates that exact row.
      if (prior.lifecycle_state ==
              scratchbird::wire::TypedUpdateJournalState::bound &&
          extents[index].lifecycle_state ==
              MgaDmlUpdateDurableJournalStateV1::aborted &&
          extents[index].exact_dujr_bytes.size() >= 176 &&
          !DmlUpdateDurableZero(std::span<const std::uint8_t>(
              extents[index].exact_dujr_bytes).subspan(152, 16))) {
        const std::string savepoint_uuid = DmlUpdateDurableUuidText(
            std::span<const std::uint8_t>(extents[index].exact_dujr_bytes)
                .subspan(152, 16));
        std::uint64_t savepoint_generation = 0;
        const auto authority = DmlUpdateDurableFindStatementSavepoint(
            context, identity, savepoint_uuid);
        if (!DmlUpdateDurableReadU64(extents[index].exact_dujr_bytes, 168,
                                     &savepoint_generation) ||
            authority.state !=
                DmlUpdateDurableSavepointLookupStateV1::present ||
            authority.authority.savepoint_uuid != savepoint_uuid ||
            authority.authority.savepoint_generation != savepoint_generation ||
            authority.authority.lifecycle !=
                MgaDmlUpdateStatementSavepointLifecycleV1::rolled_back ||
            authority.authority.publication_barrier_uuid !=
                identity.reserved_statement_barrier_uuid ||
            authority.authority.publication_barrier_generation !=
                identity.reserved_statement_barrier_generation ||
            !DmlUpdateDurableTypedUuid(savepoint_uuid,
                                       &chain.prior_savepoint_uuid)) {
          return fail("bound_aborted_savepoint_authority_invalid");
        }
        chain.prior_savepoint_generation = savepoint_generation;
      }
    }
    scratchbird::wire::TypedUpdateJournalRecord decoded;
    scratchbird::wire::TypedUpdateCarrierError error;
    if (!scratchbird::wire::DecodeAndValidateTypedUpdateJournalRecord(
            extents[index].exact_dujr_bytes, chain, &decoded, &error)) {
      return fail("durable_journal_canonical_invalid:" + error.field + ":" +
                  error.detail);
    }
    records->push_back(std::move(decoded));
  }
  return true;
}

bool DmlUpdateDurableTransactionState(
    const EngineRequestContext& context,
    const MgaDmlUpdateDurableOperationIdentityV1& identity,
    scratchbird::wire::TypedUpdateTransactionState* state,
    std::string* detail) {
  if (state == nullptr) return false;
  const auto inventory =
      LoadLocalTransactionInventoryFromDatabase(context.database_path);
  if (!inventory.ok()) {
    if (detail != nullptr) *detail = "transaction_inventory_load_failed";
    return false;
  }
  const auto lookup = LookupLocalTransaction(
      inventory.inventory,
      MakeLocalTransactionId(identity.owning_local_transaction_id));
  if (!lookup.ok() ||
      scratchbird::core::uuid::UuidToString(
          lookup.entry.identity.transaction_uuid.value) !=
          identity.owning_transaction_uuid) {
    if (detail != nullptr) *detail = "transaction_inventory_identity_missing";
    return false;
  }
  switch (lookup.entry.state) {
    case TransactionState::active:
    case TransactionState::read_only_active:
      *state = scratchbird::wire::TypedUpdateTransactionState::active_live;
      return true;
    case TransactionState::committed:
    case TransactionState::archived:
      *state = scratchbird::wire::TypedUpdateTransactionState::committed_final;
      return true;
    case TransactionState::rolled_back:
      *state =
          scratchbird::wire::TypedUpdateTransactionState::rolled_back_final;
      return true;
    case TransactionState::none:
    case TransactionState::created:
    case TransactionState::preparing:
    case TransactionState::prepared:
    case TransactionState::committing:
    case TransactionState::rolling_back:
    case TransactionState::limbo:
    case TransactionState::recovering:
    case TransactionState::failed_terminal:
      *state =
          scratchbird::wire::TypedUpdateTransactionState::dead_or_abandoned;
      return true;
  }
  if (detail != nullptr) *detail = "transaction_inventory_state_invalid";
  return false;
}

}  // namespace

struct MgaDmlUpdateDurablePreparedSuccessorV1::Impl {
  std::string path;
  std::unique_ptr<DmlUpdateDurableFileLock> lock;
  std::vector<std::uint8_t> encoded_frame;
  std::vector<std::uint8_t> encoded_invalidation_frame;
  EngineApiDiagnostic committed_diagnostic = OkDiagnostic();
  EngineApiDiagnostic cancelled_diagnostic = OkDiagnostic();
  EngineApiDiagnostic committed_ack_lost_diagnostic =
      DmlUpdateDurableMutation(
          MgaDmlUpdateDurableOperationOutcomeV1::storage_failure,
          "successor_committed_ack_lost").diagnostic;
  EngineApiDiagnostic commit_write_failed_diagnostic =
      DmlUpdateDurableMutation(
          MgaDmlUpdateDurableOperationOutcomeV1::storage_failure,
          "successor_durable_write_failed").diagnostic;
  bool valid = false;
};

MgaDmlUpdateDurablePreparedSuccessorV1::
    MgaDmlUpdateDurablePreparedSuccessorV1() = default;
MgaDmlUpdateDurablePreparedSuccessorV1::
    ~MgaDmlUpdateDurablePreparedSuccessorV1() = default;
MgaDmlUpdateDurablePreparedSuccessorV1::
    MgaDmlUpdateDurablePreparedSuccessorV1(
        MgaDmlUpdateDurablePreparedSuccessorV1&&) noexcept = default;
MgaDmlUpdateDurablePreparedSuccessorV1&
MgaDmlUpdateDurablePreparedSuccessorV1::operator=(
    MgaDmlUpdateDurablePreparedSuccessorV1&&) noexcept = default;
bool MgaDmlUpdateDurablePreparedSuccessorV1::valid() const {
  return impl_ != nullptr && impl_->valid && impl_->lock != nullptr &&
         impl_->lock->ok() && !impl_->encoded_frame.empty() &&
         !impl_->encoded_invalidation_frame.empty();
}

MgaDmlUpdateValidatedDurableAuthorityHandleV1::
    MgaDmlUpdateValidatedDurableAuthorityHandleV1() = default;
MgaDmlUpdateValidatedDurableAuthorityHandleV1::
    ~MgaDmlUpdateValidatedDurableAuthorityHandleV1() = default;
MgaDmlUpdateValidatedDurableAuthorityHandleV1::
    MgaDmlUpdateValidatedDurableAuthorityHandleV1(
        MgaDmlUpdateValidatedDurableAuthorityHandleV1&&) noexcept = default;
MgaDmlUpdateValidatedDurableAuthorityHandleV1&
MgaDmlUpdateValidatedDurableAuthorityHandleV1::operator=(
    MgaDmlUpdateValidatedDurableAuthorityHandleV1&&) noexcept = default;
bool MgaDmlUpdateValidatedDurableAuthorityHandleV1::valid() const {
  return impl_ != nullptr && !impl_->identity.validated_durable_handle_uuid.empty() &&
         impl_->identity.validated_durable_handle_generation != 0 &&
         !impl_->journal.empty() && impl_->exact_dumo.size() == 416;
}

MgaDmlUpdateDurableAuthorityReservationResultV1
ReserveMgaDmlUpdateDurableOperationAuthorityV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateDurableAuthorityReservationRequestV1& request) {
  MgaDmlUpdateDurableAuthorityReservationResultV1 result;
  MgaDmlUpdateDurableOperationIdentityV1 identity;
  identity.database_uuid = context.database_uuid.canonical;
  identity.owning_transaction_uuid = context.transaction_uuid.canonical;
  identity.owning_local_transaction_id = context.local_transaction_id;
  identity.authenticated_statement_receipt_uuid =
      context.statement_receipt_uuid.canonical;
  identity.operation_uuid = request.operation_uuid;
  identity.operation_generation = request.operation_generation;
  identity.descriptor_uuid = request.descriptor_uuid;
  identity.descriptor_generation = request.descriptor_generation;
  identity.recovery_token_uuid = request.recovery_token_uuid;
  identity.recovery_generation = request.recovery_generation;
  if (context.database_path.empty() ||
      !DmlUpdateDurableBaseIdentityValid(identity) ||
      identity.operation_generation == 0) {
    result.outcome = MgaDmlUpdateDurableOperationOutcomeV1::conflict;
    result.diagnostic = DmlUpdateDurableDiagnostic(
        "SBLR.OPERAND_INVALID",
        "sblr.dml_update_rows.durable_reservation_invalid");
    return result;
  }
  const std::string directory = DmlUpdateDurableOperationStorePath(context);
  if (!DmlUpdateDurableEnsureDirectory(directory)) {
    result.outcome = MgaDmlUpdateDurableOperationOutcomeV1::storage_failure;
    result.diagnostic = DmlUpdateDurableDiagnostic(
        "DML.UPDATE_FAILED",
        "sblr.dml_update_rows.durable_reservation_storage_failed");
    return result;
  }
  const std::string path = directory + "/" + identity.descriptor_uuid + ".duop";
  DmlUpdateDurableFileLock lock(path);
  if (!lock.ok()) {
    result.outcome = MgaDmlUpdateDurableOperationOutcomeV1::storage_failure;
    result.diagnostic = DmlUpdateDurableDiagnostic(
        "DML.UPDATE_FAILED",
        "sblr.dml_update_rows.durable_reservation_lock_failed");
    return result;
  }
  auto current = DmlUpdateDurableLoadOperation(path, true);
  if (current.ok) {
    if (!DmlUpdateDurableSameReservationRequest(context, request,
                                                 current.identity)) {
      result.outcome = MgaDmlUpdateDurableOperationOutcomeV1::access_denied;
      result.diagnostic = DmlUpdateDurableDiagnostic(
          "SECURITY.ACCESS_DENIED",
          "sblr.dml_update_rows.durable_reservation_denied");
      return result;
    }
    result.outcome = MgaDmlUpdateDurableOperationOutcomeV1::already_exact;
    result.diagnostic = OkDiagnostic();
    result.identity = current.identity;
    return result;
  }
  if (!current.missing) {
    result.outcome = current.quarantined
                         ? MgaDmlUpdateDurableOperationOutcomeV1::quarantined
                         : MgaDmlUpdateDurableOperationOutcomeV1::storage_failure;
    result.diagnostic = DmlUpdateDurableDiagnostic(
        "DML.UPDATE_FAILED",
        "sblr.dml_update_rows.durable_reservation_corrupt",
        current.detail);
    return result;
  }
  identity.validated_durable_handle_uuid =
      DmlUpdateDurableFreshIdentity(identity);
  identity.validated_durable_handle_generation = 1;
  identity.reserved_statement_barrier_uuid =
      DmlUpdateDurableFreshIdentity(
          identity, identity.validated_durable_handle_uuid);
  identity.reserved_statement_barrier_generation = 1;
  if (!DmlUpdateDurableIdentityValid(identity)) {
    result.outcome = MgaDmlUpdateDurableOperationOutcomeV1::storage_failure;
    result.diagnostic = DmlUpdateDurableDiagnostic(
        "DML.UPDATE_FAILED",
        "sblr.dml_update_rows.durable_reservation_identity_failed");
    return result;
  }
  DmlUpdateDurableFrameV1 frame;
  frame.kind = DmlUpdateDurableFrameKindV1::authority_reservation;
  frame.identity = identity;
  if (!DmlUpdateDurableAppendFrame(path, frame)) {
    result.outcome = MgaDmlUpdateDurableOperationOutcomeV1::storage_failure;
    result.diagnostic = DmlUpdateDurableDiagnostic(
        "DML.UPDATE_FAILED",
        "sblr.dml_update_rows.durable_reservation_write_failed");
    return result;
  }
  result.outcome = MgaDmlUpdateDurableOperationOutcomeV1::committed;
  result.diagnostic = OkDiagnostic();
  result.identity = std::move(identity);
  return result;
}

MgaDmlUpdateDurableOperationMutationResultV1
AbandonMgaDmlUpdateDurableOperationAuthorityReservationV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateDurableOperationIdentityV1& identity) {
  if (!DmlUpdateDurableIdentityMatchesContext(context, identity)) {
    return DmlUpdateDurableMutation(
        MgaDmlUpdateDurableOperationOutcomeV1::access_denied,
        "reservation_abandon_cross_authority");
  }
  const std::string path = DmlUpdateDurableDescriptorPath(context, identity);
  DmlUpdateDurableFileLock lock(path);
  if (!lock.ok()) {
    return DmlUpdateDurableMutation(
        MgaDmlUpdateDurableOperationOutcomeV1::storage_failure,
        "reservation_abandon_lock_failed");
  }
  const auto current = DmlUpdateDurableLoadOperation(path, true);
  if (current.missing) {
    return DmlUpdateDurableMutation(
        MgaDmlUpdateDurableOperationOutcomeV1::already_exact);
  }
  if (!current.ok || current.identity != identity) {
    return DmlUpdateDurableMutation(
        current.quarantined
            ? MgaDmlUpdateDurableOperationOutcomeV1::quarantined
            : MgaDmlUpdateDurableOperationOutcomeV1::stale,
        current.detail.empty() ? "reservation_abandon_identity_mismatch"
                               : current.detail);
  }
  if (!current.reservation_only || current.snapshot_present ||
      !current.journal.empty()) {
    return DmlUpdateDurableMutation(
        MgaDmlUpdateDurableOperationOutcomeV1::fork_or_terminal_conflict,
        "reservation_already_bound");
  }
  std::error_code error;
  const bool removed = std::filesystem::remove(path, error);
  if (error || !removed || !DmlUpdateDurableEnsureDirectory(
                              DmlUpdateDurableOperationStorePath(context))) {
    return DmlUpdateDurableMutation(
        MgaDmlUpdateDurableOperationOutcomeV1::storage_failure,
        "reservation_abandon_delete_failed");
  }
  return DmlUpdateDurableMutation(
      MgaDmlUpdateDurableOperationOutcomeV1::committed);
}

MgaDmlUpdateDurableOperationMutationResultV1
PublishMgaDmlUpdateDurableOperationBoundV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateDurablePublishBoundRequestV1& request) {
  if (!DmlUpdateDurableIdentityMatchesContext(context, request.identity)) {
    return DmlUpdateDurableMutation(
        MgaDmlUpdateDurableOperationOutcomeV1::access_denied,
        "publish_bound_cross_authority");
  }
  std::uint64_t structural_occurrence = 0;
  std::string detail;
  if (!DmlUpdateDurableSnapshotShallowValid(
          request.identity, request.authority_snapshot,
          &structural_occurrence, &detail) ||
      !DmlUpdateDurableJournalExtentMatchesBytes(
          request.identity, request.authority_snapshot,
          request.bound_journal, &detail) ||
      request.bound_journal.journal_sequence != 1 ||
      request.bound_journal.lifecycle_state !=
          MgaDmlUpdateDurableJournalStateV1::bound ||
      !DmlUpdateDurableZero(
          request.bound_journal.prior_record_sha256)) {
    return DmlUpdateDurableMutation(
        MgaDmlUpdateDurableOperationOutcomeV1::conflict,
        detail.empty() ? "publish_bound_shape_invalid" : detail);
  }
  const std::string path =
      DmlUpdateDurableDescriptorPath(context, request.identity);
  DmlUpdateDurableFileLock lock(path);
  if (!lock.ok()) {
    return DmlUpdateDurableMutation(
        MgaDmlUpdateDurableOperationOutcomeV1::storage_failure,
        "publish_bound_lock_failed");
  }
  auto current = DmlUpdateDurableLoadOperation(path, true);
  if (!current.ok || current.identity != request.identity) {
    return DmlUpdateDurableMutation(
        current.quarantined
            ? MgaDmlUpdateDurableOperationOutcomeV1::quarantined
            : current.missing
                  ? MgaDmlUpdateDurableOperationOutcomeV1::stale
                  : MgaDmlUpdateDurableOperationOutcomeV1::access_denied,
        current.detail.empty() ? "prebound_reservation_missing_or_mismatched"
                               : current.detail);
  }
  if (current.snapshot_present) {
    if (current.snapshot != request.authority_snapshot) {
      return DmlUpdateDurableMutation(
          MgaDmlUpdateDurableOperationOutcomeV1::conflict,
          "authority_snapshot_conflict");
    }
    if (!current.journal.empty()) {
      return DmlUpdateDurableMutation(
          current.journal.front() == request.bound_journal
              ? MgaDmlUpdateDurableOperationOutcomeV1::already_exact
              : MgaDmlUpdateDurableOperationOutcomeV1::conflict,
          current.journal.front() == request.bound_journal
              ? std::string{}
              : "bound_journal_conflict");
    }
  }

  if (DmlUpdateDurableFault(
          MgaDmlUpdateDurableFaultCutpointV1::before_snapshot_write)) {
    return DmlUpdateDurableMutation(
        MgaDmlUpdateDurableOperationOutcomeV1::storage_failure,
        "fault_before_snapshot_write");
  }
  DmlUpdateDurableFrameV1 reservation_frame;
  reservation_frame.kind =
      DmlUpdateDurableFrameKindV1::authority_reservation;
  reservation_frame.identity = request.identity;
  DmlUpdateDurableFrameV1 snapshot_frame;
  snapshot_frame.kind = DmlUpdateDurableFrameKindV1::authority_snapshot;
  snapshot_frame.identity = request.identity;
  std::vector<std::uint8_t> reservation_bytes;
  std::vector<std::uint8_t> snapshot_bytes;
  if (!DmlUpdateDurableEncodeSnapshot(request.authority_snapshot,
                                      &snapshot_frame.payload) ||
      !DmlUpdateDurableEncodeFrame(reservation_frame, &reservation_bytes) ||
      !DmlUpdateDurableEncodeFrame(snapshot_frame, &snapshot_bytes)) {
    return DmlUpdateDurableMutation(
        MgaDmlUpdateDurableOperationOutcomeV1::storage_failure,
        "authority_snapshot_encode_failed");
  }
  if (DmlUpdateDurableFault(
          MgaDmlUpdateDurableFaultCutpointV1::after_snapshot_write_before_bound)) {
    return DmlUpdateDurableMutation(
        MgaDmlUpdateDurableOperationOutcomeV1::storage_failure,
        "fault_after_snapshot_before_bound");
  }
  const auto bound_frame =
      DmlUpdateDurableJournalFrame(request.identity, request.bound_journal);
  std::vector<std::uint8_t> bound_bytes;
  if (!DmlUpdateDurableEncodeFrame(bound_frame, &bound_bytes)) {
    return DmlUpdateDurableMutation(
        MgaDmlUpdateDurableOperationOutcomeV1::storage_failure,
        "bound_journal_encode_failed");
  }
  std::vector<std::uint8_t> atomic_extent;
  try {
    atomic_extent.reserve(reservation_bytes.size() + snapshot_bytes.size() +
                          bound_bytes.size());
    atomic_extent.insert(atomic_extent.end(), reservation_bytes.begin(),
                         reservation_bytes.end());
    atomic_extent.insert(atomic_extent.end(), snapshot_bytes.begin(),
                         snapshot_bytes.end());
    atomic_extent.insert(atomic_extent.end(), bound_bytes.begin(),
                         bound_bytes.end());
  } catch (const std::bad_alloc&) {
    return DmlUpdateDurableMutation(
        MgaDmlUpdateDurableOperationOutcomeV1::storage_failure,
        "publish_bound_extent_allocation_failed");
  }
  if (!DmlUpdateDurableReplaceFileAtomically(path, atomic_extent)) {
    return DmlUpdateDurableMutation(
        MgaDmlUpdateDurableOperationOutcomeV1::storage_failure,
        "publish_bound_atomic_replace_failed");
  }
  return DmlUpdateDurableMutation(
      MgaDmlUpdateDurableOperationOutcomeV1::committed);
}

MgaDmlUpdateDurableOperationPrepareResultV1
PrepareMgaDmlUpdateDurableOperationSuccessorV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateDurableAppendSuccessorRequestV1& request) {
  g_dml_update_durable_prepare_calls.fetch_add(1,
                                                std::memory_order_relaxed);
  MgaDmlUpdateDurableOperationPrepareResultV1 result;
  const auto fail = [&](MgaDmlUpdateDurableOperationOutcomeV1 outcome,
                        std::string detail) {
    result.outcome = outcome;
    result.diagnostic = DmlUpdateDurableMutation(outcome,
                                                  std::move(detail)).diagnostic;
    return std::move(result);
  };
  if (!DmlUpdateDurableIdentityMatchesContext(context, request.identity)) {
    return fail(MgaDmlUpdateDurableOperationOutcomeV1::access_denied,
                "successor_cross_authority");
  }
  const std::string path =
      DmlUpdateDurableDescriptorPath(context, request.identity);
  auto lock = std::make_unique<DmlUpdateDurableFileLock>(path);
  if (!lock->ok()) {
    return fail(MgaDmlUpdateDurableOperationOutcomeV1::storage_failure,
                "successor_lock_failed");
  }
  const auto current = DmlUpdateDurableLoadOperation(path, true);
  if (!current.ok || current.identity != request.identity ||
      current.journal.empty()) {
    return fail(current.quarantined
                    ? MgaDmlUpdateDurableOperationOutcomeV1::quarantined
                    : MgaDmlUpdateDurableOperationOutcomeV1::stale,
                current.detail.empty() ? "durable_chain_unavailable"
                                       : current.detail);
  }
  std::string detail;
  if (!DmlUpdateDurableJournalExtentMatchesBytes(
          request.identity, current.snapshot, request.successor, &detail)) {
    return fail(MgaDmlUpdateDurableOperationOutcomeV1::conflict,
                std::move(detail));
  }
  if (current.staged_successor_present) {
    if (current.staged_successor == request.successor &&
        !current.staged_encoded_journal_frame.empty()) {
      std::vector<std::uint8_t> encoded_invalidation;
      if (!DmlUpdateDurableEncodePreparedInvalidation(
              request.identity, current.staged_successor,
              &encoded_invalidation)) {
        return fail(MgaDmlUpdateDurableOperationOutcomeV1::storage_failure,
                    "prepared_successor_invalidation_encode_failed");
      }
      auto impl =
          std::make_unique<MgaDmlUpdateDurablePreparedSuccessorV1::Impl>();
      impl->path = path;
      impl->lock = std::move(lock);
      impl->encoded_frame = current.staged_encoded_journal_frame;
      impl->encoded_invalidation_frame = std::move(encoded_invalidation);
      impl->valid = true;
      result.prepared.impl_ = std::move(impl);
      result.outcome = MgaDmlUpdateDurableOperationOutcomeV1::committed;
      result.diagnostic = OkDiagnostic();
      return result;
    }
    const auto& latest = current.journal.back();
    const bool cancelled_prepublication =
        current.staged_successor.lifecycle_state ==
            MgaDmlUpdateDurableJournalStateV1::published &&
        request.successor.lifecycle_state ==
            MgaDmlUpdateDurableJournalStateV1::aborted &&
        current.staged_successor.journal_sequence ==
            request.successor.journal_sequence &&
        current.staged_successor.prior_record_sha256 ==
            request.successor.prior_record_sha256 &&
        latest.journal_sequence == request.expected_prior_sequence &&
        latest.lifecycle_state == request.expected_prior_state &&
        latest.record_evidence_sha256 ==
            request.expected_prior_record_evidence_sha256;
    if (!cancelled_prepublication) {
      return fail(
          MgaDmlUpdateDurableOperationOutcomeV1::fork_or_terminal_conflict,
          "prepared_successor_conflict");
    }
    DmlUpdateDurableFrameV1 invalidation;
    invalidation.kind =
        DmlUpdateDurableFrameKindV1::prepared_successor_invalidated;
    invalidation.identity = request.identity;
    invalidation.sequence =
        current.staged_successor.journal_sequence;
    invalidation.state = static_cast<std::uint8_t>(
        current.staged_successor.lifecycle_state);
    invalidation.prior_record_sha256 =
        current.staged_successor.prior_record_sha256;
    invalidation.record_evidence_sha256 =
        current.staged_successor.record_evidence_sha256;
    if (!DmlUpdateDurableAppendFrame(path, invalidation)) {
      return fail(MgaDmlUpdateDurableOperationOutcomeV1::storage_failure,
                  "prepared_successor_invalidation_write_failed");
    }
  }
  const auto& latest = current.journal.back();
  if (latest == request.successor) {
    result.outcome = MgaDmlUpdateDurableOperationOutcomeV1::already_exact;
    result.diagnostic = OkDiagnostic();
    return result;
  }
  if (latest.lifecycle_state == MgaDmlUpdateDurableJournalStateV1::published ||
      latest.lifecycle_state == MgaDmlUpdateDurableJournalStateV1::aborted) {
    return fail(
        MgaDmlUpdateDurableOperationOutcomeV1::fork_or_terminal_conflict,
        "durable_chain_terminal");
  }
  if (latest.journal_sequence != request.expected_prior_sequence ||
      latest.lifecycle_state != request.expected_prior_state ||
      latest.record_evidence_sha256 !=
          request.expected_prior_record_evidence_sha256 ||
      request.successor.journal_sequence != latest.journal_sequence + 1 ||
      request.successor.prior_record_sha256 !=
          latest.record_evidence_sha256 ||
      !DmlUpdateDurableLegalTransition(latest.lifecycle_state,
                                       request.successor.lifecycle_state)) {
    return fail(MgaDmlUpdateDurableOperationOutcomeV1::fork_or_terminal_conflict,
                "successor_compare_and_append_conflict");
  }
  if (DmlUpdateDurableFault(
          MgaDmlUpdateDurableFaultCutpointV1::before_successor_write)) {
    return fail(MgaDmlUpdateDurableOperationOutcomeV1::storage_failure,
                "fault_before_successor_write");
  }
  DmlUpdateDurableFrameV1 frame =
      DmlUpdateDurableJournalFrame(request.identity, request.successor);
  std::vector<std::uint8_t> encoded;
  g_dml_update_durable_frame_encode_calls.fetch_add(
      1, std::memory_order_relaxed);
  g_dml_update_durable_checksum_calls.fetch_add(
      2, std::memory_order_relaxed);
  if (!DmlUpdateDurableEncodeFrame(frame, &encoded)) {
    return fail(MgaDmlUpdateDurableOperationOutcomeV1::storage_failure,
                "successor_prepare_encode_failed");
  }
  std::vector<std::uint8_t> encoded_invalidation;
  if (!DmlUpdateDurableEncodePreparedInvalidation(
          request.identity, request.successor, &encoded_invalidation)) {
    return fail(MgaDmlUpdateDurableOperationOutcomeV1::storage_failure,
                "successor_invalidation_prepare_encode_failed");
  }
  DmlUpdateDurableFrameV1 staged;
  staged.kind = DmlUpdateDurableFrameKindV1::prepared_successor;
  staged.identity = request.identity;
  staged.sequence = request.successor.journal_sequence;
  staged.state =
      static_cast<std::uint8_t>(request.successor.lifecycle_state);
  staged.prior_record_sha256 = request.successor.prior_record_sha256;
  staged.record_evidence_sha256 =
      request.successor.record_evidence_sha256;
  staged.payload = encoded;
  if (!DmlUpdateDurableAppendFrame(path, staged)) {
    return fail(MgaDmlUpdateDurableOperationOutcomeV1::storage_failure,
                "successor_prepare_stage_write_failed");
  }
  auto impl = std::make_unique<MgaDmlUpdateDurablePreparedSuccessorV1::Impl>();
  impl->path = path;
  impl->lock = std::move(lock);
  impl->encoded_frame = std::move(encoded);
  impl->encoded_invalidation_frame = std::move(encoded_invalidation);
  impl->valid = true;
  result.prepared.impl_ = std::move(impl);
  result.outcome = MgaDmlUpdateDurableOperationOutcomeV1::committed;
  result.diagnostic = OkDiagnostic();
  return result;
}

MgaDmlUpdateDurableOperationMutationResultV1
CommitPreparedMgaDmlUpdateDurableOperationSuccessorV1(
    MgaDmlUpdateDurablePreparedSuccessorV1&& prepared) {
  g_dml_update_durable_commit_calls.fetch_add(1,
                                               std::memory_order_relaxed);
  if (!prepared.valid()) {
    return DmlUpdateDurableMutation(
        MgaDmlUpdateDurableOperationOutcomeV1::conflict,
        "prepared_successor_invalid");
  }
  auto impl = std::move(prepared.impl_);
  impl->valid = false;
  const auto appended = DmlUpdateDurableAppendEncodedFrame(
      impl->path, impl->encoded_frame, true);
  if (appended == DmlUpdateDurableRawAppendResultV1::ok) {
    MgaDmlUpdateDurableOperationMutationResultV1 result;
    result.outcome = MgaDmlUpdateDurableOperationOutcomeV1::committed;
    result.diagnostic = std::move(impl->committed_diagnostic);
    return result;
  }
  if (appended ==
      DmlUpdateDurableRawAppendResultV1::after_fsync_ack_lost) {
    MgaDmlUpdateDurableOperationMutationResultV1 result;
    result.outcome = MgaDmlUpdateDurableOperationOutcomeV1::storage_failure;
    result.diagnostic =
        std::move(impl->committed_ack_lost_diagnostic);
    return result;
  }
  MgaDmlUpdateDurableOperationMutationResultV1 result;
  result.outcome = MgaDmlUpdateDurableOperationOutcomeV1::storage_failure;
  result.diagnostic = std::move(impl->commit_write_failed_diagnostic);
  return result;
}

MgaDmlUpdateDurableOperationMutationResultV1
CancelPreparedMgaDmlUpdateDurableOperationSuccessorV1(
    MgaDmlUpdateDurablePreparedSuccessorV1&& prepared) {
  if (!prepared.valid()) {
    return DmlUpdateDurableMutation(
        MgaDmlUpdateDurableOperationOutcomeV1::conflict,
        "prepared_successor_cancel_invalid");
  }
  auto impl = std::move(prepared.impl_);
  impl->valid = false;
  const auto appended = DmlUpdateDurableAppendEncodedFrame(
      impl->path, impl->encoded_invalidation_frame, false);
  if (appended == DmlUpdateDurableRawAppendResultV1::ok) {
    MgaDmlUpdateDurableOperationMutationResultV1 result;
    result.outcome = MgaDmlUpdateDurableOperationOutcomeV1::committed;
    result.diagnostic = std::move(impl->cancelled_diagnostic);
    return result;
  }
  return DmlUpdateDurableMutation(
      MgaDmlUpdateDurableOperationOutcomeV1::storage_failure,
      "prepared_successor_cancel_write_failed");
}

MgaDmlUpdateDurableOperationMutationResultV1
AppendMgaDmlUpdateDurableOperationSuccessorV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateDurableAppendSuccessorRequestV1& request) {
  auto prepared =
      PrepareMgaDmlUpdateDurableOperationSuccessorV1(context, request);
  if (!prepared.ok()) {
    MgaDmlUpdateDurableOperationMutationResultV1 result;
    result.outcome = prepared.outcome;
    result.diagnostic = std::move(prepared.diagnostic);
    return result;
  }
  return CommitPreparedMgaDmlUpdateDurableOperationSuccessorV1(
      std::move(prepared.prepared));
}

namespace {

MgaDmlUpdateDurableOperationRecoveryResultV1 DmlUpdateDurableRecoveryFailure(
    MgaDmlUpdateDurableOperationOutcomeV1 outcome, std::string detail,
    bool quarantined = false) {
  MgaDmlUpdateDurableOperationRecoveryResultV1 result;
  result.outcome = outcome;
  result.quarantined = quarantined;
  const bool denied =
      outcome == MgaDmlUpdateDurableOperationOutcomeV1::access_denied;
  const bool stale = outcome == MgaDmlUpdateDurableOperationOutcomeV1::stale;
  result.diagnostic = DmlUpdateDurableDiagnostic(
      denied ? "SECURITY.ACCESS_DENIED"
             : stale ? "MGA.TRANSACTION.STALE" : "DML.UPDATE_FAILED",
      denied ? "sblr.dml_update_rows.durable_recovery_denied"
             : stale ? "sblr.dml_update_rows.durable_recovery_stale"
                     : "sblr.dml_update_rows.durable_recovery_failed",
      std::move(detail));
  return result;
}

bool DmlUpdateDurableBuildSavepointObservation(
    const EngineRequestContext& context,
    const DmlUpdateDurableStoredOperationV1& current,
    const scratchbird::wire::TypedUpdateJournalRecord& journal_head,
    scratchbird::wire::TypedUpdateMgaRecoveryObservation* observation,
    std::string* detail) {
  const auto fail = [&](std::string reason) {
    if (detail != nullptr) *detail = std::move(reason);
    return false;
  };
  if (observation == nullptr) return fail("recovery_observation_required");

  const auto journal_state = journal_head.lifecycle_state;
  std::string required_savepoint_uuid;
  const bool journal_savepoint_present =
      !DmlUpdateDurableTypedUuidText(
           journal_head.statement_savepoint_uuid).empty();
  if (journal_state != scratchbird::wire::TypedUpdateJournalState::bound) {
    required_savepoint_uuid = DmlUpdateDurableTypedUuidText(
        journal_head.statement_savepoint_uuid);
    const bool nil_aborted =
        journal_state == scratchbird::wire::TypedUpdateJournalState::aborted &&
        !journal_savepoint_present &&
        journal_head.statement_savepoint_generation == 0;
    if (!nil_aborted &&
        (required_savepoint_uuid.empty() ||
         journal_head.statement_savepoint_generation == 0)) {
      return fail("journal_savepoint_identity_invalid");
    }
  }
  DmlUpdateDurableSavepointLookupV1 savepoint;
  if (journal_state == scratchbird::wire::TypedUpdateJournalState::bound ||
      journal_savepoint_present) {
    savepoint = DmlUpdateDurableFindStatementSavepoint(
        context, current.identity, required_savepoint_uuid);
  }
  if (savepoint.state == DmlUpdateDurableSavepointLookupStateV1::corrupt) {
    return fail(savepoint.detail.empty() ? "savepoint_authority_corrupt"
                                        : savepoint.detail);
  }
  if (savepoint.state == DmlUpdateDurableSavepointLookupStateV1::present &&
      (savepoint.authority.publication_barrier_uuid !=
           current.identity.reserved_statement_barrier_uuid ||
       savepoint.authority.publication_barrier_generation !=
           current.identity.reserved_statement_barrier_generation)) {
    return fail("savepoint_reserved_barrier_mismatch");
  }

  // A crash between opening the provider savepoint and appending intent leaves
  // a bound DUJR with an active private savepoint.  DUMO forbids representing
  // that contradictory cutpoint, so MGA rolls it back before observing the
  // bound head and records only the resulting durable no-effect proof.
  if (journal_state == scratchbird::wire::TypedUpdateJournalState::bound &&
      savepoint.state == DmlUpdateDurableSavepointLookupStateV1::present &&
      savepoint.authority.lifecycle ==
          MgaDmlUpdateStatementSavepointLifecycleV1::active) {
    const auto rolled_back = RollbackMgaDmlUpdateStatementSavepointAuthorityV1(
        context, savepoint.authority);
    if (!rolled_back.ok) {
      return fail("bound_orphan_savepoint_rollback_failed:" +
                  rolled_back.diagnostic.detail);
    }
    savepoint.authority = rolled_back.authority;
  }

  observation->statement_barrier_present = false;
  observation->no_surviving_effect_proven = false;
  observation->statement_savepoint_uuid = {};
  observation->statement_savepoint_generation = 0;
  if (savepoint.state == DmlUpdateDurableSavepointLookupStateV1::absent) {
    if (journal_state != scratchbird::wire::TypedUpdateJournalState::bound &&
        !(journal_state ==
              scratchbird::wire::TypedUpdateJournalState::aborted &&
          required_savepoint_uuid.empty())) {
      return fail("journal_savepoint_authority_missing");
    }
    observation->savepoint_state =
        scratchbird::wire::TypedUpdateSavepointState::absent;
    observation->no_surviving_effect_proven = true;
  } else {
    if (!DmlUpdateDurableTypedUuid(
            savepoint.authority.savepoint_uuid,
            &observation->statement_savepoint_uuid)) {
      return fail("savepoint_uuid_invalid");
    }
    observation->statement_savepoint_generation =
        savepoint.authority.savepoint_generation;
    switch (savepoint.authority.lifecycle) {
      case MgaDmlUpdateStatementSavepointLifecycleV1::active:
        observation->savepoint_state =
            scratchbird::wire::TypedUpdateSavepointState::active;
        break;
      case MgaDmlUpdateStatementSavepointLifecycleV1::rolled_back:
        observation->savepoint_state =
            scratchbird::wire::TypedUpdateSavepointState::rolled_back_final;
        observation->no_surviving_effect_proven = true;
        break;
      case MgaDmlUpdateStatementSavepointLifecycleV1::released:
        observation->savepoint_state =
            scratchbird::wire::TypedUpdateSavepointState::
                released_at_statement_barrier;
        observation->statement_barrier_present = true;
        break;
    }
  }

  if (!DmlUpdateDurableTypedUuid(
          current.identity.reserved_statement_barrier_uuid,
          &observation->reserved_statement_barrier_uuid)) {
    return fail("reserved_statement_barrier_invalid");
  }
  observation->reserved_statement_barrier_generation =
      current.identity.reserved_statement_barrier_generation;
  return true;
}

bool DmlUpdateDurableDecodeRecoveryAuthority(
    const DmlUpdateDurableStoredOperationV1& current,
    std::span<const scratchbird::wire::TypedUpdateJournalRecord>
        journal_records,
    const scratchbird::wire::TypedUpdateMgaRecoveryObservation& observation,
    std::string* detail) {
  const auto fail = [&](const scratchbird::wire::TypedUpdateCarrierError& error,
                        std::string prefix) {
    if (detail != nullptr) {
      *detail = std::move(prefix) + ":" + error.field + ":" + error.detail;
    }
    return false;
  };
  if (journal_records.empty()) {
    if (detail != nullptr) *detail = "journal_chain_required";
    return false;
  }
  scratchbird::wire::TypedUpdateDescriptorCarrier descriptor;
  scratchbird::wire::TypedUpdateRowPolicyVector row_policies;
  scratchbird::wire::TypedUpdateRecoveryTokenCarrier recovery_token;
  scratchbird::wire::TypedUpdateSecurityPolicySourceVector source_policies;
  scratchbird::wire::TypedUpdateSecuritySnapshotProof security_proof;
  scratchbird::wire::TypedUpdateCarrierError error;
  if (!scratchbird::wire::DecodeAndValidateTypedUpdateDescriptor(
          current.snapshot.descriptor_dudc, &descriptor, &error)) {
    return fail(error, "DUDC");
  }
  if (!scratchbird::wire::DecodeAndValidateTypedUpdateRowPolicyVector(
          current.snapshot.row_policy_vector_dupv, &row_policies, &error)) {
    return fail(error, "DUPV");
  }
  if (!scratchbird::wire::DecodeAndValidateTypedUpdateRecoveryToken(
          current.snapshot.recovery_token_durc, &recovery_token, &error)) {
    return fail(error, "DURC");
  }
  if (!scratchbird::wire::DecodeAndValidateTypedUpdateSecurityPolicySourceVector(
          current.snapshot.source_policy_vector_dusv, &source_policies,
          &error)) {
    return fail(error, "DUSV");
  }
  if (!scratchbird::wire::DecodeAndValidateTypedUpdateSecuritySnapshotProof(
          current.snapshot.security_snapshot_proof_dusp, &security_proof,
          &error)) {
    return fail(error, "DUSP");
  }
  if (!scratchbird::wire::ValidateTypedUpdateSecurityRecoveryAuthority(
          descriptor, row_policies, recovery_token, source_policies,
          security_proof, journal_records.back(), observation, &error)) {
    return fail(error, "security_recovery_authority");
  }
  return true;
}

}  // namespace

MgaDmlUpdateDurableOperationRecoveryResultV1
RecoverMgaDmlUpdateDurableOperationChainV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateDurableOperationLookupV1& lookup) {
  g_dml_update_durable_recovery_calls.fetch_add(1,
                                                 std::memory_order_relaxed);
  const std::string path = DmlUpdateDurablePathForLookup(context, lookup);
  if (path.empty()) {
    return DmlUpdateDurableRecoveryFailure(
        MgaDmlUpdateDurableOperationOutcomeV1::access_denied,
        "authenticated_descriptor_lookup_invalid");
  }
  DmlUpdateDurableFileLock lock(path);
  if (!lock.ok()) {
    return DmlUpdateDurableRecoveryFailure(
        MgaDmlUpdateDurableOperationOutcomeV1::storage_failure,
        "durable_recovery_lock_failed");
  }
  auto current = DmlUpdateDurableLoadOperation(path, true);
  if (!current.ok || current.reservation_only || !current.snapshot_present ||
      current.journal.empty()) {
    const bool denied = current.missing || current.reservation_only;
    return DmlUpdateDurableRecoveryFailure(
        current.quarantined
            ? MgaDmlUpdateDurableOperationOutcomeV1::quarantined
            : denied ? MgaDmlUpdateDurableOperationOutcomeV1::access_denied
                     : MgaDmlUpdateDurableOperationOutcomeV1::storage_failure,
        current.detail.empty() ? "durable_chain_unavailable" : current.detail,
        current.quarantined);
  }
  if (!DmlUpdateDurableIdentityMatchesContext(context, current.identity)) {
    return DmlUpdateDurableRecoveryFailure(
        MgaDmlUpdateDurableOperationOutcomeV1::access_denied,
        "durable_chain_cross_authority");
  }
  if (current.identity.descriptor_generation != lookup.descriptor_generation ||
      current.structural_occurrence_id != lookup.structural_occurrence_id) {
    return DmlUpdateDurableRecoveryFailure(
        MgaDmlUpdateDurableOperationOutcomeV1::stale,
        "descriptor_generation_or_occurrence_stale");
  }

  std::vector<scratchbird::wire::TypedUpdateJournalRecord> journal_records;
  std::string detail;
  if (!DmlUpdateDurableDecodeTypedJournalChain(
          context, current.identity, current.snapshot, current.journal,
          &journal_records, &detail)) {
    (void)DmlUpdateDurableWriteQuarantine(path);
    return DmlUpdateDurableRecoveryFailure(
        MgaDmlUpdateDurableOperationOutcomeV1::quarantined,
        std::move(detail), true);
  }

  scratchbird::wire::TypedUpdateDescriptorCarrier descriptor;
  scratchbird::wire::TypedUpdateSecuritySnapshotProof security_proof;
  scratchbird::wire::TypedUpdateCarrierError carrier_error;
  if (!scratchbird::wire::DecodeAndValidateTypedUpdateDescriptor(
          current.snapshot.descriptor_dudc, &descriptor, &carrier_error) ||
      !scratchbird::wire::DecodeAndValidateTypedUpdateSecuritySnapshotProof(
          current.snapshot.security_snapshot_proof_dusp, &security_proof,
          &carrier_error)) {
    (void)DmlUpdateDurableWriteQuarantine(path);
    return DmlUpdateDurableRecoveryFailure(
        MgaDmlUpdateDurableOperationOutcomeV1::quarantined,
        "recovery_snapshot_decode_failed:" + carrier_error.field + ":" +
            carrier_error.detail,
        true);
  }

  scratchbird::wire::TypedUpdateMgaRecoveryObservation observation;
  if (!DmlUpdateDurableTypedUuid(current.identity.validated_durable_handle_uuid,
                                  &observation.validated_mga_durable_handle_uuid) ||
      !DmlUpdateDurableTypedUuid(current.identity.database_uuid,
                                  &observation.database_uuid) ||
      !DmlUpdateDurableTypedUuid(current.identity.descriptor_uuid,
                                  &observation.descriptor_uuid) ||
      !DmlUpdateDurableTypedUuid(current.identity.operation_uuid,
                                  &observation.operation_uuid) ||
      !DmlUpdateDurableTypedUuid(
          current.identity.authenticated_statement_receipt_uuid,
          &observation.authenticated_statement_receipt_uuid) ||
      !DmlUpdateDurableTypedUuid(current.identity.owning_transaction_uuid,
                                  &observation.owning_transaction_uuid) ||
      !DmlUpdateDurableTypedUuid(current.identity.recovery_token_uuid,
                                  &observation.recovery_token_uuid)) {
    (void)DmlUpdateDurableWriteQuarantine(path);
    return DmlUpdateDurableRecoveryFailure(
        MgaDmlUpdateDurableOperationOutcomeV1::quarantined,
        "durable_identity_uuid_decode_failed", true);
  }
  observation.validated_mga_durable_handle_generation =
      current.identity.validated_durable_handle_generation;
  observation.descriptor_generation = current.identity.descriptor_generation;
  observation.operation_generation = current.identity.operation_generation;
  observation.owning_local_transaction_id =
      current.identity.owning_local_transaction_id;
  observation.recovery_generation = current.identity.recovery_generation;
  observation.latest_journal_state = journal_records.back().lifecycle_state;
  observation.durable_chain_head_sequence =
      journal_records.back().journal_sequence;
  observation.durable_chain_head_record_evidence_sha256 =
      journal_records.back().record_evidence_sha256;
  observation.catalog_snapshot_uuid = descriptor.catalog_snapshot_uuid;
  observation.catalog_generation = descriptor.catalog_generation;
  observation.security_snapshot_uuid = security_proof.security_snapshot_uuid;
  observation.security_snapshot_generation =
      security_proof.security_snapshot_generation;
  observation.security_epoch = security_proof.security_epoch;
  if (!DmlUpdateDurableTransactionState(context, current.identity,
                                        &observation.transaction_state,
                                        &detail) ||
      !DmlUpdateDurableBuildSavepointObservation(
          context, current, journal_records.back(), &observation, &detail)) {
    (void)DmlUpdateDurableWriteQuarantine(path);
    return DmlUpdateDurableRecoveryFailure(
        MgaDmlUpdateDurableOperationOutcomeV1::quarantined,
        std::move(detail), true);
  }

  scratchbird::wire::TypedUpdateMgaRecoveryObservation prior_observation;
  bool prior_present = false;
  if (!current.latest_dumo.empty()) {
    if (!scratchbird::wire::DecodeAndValidateTypedUpdateMgaRecoveryObservation(
            current.latest_dumo, &prior_observation, &carrier_error)) {
      (void)DmlUpdateDurableWriteQuarantine(path);
      return DmlUpdateDurableRecoveryFailure(
          MgaDmlUpdateDurableOperationOutcomeV1::quarantined,
          "stored_DUMO_invalid:" + carrier_error.field + ":" +
              carrier_error.detail,
          true);
    }
    observation.observation_uuid = prior_observation.observation_uuid;
    observation.observation_generation =
        prior_observation.observation_generation;
    prior_present = true;
  } else {
    const std::string fresh = DmlUpdateDurableFreshIdentity(
        current.identity, current.identity.reserved_statement_barrier_uuid);
    if (!DmlUpdateDurableTypedUuid(fresh, &observation.observation_uuid)) {
      return DmlUpdateDurableRecoveryFailure(
          MgaDmlUpdateDurableOperationOutcomeV1::storage_failure,
          "observation_identity_issue_failed");
    }
    observation.observation_generation = 1;
  }

  std::vector<std::uint8_t> exact_dumo;
  g_dml_update_durable_observation_encode_calls.fetch_add(
      1, std::memory_order_relaxed);
  if (!scratchbird::wire::EncodeTypedUpdateMgaRecoveryObservation(
          observation, &exact_dumo, &carrier_error)) {
    (void)DmlUpdateDurableWriteQuarantine(path);
    return DmlUpdateDurableRecoveryFailure(
        MgaDmlUpdateDurableOperationOutcomeV1::quarantined,
        "DUMO_encode_failed:" + carrier_error.field + ":" +
            carrier_error.detail,
        true);
  }
  if (prior_present && exact_dumo != current.latest_dumo) {
    if (observation.observation_generation ==
        std::numeric_limits<std::uint64_t>::max()) {
      (void)DmlUpdateDurableWriteQuarantine(path);
      return DmlUpdateDurableRecoveryFailure(
          MgaDmlUpdateDurableOperationOutcomeV1::quarantined,
          "observation_generation_exhausted", true);
    }
    ++observation.observation_generation;
    g_dml_update_durable_observation_encode_calls.fetch_add(
        1, std::memory_order_relaxed);
    if (!scratchbird::wire::EncodeTypedUpdateMgaRecoveryObservation(
            observation, &exact_dumo, &carrier_error)) {
      (void)DmlUpdateDurableWriteQuarantine(path);
      return DmlUpdateDurableRecoveryFailure(
          MgaDmlUpdateDurableOperationOutcomeV1::quarantined,
          "DUMO_successor_encode_failed:" + carrier_error.field + ":" +
              carrier_error.detail,
          true);
    }
  }
  observation.exact_bytes = exact_dumo;
  if (!DmlUpdateDurableDecodeRecoveryAuthority(
          current, journal_records, observation, &detail)) {
    (void)DmlUpdateDurableWriteQuarantine(path);
    return DmlUpdateDurableRecoveryFailure(
        MgaDmlUpdateDurableOperationOutcomeV1::quarantined,
        std::move(detail), true);
  }

  if (exact_dumo != current.latest_dumo) {
    if (DmlUpdateDurableFault(
            MgaDmlUpdateDurableFaultCutpointV1::before_observation_write)) {
      return DmlUpdateDurableRecoveryFailure(
          MgaDmlUpdateDurableOperationOutcomeV1::storage_failure,
          "fault_before_observation_write");
    }
    DmlUpdateDurableFrameV1 frame;
    frame.kind = DmlUpdateDurableFrameKindV1::recovery_observation;
    frame.identity = current.identity;
    frame.sequence = observation.durable_chain_head_sequence;
    frame.state = static_cast<std::uint8_t>(observation.latest_journal_state);
    frame.record_evidence_sha256 =
        observation.observation_evidence_sha256;
    frame.payload = exact_dumo;
    if (!DmlUpdateDurableAppendFrame(path, frame)) {
      return DmlUpdateDurableRecoveryFailure(
          MgaDmlUpdateDurableOperationOutcomeV1::storage_failure,
          "recovery_observation_write_failed");
    }
    if (DmlUpdateDurableFault(
            MgaDmlUpdateDurableFaultCutpointV1::
                after_observation_write_before_ack)) {
      return DmlUpdateDurableRecoveryFailure(
          MgaDmlUpdateDurableOperationOutcomeV1::storage_failure,
          "recovery_observation_committed_ack_lost");
    }
    current.latest_dumo = exact_dumo;
  }

  auto impl =
      std::make_unique<MgaDmlUpdateValidatedDurableAuthorityHandleV1::Impl>();
  impl->identity = current.identity;
  impl->snapshot = current.snapshot;
  impl->journal = current.journal;
  impl->staged_successor_present = current.staged_successor_present;
  impl->staged_successor = current.staged_successor;
  impl->staged_encoded_journal_frame =
      current.staged_encoded_journal_frame;
  std::error_code extent_error;
  impl->authenticated_store_extent_bytes =
      std::filesystem::file_size(path, extent_error);
  if (extent_error || impl->authenticated_store_extent_bytes == 0) {
    return DmlUpdateDurableRecoveryFailure(
        MgaDmlUpdateDurableOperationOutcomeV1::storage_failure,
        "durable_store_extent_observation_failed");
  }
  impl->exact_dumo = exact_dumo;
  MgaDmlUpdateDurableOperationRecoveryResultV1 result;
  result.outcome = MgaDmlUpdateDurableOperationOutcomeV1::committed;
  result.diagnostic = OkDiagnostic();
  result.identity = current.identity;
  result.authority_snapshot = current.snapshot;
  result.journal = current.journal;
  result.staged_successor_present = current.staged_successor_present;
  result.staged_successor = current.staged_successor;
  result.recovery_observation_dumo = exact_dumo;
  result.validated_handle.impl_ = std::move(impl);
  return result;
}

MgaDmlUpdateDurableOperationMutationResultV1
RollbackMgaDmlUpdateStatementFromValidatedDurableAuthorityV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateValidatedDurableAuthorityHandleV1& validated_handle) {
  if (!validated_handle.valid() ||
      !DmlUpdateDurableIdentityMatchesContext(
          context, validated_handle.impl_->identity)) {
    return DmlUpdateDurableMutation(
        MgaDmlUpdateDurableOperationOutcomeV1::access_denied,
        "validated_durable_handle_cross_authority");
  }
  scratchbird::wire::TypedUpdateMgaRecoveryObservation observation;
  scratchbird::wire::TypedUpdateCarrierError error;
  if (!scratchbird::wire::DecodeAndValidateTypedUpdateMgaRecoveryObservation(
          validated_handle.impl_->exact_dumo, &observation, &error)) {
    return DmlUpdateDurableMutation(
        MgaDmlUpdateDurableOperationOutcomeV1::quarantined,
        "validated_durable_handle_DUMO_invalid:" + error.field + ":" +
            error.detail);
  }
  if (observation.savepoint_state ==
          scratchbird::wire::TypedUpdateSavepointState::absent ||
      observation.savepoint_state ==
          scratchbird::wire::TypedUpdateSavepointState::rolled_back_final) {
    return DmlUpdateDurableMutation(
        MgaDmlUpdateDurableOperationOutcomeV1::already_exact);
  }
  if (observation.savepoint_state !=
          scratchbird::wire::TypedUpdateSavepointState::active ||
      observation.statement_barrier_present) {
    return DmlUpdateDurableMutation(
        MgaDmlUpdateDurableOperationOutcomeV1::fork_or_terminal_conflict,
        "postbarrier_savepoint_cannot_rollback");
  }
  const std::string savepoint_uuid = DmlUpdateDurableTypedUuidText(
      observation.statement_savepoint_uuid);
  const auto binding = DmlUpdateDurableStatementBinding(
      validated_handle.impl_->identity);
  auto authority = RecoverMgaDmlUpdateStatementSavepointAuthorityV1(
      context, binding, savepoint_uuid,
      observation.statement_savepoint_generation);
  if (!authority.ok || authority.authority.lifecycle !=
                           MgaDmlUpdateStatementSavepointLifecycleV1::active ||
      authority.authority.publication_barrier_uuid !=
          validated_handle.impl_->identity.reserved_statement_barrier_uuid ||
      authority.authority.publication_barrier_generation !=
          validated_handle.impl_->identity
              .reserved_statement_barrier_generation) {
    return DmlUpdateDurableMutation(
        MgaDmlUpdateDurableOperationOutcomeV1::stale,
        "validated_savepoint_authority_not_current");
  }
  authority = RollbackMgaDmlUpdateStatementSavepointAuthorityV1(
      context, authority.authority);
  if (!authority.ok || authority.authority.lifecycle !=
                           MgaDmlUpdateStatementSavepointLifecycleV1::rolled_back ||
      authority.authority.publication_barrier_present) {
    return DmlUpdateDurableMutation(
        MgaDmlUpdateDurableOperationOutcomeV1::storage_failure,
        "validated_savepoint_rollback_failed");
  }
  return DmlUpdateDurableMutation(
      MgaDmlUpdateDurableOperationOutcomeV1::committed);
}

MgaDmlUpdateDurableOperationMutationResultV1
CommitRecoveredMgaDmlUpdateDurableOperationStagedSuccessorV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateValidatedDurableAuthorityHandleV1& validated_handle) {
  if (!validated_handle.valid() ||
      !DmlUpdateDurableIdentityMatchesContext(
          context, validated_handle.impl_->identity)) {
    return DmlUpdateDurableMutation(
        MgaDmlUpdateDurableOperationOutcomeV1::access_denied,
        "recovered_staged_successor_cross_authority");
  }
  const auto& impl = *validated_handle.impl_;
  if (!impl.staged_successor_present ||
      impl.staged_successor.lifecycle_state !=
          MgaDmlUpdateDurableJournalStateV1::published ||
      impl.staged_encoded_journal_frame.empty() || impl.journal.empty() ||
      impl.staged_successor.journal_sequence !=
          impl.journal.back().journal_sequence + 1 ||
      impl.staged_successor.prior_record_sha256 !=
          impl.journal.back().record_evidence_sha256 ||
      impl.authenticated_store_extent_bytes == 0) {
    return DmlUpdateDurableMutation(
        MgaDmlUpdateDurableOperationOutcomeV1::fork_or_terminal_conflict,
        "recovered_staged_published_successor_unavailable");
  }
  const std::string path =
      DmlUpdateDurableDescriptorPath(context, impl.identity);
  DmlUpdateDurableFileLock lock(path);
  if (!lock.ok()) {
    return DmlUpdateDurableMutation(
        MgaDmlUpdateDurableOperationOutcomeV1::storage_failure,
        "recovered_staged_successor_lock_failed");
  }
  std::error_code extent_error;
  const auto current_extent = std::filesystem::file_size(path, extent_error);
  if (extent_error) {
    return DmlUpdateDurableMutation(
        MgaDmlUpdateDurableOperationOutcomeV1::storage_failure,
        "recovered_staged_successor_extent_failed");
  }
  if (current_extent == impl.authenticated_store_extent_bytes +
                            impl.staged_encoded_journal_frame.size()) {
    return DmlUpdateDurableMutation(
        MgaDmlUpdateDurableOperationOutcomeV1::already_exact);
  }
  if (current_extent != impl.authenticated_store_extent_bytes) {
    return DmlUpdateDurableMutation(
        MgaDmlUpdateDurableOperationOutcomeV1::stale,
        "recovered_staged_successor_chain_changed");
  }
  const auto appended = DmlUpdateDurableAppendEncodedFrame(
      path, impl.staged_encoded_journal_frame, true);
  if (appended == DmlUpdateDurableRawAppendResultV1::ok) {
    return DmlUpdateDurableMutation(
        MgaDmlUpdateDurableOperationOutcomeV1::committed);
  }
  return DmlUpdateDurableMutation(
      MgaDmlUpdateDurableOperationOutcomeV1::storage_failure,
      appended == DmlUpdateDurableRawAppendResultV1::after_fsync_ack_lost
          ? "recovered_staged_successor_committed_ack_lost"
          : "recovered_staged_successor_write_failed");
}

MgaDmlUpdateDurableOperationInstrumentationV1
ReadMgaDmlUpdateDurableOperationInstrumentationV1() {
  MgaDmlUpdateDurableOperationInstrumentationV1 result;
  result.prepare_calls =
      g_dml_update_durable_prepare_calls.load(std::memory_order_acquire);
  result.frame_encode_calls =
      g_dml_update_durable_frame_encode_calls.load(std::memory_order_acquire);
  result.checksum_calls =
      g_dml_update_durable_checksum_calls.load(std::memory_order_acquire);
  result.commit_calls =
      g_dml_update_durable_commit_calls.load(std::memory_order_acquire);
  result.commit_write_calls =
      g_dml_update_durable_commit_write_calls.load(std::memory_order_acquire);
  result.commit_fsync_calls =
      g_dml_update_durable_commit_fsync_calls.load(std::memory_order_acquire);
  result.recovery_calls =
      g_dml_update_durable_recovery_calls.load(std::memory_order_acquire);
  result.observation_encode_calls =
      g_dml_update_durable_observation_encode_calls.load(
          std::memory_order_acquire);
  return result;
}

void ResetMgaDmlUpdateDurableOperationInstrumentationForTestingV1() {
  g_dml_update_durable_prepare_calls.store(0, std::memory_order_release);
  g_dml_update_durable_frame_encode_calls.store(0,
                                                 std::memory_order_release);
  g_dml_update_durable_checksum_calls.store(0, std::memory_order_release);
  g_dml_update_durable_commit_calls.store(0, std::memory_order_release);
  g_dml_update_durable_commit_write_calls.store(0,
                                                 std::memory_order_release);
  g_dml_update_durable_commit_fsync_calls.store(0,
                                                 std::memory_order_release);
  g_dml_update_durable_recovery_calls.store(0, std::memory_order_release);
  g_dml_update_durable_observation_encode_calls.store(
      0, std::memory_order_release);
  g_dml_update_durable_fault_cutpoint.store(
      MgaDmlUpdateDurableFaultCutpointV1::none,
      std::memory_order_release);
}

void SetMgaDmlUpdateDurableFaultCutpointForTestingV1(
    MgaDmlUpdateDurableFaultCutpointV1 cutpoint) {
  g_dml_update_durable_fault_cutpoint.store(cutpoint,
                                             std::memory_order_release);
}

MgaDmlUpdateDurableInspectionV1 InspectMgaDmlUpdateDurableOperationForTestingV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateDurableOperationLookupV1& lookup) {
  MgaDmlUpdateDurableInspectionV1 result;
  const std::string path = DmlUpdateDurablePathForLookup(context, lookup);
  if (path.empty()) {
    result.outcome = MgaDmlUpdateDurableOperationOutcomeV1::access_denied;
    result.diagnostic = DmlUpdateDurableDiagnostic(
        "SECURITY.ACCESS_DENIED",
        "sblr.dml_update_rows.durable_inspection_denied");
    return result;
  }
  DmlUpdateDurableFileLock lock(path);
  if (!lock.ok()) {
    result.outcome = MgaDmlUpdateDurableOperationOutcomeV1::storage_failure;
    result.diagnostic = DmlUpdateDurableDiagnostic(
        "DML.UPDATE_FAILED",
        "sblr.dml_update_rows.durable_inspection_failed",
        "lock_failed");
    return result;
  }
  const auto current = DmlUpdateDurableLoadOperation(path, true);
  result.quarantined = current.quarantined;
  if (!current.ok || current.reservation_only || !current.snapshot_present ||
      current.journal.empty() ||
      !DmlUpdateDurableIdentityMatchesContext(context, current.identity) ||
      current.identity.descriptor_generation != lookup.descriptor_generation ||
      current.structural_occurrence_id != lookup.structural_occurrence_id) {
    result.outcome = current.quarantined
                         ? MgaDmlUpdateDurableOperationOutcomeV1::quarantined
                         : current.missing
                               ? MgaDmlUpdateDurableOperationOutcomeV1::access_denied
                               : MgaDmlUpdateDurableOperationOutcomeV1::stale;
    result.diagnostic = DmlUpdateDurableDiagnostic(
        current.missing ? "SECURITY.ACCESS_DENIED"
                        : current.quarantined ? "DML.UPDATE_FAILED"
                                              : "MGA.TRANSACTION.STALE",
        "sblr.dml_update_rows.durable_inspection_refused",
        current.detail);
    return result;
  }
  result.outcome = MgaDmlUpdateDurableOperationOutcomeV1::committed;
  result.diagnostic = OkDiagnostic();
  result.authority_snapshot = current.snapshot;
  result.journal = current.journal;
  result.exact_dumo_bytes = current.latest_dumo;
  return result;
}

namespace {

bool DmlUpdateDurableAuthenticateTestingLookup(
    const EngineRequestContext& context,
    const MgaDmlUpdateDurableOperationLookupV1& lookup,
    std::string* path) {
  if (path == nullptr) return false;
  *path = DmlUpdateDurablePathForLookup(context, lookup);
  if (path->empty()) return false;
  const auto current = DmlUpdateDurableLoadOperation(*path, false);
  return current.ok && !current.reservation_only &&
         current.snapshot_present && !current.journal.empty() &&
         DmlUpdateDurableIdentityMatchesContext(context, current.identity) &&
         current.identity.descriptor_generation == lookup.descriptor_generation &&
         current.structural_occurrence_id == lookup.structural_occurrence_id;
}

bool DmlUpdateDurableFsyncParent(const EngineRequestContext& context) {
  return DmlUpdateDurableEnsureDirectory(
      DmlUpdateDurableOperationStorePath(context));
}

}  // namespace

bool CorruptMgaDmlUpdateDurableExtentByteForTestingV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateDurableOperationLookupV1& lookup,
    std::uint64_t exact_file_offset, std::uint8_t xor_mask) {
  if (xor_mask == 0) return false;
  std::string path;
  if (!DmlUpdateDurableAuthenticateTestingLookup(context, lookup, &path)) {
    return false;
  }
  DmlUpdateDurableFileLock lock(path);
  if (!lock.ok()) return false;
#if defined(_WIN32)
  HANDLE handle = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER position{};
  position.QuadPart = static_cast<LONGLONG>(exact_file_offset);
  bool ok = SetFilePointerEx(handle, position, nullptr, FILE_BEGIN) != 0;
  std::uint8_t value = 0;
  DWORD count = 0;
  ok = ok && ReadFile(handle, &value, 1, &count, nullptr) != 0 && count == 1;
  value ^= xor_mask;
  ok = ok && SetFilePointerEx(handle, position, nullptr, FILE_BEGIN) != 0 &&
       WriteFile(handle, &value, 1, &count, nullptr) != 0 && count == 1 &&
       FlushFileBuffers(handle) != 0;
  CloseHandle(handle);
  return ok;
#else
  const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
  if (fd < 0) return false;
  std::uint8_t value = 0;
  const ssize_t read = ::pread(fd, &value, 1,
                               static_cast<off_t>(exact_file_offset));
  value ^= xor_mask;
  const bool ok = read == 1 &&
                  ::pwrite(fd, &value, 1,
                           static_cast<off_t>(exact_file_offset)) == 1 &&
                  ::fsync(fd) == 0;
  ::close(fd);
  return ok;
#endif
}

bool TruncateMgaDmlUpdateDurableExtentForTestingV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateDurableOperationLookupV1& lookup,
    std::uint64_t exact_file_bytes) {
  std::string path;
  if (!DmlUpdateDurableAuthenticateTestingLookup(context, lookup, &path)) {
    return false;
  }
  DmlUpdateDurableFileLock lock(path);
  if (!lock.ok()) return false;
  std::error_code error;
  const auto current = std::filesystem::file_size(path, error);
  if (error || exact_file_bytes >= current) return false;
  std::filesystem::resize_file(path, exact_file_bytes, error);
  return !error && DmlUpdateDurableFsyncParent(context);
}

bool QuarantineMgaDmlUpdateDurableOperationForTestingV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateDurableOperationLookupV1& lookup) {
  std::string path;
  if (!DmlUpdateDurableAuthenticateTestingLookup(context, lookup, &path)) {
    return false;
  }
  DmlUpdateDurableFileLock lock(path);
  return lock.ok() && DmlUpdateDurableWriteQuarantine(path) &&
         DmlUpdateDurableFsyncParent(context);
}

bool DeleteMgaDmlUpdateDurableOperationForTestingV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateDurableOperationLookupV1& lookup) {
  std::string path;
  if (!DmlUpdateDurableAuthenticateTestingLookup(context, lookup, &path)) {
    return false;
  }
  DmlUpdateDurableFileLock lock(path);
  if (!lock.ok()) return false;
  std::error_code error;
  const bool removed = std::filesystem::remove(path, error);
  if (error || !removed) return false;
  std::filesystem::remove(DmlUpdateDurableQuarantinePath(path), error);
  return !error && DmlUpdateDurableFsyncParent(context);
}

MgaDmlUpdateStatementSavepointAuthorityResultV1
CreateMgaDmlUpdateStatementSavepointAuthorityV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateStatementSavepointBindingV1& binding) {
  const std::string reserved_barrier =
      DmlUpdateStatementFreshDistinctUuid(binding);
  if (reserved_barrier.empty()) {
    return DmlUpdateStatementSavepointFailure(
        "DML.UPDATE_FAILED",
        "sblr.dml_update_rows.statement_savepoint_create_failed",
        "engine_barrier_identity_issue_failed");
  }
  return CreateMgaDmlUpdateStatementSavepointAuthorityWithReservedBarrierV1(
      context, binding, reserved_barrier, 1);
}

MgaDmlUpdateStatementSavepointAuthorityResultV1
CreateMgaDmlUpdateStatementSavepointAuthorityWithReservedBarrierV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateStatementSavepointBindingV1& binding,
    const std::string& reserved_publication_barrier_uuid,
    std::uint64_t reserved_publication_barrier_generation) {
  std::lock_guard<std::mutex> guard(
      DmlUpdateStatementSavepointJournalMutex());
  if (!DmlUpdateStatementBindingMatchesContext(context, binding) ||
      !DmlUpdateStatementParseUuid(reserved_publication_barrier_uuid) ||
      reserved_publication_barrier_generation != 1 ||
      reserved_publication_barrier_uuid == binding.database_uuid ||
      reserved_publication_barrier_uuid == binding.owning_transaction_uuid ||
      reserved_publication_barrier_uuid ==
          binding.authenticated_statement_receipt_uuid ||
      reserved_publication_barrier_uuid == binding.operation_uuid ||
      reserved_publication_barrier_uuid == binding.descriptor_uuid ||
      reserved_publication_barrier_uuid == binding.recovery_token_uuid) {
    return DmlUpdateStatementSavepointFailure(
        "MGA.TRANSACTION.STALE",
        "sblr.dml_update_rows.statement_savepoint_stale",
        "create_binding_or_reserved_barrier_not_current");
  }
  DmlUpdateStatementSavepointJournalRecordV1 record;
  record.authority.binding = binding;
  record.authority.savepoint_uuid =
      DmlUpdateStatementFreshDistinctUuid(
          binding, reserved_publication_barrier_uuid);
  record.authority.savepoint_generation = 1;
  record.authority.publication_barrier_uuid =
      reserved_publication_barrier_uuid;
  record.authority.publication_barrier_generation =
      reserved_publication_barrier_generation;
  record.authority.publication_barrier_present = false;
  record.authority.lifecycle =
      MgaDmlUpdateStatementSavepointLifecycleV1::active;
  record.private_marker = DmlUpdateStatementPrivateSavepointMarker(
      record.authority.savepoint_uuid);
  record.journal_sequence = 1;
  record.cutoffs.row_event_sequence = NextRowEventSequence(context) - 1;
  record.cutoffs.metadata_event_sequence =
      NextMetadataEventSequence(context) - 1;
  record.cutoffs.index_event_sequence = NextIndexEventSequence(context) - 1;
  record.authority.durable_presence_sha256 =
      DmlUpdateStatementSavepointRecordSha256(record);
  if (record.authority.savepoint_uuid.empty() ||
      record.authority.publication_barrier_uuid.empty() ||
      record.private_marker.empty() ||
      !DmlUpdateStatementShaNonzero(
          record.authority.durable_presence_sha256)) {
    return DmlUpdateStatementSavepointFailure(
        "DML.UPDATE_FAILED",
        "sblr.dml_update_rows.statement_savepoint_create_failed",
        "engine_savepoint_identity_issue_failed");
  }
  if (!DmlUpdateStatementAppendBinaryRecord(context, record)) {
    return DmlUpdateStatementSavepointFailure(
        "DML.UPDATE_FAILED",
        "sblr.dml_update_rows.statement_savepoint_create_failed",
        "savepoint_create_durable_append_failed");
  }
  return DmlUpdateStatementLoadSavepointAuthority(
      context, binding, record.authority.savepoint_uuid,
      record.authority.savepoint_generation);
}

MgaDmlUpdateStatementSavepointAuthorityResultV1
RecoverMgaDmlUpdateStatementSavepointAuthorityV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateStatementSavepointBindingV1& binding,
    const std::string& savepoint_uuid,
    std::uint64_t savepoint_generation) {
  std::lock_guard<std::mutex> guard(
      DmlUpdateStatementSavepointJournalMutex());
  return DmlUpdateStatementLoadSavepointAuthority(
      context, binding, savepoint_uuid, savepoint_generation);
}

MgaDmlUpdateStatementSavepointAuthorityResultV1
RevalidateMgaDmlUpdateStatementSavepointAuthorityV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateStatementSavepointAuthorityV1& admitted) {
  std::lock_guard<std::mutex> guard(
      DmlUpdateStatementSavepointJournalMutex());
  auto current = DmlUpdateStatementLoadSavepointAuthority(
      context, admitted.binding, admitted.savepoint_uuid,
      admitted.savepoint_generation);
  if (!current.ok) return current;
  if (!DmlUpdateStatementAuthorityExact(current.authority, admitted)) {
    return DmlUpdateStatementSavepointFailure(
        "MGA.TRANSACTION.STALE",
        "sblr.dml_update_rows.statement_savepoint_stale",
        "admitted_savepoint_snapshot_not_current");
  }
  return current;
}

MgaDmlUpdateStatementSavepointAuthorityResultV1
RollbackMgaDmlUpdateStatementSavepointAuthorityV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateStatementSavepointAuthorityV1& admitted) {
  std::lock_guard<std::mutex> guard(
      DmlUpdateStatementSavepointJournalMutex());
  auto current = DmlUpdateStatementLoadSavepointAuthority(
      context, admitted.binding, admitted.savepoint_uuid,
      admitted.savepoint_generation);
  if (!current.ok) return current;
  if (!DmlUpdateStatementAuthorityExact(current.authority, admitted) ||
      current.authority.lifecycle !=
          MgaDmlUpdateStatementSavepointLifecycleV1::active) {
    return DmlUpdateStatementSavepointFailure(
        "MGA.TRANSACTION.STALE",
        "sblr.dml_update_rows.statement_savepoint_stale",
        "rollback_requires_current_active_savepoint");
  }
  const auto parsed = ParseSavepoints(context);
  const auto tx = parsed.active_savepoints.find(
      admitted.binding.owning_local_transaction_id);
  const std::string marker =
      DmlUpdateStatementPrivateSavepointMarker(admitted.savepoint_uuid);
  if (tx == parsed.active_savepoints.end() ||
      tx->second.find(marker) == tx->second.end()) {
    return DmlUpdateStatementSavepointFailure(
        "MGA.TRANSACTION.ROLLBACK_FAILED",
        "sblr.dml_update_rows.statement_savepoint_rollback_failed",
        "active_savepoint_marker_missing");
  }
  DmlUpdateStatementSavepointJournalRecordV1 record;
  record.authority = admitted;
  record.authority.lifecycle =
      MgaDmlUpdateStatementSavepointLifecycleV1::rolled_back;
  record.authority.publication_barrier_present = false;
  record.private_marker = marker;
  record.journal_sequence = 2;
  record.cutoffs = tx->second.at(marker);
  record.row_upper_event_sequence = NextRowEventSequence(context) - 1;
  record.metadata_upper_event_sequence =
      NextMetadataEventSequence(context) - 1;
  record.index_upper_event_sequence = NextIndexEventSequence(context) - 1;
  record.prior_record_sha256 = admitted.durable_presence_sha256;
  record.authority.durable_presence_sha256 =
      DmlUpdateStatementSavepointRecordSha256(record);
  if (!DmlUpdateStatementShaNonzero(
          record.authority.durable_presence_sha256) ||
      !DmlUpdateStatementAppendBinaryRecord(context, record)) {
    return DmlUpdateStatementSavepointFailure(
        "MGA.TRANSACTION.ROLLBACK_FAILED",
        "sblr.dml_update_rows.statement_savepoint_rollback_failed",
        "savepoint_rollback_durable_append_failed");
  }
  return DmlUpdateStatementLoadSavepointAuthority(
      context, admitted.binding, admitted.savepoint_uuid,
      admitted.savepoint_generation);
}

MgaDmlUpdateStatementSavepointAuthorityResultV1
ReleaseMgaDmlUpdateStatementSavepointAuthorityV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateStatementSavepointAuthorityV1& admitted) {
  std::lock_guard<std::mutex> guard(
      DmlUpdateStatementSavepointJournalMutex());
  auto current = DmlUpdateStatementLoadSavepointAuthority(
      context, admitted.binding, admitted.savepoint_uuid,
      admitted.savepoint_generation);
  if (!current.ok) return current;
  if (!DmlUpdateStatementAuthorityExact(current.authority, admitted) ||
      current.authority.lifecycle !=
          MgaDmlUpdateStatementSavepointLifecycleV1::active) {
    return DmlUpdateStatementSavepointFailure(
        "MGA.TRANSACTION.STALE",
        "sblr.dml_update_rows.statement_savepoint_stale",
        "release_requires_current_active_savepoint");
  }
  const auto parsed = ParseSavepoints(context);
  const auto tx = parsed.active_savepoints.find(
      admitted.binding.owning_local_transaction_id);
  const std::string marker =
      DmlUpdateStatementPrivateSavepointMarker(admitted.savepoint_uuid);
  if (tx == parsed.active_savepoints.end() ||
      tx->second.find(marker) == tx->second.end()) {
    return DmlUpdateStatementSavepointFailure(
        "DML.UPDATE_FAILED",
        "sblr.dml_update_rows.statement_savepoint_release_failed",
        "active_savepoint_marker_missing");
  }
  DmlUpdateStatementSavepointJournalRecordV1 record;
  record.authority = admitted;
  record.authority.lifecycle =
      MgaDmlUpdateStatementSavepointLifecycleV1::released;
  record.authority.publication_barrier_present = true;
  record.private_marker = marker;
  record.journal_sequence = 2;
  record.cutoffs = tx->second.at(marker);
  record.prior_record_sha256 = admitted.durable_presence_sha256;
  record.authority.durable_presence_sha256 =
      DmlUpdateStatementSavepointRecordSha256(record);
  // The publication barrier is the durable release append below.  Build the
  // complete success value before crossing it: after the append/fsync returns
  // success this function may only move already prepared state to its caller.
  // In particular, do not reload, decode, hash, or allocate from the durable
  // journal after publication.
  MgaDmlUpdateStatementSavepointAuthorityResultV1 success;
  success.ok = true;
  success.diagnostic = OkDiagnostic();
  success.authority = record.authority;
  if (record.authority.publication_barrier_uuid.empty() ||
      record.authority.publication_barrier_generation != 1 ||
      !DmlUpdateStatementShaNonzero(
          record.authority.durable_presence_sha256) ||
      !DmlUpdateStatementAppendBinaryRecord(context, record)) {
    return DmlUpdateStatementSavepointFailure(
        "DML.UPDATE_FAILED",
        "sblr.dml_update_rows.statement_savepoint_release_failed",
        "publication_barrier_durable_append_failed");
  }
  return success;
}


bool ApplyDmlUpdateBinarySavepointRecordsForStoreModule(
    const EngineRequestContext& context,
    SavepointParsedState* state,
    std::string* refusal_detail) {
  return ApplyDmlUpdateBinarySavepointRecords(context, state,
                                               refusal_detail);
}

}  // namespace scratchbird::engine::internal_api
