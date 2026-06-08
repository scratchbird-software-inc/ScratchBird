// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "security/auth_provider_plugin_api.hpp"

#include <iostream>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace api = scratchbird::engine::internal_api;

namespace {
constexpr const char* kFuture = "4102444800000";
constexpr const char* kSig = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

api::EngineRequestContext Context() {
  api::EngineRequestContext context;
  context.database_path = "/tmp/sb_auth_provider_live_adapter_probe.sbdb";
  context.local_transaction_id = 700;
  context.security_context_present = true;
  context.database_uuid.canonical = "018f0000-0000-7000-8000-0000000c0001";
  context.session_uuid.canonical = "018f0000-0000-7000-8000-0000000c0002";
  context.principal_uuid.canonical = "018f0000-0000-7000-8000-0000000c0003";
  context.trace_tags = {"right:AUTH_PROVIDER_ADMIN", "right:OBS_METRICS_READ_FAMILY", "security.bootstrap"};
  return context;
}

std::string DependencyForProvider(const std::string& provider) {
  if (provider == "ldap_ad") { return "ldap_client"; }
  if (provider == "kerberos_pac") { return "gssapi_krb5"; }
  if (provider == "pam") { return "pam"; }
  if (provider == "radius") { return "radius_client"; }
  if (provider == "oidc_jwt") { return "oidc_jwt_client"; }
  if (provider == "saml") { return "saml_xmlsig"; }
  if (provider == "webauthn") { return "webauthn_fido2"; }
  if (provider == "workload_identity") { return "spiffe_svid_or_workload_oidc"; }
  if (provider == "certificate_mtls") { return "tls_x509"; }
  if (provider == "proxy_assertion") { return "proxy_assertion_verifier"; }
  return {};
}

api::EngineAuthenticateProviderResult LiveAuth(const std::string& provider,
                                               const std::string& payload,
                                               const std::string& principal = {},
                                               const std::vector<std::string>& extra_options = {}) {
  api::EngineAuthenticateProviderRequest request;
  request.context = Context();
  request.target_object.uuid.canonical = "018f0000-0000-7000-8000-0000000c0100";
  request.option_envelopes.push_back("provider:" + provider);
  request.option_envelopes.push_back("adapter_mode:live");
  request.option_envelopes.push_back("provider_payload:" + payload);
  if (!principal.empty()) { request.option_envelopes.push_back("principal:" + principal); }
  for (const auto& option : extra_options) { request.option_envelopes.push_back(option); }
  return api::EngineAuthenticateProvider(request);
}

api::EngineAuthenticateProviderResult RealExternalAuth(const std::string& provider, const std::string& payload, const std::string& principal = {}) {
  const std::string dependency = DependencyForProvider(provider);
  return LiveAuth(provider,
                  payload + ";client_result=success",
                  principal,
                  {"client_mode:external",
                   "parser_attach_provider:true",
                   "parser_policy_requires_provider:" + provider,
                   "dependency:" + dependency + ":available"});
}

int Finish(std::initializer_list<std::pair<std::string, bool>> checks) {
  bool ok = true;
  std::cout << "{";
  bool first = true;
  for (const auto& check : checks) {
    ok = ok && check.second;
    if (!first) { std::cout << ","; }
    std::cout << "\"" << check.first << "\":" << (check.second ? "true" : "false");
    first = false;
  }
  if (!first) { std::cout << ","; }
  std::cout << "\"ok\":" << (ok ? "true" : "false") << "}\n";
  return ok ? 0 : 1;
}

}  // namespace

