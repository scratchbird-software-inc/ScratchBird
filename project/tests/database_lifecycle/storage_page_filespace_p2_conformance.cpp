// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "foreign_filespace_quarantine.hpp"
#include "filespace_header.hpp"
#include "filespace_lifecycle.hpp"
#include "metric_registry.hpp"
#include "page_layout.hpp"
#include "page_registry.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace filespace = scratchbird::storage::filespace;
namespace metrics = scratchbird::core::metrics;
namespace page = scratchbird::storage::page;
namespace disk = scratchbird::storage::disk;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

std::uint64_t CurrentUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

TypedUuid MakeUuid(UuidKind kind, std::uint64_t millis) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, millis);
  Require(generated.ok(), "uuid generation failed");
  return generated.value;
}

std::filesystem::path TempDir() {
  const auto path = std::filesystem::temp_directory_path() /
                    ("sb_p2_storage_filespace_" + std::to_string(CurrentUnixMillis()));
  std::filesystem::create_directories(path);
  return path;
}

void TestPageLayoutRegistryCompleteness() {
  std::set<disk::PageType> families;
  for (const auto& descriptor : page::BuiltinPageFamilyRegistry()) {
    Require(families.insert(descriptor.page_type).second,
            "page family registry contains duplicate page type");
    const auto layout = page::LookupPageLayout(descriptor.page_type);
    Require(layout.ok(), "page family registry entry has no page layout");
    Require(layout.descriptor.stable_name == descriptor.stable_name,
            "page layout stable name diverged from page family registry");
    const auto capacity = page::ComputePageLayoutCapacity(descriptor.page_type, 16384);
    Require(capacity.ok(), "page layout capacity failed for supported page size");
    Require(capacity.capacity.page_size == 16384, "page layout capacity page size mismatch");
    if (!descriptor.cluster_only && !descriptor.encrypted_or_opaque && !descriptor.reserved) {
      Require(descriptor.supported_local_read, "public local page type is not readable");
    }
  }
  Require(families.size() == page::BuiltinPageLayoutRegistry().size(),
          "page family and layout registry sizes diverged");

  disk::PageClassification cluster;
  cluster.status = {scratchbird::core::platform::StatusCode::ok,
                    scratchbird::core::platform::Severity::info,
                    scratchbird::core::platform::Subsystem::storage_page};
  cluster.kind = disk::PageClassificationKind::cluster_only;
  cluster.page_type = disk::PageType::cluster_transaction;
  cluster.readable = true;
  cluster.writable = true;
  cluster.cluster_authority_required = true;
  const auto classified = page::ClassifyForPageManager(cluster);
  Require(classified.requires_cluster_authority,
          "cluster page did not require cluster authority");
  Require(!classified.may_write_body,
          "cluster page became writable without cluster mapping authority");

  const auto unknown = page::LookupPageLayout(disk::PageType::unknown);
  Require(!unknown.ok(), "unknown page type did not fail closed");
}

void TestFilespaceLifecycleAndMetrics(const std::filesystem::path& dir,
                                      const TypedUuid& database_uuid,
                                      const TypedUuid& primary_uuid,
                                      const TypedUuid& secondary_uuid) {
  filespace::FilespaceRegistry registry;
  const auto writer_uuid = MakeUuid(UuidKind::object, CurrentUnixMillis() + 100);
  filespace::FilespaceOperationRequest create;
  create.operation = filespace::FilespaceOperation::create_filespace;
  create.database_uuid = database_uuid;
  create.filespace_uuid = primary_uuid;
  create.path = (dir / "primary.sbfs").string();
  create.role = filespace::FilespaceRole::active_primary;
  create.writer_identity_uuid = writer_uuid;
  create.reason = "p2-create-primary";
  const auto primary = filespace::ApplyFilespaceOperation(&registry, create);
  Require(primary.ok(), "active primary create failed");
  Require(primary.descriptor.first_filespace, "first filespace flag was not set");
  Require(primary.descriptor.startup_authority, "active primary missing startup authority");
  Require(primary.metrics_emitted, "filespace lifecycle metrics were not emitted");

  filespace::PhysicalFilespaceHeader header;
  header.database_uuid = database_uuid;
  header.filespace_uuid = secondary_uuid;
  header.role = filespace::FilespaceRole::secondary_data;
  header.state = filespace::FilespaceState::online;
  header.page_size = 16384;
  header.physical_filespace_id = 2;
  header.total_pages = 32;
  header.free_pages = 32;
  header.header_generation = 1;
  header.writer_identity_uuid = writer_uuid;
  header.creation_operation_uuid = "p2-create-secondary-header";
  const auto header_write =
      filespace::WritePhysicalFilespaceHeader((dir / "secondary.sbfs").string(), header, true);
  Require(header_write.ok(), "secondary physical filespace header write failed");

  filespace::FilespaceOperationRequest attach;
  attach.operation = filespace::FilespaceOperation::attach_filespace;
  attach.database_uuid = database_uuid;
  attach.filespace_uuid = secondary_uuid;
  attach.path = (dir / "secondary.sbfs").string();
  attach.role = filespace::FilespaceRole::secondary_data;
  attach.reason = "p2-attach-secondary";
  attach.policy.require_physical_header_for_attach = true;
  const auto attached = filespace::ApplyFilespaceOperation(&registry, attach);
  Require(attached.ok(), "secondary attach with physical header failed");
  Require(attached.descriptor.role == filespace::FilespaceRole::secondary_data,
          "secondary filespace role mismatch");

  filespace::FilespaceOperationRequest pin = attach;
  pin.operation = filespace::FilespaceOperation::pin_filespace;
  pin.pin_kind = filespace::FilespacePinKind::transaction;
  pin.pin_owner = "p2-open-transaction";
  const auto pinned = filespace::ApplyFilespaceOperation(&registry, pin);
  Require(pinned.ok(), "filespace pin failed");

  filespace::FilespaceOperationRequest detach = attach;
  detach.operation = filespace::FilespaceOperation::detach_filespace;
  const auto detach_refused = filespace::ApplyFilespaceOperation(&registry, detach);
  Require(!detach_refused.ok(), "pinned filespace detached without refusing active pins");
  Require(detach_refused.diagnostic.diagnostic_code ==
              "SB-FILESPACE-LIFECYCLE-DETACH-PINNED",
          "pinned detach diagnostic mismatch");

  filespace::FilespaceOperationRequest unpin = pin;
  unpin.operation = filespace::FilespaceOperation::unpin_filespace;
  const auto unpinned = filespace::ApplyFilespaceOperation(&registry, unpin);
  Require(unpinned.ok(), "filespace unpin failed");
  const auto detached = filespace::ApplyFilespaceOperation(&registry, detach);
  Require(detached.ok(), "unpinned filespace detach failed");

  const auto values = metrics::DefaultMetricRegistry().SnapshotCurrent();
  bool lifecycle_metric = false;
  bool role_metric = false;
  for (const auto& value : values) {
    lifecycle_metric = lifecycle_metric ||
                       value.family == "sb_storage_filespace_lifecycle_total";
    role_metric = role_metric || value.family == "sb_filespace_role_state";
  }
  Require(lifecycle_metric, "filespace lifecycle metric was not published");
  Require(role_metric, "filespace role state metric was not published");
  Require(!registry.evidence.empty(), "filespace lifecycle evidence was not recorded");
}

