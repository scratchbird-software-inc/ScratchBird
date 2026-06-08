// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/descriptor_api.hpp"
#include "ddl/create_api.hpp"
#include "dml/insert_api.hpp"
#include "dml/select_api.hpp"
#include "query/expression_api.hpp"
#include "security/grant_api.hpp"
#include "transaction/transaction_api.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace scratchbird::engine::internal_api;

namespace {
struct Args { std::string path; bool overwrite = false; };
bool ParseArgs(int argc, char** argv, Args* args) {
  for (int i = 1; i < argc; ++i) {
    const std::string key = argv[i];
    if (key == "--overwrite") { args->overwrite = true; continue; }
    if (i + 1 >= argc) { return false; }
    const std::string value = argv[++i];
    if (key == "--path") { args->path = value; } else { return false; }
  }
  return !args->path.empty();
}
EngineRequestContext Base(const Args& args, std::vector<std::string> rights = {}) {
  EngineRequestContext context;
  context.trust_mode = EngineTrustMode::embedded_in_process;
  context.security_context_present = true;
  context.database_path = args.path;
  context.request_id = "domain-grant";
  context.principal_uuid.canonical = "00000000-0000-7000-8000-00000000d260";
  for (const auto& right : rights) { context.trace_tags.push_back("right:" + right); }
  return context;
}
EngineRequestContext Tx(EngineRequestContext base, const EngineBeginTransactionResult& tx) { base.local_transaction_id = tx.local_transaction_id; base.transaction_uuid = tx.transaction_uuid; return base; }
EngineBeginTransactionResult Begin(const EngineRequestContext& context) { EngineBeginTransactionRequest request; request.context = context; request.isolation_level = "read_committed"; return EngineBeginTransaction(request); }
bool Commit(const EngineRequestContext& context) { EngineCommitTransactionRequest request; request.context = context; return EngineCommitTransaction(request).ok; }
EngineDescriptor CharacterDescriptor() { EngineDescriptor d; d.descriptor_uuid.canonical = "00000000-0000-7000-8000-00000000d261"; d.descriptor_kind = "scalar"; d.canonical_type_name = "character"; d.encoded_descriptor = "canonical=character"; return d; }
EngineCreateDomainResult CreateGrantDomain(const EngineRequestContext& context) {
  EngineCreateDomainRequest request;
  request.context = context;
  request.localized_names.push_back({"en", "default", "grant_domain", "grant_domain", true});
  request.descriptors.push_back(CharacterDescriptor());
  request.option_envelopes.push_back("mutation_policy:require_right:DOMAIN_USE");
  request.option_envelopes.push_back("cast_policy:require_right:DOMAIN_CAST");
  request.option_envelopes.push_back("visibility_policy:require_right:DOMAIN_READ");
  return EngineCreateDomain(request);
}
EngineGetDescriptorResult Descriptor(const EngineRequestContext& context, const std::string& uuid) { EngineGetDescriptorRequest request; request.context = context; request.target_object.uuid.canonical = uuid; request.target_object.object_kind = "domain"; return EngineGetDescriptor(request); }
EngineColumnDefinition Column(const EngineDescriptor& descriptor) { EngineColumnDefinition c; c.ordinal = 1; c.names.push_back({"en", "default", "payload", "payload", true}); c.descriptor = descriptor; return c; }
EngineRowValue Row(std::string text) { EngineTypedValue v; v.encoded_value = std::move(text); EngineRowValue row; row.fields.push_back({"payload", v}); return row; }
bool HasDiagnosticCode(const EngineApiResult& result, const std::string& code) { for (const auto& d : result.diagnostics) { if (d.code == code) { return true; } } return false; }
bool HasWarningDetail(const EngineApiResult& result, const std::string& detail) { for (const auto& d : result.diagnostics) { if (!d.error && d.detail == detail) { return true; } } return false; }
std::string FieldValue(const EngineRowValue& row, const std::string& field) { for (const auto& [name, value] : row.fields) { if (name == field) { return value.encoded_value; } } return {}; }
void PrintBool(const std::string& name, bool value, bool comma) { std::cout << "  \"" << name << "\": " << (value ? "true" : "false") << (comma ? "," : "") << "\n"; }
}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!ParseArgs(argc, argv, &args)) { std::cerr << "usage: sb_domain_grant_enforcement_probe --path PATH [--overwrite]\n"; return 2; }
  if (args.overwrite) { std::filesystem::remove(args.path); }
  { std::ofstream bootstrap(args.path, std::ios::binary | std::ios::app); }

  auto setup_base = Base(args, {"DOMAIN_USE", "DOMAIN_CAST", "DOMAIN_READ"});
  const auto setup_tx = Begin(setup_base);
  auto setup = Tx(setup_base, setup_tx);
  const auto domain = CreateGrantDomain(setup);
  const auto descriptor = Descriptor(setup, domain.primary_object.uuid.canonical);
  EngineCreateTableRequest table_request; table_request.context = setup; table_request.table_names.push_back({"en", "default", "grant_table", "grant_table", true}); table_request.table_columns.push_back(Column(descriptor.descriptor));
  const auto table = EngineCreateTable(table_request);
  EngineInsertRowsRequest insert; insert.context = setup; insert.target_table = table.table_object; insert.input_rows.push_back(Row("allowed"));
  const auto inserted = EngineInsertRows(insert);
  const bool setup_ok = setup_tx.ok && domain.ok && descriptor.ok && table.ok && inserted.ok && Commit(setup);

  auto denied_base = Base(args, {});
  const auto denied_tx = Begin(denied_base);
  auto denied = Tx(denied_base, denied_tx);
  EngineInsertRowsRequest denied_insert; denied_insert.context = denied; denied_insert.target_table = table.table_object; denied_insert.input_rows.push_back(Row("denied"));
  const auto denied_insert_result = EngineInsertRows(denied_insert);
  const bool use_denied = !denied_insert_result.ok && HasDiagnosticCode(denied_insert_result, "SB_ENGINE_API_INVALID_REQUEST");
  EngineValidateDomainValueRequest denied_cast; denied_cast.context = denied; denied_cast.domain_descriptor = descriptor.descriptor; denied_cast.input_value.encoded_value = "cast"; denied_cast.input_value.descriptor = CharacterDescriptor();
  const auto denied_cast_result = EngineValidateDomainValue(denied_cast);
  const bool cast_denied = !denied_cast_result.ok && HasDiagnosticCode(denied_cast_result, "SB_ENGINE_API_INVALID_REQUEST");
  EngineSelectRowsRequest denied_select; denied_select.context = denied; denied_select.source_object = table.table_object;
  const auto denied_select_result = EngineSelectRows(denied_select);
  const bool read_denied = !denied_select_result.ok && HasDiagnosticCode(denied_select_result, "SB_ENGINE_API_INVALID_REQUEST");
  const bool denied_commit = Commit(denied);

  auto cast_base = Base(args, {"DOMAIN_CAST"});
  const auto cast_tx = Begin(cast_base);
  auto cast_context = Tx(cast_base, cast_tx);
  EngineValidateDomainValueRequest allowed_cast; allowed_cast.context = cast_context; allowed_cast.domain_descriptor = descriptor.descriptor; allowed_cast.input_value.encoded_value = "cast"; allowed_cast.input_value.descriptor = CharacterDescriptor();
  const auto allowed_cast_result = EngineValidateDomainValue(allowed_cast);
  const bool cast_allowed = allowed_cast_result.ok && Commit(cast_context);

  auto read_base = Base(args, {"DOMAIN_READ"});
  const auto read_tx = Begin(read_base);
  auto read_context = Tx(read_base, read_tx);
  EngineSelectRowsRequest allowed_select; allowed_select.context = read_context; allowed_select.source_object = table.table_object;
  const auto allowed_select_result = EngineSelectRows(allowed_select);
  const bool read_allowed = allowed_select_result.ok && allowed_select_result.result_shape.rows.size() == 1 && FieldValue(allowed_select_result.result_shape.rows.front(), "payload") == "allowed" && Commit(read_context);

  auto grant_base = Base(args, {"SEC_GRANT_ADMIN"});
  const auto grant_tx = Begin(grant_base);
  auto grant_context = Tx(grant_base, grant_tx);
  EngineGrantRightRequest grant; grant.context = grant_context; grant.option_envelopes.push_back("right:DOMAIN_UNMASK"); grant.option_envelopes.push_back("group:DEV");
  const auto grant_result = EngineGrantRight(grant);
  const bool unmask_dev_warning = grant_tx.ok && grant_result.ok && HasWarningDetail(grant_result, "DOMAIN_UNMASK:DEV") && Commit(grant_context);

  const bool ok = setup_ok && use_denied && cast_denied && read_denied && denied_commit && cast_allowed && read_allowed && unmask_dev_warning;
  std::cout << "{\n";
  PrintBool("ok", ok, true);
  PrintBool("setup_ok", setup_ok, true);
  PrintBool("use_denied", use_denied, true);
  PrintBool("cast_denied", cast_denied, true);
  PrintBool("read_denied", read_denied, true);
  PrintBool("cast_allowed", cast_allowed, true);
  PrintBool("read_allowed", read_allowed, true);
  PrintBool("unmask_dev_warning", unmask_dev_warning, false);
  std::cout << "}\n";
  return ok ? 0 : 1;
}
