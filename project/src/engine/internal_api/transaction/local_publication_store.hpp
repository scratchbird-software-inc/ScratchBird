// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "transaction/local_commit_publication_codec.hpp"
#include "transaction/transaction_api.hpp"
#include "disk_device.hpp"
#include <filesystem>
#include <fstream>
#include <limits>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace scratchbird::engine::internal_api::local_publication_store {
// An atomically replaced, sorted collection of publication receipts. It is
// neither a redo log nor finality authority. No historical receipt is pruned.
inline constexpr std::string_view kMagic = "SBMPST03";
inline constexpr std::size_t kHeaderBytes = 72;
inline std::string Path(const EngineRequestContext& context) {
  return context.database_path + ".sb.mga_transaction_publication.v3";
}
inline bool Fail(std::string* detail, std::string value) {
  if (detail) *detail = std::move(value);
  return false;
}
inline bool ValidContext(const EngineRequestContext& context, std::string* detail) {
  return (!context.database_path.empty() && context.local_transaction_id != 0 &&
          core::uuid::IsEngineIdentityUuid(context.database_uuid) &&
          core::uuid::IsEngineIdentityUuid(context.transaction_uuid)) ||
      Fail(detail, "publication_store_context_identity_invalid");
}
inline auto Bytes(std::string_view value) {
  return std::span(reinterpret_cast<const std::uint8_t*>(value.data()), value.size());
}
inline bool ReadExact(std::istream& input, std::string* value, std::uint64_t size) {
  if (size > value->max_size() || size > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max())) return false;
  value->resize(static_cast<std::size_t>(size));
  input.read(value->data(), static_cast<std::streamsize>(size));
  return input.good();
}
inline bool LegacyAbsent(const EngineRequestContext& context, std::string* detail) {
  const std::filesystem::path database(context.database_path);
  const auto parent = database.parent_path().empty() ? std::filesystem::path(".") : database.parent_path();
  const auto prefix = database.filename().string() + ".sb.mga_transaction_publication.";
  std::error_code ec;
  for (std::filesystem::directory_iterator it(parent, ec), end; !ec && it != end; it.increment(ec)) {
    const auto name = it->path().filename().string();
    if (name.starts_with(prefix) && name.ends_with(".v2"))
      return Fail(detail, "legacy_publication_receipts_require_explicit_migration");
  }
  return !ec || Fail(detail, "publication_store_directory_unreadable");
}

// Only one record is materialized at a time, never the entire history. The
// checked header extent/count forbids silently accepting a truncated suffix.
template<class Visitor>
bool Visit(const EngineRequestContext& context, Visitor&& visitor,
           bool allow_missing, std::string* detail) {
  if (!LegacyAbsent(context, detail)) return false;
  const auto path = Path(context);
  std::error_code ec;
  const bool exists = std::filesystem::exists(path, ec);
  if (ec) return Fail(detail, "publication_store_status_failed");
  if (!exists) return allow_missing || Fail(detail, "publication_store_missing");
  const auto size = std::filesystem::file_size(path, ec);
  if (ec || size < kHeaderBytes) return Fail(detail, "publication_store_extent_invalid");
  std::ifstream input(path, std::ios::binary);
  std::string header;
  if (!ReadExact(input, &header, kHeaderBytes) || !header.starts_with(kMagic))
    return Fail(detail, "publication_store_header_invalid");
  const auto hash = core::hash::ComputeSha256Digest(Bytes(header.substr(0, 40)).data(), 40);
  if (!hash.ok() || !std::equal(hash.digest.begin(), hash.digest.end(),
                               reinterpret_cast<const core::platform::byte*>(header.data() + 40)) ||
      !std::equal(context.database_uuid.bytes.begin(), context.database_uuid.bytes.end(),
                  reinterpret_cast<const std::uint8_t*>(header.data() + 8)))
    return Fail(detail, "publication_store_header_identity_or_checksum_invalid");
  std::size_t cursor = 24;
  std::uint64_t count = 0, extent = 0;
  if (!ReadBinaryU64(Bytes(header), &cursor, &count) ||
      !ReadBinaryU64(Bytes(header), &cursor, &extent) || extent != size ||
      count == 0 || count > (size - kHeaderBytes) / 96)
    return Fail(detail, "publication_store_count_or_extent_invalid");
  std::uint64_t consumed = kHeaderBytes, previous = 0;
  for (std::uint64_t ordinal = 0; ordinal < count; ++ordinal) {
    std::string record_header, manifest;
    if (size - consumed < 32 || !ReadExact(input, &record_header, 32))
      return Fail(detail, "publication_store_record_header_truncated");
    consumed += 32;
    cursor = 0;
    std::uint64_t transaction = 0, width = 0;
    if (!ReadBinaryU64(Bytes(record_header), &cursor, &transaction) || transaction <= previous)
      return Fail(detail, "publication_store_transaction_order_invalid");
    auto owner = context;
    owner.local_transaction_id = transaction;
    std::copy_n(reinterpret_cast<const std::uint8_t*>(record_header.data() + 8), 16,
                owner.transaction_uuid.bytes.begin());
    cursor = 24;
    if (!core::uuid::IsEngineIdentityUuid(owner.transaction_uuid) ||
        !ReadBinaryU64(Bytes(record_header), &cursor, &width) || width < 64 || width > size - consumed ||
        !ReadExact(input, &manifest, width))
      return Fail(detail, "publication_store_record_extent_invalid");
    consumed += width;
    LocalCommitPublicationRecoveryResult decoded;
    if (!local_publication_codec::Decode(manifest, owner, &decoded))
      return Fail(detail, "publication_store_record_identity_or_checksum_invalid");
    if (!visitor(owner, manifest)) return false;
    previous = transaction;
  }
  if (consumed != size || input.peek() != std::char_traits<char>::eof())
    return Fail(detail, "publication_store_trailing_bytes");
  return true;
}

