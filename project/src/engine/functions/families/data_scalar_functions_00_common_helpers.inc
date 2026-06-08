// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SBSQL source-layout fragment. Included only by data_scalar_functions.cpp.
// This fragment preserves pre-existing behavior; do not add unrelated future work here.
#include "families/data_scalar_functions.hpp"

#include "common/function_result_helpers.hpp"
#include "api_types.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "security/security_model.hpp"
#include "sblr/sblr_domain_runtime.hpp"
#include "sblr/sblr_sequence_runtime.hpp"
#include "uuid.hpp"

#include <array>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <cmath>
#include <iomanip>
#include <initializer_list>
#include <limits>
#include <optional>
#include <random>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <unicode/uchar.h>
#include <unicode/unorm2.h>
#include <unicode/ustring.h>
#include <unicode/utf16.h>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace scratchbird::engine::functions {
namespace {

std::string LowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

std::string UpperAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  return value;
}

bool Utf8ToUtf16(std::string_view input,
                 std::vector<UChar>* output,
                 std::string* error) {
  if (output == nullptr) return false;
  output->clear();
  UErrorCode status = U_ZERO_ERROR;
  int32_t required = 0;
  u_strFromUTF8(nullptr,
                0,
                &required,
                input.data(),
                static_cast<int32_t>(input.size()),
                &status);
  if (status != U_BUFFER_OVERFLOW_ERROR && status != U_STRING_NOT_TERMINATED_WARNING &&
      U_FAILURE(status)) {
    if (error) *error = "invalid UTF-8 text";
    return false;
  }
  status = U_ZERO_ERROR;
  output->assign(static_cast<std::size_t>(required), 0);
  u_strFromUTF8(output->data(),
                required,
                nullptr,
                input.data(),
                static_cast<int32_t>(input.size()),
                &status);
  if (U_FAILURE(status)) {
    output->clear();
    if (error) *error = "invalid UTF-8 text";
    return false;
  }
  return true;
}

bool Utf16ToUtf8(const std::vector<UChar>& input,
                 std::string* output,
                 std::string* error) {
  if (output == nullptr) return false;
  output->clear();
  UErrorCode status = U_ZERO_ERROR;
  int32_t required = 0;
  u_strToUTF8(nullptr,
              0,
              &required,
              input.data(),
              static_cast<int32_t>(input.size()),
              &status);
  if (status != U_BUFFER_OVERFLOW_ERROR && status != U_STRING_NOT_TERMINATED_WARNING &&
      U_FAILURE(status)) {
    if (error) *error = "invalid UTF-16 text";
    return false;
  }
  status = U_ZERO_ERROR;
  output->assign(static_cast<std::size_t>(required), '\0');
  u_strToUTF8(output->data(),
              required,
              nullptr,
              input.data(),
              static_cast<int32_t>(input.size()),
              &status);
  if (U_FAILURE(status)) {
    output->clear();
    if (error) *error = "invalid UTF-16 text";
    return false;
  }
  return true;
}

const UNormalizer2* UnicodeNormalizerForForm(std::string form,
                                             std::string* canonical_form,
                                             std::string* error) {
  const auto first = std::find_if(form.begin(), form.end(), [](unsigned char ch) {
    return !std::isspace(ch);
  });
  const auto last = std::find_if(form.rbegin(), form.rend(), [](unsigned char ch) {
    return !std::isspace(ch);
  }).base();
  form = first < last ? std::string(first, last) : std::string();
  form = UpperAscii(std::move(form));
  if (form.empty()) form = "NFC";
  UErrorCode status = U_ZERO_ERROR;
  const UNormalizer2* normalizer = nullptr;
  if (form == "NFC") {
    normalizer = unorm2_getNFCInstance(&status);
  } else if (form == "NFD") {
    normalizer = unorm2_getNFDInstance(&status);
  } else if (form == "NFKC") {
    normalizer = unorm2_getNFKCInstance(&status);
  } else if (form == "NFKD") {
    normalizer = unorm2_getNFKDInstance(&status);
  } else {
    if (error) *error = "unicode normalization form must be NFC, NFD, NFKC, or NFKD";
    return nullptr;
  }
  if (U_FAILURE(status) || normalizer == nullptr) {
    if (error) *error = "unicode normalizer is unavailable";
    return nullptr;
  }
  if (canonical_form) *canonical_form = form;
  return normalizer;
}

bool NormalizeUnicodeText(std::string_view text,
                          std::string form,
                          std::string* output,
                          std::string* canonical_form,
                          std::string* error) {
  const UNormalizer2* normalizer =
      UnicodeNormalizerForForm(std::move(form), canonical_form, error);
  if (normalizer == nullptr || output == nullptr) return false;
  std::vector<UChar> utf16;
  if (!Utf8ToUtf16(text, &utf16, error)) return false;
  UErrorCode status = U_ZERO_ERROR;
  int32_t required = unorm2_normalize(normalizer,
                                      utf16.data(),
                                      static_cast<int32_t>(utf16.size()),
                                      nullptr,
                                      0,
                                      &status);
  if (status != U_BUFFER_OVERFLOW_ERROR && U_FAILURE(status)) {
    if (error) *error = "unicode normalization failed";
    return false;
  }
  status = U_ZERO_ERROR;
  std::vector<UChar> normalized(static_cast<std::size_t>(required), 0);
  unorm2_normalize(normalizer,
                   utf16.data(),
                   static_cast<int32_t>(utf16.size()),
                   normalized.data(),
                   required,
                   &status);
  if (U_FAILURE(status)) {
    if (error) *error = "unicode normalization failed";
    return false;
  }
  return Utf16ToUtf8(normalized, output, error);
}

bool IsUnicodeAlphaText(std::string_view text, bool* output, std::string* error) {
  if (output == nullptr) return false;
  std::vector<UChar> utf16;
  if (!Utf8ToUtf16(text, &utf16, error)) return false;
  if (utf16.empty()) {
    *output = false;
    return true;
  }
  bool saw_codepoint = false;
  int32_t index = 0;
  const int32_t length = static_cast<int32_t>(utf16.size());
  while (index < length) {
    UChar32 codepoint = 0;
    U16_NEXT(utf16.data(), index, length, codepoint);
    if (codepoint < 0 || !u_isalpha(codepoint)) {
      *output = false;
      return true;
    }
    saw_codepoint = true;
  }
  *output = saw_codepoint;
  return true;
}

bool IdIs(const std::string& id, std::initializer_list<std::string_view> candidates) {
  for (const auto candidate : candidates) {
    if (id == candidate) return true;
  }
  return false;
}

std::string LTrimAscii(std::string value) {
  value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch) {
    return !std::isspace(ch);
  }));
  return value;
}

std::string RTrimAscii(std::string value) {
  value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) {
    return !std::isspace(ch);
  }).base(), value.end());
  return value;
}

std::string TrimAscii(std::string value) {
  return LTrimAscii(RTrimAscii(std::move(value)));
}

std::uint64_t CurrentProcessIdForSblr() {
#if defined(_WIN32)
  return static_cast<std::uint64_t>(_getpid());
#else
  return static_cast<std::uint64_t>(getpid());
#endif
}

