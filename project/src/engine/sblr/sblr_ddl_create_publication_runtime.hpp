#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr { struct SblrDdlCreatePublicationRequestV1{std::array<uint8_t,16> operation{},receipt{};}; struct SblrDdlCreatePublicationDescriptorV1{std::array<uint8_t,304> body{};}; struct SblrDdlCreatePublicationResultV1{std::array<uint8_t,176> body{};}; std::vector<uint8_t> EncodeSblrDdlCreatePublicationRequestV1(const SblrDdlCreatePublicationRequestV1&); bool DecodeSblrDdlCreatePublicationRequestV1(const uint8_t*,size_t,SblrDdlCreatePublicationRequestV1*,std::string*); std::vector<uint8_t> EncodeSblrDdlCreatePublicationDescriptorV1(const SblrDdlCreatePublicationDescriptorV1&); bool DecodeSblrDdlCreatePublicationDescriptorV1(const uint8_t*,size_t,SblrDdlCreatePublicationDescriptorV1*,std::string*); std::vector<uint8_t> EncodeSblrDdlCreatePublicationResultV1(const SblrDdlCreatePublicationResultV1&); bool DecodeSblrDdlCreatePublicationResultV1(const uint8_t*,size_t,SblrDdlCreatePublicationResultV1*,std::string*); }
