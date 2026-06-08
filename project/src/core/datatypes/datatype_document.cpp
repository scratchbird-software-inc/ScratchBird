// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "datatype_document.hpp"

#include <cctype>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

namespace scratchbird::core::datatypes {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::datatypes};
}

Status ErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::datatypes};
}

std::string TrimAsciiWhitespace(std::string value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) { value.erase(value.begin()); }
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) { value.pop_back(); }
  return value;
}

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.substr(0, prefix.size()) == prefix;
}

int HexValue(char c) {
  if (c >= '0' && c <= '9') { return c - '0'; }
  if (c >= 'a' && c <= 'f') { return 10 + c - 'a'; }
  if (c >= 'A' && c <= 'F') { return 10 + c - 'A'; }
  return -1;
}

bool HexText(std::string_view value, bool require_even_digits) {
  if (value.empty()) { return false; }
  if (require_even_digits && (value.size() % 2) != 0) { return false; }
  for (char c : value) {
    if (HexValue(c) < 0) { return false; }
  }
  return true;
}

class JsonCanonicalizer {
 public:
  explicit JsonCanonicalizer(std::string_view text) : text_(text) {}

  bool Canonicalize(std::string* out) {
    SkipWhitespace();
    if (!ParseValue()) { return false; }
    SkipWhitespace();
    if (pos_ != text_.size()) { return false; }
    *out = std::move(out_);
    return true;
  }

 private:
  void SkipWhitespace() {
    while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) { ++pos_; }
  }

  bool Consume(char expected) {
    if (pos_ < text_.size() && text_[pos_] == expected) {
      out_.push_back(expected);
      ++pos_;
      return true;
    }
    return false;
  }

  bool ConsumeLiteral(std::string_view expected) {
    if (text_.substr(pos_, expected.size()) != expected) { return false; }
    out_.append(expected);
    pos_ += expected.size();
    return true;
  }

  bool ParseValue() {
    SkipWhitespace();
    if (pos_ >= text_.size()) { return false; }
    const char c = text_[pos_];
    if (c == '{') { return ParseObject(); }
    if (c == '[') { return ParseArray(); }
    if (c == '"') { return ParseString(nullptr); }
    if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) { return ParseNumber(); }
    if (c == 't') { return ConsumeLiteral("true"); }
    if (c == 'f') { return ConsumeLiteral("false"); }
    if (c == 'n') { return ConsumeLiteral("null"); }
    return false;
  }

  bool ParseObject() {
    if (!Consume('{')) { return false; }
    SkipWhitespace();
    if (pos_ < text_.size() && text_[pos_] == '}') { return Consume('}'); }
    std::set<std::string> keys;
    while (true) {
      SkipWhitespace();
      std::string key;
      if (!ParseString(&key)) { return false; }
      if (!keys.insert(key).second) { return false; }
      SkipWhitespace();
      if (!Consume(':')) { return false; }
      if (!ParseValue()) { return false; }
      SkipWhitespace();
      if (pos_ < text_.size() && text_[pos_] == '}') { return Consume('}'); }
      if (!Consume(',')) { return false; }
    }
  }

  bool ParseArray() {
    if (!Consume('[')) { return false; }
    SkipWhitespace();
    if (pos_ < text_.size() && text_[pos_] == ']') { return Consume(']'); }
    while (true) {
      if (!ParseValue()) { return false; }
      SkipWhitespace();
      if (pos_ < text_.size() && text_[pos_] == ']') { return Consume(']'); }
      if (!Consume(',')) { return false; }
    }
  }

  bool ParseString(std::string* decoded_key) {
    if (pos_ >= text_.size() || text_[pos_] != '"') { return false; }
    out_.push_back(text_[pos_++]);
    std::string decoded;
    while (pos_ < text_.size()) {
      const unsigned char c = static_cast<unsigned char>(text_[pos_++]);
      out_.push_back(static_cast<char>(c));
      if (c == '"') {
        if (decoded_key != nullptr) { *decoded_key = std::move(decoded); }
        return true;
      }
      if (c < 0x20) { return false; }
      if (c == '\\') {
        if (pos_ >= text_.size()) { return false; }
        const char escaped = text_[pos_++];
        out_.push_back(escaped);
        if (escaped == '"' || escaped == '\\' || escaped == '/' || escaped == 'b' || escaped == 'f' ||
            escaped == 'n' || escaped == 'r' || escaped == 't') {
          decoded.push_back('\\');
          decoded.push_back(escaped);
          continue;
        }
        if (escaped == 'u') {
          decoded.append("\\u");
          for (int i = 0; i < 4; ++i) {
            if (pos_ >= text_.size() || HexValue(text_[pos_]) < 0) { return false; }
            decoded.push_back(text_[pos_]);
            out_.push_back(text_[pos_++]);
          }
          continue;
        }
        return false;
      }
      decoded.push_back(static_cast<char>(c));
    }
    return false;
  }

  bool ParseNumber() {
    const std::size_t start = pos_;
    if (pos_ < text_.size() && text_[pos_] == '-') { ++pos_; }
    if (pos_ >= text_.size()) { return false; }
    if (text_[pos_] == '0') {
      ++pos_;
      if (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) { return false; }
    } else {
      if (!std::isdigit(static_cast<unsigned char>(text_[pos_]))) { return false; }
      while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) { ++pos_; }
    }
    if (pos_ < text_.size() && text_[pos_] == '.') {
      ++pos_;
      if (pos_ >= text_.size() || !std::isdigit(static_cast<unsigned char>(text_[pos_]))) { return false; }
      while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) { ++pos_; }
    }
    if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
      ++pos_;
      if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) { ++pos_; }
      if (pos_ >= text_.size() || !std::isdigit(static_cast<unsigned char>(text_[pos_]))) { return false; }
      while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) { ++pos_; }
    }
    out_.append(text_.substr(start, pos_ - start));
    return true;
  }

  std::string_view text_;
  std::size_t pos_ = 0;
  std::string out_;
};

