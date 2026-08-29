// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_sblr_admission_test_helper.hpp"
#include "cluster_provider/cluster_provider.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "sblr_opcode_registry.hpp"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

#ifndef SCRATCHBIRD_PROJECT_SOURCE_DIR
#define SCRATCHBIRD_PROJECT_SOURCE_DIR "."
#endif

namespace {

namespace api = scratchbird::engine::internal_api;
namespace cluster_provider = scratchbird::engine::cluster_provider;
namespace sblr = scratchbird::engine::sblr;

struct OpcodeRow {
  std::string_view operation_id;
  std::string_view opcode;
  std::string_view family;
  sblr::SblrOpcodeCategory category;
  sblr::SblrOpcodeSupport support;
  sblr::SblrOpcodeTransactionEffect transaction_effect;
  sblr::SblrOpcodeSecurityClass security_class;
  bool requires_transaction_context;
  bool requires_cluster_authority;
};

constexpr std::array<OpcodeRow, 51> kRows{{
    {"engine.op.ddl_create_rule", "SBLR_DDL_CREATE_RULE", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"engine.op.ddl_drop_rule", "SBLR_DDL_DROP_RULE", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"engine.op.ddl_create_publication", "SBLR_DDL_CREATE_PUBLICATION", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"engine.op.ddl_alter_publication", "SBLR_DDL_ALTER_PUBLICATION", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"engine.op.ddl_drop_publication", "SBLR_DDL_DROP_PUBLICATION", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"engine.op.ddl_create_subscription", "SBLR_DDL_CREATE_SUBSCRIPTION", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"engine.op.ddl_alter_subscription", "SBLR_DDL_ALTER_SUBSCRIPTION", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"engine.op.ddl_drop_subscription", "SBLR_DDL_DROP_SUBSCRIPTION", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"engine.op.ddl_create_aggregate", "SBLR_DDL_CREATE_AGGREGATE", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"engine.op.ddl_drop_aggregate", "SBLR_DDL_DROP_AGGREGATE", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"engine.op.ddl_create_operator", "SBLR_DDL_CREATE_OPERATOR", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"engine.op.ddl_drop_operator", "SBLR_DDL_DROP_OPERATOR", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"engine.op.ddl_create_operator_class", "SBLR_DDL_CREATE_OPERATOR_CLASS", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"engine.op.ddl_drop_operator_class", "SBLR_DDL_DROP_OPERATOR_CLASS", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"engine.op.ddl_create_operator_family", "SBLR_DDL_CREATE_OPERATOR_FAMILY", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"engine.op.ddl_alter_operator_family", "SBLR_DDL_ALTER_OPERATOR_FAMILY", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"engine.op.ddl_drop_operator_family", "SBLR_DDL_DROP_OPERATOR_FAMILY", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"engine.op.ddl_create_cast", "SBLR_DDL_CREATE_CAST", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"engine.op.ddl_drop_cast", "SBLR_DDL_DROP_CAST", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"engine.op.ddl_create_collation", "SBLR_DDL_CREATE_COLLATION", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"engine.op.ddl_alter_collation", "SBLR_DDL_ALTER_COLLATION", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"engine.op.ddl_drop_collation", "SBLR_DDL_DROP_COLLATION", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"engine.op.ddl_create_extension", "SBLR_DDL_CREATE_EXTENSION", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"engine.op.ddl_alter_extension", "SBLR_DDL_ALTER_EXTENSION", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"engine.op.ddl_drop_extension", "SBLR_DDL_DROP_EXTENSION", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"engine.op.ddl_create_event_trigger", "SBLR_DDL_CREATE_EVENT_TRIGGER", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"engine.op.ddl_alter_event_trigger", "SBLR_DDL_ALTER_EVENT_TRIGGER", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"engine.op.ddl_drop_event_trigger", "SBLR_DDL_DROP_EVENT_TRIGGER", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"engine.op.cluster_create_placement_policy", "SBLR_CLUSTER_CREATE_PLACEMENT_POLICY", "cluster-management", sblr::SblrOpcodeCategory::cluster, sblr::SblrOpcodeSupport::cluster_refusal, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::sysarch_authorized, true, true},
    {"engine.op.cluster_alter_placement_policy", "SBLR_CLUSTER_ALTER_PLACEMENT_POLICY", "cluster-management", sblr::SblrOpcodeCategory::cluster, sblr::SblrOpcodeSupport::cluster_refusal, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::sysarch_authorized, true, true},
    {"engine.op.cluster_drop_placement_policy", "SBLR_CLUSTER_DROP_PLACEMENT_POLICY", "cluster-management", sblr::SblrOpcodeCategory::cluster, sblr::SblrOpcodeSupport::cluster_refusal, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::sysarch_authorized, true, true},
    {"engine.op.cluster_declare_region", "SBLR_CLUSTER_DECLARE_REGION", "cluster-management", sblr::SblrOpcodeCategory::cluster, sblr::SblrOpcodeSupport::cluster_refusal, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::sysarch_authorized, true, true},
    {"engine.op.cluster_declare_availability_zone", "SBLR_CLUSTER_DECLARE_AVAILABILITY_ZONE", "cluster-management", sblr::SblrOpcodeCategory::cluster, sblr::SblrOpcodeSupport::cluster_refusal, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::sysarch_authorized, true, true},
    {"engine.op.cluster_declare_data_placement", "SBLR_CLUSTER_DECLARE_DATA_PLACEMENT", "cluster-management", sblr::SblrOpcodeCategory::cluster, sblr::SblrOpcodeSupport::cluster_refusal, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::sysarch_authorized, true, true},
    {"engine.op.ddl_create_dictionary", "SBLR_DDL_CREATE_DICTIONARY", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"engine.op.ddl_drop_dictionary", "SBLR_DDL_DROP_DICTIONARY", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"engine.op.ddl_create_named_collection", "SBLR_DDL_CREATE_NAMED_COLLECTION", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"engine.op.ddl_drop_named_collection", "SBLR_DDL_DROP_NAMED_COLLECTION", "catalog-ddl", sblr::SblrOpcodeCategory::ddl, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::admin_authorized, true, false},
    {"engine.op.bitemporal_as_of", "SBLR_BITEMPORAL_AS_OF", "versioned-history-execution", sblr::SblrOpcodeCategory::data_mutation, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"engine.op.bitemporal_as_of_valid_time", "SBLR_BITEMPORAL_AS_OF_VALID_TIME", "versioned-history-execution", sblr::SblrOpcodeCategory::data_mutation, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"engine.op.bitemporal_period_overlap", "SBLR_BITEMPORAL_PERIOD_OVERLAP", "versioned-history-execution", sblr::SblrOpcodeCategory::data_mutation, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"engine.op.bitemporal_for_versions_between", "SBLR_BITEMPORAL_FOR_VERSIONS_BETWEEN", "versioned-history-execution", sblr::SblrOpcodeCategory::data_mutation, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"engine.op.versioned_branch_create", "SBLR_VERSIONED_BRANCH_CREATE", "versioned-history-execution", sblr::SblrOpcodeCategory::data_mutation, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"engine.op.versioned_branch_delete", "SBLR_VERSIONED_BRANCH_DELETE", "versioned-history-execution", sblr::SblrOpcodeCategory::data_mutation, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"engine.op.versioned_diff", "SBLR_VERSIONED_DIFF", "versioned-history-execution", sblr::SblrOpcodeCategory::data_mutation, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"engine.op.versioned_tag", "SBLR_VERSIONED_TAG", "versioned-history-execution", sblr::SblrOpcodeCategory::data_mutation, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"engine.op.versioned_revert", "SBLR_VERSIONED_REVERT", "versioned-history-execution", sblr::SblrOpcodeCategory::data_mutation, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"engine.op.versioned_reset", "SBLR_VERSIONED_RESET", "versioned-history-execution", sblr::SblrOpcodeCategory::data_mutation, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::local_or_cluster_write, sblr::SblrOpcodeSecurityClass::object_authorized, true, false},
    {"engine.op.accel_llvm_policy_set", "SBLR_ACCEL_LLVM_POLICY_SET", "acceleration-management", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::sysarch_authorized, true, false},
    {"engine.op.accel_llvm_compile", "SBLR_ACCEL_LLVM_COMPILE", "acceleration-management", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::sysarch_authorized, true, false},
    {"engine.op.accel_llvm_inspect", "SBLR_ACCEL_LLVM_INSPECT", "acceleration-management", sblr::SblrOpcodeCategory::management, sblr::SblrOpcodeSupport::implemented, sblr::SblrOpcodeTransactionEffect::management, sblr::SblrOpcodeSecurityClass::sysarch_authorized, true, false},
}};

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool HasApiDiagnostic(const api::EngineApiResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

bool HasDispatchDiagnostic(const sblr::SblrDispatchResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

std::string EvidenceMessage(const OpcodeRow& row,
                            std::string_view phase,
                            std::string_view message) {
  std::string out(row.opcode);
  out += ' ';
  out += phase;
  out += ": ";
  out += message;
  return out;
}

sblr::SblrOperationEnvelope EnvelopeFor(const OpcodeRow& row) {
  auto envelope = sblr::MakeSblrEnvelope(std::string(row.operation_id),
                                         std::string(row.opcode),
                                         "opcode-registry-conformance");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = row.requires_transaction_context;
  envelope.requires_cluster_authority = row.requires_cluster_authority;
  return scratchbird::test::sbsql::CanonicalizeEngineSblrEnvelopeForTest(
      envelope);
}

void RequireCanonicalLookupAndMetadata(const OpcodeRow& row) {
  const auto* by_opcode = sblr::LookupSblrOpcode(row.opcode);
  Require(by_opcode != nullptr,
          EvidenceMessage(row, "lookup", "canonical opcode is not registered"));
  Require(by_opcode->operation_id == row.operation_id,
          EvidenceMessage(row, "lookup", "operation id drifted"));
  Require(by_opcode->family == row.family,
          EvidenceMessage(row, "metadata", "spec family drifted"));
  Require(by_opcode->category == row.category,
          EvidenceMessage(row, "metadata", "category drifted"));
  Require(by_opcode->support == row.support,
          EvidenceMessage(row, "metadata", "support state drifted"));
  Require(by_opcode->transaction_effect == row.transaction_effect,
          EvidenceMessage(row, "metadata", "transaction effect drifted"));
  Require(by_opcode->security_class == row.security_class,
          EvidenceMessage(row, "metadata", "security class drifted"));
  Require(by_opcode->requires_security_context,
          EvidenceMessage(row, "authority", "security context is not required"));
  Require(by_opcode->requires_transaction_context == row.requires_transaction_context,
          EvidenceMessage(row, "authority", "transaction context requirement drifted"));
  Require(by_opcode->requires_cluster_authority == row.requires_cluster_authority,
          EvidenceMessage(row, "authority", "cluster authority requirement drifted"));
  Require(by_opcode->cluster_private == row.requires_cluster_authority,
          EvidenceMessage(row, "authority", "cluster-private marker drifted"));

  Require(by_opcode->executor_id == row.operation_id,
          EvidenceMessage(row, "executor", "Core executor binding drifted"));
  Require(by_opcode->executor_evidence_required,
          EvidenceMessage(row, "executor", "Core executor evidence gate is not required"));

  const auto* by_operation = sblr::LookupSblrOperation(row.operation_id);
  Require(by_operation == by_opcode,
          EvidenceMessage(row, "lookup", "operation lookup did not return canonical entry"));
}

void RequireCanonicalEnvelopeValidation(const OpcodeRow& row) {
  const auto envelope = EnvelopeFor(row);
  const auto envelope_validation = sblr::ValidateSblrEnvelope(envelope);
  Require(envelope_validation.ok,
          EvidenceMessage(row, "envelope", "base engine envelope rejected valid canonical opcode"));

  const auto* entry = sblr::LookupSblrOperation(row.operation_id);
  Require(entry != nullptr,
          EvidenceMessage(row, "opcode_validation", "canonical operation disappeared"));
  const auto opcode_validation = sblr::ValidateSblrOpcodeForEnvelope(envelope);
  if (!entry->executor_evidence_accepted) {
    Require(!opcode_validation.ok &&
                opcode_validation.diagnostic_id ==
                    "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING" &&
                opcode_validation.diagnostic_id ==
                    entry->missing_executor_evidence_diagnostic,
            EvidenceMessage(row, "opcode_validation",
                            "missing executor evidence did not fail closed"));
    return;
  }
  Require(opcode_validation.ok,
          EvidenceMessage(row, "opcode_validation", "canonical opcode validation failed"));
  Require(opcode_validation.entry == entry &&
              opcode_validation.entry->opcode == row.opcode,
          EvidenceMessage(row, "opcode_validation", "validation did not bind canonical entry"));

  auto missing_security = envelope;
  missing_security.requires_security_context = false;
  const auto security_validation = sblr::ValidateSblrOpcodeForEnvelope(missing_security);
  Require(!security_validation.ok &&
              security_validation.diagnostic_id == "SB_DIAG_SBLR_SECURITY_CONTEXT_REQUIRED",
          EvidenceMessage(row, "security_refusal", "missing security context was not refused"));

  if (row.requires_transaction_context) {
    auto missing_transaction = envelope;
    missing_transaction.requires_transaction_context = false;
    const auto transaction_validation = sblr::ValidateSblrOpcodeForEnvelope(missing_transaction);
    Require(!transaction_validation.ok &&
                transaction_validation.diagnostic_id == "SB_DIAG_SBLR_TRANSACTION_CONTEXT_REQUIRED",
            EvidenceMessage(row, "transaction_refusal", "missing transaction context was not refused"));
  }

  if (row.requires_cluster_authority) {
    auto missing_cluster = envelope;
    missing_cluster.requires_cluster_authority = false;
    const auto cluster_validation = sblr::ValidateSblrOpcodeForEnvelope(missing_cluster);
    Require(!cluster_validation.ok &&
                cluster_validation.diagnostic_id == "SB_DIAG_CLUSTER_TXN_UNAVAILABLE",
            EvidenceMessage(row, "cluster_refusal", "missing cluster authority was not refused"));
  }
}

void RequireClusterProviderBoundary(const OpcodeRow& row) {
  if (!row.requires_cluster_authority) return;
  sblr::SblrDispatchRequest request;
  request.context.security_context_present = true;
  request.context.database_uuid.canonical = "opcode-registry-database";
  request.context.session_uuid.canonical = "opcode-registry-session";
  request.context.principal_uuid.canonical = "opcode-registry-principal";
  request.context.local_transaction_id = 1;
  request.envelope = EnvelopeFor(row);

  const auto result = sblr::DispatchSblrOperation(request);
  Require(result.envelope_validated,
          EvidenceMessage(row, "cluster_boundary", "envelope did not validate"));
  Require(result.accepted,
          EvidenceMessage(row, "cluster_boundary", "dispatch did not accept boundary route"));
  Require(result.dispatched_to_api,
          EvidenceMessage(row, "cluster_boundary", "route did not reach provider boundary"));

  if (cluster_provider::ClusterProviderSupportsExecution()) {
    Require(result.api_result.ok,
            EvidenceMessage(row, "cluster_boundary", "configured provider rejected route"));
  } else {
    Require(!result.api_result.ok,
            EvidenceMessage(row, "cluster_boundary", "no-cluster provider executed core cluster route"));
    Require(result.api_result.cluster_authority_required,
            EvidenceMessage(row, "cluster_boundary", "cluster authority requirement was not preserved"));
    Require(HasApiDiagnostic(result.api_result,
                             cluster_provider::kClusterSupportNotEnabledCode),
            EvidenceMessage(row, "cluster_boundary", "API diagnostic missing"));
    Require(HasDispatchDiagnostic(result,
                                  cluster_provider::kClusterSupportNotEnabledCode),
            EvidenceMessage(row, "cluster_boundary", "dispatch diagnostic missing"));
  }
}

void RequireCanonicalOperationPreservation() {
  struct CanonicalOperationRow {
    std::string_view operation_id;
    std::string_view opcode;
  };
  constexpr std::array<CanonicalOperationRow, 9> canonical_operations{{
      {"storage.manage_operation", "SBLR_STORAGE_MANAGEMENT_OPERATION"},
      {"management.inspect_runtime", "SBLR_MANAGEMENT_INSPECT_RUNTIME"},
      {"management.control_runtime", "SBLR_MANAGEMENT_CONTROL_RUNTIME"},
      {"observability.show_metrics", "SBLR_OBSERVABILITY_SHOW_METRICS"},
      {"engine.op.mga_show_horizons", "SBLR_MGA_SHOW_HORIZONS"},
      {"engine.op.database_attach", "SBLR_DATABASE_ATTACH"},
      {"engine.op.repl_consumer_subscribe", "SBLR_REPL_CONSUMER_SUBSCRIBE"},
      {"graph.traverse", "SBLR_GRAPH_TRAVERSE"},
      {"engine.op.system_config_set", "SBLR_SYSTEM_CONFIG_SET"},
  }};

  for (const auto& canonical : canonical_operations) {
    const auto* entry = sblr::LookupSblrOperation(canonical.operation_id);
    Require(entry != nullptr, "existing canonical registry operation disappeared");
    Require(entry->opcode == canonical.opcode, "existing canonical registry opcode changed");

    auto envelope = sblr::MakeSblrEnvelope(std::string(canonical.operation_id),
                                           std::string(canonical.opcode),
                                           "opcode-canonical-preservation");
    envelope.requires_security_context = entry->requires_security_context;
    envelope.requires_transaction_context = entry->requires_transaction_context;
    envelope.requires_cluster_authority = entry->requires_cluster_authority;
    const auto validation = sblr::ValidateSblrOpcodeForEnvelope(envelope);
    if (entry->executor_evidence_accepted) {
      Require(validation.ok, "canonical operation validation regressed");
    } else {
      Require(entry->executor_evidence_required && !validation.ok &&
                  validation.diagnostic_id ==
                      "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
              "canonical operation did not fail closed on missing executor evidence");
    }
  }
}

void RequireUnknownAndMismatchDiagnostics() {
  Require(sblr::LookupSblrOpcode("SBLR_NOT_A_REAL_OPCODE") == nullptr,
          "unknown opcode unexpectedly resolved");

  auto unknown_operation = sblr::MakeSblrEnvelope("not.a.real.operation",
                                                  "SBLR_DDL_CREATE_RULE",
                                                  "opcode-unknown-operation");
  unknown_operation.requires_transaction_context = true;
  const auto unknown_operation_validation =
      sblr::ValidateSblrOpcodeForEnvelope(unknown_operation);
  Require(!unknown_operation_validation.ok &&
              unknown_operation_validation.diagnostic_id == "SB_DIAG_SBLR_UNKNOWN_OPERATION",
          "unknown operation diagnostic changed");

  auto mismatch = sblr::MakeSblrEnvelope("engine.op.ddl_create_rule",
                                         "SBLR_NOT_A_REAL_OPCODE",
                                         "opcode-mismatch");
  mismatch.requires_transaction_context = true;
  const auto mismatch_validation = sblr::ValidateSblrOpcodeForEnvelope(mismatch);
  Require(!mismatch_validation.ok &&
              mismatch_validation.diagnostic_id == "SB_DIAG_SBLR_OPCODE_MISMATCH",
          "opcode mismatch diagnostic changed");
}

void RequireProductionSourceIntegrity() {
  static constexpr std::array<std::string_view, 53> kForbidden = {
      "sbsql_sblr_final_cleanup",
      "final_cleanup",
      "B001Exact",
      "IsB001",
      "b001_",
      "_b001",
      "B002Exact",
      "IsB002",
      "b002_",
      "_b002",
      "B003Exact",
      "IsB003",
      "b003_",
      "_b003",
      "B007Exact",
      "IsB007",
      "b007_",
      "_b007",
      "B008Exact",
      "IsB008",
      "b008_",
      "_b008",
      "B009Exact",
      "IsB009",
      "b009_",
      "_b009",
      "B010Exact",
      "IsB010",
      "b010_",
      "_b010",
      "B011Exact",
      "IsB011",
      "b011_",
      "_b011",
      "B012Exact",
      "IsB012",
      "b012_",
      "_b012",
      "B013Exact",
      "IsB013",
      "b013_",
      "_b013",
      "AUDIT-0",
      "AUDIT-1",
      "AUDIT-2",
      "AUDIT-3",
      "AUDIT-4",
      "AUDIT-5",
      "AUDIT-6",
      "AUDIT-7",
      "AUDIT-8",
      "AUDIT-9",
      "SSFC-",
  };
  const std::filesystem::path source_root =
      std::filesystem::path(SCRATCHBIRD_PROJECT_SOURCE_DIR) / "src";
  for (const auto& entry : std::filesystem::recursive_directory_iterator(source_root)) {
    if (!entry.is_regular_file()) continue;
    std::ifstream in(entry.path(), std::ios::binary);
    if (!in) continue;
    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    for (const auto token : kForbidden) {
      Require(!Contains(text, token),
              std::string("production source contains forbidden batch token ") +
                  std::string(token) + " in " + entry.path().string());
    }
  }
}

}  // namespace

int main() {
  RequireProductionSourceIntegrity();
  for (const auto& row : kRows) {
    RequireCanonicalLookupAndMetadata(row);
    RequireCanonicalEnvelopeValidation(row);
    RequireClusterProviderBoundary(row);
  }
  RequireCanonicalOperationPreservation();
  RequireUnknownAndMismatchDiagnostics();
  std::cout << "sbsql_sblr_final_cleanup_b013_opcode_registry_conformance=passed\n";
  return EXIT_SUCCESS;
}
