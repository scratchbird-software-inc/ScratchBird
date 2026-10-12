// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "catalog/catalog_object_lifecycle.hpp"
#include "ddl/create_api.hpp"
#include "mga_relation_store/mga_relation_store.hpp"

#include <map>

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

// Lexical custody of the trusted CREATE preparation route, not a substitute
// for current native transaction, security, policy or savepoint admission.
// No preparation DTO or external caller can manufacture or retain this token.
class CatalogTableCreationCustody final {
 public:
  CatalogTableCreationCustody(const CatalogTableCreationCustody&) = delete;
  CatalogTableCreationCustody& operator=(const CatalogTableCreationCustody&) = delete;
  CatalogTableCreationCustody(CatalogTableCreationCustody&&) = delete;
  CatalogTableCreationCustody& operator=(CatalogTableCreationCustody&&) = delete;

 private:
  CatalogTableCreationCustody() = default;
  friend EngineCreateTableResult EngineCreateTable(const EngineCreateTableRequest& request);
};

EngineCatalogCreateObjectResult EngineCatalogCreateObjectWithTableStorage(
    const CatalogTableCreationCustody& custody,
    const EngineCatalogCreateObjectRequest& request,
    PreparedCatalogTableStorage* storage);

} // namespace scratchbird::engine::internal_api
