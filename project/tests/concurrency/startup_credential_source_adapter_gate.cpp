// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
//
// Proves the adapter produces exactly the custody object the real transport
// accepts. The assertions are against the actual transport predicate, not a
// restatement of it: a sealed object that this test accepts but
// ImportStartupCredentialTransport would reject is a failure of the adapter.

#include "../../src/server/startup_credential_source_adapter.hpp"
#include "../../src/server/startup_credential_transport.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

int checks = 0;

void Check(bool pass, const char* what) {
  ++checks;
  if (!pass) {
    std::fprintf(stderr, "FAIL startup credential source adapter: %s\n", what);
    std::exit(1);
  }
}

scratchbird::server::StartupCredentialTransportBinding Binding() {
  using scratchbird::core::platform::Uuid;
  scratchbird::server::StartupCredentialTransportBinding binding;
  // Engine identity shape; the frame encoder validates these, so placeholder
  // zero UUIDs would be refused rather than silently accepted.
  const auto identity = [](std::uint8_t tail) {
    Uuid value{{0x01, 0x8f, 0x7a, 0x10, 0x12, 0x80, 0x70, 0x00, 0x80, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00}};
    value.bytes[15] = tail;
    return value;
  };
  binding.database = identity(0x11);
  binding.principal = identity(0x12);
  binding.provider = identity(0x13);
  binding.credential_reference = identity(0x14);
  binding.generation = 1;
  binding.custodian_uid = static_cast<std::uint32_t>(::geteuid());
  return binding;
}

}  // namespace

