// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "nosql/document_api.hpp"
#include "nosql/key_value_api.hpp"
#include "transaction/transaction_api.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

using namespace scratchbird::engine::internal_api;

namespace {

struct Args {
  std::string path;
  bool overwrite = false;
};

bool ParseArgs(int argc, char** argv, Args* args) {
  for (int i = 1; i < argc; ++i) {
    const std::string key = argv[i];
    if (key == "--overwrite") {
      args->overwrite = true;
      continue;
    }
    if (i + 1 >= argc) { return false; }
    const std::string value = argv[++i];
    if (key == "--path") {
      args->path = value;
    } else {
      return false;
    }
  }
  return !args->path.empty();
}

EngineRequestContext BaseContext(const Args& args) {
  EngineRequestContext context;
  context.trust_mode = EngineTrustMode::embedded_in_process;
  context.security_context_present = true;
  context.request_id = "sbsql-v3-nosql-document-kv-probe";
  context.database_path = args.path;
  context.database_uuid.canonical = "00000000-0000-7000-8000-000000001101";
  context.session_uuid.canonical = "00000000-0000-7000-8000-000000001102";
  return context;
}

EngineRequestContext BeginContext(const Args& args) {
  auto context = BaseContext(args);
  EngineBeginTransactionRequest begin;
  begin.context = context;
  begin.isolation_level = "read_committed";
  const auto tx = EngineBeginTransaction(begin);
  if (tx.ok) {
    context.local_transaction_id = tx.local_transaction_id;
    context.transaction_uuid = tx.transaction_uuid;
  }
  return context;
}

bool CommitContext(const EngineRequestContext& context) {
  EngineCommitTransactionRequest commit;
  commit.context = context;
  return EngineCommitTransaction(commit).ok;
}

bool HasEvidence(const EngineApiResult& result, const std::string& kind, const std::string& id = {}) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && (id.empty() || evidence.evidence_id == id)) { return true; }
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
    std::cerr << "usage: sb_sbsql_v3_nosql_document_kv_probe --path PATH [--overwrite]\n";
    return 2;
  }
  if (args.overwrite) { std::filesystem::remove(args.path); }
  { std::ofstream bootstrap(args.path, std::ios::binary | std::ios::app); }

  auto write_context = BeginContext(args);
  const bool tx_started = write_context.local_transaction_id != 0;

  EngineDocumentInsertRequest insert_doc;
  insert_doc.context = write_context;
  insert_doc.target_object.uuid.canonical = "00000000-0000-7000-8000-000000001111";
  insert_doc.target_object.object_kind = "document_collection";
  insert_doc.localized_names.push_back({"en", "default", "/native/docs", "customer_doc", true});
  insert_doc.option_envelopes.push_back("document_json:{\"customer\":1}");
  const auto insert_doc_result = EngineDocumentInsert(insert_doc);

  EngineKeyValuePutRequest put_key;
  put_key.context = write_context;
  put_key.target_object.uuid.canonical = "00000000-0000-7000-8000-000000001112";
  put_key.target_object.object_kind = "key_space";
  put_key.localized_names.push_back({"en", "default", "/native/kv", "customer:1", true});
  put_key.option_envelopes.push_back("value:{\"tier\":\"gold\"}");
  const auto put_key_result = EngineKeyValuePut(put_key);

  const bool committed = tx_started && CommitContext(write_context);

  auto read_context = BeginContext(args);
  EngineDocumentFindRequest find_doc;
  find_doc.context = read_context;
  const auto find_doc_result = EngineDocumentFind(find_doc);

  EngineKeyValueGetRequest get_key;
  get_key.context = read_context;
  get_key.target_object.uuid.canonical = "00000000-0000-7000-8000-000000001112";
  const auto get_key_result = EngineKeyValueGet(get_key);
  const bool read_committed = CommitContext(read_context);

  EngineDocumentInsertRequest cluster_doc;
  cluster_doc.context = BaseContext(args);
  cluster_doc.option_envelopes.push_back("cluster_route:shared_document_collection");
  const auto cluster_doc_result = EngineDocumentInsert(cluster_doc);

  const bool document_ok = insert_doc_result.ok &&
                           HasEvidence(insert_doc_result, "nosql_surface", "document") &&
                           find_doc_result.ok &&
                           !find_doc_result.result_shape.rows.empty() &&
                           HasEvidence(find_doc_result, "nosql_behavior", "local_descriptor_scan");
  const bool key_value_ok = put_key_result.ok &&
                            HasEvidence(put_key_result, "nosql_surface", "key_value") &&
                            get_key_result.ok &&
                            !get_key_result.result_shape.rows.empty() &&
                            HasEvidence(get_key_result, "nosql_behavior", "local_key_lookup");
  const bool cluster_denied = !cluster_doc_result.ok && cluster_doc_result.cluster_authority_required;
  const bool ok = tx_started && committed && read_committed && document_ok && key_value_ok && cluster_denied;

  std::cout << "{\n";
  PrintBool("ok", ok, true);
  PrintBool("document_ok", document_ok, true);
  PrintBool("key_value_ok", key_value_ok, true);
  PrintBool("cluster_denied", cluster_denied, false);
  std::cout << "}\n";
  return ok ? 0 : 1;
}

