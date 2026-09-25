// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "canonical_utf8.hpp"
#include <cstdint>
#include <string_view>

namespace scratchbird::core::datatypes {
// Borrowed validation: bounded stack, no payload copies or document tree.
class JsonValueScanner {
 public:
  explicit JsonValueScanner(std::string_view value) : value_(value) {}
  bool Validate() { Skip(); return Value(0) && (Skip(), at_ == value_.size()); }
 private:
  std::string_view value_;
  std::size_t at_ = 0;
  void Skip() { while (at_ < value_.size() && (value_[at_] == ' ' || value_[at_] == '\t' || value_[at_] == '\r' || value_[at_] == '\n')) ++at_; }
  bool Take(char c) { if (at_ == value_.size() || value_[at_] != c) return false; ++at_; return true; }
  bool Hex4(unsigned* out) {
    *out = 0;
    for (unsigned i = 0; i < 4; ++i) {
      if (at_ == value_.size()) return false;
      const char c = value_[at_++];
      const unsigned d = c >= '0' && c <= '9' ? c-'0' : c >= 'a' && c <= 'f' ? c-'a'+10 : c >= 'A' && c <= 'F' ? c-'A'+10 : 16;
      if (d == 16) return false;
      *out = (*out << 4) | d;
    }
    return true;
  }
  bool String() {
    if (!Take('"')) return false;
    while (at_ < value_.size()) {
      const auto c = static_cast<unsigned char>(value_[at_++]);
      if (c == '"') return true;
      if (c < 0x20) return false;
      if (c != '\\') continue;
      if (at_ == value_.size()) return false;
      const char escape = value_[at_++];
      if (escape == '"' || escape == '\\' || escape == '/' || escape == 'b' || escape == 'f' || escape == 'n' || escape == 'r' || escape == 't') continue;
      if (escape != 'u') return false;
      unsigned scalar;
      if (!Hex4(&scalar)) return false;
      if (scalar >= 0xdc00 && scalar <= 0xdfff) return false;
      if (scalar >= 0xd800 && scalar <= 0xdbff) {
        if (!Take('\\') || !Take('u') || !Hex4(&scalar) || scalar < 0xdc00 || scalar > 0xdfff) return false;
      }
    }
    return false;
  }
  bool Digit() { if (at_ == value_.size() || value_[at_] < '0' || value_[at_] > '9') return false; ++at_; return true; }
  bool Number() {
    Take('-');
    if (!Take('0')) { if (at_ == value_.size() || value_[at_] < '1' || value_[at_] > '9') return false; while (Digit()) {} }
    if (Take('.')) { if (!Digit()) return false; while (Digit()) {} }
    if (Take('e') || Take('E')) { if (!Take('+')) Take('-'); if (!Digit()) return false; while (Digit()) {} }
    return true;
  }
  bool Value(unsigned depth) {
    if (at_ == value_.size()) return false;
    if (value_[at_] == '"') return String();
    if (value_[at_] == '[' || value_[at_] == '{') {
      if (depth == 256) return false;
      const bool object = value_[at_++] == '{'; const char end = object ? '}' : ']';
      Skip(); if (Take(end)) return true;
      do {
        Skip();
        if (object) { if (!String()) return false; Skip(); if (!Take(':')) return false; Skip(); }
        if (!Value(depth + 1)) return false;
        Skip(); if (Take(end)) return true;
      } while (Take(','));
      return false;
    }
    for (const auto literal : {std::string_view("null"), std::string_view("true"), std::string_view("false")}) {
      if (value_.substr(at_, literal.size()) == literal) { at_ += literal.size(); return true; }
    }
    return Number();
  }
};
inline bool ValidateJsonValue(std::string_view value) {
  return !value.empty() && value.size() <= 16777216 &&
      ValidateCanonicalUtf8(reinterpret_cast<const std::uint8_t*>(value.data()), value.size()) && JsonValueScanner(value).Validate();
}
inline std::uint32_t TextListU32(std::string_view value, std::size_t offset) {
  std::uint32_t n = 0;
  for (unsigned i = 0; i < 4; ++i) n |= static_cast<std::uint32_t>(static_cast<unsigned char>(value[offset+i])) << (8*i);
  return n;
}
inline bool ValidateTextListValue(std::string_view value) {
  if (value.size() < 12 || value.size() > 16777216 || value.substr(0,8) != "SBTL0001") return false;
  const auto count = TextListU32(value,8);
  if (count > (value.size()-12)/5) return false;
  std::size_t at = 12;
  for (std::uint32_t i = 0; i < count; ++i) {
    if (value.size()-at < 5) return false;
    const auto state = static_cast<unsigned char>(value[at++]);
    const auto length = TextListU32(value,at); at += 4;
    if (state > 1 || (state == 0 && length != 0) || length > value.size()-at ||
        !ValidateCanonicalUtf8(reinterpret_cast<const std::uint8_t*>(value.data()+at),length)) return false;
    at += length;
  }
  return at == value.size();
}
} // namespace scratchbird::core::datatypes
