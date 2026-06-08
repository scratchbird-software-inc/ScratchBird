// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace scratchbird::parser::sbsql {

enum class StatementSurfaceKind {
  kGrammarProduction,
  kCanonicalSurface,
};

enum class StatementParserCategory {
  kGeneral,
  kQuery,
  kDml,
  kDdlCatalog,
  kSecurity,
  kTransaction,
  kObservability,
  kRuntimeManagement,
  kStorageManagement,
  kJobsScheduler,
  kArchiveReplication,
  kAcceleration,
  kMultiModel,
  kMigration,
  kClusterPrivate,
};

enum class LifecycleMappingDisposition {
  kScratchBirdLifecycleApi,
  kEmulatedNonFileDiagnostic,
};

struct StatementSurfaceDescriptor {
  std::string_view surface_id;
  std::string_view fixed_uuid_v7;
  std::string_view canonical_name;
  StatementSurfaceKind kind{StatementSurfaceKind::kGrammarProduction};
  StatementParserCategory category{StatementParserCategory::kGeneral};
  std::string_view family;
  std::string_view source_status;
  std::string_view cluster_scope;
  std::string_view owner_lane;
  std::string_view sblr_operation_family;
  std::string_view parser_handler_key;
  std::string_view lowering_descriptor_key;
  std::string_view server_admission_key;
  std::string_view engine_rule_key;
  std::string_view diagnostic_key;
  std::string_view final_acceptance_rule;
  std::string_view ast_node_kind;
  std::string_view statement_form;
  std::string_view binding_contract_key;
  std::string_view admission_contract_key;
  std::string_view behavior_descriptor_key;
  bool top_level_candidate{false};
  bool exact_refusal_required{false};
  bool requires_authority_resolution{false};
};

struct LifecycleMappingDescriptor {
  std::string_view mapping_key;
  std::string_view source_dialect;
  std::string_view command_family;
  LifecycleMappingDisposition disposition{LifecycleMappingDisposition::kScratchBirdLifecycleApi};
  std::string_view operation_id;
  std::string_view sblr_operation;
  std::string_view sblr_operation_family;
  std::string_view engine_api_function;
  std::string_view request_type;
  std::string_view result_type;
  std::string_view result_shape_key;
  std::string_view diagnostic_shape_key;
  std::string_view resource_contract_key;
  std::string_view diagnostic_code;
  std::string_view diagnostic_severity;
  std::string_view diagnostic_message;
  std::string_view authority_domain;
  std::string_view security_authority_family;
  std::string_view required_right;
  std::string_view bound_object_uuid_inputs;
  bool requires_security_context{false};
  bool requires_transaction_context{false};
  bool requires_cluster_authority{false};
  bool produces_file_effects{false};
  bool parser_executes_sql{false};
  bool exact_emulated_diagnostic{false};
};

std::span<const StatementSurfaceDescriptor> BuiltinStatementSurfaceDescriptors();
std::span<const LifecycleMappingDescriptor> BuiltinSbsqlLifecycleMappings();

const StatementSurfaceDescriptor* FindStatementSurfaceById(std::string_view surface_id);
const StatementSurfaceDescriptor* FindStatementSurfaceByName(std::string_view canonical_name);
const LifecycleMappingDescriptor* FindSbsqlLifecycleMappingByOperationId(std::string_view operation_id);
const LifecycleMappingDescriptor* FindSbsqlLifecycleMappingBySblrOperation(std::string_view sblr_operation);
const LifecycleMappingDescriptor* MapSbsqlLifecycleCommand(std::string_view sql_text);

std::optional<StatementSurfaceKind> ParseStatementSurfaceKind(std::string_view kind);
std::optional<StatementParserCategory> ParseStatementParserCategory(std::string_view family);
std::string_view StatementSurfaceKindName(StatementSurfaceKind kind);
std::string_view StatementParserCategoryName(StatementParserCategory category);
std::string_view LifecycleMappingDispositionName(LifecycleMappingDisposition disposition);

} // namespace scratchbird::parser::sbsql
