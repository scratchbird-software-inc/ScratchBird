// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-WIRE-TYPED-RESULT-TRANSPORT-CODEC-ANCHOR
//
// Implements PS-RESULT-TRANSPORT-BINARY-V1 and its shared column validation.
// Codec conformance alone does not establish ABI, IPC, or SBWP adoption. Outer
// SBPS/session or engine-result authority is supplied by the carrier binding;
// inner SHA-256 values are deterministic evidence, never authentication keys.

#include "datatype_descriptor.hpp"
#include "runtime_platform.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::wire {

using scratchbird::core::datatypes::CanonicalTypeId;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

inline constexpr u16 kTypedResultTransportVersion = 1;
inline constexpr u16 kTypedResultRowDescriptorHeaderBytes = 128;
inline constexpr u32 kTypedResultColumnDescriptorPrefixBytes = 92;
inline constexpr u16 kTypedResultBatchHeaderBytes = 224;
inline constexpr u32 kTypedResultEvidenceHashBytes = 32;

using TypedResultUuid = std::array<byte, 16>;
using TypedResultEvidenceHash =
    std::array<byte, kTypedResultEvidenceHashBytes>;

// Shared by the byte codec and engine schema publication. Empty labels are
// legal; otherwise require at most 4096 bytes of strict UTF-8 without NUL.
bool ValidTypedResultColumnName(std::string_view name);

enum class TypedResultNullability : std::uint8_t {
  not_null = 0,
  nullable = 1,
  unknown = 2,
};

enum class TypedResultValueState : std::uint8_t {
  value_present = 0,
  sql_null = 1,
};

enum class TypedResultCodecStatus : std::uint8_t {
  ok = 0,
  invalid_argument,
  malformed_frame,
  unsupported_version,
  evidence_mismatch,
  resource_limit_exceeded,
  descriptor_invalid,
  descriptor_mismatch,
  cursor_mismatch,
  sequence_mismatch,
  shape_invalid,
  value_invalid,
};

struct TypedResultColumnDescriptor {
  // Ordinals are zero-based and contiguous.  name_occurrence is the zero-based
  // occurrence of name among earlier columns, so duplicate display names remain
  // unambiguous without changing their client-visible spelling.
  u32 ordinal = 0;
  u32 name_occurrence = 0;
  std::string name;
  TypedResultNullability nullability = TypedResultNullability::unknown;

  TypedResultUuid descriptor_uuid{};
  u64 descriptor_generation = 0;
  TypedResultUuid type_uuid{};
  u64 type_generation = 0;
  CanonicalTypeId canonical_type_id = CanonicalTypeId::unknown;

  std::string codec_id;
  u16 codec_version = 0;
  u64 codec_generation = 0;
  // Exact canonical payload width for fixed-width codecs, or zero for a
  // descriptor-bounded variable-width codec.
  u32 canonical_value_bytes = 0;
};

struct TypedResultRowDescriptor {
  TypedResultUuid descriptor_uuid{};
  u64 descriptor_generation = 0;

  TypedResultUuid datatype_catalog_snapshot_uuid{};
  u64 datatype_catalog_generation = 0;
  u64 datatype_registry_generation = 0;

  std::vector<TypedResultColumnDescriptor> columns;
  TypedResultEvidenceHash descriptor_evidence_sha256{};
};

struct TypedResultCell {
  u32 column_ordinal = 0;
  u32 name_occurrence = 0;
  TypedResultValueState state = TypedResultValueState::value_present;
  // Canonical datatype payload bytes, not a rendered string.  The codec wraps
  // these bytes with the existing DatatypeBinaryValue envelope.
  std::vector<byte> canonical_payload;
};

struct TypedResultRow {
  u64 row_ordinal = 0;
  std::vector<TypedResultCell> cells;
};

struct TypedResultBatch {
  TypedResultUuid execution_uuid{};
  TypedResultUuid result_set_uuid{};
  TypedResultUuid batch_uuid{};
  u64 batch_ordinal = 0;
  bool end_of_rowset = false;
  bool cursor_bound = false;

  TypedResultUuid row_descriptor_uuid{};
  u64 row_descriptor_generation = 0;
  TypedResultEvidenceHash descriptor_evidence_sha256{};
  TypedResultUuid snapshot_uuid{};
  TypedResultUuid cursor_uuid{};
  TypedResultEvidenceHash batch_evidence_sha256{};
  std::vector<TypedResultRow> rows;
};

enum class TypedResultCarrierKind : std::uint8_t {
  public_engine_abi = 0,
  ps_execute_result_v1 = 1,
  ps_fetch_result_v1 = 2,
};

