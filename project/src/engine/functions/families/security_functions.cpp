// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "families/security_functions.hpp"

#include "common/function_result_helpers.hpp"

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>

namespace scratchbird::engine::functions {
namespace {

std::string LowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

std::string Hex(const unsigned char* data, unsigned int size) {
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (unsigned int i = 0; i < size; ++i) out << std::setw(2) << static_cast<unsigned int>(data[i]);
  return out.str();
}

FunctionCallResult DispatchCryptoHmac(const FunctionCallRequest& request) {
  if (request.arguments.size() < 2 || request.arguments.size() > 3) {
    return RefuseFunctionInvalidInput(request, "sb_crypto_hmac expects key, message, and optional algorithm");
  }
  if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) {
    return MakeFunctionSuccess(request, {MakeNullValue("binary_hex")});
  }
  const std::string algorithm = request.arguments.size() == 3 ? LowerAscii(ValueAsText(request.arguments[2].value)) : "hmac-sha256";
  if (algorithm != "hmac-sha256" && algorithm != "sha256" && algorithm != "hmac_sha256") {
    return RefuseFunctionInvalidInput(request, "sb_crypto_hmac only admits hmac-sha256 in the public engine surface");
  }
  const std::string key = ValueAsText(request.arguments[0].value);
  const std::string message = ValueAsText(request.arguments[1].value);
  if (key.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return RefuseFunctionInvalidInput(request, "sb_crypto_hmac key exceeds OpenSSL HMAC length bounds");
  }
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int digest_size = 0;
  const unsigned char* result = HMAC(EVP_sha256(),
                                    key.data(),
                                    static_cast<int>(key.size()),
                                    reinterpret_cast<const unsigned char*>(message.data()),
                                    message.size(),
                                    digest.data(),
                                    &digest_size);
  if (result == nullptr) {
    return RefuseFunctionWithDiagnostic(request,
                                        scratchbird::engine::sblr::SblrStatusCode::dependency_unavailable,
                                        "SB_DIAG_SECURITY_HMAC_PROVIDER_UNAVAILABLE",
                                        "OpenSSL HMAC provider refused hmac-sha256 execution");
  }
  return MakeFunctionSuccess(request, {MakeTextValue("binary_hex", Hex(digest.data(), digest_size))});
}

}  // namespace

bool IsSecurityFunction(const FunctionCallRequest& request) {
  return request.context.function_id.rfind("sb.fn.security.", 0) == 0 ||
         request.context.function_id.rfind("security.", 0) == 0;
}

FunctionCallResult DispatchSecurityFunction(const FunctionCallRequest& request) {
  const auto& id = request.context.function_id;
  if (id == "sb.fn.security.sb_crypto_hmac" || id == "security.sb_crypto_hmac") {
    return DispatchCryptoHmac(request);
  }
  if (id == "sb.fn.security.sb_connector_external_query" || id == "security.sb_connector_external_query") {
    return RefuseFunctionWithDiagnostic(request,
                                        scratchbird::engine::sblr::SblrStatusCode::policy_refused,
                                        "SB_DIAG_SECURITY_CONNECTOR_EXTERNAL_QUERY_POLICY_REQUIRED",
                                        "external connector query execution requires connector policy, materialized authorization, and provider admission");
  }
  return RefuseFunctionWithDiagnostic(request,
                                      scratchbird::engine::sblr::SblrStatusCode::unsupported_feature,
                                      "SB_DIAG_SECURITY_FUNCTION_UNHANDLED",
                                      "security helper id is not handled by the activated security surface");
}

}  // namespace scratchbird::engine::functions
