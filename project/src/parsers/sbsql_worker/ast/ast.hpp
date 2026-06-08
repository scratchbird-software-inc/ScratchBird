// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "cst/cst.hpp"

#include <string>
#include <vector>

namespace scratchbird::parser::sbsql {

enum class StatementFamily {
  kUnknown,
  kQuery,
  kInsert,
  kUpdate,
  kDelete,
  kMerge,
  kUpsert,
  kCatalog,
  kShow,
  kSession,
  kTransaction,
  kExecute,
  kCall,
  kValues,
  kSecurity,
  kObservability,
  kRuntimeManagement,
  kStorageManagement,
  kJobsScheduler,
  kArchiveReplication,
  kAcceleration,
  kMultiModel,
  kMigration,
  kBridge,
  kClusterPrivate,
};

struct AstDocument {
  StatementFamily family{StatementFamily::kUnknown};
  std::string registry_family;
  std::string operation_family;
  std::string statement_kind;
  std::string statement_surface_id;
  std::string statement_surface_name;
  std::string statement_parser_category;
  std::string parser_handler_key;
  std::string statement_binding_contract_key;
  std::string statement_admission_contract_key;
  std::string statement_behavior_descriptor_key;
  std::string diagnostic_key;
  std::string source_text;
  std::string canonical_render;
  std::uint64_t source_hash{0};
  std::size_t root_node_index{0};
  std::size_t statement_token_begin{0};
  std::size_t statement_token_end{0};
  bool requires_name_resolution{false};
  bool produces_sblr{false};
  bool exact_refusal_required{false};
  bool requires_cluster_profile{false};
  struct Node {
    std::string kind;
    std::string text;
    SourceRange range;
    std::size_t token_begin{0};
    std::size_t token_end{0};
    bool source_artifact{false};
    std::vector<std::size_t> children;
  };
  std::vector<Node> nodes;
  MessageVectorSet messages;
};

AstDocument BuildAst(const CstDocument& cst);
std::string StatementFamilyName(StatementFamily family);

} // namespace scratchbird::parser::sbsql
