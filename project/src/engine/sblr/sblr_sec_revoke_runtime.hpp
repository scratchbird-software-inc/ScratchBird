#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr { struct SblrSecRevokeRequestV1{std::array<uint8_t,16> receipt{};uint64_t occurrence=0;}; struct SblrSecRevokeDescriptorV1{std::array<uint8_t,16> grant_uuid{},grantee_uuid{},target_object_uuid{},descriptor_evidence{};uint64_t expected_generation=0,catalog_generation=0,security_generation=0,availability=0;}; struct SblrSecRevokeResultV1{std::array<uint8_t,16> grant_uuid{};std::array<uint8_t,32> effect_evidence{};uint64_t generation=0,security_generation=0,availability=0;uint8_t status=0,publication_barrier=0;}; std::vector<uint8_t> EncodeSblrSecRevokeRequestV1(const SblrSecRevokeRequestV1&);bool DecodeSblrSecRevokeRequestV1(const uint8_t*,size_t,SblrSecRevokeRequestV1*,std::string*);std::vector<uint8_t> EncodeSblrSecRevokeDescriptorV1(const SblrSecRevokeDescriptorV1&,bool);bool DecodeSblrSecRevokeDescriptorV1(const uint8_t*,size_t,SblrSecRevokeDescriptorV1*,std::string*,bool);std::vector<uint8_t> EncodeSblrSecRevokeResultV1(const SblrSecRevokeResultV1&);bool DecodeSblrSecRevokeResultV1(const uint8_t*,size_t,SblrSecRevokeResultV1*,std::string*);}
