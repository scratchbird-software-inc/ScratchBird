// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "sblr_source_map_descriptor_registry.hpp"
#include "api_diagnostics.hpp"
#include "hash_digest.hpp"
#include "storage/disk/disk_device.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <limits>
#include <map>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace scratchbird::engine::internal_api {
namespace {
using Bytes = std::vector<std::uint8_t>;
using Sha = SblrSourceMapHashV2;
using Snapshot = SblrSourceMapDescriptorSnapshotV1;
using Result = SblrSourceMapRegistryResultV1;
namespace disk = scratchbird::storage::disk;
constexpr std::size_t kHeader = 128, kRecordHeader = 264;
constexpr std::size_t kMaximumBytes = 64U * 1024U * 1024U;
constexpr std::size_t kMaximumSmvd = 426136;
constexpr std::string_view kMagic{"SBSMR2\0\0", 8};
constexpr std::string_view kSnapshotDomain = "ScratchBird.SourceMapRegistrySnapshot.V2";
constexpr std::string_view kRecordDomain = "ScratchBird.SourceMapRegistryRecord.V2";
std::mutex registry_mutex; // Synchronization only; no cross-node cached state.
std::atomic<std::uint64_t> temporary_ordinal{0};
struct Failure { const char* code; const char* key; };
[[noreturn]] void Hidden() { throw Failure{"SECURITY.ACCESS_DENIED", "sblr.source_map.hidden"}; }
[[noreturn]] void Stale() { throw Failure{"SBLR.SOURCE_MAP.STALE", "sblr.source_map.registry_corrupt"}; }
[[noreturn]] void Invalid() { throw Failure{"SBLR.OPERAND_INVALID", "sblr.source_map.binding_invalid"}; }
[[noreturn]] void Io() { throw Failure{"SBLR.EXECUTION_FAILED", "sblr.source_map.publication_unconfirmed"}; }
[[noreturn]] void Limit() { throw Failure{"RESOURCE.BUDGET_EXCEEDED", "sblr.source_map.registry_limit"}; }
bool Valid(const EngineUuid& id) { return core::uuid::IsEngineIdentityUuid(id); }
bool Nonzero(const Sha& hash) {
  return std::any_of(hash.begin(), hash.end(), [](auto b) { return b != 0; });
}
EngineUuid NewUuid() {
  const auto id = core::uuid::IssueRuntimeIdentityV7();
  if (!id || !Valid(*id)) Io();
  return *id;
}
void Receipt(const EngineRequestContext& c, const EngineUuid& receipt) {
  if (!c.security_context_present || !c.statement_metadata_snapshot_engine_owned ||
      !Valid(c.database_uuid) || !Valid(c.principal_uuid) || !Valid(c.session_uuid) ||
      !Valid(receipt) || c.statement_receipt_uuid != receipt ||
      (!c.transaction_uuid.is_nil() && !Valid(c.transaction_uuid))) Hidden();
}
void Owner(const EngineRequestContext& c, const EngineUuid& receipt, const Snapshot& s) {
  if (s.database_uuid != c.database_uuid || s.principal_uuid != c.principal_uuid ||
      s.session_uuid != c.session_uuid || s.statement_receipt_uuid != receipt ||
      s.transaction_uuid != c.transaction_uuid) Hidden();
}
Result Success() {
  Result out;
  out.diagnostic = MakeEngineApiDiagnostic("OK", "ok", {}, false);
  out.ok = true;
  return out;
}
Result Error(const char* code, const char* key) {
  Result out;
  out.diagnostic = MakeEngineApiDiagnostic(code, key, {});
  return out;
}
template<class F> Result Run(F&& work) {
  try {
    try {
      std::lock_guard lock(registry_mutex);
      return work();
    } catch (const Failure& error) {
      return Error(error.code, error.key);
    }
  } catch (const std::bad_alloc&) {
    return Error("RESOURCE.BUDGET_EXCEEDED", "sblr.source_map.allocation_failed");
  } catch (const std::length_error&) {
    return Error("RESOURCE.BUDGET_EXCEEDED", "sblr.source_map.extent_failed");
  } catch (const std::filesystem::filesystem_error&) {
    return Error("SBLR.EXECUTION_FAILED", "sblr.source_map.filesystem_failed");
  }
}
static_assert(std::is_nothrow_move_constructible_v<Result>);
std::uint64_t Get(const std::uint8_t* p, std::size_t at, unsigned width) {
  std::uint64_t n = 0;
  for (unsigned i = 0; i < width; ++i) n |= std::uint64_t(p[at+i]) << (8*i);
  return n;
}
void Put(Bytes& out, std::size_t at, std::uint64_t n, unsigned width) {
  for (unsigned i = 0; i < width; ++i) out[at+i] = static_cast<std::uint8_t>(n >> (8*i));
}
template<class A> void Copy(Bytes& out, std::size_t at, const A& value) {
  std::copy(value.begin(), value.end(), out.begin()+at);
}
template<class A> void Read(const std::uint8_t* p, A& out) {
  std::copy_n(p, out.size(), out.begin());
}
Sha Hash(std::string_view domain, std::span<const std::uint8_t> prefix,
         std::span<const std::uint8_t> body) {
  Bytes bytes;
  if (prefix.size() > kMaximumBytes || body.size() > kMaximumBytes-prefix.size()) Limit();
  bytes.reserve(domain.size()+prefix.size()+body.size());
  bytes.insert(bytes.end(), domain.begin(), domain.end());
  bytes.insert(bytes.end(), prefix.begin(), prefix.end());
  bytes.insert(bytes.end(), body.begin(), body.end());
  const auto digest = core::hash::ComputeSha256Digest(bytes);
  if (!digest.ok() || digest.digest_bytes != 32 || !Nonzero(digest.digest)) Io();
  return digest.digest;
}
void Validate(const Snapshot& s) {
  for (const auto& id : {s.descriptor_uuid, s.registry_snapshot_uuid,
                        s.statement_receipt_uuid, s.database_uuid, s.session_uuid,
                        s.principal_uuid, s.publication_uuid})
    if (!Valid(id)) Stale();
  if ((!s.transaction_uuid.is_nil() && !Valid(s.transaction_uuid)) ||
      s.descriptor_uuid == s.publication_uuid || s.descriptor_generation != 1 ||
      !s.registry_generation || !s.publication_generation ||
      (s.lifecycle != SblrSourceMapLifecycleV1::active &&
       s.lifecycle != SblrSourceMapLifecycleV1::revoked) ||
      s.canonical_smvd.size() > kMaximumSmvd) Stale();
  const auto decoded = engine::sblr::DecodeSblrSourceMapDescriptorVectorV1(
      s.canonical_smvd.data(), s.canonical_smvd.size());
  if (decoded.status != engine::sblr::SblrSourceMapDecodeStatusV1::ok ||
      decoded.canonical_bytes != s.canonical_smvd ||
      decoded.vector.descriptor_uuid != s.descriptor_uuid.bytes ||
      decoded.vector.descriptor_generation != s.descriptor_generation ||
      decoded.vector.registry_snapshot_uuid != s.registry_snapshot_uuid.bytes ||
      decoded.vector.registry_generation != s.registry_generation ||
      decoded.vector.statement_receipt_uuid != s.statement_receipt_uuid.bytes ||
      decoded.vector.bound_ast_sha256 != s.bound_ast_sha256 ||
      decoded.vector.vector_sha256 != s.vector_sha256) Stale();
}
Bytes EncodeRecord(Snapshot& s) {
  Validate(s);
  Bytes out(kRecordHeader+s.canonical_smvd.size(), 0);
  Put(out, 0, out.size(), 4);
  out[4] = static_cast<std::uint8_t>(s.lifecycle);
  Put(out, 8, s.descriptor_generation, 8);
  Put(out, 16, s.registry_generation, 8);
  Put(out, 24, s.publication_generation, 8);
  Copy(out, 32, s.descriptor_uuid.bytes); Copy(out, 48, s.registry_snapshot_uuid.bytes);
  Copy(out, 64, s.statement_receipt_uuid.bytes); Copy(out, 80, s.database_uuid.bytes);
  Copy(out, 96, s.session_uuid.bytes); Copy(out, 112, s.transaction_uuid.bytes);
  Copy(out, 128, s.principal_uuid.bytes); Copy(out, 144, s.publication_uuid.bytes);
  Copy(out, 160, s.bound_ast_sha256); Copy(out, 192, s.vector_sha256);
  Put(out, 224, s.canonical_smvd.size(), 4);
  Copy(out, kRecordHeader, s.canonical_smvd);
  s.decision_evidence_sha256 = Hash(kRecordDomain, {out.data(), 232},
                                  {out.data()+kRecordHeader, out.size()-kRecordHeader});
  Copy(out, 232, s.decision_evidence_sha256);
  return out;
}
Snapshot DecodeRecord(std::span<const std::uint8_t> bytes) {
  if (bytes.size() < kRecordHeader || bytes.size() > kRecordHeader+kMaximumSmvd) Stale();
  const auto* p = bytes.data();
  if (Get(p, 0, 4) != bytes.size() || Get(p, 224, 4) != bytes.size()-kRecordHeader ||
      p[5] || p[6] || p[7] || Get(p, 228, 4)) Stale();
  Snapshot s;
  s.lifecycle = static_cast<SblrSourceMapLifecycleV1>(p[4]);
  s.descriptor_generation = Get(p, 8, 8); s.registry_generation = Get(p, 16, 8);
  s.publication_generation = Get(p, 24, 8);
  Read(p+32, s.descriptor_uuid.bytes); Read(p+48, s.registry_snapshot_uuid.bytes);
  Read(p+64, s.statement_receipt_uuid.bytes); Read(p+80, s.database_uuid.bytes);
  Read(p+96, s.session_uuid.bytes); Read(p+112, s.transaction_uuid.bytes);
  Read(p+128, s.principal_uuid.bytes); Read(p+144, s.publication_uuid.bytes);
  Read(p+160, s.bound_ast_sha256); Read(p+192, s.vector_sha256);
  Read(p+232, s.decision_evidence_sha256);
  s.canonical_smvd.assign(bytes.begin()+kRecordHeader, bytes.end());
  const auto expected_hash = s.decision_evidence_sha256;
  const auto exact = EncodeRecord(s);
  if (expected_hash != s.decision_evidence_sha256 ||
      !std::equal(exact.begin(), exact.end(), bytes.begin(), bytes.end())) Stale();
  return s;
}

class Registry {
 public:
  std::map<std::array<std::uint8_t,16>, Snapshot> states;
  explicit Registry(const EngineRequestContext& c) : database_(c.database_uuid) {
    if (!c.security_context_present || !c.statement_metadata_snapshot_engine_owned ||
        !Valid(database_) || c.database_path.empty()) Hidden();
    std::error_code error;
    if (!std::filesystem::is_regular_file(c.database_path, error) || error) Hidden();
    path_ = c.database_path+".sb.sblr_source_map_registry.v1";
    const auto status = std::filesystem::symlink_status(path_, error);
    if (status.type() == std::filesystem::file_type::not_found &&
        (!error || error == std::errc::no_such_file_or_directory)) return;
    if (error) Io();
    if (!std::filesystem::is_regular_file(status)) Stale();
    disk::FileDevice file;
    if (!file.Open(path_, disk::FileOpenMode::open_existing).ok()) Io();
    const auto size = file.Size();
    if (!size.ok()) Io();
    if (size.size_bytes > kMaximumBytes) Limit();
    if (size.size_bytes < kHeader) Stale();
    Bytes bytes(static_cast<std::size_t>(size.size_bytes));
    const auto read = file.ReadAt(0, bytes.data(), bytes.size());
    if (!read.ok() || read.bytes_transferred != bytes.size()) Io();
    Decode(bytes);
    // Re-establish durability after an earlier unknown post-rename outcome.
    if (!file.Sync().ok() || !disk::SyncParentDirectoryPath(path_).ok() ||
        !file.Close().ok()) Io();
  }
  void NextPublication() {
    if (generation_ == UINT64_MAX) Stale();
    ++generation_;
    publication_ = NewUuid();
    if (states.contains(publication_.bytes)) Stale();
  }
  void Stamp(Snapshot& s) const {
    s.publication_generation = generation_;
    s.publication_uuid = publication_;
  }
  // Encodes every record before any durable change. Also stages record evidence
  // for copying into the result before Publish().
  Bytes Encode() {
    Bytes bytes(kHeader, 0);
    Copy(bytes, 0, kMagic); Put(bytes, 16, 2, 2); Put(bytes, 18, kHeader, 2);
    if (!generation_ || !Valid(publication_) ||
        states.size() > std::numeric_limits<std::uint32_t>::max()) Stale();
    Put(bytes, 20, states.size(), 4); Put(bytes, 24, generation_, 8);
    Copy(bytes, 32, database_.bytes); Copy(bytes, 48, publication_.bytes);
    Copy(bytes, 64, snapshot_hash_);
    for (auto& [id, s] : states) {
      if (id != s.descriptor_uuid.bytes || s.database_uuid != database_ ||
          s.publication_generation > generation_ ||
          (s.publication_generation == generation_ && s.publication_uuid != publication_)) Stale();
      if (s.canonical_smvd.size() > kMaximumSmvd ||
          kRecordHeader+s.canonical_smvd.size() > kMaximumBytes-bytes.size()) Limit();
      const auto record = EncodeRecord(s);
      bytes.insert(bytes.end(), record.begin(), record.end());
    }
    Put(bytes, 8, bytes.size(), 8);
    const auto hash = Hash(kSnapshotDomain, {bytes.data(), 96},
                           {bytes.data()+kHeader, bytes.size()-kHeader});
    Copy(bytes, 96, hash);
    return bytes;
  }
  void Publish(const Bytes& bytes) {
    const auto ordinal = temporary_ordinal.fetch_add(1, std::memory_order_relaxed);
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto temporary = path_+".tmp."+std::to_string(tick)+"."+std::to_string(ordinal);
    const std::filesystem::path temporary_path(temporary);
    const std::filesystem::path temporary_lock(temporary+".sb.owner.lock");
    struct Cleanup {
      const std::filesystem::path& path;
      const std::filesystem::path& lock;
      bool owned=false;
      bool lock_owned=false;
      ~Cleanup() {
        // Only this exclusive temporary's lock may be removed. Never unlink
        // the stable registry/node lock, whose inode carries live ownership.
        try {
          std::error_code ignored;
          if (owned) std::filesystem::remove(path, ignored);
          if (lock_owned) std::filesystem::remove(lock, ignored);
        } catch (...) { /* Unselected temporary paths have no authority. */ }
      }
    } cleanup{temporary_path,temporary_lock};
    {
      disk::FileDevice file;
      if (!file.Open(temporary, disk::FileOpenMode::create_new).ok()) Io();
      cleanup.owned = true;
      cleanup.lock_owned = true;
      const auto write = file.WriteAt(0, bytes.data(), bytes.size());
      if (!write.ok() || write.bytes_transferred != bytes.size() ||
          !file.Sync().ok() || !file.Close().ok()) Io();
    }
    std::error_code error;
    std::filesystem::rename(temporary, path_, error);
    if (error) Io();
    cleanup.owned = false;
    if (!disk::SyncParentDirectoryPath(path_).ok()) Io();
  }
 private:
  std::string path_;
  EngineUuid database_, publication_;
  std::uint64_t generation_=0;
  Sha snapshot_hash_{};
  void Decode(const Bytes& bytes) {
    const auto* p = bytes.data();
    if (!std::equal(kMagic.begin(), kMagic.end(), bytes.begin()) ||
        Get(p, 8, 8) != bytes.size() || Get(p, 16, 2) != 2 ||
        Get(p, 18, 2) != kHeader) Stale();
    EngineUuid database;
    Read(p+32, database.bytes);
    if (database != database_) Hidden();
    generation_ = Get(p, 24, 8); Read(p+48, publication_.bytes);
    Sha previous{}; Read(p+64, previous); Read(p+96, snapshot_hash_);
    if (!generation_ || !Valid(publication_) ||
        (generation_ == 1 ? Nonzero(previous) : !Nonzero(previous)) ||
        snapshot_hash_ != Hash(kSnapshotDomain, {p,96}, {p+kHeader,bytes.size()-kHeader})) Stale();
    const auto count = Get(p, 20, 4);
    if (!count || count > (bytes.size()-kHeader)/kRecordHeader) Stale();
    std::size_t offset = kHeader;
    std::array<std::uint8_t,16> last{};
    for (std::uint64_t index = 0; index < count; ++index) {
      if (bytes.size()-offset < kRecordHeader) Stale();
      const auto size = Get(p+offset, 0, 4);
      if (size < kRecordHeader || size > bytes.size()-offset ||
          size > kRecordHeader+kMaximumSmvd) Stale();
      auto s = DecodeRecord({p+offset,static_cast<std::size_t>(size)});
      if (s.database_uuid != database_ || s.descriptor_uuid.bytes <= last ||
          s.descriptor_uuid == publication_ || s.publication_generation > generation_ ||
          (s.publication_generation == generation_ && s.publication_uuid != publication_)) Stale();
      last = s.descriptor_uuid.bytes;
      states.emplace(last, std::move(s));
      offset += static_cast<std::size_t>(size);
    }
    if (offset != bytes.size()) Stale();
  }
};
} // namespace