std::uint64_t EffectiveBackendProcessId(
    const scratchbird::engine::sblr::SblrExecutionContext& context) {
  return context.backend_process_id == 0 ? CurrentProcessIdForSblr()
                                         : context.backend_process_id;
}

std::string NormalizeSessionConfigName(std::string value) {
  value = LowerAscii(TrimAscii(std::move(value)));
  if (value == "time zone") return "timezone";
  return value;
}

std::optional<std::string> DefaultSessionConfigValue(std::string_view normalized_name) {
  if (normalized_name == "timezone") return std::string("UTC");
  if (normalized_name == "temp_buffers") return std::string("8388608");
  return std::nullopt;
}

std::optional<std::string> SessionConfigValue(
    const scratchbird::engine::sblr::SblrExecutionContext& context,
    std::string_view normalized_name) {
  if (context.session_runtime_state) {
    for (auto it = context.session_runtime_state->config_entries.rbegin();
         it != context.session_runtime_state->config_entries.rend();
         ++it) {
      if (NormalizeSessionConfigName(it->name) == normalized_name) return it->value;
    }
  }
  return DefaultSessionConfigValue(normalized_name);
}

void SetSessionConfigValue(
    const scratchbird::engine::sblr::SblrExecutionContext& context,
    std::string normalized_name,
    std::string value,
    bool is_local) {
  if (!context.session_runtime_state) return;
  for (auto& entry : context.session_runtime_state->config_entries) {
    if (NormalizeSessionConfigName(entry.name) != normalized_name) continue;
    entry.name = std::move(normalized_name);
    entry.value = std::move(value);
    entry.is_local = is_local;
    return;
  }
  context.session_runtime_state->config_entries.push_back(
      scratchbird::engine::sblr::SblrSessionConfigEntry{
          std::move(normalized_name), std::move(value), is_local});
}

bool BackendPidListed(const std::vector<std::uint64_t>& pids,
                      std::uint64_t pid) {
  return std::find(pids.begin(), pids.end(), pid) != pids.end();
}

void AppendBackendControlEvidence(
    const scratchbird::engine::sblr::SblrExecutionContext& context,
    std::string evidence) {
  if (context.session_runtime_state) {
    context.session_runtime_state->backend_control_evidence.push_back(std::move(evidence));
  }
}

std::string SessionAdvisoryLockOwnerToken(
    const scratchbird::engine::sblr::SblrExecutionContext& context) {
  if (!context.session_uuid.empty()) return context.session_uuid;
  return "process:" + std::to_string(EffectiveBackendProcessId(context));
}

scratchbird::engine::sblr::SblrSessionAdvisoryLockEntry* FindSessionAdvisoryLockEntry(
    scratchbird::engine::sblr::SblrSessionRuntimeState* state,
    std::int64_t key) {
  if (state == nullptr) return nullptr;
  for (auto& entry : state->advisory_lock_entries) {
    if (entry.key == key) return &entry;
  }
  return nullptr;
}

scratchbird::engine::sblr::SblrTransactionAdvisoryLockEntry* FindTransactionAdvisoryLockEntry(
    scratchbird::engine::sblr::SblrSessionRuntimeState* state,
    std::int64_t key) {
  if (state == nullptr) return nullptr;
  for (auto& entry : state->transaction_advisory_lock_entries) {
    if (entry.key == key) return &entry;
  }
  return nullptr;
}

void AppendAdvisoryLockEvidence(
    const scratchbird::engine::sblr::SblrExecutionContext& context,
    std::string evidence) {
  if (context.session_runtime_state) {
    context.session_runtime_state->advisory_lock_evidence.push_back(std::move(evidence));
  }
}

void AppendTransactionAdvisoryLockEvidence(
    const scratchbird::engine::sblr::SblrExecutionContext& context,
    std::string evidence) {
  if (context.session_runtime_state) {
    context.session_runtime_state->transaction_advisory_lock_evidence.push_back(
        std::move(evidence));
  }
}

bool TransactionAdvisoryLockOwnerToken(
    const scratchbird::engine::sblr::SblrExecutionContext& context,
    std::string* owner) {
  if (owner == nullptr || !context.transaction_context_present) return false;
  if (!context.transaction_uuid.empty()) {
    *owner = "transaction:" + context.transaction_uuid;
    return true;
  }
  if (context.local_transaction_id != 0) {
    *owner = "local_transaction_id:" + std::to_string(context.local_transaction_id);
    return true;
  }
  return false;
}

bool AcquireSessionAdvisoryLock(
    const scratchbird::engine::sblr::SblrExecutionContext& context,
    std::string_view function_name,
    std::int64_t key,
    bool* acquired) {
  if (acquired == nullptr || !context.session_runtime_state) return false;
  const auto owner = SessionAdvisoryLockOwnerToken(context);
  auto* entry = FindSessionAdvisoryLockEntry(context.session_runtime_state.get(), key);
  if (entry == nullptr) {
    context.session_runtime_state->advisory_lock_entries.push_back(
        scratchbird::engine::sblr::SblrSessionAdvisoryLockEntry{key, owner, 1});
    *acquired = true;
    AppendAdvisoryLockEvidence(context,
                               std::string(function_name) + ".acquired:" +
                                   std::to_string(key) + ":" + owner);
    return true;
  }
  if (entry->owner_session_uuid == owner) {
    ++entry->acquisition_count;
    *acquired = true;
    AppendAdvisoryLockEvidence(context,
                               std::string(function_name) + ".reentrant:" +
                                   std::to_string(key) + ":" + owner);
    return true;
  }
  *acquired = false;
  AppendAdvisoryLockEvidence(context,
                             std::string(function_name) + ".other_owner:" +
                                 std::to_string(key) + ":" + entry->owner_session_uuid);
  return true;
}

bool ReleaseSessionAdvisoryLock(
    const scratchbird::engine::sblr::SblrExecutionContext& context,
    std::string_view function_name,
    std::int64_t key,
    bool* released) {
  if (released == nullptr || !context.session_runtime_state) return false;
  const auto owner = SessionAdvisoryLockOwnerToken(context);
  auto& entries = context.session_runtime_state->advisory_lock_entries;
  for (auto it = entries.begin(); it != entries.end(); ++it) {
    if (it->key != key) continue;
    if (it->owner_session_uuid != owner) {
      *released = false;
      AppendAdvisoryLockEvidence(context,
                                 std::string(function_name) + ".other_owner:" +
                                     std::to_string(key) + ":" + it->owner_session_uuid);
      return true;
    }
    *released = true;
    if (it->acquisition_count > 1) {
      --it->acquisition_count;
      AppendAdvisoryLockEvidence(context,
                                 std::string(function_name) + ".released:" +
                                     std::to_string(key) + ":" + owner +
                                     ":remaining=" + std::to_string(it->acquisition_count));
      return true;
    }
    AppendAdvisoryLockEvidence(context,
                               std::string(function_name) + ".released_final:" +
                                   std::to_string(key) + ":" + owner);
    entries.erase(it);
    return true;
  }
  *released = false;
  AppendAdvisoryLockEvidence(context,
                             std::string(function_name) + ".not_found:" +
                                 std::to_string(key) + ":" + owner);
  return true;
}

