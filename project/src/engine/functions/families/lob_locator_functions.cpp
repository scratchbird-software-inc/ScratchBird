// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "families/lob_locator_functions.hpp"

#include "common/function_result_helpers.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::functions {
namespace {

constexpr std::size_t kDescriptorBytesLimit = 1U << 20;
constexpr std::size_t kLobBytesLimit = 8U << 20;

bool StartsWith(const std::string& value, const std::string& prefix) {
  return value.rfind(prefix, 0) == 0;
}

bool IdIs(const std::string& id, std::initializer_list<std::string_view> candidates) {
  for (const auto candidate : candidates) {
    if (id == candidate) return true;
  }
  return false;
}

std::string Trim(std::string value) {
  value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch) {
    return !std::isspace(ch);
  }));
  value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) {
    return !std::isspace(ch);
  }).base(), value.end());
  return value;
}

std::string LowerAscii(std::string value) {
  for (auto& ch : value) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return value;
}

std::string JsonEscape(std::string value) {
  std::string out = "\"";
  for (const char ch : value) {
    switch (ch) {
      case '\\':
      case '"':
        out.push_back('\\');
        out.push_back(ch);
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
        out.push_back(ch);
        break;
    }
  }
  out.push_back('"');
  return out;
}

int HexValue(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
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
  if (out == nullptr || hex.size() % 2 != 0) return false;
  std::vector<std::uint8_t> bytes;
  bytes.reserve(hex.size() / 2);
  for (std::size_t i = 0; i < hex.size(); i += 2) {
    const int hi = HexValue(hex[i]);
    const int lo = HexValue(hex[i + 1]);
    if (hi < 0 || lo < 0) return false;
    bytes.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
  }
  *out = std::move(bytes);
  return true;
}

std::optional<std::string> JsonStringForKey(const std::string& document,
                                            std::string_view key) {
  if (document.size() > kDescriptorBytesLimit) return std::nullopt;
  const std::string needle = "\"" + std::string(key) + "\"";
  const auto key_pos = document.find(needle);
  if (key_pos == std::string::npos) return std::nullopt;
  const auto colon = document.find(':', key_pos + needle.size());
  if (colon == std::string::npos) return std::nullopt;
  std::size_t cursor = colon + 1;
  while (cursor < document.size() && std::isspace(static_cast<unsigned char>(document[cursor]))) ++cursor;
  if (cursor >= document.size() || document[cursor] != '"') return std::nullopt;
  ++cursor;
  std::string out;
  bool escaping = false;
  for (; cursor < document.size(); ++cursor) {
    const char ch = document[cursor];
    if (escaping) {
      switch (ch) {
        case 'n':
          out.push_back('\n');
          break;
        case 'r':
          out.push_back('\r');
          break;
        case 't':
          out.push_back('\t');
          break;
        default:
          out.push_back(ch);
          break;
      }
      escaping = false;
      continue;
    }
    if (ch == '\\') {
      escaping = true;
      continue;
    }
    if (ch == '"') return out;
    out.push_back(ch);
  }
  return std::nullopt;
}

std::optional<std::int64_t> JsonIntForKey(const std::string& document,
                                          std::string_view key) {
  if (document.size() > kDescriptorBytesLimit) return std::nullopt;
  const std::string needle = "\"" + std::string(key) + "\"";
  const auto key_pos = document.find(needle);
  if (key_pos == std::string::npos) return std::nullopt;
  const auto colon = document.find(':', key_pos + needle.size());
  if (colon == std::string::npos) return std::nullopt;
  std::size_t cursor = colon + 1;
  while (cursor < document.size() && std::isspace(static_cast<unsigned char>(document[cursor]))) ++cursor;
  std::size_t end = cursor;
  if (end < document.size() && document[end] == '-') ++end;
  while (end < document.size() && std::isdigit(static_cast<unsigned char>(document[end]))) ++end;
  if (end == cursor) return std::nullopt;
  try {
    return std::stoll(document.substr(cursor, end - cursor));
  } catch (...) {
    return std::nullopt;
  }
}

