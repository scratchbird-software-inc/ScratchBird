#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
using DdlCreateRewriteRuleUuid=std::array<std::uint8_t,16>; using DdlCreateRewriteRuleSha=std::array<std::uint8_t,32>;
struct SblrDdlCreateRewriteRuleRequestV1 { DdlCreateRewriteRuleUuid receipt{}; std::uint64_t occurrence=0; std::uint32_t rule_occurrence=0; };
struct SblrDdlCreateRewriteRuleDescriptorV1 { std::array<std::uint8_t,400> body{}; DdlCreateRewriteRuleSha evidence{}; std::uint64_t availability=0; };
struct SblrDdlCreateRewriteRuleResultV1 { std::array<std::uint8_t,240> body{}; DdlCreateRewriteRuleSha evidence{}; std::uint64_t availability=0; DdlCreateRewriteRuleUuid publication_barrier{}; };
std::vector<std::uint8_t> EncodeSblrDdlCreateRewriteRuleRequestV1(const SblrDdlCreateRewriteRuleRequestV1&); bool DecodeSblrDdlCreateRewriteRuleRequestV1(const std::uint8_t*,std::size_t,SblrDdlCreateRewriteRuleRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrDdlCreateRewriteRuleDescriptorV1(const SblrDdlCreateRewriteRuleDescriptorV1&,bool); bool DecodeSblrDdlCreateRewriteRuleDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlCreateRewriteRuleDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrDdlCreateRewriteRuleResultV1(const SblrDdlCreateRewriteRuleResultV1&); bool DecodeSblrDdlCreateRewriteRuleResultV1(const std::uint8_t*,std::size_t,SblrDdlCreateRewriteRuleResultV1*,std::string*);
}
