#include "engine/sblr/sblr_bulk_import_stream_runtime.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>

namespace {

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

scratchbird::engine::sblr::SblrBulkImportStreamDescriptorV1 Descriptor() {
    namespace s = scratchbird::engine::sblr;
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

scratchbird::engine::sblr::SblrBulkImportStreamResultV1 Result(
    const scratchbird::engine::sblr::SblrBulkImportStreamDescriptorV1&
        descriptor) {
    namespace s = scratchbird::engine::sblr;
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

}  // namespace

int main()
{
    namespace s = scratchbird::engine::sblr;
    std::string detail;

    s::SblrBulkImportStreamRequestV1 request;
    request.receipt[0] = 1;
    request.occurrence = 1;
    request.import_occurrence = 1;
    auto request_bytes = s::EncodeSblrBulkImportStreamRequestV1(request);
    s::SblrBulkImportStreamRequestV1 decoded_request;
    if (request_bytes.size() != 64 ||
        !s::DecodeSblrBulkImportStreamRequestV1(
            request_bytes.data(), request_bytes.size(), &decoded_request,
            &detail)) {
        return 1;
    }
    if (s::BulkImportWireKindOf(request_bytes.data(), request_bytes.size()) != s::BulkImportWireKind::request ||
        s::RequestReceipt(decoded_request)[0] != 1 || s::RequestOccurrence(decoded_request) != 1 ||
        s::RequestImportOccurrence(decoded_request) != 1) return 12;
    request_bytes[44] = 1;
    if (s::DecodeSblrBulkImportStreamRequestV1(
            request_bytes.data(), request_bytes.size(), &decoded_request,
            &detail)) {
        return 2;
    }

    auto descriptor = Descriptor();
    auto descriptor_bytes =
        s::EncodeSblrBulkImportStreamDescriptorV1(descriptor, false);
    s::SblrBulkImportStreamDescriptorV1 decoded_descriptor;
    if (descriptor_bytes.size() != 424 ||
        !s::DecodeSblrBulkImportStreamDescriptorV1(
            descriptor_bytes.data(), descriptor_bytes.size(),
            &decoded_descriptor, &detail, false)) {
        return 3;
    }
    if (s::BulkImportWireKindOf(descriptor_bytes.data(), descriptor_bytes.size()) != s::BulkImportWireKind::descriptor ||
        s::DescriptorCanonicalBody(decoded_descriptor)[0] != 1 || s::DescriptorAvailabilityGeneration(decoded_descriptor) != 1) return 13;
    auto operation_bytes =
        s::EncodeSblrBulkImportStreamDescriptorV1(decoded_descriptor, true);
    if (operation_bytes.size() != 424 ||
        !s::DecodeSblrBulkImportStreamDescriptorV1(
            operation_bytes.data(), operation_bytes.size(),
            &decoded_descriptor, &detail, true)) {
        return 4;
    }
    operation_bytes[384] ^= 1;
    if (s::DecodeSblrBulkImportStreamDescriptorV1(
            operation_bytes.data(), operation_bytes.size(),
            &decoded_descriptor, &detail, true)) {
        return 5;
    }

    auto result = Result(descriptor);
    auto result_bytes = s::EncodeSblrBulkImportStreamResultV1(result);
    s::SblrBulkImportStreamResultV1 decoded_result;
    if (result_bytes.size() != 192 ||
        !s::DecodeSblrBulkImportStreamResultV1(
            result_bytes.data(), result_bytes.size(), &decoded_result,
            &detail)) {
        return 6;
    }
    if (s::BulkImportWireKindOf(result_bytes.data(), result_bytes.size()) != s::BulkImportWireKind::result ||
        s::ResultCanonicalBody(decoded_result)[0] != 17 || s::ResultAvailabilityGeneration(decoded_result) != 1) return 14;
    result_bytes[152] ^= 1;
    if (s::DecodeSblrBulkImportStreamResultV1(
            result_bytes.data(), result_bytes.size(), &decoded_result,
            &detail)) {
        return 7;
    }

    if (s::DecodeSblrBulkImportStreamRequestV1(
            nullptr, 64, &decoded_request, &detail)) {
        return 8;
    }
    if (s::DecodeSblrBulkImportStreamDescriptorV1(
            nullptr, 424, &decoded_descriptor, &detail, false)) {
        return 9;
    }
    if (s::DecodeSblrBulkImportStreamDescriptorV1(
            nullptr, 424, &decoded_descriptor, &detail, true)) {
        return 10;
    }
    if (s::DecodeSblrBulkImportStreamResultV1(
            nullptr, 192, &decoded_result, &detail)) {
        return 11;
    }

    return 0;
}
