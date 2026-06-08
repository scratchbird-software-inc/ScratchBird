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

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>

using namespace scratchbird::engine::internal_api;

namespace {

struct Args {
  std::string path;
  bool overwrite = false;
};

bool ParseArgs(int argc, char** argv, Args* args) {
  for (int i = 1; i < argc; ++i) {
    const std::string key = argv[i];
    if (key == "--overwrite") { args->overwrite = true; continue; }
    if (i + 1 >= argc) { return false; }
    const std::string value = argv[++i];
    if (key == "--path") { args->path = value; }
    else { return false; }
  }
  return !args->path.empty();
}

EngineRequestContext BaseContext(const Args& args, bool security_context_present) {
  EngineRequestContext context;
  context.trust_mode = EngineTrustMode::embedded_in_process;
  context.security_context_present = security_context_present;
  context.request_id = security_context_present ? "domain-read-policy-secure" : "domain-read-policy-untrusted";
  context.database_path = args.path;
  context.principal_uuid.canonical = "00000000-0000-7000-8000-00000000abcd";
  return context;
}

EngineRequestContext TxContext(EngineRequestContext base, const EngineBeginTransactionResult& tx) {
  base.local_transaction_id = tx.local_transaction_id;
  base.transaction_uuid = tx.transaction_uuid;
  return base;
}

EngineBeginTransactionResult Begin(const EngineRequestContext& base) {
  EngineBeginTransactionRequest request;
  request.context = base;
  request.isolation_level = "read_committed";
  return EngineBeginTransaction(request);
}

bool Commit(const EngineRequestContext& tx_context) {
  EngineCommitTransactionRequest request;
  request.context = tx_context;
  return EngineCommitTransaction(request).ok;
}

EngineDescriptor CharacterDescriptor() {
  EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = "00000000-0000-7000-8000-000000000301";
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "character";
  descriptor.encoded_descriptor = "canonical=character;nullable=true";
  return descriptor;
}

EngineCreateDomainResult CreateSecretDomain(const EngineRequestContext& tx_context) {
  EngineCreateDomainRequest request;
  request.context = tx_context;
  request.localized_names.push_back({"en", "default", "masked_secret", "masked_secret", true});
  request.descriptors.push_back(CharacterDescriptor());
  request.option_envelopes.push_back("masking_policy:last4");
  request.option_envelopes.push_back("visibility_policy:require_security_context");
  request.option_envelopes.push_back("encryption_policy:required:key1");
  return EngineCreateDomain(request);
}

EngineGetDescriptorResult LookupDescriptor(const EngineRequestContext& tx_context, const std::string& uuid) {
  EngineGetDescriptorRequest request;
  request.context = tx_context;
  request.target_object.uuid.canonical = uuid;
  request.target_object.object_kind = "domain";
  return EngineGetDescriptor(request);
}

EngineColumnDefinition Column(std::string name, EngineDescriptor descriptor, std::uint32_t ordinal) {
  EngineColumnDefinition column;
  column.ordinal = ordinal;
  column.names.push_back({"en", "default", name, name, true});
  column.descriptor = std::move(descriptor);
  return column;
}

EngineTypedValue Value(std::string value) {
  EngineTypedValue typed;
  typed.encoded_value = std::move(value);
  typed.is_null = false;
  return typed;
}

std::string FieldValue(const EngineRowValue& row, const std::string& field) {
  for (const auto& [name, value] : row.fields) {
    if (name == field) { return value.encoded_value; }
  }
  return {};
}

bool HasDiagnostic(const EngineApiResult& result, const std::string& code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) { return true; }
  }
  return false;
}

void PrintBool(const std::string& name, bool value, bool comma) {
  std::cout << "  \"" << name << "\": " << (value ? "true" : "false") << (comma ? "," : "") << "\n";
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!ParseArgs(argc, argv, &args)) {
    std::cerr << "usage: sb_domain_read_policy_probe --path PATH [--overwrite]\n";
    return 2;
  }
  if (args.overwrite) { std::filesystem::remove(args.path); }
  { std::ofstream bootstrap(args.path, std::ios::binary | std::ios::app); }

  const auto secure_base = BaseContext(args, true);
  const auto setup_tx = Begin(secure_base);
  const auto setup_context = TxContext(secure_base, setup_tx);
  const auto domain = CreateSecretDomain(setup_context);
  const auto descriptor = LookupDescriptor(setup_context, domain.primary_object.uuid.canonical);

  EngineCreateTableRequest create_table;
  create_table.context = setup_context;
  create_table.table_names.push_back({"en", "default", "secrets", "secrets", true});
  create_table.table_columns.push_back(Column("secret", descriptor.descriptor, 1));
  const auto table = EngineCreateTable(create_table);

  EngineRowValue row;
  row.fields.push_back({"secret", Value("123456789")});
  EngineInsertRowsRequest insert;
  insert.context = setup_context;
  insert.target_table = table.table_object;
  insert.input_rows.push_back(row);
  const auto inserted = EngineInsertRows(insert);
  const bool setup_ok = setup_tx.ok && domain.ok && descriptor.ok && table.ok && inserted.ok && Commit(setup_context);

  const auto secure_read_tx = Begin(secure_base);
  const auto secure_read_context = TxContext(secure_base, secure_read_tx);
  EngineSelectRowsRequest secure_select;
  secure_select.context = secure_read_context;
  secure_select.source_object = table.table_object;
  const auto secure_result = EngineSelectRows(secure_select);
  const bool masked_read = secure_result.ok && secure_result.result_shape.rows.size() == 1 &&
                           FieldValue(secure_result.result_shape.rows.front(), "secret") == "*****6789";
  const bool secure_commit = Commit(secure_read_context);

  const auto untrusted_base = BaseContext(args, false);
  const auto denied_tx = Begin(untrusted_base);
  const auto denied_context = TxContext(untrusted_base, denied_tx);
  EngineSelectRowsRequest denied_select;
  denied_select.context = denied_context;
  denied_select.source_object = table.table_object;
  const auto denied_result = EngineSelectRows(denied_select);
  const bool denied_without_security_context = !denied_result.ok && HasDiagnostic(denied_result, "SB_ENGINE_API_INVALID_REQUEST");
  const bool denied_commit = Commit(denied_context);

  const bool ok = setup_ok && masked_read && secure_commit && denied_without_security_context && denied_commit;
  std::cout << "{\n";
  PrintBool("ok", ok, true);
  PrintBool("setup_ok", setup_ok, true);
  PrintBool("masked_read", masked_read, true);
  PrintBool("denied_without_security_context", denied_without_security_context, false);
  std::cout << "}\n";
  return ok ? 0 : 1;
}
