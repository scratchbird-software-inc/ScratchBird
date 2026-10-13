// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "catalog/catalog_object_lifecycle.hpp"
#include "ddl/create_api.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "transaction_inventory.hpp"
#include "catalog/column_metadata_codec.hpp"
#include "mga_relation_store/mga_savepoint_store.hpp"

#include <map>
#include <functional>
#include <array>

namespace scratchbird::engine::internal_api {

// Engine-local preparation data. Generations/schema/catalog-row identity are
// assigned from actual authorized catalog preparation by the owning coordinator,
// never accepted as publication authority from this structure.
struct PreparedCatalogTableStorage {
  CrudTableRecord table;
  std::vector<CrudIndexRecord> indexes;
  std::map<EngineUuid, std::vector<EngineLocalizedName>> index_names;
  EngineUuid schema_uuid;
  EngineCreateTableResult result;
};

// Actual read-only source fingerprint, not an admission or caller authority.
struct CatalogTableCreationSources {
  transaction::mga::TransactionInventoryPublicationBase inventory;
  struct Journal {
    bool present = false;
    std::uint64_t bytes = 0;
    std::array<std::uint8_t,32> sha256{};
    bool operator==(const Journal&) const = default;
  };
  std::array<Journal,5> journals;
  std::map<std::uint64_t, std::map<std::string, SavepointCutoffs>> active_savepoints;
  std::map<std::uint64_t, std::vector<SavepointRollbackRange>> rollback_ranges;
  std::uint64_t next_savepoint_ordinal = 0;
  std::vector<EngineUuid> context_identities;
  std::vector<EngineApiU64> context_generations;
  std::array<std::vector<EngineApiU64>,2> visibility_exclusions;
  std::vector<std::string> context_modes;
  bool operator==(const CatalogTableCreationSources&) const = default;
};
EngineApiDiagnostic CaptureCatalogTableCreationSources(
    const EngineRequestContext&, CatalogTableCreationSources*);

// Deliberately excludes effectful column metadata. This is a capability
// profile, not a claim that defaults, domains or constraints cannot exist.
inline bool IsPlainSavepointTableColumn(const EngineColumnDefinition& column) {
  if (column.descriptor.descriptor_kind != "scalar" ||
      !column.default_expression_envelope.empty()) return false;
  CatalogColumnMetadata fields;
  if (!AdmitCatalogColumnMetadata(column.descriptor.encoded_descriptor, &fields)) return false;
  for (const auto& [key, value] : fields.text) {
    if (key != "type" && key != "canonical" && key != "nullable" &&
        key != "datatype_descriptor_generation" && key != "type_generation" &&
        key != "codec_id" && key != "codec_version" && key != "codec_generation" &&
        key != "null_encoding" && key != "length" && key != "precision" &&
        key != "scale" && key != "maximum_bytes" && key != "maximum_characters") return false;
  }
  for (const auto& [key, value] : fields.identities) {
    if (key != "type_uuid" && key != "datatype_descriptor_uuid" &&
        key != "codec_uuid" && key != "charset_uuid" && key != "collation_uuid") return false;
  }
  return true;
}

// Lexical custody of the trusted CREATE preparation route, not a substitute
// for current native transaction, security, policy or savepoint admission.
// No preparation DTO or external caller can manufacture or retain this token.
class CatalogTableCreationCustody final {
 public:
  CatalogTableCreationCustody(const CatalogTableCreationCustody&) = delete;
  CatalogTableCreationCustody& operator=(const CatalogTableCreationCustody&) = delete;
  CatalogTableCreationCustody(CatalogTableCreationCustody&&) = delete;
  CatalogTableCreationCustody& operator=(CatalogTableCreationCustody&&) = delete;
  EngineApiDiagnostic Readmit() const { return readmit_(); }

 private:
  explicit CatalogTableCreationCustody(std::function<EngineApiDiagnostic()> readmit)
      : readmit_(std::move(readmit)) {}
  friend EngineCreateTableResult EngineCreateTable(const EngineCreateTableRequest& request);
  const std::function<EngineApiDiagnostic()> readmit_;
};

EngineCatalogCreateObjectResult EngineCatalogCreateObjectWithTableStorage(
    const CatalogTableCreationCustody& custody,
    const EngineCatalogCreateObjectRequest& request,
    PreparedCatalogTableStorage* storage);

} // namespace scratchbird::engine::internal_api
