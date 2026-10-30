// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
//
// End-to-end coverage for the startup credential path against a real database.
//
// Every other gate in this family works on constructed state, which proves the
// pieces agree with each other but not that a database actually commits what
// they expect. This one creates a database with an internal service principal,
// reads the committed runtime authority binding back out of the closed file,
// frames a credential against it, and acquires that credential. If creation and
// the bootstrap read ever disagree about what was written, this fails and the
// constructed-state gates do not.
//
// It also covers the inverse, which is the state every existing database is in:
// a database created without a service principal commits no binding, reports
// none, and refuses to produce one.

#include "../../src/server/startup_credential_binding.hpp"
#include "../../src/server/startup_credential_provisioning.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace db = scratchbird::storage::database;
namespace mem = scratchbird::core::memory;
namespace uuid = scratchbird::core::uuid;
using namespace scratchbird::server;
using Uuid = scratchbird::core::platform::Uuid;
using UuidKind = scratchbird::core::platform::UuidKind;

namespace {

int checks = 0;

// Removed at exit, including after a failed Check (tests clean up after
// themselves).
std::filesystem::path g_scratch;
void RemoveScratch() {
  if (g_scratch.empty()) return;
  std::error_code ignored;
  std::filesystem::remove_all(g_scratch, ignored);
}

void Check(bool pass, const char* what) {
  ++checks;
  if (!pass) {
    std::fprintf(stderr, "FAIL startup credential committed binding: %s\n", what);
    std::exit(1);
  }
}

std::uint64_t CurrentUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

// A real envelope of the form the create path validates and the engine
// verifies. Fixture material: it authenticates nothing by existing.
constexpr const char* kFingerprint =
    "local-password-pbkdf2-sha256:v1:iterations=600000:"
    "salt=0123456789abcdef0123456789abcdef:"
    "verifier=58a793aad0bd6840ad8d92f6627a23f6142c4ce58210c5f135ea3e2134d43142";

struct Created {
  bool ok = false;
  Uuid database_uuid;
  Uuid service_principal_uuid;
  std::uint64_t identity_seed = 0;
  std::filesystem::path path;
};

// Creates a database, optionally provisioning an internal service principal.
// The service principal is the only difference between the two cases, so the
// binding's presence or absence is attributable to it alone.
Created CreateDatabase(const std::filesystem::path& path, bool with_service) {
  Created out;
  out.path = path;
  const auto now = CurrentUnixMillis();
  const auto database_uuid = uuid::GenerateEngineIdentityV7(UuidKind::database, now);
  const auto filespace_uuid = uuid::GenerateEngineIdentityV7(UuidKind::filespace, now + 1);
  Check(database_uuid.ok() && filespace_uuid.ok(), "fixture UUID generation");

  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid = database_uuid.value;
  create.filespace_uuid = filespace_uuid.value;
  create.creation_unix_epoch_millis = now;
  create.require_resource_seed_pack = true;
  create.allow_minimal_resource_bootstrap = false;
  create.resource_seed_pack_root =
      (std::filesystem::path(__FILE__).lexically_normal().parent_path().parent_path().parent_path() /
       "resources/seed-packs/initial-resource-pack").string();
  create.bootstrap_principal_name = "fixture_owner";
  create.require_bootstrap_principal = true;
  create.allow_uncredentialed_bootstrap = false;
  create.bootstrap_credential_fingerprint = kFingerprint;
  if (with_service) {
    create.internal_service_principal_name = "sb_internal_service";
    create.internal_service_credential_fingerprint = kFingerprint;
    create.require_internal_service_principal = true;
  }
  const auto templates =
      std::filesystem::path(__FILE__).lexically_normal().parent_path().parent_path().parent_path() /
      "config/templates";
  create.metric_policy_defaults_path = (templates / "metric-policy-defaults.conf").string();
  create.metric_policy_families_path = (templates / "metric-policy-families.conf").string();
  create.allow_overwrite = false;

  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::fprintf(stderr, "  create failed: %s:%s:%s\n",
                 created.diagnostic.diagnostic_code.c_str(),
                 created.diagnostic.message_key.c_str(),
                 created.diagnostic.remediation_hint.c_str());
    return out;
  }
  out.ok = true;
  out.database_uuid = database_uuid.value.value;
  out.service_principal_uuid = created.internal_service_principal_uuid.value;
  // The create path derives bootstrap identities from this seed.
  out.identity_seed = now + 900000;
  return out;
}

std::filesystem::path ScratchDir() {
  auto base = std::filesystem::temp_directory_path() /
              ("sb-committed-binding-" + std::to_string(::getpid()));
  std::filesystem::remove_all(base);
  std::filesystem::create_directories(base);
  return base;
}

}  // namespace

