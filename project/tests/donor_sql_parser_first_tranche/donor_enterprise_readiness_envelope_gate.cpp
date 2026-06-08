// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "donor_dialect.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

using scratchbird::parser::donor::DialectProfile;
using scratchbird::parser::donor::MappingDisposition;
using scratchbird::parser::donor::OperationPattern;
using scratchbird::parser::donor::ParseStatement;
using scratchbird::parser::donor::PatternMatch;
using scratchbird::parser::donor::SurfaceDescriptor;

constexpr OperationPattern kPatterns[] = {
    {"SELECT", PatternMatch::kPrefix, "query", "common.query.select",
     MappingDisposition::kAdmittedSblr, "common.query.select",
     "SBLR_DONOR_COMMON_SELECT", "EngineSelect",
     "COMMON.EMULATION.SELECT_ROUTE",
     "Common select route for readiness envelope proof.", true, true},
    {"CHANGE REPLICATION", PatternMatch::kPrefix, "replication",
     "common.replication.change", MappingDisposition::kParserSupportUdr,
     "common.udr.replication.change", "SBLR_DONOR_COMMON_REPLICATION_ROUTE",
     "ParserSupportReplicationRoute", "COMMON.EMULATION.REPLICATION_ROUTE",
     "Replication requests route through parser-support UDR policy.", true, false},
    {"LOAD DATA", PatternMatch::kPrefix, "bulk_io",
     "common.bulk_io.load_data", MappingDisposition::kPolicyRefusal,
     "common.policy.file.load_data", "", "",
     "COMMON.AUTHORITY.FILE_IO_DENIED",
     "Server-local donor file access is refused by policy.", true, false},
    {"GRANT", PatternMatch::kPrefix, "security", "common.security.grant",
     MappingDisposition::kSecurityRefusal, "common.security.grant", "", "",
     "COMMON.AUTHORITY.SECURITY_DENIED",
     "Parser cannot grant security authority.", true, false},
    {"REPAIR", PatternMatch::kPrefix, "maintenance", "common.maintenance.repair",
     MappingDisposition::kUnsupportedRefusal, "common.unsupported.repair", "", "",
     "COMMON.AUTHORITY.UNSUPPORTED_DENIED",
     "Low-level repair and verify commands are unsupported from donor syntax.",
     true, false},
};

constexpr std::array<SurfaceDescriptor, 0> kNoSurfaces{};

