// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Row {
  std::string provider;
  std::string dependency;
  std::string service_fixture;
  std::string trust_material;
  std::string negative_fixture_requirements;
  std::string missing_dependency_policy;
  std::string status;
  std::string notes;
};

std::vector<std::string> SplitCsvLine(const std::string& line) {
  std::vector<std::string> out;
  std::string field;
  bool quoted = false;
  for (char c : line) {
    if (c == '"') {
      quoted = !quoted;
    } else if (c == ',' && !quoted) {
      out.push_back(field);
      field.clear();
    } else {
      field.push_back(c);
    }
  }
  out.push_back(field);
  return out;
}

bool NonEmpty(const std::string& value) { return !value.empty(); }

bool AllowedPolicy(const std::string& value) {
  return value == "fail" || value == "skip_with_evidence" || value == "unsupported_on_host";
}

std::string FindInventoryPath(int argc, char** argv) {
  if (argc > 1 && argv[1] && *argv[1]) { return argv[1]; }
  const char* explicit_path = std::getenv("SB_AUTH_PROVIDER_DEPENDENCY_INVENTORY");
  if (explicit_path && *explicit_path) { return explicit_path; }
  std::filesystem::path cursor = std::filesystem::absolute(argc > 0 ? argv[0] : "").parent_path();
  for (int i = 0; i < 8 && !cursor.empty(); ++i) {
    auto candidate = cursor / "fixtures" / "AUTH_PROVIDER_DEPENDENCY_INVENTORY.csv";
    if (std::filesystem::exists(candidate)) { return candidate.string(); }
    candidate = cursor / "project" / "tools" / "sb_auth_provider_dependency_inventory_probe" / "fixtures" /
                "AUTH_PROVIDER_DEPENDENCY_INVENTORY.csv";
    if (std::filesystem::exists(candidate)) { return candidate.string(); }
    cursor = cursor.parent_path();
  }
  return "AUTH_PROVIDER_DEPENDENCY_INVENTORY.csv";
}

int Finish(const std::map<std::string, bool>& checks) {
  bool ok = true;
  std::cout << "{";
  bool first = true;
  for (const auto& [name, value] : checks) {
    ok = ok && value;
    if (!first) { std::cout << ","; }
    std::cout << "\"" << name << "\":" << (value ? "true" : "false");
    first = false;
  }
  if (!first) { std::cout << ","; }
  std::cout << "\"ok\":" << (ok ? "true" : "false") << "}\n";
  return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string path = FindInventoryPath(argc, argv);
  std::ifstream file(path);
  std::vector<Row> rows;
  std::string line;
  bool header_ok = false;
  if (std::getline(file, line)) {
    header_ok = line == "provider,required_client_dependency,required_service_fixture,required_trust_material,negative_fixture_requirements,missing_dependency_policy,status,notes";
  }
  while (std::getline(file, line)) {
    if (line.empty()) { continue; }
    const auto fields = SplitCsvLine(line);
    if (fields.size() != 8) { continue; }
    rows.push_back({fields[0], fields[1], fields[2], fields[3], fields[4], fields[5], fields[6], fields[7]});
  }

  const std::map<std::string, std::string> expected_dependencies = {
      {"ldap_ad", "ldap_client"},
      {"kerberos_pac", "gssapi_krb5"},
      {"pam", "pam"},
      {"radius", "radius_client"},
      {"oidc_jwt", "oidc_jwt_client"},
      {"saml", "saml_xmlsig"},
      {"webauthn", "webauthn_fido2"},
      {"workload_identity", "spiffe_svid_or_workload_oidc"},
      {"certificate_mtls", "tls_x509"},
      {"proxy_assertion", "proxy_assertion_verifier"},
  };

  std::map<std::string, Row> by_provider;
  bool no_duplicate = true;
  bool fields_complete = true;
  bool policy_vocab = true;
  bool status_not_claiming_complete = true;
  for (const auto& row : rows) {
    no_duplicate = no_duplicate && by_provider.emplace(row.provider, row).second;
    fields_complete = fields_complete && NonEmpty(row.provider) && NonEmpty(row.dependency) && NonEmpty(row.service_fixture) &&
                      NonEmpty(row.trust_material) && NonEmpty(row.negative_fixture_requirements) && NonEmpty(row.notes);
    policy_vocab = policy_vocab && AllowedPolicy(row.missing_dependency_policy);
    status_not_claiming_complete = status_not_claiming_complete && row.status != "complete" && row.status != "validated";
  }

  bool expected_providers = by_provider.size() == expected_dependencies.size();
  bool dependency_names = true;
  bool hardware_skip_bounded = true;
  for (const auto& [provider, dependency] : expected_dependencies) {
    const auto it = by_provider.find(provider);
    expected_providers = expected_providers && it != by_provider.end();
    if (it != by_provider.end()) {
      dependency_names = dependency_names && it->second.dependency == dependency;
      if (it->second.missing_dependency_policy == "skip_with_evidence") {
        hardware_skip_bounded = hardware_skip_bounded && it->second.notes.find("evidence") != std::string::npos;
      }
    }
  }

  return Finish({
      {"dependency_inventory_readable", file.good() || !rows.empty()},
      {"header_ok", header_ok},
      {"expected_provider_rows", expected_providers},
      {"no_duplicate_provider_rows", no_duplicate},
      {"dependency_names_match_contract", dependency_names},
      {"required_fields_complete", fields_complete},
      {"missing_dependency_policy_vocab", policy_vocab},
      {"skip_requires_evidence", hardware_skip_bounded},
      {"inventory_does_not_claim_validation", status_not_claiming_complete},
  });
}
