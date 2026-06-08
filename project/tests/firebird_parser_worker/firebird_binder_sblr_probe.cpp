// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "firebird_dialect.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool Probe(std::string_view sql,
           std::string_view family,
           std::string_view operation) {
  const auto parsed = scratchbird::parser::firebird::ParseStatement(sql);
  if (!parsed.ok) {
    std::cerr << "parse failed: " << sql << '\n';
    return false;
  }
  if (parsed.statement_family != family) {
    std::cerr << "statement family mismatch: " << parsed.statement_family << '\n';
    return false;
  }
  if (parsed.operation_family != operation) {
    std::cerr << "operation family mismatch: " << parsed.operation_family << '\n';
    return false;
  }
  if (!Contains(parsed.sblr_envelope, "SBLRExecutionEnvelope.v3") ||
      !Contains(parsed.sblr_envelope, "\"statement_family\":\"") ||
      !Contains(parsed.sblr_envelope, "\"operation_family\":\"") ||
      !Contains(parsed.sblr_envelope, "\"enterprise_readiness_evidence\":{") ||
      !Contains(parsed.sblr_envelope,
                "\"evidence_contract\":\"donor_parser_enterprise_readiness_evidence.v1\"") ||
      !Contains(parsed.sblr_envelope,
                "\"procedural_body_encoding_status\":\"route_and_descriptor_only_not_enterprise\"") ||
      !Contains(parsed.sblr_envelope,
                "\"observable_equivalence_status\":\"donor_native_equivalence_proof_pending\"") ||
      !Contains(parsed.sblr_envelope, "\"enterprise_implemented_proven\":false") ||
      !Contains(parsed.sblr_envelope, "\"descriptor_resolution\":\"uuid_required\"") ||
      !Contains(parsed.sblr_envelope, "\"engine_authority\":\"scratchbird\"") ||
      !Contains(parsed.sblr_envelope, "\"finite_subset\":true") ||
      !Contains(parsed.sblr_envelope, "\"sql_text_included\":false")) {
    std::cerr << "SBLR verifier envelope mismatch: " << parsed.sblr_envelope << '\n';
    return false;
  }
  if ((operation == "firebird.ddl.create.procedure" ||
       operation == "firebird.psql.execute_block") &&
      (!Contains(parsed.sblr_envelope,
                 "\"firebird_psql_functional_encoding_evidence\":{") ||
       !Contains(parsed.sblr_envelope,
                 "\"functional_encoding_status\":\"firebird_psql_parser_bound_sblr_encoded\"") ||
       !Contains(parsed.sblr_envelope,
                 "\"parser_bound_sblr_body_instruction_stream\":true") ||
       !Contains(parsed.sblr_envelope,
                 "\"uuid_dependency_bindings_bound\":true") ||
       !Contains(parsed.sblr_envelope,
                 "\"body_lowering_status\":\"parser_bound_sblr_instruction_stream_encoded\"") ||
       !Contains(parsed.sblr_envelope,
                 "\"runtime_equivalence_status\":\"pending_donor_native_psql_replay\""))) {
    std::cerr << "Firebird PSQL functional encoding evidence mismatch: "
              << parsed.sblr_envelope << '\n';
    return false;
  }
  if (Contains(parsed.sblr_envelope, sql)) {
    std::cerr << "SBLR envelope leaked donor SQL text\n";
    return false;
  }
  return true;
}

} // namespace

int main() {
  if (!Probe("select rdb$db_key from rdb$database",
             "catalog_overlay",
             "firebird.catalog_overlay.rdb_core")) {
    return EXIT_FAILURE;
  }
  if (!Probe("select first 1 id from t",
             "query",
             "firebird.query.select.first_skip_rows")) {
    return EXIT_FAILURE;
  }
  if (!Probe("insert into t(id) values (?) returning id",
             "dml",
             "firebird.dml.insert.returning")) {
    return EXIT_FAILURE;
  }
  if (!Probe("update t set id = ? where current of c_t",
             "dml",
             "firebird.dml.cursor.update_current_of")) {
    return EXIT_FAILURE;
  }
  if (!Probe("create procedure p as begin end",
             "ddl",
             "firebird.ddl.create.procedure")) {
    return EXIT_FAILURE;
  }
  if (!Probe("create user alice password 'secret'",
             "ddl",
             "firebird.ddl.create.user")) {
    return EXIT_FAILURE;
  }
  if (!Probe("grant select on t to report_role",
             "ddl",
             "firebird.ddl.grant")) {
    return EXIT_FAILURE;
  }
  if (!Probe("set transaction read committed",
             "transaction",
             "firebird.transaction.set_transaction")) {
    return EXIT_FAILURE;
  }
  if (!Probe("savepoint before_t",
             "transaction",
             "firebird.transaction.savepoint")) {
    return EXIT_FAILURE;
  }
  if (!Probe("execute block as begin suspend; end",
             "psql",
             "firebird.psql.execute_block")) {
    return EXIT_FAILURE;
  }
  if (!Probe("create procedure p as begin post_event 'secret'; end",
             "ddl",
             "firebird.ddl.create.procedure")) {
    return EXIT_FAILURE;
  }
  if (!Probe("create database 'x.fdb'",
             "non_file_emulation",
             "firebird.emulated.database_lifecycle")) {
    return EXIT_FAILURE;
  }

  const auto invalid = scratchbird::parser::firebird::ParseStatement("malformed");
  if (invalid.ok || !invalid.sblr_envelope.empty()) {
    std::cerr << "invalid input produced SBLR envelope\n";
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