std::int64_t AdvisoryLockKeyFromText(std::string_view lock_name) {
  std::uint64_t hash = 1469598103934665603ull;
  for (const unsigned char byte : lock_name) {
    hash ^= static_cast<std::uint64_t>(byte);
    hash *= 1099511628211ull;
  }
  return static_cast<std::int64_t>(hash & 0x7fffffffffffffffull);
}

bool AcquireTransactionAdvisoryLock(
    const scratchbird::engine::sblr::SblrExecutionContext& context,
    std::string_view function_name,
    std::int64_t key,
    bool* acquired) {
  if (acquired == nullptr || !context.session_runtime_state) return false;
  std::string owner;
  if (!TransactionAdvisoryLockOwnerToken(context, &owner)) return false;
  auto* entry = FindTransactionAdvisoryLockEntry(context.session_runtime_state.get(), key);
  if (entry == nullptr) {
    context.session_runtime_state->transaction_advisory_lock_entries.push_back(
        scratchbird::engine::sblr::SblrTransactionAdvisoryLockEntry{key, owner, 1});
    *acquired = true;
    AppendTransactionAdvisoryLockEvidence(context,
                                          std::string(function_name) + ".acquired:" +
                                              std::to_string(key) + ":" + owner);
    return true;
  }
  if (entry->owner_transaction_token == owner) {
    ++entry->acquisition_count;
    *acquired = true;
    AppendTransactionAdvisoryLockEvidence(context,
                                          std::string(function_name) + ".reentrant:" +
                                              std::to_string(key) + ":" + owner);
    return true;
  }
  *acquired = false;
  AppendTransactionAdvisoryLockEvidence(context,
                                        std::string(function_name) + ".other_owner:" +
                                            std::to_string(key) + ":" +
                                            entry->owner_transaction_token);
  return true;
}

std::string LTrimChars(std::string value, const std::string& chars) {
  value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&chars](char ch) {
    return chars.find(ch) == std::string::npos;
  }));
  return value;
}

std::string RTrimChars(std::string value, const std::string& chars) {
  value.erase(std::find_if(value.rbegin(), value.rend(), [&chars](char ch) {
    return chars.find(ch) == std::string::npos;
  }).base(), value.end());
  return value;
}

std::string TrimChars(std::string value, const std::string& chars) {
  return LTrimChars(RTrimChars(std::move(value), chars), chars);
}

std::string ReverseText(std::string value) {
  std::reverse(value.begin(), value.end());
  return value;
}

std::string InitcapText(std::string value) {
  bool start_word = true;
  for (char& ch : value) {
    const unsigned char byte = static_cast<unsigned char>(ch);
    if (std::isalnum(byte)) {
      ch = static_cast<char>(start_word ? std::toupper(byte) : std::tolower(byte));
      start_word = false;
    } else {
      start_word = true;
    }
  }
  return value;
}

std::string TranslateText(std::string input, const std::string& from, const std::string& to) {
  std::string out;
  out.reserve(input.size());
  for (const char ch : input) {
    const auto index = from.find(ch);
    if (index == std::string::npos) {
      out.push_back(ch);
    } else if (index < to.size()) {
      out.push_back(to[index]);
    }
  }
  return out;
}

bool RegexOptionsFromFlags(std::string_view flags_text,
                           bool allow_global,
                           std::regex::flag_type* regex_flags,
                           bool* global) {
  *regex_flags = std::regex::ECMAScript;
  *global = false;
  for (const char ch : flags_text) {
    switch (ch) {
      case 'i':
      case 'I':
        *regex_flags |= std::regex::icase;
        break;
      case 'g':
      case 'G':
        if (!allow_global) return false;
        *global = true;
        break;
      default:
        return false;
    }
  }
  return true;
}

std::string JsonStringEscape(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 2);
  out.push_back('"');
  for (const unsigned char ch : text) {
    switch (ch) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (ch < 0x20) {
          std::ostringstream escaped;
          escaped << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                  << static_cast<int>(ch);
          out += escaped.str();
        } else {
          out.push_back(static_cast<char>(ch));
        }
        break;
    }
  }
  out.push_back('"');
  return out;
}

std::string ValueStateName(const scratchbird::engine::sblr::SblrValue& value) {
  if (value.is_null) return "sql_null";
  switch (value.payload_kind) {
    case scratchbird::engine::sblr::SblrValuePayloadKind::boolean:
      return "boolean_value";
    case scratchbird::engine::sblr::SblrValuePayloadKind::signed_integer:
      return "signed_integer_value";
    case scratchbird::engine::sblr::SblrValuePayloadKind::unsigned_integer:
      return "unsigned_integer_value";
    case scratchbird::engine::sblr::SblrValuePayloadKind::real64:
      return "real64_value";
    case scratchbird::engine::sblr::SblrValuePayloadKind::high_precision_numeric_text:
      return "numeric_text_value";
    case scratchbird::engine::sblr::SblrValuePayloadKind::text:
      return "text_value";
    case scratchbird::engine::sblr::SblrValuePayloadKind::binary:
      return "binary_value";
    case scratchbird::engine::sblr::SblrValuePayloadKind::uuid_text:
      return "uuid_value";
    case scratchbird::engine::sblr::SblrValuePayloadKind::temporal_text:
      return "temporal_value";
    case scratchbird::engine::sblr::SblrValuePayloadKind::descriptor_payload:
      return "descriptor_value";
    case scratchbird::engine::sblr::SblrValuePayloadKind::none:
      return "untyped_value";
  }
  return "untyped_value";
}

std::string PayloadKindName(const scratchbird::engine::sblr::SblrValue& value) {
  switch (value.payload_kind) {
    case scratchbird::engine::sblr::SblrValuePayloadKind::boolean:
      return "boolean";
    case scratchbird::engine::sblr::SblrValuePayloadKind::signed_integer:
      return "signed_integer";
    case scratchbird::engine::sblr::SblrValuePayloadKind::unsigned_integer:
      return "unsigned_integer";
    case scratchbird::engine::sblr::SblrValuePayloadKind::real64:
      return "real64";
    case scratchbird::engine::sblr::SblrValuePayloadKind::high_precision_numeric_text:
      return "high_precision_numeric_text";
    case scratchbird::engine::sblr::SblrValuePayloadKind::text:
      return "text";
    case scratchbird::engine::sblr::SblrValuePayloadKind::binary:
      return "binary";
    case scratchbird::engine::sblr::SblrValuePayloadKind::uuid_text:
      return "uuid_text";
    case scratchbird::engine::sblr::SblrValuePayloadKind::temporal_text:
      return "temporal_text";
    case scratchbird::engine::sblr::SblrValuePayloadKind::descriptor_payload:
      return "descriptor_payload";
    case scratchbird::engine::sblr::SblrValuePayloadKind::none:
      return "none";
  }
  return "none";
}

