// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_management_operation.hpp"
#include "native_common_page_header.hpp"

namespace scratchbird::storage::database {
struct NativeManagementExtentRoot {
  disk::NativePageReference first;
  Uuid object_uuid,operation_uuid;
  u64 revision=0;
  u32 aggregate_bytes=0,page_count=0;
  std::array<byte,32> aggregate_sha256{},first_page_sha256{};
  bool operator==(const NativeManagementExtentRoot&) const=default;
};
enum class NativeManagementExtentError {
  none,invalid_request,invalid_header,invalid_identity,invalid_extent,
  invalid_integrity,invalid_record,binding_mismatch,resource_exhausted,
  hash_failure,io_failure,bootstrap_failure,encrypted_requires_authority,
  cluster_requires_authority,invalid_workspace
};
struct NativeManagementExtentImage {
  NativeManagementExtentError error=NativeManagementExtentError::invalid_request;
  std::optional<NativeManagementExtentRoot> root;
  std::vector<std::vector<byte>> pages;
  bool ok() const noexcept{return error==NativeManagementExtentError::none&&root.has_value();}
};
struct NativeManagementExtentRead {
  NativeManagementExtentError error=NativeManagementExtentError::invalid_request;
  std::optional<NativeManagementOperation> record;
  std::vector<disk::NativeCommonPageHeader> page_headers;
  bool ok() const noexcept{return error==NativeManagementExtentError::none&&record.has_value();}
};
// Immutable physical representation only; no allocation, selection or writes.
NativeManagementExtentError ValidateNativeManagementExtentRoot(const NativeManagementExtentRoot&,
  const Uuid& database,const Uuid& bootstrap,u64 budget) noexcept;
NativeManagementExtentImage EncodeNativeManagementExtent(const NativeManagementOperation&,
  const Uuid& object_uuid,const std::vector<disk::NativeCommonPageHeader>&,u64 budget) noexcept;
NativeManagementExtentRead DecodeNativeManagementExtent(const std::vector<std::vector<byte>>&,
  const NativeManagementExtentRoot&,const Uuid& database,const Uuid& bootstrap,u64 budget) noexcept;
// Reads the complete referenced extent from the already-owned primary device.
// Does not grant selected-history authority, authenticate or verify effects.
NativeManagementExtentRead ReadNativeManagementExtentFromOpenDevice(
  const disk::NativeFilespaceDevice&,const NativeManagementExtentRoot&,
  const Uuid& database,const Uuid& bootstrap,u64 budget) noexcept;
struct NativeManagementExtentViewRead {
  NativeManagementExtentError error=NativeManagementExtentError::invalid_request;
  std::optional<NativeManagementOperationView> record;
  std::span<const disk::NativeCommonPageHeader> page_headers;
  std::span<const byte> aggregate;
  bool ok() const noexcept {return error==NativeManagementExtentError::none&&record.has_value();}
};
struct NativeManagementExtentViewWorkspace {
  std::span<byte> aggregate;
  std::span<disk::NativeCommonPageHeader> page_headers;
  std::span<NativeManagementStepView> steps;
  // Reused only after all page identities are verified. Needs at least
  // max(2*page_count+1,2*step_count+7) slots, compared as full binary UUIDs.
  std::span<Uuid> identities;
};
struct NativeManagementExtentEncoding {
  NativeManagementExtentError error=NativeManagementExtentError::invalid_request;
  std::optional<NativeManagementExtentRoot> root;
  // Ordered complete physical pages concatenated without gaps; exact prefix.
  std::span<const byte> pages;
  bool ok() const noexcept {return error==NativeManagementExtentError::none&&root.has_value();}
};
// Complete staged-image encoding only. Output, aggregate scratch and identity
// scratch must be mutually disjoint and excluded from ALL input backing.
// Identity capacity is max(2*page_count+1,2*step_count+7). No implicit heap,
// allocation authority, physical effect or durable publication is supplied.
NativeManagementExtentEncoding EncodeNativeManagementExtentInto(
  const NativeManagementOperationView&,const Uuid& object_uuid,
  std::span<const disk::NativeCommonPageHeader>,u64 budget,std::span<byte> output,
  std::span<byte> aggregate,std::span<Uuid> identities) noexcept;
// Complete immutable extent/aggregate inspection. All input page spans, their
// descriptor array, root, and caller arrays are disjoint. Aggregate and steps
// outlive the record; this is not an actual grant or publication/history proof.
NativeManagementExtentViewRead DecodeNativeManagementExtentInto(
  std::span<const std::span<const byte>>,const NativeManagementExtentRoot&,
  const Uuid& database,const Uuid& bootstrap,u64 budget,
  NativeManagementExtentViewWorkspace) noexcept;
} // namespace scratchbird::storage::database
