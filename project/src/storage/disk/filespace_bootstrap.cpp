// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "filespace_bootstrap.hpp"
#include "disk_device.hpp"
#include <openssl/evp.h>
#include <algorithm>
#include <new>
#include <stdexcept>

namespace scratchbird::storage::disk {
namespace {
using Error = FilespaceBootstrapError;
using namespace scratchbird::core::platform;
bool IsV7(const Uuid& id) noexcept {
  return (id.bytes[6] & 0xf0u) == 0x70u && (id.bytes[8] & 0xc0u) == 0x80u;
}
bool Equal(const Uuid& a, const Uuid& b) noexcept { return a.bytes == b.bytes; }
bool Nil(const Uuid& id) noexcept {
  return std::all_of(id.bytes.begin(), id.bytes.end(), [](byte b) { return b == 0; });
}
Uuid ReadUuid(const byte* bytes) noexcept {
  Uuid id;
  std::copy_n(bytes, id.bytes.size(), id.bytes.begin());
  return id;
}
void WriteUuid(byte* bytes, const Uuid& id) noexcept {
  std::copy(id.bytes.begin(), id.bytes.end(), bytes);
}
bool Digest(const byte* bytes, std::array<byte, 32>& digest) noexcept {
  unsigned length = 0;
  return EVP_Digest(bytes, 104, digest.data(), &length, EVP_sha256(), nullptr) == 1
      && length == digest.size();
}
FilespaceBootstrapDecodeResult Failed(Error error) noexcept { return {error, std::nullopt}; }
}  // namespace

const CanonicalFilespacePageProfile* FindCanonicalFilespacePageProfile(const Uuid& id) noexcept {
  for (const auto& profile : kCanonicalFilespacePageProfiles)
    if (Equal(profile.uuid, id)) return &profile;
  return nullptr;
}
const CanonicalFilespacePageProfile* FindCanonicalFilespacePageProfileForSize(u32 size) noexcept {
  for (const auto& profile : kCanonicalFilespacePageProfiles)
    if (profile.page_size_bytes == size) return &profile;
  return nullptr;
}
Error ValidateFilespaceBootstrap(const FilespaceBootstrap& value,
                                const FilespaceBootstrapBinding* expected) noexcept {
  if (!IsV7(value.database_uuid) || !IsV7(value.filespace_uuid)) return Error::invalid_identity;
  const auto* profile = FindCanonicalFilespacePageProfile(value.page_size_profile_uuid);
  if (!profile || profile->page_size_bytes != value.page_size_bytes)
    return Error::unsupported_page_profile;
  if (value.durable_format_generation != 1 || value.durable_format_generation != profile->layout_generation)
    return Error::unsupported_format_generation;
  if ((value.flags & ~u32{3}) != 0) return Error::unknown_flags;
  if (!Equal(value.checksum_profile_uuid, kNativeBootstrapIntegrityProfile))
    return Error::unsupported_checksum_profile;
  if (value.filespace_role < 1 || value.filespace_role > 14) return Error::invalid_role;
  if (value.lifecycle_state < 1 || value.lifecycle_state > 15) return Error::invalid_state;
  if ((value.flags & FilespaceBootstrapFlag::payload_encrypted) != 0) {
    if (!IsV7(value.encryption_profile_uuid)) return Error::invalid_encryption_profile;
  } else if (!Nil(value.encryption_profile_uuid)) return Error::invalid_encryption_profile;
  if (expected && (!IsV7(expected->database_uuid) || !IsV7(expected->filespace_uuid)
      || !Equal(expected->database_uuid, value.database_uuid)
      || !Equal(expected->filespace_uuid, value.filespace_uuid)
      || !Equal(expected->page_size_profile_uuid, value.page_size_profile_uuid)))
    return Error::binding_mismatch;
  return Error::none;
}

FilespaceBootstrapEncodeResult EncodeFilespaceBootstrap(const FilespaceBootstrap& value) noexcept {
  const auto error = ValidateFilespaceBootstrap(value);
  if (error != Error::none) return {error, std::nullopt};
  SerializedFilespaceBootstrap bytes{};
  bytes[0] = 'S'; bytes[1] = 'B'; bytes[2] = 'F'; bytes[3] = 'P';
  StoreLittle16(bytes.data() + 4, 1);
  StoreLittle16(bytes.data() + 6, kFilespaceBootstrapBytes);
  StoreLittle32(bytes.data() + 8, value.page_size_bytes);
  StoreLittle32(bytes.data() + 12, value.flags);
  WriteUuid(bytes.data() + 16, value.database_uuid);
  WriteUuid(bytes.data() + 32, value.filespace_uuid);
  WriteUuid(bytes.data() + 48, value.page_size_profile_uuid);
  StoreLittle32(bytes.data() + 64, value.durable_format_generation);
  StoreLittle16(bytes.data() + 68, value.filespace_role);
  StoreLittle16(bytes.data() + 70, value.lifecycle_state);
  WriteUuid(bytes.data() + 72, value.checksum_profile_uuid);
  WriteUuid(bytes.data() + 88, value.encryption_profile_uuid);
  std::array<byte, 32> digest{};
  if (!Digest(bytes.data(), digest)) return {Error::hash_provider_failure, std::nullopt};
  std::copy(digest.begin(), digest.end(), bytes.begin() + 104);
  return {Error::none, bytes};
}

FilespaceBootstrapDecodeResult DecodeFilespaceBootstrap(
    const byte* bytes, std::size_t size, const FilespaceBootstrapBinding* expected) noexcept {
  if (!bytes || size != kFilespaceBootstrapBytes || bytes[0] != 'S' || bytes[1] != 'B'
      || bytes[2] != 'F' || bytes[3] != 'P' || LoadLittle16(bytes + 4) != 1
      || LoadLittle16(bytes + 6) != kFilespaceBootstrapBytes)
    return Failed(Error::invalid_framing);
  if (std::any_of(bytes + 136, bytes + size, [](byte b) { return b != 0; }))
    return Failed(Error::reserved_nonzero);
  std::array<byte, 32> digest{};
  if (!Digest(bytes, digest)) return Failed(Error::hash_provider_failure);
  if (!std::equal(digest.begin(), digest.end(), bytes + 104)) return Failed(Error::digest_mismatch);
  FilespaceBootstrap value;
  value.database_uuid = ReadUuid(bytes + 16);
  value.filespace_uuid = ReadUuid(bytes + 32);
  value.page_size_profile_uuid = ReadUuid(bytes + 48);
  value.checksum_profile_uuid = ReadUuid(bytes + 72);
  value.encryption_profile_uuid = ReadUuid(bytes + 88);
  value.page_size_bytes = LoadLittle32(bytes + 8);
  value.flags = LoadLittle32(bytes + 12);
  value.durable_format_generation = LoadLittle32(bytes + 64);
  value.filespace_role = LoadLittle16(bytes + 68);
  value.lifecycle_state = LoadLittle16(bytes + 70);
  const auto error = ValidateFilespaceBootstrap(value, expected);
  if (error != Error::none) return Failed(error);
  return {Error::none, value};
}

FilespaceBootstrapDecodeResult ReadFilespaceBootstrapFromOpenDevice(
    FileDevice& device, const FilespaceBootstrapBinding* expected) noexcept {
  try {
    if (!device.is_open()) return Failed(Error::device_not_open);
    SerializedFilespaceBootstrap bytes{};
    const auto read = device.ReadAt(0, bytes.data(), bytes.size());
    if (!read.ok() || read.bytes_transferred != bytes.size()) return Failed(Error::io_failure);
    return DecodeFilespaceBootstrap(bytes.data(), bytes.size(), expected);
  } catch (const std::bad_alloc&) {
    return Failed(Error::resource_exhausted);
  } catch (const std::length_error&) {
    return Failed(Error::resource_exhausted);
  } catch (...) {
    return Failed(Error::io_failure);
  }
}
}  // namespace scratchbird::storage::disk
