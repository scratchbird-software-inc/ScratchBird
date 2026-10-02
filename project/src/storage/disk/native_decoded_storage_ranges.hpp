// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <array>
#include <cstdint>
#include <limits>
#include <span>

namespace scratchbird::storage::disk::detail {
struct NativeDecodedStorageRange {
  std::uintptr_t begin=0;
  std::size_t bytes=0;
  bool valid=false;
};
template<class T,std::size_t Extent>
NativeDecodedStorageRange DecodedRange(std::span<T,Extent> region) noexcept {
  if(region.empty())return {0,0,true};
  if(!region.data()||region.size()>std::numeric_limits<std::size_t>::max()/sizeof(T))return {};
  const auto begin=reinterpret_cast<std::uintptr_t>(region.data());
  const auto bytes=region.size()*sizeof(T);
  return {begin,bytes,bytes<=std::numeric_limits<std::uintptr_t>::max()-begin};
}
// Callers supply valid process-memory spans. Reject arithmetic wrap and
// aliasing before a decoder can overwrite input or another metadata region.
template<class... Regions> bool DisjointNativeDecodeRegions(Regions... regions) noexcept {
  const std::array<NativeDecodedStorageRange,sizeof...(Regions)> ranges{DecodedRange(regions)...};
  for(std::size_t i=0;i<ranges.size();++i){const auto& a=ranges[i];if(!a.valid)return false;
    if(!a.bytes)continue;
    for(std::size_t j=0;j<i;++j){const auto& b=ranges[j];if(!b.bytes)continue;
      if(a.begin<=b.begin?a.bytes>b.begin-a.begin:b.bytes>a.begin-b.begin)return false;
    }
  }
  return true;
}
} // namespace scratchbird::storage::disk::detail
