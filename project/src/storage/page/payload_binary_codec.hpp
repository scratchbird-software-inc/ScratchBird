// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "uuid.hpp"
#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
namespace scratchbird::storage::page::payload_binary {
inline constexpr std::size_t kMaximumBytes=64*1024*1024;
inline void PutU64(std::string& out,std::uint64_t value) {
  for(unsigned n=0;n<8;++n)out.push_back(static_cast<char>(value>>(8*n)));
}
inline bool PutString(std::string& out,std::string_view value) {
  if(value.size()>kMaximumBytes||out.size()>kMaximumBytes-8||value.size()>kMaximumBytes-out.size()-8)return false;
  PutU64(out,value.size());out.append(value);return true;
}
inline bool PutUuid(std::string& out,const core::platform::TypedUuid& id) {
  if(static_cast<unsigned>(id.kind)>static_cast<unsigned>(core::platform::UuidKind::unknown)||
      (!id.value.is_nil()&&!core::uuid::IsEngineIdentityUuid(id.value)))return false;
  out.push_back(static_cast<char>(id.kind));
  out.append(reinterpret_cast<const char*>(id.value.bytes.data()),16);return true;
}
struct Reader {
  std::string_view bytes; std::size_t cursor=0;
  bool U64(std::uint64_t& value) {
    if(cursor>bytes.size()||bytes.size()-cursor<8)return false;
    value=0;for(unsigned n=0;n<8;++n)value|=static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[cursor++]))<<(8*n);
    return true;
  }
  bool String(std::string& value) {
    std::uint64_t size=0;if(!U64(size)||size>kMaximumBytes||size>bytes.size()-cursor)return false;
    value.assign(bytes.substr(cursor,size));cursor+=size;return true;
  }
  bool Uuid(core::platform::TypedUuid& id) {
    if(cursor>bytes.size()||bytes.size()-cursor<17)return false;
    const auto kind=static_cast<unsigned char>(bytes[cursor++]);
    if(kind>static_cast<unsigned>(core::platform::UuidKind::unknown))return false;
    id.kind=static_cast<core::platform::UuidKind>(kind);
    std::copy_n(reinterpret_cast<const std::uint8_t*>(bytes.data()+cursor),16,id.value.bytes.begin());cursor+=16;
    return id.value.is_nil()||core::uuid::IsEngineIdentityUuid(id.value);
  }
};
} // namespace scratchbird::storage::page::payload_binary
