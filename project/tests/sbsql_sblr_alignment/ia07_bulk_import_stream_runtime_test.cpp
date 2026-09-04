#include "engine/sblr/sblr_bulk_import_stream_runtime.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace s = scratchbird::engine::sblr;

[[noreturn]] void Fail(std::string_view detail) {
    std::cerr << "bulk_import_stream_runtime: " << detail << '\n';
    std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view detail) {
    if (!condition) Fail(detail);
}

template <std::size_t N>
void StoreU64(std::array<std::uint8_t, N>* body, std::size_t offset,
              std::uint64_t value) {
    for (std::size_t index = 0; index < 8; ++index) {
        (*body)[offset + index] =
            static_cast<std::uint8_t>(value >> (index * 8U));
    }
}

template <std::size_t N>
void Fill(std::array<std::uint8_t, N>* body, std::size_t offset,
          std::size_t extent, std::uint8_t seed) {
    for (std::size_t index = 0; index < extent; ++index) {
        (*body)[offset + index] =
            static_cast<std::uint8_t>(seed + index);
    }
}

template <std::size_t N>
void Clear(std::array<std::uint8_t, N>* body, std::size_t offset,
           std::size_t extent) {
    std::fill_n(body->begin() + offset, extent, 0);
}

s::SblrBulkImportStreamDescriptorV1 Descriptor() {
    s::SblrBulkImportStreamDescriptorV1 descriptor;
    for (const auto [offset, seed] : {
             std::pair<std::size_t, std::uint8_t>{0, 1},
             {32, 17}, {56, 33}, {80, 49}, {104, 65}, {120, 81},
             {144, 97}, {168, 113}, {192, 129}, {216, 145},
             {240, 161}, {328, 177}}) {
        Fill(&descriptor.canonical_body, offset, 16, seed);
    }
    for (const auto offset : std::array<std::size_t, 11>{
             16, 48, 72, 96, 136, 160, 184, 208, 232, 256, 344}) {
        StoreU64(&descriptor.canonical_body, offset, 1);
    }
    descriptor.canonical_body[24] = 1;
    Fill(&descriptor.canonical_body, 264, 32, 193);
    Fill(&descriptor.canonical_body, 296, 32, 225);
    descriptor.availability_generation = 1;
    return descriptor;
}

s::SblrBulkImportStreamResultV1 Result(
    const s::SblrBulkImportStreamDescriptorV1& descriptor) {
    s::SblrBulkImportStreamResultV1 result;
    std::copy_n(descriptor.canonical_body.begin() + 32, 16,
                result.canonical_body.begin());
    StoreU64(&result.canonical_body, 16, 1);
    Fill(&result.canonical_body, 24, 16, 201);
    StoreU64(&result.canonical_body, 40, 1);
    StoreU64(&result.canonical_body, 48, 1);
    StoreU64(&result.canonical_body, 56, 0);
    StoreU64(&result.canonical_body, 64, 4);
    StoreU64(&result.canonical_body, 72, 1);
    std::copy_n(descriptor.canonical_body.begin() + 80, 16,
                result.canonical_body.begin() + 80);
    StoreU64(&result.canonical_body, 96, 1);
    Fill(&result.canonical_body, 104, 32, 19);
    result.availability_generation = 1;
    return result;
}

template <typename Decode>
void RequireHeaderRefusals(const std::vector<std::uint8_t>& canonical,
                           Decode&& decode,
                           std::string_view carrier) {
    Require(!canonical.empty(), "canonical carrier missing");
    for (const auto offset : std::array<std::size_t, 7>{0, 4, 6, 8, 9, 12,
                                                        15}) {
        auto malformed = canonical;
        malformed[offset] ^= 0x01;
        Require(!decode(malformed),
                std::string(carrier) + " accepted a malformed header");
    }
    auto truncated = canonical;
    truncated.pop_back();
    Require(!decode(truncated),
            std::string(carrier) + " accepted a truncated carrier");
    auto trailing = canonical;
    trailing.push_back(0);
    Require(!decode(trailing),
            std::string(carrier) + " accepted trailing bytes");
}

