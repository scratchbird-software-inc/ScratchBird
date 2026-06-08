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

EngineRequestContext BaseContext(const Args& args) {
  EngineRequestContext context;
  context.trust_mode = EngineTrustMode::embedded_in_process;
  context.security_context_present = true;
  context.request_id = "wire-driver-datatype-metadata-probe";
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
  request.option_envelopes.push_back("driver_metadata:driver_display_type=positive_int32,donor_label=POSITIVE_INT");
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
    std::cerr << "usage: sb_wire_driver_datatype_metadata_probe --path PATH --creation-ms MILLIS [--overwrite]\n";
    return 2;
  }
  if (args.overwrite) { std::filesystem::remove(args.path); }
  { std::ofstream bootstrap(args.path, std::ios::binary | std::ios::app); }

  const auto native_metadata = RenderWireDriverMetadata(ScalarDescriptor("int32"));
  const bool native_distinguished = native_metadata.native_descriptor && !native_metadata.domain_descriptor &&
                                    native_metadata.canonical_type_name == "int32" &&
                                    native_metadata.driver_display_type == "int32" &&
                                    native_metadata.donor_label.empty();

  const auto donor_descriptor = scratchbird::core::datatypes::ResolveDonorTypeLabelPlaceholder(
      scratchbird::core::datatypes::DonorDialectId::postgresql, "INTEGER");
  EngineDescriptor donor_engine_descriptor = ScalarDescriptor(
      donor_descriptor.ok() ? donor_descriptor.descriptor.stable_name : std::string{});
  const auto donor_metadata = RenderWireDriverMetadata(donor_engine_descriptor, "postgresql", "INTEGER");
  const bool donor_label_distinguished = donor_descriptor.ok() && donor_metadata.canonical_type_name == "int32" &&
                                         donor_metadata.donor_dialect == "postgresql" &&
                                         donor_metadata.donor_label == "INTEGER" &&
                                         donor_metadata.donor_label_alias_only &&
                                         donor_metadata.driver_display_type == "INTEGER";

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
      domain_metadata.driver_metadata_envelope.find("donor_label=POSITIVE_INT") != std::string::npos;
  const bool domain_wire_metadata_ok =
      domain_metadata.wire_metadata_envelope.find("wire_in=int32_text") != std::string::npos;
  const bool domain_distinguished = setup_commit && domain_lookup_ok && domain_kind_ok && domain_uuid_ok &&
                                    domain_base_ok && domain_driver_metadata_ok && domain_wire_metadata_ok;
  const bool read_commit = Commit(read_context);

  const bool ok = native_distinguished && donor_label_distinguished && domain_distinguished &&
                  opaque_distinguished && read_commit;

  std::cout << "{\n";
  PrintBool("ok", ok, true);
  PrintBool("native_distinguished", native_distinguished, true);
  PrintBool("donor_label_alias_only", donor_label_distinguished, true);
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
