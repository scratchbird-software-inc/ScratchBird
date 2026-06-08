// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sbsql_v3_ast_catalog.hpp"

#include <iostream>
#include <vector>

int main(){namespace ast=scratchbird::parser::sbsql_v3_ast; std::vector<std::string> errors; auto node=ast::MakeAstCatalogNode("sbsql.query_dml","select.query","sbv3_select_query",{0,42},{{0,6},{7,42}},"SELECT * FROM t"); node.header.parser_package_uuid="018f0000-0000-7000-8000-000000000401"; node.header.parser_package_version="v3-stage2"; node.header.registry_snapshot_uuid="018f0000-0000-7000-8000-000000000402"; node.header.diagnostic_context_id="diag-stage2"; const bool valid=ast::ValidateAstCatalogNode(node,&errors); const std::string json=ast::SerializeAstCatalogNodeToJson(node); const bool ok=valid && node.ast_node=="QueryDmlAst" && node.bound_ast_node=="BoundQueryDml" && node.header.token_spans.size()==2 && !node.raw_command_engine_authority && json.find("QueryDmlAst")!=std::string::npos; std::cout << "{\"ok\":" << (ok?"true":"false") << ",\"ast_node\":\"" << node.ast_node << "\",\"required_field_count\":" << node.required_fields.size() << ",\"errors\":" << errors.size() << "}\n"; return ok?0:1;}