std::string DescriptorDocumentJson(const scratchbird::engine::sblr::SblrValue& value) {
  std::string out = "{";
  out += "\"descriptor_id\":";
  out += JsonStringEscape(value.descriptor_id.empty() ? "unknown" : value.descriptor_id);
  out += ",\"payload_kind\":";
  out += JsonStringEscape(PayloadKindName(value));
  out += ",\"is_null\":";
  out += value.is_null ? "true" : "false";
  out += ",\"charset_name\":";
  out += JsonStringEscape(value.charset_name.empty() ? "UTF-8" : value.charset_name);
  out += ",\"collation_name\":";
  out += JsonStringEscape(value.collation_name.empty() ? "unicode_root" : value.collation_name);
  out += "}";
  return out;
}

struct CatalogContextObject {
  std::string uuid;
  std::string name;
  std::string object_class;
  std::string owner_uuid;
};

bool ClassFilterMatches(std::string_view object_class, std::string_view filter) {
  if (filter.empty()) return true;
  const std::string lowered_class = LowerAscii(std::string(object_class));
  const std::string lowered_filter = LowerAscii(TrimAscii(std::string(filter)));
  if (lowered_filter.empty()) return true;
  if (lowered_class == lowered_filter) return true;
  if (lowered_class == "database" && lowered_filter == "catalog") return true;
  if (lowered_class == "node" && lowered_filter == "server") return true;
  return false;
}

std::vector<CatalogContextObject> CatalogContextObjects(
    const scratchbird::engine::sblr::SblrExecutionContext& context) {
  const std::string owner_uuid = context.user_uuid;
  std::vector<CatalogContextObject> objects;
  auto add = [&](const std::string& uuid, std::string name, std::string object_class) {
    if (uuid.empty()) return;
    objects.push_back(CatalogContextObject{uuid, std::move(name), std::move(object_class), owner_uuid});
  };
  add(context.database_uuid, "current_database", "database");
  add(context.current_schema_uuid, "current_schema", "schema");
  add(context.user_uuid, "current_user", "user");
  add(context.current_role_uuid, "current_role", "role");
  add(context.node_uuid, "current_server", "node");
  add(context.session_uuid, "current_session", "session");
  add(context.transaction_uuid, "current_transaction", "transaction");
  add(context.statement_uuid, "current_statement", "statement");
  add(context.security_snapshot_uuid, "current_security_snapshot", "security_snapshot");
  return objects;
}

std::optional<CatalogContextObject> CatalogObjectForUuid(
    const scratchbird::engine::sblr::SblrExecutionContext& context,
    std::string_view uuid) {
  const std::string wanted = LowerAscii(TrimAscii(std::string(uuid)));
  if (wanted.empty()) return std::nullopt;
  for (const auto& object : CatalogContextObjects(context)) {
    if (LowerAscii(object.uuid) == wanted) return object;
  }
  return std::nullopt;
}

std::optional<CatalogContextObject> CatalogObjectForName(
    const scratchbird::engine::sblr::SblrExecutionContext& context,
    std::string_view name,
    std::string_view class_filter = {}) {
  std::string wanted = LowerAscii(TrimAscii(std::string(name)));
  if (wanted.empty()) return std::nullopt;
  if (wanted == "database" || wanted == "catalog") wanted = "current_database";
  if (wanted == "schema") wanted = "current_schema";
  if (wanted == "user") wanted = "current_user";
  if (wanted == "role") wanted = "current_role";
  if (wanted == "node" || wanted == "server") wanted = "current_server";
  if (wanted == "session") wanted = "current_session";
  if (wanted == "transaction") wanted = "current_transaction";
  if (wanted == "statement") wanted = "current_statement";
  for (const auto& object : CatalogContextObjects(context)) {
    if (LowerAscii(object.name) == wanted && ClassFilterMatches(object.object_class, class_filter)) {
      return object;
    }
  }
  return std::nullopt;
}

std::string ExecutionTypeDescriptorJson(
    const scratchbird::engine::sblr::SblrExecutionContext& context) {
  std::string out = "{";
  out += "\"descriptor_kind\":\"ExecutionTypeDescriptor\"";
  out += ",\"database_uuid\":";
  out += JsonStringEscape(context.database_uuid);
  out += ",\"transaction_context_present\":";
  out += context.transaction_context_present ? "true" : "false";
  out += ",\"transaction_uuid\":";
  out += JsonStringEscape(context.transaction_uuid);
  out += ",\"transaction_isolation_level\":";
  out += JsonStringEscape(context.transaction_isolation_level);
  out += ",\"statement_uuid\":";
  out += JsonStringEscape(context.statement_uuid);
  out += ",\"user_uuid\":";
  out += JsonStringEscape(context.user_uuid);
  out += ",\"security_context_present\":";
  out += context.security_context_present ? "true" : "false";
  out += ",\"parser_profile_uuid\":";
  out += JsonStringEscape(context.parser_profile_uuid);
  out += ",\"client_protocol_uuid\":";
  out += JsonStringEscape(context.client_protocol_uuid);
  out += ",\"read_only_mode\":";
  out += context.read_only_mode ? "true" : "false";
  out += ",\"restricted_open_mode\":";
  out += context.restricted_open_mode ? "true" : "false";
  out += "}";
  return out;
}

std::string ContextColumnDescriptorSummaryJson(
    const scratchbird::engine::sblr::SblrExecutionContext& context) {
  std::string out = "{";
  out += "\"descriptor_kind\":\"column_descriptor_set\"";
  out += ",\"table_uuid\":";
  out += JsonStringEscape(context.database_uuid);
  out += ",\"source\":\"sblr_execution_context\"";
  out += ",\"column_names\":[\"database_uuid\",\"current_schema_uuid\",\"user_uuid\","
         "\"current_role_uuid\",\"statement_uuid\",\"transaction_uuid\"]";
  out += "}";
  return out;
}

std::optional<std::string> ContextColumnDescriptorJson(
    const scratchbird::engine::sblr::SblrExecutionContext& context,
    std::string_view table_uuid,
    std::string_view column_name) {
  const std::string table = LowerAscii(TrimAscii(std::string(table_uuid)));
  if (table.empty()) return std::nullopt;
  if (table != LowerAscii(context.database_uuid) &&
      table != LowerAscii(context.current_schema_uuid)) {
    return std::nullopt;
  }
  const std::string column = LowerAscii(TrimAscii(std::string(column_name)));
  std::string descriptor_id;
  bool nullable = true;
  if (column == "database_uuid") {
    descriptor_id = "uuid";
    nullable = context.database_uuid.empty();
  } else if (column == "current_schema_uuid") {
    descriptor_id = "uuid";
    nullable = context.current_schema_uuid.empty();
  } else if (column == "user_uuid") {
    descriptor_id = "uuid";
    nullable = context.user_uuid.empty();
  } else if (column == "current_role_uuid") {
    descriptor_id = "uuid";
    nullable = context.current_role_uuid.empty();
  } else if (column == "statement_uuid") {
    descriptor_id = "uuid";
    nullable = context.statement_uuid.empty();
  } else if (column == "transaction_uuid") {
    descriptor_id = "uuid";
    nullable = !context.transaction_context_present || context.transaction_uuid.empty();
  } else {
    return std::nullopt;
  }

  std::string out = "{";
  out += "\"descriptor_kind\":\"column_descriptor\"";
  out += ",\"table_uuid\":";
  out += JsonStringEscape(std::string(table_uuid));
  out += ",\"column_name\":";
  out += JsonStringEscape(column);
  out += ",\"descriptor_id\":";
  out += JsonStringEscape(descriptor_id);
  out += ",\"nullable\":";
  out += nullable ? "true" : "false";
  out += ",\"source\":\"sblr_execution_context\"";
  out += "}";
  return out;
}

