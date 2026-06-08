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
#include <vector>

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
EngineRequestContext Base(const Args& args, std::vector<std::string> rights = {}) {
  EngineRequestContext context;
  context.trust_mode = EngineTrustMode::embedded_in_process;
  context.security_context_present = true;
  context.database_path = args.path;
  context.request_id = "domain-method-dispatch";
  context.principal_uuid.canonical = "00000000-0000-7000-8000-00000000d170";
  for (const auto& right : rights) { context.trace_tags.push_back("right:" + right); }
  return context;
}
EngineRequestContext Tx(EngineRequestContext base, const EngineBeginTransactionResult& tx) { base.local_transaction_id = tx.local_transaction_id; base.transaction_uuid = tx.transaction_uuid; return base; }
EngineBeginTransactionResult Begin(const EngineRequestContext& context) { EngineBeginTransactionRequest request; request.context = context; request.isolation_level = "read_committed"; return EngineBeginTransaction(request); }
bool Commit(const EngineRequestContext& context) { EngineCommitTransactionRequest request; request.context = context; return EngineCommitTransaction(request).ok; }
EngineDescriptor CharacterDescriptor() { EngineDescriptor d; d.descriptor_uuid.canonical = "00000000-0000-7000-8000-00000000d171"; d.descriptor_kind = "scalar"; d.canonical_type_name = "character"; d.encoded_descriptor = "canonical=character"; return d; }
EngineCreateDomainResult CreateDomain(const EngineRequestContext& context, const std::string& name, const std::string& method_binding) {
  EngineCreateDomainRequest request;
  request.context = context;
  request.localized_names.push_back({"en", "default", name, name, true});
  request.descriptors.push_back(CharacterDescriptor());
  request.option_envelopes.push_back("method_binding:" + method_binding);
  return EngineCreateDomain(request);
}
EngineGetDescriptorResult Descriptor(const EngineRequestContext& context, const std::string& uuid) { EngineGetDescriptorRequest request; request.context = context; request.target_object.uuid.canonical = uuid; request.target_object.object_kind = "domain"; return EngineGetDescriptor(request); }
EngineTypedValue Input(std::string value) { EngineTypedValue typed; typed.descriptor = CharacterDescriptor(); typed.encoded_value = std::move(value); return typed; }
EngineInvokeDomainMethodResult Invoke(const EngineRequestContext& context, const EngineDescriptor& descriptor, const std::string& method, const std::string& value) {
  EngineInvokeDomainMethodRequest request;
  request.context = context;
  request.domain_descriptor = descriptor;
  request.input_value = Input(value);
  request.method_name = method;
  return EngineInvokeDomainMethod(request);
}
bool HasDiagnosticCode(const EngineApiResult& result, const std::string& code) { for (const auto& diagnostic : result.diagnostics) { if (diagnostic.code == code) { return true; } } return false; }
void PrintBool(const std::string& name, bool value, bool comma) { std::cout << "  \"" << name << "\": " << (value ? "true" : "false") << (comma ? "," : "") << "\n"; }
}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!ParseArgs(argc, argv, &args)) { std::cerr << "usage: sb_domain_method_dispatch_probe --path PATH [--overwrite]\n"; return 2; }
  if (args.overwrite) { std::filesystem::remove(args.path); }
  { std::ofstream bootstrap(args.path, std::ios::binary | std::ios::app); }

  auto setup_base = Base(args, {"DOMAIN_METHOD"});
  const auto setup_tx = Begin(setup_base);
  auto setup = Tx(setup_base, setup_tx);
  const auto upper_domain = CreateDomain(setup, "upper_domain", "builtin:upper;require_right:DOMAIN_METHOD");
  const auto udr_domain = CreateDomain(setup, "udr_domain", "udr:demo_method;require_right:DOMAIN_METHOD");
  const auto upper_descriptor = Descriptor(setup, upper_domain.primary_object.uuid.canonical);
  const auto udr_descriptor = Descriptor(setup, udr_domain.primary_object.uuid.canonical);
  const bool setup_ok = setup_tx.ok && upper_domain.ok && udr_domain.ok && upper_descriptor.ok && udr_descriptor.ok && Commit(setup);

  auto denied_base = Base(args, {});
  const auto denied_tx = Begin(denied_base);
  auto denied = Tx(denied_base, denied_tx);
  const auto denied_result = Invoke(denied, upper_descriptor.descriptor, "upper", "abc");
  const bool right_denied = !denied_result.ok && HasDiagnosticCode(denied_result, "SB_ENGINE_API_INVALID_REQUEST") && Commit(denied);

  auto allowed_base = Base(args, {"DOMAIN_METHOD"});
  const auto allowed_tx = Begin(allowed_base);
  auto allowed = Tx(allowed_base, allowed_tx);
  const auto upper_result = Invoke(allowed, upper_descriptor.descriptor, "upper", "abc");
  const auto missing_result = Invoke(allowed, upper_descriptor.descriptor, "lower", "ABC");
  const auto udr_result = Invoke(allowed, udr_descriptor.descriptor, "demo_method", "abc");
  const bool upper_ok = upper_result.ok && upper_result.value.encoded_value == "ABC";
  const bool missing_rejected = !missing_result.ok && HasDiagnosticCode(missing_result, "SB_ENGINE_API_INVALID_REQUEST");
  const bool udr_rejected = !udr_result.ok && HasDiagnosticCode(udr_result, "SB_ENGINE_API_INVALID_REQUEST");
  const bool allowed_commit = Commit(allowed);

  const bool ok = setup_ok && right_denied && upper_ok && missing_rejected && udr_rejected && allowed_commit;
  std::cout << "{\n";
  PrintBool("ok", ok, true);
  PrintBool("setup_ok", setup_ok, true);
  PrintBool("right_denied", right_denied, true);
  PrintBool("upper_ok", upper_ok, true);
  PrintBool("missing_rejected", missing_rejected, true);
  PrintBool("udr_rejected", udr_rejected, false);
  std::cout << "}\n";
  return ok ? 0 : 1;
}
