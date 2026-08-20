#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
using RrUuid=std::array<std::uint8_t,16>; using RrSha=std::array<std::uint8_t,32>;
struct SblrReservationReleaseRequestV1{RrUuid receipt{},transaction{},relation{};std::uint64_t occurrence=0;};
struct SblrReservationReleaseDescriptorV1{RrUuid reservation{},transaction{},relation{};std::uint64_t reservation_generation=0,local_transaction_id=0,catalog_generation=0,policy_generation=0,availability_generation=0;std::uint8_t mode=0;RrSha reservation_evidence{};};
struct SblrReservationReleaseResultV1{RrUuid transaction{},reservation{},relation{};std::uint64_t local_transaction_id=0,reservation_generation=0,release_sequence=0,availability_generation=0;std::uint8_t final_state=0;RrSha release_evidence{},result_evidence{};};
std::vector<std::uint8_t> EncodeSblrReservationReleaseRequestV1(const SblrReservationReleaseRequestV1&);bool DecodeSblrReservationReleaseRequestV1(const std::uint8_t*,std::size_t,SblrReservationReleaseRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrReservationReleaseDescriptorV1(const SblrReservationReleaseDescriptorV1&,bool operand=false);bool DecodeSblrReservationReleaseDescriptorV1(const std::uint8_t*,std::size_t,SblrReservationReleaseDescriptorV1*,std::string*,bool operand=false);
std::vector<std::uint8_t> EncodeSblrReservationReleaseResultV1(const SblrReservationReleaseResultV1&);bool DecodeSblrReservationReleaseResultV1(const std::uint8_t*,std::size_t,SblrReservationReleaseResultV1*,std::string*);
}
