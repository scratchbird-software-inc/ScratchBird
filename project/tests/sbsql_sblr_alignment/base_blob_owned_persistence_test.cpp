// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../support/blob_persistence_fixture.hpp"
#include "../support/owned_temp_directory.hpp"
#include "disk_device.hpp"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

thread_local long allocation_budget = -1;
thread_local unsigned allocation_count = 0;
thread_local bool allocation_fault_triggered = false;
thread_local std::stop_source* stop_on_allocation = nullptr;
thread_local long stop_allocation_budget = 0;
void* operator new(std::size_t n) {
  ++allocation_count;
  if (stop_on_allocation && stop_allocation_budget-- == 0) {
    stop_on_allocation->request_stop(); stop_on_allocation = nullptr;
  }
  if (allocation_budget >= 0 && allocation_budget-- == 0) {
    allocation_fault_triggered = true; throw std::bad_alloc();
  }
  if (auto* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
#include "../support/aligned_allocation_fault_bridge.hpp"

namespace dt = scratchbird::core::datatypes;
namespace mem = scratchbird::core::memory;
namespace disk = scratchbird::storage::disk;
using byte = scratchbird::core::platform::byte;
using Fixture = scratchbird::tests::BlobPersistenceFixture;
using Failure = dt::BlobOwnedPersistenceFailureV3;
std::atomic<unsigned> checks{0};
void Check(bool ok, const char* why) {
  ++checks; if (!ok) throw std::runtime_error(why);
}
namespace {
#include "base_blob_component_vectors.inc"
std::vector<byte> Golden(unsigned index) {
  const auto hex = kBlobComponentGoldenVectors[index].component_hex;
  const auto digit = [](char c) -> byte {
    if (c >= '0' && c <= '9') return c-'0';
    if (c >= 'a' && c <= 'f') return c-'a'+10;
    throw std::runtime_error("bad independent BLOB fixture hex");
  };
  std::vector<byte> bytes(hex.size()/2);
  for (std::size_t i = 0; i < bytes.size(); ++i)
    bytes[i] = static_cast<byte>((digit(hex[2*i])<<4)|digit(hex[2*i+1]));
  return bytes;
}
bool Equal(std::span<const byte> a, std::span<const byte> b) {
  return std::equal(a.begin(), a.end(), b.begin(), b.end());
}
void Clean(Fixture& f) {
  const auto physical = f.manager.Snapshot();
  Check(physical.current_bytes == 0 && physical.reserved_capacity_bytes == 0 &&
        physical.active_capacity_reservation_count == 0 && physical.active_allocation_count == 0 &&
        physical.sharded_accounting_current_bytes == 0, "standalone physical ownership leaked");
  const auto budget = f.ledger.Snapshot();
  Check(budget.current_bytes == 0 && budget.reserved_bytes == 0 &&
        budget.active_allocation_count == 0 && budget.active_reservation_count == 0,
        "standalone hierarchical budget leaked");
}
void GoldenRoundTrips() {
  Fixture f;
  for (unsigned index = 0; index < 5; ++index) {
    const auto expected = Golden(index);
    for (auto chunk : {std::size_t{1}, 2uL, 3uL, 319uL, 320uL, 65536uL, 1048576uL}) {
      {
        dt::BlobOwnedPersistenceBytesV3 source, encoded;
        const auto content = std::span<const byte>(expected).subspan(320);
        f.Copy(content, source);
        auto grant = f.Grant();
        const auto result = dt::EncodeOwnedBlobComponentV3(f.profile,
            kBlobComponentGoldenVectors[index].state, source, grant, encoded, {chunk});
        Check(result.ok() && !grant && encoded.bound() && Equal(encoded.bytes(), expected),
              "complete encoder differs from independent registered bytes");
        Check(result.digest_bytes == content.size() && result.copied_bytes == content.size() && result.header_bytes_written==320,
              "encoder lost actual two-pass work counters");
        dt::BlobOwnedPersistenceDecodedV3 decoded;
        auto target = f.Grant();
        const auto restored = dt::DecodeOwnedBlobComponentV3(f.profile, encoded, true, target, decoded, {chunk});
        Check(restored.ok() && !target && decoded.content().bound() &&
              decoded.state() == kBlobComponentGoldenVectors[index].state &&
              Equal(decoded.content().bytes(), content), "complete decode lost exact value/state");
        Check(restored.digest_bytes == content.size() && restored.copied_bytes == content.size() && restored.header_bytes_written==0,
              "decoder lost actual hash/copy work counters");
      }
      Clean(f);
    }
  }
}
void LongBinaryRoundTrips() {
  Fixture f;
  for (std::size_t length : {63,64,65,1023,1024,1025,65535,65536,65537,131073}) {
    std::vector<byte> bytes(length);
    for (std::size_t i = 0; i < length; ++i) bytes[i] = static_cast<byte>(i*73+(i>>8));
    {
      dt::BlobOwnedPersistenceBytesV3 source, encoded;
      f.Copy(bytes, source);
      auto grant = f.Grant();
      Check(dt::EncodeOwnedBlobComponentV3(f.profile, dt::BlobValueStateV3::value,
          source, grant, encoded, {997}).ok(), "large binary component encoding failed");
      dt::BlobOwnedPersistenceDecodedV3 decoded; grant = f.Grant();
      Check(dt::DecodeOwnedBlobComponentV3(f.profile, encoded, true, grant, decoded, {1024}).ok() &&
          Equal(decoded.content().bytes(), bytes), "fragmented binary roundtrip changed octets");
    }
    Clean(f);
  }
}
void PrivateRefusalsAndCancellation() {
  Fixture f;
  {
    dt::BlobOwnedPersistenceBytesV3 empty, source, output;
    const std::array<byte,3> abc{'a','b','c'};
    f.Copy(abc, source);
    auto grant = f.Grant(); auto* original = grant.get();
    const auto baseline = f.manager.Snapshot().current_bytes;
    Check(dt::EncodeOwnedBlobComponentV3(f.profile, dt::BlobValueStateV3::value,
        empty, grant, output).failure == Failure::invalid_argument, "unowned source admitted");
    for (const auto chunk : {std::size_t{0}, std::size_t{1048577}}) {
      Check(dt::CopyBlobPersistenceBytesV3(grant, abc, output, {chunk}).failure == Failure::invalid_argument,
            "invalid chunk silently defaulted");
      Check(dt::EncodeOwnedBlobComponentV3(f.profile, dt::BlobValueStateV3::value,
          source, grant, output, {chunk}).failure == Failure::invalid_argument, "invalid encoder chunk admitted");
    }
    Check(dt::CopyBlobPersistenceBytesV3(grant,
        {reinterpret_cast<const byte*>(&output), sizeof(output)}, output).failure == Failure::invalid_argument,
        "copy source/output object alias admitted");
    Check(dt::CopyBlobPersistenceBytesV3(grant, {static_cast<const byte*>(nullptr),1}, output).failure == Failure::invalid_argument,
          "null nonempty source admitted");
    Check(dt::CopyBlobPersistenceBytesV3(grant,
        {reinterpret_cast<const byte*>(UINTPTR_MAX-1),3}, output).failure == Failure::invalid_argument,
        "overflowed source extent admitted");
    auto bad = f.profile; bad.profile_fingerprint[0] ^= 1;
    Check(dt::EncodeOwnedBlobComponentV3(bad, dt::BlobValueStateV3::value, source, grant, output)
          .component.failure == dt::BlobComponentPassFailureV3::profile_invalid, "stale profile admitted");
    Check(dt::EncodeOwnedBlobComponentV3(f.profile, dt::BlobValueStateV3::sql_null, source, grant, output)
          .component.failure == dt::BlobComponentPassFailureV3::invalid_encode_state, "dirty SQL NULL admitted");
    Check(dt::EncodeOwnedBlobComponentV3(f.profile, static_cast<dt::BlobValueStateV3>(2), source, grant, output)
          .component.failure == dt::BlobComponentPassFailureV3::invalid_encode_state, "third component state admitted");
    std::stop_source stop; stop.request_stop();
    Check(dt::CopyBlobPersistenceBytesV3(grant, abc, output, {1,stop.get_token()}).failure == Failure::cancelled,
          "pre-cancelled owned copy published");
    Check(dt::EncodeOwnedBlobComponentV3(f.profile, dt::BlobValueStateV3::value,
        source, grant, output, {1,stop.get_token()}).failure == Failure::cancelled,
          "pre-cancelled component encode published");
    std::stop_source during;
    stop_on_allocation = &during; stop_allocation_budget = 0;
    const auto cancelled = dt::EncodeOwnedBlobComponentV3(f.profile, dt::BlobValueStateV3::value,
        source, grant, output, {1,during.get_token()});
    stop_on_allocation = nullptr;
    Check(cancelled.failure == Failure::cancelled && !output.bound(),
          "cancellation during actual resource allocation published");
    Check(grant.get() == original && f.manager.Snapshot().current_bytes == baseline && !output.bound(),
          "private refusal/cancellation changed grant/output/backing");
    std::unique_ptr<mem::ReservationBackedMemoryResource> absent;
    Check(dt::CopyBlobPersistenceBytesV3(absent, abc, output).failure == Failure::invalid_grant,
          "missing actual physical grant admitted");
    auto occupied = grant->Allocate({1,1,"occupied"});
    Check(occupied.ok(), "occupied grant setup failed");
    Check(dt::CopyBlobPersistenceBytesV3(grant, abc, output).failure == Failure::invalid_grant,
          "shared/occupied grant admitted");
    Check(grant->DeallocateNoAlloc(occupied.pointer,1,1).ok(), "occupied test allocation cleanup failed");
    Check(dt::CopyBlobPersistenceBytesV3(grant, abc, output).ok(), "valid copy after refusal failed");
    auto extra = f.Grant();
    Check(dt::CopyBlobPersistenceBytesV3(extra, abc, output).failure == Failure::output_in_use && extra,
          "occupied output replaced");
    Check(dt::EncodeOwnedBlobComponentV3(f.profile, dt::BlobValueStateV3::value,
        source, extra, source).failure == Failure::output_in_use, "self-alias source overwritten");
  }
  Clean(f);
}
void MalformedFrames() {
  Fixture f;
  const auto golden = Golden(3);
  const auto verify = [&](const std::vector<byte>& bytes, bool nullable = true) {
    dt::BlobOwnedPersistenceBytesV3 source; f.Copy(bytes, source);
    auto grant = f.Grant(); auto* original = grant.get();
    const auto before = f.manager.Snapshot().current_bytes;
    const auto header = std::span<const byte>(bytes).first(std::min<std::size_t>(320,bytes.size()));
    auto expected = dt::ValidateBlobComponentHeaderNoAllocV3(f.profile, bytes.size(), header, nullable);
    if (expected.ok()) {
      dt::BlobComponentDigestPassV3 pass;
      expected = pass.BeginDecode(f.profile, bytes.size(), header, nullable);
      if (expected.ok()) expected = pass.UpdateContent(std::span<const byte>(bytes).subspan(320));
      if (expected.ok()) expected = pass.FinishDecode();
    }
    Check(!expected.ok(), "malformed fixture unexpectedly valid");
    dt::BlobOwnedPersistenceDecodedV3 out;
    const auto actual = dt::DecodeOwnedBlobComponentV3(f.profile, source, nullable, grant, out, {1});
    Check(actual.failure == Failure::component_failed &&
        actual.component.failure == expected.failure && actual.component.diagnostic_uuid == expected.diagnostic_uuid &&
        actual.component.diagnostic_generation == expected.diagnostic_generation &&
        actual.component.reason_enum == expected.reason_enum && actual.component.format_enum == expected.format_enum &&
        actual.component.failure_offset_present == expected.failure_offset_present &&
        actual.component.failure_offset == expected.failure_offset, "complete decoder lost exact diagnostic precedence");
    Check(grant.get() == original && !out.content().bound() &&
        f.manager.Snapshot().current_bytes == before && grant->Snapshot().allocated_bytes == 0,
        "malformed decode published/consumed grant/leaked physical staging");
    if (expected.failure==dt::BlobComponentPassFailureV3::integrity_failed)
      Check(actual.digest_bytes==bytes.size()-320 && actual.copied_bytes==bytes.size()-320 &&
          actual.header_bytes_written==0,"integrity failure erased discarded actual content work");
  };
  for (std::size_t length = 0; length < golden.size(); ++length)
    verify(std::vector<byte>(golden.begin(), golden.begin()+length));
  for (std::size_t at = 0; at < golden.size(); ++at) {
    auto bad = golden; bad[at] ^= 0x80; verify(bad);
  }
  auto trailing = golden; trailing.push_back(0); verify(trailing);
  verify(Golden(0), false);
  // Structural header failure wins BEFORE later hash/output resource admission.
  auto bad = golden; bad[0] = 'X';
  dt::BlobOwnedPersistenceBytesV3 source; f.Copy(bad, source);
  std::unique_ptr<mem::ReservationBackedMemoryResource> no_grant;
  dt::BlobOwnedPersistenceDecodedV3 out;
  allocation_budget = 0; allocation_fault_triggered = false;
  const auto refused = dt::DecodeOwnedBlobComponentV3(f.profile, source, true, no_grant, out);
  allocation_budget = -1;
  Check(refused.component.reason_enum == 42 && !allocation_fault_triggered,
        "malformed header attempted later allocation or lost initial precedence");
}
void Quotas() {
  for (const bool decode : {false,true}) {
    Fixture calibration;
    std::size_t required = 0;
    {
      dt::BlobOwnedPersistenceBytesV3 source; const auto frame = Golden(3);
      calibration.Copy(decode ? std::span<const byte>(frame) : std::span<const byte>(frame).subspan(320), source);
      const auto before = calibration.manager.Snapshot().current_bytes;
      auto grant = calibration.Grant(); dt::BlobOwnedPersistenceBytesV3 encoded;
      dt::BlobOwnedPersistenceDecodedV3 decoded;
      Check((decode ? dt::DecodeOwnedBlobComponentV3(calibration.profile, source,true,grant,decoded)
          : dt::EncodeOwnedBlobComponentV3(calibration.profile,dt::BlobValueStateV3::value,source,grant,encoded)).ok(),
          "quota calibration failed");
      required = calibration.manager.Snapshot().peak_bytes-before;
    }
    Clean(calibration);
    for (auto quota : {std::size_t{1},std::size_t{320},required-1,required}) {
      Fixture f;
      {
        dt::BlobOwnedPersistenceBytesV3 source; const auto frame = Golden(3);
        f.Copy(decode ? std::span<const byte>(frame) : std::span<const byte>(frame).subspan(320), source);
        const auto before = f.manager.Snapshot().current_bytes;
        auto grant = f.Grant(quota); auto* original = grant.get();
        dt::BlobOwnedPersistenceBytesV3 encoded; dt::BlobOwnedPersistenceDecodedV3 decoded;
        const auto result = decode ? dt::DecodeOwnedBlobComponentV3(f.profile,source,true,grant,decoded)
            : dt::EncodeOwnedBlobComponentV3(f.profile,dt::BlobValueStateV3::value,source,grant,encoded);
        Check(result.ok() == (quota==required), "exact physical workspace/output quota not enforced");
        if (!result.ok()) Check(grant.get()==original && !encoded.bound() && !decoded.content().bound() &&
            grant->Snapshot().allocated_bytes==0 && f.manager.Snapshot().current_bytes==before,
            "quota refusal changed source/grant/physical ownership");
      }
      Clean(f);
    }
  }
}
void AllocationFaults() {
  for (unsigned operation = 0; operation < 3; ++operation) {
    unsigned positions = 0, post_hash_failures = 0;
    for (long fault = -1; fault < static_cast<long>(positions); ++fault) {
      Fixture f;
      {
        const auto frame = Golden(3); dt::BlobOwnedPersistenceBytesV3 source;
        f.Copy(operation==2 ? std::span<const byte>(frame) : std::span<const byte>(frame).subspan(320),source);
        auto grant = f.Grant(); auto* original = grant.get();
        const auto baseline = f.manager.Snapshot().current_bytes;
        dt::BlobOwnedPersistenceBytesV3 bytes; dt::BlobOwnedPersistenceDecodedV3 decoded;
        allocation_count = 0; allocation_fault_triggered = false; allocation_budget = fault;
        const auto result = operation==0 ? dt::CopyBlobPersistenceBytesV3(grant,source.bytes(),bytes)
            : operation==1 ? dt::EncodeOwnedBlobComponentV3(f.profile,dt::BlobValueStateV3::value,source,grant,bytes)
            : dt::DecodeOwnedBlobComponentV3(f.profile,source,true,grant,decoded);
        allocation_budget = -1;
        if (fault==-1) { positions = allocation_count; Check(result.ok(),"fault calibration failed"); }
        else Check(allocation_fault_triggered,"declared owning allocation fault did not fire");
        if (result.ok()) Check(!grant && (operation==2 ? decoded.content().bound() : bytes.bound()),
            "successful optional allocation failure lost complete owner");
        else {
          Check(grant.get()==original && !bytes.bound() && !decoded.content().bound() &&
              grant->Snapshot().allocated_bytes==0 && f.manager.Snapshot().current_bytes==baseline,
              "allocation fault lost output/grant/physical failure atomicity");
          if (operation==1 && result.digest_bytes==3) ++post_hash_failures;
        }
      }
      Clean(f);
    }
    Check(positions>0,"no actual owning allocation faults observed");
    if (operation==1) Check(post_hash_failures>0,"post-hash exception erased actual observed digest work");
    std::cout << "owned_blob_fault_positions[" << operation << "]=" << positions << '\n';
  }
}
void ConcurrentReaders() {
  Fixture f;
  {
    const auto frame = Golden(3); dt::BlobOwnedPersistenceBytesV3 source; f.Copy(frame,source);
    std::atomic<unsigned> failures{0}; std::vector<std::thread> readers;
    for (unsigned i=0;i<4;++i) readers.emplace_back([&] {
      try {
        for (unsigned n=0;n<32;++n) {
          auto grant=f.Grant(); dt::BlobOwnedPersistenceDecodedV3 decoded;
          if (!dt::DecodeOwnedBlobComponentV3(f.profile,source,true,grant,decoded,{1}).ok() ||
              !Equal(decoded.content().bytes(),std::span<const byte>(frame).subspan(320))) ++failures;
        }
      } catch (...) { ++failures; }
    });
    for (auto& reader:readers) reader.join();
    Check(failures==0,"concurrent immutable owned decode corrupted shared source/custody");
  }
  Clean(f);
}
void PhysicalAllocatorFaults() {
  for (unsigned operation=0;operation<3;++operation) {
    for (unsigned sequence=1;sequence<=(operation==0 ? 1u : 2u);++sequence) {
      Fixture f;
      {
        const auto frame=Golden(3); dt::BlobOwnedPersistenceBytesV3 source;
        f.Copy(operation==2 ? std::span<const byte>(frame) : std::span<const byte>(frame).subspan(320),source);
        auto grant=f.Grant(); auto* original=grant.get();
        const auto before=f.manager.Snapshot().current_bytes;
        mem::MemoryFailureInjectionConfiguration fault;
        fault.test_guard=mem::MakeMemoryFailureInjectionTestGuard(); fault.fixture_enabled=true;
        fault.fixture_name="owned_blob_persistence"; fault.evidence_note="actual scoped physical backing failure";
        mem::MemoryFailureInjectionRule rule; rule.rule_id="owned_blob_actual_backing";
        rule.scope_kind=mem::MemoryFailureInjectionScopeKind::context;
        rule.binary_scope_uuid=Fixture::Id(3); rule.fail_on_matched_sequence=sequence;
        fault.rules.push_back(rule);
        Check(f.manager.EnableAllocationFailureInjection(fault).ok(),"native allocator fault enable failed");
        dt::BlobOwnedPersistenceBytesV3 bytes; dt::BlobOwnedPersistenceDecodedV3 decoded;
        const auto result=operation==0 ? dt::CopyBlobPersistenceBytesV3(grant,source.bytes(),bytes)
            : operation==1 ? dt::EncodeOwnedBlobComponentV3(f.profile,dt::BlobValueStateV3::value,source,grant,bytes)
            : dt::DecodeOwnedBlobComponentV3(f.profile,source,true,grant,decoded);
        Check(f.manager.DisableAllocationFailureInjection().ok(),"native allocator fault disable failed");
        Check(result.failure==Failure::allocation_failed && !result.memory_diagnostic.diagnostic_code.empty() &&
              grant.get()==original && !bytes.bound() && !decoded.content().bound() &&
              grant->Snapshot().allocated_bytes==0 && f.manager.Snapshot().current_bytes==before,
              "actual backing failure lost vector/physical atomicity");
        if (operation==1 && sequence==2) Check(result.digest_bytes==3,"physical output failure erased first-pass work");
      }
      Clean(f);
    }
  }
}
void CancellationAtEveryAllocation() {
  for (unsigned operation=0;operation<3;++operation) {
    unsigned positions=0;
    for (long step=-1;step<static_cast<long>(positions);++step) {
      Fixture f;
      {
        const auto frame=Golden(3); dt::BlobOwnedPersistenceBytesV3 source;
        f.Copy(operation==2 ? std::span<const byte>(frame) : std::span<const byte>(frame).subspan(320),source);
        auto grant=f.Grant(); auto* original=grant.get();
        const auto before=f.manager.Snapshot().current_bytes;
        dt::BlobOwnedPersistenceBytesV3 bytes; dt::BlobOwnedPersistenceDecodedV3 decoded;
        std::stop_source stop;
        allocation_count=0;
        if (step>=0) { stop_on_allocation=&stop; stop_allocation_budget=step; }
        const auto result=operation==0 ? dt::CopyBlobPersistenceBytesV3(grant,source.bytes(),bytes,{1,stop.get_token()})
            : operation==1 ? dt::EncodeOwnedBlobComponentV3(f.profile,dt::BlobValueStateV3::value,source,grant,bytes,{1,stop.get_token()})
            : dt::DecodeOwnedBlobComponentV3(f.profile,source,true,grant,decoded,{1,stop.get_token()});
        stop_on_allocation=nullptr;
        if (step==-1) { positions=allocation_count; Check(result.ok(),"cancellation calibration failed"); }
        else Check(stop.stop_requested() && result.failure==Failure::cancelled &&
              grant.get()==original && !bytes.bound() && !decoded.content().bound() &&
              grant->Snapshot().allocated_bytes==0 && f.manager.Snapshot().current_bytes==before,
              "actual allocation-boundary cancellation published or lost cleanup custody");
      }
      Clean(f);
    }
    std::cout << "owned_blob_cancellation_positions[" << operation << "]=" << positions << '\n';
  }
}
int Reopen(const char* path) {
  Fixture f; disk::FileDevice device;
  Check(device.Open(path,disk::FileOpenMode::open_existing_read_only).ok(),"independent owned frame open failed");
  std::size_t offset=0;
  for (unsigned i=0;i<5;++i) {
    const auto expected=Golden(i); std::vector<byte> bytes(expected.size());
    const auto read=device.ReadAt(offset,bytes.data(),bytes.size());
    Check(read.ok() && read.bytes_transferred==bytes.size() && bytes==expected,"reopened component changed exact bytes");
    {
      dt::BlobOwnedPersistenceBytesV3 source; f.Copy(bytes,source);
      auto grant=f.Grant(); dt::BlobOwnedPersistenceDecodedV3 decoded;
      Check(dt::DecodeOwnedBlobComponentV3(f.profile,source,true,grant,decoded).ok() &&
          decoded.state()==kBlobComponentGoldenVectors[i].state &&
          Equal(decoded.content().bytes(),std::span<const byte>(expected).subspan(320)),
          "independent complete persistence decode failed");
    }
    Clean(f); offset+=expected.size();
  }
  const auto size=device.Size();
  Check(size.ok() && size.size_bytes==offset,"persisted component stream trailing/missing bytes");
  Check(device.Close().ok(),"independent persistence device close failed"); return 0;
}
void FilePersistence(const char* executable) {
  scratchbird::tests::OwnedTempDirectory temporary;
  const auto path=temporary.path()/"owned-native-components.bin";
  Fixture f;
  {
    disk::FileDevice device;
    Check(device.Open(path.string(),disk::FileOpenMode::create_new).ok(),"owned frame file creation failed");
    std::size_t offset=0;
    for (unsigned i=0;i<5;++i) {
      const auto expected=Golden(i);
      {
        dt::BlobOwnedPersistenceBytesV3 source,encoded;
        f.Copy(std::span<const byte>(expected).subspan(320),source); auto grant=f.Grant();
        Check(dt::EncodeOwnedBlobComponentV3(f.profile,kBlobComponentGoldenVectors[i].state,
              source,grant,encoded).ok() && Equal(encoded.bytes(),expected),"persisted encoder is not exact native codec");
        const auto write=device.WriteAt(offset,encoded.bytes().data(),encoded.bytes().size());
        Check(write.ok() && write.bytes_transferred==expected.size(),"actual native component write failed");
      }
      offset+=expected.size(); Clean(f);
    }
    Check(device.Sync().ok(),"actual native component synchronization failed");
    Check(device.Close().ok(),"actual native component close failed");
  }
  const auto pid=::fork(); Check(pid>=0,"independent persistence reader fork failed");
  if (pid==0) { ::execl(executable,executable,"--reopen",path.c_str(),static_cast<char*>(nullptr)); ::_exit(127); }
  int status=0;
  Check(::waitpid(pid,&status,0)==pid && WIFEXITED(status) && WEXITSTATUS(status)==0,
        "independent fresh process persistence decode failed");
  temporary.Cleanup();
}
} // namespace
int main(int argc,char** argv) {
  try {
    if (argc==3 && std::string_view(argv[1])=="--reopen") return Reopen(argv[2]);
    GoldenRoundTrips(); LongBinaryRoundTrips(); PrivateRefusalsAndCancellation();
    MalformedFrames(); Quotas(); AllocationFaults(); PhysicalAllocatorFaults();
    CancellationAtEveryAllocation(); ConcurrentReaders();
    const auto executable=std::filesystem::absolute(argv[0]); FilePersistence(executable.c_str());
    std::cout << "owned_blob_persistence_checks=" << checks.load() << '\n'; return 0;
  } catch (const std::exception& error) {
    allocation_budget=-1; stop_on_allocation=nullptr;
    std::cerr << "owned BLOB persistence failure: " << error.what() << '\n'; return 1;
  }
}
