// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sb_udr_builtin_packages.hpp"
#include "sb_udr_runtime.hpp"

#ifdef SB_UDR_BUILTIN_HAS_ENGINE_OUTBOX
#include "cloud/external_effect_outbox.hpp"
#endif

#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <string_view>

namespace {

namespace builtin = scratchbird::udr::builtin_packages;
namespace runtime = scratchbird::udr::runtime;

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

void RequireRuntimeOk(const runtime::UdrStatus& status,
                      std::string_view message) {
  if (!status.ok) {
    std::cerr << status.diagnostic_code << ':' << status.detail << '\n';
    Require(false, message);
  }
}

bool IsVerticalLike(const builtin::BuiltinUdrPackageSpec& spec) {
  return Contains(spec.capability_role, "vertical.") ||
         spec.category == "scientific" ||
         spec.category == "numeric" ||
         spec.category == "ml_ai" ||
         spec.category == "data_format" ||
         spec.category == "data_processing" ||
         spec.category == "governance" ||
         spec.category == "business_logic" ||
         spec.category == "procedural_runtime" ||
         spec.family_id == "mssql_tds_bridge" ||
         spec.family_id == "odbc_datasource_connector";
}

std::string TrustedContext(const builtin::BuiltinUdrPackageSpec& spec) {
  std::string context =
      "engine_context=trusted;sblr_authorized_invocation=true;"
      "user_uuid=019f7100-0000-7000-8000-00000000aa01;"
      "request_uuid=019f7100-0000-7000-8000-00000000aa02;";
  if (spec.cluster_scoped) {
    context += "cluster_provider_authority=true;";
  }
  return context;
}

std::string FileProviderContext(const builtin::BuiltinUdrPackageSpec& spec,
                                std::string_view pushdown,
                                std::string_view max_output_rows,
                                std::string_view source_path = {}) {
  std::string context = TrustedContext(spec);
  context +=
      "credential_ref=credential:019f7100-0000-7000-8000-00000000cc01;"
      "path_policy=read_only_fixture_root;"
      "path_root_uuid=019f7100-0000-7000-8000-00000000cc02;"
      "schema=id:int,name:text;"
      "stream_chunk_bytes=64;";
  if (!pushdown.empty()) {
    context += "pushdown=";
    context += pushdown;
    context += ';';
  }
  if (!max_output_rows.empty()) {
    context += "max_output_rows=";
    context += max_output_rows;
    context += ';';
  }
  if (!source_path.empty()) {
    context += "path_root=/tmp;";
    context += "source_path=";
    context += source_path;
    context += ';';
  }
  return context;
}

std::string FileProviderInlineStream(const builtin::BuiltinUdrPackageSpec& spec) {
  if (spec.family_id == "tsv_file_provider") {
    return "id\tname\n1\tAlpha\n2\tBeta\n3\tGamma\n";
  }
  return "id,name\n1,Alpha\n2,Beta\n3,Gamma\n";
}

std::string SideEffectContext(const builtin::BuiltinUdrPackageSpec& spec,
                              bool committed = true) {
  std::string context = TrustedContext(spec);
  context +=
      "mga_evidence=true;"
      "idempotency_key=udr-side-effect-idempotency-1;"
      "transaction_uuid=019f7100-0000-7000-8000-00000000dd01;"
      "local_transaction_id=77;"
      "transaction_inventory_generation=12;"
      "commit_evidence_hash=mga-commit-proof-77;"
      "finality_mode=single_node_mga;"
      "provider_profile_uuid=provider:udr-side-effect-test;"
      "redaction_policy_uuid=redaction:udr-side-effect-test;"
      "source_object_ref=redacted-object:udr-test;"
      "effect_class=udr_test_side_effect;"
      "now_epoch_ms=1000;"
      "max_pending_records=8;";
  if (committed) {
    context += "mga_commit_visible=true;durable_commit_evidence=true;";
  }
  return context;
}

std::string SecurityDefinerContext(const builtin::BuiltinUdrPackageSpec& spec) {
  return TrustedContext(spec) +
      "caller_principal_uuid=019f7100-0000-7000-8000-00000000ee01;"
      "effective_principal_uuid=019f7100-0000-7000-8000-00000000ee02;"
      "role_chain_proof=role-chain-proof-hash;"
      "group_chain_proof=group-chain-proof-hash;"
      "policy_epoch=44;"
      "security_epoch=45;"
      "masking_epoch=46;"
      "security_definer_authorized=true;";
}

void VerifyDescriptorShape(const builtin::BuiltinUdrPackageSpec& spec,
                           const runtime::UdrPackageDescriptor& descriptor) {
  Require(descriptor.package_uuid == spec.package_uuid,
          "builtin descriptor package UUID mismatch");
  Require(descriptor.package_name == spec.package_name,
          "builtin descriptor package name mismatch");
  Require(descriptor.abi_version == "sb_udr_v1",
          "builtin descriptor ABI mismatch");
  Require(descriptor.runtime_language == "cpp",
          "builtin descriptor runtime language must be cpp");
  Require(descriptor.trusted_cpp,
          "builtin descriptor must be trusted C++");
  Require(!descriptor.source_revision.empty(),
          "builtin descriptor source revision required");
  Require(!descriptor.binary_hash.empty(),
          "builtin descriptor binary hash required");
  Require(!descriptor.signature_policy.empty(),
          "builtin descriptor signature policy required");
  Require(descriptor.capability_role == spec.capability_role,
          "builtin descriptor capability role mismatch");
  Require(descriptor.entrypoints.size() == 3,
          "builtin descriptor entrypoint count mismatch");
}

void VerifyRuntimeCallbacks(const builtin::BuiltinUdrPackageSpec& spec) {
  auto descriptor = builtin::BuiltinUdrPackageDescriptor(spec);
  VerifyDescriptorShape(spec, descriptor);
  RequireRuntimeOk(runtime::RegisterPackage(descriptor),
                   "builtin package registration failed");
  RequireRuntimeOk(runtime::LoadPackage(spec.package_uuid),
                   "builtin package load failed");

  const auto state = runtime::GetPackageState(spec.package_uuid);
  Require(state.has_value(), "builtin package state missing after load");
  Require(state->loaded, "builtin package state not loaded");
  Require(state->abi_version == "sb_udr_v1",
          "builtin runtime state ABI mismatch");

  const runtime::UdrCallInput describe{
      std::string(spec.package_uuid), "describe_capabilities", {}, TrustedContext(spec)};
  const auto described = runtime::InvokePackage(describe);
  Require(described.ok, "builtin package describe_capabilities failed");
  Require(Contains(described.payload, spec.family_id),
          "builtin package describe payload missing family id");
  Require(Contains(described.payload, "sblr_uuid_mga_security"),
          "builtin package describe payload missing authority boundary");
  Require(Contains(described.payload, "\"parser_execution_authority\":false"),
          "builtin package describe payload allowed parser execution authority");

  const runtime::UdrCallInput missing_context{
      std::string(spec.package_uuid), "validate_admission", {}, {}};
  const auto missing_context_result = runtime::InvokePackage(missing_context);
  Require(!missing_context_result.ok,
          "builtin package validate_admission accepted missing context");
  Require(Contains(missing_context_result.message_vector_json,
                   "UDR.BUILTIN.CONTEXT_REQUIRED"),
          "builtin package missing-context diagnostic mismatch");

  const runtime::UdrCallInput validate{
      std::string(spec.package_uuid), "validate_admission", {}, TrustedContext(spec)};
  const auto validation = runtime::InvokePackage(validate);
  Require(validation.ok, "builtin package validate_admission failed");
  Require(Contains(validation.message_vector_json, "engine_mga_authority"),
          "builtin package validation missing MGA authority proof");

  const runtime::UdrCallInput execute{
      std::string(spec.package_uuid), "execute", "operation_payload", TrustedContext(spec)};
  const auto executed = runtime::InvokePackage(execute);
  Require(!executed.ok,
          "builtin package execute must fail closed without package-specific policy");
  Require(Contains(executed.message_vector_json, spec.diagnostic_code),
          "builtin package execute diagnostic mismatch");
  Require(Contains(executed.message_vector_json, "\"no_external_effects\":true"),
          "builtin package execute did not prove no external effects");
  Require(Contains(executed.message_vector_json, "\"engine_mga_authority\":true"),
          "builtin package execute did not preserve MGA authority");

  const runtime::UdrCallInput missing_sblr_authority{
      std::string(spec.package_uuid),
      "execute",
      "sql=DROP TABLE spoofed_payload;",
      "engine_context=trusted;"};
  const auto missing_sblr = runtime::InvokePackage(missing_sblr_authority);
  Require(!missing_sblr.ok,
          "builtin package admitted execute without SBLR invocation authority");
  Require(Contains(missing_sblr.message_vector_json, "UDR.BUILTIN.CONTEXT_REQUIRED"),
          "builtin package missing SBLR authority diagnostic mismatch");

  const runtime::UdrCallInput bad_entrypoint{
      std::string(spec.package_uuid),
      "execute_sql_text_directly",
      "SELECT 1",
      TrustedContext(spec)};
  const auto entrypoint_refusal = runtime::InvokePackage(bad_entrypoint);
  Require(!entrypoint_refusal.ok,
          "builtin package admitted unknown direct-SQL entrypoint");
  Require(Contains(entrypoint_refusal.message_vector_json,
                   "UDR.RUNTIME.ENTRYPOINT_NOT_FOUND"),
          "builtin package unknown entrypoint diagnostic mismatch");

  if (spec.cluster_scoped) {
    const runtime::UdrCallInput cluster_without_provider{
        std::string(spec.package_uuid),
        "validate_admission",
        {},
        "engine_context=trusted;sblr_authorized_invocation=true;"};
    const auto cluster_refusal = runtime::InvokePackage(cluster_without_provider);
    Require(!cluster_refusal.ok,
            "cluster-scoped UDR admitted without provider authority");
    Require(Contains(cluster_refusal.message_vector_json,
                     "UDR.CLUSTER.PROVIDER_REQUIRED"),
            "cluster-scoped UDR provider diagnostic mismatch");
  }

  if (spec.category == "file_provider") {
    const runtime::UdrCallInput admitted_file_read{
        std::string(spec.package_uuid),
        "execute",
        FileProviderInlineStream(spec),
        FileProviderContext(spec, "id=2", "1")};
    const auto file_read = runtime::InvokePackage(admitted_file_read);
    Require(file_read.ok, "file-provider UDR should admit policy-backed inline stream reads");
    Require(Contains(file_read.payload, "\"schema_enforced\":true"),
            "file-provider UDR did not enforce schema");
    Require(Contains(file_read.payload, "\"pushdown_applied\":true"),
            "file-provider UDR did not apply pushdown");
    Require(Contains(file_read.payload, "\"streaming\":true"),
            "file-provider UDR did not expose streaming proof");
    Require(Contains(file_read.payload, "\"resource_budget_checked\":true"),
            "file-provider UDR did not check resource budget");
    Require(Contains(file_read.payload, "\"output_rows\":1"),
            "file-provider UDR pushdown output row count mismatch");
    Require(Contains(file_read.payload, "\"name\":\"Beta\""),
            "file-provider UDR returned the wrong pushed-down row");
    Require(Contains(file_read.message_vector_json, "UDR.FILE_PROVIDER.STREAM_READ"),
            "file-provider UDR stream-read diagnostic missing");

    const runtime::UdrCallInput budget_refusal{
        std::string(spec.package_uuid),
        "execute",
        FileProviderInlineStream(spec),
        FileProviderContext(spec, {}, "1")};
    const auto budget = runtime::InvokePackage(budget_refusal);
    Require(!budget.ok, "file-provider UDR ignored max_output_rows");
    Require(Contains(budget.message_vector_json,
                     "UDR.FILE_PROVIDER.RESOURCE_BUDGET_EXCEEDED"),
            "file-provider UDR budget refusal diagnostic mismatch");

    const runtime::UdrCallInput schema_refusal{
        std::string(spec.package_uuid),
        "execute",
        FileProviderInlineStream(spec),
        TrustedContext(spec) +
            "credential_ref=credential:019f7100-0000-7000-8000-00000000cc01;"
            "path_policy=read_only_fixture_root;"
            "path_root_uuid=019f7100-0000-7000-8000-00000000cc02;"
            "schema=missing:int,name:text;"};
    const auto schema = runtime::InvokePackage(schema_refusal);
    Require(!schema.ok, "file-provider UDR ignored schema mismatch");
    Require(Contains(schema.message_vector_json, "UDR.FILE_PROVIDER.SCHEMA_MISMATCH"),
            "file-provider UDR schema refusal diagnostic mismatch");

    const runtime::UdrCallInput path_escape{
        std::string(spec.package_uuid),
        "execute",
        FileProviderInlineStream(spec),
        TrustedContext(spec) +
            "credential_ref=credential:019f7100-0000-7000-8000-00000000cc01;"
            "path_policy=read_only_fixture_root;"
            "path_root_uuid=019f7100-0000-7000-8000-00000000cc02;"
            "schema=id:int,name:text;"
            "path_root=/tmp/sb_udr_allowed_root;"
            "source_path=/tmp/sb_udr_disallowed_root/escape.fixture;"};
    const auto path_escape_result = runtime::InvokePackage(path_escape);
    Require(!path_escape_result.ok,
            "file-provider UDR admitted source_path outside admitted root");
    Require(Contains(path_escape_result.message_vector_json,
                     "source_path_outside_admitted_path_root"),
            "file-provider UDR path-root refusal detail mismatch");

    const std::string temp_path =
        "/tmp/sb_udr_file_provider_" + std::string(spec.family_id) + ".fixture";
    {
      std::ofstream fixture(temp_path);
      fixture << FileProviderInlineStream(spec);
    }
    const runtime::UdrCallInput file_path_read{
        std::string(spec.package_uuid),
        "execute",
        {},
        FileProviderContext(spec, "id=3", "1", temp_path)};
    const auto file_path = runtime::InvokePackage(file_path_read);
    std::remove(temp_path.c_str());
    Require(file_path.ok, "file-provider UDR rejected admitted source_path read");
    Require(Contains(file_path.payload, "\"source_path_admitted\":true"),
            "file-provider UDR did not record admitted source_path read");
    Require(Contains(file_path.payload, "\"name\":\"Gamma\""),
            "file-provider UDR source_path pushdown returned wrong row");
  }

  if (spec.side_effects) {
#ifdef SB_UDR_BUILTIN_HAS_ENGINE_OUTBOX
    scratchbird::engine::internal_api::ResetExternalEffectOutboxForTests();
#endif
    const runtime::UdrCallInput precommit_side_effect{
        std::string(spec.package_uuid),
        "execute",
        "payload_hash_only=true",
        SideEffectContext(spec, false)};
    const auto precommit = runtime::InvokePackage(precommit_side_effect);
    Require(!precommit.ok, "side-effects UDR admitted pre-commit context");
    Require(Contains(precommit.message_vector_json, "UDR.SIDE_EFFECTS.PRECOMMIT_REFUSED"),
            "side-effects UDR pre-commit diagnostic mismatch");
    Require(Contains(precommit.message_vector_json, "\"external_effect_visible\":false"),
            "side-effects UDR pre-commit refusal exposed external effect");

    const runtime::UdrCallInput side_effect{
        std::string(spec.package_uuid),
        "execute",
        "payload_hash_only=true",
        SideEffectContext(spec, true)};
    const auto admitted = runtime::InvokePackage(side_effect);
    Require(admitted.ok, "side-effects UDR did not admit committed outbox operation");
    Require(Contains(admitted.payload, "\"outbox_durable_intent\":true"),
            "side-effects UDR missing durable outbox intent proof");
    Require(Contains(admitted.payload, "\"mga_evidence_before_success\":true"),
            "side-effects UDR missing MGA evidence-before-success proof");
    Require(Contains(admitted.payload, "\"external_effect_visible\":false"),
            "side-effects UDR made external effect visible in transaction path");
    Require(Contains(admitted.message_vector_json, "UDR.SIDE_EFFECTS.OUTBOX_ADMITTED"),
            "side-effects UDR admission diagnostic mismatch");

    const auto replay = runtime::InvokePackage(side_effect);
    Require(replay.ok, "side-effects UDR idempotent replay failed");
    Require(Contains(replay.payload, "\"idempotent_replay\":true"),
            "side-effects UDR replay was not deduplicated");
    Require(Contains(replay.message_vector_json, "UDR.SIDE_EFFECTS.IDEMPOTENT_REPLAY"),
            "side-effects UDR replay diagnostic mismatch");
  }

  if (spec.security_definer) {
    const runtime::UdrCallInput missing_role_chain{
        std::string(spec.package_uuid),
        "execute",
        "safe_payload=true",
        TrustedContext(spec) +
            "caller_principal_uuid=019f7100-0000-7000-8000-00000000ee01;"
            "effective_principal_uuid=019f7100-0000-7000-8000-00000000ee02;"};
    const auto missing = runtime::InvokePackage(missing_role_chain);
    Require(!missing.ok, "security-definer UDR admitted incomplete context");
    Require(Contains(missing.message_vector_json, "UDR.SECURITY_DEFINER.CONTEXT_REQUIRED"),
            "security-definer missing-context diagnostic mismatch");

    const runtime::UdrCallInput security_definer{
        std::string(spec.package_uuid),
        "execute",
        "safe_payload=true",
        SecurityDefinerContext(spec)};
    const auto admitted = runtime::InvokePackage(security_definer);
    Require(admitted.ok, "security-definer UDR rejected engine-admitted context");
    Require(Contains(admitted.payload, "\"role_chain_proof\""),
            "security-definer UDR missing role-chain proof");
    Require(Contains(admitted.payload, "\"group_chain_proof\""),
            "security-definer UDR missing group-chain proof");
    Require(Contains(admitted.payload, "\"redaction_enforced\":true"),
            "security-definer UDR missing redaction proof");
    Require(Contains(admitted.payload, "\"payload_context_ignored\":true"),
            "security-definer UDR did not ignore payload context");

    const runtime::UdrCallInput spoofed{
        std::string(spec.package_uuid),
        "execute",
        "effective_principal_uuid=019f7100-0000-7000-8000-00000000bad0;",
        SecurityDefinerContext(spec)};
    const auto spoofed_result = runtime::InvokePackage(spoofed);
    Require(!spoofed_result.ok,
            "security-definer UDR accepted spoofed payload principal context");
    Require(Contains(spoofed_result.message_vector_json,
                     "UDR.SECURITY_DEFINER.SPOOFED_PAYLOAD_CONTEXT"),
            "security-definer spoofing diagnostic mismatch");
  }

  RequireRuntimeOk(runtime::UnloadPackage(spec.package_uuid),
                   "builtin package unload failed");
}

void VerifyRuntimeRefusals() {
  auto descriptor =
      builtin::BuiltinUdrPackageDescriptor(builtin::BuiltinUdrPackageSpecs().front());
  descriptor.package_uuid = "019f7100-0000-7000-8000-00000000bad1";
  descriptor.package_name = "sbup_bad_non_cpp";
  descriptor.runtime_language = "python";
  const auto refused = runtime::RegisterPackage(descriptor);
  Require(!refused.ok, "non-C++ builtin package registration was admitted");
  Require(refused.diagnostic_code == "UDR.RUNTIME.NON_CPP_RUNTIME_FORBIDDEN",
          "non-C++ builtin package refusal diagnostic mismatch");

  descriptor.runtime_language = "cpp";
  descriptor.source_revision.clear();
  const auto no_provenance = runtime::RegisterPackage(descriptor);
  Require(!no_provenance.ok, "builtin package without provenance was admitted");
  Require(no_provenance.diagnostic_code == "UDR.RUNTIME.PROVENANCE_REQUIRED",
          "missing provenance diagnostic mismatch");
}

void VerifyDeploymentManifest(std::size_t expected_package_count) {
  const auto rows = builtin::BuiltinUdrPackageDeploymentManifest();
  Require(rows.size() == expected_package_count,
          "builtin UDR deployment manifest package count mismatch");

  std::map<std::string, builtin::BuiltinUdrPackageDeploymentManifestRow> by_uuid;
  for (const auto& row : rows) {
    Require(row.abi_version == "sb_udr_v1",
            "builtin UDR deployment manifest ABI mismatch");
    Require(row.runtime_language == "cpp",
            "builtin UDR deployment manifest runtime mismatch");
    Require(row.source_revision == "scratchbird-builtin-udr-catalog-v1",
            "builtin UDR deployment manifest source revision mismatch");
    Require(row.binary_hash == "sha256:scratchbird-builtin-udr-catalog-v1",
            "builtin UDR deployment manifest binary hash mismatch");
    Require(row.signature_policy == "release-key-or-local-dev-admission",
            "builtin UDR deployment manifest signature policy mismatch");
    Require(row.install_component == "lib/scratchbird/udr",
            "builtin UDR deployment manifest install component mismatch");
    Require(row.public_abi_status == "frozen_builtin_udr_sb_udr_v1",
            "builtin UDR deployment manifest ABI freeze status mismatch");
    Require(Contains(row.entrypoints_csv, "describe_capabilities:metadata"),
            "builtin UDR deployment manifest missing describe entrypoint");
    Require(Contains(row.entrypoints_csv, "validate_admission:policy"),
            "builtin UDR deployment manifest missing validate entrypoint");
    Require(Contains(row.entrypoints_csv, "execute:operation"),
            "builtin UDR deployment manifest missing execute entrypoint");
    Require(by_uuid.emplace(row.package_uuid, row).second,
            "builtin UDR deployment manifest duplicate package UUID");
  }

  for (const auto& spec : builtin::BuiltinUdrPackageSpecs()) {
    const auto found = by_uuid.find(std::string(spec.package_uuid));
    Require(found != by_uuid.end(),
            "builtin UDR deployment manifest missing package row");
    const auto& row = found->second;
    Require(row.package_name == spec.package_name,
            "builtin UDR deployment manifest package name mismatch");
    Require(row.family_id == spec.family_id,
            "builtin UDR deployment manifest family mismatch");
    Require(row.category == spec.category,
            "builtin UDR deployment manifest category mismatch");
    Require(row.capability_role == spec.capability_role,
            "builtin UDR deployment manifest capability role mismatch");
    Require(row.release_policy == spec.release_policy,
            "builtin UDR deployment manifest release policy mismatch");
    Require(row.diagnostic_code == spec.diagnostic_code,
            "builtin UDR deployment manifest diagnostic mismatch");
  }

  const auto json = builtin::BuiltinUdrPackageDeploymentManifestJson();
  Require(Contains(json, "\"manifest_kind\":\"builtin_trusted_cpp_udr_packages\""),
          "builtin UDR deployment manifest JSON missing kind");
  Require(Contains(json, "\"public_abi_status\":\"frozen_builtin_udr_sb_udr_v1\""),
          "builtin UDR deployment manifest JSON missing ABI freeze status");
  Require(Contains(json, "\"package_count\":49"),
          "builtin UDR deployment manifest JSON missing package count");
}

}  // namespace

