#include "engine/sblr/sblr_bulk_import_stream_runtime.hpp"

#include <string>

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
    request_bytes[44] = 1;
    if (s::DecodeSblrBulkImportStreamRequestV1(
            request_bytes.data(), request_bytes.size(), &decoded_request,
            &detail)) {
        return 2;
    }

    s::SblrBulkImportStreamDescriptorV1 descriptor;
    descriptor.canonical_body[0] = 1;
    descriptor.availability_generation = 1;
    auto descriptor_bytes =
        s::EncodeSblrBulkImportStreamDescriptorV1(descriptor, false);
    s::SblrBulkImportStreamDescriptorV1 decoded_descriptor;
    if (descriptor_bytes.size() != 424 ||
        !s::DecodeSblrBulkImportStreamDescriptorV1(
            descriptor_bytes.data(), descriptor_bytes.size(),
            &decoded_descriptor, &detail, false)) {
        return 3;
    }
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

    s::SblrBulkImportStreamResultV1 result;
    result.canonical_body[0] = 1;
    result.availability_generation = 1;
    auto result_bytes = s::EncodeSblrBulkImportStreamResultV1(result);
    s::SblrBulkImportStreamResultV1 decoded_result;
    if (result_bytes.size() != 192 ||
        !s::DecodeSblrBulkImportStreamResultV1(
            result_bytes.data(), result_bytes.size(), &decoded_result,
            &detail)) {
        return 6;
    }
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