std::string IndexDescriptorSummaryJson() {
  return "{\"descriptor_kind\":\"index_descriptor\",\"available\":false,"
         "\"source\":\"catalog_snapshot_unresolved\"}";
}

std::uint64_t DiagnosticCount(const scratchbird::engine::sblr::SblrExecutionContext& context) {
  if (!context.current_diagnostic_id.empty() || !context.current_diagnostic_uuid.empty()) return 1;
  return (!context.current_sqlstate.empty() && context.current_sqlstate != "00000") ? 1 : 0;
}

std::string ErrorClassForSqlstate(std::string_view sqlstate) {
  if (sqlstate.empty() || sqlstate == "00000") return "successful_completion";
  const std::string prefix = UpperAscii(std::string(sqlstate.substr(0, std::min<std::size_t>(2, sqlstate.size()))));
  if (prefix == "00") return "successful_completion";
  if (prefix == "01") return "warning";
  if (prefix == "02") return "no_data";
  if (prefix == "08") return "connection_exception";
  if (prefix == "0A") return "feature_not_supported";
  if (prefix == "22") return "data_exception";
  if (prefix == "23") return "integrity_constraint_violation";
  if (prefix == "28") return "invalid_authorization_contract";
  if (prefix == "2D") return "invalid_transaction_termination";
  if (prefix == "40") return "transaction_rollback";
  if (prefix == "42") return "syntax_error_or_access_rule_violation";
  if (prefix == "HY") return "cli_specific_condition";
  return "sqlstate_class_" + LowerAscii(prefix);
}

std::int64_t GdscodeForContext(const scratchbird::engine::sblr::SblrExecutionContext& context) {
  return DiagnosticCount(context) == 0 ? 0 : -1;
}

std::optional<std::string> DiagnosticFieldText(
    const scratchbird::engine::sblr::SblrExecutionContext& context,
    std::string_view field_name) {
  const std::string field = LowerAscii(TrimAscii(std::string(field_name)));
  if (field == "sqlstate") return context.current_sqlstate.empty() ? "00000" : context.current_sqlstate;
  if (field == "diagnostic_id" || field == "message_id") {
    if (context.current_diagnostic_id.empty()) return std::nullopt;
    return context.current_diagnostic_id;
  }
  if (field == "diagnostic_uuid") {
    if (context.current_diagnostic_uuid.empty()) return std::nullopt;
    return context.current_diagnostic_uuid;
  }
  if (field == "error_class") return ErrorClassForSqlstate(context.current_sqlstate);
  if (field == "gdscode") return std::to_string(GdscodeForContext(context));
  if (field == "last_error_position") return std::nullopt;
  if (field == "diagnostic_count") return std::to_string(DiagnosticCount(context));
  return std::nullopt;
}

scratchbird::engine::internal_api::EngineRequestContext EngineRequestContextFromSblr(
    const scratchbird::engine::sblr::SblrExecutionContext& context) {
  scratchbird::engine::internal_api::EngineRequestContext out;
  out.database_path = context.database_path;
  out.database_uuid.canonical = context.database_uuid;
  out.cluster_uuid.canonical = context.cluster_uuid;
  out.node_uuid.canonical = context.node_uuid;
  out.transaction_uuid.canonical = context.transaction_uuid;
  out.local_transaction_id = context.local_transaction_id;
  out.snapshot_visible_through_local_transaction_id =
      context.snapshot_visible_through_local_transaction_id;
  out.transaction_isolation_level = context.transaction_isolation_level;
  out.statement_uuid.canonical = context.statement_uuid;
  out.principal_uuid.canonical = context.user_uuid;
  out.current_role_uuid.canonical = context.current_role_uuid;
  out.current_schema_uuid.canonical = context.current_schema_uuid;
  out.session_uuid.canonical = context.session_uuid;
  out.current_sqlstate = context.current_sqlstate;
  out.current_diagnostic_uuid.canonical = context.current_diagnostic_uuid;
  out.client_protocol_uuid = context.client_protocol_uuid;
  out.application_name = context.application_name;
  out.security_context_present = context.security_context_present;
  out.cluster_authority_available = context.cluster_authority_available;
  out.read_only_mode = context.read_only_mode;
  out.statement_timestamp = context.statement_timestamp;
  out.transaction_timestamp = context.transaction_timestamp;
  out.current_timestamp = context.current_timestamp;
  out.current_monotonic_ns = context.current_monotonic_ns;
  out.deterministic_random_u64 = context.deterministic_random_u64;
  out.deterministic_random_u64_present = context.deterministic_random_u64_present;
  out.deterministic_random_bytes_hex = context.deterministic_random_bytes_hex;
  out.deterministic_uuid_text = context.deterministic_uuid_text;
  out.last_row_count = context.last_row_count;
  out.last_row_count_present = context.last_row_count_present;
  return out;
}

scratchbird::engine::sblr::SblrValue MakeBooleanValue(bool input) {
  auto value = MakeInt64Value("boolean", input ? 1 : 0);
  value.payload_kind = scratchbird::engine::sblr::SblrValuePayloadKind::boolean;
  return value;
}

struct PrivilegeObjectDescriptor {
  std::string uuid;
  std::string name;
  std::string object_class;
  std::string owner_uuid;
  std::vector<std::pair<std::string, std::string>> columns;
};

struct PrivilegeTextRule {
  bool supported = false;
  bool all_privileges = false;
  std::string security_right;
};

std::string UnquoteCatalogToken(std::string value) {
  value = TrimAscii(std::move(value));
  if (value.size() >= 2) {
    const char first = value.front();
    const char last = value.back();
    if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
      value = value.substr(1, value.size() - 2);
    }
  }
  return TrimAscii(std::move(value));
}

std::string NormalizeCatalogToken(std::string value) {
  return LowerAscii(UnquoteCatalogToken(std::move(value)));
}

bool PrincipalMatchesCurrentUser(
    const scratchbird::engine::sblr::SblrExecutionContext& context,
    std::string_view user_text) {
  const std::string user = NormalizeCatalogToken(std::string(user_text));
  if (user.empty()) return false;
  if (user == "current_user" || user == "session_user" || user == "user" ||
      user == "current_principal") {
    return !context.user_uuid.empty();
  }
  return !context.user_uuid.empty() && user == LowerAscii(context.user_uuid);
}

