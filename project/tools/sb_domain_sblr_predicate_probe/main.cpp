// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/descriptor_api.hpp"
#include "ddl/create_api.hpp"
#include "query/expression_api.hpp"
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
EngineRequestContext Base(const Args& args) { EngineRequestContext c; c.trust_mode = EngineTrustMode::embedded_in_process; c.security_context_present = true; c.database_path = args.path; c.request_id = "domain-sblr-predicate"; c.principal_uuid.canonical = "00000000-0000-7000-8000-00000000d270"; return c; }
EngineRequestContext Tx(EngineRequestContext base, const EngineBeginTransactionResult& tx) { base.local_transaction_id = tx.local_transaction_id; base.transaction_uuid = tx.transaction_uuid; return base; }
EngineBeginTransactionResult Begin(const EngineRequestContext& context) { EngineBeginTransactionRequest request; request.context = context; request.isolation_level = "read_committed"; return EngineBeginTransaction(request); }
bool Commit(const EngineRequestContext& context) { EngineCommitTransactionRequest request; request.context = context; return EngineCommitTransaction(request).ok; }
EngineDescriptor CharacterDescriptor() { EngineDescriptor d; d.descriptor_uuid.canonical = "00000000-0000-7000-8000-00000000d271"; d.descriptor_kind = "scalar"; d.canonical_type_name = "character"; d.encoded_descriptor = "canonical=character"; return d; }
EngineCreateDomainResult CreateDomain(const EngineRequestContext& context, const std::string& name, const std::string& predicate) {
  EngineCreateDomainRequest request;
  request.context = context;
  request.localized_names.push_back({"en", "default", name, name, true});
  request.descriptors.push_back(CharacterDescriptor());
  request.option_envelopes.push_back("check_constraint:" + predicate);
  return EngineCreateDomain(request);
}
EngineGetDescriptorResult Descriptor(const EngineRequestContext& context, const std::string& uuid) { EngineGetDescriptorRequest request; request.context = context; request.target_object.uuid.canonical = uuid; request.target_object.object_kind = "domain"; return EngineGetDescriptor(request); }
EngineValidateDomainValueResult Validate(const EngineRequestContext& context, const EngineDescriptor& descriptor, const std::string& value) {
  EngineValidateDomainValueRequest request;
  request.context = context;
  request.domain_descriptor = descriptor;
  request.input_value.descriptor = CharacterDescriptor();
  request.input_value.encoded_value = value;
  return EngineValidateDomainValue(request);
}
bool HasDiagnosticCode(const EngineApiResult& result, const std::string& code) { for (const auto& diagnostic : result.diagnostics) { if (diagnostic.code == code) { return true; } } return false; }
void PrintBool(const std::string& name, bool value, bool comma) { std::cout << "  \"" << name << "\": " << (value ? "true" : "false") << (comma ? "," : "") << "\n"; }
}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!ParseArgs(argc, argv, &args)) { std::cerr << "usage: sb_domain_sblr_predicate_probe --path PATH [--overwrite]\n"; return 2; }
  if (args.overwrite) { std::filesystem::remove(args.path); }
  { std::ofstream bootstrap(args.path, std::ios::binary | std::ios::app); }

  const auto base = Base(args);
  const auto setup_tx = Begin(base);
  auto setup = Tx(base, setup_tx);
  const auto length_domain = CreateDomain(setup, "sblr_length_domain", "sblr_predicate:length_gte:3");
  const auto unsupported_domain = CreateDomain(setup, "sblr_unsupported_domain", "sblr_predicate:unsupported:alpha");
  const auto length_descriptor = Descriptor(setup, length_domain.primary_object.uuid.canonical);
  const auto unsupported_descriptor = Descriptor(setup, unsupported_domain.primary_object.uuid.canonical);
  const bool setup_ok = setup_tx.ok && length_domain.ok && unsupported_domain.ok && length_descriptor.ok && unsupported_descriptor.ok && Commit(setup);

  const auto validate_tx = Begin(base);
  auto validate_context = Tx(base, validate_tx);
  const auto pass_result = Validate(validate_context, length_descriptor.descriptor, "abcd");
  const auto fail_result = Validate(validate_context, length_descriptor.descriptor, "ab");
  const auto unsupported_result = Validate(validate_context, unsupported_descriptor.descriptor, "alpha");
  const bool pass_ok = pass_result.ok;
  const bool short_rejected = !fail_result.ok && HasDiagnosticCode(fail_result, "SB_ENGINE_API_INVALID_REQUEST");
  const bool unsupported_rejected = !unsupported_result.ok && HasDiagnosticCode(unsupported_result, "SB_ENGINE_API_INVALID_REQUEST");
  const bool validate_commit = Commit(validate_context);

  const bool ok = setup_ok && pass_ok && short_rejected && unsupported_rejected && validate_commit;
  std::cout << "{\n";
  PrintBool("ok", ok, true);
  PrintBool("setup_ok", setup_ok, true);
  PrintBool("pass_ok", pass_ok, true);
  PrintBool("short_rejected", short_rejected, true);
  PrintBool("unsupported_rejected", unsupported_rejected, false);
  std::cout << "}\n";
  return ok ? 0 : 1;
}