bool JsonBoolForKey(const std::string& document, std::string_view key, bool default_value) {
  const std::string needle = "\"" + std::string(key) + "\"";
  const auto key_pos = document.find(needle);
  if (key_pos == std::string::npos) return default_value;
  const auto colon = document.find(':', key_pos + needle.size());
  if (colon == std::string::npos) return default_value;
  std::size_t cursor = colon + 1;
  while (cursor < document.size() && std::isspace(static_cast<unsigned char>(document[cursor]))) ++cursor;
  if (document.compare(cursor, 4, "true") == 0) return true;
  if (document.compare(cursor, 5, "false") == 0) return false;
  return default_value;
}

bool HasKind(const std::string& document, std::string_view kind) {
  if (document.size() > kDescriptorBytesLimit) return false;
  return document.find("\"kind\":\"" + std::string(kind) + "\"") != std::string::npos;
}

std::vector<std::uint8_t> BytesFromValue(const scratchbird::engine::sblr::SblrValue& value) {
  if (value.payload_kind == scratchbird::engine::sblr::SblrValuePayloadKind::binary) return value.binary_value;
  const std::string text = ValueAsText(value);
  return std::vector<std::uint8_t>(text.begin(), text.end());
}

bool Int64FromValue(const scratchbird::engine::sblr::SblrValue& value, std::int64_t* out) {
  if (out == nullptr || IsSqlNull(value)) return false;
  if (value.has_int64_value) {
    *out = value.int64_value;
    return true;
  }
  try {
    *out = std::stoll(ValueAsText(value));
    return true;
  } catch (...) {
    return false;
  }
}

struct LobDescriptor {
  std::string locator_id = "lob:inline";
  std::string state = "open";
  std::string mode = "read_write";
  std::string lob_class = "binary";
  std::string media_type = "application/octet-stream";
  std::vector<std::uint8_t> data;
  bool valid = true;
};

std::string MakeLobDescriptorJson(const LobDescriptor& descriptor) {
  std::string out = "{\"kind\":\"lob_locator\",\"locator_id\":";
  out += JsonEscape(descriptor.locator_id);
  out += ",\"state\":";
  out += JsonEscape(descriptor.state);
  out += ",\"mode\":";
  out += JsonEscape(descriptor.mode);
  out += ",\"class\":";
  out += JsonEscape(descriptor.lob_class);
  out += ",\"media_type\":";
  out += JsonEscape(descriptor.media_type);
  out += ",\"data_hex\":";
  out += JsonEscape(HexEncodeBytes(descriptor.data));
  out += ",\"size\":";
  out += std::to_string(descriptor.data.size());
  out += ",\"valid\":";
  out += descriptor.valid ? "true" : "false";
  out += "}";
  return out;
}

LobDescriptor DefaultLob() {
  LobDescriptor descriptor;
  return descriptor;
}

std::optional<LobDescriptor> ParseLobDescriptor(const scratchbird::engine::sblr::SblrValue& value) {
  if (IsSqlNull(value)) return std::nullopt;
  const std::string document = Trim(ValueAsText(value));
  if (!HasKind(document, "lob_locator")) return std::nullopt;
  LobDescriptor descriptor;
  descriptor.locator_id = JsonStringForKey(document, "locator_id").value_or("lob:inline");
  descriptor.state = JsonStringForKey(document, "state").value_or("open");
  descriptor.mode = JsonStringForKey(document, "mode").value_or("read_write");
  descriptor.lob_class = JsonStringForKey(document, "class").value_or("binary");
  descriptor.media_type = JsonStringForKey(document, "media_type").value_or("application/octet-stream");
  descriptor.valid = JsonBoolForKey(document, "valid", true);
  const std::string data_hex = JsonStringForKey(document, "data_hex").value_or("");
  if (!HexDecodeBytes(data_hex, &descriptor.data)) return std::nullopt;
  if (descriptor.data.size() > kLobBytesLimit) return std::nullopt;
  return descriptor;
}