const DialectProfile& TestProfile() {
  static const DialectProfile profile{
      "common_enterprise_readiness_test",
      "Common Enterprise Readiness Test",
      "sbp_common_enterprise_readiness_test",
      "sbu_common_enterprise_readiness_test",
      "test_only",
      "COMMON",
      "sblr.donor.common.enterprise_readiness_test.v1",
      kPatterns,
      kNoSurfaces,
      kNoSurfaces,
      kNoSurfaces,
      kNoSurfaces,
      5,
      1,
      0,
      0,
      0,
      1,
      3,
      1,
      1,
  };
  return profile;
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool Expect(bool condition, std::string_view message) {
  if (condition) return true;
  std::cerr << message << '\n';
  return false;
}

bool ExpectField(std::string_view envelope,
                 std::string_view field,
                 std::string_view value) {
  return Expect(
      Contains(envelope, "\"" + std::string(field) + "\":\"" + std::string(value) + "\""),
      "missing or mismatched enterprise readiness field: " + std::string(field));
}

bool ExpectNoEnterpriseClaim(std::string_view envelope,
                             std::string_view field) {
  return Expect(
      !Contains(envelope, "\"" + std::string(field) +
                              "\":\"enterprise_implemented_proven\""),
      "field reused enterprise implementation proof marker: " + std::string(field));
}

bool ExpectReadinessSection(std::string_view envelope) {
  bool ok = true;
  ok &= Expect(Contains(envelope, "\"enterprise_readiness_evidence\":{"),
               "missing enterprise readiness evidence section");
  ok &= ExpectField(envelope, "evidence_contract",
                    "donor_parser_enterprise_readiness_evidence.v1");
  ok &= ExpectField(envelope, "completion_claim", "not_enterprise_ready");
  ok &= Expect(Contains(envelope, "\"enterprise_implemented_proven\":false"),
               "readiness section missing enterprise implementation proof");

  ok &= ExpectField(envelope, "procedural_body_encoding_status",
                    "route_and_descriptor_only_not_enterprise");
  ok &= ExpectField(envelope, "datatype_exactness_status",
                    "surface_cataloged_exactness_proof_pending");
  ok &= ExpectField(envelope, "semantic_defaults_status",
                    "semantic_profile_proof_pending");
  ok &= ExpectField(envelope, "observable_equivalence_status",
                    "donor_native_equivalence_proof_pending");
  ok &= ExpectField(envelope, "donor_native_regression_status",
                    "donor_native_regression_proof_pending");
  ok &= ExpectField(envelope, "sandbox_scope_status",
                    "admitted_policy_gate_present_runtime_proof_pending");
  ok &= ExpectField(envelope, "cluster_surface_routing_status",
                    "route_or_fail_closed_policy_gate_not_enterprise");
  ok &= ExpectField(envelope, "logical_stream_backup_restore_status",
                    "policy_matrix_gate_present_stream_runtime_proof_pending");
  ok &= ExpectField(envelope, "cdc_replication_etl_status",
                    "parser_support_udr_policy_gate_route_only_not_enterprise");
  ok &= ExpectField(envelope, "low_level_repair_verify_status",
                    "fail_closed_policy_denial_present_runtime_proof_pending");

  constexpr std::string_view kCompletedStatusFields[] = {
      "procedural_body_encoding_status",
      "datatype_exactness_status",
      "semantic_defaults_status",
      "observable_equivalence_status",
      "donor_native_regression_status",
      "cluster_surface_routing_status",
      "logical_stream_backup_restore_status",
      "cdc_replication_etl_status",
  };
  for (const auto field : kCompletedStatusFields) {
    ok &= ExpectNoEnterpriseClaim(envelope, field);
  }
  return ok;
}

bool ExpectRoute(std::string_view sql,
                 std::string_view mapping_disposition,
                 bool fail_closed_refusal) {
  const auto result = ParseStatement(sql, TestProfile());
  bool ok = true;
  ok &= Expect(result.ok, "common parser did not route statement: " + std::string(sql));
  ok &= Expect(result.fail_closed_refusal == fail_closed_refusal,
               "fail_closed_refusal mismatch for statement: " + std::string(sql));
  ok &= Expect(Contains(result.sblr_envelope, "\"mapping_disposition\":\"" +
                                             std::string(mapping_disposition) + "\""),
               "mapping disposition missing from envelope for statement: " +
                   std::string(sql));
  ok &= Expect(Contains(result.sblr_envelope,
                        "\"fail_closed_refusal\":" +
                            std::string(fail_closed_refusal ? "true" : "false")),
               "fail_closed_refusal field missing from envelope for statement: " +
                   std::string(sql));
  ok &= Expect(Contains(result.sblr_envelope,
                        "\"parser_evidence\":{\"dialect\":\"common_enterprise_readiness_test\""),
               "common envelope lost parser evidence for statement: " +
                   std::string(sql));
  ok &= ExpectReadinessSection(result.sblr_envelope);
  return ok;
}

} // namespace

int main() {
  bool ok = true;
  ok &= ExpectRoute("select * from t", "admitted_sblr", false);
  ok &= ExpectRoute("change replication source to source_host = 'db'",
                    "parser_support_udr", false);
  ok &= ExpectRoute("load data infile '/server/path' into table t",
                    "policy_refusal_fail_closed", true);
  ok &= ExpectRoute("grant select on t to u",
                    "security_refusal_fail_closed", true);
  ok &= ExpectRoute("repair table t",
                    "unsupported_refusal_fail_closed", true);
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
