// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_dirty_manifest.hpp"
#include "database_format.hpp"
#include "filespace_header.hpp"
#include "filespace_lifecycle.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace database = scratchbird::storage::database;
namespace disk = scratchbird::storage::disk;
namespace filespace = scratchbird::storage::filespace;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

struct Args {
  std::filesystem::path database_path;
  std::vector<std::filesystem::path> filespace_paths;
  std::filesystem::path registry_path;
  std::filesystem::path dirty_manifest_path;
  std::filesystem::path out_path;
  std::filesystem::path work_dir;
  bool self_test = false;
};

struct VerifyResult {
  bool ok = false;
  std::string diagnostic_code;
  std::string bundle_text;
};

struct OfflineFileHealth {
  bool file_present = false;
  bool size_query_ok = false;
  bool size_aligned = false;
  bool can_read = false;
  u64 size_bytes = 0;
  std::string health = "error";
};

std::string BaseName(const std::filesystem::path& path) {
  return path.filename().string();
}

std::string UuidText(const TypedUuid& value) {
  return uuid::UuidToString(value.value);
}

TypedUuid MakeIdentity(UuidKind kind, u64 salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, 1831000000000ull + salt);
  if (!generated.ok()) {
    std::cerr << generated.diagnostic.diagnostic_code << '\n';
    std::exit(EXIT_FAILURE);
  }
  return generated.value;
}

bool ReadTextFile(const std::filesystem::path& path, std::string* text) {
  if (text == nullptr) {
    return false;
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  *text = buffer.str();
  return true;
}

bool WriteTextFile(const std::filesystem::path& path, std::string_view text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    return false;
  }
  out << text;
  return static_cast<bool>(out);
}

OfflineFileHealth CheckOfflineFileHealth(const std::filesystem::path& path,
                                         u32 page_size) {
  OfflineFileHealth health;
  std::error_code ec;
  health.file_present = std::filesystem::is_regular_file(path, ec);
  if (ec) {
    health.file_present = false;
  }
  health.size_bytes = std::filesystem::file_size(path, ec);
  health.size_query_ok = !ec;
  health.size_aligned = health.size_query_ok &&
                        (page_size == 0 || (health.size_bytes % page_size) == 0);
  std::ifstream in(path, std::ios::binary);
  health.can_read = static_cast<bool>(in);
  health.health =
      health.file_present && health.size_query_ok && health.size_aligned && health.can_read
          ? "ok"
          : "error";
  return health;
}

bool ReadDatabaseHeaderOffline(const std::filesystem::path& path,
                               disk::SerializedDatabaseHeader* serialized) {
  if (serialized == nullptr) {
    return false;
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  in.read(reinterpret_cast<char*>(serialized->data()),
          static_cast<std::streamsize>(serialized->size()));
  return in.gcount() == static_cast<std::streamsize>(serialized->size());
}

VerifyResult Failure(std::string diagnostic_code) {
  VerifyResult result;
  result.ok = false;
  result.diagnostic_code = std::move(diagnostic_code);
  return result;
}

bool WriteDatabaseFixture(const std::filesystem::path& path,
                          const TypedUuid& database_uuid,
                          u32 page_size) {
  std::filesystem::create_directories(path.parent_path());
  const auto header = disk::MakeDatabaseHeader(
      database_uuid.value,
      page_size,
      1831000000000ull,
      0,
      disk::DatabaseCompatibilityFlag::public_node_safe_header_open |
          disk::DatabaseCompatibilityFlag::unknown_page_safe_classification_required);
  if (!header.ok()) {
    return false;
  }
  const auto serialized = disk::SerializeDatabaseHeader(header.header);
  if (!serialized.ok()) {
    return false;
  }
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    return false;
  }
  out.write(reinterpret_cast<const char*>(serialized.serialized.data()),
            static_cast<std::streamsize>(serialized.serialized.size()));
  const unsigned char zero = 0;
  out.seekp(static_cast<std::streamoff>(page_size) - 1);
  out.write(reinterpret_cast<const char*>(&zero), sizeof(zero));
  return static_cast<bool>(out);
}

