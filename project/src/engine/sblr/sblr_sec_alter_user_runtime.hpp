#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
struct SblrSecAlterUserRequestV1 { std::array<std::uint8_t,16> receipt{},frame{}; std::uint64_t frame_generation=0, occurrence=0; };
struct SblrSecAlterUserDescriptorV1 { std::array<std::uint8_t,16> user_uuid{},default_role_uuid{},security_snapshot_uuid{},policy_snapshot_uuid{},password_digest{},secret_evidence{},descriptor_evidence{}; std::uint64_t expected_generation=0,option_mask=0,catalog_generation=0,security_generation=0,availability=0; std::uint8_t authentication_method=0,account_state=0,password_present=0,default_role_present=0; };
struct SblrSecAlterUserResultV1 { std::array<std::uint8_t,16> user_uuid{}; std::uint64_t generation=0,security_generation=0,availability=0; std::array<std::uint8_t,32> effect_evidence{}; std::uint8_t status=0,publication_barrier=0; };
std::vector<std::uint8_t> EncodeSblrSecAlterUserRequestV1(const SblrSecAlterUserRequestV1&); bool DecodeSblrSecAlterUserRequestV1(const std::uint8_t*,std::size_t,SblrSecAlterUserRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrSecAlterUserDescriptorV1(const SblrSecAlterUserDescriptorV1&,bool); bool DecodeSblrSecAlterUserDescriptorV1(const std::uint8_t*,std::size_t,SblrSecAlterUserDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrSecAlterUserResultV1(const SblrSecAlterUserResultV1&); bool DecodeSblrSecAlterUserResultV1(const std::uint8_t*,std::size_t,SblrSecAlterUserResultV1*,std::string*);
}
