// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog_page.hpp"
#include "index_btree_page.hpp"
#include "index_hash_page.hpp"
#include "index_page_family.hpp"
#include "index_specialized_pages.hpp"
#include "overflow_persistence.hpp"
#include "page_body_integrity.hpp"
#include "page_header.hpp"
#include "page_registry.hpp"
#include "page_skeleton.hpp"
#include "row_data_page.hpp"
#include "structured_page_body.hpp"
#include "transaction_inventory_page.hpp"
#include "transaction_horizon.hpp"
#include "transaction_inventory.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace disk = scratchbird::storage::disk;
namespace page = scratchbird::storage::page;
namespace platform = scratchbird::core::platform;
namespace txn = scratchbird::transaction::mga;
namespace uuid = scratchbird::core::uuid;

constexpr platform::u32 kPageSize = 8192;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

platform::TypedUuid MakeUuid(platform::UuidKind kind, platform::u64 seed) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, 1980000000000ull + seed);
  if (!generated.ok()) {
    Fail("uuid generation failed");
  }
  return generated.value;
}

disk::SerializedPageHeader HeaderFor(disk::PageType page_type,
                                     platform::u64 page_number) {
  disk::PageHeader header;
  header.page_size = kPageSize;
  header.page_type = page_type;
  header.database_uuid = MakeUuid(platform::UuidKind::database, 1).value;
  header.filespace_uuid = MakeUuid(platform::UuidKind::filespace, 2).value;
  header.page_uuid = MakeUuid(platform::UuidKind::page, 1000 + page_number).value;
  header.page_number = page_number;
  header.page_generation = 3;
  const auto serialized = disk::SerializePageHeader(header);
  if (!serialized.ok()) {
    std::cerr << serialized.diagnostic.diagnostic_code << '\n';
  }
  Require(serialized.ok(), "page header serialization failed");
  return serialized.serialized;
}

std::vector<platform::byte> Payload(std::string_view value) {
  return std::vector<platform::byte>(value.begin(), value.end());
}

page::StructuredPageBody StructuredBodyFor(const page::PageFamilyDescriptor& descriptor,
                                           platform::u64 page_number) {
  page::StructuredPageBody body;
  body.page_type = descriptor.page_type;
  body.page_family = descriptor.family;
  body.page_number = page_number;
  body.generation = 7;
  body.records.push_back({1, 1, descriptor.stable_name + ".primary", Payload("primary")});
  body.records.push_back({2, 2, descriptor.stable_name + ".secondary", Payload("secondary")});
  return body;
}

void ProveStructuredBody(const page::PageFamilyDescriptor& descriptor,
                         platform::u64 page_number) {
  const auto built = page::BuildStructuredPageBody(
      StructuredBodyFor(descriptor, page_number), kPageSize);
  if (!built.ok()) {
    std::cerr << descriptor.stable_name << " " << built.diagnostic.diagnostic_code << '\n';
  }
  Require(built.ok(), "structured page body build failed");

  const auto parsed = page::ParseStructuredPageBody(built.serialized);
  Require(parsed.ok(), "structured page body parse failed");
  Require(parsed.body.page_type == descriptor.page_type,
          "structured page type did not round trip");
  Require(parsed.body.page_family == descriptor.family,
          "structured page family did not round trip");
  Require(parsed.body.records.size() == 2,
          "structured page records did not round trip");

  page::StructuredPageBodyMutation upsert;
  upsert.kind = page::StructuredPageBodyMutationKind::upsert_record;
  upsert.record = {1, 3, descriptor.stable_name + ".primary", Payload("updated")};
  const auto upserted = page::ApplyStructuredPageBodyMutation(parsed.body,
                                                              upsert,
                                                              kPageSize);
  Require(upserted.ok(), "structured page body upsert failed");
  const auto parsed_upsert = page::ParseStructuredPageBody(upserted.serialized);
  Require(parsed_upsert.ok(), "structured page body upsert parse failed");
  Require(parsed_upsert.body.generation == parsed.body.generation + 1,
          "structured page body generation did not advance");
  Require(parsed_upsert.body.records.size() == 2,
          "structured page body upsert changed record count");
  Require(parsed_upsert.body.records[0].payload == Payload("updated"),
          "structured page body upsert did not replace record");

  page::StructuredPageBodyMutation deletion;
  deletion.kind = page::StructuredPageBodyMutationKind::delete_record;
  deletion.record = {2, 0, descriptor.stable_name + ".secondary", {}};
  const auto deleted = page::ApplyStructuredPageBodyMutation(parsed_upsert.body,
                                                            deletion,
                                                            kPageSize);
  Require(deleted.ok(), "structured page body delete failed");
  const auto parsed_deleted = page::ParseStructuredPageBody(deleted.serialized);
  Require(parsed_deleted.ok(), "structured page body delete parse failed");
  Require(parsed_deleted.body.records.size() == 1,
          "structured page body delete did not remove record");

  page::PageBodyAgreementRequest agreement;
  agreement.header = HeaderFor(descriptor.page_type, page_number);
  agreement.body = upserted.serialized;
  agreement.checksum_profile = page::PageBodyChecksumProfile::strong;
  const auto agreed = page::ValidatePageBodyAgreement(agreement);
  if (!agreed.ok()) {
    std::cerr << descriptor.stable_name << " " << agreed.diagnostic.diagnostic_code << '\n';
  }
  Require(agreed.ok(), "structured page body agreement failed");
  Require(agreed.production_admitted, "structured page body was not admitted");
  if (descriptor.supported_local_write) {
    Require(agreed.production_mutating,
            "structured writable page body did not admit mutation");
  }
}

