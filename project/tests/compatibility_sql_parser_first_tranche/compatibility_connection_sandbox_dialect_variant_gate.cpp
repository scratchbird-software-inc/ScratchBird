// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "apache_ignite_dialect.hpp"
#include "cassandra_dialect.hpp"
#include "clickhouse_dialect.hpp"
#include "cockroachdb_dialect.hpp"
#include "dolt_dialect.hpp"
#include "compatibility_dialect.hpp"
#include "compatibility_worker_session.hpp"
#include "duckdb_dialect.hpp"
#include "firebird_dialect.hpp"
#include "foundationdb_dialect.hpp"
#include "immudb_dialect.hpp"
#include "influxdb_dialect.hpp"
#include "mariadb_dialect.hpp"
#include "milvus_dialect.hpp"
#include "mongodb_dialect.hpp"
#include "mysql_dialect.hpp"
#include "neo4j_dialect.hpp"
#include "opensearch_dialect.hpp"
#include "opensearch_sql_ppl_dialect.hpp"
#include "postgresql_dialect.hpp"
#include "redis_dialect.hpp"
#include "sqlite_dialect.hpp"
#include "tidb_dialect.hpp"
#include "tikv_dialect.hpp"
#include "vitess_dialect.hpp"
#include "xtdb_dialect.hpp"
#include "yugabytedb_dialect.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using scratchbird::parser::compatibility::ConnectionSandboxReportJson;
using scratchbird::parser::compatibility::DialectProfile;
using scratchbird::parser::compatibility::DialectVariantReportJson;

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool Expect(bool condition, std::string_view message) {
  if (condition) return true;
  std::cerr << message << '\n';
  return false;
}

const DialectProfile& FirebirdPolicyProfile() {
  static const DialectProfile profile{
      "firebird",
      "Firebird",
      "sbp_firebird",
      "sbu_firebird_parser_support",
      "5.0.4",
      "FIREBIRD",
      "sblr.compatibility.firebird.profile.v1",
      {},
      {},
      {},
      {},
      {},
      318,
      258,
      0,
      0,
      0,
      0,
      0,
      0,
      0};
  return profile;
}

struct CompatibilityProfile {
  std::string_view compatibility;
  const DialectProfile* profile;
};

std::vector<CompatibilityProfile> CompatibilityProfiles() {
  return {
      {"firebird", &FirebirdPolicyProfile()},
      {"postgresql", &scratchbird::parser::postgresql::Profile()},
      {"mysql", &scratchbird::parser::mysql::Profile()},
      {"sqlite", &scratchbird::parser::sqlite::Profile()},
      {"mariadb", &scratchbird::parser::mariadb::Profile()},
      {"duckdb", &scratchbird::parser::duckdb::Profile()},
      {"clickhouse", &scratchbird::parser::clickhouse::Profile()},
      {"tidb", &scratchbird::parser::tidb::Profile()},
      {"vitess", &scratchbird::parser::vitess::Profile()},
      {"cockroachdb", &scratchbird::parser::cockroachdb::Profile()},
      {"yugabytedb", &scratchbird::parser::yugabytedb::Profile()},
      {"cassandra", &scratchbird::parser::cassandra::Profile()},
      {"mongodb", &scratchbird::parser::mongodb::Profile()},
      {"redis", &scratchbird::parser::redis::Profile()},
      {"opensearch_sql_ppl", &scratchbird::parser::opensearch_sql_ppl::Profile()},
      {"opensearch", &scratchbird::parser::opensearch::Profile()},
      {"neo4j", &scratchbird::parser::neo4j::Profile()},
      {"influxdb", &scratchbird::parser::influxdb::Profile()},
      {"milvus", &scratchbird::parser::milvus::Profile()},
      {"dolt", &scratchbird::parser::dolt::Profile()},
      {"apache_ignite", &scratchbird::parser::apache_ignite::Profile()},
      {"tikv", &scratchbird::parser::tikv::Profile()},
      {"foundationdb", &scratchbird::parser::foundationdb::Profile()},
      {"immudb", &scratchbird::parser::immudb::Profile()},
      {"xtdb", &scratchbird::parser::xtdb::Profile()},
  };
}

bool CheckSandboxReport(const CompatibilityProfile& compatibility) {
  const auto report = ConnectionSandboxReportJson(*compatibility.profile);
  const std::string label(compatibility.compatibility);
  return Expect(Contains(report, "\"ok\":true"), label + " sandbox ok missing") &&
         Expect(Contains(report, "\"dialect\":\"" + label + "\""),
                label + " sandbox dialect missing") &&
         Expect(Contains(report, "compatibility_connection_schema_root_v1"),
                label + " sandbox contract missing") &&
         Expect(Contains(report, "relative_to_connection_schema_root"),
                label + " root-relative resolution missing") &&
         Expect(Contains(report, "\"direct_cross_root_access\":\"unsupported_denied\""),
                label + " cross-root denial missing") &&
         Expect(Contains(report, "\"server_local_file_access\":\"default_denied\""),
                label + " server-local file denial missing") &&
         Expect(Contains(report, "\"catalog_projection_authority\":\"catalog_emulation_definer_authority\""),
                label + " catalog definer authority missing") &&
         Expect(Contains(report, "\"catalog_projection_can_query_outside_sandbox\":true"),
                label + " catalog projection cross-root read contract missing") &&
         Expect(Contains(report, "\"catalog_projection_user_authority\":false"),
                label + " catalog projection user authority drift") &&
         Expect(Contains(report, "\"catalog_projection_select_grant_required\":true"),
                label + " catalog projection select grant missing") &&
         Expect(Contains(report, "\"catalog_projection_does_not_grant_base_object_access\":true"),
                label + " catalog projection base-object isolation missing") &&
         Expect(Contains(report, "\"sbsql_global_tree_visibility_inherited\":false"),
                label + " SBsql global visibility inheritance drift") &&
         Expect(Contains(report, "\"engine_authorization_authority\":\"scratchbird_engine\""),
                label + " engine authorization authority missing") &&
         Expect(Contains(report, "\"parser_authorization_authority\":false"),
                label + " parser authorization authority drift") &&
         Expect(Contains(report, "\"mga_transaction_authority\":\"scratchbird_engine\""),
                label + " MGA authority missing") &&
         Expect(Contains(report, "\"tenant_escape_policy\":\"fail_closed\""),
                label + " tenant escape policy missing");
}

