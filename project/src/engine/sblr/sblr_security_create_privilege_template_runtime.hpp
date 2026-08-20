#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
using PrivilegeTemplateUuid=std::array<std::uint8_t,16>;
using PrivilegeTemplateSha=std::array<std::uint8_t,32>;
struct SblrSecurityCreatePrivilegeTemplateRequestV1 { PrivilegeTemplateUuid receipt{}; std::uint64_t occurrence=0; std::uint32_t template_occurrence=0; };
struct SblrSecurityCreatePrivilegeTemplateDescriptorV1 { std::array<std::uint8_t,400> body{}; PrivilegeTemplateSha evidence{}; std::uint64_t availability=0; };
struct SblrSecurityCreatePrivilegeTemplateResultV1 { std::array<std::uint8_t,240> body{}; PrivilegeTemplateSha evidence{}; std::uint64_t availability=0; PrivilegeTemplateUuid publication_barrier{}; };
std::vector<std::uint8_t> EncodeSblrSecurityCreatePrivilegeTemplateRequestV1(const SblrSecurityCreatePrivilegeTemplateRequestV1&);
bool DecodeSblrSecurityCreatePrivilegeTemplateRequestV1(const std::uint8_t*,std::size_t,SblrSecurityCreatePrivilegeTemplateRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrSecurityCreatePrivilegeTemplateDescriptorV1(const SblrSecurityCreatePrivilegeTemplateDescriptorV1&,bool);
bool DecodeSblrSecurityCreatePrivilegeTemplateDescriptorV1(const std::uint8_t*,std::size_t,SblrSecurityCreatePrivilegeTemplateDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrSecurityCreatePrivilegeTemplateResultV1(const SblrSecurityCreatePrivilegeTemplateResultV1&);
bool DecodeSblrSecurityCreatePrivilegeTemplateResultV1(const std::uint8_t*,std::size_t,SblrSecurityCreatePrivilegeTemplateResultV1*,std::string*);
}