bool WriteDirtyManifestFixture(const std::filesystem::path& path,
                               const TypedUuid& filespace_uuid) {
  database::DirtyObjectManifest manifest;
  manifest.checkpoint_generation = 7;
  manifest.completed = true;
  manifest.classification_only = true;

  database::DirtyObjectManifestEntry entry;
  entry.kind = database::DirtyObjectKind::filespace_header;
  entry.object_uuid = filespace_uuid;
  entry.page_number = 0;
  entry.page_generation = 1;
  entry.object_checksum = 11;
  entry.local_transaction_id = 1;
  entry.operation_envelope_checksum = 13;
  entry.transaction_evidence_checksum = 17;
  entry.dirty = true;
  entry.authoritative = true;
  manifest.entries.push_back(entry);

  const auto built = database::BuildDirtyObjectManifest(manifest);
  if (!built.ok()) {
    return false;
  }
  return WriteTextFile(path, built.serialized);
}

filespace::PhysicalFilespaceHeader MakeFilespaceHeader(const TypedUuid& database_uuid,
                                                       const TypedUuid& filespace_uuid,
                                                       const TypedUuid& writer_uuid) {
  filespace::PhysicalFilespaceHeader header;
  header.database_uuid = database_uuid;
  header.filespace_uuid = filespace_uuid;
  header.role = filespace::FilespaceRole::secondary_data;
  header.state = filespace::FilespaceState::online;
  header.page_size = 8192;
  header.physical_filespace_id = 2;
  header.total_pages = 2;
  header.free_pages = 1;
  header.preallocated_pages = 0;
  header.allocation_root_page = 1;
  header.header_generation = 3;
  header.writer_identity_uuid = writer_uuid;
  header.creation_operation_uuid = "public-disk-resource-bundle-fixture";
  return header;
}

filespace::FilespaceRegistry MakeRegistry(const filespace::PhysicalFilespaceHeader& header,
                                          const std::filesystem::path& filespace_path) {
  filespace::FilespaceRegistry registry;
  filespace::FilespaceDescriptor descriptor;
  descriptor.database_uuid = header.database_uuid;
  descriptor.filespace_uuid = header.filespace_uuid;
  descriptor.path = BaseName(filespace_path);
  descriptor.role = header.role;
  descriptor.state = filespace::FilespaceState::attached;
  descriptor.page_size = header.page_size;
  descriptor.generation = 1;
  descriptor.active = true;
  descriptor.physical_filespace_id = header.physical_filespace_id;
  descriptor.total_pages = header.total_pages;
  descriptor.free_pages = header.free_pages;
  descriptor.preallocated_pages = header.preallocated_pages;
  descriptor.allocation_root_page = header.allocation_root_page;
  descriptor.header_generation = header.header_generation;
  descriptor.writer_identity_uuid = header.writer_identity_uuid;
  registry.filespaces.push_back(std::move(descriptor));
  return registry;
}

bool HeaderMatchesDescriptor(const filespace::PhysicalFilespaceHeader& header,
                             const filespace::FilespaceDescriptor& descriptor) {
  return header.database_uuid.value == descriptor.database_uuid.value &&
         header.filespace_uuid.value == descriptor.filespace_uuid.value &&
         header.role == descriptor.role &&
         header.page_size == descriptor.page_size &&
         header.physical_filespace_id == descriptor.physical_filespace_id &&
         header.total_pages == descriptor.total_pages &&
         header.free_pages == descriptor.free_pages &&
         header.preallocated_pages == descriptor.preallocated_pages &&
         header.allocation_root_page == descriptor.allocation_root_page &&
         header.header_generation == descriptor.header_generation &&
         header.writer_identity_uuid.value == descriptor.writer_identity_uuid.value;
}

