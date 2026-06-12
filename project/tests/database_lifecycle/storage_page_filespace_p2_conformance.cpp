// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "foreign_filespace_quarantine.hpp"
#include "filespace_discovery.hpp"
#include "filespace_header.hpp"
#include "filespace_lifecycle.hpp"
#include "filespace_package.hpp"
#include "index_page_family.hpp"
#include "metric_registry.hpp"
#include "page_layout.hpp"
#include "page_filespace_handoff.hpp"
#include "page_registry.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <utility>
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
    Require(descriptor.stable_name.find("tablespace") == std::string::npos,
            "page family registry retained native tablespace terminology");
    Require(!descriptor.implementation_search_key.empty(),
            "page family registry entry omitted implementation search key");
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
  Require(families.size() == disk::kDeclaredPageTypes.size(),
          "declared page type enum and page registry sizes diverged");

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

void TestPageRegistryStatusMatrixAndReservedFamilies() {
  const auto filespace_directory = page::LookupPageFamily(disk::PageType::filespace_directory);
  Require(filespace_directory.ok(), "filespace directory page type missing from registry");
  Require(filespace_directory.descriptor.family == page::PageFamily::filespace_control,
          "filespace directory did not migrate to filespace_control family");
  Require(page::PageRegistryStatusAllowsProductSupport(filespace_directory.descriptor.registry_status),
          "implemented filespace directory rejected support claim status");

  const auto reserved_filespace =
      page::LookupPageFamily(disk::PageType::filespace_lifecycle_state);
  Require(reserved_filespace.ok(), "reserved filespace lifecycle page missing");
  Require(reserved_filespace.descriptor.registry_status == page::PageRegistryStatus::reserved,
          "filespace lifecycle page was not reserved");
  Require(!reserved_filespace.descriptor.supported_local_read &&
              !reserved_filespace.descriptor.supported_local_write,
          "reserved filespace lifecycle page claimed local support");
  const auto reserved_claim = page::ValidatePageTypeProductSupportClaim(
      disk::PageType::filespace_lifecycle_state, "release-supported lifecycle page");
  Require(!reserved_claim.ok(), "reserved filespace page support overclaim passed");
  Require(reserved_claim.diagnostic.diagnostic_code ==
              "SB_DIAG_PAGE_RESERVED_SUPPORT_OVERCLAIM",
          "reserved page support overclaim diagnostic mismatch");

  const auto shard = page::LookupPageFamily(disk::PageType::shard_placement_map);
  Require(shard.ok(), "shard placement page missing");
  Require(shard.descriptor.cluster_only, "shard placement map did not require cluster scope");
  Require(shard.descriptor.registry_status == page::PageRegistryStatus::reserved,
          "shard placement map was not reserved");

  const auto protected_material =
      page::LookupPageFamily(disk::PageType::protected_material_root);
  Require(protected_material.ok(), "protected material root page missing");
  Require(protected_material.descriptor.encrypted_or_opaque,
          "protected material root did not require protected access");

  const auto superseded = page::LookupPageFamily(disk::PageType::name_registry_superseded);
  Require(superseded.ok(), "superseded name registry page missing");
  Require(superseded.descriptor.registry_status == page::PageRegistryStatus::superseded,
          "name registry page did not remain superseded");
  const auto superseded_claim = page::ValidatePageTypeProductSupportClaim(
      disk::PageType::name_registry_superseded, "standalone name registry authority");
  Require(!superseded_claim.ok(), "superseded name registry support claim passed");

  const auto row_primary = page::ValidatePageTypeFilespaceRole(
      disk::PageType::row_data, filespace::FilespaceRole::active_primary, "allocate");
  Require(row_primary.ok(), "row data rejected primary filespace role");
  const auto row_archive_allocate = page::ValidatePageTypeFilespaceRole(
      disk::PageType::row_data, filespace::FilespaceRole::archive_history, "allocate");
  Require(!row_archive_allocate.ok(), "row data allocation in archive role passed");
  Require(row_archive_allocate.diagnostic.diagnostic_code ==
              "SB_DIAG_PAGE_FILESPACE_ROLE_FORBIDDEN",
          "filespace role diagnostic mismatch");
  const auto shard_role = page::ValidatePageTypeFilespaceRole(
      disk::PageType::shard_placement_map, filespace::FilespaceRole::secondary_shard, "inspect");
  Require(shard_role.ok(), "shard placement rejected shard-bearing filespace role");

  disk::PageHeader header;
  header.page_type = disk::PageType::filespace_lifecycle_state;
  header.database_uuid = MakeUuid(UuidKind::database, CurrentUnixMillis() + 900).value;
  header.filespace_uuid = MakeUuid(UuidKind::filespace, CurrentUnixMillis() + 901).value;
  header.page_uuid = MakeUuid(UuidKind::page, CurrentUnixMillis() + 902).value;
  header.page_number = 44;
  header.page_generation = 1;
  const auto serialized = disk::SerializePageHeader(header);
  Require(serialized.ok(), "reserved page header serialization failed");
  const auto classified_header = disk::ClassifyPageHeader(serialized.serialized);
  Require(classified_header.kind == disk::PageClassificationKind::reserved_local,
          "reserved page type was not recognized as reserved");
  const auto classified_manager = page::ClassifyForPageManager(classified_header);
  Require(!classified_manager.may_read_body && !classified_manager.may_write_body,
          "reserved page type allowed body access");
  Require(classified_manager.diagnostic.diagnostic_code ==
              "SB_DIAG_PAGE_TYPE_STATUS_UNSUPPORTED",
          "reserved page manager diagnostic mismatch");
}