bool XmlNameText(std::string_view value) {
  if (value.empty()) { return false; }
  const unsigned char first = static_cast<unsigned char>(value.front());
  if (!std::isalpha(first) && first != '_' && first != ':') { return false; }
  for (char c : value) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (!std::isalnum(u) && c != '_' && c != '-' && c != ':' && c != '.') { return false; }
  }
  return true;
}

bool CanonicalizeXmlText(const std::string& input, std::string* out) {
  std::string value = TrimAsciiWhitespace(input);
  if (StartsWith(value, "<?xml")) {
    const auto declaration_end = value.find("?>");
    if (declaration_end == std::string::npos) { return false; }
    value = TrimAsciiWhitespace(value.substr(declaration_end + 2));
  }
  if (value.size() < 7 || value.front() != '<' || value.back() != '>') { return false; }
  if (value[1] == '/' || value[1] == '!' || value[1] == '?') { return false; }
  const auto open_end = value.find('>');
  if (open_end == std::string::npos || open_end < 2) { return false; }
  std::string open_name = value.substr(1, open_end - 1);
  const auto attr_pos = open_name.find_first_of(" \t\r\n/");
  const bool self_closing = !open_name.empty() && open_name.back() == '/';
  if (attr_pos != std::string::npos) { open_name = open_name.substr(0, attr_pos); }
  if (!XmlNameText(open_name)) { return false; }
  if (self_closing || (open_end > 0 && value[open_end - 1] == '/')) {
    *out = value;
    return true;
  }
  const std::string close = "</" + open_name + ">";
  if (value.size() < close.size() || value.substr(value.size() - close.size()) != close) { return false; }
  *out = value;
  return true;
}

bool CanonicalizeJsonText(const std::string& input, std::string* out) {
  return JsonCanonicalizer(input).Canonicalize(out);
}

bool CanonicalizeBinaryEnvelope(const std::string& input, std::string_view prefix, std::string* out) {
  const std::string value = TrimAsciiWhitespace(input);
  if (!StartsWith(value, prefix)) { return false; }
  const auto payload_pos = value.find("payload=");
  if (payload_pos == std::string::npos) { return false; }
  const std::string payload = value.substr(payload_pos + 8);
  if (!HexText(payload, true)) { return false; }
  *out = std::string(prefix) + "payload=" + payload;
  return true;
}

