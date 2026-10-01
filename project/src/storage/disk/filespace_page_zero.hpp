// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "filespace_bootstrap.hpp"
#include <vector>

namespace scratchbird::storage::disk {
using scratchbird::core::platform::u64;
// NATIVE_FILESPACE_PAGE_ZERO_V1; never a prototype PageType enum cast.
struct FilespaceRootReference {
  u16 kind = 0;
  u32 page_type = 0;
  Uuid filespace_uuid;
  u64 page_number = 0;
  u64 page_generation = 0;
  Uuid page_size_profile_uuid;
  Uuid object_uuid;
};
struct FilespacePageZero {
  FilespaceBootstrap bootstrap;
  Uuid page_uuid;
  Uuid creation_operation_uuid;
  Uuid writer_identity_uuid;
  u64 page_generation = 0;
  u64 root_set_generation = 0;
  u64 total_pages = 0;
  u64 free_pages = 0;
  u64 preallocated_pages = 0;
  u64 creation_utc_millis = 0;
  std::vector<FilespaceRootReference> roots;
};
enum class FilespacePageZeroError {
  none, invalid_bootstrap, invalid_common_header, invalid_family,
  integrity_mismatch, hash_provider_failure, invalid_capacity,
  invalid_root_directory, required_root_missing, probe_changed,
  device_not_open, io_failure, resource_exhausted
};
enum class FilespaceRecoveryRootError {
  none, invalid_request, invalid_bootstrap, invalid_directory, invalid_extent,
  encrypted_requires_authority, cluster_requires_authority, changed_observation,
  io_failure, hash_failure, resource_exhausted
};
struct FilespaceRecoveryRootCandidates {
  FilespaceRecoveryRootError error=FilespaceRecoveryRootError::invalid_request;
  FilespacePageZeroError image_error=FilespacePageZeroError::none;
  core::platform::DiagnosticRecord diagnostic;
  std::optional<FilespaceBootstrap> bootstrap;
  std::vector<FilespaceRootReference> roots;
  u64 observed_size_bytes=0;
  bool ok() const noexcept {return error==FilespaceRecoveryRootError::none&&bootstrap.has_value()&&!roots.empty();}
};
// UNTRUSTED recovery locations only. Does not validate the mutable page-zero
// body or grant authority to repair it. The owner must bind actual durable
// watermark/plan/original images before effects. Never used for ordinary open.
FilespaceRecoveryRootCandidates ProbeFilespaceRecoveryRootCandidatesFromOpenDevice(
    FileDevice&,const FilespaceBootstrapBinding&,u64 maximum_verification_image_bytes) noexcept;
struct FilespacePageZeroDecodeResult {
  FilespacePageZeroError error = FilespacePageZeroError::invalid_family;
  std::optional<FilespacePageZero> record;
  bool ok() const noexcept { return error == FilespacePageZeroError::none && record.has_value(); }
};
struct FilespacePageZeroEncodeResult {
  FilespacePageZeroError error = FilespacePageZeroError::invalid_family;
  std::optional<std::vector<byte>> bytes;
  bool ok() const noexcept { return error == FilespacePageZeroError::none && bytes.has_value(); }
};
u32 CanonicalPageZeroRootPageType(u16 root_kind) noexcept;
FilespacePageZeroEncodeResult EncodeFilespacePageZero(const FilespacePageZero&) noexcept;
FilespacePageZeroDecodeResult DecodeFilespacePageZero(
    const byte*, std::size_t, const FilespaceBootstrapBinding* expected = nullptr) noexcept;
// Complete image/metadata validation, not target-root resolution or serving
// admission. The caller must verify actual roots and MGA recovery authority.
FilespacePageZeroDecodeResult ReadFilespacePageZeroFromOpenDevice(
    FileDevice&, const FilespaceBootstrapBinding* expected = nullptr) noexcept;

enum class FilespaceExtentRelation { unknown, matching, shorter, longer };
struct FilespacePageZeroRecoveryObservation {
  FilespacePageZeroError error=FilespacePageZeroError::invalid_family;
  std::optional<FilespacePageZero> record;
  u64 declared_bytes=0, observed_bytes=0, complete_pages=0, trailing_bytes=0;
  FilespaceExtentRelation relation=FilespaceExtentRelation::unknown;
  bool ok() const noexcept {return error==FilespacePageZeroError::none&&record.has_value();}
};
// Read-only recovery evidence, NOT ordinary open/capacity or effect admission.
// A successful observation can describe a short, excess or unaligned file.
// Root references are declared metadata, not proof of their physical backing.
FilespacePageZeroRecoveryObservation ObserveFilespacePageZeroForRecoveryFromOpenDevice(
    FileDevice&, const FilespaceBootstrapBinding& expected) noexcept;

enum class FilespacePageZeroBodyError {
  none, invalid_request, invalid_device, image_failure, invalid_transition,
  extent_mismatch, preimage_changed, readback_mismatch, io_failure,
  hash_failure, resource_exhausted
};
struct FilespacePageZeroBodyResult {
  FilespacePageZeroBodyError error=FilespacePageZeroBodyError::invalid_request;
  FilespacePageZeroError image_error=FilespacePageZeroError::none;
  FilespacePageZeroError observed_body_error=FilespacePageZeroError::none;
  core::platform::DiagnosticRecord diagnostic;
  bool original_preimage_verified=false, target_already_present=false;
  bool damaged_body_observed=false, write_attempted=false, uncertain_write=false;
  u64 confirmed_bytes=0, observed_size_bytes=0;
  bool sync_attempted=false, sync_completed=false, postimage_verified=false;
  bool ok() const noexcept {
    return error==FilespacePageZeroBodyError::none&&sync_completed&&postimage_verified;
  }
};
// Trusted physical actuator only. The owning native publisher must durably
// retain/admit the original operation and both exact images BEFORE growth.
// Does not extend storage, grant capacity, select a checkpoint or complete MGA.
// Writes only [4096,4480), preserving bootstrap and recovery-root locations.
FilespacePageZeroBodyResult WriteFilespacePageZeroGrowthBodyFromOpenDevice(
    FileDevice&, const FilespaceBootstrapBinding&, const std::vector<byte>& before,
    const std::vector<byte>& after, u64 maximum_verification_image_bytes) noexcept;
// Explicit original-operation repair only, not ordinary-open auto-repair.
// Invalid mutable bytes may be repaired; immutable damage and unexpected valid
// metadata refuse. Caller-selected images are not proof of recovery authority.
FilespacePageZeroBodyResult RepairFilespacePageZeroGrowthBodyFromOpenDevice(
    FileDevice&, const FilespaceBootstrapBinding&, const std::vector<byte>& before,
    const std::vector<byte>& after, u64 maximum_verification_image_bytes) noexcept;
}  // namespace scratchbird::storage::disk
