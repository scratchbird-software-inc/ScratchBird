// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "sblr_parameter_set_registry.hpp"
#include "hash_digest.hpp"
#include "uuid.hpp"
#include "engine/sblr/sblr_parameter_runtime.hpp"
#include <array>
#include <iterator>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <algorithm>
#include <new>
#include <stdexcept>
#if !defined(_WIN32)
#include <sys/wait.h>
#include <sys/stat.h>
#include <cerrno>
#include <unistd.h>
#endif

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace api = scratchbird::engine::internal_api;
namespace uuid = scratchbird::core::uuid;
namespace { long fail_after = -1; }
#if defined(SB_PARAMETER_FSYNC_FAULTS)
namespace { bool fail_directory_sync = false; }
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd) {
  struct stat info{};
  if (fail_directory_sync && ::fstat(fd, &info) == 0 && S_ISDIR(info.st_mode)) {
    errno = EIO;
    return -1;
  }
  return __real_fsync(fd);
}
#endif
void* operator new(std::size_t n) {
  if (fail_after == 0) throw std::bad_alloc();
  if (fail_after > 0) --fail_after;
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
using scratchbird::core::platform::UuidKind;
[[noreturn]] void Fail(const char* text) { throw std::runtime_error(text); }
std::size_t checks = 0;
void Require(bool value, const char* text) { ++checks; if (!value) Fail(text); }
api::EngineUuid Id(UuidKind, std::uint64_t) {
  const auto value = uuid::IssueRuntimeIdentityV7();
  Require(value.has_value(), "UUID runtime issuance failed");
  return *value;
}
std::string IdentityFileKeyOracle(const api::EngineUuid& id) {
  std::string input = "SB_PARAMETER_FILE_KEY_V2";
  input.append(reinterpret_cast<const char*>(id.bytes.data()), id.bytes.size());
  std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
  Require(SHA256(reinterpret_cast<const unsigned char*>(input.data()), input.size(),
                 digest.data()) != nullptr, "file-key digest oracle failed");
  constexpr char hex[] = "0123456789abcdef";
  std::string key;
  for (const auto value : digest) { key.push_back(hex[value >> 4]); key.push_back(hex[value & 15]); }
  Require(key.find(uuid::UuidToString(id)) == std::string::npos,
          "filesystem token exposed a display UUID");
  return key;
}
struct Fixture {
  std::string base; api::EngineUuid database_uuid;
  std::vector<std::string> stores;
  ~Fixture(){std::error_code ignored;for(const auto& path:stores)std::filesystem::remove(path,ignored);}
  std::string Store(const api::EngineUuid& descriptor) {
    auto path=base+".sb.sblr_parameter_set."+IdentityFileKeyOracle(descriptor)+".v2";stores.push_back(path);return path;
  }
  std::string BindStore(const api::EngineUuid& descriptor) {
    auto path=base+".sb.sblr_parameter_bind."+IdentityFileKeyOracle(descriptor)+".v2";stores.push_back(path);return path;
  }
};
Fixture MakeFixture(std::uint64_t salt) {
  Fixture f;f.base=(std::filesystem::temp_directory_path()/("sb_parameter_registry_"+std::to_string(salt)+"_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))).string();f.database_uuid=Id(UuidKind::database,salt);return f;
}
api::EngineRequestContext Context(const Fixture& f,bool authority=true) {
  api::EngineRequestContext c;c.database_path=f.base;c.database_uuid=f.database_uuid;c.session_uuid=Id(UuidKind::object,100);c.catalog_generation_id=11;c.security_epoch=12;c.resource_epoch=13;c.security_context_present=true;if(authority){c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_statement_context_receipt");c.trace_tags.push_back("right:SBLR_PARAMETER_SET_ADMIN");}return c;
}
api::SblrParameterSetIssueRequest Request(std::uint64_t salt,bool dynamic=false) {
  api::SblrParameterSetIssueRequest r;r.statement_receipt_uuid=Id(UuidKind::object,salt+1);r.execution_uuid=Id(UuidKind::object,salt+2);r.batch_uuid=Id(UuidKind::object,salt+3);r.batch_generation=4;
  if(dynamic){r.dynamic_package_uuid=Id(UuidKind::object,salt+4);r.dynamic_generation=5;}else{r.prepared_statement_uuid=Id(UuidKind::object,salt+4);r.prepared_generation=5;}
  r.slots.push_back({Id(UuidKind::object,salt+5),7,api::SblrParameterDirection::in,false});
  r.slots.push_back({Id(UuidKind::object,salt+6),8,api::SblrParameterDirection::inout,true});r.reason_code=dynamic?"test.dynamic.batch.issue":"test.prepared.batch.issue";return r;
}
std::string Sha256(const std::vector<std::uint8_t>& bytes) {
  const auto digest=scratchbird::core::hash::ComputeSha256Digest(bytes);
  Require(digest.ok(),"value hash failed");
  return "sha256:"+scratchbird::core::hash::HexLower(digest.digest);
}
api::SblrParameterBindPublicationRequest BindRequest(
    const api::EngineRequestContext& context,
    const api::SblrParameterSetSnapshot& set) {
  api::SblrParameterBindPublicationRequest request;
  request.statement_receipt_uuid=Id(UuidKind::object,5001);
  request.execution_uuid=Id(UuidKind::object,5002);
  request.prepared_statement_uuid=set.prepared_statement_uuid;
  request.prepared_generation=set.prepared_generation;
  request.parameter_set_descriptor_uuid=set.parameter_set_descriptor_uuid;
  request.parameter_set_generation=set.descriptor_generation;
  request.ordered_slot_table_sha256=set.slots_sha256;
  request.batch_uuid=set.batch_uuid;
  request.batch_generation=set.batch_generation;
  request.dynamic_package_uuid=set.dynamic_package_uuid;
  request.dynamic_generation=set.dynamic_generation;
  request.catalog_snapshot_uuid=Id(UuidKind::object,5003);
  request.catalog_generation=context.catalog_generation_id;
  request.security_epoch=context.security_epoch;
  request.resource_epoch=context.resource_epoch;
  request.mga_snapshot_uuid=Id(UuidKind::object,5004);
  request.executor_availability_generation=1;
  scratchbird::engine::sblr::SblrParameterValueSetV1 values;
  values.parameter_set_descriptor_uuid = set.parameter_set_descriptor_uuid.bytes;
  values.descriptor_generation = set.descriptor_generation;
  values.execution_uuid = request.execution_uuid.bytes;
  values.statement_receipt_uuid = request.statement_receipt_uuid.bytes;
  for (const auto& slot : set.slots) {
    scratchbird::engine::sblr::SblrParameterValueRecordV1 value;
    value.slot_ordinal = slot.slot_ordinal;
    value.slot_uuid = slot.slot_uuid.bytes;
    value.datatype_descriptor_uuid = slot.datatype_descriptor_uuid.bytes;
    value.datatype_descriptor_generation = slot.datatype_descriptor_generation;
    value.direction = static_cast<scratchbird::engine::sblr::SblrParameterDirectionV1>(slot.direction);
    value.state = scratchbird::engine::sblr::SblrParameterValueStateV1::value;
    value.canonical_value_bytes = {1,0,0,0,0,0,0,0};
    values.records.push_back(value);
  }
  request.canonical_value_vector =
      scratchbird::engine::sblr::EncodeSblrParameterValueSetV1(values);
  Require(!request.canonical_value_vector.empty(), "canonical SBPV encoding failed");
  request.value_vector_sha256=Sha256(request.canonical_value_vector);
  return request;
}

std::string Read(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  Require(bool(in), "fixture read failed");
  return std::string(std::istreambuf_iterator<char>(in), {});
}
void Write(const std::string& path, const std::string& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), bytes.size());
  out.close();
  Require(bool(out), "fixture write failed");
}
std::uint64_t Number(std::string_view bytes, std::size_t at, unsigned width) {
  Require(at + width <= bytes.size(), "oracle read extent invalid");
  std::uint64_t result = 0;
  for (unsigned i = 0; i < width; ++i)
    result |= std::uint64_t(static_cast<unsigned char>(bytes[at + i])) << (8 * i);
  return result;
}
void Number(std::string& bytes, std::size_t at, std::uint64_t n, unsigned width) {
  Require(at + width <= bytes.size(), "oracle write extent invalid");
  for (unsigned i = 0; i < width; ++i) bytes[at + i] = static_cast<char>(n >> (8 * i));
}
void Identity(std::string_view bytes, std::size_t at, const api::EngineUuid& id) {
  Require(at + 16 <= bytes.size() &&
      std::equal(id.bytes.begin(), id.bytes.end(),
          reinterpret_cast<const unsigned char*>(bytes.data() + at)),
      "binary identity differs from authority");
}
std::array<unsigned char, 32> Digest(std::string_view bytes) {
  std::array<unsigned char, 32> digest{};
  Require(SHA256(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(),
                  digest.data()) != nullptr, "independent hash failed");
  return digest;
}
void CheckDigest(std::string_view bytes, std::size_t at, std::string_view material) {
  const auto digest = Digest(material);
  Require(at + digest.size() <= bytes.size() &&
      std::equal(digest.begin(), digest.end(),
          reinterpret_cast<const unsigned char*>(bytes.data() + at)),
      "binary hash differs from independent oracle");
}
void SetDigest(std::string& bytes, std::size_t at, std::string_view material) {
  const auto digest = Digest(material);
  std::copy(digest.begin(), digest.end(), bytes.begin() + at);
}
void SealSet(std::string& pair) {
  const auto size = Number(pair, 8, 4);
  std::string record = pair.substr(0, size);
  const auto count = Number(record, 248, 4);
  std::string slots("ScratchBird.SblrParameterSlots.V2");
  slots.append(record, 248, 4);
  slots.append(record, 320, count * 48);
  SetDigest(record, 256, slots);
  record[14] = 0;
  std::fill_n(record.begin() + 288, 32, 0);
  SetDigest(record, 288, std::string("ScratchBird.SblrParameterSetRegistry.V2") + record);
  record[14] = 1;
  pair = record;
  record[14] = 2;
  pair += record;
}
void CheckSetBytes(const std::string& bytes, const api::SblrParameterSetSnapshot& value,
                   std::string_view reason) {
  const auto size = 320 + value.slots.size() * 48 + reason.size();
  Require(bytes.size() == size * 2 &&
          bytes.substr(0, 8) == std::string("SBPSR2\0\0", 8),
          "parameter registry is not exact binary V2");
  const std::array<api::EngineUuid, 10> identities{
      value.snapshot_uuid, value.database_uuid, value.session_uuid,
      value.statement_receipt_uuid, value.execution_uuid,
      value.parameter_set_descriptor_uuid, value.prepared_statement_uuid,
      value.batch_uuid, value.dynamic_package_uuid, {}};
  const std::array<std::uint64_t, 9> numbers{
      1, 1, value.prepared_generation, value.batch_generation, value.dynamic_generation,
      value.catalog_generation, value.security_epoch, value.resource_epoch, 0};
  for (unsigned phase = 1; phase <= 2; ++phase) {
    std::string record = bytes.substr((phase - 1) * size, size);
    Require(Number(record, 8, 4) == size && Number(record, 12, 2) == 2 &&
            Number(record, 14, 1) == phase && Number(record, 15, 1) == 1,
            "parameter header differs");
    for (std::size_t i = 0; i < identities.size(); ++i) Identity(record, 16 + 16 * i, identities[i]);
    for (std::size_t i = 0; i < numbers.size(); ++i)
      Require(Number(record, 176 + i * 8, 8) == numbers[i], "parameter counter differs");
    Require(Number(record, 248, 4) == value.slots.size() &&
            Number(record, 252, 4) == reason.size() &&
            record.substr(320 + value.slots.size() * 48) == reason,
            "parameter slot/reason extents differ");
    for (std::size_t i = 0; i < value.slots.size(); ++i) {
      const auto at = 320 + i * 48;
      const auto& slot = value.slots[i];
      Require(Number(record, at, 4) == i, "slot ordinal differs");
      Identity(record, at + 4, slot.slot_uuid);
      Identity(record, at + 20, slot.datatype_descriptor_uuid);
      Require(Number(record, at + 36, 8) == slot.datatype_descriptor_generation &&
          Number(record, at + 44, 1) == static_cast<unsigned>(slot.direction) &&
          Number(record, at + 45, 1) == unsigned(slot.nullable) &&
          Number(record, at + 46, 2) == 0, "slot attributes differ");
    }
    std::string slots("ScratchBird.SblrParameterSlots.V2");
    slots.append(record, 248, 4);
    slots.append(record, 320, value.slots.size() * 48);
    CheckDigest(record, 256, slots);
    const auto original = record;
    record[14] = 0;
    std::fill_n(record.begin() + 288, 32, 0);
    CheckDigest(original, 288, std::string("ScratchBird.SblrParameterSetRegistry.V2") + record);
  }
}
void CheckBindBytes(std::string bytes, const api::SblrParameterBindPublicationSnapshot& value) {
  Require(bytes.size() == 360 + value.canonical_value_vector.size() &&
          bytes.substr(0, 8) == std::string("SBPBR2\0\0", 8) &&
          Number(bytes, 8, 4) == bytes.size() && Number(bytes, 12, 2) == 2 &&
          Number(bytes, 14, 2) == 0 && Number(bytes, 356, 4) == 0 &&
          Number(bytes, 352, 4) == value.canonical_value_vector.size(), "binding framing differs");
  const std::array<api::EngineUuid, 11> ids{
      value.database_uuid, value.session_uuid, value.statement_receipt_uuid, value.execution_uuid,
      value.prepared_statement_uuid, value.parameter_set_descriptor_uuid, value.batch_uuid,
      value.dynamic_package_uuid, value.catalog_snapshot_uuid, value.mga_snapshot_uuid,
      value.bind_evidence_uuid};
  for (std::size_t i = 0; i < ids.size(); ++i) Identity(bytes, 16 + 16 * i, ids[i]);
  const std::array<std::uint64_t, 8> numbers{
      value.prepared_generation, value.parameter_set_generation, value.batch_generation,
      value.dynamic_generation, value.catalog_generation, value.security_epoch,
      value.resource_epoch, value.executor_availability_generation};
  for (std::size_t i = 0; i < numbers.size(); ++i)
    Require(Number(bytes, 192 + 8 * i, 8) == numbers[i], "binding counter differs");
  Require(std::equal(value.canonical_value_vector.begin(), value.canonical_value_vector.end(),
      reinterpret_cast<const unsigned char*>(bytes.data() + 360)), "canonical binding payload differs");
  CheckDigest(bytes, 288, std::string_view(bytes).substr(360));
  const auto original = bytes;
  std::fill_n(bytes.begin() + 320, 32, 0);
  CheckDigest(original, 320, std::string("ScratchBird.SblrParameterBindPublication.V2") + bytes);
}
int RecoveryChild(int argc, char** argv) {
  Require(argc == 6, "child arguments invalid");
  Fixture f;
  f.base = argv[2];
  auto db = uuid::ParseDurableEngineIdentityUuid(UuidKind::database, argv[3]);
  auto session = uuid::ParseTypedUuid(UuidKind::session, argv[4]);
  auto id = uuid::ParseDurableEngineIdentityUuid(UuidKind::object, argv[5]);
  Require(db.ok() && session.ok() && id.ok(), "child fixture identity malformed");
  f.database_uuid = db.value.value;
  auto context = Context(f);
  context.session_uuid = session.value.value;
  const auto loaded = api::LoadSblrParameterSet(context, id.value.value);
  Require(loaded.ok, "fresh process could not load descriptor metadata");
  Require(!api::LoadSblrParameterBinding(context, id.value.value).ok,
          "fresh process recovered executable binding");
  Require(!api::PublishSblrParameterBinding(context, loaded.snapshot,
      BindRequest(context, loaded.snapshot)).ok, "fresh process reconstructed live authority");
  std::cout << "parameter registry checks=" << checks << "\n"; return EXIT_SUCCESS;
}
void ExtendedTests(const char* executable) {
  Fixture f = MakeFixture(70);
  auto c = Context(f);
  auto request = Request(7100);
  const auto set = api::IssueSblrParameterSet(c, request);
  Require(set.ok, "extended issue failed");
  const auto path = f.Store(set.snapshot.parameter_set_descriptor_uuid);
  const auto bytes = Read(path);
  CheckSetBytes(bytes, set.snapshot, request.reason_code);
  const auto bind_request = BindRequest(c, set.snapshot);
  const auto bound = api::PublishSblrParameterBinding(c, set.snapshot, bind_request);
  Require(bound.ok, "extended bind failed");
  const auto bind_path = f.BindStore(set.snapshot.parameter_set_descriptor_uuid);
  const auto bind_bytes = Read(bind_path);
  CheckBindBytes(bind_bytes, bound.snapshot);
#if defined(SB_PARAMETER_FSYNC_FAULTS)
  Require(std::filesystem::remove(bind_path), "barrier fixture removal failed");
  fail_directory_sync = true;
  const auto uncertain = api::PublishSblrParameterBinding(c, set.snapshot, bind_request);
  Require(!uncertain.ok && uncertain.diagnostic.code == "SBLR.EXECUTION_FAILED",
          "failed directory barrier reported publication success");
  const auto uncertain_bytes = Read(bind_path);
  Require(!api::LoadSblrParameterBinding(c,set.snapshot.parameter_set_descriptor_uuid).ok &&
          !api::PublishSblrParameterBinding(c,set.snapshot,bind_request).ok &&
          Read(bind_path)==uncertain_bytes,
          "failed directory barrier was laundered through read or replay");
  fail_directory_sync = false;
  const auto confirmed = api::PublishSblrParameterBinding(c,set.snapshot,bind_request);
  Require(confirmed.ok && confirmed.replayed && Read(bind_path)==uncertain_bytes,
          "exact replay failed to confirm uncertain publication without replacement");
  Identity(uncertain_bytes,176,confirmed.snapshot.bind_evidence_uuid);
  Write(bind_path,bind_bytes);
#endif

  std::size_t allocation_faults = 0;
  for (long fault = 0; fault < 2048; ++fault) {
    Require(std::filesystem::remove(bind_path), "fault fixture binding removal failed");
    bool threw = false;
    api::SblrParameterBindPublicationResult result;
    fail_after = fault;
    try {
      result = api::PublishSblrParameterBinding(c, set.snapshot, bind_request);
    } catch (const std::bad_alloc&) {
      threw = true;
    }
    fail_after = -1;
    if (threw) ++allocation_faults;
    else Require(result.ok, "nonallocating bind failed during fault sweep");
    if (std::filesystem::exists(bind_path)) {
      // An exception after the link/barrier is not a rollback. Any visible
      // record must still be a complete truthful publication, never a stub.
      const auto durable = api::LoadSblrParameterBinding(c, set.snapshot.parameter_set_descriptor_uuid);
      Require(durable.ok && durable.snapshot.canonical_value_vector == bind_request.canonical_value_vector,
              "allocation failure left a corrupt binding");
    } else {
      Require(threw, "successful binding has no durable record");
    }
    Require(Read(path) == bytes, "binding allocation fault changed descriptor");
    bool leaked = false;
    for (const auto& entry : std::filesystem::directory_iterator(
             std::filesystem::path(bind_path).parent_path())) {
      leaked = leaked || entry.path().string().starts_with(bind_path + ".tmp.");
    }
    Require(!leaked, "allocation failure leaked a provisional binding");
    Write(bind_path, bind_bytes);
    if (!threw) break;
    Require(fault != 2047, "allocation fault sweep never reached success");
  }
  Require(allocation_faults > 20, "allocation sweep did not reach actual owner allocations");
  std::cout << "binding allocation faults=" << allocation_faults << '\n';

#if !defined(_WIN32)
  const auto db = uuid::UuidToString(c.database_uuid);
  const auto session = uuid::UuidToString(c.session_uuid);
  const auto id = uuid::UuidToString(set.snapshot.parameter_set_descriptor_uuid);
  const auto child = ::fork();
  Require(child >= 0, "fork failed");
  if (child == 0) {
    ::execl(executable, executable, "--recovery-child", f.base.c_str(), db.c_str(),
            session.c_str(), id.c_str(), static_cast<char*>(nullptr));
    ::_exit(99);
  }
  int status = 0;
  Require(::waitpid(child, &status, 0) == child && WIFEXITED(status) &&
          WEXITSTATUS(status) == 0, "fresh-process recovery contract failed");
  Require(Read(path) == bytes && Read(bind_path) == bind_bytes,
          "fresh-process refusal changed disk");
#endif
  auto cross = c;
  cross.session_uuid = Id(UuidKind::session, 1);
  Require(!api::LoadSblrParameterSet(cross, set.snapshot.parameter_set_descriptor_uuid).ok &&
          !api::LoadSblrParameterBinding(cross, set.snapshot.parameter_set_descriptor_uuid).ok &&
          !api::PublishSblrParameterBinding(cross, set.snapshot, bind_request).ok,
          "cross-session binding admitted");
  cross = c; cross.database_uuid = Id(UuidKind::database, 1);
  Require(!api::LoadSblrParameterBinding(cross, set.snapshot.parameter_set_descriptor_uuid).ok,
          "cross-database binding admitted");
  auto forged = set.snapshot; forged.slots[0].nullable = true;
  Require(!api::PublishSblrParameterBinding(c, forged, bind_request).ok,
          "caller-mutated admitted snapshot gained authority");
  auto observed = set.snapshot; observed.snapshot_generation = 999;
  const auto before = observed;
  Require(api::RevalidateSblrParameterSet(c, set.snapshot, request.statement_receipt_uuid,
      request.execution_uuid, request.prepared_statement_uuid, request.prepared_generation + 1,
      request.batch_uuid, request.batch_generation, {}, 0, &observed).code == "SBLR.PARAMETER.STALE" &&
      observed.snapshot_generation == before.snapshot_generation &&
      observed.snapshot_uuid == before.snapshot_uuid, "failed revalidation changed output");

  for (std::size_t i = 0; i < bytes.size(); ++i) {
    auto changed = bytes; changed[i] ^= 1; Write(path, changed);
    Require(!api::LoadSblrParameterSet(c, set.snapshot.parameter_set_descriptor_uuid).ok,
            "single-byte registry corruption accepted");
    Write(path, bytes.substr(0, i));
    Require(!api::LoadSblrParameterSet(c, set.snapshot.parameter_set_descriptor_uuid).ok,
            "truncated registry accepted");
  }
  Write(path, bytes);
  for (std::size_t i = 0; i < bind_bytes.size(); ++i) {
    auto changed = bind_bytes; changed[i] ^= 1; Write(bind_path, changed);
    Require(!api::LoadSblrParameterBinding(c, set.snapshot.parameter_set_descriptor_uuid).ok,
            "single-byte binding corruption accepted");
  }
  Write(bind_path, bind_bytes);
  for (const auto at : {14U,22U,38U,54U,70U,86U,102U,118U,150U,166U,182U,356U}) {
    auto changed = bind_bytes;
    changed[at] ^= 0x30;
    std::fill_n(changed.begin() + 320, 32, 0);
    SetDigest(changed, 320,
        std::string("ScratchBird.SblrParameterBindPublication.V2") + changed);
    Write(bind_path, changed);
    Require(!api::LoadSblrParameterBinding(c, set.snapshot.parameter_set_descriptor_uuid).ok,
            "rehashed invalid binding accepted");
  }
  Write(bind_path, bind_bytes);
  // Recompute both valid evidence hashes: these must fail semantic checks,
  // not merely rely on a digest mismatch.
  const auto reject = [&](std::string changed) {
    SealSet(changed);
    Write(path, changed);
    Require(!api::LoadSblrParameterSet(c, set.snapshot.parameter_set_descriptor_uuid).ok,
            "semantically invalid rehashed registry accepted");
    Write(path, bytes);
  };
  for (const auto at : {16U,32U,48U,64U,80U,96U,112U,128U,324U,340U,372U,388U}) {
    for (unsigned version = 0; version < 16; ++version) {
      if (version == 7) continue;
      auto changed = bytes;
      changed[at + 6] = static_cast<char>((version << 4) | (changed[at + 6] & 15));
      reject(std::move(changed));
    }
  }
  { auto changed=bytes;std::copy_n(changed.begin()+324,16,changed.begin()+372);reject(changed); }
  for (const auto at : {320U,364U,365U,366U,367U}) {
    auto changed=bytes;changed[at]=static_cast<char>(255);reject(changed);
  }
  {auto changed=bytes;Number(changed,192,0,8);reject(changed);}
  {auto changed=bytes;Number(changed,176,UINT64_MAX,8);reject(changed);}
  {auto changed=bytes;Number(changed,240,1,8);reject(changed);}
  {auto changed=bytes;std::copy_n(changed.begin()+112,16,changed.begin()+144);Number(changed,208,1,8);reject(changed);}
  for (const auto& legacy : {std::string("SBPSR1\tEVIDENCE\n"), bytes + "SBPSR1\n", bytes + bytes}) {
    Write(path, legacy);
    Require(!api::LoadSblrParameterSet(c, set.snapshot.parameter_set_descriptor_uuid).ok,
            "legacy/mixed/duplicate journal admitted");
  }
  Write(path, bytes);
  Write(bind_path, "SBPBR1\tlegacy\n");
  Require(!api::LoadSblrParameterBinding(c,set.snapshot.parameter_set_descriptor_uuid).ok,
          "legacy binding admitted");
  Write(bind_path, bind_bytes);
  std::filesystem::resize_file(path, 1024U * 1024U + 1);
  Require(!api::LoadSblrParameterSet(c,set.snapshot.parameter_set_descriptor_uuid).ok,
          "oversized registry admitted");
  Write(path, bytes);
#if !defined(_WIN32)
  const auto saved = path + ".test-saved";
  f.stores.push_back(saved);
  std::filesystem::rename(path, saved);
  std::filesystem::create_symlink(saved, path);
  Require(!api::LoadSblrParameterSet(c,set.snapshot.parameter_set_descriptor_uuid).ok,
          "registry symlink admitted");
  std::filesystem::remove(path);
  std::filesystem::rename(saved, path);
#endif
  auto invalid = bind_request; invalid.canonical_value_vector = {'S','B','P','V',1,2,3,4};
  invalid.value_vector_sha256 = Sha256(invalid.canonical_value_vector);
  Require(api::PublishSblrParameterBinding(c,set.snapshot,invalid).diagnostic.code == "SBLR.OPERAND_INVALID",
          "fake canonical value vector admitted");
  Require(Read(path)==bytes && Read(bind_path)==bind_bytes,"refusal modified durable bytes");
  auto stale_session = c; stale_session.session_uuid.bytes[6] =
      (stale_session.session_uuid.bytes[6] & 15) | 0x40;
  Require(api::IssueSblrParameterSet(stale_session,request).diagnostic.code == "SBLR.OPERAND_INVALID",
          "non-v7 system session admitted");
  Require(EVP_set_default_properties(nullptr, "provider=sb_missing_parameter_test") == 1,
          "hash provider fault setup failed");
  const auto hash_failed = api::LoadSblrParameterSet(c,set.snapshot.parameter_set_descriptor_uuid);
  Require(EVP_set_default_properties(nullptr, "") == 1, "hash provider restore failed");
  Require(!hash_failed.ok && Read(path)==bytes,"hash failure accepted or changed registry");
  Require(api::BeginSblrParameterSetRegistryRecovery(c).code=="OK","recovery boundary failed");
  Require(api::LoadSblrParameterSet(c,set.snapshot.parameter_set_descriptor_uuid).ok &&
          !api::LoadSblrParameterBinding(c,set.snapshot.parameter_set_descriptor_uuid).ok &&
          !api::PublishSblrParameterBinding(c,set.snapshot,bind_request).ok,
          "recovery restored execution binding authority");
  const auto revoked = api::InvalidateSblrParameterSet(c,set.snapshot.parameter_set_descriptor_uuid,
      set.snapshot.snapshot_uuid,1,"test.revoke");
  Require(revoked.ok,"recovered metadata revocation failed");
  const auto revoked_bytes = Read(path);
  Require(!api::InvalidateSblrParameterSet(c,set.snapshot.parameter_set_descriptor_uuid,
      revoked.snapshot.snapshot_uuid,2,"test.revoke.again").ok && Read(path)==revoked_bytes,
      "repeated revocation appended an invalid transition");
}

int Run(int argc, char** argv);
int main(int argc, char** argv) {
  try {
    return Run(argc, argv);
  } catch (const std::exception& error) {
    fail_after = -1;
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
int Run(int argc, char** argv){
  if (argc > 1) return RecoveryChild(argc, argv);
  const auto executable = std::filesystem::absolute(argv[0]).string();
  ExtendedTests(executable.c_str());

  Fixture fixture=MakeFixture(1);auto context=Context(fixture);auto request=Request(1000);
  auto denied=api::IssueSblrParameterSet(Context(fixture,false),request);Require(!denied.ok&&denied.diagnostic.code=="SECURITY.ACCESS_DENIED","unauthorized issue admitted");
  auto issued=api::IssueSblrParameterSet(context,request);Require(issued.ok&&issued.snapshot.snapshot_generation==1&&issued.snapshot.descriptor_generation==1&&issued.snapshot.slots.size()==2&&issued.snapshot.slots[0].slot_uuid!=issued.snapshot.slots[1].slot_uuid,"engine-issued immutable descriptor missing");fixture.Store(issued.snapshot.parameter_set_descriptor_uuid);
  auto restarted=api::LoadSblrParameterSet(context,issued.snapshot.parameter_set_descriptor_uuid);Require(restarted.ok&&restarted.snapshot.decision_evidence_sha256==issued.snapshot.decision_evidence_sha256&&restarted.snapshot.slots_sha256==issued.snapshot.slots_sha256,"same-process reload did not preserve descriptor");
  const auto bind_request=BindRequest(context,issued.snapshot);
  auto bound=api::PublishSblrParameterBinding(context,issued.snapshot,bind_request);Require(bound.ok&&!bound.replayed&&!bound.snapshot.bind_evidence_uuid.is_nil(),"durable parameter binding failed");fixture.BindStore(issued.snapshot.parameter_set_descriptor_uuid);
  auto replay=api::PublishSblrParameterBinding(context,issued.snapshot,bind_request);Require(replay.ok&&replay.replayed&&replay.snapshot.bind_evidence_uuid==bound.snapshot.bind_evidence_uuid&&replay.snapshot.publication_evidence_sha256==bound.snapshot.publication_evidence_sha256,"exact parameter binding replay changed evidence");
  auto changed_bind=bind_request;auto changed_values=scratchbird::engine::sblr::DecodeSblrParameterValueSetV1(changed_bind.canonical_value_vector.data(),changed_bind.canonical_value_vector.size());Require(changed_values.ok,"canonical SBPV decode failed");changed_values.value.records.back().canonical_value_bytes.back()^=1;changed_bind.canonical_value_vector=scratchbird::engine::sblr::EncodeSblrParameterValueSetV1(changed_values.value);changed_bind.value_vector_sha256=Sha256(changed_bind.canonical_value_vector);auto conflict=api::PublishSblrParameterBinding(context,issued.snapshot,changed_bind);Require(!conflict.ok&&conflict.diagnostic.code=="SBLR.PARAMETER.STALE","changed parameter value overwrote durable binding");
  auto loaded_bind=api::LoadSblrParameterBinding(context,issued.snapshot.parameter_set_descriptor_uuid);Require(loaded_bind.ok&&loaded_bind.snapshot.canonical_value_vector==bind_request.canonical_value_vector&&loaded_bind.snapshot.bind_evidence_uuid==bound.snapshot.bind_evidence_uuid,"same-process reload did not preserve parameter binding");

  Fixture cancellation_fixture=MakeFixture(4);auto cancellation_context=Context(cancellation_fixture);auto cancellation_issue=api::IssueSblrParameterSet(cancellation_context,Request(5000));Require(cancellation_issue.ok,"cancellation fixture issue failed");cancellation_fixture.Store(cancellation_issue.snapshot.parameter_set_descriptor_uuid);const auto cancellation_bind_request=BindRequest(cancellation_context,cancellation_issue.snapshot);const auto cancellation_bind_path=cancellation_fixture.BindStore(cancellation_issue.snapshot.parameter_set_descriptor_uuid);cancellation_context.query_cancellation_requested=[] { return true; };auto cancelled_bind=api::PublishSblrParameterBinding(cancellation_context,cancellation_issue.snapshot,cancellation_bind_request);Require(!cancelled_bind.ok&&cancelled_bind.diagnostic.code=="PROCESS.CANCELLED"&&cancelled_bind.diagnostic.message_key=="sblr.parameter_bind.cancelled"&&cancelled_bind.diagnostic.detail=="binding was cancelled before durable publication","pre-publication cancellation did not fail closed");Require(!std::filesystem::exists(cancellation_bind_path),"cancelled parameter binding published durable state");cancellation_context.query_cancellation_requested=[] { return false; };auto post_cancel_bind=api::PublishSblrParameterBinding(cancellation_context,cancellation_issue.snapshot,cancellation_bind_request);Require(post_cancel_bind.ok&&!post_cancel_bind.replayed,"parameter binding did not recover after pre-publication cancellation");cancellation_context.query_cancellation_requested=[] { return true; };auto post_publication_replay=api::PublishSblrParameterBinding(cancellation_context,cancellation_issue.snapshot,cancellation_bind_request);Require(post_publication_replay.ok&&post_publication_replay.replayed&&post_publication_replay.snapshot.bind_evidence_uuid==post_cancel_bind.snapshot.bind_evidence_uuid&&post_publication_replay.snapshot.publication_evidence_sha256==post_cancel_bind.snapshot.publication_evidence_sha256,"post-publication cancellation retracted or changed the exact binding");
  api::SblrParameterSetSnapshot observed;auto diagnostic=api::RevalidateSblrParameterSet(context,issued.snapshot,request.statement_receipt_uuid,request.execution_uuid,request.prepared_statement_uuid,request.prepared_generation,request.batch_uuid,request.batch_generation,{},0,&observed);Require(diagnostic.code=="OK","valid prepared/batch binding refused");
  diagnostic=api::RevalidateSblrParameterSet(context,issued.snapshot,request.statement_receipt_uuid,request.execution_uuid,request.prepared_statement_uuid,request.prepared_generation+1,request.batch_uuid,request.batch_generation,{},0,nullptr);Require(diagnostic.code=="SBLR.PARAMETER.STALE","prepared generation drift admitted");
  auto stale=api::InvalidateSblrParameterSet(context,issued.snapshot.parameter_set_descriptor_uuid,Id(UuidKind::object,9000),1,"test.stale");Require(!stale.ok&&stale.diagnostic.code=="SBLR.PARAMETER.STALE","stale compare admitted");
  auto revoked=api::InvalidateSblrParameterSet(context,issued.snapshot.parameter_set_descriptor_uuid,issued.snapshot.snapshot_uuid,issued.snapshot.snapshot_generation,"test.prepared.invalidate");Require(revoked.ok&&revoked.snapshot.snapshot_generation==2&&revoked.snapshot.descriptor_generation==2&&revoked.snapshot.state==api::SblrParameterSetState::revoked,"durable invalidation failed");
  diagnostic=api::RevalidateSblrParameterSet(context,issued.snapshot,request.statement_receipt_uuid,request.execution_uuid,request.prepared_statement_uuid,request.prepared_generation,request.batch_uuid,request.batch_generation,{},0,nullptr);Require(diagnostic.code=="SBLR.PARAMETER.STALE","revoked set admitted");

  auto dynamic_request=Request(2000,true);auto dynamic=api::IssueSblrParameterSet(context,dynamic_request);Require(dynamic.ok,"dynamic/batch issue refused");fixture.Store(dynamic.snapshot.parameter_set_descriptor_uuid);diagnostic=api::RevalidateSblrParameterSet(context,dynamic.snapshot,dynamic_request.statement_receipt_uuid,dynamic_request.execution_uuid,{},0,dynamic_request.batch_uuid,dynamic_request.batch_generation,dynamic_request.dynamic_package_uuid,dynamic_request.dynamic_generation,nullptr);Require(diagnostic.code=="OK","dynamic generation binding refused");
  auto contradictory_request=dynamic_request;contradictory_request.prepared_statement_uuid=Id(UuidKind::object,2200);contradictory_request.prepared_generation=1;auto contradictory_identity=api::IssueSblrParameterSet(context,contradictory_request);Require(!contradictory_identity.ok&&contradictory_identity.diagnostic.code=="SBLR.OPERAND_INVALID","prepared/dynamic identity contradiction admitted");
  Require(api::BeginSblrParameterSetRegistryRecovery(context).code=="OK","startup recovery boundary failed");auto recovered_metadata=api::LoadSblrParameterSet(context,dynamic.snapshot.parameter_set_descriptor_uuid);Require(recovered_metadata.ok&&recovered_metadata.snapshot.slots_sha256==dynamic.snapshot.slots_sha256,"prepared/dynamic descriptor metadata did not recover");diagnostic=api::RevalidateSblrParameterSet(context,dynamic.snapshot,dynamic_request.statement_receipt_uuid,dynamic_request.execution_uuid,{},0,dynamic_request.batch_uuid,dynamic_request.batch_generation,dynamic_request.dynamic_package_uuid,dynamic_request.dynamic_generation,nullptr);Require(diagnostic.code=="SBLR.PARAMETER.STALE","execution receipt authority recovered across restart");

  Fixture torn_fixture=MakeFixture(2);auto torn_context=Context(torn_fixture);auto torn_issue=api::IssueSblrParameterSet(torn_context,Request(3000));Require(torn_issue.ok,"torn fixture issue failed");const auto torn_path=torn_fixture.Store(torn_issue.snapshot.parameter_set_descriptor_uuid);{std::ifstream in(torn_path,std::ios::binary);std::string bytes((std::istreambuf_iterator<char>(in)),{});Require(bytes.size()>24,"binary frame absent");std::uint32_t size=0;for(unsigned i=0;i<4;++i)size|=std::uint32_t(static_cast<unsigned char>(bytes[8+i]))<<(8*i);Require(size<=bytes.size(),"binary frame extent invalid");std::ofstream out(torn_path,std::ios::binary|std::ios::app);out.write(bytes.data(),size);}auto torn=api::LoadSblrParameterSet(torn_context,torn_issue.snapshot.parameter_set_descriptor_uuid);Require(!torn.ok&&torn.diagnostic.code=="SBLR.PARAMETER.STALE","torn evidence recovered");
  Fixture corrupt_fixture=MakeFixture(3);auto corrupt_context=Context(corrupt_fixture);auto corrupt_issue=api::IssueSblrParameterSet(corrupt_context,Request(4000));Require(corrupt_issue.ok,"corrupt fixture issue failed");const auto corrupt_path=corrupt_fixture.Store(corrupt_issue.snapshot.parameter_set_descriptor_uuid);{std::ifstream in(corrupt_path,std::ios::binary);std::string bytes((std::istreambuf_iterator<char>(in)),{});Require(bytes.size()>320,"binary evidence hash absent");bytes[256]^=1;std::ofstream out(corrupt_path,std::ios::binary|std::ios::trunc);out<<bytes;}auto corrupt=api::LoadSblrParameterSet(corrupt_context,corrupt_issue.snapshot.parameter_set_descriptor_uuid);Require(!corrupt.ok&&corrupt.diagnostic.code=="SBLR.PARAMETER.STALE","corrupt evidence recovered");std::cout << "parameter registry checks=" << checks << '\n';return EXIT_SUCCESS;
}