void ProveAdmissionRegistry() {
  platform::u64 page_number = 100;
  for (const auto& descriptor : page::BuiltinPageFamilyRegistry()) {
    const auto header = HeaderFor(descriptor.page_type, page_number++);
    const auto admission = page::ClassifyPageBodyProductionAdmission(
        disk::ClassifyPageHeader(header));
    if (descriptor.cluster_only) {
      Require(!admission.admitted,
              "cluster page body was admitted locally");
      Require(admission.kind ==
                  page::PageBodyProductionAdmissionKind::external_cluster_provider_required,
              "cluster page body used wrong refusal kind");
      continue;
    }
    if (descriptor.encrypted_or_opaque) {
      Require(!admission.admitted,
              "encrypted opaque page body was admitted locally");
      Require(admission.kind ==
                  page::PageBodyProductionAdmissionKind::decryption_required,
              "encrypted opaque page body used wrong refusal kind");
      continue;
    }
    if (descriptor.reserved) {
      Require(!admission.admitted,
              "reserved page body was admitted locally");
      Require(admission.kind ==
                  page::PageBodyProductionAdmissionKind::reserved_nonmutating,
              "reserved page body used wrong refusal kind");
      continue;
    }

    const auto skeleton = page::LookupPageSkeleton(descriptor.page_type);
    Require(skeleton.ok(), "local page skeleton lookup failed");
    Require(skeleton.descriptor.state == page::PageSkeletonState::body_implemented,
            "local page skeleton is not body implemented");
    Require(skeleton.descriptor.body_parser_available,
            "local page body parser is unavailable");
    if (descriptor.supported_local_write) {
      Require(skeleton.descriptor.body_mutation_available,
              "local writable page body mutation is unavailable");
      Require(admission.admitted &&
                  admission.kind ==
                      page::PageBodyProductionAdmissionKind::local_engine_mutating,
              "local writable page body was not admitted for mutation");
    } else {
      Require(admission.admitted &&
                  admission.kind ==
                      page::PageBodyProductionAdmissionKind::local_engine_read_only,
              "local read-only page body was not admitted for interpretation");
    }
    ProveStructuredBody(descriptor, page_number++);
  }
}

