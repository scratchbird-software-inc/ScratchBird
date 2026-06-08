// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/datatype_transport_api.hpp"
#include "catalog/descriptor_api.hpp"
#include "ddl/create_api.hpp"
#include "transaction/transaction_api.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

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
  context.request_id = "datatype-storage-backup-replication-probe";
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

EngineTypedValue ValueFor(const EngineDescriptor& descriptor, std::string value) {
  EngineTypedValue typed;
  typed.descriptor = descriptor;
  typed.encoded_value = std::move(value);
  return typed;
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

bool RoundTrip(const EngineDatatypeTransportRecord& record) {
  const auto encoded = EncodeDatatypeTransportRecord(record);
  if (!encoded.ok || encoded.encoded_envelope.empty()) { return false; }
  const auto decoded = DecodeDatatypeTransportRecord(encoded.encoded_envelope);
  return decoded.ok &&
         decoded.record.transport_scope == record.transport_scope &&
         decoded.record.descriptor.descriptor_uuid.canonical == record.descriptor.descriptor_uuid.canonical &&
         decoded.record.descriptor.descriptor_kind == record.descriptor.descriptor_kind &&
         decoded.record.descriptor.canonical_type_name == record.descriptor.canonical_type_name &&
         decoded.record.descriptor.encoded_descriptor == record.descriptor.encoded_descriptor &&
         decoded.record.value.encoded_value == record.value.encoded_value &&
         decoded.record.value.is_null == record.value.is_null &&
         decoded.record.donor_dialect == record.donor_dialect &&
         decoded.record.donor_label == record.donor_label &&
         decoded.record.donor_label_alias_only == record.donor_label_alias_only &&
         decoded.record.opaque_render_only == record.opaque_render_only;
}

bool ScopeRoundTrips(const EngineDescriptor& descriptor,
                     const EngineTypedValue& value,
                     std::string donor_dialect = {},
                     std::string donor_label = {},
                     bool opaque = false) {
  for (const std::string& scope : {"backup", "restore", "archive", "replication"}) {
    EngineDatatypeTransportRecord record;
    record.transport_scope = scope;
    record.descriptor = descriptor;
    record.value = value;
    record.donor_dialect = donor_dialect;
    record.donor_label = donor_label;
    record.opaque_render_only = opaque;
    if (!RoundTrip(record)) { return false; }
  }
  return true;
}

void PrintBool(const std::string& name, bool value, bool comma) {
  std::cout << "  \"" << name << "\": " << (value ? "true" : "false") << (comma ? "," : "") << "\n";
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!ParseArgs(argc, argv, &args)) {
    std::cerr << "usage: sb_datatype_storage_backup_replication_probe --path PATH --creation-ms MILLIS [--overwrite]\n";
    return 2;
  }
  if (args.overwrite) { std::filesystem::remove(args.path); }
  { std::ofstream bootstrap(args.path, std::ios::binary | std::ios::app); }

  const auto native_descriptor = ScalarDescriptor("int32");
  const bool native_transport = ScopeRoundTrips(native_descriptor, ValueFor(native_descriptor, "42"));

  const bool donor_transport =
      ScopeRoundTrips(native_descriptor, ValueFor(native_descriptor, "42"), "postgresql", "INTEGER");

  const auto opaque_descriptor = ScalarDescriptor("opaque_extension");
  const bool opaque_transport =
      ScopeRoundTrips(opaque_descriptor, ValueFor(opaque_descriptor, "opaque-render-token"), {}, {}, true);

  const auto base = BaseContext(args);
  const auto setup_tx = Begin(base);
  const auto setup_context = TxContext(base, setup_tx);
  const auto create_domain = CreateDomain(setup_context);
  const bool setup_commit = create_domain.ok && Commit(setup_context);
  const auto read_tx = Begin(base);
  const auto read_context = TxContext(base, read_tx);
  const auto lookup_domain = LookupDescriptor(read_context, create_domain.primary_object.uuid.canonical);
  const bool domain_transport = setup_commit && lookup_domain.ok &&
                                ScopeRoundTrips(lookup_domain.descriptor, ValueFor(lookup_domain.descriptor, "7"));
  const bool read_commit = Commit(read_context);

  const bool bad_scope_rejected = !EncodeDatatypeTransportRecord({"unknown", native_descriptor, ValueFor(native_descriptor, "1")}).ok;
  const bool bad_envelope_rejected = !DecodeDatatypeTransportRecord("not-a-transport-envelope").ok;

  const bool ok = native_transport && donor_transport && opaque_transport && domain_transport &&
                  bad_scope_rejected && bad_envelope_rejected && read_commit;

  std::cout << "{\n";
  PrintBool("ok", ok, true);
  PrintBool("native_transport", native_transport, true);
  PrintBool("donor_transport", donor_transport, true);
  PrintBool("opaque_transport", opaque_transport, true);
  PrintBool("domain_transport", domain_transport, true);
  PrintBool("bad_scope_rejected", bad_scope_rejected, true);
  PrintBool("bad_envelope_rejected", bad_envelope_rejected, false);
  std::cout << "}\n";
  return ok ? 0 : 1;
}