void TestIndexSpecialHeaderContract() {
  page::IndexSpecialHeader header;
  header.index_object_uuid = MakeUuid(UuidKind::object, CurrentUnixMillis() + 910);
  header.family_uuid = MakeUuid(UuidKind::object, CurrentUnixMillis() + 911);
  header.family = page::IndexPageFamilyKind::btree;
  header.page_type = disk::PageType::index_btree_root;
  header.resource_epoch = 1;
  header.mutation_epoch = 2;
  header.logical_page_number = 3;
  const auto built = page::BuildIndexSpecialHeader(header);
  Require(built.ok(), "IndexSpecialHeader build failed");
  const auto parsed = page::ParseIndexSpecialHeader(built.serialized);
  Require(parsed.ok(), "IndexSpecialHeader parse failed");
  Require(parsed.header.family == page::IndexPageFamilyKind::btree,
          "IndexSpecialHeader family mismatch");

  header.family = page::IndexPageFamilyKind::unknown;
  const auto refused = page::BuildIndexSpecialHeader(header);
  Require(!refused.ok(), "invalid IndexSpecialHeader was accepted");
  Require(refused.diagnostic.diagnostic_code ==
              "SB_DIAG_PAGE_INDEX_SPECIAL_HEADER_INVALID",
          "IndexSpecialHeader diagnostic mismatch");
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

void TestFilespaceQuarantineLifecycle(const std::filesystem::path& dir,
                                      const TypedUuid& database_uuid,
                                      const TypedUuid& primary_uuid,
                                      const TypedUuid& quarantine_uuid,
                                      const TypedUuid& pinned_uuid) {
  filespace::FilespaceRegistry registry;
  const auto writer_uuid = MakeUuid(UuidKind::object, CurrentUnixMillis() + 150);

  filespace::FilespaceOperationRequest create;
  create.operation = filespace::FilespaceOperation::create_filespace;
  create.database_uuid = database_uuid;
  create.filespace_uuid = primary_uuid;
  create.path = (dir / "quarantine-primary.sbfs").string();
  create.role = filespace::FilespaceRole::active_primary;
  create.physical_filespace_id = 1;
  create.total_pages = 64;
  create.free_pages = 48;
  create.preallocated_pages = 8;
  create.allocation_root_page = 4;
  create.header_generation = 1;
  create.writer_identity_uuid = writer_uuid;
  create.reason = "p2-quarantine-create-primary";
  const auto primary = filespace::ApplyFilespaceOperation(&registry, create);
  Require(primary.ok(), "quarantine primary create failed");

  filespace::FilespaceOperationRequest primary_quarantine = create;
  primary_quarantine.operation = filespace::FilespaceOperation::quarantine_filespace;
  primary_quarantine.reason = "p2-quarantine-active-primary-refused";
  const auto active_primary_refused =
      filespace::ApplyFilespaceOperation(&registry, primary_quarantine);
  Require(!active_primary_refused.ok(),
          "active primary filespace quarantine was admitted");
  Require(active_primary_refused.diagnostic.diagnostic_code ==
              "SB-FILESPACE-LIFECYCLE-ACTIVE-PRIMARY-QUARANTINE-FORBIDDEN",
          "active primary quarantine diagnostic mismatch");

  const auto quarantine_path = dir / "quarantine-secondary.sbfs";
  filespace::FilespaceOperationRequest secondary = create;
  secondary.filespace_uuid = quarantine_uuid;
  secondary.path = quarantine_path.string();
  secondary.role = filespace::FilespaceRole::secondary_data;
  secondary.physical_filespace_id = 2;
  secondary.reason = "p2-quarantine-create-secondary";
  const auto secondary_created = filespace::ApplyFilespaceOperation(&registry, secondary);
  Require(secondary_created.ok(), "quarantine secondary create failed");
  Require(!std::filesystem::exists(quarantine_path),
          "logical secondary create wrote a physical filespace file");

  filespace::FilespaceOperationRequest pinned = secondary;
  pinned.filespace_uuid = pinned_uuid;
  pinned.path = (dir / "quarantine-pinned.sbfs").string();
  pinned.physical_filespace_id = 3;
  pinned.reason = "p2-quarantine-create-pinned-secondary";
  const auto pinned_created = filespace::ApplyFilespaceOperation(&registry, pinned);
  Require(pinned_created.ok(), "quarantine pinned secondary create failed");

  filespace::FilespaceOperationRequest pin = pinned;
  pin.operation = filespace::FilespaceOperation::pin_filespace;
  pin.pin_kind = filespace::FilespacePinKind::transaction;
  pin.pin_owner = "p2-open-quarantine-blocker";
  pin.reason = "p2-quarantine-pin-secondary";
  const auto pin_result = filespace::ApplyFilespaceOperation(&registry, pin);
  Require(pin_result.ok(), "quarantine pin setup failed");

  filespace::FilespaceOperationRequest pinned_quarantine = pinned;
  pinned_quarantine.operation = filespace::FilespaceOperation::quarantine_filespace;
  pinned_quarantine.reason = "p2-quarantine-pinned-secondary-refused";
  const auto pinned_refused = filespace::ApplyFilespaceOperation(&registry, pinned_quarantine);
  Require(!pinned_refused.ok(), "pinned filespace quarantine was admitted");
  Require(pinned_refused.diagnostic.diagnostic_code ==
              "SB-FILESPACE-LIFECYCLE-QUARANTINE-PINNED",
          "pinned quarantine diagnostic mismatch");

  filespace::FilespaceOperationRequest quarantine = secondary;
  quarantine.operation = filespace::FilespaceOperation::quarantine_filespace;
  quarantine.reason = "p2-quarantine-secondary";
  const auto quarantined = filespace::ApplyFilespaceOperation(&registry, quarantine);
  Require(quarantined.ok(), "secondary filespace quarantine failed");
  Require(quarantined.descriptor.filespace_uuid.value == quarantine_uuid.value,
          "quarantine changed filespace identity");
  Require(quarantined.descriptor.role == filespace::FilespaceRole::secondary_data,
          "quarantine changed non-primary filespace role");
  Require(quarantined.descriptor.state == filespace::FilespaceState::quarantine,
          "quarantine did not set quarantine state");
  Require(!quarantined.descriptor.active && quarantined.descriptor.read_only,
          "quarantine did not fence ordinary access");
  Require(!quarantined.descriptor.startup_authority &&
              !quarantined.descriptor.catalog_persistence_owner &&
              !quarantined.descriptor.filespace_manifest_owner &&
              !quarantined.descriptor.recovery_evidence_owner &&
              !quarantined.descriptor.first_filespace,
          "quarantine retained root authority flags");
  Require(quarantined.descriptor.generation > secondary_created.descriptor.generation,
          "quarantine did not advance lifecycle generation");
  Require(quarantined.durable_state_changed,
          "quarantine did not report durable metadata change");
  Require(quarantined.cache_invalidation_required,
          "quarantine did not require cache invalidation");
  Require(quarantined.evidence.operation ==
              filespace::FilespaceOperation::quarantine_filespace,
          "quarantine evidence operation mismatch");
  Require(quarantined.evidence.previous_state == filespace::FilespaceState::attached &&
              quarantined.evidence.new_state == filespace::FilespaceState::quarantine,
          "quarantine evidence state transition mismatch");
  Require(!std::filesystem::exists(quarantine_path),
          "quarantine operation wrote or moved a physical filespace file");
}

void TestFilespaceMoveLifecycle(const std::filesystem::path& dir,
                                const TypedUuid& database_uuid,
                                const TypedUuid& primary_uuid,
                                const TypedUuid& move_uuid,
                                const TypedUuid& pinned_uuid) {
  filespace::FilespaceRegistry registry;
  const auto writer_uuid = MakeUuid(UuidKind::object, CurrentUnixMillis() + 151);

  filespace::FilespaceOperationRequest create;
  create.operation = filespace::FilespaceOperation::create_filespace;
  create.database_uuid = database_uuid;
  create.filespace_uuid = primary_uuid;
  create.path = (dir / "move-primary.sbfs").string();
  create.role = filespace::FilespaceRole::active_primary;
  create.physical_filespace_id = 1;
  create.total_pages = 64;
  create.free_pages = 48;
  create.preallocated_pages = 8;
  create.allocation_root_page = 4;
  create.header_generation = 1;
  create.writer_identity_uuid = writer_uuid;
  create.reason = "p2-move-create-primary";
  const auto primary = filespace::ApplyFilespaceOperation(&registry, create);
  Require(primary.ok(), "move primary create failed");

  filespace::FilespaceOperationRequest primary_move = create;
  primary_move.operation = filespace::FilespaceOperation::move_filespace;
  primary_move.path = (dir / "move-primary-target.sbfs").string();
  primary_move.policy.allow_filespace_move = true;
  primary_move.policy.page_agent_relocation_complete_for_move = true;
  primary_move.policy.startup_open_safe_for_move = true;
  primary_move.reason = "p2-move-active-primary-refused";
  const auto active_primary_refused =
      filespace::ApplyFilespaceOperation(&registry, primary_move);
  Require(!active_primary_refused.ok(), "active primary filespace move was admitted");
  Require(active_primary_refused.diagnostic.diagnostic_code ==
              "SB-FILESPACE-LIFECYCLE-ACTIVE-PRIMARY-MOVE-FORBIDDEN",
          "active primary move diagnostic mismatch");

  const auto source_path = dir / "move-secondary-source.sbfs";
  const auto target_path = dir / "move-secondary-target.sbfs";
  filespace::FilespaceOperationRequest secondary = create;
  secondary.filespace_uuid = move_uuid;
  secondary.path = source_path.string();
  secondary.role = filespace::FilespaceRole::secondary_data;
  secondary.physical_filespace_id = 2;
  secondary.reason = "p2-move-create-secondary";
  const auto secondary_created = filespace::ApplyFilespaceOperation(&registry, secondary);
  Require(secondary_created.ok(), "move secondary create failed");

  filespace::FilespaceOperationRequest missing_approval = secondary;
  missing_approval.operation = filespace::FilespaceOperation::move_filespace;
  missing_approval.path = target_path.string();
  missing_approval.reason = "p2-move-approval-missing";
  const auto approval_refused =
      filespace::ApplyFilespaceOperation(&registry, missing_approval);
  Require(!approval_refused.ok(), "filespace move admitted without proof");
  Require(approval_refused.diagnostic.diagnostic_code ==
              "SB-FILESPACE-MOVE-APPROVAL-MISSING",
          "move approval diagnostic mismatch");

  filespace::FilespaceOperationRequest pinned = secondary;
  pinned.filespace_uuid = pinned_uuid;
  pinned.path = (dir / "move-pinned-source.sbfs").string();
  pinned.physical_filespace_id = 3;
  pinned.reason = "p2-move-create-pinned-secondary";
  const auto pinned_created = filespace::ApplyFilespaceOperation(&registry, pinned);
  Require(pinned_created.ok(), "move pinned secondary create failed");

  filespace::FilespaceOperationRequest pin = pinned;
  pin.operation = filespace::FilespaceOperation::pin_filespace;
  pin.pin_kind = filespace::FilespacePinKind::transaction;
  pin.pin_owner = "p2-open-move-blocker";
  pin.reason = "p2-move-pin-secondary";
  const auto pin_result = filespace::ApplyFilespaceOperation(&registry, pin);
  Require(pin_result.ok(), "move pin setup failed");

  filespace::FilespaceOperationRequest pinned_move = pinned;
  pinned_move.operation = filespace::FilespaceOperation::move_filespace;
  pinned_move.path = (dir / "move-pinned-target.sbfs").string();
  pinned_move.policy.allow_filespace_move = true;
  pinned_move.policy.page_agent_relocation_complete_for_move = true;
  pinned_move.policy.startup_open_safe_for_move = true;
  pinned_move.reason = "p2-move-pinned-secondary-refused";
  const auto pinned_refused = filespace::ApplyFilespaceOperation(&registry, pinned_move);
  Require(!pinned_refused.ok(), "pinned filespace move was admitted");
  Require(pinned_refused.diagnostic.diagnostic_code == "SB-FILESPACE-MOVE-BLOCKED",
          "pinned move diagnostic mismatch");

  filespace::FilespaceOperationRequest move = secondary;
  move.operation = filespace::FilespaceOperation::move_filespace;
  move.path = target_path.string();
  move.policy.allow_filespace_move = true;
  move.policy.page_agent_relocation_complete_for_move = true;
  move.policy.startup_open_safe_for_move = true;
  move.reason = "p2-move-secondary";
  const auto moved = filespace::ApplyFilespaceOperation(&registry, move);
  Require(moved.ok(), "secondary filespace move failed");
  Require(moved.descriptor.filespace_uuid.value == move_uuid.value,
          "move changed filespace identity");
  Require(moved.descriptor.path == target_path.string(), "move did not update locator path");
  Require(moved.descriptor.role == filespace::FilespaceRole::secondary_data,
          "move changed secondary filespace role");
  Require(moved.descriptor.state == filespace::FilespaceState::attached,
          "move did not preserve attached state");
  Require(moved.descriptor.generation > secondary_created.descriptor.generation,
          "move did not advance lifecycle generation");
  Require(moved.durable_state_changed, "move did not report durable metadata change");
  Require(moved.cache_invalidation_required, "move did not require cache invalidation");
  Require(moved.evidence.operation == filespace::FilespaceOperation::move_filespace,
          "move evidence operation mismatch");
  Require(moved.evidence.previous_state == filespace::FilespaceState::attached &&
              moved.evidence.new_state == filespace::FilespaceState::attached,
          "move evidence state transition mismatch");
  Require(!std::filesystem::exists(source_path) && !std::filesystem::exists(target_path),
          "move operation wrote or moved a physical filespace file");
}

void TestFilespacePhysicalDeleteLifecycle(const std::filesystem::path& dir,
                                          const TypedUuid& database_uuid,
                                          const TypedUuid& primary_uuid,
                                          const TypedUuid& delete_uuid,
                                          const TypedUuid& pinned_uuid) {
  filespace::FilespaceRegistry registry;
  const auto writer_uuid = MakeUuid(UuidKind::object, CurrentUnixMillis() + 152);

  filespace::FilespaceOperationRequest create;
  create.operation = filespace::FilespaceOperation::create_filespace;
  create.database_uuid = database_uuid;
  create.filespace_uuid = primary_uuid;
  create.path = (dir / "physical-delete-primary.sbfs").string();
  create.role = filespace::FilespaceRole::active_primary;
  create.physical_filespace_id = 1;
  create.total_pages = 64;
  create.free_pages = 48;
  create.preallocated_pages = 8;
  create.allocation_root_page = 4;
  create.header_generation = 1;
  create.writer_identity_uuid = writer_uuid;
  create.reason = "p2-physical-delete-create-primary";
  const auto primary = filespace::ApplyFilespaceOperation(&registry, create);
  Require(primary.ok(), "physical delete primary create failed");

  filespace::FilespaceOperationRequest primary_delete = create;
  primary_delete.operation = filespace::FilespaceOperation::delete_physical_filespace;
  primary_delete.policy.allow_physical_filespace_delete = true;
  primary_delete.policy.physical_delete_legal_hold_clear = true;
  primary_delete.policy.physical_delete_retention_satisfied = true;
  primary_delete.policy.physical_delete_cleanup_horizon_authoritative = true;
  primary_delete.reason = "p2-physical-delete-active-primary-refused";
  const auto active_primary_refused =
      filespace::ApplyFilespaceOperation(&registry, primary_delete);
  Require(!active_primary_refused.ok(),
          "active primary physical filespace delete was admitted");
  Require(active_primary_refused.diagnostic.diagnostic_code ==
              "SB-FILESPACE-LIFECYCLE-ACTIVE-PRIMARY-PHYSICAL-DELETE-FORBIDDEN",
          "active primary physical delete diagnostic mismatch");

  const auto delete_path = dir / "physical-delete-secondary.sbfs";
  filespace::FilespaceOperationRequest secondary = create;
  secondary.filespace_uuid = delete_uuid;
  secondary.path = delete_path.string();
  secondary.role = filespace::FilespaceRole::secondary_data;
  secondary.physical_filespace_id = 2;
  secondary.reason = "p2-physical-delete-create-secondary";
  const auto secondary_created = filespace::ApplyFilespaceOperation(&registry, secondary);
  Require(secondary_created.ok(), "physical delete secondary create failed");
  {
    std::ofstream physical(delete_path);
    physical << "scratchbird physical delete fixture";
  }
  Require(std::filesystem::exists(delete_path), "physical delete fixture file missing");

  filespace::FilespaceOperationRequest unfenced_delete = secondary;
  unfenced_delete.operation = filespace::FilespaceOperation::delete_physical_filespace;
  unfenced_delete.policy.allow_physical_filespace_delete = true;
  unfenced_delete.policy.physical_delete_legal_hold_clear = true;
  unfenced_delete.policy.physical_delete_retention_satisfied = true;
  unfenced_delete.policy.physical_delete_cleanup_horizon_authoritative = true;
  unfenced_delete.reason = "p2-physical-delete-unfenced-refused";
  const auto unfenced_refused =
      filespace::ApplyFilespaceOperation(&registry, unfenced_delete);
  Require(!unfenced_refused.ok(), "unfenced physical filespace delete was admitted");
  Require(unfenced_refused.diagnostic.diagnostic_code ==
              "SB-FILESPACE-LIFECYCLE-PHYSICAL-DELETE-NOT-FENCED",
          "unfenced physical delete diagnostic mismatch");
  Require(std::filesystem::exists(delete_path),
          "unfenced physical delete removed the file");

  filespace::FilespaceOperationRequest logical_drop = secondary;
  logical_drop.operation = filespace::FilespaceOperation::drop_filespace;
  logical_drop.reason = "p2-physical-delete-logical-drop";
  const auto dropped = filespace::ApplyFilespaceOperation(&registry, logical_drop);
  Require(dropped.ok(), "physical delete logical drop setup failed");
  Require(dropped.descriptor.state == filespace::FilespaceState::drop_pending,
          "physical delete setup did not enter drop_pending");

  filespace::FilespaceOperationRequest missing_policy = secondary;
  missing_policy.operation = filespace::FilespaceOperation::delete_physical_filespace;
  missing_policy.reason = "p2-physical-delete-policy-missing";
  const auto policy_refused =
      filespace::ApplyFilespaceOperation(&registry, missing_policy);
  Require(!policy_refused.ok(), "physical delete admitted without delete policy");
  Require(policy_refused.diagnostic.diagnostic_code ==
              "SB-FILESPACE-LIFECYCLE-PHYSICAL-DELETE-FORBIDDEN",
          "physical delete policy diagnostic mismatch");
  Require(std::filesystem::exists(delete_path),
          "policy-refused physical delete removed the file");

  filespace::FilespaceOperationRequest legal_hold = missing_policy;
  legal_hold.policy.allow_physical_filespace_delete = true;
  legal_hold.reason = "p2-physical-delete-legal-hold";
  const auto legal_refused = filespace::ApplyFilespaceOperation(&registry, legal_hold);
  Require(!legal_refused.ok(), "physical delete admitted under legal hold");
  Require(legal_refused.diagnostic.diagnostic_code ==
              "SB-FILESPACE-LIFECYCLE-PHYSICAL-DELETE-LEGAL-HOLD",
          "physical delete legal-hold diagnostic mismatch");

  filespace::FilespaceOperationRequest retention = legal_hold;
  retention.policy.physical_delete_legal_hold_clear = true;
  retention.reason = "p2-physical-delete-retention";
  const auto retention_refused = filespace::ApplyFilespaceOperation(&registry, retention);
  Require(!retention_refused.ok(), "physical delete admitted before retention");
  Require(retention_refused.diagnostic.diagnostic_code ==
              "SB-FILESPACE-LIFECYCLE-PHYSICAL-DELETE-RETENTION",
          "physical delete retention diagnostic mismatch");

  filespace::FilespaceOperationRequest cleanup_horizon = retention;
  cleanup_horizon.policy.physical_delete_retention_satisfied = true;
  cleanup_horizon.reason = "p2-physical-delete-cleanup-horizon";
  const auto cleanup_refused =
      filespace::ApplyFilespaceOperation(&registry, cleanup_horizon);
  Require(!cleanup_refused.ok(), "physical delete admitted without cleanup horizon");
  Require(cleanup_refused.diagnostic.diagnostic_code ==
              "SB-FILESPACE-LIFECYCLE-PHYSICAL-DELETE-CLEANUP-HORIZON",
          "physical delete cleanup-horizon diagnostic mismatch");
  Require(std::filesystem::exists(delete_path),
          "cleanup-horizon-refused physical delete removed the file");

  const auto pinned_path = dir / "physical-delete-pinned.sbfs";
  filespace::FilespaceOperationRequest pinned = secondary;
  pinned.filespace_uuid = pinned_uuid;
  pinned.path = pinned_path.string();
  pinned.physical_filespace_id = 3;
  pinned.reason = "p2-physical-delete-create-pinned";
  const auto pinned_created = filespace::ApplyFilespaceOperation(&registry, pinned);
  Require(pinned_created.ok(), "physical delete pinned create failed");
  {
    std::ofstream physical(pinned_path);
    physical << "scratchbird pinned physical delete fixture";
  }
  filespace::FilespaceOperationRequest pinned_drop = pinned;
  pinned_drop.operation = filespace::FilespaceOperation::drop_filespace;
  pinned_drop.reason = "p2-physical-delete-pinned-logical-drop";
  const auto pinned_dropped = filespace::ApplyFilespaceOperation(&registry, pinned_drop);
  Require(pinned_dropped.ok(), "physical delete pinned logical drop failed");
  filespace::FilespaceOperationRequest pin = pinned;
  pin.operation = filespace::FilespaceOperation::pin_filespace;
  pin.pin_kind = filespace::FilespacePinKind::transaction;
  pin.pin_owner = "p2-open-delete-blocker";
  pin.reason = "p2-physical-delete-pin";
  const auto pinned_marker = filespace::ApplyFilespaceOperation(&registry, pin);
  Require(pinned_marker.ok(), "physical delete pin setup failed");
  filespace::FilespaceOperationRequest pinned_delete = cleanup_horizon;
  pinned_delete.filespace_uuid = pinned_uuid;
  pinned_delete.path = pinned_path.string();
  pinned_delete.policy.physical_delete_cleanup_horizon_authoritative = true;
  pinned_delete.reason = "p2-physical-delete-pinned-refused";
  const auto pinned_refused =
      filespace::ApplyFilespaceOperation(&registry, pinned_delete);
  Require(!pinned_refused.ok(), "pinned physical filespace delete was admitted");
  Require(pinned_refused.diagnostic.diagnostic_code ==
              "SB-FILESPACE-LIFECYCLE-PHYSICAL-DELETE-PINNED",
          "pinned physical delete diagnostic mismatch");
  Require(std::filesystem::exists(pinned_path),
          "pinned physical delete removed the file");

  filespace::FilespaceOperationRequest physical_delete = cleanup_horizon;
  physical_delete.policy.physical_delete_cleanup_horizon_authoritative = true;
  physical_delete.reason = "p2-physical-delete-success";
  const auto deleted = filespace::ApplyFilespaceOperation(&registry, physical_delete);
  Require(deleted.ok(), "physical filespace delete failed");
  Require(!std::filesystem::exists(delete_path),
          "physical filespace delete did not remove the file");
  Require(deleted.physical_file_removed, "physical filespace delete result lacked removal evidence");
  Require(deleted.descriptor.state == filespace::FilespaceState::dropped,
          "physical filespace delete did not mark descriptor dropped");
  Require(!deleted.descriptor.active && deleted.descriptor.read_only,
          "physical filespace delete did not fence descriptor");
  Require(deleted.durable_state_changed,
          "physical filespace delete did not report durable metadata change");
  Require(deleted.cache_invalidation_required,
          "physical filespace delete did not require cache invalidation");
  Require(deleted.evidence.operation ==
              filespace::FilespaceOperation::delete_physical_filespace,
          "physical filespace delete evidence operation mismatch");
  Require(deleted.evidence.previous_state == filespace::FilespaceState::drop_pending &&
              deleted.evidence.new_state == filespace::FilespaceState::dropped,
          "physical filespace delete evidence state transition mismatch");
}

const filespace::FilespaceDescriptor* DescriptorByUuid(
    const filespace::FilespaceRegistry& registry,
    const TypedUuid& filespace_uuid) {
  for (const auto& descriptor : registry.filespaces) {
    if (descriptor.filespace_uuid.value == filespace_uuid.value) {
      return &descriptor;
    }
  }
  return nullptr;
}

void TestFilespaceMergeLifecycle(const std::filesystem::path& dir,
                                 const TypedUuid& database_uuid,
                                 const TypedUuid& primary_uuid,
                                 const TypedUuid& source_uuid,
                                 const TypedUuid& target_uuid,
                                 const TypedUuid& pinned_uuid) {
  filespace::FilespaceRegistry registry;
  const auto writer_uuid = MakeUuid(UuidKind::object, CurrentUnixMillis() + 153);

  filespace::FilespaceOperationRequest create;
  create.operation = filespace::FilespaceOperation::create_filespace;
  create.database_uuid = database_uuid;
  create.filespace_uuid = primary_uuid;
  create.path = (dir / "merge-primary.sbfs").string();
  create.role = filespace::FilespaceRole::active_primary;
  create.physical_filespace_id = 1;
  create.total_pages = 64;
  create.free_pages = 48;
  create.preallocated_pages = 8;
  create.allocation_root_page = 4;
  create.header_generation = 1;
  create.writer_identity_uuid = writer_uuid;
  create.reason = "p2-merge-create-primary";
  const auto primary = filespace::ApplyFilespaceOperation(&registry, create);
  Require(primary.ok(), "merge primary create failed");

  filespace::FilespaceOperationRequest primary_merge = create;
  primary_merge.operation = filespace::FilespaceOperation::merge_filespace;
  primary_merge.merge_target_filespace_uuid = target_uuid;
  primary_merge.policy.allow_filespace_merge = true;
  primary_merge.policy.page_agent_merge_complete_for_merge = true;
  primary_merge.policy.startup_open_safe_for_merge = true;
  primary_merge.reason = "p2-merge-active-primary-refused";
  const auto active_primary_refused =
      filespace::ApplyFilespaceOperation(&registry, primary_merge);
  Require(!active_primary_refused.ok(), "active primary filespace merge was admitted");
  Require(active_primary_refused.diagnostic.diagnostic_code ==
              "SB-FILESPACE-LIFECYCLE-ACTIVE-PRIMARY-MERGE-FORBIDDEN",
          "active primary merge diagnostic mismatch");

  const auto target_path = dir / "merge-target.sbfs";
  filespace::FilespaceOperationRequest target = create;
  target.filespace_uuid = target_uuid;
  target.path = target_path.string();
  target.role = filespace::FilespaceRole::secondary_data;
  target.physical_filespace_id = 2;
  target.reason = "p2-merge-create-target";
  const auto target_created = filespace::ApplyFilespaceOperation(&registry, target);
  Require(target_created.ok(), "merge target create failed");

  const auto source_path = dir / "merge-source.sbfs";
  filespace::FilespaceOperationRequest source = create;
  source.filespace_uuid = source_uuid;
  source.path = source_path.string();
  source.role = filespace::FilespaceRole::secondary_data;
  source.physical_filespace_id = 3;
  source.reason = "p2-merge-create-source";
  const auto source_created = filespace::ApplyFilespaceOperation(&registry, source);
  Require(source_created.ok(), "merge source create failed");

  filespace::FilespaceOperationRequest missing_approval = source;
  missing_approval.operation = filespace::FilespaceOperation::merge_filespace;
  missing_approval.merge_target_filespace_uuid = target_uuid;
  missing_approval.reason = "p2-merge-approval-missing";
  const auto approval_refused =
      filespace::ApplyFilespaceOperation(&registry, missing_approval);
  Require(!approval_refused.ok(), "filespace merge admitted without proof");
  Require(approval_refused.diagnostic.diagnostic_code ==
              "SB-FILESPACE-LIFECYCLE-MERGE-FORBIDDEN",
          "merge approval diagnostic mismatch");

  filespace::FilespaceOperationRequest pinned = source;
  pinned.filespace_uuid = pinned_uuid;
  pinned.path = (dir / "merge-pinned-source.sbfs").string();
  pinned.physical_filespace_id = 4;
  pinned.reason = "p2-merge-create-pinned-source";
  const auto pinned_created = filespace::ApplyFilespaceOperation(&registry, pinned);
  Require(pinned_created.ok(), "merge pinned source create failed");

  filespace::FilespaceOperationRequest pin = pinned;
  pin.operation = filespace::FilespaceOperation::pin_filespace;
  pin.pin_kind = filespace::FilespacePinKind::transaction;
  pin.pin_owner = "p2-open-merge-blocker";
  pin.reason = "p2-merge-pin-source";
  const auto pin_result = filespace::ApplyFilespaceOperation(&registry, pin);
  Require(pin_result.ok(), "merge pin setup failed");

  filespace::FilespaceOperationRequest pinned_merge = pinned;
  pinned_merge.operation = filespace::FilespaceOperation::merge_filespace;
  pinned_merge.merge_target_filespace_uuid = target_uuid;
  pinned_merge.policy.allow_filespace_merge = true;
  pinned_merge.policy.page_agent_merge_complete_for_merge = true;
  pinned_merge.policy.startup_open_safe_for_merge = true;
  pinned_merge.reason = "p2-merge-pinned-source-refused";
  const auto pinned_refused = filespace::ApplyFilespaceOperation(&registry, pinned_merge);
  Require(!pinned_refused.ok(), "pinned filespace merge was admitted");
  Require(pinned_refused.diagnostic.diagnostic_code ==
              "SB-FILESPACE-LIFECYCLE-MERGE-PINNED",
          "pinned merge diagnostic mismatch");

  filespace::FilespaceOperationRequest merge = source;
  merge.operation = filespace::FilespaceOperation::merge_filespace;
  merge.merge_target_filespace_uuid = target_uuid;
  merge.policy.allow_filespace_merge = true;
  merge.policy.page_agent_merge_complete_for_merge = true;
  merge.policy.startup_open_safe_for_merge = true;
  merge.reason = "p2-merge-source-into-target";
  const auto merged = filespace::ApplyFilespaceOperation(&registry, merge);
  Require(merged.ok(), "secondary filespace merge failed");
  Require(merged.descriptor.filespace_uuid.value == source_uuid.value,
          "merge changed source filespace identity");
  Require(merged.descriptor.role == filespace::FilespaceRole::drop_pending,
          "merge did not retire source role");
  Require(merged.descriptor.state == filespace::FilespaceState::drop_pending,
          "merge did not leave source drop_pending");
  Require(!merged.descriptor.active && merged.descriptor.read_only,
          "merge did not fence source ordinary access");
  Require(!merged.descriptor.startup_authority &&
              !merged.descriptor.catalog_persistence_owner &&
              !merged.descriptor.filespace_manifest_owner &&
              !merged.descriptor.recovery_evidence_owner &&
              !merged.descriptor.first_filespace,
          "merge source retained root authority flags");
  Require(merged.descriptor.generation > source_created.descriptor.generation,
          "merge did not advance source lifecycle generation");
  Require(merged.durable_state_changed, "merge did not report durable metadata change");
  Require(merged.cache_invalidation_required, "merge did not require cache invalidation");
  Require(merged.evidence.operation == filespace::FilespaceOperation::merge_filespace,
          "merge evidence operation mismatch");
  Require(merged.evidence.previous_state == filespace::FilespaceState::attached &&
              merged.evidence.new_state == filespace::FilespaceState::drop_pending,
          "merge evidence state transition mismatch");
  Require(merged.evidence.previous_role == filespace::FilespaceRole::secondary_data &&
              merged.evidence.new_role == filespace::FilespaceRole::drop_pending,
          "merge evidence role transition mismatch");

  const auto* target_after = DescriptorByUuid(registry, target_uuid);
  Require(target_after != nullptr, "merge target disappeared from registry");
  Require(target_after->role == filespace::FilespaceRole::secondary_data &&
              target_after->state == filespace::FilespaceState::attached &&
              target_after->active,
          "merge target descriptor state drifted");
  Require(target_after->generation > target_created.descriptor.generation,
          "merge did not advance target lifecycle generation");
  Require(!std::filesystem::exists(source_path) && !std::filesystem::exists(target_path),
          "merge operation wrote or moved a physical filespace file");
}

filespace::PhysicalFilespaceHeader ReplacementPhysicalHeader(const TypedUuid& database_uuid,
                                                             const TypedUuid& filespace_uuid,
                                                             filespace::FilespaceRole role,
                                                             filespace::FilespaceState state,
                                                             int physical_filespace_id,
                                                             const TypedUuid& writer_uuid,
                                                             std::string operation_uuid) {
  filespace::PhysicalFilespaceHeader header;
  header.database_uuid = database_uuid;
  header.filespace_uuid = filespace_uuid;
  header.role = role;
  header.state = state;
  header.page_size = 16384;
  header.physical_filespace_id = physical_filespace_id;
  header.total_pages = 128;
  header.free_pages = 96;
  header.preallocated_pages = 16;
  header.allocation_root_page = 4;
  header.header_generation = 1;
  header.writer_identity_uuid = writer_uuid;
  header.creation_operation_uuid = std::move(operation_uuid);
  return header;
}

void WriteReplacementPhysicalHeader(const std::filesystem::path& path,
                                    const filespace::PhysicalFilespaceHeader& header,
                                    std::string_view label) {
  const auto written = filespace::WritePhysicalFilespaceHeader(path.string(), header, true);
  Require(written.ok(), std::string(label) + " physical header write failed");
}

void TestFilespaceRepairRebuildSalvageLifecycle(const std::filesystem::path& dir,
                                                const TypedUuid& database_uuid,
                                                const TypedUuid& primary_uuid,
                                                const std::vector<TypedUuid>& filespace_uuids) {
  filespace::FilespaceRegistry registry;
  const auto writer_uuid = MakeUuid(UuidKind::object, CurrentUnixMillis() + 174);

  filespace::FilespaceOperationRequest create;
  create.operation = filespace::FilespaceOperation::create_filespace;
  create.database_uuid = database_uuid;
  create.path = (dir / "repair-primary.sbfs").string();
  create.filespace_uuid = primary_uuid;
  create.role = filespace::FilespaceRole::active_primary;
  create.physical_filespace_id = 1;
  create.total_pages = 128;
  create.free_pages = 96;
  create.preallocated_pages = 16;
  create.allocation_root_page = 4;
  create.header_generation = 1;
  create.writer_identity_uuid = writer_uuid;
  create.reason = "p2-repair-create-primary";
  const auto primary = filespace::ApplyFilespaceOperation(&registry, create);
  Require(primary.ok(), "filespace repair primary create failed");

  filespace::FilespaceOperationRequest primary_repair = create;
  primary_repair.operation = filespace::FilespaceOperation::repair_filespace;
  primary_repair.policy.allow_filespace_repair = true;
  primary_repair.policy.repair_plan_authorized = true;
  primary_repair.policy.repair_evidence_preserved = true;
  primary_repair.reason = "p2-repair-active-primary-refused";
  const auto primary_refused = filespace::ApplyFilespaceOperation(&registry, primary_repair);
  Require(!primary_refused.ok(), "active-primary repair was admitted");
  Require(primary_refused.diagnostic.diagnostic_code ==
              "SB-FILESPACE-LIFECYCLE-ACTIVE-PRIMARY-REPAIR-FORBIDDEN",
          "active-primary repair diagnostic mismatch");

  auto create_secondary = [&](const TypedUuid& filespace_uuid,
                              const std::filesystem::path& path,
                              int physical_id,
                              std::string reason) {
    filespace::FilespaceOperationRequest request = create;
    request.filespace_uuid = filespace_uuid;
    request.path = path.string();
    request.role = filespace::FilespaceRole::secondary_data;
    request.physical_filespace_id = static_cast<unsigned short>(physical_id);
    request.reason = std::move(reason);
    const auto created = filespace::ApplyFilespaceOperation(&registry, request);
    Require(created.ok(), "filespace repair secondary create failed");
    return request;
  };

  const auto repair_path = dir / "repair-secondary.sbfs";
  auto repair = create_secondary(filespace_uuids.at(0),
                                 repair_path,
                                 2,
                                 "p2-repair-create-secondary");
  WriteReplacementPhysicalHeader(
      repair_path,
      ReplacementPhysicalHeader(database_uuid,
                                filespace_uuids.at(0),
                                filespace::FilespaceRole::secondary_data,
                                filespace::FilespaceState::attached,
                                2,
                                writer_uuid,
                                "p2-repair-header"),
      "repair");

  filespace::FilespaceOperationRequest repair_missing_policy = repair;
  repair_missing_policy.operation = filespace::FilespaceOperation::repair_filespace;
  repair_missing_policy.reason = "p2-repair-missing-policy";
  const auto repair_refused =
      filespace::ApplyFilespaceOperation(&registry, repair_missing_policy);
  Require(!repair_refused.ok(), "filespace repair admitted without policy");
  Require(repair_refused.diagnostic.diagnostic_code ==
              "SB-FILESPACE-LIFECYCLE-REPAIR-FORBIDDEN",
          "filespace repair missing-policy diagnostic mismatch");

  repair.operation = filespace::FilespaceOperation::repair_filespace;
  repair.policy.allow_filespace_repair = true;
  repair.policy.repair_plan_authorized = true;
  repair.policy.repair_evidence_preserved = true;
  repair.reason = "p2-repair-success";
  const auto repaired = filespace::ApplyFilespaceOperation(&registry, repair);
  Require(repaired.ok(), "filespace repair failed");
  Require(repaired.descriptor.state == filespace::FilespaceState::read_only &&
              repaired.descriptor.read_only && repaired.descriptor.active,
          "filespace repair did not fence descriptor read-only");
  const auto repaired_header = filespace::ReadPhysicalFilespaceHeader(repair_path.string());
  Require(repaired_header.ok(), "filespace repair header read failed");
  Require(repaired_header.header.state == filespace::FilespaceState::read_only,
          "filespace repair did not rewrite physical header state");

  const auto rebuild_path = dir / "rebuild-secondary.sbfs";
  auto rebuild = create_secondary(filespace_uuids.at(1),
                                  rebuild_path,
                                  3,
                                  "p2-rebuild-create-secondary");
  {
    std::ofstream damaged(rebuild_path);
    damaged << "damaged filespace header";
  }
  rebuild.operation = filespace::FilespaceOperation::rebuild_filespace;
  rebuild.policy.allow_filespace_rebuild = true;
  rebuild.policy.repair_plan_authorized = true;
  rebuild.policy.repair_evidence_preserved = true;
  rebuild.policy.page_agent_rebuild_complete = true;
  rebuild.policy.startup_open_safe_for_rebuild = true;
  rebuild.reason = "p2-rebuild-source-missing";
  const auto rebuild_refused = filespace::ApplyFilespaceOperation(&registry, rebuild);
  Require(!rebuild_refused.ok(), "filespace rebuild admitted without source proof");
  Require(rebuild_refused.diagnostic.diagnostic_code ==
              "SB-FILESPACE-LIFECYCLE-REBUILD-SOURCE-UNVERIFIED",
          "filespace rebuild source diagnostic mismatch");
  rebuild.policy.rebuild_source_verified = true;
  rebuild.reason = "p2-rebuild-success";
  const auto rebuilt = filespace::ApplyFilespaceOperation(&registry, rebuild);
  Require(rebuilt.ok(), "filespace rebuild failed");
  const auto rebuilt_header = filespace::ReadPhysicalFilespaceHeader(rebuild_path.string());
  Require(rebuilt_header.ok(), "filespace rebuild did not create valid physical header");
  Require(rebuilt_header.header.state == filespace::FilespaceState::read_only,
          "filespace rebuild did not fence physical header read-only");

  const auto salvage_path = dir / "salvage-secondary.sbfs";
  auto salvage = create_secondary(filespace_uuids.at(2),
                                  salvage_path,
                                  4,
                                  "p2-salvage-create-secondary");
  WriteReplacementPhysicalHeader(
      salvage_path,
      ReplacementPhysicalHeader(database_uuid,
                                filespace_uuids.at(2),
                                filespace::FilespaceRole::secondary_data,
                                filespace::FilespaceState::attached,
                                4,
                                writer_uuid,
                                "p2-salvage-header"),
      "salvage");
  salvage.operation = filespace::FilespaceOperation::salvage_filespace;
  salvage.policy.allow_filespace_salvage = true;
  salvage.policy.repair_plan_authorized = true;
  salvage.policy.repair_evidence_preserved = true;
  salvage.policy.salvage_output_quarantined = true;
  salvage.reason = "p2-salvage-review-missing";
  const auto salvage_refused = filespace::ApplyFilespaceOperation(&registry, salvage);
  Require(!salvage_refused.ok(), "filespace salvage admitted without review");
  Require(salvage_refused.diagnostic.diagnostic_code ==
              "SB-FILESPACE-LIFECYCLE-SALVAGE-REVIEW-REQUIRED",
          "filespace salvage review diagnostic mismatch");
  salvage.policy.salvage_review_authorized = true;
  salvage.reason = "p2-salvage-success";
  const auto salvaged = filespace::ApplyFilespaceOperation(&registry, salvage);
  Require(salvaged.ok(), "filespace salvage failed");
  Require(salvaged.descriptor.role == filespace::FilespaceRole::archive_detached &&
              salvaged.descriptor.state == filespace::FilespaceState::quarantine &&
              !salvaged.descriptor.active && salvaged.descriptor.read_only,
          "filespace salvage did not isolate descriptor");
  const auto salvage_header = filespace::ReadPhysicalFilespaceHeader(salvage_path.string());
  Require(salvage_header.ok(), "filespace salvage header read failed");
  Require(salvage_header.header.role == filespace::FilespaceRole::archive_detached &&
              salvage_header.header.state == filespace::FilespaceState::quarantine,
          "filespace salvage did not quarantine physical header");
}

void RequireSingleActivePrimaryAuthority(const filespace::FilespaceRegistry& registry) {
  int active_primary_count = 0;
  int startup_owner_count = 0;
  int catalog_owner_count = 0;
  int manifest_owner_count = 0;
  int recovery_owner_count = 0;
  for (const auto& descriptor : registry.filespaces) {
    if (descriptor.role == filespace::FilespaceRole::active_primary) ++active_primary_count;
    if (descriptor.startup_authority) ++startup_owner_count;
    if (descriptor.catalog_persistence_owner) ++catalog_owner_count;
    if (descriptor.filespace_manifest_owner) ++manifest_owner_count;
    if (descriptor.recovery_evidence_owner) ++recovery_owner_count;
  }
  Require(active_primary_count == 1, "primary replacement left invalid active primary count");
  Require(startup_owner_count == 1, "primary replacement left invalid startup authority count");
  Require(catalog_owner_count == 1, "primary replacement left invalid catalog owner count");
  Require(manifest_owner_count == 1, "primary replacement left invalid manifest owner count");
  Require(recovery_owner_count == 1, "primary replacement left invalid recovery owner count");
}

void TestActivePrimaryReplacementAuthoritySwitch(const std::filesystem::path& dir,
                                                 const TypedUuid& database_uuid,
                                                 const TypedUuid& primary_uuid,
                                                 const TypedUuid& replacement_uuid) {
  filespace::FilespaceRegistry registry;
  const auto writer_uuid = MakeUuid(UuidKind::object, CurrentUnixMillis() + 300);
  const auto primary_path = dir / "replace_primary.sbfs";
  const auto replacement_path = dir / "replace_candidate.sbfs";

  filespace::FilespaceOperationRequest create;
  create.operation = filespace::FilespaceOperation::create_filespace;
  create.database_uuid = database_uuid;
  create.filespace_uuid = primary_uuid;
  create.path = primary_path.string();
  create.role = filespace::FilespaceRole::active_primary;
  create.physical_filespace_id = 1;
  create.total_pages = 128;
  create.free_pages = 96;
  create.preallocated_pages = 16;
  create.allocation_root_page = 4;
  create.header_generation = 1;
  create.writer_identity_uuid = writer_uuid;
  create.reason = "p2-create-active-primary-for-replacement";
  const auto primary = filespace::ApplyFilespaceOperation(&registry, create);
  Require(primary.ok(), "replacement active primary create failed");
  WriteReplacementPhysicalHeader(
      primary_path,
      ReplacementPhysicalHeader(database_uuid,
                                primary_uuid,
                                filespace::FilespaceRole::active_primary,
                                filespace::FilespaceState::online,
                                1,
                                writer_uuid,
                                "p2-create-active-primary-physical-header"),
      "active primary");

  filespace::FilespaceOperationRequest attach;
  attach.operation = filespace::FilespaceOperation::attach_filespace;
  attach.database_uuid = database_uuid;
  attach.filespace_uuid = replacement_uuid;
  attach.path = replacement_path.string();
  attach.role = filespace::FilespaceRole::primary_candidate;
  attach.physical_filespace_id = 2;
  attach.total_pages = 128;
  attach.free_pages = 96;
  attach.preallocated_pages = 16;
  attach.allocation_root_page = 4;
  attach.writer_identity_uuid = writer_uuid;
  attach.reason = "p2-attach-primary-candidate";
  attach.policy.require_physical_header_for_attach = true;
  WriteReplacementPhysicalHeader(
      replacement_path,
      ReplacementPhysicalHeader(database_uuid,
                                replacement_uuid,
                                filespace::FilespaceRole::primary_candidate,
                                filespace::FilespaceState::online,
                                2,
                                writer_uuid,
                                "p2-create-primary-candidate-physical-header"),
      "primary candidate");
  const auto attached = filespace::ApplyFilespaceOperation(&registry, attach);
  Require(attached.ok(), "primary candidate attach failed");
  Require(attached.descriptor.role == filespace::FilespaceRole::primary_candidate,
          "attached replacement filespace was not a primary candidate");

  filespace::FilespaceOperationRequest promote = attach;
  promote.operation = filespace::FilespaceOperation::promote_filespace;
  promote.reason = "p2-promote-primary-candidate-without-replacement";
  promote.policy.allow_primary_replacement = false;
  promote.policy.require_physical_header_for_promote = true;
  const auto replacement_refused = filespace::ApplyFilespaceOperation(&registry, promote);
  Require(!replacement_refused.ok(), "primary replacement was admitted without replacement policy");
  Require(replacement_refused.diagnostic.diagnostic_code ==
              "SB-FILESPACE-LIFECYCLE-PRIMARY-ALREADY-EXISTS",
          "primary replacement refusal diagnostic mismatch");

  promote.reason = "p2-promote-primary-candidate-with-replacement";
  promote.policy.allow_primary_replacement = true;
  const auto promoted = filespace::ApplyFilespaceOperation(&registry, promote);
  Require(promoted.ok(), "primary replacement promotion failed");
  Require(promoted.descriptor.role == filespace::FilespaceRole::active_primary,
          "replacement filespace did not become active primary");
  Require(promoted.descriptor.startup_authority &&
              promoted.descriptor.catalog_persistence_owner &&
              promoted.descriptor.filespace_manifest_owner &&
              promoted.descriptor.recovery_evidence_owner,
          "replacement active primary did not receive all root authority flags");
  Require(promoted.evidence.operation == filespace::FilespaceOperation::promote_filespace,
          "primary replacement promotion evidence operation mismatch");
  Require(promoted.evidence.previous_role == filespace::FilespaceRole::primary_candidate &&
              promoted.evidence.new_role == filespace::FilespaceRole::active_primary,
          "primary replacement promotion evidence role transition mismatch");
  Require(promoted.durable_state_changed, "primary replacement did not report durable state change");

  const auto* old_primary = DescriptorByUuid(registry, primary_uuid);
  Require(old_primary != nullptr, "old active primary disappeared from registry");
  Require(old_primary->role == filespace::FilespaceRole::primary_shadow,
          "old active primary was not demoted to primary shadow");
  Require(old_primary->read_only, "old active primary shadow was not fenced read-only");
  Require(!old_primary->startup_authority &&
              !old_primary->catalog_persistence_owner &&
              !old_primary->filespace_manifest_owner &&
              !old_primary->recovery_evidence_owner,
          "old active primary retained root authority after replacement");
  Require(old_primary->generation > primary.descriptor.generation,
          "old active primary demotion did not advance lifecycle generation");
  Require(old_primary->header_generation == 2,
          "old active primary physical header generation was not advanced");

  const auto* replacement = DescriptorByUuid(registry, replacement_uuid);
  Require(replacement != nullptr, "replacement active primary missing from registry");
  Require(replacement->role == filespace::FilespaceRole::active_primary,
          "replacement registry descriptor role mismatch");
  Require(!replacement->read_only, "replacement active primary was left read-only");
  Require(replacement->header_generation == 2,
          "replacement active primary physical header generation was not advanced");
  RequireSingleActivePrimaryAuthority(registry);

  const auto promoted_header = filespace::ReadPhysicalFilespaceHeader(replacement_path.string());
  Require(promoted_header.ok(), "replacement physical header did not read after promote");
  Require(promoted_header.header.role == filespace::FilespaceRole::active_primary,
          "replacement physical header did not become active primary");
  Require(promoted_header.header.state == filespace::FilespaceState::online,
          "replacement physical header state mismatch");
  Require(promoted_header.header.header_generation == 2,
          "replacement physical header generation mismatch");

  const auto demoted_header = filespace::ReadPhysicalFilespaceHeader(primary_path.string());
  Require(demoted_header.ok(), "old primary physical header did not read after demotion");
  Require(demoted_header.header.role == filespace::FilespaceRole::primary_shadow,
          "old primary physical header was not demoted to shadow");
  Require(demoted_header.header.state == filespace::FilespaceState::read_only,
          "old primary physical header was not fenced read-only");
  Require(demoted_header.header.header_generation == 2,
          "old primary physical header generation mismatch");

  page::PageFilespaceAgentRequestPolicy relocation_policy;
  relocation_policy.policy_uuid = MakeUuid(UuidKind::object, CurrentUnixMillis() + 310);
  relocation_policy.page_agent_may_relocate_pages = true;

  page::PageFilespaceAgentRequest relocation;
  relocation.request_uuid = MakeUuid(UuidKind::object, CurrentUnixMillis() + 311);
  relocation.database_uuid = database_uuid;
  relocation.filespace_uuid = replacement_uuid;
  relocation.policy_uuid = relocation_policy.policy_uuid;
  relocation.kind = page::PageFilespaceAgentRequestKind::relocate_pages;
  relocation.requesting_agent = "filespace_lifecycle_manager";
  relocation.responding_agent = "page_allocation_manager";
  relocation.page_family = "database_header";
  relocation.requested_pages = 5;
  relocation.relocated_pages = 5;
  relocation.reason = "p2-active-primary-core-page-relocation";

  page::PageFilespaceAgentRequestQueue relocation_queue;
  const auto queued =
      page::EnqueuePageFilespaceAgentRequest(&relocation_queue, relocation, relocation_policy);
  Require(queued.ok(), "core-page relocation request was not queued");
  Require(queued.record.page_agent_action_required,
          "core-page relocation was not owned by the page agent");
  Require(queued.record.request.state == page::PageFilespaceAgentRequestState::waiting_page_agent,
          "core-page relocation did not wait on page agent");

  const auto approved = page::TransitionPageFilespaceAgentRequestWithEvidence(
      &relocation_queue,
      relocation.request_uuid,
      page::PageFilespaceAgentRequestState::approved,
      "replacement root-authority equivalence approved",
      "ok",
      "core_page_relocation_approved",
      MakeUuid(UuidKind::object, CurrentUnixMillis() + 312));
  Require(approved.ok(), "core-page relocation approve transition failed");
  const auto in_flight = page::TransitionPageFilespaceAgentRequestWithEvidence(
      &relocation_queue,
      relocation.request_uuid,
      page::PageFilespaceAgentRequestState::in_flight,
      "replacement core pages relocating",
      "ok",
      "core_page_relocation_in_flight",
      MakeUuid(UuidKind::object, CurrentUnixMillis() + 313));
  Require(in_flight.ok(), "core-page relocation in-flight transition failed");
  const auto completed = page::TransitionPageFilespaceAgentRequestWithEvidence(
      &relocation_queue,
      relocation.request_uuid,
      page::PageFilespaceAgentRequestState::completed,
      "replacement core pages verified equivalent",
      "ok",
      "core_page_relocation_equivalence_verified",
      MakeUuid(UuidKind::object, CurrentUnixMillis() + 314));
  Require(completed.ok(), "core-page relocation completed transition failed");

  const auto restored =
      page::RestorePageFilespaceAgentRequestQueue(page::SerializePageFilespaceAgentRequestQueue(relocation_queue));
  Require(restored.ok(), "completed relocation queue did not restore");
  const auto recovery = page::ClassifyPageFilespaceAgentRequestQueueForRecovery(restored.queue);
  Require(recovery.ok(), "completed relocation recovery classified as unsafe");
  Require(recovery.classifications.size() == 1, "completed relocation recovery count mismatch");
  Require(recovery.classifications.front().action ==
              page::PageFilespaceAgentRequestRecoveryAction::no_action,
          "completed relocation recovery did not resolve as no-op");

  page::PageFilespaceAgentRequestQueue interrupted_queue;
  relocation.request_uuid = MakeUuid(UuidKind::object, CurrentUnixMillis() + 315);
  relocation.reason = "p2-active-primary-core-page-relocation-interrupted";
  const auto interrupted =
      page::EnqueuePageFilespaceAgentRequest(&interrupted_queue, relocation, relocation_policy);
  Require(interrupted.ok(), "interrupted relocation request was not queued");
  Require(page::TransitionPageFilespaceAgentRequest(&interrupted_queue,
                                                    relocation.request_uuid,
                                                    page::PageFilespaceAgentRequestState::approved,
                                                    "interrupted relocation approved")
              .ok(),
          "interrupted relocation approve transition failed");
  Require(page::TransitionPageFilespaceAgentRequest(&interrupted_queue,
                                                    relocation.request_uuid,
                                                    page::PageFilespaceAgentRequestState::in_flight,
                                                    "interrupted relocation in flight")
              .ok(),
          "interrupted relocation in-flight transition failed");
  const auto restored_interrupted =
      page::RestorePageFilespaceAgentRequestQueue(page::SerializePageFilespaceAgentRequestQueue(interrupted_queue));
  Require(restored_interrupted.ok(), "interrupted relocation queue did not restore");
  const auto interrupted_recovery =
      page::ClassifyPageFilespaceAgentRequestQueueForRecovery(restored_interrupted.queue);
  Require(!interrupted_recovery.ok(), "interrupted relocation recovery did not fail closed");
  Require(interrupted_recovery.classifications.front().fail_closed,
          "interrupted relocation recovery did not mark fail-closed");
  Require(interrupted_recovery.classifications.front().diagnostic_code ==
              "page_filespace_agent_recovery_partial_state",
          "interrupted relocation recovery diagnostic mismatch");

  filespace::FilespaceOperationRequest archive_old = create;
  archive_old.operation = filespace::FilespaceOperation::assign_archive_owner;
  archive_old.filespace_uuid = primary_uuid;
  archive_old.path = primary_path.string();
  archive_old.reason = "p2-archive-old-primary-after-core-page-relocation";
  const auto archived = filespace::ApplyFilespaceOperation(&registry, archive_old);
  Require(archived.ok(), "old primary archive after relocation proof failed");
  Require(archived.descriptor.role == filespace::FilespaceRole::archive_history,
          "old primary archive role mismatch after relocation proof");
  Require(!archived.descriptor.startup_authority &&
              !archived.descriptor.catalog_persistence_owner &&
              !archived.descriptor.filespace_manifest_owner &&
              !archived.descriptor.recovery_evidence_owner,
          "archived old primary regained root authority");

  filespace::FilespaceOperationRequest drop_old = archive_old;
  drop_old.operation = filespace::FilespaceOperation::drop_filespace;
  drop_old.reason = "p2-drop-old-primary-after-core-page-relocation";
  const auto dropped = filespace::ApplyFilespaceOperation(&registry, drop_old);
  Require(dropped.ok(), "old primary logical drop after relocation proof failed");
  Require(dropped.descriptor.state == filespace::FilespaceState::drop_pending,
          "old primary drop did not leave drop_pending evidence");
  Require(!dropped.descriptor.active, "old primary drop left descriptor active");
  RequireSingleActivePrimaryAuthority(registry);
}

void TestSnapshotShadowLifecycleCommands(const std::filesystem::path& dir,
                                         const TypedUuid& database_uuid,
                                         const TypedUuid& primary_uuid,
                                         const TypedUuid& snapshot_uuid,
                                         const TypedUuid& shadow_uuid) {
  filespace::FilespaceRegistry registry;
  const auto writer_uuid = MakeUuid(UuidKind::object, CurrentUnixMillis() + 500);

  filespace::FilespaceOperationRequest primary_create;
  primary_create.operation = filespace::FilespaceOperation::create_filespace;
  primary_create.database_uuid = database_uuid;
  primary_create.filespace_uuid = primary_uuid;
  primary_create.path = (dir / "snapshot_shadow_primary.sbfs").string();
  primary_create.role = filespace::FilespaceRole::active_primary;
  primary_create.writer_identity_uuid = writer_uuid;
  primary_create.reason = "p2-snapshot-shadow-create-primary";
  const auto primary = filespace::ApplyFilespaceOperation(&registry, primary_create);
  Require(primary.ok(), "snapshot/shadow primary create failed");

  filespace::FilespaceOperationRequest snapshot;
  snapshot.operation = filespace::FilespaceOperation::create_snapshot_filespace;
  snapshot.database_uuid = database_uuid;
  snapshot.filespace_uuid = snapshot_uuid;
  snapshot.path = (dir / "local_snapshot.sbfs").string();
  snapshot.role = filespace::FilespaceRole::primary_snapshot;
  snapshot.physical_filespace_id = 2;
  snapshot.total_pages = 128;
  snapshot.free_pages = 96;
  snapshot.preallocated_pages = 16;
  snapshot.allocation_root_page = 4;
  snapshot.header_generation = 1;
  snapshot.writer_identity_uuid = writer_uuid;
  snapshot.reason = "p2-create-local-snapshot-filespace";
  const auto snapshot_created = filespace::ApplyFilespaceOperation(&registry, snapshot);
  Require(snapshot_created.ok(), "snapshot filespace create failed");
  Require(snapshot_created.descriptor.role == filespace::FilespaceRole::primary_snapshot,
          "snapshot filespace role mismatch");
  Require(snapshot_created.descriptor.state == filespace::FilespaceState::attached,
          "snapshot filespace state mismatch");
  Require(snapshot_created.descriptor.read_only, "snapshot filespace was not read-only");
  Require(snapshot_created.descriptor.active, "snapshot filespace was not active");
  Require(!snapshot_created.descriptor.startup_authority &&
              !snapshot_created.descriptor.catalog_persistence_owner &&
              !snapshot_created.descriptor.filespace_manifest_owner &&
              !snapshot_created.descriptor.recovery_evidence_owner,
          "snapshot filespace received primary root authority");
  Require(snapshot_created.evidence.operation ==
              filespace::FilespaceOperation::create_snapshot_filespace,
          "snapshot create evidence operation mismatch");
  Require(snapshot_created.durable_state_changed,
          "snapshot create did not report durable state change");

  const auto snapshot_generation = snapshot_created.descriptor.generation;
  filespace::FilespaceOperationRequest snapshot_refresh = snapshot;
  snapshot_refresh.operation = filespace::FilespaceOperation::refresh_snapshot_or_shadow;
  snapshot_refresh.reason = "p2-refresh-local-snapshot-filespace";
  const auto snapshot_refreshed =
      filespace::ApplyFilespaceOperation(&registry, snapshot_refresh);
  Require(snapshot_refreshed.ok(), "snapshot filespace refresh failed");
  Require(snapshot_refreshed.descriptor.role == filespace::FilespaceRole::primary_snapshot,
          "snapshot refresh changed the role");
  Require(snapshot_refreshed.descriptor.generation > snapshot_generation,
          "snapshot refresh did not advance generation");
  Require(snapshot_refreshed.descriptor.read_only && snapshot_refreshed.descriptor.active,
          "snapshot refresh did not preserve read-only active state");
  Require(snapshot_refreshed.durable_state_changed,
          "snapshot refresh did not report durable state change");

  filespace::FilespaceOperationRequest snapshot_validate = snapshot;
  snapshot_validate.operation = filespace::FilespaceOperation::verify_filespace;
  snapshot_validate.reason = "p2-validate-local-snapshot-filespace";
  const auto snapshot_validated =
      filespace::ApplyFilespaceOperation(&registry, snapshot_validate);
  Require(snapshot_validated.ok(), "snapshot filespace validate failed");
  Require(snapshot_validated.evidence.operation ==
              filespace::FilespaceOperation::verify_filespace,
          "snapshot validate evidence operation mismatch");
  Require(!snapshot_validated.durable_state_changed,
          "snapshot validate reported durable state change");

  filespace::FilespaceOperationRequest snapshot_retire = snapshot;
  snapshot_retire.operation = filespace::FilespaceOperation::retire_snapshot_or_shadow;
  snapshot_retire.reason = "p2-retire-local-snapshot-filespace";
  const auto snapshot_retired =
      filespace::ApplyFilespaceOperation(&registry, snapshot_retire);
  Require(snapshot_retired.ok(), "snapshot filespace retire failed");
  Require(snapshot_retired.descriptor.role == filespace::FilespaceRole::primary_snapshot,
          "snapshot retire changed the role");
  Require(snapshot_retired.descriptor.state == filespace::FilespaceState::detached,
          "snapshot retire did not detach");
  Require(!snapshot_retired.descriptor.active && snapshot_retired.descriptor.read_only,
          "snapshot retire did not leave an inactive read-only descriptor");
  Require(snapshot_retired.evidence.operation ==
              filespace::FilespaceOperation::retire_snapshot_or_shadow,
          "snapshot retire evidence operation mismatch");

  filespace::FilespaceOperationRequest shadow = snapshot;
  shadow.operation = filespace::FilespaceOperation::create_shadow_filespace;
  shadow.filespace_uuid = shadow_uuid;
  shadow.path = (dir / "local_shadow.sbfs").string();
  shadow.role = filespace::FilespaceRole::primary_shadow;
  shadow.physical_filespace_id = 3;
  shadow.reason = "p2-create-local-shadow-filespace";
  const auto shadow_created = filespace::ApplyFilespaceOperation(&registry, shadow);
  Require(shadow_created.ok(), "shadow filespace create failed");
  Require(shadow_created.descriptor.role == filespace::FilespaceRole::primary_shadow,
          "shadow filespace role mismatch");
  Require(shadow_created.descriptor.state == filespace::FilespaceState::attached,
          "shadow filespace state mismatch");
  Require(shadow_created.descriptor.read_only && shadow_created.descriptor.active,
          "shadow filespace was not read-only active");
  Require(!shadow_created.descriptor.startup_authority &&
              !shadow_created.descriptor.catalog_persistence_owner &&
              !shadow_created.descriptor.filespace_manifest_owner &&
              !shadow_created.descriptor.recovery_evidence_owner,
          "shadow filespace received primary root authority before promote");

  const auto shadow_generation = shadow_created.descriptor.generation;
  filespace::FilespaceOperationRequest shadow_refresh = shadow;
  shadow_refresh.operation = filespace::FilespaceOperation::refresh_snapshot_or_shadow;
  shadow_refresh.reason = "p2-refresh-local-shadow-filespace";
  const auto shadow_refreshed =
      filespace::ApplyFilespaceOperation(&registry, shadow_refresh);
  Require(shadow_refreshed.ok(), "shadow filespace refresh failed");
  Require(shadow_refreshed.descriptor.role == filespace::FilespaceRole::primary_shadow,
          "shadow refresh changed the role");
  Require(shadow_refreshed.descriptor.generation > shadow_generation,
          "shadow refresh did not advance generation");
  Require(shadow_refreshed.descriptor.read_only && shadow_refreshed.descriptor.active,
          "shadow refresh did not preserve read-only active state");

  filespace::FilespaceOperationRequest shadow_validate = shadow;
  shadow_validate.operation = filespace::FilespaceOperation::verify_filespace;
  shadow_validate.reason = "p2-validate-local-shadow-filespace";
  const auto shadow_validated =
      filespace::ApplyFilespaceOperation(&registry, shadow_validate);
  Require(shadow_validated.ok(), "shadow filespace validate failed");
  Require(shadow_validated.evidence.operation ==
              filespace::FilespaceOperation::verify_filespace,
          "shadow validate evidence operation mismatch");
  Require(!shadow_validated.durable_state_changed,
          "shadow validate reported durable state change");

  filespace::FilespaceOperationRequest shadow_promote = shadow;
  shadow_promote.operation = filespace::FilespaceOperation::promote_filespace;
  shadow_promote.reason = "p2-promote-local-shadow-filespace";
  shadow_promote.policy.allow_primary_replacement = true;
  const auto shadow_promoted =
      filespace::ApplyFilespaceOperation(&registry, shadow_promote);
  Require(shadow_promoted.ok(), "shadow filespace promote failed");
  Require(shadow_promoted.descriptor.role == filespace::FilespaceRole::active_primary,
          "shadow promote did not produce active primary");
  Require(shadow_promoted.descriptor.state == filespace::FilespaceState::attached,
          "shadow promote state mismatch");
  Require(shadow_promoted.descriptor.active && !shadow_promoted.descriptor.read_only,
          "shadow promote did not make the filespace writable active primary");
  Require(shadow_promoted.descriptor.startup_authority &&
              shadow_promoted.descriptor.catalog_persistence_owner &&
              shadow_promoted.descriptor.filespace_manifest_owner &&
              shadow_promoted.descriptor.recovery_evidence_owner,
          "shadow promote did not install root authority flags");
  Require(shadow_promoted.evidence.previous_role == filespace::FilespaceRole::primary_shadow &&
              shadow_promoted.evidence.new_role == filespace::FilespaceRole::active_primary,
          "shadow promote evidence role transition mismatch");
  Require(shadow_promoted.durable_state_changed,
          "shadow promote did not report durable state change");

  const auto* old_primary = DescriptorByUuid(registry, primary_uuid);
  Require(old_primary != nullptr, "snapshot/shadow old primary missing");
  Require(old_primary->role == filespace::FilespaceRole::primary_shadow,
          "snapshot/shadow old primary was not demoted to shadow");
  Require(old_primary->read_only,
          "snapshot/shadow old primary was not fenced read-only");
  Require(!old_primary->startup_authority &&
              !old_primary->catalog_persistence_owner &&
              !old_primary->filespace_manifest_owner &&
              !old_primary->recovery_evidence_owner,
          "snapshot/shadow old primary retained root authority");

  const auto* retired_snapshot = DescriptorByUuid(registry, snapshot_uuid);
  Require(retired_snapshot != nullptr, "retired snapshot descriptor missing");
  Require(retired_snapshot->role == filespace::FilespaceRole::primary_snapshot &&
              retired_snapshot->state == filespace::FilespaceState::detached &&
              !retired_snapshot->active && retired_snapshot->read_only,
          "retired snapshot descriptor state drifted");

  RequireSingleActivePrimaryAuthority(registry);
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

filespace::FilespaceDescriptor ExpectedFilespace(const TypedUuid& database_uuid,
                                                 const TypedUuid& filespace_uuid,
                                                 const TypedUuid& writer_uuid,
                                                 std::string path,
                                                 std::uint16_t physical_id,
                                                 std::uint64_t generation) {
  filespace::FilespaceDescriptor descriptor;
  descriptor.database_uuid = database_uuid;
  descriptor.filespace_uuid = filespace_uuid;
  descriptor.writer_identity_uuid = writer_uuid;
  descriptor.path = std::move(path);
  descriptor.role = filespace::FilespaceRole::secondary_data;
  descriptor.state = filespace::FilespaceState::online;
  descriptor.physical_filespace_id = physical_id;
  descriptor.header_generation = generation;
  return descriptor;
}

filespace::FilespaceDiscoveryCandidate ObservedFilespace(
    const filespace::FilespaceDescriptor& descriptor) {
  filespace::FilespaceDiscoveryCandidate candidate;
  candidate.database_uuid = descriptor.database_uuid;
  candidate.filespace_uuid = descriptor.filespace_uuid;
  candidate.writer_identity_uuid = descriptor.writer_identity_uuid;
  candidate.path = descriptor.path;
  candidate.role = descriptor.role;
  candidate.state = descriptor.state;
  candidate.physical_filespace_id = descriptor.physical_filespace_id;
  candidate.header_generation = descriptor.header_generation;
  return candidate;
}

const filespace::FilespaceDiscoveryRow& RequireDiscoveryRow(
    const filespace::FilespaceDiscoveryResult& result,
    filespace::FilespaceDiscoveryClassification classification) {
  for (const auto& row : result.rows) {
    if (row.classification == classification) {
      return row;
    }
  }
  std::cerr << "missing filespace discovery classification "
            << filespace::FilespaceDiscoveryClassificationName(classification) << '\n';
  std::exit(EXIT_FAILURE);
}

void TestFilespaceOrphanStaleDiscovery(const std::filesystem::path& dir,
                                       const TypedUuid& database_uuid,
                                       const TypedUuid& foreign_database_uuid,
                                       const std::vector<TypedUuid>& filespace_uuids,
                                       const TypedUuid& writer_uuid,
                                       const TypedUuid& replacement_writer_uuid) {
  Require(filespace_uuids.size() >= 10, "filespace discovery fixture needs 10 uuids");

  filespace::FilespaceDiscoveryRequest request;
  request.database_uuid = database_uuid;

  request.expected.push_back(ExpectedFilespace(database_uuid,
                                               filespace_uuids[0],
                                               writer_uuid,
                                               (dir / "ok.fsp").string(),
                                               10,
                                               5));
  request.expected.push_back(ExpectedFilespace(database_uuid,
                                               filespace_uuids[1],
                                               writer_uuid,
                                               (dir / "missing.fsp").string(),
                                               11,
                                               5));
  request.expected.push_back(ExpectedFilespace(database_uuid,
                                               filespace_uuids[2],
                                               writer_uuid,
                                               (dir / "wrong_database.fsp").string(),
                                               12,
                                               5));
  request.expected.push_back(ExpectedFilespace(database_uuid,
                                               filespace_uuids[3],
                                               writer_uuid,
                                               (dir / "wrong_filespace.fsp").string(),
                                               13,
                                               5));
  request.expected.push_back(ExpectedFilespace(database_uuid,
                                               filespace_uuids[4],
                                               writer_uuid,
                                               (dir / "duplicate.fsp").string(),
                                               14,
                                               5));
  request.expected.push_back(ExpectedFilespace(database_uuid,
                                               filespace_uuids[5],
                                               writer_uuid,
                                               (dir / "stale.fsp").string(),
                                               15,
                                               8));
  request.expected.push_back(ExpectedFilespace(database_uuid,
                                               filespace_uuids[6],
                                               writer_uuid,
                                               (dir / "replaced.fsp").string(),
                                               16,
                                               5));
  request.expected.push_back(ExpectedFilespace(database_uuid,
                                               filespace_uuids[7],
                                               writer_uuid,
                                               (dir / "quarantine.fsp").string(),
                                               17,
                                               5));

  request.observed.push_back(ObservedFilespace(request.expected[0]));

  auto wrong_database_observed = ObservedFilespace(request.expected[2]);
  wrong_database_observed.database_uuid = foreign_database_uuid;
  request.observed.push_back(wrong_database_observed);

  auto wrong_filespace_observed = ObservedFilespace(request.expected[3]);
  wrong_filespace_observed.filespace_uuid = filespace_uuids[8];
  wrong_filespace_observed.path = request.expected[3].path;
  request.observed.push_back(wrong_filespace_observed);

  request.observed.push_back(ObservedFilespace(request.expected[4]));
  auto duplicate_observed = ObservedFilespace(request.expected[4]);
  duplicate_observed.path = (dir / "duplicate-copy.fsp").string();
  request.observed.push_back(duplicate_observed);

  auto stale_observed = ObservedFilespace(request.expected[5]);
  stale_observed.header_generation = 2;
  request.observed.push_back(stale_observed);

  auto replaced_observed = ObservedFilespace(request.expected[6]);
  replaced_observed.header_generation = 12;
  replaced_observed.writer_identity_uuid = replacement_writer_uuid;
  request.observed.push_back(replaced_observed);

  auto quarantined_observed = ObservedFilespace(request.expected[7]);
  quarantined_observed.state = filespace::FilespaceState::quarantine;
  quarantined_observed.role = filespace::FilespaceRole::import_candidate;
  request.observed.push_back(quarantined_observed);

  auto orphan_observed = ObservedFilespace(ExpectedFilespace(database_uuid,
                                                            filespace_uuids[9],
                                                            writer_uuid,
                                                            (dir / "orphan.fsp").string(),
                                                            18,
                                                            1));
  request.observed.push_back(orphan_observed);

  const auto result = filespace::DiscoverFilespaceAnomalies(request);
  Require(result.ok(), "filespace discovery classifier failed");
  Require(result.rows.size() == 9, "filespace discovery row count mismatch");
  Require(result.anomaly_count == 8, "filespace discovery anomaly count mismatch");
  Require(result.quarantine_required, "filespace discovery did not require quarantine");
  Require(result.operator_review_required, "filespace discovery did not require operator review");
  Require(!result.durable_state_changed, "filespace discovery mutated durable state");

  const auto& ok = RequireDiscoveryRow(result, filespace::FilespaceDiscoveryClassification::ok);
  Require(ok.normal_access_allowed, "clean filespace was blocked from normal access");

  const auto& missing =
      RequireDiscoveryRow(result, filespace::FilespaceDiscoveryClassification::missing);
  Require(!missing.normal_access_allowed, "missing filespace allowed normal access");
  Require(missing.recommended_action == "restore_or_reattach_after_verify",
          "missing filespace recovery action drifted");

  const auto& wrong_database =
      RequireDiscoveryRow(result, filespace::FilespaceDiscoveryClassification::wrong_database);
  Require(wrong_database.quarantine_required, "wrong database filespace was not quarantined");
  Require(wrong_database.release_requires_authority,
          "wrong database filespace release did not require authority");

  const auto& wrong_filespace =
      RequireDiscoveryRow(result, filespace::FilespaceDiscoveryClassification::wrong_filespace);
  Require(wrong_filespace.quarantine_required, "wrong filespace identity was not quarantined");

  const auto& duplicate_identity =
      RequireDiscoveryRow(result, filespace::FilespaceDiscoveryClassification::duplicate_identity);
  Require(duplicate_identity.cache_invalidation_required,
          "duplicate filespace identity did not require cache invalidation");

  const auto& stale =
      RequireDiscoveryRow(result, filespace::FilespaceDiscoveryClassification::stale_header);
  Require(stale.cache_invalidation_required, "stale header did not require cache invalidation");
  Require(!stale.quarantine_required, "stale header was incorrectly routed to quarantine");

  const auto& replaced =
      RequireDiscoveryRow(result, filespace::FilespaceDiscoveryClassification::replaced_header);
  Require(replaced.operator_review_required, "replaced header did not require operator review");

  const auto& orphan =
      RequireDiscoveryRow(result, filespace::FilespaceDiscoveryClassification::foreign_orphan);
  Require(orphan.quarantine_required, "foreign orphan was not routed to quarantine");
  Require(orphan.recommended_action == "import_into_foreign_filespace_quarantine",
          "foreign orphan action drifted");

  const auto& quarantine =
      RequireDiscoveryRow(result,
                          filespace::FilespaceDiscoveryClassification::quarantined_candidate);
  Require(quarantine.quarantine_required, "quarantined candidate was not fenced");
  Require(quarantine.release_requires_authority,
          "quarantined candidate release did not require authority");
}

void TestFilespaceRuntimeFilesystemDiscoveryScan(const std::filesystem::path& dir,
                                                const TypedUuid& database_uuid,
                                                const std::vector<TypedUuid>& filespace_uuids,
                                                const TypedUuid& writer_uuid) {
  Require(filespace_uuids.size() >= 4, "runtime discovery fixture needs 4 uuids");

  const auto ok = ExpectedFilespace(database_uuid,
                                    filespace_uuids[0],
                                    writer_uuid,
                                    (dir / "scan-ok.fsp").string(),
                                    51,
                                    1);
  const auto missing = ExpectedFilespace(database_uuid,
                                         filespace_uuids[1],
                                         writer_uuid,
                                         (dir / "scan-missing.fsp").string(),
                                         52,
                                         1);
  const auto stale = ExpectedFilespace(database_uuid,
                                       filespace_uuids[2],
                                       writer_uuid,
                                       (dir / "scan-stale.fsp").string(),
                                       53,
                                       5);
  const auto orphan_path = dir / "scan-orphan.fsp";

  auto ok_header = ReplacementPhysicalHeader(database_uuid,
                                             filespace_uuids[0],
                                             filespace::FilespaceRole::secondary_data,
                                             filespace::FilespaceState::online,
                                             51,
                                             writer_uuid,
                                             "p2-scan-ok-header");
  ok_header.header_generation = 1;
  WriteReplacementPhysicalHeader(ok.path, ok_header, "runtime scan clean");

  auto stale_header = ReplacementPhysicalHeader(database_uuid,
                                                filespace_uuids[2],
                                                filespace::FilespaceRole::secondary_data,
                                                filespace::FilespaceState::online,
                                                53,
                                                writer_uuid,
                                                "p2-scan-stale-header");
  stale_header.header_generation = 2;
  WriteReplacementPhysicalHeader(stale.path, stale_header, "runtime scan stale");

  auto orphan_header = ReplacementPhysicalHeader(database_uuid,
                                                 filespace_uuids[3],
                                                 filespace::FilespaceRole::secondary_data,
                                                 filespace::FilespaceState::online,
                                                 54,
                                                 writer_uuid,
                                                 "p2-scan-orphan-header");
  orphan_header.header_generation = 1;
  WriteReplacementPhysicalHeader(orphan_path, orphan_header, "runtime scan orphan");

  filespace::FilespaceDiscoveryFilesystemScanRequest scan;
  scan.database_uuid = database_uuid;
  scan.expected = {ok, missing, stale};
  scan.observed_paths = {
      ok.path,
      missing.path,
      stale.path,
      orphan_path.string(),
      ok.path,
  };

  const auto scanned = filespace::DiscoverFilespaceAnomaliesFromFilesystem(scan);
  Require(scanned.ok(), "runtime filespace discovery scan failed");
  Require(scanned.runtime_filesystem_scan_executed,
          "runtime filespace discovery scan flag missing");
  Require(scanned.scanned_path_count == 4,
          "runtime filespace discovery did not de-duplicate scan paths");
  Require(scanned.observed_header_count == 3,
          "runtime filespace discovery observed header count mismatch");
  Require(scanned.observed.size() == 3,
          "runtime filespace discovery did not expose observed header candidates");
  Require(scanned.missing_path_count == 1,
          "runtime filespace discovery missing path count mismatch");
  Require(scanned.unreadable_header_count == 0,
          "runtime filespace discovery reported unexpected unreadable header");
  Require(!scanned.durable_state_changed,
          "runtime filespace discovery scan mutated durable state");
  Require(!scanned.cleanup_or_quarantine_executed,
          "runtime filespace discovery scan executed cleanup or quarantine");

  const auto& discovery = scanned.discovery;
  Require(discovery.ok(), "runtime filespace discovery classifier failed");
  Require(discovery.rows.size() == 4, "runtime filespace discovery row count mismatch");
  Require(discovery.anomaly_count == 3,
          "runtime filespace discovery anomaly count mismatch");
  Require(!discovery.durable_state_changed,
          "runtime filespace discovery classifier mutated durable state");

  const auto& clean = RequireDiscoveryRow(discovery, filespace::FilespaceDiscoveryClassification::ok);
  Require(clean.normal_access_allowed, "runtime scan clean filespace was blocked");
  const auto& missing_row =
      RequireDiscoveryRow(discovery, filespace::FilespaceDiscoveryClassification::missing);
  Require(!missing_row.normal_access_allowed, "runtime scan missing filespace allowed access");
  const auto& stale_row =
      RequireDiscoveryRow(discovery, filespace::FilespaceDiscoveryClassification::stale_header);
  Require(stale_row.cache_invalidation_required,
          "runtime scan stale header did not require cache invalidation");
  const auto& orphan =
      RequireDiscoveryRow(discovery, filespace::FilespaceDiscoveryClassification::foreign_orphan);
  Require(orphan.quarantine_required, "runtime scan orphan did not require quarantine");
  Require(orphan.release_requires_authority,
          "runtime scan orphan release did not require authority");
}

void TestFilespaceDiscoveryExecution(const std::filesystem::path& dir,
                                     const TypedUuid& database_uuid,
                                     const TypedUuid& foreign_database_uuid,
                                     const std::vector<TypedUuid>& filespace_uuids,
                                     const TypedUuid& writer_uuid) {
  Require(filespace_uuids.size() >= 4, "discovery execution fixture needs 4 uuids");

  filespace::FilespaceRegistry registry;
  const auto wrong_database = ExpectedFilespace(database_uuid,
                                                filespace_uuids[0],
                                                writer_uuid,
                                                (dir / "exec-wrong-database.fsp").string(),
                                                61,
                                                1);
  auto quarantined_import = ExpectedFilespace(database_uuid,
                                              filespace_uuids[1],
                                              writer_uuid,
                                              (dir / "exec-release-quarantine.fsp").string(),
                                              62,
                                              1);
  quarantined_import.role = filespace::FilespaceRole::import_candidate;
  quarantined_import.state = filespace::FilespaceState::quarantine;
  quarantined_import.read_only = true;
  quarantined_import.active = false;
  registry.filespaces.push_back(wrong_database);
  registry.filespaces.push_back(quarantined_import);

  const auto orphan_path = dir / "exec-foreign-orphan.fsp";
  const auto orphan_descriptor = ExpectedFilespace(database_uuid,
                                                   filespace_uuids[2],
                                                   writer_uuid,
                                                   orphan_path.string(),
                                                   63,
                                                   1);
  auto orphan_header = ReplacementPhysicalHeader(database_uuid,
                                                 filespace_uuids[2],
                                                 filespace::FilespaceRole::secondary_data,
                                                 filespace::FilespaceState::online,
                                                 63,
                                                 writer_uuid,
                                                 "p2-discovery-execution-orphan");
  WriteReplacementPhysicalHeader(orphan_path, orphan_header, "discovery execution orphan");

  auto release_header = ReplacementPhysicalHeader(database_uuid,
                                                  filespace_uuids[1],
                                                  filespace::FilespaceRole::import_candidate,
                                                  filespace::FilespaceState::quarantine,
                                                  62,
                                                  writer_uuid,
                                                  "p2-discovery-execution-release");
  WriteReplacementPhysicalHeader(quarantined_import.path,
                                 release_header,
                                 "discovery execution release");

  filespace::FilespaceDiscoveryRequest discovery;
  discovery.database_uuid = database_uuid;
  discovery.expected.push_back(wrong_database);
  discovery.expected.push_back(quarantined_import);

  auto wrong_observed = ObservedFilespace(wrong_database);
  wrong_observed.database_uuid = foreign_database_uuid;
  discovery.observed.push_back(wrong_observed);
  discovery.observed.push_back(ObservedFilespace(quarantined_import));
  discovery.observed.push_back(ObservedFilespace(orphan_descriptor));

  filespace::FilespaceDiscoveryExecutionRequest execution;
  execution.discovery = discovery;
  execution.execute_quarantine_actions = true;
  execution.execute_release_actions = true;
  execution.physical_header_required_for_quarantine = true;
  execution.header_inspection_passed = true;
  execution.release_authorized = true;
  execution.operation_uuid = "p2-discovery-execution";
  execution.inspector_uuid = "019e2000-0000-7000-8000-000000000301";
  execution.release_authority_uuid = "019e2000-0000-7000-8000-000000000302";

  const auto executed =
      filespace::ExecuteFilespaceDiscoveryActions(&registry, execution);
  Require(executed.ok(), "filespace discovery execution failed");
  Require(executed.discovery.rows.size() == 3,
          "filespace discovery execution row count mismatch");
  Require(executed.discovery.anomaly_count == 3,
          "filespace discovery execution anomaly count mismatch");
  Require(executed.quarantine_execution_count == 2,
          "filespace discovery quarantine execution count mismatch");
  Require(executed.release_execution_count == 1,
          "filespace discovery release execution count mismatch");
  Require(executed.cleanup_or_quarantine_executed,
          "filespace discovery execution did not report quarantine execution");
  Require(executed.release_executed,
          "filespace discovery execution did not report release execution");
  Require(executed.durable_state_changed && executed.discovery.durable_state_changed,
          "filespace discovery execution did not report durable state change");
  Require(executed.cache_invalidation_required &&
              executed.discovery.cache_invalidation_required,
          "filespace discovery execution did not require cache invalidation");

  const auto* wrong_after = DescriptorByUuid(registry, filespace_uuids[0]);
  Require(wrong_after != nullptr &&
              wrong_after->state == filespace::FilespaceState::quarantine &&
              wrong_after->read_only && !wrong_after->active,
          "wrong-database filespace was not quarantined");

  const auto* released_after = DescriptorByUuid(registry, filespace_uuids[1]);
  Require(released_after != nullptr &&
              released_after->state == filespace::FilespaceState::detached &&
              released_after->read_only && !released_after->active,
          "quarantined import candidate was not released to detached state");

  const auto* orphan_after = DescriptorByUuid(registry, filespace_uuids[2]);
  Require(orphan_after != nullptr &&
              orphan_after->role == filespace::FilespaceRole::import_candidate &&
              orphan_after->state == filespace::FilespaceState::quarantine &&
              orphan_after->read_only && !orphan_after->active,
          "foreign orphan was not imported into quarantine");
  Require(std::filesystem::exists(orphan_path) &&
              std::filesystem::exists(quarantined_import.path),
          "discovery execution removed a physical filespace file");
}

void TestFilespaceDiscoveryPhysicalCleanupExecution(
    const std::filesystem::path& dir,
    const TypedUuid& database_uuid,
    const std::vector<TypedUuid>& filespace_uuids,
    const TypedUuid& writer_uuid) {
  Require(filespace_uuids.size() >= 6, "discovery cleanup fixture needs 6 uuids");

  filespace::FilespaceRegistry registry;
  auto cleanup_candidate = ExpectedFilespace(database_uuid,
                                             filespace_uuids[4],
                                             writer_uuid,
                                             (dir / "exec-cleanup-quarantine.fsp").string(),
                                             64,
                                             1);
  cleanup_candidate.role = filespace::FilespaceRole::import_candidate;
  cleanup_candidate.state = filespace::FilespaceState::quarantine;
  cleanup_candidate.read_only = true;
  cleanup_candidate.active = false;
  registry.filespaces.push_back(cleanup_candidate);

  const auto cleanup_header = ReplacementPhysicalHeader(database_uuid,
                                                        filespace_uuids[4],
                                                        filespace::FilespaceRole::import_candidate,
                                                        filespace::FilespaceState::quarantine,
                                                        64,
                                                        writer_uuid,
                                                        "p2-discovery-cleanup-candidate");
  WriteReplacementPhysicalHeader(cleanup_candidate.path,
                                 cleanup_header,
                                 "discovery cleanup candidate");

  filespace::FilespaceDiscoveryRequest policy_refusal_discovery;
  policy_refusal_discovery.database_uuid = database_uuid;
  policy_refusal_discovery.expected.push_back(cleanup_candidate);
  policy_refusal_discovery.observed.push_back(ObservedFilespace(cleanup_candidate));

  filespace::FilespaceDiscoveryExecutionRequest missing_policy;
  missing_policy.discovery = policy_refusal_discovery;
  missing_policy.execute_physical_cleanup_actions = true;
  missing_policy.operation_uuid = "p2-discovery-cleanup-policy-missing";
  const auto refused =
      filespace::ExecuteFilespaceDiscoveryActions(&registry, missing_policy);
  Require(!refused.ok(), "discovery physical cleanup admitted without policy");
  Require(refused.diagnostic.diagnostic_code ==
              "SB-FILESPACE-LIFECYCLE-PHYSICAL-DELETE-FORBIDDEN",
          "discovery physical cleanup policy diagnostic mismatch");
  Require(std::filesystem::exists(cleanup_candidate.path),
          "policy-refused discovery cleanup removed the file");

  filespace::FilespaceDiscoveryExecutionRequest cleanup = missing_policy;
  cleanup.operation_uuid = "p2-discovery-cleanup-success";
  cleanup.allow_physical_filespace_delete = true;
  cleanup.physical_delete_legal_hold_clear = true;
  cleanup.physical_delete_retention_satisfied = true;
  cleanup.physical_delete_cleanup_horizon_authoritative = true;
  const auto cleaned =
      filespace::ExecuteFilespaceDiscoveryActions(&registry, cleanup);
  Require(cleaned.ok(), "discovery physical cleanup failed");
  Require(cleaned.physical_cleanup_execution_count == 1,
          "discovery physical cleanup execution count mismatch");
  Require(cleaned.physical_cleanup_executed && cleaned.physical_file_removed,
          "discovery physical cleanup did not report file removal");
  Require(cleaned.cleanup_or_quarantine_executed,
          "discovery physical cleanup did not set cleanup execution evidence");
  Require(cleaned.durable_state_changed && cleaned.discovery.durable_state_changed,
          "discovery physical cleanup did not report durable state change");
  Require(cleaned.cache_invalidation_required &&
              cleaned.discovery.cache_invalidation_required,
          "discovery physical cleanup did not require cache invalidation");
  Require(!std::filesystem::exists(cleanup_candidate.path),
          "discovery physical cleanup left the physical file");
  const auto* cleanup_after = DescriptorByUuid(registry, filespace_uuids[4]);
  Require(cleanup_after != nullptr &&
              cleanup_after->state == filespace::FilespaceState::dropped &&
              cleanup_after->read_only && !cleanup_after->active,
          "discovery physical cleanup did not mark descriptor dropped");

  filespace::FilespaceRegistry orphan_registry;
  const auto orphan_path = dir / "exec-cleanup-foreign-orphan.fsp";
  const auto orphan_descriptor = ExpectedFilespace(database_uuid,
                                                   filespace_uuids[5],
                                                   writer_uuid,
                                                   orphan_path.string(),
                                                   65,
                                                   1);
  const auto orphan_header = ReplacementPhysicalHeader(database_uuid,
                                                       filespace_uuids[5],
                                                       filespace::FilespaceRole::secondary_data,
                                                       filespace::FilespaceState::online,
                                                       65,
                                                       writer_uuid,
                                                       "p2-discovery-cleanup-orphan");
  WriteReplacementPhysicalHeader(orphan_path, orphan_header, "discovery cleanup orphan");

  filespace::FilespaceDiscoveryRequest orphan_discovery;
  orphan_discovery.database_uuid = database_uuid;
  orphan_discovery.observed.push_back(ObservedFilespace(orphan_descriptor));

  filespace::FilespaceDiscoveryExecutionRequest orphan_cleanup;
  orphan_cleanup.discovery = orphan_discovery;
  orphan_cleanup.execute_quarantine_actions = true;
  orphan_cleanup.execute_physical_cleanup_actions = true;
  orphan_cleanup.physical_header_required_for_quarantine = true;
  orphan_cleanup.header_inspection_passed = true;
  orphan_cleanup.operation_uuid = "p2-discovery-orphan-cleanup";
  orphan_cleanup.inspector_uuid = "019e2000-0000-7000-8000-000000000303";
  orphan_cleanup.allow_physical_filespace_delete = true;
  orphan_cleanup.physical_delete_legal_hold_clear = true;
  orphan_cleanup.physical_delete_retention_satisfied = true;
  orphan_cleanup.physical_delete_cleanup_horizon_authoritative = true;

  const auto orphan_cleaned =
      filespace::ExecuteFilespaceDiscoveryActions(&orphan_registry, orphan_cleanup);
  Require(orphan_cleaned.ok(), "discovery orphan physical cleanup failed");
  Require(orphan_cleaned.quarantine_execution_count == 1,
          "discovery orphan cleanup did not import quarantine first");
  Require(orphan_cleaned.physical_cleanup_execution_count == 1,
          "discovery orphan cleanup count mismatch");
  Require(orphan_cleaned.physical_cleanup_executed &&
              orphan_cleaned.physical_file_removed,
          "discovery orphan cleanup did not remove the physical file");
  Require(!std::filesystem::exists(orphan_path),
          "discovery orphan cleanup left the physical file");
  const auto* orphan_after = DescriptorByUuid(orphan_registry, filespace_uuids[5]);
  Require(orphan_after != nullptr &&
              orphan_after->role == filespace::FilespaceRole::import_candidate &&
              orphan_after->state == filespace::FilespaceState::dropped,
          "discovery orphan cleanup did not retire the quarantined descriptor");
}

void TestFilespacePackageWorkflow(const std::filesystem::path& dir,
                                  const TypedUuid& source_database_uuid,
                                  const TypedUuid& target_database_uuid,
                                  const TypedUuid& package_uuid,
                                  const std::vector<TypedUuid>& filespace_uuids,
                                  const TypedUuid& writer_uuid) {
  Require(filespace_uuids.size() >= 2, "filespace package fixture needs 2 uuids");

  const auto first = ExpectedFilespace(source_database_uuid,
                                       filespace_uuids[0],
                                       writer_uuid,
                                       (dir / "package-first.fsp").string(),
                                       30,
                                       3);
  const auto second = ExpectedFilespace(source_database_uuid,
                                        filespace_uuids[1],
                                        writer_uuid,
                                        (dir / "package-second.fsp").string(),
                                        31,
                                        4);

  filespace::FilespacePackageRequest export_request;
  export_request.package_uuid = package_uuid;
  export_request.database_uuid = source_database_uuid;
  export_request.package_name = "p2_filespace_package";
  export_request.descriptors = {first, second};

  const auto exported = filespace::ExportFilespacePackageManifest(export_request);
  Require(exported.ok(), "filespace package export failed");
  Require(exported.manifest.members.size() == 2, "filespace package member count mismatch");
  Require(!exported.manifest.root_authority_present,
          "filespace package exported root authority");
  Require(!exported.manifest.manifest_checksum.empty(),
          "filespace package manifest checksum missing");

  filespace::FilespacePackageRequest inspect_request;
  inspect_request.manifest = exported.manifest;
  const auto inspected = filespace::InspectFilespacePackageManifest(inspect_request);
  Require(inspected.ok(), "filespace package inspect failed");
  Require(inspected.diagnostic.diagnostic_code == "SB-FILESPACE-PACKAGE-INSPECTED",
          "filespace package inspect diagnostic drifted");

  const auto package_file_path = dir / "p2_filespace_package.sbpkg";
  filespace::FilespacePackageFileWriteRequest write_request;
  write_request.path = package_file_path;
  write_request.manifest = exported.manifest;
  const auto written = filespace::WriteFilespacePackageFile(write_request);
  Require(written.ok(), "filespace package manifest file write failed");
  Require(written.runtime_package_file_io_executed,
          "filespace package manifest write did not execute runtime file IO");
  Require(!written.physical_package_transfer_executed,
          "filespace package manifest write transferred physical package data");
  Require(!written.encrypted_material_included,
          "filespace package manifest write included encrypted material");
  Require(!written.durable_state_changed,
          "filespace package manifest write reported durable database state change");
  Require(written.checksum_verified, "filespace package manifest write did not verify checksum");
  Require(written.file_flushed, "filespace package manifest write did not flush file stream");
  Require(!written.filesystem_sync_executed,
          "filespace package manifest write overclaimed filesystem sync");
  Require(written.byte_count > 0, "filespace package manifest write byte count missing");
  Require(std::filesystem::exists(package_file_path),
          "filespace package manifest file was not created");

  const auto duplicate_write = filespace::WriteFilespacePackageFile(write_request);
  Require(!duplicate_write.ok(), "filespace package manifest overwrite was accepted");
  Require(duplicate_write.diagnostic.diagnostic_code ==
              "SB-FILESPACE-PACKAGE-FILE-EXISTS",
          "filespace package manifest overwrite diagnostic mismatch");

  filespace::FilespacePackageFileReadRequest read_request;
  read_request.path = package_file_path;
  const auto read = filespace::ReadFilespacePackageFile(read_request);
  Require(read.ok(), "filespace package manifest file read failed");
  Require(read.runtime_package_file_io_executed,
          "filespace package manifest read did not execute runtime file IO");
  Require(!read.physical_package_transfer_executed,
          "filespace package manifest read transferred physical package data");
  Require(!read.encrypted_material_included,
          "filespace package manifest read included encrypted material");
  Require(!read.durable_state_changed,
          "filespace package manifest read reported durable database state change");
  Require(read.checksum_verified, "filespace package manifest read did not verify checksum");
  Require(read.byte_count == written.byte_count,
          "filespace package manifest read byte count mismatch");
  Require(read.manifest.manifest_checksum == exported.manifest.manifest_checksum,
          "filespace package manifest file checksum mismatch");
  Require(read.manifest.members.size() == 2,
          "filespace package manifest file member count mismatch");
  Require(read.manifest.members.front().path == first.path,
          "filespace package manifest file member path mismatch");

  {
    std::ofstream first_physical(first.path, std::ios::binary | std::ios::trunc);
    first_physical << "scratchbird physical package member one";
  }
  {
    std::ofstream second_physical(second.path, std::ios::binary | std::ios::trunc);
    second_physical << "scratchbird physical package member two";
  }

  filespace::FilespacePackageFileWriteRequest forbidden_physical = write_request;
  forbidden_physical.path = dir / "p2_filespace_package_physical_forbidden.sbpkg";
  forbidden_physical.execute_physical_package_transfer = true;
  const auto physical_forbidden = filespace::WriteFilespacePackageFile(forbidden_physical);
  Require(!physical_forbidden.ok(),
          "filespace package physical transfer was admitted without authority");
  Require(physical_forbidden.diagnostic.diagnostic_code ==
              "SB-FILESPACE-PACKAGE-PHYSICAL-TRANSFER-FORBIDDEN",
          "filespace package physical transfer forbidden diagnostic mismatch");

  const auto physical_package_path = dir / "p2_filespace_package_physical.sbpkg";
  filespace::FilespacePackageFileWriteRequest physical_write = write_request;
  physical_write.path = physical_package_path;
  physical_write.execute_physical_package_transfer = true;
  physical_write.allow_physical_package_transfer = true;
  const auto physical_written = filespace::WriteFilespacePackageFile(physical_write);
  Require(physical_written.ok(), "filespace package physical transfer write failed");
  Require(physical_written.physical_package_transfer_executed,
          "filespace package physical transfer write did not execute transfer");
  Require(physical_written.physical_member_count == 2,
          "filespace package physical transfer member count mismatch");
  Require(physical_written.physical_byte_count > 0,
          "filespace package physical transfer byte count missing");
  const auto physical_member_dir =
      physical_package_path.parent_path() /
      (physical_package_path.filename().string() + ".members");
  Require(std::filesystem::exists(
              physical_member_dir /
              (uuid::UuidToString(filespace_uuids[0].value) + ".fsp")) &&
              std::filesystem::exists(
                  physical_member_dir /
                  (uuid::UuidToString(filespace_uuids[1].value) + ".fsp")),
          "filespace package physical transfer did not copy member files");

  const auto restore_dir = dir / "p2_filespace_package_restore";
  filespace::FilespacePackageFileReadRequest physical_read;
  physical_read.path = physical_package_path;
  physical_read.physical_output_directory = restore_dir;
  physical_read.execute_physical_package_transfer = true;
  physical_read.allow_physical_package_transfer = true;
  const auto physical_read_result = filespace::ReadFilespacePackageFile(physical_read);
  Require(physical_read_result.ok(), "filespace package physical transfer read failed");
  Require(physical_read_result.physical_package_transfer_executed,
          "filespace package physical transfer read did not execute transfer");
  Require(physical_read_result.physical_member_count == 2,
          "filespace package physical read member count mismatch");
  Require(physical_read_result.physical_byte_count ==
              physical_written.physical_byte_count,
          "filespace package physical read byte count mismatch");
  Require(std::filesystem::exists(
              restore_dir / (uuid::UuidToString(filespace_uuids[0].value) + ".fsp")) &&
              std::filesystem::exists(
                  restore_dir / (uuid::UuidToString(filespace_uuids[1].value) + ".fsp")),
          "filespace package physical transfer did not restore member files");

  {
    std::ofstream tamper_file(package_file_path, std::ios::binary | std::ios::app);
    tamper_file << "tamper\n";
  }
  const auto tampered_file = filespace::ReadFilespacePackageFile(read_request);
  Require(!tampered_file.ok(), "tampered filespace package file was accepted");
  Require(tampered_file.runtime_package_file_io_executed,
          "tampered filespace package file did not record runtime file IO");
  Require(tampered_file.diagnostic.diagnostic_code ==
              "SB-FILESPACE-PACKAGE-FILE-TRAILING-CONTENT",
          "tampered filespace package file diagnostic mismatch");

  auto tampered_manifest = exported.manifest;
  tampered_manifest.members.front().header_generation += 1;
  filespace::FilespacePackageRequest tamper_request;
  tamper_request.manifest = tampered_manifest;
  const auto tampered = filespace::InspectFilespacePackageManifest(tamper_request);
  Require(!tampered.ok(), "tampered filespace package manifest was accepted");
  Require(tampered.diagnostic.diagnostic_code ==
              "SB-FILESPACE-PACKAGE-MEMBER-CHECKSUM-MISMATCH",
          "tampered filespace package diagnostic mismatch");

  auto root_descriptor = first;
  root_descriptor.startup_authority = true;
  filespace::FilespacePackageRequest root_export_request = export_request;
  root_export_request.descriptors = {root_descriptor};
  const auto root_export = filespace::ExportFilespacePackageManifest(root_export_request);
  Require(!root_export.ok(), "root-authority filespace package export was accepted");
  Require(root_export.diagnostic.diagnostic_code ==
              "SB-FILESPACE-PACKAGE-ROOT-AUTHORITY-REFUSED",
          "root-authority filespace package refusal diagnostic mismatch");

  filespace::FilespaceRegistry import_registry;
  filespace::FilespacePackageRequest import_request;
  import_request.manifest = exported.manifest;
  import_request.target_database_uuid = target_database_uuid;
  import_request.inspection_passed = true;
  const auto imported =
      filespace::ImportFilespacePackageToQuarantine(&import_registry, import_request);
  Require(imported.ok(), "filespace package import to quarantine failed");
  Require(imported.staged_count == 2, "filespace package staged count mismatch");
  Require(imported.durable_state_changed, "filespace package import did not mutate state");
  Require(import_registry.filespaces.size() == 2, "filespace package registry stage count mismatch");
  for (const auto& descriptor : import_registry.filespaces) {
    Require(descriptor.database_uuid.value == target_database_uuid.value,
            "filespace package staged descriptor database mismatch");
    Require(descriptor.role == filespace::FilespaceRole::import_candidate,
            "filespace package staged descriptor role mismatch");
    Require(descriptor.state == filespace::FilespaceState::quarantine,
            "filespace package staged descriptor state mismatch");
    Require(descriptor.read_only && !descriptor.active,
            "filespace package staged descriptor was not fenced");
    Require(!descriptor.startup_authority &&
                !descriptor.catalog_persistence_owner &&
                !descriptor.filespace_manifest_owner &&
                !descriptor.recovery_evidence_owner &&
                !descriptor.first_filespace,
            "filespace package staged descriptor gained root authority");
  }

  const auto duplicate_import =
      filespace::ImportFilespacePackageToQuarantine(&import_registry, import_request);
  Require(!duplicate_import.ok(), "duplicate filespace package import was accepted");
  Require(duplicate_import.diagnostic.diagnostic_code ==
              "SB-FILESPACE-PACKAGE-IMPORT-DUPLICATE-FILESPACE",
          "duplicate filespace package import diagnostic mismatch");

  const auto admit_refused =
      filespace::AdmitFilespacePackage(&import_registry, import_request);
  Require(!admit_refused.ok(), "filespace package admitted without authority");
  Require(admit_refused.diagnostic.diagnostic_code ==
              "SB-FILESPACE-PACKAGE-ADMIT-AUTHORITY-REQUIRED",
          "filespace package admit authority diagnostic mismatch");

  auto admit_request = import_request;
  admit_request.admission_authorized = true;
  admit_request.operator_identity = "019e2000-0000-7000-8000-000000000301";
  const auto admitted = filespace::AdmitFilespacePackage(&import_registry, admit_request);
  Require(admitted.ok(), "filespace package admit failed");
  Require(admitted.admitted_count == 2, "filespace package admitted count mismatch");
  for (const auto& descriptor : import_registry.filespaces) {
    Require(descriptor.role == filespace::FilespaceRole::secondary_data,
            "filespace package admitted descriptor role mismatch");
    Require(descriptor.state == filespace::FilespaceState::detached,
            "filespace package admitted descriptor state mismatch");
    Require(!descriptor.read_only && !descriptor.active,
            "filespace package admitted descriptor access state mismatch");
  }

  filespace::FilespaceRegistry reject_registry;
  const auto staged_for_reject =
      filespace::ImportFilespacePackageToQuarantine(&reject_registry, import_request);
  Require(staged_for_reject.ok(), "filespace package reject staging failed");

  auto reject_request = import_request;
  reject_request.reject_authorized = true;
  reject_request.operator_identity = "019e2000-0000-7000-8000-000000000302";
  const auto rejected = filespace::RejectFilespacePackage(&reject_registry, reject_request);
  Require(rejected.ok(), "filespace package reject failed");
  Require(rejected.rejected_count == 2, "filespace package rejected count mismatch");
  for (const auto& descriptor : reject_registry.filespaces) {
    Require(descriptor.role == filespace::FilespaceRole::archive_detached,
            "filespace package rejected descriptor role mismatch");
    Require(descriptor.state == filespace::FilespaceState::deleted,
            "filespace package rejected descriptor state mismatch");
    Require(descriptor.read_only && !descriptor.active,
            "filespace package rejected descriptor was not fenced");
  }
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
  const auto replacement_uuid = MakeUuid(UuidKind::filespace, now + 4);
  const auto snapshot_primary_uuid = MakeUuid(UuidKind::filespace, now + 5);
  const auto snapshot_uuid = MakeUuid(UuidKind::filespace, now + 6);
  const auto shadow_uuid = MakeUuid(UuidKind::filespace, now + 7);
  const auto discovery_writer_uuid = MakeUuid(UuidKind::object, now + 8);
  const auto discovery_replacement_writer_uuid = MakeUuid(UuidKind::object, now + 9);
  const auto foreign_database_uuid = MakeUuid(UuidKind::database, now + 10);
  const auto package_uuid = MakeUuid(UuidKind::object, now + 21);
  const auto package_writer_uuid = MakeUuid(UuidKind::object, now + 22);
  const auto package_target_database_uuid = MakeUuid(UuidKind::database, now + 23);
  const auto quarantine_primary_uuid = MakeUuid(UuidKind::filespace, now + 26);
  const auto quarantine_uuid = MakeUuid(UuidKind::filespace, now + 27);
  const auto quarantine_pinned_uuid = MakeUuid(UuidKind::filespace, now + 28);
  const auto move_primary_uuid = MakeUuid(UuidKind::filespace, now + 29);
  const auto move_uuid = MakeUuid(UuidKind::filespace, now + 30);
  const auto move_pinned_uuid = MakeUuid(UuidKind::filespace, now + 31);
  const auto physical_delete_primary_uuid = MakeUuid(UuidKind::filespace, now + 32);
  const auto physical_delete_uuid = MakeUuid(UuidKind::filespace, now + 33);
  const auto physical_delete_pinned_uuid = MakeUuid(UuidKind::filespace, now + 34);
  const auto merge_primary_uuid = MakeUuid(UuidKind::filespace, now + 35);
  const auto merge_source_uuid = MakeUuid(UuidKind::filespace, now + 36);
  const auto merge_target_uuid = MakeUuid(UuidKind::filespace, now + 37);
  const auto merge_pinned_uuid = MakeUuid(UuidKind::filespace, now + 38);
  const auto repair_primary_uuid = MakeUuid(UuidKind::filespace, now + 39);
  std::vector<TypedUuid> repair_filespace_uuids;
  for (std::uint64_t offset = 40; offset < 43; ++offset) {
    repair_filespace_uuids.push_back(MakeUuid(UuidKind::filespace, now + offset));
  }
  std::vector<TypedUuid> discovery_filespace_uuids;
  for (std::uint64_t offset = 11; offset < 21; ++offset) {
    discovery_filespace_uuids.push_back(MakeUuid(UuidKind::filespace, now + offset));
  }
  std::vector<TypedUuid> package_filespace_uuids;
  for (std::uint64_t offset = 24; offset < 26; ++offset) {
    package_filespace_uuids.push_back(MakeUuid(UuidKind::filespace, now + offset));
  }

  TestPageLayoutRegistryCompleteness();
  TestPageRegistryStatusMatrixAndReservedFamilies();
  TestIndexSpecialHeaderContract();
  TestFilespaceLifecycleAndMetrics(dir, database_uuid, primary_uuid, secondary_uuid);
  TestFilespaceQuarantineLifecycle(dir,
                                   database_uuid,
                                   quarantine_primary_uuid,
                                   quarantine_uuid,
                                   quarantine_pinned_uuid);
  TestFilespaceMoveLifecycle(dir,
                             database_uuid,
                             move_primary_uuid,
                             move_uuid,
                             move_pinned_uuid);
  TestFilespaceMergeLifecycle(dir,
                              database_uuid,
                              merge_primary_uuid,
                              merge_source_uuid,
                              merge_target_uuid,
                              merge_pinned_uuid);
  TestFilespacePhysicalDeleteLifecycle(dir,
                                       database_uuid,
                                       physical_delete_primary_uuid,
                                       physical_delete_uuid,
                                       physical_delete_pinned_uuid);
  TestFilespaceRepairRebuildSalvageLifecycle(dir,
                                             database_uuid,
                                             repair_primary_uuid,
                                             repair_filespace_uuids);
  TestActivePrimaryReplacementAuthoritySwitch(dir, database_uuid, primary_uuid, replacement_uuid);
  TestSnapshotShadowLifecycleCommands(dir,
                                      database_uuid,
                                      snapshot_primary_uuid,
                                      snapshot_uuid,
                                      shadow_uuid);
  TestForeignFilespaceQuarantine(dir, database_uuid, foreign_uuid);
  TestFilespaceOrphanStaleDiscovery(dir,
                                    database_uuid,
                                    foreign_database_uuid,
                                    discovery_filespace_uuids,
                                    discovery_writer_uuid,
                                    discovery_replacement_writer_uuid);
  TestFilespaceRuntimeFilesystemDiscoveryScan(dir,
                                             database_uuid,
                                             discovery_filespace_uuids,
                                             discovery_writer_uuid);
  TestFilespaceDiscoveryExecution(dir,
                                  database_uuid,
                                  foreign_database_uuid,
                                  discovery_filespace_uuids,
                                  discovery_writer_uuid);
  TestFilespaceDiscoveryPhysicalCleanupExecution(dir,
                                                 database_uuid,
                                                 discovery_filespace_uuids,
                                                 discovery_writer_uuid);
  TestFilespacePackageWorkflow(dir,
                               database_uuid,
                               package_target_database_uuid,
                               package_uuid,
                               package_filespace_uuids,
                               package_writer_uuid);
  return EXIT_SUCCESS;
}
