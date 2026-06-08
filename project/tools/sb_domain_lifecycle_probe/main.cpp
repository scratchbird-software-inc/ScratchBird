// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/descriptor_api.hpp"
#include "ddl/alter_api.hpp"
#include "ddl/create_api.hpp"
#include "ddl/drop_api.hpp"
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
    if (key == "--path" && i + 1 < argc) {
      args->path = argv[++i];
      continue;
    }
    return false;
  }
  return !args->path.empty();
}

EngineRequestContext BaseContext(const Args& args) {
  EngineRequestContext context;
  context.trust_mode = EngineTrustMode::embedded_in_process;
  context.security_context_present = true;
  context.request_id = "domain-lifecycle-probe";
  context.database_path = args.path;
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
  descriptor.descriptor_uuid.canonical = "00000000-0000-7000-8000-000000000101";
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "int32";
  descriptor.encoded_descriptor = "canonical=int32";
  return descriptor;
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!ParseArgs(argc, argv, &args)) {
    std::cerr << "usage: sb_domain_lifecycle_probe --path PATH [--overwrite]\n";
    return 2;
  }
  if (args.overwrite) { std::filesystem::remove(args.path); }
  { std::ofstream bootstrap(args.path, std::ios::binary | std::ios::app); }

  const auto base = BaseContext(args);
  auto tx = Begin(base);
  auto context = TxContext(base, tx);

  EngineCreateDomainRequest create;
  create.context = context;
  create.target_object.uuid.canonical = "018f7f8f-7c00-7000-8000-000000001001";
  create.localized_names.push_back({"en", "default", "positive_int", "positive_int", true});
  create.descriptors.push_back(IntDescriptor());
  create.policy_profile.encoded_profiles.push_back("nullable:false");
  create.option_envelopes.push_back("default_expression:literal:1");
  create.option_envelopes.push_back("check_constraint:gt:0");
  const auto created = EngineCreateDomain(create);
  const bool create_ok = created.ok;

  EngineGetDescriptorRequest lookup;
  lookup.context = context;
  lookup.target_object.uuid.canonical = create.target_object.uuid.canonical;
  lookup.target_object.object_kind = "domain";
  const auto descriptor = EngineGetDescriptor(lookup);
  const bool lookup_ok = descriptor.ok && descriptor.descriptor.descriptor_kind == "domain";

  EngineAlterObjectRequest alter;
  alter.context = context;
  alter.target_object = lookup.target_object;
  alter.option_envelopes.push_back("default_expression:literal:2");
  const auto altered = EngineAlterObject(alter);
  const bool alter_ok = altered.ok;
  const bool commit_ok = Commit(context);

  auto drop_tx = Begin(base);
  auto drop_context = TxContext(base, drop_tx);
  EngineDropObjectRequest drop;
  drop.context = drop_context;
  drop.target_object = lookup.target_object;
  const auto dropped = EngineDropObject(drop);
  const bool drop_ok = dropped.ok && Commit(drop_context);

  auto read_tx = Begin(base);
  auto read_context = TxContext(base, read_tx);
  lookup.context = read_context;
  const auto after_drop = EngineGetDescriptor(lookup);
  const bool drop_hidden = !after_drop.ok;
  const bool read_commit = Commit(read_context);
  const bool ok = create_ok && lookup_ok && alter_ok && commit_ok && drop_ok && drop_hidden && read_commit;
  std::cout << "{\n";
  std::cout << "  \"ok\": " << (ok ? "true" : "false") << ",\n";
  std::cout << "  \"create_ok\": " << (create_ok ? "true" : "false") << ",\n";
  std::cout << "  \"lookup_ok\": " << (lookup_ok ? "true" : "false") << ",\n";
  std::cout << "  \"alter_ok\": " << (alter_ok ? "true" : "false") << ",\n";
  std::cout << "  \"drop_hidden\": " << (drop_hidden ? "true" : "false") << "\n";
  std::cout << "}\n";
  return ok ? 0 : 1;
}
