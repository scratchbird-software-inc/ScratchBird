#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr { struct SblrDdlDropRuleRequestV1{std::array<uint8_t,16> operation{},receipt{};}; struct SblrDdlDropRuleDescriptorV1{std::array<uint8_t,272> body{};}; struct SblrDdlDropRuleResultV1{std::array<uint8_t,176> body{};}; std::vector<uint8_t> EncodeSblrDdlDropRuleRequestV1(const SblrDdlDropRuleRequestV1&);bool DecodeSblrDdlDropRuleRequestV1(const uint8_t*,size_t,SblrDdlDropRuleRequestV1*,std::string*);std::vector<uint8_t> EncodeSblrDdlDropRuleDescriptorV1(const SblrDdlDropRuleDescriptorV1&);bool DecodeSblrDdlDropRuleDescriptorV1(const uint8_t*,size_t,SblrDdlDropRuleDescriptorV1*,std::string*);std::vector<uint8_t> EncodeSblrDdlDropRuleResultV1(const SblrDdlDropRuleResultV1&);bool DecodeSblrDdlDropRuleResultV1(const uint8_t*,size_t,SblrDdlDropRuleResultV1*,std::string*);}
