// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "ast/ast.hpp"
#include "common/common.hpp"

#include <string>
#include <vector>

namespace scratchbird::parser::sbsql {

struct BoundStatement {
  bool bound{false};
  std::uint32_t bound_ast_format_version{1};
  std::uint32_t parser_api_major{0};
  std::uint32_t protocol_version{0};
  std::uint64_t catalog_epoch{0};
  std::uint64_t security_policy_epoch{0};
  std::uint64_t descriptor_epoch{0};
  std::string parser_package_uuid;
  std::string parser_package_version;
  std::string parser_build_id;
  std::string command_registry_snapshot_uuid;
  std::string session_uuid;
  std::string connection_uuid;
  std::string database_uuid;
  std::string dialect_profile_uuid;
  std::string registry_family;
  std::string operation_family;
  std::string command_family;
  std::string surface_key;
  std::string sblr_operation_key;
  std::string statement_surface_id;
  std::string statement_surface_name;
  std::string statement_parser_category;
  std::string parser_handler_key;
  std::string binding_contract_key;
  std::string admission_contract_key;
  std::string behavior_descriptor_key;
  std::string diagnostic_key;
  std::string name_resolution_authority_key;
  std::string descriptor_authority_key;
  std::string security_authority_key;
  std::string transaction_authority_key;
  std::string transaction_context;
  std::string result_shape_key;
  std::string diagnostic_shape_key;
  std::string resource_contract_key;
  std::string conformance_case_key;
  std::string trace_key;
  std::string edition_gate_result;
  std::string profile_gate_result;
  std::string granted_scope;
  std::uint64_t statement_hash{0};
  bool requires_name_resolution{false};
  bool requires_descriptor_authority{false};
  bool requires_security_authority{false};
  bool requires_transaction_authority{false};
  bool requires_cluster_profile{false};
  bool exact_refusal_required{false};
  std::vector<std::string> resolved_object_uuids;
  std::vector<std::string> descriptor_refs;
  std::vector<std::string> policy_refs;
  std::vector<std::string> required_rights;
  std::vector<std::string> required_authority_steps;
  MessageVectorSet messages;
};

BoundStatement BindAst(const AstDocument& ast,
                       const CstDocument& cst,
                       const ParserConfig& config,
                       const SessionContext& session,
                       const std::vector<std::string>& resolved_object_uuids = {});

} // namespace scratchbird::parser::sbsql