bool CanonicalizeHstoreDomainEnvelope(const std::string& input, std::string* out) {
  const std::string value = TrimAsciiWhitespace(input);
  if (!StartsWith(value, "SBHSTORE1;items=")) { return false; }
  const std::string payload = value.substr(16);
  if (payload.empty()) {
    *out = value;
    return true;
  }
  std::size_t start = 0;
  while (start <= payload.size()) {
    const auto comma = payload.find(',', start);
    const std::string pair = payload.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
    const auto colon = pair.find(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= pair.size()) { return false; }
    if (!HexText(pair.substr(0, colon), true) || !HexText(pair.substr(colon + 1), true)) { return false; }
    if (comma == std::string::npos) { break; }
    start = comma + 1;
  }
  *out = value;
  return true;
}

DocumentCanonicalizationResult Failure(std::string detail) {
  DocumentCanonicalizationResult result;
  result.status = ErrorStatus();
  result.diagnostic = MakeDocumentCanonicalizationDiagnostic(result.status,
                                                            "SB_DATATYPE_DOCUMENT_CANONICALIZATION_REJECTED",
                                                            "datatype.document_canonicalization.rejected",
                                                            std::move(detail));
  return result;
}

DocumentCanonicalizationResult Success(CanonicalTypeId type_id, std::string value, std::string format) {
  DocumentCanonicalizationResult result;
  result.status = OkStatus();
  result.canonical_type_id = type_id;
  result.canonical_value = std::move(value);
  result.canonical_format = std::move(format);
  result.diagnostic = MakeDocumentCanonicalizationDiagnostic(result.status, "SB_DATATYPE_OK", "datatype.ok");
  return result;
}

}  // namespace

DocumentCanonicalizationResult CanonicalizeDocumentValue(const DocumentCanonicalizationRequest& request) {
  std::string canonical;
  switch (request.type_id) {
    case CanonicalTypeId::document: {
      const std::string value = TrimAsciiWhitespace(request.encoded_value);
      if (!value.empty() && (value.front() == '{' || value.front() == '[') && CanonicalizeJsonText(value, &canonical)) {
        return Success(CanonicalTypeId::json_document, canonical, "json_text");
      }
      if (!value.empty() && value.front() == '<' && CanonicalizeXmlText(value, &canonical)) {
        return Success(CanonicalTypeId::xml_document, canonical, "xml_text");
      }
      return Failure("document_family_unresolved");
    }
    case CanonicalTypeId::json_document:
    case CanonicalTypeId::object_document:
    case CanonicalTypeId::flattened_object_document:
      if (!CanonicalizeJsonText(request.encoded_value, &canonical)) { return Failure("json_document_invalid"); }
      return Success(request.type_id, canonical, "json_text");
    case CanonicalTypeId::binary_json_document:
      if (!CanonicalizeBinaryEnvelope(request.encoded_value, "SBBJSON1;", &canonical)) {
        return Failure("binary_json_document_invalid");
      }
      return Success(request.type_id, canonical, "binary_json_envelope");
    case CanonicalTypeId::bson_document:
      if (!CanonicalizeBinaryEnvelope(request.encoded_value, "SBBSON1;", &canonical)) {
        return Failure("bson_document_invalid");
      }
      return Success(request.type_id, canonical, "bson_envelope");
    case CanonicalTypeId::xml_document:
      if (!CanonicalizeXmlText(request.encoded_value, &canonical)) { return Failure("xml_document_invalid"); }
      return Success(request.type_id, canonical, "xml_text");
    case CanonicalTypeId::hstore_document:
      if (!request.allow_hstore_domain) { return Failure("hstore_requires_domain_profile"); }
      if (!CanonicalizeHstoreDomainEnvelope(request.encoded_value, &canonical)) {
        return Failure("hstore_domain_envelope_invalid");
      }
      return Success(request.type_id, canonical, "hstore_domain_envelope");
    default:
      return Failure("unsupported_document_type:" + std::string(CanonicalTypeName(request.type_id)));
  }
}

DiagnosticRecord MakeDocumentCanonicalizationDiagnostic(Status status,
                                                       std::string diagnostic_code,
                                                       std::string message_key,
                                                       std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) { arguments.push_back({"detail", std::move(detail)}); }
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "core.datatypes.document_canonicalization");
}

}  // namespace scratchbird::core::datatypes
