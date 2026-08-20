#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
using PrivilegeTemplateUuid=std::array<std::uint8_t,16>;
using PrivilegeTemplateSha=std::array<std::uint8_t,32>;
struct SblrSecurityAlterPrivilegeTemplateRequestV1 { PrivilegeTemplateUuid receipt{}; std::uint64_t occurrence=0; std::uint32_t template_occurrence=0; };
struct SblrSecurityAlterPrivilegeTemplateDescriptorV1 { std::array<std::uint8_t,400> body{}; PrivilegeTemplateSha evidence{}; std::uint64_t availability=0; };
struct SblrSecurityAlterPrivilegeTemplateResultV1 { std::array<std::uint8_t,240> body{}; PrivilegeTemplateSha evidence{}; std::uint64_t availability=0; PrivilegeTemplateUuid publication_barrier{}; };
std::vector<std::uint8_t> EncodeSblrSecurityAlterPrivilegeTemplateRequestV1(const SblrSecurityAlterPrivilegeTemplateRequestV1&);
bool DecodeSblrSecurityAlterPrivilegeTemplateRequestV1(const std::uint8_t*,std::size_t,SblrSecurityAlterPrivilegeTemplateRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrSecurityAlterPrivilegeTemplateDescriptorV1(const SblrSecurityAlterPrivilegeTemplateDescriptorV1&,bool);
bool DecodeSblrSecurityAlterPrivilegeTemplateDescriptorV1(const std::uint8_t*,std::size_t,SblrSecurityAlterPrivilegeTemplateDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrSecurityAlterPrivilegeTemplateResultV1(const SblrSecurityAlterPrivilegeTemplateResultV1&);
bool DecodeSblrSecurityAlterPrivilegeTemplateResultV1(const std::uint8_t*,std::size_t,SblrSecurityAlterPrivilegeTemplateResultV1*,std::string*);
}