VerifyResult BuildDiskResourceBundle(const Args& args) {
  if (args.database_path.empty() || args.filespace_paths.empty() || args.out_path.empty()) {
    return Failure("SB-PUBLIC-DISK-BUNDLE-ARGS-INVALID");
  }

  disk::SerializedDatabaseHeader serialized_header{};
  if (!ReadDatabaseHeaderOffline(args.database_path, &serialized_header)) {
    return Failure("SB-PUBLIC-DISK-BUNDLE-DATABASE-READ-FAILED");
  }
  const auto database_header = disk::ParseDatabaseHeader(serialized_header);
  if (!database_header.ok()) {
    return Failure(database_header.diagnostic.diagnostic_code);
  }

  const auto database_health =
      CheckOfflineFileHealth(args.database_path, database_header.header.page_size);
  if (database_health.health != "ok") {
    return Failure("SB-PUBLIC-DISK-BUNDLE-DATABASE-HEALTH-FAILED");
  }

  std::map<std::string, filespace::PhysicalFilespaceHeaderResult> filespaces_by_uuid;
  std::ostringstream bundle;
  bundle << "scratchbird.public.disk_resource_bundle.v1\n";
  bundle << "authority=observability_only\n";
  bundle << "private_proof_inputs=absent\n";
  bundle << "database.file=" << BaseName(args.database_path) << '\n';
  bundle << "database.uuid=" << uuid::UuidToString(database_header.header.database_uuid) << '\n';
  bundle << "database.page_size=" << database_header.header.page_size << '\n';
  bundle << "database.size_bytes=" << database_health.size_bytes << '\n';
  bundle << "database.health=" << database_health.health << '\n';
  bundle << "database.offline_read_only=true\n";

  bundle << "filespace.count=" << args.filespace_paths.size() << '\n';
  for (std::size_t index = 0; index < args.filespace_paths.size(); ++index) {
    const auto& path = args.filespace_paths[index];
    const auto header = filespace::ReadPhysicalFilespaceHeaderOffline(path.string());
    if (!header.ok()) {
      return Failure(header.diagnostic.diagnostic_code);
    }
    if (header.header.database_uuid.value != database_header.header.database_uuid) {
      return Failure("SB-PUBLIC-DISK-BUNDLE-FILESPACE-DATABASE-UUID-MISMATCH");
    }

    const auto health = CheckOfflineFileHealth(path, header.header.page_size);
    if (health.health != "ok") {
      return Failure("SB-PUBLIC-DISK-BUNDLE-FILESPACE-HEALTH-FAILED");
    }

    const std::string prefix = "filespace." + std::to_string(index) + ".";
    bundle << prefix << "file=" << BaseName(path) << '\n';
    bundle << prefix << "uuid=" << UuidText(header.header.filespace_uuid) << '\n';
    bundle << prefix << "page_size=" << header.header.page_size << '\n';
    bundle << prefix << "total_pages=" << header.header.total_pages << '\n';
    bundle << prefix << "free_pages=" << header.header.free_pages << '\n';
    bundle << prefix << "preallocated_pages=" << header.header.preallocated_pages << '\n';
    bundle << prefix << "allocation_root_page=" << header.header.allocation_root_page << '\n';
    bundle << prefix << "header_generation=" << header.header.header_generation << '\n';
    bundle << prefix << "capacity_bytes=" << header.expected_capacity_bytes << '\n';
    bundle << prefix << "file_size_bytes=" << header.file_size_bytes << '\n';
    bundle << prefix << "file_size_matches_capacity="
           << (header.file_size_matches_capacity ? "true" : "false") << '\n';
    bundle << prefix << "health=" << health.health << '\n';
    bundle << prefix << "offline_read_only=true\n";
    filespaces_by_uuid[UuidText(header.header.filespace_uuid)] = header;
  }

  bool registry_capacity_windows_match = true;
  if (!args.registry_path.empty()) {
    std::string registry_text;
    if (!ReadTextFile(args.registry_path, &registry_text)) {
      return Failure("SB-PUBLIC-DISK-BUNDLE-REGISTRY-READ-FAILED");
    }
    const auto parsed = filespace::ParseFilespaceRegistry(registry_text);
    if (!parsed.ok()) {
      return Failure(parsed.diagnostic.diagnostic_code);
    }
    for (const auto& descriptor : parsed.registry.filespaces) {
      if (descriptor.database_uuid.value != database_header.header.database_uuid) {
        return Failure("SB-PUBLIC-DISK-BUNDLE-REGISTRY-DATABASE-UUID-MISMATCH");
      }
      const auto found = filespaces_by_uuid.find(UuidText(descriptor.filespace_uuid));
      if (found == filespaces_by_uuid.end() ||
          !HeaderMatchesDescriptor(found->second.header, descriptor)) {
        registry_capacity_windows_match = false;
      }
    }
    if (!registry_capacity_windows_match) {
      return Failure("SB-PUBLIC-DISK-BUNDLE-REGISTRY-CAPACITY-MISMATCH");
    }
    bundle << "registry.present=true\n";
    bundle << "registry.file=" << BaseName(args.registry_path) << '\n';
    bundle << "registry.filespace_count=" << parsed.registry.filespaces.size() << '\n';
    bundle << "registry.capacity_windows_match=true\n";
  } else {
    bundle << "registry.present=false\n";
  }

  if (!args.dirty_manifest_path.empty()) {
    std::string manifest_text;
    if (!ReadTextFile(args.dirty_manifest_path, &manifest_text)) {
      return Failure("SB-PUBLIC-DISK-BUNDLE-DIRTY-MANIFEST-READ-FAILED");
    }
    const auto parsed = database::ParseDirtyObjectManifest(manifest_text);
    if (!parsed.ok()) {
      return Failure(parsed.diagnostic.diagnostic_code);
    }
    const auto classified = database::ClassifyDirtyObjectManifestForRecovery(parsed.manifest);
    if (!classified.ok()) {
      return Failure(classified.diagnostic.diagnostic_code);
    }
    bundle << "dirty_manifest.present=true\n";
    bundle << "dirty_manifest.file=" << BaseName(args.dirty_manifest_path) << '\n';
    bundle << "dirty_manifest.classification_only="
           << (parsed.manifest.classification_only ? "true" : "false") << '\n';
    bundle << "dirty_manifest.entry_count=" << parsed.manifest.entries.size() << '\n';
    bundle << "dirty_manifest.rebuild_by_scan_required="
           << (classified.rebuild_by_scan_required ? "true" : "false") << '\n';
    bundle << "dirty_manifest.quarantine_required="
           << (classified.quarantine_required ? "true" : "false") << '\n';
  } else {
    bundle << "dirty_manifest.present=false\n";
  }

  const std::string text = bundle.str();
  if (!WriteTextFile(args.out_path, text)) {
    return Failure("SB-PUBLIC-DISK-BUNDLE-WRITE-FAILED");
  }
  VerifyResult result;
  result.ok = true;
  result.diagnostic_code = "SB-PUBLIC-DISK-BUNDLE-OK";
  result.bundle_text = text;
  return result;
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

int RunSelfTest(const Args& requested_args) {
  const std::filesystem::path work_dir =
      requested_args.work_dir.empty()
          ? std::filesystem::current_path() / "public_disk_resource_bundle_self_test"
          : requested_args.work_dir;
  std::error_code ignored;
  std::filesystem::remove_all(work_dir, ignored);
  std::filesystem::create_directories(work_dir);

  const auto database_uuid = MakeIdentity(UuidKind::database, 1);
  const auto filespace_uuid = MakeIdentity(UuidKind::filespace, 2);
  const auto writer_uuid = MakeIdentity(UuidKind::object, 3);
  const auto database_path = work_dir / "public-resource.sbdb";
  const auto filespace_path = work_dir / "public-resource-secondary.sbfs";
  const auto registry_path = work_dir / "filespace.registry";
  const auto dirty_manifest_path = work_dir / "dirty.manifest";
  const auto out_path = requested_args.out_path.empty()
                            ? work_dir / "disk-resource.bundle"
                            : requested_args.out_path;

  if (!WriteDatabaseFixture(database_path, database_uuid, 8192)) {
    std::cerr << "self-test database fixture write failed\n";
    return EXIT_FAILURE;
  }

  const auto header = MakeFilespaceHeader(database_uuid, filespace_uuid, writer_uuid);
  const auto header_write =
      filespace::WritePhysicalFilespaceHeader(filespace_path.string(), header, false);
  if (!header_write.ok()) {
    std::cerr << header_write.diagnostic.diagnostic_code << '\n';
    return EXIT_FAILURE;
  }

  const auto registry = filespace::SerializeFilespaceRegistry(MakeRegistry(header, filespace_path));
  if (!registry.ok() || !WriteTextFile(registry_path, registry.payload)) {
    std::cerr << "self-test registry fixture write failed\n";
    return EXIT_FAILURE;
  }
  if (!WriteDirtyManifestFixture(dirty_manifest_path, filespace_uuid)) {
    std::cerr << "self-test dirty manifest fixture write failed\n";
    return EXIT_FAILURE;
  }

  Args args;
  args.database_path = database_path;
  args.filespace_paths.push_back(filespace_path);
  args.registry_path = registry_path;
  args.dirty_manifest_path = dirty_manifest_path;
  args.out_path = out_path;
  const auto ok = BuildDiskResourceBundle(args);
  if (!ok.ok) {
    std::cerr << ok.diagnostic_code << '\n';
    return EXIT_FAILURE;
  }
  if (Contains(ok.bundle_text, work_dir.string())) {
    std::cerr << "support bundle leaked an absolute proof input\n";
    return EXIT_FAILURE;
  }
  if (!Contains(ok.bundle_text, "authority=observability_only") ||
      !Contains(ok.bundle_text, "private_proof_inputs=absent") ||
      !Contains(ok.bundle_text, "registry.capacity_windows_match=true") ||
      !Contains(ok.bundle_text, "dirty_manifest.classification_only=true") ||
      !Contains(ok.bundle_text, "database.offline_read_only=true") ||
      !Contains(ok.bundle_text, "file_size_matches_capacity=true")) {
    std::cerr << "support bundle omitted required disk summary fields\n";
    return EXIT_FAILURE;
  }
  for (const auto& sidecar : std::vector<std::filesystem::path>{
           database_path.string() + ".sb.owner.lock",
           filespace_path.string() + ".sb.owner.lock",
           database_path.string() + ".sb.route.owner.lock",
           filespace_path.string() + ".sb.route.owner.lock"}) {
    if (std::filesystem::exists(sidecar)) {
      std::cerr << "offline verifier created an ownership sidecar\n";
      return EXIT_FAILURE;
    }
  }

  auto mismatched_registry = MakeRegistry(header, filespace_path);
  mismatched_registry.filespaces.front().total_pages += 1;
  const auto mismatched_serialized = filespace::SerializeFilespaceRegistry(mismatched_registry);
  if (!mismatched_serialized.ok() ||
      !WriteTextFile(registry_path, mismatched_serialized.payload)) {
    std::cerr << "self-test mismatched registry fixture write failed\n";
    return EXIT_FAILURE;
  }
  const auto refused = BuildDiskResourceBundle(args);
  if (refused.ok ||
      refused.diagnostic_code != "SB-PUBLIC-DISK-BUNDLE-REGISTRY-CAPACITY-MISMATCH") {
    std::cerr << "capacity mismatch was not refused\n";
    return EXIT_FAILURE;
  }

  std::cout << "public_disk_resource_bundle_gate=passed\n";
  return EXIT_SUCCESS;
}

void Usage() {
  std::cerr
      << "usage: public_disk_resource_bundle --database PATH --filespace PATH "
         "[--filespace PATH ...] [--registry PATH] [--dirty-manifest PATH] "
         "--out PATH\n"
      << "       public_disk_resource_bundle --self-test --work-dir PATH --out PATH\n";
}

bool ParseArgs(int argc, char** argv, Args* args) {
  if (args == nullptr) {
    return false;
  }
  for (int i = 1; i < argc; ++i) {
    const std::string key = argv[i];
    if (key == "--self-test") {
      args->self_test = true;
      continue;
    }
    if (i + 1 >= argc) {
      return false;
    }
    const std::string value = argv[++i];
    if (key == "--database") {
      args->database_path = value;
    } else if (key == "--filespace") {
      args->filespace_paths.emplace_back(value);
    } else if (key == "--registry") {
      args->registry_path = value;
    } else if (key == "--dirty-manifest") {
      args->dirty_manifest_path = value;
    } else if (key == "--out") {
      args->out_path = value;
    } else if (key == "--work-dir") {
      args->work_dir = value;
    } else {
      return false;
    }
  }
  if (args->self_test) {
    return !args->work_dir.empty();
  }
  return !args->database_path.empty() &&
         !args->filespace_paths.empty() &&
         !args->out_path.empty();
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!ParseArgs(argc, argv, &args)) {
    Usage();
    return EXIT_FAILURE;
  }
  if (args.self_test) {
    return RunSelfTest(args);
  }
  const auto result = BuildDiskResourceBundle(args);
  if (!result.ok) {
    std::cerr << result.diagnostic_code << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "public_disk_resource_bundle=passed\n";
  return EXIT_SUCCESS;
}
