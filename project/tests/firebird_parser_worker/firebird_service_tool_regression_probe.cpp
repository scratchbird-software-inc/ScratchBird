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
#include <string>
#include <string_view>

namespace {

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

struct Case {
  std::string_view command;
  std::string_view operation_family;
};

bool Probe(const Case& test) {
  const auto parsed = scratchbird::parser::firebird::ParseStatement(test.command);
  if (parsed.ok) {
    std::cerr << "service tool command was accepted: " << test.command << '\n';
    std::cerr << parsed.sblr_envelope << '\n';
    return false;
  }
  if (parsed.statement_family != "low_level_utility") {
    std::cerr << "service tool statement family mismatch for " << test.command
              << ": " << parsed.statement_family << '\n';
    return false;
  }
  if (parsed.operation_family != test.operation_family) {
    std::cerr << "service tool operation family mismatch for " << test.command
              << ": " << parsed.operation_family << '\n';
    return false;
  }
  if (!parsed.sblr_envelope.empty()) {
    std::cerr << "service tool denial produced SBLR for " << test.command
              << '\n';
    return false;
  }
  if (!Contains(parsed.message_vector_json,
                "FIREBIRD.AUTHORITY.UNSUPPORTED_DENIED")) {
    std::cerr << "service tool denial diagnostic missing for " << test.command
              << '\n'
              << parsed.message_vector_json << '\n';
    return false;
  }
  if (!Contains(parsed.message_vector_json,
                "\"real_firebird_file_effects\":\"false\"") ||
      !Contains(parsed.message_vector_json,
                "\"reference_engine_sql_executed\":\"false\"")) {
    std::cerr << "service tool denial proof fields missing for "
              << test.command << '\n'
              << parsed.message_vector_json << '\n';
    return false;
  }
  std::cout << test.command << " => " << parsed.operation_family << '\n';
  return true;
}

bool ProbeGbakLogicalStream(std::string_view command,
                            std::string_view operation_family) {
  const auto parsed = scratchbird::parser::firebird::ParseStatement(command);
  if (!parsed.ok) {
    std::cerr << "gbak logical stream command rejected: " << command << '\n'
              << parsed.message_vector_json << '\n';
    return false;
  }
  if (parsed.statement_family != "logical_stream_backup_restore") {
    std::cerr << "gbak logical stream statement family mismatch for "
              << command << ": " << parsed.statement_family << '\n';
    return false;
  }
  if (parsed.operation_family != operation_family) {
    std::cerr << "gbak logical stream operation mismatch for " << command
              << ": " << parsed.operation_family << '\n';
    return false;
  }
  if (parsed.scratchbird_lifecycle_api ||
      parsed.real_firebird_file_effects ||
      parsed.reference_engine_sql_executed) {
    std::cerr << "gbak logical stream claimed forbidden authority for "
              << command << '\n';
    return false;
  }
  if (!Contains(parsed.sblr_envelope,
                "\"statement_family\":\"logical_stream_backup_restore\"") ||
      !Contains(parsed.sblr_envelope,
                "\"firebird_gbak_logical_stream_evidence\":{") ||
      !Contains(parsed.sblr_envelope,
                "\"evidence_contract\":\"firebird_gbak_logical_stream_evidence.v1\"") ||
      !Contains(parsed.sblr_envelope,
                "\"remote_client_stream\":true") ||
      !Contains(parsed.sblr_envelope,
                "\"single_connected_legacy_database_scope\":true") ||
      !Contains(parsed.sblr_envelope,
                "\"server_local_file_access\":\"default_denied\"") ||
      !Contains(parsed.sblr_envelope,
                "\"physical_page_copy_allowed\":false") ||
      !Contains(parsed.sblr_envelope,
                "\"nbackup_allowed\":false") ||
      !Contains(parsed.sblr_envelope,
                "\"raw_database_file_restore_allowed\":false") ||
      !Contains(parsed.sblr_envelope,
                "\"raw_database_file_backup_allowed\":false") ||
      !Contains(parsed.sblr_envelope,
                "\"sblr_requirement\":\"required_logical_stream_backup_restore_surface\"") ||
      !Contains(parsed.sblr_envelope,
                "\"engine_authority\":\"scratchbird_mga_catalog_sblr\"") ||
      !Contains(parsed.sblr_envelope,
                "\"scratchbird_lifecycle_api\":false") ||
      !Contains(parsed.sblr_envelope,
                "\"real_firebird_file_effects\":false") ||
      !Contains(parsed.sblr_envelope,
                "\"reference_engine_sql_executed\":false") ||
      !Contains(parsed.sblr_envelope,
                "\"engine_authority\":\"scratchbird\"")) {
    std::cerr << "gbak logical stream SBLR evidence mismatch for "
              << command << '\n'
              << parsed.sblr_envelope << '\n';
    return false;
  }
  const bool is_backup = operation_family == "firebird.logical_stream.gbak_backup";
  if (is_backup) {
    if (!Contains(parsed.sblr_envelope, "\"backup_stream\":true") ||
        !Contains(parsed.sblr_envelope, "\"restore_stream\":false") ||
        !Contains(parsed.sblr_envelope, "\"stdout_stream_bound\":true") ||
        !Contains(parsed.sblr_envelope, "\"stdin_stream_bound\":false")) {
      std::cerr << "gbak backup logical stream direction proof mismatch for "
                << command << '\n'
                << parsed.sblr_envelope << '\n';
      return false;
    }
  } else if (!Contains(parsed.sblr_envelope, "\"backup_stream\":false") ||
             !Contains(parsed.sblr_envelope, "\"restore_stream\":true") ||
             !Contains(parsed.sblr_envelope, "\"stdout_stream_bound\":false") ||
             !Contains(parsed.sblr_envelope, "\"stdin_stream_bound\":true")) {
    std::cerr << "gbak restore logical stream direction proof mismatch for "
              << command << '\n'
              << parsed.sblr_envelope << '\n';
    return false;
  }
  std::cout << command << " => " << parsed.operation_family << '\n';
  return true;
}

} // namespace