int main() {
  runtime::ResetRuntimeForTest();
  const auto specs = builtin::BuiltinUdrPackageSpecs();
  Require(specs.size() >= 49, "builtin UDR package catalog is incomplete");

  std::set<std::string_view> uuids;
  std::set<std::string_view> names;
  std::map<std::string_view, int> category_counts;
  int file_provider_count = 0;
  int vertical_like_count = 0;
  int cluster_count = 0;
  int external_io_count = 0;

  for (const auto& spec : specs) {
    Require(!Contains(spec.family_id, "donor"),
            "builtin UDR family id uses prohibited donor terminology");
    Require(!Contains(spec.package_name, "donor"),
            "builtin UDR package name uses prohibited donor terminology");
    Require(uuids.insert(spec.package_uuid).second,
            "duplicate builtin UDR package UUID");
    Require(names.insert(spec.package_name).second,
            "duplicate builtin UDR package name");
    ++category_counts[spec.category];
    if (spec.category == "file_provider") ++file_provider_count;
    if (IsVerticalLike(spec)) ++vertical_like_count;
    if (spec.cluster_scoped) ++cluster_count;
    if (spec.external_io) ++external_io_count;
    VerifyRuntimeCallbacks(spec);
  }

  Require(file_provider_count == 8,
          "builtin UDR catalog must include all eight file-provider formats");
  Require(vertical_like_count >= 32,
          "builtin UDR catalog must include all vertical UDR family rows");
  Require(cluster_count == 1,
          "builtin UDR catalog must include the cluster fabric gate row");
  Require(external_io_count >= 10,
          "builtin UDR catalog must identify external-IO controlled packages");

  VerifyRuntimeRefusals();
  VerifyDeploymentManifest(specs.size());
  return EXIT_SUCCESS;
}