void ProveSpecificCodecs() {
  page::CatalogPageRow catalog_row;
  catalog_row.kind = page::CatalogPageRowKind::typed_catalog_record;
  catalog_row.ordinal = 1;
  catalog_row.payload = "catalog";
  const auto catalog = page::BuildCatalogPageSet({catalog_row}, kPageSize, 300, 350);
  if (!catalog.ok()) {
    std::cerr << catalog.diagnostic.diagnostic_code << '\n';
  }
  Require(catalog.ok() && !catalog.pages.empty(), "catalog page build failed");
  Require(page::ParseCatalogPageBody(catalog.pages[0].body, 300).ok(),
          "catalog page parse failed");

  page::TransactionInventoryPageBody inventory_body;
  inventory_body.page_number = 301;
  inventory_body.inventory_generation = 2;
  inventory_body.inventory = txn::MakeEmptyLocalTransactionInventory();
  const auto inventory = page::BuildTransactionInventoryPageBody(
      inventory_body, kPageSize);
  if (!inventory.ok()) {
    std::cerr << inventory.diagnostic.diagnostic_code << '\n';
  }
  Require(inventory.ok(), "transaction inventory page build failed");
  Require(page::ParseTransactionInventoryPageBody(inventory.serialized, 301).ok(),
          "transaction inventory page parse failed");

  page::RowDataPageBody row_body;
  row_body.relation_uuid = MakeUuid(platform::UuidKind::object, 10);
  row_body.segment_id = 1;
  row_body.segment_generation = 1;
  row_body.page_number = 302;
  row_body.page_generation = 1;
  const auto row = page::BuildRowDataPageBody(row_body, kPageSize);
  if (!row.ok()) {
    std::cerr << row.diagnostic.diagnostic_code << '\n';
  }
  Require(row.ok(), "row data page build failed");
  Require(page::ParseRowDataPageBody(row.serialized, 302).ok(),
          "row data page parse failed");

  page::IndexBtreePageBody btree_body;
  btree_body.index_uuid = MakeUuid(platform::UuidKind::object, 11);
  btree_body.page_number = 303;
  btree_body.tree_level = 0;
  btree_body.page_kind = page::IndexBtreePageKind::leaf;
  const auto btree = page::BuildIndexBtreePageBody(btree_body, kPageSize);
  if (!btree.ok()) {
    std::cerr << btree.diagnostic.diagnostic_code << '\n';
  }
  Require(btree.ok(), "btree page build failed");
  Require(page::ParseIndexBtreePageBody(btree.serialized, 303).ok(),
          "btree page parse failed");

  page::IndexHashPageBody hash_body;
  hash_body.index_uuid = MakeUuid(platform::UuidKind::object, 12);
  hash_body.page_number = 304;
  hash_body.page_kind = page::IndexHashPageKind::directory;
  hash_body.hash_seed = 101;
  hash_body.hash_seed_high64 = 202;
  hash_body.bucket_count = 1;
  hash_body.directory_bucket_page_numbers.push_back(305);
  const auto hash = page::BuildIndexHashPageBody(hash_body, kPageSize);
  if (!hash.ok()) {
    std::cerr << hash.diagnostic.diagnostic_code << '\n';
  }
  Require(hash.ok(), "hash page build failed");
  Require(page::ParseIndexHashPageBody(hash.serialized, 304).ok(),
          "hash page parse failed");

  page::IndexSpecializedPageBody specialized_body;
  specialized_body.header.index_object_uuid = MakeUuid(platform::UuidKind::object, 13);
  specialized_body.header.family_uuid = MakeUuid(platform::UuidKind::object, 14);
  specialized_body.header.family = page::IndexPageFamilyKind::bitmap;
  specialized_body.header.page_type = disk::PageType::index_bitmap;
  specialized_body.header.logical_page_number = 306;
  specialized_body.header.resource_epoch = 1;
  specialized_body.header.mutation_epoch = 1;
  specialized_body.entries.push_back({1, 1, MakeUuid(platform::UuidKind::object, 15), 7, 9, Payload("bitmap")});
  const auto specialized = page::BuildIndexSpecializedPageBody(specialized_body, kPageSize);
  if (!specialized.ok()) {
    std::cerr << specialized.diagnostic.diagnostic_code << '\n';
  }
  Require(specialized.ok(), "specialized index page build failed");
  Require(page::ParseIndexSpecializedPageBody(specialized.serialized).ok(),
          "specialized index page parse failed");
}

void ProveCorruptionRefusal() {
  const auto descriptor = page::LookupPageFamily(disk::PageType::metrics);
  Require(descriptor.ok(), "metrics descriptor lookup failed");
  const auto built = page::BuildStructuredPageBody(
      StructuredBodyFor(descriptor.descriptor, 400), kPageSize);
  Require(built.ok(), "metrics structured body build failed");
  auto corrupted = built.serialized;
  corrupted[page::kStructuredPageBodyHeaderBytes + 20] ^= 0x7f;
  Require(!page::ParseStructuredPageBody(corrupted).ok(),
          "corrupted structured page body parsed successfully");

  page::PageBodyAgreementRequest agreement;
  agreement.header = HeaderFor(disk::PageType::metrics, 400);
  agreement.body = built.serialized;
  agreement.body[0] = 'X';
  const auto refused = page::ValidatePageBodyAgreement(agreement);
  Require(!refused.ok(), "unknown body kind agreement succeeded");
  Require(refused.kind == page::PageBodyAgreementKind::unsupported_body_kind,
          "unknown body kind used wrong refusal");
}

}  // namespace

int main() {
  ProveAdmissionRegistry();
  ProveSpecificCodecs();
  ProveCorruptionRefusal();
  return EXIT_SUCCESS;
}
