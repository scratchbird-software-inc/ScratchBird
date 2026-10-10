// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "openpgp_seipd.hpp"

namespace scratchbird::core::crypto {
// RFC9580 section6 canonical MESSAGE armor: LF,64 columns, no metadata/CRC.
// Raw bytes are opaque; this codec performs no packet authentication or parsing.
PgpSize ArmorEncodedSize(std::size_t input_bytes) noexcept;
PgpSize ArmorDecodedSize(PgpInput, PgpCancellation = {});
// No allocation, I/O or charset conversion. Caller owns admitted immutable
// input and exclusive private output. Null-positive, overflowing or overlapping
// extents are rejected untouched. Output must match the exact size query.
// Once extent checks pass, every failure/exception erases all output. Probes
// are synchronous and may throw; they are polled in bounded slices and before
// success. Decode accepts a single matching RFC envelope, never bare base64.
PgpCode EncodeArmor(PgpInput, PgpOutput, PgpCancellation = {});
PgpCode DecodeArmor(PgpInput, PgpOutput, PgpCancellation = {});
} // namespace scratchbird::core::crypto
