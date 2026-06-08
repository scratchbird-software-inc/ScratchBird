// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

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

EngineRequestContext BaseContext(const std::string& path) {
  EngineRequestContext context;
  context.trust_mode = EngineTrustMode::embedded_in_process;
  context.security_context_present = true;
  context.request_id = "domain-enforcement-probe";
  context.database_path = path;
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

bool Commit(const EngineRequestContext& context) {
  EngineCommitTransactionRequest request;
  request.context = context;
  return EngineCommitTransaction(request).ok;
}

EngineDescriptor IntDescriptor() {
  EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = "00000000-0000-7000-8000-000000000201";
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "int32";
  descriptor.encoded_descriptor = "canonical=int32";
  return descriptor;
}

}  // namespace

int main(int argc, char** argv) {
  std::string path;
  bool overwrite = false;
  for (int i = 1; i < argc; ++i) {
    const std::string key = argv[i];
    if (key == "--overwrite") { overwrite = true; }
    else if (key == "--path" && i + 1 < argc) { path = argv[++i]; }
    else { return 2; }
  }
  if (path.empty()) { return 2; }
  if (overwrite) { std::filesystem::remove(path); }
  { std::ofstream bootstrap(path, std::ios::binary | std::ios::app); }

  const auto base = BaseContext(path);
  auto tx = Begin(base);
  auto context = TxContext(base, tx);

  EngineCreateDomainRequest domain;
  domain.context = context;
  domain.localized_names.push_back({"en", "default", "positive_int", "positive_int", true});
  domain.descriptors.push_back(IntDescriptor());
  domain.policy_profile.encoded_profiles.push_back("nullable:false");
  domain.option_envelopes.push_back("default_expression:literal:7");
  domain.option_envelopes.push_back("check_constraint:gt:0");
  const auto domain_result = EngineCreateDomain(domain);

  EngineCreateTableRequest table;
  table.context = context;
  table.table_names.push_back({"en", "default", "domain_table", "domain_table", true});
  EngineColumnDefinition id;
  id.ordinal = 0;
  id.names.push_back({"en", "default", "id", "id", true});
  id.descriptor = IntDescriptor();
  table.table_columns.push_back(id);
  EngineColumnDefinition value;
  value.ordinal = 1;
  value.names.push_back({"en", "default", "value", "value", true});
  value.descriptor.descriptor_uuid = domain_result.primary_object.uuid;
  value.descriptor.descriptor_kind = "domain";
  value.descriptor.canonical_type_name = "positive_int";
  value.descriptor.encoded_descriptor =
      "domain_uuid=" + domain_result.primary_object.uuid.canonical +
      ";base_type=int32;nullable=false";
  table.table_columns.push_back(value);
  const auto table_result = EngineCreateTable(table);

  EngineInsertRowsRequest missing_default;
  missing_default.context = context;
  missing_default.target_table = table_result.table_object;
  EngineRowValue missing_row;
  missing_row.fields.push_back({"id", {IntDescriptor(), "1", false}});
  missing_default.input_rows.push_back(missing_row);
  const auto missing_insert = EngineInsertRows(missing_default);

  EngineInsertRowsRequest invalid;
  invalid.context = context;
  invalid.target_table = table_result.table_object;
  EngineRowValue bad_row;
  bad_row.fields.push_back({"id", {IntDescriptor(), "2", false}});
  bad_row.fields.push_back({"value", {IntDescriptor(), "-1", false}});
  invalid.input_rows.push_back(bad_row);
  const auto invalid_insert = EngineInsertRows(invalid);

  const bool commit_ok = Commit(context);
  const bool ok = domain_result.ok && table_result.ok && missing_insert.ok && !invalid_insert.ok && commit_ok;
  std::cout << "{\n";
  std::cout << "  \"ok\": " << (ok ? "true" : "false") << ",\n";
  std::cout << "  \"default_insert_ok\": " << (missing_insert.ok ? "true" : "false") << ",\n";
  std::cout << "  \"invalid_rejected\": " << (!invalid_insert.ok ? "true" : "false") << "\n";
  std::cout << "}\n";
  return ok ? 0 : 1;
}
