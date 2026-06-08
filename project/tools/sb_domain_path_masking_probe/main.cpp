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
EngineRequestContext Base(const Args& args, bool unmask) {
  EngineRequestContext context;
  context.trust_mode = EngineTrustMode::embedded_in_process;
  context.security_context_present = true;
  context.database_path = args.path;
  context.request_id = unmask ? "domain-path-unmask" : "domain-path-mask";
  context.principal_uuid.canonical = "00000000-0000-7000-8000-00000000d160";
  if (unmask) { context.trace_tags.push_back("right:DOMAIN_UNMASK"); }
  return context;
}
EngineRequestContext Tx(EngineRequestContext base, const EngineBeginTransactionResult& tx) {
  base.local_transaction_id = tx.local_transaction_id;
  base.transaction_uuid = tx.transaction_uuid;
  return base;
}
EngineBeginTransactionResult Begin(const EngineRequestContext& context) {
  EngineBeginTransactionRequest request;
  request.context = context;
  request.isolation_level = "read_committed";
  return EngineBeginTransaction(request);
}
bool Commit(const EngineRequestContext& context) {
  EngineCommitTransactionRequest request;
  request.context = context;
  return EngineCommitTransaction(request).ok;
}
EngineDescriptor CharacterDescriptor() {
  EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = "00000000-0000-7000-8000-00000000d161";
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "character";
  descriptor.encoded_descriptor = "canonical=character";
  return descriptor;
}
EngineCreateDomainResult CreatePathDomain(const EngineRequestContext& context) {
  EngineCreateDomainRequest request;
  request.context = context;
  request.localized_names.push_back({"en", "default", "payload_domain", "payload_domain", true});
  request.descriptors.push_back(CharacterDescriptor());
  request.option_envelopes.push_back("masking_policy:path:secret:last4");
  request.option_envelopes.push_back("element_path:path:secret;path:public");
  return EngineCreateDomain(request);
}
EngineGetDescriptorResult Descriptor(const EngineRequestContext& context, const std::string& uuid) {
  EngineGetDescriptorRequest request;
  request.context = context;
  request.target_object.uuid.canonical = uuid;
  request.target_object.object_kind = "domain";
  return EngineGetDescriptor(request);
}
EngineColumnDefinition Column(const EngineDescriptor& descriptor) {
  EngineColumnDefinition column;
  column.ordinal = 1;
  column.names.push_back({"en", "default", "payload", "payload", true});
  column.descriptor = descriptor;
  return column;
}
std::string FieldValue(const EngineRowValue& row, const std::string& field) {
  for (const auto& [name, value] : row.fields) { if (name == field) { return value.encoded_value; } }
  return {};
}
void PrintBool(const std::string& name, bool value, bool comma) {
  std::cout << "  \"" << name << "\": " << (value ? "true" : "false") << (comma ? "," : "") << "\n";
}
}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!ParseArgs(argc, argv, &args)) { std::cerr << "usage: sb_domain_path_masking_probe --path PATH [--overwrite]\n"; return 2; }
  if (args.overwrite) { std::filesystem::remove(args.path); }
  { std::ofstream bootstrap(args.path, std::ios::binary | std::ios::app); }

  auto setup_base = Base(args, false);
  const auto setup_tx = Begin(setup_base);
  auto setup = Tx(setup_base, setup_tx);
  const auto domain = CreatePathDomain(setup);
  const auto descriptor = Descriptor(setup, domain.primary_object.uuid.canonical);
  EngineCreateTableRequest table_request;
  table_request.context = setup;
  table_request.table_names.push_back({"en", "default", "payloads", "payloads", true});
  table_request.table_columns.push_back(Column(descriptor.descriptor));
  const auto table = EngineCreateTable(table_request);
  EngineRowValue row;
  EngineTypedValue value;
  value.encoded_value = "secret=123456789;public=visible";
  row.fields.push_back({"payload", value});
  EngineInsertRowsRequest insert;
  insert.context = setup;
  insert.target_table = table.table_object;
  insert.input_rows.push_back(row);
  const auto inserted = EngineInsertRows(insert);
  const bool setup_ok = setup_tx.ok && domain.ok && descriptor.ok && table.ok && inserted.ok && Commit(setup);

  auto masked_base = Base(args, false);
  const auto masked_tx = Begin(masked_base);
  auto masked_context = Tx(masked_base, masked_tx);
  EngineSelectRowsRequest masked_select;
  masked_select.context = masked_context;
  masked_select.source_object = table.table_object;
  const auto masked_result = EngineSelectRows(masked_select);
  const bool path_masked = masked_result.ok && masked_result.result_shape.rows.size() == 1 &&
      FieldValue(masked_result.result_shape.rows.front(), "payload") == "secret=*****6789;public=visible";
  const bool masked_commit = Commit(masked_context);

  auto unmask_base = Base(args, true);
  const auto unmask_tx = Begin(unmask_base);
  auto unmask_context = Tx(unmask_base, unmask_tx);
  EngineSelectRowsRequest unmask_select;
  unmask_select.context = unmask_context;
  unmask_select.source_object = table.table_object;
  const auto unmask_result = EngineSelectRows(unmask_select);
  const bool unmasked = unmask_result.ok && unmask_result.result_shape.rows.size() == 1 &&
      FieldValue(unmask_result.result_shape.rows.front(), "payload") == "secret=123456789;public=visible";
  const bool unmask_commit = Commit(unmask_context);

  const bool ok = setup_ok && path_masked && masked_commit && unmasked && unmask_commit;
  std::cout << "{\n";
  PrintBool("ok", ok, true);
  PrintBool("setup_ok", setup_ok, true);
  PrintBool("path_masked", path_masked, true);
  PrintBool("unmasked", unmasked, false);
  std::cout << "}\n";
  return ok ? 0 : 1;
}