int main() {
  // Creation allocates through the process default manager, which refuses
  // until explicitly configured. This is fixture configuration, not a runtime
  // fallback: nothing here confers any right.
  {
    const auto configured = mem::ConfigureDefaultMemoryManagerForFixture(
        mem::DefaultLocalEngineMemoryPolicy(),
        "startup_credential_committed_binding_gate");
    Check(configured.ok(), "fixture default memory manager configured");
  }
  const auto scratch = ScratchDir();
  g_scratch = scratch;
  std::atexit(RemoveScratch);

  // 1. A database created with an internal service principal commits a runtime
  //    authority binding, and the closed-file bootstrap read returns it.
  Uuid service_database_uuid{};
  db::DatabaseBootstrapSecurityCatalogState service_state;
  {
    const auto created = CreateDatabase(scratch / "with-service.sbdb", true);
    Check(created.ok, "database with an internal service principal is created");
    // Isolate creation from the read: if this fails, creation never
    // provisioned the service principal and the binding was never written.
    Check(uuid::IsEngineIdentityUuid(created.service_principal_uuid),
          "creation provisioned an internal service principal");
    service_database_uuid = created.database_uuid;

    const auto read = db::ReadDatabaseBootstrapSecurityCatalog(created.path.string());
    Check(read.ok(), "bootstrap security catalog reads back");
    Check(read.state.present && read.state.committed_by_inventory,
          "bootstrap rows are present and proven committed");
    Check(read.state.runtime_authority_binding_present,
          "the committed runtime authority binding is reported");

    // Identities must be real committed engine identities, not zeroes that
    // happen to satisfy a presence flag.
    Check(uuid::IsEngineIdentityUuid(read.state.runtime_binding_uuid),
          "binding uuid is an engine identity");
    Check(uuid::IsEngineIdentityUuid(read.state.service_principal_uuid),
          "service principal uuid is an engine identity");
    Check(uuid::IsEngineIdentityUuid(read.state.provider_uuid),
          "provider uuid is an engine identity");
    Check(read.state.credential_reference_uuid.has_value() &&
              uuid::IsEngineIdentityUuid(*read.state.credential_reference_uuid),
          "credential reference uuid is a committed engine identity");
    Check(read.state.runtime_authority_generation != 0,
          "binding generation is non-zero");

    // Distinctness matters: a read that collapsed several fields onto one
    // source would still pass every check above.
    Check(read.state.runtime_binding_uuid != read.state.service_principal_uuid &&
              read.state.runtime_binding_uuid != read.state.provider_uuid &&
              read.state.service_principal_uuid != read.state.provider_uuid &&
              *read.state.credential_reference_uuid != read.state.provider_uuid &&
              *read.state.credential_reference_uuid != read.state.service_principal_uuid,
          "the four committed identities are distinct from one another");
    Check(read.state.service_principal_uuid != read.state.principal_uuid.value,
          "the service principal is not the bootstrap owner principal");
    // Note on what cannot be asserted here: these identities are UUIDv7 from
    // GenerateEngineIdentityV7, which takes its timestamp component from the
    // create seed but fills the remainder from RandomBytes16. They are not
    // reproducible from the seed, so a test cannot recompute and compare them.
    // What is checkable is what this gate already checks: that they are engine
    // identities, that they are mutually distinct, that they are distinct from
    // the bootstrap owner, and that the whole credential chain resolves through
    // them. A reordering of the derivation would surface as a role mismatch in
    // the end-to-end assertions below, not as a value mismatch here.
    Check(created.service_principal_uuid == read.state.service_principal_uuid,
          "the identity create reports is the one committed to the catalog");
    service_state = read.state;
  }

  // 2. The binding builder turns that committed state into a frameable binding.
  StartupCredentialTransportBinding bound_binding;
  {
    const auto bound =
        BuildStartupCredentialBinding(service_state, service_database_uuid);
    Check(bound.ok(), "a committed binding yields a transport binding");
    Check(bound.binding.database == service_database_uuid,
          "binding carries the created database");
    Check(bound.binding.principal == service_state.service_principal_uuid,
          "binding carries the committed service principal");
    Check(bound.binding.provider == service_state.provider_uuid,
          "binding carries the committed provider");
    Check(bound.binding.credential_reference == *service_state.credential_reference_uuid,
          "binding carries the committed credential reference");
    Check(bound.binding.generation == service_state.runtime_authority_generation,
          "binding carries the committed generation");
    Check(ValidStartupCredentialFrameBinding(bound.binding),
          "the binding built from a real database is frameable");
    bound_binding = bound.binding;
  }

  // 3. The whole chain against that database: operator secret, framed to the
  //    committed binding, sealed, published, acquired back byte for byte. This
  //    is what no constructed-state gate can establish.
  {
    const std::string secret = kFingerprint;
    const auto secret_path = scratch / "startup.secret";
    const int fd = ::open(secret_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0400);
    Check(fd >= 0 && ::write(fd, secret.data(), secret.size()) ==
                         static_cast<ssize_t>(secret.size()),
          "operator secret staged");
    ::close(fd);

    mem::MemoryTag tag;
    tag.category = mem::MemoryCategory::core_runtime;
    tag.purpose = "committed binding credential custody with heap-backed purpose metadata";
    tag.callsite = "committed binding gate with retained heap-backed callsite metadata";
    tag.binary_ownership[mem::MemoryBinaryScopeKind::database] = bound_binding.database.bytes;
    tag.binary_ownership[mem::MemoryBinaryScopeKind::owner] = bound_binding.principal.bytes;
    tag.binary_ownership[mem::MemoryBinaryScopeKind::context] = bound_binding.database.bytes;

    mem::MemoryManager manager(mem::DefaultLocalEngineMemoryPolicy());
    StartupCredentialSource source(bound_binding.database, 4);

    StartupCredentialProvisionRequest provision;
    provision.source = StartupCredentialProvisionSource::operator_file;
    provision.name_or_path = secret_path.string();
    provision.binding = bound_binding;
    // Expected current generation of a fresh source is zero; the generation
    // being published is the binding's.
    provision.publish = {bound_binding.database, bound_binding.principal,
                         0, {}, {}};

    const auto provisioned =
        ProvisionStartupCredential(provision, source, manager, tag);
    // Report the actual values rather than a boolean. A provisioning failure
    // has five independent causes and collapsing them loses which one fired.
    if (!provisioned.ok()) {
      std::fprintf(stderr,
          "  provision detail: error=%d code=%s adapter_error=%d native_error=%d "
          "publish.error=%d publish.transport=%d publish.wait=%d publish.native=%d\n",
          static_cast<int>(provisioned.error),
          StartupCredentialProvisionDiagnosticCode(provisioned.error),
          static_cast<int>(provisioned.adapter_error),
          provisioned.native_error,
          static_cast<int>(provisioned.publish.error),
          static_cast<int>(provisioned.publish.transport),
          static_cast<int>(provisioned.publish.wait),
          provisioned.publish.native_error);
    }
    Check(provisioned.ok(),
          "a credential framed to the committed binding publishes");

    StartupCredentialSource::Request acquire{bound_binding.database,
                                             bound_binding.credential_reference,
                                             bound_binding.generation, {}, {}};
    auto material = source.Acquire(acquire, manager, tag);
    Check(material.outcome.ok() && material.lease,
          "the published credential is acquirable under the committed generation");
    Check(material.lease->credential() == secret,
          "acquired credential is byte-for-byte the operator secret, carried "
          "under identities this database actually committed");
    material.lease.reset();
    source.Close();
  }

  // 4. No caller-supplied service credential. This used to mean no service
  //    principal and no binding at all. Owner direction 2026-10-30
  //    (SB_L5_PRINCIPAL_ALWAYS_PROVISIONED): every create now writes L5 and its
  //    binding; with no credential the binding names none, so a startup
  //    credential still cannot be framed and the refusal stays.
  {
    const auto created = CreateDatabase(scratch / "no-service.sbdb", false);
    Check(created.ok, "database without a caller-supplied service credential is created");

    const auto read = db::ReadDatabaseBootstrapSecurityCatalog(created.path.string());
    Check(read.ok() && read.state.present && read.state.committed_by_inventory,
          "its bootstrap rows still read back normally");
    Check(read.state.runtime_authority_binding_present &&
              !read.state.service_principal_uuid.is_nil(),
          "L5's runtime authority binding is committed regardless");
    Check(!read.state.credential_reference_uuid.has_value(),
          "no credential reference is invented");

    const auto bound =
        BuildStartupCredentialBinding(read.state, created.database_uuid);
    Check(!bound.ok() &&
              bound.error == StartupCredentialBindingError::credential_reference_absent,
          "a binding with no credential refuses to produce a startup credential binding");
    Check(!ValidStartupCredentialFrameBinding(bound.binding),
          "and leaves no frameable binding behind");
  }

  std::filesystem::remove_all(scratch);
  std::printf("PASS startup credential committed binding checks=%d\n", checks);
  return 0;
}