FunctionCallResult InvalidLob(const FunctionCallRequest& request, std::string detail) {
  return RefuseFunctionInvalidInput(request, std::move(detail));
}

FunctionCallResult LobDescriptorResult(const FunctionCallRequest& request, const LobDescriptor& descriptor) {
  return MakeFunctionSuccess(request, {MakeTextValue("json_document", MakeLobDescriptorJson(descriptor))});
}

std::string MediaForClass(std::string_view lob_class) {
  if (lob_class == "text" || lob_class == "character" || lob_class == "clob") return "text/plain";
  return "application/octet-stream";
}

std::string NormalizeClass(std::string value) {
  value = LowerAscii(Trim(std::move(value)));
  if (value == "text" || value == "character" || value == "clob") return "text";
  if (value.empty() || value == "binary" || value == "blob") return "binary";
  return value;
}

std::string MakeLocatorDescriptor(std::string locator_id,
                                  std::string source_kind,
                                  std::int64_t position,
                                  bool valid) {
  std::string out = "{\"kind\":\"locator\",\"locator_id\":";
  out += JsonEscape(std::move(locator_id));
  out += ",\"source_handle_kind\":";
  out += JsonEscape(std::move(source_kind));
  out += ",\"position\":";
  out += std::to_string(position);
  out += ",\"valid\":";
  out += valid ? "true" : "false";
  out += "}";
  return out;
}

bool LocatorValid(const scratchbird::engine::sblr::SblrValue& value) {
  if (IsSqlNull(value)) return false;
  const std::string document = Trim(ValueAsText(value));
  if (HasKind(document, "lob_locator")) {
    return JsonBoolForKey(document, "valid", true) && JsonStringForKey(document, "state").value_or("open") != "closed";
  }
  if (HasKind(document, "locator")) return JsonBoolForKey(document, "valid", true);
  return false;
}

}  // namespace

bool IsLobLocatorFunction(const FunctionCallRequest& request) {
  return request.context.function_id.rfind("sb.lob.", 0) == 0 ||
         request.context.function_id.rfind("sb.locator.", 0) == 0;
}

