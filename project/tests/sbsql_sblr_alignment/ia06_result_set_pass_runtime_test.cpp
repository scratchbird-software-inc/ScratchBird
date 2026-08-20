#include "engine/sblr/sblr_result_set_pass_runtime.hpp"

int main() {
    using namespace scratchbird::engine::sblr;
    std::string detail;
    SblrResultSetPassRequestV1 request;
    request.receipt[0] = 1;
    request.occurrence = 2;
    request.pass_occurrence = 3;
    auto request_bytes = EncodeSblrResultSetPassRequestV1(request);
    SblrResultSetPassRequestV1 decoded_request;
    if (request_bytes.size() != 64 ||
        !DecodeSblrResultSetPassRequestV1(request_bytes.data(), request_bytes.size(),
                                          &decoded_request, &detail)) return 1;

    SblrResultSetPassDescriptorV1 descriptor;
    descriptor.descriptor[0] = descriptor.source_handle[0] =
        descriptor.owner_session[0] = descriptor.owner_transaction[0] =
        descriptor.row_shape[0] = descriptor.security[0] = descriptor.mga[0] =
        descriptor.recipient_session[0] = descriptor.source_evidence[0] = 1;
    descriptor.descriptor_generation = descriptor.source_generation =
        descriptor.owner_local_transaction_id = descriptor.row_shape_generation =
        descriptor.security_generation = descriptor.mga_generation =
        descriptor.availability_generation = 1;
    descriptor.lifetime = 1;
    descriptor.transfer_mode = 1;
    descriptor.source_state = 1;
    descriptor.expiry_monotonic_ns = 10;
    descriptor.maximum_consumers = 1;
    auto issued = EncodeSblrResultSetPassDescriptorV1(descriptor, false);
    SblrResultSetPassDescriptorV1 decoded_descriptor;
    if (issued.size() != 288 ||
        !DecodeSblrResultSetPassDescriptorV1(issued.data(), issued.size(),
                                             &decoded_descriptor, &detail, false)) return 2;
    auto operand = EncodeSblrResultSetPassDescriptorV1(decoded_descriptor, true);
    if (operand.size() != 288 ||
        !DecodeSblrResultSetPassDescriptorV1(operand.data(), operand.size(),
                                             &decoded_descriptor, &detail, true)) return 3;
    operand[195] = 1;
    if (DecodeSblrResultSetPassDescriptorV1(operand.data(), operand.size(),
                                            &decoded_descriptor, &detail, true)) return 4;

    SblrResultSetPassHandleV1 handle;
    handle.descriptor[0] = handle.passed_handle[0] = handle.source_handle[0] =
        handle.recipient_session[0] = handle.row_shape[0] = handle.lease[0] =
        handle.transfer_evidence[0] = 1;
    handle.descriptor_generation = handle.passed_generation =
        handle.availability_generation = 1;
    handle.lifetime = 1;
    handle.state = 1;
    handle.expiry_monotonic_ns = 10;
    auto handle_bytes = EncodeSblrResultSetPassHandleV1(handle);
    SblrResultSetPassHandleV1 decoded_handle;
    return handle_bytes.size() == 216 &&
           DecodeSblrResultSetPassHandleV1(handle_bytes.data(), handle_bytes.size(),
                                            &decoded_handle, &detail) ? 0 : 5;
}
