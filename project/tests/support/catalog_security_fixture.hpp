// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_security_record_codec.hpp"
#include <charconv>
#include <stdexcept>

// Human-readable test fixture boundary only. Never production catalog storage,
// admission, credentials or proof of commitment.
namespace scratchbird::tests {
inline std::map<std::string,std::string> SecurityFixtureAttributes(
    const core::catalog::CatalogSecurityRecord& record) {
  std::map<std::string,std::string> result;
  for(const auto& [key,value]:record.attributes)std::visit([&](const auto& v){
    using T=std::decay_t<decltype(v)>;
    if constexpr(std::is_same_v<T,std::string>)result.emplace(key,v);
    else if constexpr(std::is_same_v<T,bool>)result.emplace(key,v?"1":"0");
    else result.emplace(key,std::to_string(v));
  },value);
  return result;
}
inline core::catalog::CatalogSecurityRecord::AttributeValue SecurityFixtureValue(
    std::string_view name,const std::string& text) {
  namespace c=core::catalog;
  const auto type=c::CatalogSecurityAttributeType(name);
  if(type==c::CatalogValueType::boolean) {
    if(text!="0"&&text!="1")throw std::invalid_argument("fixture boolean");
    return text=="1";
  }
  if(type==c::CatalogValueType::unsigned_integer) {
    core::platform::u64 value=0;
    const auto parsed=std::from_chars(text.data(),text.data()+text.size(),value);
    if(parsed.ec!=std::errc{}||parsed.ptr!=text.data()+text.size())
      throw std::invalid_argument("fixture unsigned integer");
    return value;
  }
  return text;
}
} // namespace scratchbird::tests