void TestForeignFilespaceQuarantine(const std::filesystem::path& dir,
                                    const TypedUuid& database_uuid,
                                    const TypedUuid& foreign_uuid) {
  filespace::FilespaceRegistry registry;
  const auto writer_uuid = MakeUuid(UuidKind::object, CurrentUnixMillis() + 200);
  filespace::PhysicalFilespaceHeader header;
  header.database_uuid = database_uuid;
  header.filespace_uuid = foreign_uuid;
  header.role = filespace::FilespaceRole::secondary_data;
  header.state = filespace::FilespaceState::online;
  header.page_size = 16384;
  header.physical_filespace_id = 3;
  header.total_pages = 64;
  header.free_pages = 64;
  header.header_generation = 1;
  header.writer_identity_uuid = writer_uuid;
  header.creation_operation_uuid = "p2-foreign-header";
  const auto header_write =
      filespace::WritePhysicalFilespaceHeader((dir / "foreign.sbfs").string(), header, true);
  Require(header_write.ok(), "foreign filespace header write failed");

  filespace::ForeignFilespaceQuarantineRequest request;
  request.database_uuid = database_uuid;
  request.filespace_uuid = foreign_uuid;
  request.path = (dir / "foreign.sbfs").string();
  request.operation_uuid = "p2-foreign-import";
  const auto imported =
      filespace::ImportForeignFilespaceIntoQuarantine(&registry, request);
  Require(imported.ok(), "foreign filespace import quarantine failed");
  Require(imported.quarantine_fence_active, "foreign filespace quarantine fence missing");
  Require(imported.descriptor.state == filespace::FilespaceState::quarantine,
          "foreign filespace did not enter quarantine");
  Require(imported.descriptor.read_only, "quarantined foreign filespace was not read-only");

  const auto duplicate =
      filespace::ImportForeignFilespaceIntoQuarantine(&registry, request);
  Require(!duplicate.ok(), "duplicate foreign filespace identity was admitted");

  request.inspector_uuid = "019e2000-0000-7000-8000-000000000201";
  const auto inspected = filespace::InspectForeignFilespaceQuarantine(registry, request);
  Require(inspected.ok(), "foreign filespace inspection failed");
  Require(inspected.inspection_passed && inspected.release_allowed,
          "foreign filespace inspection did not authorize release eligibility");

  const auto release_refused =
      filespace::ReleaseForeignFilespaceQuarantine(&registry, request);
  Require(!release_refused.ok(), "foreign filespace released without authority");
  Require(release_refused.diagnostic.diagnostic_code ==
              "SB-FOREIGN-FILESPACE-RELEASE-AUTHORITY-REQUIRED",
          "foreign filespace release refusal diagnostic mismatch");

  request.header_inspection_passed = true;
  request.release_authorized = true;
  request.release_authority_uuid = "019e2000-0000-7000-8000-000000000202";
  const auto released =
      filespace::ReleaseForeignFilespaceQuarantine(&registry, request);
  Require(released.ok(), "foreign filespace release failed");
  Require(!released.quarantine_fence_active, "foreign filespace release left quarantine fence active");
  Require(released.descriptor.state == filespace::FilespaceState::detached,
          "foreign filespace release did not land in detached state");
}

}  // namespace

int main() {
  const auto dir = TempDir();
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      std::error_code ignored;
      std::filesystem::remove_all(path, ignored);
    }
  } cleanup{dir};

  const auto now = CurrentUnixMillis();
  const auto database_uuid = MakeUuid(UuidKind::database, now);
  const auto primary_uuid = MakeUuid(UuidKind::filespace, now + 1);
  const auto secondary_uuid = MakeUuid(UuidKind::filespace, now + 2);
  const auto foreign_uuid = MakeUuid(UuidKind::filespace, now + 3);

  TestPageLayoutRegistryCompleteness();
  TestFilespaceLifecycleAndMetrics(dir, database_uuid, primary_uuid, secondary_uuid);
  TestForeignFilespaceQuarantine(dir, database_uuid, foreign_uuid);
  return EXIT_SUCCESS;
}
