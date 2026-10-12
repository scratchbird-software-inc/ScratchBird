// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog/catalog_object_lifecycle.hpp"
#include "catalog/relation_projection_view.hpp"
#include "ddl/create_api.hpp"
#include <functional>
#include <optional>

namespace scratchbird::engine::internal_api {
// Lexical custody of an actually validated typed definition. Neither the
// proposal DTO nor an arbitrary payload option can manufacture this boundary.
class CatalogViewCreationCustody final {
 public:
  CatalogViewCreationCustody(const CatalogViewCreationCustody&) = delete;
  CatalogViewCreationCustody& operator=(const CatalogViewCreationCustody&) = delete;
  const EngineRelationProjectionViewDescriptor& definition() const { return definition_; }
  const std::string& payload() const { return payload_; }
  EngineApiDiagnostic Readmit() const { return readmit_(); }
 private:
  CatalogViewCreationCustody(EngineRelationProjectionViewDescriptor definition,
      std::string payload, std::function<EngineApiDiagnostic()> readmit)
      : definition_(std::move(definition)), payload_(std::move(payload)), readmit_(std::move(readmit)) {}
  friend EngineCreateViewResult EngineCreateView(const EngineCreateViewRequest&);
  const EngineRelationProjectionViewDescriptor definition_;
  const std::string payload_;
  const std::function<EngineApiDiagnostic()> readmit_;
};
EngineCatalogCreateObjectResult EngineCatalogCreateViewWithDefinition(
    const CatalogViewCreationCustody&, const EngineCatalogCreateObjectRequest&);
EngineApiDiagnostic ValidateCatalogViewCreationAdmission(
    const EngineCatalogCreateObjectRequest&, const EngineRelationProjectionViewDescriptor&,
    std::optional<std::uint64_t>* expected_catalog_epoch,
    std::optional<std::vector<std::string>>* expected_schema_frames);
} // namespace scratchbird::engine::internal_api
