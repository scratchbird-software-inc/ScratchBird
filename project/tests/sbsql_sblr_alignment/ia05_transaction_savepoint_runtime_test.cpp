#include "engine/sblr/sblr_savepoint_runtime.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace sblr = scratchbird::engine::sblr;

namespace {
template <class Array>
void Seed(Array& value, std::uint8_t seed) {
    for (std::size_t i = 0; i < value.size(); ++i) {
        value[i] = static_cast<std::uint8_t>(seed + i);
    }
}

template <class Value, class Decode>
bool RejectMutation(const std::vector<std::uint8_t>& canonical,
                    std::size_t offset,
                    Decode decode) {
    auto malformed = canonical;
    malformed[offset] ^= 0x80;
    Value value;
    std::string detail;
    return !decode(malformed.data(), malformed.size(), &value, &detail);
}
}  // namespace

int main() {
    sblr::SblrSavepointCoordinationRequestV1 request;
    Seed(request.preliminary_receipt_uuid, 1);
    Seed(request.transaction_uuid, 21);
    request.local_transaction_id = 41;
    Seed(request.transaction_handle_evidence_sha256, 51);
    request.symbol_occurrence_id = 91;
    Seed(request.canonical_symbol_sha256, 101);
    const auto request_bytes = sblr::EncodeSblrSavepointCoordinationRequestV1(request);
    sblr::SblrSavepointCoordinationRequestV1 decoded_request;
    std::string detail;
    if (request_bytes.size() != 128 ||
        !sblr::DecodeSblrSavepointCoordinationRequestV1(
            request_bytes.data(), request_bytes.size(), &decoded_request, &detail) ||
        decoded_request.symbol_occurrence_id != request.symbol_occurrence_id ||
        !RejectMutation<sblr::SblrSavepointCoordinationRequestV1>(
            request_bytes, 7, sblr::DecodeSblrSavepointCoordinationRequestV1) ||
        !RejectMutation<sblr::SblrSavepointCoordinationRequestV1>(
            request_bytes, 12, sblr::DecodeSblrSavepointCoordinationRequestV1)) {
        return 1;
    }

    sblr::SblrSavepointCoordinationResultV1 result;
    result.preliminary_receipt_uuid = request.preliminary_receipt_uuid;
    Seed(result.descriptor_uuid, 121);
    result.descriptor_generation = 2;
    Seed(result.savepoint_uuid, 141);
    result.savepoint_generation = 3;
    result.transaction_ordinal = 1;
    result.symbol_occurrence_id = request.symbol_occurrence_id;
    result.canonical_symbol_sha256 = request.canonical_symbol_sha256;
    const auto result_bytes = sblr::EncodeSblrSavepointCoordinationResultV1(result);
    sblr::SblrSavepointCoordinationResultV1 decoded_result;
    if (result_bytes.size() != 160 ||
        !sblr::DecodeSblrSavepointCoordinationResultV1(
            result_bytes.data(), result_bytes.size(), &decoded_result, &detail) ||
        !RejectMutation<sblr::SblrSavepointCoordinationResultV1>(
            result_bytes, 9, sblr::DecodeSblrSavepointCoordinationResultV1) ||
        !RejectMutation<sblr::SblrSavepointCoordinationResultV1>(
            result_bytes, 128, sblr::DecodeSblrSavepointCoordinationResultV1)) {
        return 2;
    }

    sblr::SblrSavepointDescriptorV1 descriptor;
    descriptor.descriptor_uuid = result.descriptor_uuid;
    descriptor.descriptor_generation = result.descriptor_generation;
    descriptor.savepoint_uuid = result.savepoint_uuid;
    descriptor.savepoint_generation = result.savepoint_generation;
    descriptor.transaction_uuid = request.transaction_uuid;
    descriptor.local_transaction_id = request.local_transaction_id;
    descriptor.transaction_ordinal = result.transaction_ordinal;
    descriptor.descriptor_evidence_sha256 = decoded_result.descriptor_evidence_sha256;
    const auto descriptor_bytes = sblr::EncodeSblrSavepointDescriptorV1(descriptor);
    sblr::SblrSavepointDescriptorV1 decoded_descriptor;
    if (descriptor_bytes.size() != 128 ||
        !sblr::DecodeSblrSavepointDescriptorV1(
            descriptor_bytes.data(), descriptor_bytes.size(), &decoded_descriptor, &detail)) {
        return 3;
    }

    sblr::SblrSavepointHandleV1 handle;
    handle.savepoint_uuid = result.savepoint_uuid;
    handle.savepoint_generation = result.savepoint_generation;
    handle.transaction_uuid = request.transaction_uuid;
    handle.local_transaction_id = request.local_transaction_id;
    handle.transaction_ordinal = result.transaction_ordinal;
    handle.stack_generation = 4;
    handle.executor_availability_generation = 5;
    const auto handle_bytes = sblr::EncodeSblrSavepointHandleV1(handle);
    sblr::SblrSavepointHandleV1 decoded_handle;
    if (handle_bytes.size() != 144 ||
        !sblr::DecodeSblrSavepointHandleV1(
            handle_bytes.data(), handle_bytes.size(), &decoded_handle, &detail) ||
        !RejectMutation<sblr::SblrSavepointHandleV1>(
            handle_bytes, 81, sblr::DecodeSblrSavepointHandleV1) ||
        !RejectMutation<sblr::SblrSavepointHandleV1>(
            handle_bytes, 88, sblr::DecodeSblrSavepointHandleV1) ||
        !RejectMutation<sblr::SblrSavepointHandleV1>(
            handle_bytes, 143, sblr::DecodeSblrSavepointHandleV1)) {
        return 4;
    }
    return 0;
}
