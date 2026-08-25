#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr { struct SblrSecGrantRequestV1{std::array<uint8_t,16> receipt{};uint64_t occurrence=0;}; struct SblrSecGrantDescriptorV1{std::array<uint8_t,16> grant_uuid{},grantee_uuid{},target_object_uuid{},descriptor_evidence{};uint64_t expected_generation=0,catalog_generation=0,security_generation=0,availability=0;}; struct SblrSecGrantResultV1{std::array<uint8_t,16> grant_uuid{};std::array<uint8_t,32> effect_evidence{};uint64_t generation=0,security_generation=0,availability=0;uint8_t status=0,publication_barrier=0;}; std::vector<uint8_t> EncodeSblrSecGrantRequestV1(const SblrSecGrantRequestV1&);bool DecodeSblrSecGrantRequestV1(const uint8_t*,size_t,SblrSecGrantRequestV1*,std::string*);std::vector<uint8_t> EncodeSblrSecGrantDescriptorV1(const SblrSecGrantDescriptorV1&,bool);bool DecodeSblrSecGrantDescriptorV1(const uint8_t*,size_t,SblrSecGrantDescriptorV1*,std::string*,bool);std::vector<uint8_t> EncodeSblrSecGrantResultV1(const SblrSecGrantResultV1&);bool DecodeSblrSecGrantResultV1(const uint8_t*,size_t,SblrSecGrantResultV1*,std::string*);}