int main() {
  const auto ldap = LiveAuth("ldap_ad", "user=alice;password=secret;endpoint=ldap.example:636;starttls=true;bind=allow;groups=CN_DBA;path=alice,DBA");
  const auto ldap_bad = LiveAuth("ldap_ad", "user=alice;password=secret;endpoint=ldap.example:389;starttls=false;bind=allow;groups=CN_DBA");
  const auto kerberos = LiveAuth("kerberos_pac", std::string("spn=scratchbird/db@EXAMPLE;subject=alice@EXAMPLE;nonce=n1;kdc=kdc.example;pac_groups=DBA;exp=") + kFuture + ";sig=" + kSig);
  const auto pam = LiveAuth("pam", "service=scratchbird;module=pam_unix;password=secret;prompt=hidden;account=allow;session=open");
  const auto radius = LiveAuth("radius", std::string("password=secret;authenticator=") + kSig + ";result=accept;attribute=Filter-Id:DBA", "alice");
  const auto oidc = LiveAuth("oidc_jwt", std::string("iss=issuer;aud=scratchbird;sub=alice;alg=HS256;groups=DBA;exp=") + kFuture + ";sig=" + kSig);
  const auto oidc_bad = LiveAuth("oidc_jwt", std::string("iss=issuer;aud=scratchbird;sub=alice;alg=none;groups=DBA;exp=1;sig=") + kSig);
  const auto saml = LiveAuth("saml", std::string("issuer=idp;audience=scratchbird;subject=alice;attributes=DBA;not_on_or_after=") + kFuture + ";signature=" + kSig);
  const auto webauthn = LiveAuth("webauthn", std::string("challenge=c1;rp=scratchbird;origin=login.example;credential=cred1;subject=alice;uv=true;exp=") + kFuture + ";signature=" + kSig);
  const auto workload = LiveAuth("workload_identity", std::string("spiffe_id=spiffe://example/service;trust_bundle=bundle1;service_group=svc;exp=") + kFuture + ";sig=" + kSig);
  const auto mtls = LiveAuth("certificate_mtls", std::string("subject=CN=alice;san=URI:spiffe://example/alice;chain=trusted;revoked=false;eku=clientAuth;group=DBA;fingerprint=") + kSig);
  const auto proxy = LiveAuth("proxy_assertion", std::string("iss=proxy;sub=alice;aud=scratchbird;proxy=edge1;source=trusted;groups=DBA;exp=") + kFuture + ";sig=" + kSig);
  const auto proxy_bad = LiveAuth("proxy_assertion", std::string("iss=proxy;sub=alice;aud=scratchbird;proxy=edge1;source=untrusted;groups=DBA;exp=") + kFuture + ";sig=" + kSig);
  const auto real_ldap = RealExternalAuth("ldap_ad", "user=alice;password=secret;endpoint=ldap.example:636;starttls=true;bind=allow;groups=CN_DBA;path=alice,DBA");
  const auto real_kerberos = RealExternalAuth("kerberos_pac", std::string("spn=scratchbird/db@EXAMPLE;subject=alice@EXAMPLE;nonce=n1;kdc=kdc.example;pac_groups=DBA;exp=") + kFuture + ";sig=" + kSig);
  const auto real_pam = RealExternalAuth("pam", "service=scratchbird;module=pam_unix;password=secret;prompt=hidden;account=allow;session=open");
  const auto real_radius = RealExternalAuth("radius", std::string("password=secret;authenticator=") + kSig + ";result=accept;attribute=Filter-Id:DBA", "alice");
  const auto real_oidc = RealExternalAuth("oidc_jwt", std::string("iss=issuer;aud=scratchbird;sub=alice;alg=HS256;groups=DBA;exp=") + kFuture + ";sig=" + kSig);
  const auto real_saml = RealExternalAuth("saml", std::string("issuer=idp;audience=scratchbird;subject=alice;attributes=DBA;not_on_or_after=") + kFuture + ";signature=" + kSig);
  const auto real_webauthn = RealExternalAuth("webauthn", std::string("challenge=c1;rp=scratchbird;origin=login.example;credential=cred1;subject=alice;uv=true;exp=") + kFuture + ";signature=" + kSig);
  const auto real_workload = RealExternalAuth("workload_identity", std::string("spiffe_id=spiffe://example/service;trust_bundle=bundle1;service_group=svc;exp=") + kFuture + ";sig=" + kSig);
  const auto real_mtls = RealExternalAuth("certificate_mtls", std::string("subject=CN=alice;san=URI:spiffe://example/alice;chain=trusted;revoked=false;eku=clientAuth;group=DBA;fingerprint=") + kSig);
  const auto real_proxy = RealExternalAuth("proxy_assertion", std::string("iss=proxy;sub=alice;aud=scratchbird;proxy=edge1;source=trusted;groups=DBA;exp=") + kFuture + ";sig=" + kSig);
  const auto parser_attach_no_policy = LiveAuth("ldap_ad",
                                                "user=alice;password=secret;endpoint=ldap.example:636;starttls=true;bind=allow;groups=CN_DBA",
                                                "",
                                                {"parser_attach_provider:true", "dependency:ldap_client:available"});
  const auto parser_attach_mismatch = LiveAuth("ldap_ad",
                                               "user=alice;password=secret;endpoint=ldap.example:636;starttls=true;bind=allow;groups=CN_DBA",
                                               "",
                                               {"parser_attach_provider:true", "parser_policy_requires_provider:oidc_jwt", "dependency:ldap_client:available"});
  const auto parser_missing_dependency = LiveAuth("ldap_ad",
                                                  "user=alice;password=secret;endpoint=ldap.example:636;starttls=true;bind=allow;groups=CN_DBA",
                                                  "",
                                                  {"parser_attach_provider:true", "parser_policy_requires_provider:ldap_ad"});
  const auto external_missing_success = LiveAuth("ldap_ad",
                                                 "user=alice;password=secret;endpoint=ldap.example:636;starttls=true;bind=allow;groups=CN_DBA",
                                                 "",
                                                 {"client_mode:external", "parser_attach_provider:true", "parser_policy_requires_provider:ldap_ad", "dependency:ldap_client:available"});
  const auto external_service_down = LiveAuth("ldap_ad",
                                              "user=alice;password=secret;endpoint=ldap.example:636;starttls=true;bind=allow;groups=CN_DBA;client_result=success;external_service_unavailable=true",
                                              "",
                                              {"client_mode:external", "parser_attach_provider:true", "parser_policy_requires_provider:ldap_ad", "dependency:ldap_client:available"});
  const auto duplicate_key = LiveAuth("ldap_ad", "user=alice;user=bob;password=secret;endpoint=ldap.example:636;starttls=true;bind=allow;groups=CN_DBA");
  const auto unsafe_control = LiveAuth("ldap_ad", std::string("user=alice\npassword=secret;endpoint=ldap.example:636;starttls=true;bind=allow;groups=CN_DBA"));
  const auto oversized_payload = LiveAuth("ldap_ad", std::string(8200, 'a'));
  const auto replayed = LiveAuth("oidc_jwt", std::string("iss=issuer;aud=scratchbird;sub=alice;alg=HS256;groups=DBA;replay=true;exp=") + kFuture + ";sig=" + kSig);
  const auto bad_sig = LiveAuth("oidc_jwt", std::string("iss=issuer;aud=scratchbird;sub=alice;alg=HS256;groups=DBA;exp=") + kFuture + ";sig=bad");
  const auto weak_alg = LiveAuth("oidc_jwt", std::string("iss=issuer;aud=scratchbird;sub=alice;alg=md5;groups=DBA;exp=") + kFuture + ";sig=" + kSig);
  const auto mtls_missing_group = LiveAuth("certificate_mtls", std::string("subject=CN=alice;san=URI:spiffe://example/alice;chain=trusted;revoked=false;eku=clientAuth;fingerprint=") + kSig);

  return Finish({
      {"ldap_live", ldap.ok && ldap.authenticated},
      {"ldap_starttls_denied", !ldap_bad.ok},
      {"kerberos_live", kerberos.ok && kerberos.authenticated},
      {"pam_live", pam.ok && pam.authenticated},
      {"radius_live", radius.ok && radius.authenticated},
      {"oidc_live", oidc.ok && oidc.authenticated},
      {"oidc_bad_denied", !oidc_bad.ok},
      {"saml_live", saml.ok && saml.authenticated},
      {"webauthn_live", webauthn.ok && webauthn.authenticated},
      {"workload_live", workload.ok && workload.authenticated},
      {"mtls_live", mtls.ok && mtls.authenticated},
      {"proxy_live", proxy.ok && proxy.authenticated},
      {"proxy_untrusted_denied", !proxy_bad.ok},
      {"real_ldap_client_gate", real_ldap.ok && real_ldap.authenticated},
      {"real_kerberos_client_gate", real_kerberos.ok && real_kerberos.authenticated},
      {"real_pam_client_gate", real_pam.ok && real_pam.authenticated},
      {"real_radius_client_gate", real_radius.ok && real_radius.authenticated},
      {"real_oidc_client_gate", real_oidc.ok && real_oidc.authenticated},
      {"real_saml_client_gate", real_saml.ok && real_saml.authenticated},
      {"real_webauthn_client_gate", real_webauthn.ok && real_webauthn.authenticated},
      {"real_workload_client_gate", real_workload.ok && real_workload.authenticated},
      {"real_mtls_client_gate", real_mtls.ok && real_mtls.authenticated},
      {"real_proxy_client_gate", real_proxy.ok && real_proxy.authenticated},
      {"parser_attach_no_policy_denied", !parser_attach_no_policy.ok},
      {"parser_attach_mismatch_denied", !parser_attach_mismatch.ok},
      {"parser_missing_dependency_denied", !parser_missing_dependency.ok},
      {"external_missing_success_denied", !external_missing_success.ok},
      {"external_service_down_denied", !external_service_down.ok},
      {"duplicate_key_denied", !duplicate_key.ok},
      {"unsafe_control_denied", !unsafe_control.ok},
      {"oversized_payload_denied", !oversized_payload.ok},
      {"replayed_denied", !replayed.ok},
      {"bad_sig_denied", !bad_sig.ok},
      {"weak_alg_denied", !weak_alg.ok},
      {"mtls_missing_group_denied", !mtls_missing_group.ok},
  });
}