void RequireDescriptorFieldRefusals(
    const s::SblrBulkImportStreamDescriptorV1& canonical) {
    std::string detail;
    for (const auto [offset, extent] : {
             std::pair<std::size_t, std::size_t>{0, 16}, {16, 8}, {24, 4},
             {32, 16}, {48, 8}, {56, 16}, {72, 8}, {80, 16}, {96, 8},
             {104, 16}, {120, 16}, {136, 8}, {144, 16}, {160, 8},
             {168, 16}, {184, 8}, {192, 16}, {208, 8}, {216, 16},
             {232, 8}, {240, 16}, {256, 8}, {264, 32}, {296, 32},
             {328, 16}, {344, 8}}) {
        auto value = canonical;
        Clear(&value.canonical_body, offset, extent);
        Require(!s::ValidateSblrBulkImportStreamDescriptorV1(value, &detail),
                "descriptor accepted a zero required field");
        Require(s::EncodeSblrBulkImportStreamDescriptorV1(value, false).empty(),
                "descriptor encoded a zero required field");
    }

    auto reserved_flag = canonical;
    reserved_flag.canonical_body[28] = 0x02;
    Require(!s::ValidateSblrBulkImportStreamDescriptorV1(reserved_flag,
                                                         &detail),
            "descriptor accepted a reserved flag");

    auto local_with_fence = canonical;
    Fill(&local_with_fence.canonical_body, 352, 16, 241);
    Require(!s::ValidateSblrBulkImportStreamDescriptorV1(local_with_fence,
                                                         &detail),
            "local descriptor accepted a cluster fence");

    auto cluster_without_fence = canonical;
    cluster_without_fence.canonical_body[28] = 1;
    Require(!s::ValidateSblrBulkImportStreamDescriptorV1(
                cluster_without_fence, &detail),
            "cluster descriptor accepted a missing fence");

    auto cluster = canonical;
    cluster.canonical_body[28] = 1;
    Fill(&cluster.canonical_body, 352, 16, 241);
    Require(s::ValidateSblrBulkImportStreamDescriptorV1(cluster, &detail),
            "canonical cluster descriptor was refused");
    Require(!s::EncodeSblrBulkImportStreamDescriptorV1(cluster, false).empty(),
            "canonical cluster descriptor did not encode");

    auto no_availability = canonical;
    no_availability.availability_generation = 0;
    Require(!s::ValidateSblrBulkImportStreamDescriptorV1(no_availability,
                                                         &detail),
            "descriptor accepted zero executor generation");
}

void RequireResultFieldRefusals(
    const s::SblrBulkImportStreamResultV1& canonical) {
    std::string detail;
    for (const auto [offset, extent] : {
             std::pair<std::size_t, std::size_t>{0, 16}, {16, 8}, {24, 16},
             {40, 8}, {48, 8}, {64, 8}, {72, 8}, {80, 16}, {96, 8},
             {104, 32}}) {
        auto value = canonical;
        Clear(&value.canonical_body, offset, extent);
        Require(!s::ValidateSblrBulkImportStreamResultV1(value, &detail),
                "BIRS accepted a zero required field");
        Require(s::EncodeSblrBulkImportStreamResultV1(value).empty(),
                "BIRS encoded a zero required field");
    }

    auto same_publication = canonical;
    std::copy_n(same_publication.canonical_body.begin(), 16,
                same_publication.canonical_body.begin() + 24);
    Require(!s::ValidateSblrBulkImportStreamResultV1(same_publication,
                                                     &detail),
            "BIRS accepted publication identity equal to stream identity");

    auto rejected = canonical;
    StoreU64(&rejected.canonical_body, 56, 1);
    Require(!s::ValidateSblrBulkImportStreamResultV1(rejected, &detail),
            "BIRS accepted rejected rows in v1");

    for (const auto [offset, over_limit] : {
             std::pair<std::size_t, std::uint64_t>{48, 1'048'577ULL},
             {64, 17'179'869'185ULL}, {72, 262'145ULL}}) {
        auto value = canonical;
        StoreU64(&value.canonical_body, offset, over_limit);
        Require(!s::ValidateSblrBulkImportStreamResultV1(value, &detail),
                "BIRS accepted an absolute limit violation");
    }

    auto no_availability = canonical;
    no_availability.availability_generation = 0;
    Require(!s::ValidateSblrBulkImportStreamResultV1(no_availability,
                                                     &detail),
            "BIRS accepted zero executor generation");
}

}  // namespace