// The active carrier supplies these already-validated outer facts. The codec
// checks their exact agreement with the packet; it does not authenticate them.
struct TypedResultCarrierBinding {
  TypedResultCarrierKind kind = TypedResultCarrierKind::public_engine_abi;
  u64 row_count = 0;
  bool end_of_rowset = false;
  TypedResultUuid execution_uuid{};
  TypedResultUuid result_set_uuid{};
  TypedResultUuid snapshot_uuid{};
  TypedResultUuid cursor_uuid{};
  TypedResultUuid cursor_stream_descriptor_uuid{};
  u16 cursor_stream_descriptor_version = 0;
  u64 cursor_stream_descriptor_generation = 0;
};

// Optional state for decoding a cursor sequence.  A successful first decode
// fixes the cursor and descriptor identities.  Later batches must be exactly
// contiguous and may not follow a terminal batch.  The state is not modified
// on refusal.
struct TypedResultCursorBatchState {
  bool initialized = false;
  bool terminal = false;
  TypedResultUuid cursor_uuid{};
  TypedResultUuid execution_uuid{};
  TypedResultUuid result_set_uuid{};
  TypedResultUuid snapshot_uuid{};
  TypedResultUuid cursor_stream_descriptor_uuid{};
  u16 cursor_stream_descriptor_version = 0;
  u64 cursor_stream_descriptor_generation = 0;
  TypedResultUuid row_descriptor_uuid{};
  u64 row_descriptor_generation = 0;
  TypedResultEvidenceHash descriptor_evidence_sha256{};
  u64 next_batch_ordinal = 0;
  std::vector<TypedResultUuid> seen_batch_uuids;
};

struct TypedResultDescriptorCodecResult {
  TypedResultCodecStatus status = TypedResultCodecStatus::ok;
  std::string diagnostic_code;
  std::string detail;
  std::vector<byte> encoded;
  TypedResultRowDescriptor descriptor;

  bool ok() const { return status == TypedResultCodecStatus::ok; }
};

struct TypedResultBatchCodecResult {
  TypedResultCodecStatus status = TypedResultCodecStatus::ok;
  std::string diagnostic_code;
  std::string detail;
  std::vector<byte> encoded;
  TypedResultBatch batch;

  bool ok() const { return status == TypedResultCodecStatus::ok; }
};

TypedResultDescriptorCodecResult EncodeTypedResultRowDescriptor(
    const TypedResultRowDescriptor& descriptor);

// Component-byte verification is deliberately separate from catalog access.
// Before publishing the returned descriptor, the owning route must resolve
// each exact catalog/registry/descriptor tuple and compare every returned
// type/codec field under the admitted session snapshot.
TypedResultDescriptorCodecResult DecodeTypedResultRowDescriptor(
    const std::vector<byte>& encoded);

TypedResultBatchCodecResult EncodeTypedResultBatch(
    const TypedResultBatch& batch,
    const TypedResultRowDescriptor& descriptor,
    const TypedResultCarrierBinding& carrier_binding,
    // Exact complete packet ceiling, including every row/cell/envelope header
    // and the fixed evidence area. Zero permits no nonempty packet. A larger
    // caller value cannot widen the transport's 16 MiB maximum. Admission
    // precedes descriptor/value copying and packet allocation; this byte limit
    // is not a physical memory reservation or descriptor authority.
    u64 maximum_bytes = 16ull * 1024ull * 1024ull);

// Consuming overload: validation and packet allocation finish before batch
// ownership moves. Refusal or an allocation exception leaves the input intact.
// Success retains the original row/cell/payload storage with verified evidence;
// it does not create a physical grant or commit a producer/cursor transition.
TypedResultBatchCodecResult EncodeTypedResultBatch(
    TypedResultBatch&& batch,
    const TypedResultRowDescriptor& descriptor,
    const TypedResultCarrierBinding& carrier_binding,
    u64 maximum_bytes = 16ull * 1024ull * 1024ull);

// Carrier facts must come from the already-admitted SBPS result/fetch or
// public-engine result handle; packet bytes cannot create the binding.
TypedResultBatchCodecResult DecodeTypedResultBatch(
    const std::vector<byte>& encoded,
    const TypedResultRowDescriptor& expected_descriptor,
    const TypedResultCarrierBinding& carrier_binding,
    TypedResultCursorBatchState* cursor_state = nullptr);

const char* TypedResultCodecStatusName(TypedResultCodecStatus status);

}  // namespace scratchbird::wire
