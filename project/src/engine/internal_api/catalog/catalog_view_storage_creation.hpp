// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog/catalog_object_lifecycle.hpp"
#include "catalog/relation_projection_view.hpp"
#include "catalog/global_aggregate_view.hpp"
#include "catalog/relation_descriptor_projection.hpp"
#include "ddl/create_api.hpp"
#include <functional>
#include <optional>
#include <variant>

namespace scratchbird::engine::internal_api {
struct CatalogViewSourceBinding {
  enum class Kind { relation_column, deferred_inventory };
  EngineUuid view_uuid, source_relation_uuid, source_column_uuid;
  std::uint64_t view_generation = 0, source_relation_descriptor_generation = 0;
  Kind kind = Kind::relation_column;
};
// Lexical custody of an actually validated typed definition. Neither the
// proposal DTO nor an arbitrary payload option can manufacture this boundary.
class CatalogViewCreationCustody final {
 public:
  CatalogViewCreationCustody(const CatalogViewCreationCustody&) = delete;
  CatalogViewCreationCustody& operator=(const CatalogViewCreationCustody&) = delete;
  const EngineRelationProjectionViewDescriptor* projection() const {
    return std::get_if<EngineRelationProjectionViewDescriptor>(&definition_);
  }
  const EngineGlobalAggregateViewDescriptor* aggregate() const {
    return std::get_if<EngineGlobalAggregateViewDescriptor>(&definition_);
  }
  const EngineCatalogRelationProjectionViewDescriptor* inventory() const {
    return std::get_if<EngineCatalogRelationProjectionViewDescriptor>(&definition_);
  }
  CatalogViewSourceBinding source() const {
    return std::visit([](const auto& d) {
      if constexpr (requires { d.source_relation_name; }) {
        return CatalogViewSourceBinding{d.view_uuid, {}, {}, 1, 0,
            CatalogViewSourceBinding::Kind::deferred_inventory};
      } else {
      EngineUuid column;
      if constexpr (requires { d.source_column_uuid; }) column = d.source_column_uuid;
      else column = d.outputs.front().source_column_uuid;
      return CatalogViewSourceBinding{d.view_uuid, d.source_relation_uuid, column,
          d.view_descriptor_generation, d.source_relation_descriptor_generation};
      }
    }, definition_);
  }
  const std::string& payload() const { return payload_; }
  EngineApiDiagnostic Readmit() const { return readmit_(); }
 private:
  CatalogViewCreationCustody(EngineRelationProjectionViewDescriptor definition,
      std::string payload, std::function<EngineApiDiagnostic()> readmit)
      : definition_(std::move(definition)), payload_(std::move(payload)), readmit_(std::move(readmit)) {}
  CatalogViewCreationCustody(EngineGlobalAggregateViewDescriptor definition,
      std::string payload, std::function<EngineApiDiagnostic()> readmit)
      : definition_(std::move(definition)), payload_(std::move(payload)), readmit_(std::move(readmit)) {}
  CatalogViewCreationCustody(EngineCatalogRelationProjectionViewDescriptor definition,
      std::string payload, std::function<EngineApiDiagnostic()> readmit)
      : definition_(std::move(definition)), payload_(std::move(payload)), readmit_(std::move(readmit)) {}
  friend EngineCreateViewResult EngineCreateView(const EngineCreateViewRequest&);
  const std::variant<EngineRelationProjectionViewDescriptor, EngineGlobalAggregateViewDescriptor,
      EngineCatalogRelationProjectionViewDescriptor> definition_;
  const std::string payload_;
  const std::function<EngineApiDiagnostic()> readmit_;
};
EngineCatalogCreateObjectResult EngineCatalogCreateViewWithDefinition(
    const CatalogViewCreationCustody&, const EngineCatalogCreateObjectRequest&);
EngineApiDiagnostic ValidateCatalogViewCreationAdmission(
    const EngineCatalogCreateObjectRequest&, const CatalogViewSourceBinding&,
    std::optional<std::uint64_t>* expected_catalog_epoch,
    std::optional<std::vector<std::string>>* expected_schema_frames);
} // namespace scratchbird::engine::internal_api
