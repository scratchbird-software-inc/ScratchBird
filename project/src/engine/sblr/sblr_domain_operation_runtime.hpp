#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr{using DomainOperationUuid=std::array<std::uint8_t,16>;using DomainOperationSha=std::array<std::uint8_t,32>;struct SblrDomainOperationRequestV1{DomainOperationUuid receipt{};std::uint64_t occurrence=0;std::uint32_t domain_operation_occurrence=0;};struct SblrDomainOperationDescriptorV1{std::array<std::uint8_t,360>canonical_body{};DomainOperationSha evidence{};std::uint64_t availability_generation=0;};struct SblrDomainOperationResultV1{std::array<std::uint8_t,176>canonical_body{};DomainOperationSha executor_evidence{};std::uint64_t availability_generation=0;};std::vector<std::uint8_t>EncodeSblrDomainOperationRequestV1(const SblrDomainOperationRequestV1&);bool DecodeSblrDomainOperationRequestV1(const std::uint8_t*,std::size_t,SblrDomainOperationRequestV1*,std::string*);std::vector<std::uint8_t>EncodeSblrDomainOperationDescriptorV1(const SblrDomainOperationDescriptorV1&,bool);bool DecodeSblrDomainOperationDescriptorV1(const std::uint8_t*,std::size_t,SblrDomainOperationDescriptorV1*,std::string*,bool);std::vector<std::uint8_t>EncodeSblrDomainOperationResultV1(const SblrDomainOperationResultV1&);bool DecodeSblrDomainOperationResultV1(const std::uint8_t*,std::size_t,SblrDomainOperationResultV1*,std::string*);}
