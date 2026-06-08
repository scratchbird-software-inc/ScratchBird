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
  context.request_id = "domain-encryption-key-policy";
  context.principal_uuid.canonical = "00000000-0000-7000-8000-00000000d360";
  for (const auto& right : rights) { context.trace_tags.push_back("right:" + right); }
  return context;
}
EngineRequestContext Tx(EngineRequestContext base, const EngineBeginTransactionResult& tx) { base.local_transaction_id = tx.local_transaction_id; base.transaction_uuid = tx.transaction_uuid; return base; }
EngineBeginTransactionResult Begin(const EngineRequestContext& context) { EngineBeginTransactionRequest request; request.context = context; request.isolation_level = "read_committed"; return EngineBeginTransaction(request); }
bool Commit(const EngineRequestContext& context) { EngineCommitTransactionRequest request; request.context = context; return EngineCommitTransaction(request).ok; }
EngineDescriptor CharacterDescriptor() { EngineDescriptor d; d.descriptor_uuid.canonical = "00000000-0000-7000-8000-00000000d361"; d.descriptor_kind = "scalar"; d.canonical_type_name = "character"; d.encoded_descriptor = "canonical=character"; return d; }
EngineCreateDomainResult CreateEncryptedDomain(const EngineRequestContext& context) {
  EngineCreateDomainRequest request;
  request.context = context;
  request.localized_names.push_back({"en", "default", "encrypted_domain", "encrypted_domain", true});
  request.descriptors.push_back(CharacterDescriptor());
  request.option_envelopes.push_back("encryption_policy:key_policy:key_alpha");
  return EngineCreateDomain(request);
}
EngineGetDescriptorResult Descriptor(const EngineRequestContext& context, const std::string& uuid) { EngineGetDescriptorRequest request; request.context = context; request.target_object.uuid.canonical = uuid; request.target_object.object_kind = "domain"; return EngineGetDescriptor(request); }
EngineColumnDefinition Column(const EngineDescriptor& descriptor) { EngineColumnDefinition c; c.ordinal = 1; c.names.push_back({"en", "default", "secret", "secret", true}); c.descriptor = descriptor; return c; }
EngineRowValue Row(std::string text) { EngineTypedValue v; v.encoded_value = std::move(text); EngineRowValue row; row.fields.push_back({"secret", v}); return row; }
bool HasDiagnosticCode(const EngineApiResult& result, const std::string& code) { for (const auto& d : result.diagnostics) { if (d.code == code) { return true; } } return false; }
std::string FieldValue(const EngineRowValue& row, const std::string& field) { for (const auto& [name, value] : row.fields) { if (name == field) { return value.encoded_value; } } return {}; }
void PrintBool(const std::string& name, bool value, bool comma) { std::cout << "  \"" << name << "\": " << (value ? "true" : "false") << (comma ? "," : "") << "\n"; }
}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!ParseArgs(argc, argv, &args)) { std::cerr << "usage: sb_domain_encryption_key_policy_probe --path PATH [--overwrite]\n"; return 2; }
  if (args.overwrite) { std::filesystem::remove(args.path); }
  { std::ofstream bootstrap(args.path, std::ios::binary | std::ios::app); }

  auto setup_base = Base(args, {"DOMAIN_KEY_USE:key_alpha"});
  const auto setup_tx = Begin(setup_base);
  auto setup = Tx(setup_base, setup_tx);
  const auto domain = CreateEncryptedDomain(setup);
  const auto descriptor = Descriptor(setup, domain.primary_object.uuid.canonical);
  EngineCreateTableRequest table_request; table_request.context = setup; table_request.table_names.push_back({"en", "default", "encrypted_table", "encrypted_table", true}); table_request.table_columns.push_back(Column(descriptor.descriptor));
  const auto table = EngineCreateTable(table_request);
  EngineInsertRowsRequest insert; insert.context = setup; insert.target_table = table.table_object; insert.input_rows.push_back(Row("protected-value"));
  const auto inserted = EngineInsertRows(insert);
  const bool setup_ok = setup_tx.ok && domain.ok && descriptor.ok && table.ok && inserted.ok && Commit(setup);

  auto denied_base = Base(args, {});
  const auto denied_tx = Begin(denied_base);
  auto denied = Tx(denied_base, denied_tx);
  EngineSelectRowsRequest denied_select; denied_select.context = denied; denied_select.source_object = table.table_object;
  const auto denied_result = EngineSelectRows(denied_select);
  const bool key_denied = !denied_result.ok && HasDiagnosticCode(denied_result, "SB_ENGINE_API_INVALID_REQUEST") && Commit(denied);

  auto allowed_base = Base(args, {"DOMAIN_KEY_USE:key_alpha"});
  const auto allowed_tx = Begin(allowed_base);
  auto allowed = Tx(allowed_base, allowed_tx);
  EngineSelectRowsRequest allowed_select; allowed_select.context = allowed; allowed_select.source_object = table.table_object;
  const auto allowed_result = EngineSelectRows(allowed_select);
  const bool key_allowed = allowed_result.ok && allowed_result.result_shape.rows.size() == 1 &&
      FieldValue(allowed_result.result_shape.rows.front(), "secret") == "protected-value" && Commit(allowed);

  auto admin_base = Base(args, {"DOMAIN_KEY_ADMIN:key_alpha"});
  const auto admin_tx = Begin(admin_base);
  auto admin = Tx(admin_base, admin_tx);
  EngineSelectRowsRequest admin_select; admin_select.context = admin; admin_select.source_object = table.table_object;
  const auto admin_result = EngineSelectRows(admin_select);
  const bool admin_allowed = admin_result.ok && admin_result.result_shape.rows.size() == 1 && Commit(admin);

  const bool ok = setup_ok && key_denied && key_allowed && admin_allowed;
  std::cout << "{\n";
  PrintBool("ok", ok, true);
  PrintBool("setup_ok", setup_ok, true);
  PrintBool("key_denied", key_denied, true);
  PrintBool("key_allowed", key_allowed, true);
  PrintBool("admin_allowed", admin_allowed, false);
  std::cout << "}\n";
  return ok ? 0 : 1;
}
