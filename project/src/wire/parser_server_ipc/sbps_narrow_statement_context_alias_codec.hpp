// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// Exact raw aliases for the private narrow-profile statement-context pair.
// This component never translates or reconstructs schema 7031/7032.  The
// result API accepts the complete canonical schema-7032-v71 source bytes and
// requires the schema-7710 bytes to be identical before publishing a copy.

#include "runtime_platform.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace scratchbird::parser::ipc {

using scratchbird::core::platform::byte;
using PsStatementContextUuidV1 = std::array<byte, 16>;

inline constexpr std::uint16_t kPsNarrowStatementContextRequestMessageV1 = 696;
inline constexpr std::uint16_t kPsNarrowStatementContextResultMessageV1 = 697;
inline constexpr std::uint32_t kPsNarrowStatementContextRequestSchemaV1 = 7709;
inline constexpr std::uint32_t kPsNarrowStatementContextResultSchemaV1 = 7710;
inline constexpr std::uint32_t kPsStatementContextSourceRequestSchemaV11 = 7031;
inline constexpr std::uint32_t kPsStatementContextSourceResultSchemaV11 = 7032;
inline constexpr std::uint16_t kPsStatementContextProjectionVersionV11 = 11;
inline constexpr std::uint16_t kPsStatementContextExtensionWireVersionV71 = 71;
inline constexpr std::size_t kPsStatementContextRequestBytesV11 = 42;
inline constexpr std::size_t kPsStatementContextExtensionPrefixBytesV71 = 260;
inline constexpr std::size_t kPsStatementContextDiagnosticRowBytesV71 = 72;
inline constexpr std::size_t kPsStatementContextTrailerBytesV71 = 776;
inline constexpr std::size_t kPsStatementContextTransactionHandleBytesV1 = 152;
inline constexpr std::uint64_t
    kPsStatementContextMinimumMgaDecodedBytesPerPass = 64ull * 1024ull;
inline constexpr std::uint64_t
    kPsStatementContextMaximumMgaDecodedBytesPerPass =
        1024ull * 1024ull * 1024ull * 1024ull;

enum class PsStatementContextAliasStatusV1 : std::uint8_t {
  ok = 0,
  invalid_argument,
  resource_limit_exceeded,
  source_schema_invalid,
  alias_bytes_mismatch,
  request_authority_mismatch,
};

struct PsStatementContextAliasDiagnosticV1 {
  PsStatementContextAliasStatusV1 status =
      PsStatementContextAliasStatusV1::invalid_argument;
  std::string diagnostic_code;
  std::string field;
  std::string detail;

  [[nodiscard]] bool ok() const {
    return status == PsStatementContextAliasStatusV1::ok;
  }
};

struct PsNarrowStatementContextRequestV1 {
  PsStatementContextUuidV1 session_uuid{};
  std::uint64_t owning_local_transaction_id = 0;
  PsStatementContextUuidV1 owning_transaction_uuid{};
};

struct PsNarrowStatementContextRequestValidationContextV1 {
  PsStatementContextUuidV1 expected_session_uuid{};
  std::uint64_t expected_owning_local_transaction_id = 0;
  PsStatementContextUuidV1 expected_owning_transaction_uuid{};
};

struct PsNarrowStatementContextRequestCodecResultV1 {
  PsStatementContextAliasDiagnosticV1 outcome;
  std::vector<byte> canonical_payload;
  PsNarrowStatementContextRequestV1 request;

  [[nodiscard]] bool ok() const { return outcome.ok(); }
};

struct PsNarrowStatementContextResultSummaryV1 {
  std::size_t extension_offset = 0;
  std::uint32_t diagnostic_identity_row_count = 0;
  std::uint64_t maximum_mga_relation_decoded_bytes_per_pass = 0;
  PsStatementContextUuidV1 preliminary_receipt_uuid{};
  PsStatementContextUuidV1 owning_transaction_uuid{};
  std::uint64_t owning_local_transaction_id = 0;
};

struct PsNarrowStatementContextResultAliasResultV1 {
  PsStatementContextAliasDiagnosticV1 outcome;
  std::vector<byte> canonical_payload;
  PsNarrowStatementContextResultSummaryV1 summary;

  [[nodiscard]] bool ok() const { return outcome.ok(); }
};

PsNarrowStatementContextRequestCodecResultV1
EncodeAndValidatePsNarrowStatementContextRequestV1(
    const PsNarrowStatementContextRequestV1& request,
    const PsNarrowStatementContextRequestValidationContextV1& context);

PsNarrowStatementContextRequestCodecResultV1
DecodeAndValidatePsNarrowStatementContextRequestV1(
    std::span<const byte> payload,
    const PsNarrowStatementContextRequestValidationContextV1& context);

// Proves that the schema-7709 payload is byte-identical to the already
// canonical schema-7031-v11 source and that both match the admitted outer
// session/transaction tuple.
PsNarrowStatementContextRequestCodecResultV1
ValidatePsNarrowStatementContextRequestAliasIdentityV1(
    std::span<const byte> schema7709_payload,
    std::span<const byte> canonical_schema7031_v11_payload,
    const PsNarrowStatementContextRequestValidationContextV1& context);

// `canonical_schema7032_v71_payload` must be the exact complete output of the
// authoritative schema-7032-v71 producer/validator.  This function then
// independently checks the v71 base/extension/trailer shape and refuses unless
// the schema-7710 payload is byte-for-byte identical.  No partial view is
// returned on failure.
PsNarrowStatementContextResultAliasResultV1
ValidateAndAdoptPsNarrowStatementContextResultAliasV1(
    std::span<const byte> schema7710_payload,
    std::span<const byte> canonical_schema7032_v71_payload);

const char* PsStatementContextAliasStatusNameV1(
    PsStatementContextAliasStatusV1 status);

}  // namespace scratchbird::parser::ipc
