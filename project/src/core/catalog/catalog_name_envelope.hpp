// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_name_record_codec.hpp"

namespace scratchbird::core::catalog {
// CATALOG_NAME_RESIDENT_ENVELOPE_V1. This locator references the owning full
// MGA version header. It does not replace inventory, visibility or publication.
struct CatalogNameVersionBinding {
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  TypedUuid row_uuid;
  TypedUuid version_uuid;
  TypedUuid catalog_object_uuid;
  TypedUuid creating_transaction_uuid;
  u64 page_id = 0;
  u32 slot_id = 0;
  u64 storage_generation = 0;
  u64 version_sequence = 0;
  u64 creating_transaction_number = 0;
  u64 catalog_generation = 0;
};
using CatalogNamePayload = std::variant<CatalogNameVector, CatalogNameEntry>;
struct CatalogNameEnvelope {
  CatalogNameVersionBinding binding;
  CatalogNamePayload payload;
};
enum class CatalogNameEnvelopeError : u8 {
  none, invalid_header, unsupported_format, invalid_identity, invalid_payload,
  binding_mismatch, size_limit
};
struct CatalogNameEnvelopeEncodeResult {
  CatalogNameEnvelopeError error = CatalogNameEnvelopeError::none;
  std::vector<byte> bytes;
  bool ok() const { return error == CatalogNameEnvelopeError::none && !bytes.empty(); }
};
struct CatalogNameEnvelopeDecodeResult {
  CatalogNameEnvelopeError error = CatalogNameEnvelopeError::none;
  std::optional<CatalogNameEnvelope> record;
  bool ok() const { return error == CatalogNameEnvelopeError::none && record.has_value(); }
};
CatalogNameEnvelopeEncodeResult EncodeCatalogNameEnvelope(const CatalogNameEnvelope& record);
// expected must come from owning MGA/storage authority, never copied from
// untrusted envelope bytes to manufacture agreement.
CatalogNameEnvelopeDecodeResult DecodeCatalogNameEnvelope(
    const std::vector<byte>& bytes, const CatalogNameVersionBinding& expected);
}  // namespace scratchbird::core::catalog
