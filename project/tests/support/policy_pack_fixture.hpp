// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "hash_digest.hpp"
#include <filesystem>
#include <fstream>
#include <regex>
#include <stdexcept>

namespace scratchbird::tests {
// Explicit fixture mutation/resealing, not a production signature/provider.
// Only use on a private owned copy of the public source pack.
template<class Edit> void RewriteAndResealPolicyPack(const std::filesystem::path& root,Edit edit) {
  const auto require=[](bool ok,const char* why){if(!ok)throw std::runtime_error(why);};
  const auto read=[&](const auto& path){std::ifstream input(path,std::ios::binary);
    require(bool(input),"policy fixture read failed");return std::string(std::istreambuf_iterator<char>(input),{});};
  const auto write=[&](const auto& path,const std::string& value){std::ofstream output(path,std::ios::binary|std::ios::trunc);
    output.write(value.data(),value.size());require(bool(output),"policy fixture write failed");};
  const auto replace=[](std::string& value,const std::string& from,const std::string& to){
    for(std::size_t at=0;(at=value.find(from,at))!=value.npos;at+=to.size())value.replace(at,from.size(),to);};
  const auto sha=[&](const std::string& value){std::string normalized;normalized.reserve(value.size());
    for(std::size_t i=0;i<value.size();++i){if(value[i]=='\r'&&i+1<value.size()&&value[i+1]=='\n')continue;normalized+=value[i];}
    const auto result=core::hash::ComputeSha256Digest(reinterpret_cast<const core::platform::byte*>(normalized.data()),normalized.size());
    require(result.ok(),"policy fixture hash failed");return core::hash::HexLower(result.digest);};
  auto manifest=read(root/"POLICY_PACK_MANIFEST.json");edit(std::string("POLICY_PACK_MANIFEST.json"),manifest,replace);
  const auto original=manifest;
  const std::regex entry(R"re("path"\s*:\s*"([^"]+)"\s*,\s*"sha256"\s*:\s*"([0-9a-f]{64})")re");
  std::string aggregate;unsigned files=0;
  for(auto it=std::sregex_iterator(original.begin(),original.end(),entry);it!=std::sregex_iterator();++it){
    const auto relative=(*it)[1].str();auto value=read(root/relative);edit(relative,value,replace);write(root/relative,value);
    const auto digest=sha(value);replace(manifest,(*it)[2].str(),digest);
    aggregate+=relative;aggregate.push_back('\0');aggregate+=digest;aggregate.push_back('\n');++files;
  }
  require(files==9,"policy fixture retains complete manifest");
  const std::regex field(R"re("content_sha256"\s*:\s*"([0-9a-f]{64})")re");std::smatch match;
  require(std::regex_search(manifest,match,field),"policy fixture aggregate field missing");
  manifest.replace(match.position(1),match.length(1),sha(aggregate));write(root/"POLICY_PACK_MANIFEST.json",manifest);
}
} // namespace scratchbird::tests