PrivilegeTextRule NormalizePrivilegeText(std::string privilege_text,
                                         std::string_view object_kind) {
  std::string privilege = UpperAscii(UnquoteCatalogToken(std::move(privilege_text)));
  const std::string grant_suffix = " WITH GRANT OPTION";
  const auto grant_pos = privilege.find(grant_suffix);
  if (grant_pos != std::string::npos) {
    privilege = TrimAscii(privilege.substr(0, grant_pos));
  }
  if (privilege == "ALL" || privilege == "ALL PRIVILEGES") {
    return PrivilegeTextRule{true, true, {}};
  }

  auto supported = [&](std::initializer_list<std::string_view> values) {
    for (const auto value : values) {
      if (privilege == value) return true;
    }
    return false;
  };

  if (object_kind == "table") {
    if (supported({"SELECT", "INSERT", "UPDATE", "DELETE", "TRUNCATE",
                   "REFERENCES", "TRIGGER"})) {
      return PrivilegeTextRule{true, false, privilege};
    }
  } else if (object_kind == "column") {
    if (supported({"SELECT", "INSERT", "UPDATE", "REFERENCES"})) {
      return PrivilegeTextRule{true, false, privilege};
    }
  } else if (object_kind == "function") {
    if (privilege == "EXECUTE") return PrivilegeTextRule{true, false, "EXECUTE"};
  } else if (object_kind == "schema") {
    if (privilege == "USAGE") return PrivilegeTextRule{true, false, "VISIBLE"};
    if (supported({"CREATE", "ALTER", "DROP"})) {
      return PrivilegeTextRule{true, false, privilege};
    }
  }
  return PrivilegeTextRule{};
}

PrivilegeObjectDescriptor PrivilegeObjectFromCrudTable(
    const scratchbird::engine::internal_api::CrudTableRecord& table,
    const scratchbird::engine::sblr::SblrExecutionContext& context) {
  return PrivilegeObjectDescriptor{table.table_uuid,
                                   table.default_name,
                                   "table",
                                   context.user_uuid,
                                   table.columns};
}

std::optional<PrivilegeObjectDescriptor> ResolvePrivilegeTable(
    const scratchbird::engine::sblr::SblrExecutionContext& context,
    const scratchbird::engine::internal_api::EngineRequestContext& api_context,
    std::string_view table_identifier) {
  const std::string wanted_text = UnquoteCatalogToken(std::string(table_identifier));
  const std::string wanted = LowerAscii(wanted_text);
  if (wanted.empty()) return std::nullopt;

  if (!api_context.database_path.empty()) {
    const auto loaded =
        scratchbird::engine::internal_api::LoadMgaRelationStoreState(api_context);
    if (loaded.ok) {
      const auto state =
          scratchbird::engine::internal_api::BuildCrudCompatibilityStateFromMga(loaded.state);
      if (const auto table = scratchbird::engine::internal_api::FindVisibleCrudTable(
              state, wanted_text, api_context.local_transaction_id)) {
        return PrivilegeObjectFromCrudTable(*table, context);
      }
      for (const auto& candidate : state.tables) {
        if (NormalizeCatalogToken(candidate.default_name) != wanted) continue;
        if (const auto table = scratchbird::engine::internal_api::FindVisibleCrudTable(
                state, candidate.table_uuid, api_context.local_transaction_id)) {
          return PrivilegeObjectFromCrudTable(*table, context);
        }
      }
    }
  }

  return std::nullopt;
}

std::optional<PrivilegeObjectDescriptor> ResolvePrivilegeSchema(
    const scratchbird::engine::sblr::SblrExecutionContext& context,
    std::string_view schema_identifier) {
  const std::string wanted = UnquoteCatalogToken(std::string(schema_identifier));
  std::optional<CatalogContextObject> object = CatalogObjectForUuid(context, wanted);
  if (!object) object = CatalogObjectForName(context, wanted, "schema");
  if (!object || !ClassFilterMatches(object->object_class, "schema")) return std::nullopt;
  return PrivilegeObjectDescriptor{object->uuid, object->name, "schema", object->owner_uuid, {}};
}

std::string NormalizeFunctionPrivilegeName(std::string value) {
  value = NormalizeCatalogToken(std::move(value));
  const auto paren = value.find('(');
  if (paren != std::string::npos) value = value.substr(0, paren);
  return value;
}

std::optional<PrivilegeObjectDescriptor> ResolvePrivilegeFunction(
    const scratchbird::engine::sblr::SblrExecutionContext& context,
    std::string_view function_identifier) {
  struct FunctionBuiltin {
    std::string_view builtin_id;
    std::string_view builtin_uuid;
    std::string_view canonical_name;
  };
  static constexpr std::array<FunctionBuiltin, 4> kPrivilegeFunctions{{
      {"sb.scalar.has_table_privilege", "019dffbb-f000-7da2-9e38-d4654d06dc78", "has_table_privilege"},
      {"sb.scalar.has_column_privilege", "019dffbb-f000-7437-a6e4-cbb9d1d75a44", "has_column_privilege"},
      {"sb.scalar.has_function_privilege", "019dffbb-f000-7e3a-a0b9-7c75989b7e24", "has_function_privilege"},
      {"sb.scalar.has_schema_privilege", "019dffbb-f000-721e-9b45-dd414479d540", "has_schema_privilege"},
  }};

  const std::string wanted = NormalizeFunctionPrivilegeName(std::string(function_identifier));
  if (wanted.empty()) return std::nullopt;
  for (const auto& function : kPrivilegeFunctions) {
    if (wanted == function.builtin_id || wanted == function.canonical_name ||
        wanted == LowerAscii(std::string(function.builtin_uuid))) {
      return PrivilegeObjectDescriptor{std::string(function.builtin_uuid),
                                       std::string(function.builtin_id),
                                       "function",
                                       context.user_uuid,
                                       {}};
    }
  }
  return std::nullopt;
}

bool PrivilegeTableHasColumn(const PrivilegeObjectDescriptor& table,
                             std::string_view column_identifier) {
  const std::string wanted = NormalizeCatalogToken(std::string(column_identifier));
  if (wanted.empty()) return false;
  for (const auto& column : table.columns) {
    if (NormalizeCatalogToken(column.first) == wanted) return true;
  }
  return false;
}

bool PrivilegeObjectAllowsCurrentPrincipal(
    const scratchbird::engine::sblr::SblrExecutionContext& context,
    const scratchbird::engine::internal_api::EngineRequestContext& api_context,
    const PrivilegeObjectDescriptor& object,
    const PrivilegeTextRule& privilege) {
  if (!privilege.supported) return false;
  if (!context.user_uuid.empty() && LowerAscii(object.owner_uuid) == LowerAscii(context.user_uuid)) {
    return true;
  }
  if (scratchbird::engine::internal_api::SecurityContextHasAnyAdmin(
          api_context, {"SEC_GRANT_ADMIN", "POLICY_ADMIN"})) {
    return true;
  }
  if (!privilege.all_privileges &&
      scratchbird::engine::internal_api::IsKnownSecurityRight(privilege.security_right) &&
      scratchbird::engine::internal_api::SecurityContextHasRight(
          api_context, privilege.security_right, object.uuid)) {
    return true;
  }
  return false;
}

bool PrivilegePrincipalAllowed(const FunctionCallRequest& request,
                               std::size_t user_arg_index,
                               bool has_user_arg) {
  if (!has_user_arg) return !request.context.sblr_context.user_uuid.empty();
  return PrincipalMatchesCurrentUser(
      request.context.sblr_context,
      ValueAsText(request.arguments[user_arg_index].value));
}

