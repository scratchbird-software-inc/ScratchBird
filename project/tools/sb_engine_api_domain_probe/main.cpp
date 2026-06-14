// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/descriptor_api.hpp"
#include "ddl/create_api.hpp"
#include "database_lifecycle.hpp"
#include "memory.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

using namespace scratchbird::engine::internal_api;

namespace {

struct Args {
  std::string path;
  std::string seed_pack_root;
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
    else if (key == "--seed-pack-root") { args->seed_pack_root = value; }
    else if (key == "--creation-ms") { args->creation_millis = static_cast<std::uint64_t>(std::stoull(value)); }
    else { return false; }
  }
  return !args->path.empty() && !args->seed_pack_root.empty() && args->creation_millis != 0;
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
  const auto database_uuid =
      scratchbird::core::uuid::GenerateEngineIdentityV7(scratchbird::core::platform::UuidKind::database,
                                                        args.creation_millis + 10);
  const auto principal_uuid =
      scratchbird::core::uuid::GenerateEngineIdentityV7(scratchbird::core::platform::UuidKind::principal,
                                                        args.creation_millis + 12);
  if (database_uuid.ok()) {
    context.database_uuid.canonical = scratchbird::core::uuid::UuidToString(database_uuid.value.value);
  }
  if (principal_uuid.ok()) {
    context.principal_uuid.canonical = scratchbird::core::uuid::UuidToString(principal_uuid.value.value);
  }
  context.session_uuid.canonical = "018f0000-0000-7000-8000-00000000d0de";
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  return context;
}

bool CreateProbeDatabase(const Args& args) {
  if (args.overwrite) {
    std::filesystem::remove(args.path + ".sb.mga_row_versions");
    std::filesystem::remove(args.path + ".sb.mga_relation_metadata");
    std::filesystem::remove(args.path + ".sb.mga_index_entries");
    std::filesystem::remove(args.path + ".sb.mga_large_values");
    std::filesystem::remove(args.path + ".sb.mga_savepoints");
    std::filesystem::remove(args.path + ".sb.mga_relation_descriptors");
  }
  const auto database_uuid =
      scratchbird::core::uuid::GenerateEngineIdentityV7(scratchbird::core::platform::UuidKind::database,
                                                        args.creation_millis + 10);
  const auto filespace_uuid =
      scratchbird::core::uuid::GenerateEngineIdentityV7(scratchbird::core::platform::UuidKind::filespace,
                                                        args.creation_millis + 11);
  if (!database_uuid.ok()) {
    std::cerr << database_uuid.diagnostic.diagnostic_code << ":"
              << database_uuid.diagnostic.message_key << "\n";
    return false;
  }
  if (!filespace_uuid.ok()) {
    std::cerr << filespace_uuid.diagnostic.diagnostic_code << ":"
              << filespace_uuid.diagnostic.message_key << "\n";
    return false;
  }
  scratchbird::storage::database::DatabaseCreateConfig create;
  create.path = args.path;
  create.database_uuid = database_uuid.value;
  create.filespace_uuid = filespace_uuid.value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = args.creation_millis;
  create.resource_seed_pack_root = args.seed_pack_root;
  create.require_resource_seed_pack = true;
  create.allow_overwrite = args.overwrite;
  const auto created = scratchbird::storage::database::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ":"
              << created.diagnostic.message_key << "\n";
    return false;
  }
  return true;
}

EngineRequestContext TxContext(EngineRequestContext base, const EngineBeginTransactionResult& tx) {
  base.local_transaction_id = tx.local_transaction_id;
  base.transaction_uuid = tx.transaction_uuid;
  base.transaction_isolation_level = tx.isolation_level;
  base.snapshot_visible_through_local_transaction_id = tx.snapshot_visible_through_local_transaction_id;
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
    std::cerr << "usage: sb_engine_api_domain_probe --path PATH --seed-pack-root PATH --creation-ms MILLIS [--overwrite]\n";
    return 2;
  }
  auto memory_policy = scratchbird::core::memory::DefaultLocalEngineMemoryPolicy();
  memory_policy.policy_name = "sb_engine_api_domain_probe";
  const auto memory_configured =
      scratchbird::core::memory::ConfigureDefaultMemoryManagerForFixture(
          memory_policy, "sb_engine_api_domain_probe");
  if (!memory_configured.ok()) {
    std::cerr << memory_configured.diagnostic.diagnostic_code << ":"
              << memory_configured.diagnostic.message_key << "\n";
    return 1;
  }
  if (!CreateProbeDatabase(args)) return 1;

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