int main() {
  if (!ProbeGbakLogicalStream(
          "GBAK -backup scratchbird-firebird.fdb stdout",
          "firebird.logical_stream.gbak_backup")) {
    return EXIT_FAILURE;
  }
  if (!ProbeGbakLogicalStream(
          "gbak -b scratchbird-firebird.fdb stdout",
          "firebird.logical_stream.gbak_backup")) {
    return EXIT_FAILURE;
  }
  if (!ProbeGbakLogicalStream(
          "gbak -b scratchbird-firebird.fdb stdout -parallel 2",
          "firebird.logical_stream.gbak_backup")) {
    return EXIT_FAILURE;
  }
  if (!ProbeGbakLogicalStream(
          "GBAK -restore stdin scratchbird-firebird.fdb",
          "firebird.logical_stream.gbak_restore")) {
    return EXIT_FAILURE;
  }
  if (!ProbeGbakLogicalStream(
          "gbak -r stdin scratchbird-firebird.fdb",
          "firebird.logical_stream.gbak_restore")) {
    return EXIT_FAILURE;
  }
  if (!ProbeGbakLogicalStream(
          "gbak -r stdin scratchbird-firebird.fdb -parallel 2",
          "firebird.logical_stream.gbak_restore")) {
    return EXIT_FAILURE;
  }
  if (!ProbeGbakLogicalStream(
          "gbak -create stdin scratchbird-firebird.fdb",
          "firebird.logical_stream.gbak_restore")) {
    return EXIT_FAILURE;
  }
  if (!ProbeGbakLogicalStream(
          "gbak -c stdin scratchbird-firebird.fdb",
          "firebird.logical_stream.gbak_restore")) {
    return EXIT_FAILURE;
  }
  if (!ProbeGbakLogicalStream(
          "gbak -replace stdin scratchbird-firebird.fdb",
          "firebird.logical_stream.gbak_restore")) {
    return EXIT_FAILURE;
  }

  const Case cases[] = {
      {"GBAK -backup scratchbird-firebird.fdb scratchbird-firebird.fbk",
       "firebird.emulated.reference_native_tool"},
      {"GBAK -backup scratchbird-firebird.fdb scratchbird-firebird.fbk -y stdout",
       "firebird.emulated.reference_native_tool"},
      {"GBAK -restore scratchbird-firebird.fbk scratchbird-firebird.fdb",
       "firebird.emulated.reference_native_tool"},
      {"GBAK -restore -y stdin scratchbird-firebird.fbk scratchbird-firebird.fdb",
       "firebird.emulated.reference_native_tool"},
      {"GBAK -replace scratchbird-firebird.fbk scratchbird-firebird.fdb",
       "firebird.emulated.reference_native_tool"},
      {"GFIX -validate scratchbird-firebird.fdb",
       "firebird.emulated.reference_native_tool"},
      {"GFIX -mend scratchbird-firebird.fdb",
       "firebird.emulated.reference_native_tool"},
      {"GFIX -sweep scratchbird-firebird.fdb",
       "firebird.emulated.reference_native_tool"},
      {"GSTAT -header scratchbird-firebird.fdb",
       "firebird.emulated.reference_native_tool"},
      {"GSTAT -data scratchbird-firebird.fdb",
       "firebird.emulated.reference_native_tool"},
      {"GSTAT -index scratchbird-firebird.fdb",
       "firebird.emulated.reference_native_tool"},
      {"NBACKUP -backup 0 scratchbird-firebird.fdb scratchbird-firebird.nbk",
       "firebird.emulated.incremental_backup"},
      {"NBACKUP -lock scratchbird-firebird.fdb",
       "firebird.emulated.incremental_backup"},
      {"NBACKUP -unlock scratchbird-firebird.fdb",
       "firebird.emulated.incremental_backup"},
      {"NBACKUP -fixup scratchbird-firebird.fdb",
       "firebird.emulated.incremental_backup"},
      {"FBSVCMGR service_mgr action_db_stats dbname scratchbird-firebird.fdb",
       "firebird.emulated.reference_native_tool"},
      {"FBSVCMGR service_mgr action_backup dbname scratchbird-firebird.fdb",
       "firebird.emulated.reference_native_tool"},
      {"FBSVCMGR service_mgr action_restore dbname scratchbird-firebird.fdb",
       "firebird.emulated.reference_native_tool"},
      {"FBSVCMGR service_mgr action_validate dbname scratchbird-firebird.fdb",
       "firebird.emulated.reference_native_tool"},
      {"FBTRACEMGR -service service_mgr -list",
       "firebird.emulated.reference_native_tool"},
      {"GSEC display", "firebird.emulated.reference_native_tool"},
      {"GPRE sample.e", "firebird.emulated.reference_native_tool"},
      {"GSPLIT -join_backup_file split01.fbk split02.fbk",
       "firebird.emulated.reference_native_tool"},
      {"FB_LOCK_PRINT -d scratchbird-firebird.fdb",
       "firebird.emulated.reference_native_tool"},
      {"FBGUARD -onetime", "firebird.emulated.reference_native_tool"},
  };

  for (const auto& test : cases) {
    if (!Probe(test)) return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