int main() {
    std::string detail;

    s::SblrBulkImportStreamRequestV1 request;
    request.receipt[0] = 1;
    request.occurrence = 1;
    request.import_occurrence = 1;
    const auto request_bytes = s::EncodeSblrBulkImportStreamRequestV1(request);
    s::SblrBulkImportStreamRequestV1 decoded_request;
    auto decode_request = [&](const std::vector<std::uint8_t>& bytes) {
        return s::DecodeSblrBulkImportStreamRequestV1(
            bytes.data(), bytes.size(), &decoded_request, &detail);
    };
    Require(request_bytes.size() == s::BulkImportWireLayout::request_size &&
                decode_request(request_bytes),
            "canonical BIRQ did not decode");
    Require(s::EncodeSblrBulkImportStreamRequestV1(decoded_request) ==
                request_bytes,
            "BIRQ was not byte-identical after re-encoding");
    Require(s::BulkImportWireKindOf(request_bytes.data(), request_bytes.size()) ==
                s::BulkImportWireKind::request &&
                s::RequestReceipt(decoded_request)[0] == 1 &&
                s::RequestOccurrence(decoded_request) == 1 &&
                s::RequestImportOccurrence(decoded_request) == 1,
            "BIRQ typed accessors drifted");
    RequireHeaderRefusals(request_bytes, decode_request, "BIRQ");
    for (const auto [offset, extent] : {
             std::pair<std::size_t, std::size_t>{16, 16}, {32, 8}, {40, 4}}) {
        auto malformed = request_bytes;
        std::fill_n(malformed.begin() + offset, extent, 0);
        Require(!decode_request(malformed),
                "BIRQ accepted a zero required identity");
    }
    for (const auto offset : {44U, 63U}) {
        auto malformed = request_bytes;
        malformed[offset] = 1;
        Require(!decode_request(malformed),
                "BIRQ accepted non-zero reserved bytes");
    }

    const auto descriptor = Descriptor();
    RequireDescriptorFieldRefusals(descriptor);
    const auto descriptor_bytes =
        s::EncodeSblrBulkImportStreamDescriptorV1(descriptor, false);
    s::SblrBulkImportStreamDescriptorV1 decoded_descriptor;
    auto decode_descriptor = [&](const std::vector<std::uint8_t>& bytes) {
        return s::DecodeSblrBulkImportStreamDescriptorV1(
            bytes.data(), bytes.size(), &decoded_descriptor, &detail, false);
    };
    Require(descriptor_bytes.size() == s::BulkImportWireLayout::descriptor_size &&
                decode_descriptor(descriptor_bytes),
            "canonical BIRD did not decode");
    Require(s::EncodeSblrBulkImportStreamDescriptorV1(decoded_descriptor,
                                                      false) == descriptor_bytes,
            "BIRD was not byte-identical after re-encoding");
    Require(s::BulkImportWireKindOf(descriptor_bytes.data(),
                                    descriptor_bytes.size()) ==
                s::BulkImportWireKind::descriptor &&
                s::DescriptorAvailabilityGeneration(decoded_descriptor) == 1,
            "BIRD typed accessors drifted");
    RequireHeaderRefusals(descriptor_bytes, decode_descriptor, "BIRD");
    auto bad_evidence = descriptor_bytes;
    bad_evidence[384] ^= 1;
    Require(!decode_descriptor(bad_evidence), "BIRD accepted evidence drift");

    auto operation_bytes = descriptor_bytes;
    operation_bytes[0] = 'B';
    operation_bytes[1] = 'I';
    operation_bytes[2] = 'R';
    operation_bytes[3] = 'O';
    s::SblrBulkImportStreamDescriptorV1 decoded_operation;
    auto decode_operation = [&](const std::vector<std::uint8_t>& bytes) {
        return s::DecodeSblrBulkImportStreamDescriptorV1(
            bytes.data(), bytes.size(), &decoded_operation, &detail, true);
    };
    Require(decode_operation(operation_bytes), "canonical BIRO did not decode");
    Require(std::equal(descriptor_bytes.begin() + 4, descriptor_bytes.end(),
                       operation_bytes.begin() + 4),
            "BIRO changed bytes beyond the literal magic replacement");
    Require(s::EncodeSblrBulkImportStreamDescriptorV1(decoded_operation, true) ==
                operation_bytes,
            "BIRO was not byte-identical after re-encoding");
    RequireHeaderRefusals(operation_bytes, decode_operation, "BIRO");

    const auto result = Result(descriptor);
    RequireResultFieldRefusals(result);
    const auto result_bytes = s::EncodeSblrBulkImportStreamResultV1(result);
    s::SblrBulkImportStreamResultV1 decoded_result;
    auto decode_result = [&](const std::vector<std::uint8_t>& bytes) {
        return s::DecodeSblrBulkImportStreamResultV1(
            bytes.data(), bytes.size(), &decoded_result, &detail);
    };
    Require(result_bytes.size() == s::BulkImportWireLayout::result_size &&
                decode_result(result_bytes),
            "canonical BIRS did not decode");
    Require(s::EncodeSblrBulkImportStreamResultV1(decoded_result) == result_bytes,
            "BIRS was not byte-identical after re-encoding");
    Require(s::BulkImportWireKindOf(result_bytes.data(), result_bytes.size()) ==
                s::BulkImportWireKind::result &&
                s::ResultAvailabilityGeneration(decoded_result) == 1,
            "BIRS typed accessors drifted");
    RequireHeaderRefusals(result_bytes, decode_result, "BIRS");
    bad_evidence = result_bytes;
    bad_evidence[152] ^= 1;
    Require(!decode_result(bad_evidence), "BIRS accepted evidence drift");

    Require(!s::DecodeSblrBulkImportStreamRequestV1(
                nullptr, request_bytes.size(), &decoded_request, &detail) &&
                !s::DecodeSblrBulkImportStreamDescriptorV1(
                    nullptr, descriptor_bytes.size(), &decoded_descriptor,
                    &detail, false) &&
                !s::DecodeSblrBulkImportStreamDescriptorV1(
                    nullptr, operation_bytes.size(), &decoded_operation,
                    &detail, true) &&
                !s::DecodeSblrBulkImportStreamResultV1(
                    nullptr, result_bytes.size(), &decoded_result, &detail),
            "a fixed carrier accepted null input");

    return EXIT_SUCCESS;
}