std::string JsonArrayFromTextValues(const std::vector<std::string>& values) {
  std::string out = "[";
  bool first = true;
  for (const auto& value : values) {
    if (!first) out.push_back(',');
    first = false;
    out += JsonStringEscape(value);
  }
  out.push_back(']');
  return out;
}

std::string JsonArrayFromOptionalTextValues(const std::vector<std::optional<std::string>>& values) {
  std::string out = "[";
  bool first = true;
  for (const auto& value : values) {
    if (!first) out.push_back(',');
    first = false;
    out += value.has_value() ? JsonStringEscape(*value) : std::string("null");
  }
  out.push_back(']');
  return out;
}

bool IsAsciiText(std::string_view text) {
  return std::all_of(text.begin(), text.end(), [](unsigned char ch) {
    return ch < 0x80;
  });
}

bool IsIdentifierSafeUnquoted(std::string_view text) {
  if (text.empty()) return false;
  const auto first = static_cast<unsigned char>(text.front());
  if (!(std::islower(first) || first == '_')) return false;
  return std::all_of(text.begin() + 1, text.end(), [](unsigned char ch) {
    return std::islower(ch) || std::isdigit(ch) || ch == '_';
  });
}

std::string QuoteIdentifierText(std::string_view text) {
  if (IsIdentifierSafeUnquoted(text)) return std::string(text);
  std::string out;
  out.reserve(text.size() + 2);
  out.push_back('"');
  for (const char ch : text) {
    if (ch == '"') out.push_back('"');
    out.push_back(ch);
  }
  out.push_back('"');
  return out;
}

std::string QuoteLiteralText(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 2);
  out.push_back('\'');
  for (const char ch : text) {
    if (ch == '\'') out.push_back('\'');
    out.push_back(ch);
  }
  out.push_back('\'');
  return out;
}

bool IsUtf8EncodingName(std::string_view encoding) {
  const auto lowered = LowerAscii(std::string(encoding));
  return lowered.empty() || lowered == "utf8" || lowered == "utf-8";
}

bool ParseJsonWhitespace(std::string_view text, std::size_t* index) {
  while (*index < text.size()) {
    const auto ch = static_cast<unsigned char>(text[*index]);
    if (!std::isspace(ch)) break;
    ++*index;
  }
  return *index <= text.size();
}

bool ParseJsonArrayString(std::string_view text, std::size_t* index, std::string* out) {
  if (*index >= text.size() || text[*index] != '"') return false;
  ++*index;
  out->clear();
  while (*index < text.size()) {
    const char ch = text[*index];
    ++*index;
    if (ch == '"') return true;
    if (static_cast<unsigned char>(ch) < 0x20) return false;
    if (ch != '\\') {
      out->push_back(ch);
      continue;
    }
    if (*index >= text.size()) return false;
    const char escaped = text[*index];
    ++*index;
    switch (escaped) {
      case '"':
      case '\\':
      case '/':
        out->push_back(escaped);
        break;
      case 'b':
        out->push_back('\b');
        break;
      case 'f':
        out->push_back('\f');
        break;
      case 'n':
        out->push_back('\n');
        break;
      case 'r':
        out->push_back('\r');
        break;
      case 't':
        out->push_back('\t');
        break;
      default:
        return false;
    }
  }
  return false;
}

bool ParseJsonTextArray(std::string_view text, std::vector<std::optional<std::string>>* out) {
  std::size_t index = 0;
  ParseJsonWhitespace(text, &index);
  if (index >= text.size() || text[index] != '[') return false;
  ++index;
  out->clear();
  ParseJsonWhitespace(text, &index);
  if (index < text.size() && text[index] == ']') {
    ++index;
    ParseJsonWhitespace(text, &index);
    return index == text.size();
  }
  while (index < text.size()) {
    ParseJsonWhitespace(text, &index);
    if (index >= text.size()) return false;
    if (text.substr(index, 4) == "null") {
      out->push_back(std::nullopt);
      index += 4;
    } else {
      std::string value;
      if (!ParseJsonArrayString(text, &index, &value)) return false;
      out->push_back(std::move(value));
    }
    ParseJsonWhitespace(text, &index);
    if (index >= text.size()) return false;
    if (text[index] == ',') {
      ++index;
      continue;
    }
    if (text[index] == ']') {
      ++index;
      ParseJsonWhitespace(text, &index);
      return index == text.size();
    }
    return false;
  }
  return false;
}

bool Utf8NextCodepoint(std::string_view text, std::size_t* index, std::int64_t* codepoint) {
  if (*index >= text.size()) return false;
  const auto byte0 = static_cast<unsigned char>(text[*index]);
  if (byte0 < 0x80) {
    *codepoint = static_cast<std::int64_t>(byte0);
    ++*index;
    return true;
  }
  std::size_t width = 0;
  std::int64_t value = 0;
  if ((byte0 & 0xe0) == 0xc0) {
    width = 2;
    value = byte0 & 0x1f;
  } else if ((byte0 & 0xf0) == 0xe0) {
    width = 3;
    value = byte0 & 0x0f;
  } else if ((byte0 & 0xf8) == 0xf0) {
    width = 4;
    value = byte0 & 0x07;
  } else {
    return false;
  }
  if (text.size() - *index < width) return false;
  for (std::size_t offset = 1; offset < width; ++offset) {
    const auto cont = static_cast<unsigned char>(text[*index + offset]);
    if ((cont & 0xc0) != 0x80) return false;
    value = (value << 6) | (cont & 0x3f);
  }
  if ((width == 2 && value < 0x80) ||
      (width == 3 && value < 0x800) ||
      (width == 4 && (value < 0x10000 || value > 0x10ffff)) ||
      (value >= 0xd800 && value <= 0xdfff)) {
    return false;
  }
  *index += width;
  *codepoint = value;
  return true;
}

bool IsValidUtf8(std::string_view text) {
  std::size_t index = 0;
  while (index < text.size()) {
    std::int64_t codepoint = 0;
    if (!Utf8NextCodepoint(text, &index, &codepoint)) return false;
  }
  return true;
}

bool Utf8CharacterSlices(std::string_view text, std::vector<std::string>* out) {
  out->clear();
  std::size_t index = 0;
  while (index < text.size()) {
    const std::size_t start = index;
    std::int64_t codepoint = 0;
    if (!Utf8NextCodepoint(text, &index, &codepoint)) return false;
    out->emplace_back(text.substr(start, index - start));
  }
  return true;
}