bool CheckVariantReport(const CompatibilityProfile& compatibility) {
  const auto report = DialectVariantReportJson(*compatibility.profile);
  const std::string label(compatibility.compatibility);
  bool ok = Expect(Contains(report, "\"ok\":true"), label + " variant ok missing") &&
            Expect(Contains(report, "\"dialect\":\"" + label + "\""),
                   label + " variant dialect missing") &&
            Expect(Contains(report, "compatibility_supported_variant_surface_v1"),
                   label + " variant contract missing") &&
            Expect(Contains(report, "\"parser_cross_dialect_detection\":false"),
                   label + " cross-dialect detection drift") &&
            Expect(Contains(report, "\"parser_cross_dialect_dispatch\":false"),
                   label + " cross-dialect dispatch drift") &&
            Expect(Contains(report, "\"sbsql_variant_admitted\":false"),
                   label + " SBsql variant drift") &&
            Expect(Contains(report, "\"reasonable_subset_policy\":\"declared_and_tested_per_compatibility_variant\""),
                   label + " reasonable subset policy missing") &&
            Expect(!Contains(report, "TODO") && !Contains(report, "PLACEHOLDER") &&
                       !Contains(report, "STUB") && !Contains(report, "DEFER"),
                   label + " variant report contains incomplete marker");

  if (compatibility.compatibility == "firebird") {
    ok = ok && Expect(Contains(report, "firebird_sql_dialect_1_compat"),
                      "firebird dialect 1 coverage missing") &&
         Expect(Contains(report, "firebird_sql_dialect_3"),
                "firebird dialect 3 coverage missing") &&
         Expect(Contains(report, "firebird_psql"),
                "firebird PSQL coverage missing");
  } else if (compatibility.compatibility == "postgresql") {
    ok = ok && Expect(Contains(report, "postgresql_simple_query_sql"),
                      "postgresql simple-query coverage missing") &&
         Expect(Contains(report, "postgresql_extended_query_protocol"),
                "postgresql extended-query coverage missing") &&
         Expect(Contains(report, "postgresql_plpgsql_udr_body"),
                "postgresql PL/pgSQL coverage missing") &&
         Expect(Contains(report, "postgresql_logical_replication_protocol"),
                "postgresql logical-replication coverage missing");
  } else if (compatibility.compatibility == "mysql") {
    ok = ok && Expect(Contains(report, "mysql_text_protocol_sql"),
                      "mysql text protocol coverage missing") &&
         Expect(Contains(report, "mysql_binary_prepared_protocol"),
                "mysql binary prepared protocol coverage missing") &&
         Expect(Contains(report, "mysql_replication_binlog_stream"),
                "mysql binlog stream coverage missing");
  } else if (compatibility.compatibility == "mariadb") {
    ok = ok && Expect(Contains(report, "mariadb_sql_mode_mysql_compat"),
                      "mariadb MySQL-compatible SQL mode coverage missing") &&
         Expect(Contains(report, "mariadb_sql_mode_oracle_reasonable_subset"),
                "mariadb Oracle SQL mode subset coverage missing") &&
         Expect(Contains(report, "mariadb_replication_binlog_stream"),
                "mariadb binlog stream coverage missing");
  }
  return ok;
}

bool CheckSharedWorkerReports() {
  bool close = false;
  const auto& profile = scratchbird::parser::postgresql::Profile();
  const auto sandbox =
      scratchbird::parser::compatibility::HandleWorkerCommand("CONNECTION_SANDBOX_REPORT",
                                                      profile,
                                                      &close);
  const auto variants =
      scratchbird::parser::compatibility::HandleWorkerCommand("DIALECT_VARIANT_REPORT",
                                                      profile,
                                                      &close);
  return Expect(!close, "worker metadata reports closed the session") &&
         Expect(Contains(sandbox, "SANDBOX {\"ok\":true"),
                "worker sandbox report prefix missing") &&
         Expect(Contains(sandbox, "compatibility_connection_schema_root_v1"),
                "worker sandbox contract missing") &&
         Expect(Contains(sandbox, "\"direct_cross_root_access\":\"unsupported_denied\""),
                "worker cross-root denial missing") &&
         Expect(Contains(variants, "VARIANTS {\"ok\":true"),
                "worker variant report prefix missing") &&
         Expect(Contains(variants, "postgresql_extended_query_protocol"),
                "worker variant report lacks PostgreSQL extended query variant") &&
         Expect(Contains(variants, "\"parser_cross_dialect_dispatch\":false"),
                "worker variant report cross-dialect dispatch drift");
}

}  // namespace

int main() {
  bool ok = true;
  const auto compatibilitys = CompatibilityProfiles();
  ok = ok && Expect(compatibilitys.size() == 25, "compatibility profile count drift");
  for (const auto& compatibility : compatibilitys) {
    ok = CheckSandboxReport(compatibility) && ok;
    ok = CheckVariantReport(compatibility) && ok;
  }
  ok = CheckSharedWorkerReports() && ok;
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
