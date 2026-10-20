// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
//
// Proves the server can obtain an operator-provisioned secret and publish it as
// real custody, and that every way of not having one is a refusal rather than a
// quietly uncredentialed start. The closing group is the one that matters: it
// acquires the published material back and requires the exact provisioned bytes,
// so the path is proven end to end rather than at its seams.

#include "../../src/server/startup_credential_provisioning.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace mem = scratchbird::core::memory;
using namespace scratchbird::server;
using E = StartupCredentialProvisionError;

namespace {

int checks = 0;

void Check(bool pass, const char* what) {
  ++checks;
  if (!pass) {
    std::fprintf(stderr, "FAIL startup credential provisioning: %s\n", what);
    std::exit(1);
  }
}

scratchbird::core::platform::Uuid Id(std::uint8_t tail) {
  scratchbird::core::platform::Uuid value{{0x01, 0x8f, 0x7a, 0x10, 0x12, 0x80,
                                           0x70, 0x00, 0x80, 0x00, 0x00, 0x00,
                                           0x00, 0x00, 0x00, 0x00}};
  value.bytes[15] = tail;
  return value;
}

StartupCredentialTransportBinding Binding() {
  StartupCredentialTransportBinding binding;
  binding.database = Id(0x21);
  binding.principal = Id(0x22);
  binding.provider = Id(0x23);
  binding.credential_reference = Id(0x24);
  binding.generation = 9;
  binding.custodian_uid = static_cast<std::uint32_t>(::geteuid());
  return binding;
}

mem::MemoryTag Tag(const StartupCredentialTransportBinding& binding) {
  mem::MemoryTag tag;
  tag.category = mem::MemoryCategory::core_runtime;
  tag.purpose = "server startup credential provisioning with heap-backed purpose metadata";
  tag.callsite = "startup credential provisioning gate with retained heap-backed callsite metadata";
  tag.binary_ownership[mem::MemoryBinaryScopeKind::database] = binding.database.bytes;
  tag.binary_ownership[mem::MemoryBinaryScopeKind::owner] = binding.principal.bytes;
  tag.binary_ownership[mem::MemoryBinaryScopeKind::context] = binding.database.bytes;
  return tag;
}

// Writes a provisioned secret file with an explicit mode, as an operator would.
std::string WriteSecretFile(const char* suffix, std::string_view contents,
                            mode_t mode) {
  std::string path = std::string("/tmp/sb-provision-gate-") +
                     std::to_string(::getpid()) + "-" + suffix;
  ::unlink(path.c_str());
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode);
  Check(fd >= 0, "operator secret file created");
  Check(::write(fd, contents.data(), contents.size()) ==
            static_cast<ssize_t>(contents.size()),
        "operator secret file written");
  Check(::fchmod(fd, mode) == 0, "operator secret file mode applied");
  ::close(fd);
  return path;
}

StartupCredentialProvisionRequest FileRequest(const std::string& path) {
  StartupCredentialProvisionRequest request;
  request.source = StartupCredentialProvisionSource::operator_file;
  request.name_or_path = path;
  request.binding = Binding();
  request.publish = {Id(0x31), Id(0x32), 0, {}, {}};
  return request;
}

}  // namespace

