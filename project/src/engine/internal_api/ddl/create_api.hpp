// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_DDL_CREATE_API
struct EngineCreateDatabaseRequest : EngineApiRequest {};
struct EngineCreateDatabaseResult : EngineApiResult {};
EngineCreateDatabaseResult EngineCreateDatabase(const EngineCreateDatabaseRequest& request);

struct EngineCreateSchemaRequest : EngineApiRequest {};
struct EngineCreateSchemaResult : EngineApiResult {};
EngineCreateSchemaResult EngineCreateSchema(const EngineCreateSchemaRequest& request);

struct EngineCreateTableRequest : EngineApiRequest {
  EngineObjectReference target_database;
  EngineObjectReference target_schema;
  EngineUuid requested_table_uuid;
  std::vector<EngineLocalizedName> table_names;
  std::vector<EngineColumnDefinition> table_columns;
  std::vector<EngineConstraintDefinition> table_constraints;
  std::vector<EngineIndexDefinition> table_indexes;
  EngineProfileSet table_physical_profile;
  EngineProfileSet table_policy_profile;
  EngineProfileSet table_compatibility_profile;
};
struct EngineCreateTableResult : EngineApiResult {
  EngineObjectReference table_object;
  EngineUuid table_catalog_row_uuid;
  std::vector<EngineObjectReference> created_catalog_records;
  std::vector<EngineObjectReference> created_storage_objects;
  EngineDescriptor effective_table_descriptor;
};
EngineCreateTableResult EngineCreateTable(const EngineCreateTableRequest& request);

struct EngineCreateIndexRequest : EngineApiRequest {};
struct EngineCreateIndexResult : EngineApiResult {};
EngineCreateIndexResult EngineCreateIndex(const EngineCreateIndexRequest& request);

struct EngineCreateIndexTemplateRequest : EngineApiRequest {};
struct EngineCreateIndexTemplateResult : EngineApiResult {};
EngineCreateIndexTemplateResult EngineCreateIndexTemplate(const EngineCreateIndexTemplateRequest& request);

struct EngineCreateStatisticsRequest : EngineApiRequest {
  EngineObjectReference target_table;
  EngineUuid requested_statistics_uuid;
  std::vector<EngineLocalizedName> statistics_names;
  std::vector<std::string> statistics_kinds;
  std::vector<std::string> expression_envelopes;
};
struct EngineCreateStatisticsResult : EngineApiResult {
  EngineBoundObjectIdentity bound_object_identity;
  std::uint64_t metadata_cache_epoch = 0;
};
EngineCreateStatisticsResult EngineCreateStatistics(const EngineCreateStatisticsRequest& request);

struct EngineCreateDomainRequest : EngineApiRequest {};
struct EngineCreateDomainResult : EngineApiResult {};
EngineCreateDomainResult EngineCreateDomain(const EngineCreateDomainRequest& request);

struct EngineCreateSequenceRequest : EngineApiRequest {};
struct EngineCreateSequenceResult : EngineApiResult {};
EngineCreateSequenceResult EngineCreateSequence(const EngineCreateSequenceRequest& request);

struct EngineCreateViewRequest : EngineApiRequest {};
struct EngineCreateViewResult : EngineApiResult {};
EngineCreateViewResult EngineCreateView(const EngineCreateViewRequest& request);

struct EngineCreateSynonymRequest : EngineApiRequest {};
struct EngineCreateSynonymResult : EngineApiResult {
  EngineBoundObjectIdentity bound_object_identity;
  std::uint64_t metadata_cache_epoch = 0;
};
EngineCreateSynonymResult EngineCreateSynonym(const EngineCreateSynonymRequest& request);

struct EngineCreateConstraintRequest : EngineApiRequest {};
struct EngineCreateConstraintResult : EngineApiResult {
  EngineBoundObjectIdentity bound_object_identity;
  std::uint64_t metadata_cache_epoch = 0;
};
EngineCreateConstraintResult EngineCreateConstraint(const EngineCreateConstraintRequest& request);

struct EngineCreateFunctionRequest : EngineApiRequest {};
struct EngineCreateFunctionResult : EngineApiResult {};
EngineCreateFunctionResult EngineCreateFunction(const EngineCreateFunctionRequest& request);

struct EngineCreateProcedureRequest : EngineApiRequest {};
struct EngineCreateProcedureResult : EngineApiResult {};
EngineCreateProcedureResult EngineCreateProcedure(const EngineCreateProcedureRequest& request);

struct EngineCreateTriggerRequest : EngineApiRequest {};
struct EngineCreateTriggerResult : EngineApiResult {};
EngineCreateTriggerResult EngineCreateTrigger(const EngineCreateTriggerRequest& request);

}  // namespace scratchbird::engine::internal_api