FunctionCallResult DispatchLobLocatorFunction(const FunctionCallRequest& request) {
  const auto& id = request.context.function_id;

  if (IdIs(id, {"sb.lob.create"})) {
    if (request.arguments.size() > 2) return InvalidLob(request, "lob_create expects optional class and media type");
    LobDescriptor descriptor = DefaultLob();
    descriptor.locator_id = "lob:create";
    if (!request.arguments.empty() && !IsSqlNull(request.arguments[0].value)) {
      descriptor.lob_class = NormalizeClass(ValueAsText(request.arguments[0].value));
      descriptor.media_type = MediaForClass(descriptor.lob_class);
    }
    if (request.arguments.size() == 2 && !IsSqlNull(request.arguments[1].value)) {
      descriptor.media_type = ValueAsText(request.arguments[1].value);
    }
    return LobDescriptorResult(request, descriptor);
  }

  if (IdIs(id, {"sb.lob.open"})) {
    if (request.arguments.size() > 2) return InvalidLob(request, "lob_open expects optional locator and mode");
    LobDescriptor descriptor = DefaultLob();
    if (!request.arguments.empty()) {
      const auto parsed = ParseLobDescriptor(request.arguments[0].value);
      if (!parsed) return InvalidLob(request, "lob_open requires a lob_locator descriptor");
      descriptor = *parsed;
    }
    descriptor.state = "open";
    if (request.arguments.size() == 2 && !IsSqlNull(request.arguments[1].value)) {
      const std::string mode = LowerAscii(Trim(ValueAsText(request.arguments[1].value)));
      if (mode != "read" && mode != "write" && mode != "read_write") {
        return InvalidLob(request, "lob_open mode must be read, write, or read_write");
      }
      descriptor.mode = mode;
    }
    return LobDescriptorResult(request, descriptor);
  }

  if (IdIs(id, {"sb.lob.close"})) {
    if (request.arguments.size() > 1) return InvalidLob(request, "lob_close expects optional locator");
    LobDescriptor descriptor = DefaultLob();
    if (!request.arguments.empty()) {
      const auto parsed = ParseLobDescriptor(request.arguments[0].value);
      if (!parsed) return InvalidLob(request, "lob_close requires a lob_locator descriptor");
      descriptor = *parsed;
    }
    descriptor.state = "closed";
    return LobDescriptorResult(request, descriptor);
  }

  if (IdIs(id, {"sb.lob.size"})) {
    if (request.arguments.size() > 1) return InvalidLob(request, "lob_size expects optional locator");
    if (request.arguments.empty()) return MakeFunctionSuccess(request, {MakeInt64Value("int64", 0)});
    const auto parsed = ParseLobDescriptor(request.arguments[0].value);
    if (!parsed) return InvalidLob(request, "lob_size requires a lob_locator descriptor");
    return MakeFunctionSuccess(request, {MakeInt64Value("int64", static_cast<std::int64_t>(parsed->data.size()))});
  }

  if (IdIs(id, {"sb.lob.read"})) {
    if (request.arguments.size() != 3) return InvalidLob(request, "lob_read expects locator, 1-based offset, and length");
    const auto parsed = ParseLobDescriptor(request.arguments[0].value);
    if (!parsed) return InvalidLob(request, "lob_read requires a lob_locator descriptor");
    std::int64_t offset = 0;
    std::int64_t length = 0;
    if (!Int64FromValue(request.arguments[1].value, &offset) || !Int64FromValue(request.arguments[2].value, &length) ||
        offset < 1 || length < 0) {
      return InvalidLob(request, "lob_read offset must be 1-based and length must be non-negative");
    }
    const std::size_t start = static_cast<std::size_t>(offset - 1);
    if (start >= parsed->data.size()) {
      if (parsed->lob_class == "text") return MakeFunctionSuccess(request, {MakeTextValue("character", "")});
      return MakeFunctionSuccess(request, {MakeBinaryValue("binary", {})});
    }
    const std::size_t count = std::min<std::size_t>(static_cast<std::size_t>(length), parsed->data.size() - start);
    std::vector<std::uint8_t> bytes(parsed->data.begin() + static_cast<std::ptrdiff_t>(start),
                                    parsed->data.begin() + static_cast<std::ptrdiff_t>(start + count));
    if (parsed->lob_class == "text") {
      return MakeFunctionSuccess(request, {MakeTextValue("character", std::string(bytes.begin(), bytes.end()))});
    }
    return MakeFunctionSuccess(request, {MakeBinaryValue("binary", std::move(bytes))});
  }

  if (IdIs(id, {"sb.lob.write"})) {
    if (request.arguments.size() != 3) return InvalidLob(request, "lob_write expects locator, 1-based offset, and data");
    auto parsed = ParseLobDescriptor(request.arguments[0].value);
    if (!parsed) return InvalidLob(request, "lob_write requires a lob_locator descriptor");
    std::int64_t offset = 0;
    if (!Int64FromValue(request.arguments[1].value, &offset) || offset < 1) {
      return InvalidLob(request, "lob_write offset must be 1-based");
    }
    std::vector<std::uint8_t> bytes = BytesFromValue(request.arguments[2].value);
    const std::size_t start = static_cast<std::size_t>(offset - 1);
    if (start > parsed->data.size() || parsed->data.size() + bytes.size() > kLobBytesLimit) {
      return InvalidLob(request, "lob_write would exceed bounded locator storage");
    }
    if (start + bytes.size() > parsed->data.size()) parsed->data.resize(start + bytes.size(), 0);
    std::copy(bytes.begin(), bytes.end(), parsed->data.begin() + static_cast<std::ptrdiff_t>(start));
    return LobDescriptorResult(request, *parsed);
  }

  if (IdIs(id, {"sb.lob.append"})) {
    if (request.arguments.empty() || request.arguments.size() > 2) {
      return InvalidLob(request, "lob_append expects data or locator plus data");
    }
    LobDescriptor descriptor = DefaultLob();
    const scratchbird::engine::sblr::SblrValue* data_value = nullptr;
    if (request.arguments.size() == 1) {
      data_value = &request.arguments[0].value;
    } else {
      const auto parsed = ParseLobDescriptor(request.arguments[0].value);
      if (!parsed) return InvalidLob(request, "lob_append requires a lob_locator descriptor");
      descriptor = *parsed;
      data_value = &request.arguments[1].value;
    }
    std::vector<std::uint8_t> bytes = BytesFromValue(*data_value);
    if (descriptor.data.size() + bytes.size() > kLobBytesLimit) {
      return InvalidLob(request, "lob_append would exceed bounded locator storage");
    }
    descriptor.data.insert(descriptor.data.end(), bytes.begin(), bytes.end());
    return LobDescriptorResult(request, descriptor);
  }

  if (IdIs(id, {"sb.lob.truncate"})) {
    if (request.arguments.size() != 2) return InvalidLob(request, "lob_truncate expects locator and length");
    auto parsed = ParseLobDescriptor(request.arguments[0].value);
    if (!parsed) return InvalidLob(request, "lob_truncate requires a lob_locator descriptor");
    std::int64_t length = 0;
    if (!Int64FromValue(request.arguments[1].value, &length) || length < 0) {
      return InvalidLob(request, "lob_truncate length must be non-negative");
    }
    parsed->data.resize(std::min<std::size_t>(static_cast<std::size_t>(length), parsed->data.size()));
    return LobDescriptorResult(request, *parsed);
  }

  if (IdIs(id, {"sb.lob.locator_to_text"})) {
    if (request.arguments.size() > 1) return InvalidLob(request, "lob_locator_to_text expects optional locator");
    if (request.arguments.empty()) return MakeFunctionSuccess(request, {MakeTextValue("character", "")});
    const auto parsed = ParseLobDescriptor(request.arguments[0].value);
    if (!parsed) return InvalidLob(request, "lob_locator_to_text requires a lob_locator descriptor");
    return MakeFunctionSuccess(request, {MakeTextValue("character", std::string(parsed->data.begin(), parsed->data.end()))});
  }

  if (IdIs(id, {"sb.lob.locator_to_binary"})) {
    if (request.arguments.size() > 1) return InvalidLob(request, "lob_locator_to_binary expects optional locator");
    if (request.arguments.empty()) return MakeFunctionSuccess(request, {MakeBinaryValue("binary", {})});
    const auto parsed = ParseLobDescriptor(request.arguments[0].value);
    if (!parsed) return InvalidLob(request, "lob_locator_to_binary requires a lob_locator descriptor");
    return MakeFunctionSuccess(request, {MakeBinaryValue("binary", parsed->data)});
  }

  if (IdIs(id, {"sb.locator.locator"})) {
    if (!request.arguments.empty()) return InvalidLob(request, "locator expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("json_document", MakeLocatorDescriptor("locator:inline", "generic", 0, true))});
  }

  if (IdIs(id, {"sb.locator.validity"})) {
    if (request.arguments.size() > 1) return InvalidLob(request, "locator_validity expects optional locator");
    const bool valid = !request.arguments.empty() && LocatorValid(request.arguments[0].value);
    return MakeFunctionSuccess(request, {MakeInt64Value("boolean", valid ? 1 : 0)});
  }

  if (IdIs(id, {"sb.locator.current_row"})) {
    if (!request.arguments.empty()) return InvalidLob(request, "current_row_locator marker expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("json_document", MakeLocatorDescriptor("row:current", "row", 0, true))});
  }

  return RefuseFunctionWithDiagnostic(request,
                                      scratchbird::engine::sblr::SblrStatusCode::unsupported_feature,
                                      "SB_DIAG_FUNCTION_FAMILY_HANDLER_MISSING",
                                      "LOB/locator function is not implemented");
}

}  // namespace scratchbird::engine::functions