int main() {
  const std::string secret =
      "local-password-pbkdf2-sha256:v1:iterations=600000:salt=0011223344556677"
      "8899aabbccddeeff:verifier=00112233445566778899aabbccddeeff001122334455"
      "66778899aabbccddeeff";

  // 1. Every way of not having a usable secret is a refusal. None of these may
  //    produce a published source, because a server that starts uncredentialed
  //    while reporting success is the failure this whole path exists to prevent.
  {
    const auto binding = Binding();
    const auto tag = Tag(binding);

    struct Case {
      const char* what;
      StartupCredentialProvisionSource source;
      std::string path;
      E expected;
    };

    const auto world = WriteSecretFile("world", secret, 0444);
    const auto group = WriteSecretFile("group", secret, 0440);
    const auto empty = WriteSecretFile("empty", "", 0400);
    const auto newline_only = WriteSecretFile("newline", "\n", 0400);
    const std::string oversized_text(kStartupCredentialMaximumBytes + 8, 'x');
    const auto oversized = WriteSecretFile("oversized", oversized_text, 0400);

    const Case cases[] = {
        {"an unconfigured source refuses",
         StartupCredentialProvisionSource::not_configured, {}, E::not_configured},
        {"a configured source with no name refuses",
         StartupCredentialProvisionSource::operator_file, {}, E::not_configured},
        {"a missing file refuses",
         StartupCredentialProvisionSource::operator_file,
         "/tmp/sb-provision-gate-does-not-exist", E::unreadable},
        {"a world-readable secret refuses on its permissions",
         StartupCredentialProvisionSource::operator_file, world, E::custody_rejected_mode},
        {"a group-readable secret refuses on its permissions",
         StartupCredentialProvisionSource::operator_file, group, E::custody_rejected_mode},
        {"a directory refuses on its type",
         StartupCredentialProvisionSource::operator_file, "/tmp", E::custody_rejected_type},
        {"an empty secret refuses",
         StartupCredentialProvisionSource::operator_file, empty, E::empty},
        {"a secret that is only a newline refuses",
         StartupCredentialProvisionSource::operator_file, newline_only, E::empty},
        {"an oversized secret refuses",
         StartupCredentialProvisionSource::operator_file, oversized, E::oversized},
    };

    for (const auto& c : cases) {
      mem::MemoryManager manager(mem::DefaultLocalEngineMemoryPolicy());
      StartupCredentialSource source(Id(0x31), 4);
      StartupCredentialProvisionRequest request;
      request.source = c.source;
      request.name_or_path = c.path;
      request.binding = binding;
      request.publish = {Id(0x31), Id(0x32), 0, {}, {}};
      const auto out = ProvisionStartupCredential(request, source, manager, tag);
      Check(out.error == c.expected, c.what);
      Check(!out.ok(), "a refusal never reports a provisioned credential");
      Check(StartupCredentialProvisionDiagnosticCode(out.error)[0] != '\0',
            "a refusal carries a diagnostic code");
      source.Close();
    }

    for (const auto* p : {&world, &group, &empty, &newline_only, &oversized})
      ::unlink(p->c_str());
  }

  // 1b. A secret owned by someone other than this process is refused on its
  //     ownership specifically. This needs a foreign-owned file, which the
  //     earlier group cannot create, so it was the one custody conjunct a
  //     mutation could delete without any assertion noticing. Both branches
  //     below isolate ownership: the file's other properties are acceptable, or
  //     would report a different error if they were not.
  {
    const auto binding = Binding();
    const auto tag = Tag(binding);
    std::string foreign;
    bool usable = false;

    if (::geteuid() == 0) {
      // As root, manufacture the case directly.
      foreign = WriteSecretFile("foreign", secret, 0400);
      usable = ::chown(foreign.c_str(), 65534, 65534) == 0;
    } else {
      // As an ordinary user, a root-owned readable file serves. Ownership is
      // checked before permissions, so this reports ownership and not mode.
      struct ::stat info {};
      if (::stat("/etc/passwd", &info) == 0 && S_ISREG(info.st_mode) &&
          info.st_uid != ::geteuid() && ::access("/etc/passwd", R_OK) == 0) {
        foreign = "/etc/passwd";
        usable = true;
      }
    }

    if (usable) {
      mem::MemoryManager manager(mem::DefaultLocalEngineMemoryPolicy());
      StartupCredentialSource source(Id(0x31), 4);
      const auto out =
          ProvisionStartupCredential(FileRequest(foreign), source, manager, tag);
      Check(out.error == E::custody_rejected_owner,
            "a secret owned by another user refuses on its ownership");
      Check(!out.ok(), "that refusal publishes nothing");
      source.Close();
      if (::geteuid() == 0) ::unlink(foreign.c_str());
    } else {
      // Never silently skip: an unrunnable check is reported, not passed.
      std::fprintf(stderr,
                   "NOTE startup credential provisioning: ownership refusal "
                   "unverifiable in this environment (no foreign-owned readable "
                   "file available)\n");
    }
  }

  // 1c. Two behaviours the header claims. A documented property with no test is
  //     a claim, not a guarantee, so each is asserted rather than described.
  {
    const auto binding = Binding();
    const auto tag = Tag(binding);

    // O_NOFOLLOW: a symlink is refused outright rather than followed to a
    // target whose custody was never checked. Without it, an attacker who can
    // replace the configured path with a link redirects the read.
    {
      const auto target = WriteSecretFile("linktarget", secret, 0400);
      const std::string link = "/tmp/sb-provision-gate-" +
                               std::to_string(::getpid()) + "-link";
      ::unlink(link.c_str());
      Check(::symlink(target.c_str(), link.c_str()) == 0, "symlink staged");

      mem::MemoryManager manager(mem::DefaultLocalEngineMemoryPolicy());
      StartupCredentialSource source(Id(0x31), 4);
      const auto out =
          ProvisionStartupCredential(FileRequest(link), source, manager, tag);
      Check(out.error == E::unreadable,
            "a symlinked secret is refused rather than followed");
      Check(!out.ok(), "that refusal publishes nothing");
      source.Close();
      ::unlink(link.c_str());
      ::unlink(target.c_str());
    }

    // Exactly one trailing newline is punctuation. A secret that genuinely ends
    // in a newline is provisioned by appending a second, which only works if
    // the trim is not greedy.
    {
      const std::string ends_with_newline = secret + "\n";
      const auto path = WriteSecretFile("twonewlines", ends_with_newline + "\n", 0400);

      mem::MemoryManager manager(mem::DefaultLocalEngineMemoryPolicy());
      StartupCredentialSource source(Id(0x31), 4);
      const auto out =
          ProvisionStartupCredential(FileRequest(path), source, manager, tag);
      Check(out.ok(), "a secret ending in a newline provisions");
      StartupCredentialSource::Request acquire{Id(0x31), Id(0x34),
                                               binding.generation, {}, {}};
      auto material = source.Acquire(acquire, manager, tag);
      Check(material.outcome.ok() && material.lease, "it is acquirable");
      Check(material.lease->credential() == ends_with_newline,
            "exactly one trailing newline is removed, so a secret that ends in "
            "one survives");
      material.lease.reset();
      source.Close();
      ::unlink(path.c_str());
    }
  }

  // 2. Configured for the service manager but given nothing by it: refuse
  //    rather than fall back to a path. A fallback would silently accept weaker
  //    custody than the operator configured.
  {
    const auto binding = Binding();
    const auto tag = Tag(binding);
    const char* saved = std::getenv("CREDENTIALS_DIRECTORY");
    const std::string restore = saved ? saved : "";
    ::unsetenv("CREDENTIALS_DIRECTORY");

    mem::MemoryManager manager(mem::DefaultLocalEngineMemoryPolicy());
    StartupCredentialSource source(Id(0x31), 4);
    StartupCredentialProvisionRequest request;
    request.source = StartupCredentialProvisionSource::service_manager;
    request.name_or_path = "sb-startup";
    request.binding = binding;
    request.publish = {Id(0x31), Id(0x32), 0, {}, {}};
    const auto out = ProvisionStartupCredential(request, source, manager, tag);
    Check(out.error == E::no_service_credentials,
          "service-manager source with no credentials passed refuses");
    Check(!out.ok(), "that refusal publishes nothing");
    source.Close();

    if (!restore.empty()) ::setenv("CREDENTIALS_DIRECTORY", restore.c_str(), 1);
  }

  // 3. The service-manager source reads from the directory the service manager
  //    actually passed, not from a path of our choosing.
  {
    const auto binding = Binding();
    const auto tag = Tag(binding);
    std::string directory = "/tmp/sb-provision-gate-creds-" + std::to_string(::getpid());
    ::mkdir(directory.c_str(), 0700);
    const std::string file = directory + "/sb-startup";
    ::unlink(file.c_str());
    const int fd = ::open(file.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0400);
    Check(fd >= 0 && ::write(fd, secret.data(), secret.size()) ==
                         static_cast<ssize_t>(secret.size()),
          "service manager credential staged");
    ::close(fd);
    const char* saved = std::getenv("CREDENTIALS_DIRECTORY");
    const std::string restore = saved ? saved : "";
    ::setenv("CREDENTIALS_DIRECTORY", directory.c_str(), 1);

    mem::MemoryManager manager(mem::DefaultLocalEngineMemoryPolicy());
    StartupCredentialSource source(Id(0x31), 4);
    StartupCredentialProvisionRequest request;
    request.source = StartupCredentialProvisionSource::service_manager;
    request.name_or_path = "sb-startup";
    request.binding = binding;
    request.publish = {Id(0x31), Id(0x32), 0, {}, {}};
    const auto out = ProvisionStartupCredential(request, source, manager, tag);
    Check(out.ok(), "service-manager provisioned secret publishes");
    source.Close();

    if (restore.empty()) ::unsetenv("CREDENTIALS_DIRECTORY");
    else ::setenv("CREDENTIALS_DIRECTORY", restore.c_str(), 1);
    ::unlink(file.c_str());
    ::rmdir(directory.c_str());
  }

  // 4. The assertion that proves the path rather than its seams: provision from
  //    an operator file, then acquire the published material back through the
  //    same interface startup uses and require the exact provisioned bytes. A
  //    disagreement anywhere between file, seal, publish, import and lease
  //    fails here.
  {
    const auto binding = Binding();
    const auto tag = Tag(binding);
    const auto path = WriteSecretFile("good", secret + "\n", 0400);

    mem::MemoryManager manager(mem::DefaultLocalEngineMemoryPolicy());
    StartupCredentialSource source(Id(0x31), 4);
    const auto out = ProvisionStartupCredential(FileRequest(path), source, manager, tag);
    Check(out.ok(), "operator-provisioned secret publishes");
    Check(out.error == E::none, "publication reports no error");

    // Acquisition is bound to the published generation, so startup cannot read
    // material that a later publication has superseded.
    StartupCredentialSource::Request acquire{Id(0x31), Id(0x33),
                                             binding.generation, {}, {}};
    auto material = source.Acquire(acquire, manager, tag);
    Check(material.outcome.ok() && material.lease,
          "published credential is acquirable by startup");
    Check(material.lease->credential() == secret,
          "acquired credential is byte-for-byte the provisioned secret, "
          "with the trailing newline treated as punctuation");
    material.lease.reset();
    source.Close();
    ::unlink(path.c_str());
  }

  std::printf("PASS startup credential provisioning checks=%d\n", checks);
  return 0;
}
