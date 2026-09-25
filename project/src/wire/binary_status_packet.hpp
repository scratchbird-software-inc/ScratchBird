// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "public_result_packet.hpp"
#include "../core/platform/runtime_platform.hpp"
#include <sstream>
#include <stdexcept>
namespace scratchbird::wire::binary_status {
// Status structure and scalar presentation fragments remain opaque to the
// transport. UUID fragments are always native 16-byte atoms; only a client
// display renderer may turn them into hexadecimal UUID text.
inline constexpr std::string_view kContract = "binary_status_json.v1";
struct Text {
  std::string value;
};
inline std::ostream& operator<<(std::ostream& out, const Text& text) {
  return out << text.value;
}
struct Identity {
  std::string bytes;
  explicit Identity(const core::platform::Uuid& id)
      : bytes(reinterpret_cast<const char*>(id.bytes.data()),id.bytes.size()) {}
  explicit Identity(const std::array<std::uint8_t,16>& id)
      : bytes(reinterpret_cast<const char*>(id.data()),id.size()) {}
  explicit Identity(std::string_view value) : bytes(value.empty() ? std::string(16,'\0') : std::string(value)) {
    if (bytes.size()!=16) throw std::invalid_argument("status_uuid_requires_binary16");
  }
};
inline bool Decode(std::string_view bytes, std::vector<public_result::Field>* output) {
  std::vector<public_result::Field> fields;
  if (!public_result::Decode(bytes,&fields) || fields.empty() ||
      fields.front().name!="contract" || fields.front().kind!=public_result::Kind::text ||
      fields.front().value!=kContract) return false;
  for (std::size_t i=1;i<fields.size();++i)
    if (!((fields[i].name=="text" && fields[i].kind==public_result::Kind::text) ||
          (fields[i].name=="uuid" && fields[i].kind==public_result::Kind::uuid))) return false;
  *output=std::move(fields);return true;
}
class Stream {
  std::ostringstream pending_;
  std::vector<public_result::Field> fields_{{"contract",public_result::Kind::text,std::string(kContract)}};
  void Flush() {
    auto text=pending_.str();
    if (!text.empty()) fields_.push_back({"text",public_result::Kind::text,std::move(text)});
    pending_.str(""); pending_.clear();
  }
 public:
  Stream& operator<<(const Text& text) {
    pending_ << text.value; return *this;
  }
  Stream& operator<<(const Identity& id) {
    Flush();fields_.push_back({"uuid",public_result::Kind::uuid,id.bytes});return *this;
  }
  Stream& operator<<(std::string_view text) {
    if (text.starts_with(public_result::kMagic)) {
      std::vector<public_result::Field> nested;
      if (!scratchbird::wire::binary_status::Decode(text,&nested)) throw std::invalid_argument("status_nested_packet_invalid");
      Flush();fields_.insert(fields_.end(),std::make_move_iterator(nested.begin()+1),std::make_move_iterator(nested.end()));
    } else pending_<<text;
    return *this;
  }
  Stream& operator<<(const std::string& text) { return *this << std::string_view(text); }
  Stream& operator<<(const char* text) { pending_<<text; return *this; }
  template<class T> Stream& operator<<(const T& value) { pending_<<value;return *this; }
  std::string str() {
    Flush();std::string encoded;
    if (!public_result::Encode(fields_,&encoded)) throw std::invalid_argument("status_packet_invalid");
    return encoded;
  }
};
}  // namespace scratchbird::wire::binary_status