Result IssueSblrSourceMapDescriptorV1(
    const EngineRequestContext& c, const EngineUuid& receipt, const Sha& bound,
    const EngineUuid& registry, std::uint64_t generation,
    std::vector<engine::sblr::SblrSourceMapEntryV1> entries) {
  return Run([&] {
    Receipt(c, receipt);
    Registry store(c);
    for (const auto& [id, existing] : store.states) {
      (void)id;
      if (existing.statement_receipt_uuid == receipt) Owner(c, receipt, existing);
    }
    if (!Valid(registry) || !generation || !Nonzero(bound) || entries.empty()) Invalid();
    store.NextPublication();
    Snapshot s;
    s.descriptor_uuid = NewUuid(); s.descriptor_generation = 1;
    s.registry_snapshot_uuid = registry; s.registry_generation = generation;
    s.statement_receipt_uuid = receipt; s.database_uuid = c.database_uuid;
    s.session_uuid = c.session_uuid; s.transaction_uuid = c.transaction_uuid;
    s.principal_uuid = c.principal_uuid; s.bound_ast_sha256 = bound;
    s.lifecycle = SblrSourceMapLifecycleV1::active;
    store.Stamp(s);
    engine::sblr::SblrSourceMapDescriptorVectorV1 v;
    v.descriptor_uuid = s.descriptor_uuid.bytes; v.descriptor_generation = 1;
    v.registry_snapshot_uuid = registry.bytes; v.registry_generation = generation;
    v.statement_receipt_uuid = receipt.bytes; v.bound_ast_sha256 = bound;
    v.entries = std::move(entries);
    s.canonical_smvd = engine::sblr::EncodeSblrSourceMapDescriptorVectorV1(&v);
    if (s.canonical_smvd.empty()) Invalid();
    s.vector_sha256 = v.vector_sha256;
    const auto [found, added] = store.states.emplace(s.descriptor_uuid.bytes, std::move(s));
    if (!added) Stale();
    const auto bytes = store.Encode();
    auto out = Success();
    out.snapshot = found->second;
    store.Publish(bytes);
    return out;
  });
}
Result LookupSblrSourceMapDescriptorV1(
    const EngineRequestContext& c, const EngineUuid& receipt,
    const EngineUuid& descriptor, std::uint64_t descriptor_generation,
    const Sha& bound, const EngineUuid& registry, std::uint64_t generation) {
  return Run([&] {
    Receipt(c, receipt);
    if (!Valid(descriptor)) Invalid();
    Registry store(c);
    const auto found = store.states.find(descriptor.bytes);
    if (found == store.states.end()) Hidden();
    Owner(c, receipt, found->second);
    const auto& s = found->second;
    if (s.lifecycle != SblrSourceMapLifecycleV1::active ||
        s.descriptor_generation != descriptor_generation ||
        s.registry_snapshot_uuid != registry || s.registry_generation != generation ||
        s.bound_ast_sha256 != bound) Stale();
    auto out = Success(); out.snapshot = s; return out;
  });
}
EngineApiDiagnostic RevokeSblrSourceMapDescriptorsV1(
    const EngineRequestContext& c, const EngineUuid& receipt) {
  return Run([&] {
    Receipt(c, receipt);
    Registry store(c);
    bool any = false;
    for (const auto& [id, s] : store.states) {
      (void)id;
      if (s.statement_receipt_uuid == receipt) {
        Owner(c, receipt, s);
        any = any || s.lifecycle == SblrSourceMapLifecycleV1::active;
      }
    }
    auto out = Success();
    if (!any) return out;
    store.NextPublication();
    for (auto& [id, s] : store.states) {
      (void)id;
      if (s.statement_receipt_uuid == receipt && s.lifecycle == SblrSourceMapLifecycleV1::active) {
        s.lifecycle = SblrSourceMapLifecycleV1::revoked; store.Stamp(s);
      }
    }
    const auto bytes = store.Encode();
    store.Publish(bytes);
    return out;
  }).diagnostic;
}
EngineApiDiagnostic RecoverSblrSourceMapDescriptorRegistryV1(const EngineRequestContext& c) {
  return Run([&] {
    Registry store(c);
    auto out = Success();
    if (std::none_of(store.states.begin(), store.states.end(), [](const auto& item) {
          return item.second.lifecycle == SblrSourceMapLifecycleV1::active;
        })) return out;
    store.NextPublication();
    for (auto& [id, s] : store.states) {
      (void)id;
      if (s.lifecycle == SblrSourceMapLifecycleV1::active) {
        s.lifecycle = SblrSourceMapLifecycleV1::revoked; store.Stamp(s);
      }
    }
    const auto bytes = store.Encode();
    store.Publish(bytes);
    return out;
  }).diagnostic;
}
} // namespace scratchbird::engine::internal_api