std::string LatinFoldCodepoint(std::int64_t codepoint) {
  switch (codepoint) {
    case 0x00c0:
    case 0x00c1:
    case 0x00c2:
    case 0x00c3:
    case 0x00c4:
    case 0x00c5:
      return "A";
    case 0x00c6:
      return "AE";
    case 0x00c7:
      return "C";
    case 0x00c8:
    case 0x00c9:
    case 0x00ca:
    case 0x00cb:
      return "E";
    case 0x00cc:
    case 0x00cd:
    case 0x00ce:
    case 0x00cf:
      return "I";
    case 0x00d0:
      return "D";
    case 0x00d1:
      return "N";
    case 0x00d2:
    case 0x00d3:
    case 0x00d4:
    case 0x00d5:
    case 0x00d6:
    case 0x00d8:
      return "O";
    case 0x00d9:
    case 0x00da:
    case 0x00db:
    case 0x00dc:
      return "U";
    case 0x00dd:
      return "Y";
    case 0x00de:
      return "TH";
    case 0x00df:
      return "ss";
    case 0x00e0:
    case 0x00e1:
    case 0x00e2:
    case 0x00e3:
    case 0x00e4:
    case 0x00e5:
      return "a";
    case 0x00e6:
      return "ae";
    case 0x00e7:
      return "c";
    case 0x00e8:
    case 0x00e9:
    case 0x00ea:
    case 0x00eb:
      return "e";
    case 0x00ec:
    case 0x00ed:
    case 0x00ee:
    case 0x00ef:
      return "i";
    case 0x00f0:
      return "d";
    case 0x00f1:
      return "n";
    case 0x00f2:
    case 0x00f3:
    case 0x00f4:
    case 0x00f5:
    case 0x00f6:
    case 0x00f8:
      return "o";
    case 0x00f9:
    case 0x00fa:
    case 0x00fb:
    case 0x00fc:
      return "u";
    case 0x00fd:
    case 0x00ff:
      return "y";
    case 0x00fe:
      return "th";
    default:
      break;
  }
  if (codepoint >= 0x0300 && codepoint <= 0x036f) return "";
  return "?";
}

bool FoldUtf8ToAscii(std::string_view text, std::string* out) {
  out->clear();
  out->reserve(text.size());
  std::size_t index = 0;
  while (index < text.size()) {
    std::int64_t codepoint = 0;
    if (!Utf8NextCodepoint(text, &index, &codepoint)) return false;
    if (codepoint < 0x80) {
      out->push_back(static_cast<char>(codepoint));
    } else {
      *out += LatinFoldCodepoint(codepoint);
    }
  }
  return true;
}

std::uint32_t Crc32Bytes(const std::vector<std::uint8_t>& bytes) {
  std::uint32_t crc = 0xffffffffu;
  for (const auto byte : bytes) {
    crc ^= static_cast<std::uint32_t>(byte);
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
  }
  return ~crc;
}

std::uint32_t Crc32cBytes(const std::vector<std::uint8_t>& bytes) {
  std::uint32_t crc = 0xffffffffu;
  for (const auto byte : bytes) {
    crc ^= static_cast<std::uint32_t>(byte);
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0x82f63b78u & (0u - (crc & 1u)));
    }
  }
  return ~crc;
}

bool Utf8FirstCodepoint(std::string_view text, std::int64_t* codepoint) {
  if (text.empty()) {
    *codepoint = 0;
    return true;
  }
  const auto byte0 = static_cast<unsigned char>(text[0]);
  if (byte0 < 0x80) {
    *codepoint = static_cast<std::int64_t>(byte0);
    return true;
  }
  std::size_t width = 0;
  std::int64_t value = 0;
  if ((byte0 & 0xe0) == 0xc0) {
    width = 2;
    value = byte0 & 0x1f;
  } else if ((byte0 & 0xf0) == 0xe0) {
    width = 3;
    value = byte0 & 0x0f;
  } else if ((byte0 & 0xf8) == 0xf0) {
    width = 4;
    value = byte0 & 0x07;
  } else {
    return false;
  }
  if (text.size() < width) return false;
  for (std::size_t i = 1; i < width; ++i) {
    const auto cont = static_cast<unsigned char>(text[i]);
    if ((cont & 0xc0) != 0x80) return false;
    value = (value << 6) | (cont & 0x3f);
  }
  if ((width == 2 && value < 0x80) ||
      (width == 3 && value < 0x800) ||
      (width == 4 && (value < 0x10000 || value > 0x10ffff)) ||
      (value >= 0xd800 && value <= 0xdfff)) {
    return false;
  }
  *codepoint = value;
  return true;
}

int HexValue(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
  if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
  return -1;
}

std::vector<std::uint8_t> RawBytesFromValue(const scratchbird::engine::sblr::SblrValue& value) {
  if (value.payload_kind == scratchbird::engine::sblr::SblrValuePayloadKind::binary) return value.binary_value;
  const auto text = ValueAsText(value);
  return std::vector<std::uint8_t>(text.begin(), text.end());
}

std::string HexEncodeBytes(const std::vector<std::uint8_t>& bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (const auto byte : bytes) {
    out.push_back(kHex[(byte >> 4) & 0x0f]);
    out.push_back(kHex[byte & 0x0f]);
  }
  return out;
}

bool HexDecodeBytes(const std::string& hex, std::vector<std::uint8_t>* out) {
  if ((hex.size() % 2) != 0) return false;
  out->clear();
  out->reserve(hex.size() / 2);
  for (std::size_t i = 0; i < hex.size(); i += 2) {
    const int high = HexValue(hex[i]);
    const int low = HexValue(hex[i + 1]);
    if (high < 0 || low < 0) return false;
    out->push_back(static_cast<std::uint8_t>((high << 4) | low));
  }
  return true;
}

std::vector<std::uint8_t> BytesFromHexPrefix(const std::string& hex, std::size_t byte_count) {
  std::vector<std::uint8_t> out;
  out.reserve(byte_count);
  for (std::size_t i = 0; i < byte_count && (i * 2 + 1) < hex.size(); ++i) {
    const int high = HexValue(hex[i * 2]);
    const int low = HexValue(hex[i * 2 + 1]);
    if (high < 0 || low < 0) break;
    out.push_back(static_cast<std::uint8_t>((high << 4) | low));
  }
  return out;
}

std::string Base64EncodeBytes(const std::vector<std::uint8_t>& bytes) {
  static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (std::size_t i = 0; i < bytes.size(); i += 3) {
    const std::uint32_t a = bytes[i];
    const std::uint32_t b = i + 1 < bytes.size() ? bytes[i + 1] : 0;
    const std::uint32_t c = i + 2 < bytes.size() ? bytes[i + 2] : 0;
    const std::uint32_t triple = (a << 16) | (b << 8) | c;
    out.push_back(kAlphabet[(triple >> 18) & 0x3f]);
    out.push_back(kAlphabet[(triple >> 12) & 0x3f]);
    out.push_back(i + 1 < bytes.size() ? kAlphabet[(triple >> 6) & 0x3f] : '=');
    out.push_back(i + 2 < bytes.size() ? kAlphabet[triple & 0x3f] : '=');
  }
  return out;
}

int Base64Value(char ch) {
  if (ch >= 'A' && ch <= 'Z') return ch - 'A';
  if (ch >= 'a' && ch <= 'z') return 26 + (ch - 'a');
  if (ch >= '0' && ch <= '9') return 52 + (ch - '0');
  if (ch == '+') return 62;
  if (ch == '/') return 63;
  return -1;
}

bool Base64DecodeBytes(const std::string& text, std::vector<std::uint8_t>* out) {
  if ((text.size() % 4) != 0) return false;
