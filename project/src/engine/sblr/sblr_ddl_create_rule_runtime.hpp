#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr { struct SblrDdlCreateRuleRequestV1{std::array<uint8_t,16> operation{},receipt{};}; struct SblrDdlCreateRuleDescriptorV1{std::array<uint8_t,304> body{};}; struct SblrDdlCreateRuleResultV1{std::array<uint8_t,176> body{};}; std::vector<uint8_t> EncodeSblrDdlCreateRuleRequestV1(const SblrDdlCreateRuleRequestV1&);bool DecodeSblrDdlCreateRuleRequestV1(const uint8_t*,size_t,SblrDdlCreateRuleRequestV1*,std::string*);std::vector<uint8_t> EncodeSblrDdlCreateRuleDescriptorV1(const SblrDdlCreateRuleDescriptorV1&);bool DecodeSblrDdlCreateRuleDescriptorV1(const uint8_t*,size_t,SblrDdlCreateRuleDescriptorV1*,std::string*);std::vector<uint8_t> EncodeSblrDdlCreateRuleResultV1(const SblrDdlCreateRuleResultV1&);bool DecodeSblrDdlCreateRuleResultV1(const uint8_t*,size_t,SblrDdlCreateRuleResultV1*,std::string*);}
