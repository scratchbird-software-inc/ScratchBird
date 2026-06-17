// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_dispatch.hpp"

#include <iostream>
#include <cstdio>
#include <string>

namespace {

scratchbird::engine::internal_api::EngineRequestContext Context() {
  scratchbird::engine::internal_api::EngineRequestContext context;
  context.database_path = "/tmp/sb_sblr_dispatch_probe.sbdb";
  context.database_uuid.canonical = "018f0000-0000-7000-8000-000000000101";
  context.principal_uuid.canonical = "018f0000-0000-7000-8000-000000000102";
  context.session_uuid.canonical = "018f0000-0000-7000-8000-000000000103";
  context.security_context_present = true;
  return context;
}

bool HasDispatchDiagnostic(const scratchbird::engine::sblr::SblrDispatchResult& result,
                           const std::string& code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) { return true; }
  }
  return false;
}

}  // namespace

int main() {
  namespace sblr = scratchbird::engine::sblr;
  sblr::SblrDispatchRequest request;
  request.context = Context();
  request.envelope = sblr::MakeSblrEnvelope("observability.show_version", "SBLR_OBSERVABILITY_SHOW_VERSION", "TRACE-SBLR-DISPATCH");
  const auto show = sblr::DispatchSblrOperation(request);

  request.envelope = sblr::MakeSblrEnvelope("cluster.inspect_state", "SBLR_CLUSTER_INSPECT_STATE", "TRACE-SBLR-CLUSTER");
  request.envelope.requires_cluster_authority = true;
  const auto cluster = sblr::DispatchSblrOperation(request);

  request.envelope = sblr::MakeSblrEnvelope("unknown.operation", "unknown", "TRACE-SBLR-UNKNOWN");
  const auto unknown = sblr::DispatchSblrOperation(request);

  request.envelope = sblr::MakeSblrEnvelope("observability.show_version", "SBLR_OBSERVABILITY_SHOW_VERSION", "TRACE-SBLR-SQL-TEXT");
  request.envelope.contains_sql_text = true;
  const auto sql_text = sblr::DispatchSblrOperation(request);

  request.envelope = sblr::MakeSblrEnvelope("observability.show_version", "SBLR_OBSERVABILITY_SHOW_VERSION", "TRACE-SBLR-NAME-AUTHORITY");
  request.envelope.parser_resolved_names_to_uuids = false;
  const auto unresolved_names = sblr::DispatchSblrOperation(request);

  request.envelope = sblr::MakeSblrEnvelope("observability.show_version", "SBLR_DML_SELECT_ROWS", "TRACE-SBLR-OPCODE-MISMATCH");
  const auto opcode_mismatch = sblr::DispatchSblrOperation(request);

  sblr::SblrDispatchRequest forged_security;
  forged_security.context = Context();
  forged_security.context.security_context_present = false;
  forged_security.context.trace_tags = {"right:OBS_METRICS_READ_ALL", "parser_claim:trusted"};
  forged_security.envelope = sblr::MakeSblrEnvelope("observability.show_metrics", "SBLR_OBSERVABILITY_SHOW_METRICS", "TRACE-SBLR-FORGED-SECURITY");
  const auto missing_security = sblr::DispatchSblrOperation(forged_security);

  sblr::SblrDispatchRequest missing_transaction;
  missing_transaction.context = Context();
  missing_transaction.envelope = sblr::MakeSblrEnvelope("dml.insert_rows", "SBLR_DML_INSERT_ROWS", "TRACE-SBLR-MISSING-TXN");
  missing_transaction.envelope.requires_transaction_context = true;
  const auto missing_txn = sblr::DispatchSblrOperation(missing_transaction);

  std::remove("/tmp/sb_sblr_dispatch_probe.sbdb.sb.api_events");
  sblr::SblrDispatchRequest migration_begin;
  migration_begin.context = Context();
  migration_begin.context.local_transaction_id = 42;
  migration_begin.context.snapshot_visible_through_local_transaction_id = 42;
  migration_begin.envelope = sblr::MakeSblrEnvelope("op.migration.begin_from_reference",
                                                    "SBLR_MIGRATION_BEGIN_FROM_REFERENCE",
                                                    "TRACE-SBLR-MIGRATION-BEGIN");
  migration_begin.envelope.requires_transaction_context = true;
  migration_begin.api_request.option_envelopes = {
      "reference_profile:postgres",
      "reference_package:pg_compat_pack"};
  const auto migration_started = sblr::DispatchSblrOperation(migration_begin);

  sblr::SblrDispatchRequest migration_alter;
  migration_alter.context = migration_begin.context;
  migration_alter.envelope = sblr::MakeSblrEnvelope("op.migration.alter",
                                                    "SBLR_MIGRATION_ALTER",
                                                    "TRACE-SBLR-MIGRATION-ALTER");
  migration_alter.envelope.requires_transaction_context = true;
  migration_alter.api_request.option_envelopes = {
      "migration_ref:019f0000-0000-7000-8000-00000000a001",
      "migration_action:start"};
  const auto migration_altered = sblr::DispatchSblrOperation(migration_alter);

  const bool ok = show.accepted && show.dispatched_to_api && show.api_result.ok &&
                  !cluster.accepted && cluster.api_result.cluster_authority_required &&
                  !unknown.accepted &&
                  !sql_text.accepted && HasDispatchDiagnostic(sql_text, "SB_SBLR_SQL_TEXT_FORBIDDEN") &&
                  !unresolved_names.accepted && HasDispatchDiagnostic(unresolved_names, "SB_SBLR_NAMES_NOT_RESOLVED_TO_UUIDS") &&
                  !opcode_mismatch.accepted && HasDispatchDiagnostic(opcode_mismatch, "SB_SBLR_DISPATCH_OPCODE_MISMATCH") &&
                  !missing_security.accepted && HasDispatchDiagnostic(missing_security, "SB_SBLR_DISPATCH_SECURITY_CONTEXT_REQUIRED") &&
                  !missing_txn.accepted && HasDispatchDiagnostic(missing_txn, "SB_SBLR_DISPATCH_TRANSACTION_CONTEXT_REQUIRED") &&
                  migration_started.accepted && migration_started.dispatched_to_api && migration_started.api_result.ok &&
                  migration_altered.accepted && migration_altered.dispatched_to_api && migration_altered.api_result.ok;
  if (!migration_started.api_result.ok || !migration_altered.api_result.ok) {
    std::cerr << "migration_started="
              << sblr::SerializeSblrDispatchResultToJson(migration_started);
    std::cerr << "migration_altered="
              << sblr::SerializeSblrDispatchResultToJson(migration_altered);
  }
  std::cout << "{\"ok\":" << (ok ? "true" : "false")
            << ",\"show_ok\":" << (show.api_result.ok ? "true" : "false")
            << ",\"cluster_required\":" << (cluster.api_result.cluster_authority_required ? "true" : "false")
            << ",\"unknown_accepted\":" << (unknown.accepted ? "true" : "false")
            << ",\"sql_text_rejected\":" << (!sql_text.accepted ? "true" : "false")
            << ",\"unresolved_names_rejected\":" << (!unresolved_names.accepted ? "true" : "false")
            << ",\"opcode_mismatch_rejected\":" << (!opcode_mismatch.accepted ? "true" : "false")
            << ",\"forged_security_rejected\":" << (!missing_security.accepted ? "true" : "false")
            << ",\"missing_transaction_rejected\":" << (!missing_txn.accepted ? "true" : "false")
            << ",\"migration_started\":" << (migration_started.api_result.ok ? "true" : "false")
            << ",\"migration_altered\":" << (migration_altered.api_result.ok ? "true" : "false")
            << "}\n";
  return ok ? 0 : 1;
}
