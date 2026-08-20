#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
using PrivilegeTemplateUuid=std::array<std::uint8_t,16>;
using PrivilegeTemplateSha=std::array<std::uint8_t,32>;
struct SblrSecurityDropPrivilegeTemplateRequestV1 { PrivilegeTemplateUuid receipt{}; std::uint64_t occurrence=0; std::uint32_t template_occurrence=0; };
struct SblrSecurityDropPrivilegeTemplateDescriptorV1 { std::array<std::uint8_t,400> body{}; PrivilegeTemplateSha evidence{}; std::uint64_t availability=0; };
struct SblrSecurityDropPrivilegeTemplateResultV1 { std::array<std::uint8_t,240> body{}; PrivilegeTemplateSha evidence{}; std::uint64_t availability=0; PrivilegeTemplateUuid publication_barrier{}; };
std::vector<std::uint8_t> EncodeSblrSecurityDropPrivilegeTemplateRequestV1(const SblrSecurityDropPrivilegeTemplateRequestV1&);
bool DecodeSblrSecurityDropPrivilegeTemplateRequestV1(const std::uint8_t*,std::size_t,SblrSecurityDropPrivilegeTemplateRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrSecurityDropPrivilegeTemplateDescriptorV1(const SblrSecurityDropPrivilegeTemplateDescriptorV1&,bool);
bool DecodeSblrSecurityDropPrivilegeTemplateDescriptorV1(const std::uint8_t*,std::size_t,SblrSecurityDropPrivilegeTemplateDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrSecurityDropPrivilegeTemplateResultV1(const SblrSecurityDropPrivilegeTemplateResultV1&);
bool DecodeSblrSecurityDropPrivilegeTemplateResultV1(const std::uint8_t*,std::size_t,SblrSecurityDropPrivilegeTemplateResultV1*,std::string*);
}
