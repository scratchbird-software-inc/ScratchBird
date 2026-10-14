// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "database_lifecycle.hpp"
#include "metric_registry.hpp"
#include "memory.hpp"
#include "catalog_page.hpp"
#include "catalog_record_codec.hpp"
#include "catalog_schema_record_codec.hpp"
#include "page_header.hpp"
#include "observability/metrics_api.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"
#include "../support/engine_statement_fixture.hpp"
#include "../support/owned_temp_directory.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <functional>

namespace db = scratchbird::storage::database;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;
static void Require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
namespace catalog = scratchbird::core::catalog;
namespace page = scratchbird::storage::page;
namespace disk = scratchbird::storage::disk;
struct CatalogImage {
  std::vector<page::SerializedCatalogPageBody> pages;
  std::vector<page::CatalogPageRow> rows;
};
static CatalogImage CaptureCatalog(const std::string& path, unsigned size) {
  CatalogImage image;
  std::ifstream file(path, std::ios::binary);
  std::set<std::uint64_t> visited;
  auto number = db::kCatalogPageNumber;
  while (number) {
    Require(visited.insert(number).second, "fixture catalog cycle");
    std::vector<scratchbird::core::platform::byte> body(size - disk::kPageHeaderSerializedBytes);
    file.seekg(number * std::uint64_t(size) + disk::kPageHeaderSerializedBytes);
    file.read(reinterpret_cast<char*>(body.data()), body.size());
    Require(file.good(), "fixture catalog read failed");
    const auto parsed = page::ParseCatalogPageBody(body, number);
    Require(parsed.ok(), "fixture catalog integrity failed");
    image.pages.push_back({number, parsed.body.next_page_number, std::move(body)});
    image.rows.insert(image.rows.end(), parsed.body.rows.begin(), parsed.body.rows.end());
    number = parsed.body.next_page_number;
  }
  return image;
}
static void WriteBodies(const std::string& path, unsigned size,
                        const std::vector<page::SerializedCatalogPageBody>& pages) {
  std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
  for (const auto& body : pages) {
    Require(page::ParseCatalogPageBody(body.body, body.page_number).ok(), "fault body not structurally valid");
    file.seekp(body.page_number * std::uint64_t(size) + disk::kPageHeaderSerializedBytes);
    file.write(reinterpret_cast<const char*>(body.body.data()), body.body.size());
    Require(file.good(), "fault body write failed");
  }
  file.flush(); Require(file.good(), "fault body flush failed");
}
static void CheckFault(const db::DatabaseCreateConfig& create, const CatalogImage& original,
                       const std::function<void(std::vector<page::CatalogPageRow>&)>& change,
                       bool expect_refusal = true) {
  auto rows = original.rows;
  change(rows);
  const auto resealed = page::BuildCatalogPageSet(rows, create.page_size, db::kCatalogPageNumber,
      original.pages.size() > 1 ? original.pages[1].page_number : db::kCatalogPageNumber + 1);
  Require(resealed.ok() && resealed.pages.size() <= original.pages.size(), "fault requires additional page allocation");
  for (std::size_t i = 0; i < resealed.pages.size(); ++i)
    Require(resealed.pages[i].page_number == original.pages[i].page_number, "fault changed page identity");
  WriteBodies(create.path, create.page_size, resealed.pages);
  const auto refused = db::ReadDatabaseBootstrapMetricCatalog(create.path, create.database_uuid.value);
  if (expect_refusal)
    Require(!refused.ok() && refused.definitions.empty(), "semantic catalog corruption exposed partial definitions");
  else Require(refused.ok() && refused.definitions.size() == 7, "valid exact rate-source dependency refused");
  WriteBodies(create.path, create.page_size, original.pages);
  Require(db::ReadDatabaseBootstrapMetricCatalog(create.path, create.database_uuid.value).ok(),
          "original catalog not restored");
}
static void ChangeRecord(std::vector<page::CatalogPageRow>& rows, catalog::CatalogRecordKind kind,
    const std::function<bool(catalog::CatalogTypedRecord&)>& change) {
  for (auto& row : rows) {
    if (row.kind != page::CatalogPageRowKind::typed_catalog_record) continue;
    auto record = catalog::DecodeCatalogTypedRecord(row);
    Require(record.ok(), "fault input typed record invalid");
    if (record.record.header.kind != kind || !change(record.record)) continue;
    const auto encoded = catalog::EncodeCatalogTypedRecord(record.record, row.ordinal);
    Require(encoded.ok(), "fault typed record could not be validly encoded");
    row = encoded.row;
    return;
  }
  throw std::runtime_error("fault record not found");
}