inline bool Read(const EngineRequestContext& context, std::string* output, std::string* detail) {
  if (!output || !ValidContext(context, detail)) return false;
  const auto guard = AcquireTransactionInventoryGuard(context.database_path);
  std::string selected;
  const auto visited = Visit(context, [&](const auto& owner, const auto& manifest) {
    if (owner.local_transaction_id == context.local_transaction_id) {
      if (owner.transaction_uuid != context.transaction_uuid)
        return Fail(detail, "publication_store_transaction_identity_mismatch");
      selected = manifest;
    }
    return true;
  }, false, detail);
  if (!visited) return false;
  if (selected.empty()) return Fail(detail, "publication_store_transaction_missing");
  *output = std::move(selected);
  return true;
}

inline bool Publish(const EngineRequestContext& context, std::string_view manifest,
                    std::string* detail) {
  if (!ValidContext(context, detail)) return false;
  const auto guard = AcquireTransactionInventoryGuard(context.database_path);
  LocalCommitPublicationRecoveryResult decoded;
  if (!local_publication_codec::Decode(manifest, context, &decoded))
    return Fail(detail, "publication_store_new_manifest_invalid");
  const std::filesystem::path destination(Path(context));
  const auto temporary = std::filesystem::path(destination.string() + ".tmp");
  // Only the exclusively opened database's regular staging file can be left
  // by an interrupted publication. Never follow a substituted symlink. The
  // exclusive create below also closes the status/open replacement race.
  std::error_code staging_error;
  const auto staging_status = std::filesystem::symlink_status(temporary, staging_error);
  if (staging_error && staging_error != std::errc::no_such_file_or_directory)
    return Fail(detail, "publication_store_temporary_status_failed");
  if (std::filesystem::exists(staging_status)) {
    if (!std::filesystem::is_regular_file(staging_status) ||
        !std::filesystem::remove(temporary, staging_error) || staging_error)
      return Fail(detail, "publication_store_temporary_not_regular_or_unremovable");
  }
  std::ofstream output(temporary, std::ios::binary | std::ios::noreplace);
  if (!output) return Fail(detail, "publication_store_temporary_open_failed");
  struct RemoveTemporary {
    std::filesystem::path path;
    std::ofstream& output;
    ~RemoveTemporary() {
      if (output.is_open()) output.close();
      std::error_code ec;
      std::filesystem::remove(path, ec);
    }
  } cleanup{temporary, output};
  const std::string placeholder(kHeaderBytes, '\0');
  output.write(placeholder.data(), placeholder.size());
  std::uint64_t count = 0, extent = kHeaderBytes;
  const auto write = [&](const EngineRequestContext& owner, std::string_view value) {
    if (extent > std::numeric_limits<std::uint64_t>::max() - 32 ||
        value.size() > std::numeric_limits<std::uint64_t>::max() - extent - 32 ||
        value.size() > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max()))
      return Fail(detail, "publication_store_extent_overflow");
    std::string record;
    AppendBinaryU64(&record, owner.local_transaction_id);
    record.append(reinterpret_cast<const char*>(owner.transaction_uuid.bytes.data()), 16);
    AppendBinaryU64(&record, value.size());
    output.write(record.data(), record.size());
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
    extent += record.size() + value.size();
    ++count;
    return output.good() || Fail(detail, "publication_store_record_write_failed");
  };
  bool inserted = false;
  if (!Visit(context, [&](const auto& owner, const auto& old_manifest) {
        if (!inserted && owner.local_transaction_id >= context.local_transaction_id) {
          if (owner.local_transaction_id == context.local_transaction_id &&
              owner.transaction_uuid != context.transaction_uuid)
            return Fail(detail, "publication_store_transaction_identity_mismatch");
          if (!write(context, manifest)) return false;
          inserted = true;
        }
        return owner.local_transaction_id == context.local_transaction_id || write(owner, old_manifest);
      }, true, detail)) return false;
  if (!inserted && !write(context, manifest)) return false;
  std::string header(kMagic);
  header.append(reinterpret_cast<const char*>(context.database_uuid.bytes.data()), 16);
  AppendBinaryU64(&header, count);
  AppendBinaryU64(&header, extent);
  const auto hash = core::hash::ComputeSha256Digest(Bytes(header).data(), header.size());
  if (!hash.ok()) return Fail(detail, "publication_store_header_hash_failed");
  header.append(reinterpret_cast<const char*>(hash.digest.data()), hash.digest.size());
  output.seekp(0);
  output.write(header.data(), header.size());
  output.close();
  if (!output) return Fail(detail, "publication_store_temporary_write_failed");
  if (!storage::disk::SyncFilesystemPath(temporary.string(), true).ok())
    return Fail(detail, "publication_store_temporary_sync_failed");
#if defined(_WIN32)
  if (!::MoveFileExW(temporary.wstring().c_str(), destination.wstring().c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    return Fail(detail, "publication_store_atomic_replace_failed");
#else
  std::error_code ec;
  std::filesystem::rename(temporary, destination, ec);
  if (ec) return Fail(detail, "publication_store_atomic_replace_failed:" + ec.message());
#endif
  if (!storage::disk::SyncParentDirectoryPath(destination.string()).ok())
    return Fail(detail, "publication_store_parent_sync_failed");
  return true;
}
}  // namespace scratchbird::engine::internal_api::local_publication_store