int main() {
  using namespace scratchbird::server;
  using E = StartupCredentialSourceAdapterError;

  const std::string credential =
      "local-password-pbkdf2-sha256:v1:iterations=600000:salt=00112233445566"
      "778899aabbccddeeff:verifier=00112233445566778899aabbccddeeff001122334"
      "45566778899aabbccddeeff";

  // 1. The custody predicate the transport enforces, checked field by field.
  {
    auto created = CreateSealedStartupCredentialSource(Binding(), credential);
    Check(created.ok(), "sealed source is created");
    struct stat state{};
    Check(::fstat(created.fd, &state) == 0, "created descriptor is stattable");
    Check(S_ISREG(state.st_mode), "custody object is a regular file");
    Check(state.st_nlink == 0, "custody object is anonymous, reachable by no path");
    Check((state.st_mode & 07777) == 0400, "custody object is owner-read-only");
    Check(state.st_uid == static_cast<uid_t>(::geteuid()),
          "custody object is owned by the custodian");
    const int seals = ::fcntl(created.fd, F_GET_SEALS);
    Check(seals >= 0, "seals are readable");
    Check((seals & F_SEAL_WRITE) != 0, "frame is immutable");
    Check((seals & F_SEAL_SHRINK) != 0, "frame cannot shrink");
    Check((seals & F_SEAL_GROW) != 0, "frame cannot grow");
    Check((seals & F_SEAL_SEAL) != 0, "seals cannot be removed by a later holder");
    Check(static_cast<std::size_t>(state.st_size) ==
              kStartupCredentialHeaderBytes + credential.size(),
          "frame extent is header plus credential");
    // Sealed write must actually fail, not merely be declared sealed.
    const char byte = 'x';
    Check(::pwrite(created.fd, &byte, 1, 0) < 0,
          "a sealed frame refuses a write");
    ::close(created.fd);
  }

  // 2. Refusals return no descriptor. A refusal that leaked a usable fd would
  //    be worse than a failure.
  {
    auto empty = CreateSealedStartupCredentialSource(Binding(), {});
    Check(!empty.ok() && empty.fd < 0 && empty.error == E::invalid_request,
          "an empty credential is refused with no descriptor");

    auto oversized = CreateSealedStartupCredentialSource(
        Binding(), std::string(kStartupCredentialMaximumBytes + 1, 'a'));
    Check(!oversized.ok() && oversized.fd < 0 &&
              oversized.error == E::invalid_request,
          "an oversized credential is refused with no descriptor");

    auto embedded_nul = CreateSealedStartupCredentialSource(
        Binding(), std::string("abc\0def", 7));
    Check(!embedded_nul.ok() && embedded_nul.fd < 0 &&
              embedded_nul.error == E::frame,
          "a credential containing NUL is refused by the frame encoder");

    auto no_custodian = Binding();
    no_custodian.custodian_uid.reset();
    auto missing = CreateSealedStartupCredentialSource(no_custodian, credential);
    Check(!missing.ok() && missing.fd < 0 && missing.error == E::invalid_request,
          "an absent custodian is refused with no descriptor");

    auto wrong_custodian = Binding();
    wrong_custodian.custodian_uid =
        static_cast<std::uint32_t>(::geteuid()) + 1u;
    auto mismatched =
        CreateSealedStartupCredentialSource(wrong_custodian, credential);
    Check(!mismatched.ok() && mismatched.fd < 0 &&
              mismatched.error == E::invalid_request,
          "a custodian other than this process is refused");
  }

  // 3. The frame the transport will inspect binds to the declared binding and
  //    rejects a different one, so custody is not transferable by relabelling.
  {
    auto created = CreateSealedStartupCredentialSource(Binding(), credential);
    Check(created.ok(), "sealed source for frame inspection");
    std::uint8_t header[kStartupCredentialHeaderBytes]{};
    Check(::pread(created.fd, header, sizeof(header), 0) ==
              static_cast<ssize_t>(sizeof(header)),
          "frame header is readable");
    const std::size_t total_bytes =
        kStartupCredentialHeaderBytes + credential.size();
    const auto matched = InspectStartupCredentialFrameHeader(
        std::span<const std::uint8_t>(header, sizeof(header)), total_bytes,
        Binding());
    Check(matched.error == StartupCredentialFrameError::none,
          "frame header matches the binding it was built against");
    Check(matched.credential_bytes == credential.size(),
          "frame header reports the exact credential extent");
    auto other = Binding();
    other.generation = 2;
    const auto rejected = InspectStartupCredentialFrameHeader(
        std::span<const std::uint8_t>(header, sizeof(header)), total_bytes,
        other);
    Check(rejected.error != StartupCredentialFrameError::none,
          "frame header refuses a different generation");
    ::close(created.fd);
  }

  // 4. The assertion that is not a restatement of the predicate: hand the
  //    adapter's descriptor to the real importer. Groups 1-3 check custody
  //    fields as this test reads them, which is circular because the same
  //    author wrote both readings. This one fails if the adapter and the
  //    transport disagree about anything at all, including whatever the
  //    transport checks that this test never thought to.
  {
    namespace mem = scratchbird::core::memory;
    const auto binding = Binding();
    auto created = CreateSealedStartupCredentialSource(binding, credential);
    Check(created.ok(), "sealed source for transport round-trip");

    auto policy = mem::DefaultLocalEngineMemoryPolicy();
    const std::size_t page_size =
        static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
    policy.hard_limit_bytes = page_size * 2;
    policy.per_context_limit_bytes = page_size * 2;
    policy.soft_limit_bytes = 0;
    mem::MemoryManager manager(policy);
    mem::MemoryTag tag;
    tag.binary_ownership[mem::MemoryBinaryScopeKind::context] =
        binding.database.bytes;
    tag.binary_ownership[mem::MemoryBinaryScopeKind::owner] =
        binding.principal.bytes;
    tag.binary_ownership[mem::MemoryBinaryScopeKind::database] =
        binding.database.bytes;

    auto imported =
        ImportStartupCredentialTransport(created.fd, binding, manager, tag);
    Check(imported.error == StartupCredentialTransportError::none,
          "the real transport accepts the adapter's sealed custody object");
    Check(imported.ok(), "import yields a valid protected buffer");
    Check(imported.credential_size == credential.size(),
          "imported credential extent matches what was sealed");
    Check(imported.credential() == credential,
          "imported credential bytes are byte-for-byte what was sealed");

    // The transport must also refuse the same object under a binding it was
    // not framed against, or the adapter's framing would be decorative.
    auto other = binding;
    other.generation = binding.generation + 1;
    Check(ImportStartupCredentialTransport(created.fd, other, manager, tag)
                  .error != StartupCredentialTransportError::none,
          "the transport refuses the object under a different generation");

    ::close(created.fd);
  }

  std::printf("PASS startup credential source adapter checks=%d\n", checks);
  return 0;
}
