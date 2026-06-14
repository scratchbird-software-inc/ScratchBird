// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/descriptor_api.hpp"
#include "catalog/wire_driver_metadata_api.hpp"
#include "datatype_exchange.hpp"
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

EngineRequestContext BaseContext(const Args& args) {
  EngineRequestContext context;
  context.trust_mode = EngineTrustMode::embedded_in_process;
  context.security_context_present = true;
  context.request_id = "wire-driver-datatype-metadata-probe";
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
  context.session_uuid.canonical = "018f0000-0000-7000-8000-00000000d7a7";
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

EngineDescriptor ScalarDescriptor(std::string type_name) {
  EngineDescriptor descriptor;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = std::move(type_name);
  descriptor.encoded_descriptor = "canonical=" + descriptor.canonical_type_name;
  return descriptor;
}

EngineCreateDomainResult CreateDomain(const EngineRequestContext& tx_context) {
  EngineCreateDomainRequest request;
  request.context = tx_context;
  request.localized_names.push_back({"en", "default", "positive_int32", "positive_int32", true});
  request.descriptors.push_back(ScalarDescriptor("int32"));
  request.policy_profile.encoded_profiles.push_back("nullable:false");
  request.option_envelopes.push_back("driver_metadata:driver_display_type=positive_int32,reference_label=POSITIVE_INT");
  request.option_envelopes.push_back("wire_metadata:wire_in=int32_text,wire_out=int32_text");
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
    std::cerr << "usage: sb_wire_driver_datatype_metadata_probe --path PATH --seed-pack-root PATH --creation-ms MILLIS [--overwrite]\n";
    return 2;
  }
  auto memory_policy = scratchbird::core::memory::DefaultLocalEngineMemoryPolicy();
  memory_policy.policy_name = "sb_wire_driver_datatype_metadata_probe";
  const auto memory_configured =
      scratchbird::core::memory::ConfigureDefaultMemoryManagerForFixture(
          memory_policy, "sb_wire_driver_datatype_metadata_probe");
  if (!memory_configured.ok()) {
    std::cerr << memory_configured.diagnostic.diagnostic_code << ":"
              << memory_configured.diagnostic.message_key << "\n";
    return 1;
  }
  if (!CreateProbeDatabase(args)) return 1;

  const auto native_metadata = RenderWireDriverMetadata(ScalarDescriptor("int32"));
  const bool native_distinguished = native_metadata.native_descriptor && !native_metadata.domain_descriptor &&
                                    native_metadata.canonical_type_name == "int32" &&
                                    native_metadata.driver_display_type == "int32" &&
                                    native_metadata.reference_label.empty();

  const auto reference_descriptor = scratchbird::core::datatypes::ResolveReferenceTypeLabelPlaceholder(
      scratchbird::core::datatypes::ReferenceDialectId::postgresql, "INTEGER");
  EngineDescriptor reference_engine_descriptor = ScalarDescriptor(
      reference_descriptor.ok() ? reference_descriptor.descriptor.stable_name : std::string{});
  const auto reference_metadata = RenderWireDriverMetadata(reference_engine_descriptor, "postgresql", "INTEGER");
  const bool reference_label_distinguished = reference_descriptor.ok() && reference_metadata.canonical_type_name == "int32" &&
                                         reference_metadata.compatibility_dialect == "postgresql" &&
                                         reference_metadata.reference_label == "INTEGER" &&
                                         reference_metadata.reference_label_alias_only &&
                                         reference_metadata.driver_display_type == "INTEGER";

  const auto opaque_metadata = RenderWireDriverMetadata(ScalarDescriptor("opaque_extension"));
  const bool opaque_distinguished = opaque_metadata.opaque_render_only && !opaque_metadata.comparison_supported &&
                                    !opaque_metadata.mutation_supported &&
                                    opaque_metadata.driver_display_type == "opaque_extension";

  const auto base = BaseContext(args);
  const auto setup_tx = Begin(base);
  const auto setup_context = TxContext(base, setup_tx);
  const auto create_domain = CreateDomain(setup_context);
  const bool setup_commit = create_domain.ok && Commit(setup_context);
  const auto read_tx = Begin(base);
  const auto read_context = TxContext(base, read_tx);
  const auto lookup_domain = LookupDescriptor(read_context, create_domain.primary_object.uuid.canonical);
  const auto domain_metadata = RenderWireDriverMetadata(lookup_domain.descriptor);
  const bool domain_lookup_ok = lookup_domain.ok;
  const bool domain_kind_ok = domain_metadata.domain_descriptor && !domain_metadata.native_descriptor;
  const bool domain_uuid_ok = domain_metadata.domain_uuid == create_domain.primary_object.uuid.canonical;
  const bool domain_base_ok = domain_metadata.base_canonical_type_name == "int32";
  const bool domain_driver_metadata_ok =
      domain_metadata.driver_metadata_envelope.find("reference_label=POSITIVE_INT") != std::string::npos;
  const bool domain_wire_metadata_ok =
      domain_metadata.wire_metadata_envelope.find("wire_in=int32_text") != std::string::npos;
  const bool domain_distinguished = setup_commit && domain_lookup_ok && domain_kind_ok && domain_uuid_ok &&
                                    domain_base_ok && domain_driver_metadata_ok && domain_wire_metadata_ok;
  const bool read_commit = Commit(read_context);

  const bool ok = native_distinguished && reference_label_distinguished && domain_distinguished &&
                  opaque_distinguished && read_commit;

  std::cout << "{\n";
  PrintBool("ok", ok, true);
  PrintBool("native_distinguished", native_distinguished, true);
  PrintBool("reference_label_alias_only", reference_label_distinguished, true);
  PrintBool("domain_distinguished", domain_distinguished, true);
  PrintBool("domain_lookup_ok", domain_lookup_ok, true);
  PrintBool("domain_kind_ok", domain_kind_ok, true);
  PrintBool("domain_uuid_ok", domain_uuid_ok, true);
  PrintBool("domain_base_ok", domain_base_ok, true);
  PrintBool("domain_driver_metadata_ok", domain_driver_metadata_ok, true);
  PrintBool("domain_wire_metadata_ok", domain_wire_metadata_ok, true);
  PrintBool("opaque_render_only", opaque_distinguished, false);
  std::cout << "}\n";
  return ok ? 0 : 1;
}
