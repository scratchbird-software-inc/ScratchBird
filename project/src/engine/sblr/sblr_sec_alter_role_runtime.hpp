#pragma once
#include "sblr_sec_drop_role_runtime.hpp"
namespace scratchbird::engine::sblr { using SblrSecAlterRoleRequestV1=SblrSecDropRoleRequestV1; using SblrSecAlterRoleDescriptorV1=SblrSecDropRoleDescriptorV1; using SblrSecAlterRoleResultV1=SblrSecDropRoleResultV1;
std::vector<std::uint8_t> EncodeSblrSecAlterRoleRequestV1(const SblrSecAlterRoleRequestV1&); bool DecodeSblrSecAlterRoleRequestV1(const std::uint8_t*,std::size_t,SblrSecAlterRoleRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrSecAlterRoleDescriptorV1(const SblrSecAlterRoleDescriptorV1&,bool); bool DecodeSblrSecAlterRoleDescriptorV1(const std::uint8_t*,std::size_t,SblrSecAlterRoleDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrSecAlterRoleResultV1(const SblrSecAlterRoleResultV1&); bool DecodeSblrSecAlterRoleResultV1(const std::uint8_t*,std::size_t,SblrSecAlterRoleResultV1*,std::string*); }
