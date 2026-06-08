// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/descriptor_api.hpp"
#include "ddl/create_api.hpp"
#include "transaction/transaction_api.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

using namespace scratchbird::engine::internal_api;

namespace {

struct Args {
  std::string path;
  std::uint64_t creation_millis = 0;
  bool overwrite = false;
};

bool ParseArgs(int argc, char** argv, Args* args) {
  for (int i = 1; i < argc; ++i) {
    const std::string key = argv[i];
    if (key == "--overwrite") { args->overwrite = true; continue; }
    if (i + 1 >= argc) { return false; }
    const std::string value = argv[++i];
    if (key == "--path") { args->path = value; }
    else if (key == "--creation-ms") { args->creation_millis = static_cast<std::uint64_t>(std::stoull(value)); }
    else { return false; }
  }
  return !args->path.empty() && args->creation_millis != 0;
}

bool HasDiagnostic(const EngineApiResult& result, const std::string& code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) { return true; }
  }
  return false;
}

bool HasEvidence(const EngineApiResult& result, const std::string& kind) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind) { return true; }
  }
  return false;
}

EngineRequestContext BaseContext(const Args& args) {
  EngineRequestContext context;
  context.trust_mode = EngineTrustMode::embedded_in_process;
  context.security_context_present = true;
  context.request_id = "engine-api-domain-probe";
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

bool Commit(const EngineRequestContext& tx_context) {
  EngineCommitTransactionRequest request;
  request.context = tx_context;
  return EngineCommitTransaction(request).ok;
}

bool Rollback(const EngineRequestContext& tx_context) {
  EngineRollbackTransactionRequest request;
  request.context = tx_context;
  return EngineRollbackTransaction(request).ok;
}

EngineDescriptor BaseIntDescriptor() {
  EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = "00000000-0000-7000-8000-000000000001";
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "int32";
  descriptor.encoded_descriptor = "canonical=int32;nullable=true";
  return descriptor;
}

EngineCreateDomainResult CreateDomain(const EngineRequestContext& tx_context, std::string name) {
  EngineCreateDomainRequest request;
  request.context = tx_context;
  request.localized_names.push_back({"en", "default", name, name, true});
  request.descriptors.push_back(BaseIntDescriptor());
  request.policy_profile.encoded_profiles.push_back("nullable:false");
  return EngineCreateDomain(request);
}

EngineGetDescriptorResult LookupDescriptor(const EngineRequestContext& tx_context, const std::string& uuid) {
  EngineGetDescriptorRequest request;
  request.context = tx_context;
  request.target_object.uuid.canonical = uuid;
  request.target_object.object_kind = "domain";
  return EngineGetDescriptor(request);
}

void PrintBool(const std::string& name, bool value, bool comma) {
  std::cout << "  \"" << name << "\": " << (value ? "true" : "false") << (comma ? "," : "") << "\n";
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!ParseArgs(argc, argv, &args)) {
    std::cerr << "usage: sb_engine_api_domain_probe --path PATH --creation-ms MILLIS [--overwrite]\n";
    return 2;
  }
  if (args.overwrite) { std::filesystem::remove(args.path); }
  { std::ofstream bootstrap(args.path, std::ios::binary | std::ios::app); }

  const auto base = BaseContext(args);
  const auto setup_tx = Begin(base);
  const auto setup_context = TxContext(base, setup_tx);
  const auto create_result = CreateDomain(setup_context, "positive_int32");
  const std::string domain_uuid = create_result.primary_object.uuid.canonical;
  const auto lookup_in_tx = LookupDescriptor(setup_context, domain_uuid);
  const bool create_and_lookup_in_tx = create_result.ok && lookup_in_tx.ok && lookup_in_tx.descriptor.descriptor_kind == "domain" &&
                                       HasEvidence(create_result, "domain_event") && HasEvidence(lookup_in_tx, "domain_descriptor_lookup");
  const bool setup_commit = Commit(setup_context);

  const auto read_tx = Begin(base);
  const auto read_context = TxContext(base, read_tx);
  const auto lookup_after_reopen = LookupDescriptor(read_context, domain_uuid);
  const bool reopen_lookup = lookup_after_reopen.ok && lookup_after_reopen.descriptor.encoded_descriptor.find("nullable=false") != std::string::npos;
  const bool read_commit = Commit(read_context);

  const auto rollback_tx = Begin(base);
  const auto rollback_context = TxContext(base, rollback_tx);
  const auto rollback_domain = CreateDomain(rollback_context, "rolled_back_domain");
  const std::string rollback_uuid = rollback_domain.primary_object.uuid.canonical;
  const bool rollback_visible_in_tx = rollback_domain.ok && LookupDescriptor(rollback_context, rollback_uuid).ok;
  const bool rollback_done = Rollback(rollback_context);
  const auto rollback_read_tx = Begin(base);
  const auto rollback_read_context = TxContext(base, rollback_read_tx);
  const auto rollback_lookup = LookupDescriptor(rollback_read_context, rollback_uuid);
  const bool rollback_hidden = !rollback_lookup.ok && HasDiagnostic(rollback_lookup, "SB_ENGINE_API_INVALID_REQUEST");
  const bool rollback_read_commit = Commit(rollback_read_context);

  const auto unsupported_tx = Begin(base);
  const auto unsupported_context = TxContext(base, unsupported_tx);
  EngineCreateDomainRequest unsupported;
  unsupported.context = unsupported_context;
  unsupported.descriptors.push_back(BaseIntDescriptor());
  unsupported.localized_names.push_back({"en", "default", "unsupported_domain", "unsupported_domain", true});
  EngineConstraintDefinition constraint;
  constraint.constraint_kind = "check";
  constraint.canonical_constraint_envelope = "value > 0";
  unsupported.constraints.push_back(constraint);
  const auto unsupported_result = EngineCreateDomain(unsupported);
  const bool unsupported_rejected = !unsupported_result.ok && HasDiagnostic(unsupported_result, "SB_ENGINE_API_UNSUPPORTED_PROFILE");
  const bool unsupported_rollback = Rollback(unsupported_context);

  const bool ok = create_and_lookup_in_tx && setup_commit && reopen_lookup && read_commit && rollback_visible_in_tx &&
                  rollback_done && rollback_hidden && rollback_read_commit && unsupported_rejected && unsupported_rollback;

  std::cout << "{\n";
  PrintBool("ok", ok, true);
  PrintBool("create_and_lookup_in_tx", create_and_lookup_in_tx, true);
  PrintBool("reopen_lookup", reopen_lookup, true);
  PrintBool("rollback_hidden", rollback_hidden, true);
  PrintBool("unsupported_rejected", unsupported_rejected, false);
  std::cout << "}\n";
  return ok ? 0 : 1;
}
