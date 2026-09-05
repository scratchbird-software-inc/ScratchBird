// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "security/auth_provider_contract_adapter.hpp"

#include "api_diagnostics.hpp"
#include "security/auth_provider_model.hpp"
#include "security/security_model.hpp"

#include <cctype>
#include <set>
#include <string_view>

namespace scratchbird::engine::internal_api {
namespace {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_AUTH_PROVIDER_CONTRACT_ADAPTER_BEHAVIOR
constexpr std::size_t kMaxPayloadBytes = 8192;
constexpr std::size_t kMaxPayloadFields = 64;
constexpr std::size_t kMaxKeyBytes = 64;
constexpr std::size_t kMaxValueBytes = 2048;

bool StartsWith(const std::string& value, const std::string& prefix) {
  return value.rfind(prefix, 0) == 0;
}

std::string OptionValue(const EngineApiRequest& request,
                        const std::string& prefix) {
  for (const auto& option : request.option_envelopes) {
    if (StartsWith(option, prefix)) { return option.substr(prefix.size()); }
  }
  return {};
}

bool OptionPresent(const EngineApiRequest& request, const std::string& value) {
  for (const auto& option : request.option_envelopes) {
    if (option == value) { return true; }
  }
  return false;
}

bool IsSafePayloadByte(unsigned char value) {
  return value >= 0x20 && value <= 0x7e && value != '"' && value != '\\' &&
         value != '`';
}

bool IsKeyCharacter(char value) {
  const auto byte = static_cast<unsigned char>(value);
  return std::isalnum(byte) || value == '_' || value == '-' || value == '.' ||
         value == ':' || value == '/' || value == '@';
}

AuthProviderContractEvidenceResult Deny(const EngineApiRequest& request,
                                        std::string detail) {
  AuthProviderContractEvidenceResult result;
  result.evaluated = true;
  result.provider_family =
      CanonicalAuthProviderFamily(OptionValue(request, "provider:"));
  result.diagnostic = MakeSecurityDiagnostic(
      "SECURITY.AUTHENTICATION.REQUEST_INVALID", std::move(detail));
  result.rows.push_back({"contract_adapter", "deny"});
  result.rows.push_back({"authentication_authority", "none"});
  return result;
}

bool ValidatePayloadSyntax(std::string_view payload, std::string* detail) {
  if (payload.empty() || payload.size() > kMaxPayloadBytes) {
    if (detail) { *detail = "provider_payload_unsafe_or_oversized"; }
    return false;
  }
  for (const unsigned char value : payload) {
    if (!IsSafePayloadByte(value)) {
      if (detail) { *detail = "provider_payload_unsafe_or_oversized"; }
      return false;
    }
  }

  std::set<std::string> keys;
  std::size_t cursor = 0;
  while (cursor < payload.size()) {
    const std::size_t separator = payload.find(';', cursor);
    const std::size_t end =
        separator == std::string_view::npos ? payload.size() : separator;
    if (end <= cursor || keys.size() >= kMaxPayloadFields) {
      if (detail) { *detail = "provider_payload_segment_invalid"; }
      return false;
    }

    const std::string_view field = payload.substr(cursor, end - cursor);
    const std::size_t equals = field.find('=');
    if (equals == std::string_view::npos || equals == 0 ||
        equals + 1 >= field.size() || equals > kMaxKeyBytes ||
        field.size() - equals - 1 > kMaxValueBytes) {
      if (detail) { *detail = "provider_payload_field_invalid"; }
      return false;
    }
    for (const char value : field.substr(0, equals)) {
      if (!IsKeyCharacter(value)) {
        if (detail) { *detail = "provider_payload_key_invalid"; }
        return false;
      }
    }
    if (!keys.emplace(field.substr(0, equals)).second) {
      if (detail) { *detail = "provider_payload_duplicate_key"; }
      return false;
    }

    if (separator == std::string_view::npos) { break; }
    if (separator + 1 == payload.size()) {
      if (detail) { *detail = "provider_payload_segment_invalid"; }
      return false;
    }
    cursor = separator + 1;
  }
  return !keys.empty();
}

}  // namespace

AuthProviderContractEvidenceResult ValidateAuthProviderContractEvidence(
    const EngineApiRequest& request) {
  const std::string payload = OptionValue(request, "provider_payload:");
  const bool contract_requested =
      OptionPresent(request, "adapter_mode:contract") ||
      OptionPresent(request, "adapter_mode:live") ||
      OptionPresent(request, "provider_driver:live") || !payload.empty();
  if (!contract_requested) { return {}; }
  if (payload.empty()) { return Deny(request, "provider_payload_required"); }

  std::string detail;
  if (!ValidatePayloadSyntax(payload, &detail)) {
    return Deny(request, std::move(detail));
  }

  AuthProviderContractEvidenceResult result;
  result.evaluated = true;
  result.ok = true;
  result.provider_family =
      CanonicalAuthProviderFamily(OptionValue(request, "provider:"));
  result.diagnostic =
      MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  result.evidence.push_back(
      {"auth_provider_contract_adapter", result.provider_family});
  result.rows.push_back({"contract_adapter", "validated"});
  result.rows.push_back({"provider_payload_authority", "untrusted"});
  result.rows.push_back({"authentication_authority", "none"});
  return result;
}

}  // namespace scratchbird::engine::internal_api
