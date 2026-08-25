#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr { struct SblrSecDropUserRequestV1{std::array<uint8_t,16> receipt{};uint64_t occurrence=0;}; struct SblrSecDropUserDescriptorV1{std::array<uint8_t,16> user_uuid{},descriptor_evidence{};uint64_t expected_generation=0,catalog_generation=0,security_generation=0,availability=0;}; struct SblrSecDropUserResultV1{std::array<uint8_t,16> user_uuid{};std::array<uint8_t,32> effect_evidence{};uint64_t generation=0,security_generation=0,availability=0;uint8_t status=0,publication_barrier=0;}; std::vector<uint8_t> EncodeSblrSecDropUserRequestV1(const SblrSecDropUserRequestV1&);bool DecodeSblrSecDropUserRequestV1(const uint8_t*,size_t,SblrSecDropUserRequestV1*,std::string*);std::vector<uint8_t> EncodeSblrSecDropUserDescriptorV1(const SblrSecDropUserDescriptorV1&,bool);bool DecodeSblrSecDropUserDescriptorV1(const uint8_t*,size_t,SblrSecDropUserDescriptorV1*,std::string*,bool);std::vector<uint8_t> EncodeSblrSecDropUserResultV1(const SblrSecDropUserResultV1&);bool DecodeSblrSecDropUserResultV1(const uint8_t*,size_t,SblrSecDropUserResultV1*,std::string*);}