int main(int argc, char** argv) {
  try {
    auto memory_policy = scratchbird::core::memory::DefaultLocalEngineMemoryPolicy();
    memory_policy.policy_name = "metric_creation_publication_conformance";
    Require(scratchbird::core::memory::ConfigureDefaultMemoryManagerForFixture(
        memory_policy, "metric_creation_publication_conformance").ok(), "memory fixture admission failed");
    scratchbird::tests::OwnedTempDirectory temporary;
    const auto source = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / "config/templates";
    const auto defaults = temporary.path() / "defaults.conf";
    const auto families = temporary.path() / "families.conf";
    std::filesystem::copy_file(source / "metric-policy-defaults.conf", defaults);
    std::filesystem::copy_file(source / "metric-policy-families.conf", families);
    db::DatabaseCreateConfig create;
    if (argc == 2) create.page_size = static_cast<scratchbird::core::platform::u32>(std::stoul(argv[1]));
    create.path = (temporary.path() / "node.sb").string();
    const auto database = uuid::GenerateEngineIdentityV7(UuidKind::database, 1792000000000ULL);
    const auto filespace = uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1792000000001ULL);
    Require(database.ok() && filespace.ok(), "identity allocation failed");
    create.database_uuid = database.value;
    create.filespace_uuid = filespace.value;
    create.creation_unix_epoch_millis = 1792000000002ULL;
    scratchbird::tests::ConfigureCredentialedFixtureBootstrap(create);
    create.metric_policy_defaults_path = defaults.string();
    Require(!db::CreateDatabaseFile(create).ok() && !std::filesystem::exists(create.path),
            "one-file configuration must refuse before creation");
    create.metric_policy_families_path = (temporary.path() / "missing.conf").string();
    Require(!db::CreateDatabaseFile(create).ok() && !std::filesystem::exists(create.path),
            "missing configuration must refuse before creation");
    create.metric_policy_families_path = families.string();
    {
      std::ofstream file(families, std::ios::app);
      file << "\n[unknown_native_family]\nprofile = operational\n";
      Require(file.good(), "unknown family fixture write failed");
    }
    Require(!db::CreateDatabaseFile(create).ok() && !std::filesystem::exists(create.path),
            "unknown family must refuse before storage effects");
    std::filesystem::copy_file(source / "metric-policy-families.conf", families,
                              std::filesystem::copy_options::overwrite_existing);
    {
      std::ofstream file(families, std::ios::app);
      file << "\n[sb_tx_begin_total]\nprofile = operational\n";
      Require(file.good(), "duplicate family fixture write failed");
    }
    Require(!db::CreateDatabaseFile(create).ok() && !std::filesystem::exists(create.path),
            "duplicate family must refuse before storage effects");
    std::filesystem::copy_file(source / "metric-policy-families.conf", families,
                              std::filesystem::copy_options::overwrite_existing);
    const auto registry_before = scratchbird::core::metrics::DefaultMetricRegistry().Descriptors(true).size();
    const auto created = db::CreateDatabaseFile(create);
    if (!created.ok()) std::cerr << created.diagnostic.diagnostic_code << ':' << created.diagnostic.message_key << '\n';
    Require(created.ok(), "configured creation failed");
    const auto read = db::ReadDatabaseBootstrapMetricCatalog(create.path, create.database_uuid.value);
    Require(read.ok() && read.definitions.size() == 7, "seven complete native bundles not persisted");
    std::set<scratchbird::core::platform::Uuid> identities;
    std::set<std::string> names;
    for (const auto& bundle : read.definitions) {
      const auto& binding = bundle.descriptor.binding;
      Require(names.insert(bundle.descriptor.definition.family).second, "duplicate family");
      for (const auto& id : {binding.metric_uuid, binding.retention_policy_uuid, binding.visibility_policy_uuid})
        Require(uuid::IsEngineIdentityUuid(id) && identities.insert(id).second, "identity not distinct native v7");
      Require(binding.descriptor_generation == 1 && binding.retention_policy_generation == 1 &&
              binding.visibility_policy_generation == 1, "generation not creation generation");
      Require(bundle.retention.policy.policy_uuid == binding.retention_policy_uuid &&
              bundle.visibility.policy_uuid == binding.visibility_policy_uuid &&
              bundle.visibility.database_uuid == create.database_uuid.value &&
              bundle.visibility.metric_uuid == binding.metric_uuid, "policy binding mismatch");
      Require(bundle.descriptor.origin_local_transaction_id != 0 &&
              bundle.descriptor.origin_local_transaction_id == bundle.retention.origin_local_transaction_id &&
              bundle.descriptor.origin_local_transaction_id == bundle.visibility.origin_local_transaction_id &&
              bundle.descriptor.origin_transaction_uuid.value == bundle.retention.origin_transaction_uuid.value &&
              bundle.descriptor.origin_transaction_uuid.value == bundle.visibility.origin_transaction_uuid.value,
              "dependencies lack coherent creation origin");
      if (bundle.labels) Require(uuid::IsEngineIdentityUuid(binding.label_schema_uuid) &&
          identities.insert(binding.label_schema_uuid).second && binding.label_schema_generation == 1 &&
          bundle.labels->origin_transaction_uuid.value == bundle.descriptor.origin_transaction_uuid.value,
          "label binding or origin invalid");
    }
    Require(names.count("sb_mga_row_versions_reclaimed_total") == 1, "MGA definition absent");
    std::filesystem::remove(defaults);
    std::filesystem::remove(families);
    const auto reopened = db::ReadDatabaseBootstrapMetricCatalog(create.path, create.database_uuid.value);
    Require(reopened.ok() && reopened.definitions.size() == read.definitions.size(), "reopen depended on input files");
    for (std::size_t i = 0; i < read.definitions.size(); ++i)
      Require(reopened.definitions[i].descriptor.binding.metric_uuid == read.definitions[i].descriptor.binding.metric_uuid,
              "reopen reallocated identity");
    Require(scratchbird::core::metrics::DefaultMetricRegistry().Descriptors(true).size() == registry_before,
            "creation mutated process-global registry");
    {
      namespace api = scratchbird::engine::internal_api;
      auto owner = scratchbird::tests::BootstrapFixtureOwnerContext(create);
      api::EngineBeginTransactionRequest begin;
      begin.context = owner;
      begin.isolation_level = "read_committed";
      const auto transaction = api::EngineBeginTransaction(begin);
      Require(transaction.ok, "registry transaction admission failed");
      owner.transaction_uuid = transaction.transaction_uuid;
      owner.local_transaction_id = transaction.local_transaction_id;
      owner.transaction_isolation_level = transaction.isolation_level;
      owner.snapshot_visible_through_local_transaction_id = transaction.snapshot_visible_through_local_transaction_id;
      scratchbird::tests::FixtureEngineSession session(owner);
      scratchbird::tests::FixtureEngineStatement statement(session, owner);
      api::EngineSysMetricsRegistryRequest request;
      request.context = statement.context;
      const auto visible = api::EngineSysMetricsRegistry(request);
      Require(visible.ok && visible.result_shape.rows.size() == 7, "live registry omitted native definitions");
      for (const auto& row : visible.result_shape.rows) {
        bool unvalidated = false;
        for (const auto& field : row.fields)
          if (field.first == "readiness") unvalidated = field.second.encoded_value == "unvalidated";
        Require(unvalidated, "publication asserted producer activation");
      }
      request.context.authorization_context = {};
      const auto denied = api::EngineSysMetricsRegistry(request);
      Require(!denied.ok && denied.result_shape.rows.empty(), "unauthorized registry disclosed native definitions");
      request.context = statement.context;
      request.context.authorization_context.authority_uuid = create.filespace_uuid.value;
      const auto foreign_authority = api::EngineSysMetricsRegistry(request);
      Require(!foreign_authority.ok && foreign_authority.result_shape.rows.empty(), "foreign authority disclosed definitions");
      request.context = statement.context;
      ++request.context.security_epoch;
      const auto expired = api::EngineSysMetricsRegistry(request);
      Require(!expired.ok && expired.result_shape.rows.empty(), "expired authority disclosed definitions");
      api::EngineRollbackTransactionRequest rollback;
      rollback.context = statement.context;
      Require(api::EngineRollbackTransaction(rollback).ok, "registry transaction cleanup failed");
    }
    const auto catalog_image = CaptureCatalog(create.path, create.page_size);
    const auto foreign_object = uuid::GenerateEngineIdentityV7(UuidKind::object, 1792000000040ULL);
    const auto foreign_transaction = uuid::GenerateEngineIdentityV7(UuidKind::transaction, 1792000000041ULL);
    Require(foreign_object.ok() && foreign_transaction.ok(), "fault identity allocation failed");
    CheckFault(create, catalog_image, [&](auto& rows) {
      const auto descriptor = std::find_if(rows.begin(), rows.end(), [&](const auto& row) {
        if (row.kind != page::CatalogPageRowKind::typed_catalog_record) return false;
        const auto decoded = catalog::DecodeCatalogTypedRecord(row);
        Require(decoded.ok(), "orphan fault input decode failed");
        return decoded.record.header.kind == catalog::CatalogRecordKind::metric_descriptor;
      });
      Require(descriptor != rows.end(), "orphan fault descriptor missing");
      rows.erase(descriptor);
    });
    CheckFault(create, catalog_image, [&](auto& rows) {
      ChangeRecord(rows, catalog::CatalogRecordKind::database, [&](auto& record) {
        auto database_record = catalog::DecodeCatalogDatabaseRecord(record.payload);
        Require(database_record.ok(), "database manifest fault decode failed");
        ++database_record.record->catalog_manifest_format_version;
        const auto encoded = catalog::EncodeCatalogDatabaseRecord(*database_record.record);
        Require(encoded.ok(), "database manifest fault encode failed");
        record.payload.assign(encoded.bytes.begin(), encoded.bytes.end()); return true;
      });
    });
    for (unsigned fault = 0; fault < 3; ++fault) CheckFault(create, catalog_image, [&](auto& rows) {
      ChangeRecord(rows, catalog::CatalogRecordKind::schema, [&](auto& record) {
        auto schema = catalog::DecodeCatalogSchemaRecord(record.payload);
        Require(schema.ok(), "schema fault decode failed");
        if (schema.record->path_cache != "sys.metrics") return false;
        if (fault == 0) schema.record->schema_object_uuid = foreign_object.value;
        else schema.record->parent_object_uuid = foreign_object.value;
        if (fault == 2) record.header.parent_uuid = foreign_object.value;
        const auto encoded = catalog::EncodeCatalogSchemaRecord(*schema.record);
        Require(encoded.ok(), "schema fault encode failed");
        record.payload.assign(encoded.bytes.begin(), encoded.bytes.end()); return true;
      });
    });
    CheckFault(create, catalog_image, [&](auto& rows) {
      scratchbird::core::platform::Uuid visibility_identity, reused_identity;
      ChangeRecord(rows, catalog::CatalogRecordKind::metric_descriptor, [&](auto& record) {
        auto descriptor = catalog::DecodeCatalogMetricDescriptor(record.payload);
        Require(descriptor.ok(), "duplicate identity descriptor decode failed");
        reused_identity = record.header.parent_uuid.value;
        visibility_identity = descriptor.record->binding.visibility_policy_uuid;
        descriptor.record->binding.metric_uuid = reused_identity;
        record.header.object_uuid.value = reused_identity;
        const auto encoded = catalog::EncodeCatalogMetricDescriptor(*descriptor.record);
        Require(encoded.ok(), "duplicate identity descriptor encode failed");
        record.payload.assign(encoded.bytes.begin(), encoded.bytes.end()); return true;
      });
      ChangeRecord(rows, catalog::CatalogRecordKind::policy, [&](auto& record) {
        if (record.header.object_uuid.value != visibility_identity) return false;
        auto visibility = catalog::DecodeCatalogMetricVisibilityPolicy(record.payload);
        Require(visibility.ok(), "duplicate identity visibility decode failed");
        visibility.record->metric_uuid = reused_identity;
        const auto encoded = catalog::EncodeCatalogMetricVisibilityPolicy(*visibility.record);
        Require(encoded.ok(), "duplicate identity visibility encode failed");
        record.payload.assign(encoded.bytes.begin(), encoded.bytes.end()); return true;
      });
    });
    for (unsigned fault = 0; fault < 3; ++fault) CheckFault(create, catalog_image, [&](auto& rows) {
      ChangeRecord(rows, catalog::CatalogRecordKind::metric_descriptor, [&](auto& record) {
        auto descriptor = catalog::DecodeCatalogMetricDescriptor(record.payload);
        Require(descriptor.ok(), "descriptor fault decode failed");
        if (fault == 0) ++descriptor.record->binding.retention_policy_generation;
        if (fault == 1) descriptor.record->origin_transaction_uuid = foreign_transaction.value;
        if (fault == 2) descriptor.record->binding.retention_policy_uuid = foreign_object.value.value;
        const auto encoded = catalog::EncodeCatalogMetricDescriptor(*descriptor.record);
        Require(encoded.ok(), "descriptor fault encode failed");
        record.payload.assign(encoded.bytes.begin(), encoded.bytes.end()); return true;
      });
    });
    for (unsigned fault = 0; fault < 4; ++fault) CheckFault(create, catalog_image, [&](auto& rows) {
      ChangeRecord(rows, catalog::CatalogRecordKind::metric_descriptor, [&](auto& record) {
        auto descriptor = catalog::DecodeCatalogMetricDescriptor(record.payload);
        Require(descriptor.ok(), "rate fault decode failed");
        if (descriptor.record->definition.family != "sb_tx_begin_total") return false;
        descriptor.record->definition.type = scratchbird::core::metrics::MetricType::rate;
        descriptor.record->definition.rate_window_nanoseconds = 1000000000;
        const auto source_name = fault == 2 ? "sb_tx_active_transactions" : "sb_tx_commit_total";
        const auto source = std::find_if(read.definitions.begin(), read.definitions.end(), [&](const auto& candidate) {
          return candidate.descriptor.definition.family == source_name;
        });
        Require(source != read.definitions.end(), "rate fault source absent");
        descriptor.record->binding.rate_source_counter_uuid = fault == 0 ? foreign_object.value.value :
            source->descriptor.binding.metric_uuid;
        descriptor.record->binding.rate_source_counter_generation = fault == 1 ? 2 : 1;
        const auto encoded = catalog::EncodeCatalogMetricDescriptor(*descriptor.record);
        Require(encoded.ok(), "rate fault encode failed");
        record.payload.assign(encoded.bytes.begin(), encoded.bytes.end()); return true;
      });
    }, fault != 3);
    const auto foreign = db::ReadDatabaseBootstrapMetricCatalog(create.path, create.filespace_uuid.value);
    Require(!foreign.ok() && foreign.definitions.empty(), "foreign node identity accepted");
    {
      std::fstream file(create.path, std::ios::in | std::ios::out | std::ios::binary);
      char byte = 0;
      file.read(&byte, 1); byte ^= 1; file.seekp(0); file.write(&byte, 1);
      Require(file.good(), "corruption injection failed");
    }
    const auto corrupt = db::ReadDatabaseBootstrapMetricCatalog(create.path, create.database_uuid.value);
    Require(!corrupt.ok() && corrupt.definitions.empty(), "corrupt header exposed definitions");
    temporary.Cleanup();
    std::cout << "native metric creation publication PASS\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n'; return 1;
  }
}
