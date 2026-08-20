#include "engine/sblr/sblr_read_range_runtime.hpp"

int main()
{
    using namespace scratchbird::engine::sblr;
    std::string detail;

    SblrReadRangeRequestV1 request;
    request.receipt[0] = 1;
    request.occurrence = 2;
    request.range_occurrence = 3;
    auto request_bytes = EncodeSblrReadRangeRequestV1(request);
    SblrReadRangeRequestV1 decoded_request;
    if (request_bytes.size() != 64 ||
        !DecodeSblrReadRangeRequestV1(request_bytes.data(), request_bytes.size(), &decoded_request, &detail)) return 1;

    SblrReadRangeDescriptorV1 descriptor;
    descriptor.descriptor[0] = descriptor.relation[0] = descriptor.index[0] = 1;
    descriptor.schema[0] = descriptor.mga[0] = descriptor.security[0] = 1;
    descriptor.key_type[0] = descriptor.codec[0] = descriptor.collation[0] = 1;
    descriptor.descriptor_generation = descriptor.relation_generation = descriptor.index_generation = 1;
    descriptor.schema_generation = descriptor.mga_generation = descriptor.security_generation = 1;
    descriptor.codec_generation = descriptor.collation_generation = descriptor.availability_generation = 1;
    descriptor.lower_state = descriptor.upper_state = 1;
    descriptor.lower_length = descriptor.upper_length = 2;
    descriptor.lower[0] = 0x11; descriptor.lower[1] = 0x12;
    descriptor.upper[0] = 0x21; descriptor.upper[1] = 0x22;
    descriptor.inclusivity = 3;
    descriptor.direction = 1;
    descriptor.maximum_rows = 64;

    auto issued = EncodeSblrReadRangeDescriptorV1(descriptor, false);
    SblrReadRangeDescriptorV1 decoded_descriptor;
    if (issued.size() != 404 ||
        !DecodeSblrReadRangeDescriptorV1(issued.data(), issued.size(), &decoded_descriptor, &detail, false)) return 2;
    auto operand = EncodeSblrReadRangeDescriptorV1(decoded_descriptor, true);
    if (operand.size() != 404 ||
        !DecodeSblrReadRangeDescriptorV1(operand.data(), operand.size(), &decoded_descriptor, &detail, true)) return 3;

    auto malformed = operand;
    malformed[214] ^= 1; // mutate canonical lower-bound padding
    if (DecodeSblrReadRangeDescriptorV1(malformed.data(), malformed.size(), &decoded_descriptor, &detail, true)) return 4;
    malformed = operand;
    malformed[203] = 0; // invalid direction
    if (DecodeSblrReadRangeDescriptorV1(malformed.data(), malformed.size(), &decoded_descriptor, &detail, true)) return 5;

    SblrReadRangeResultV1 result;
    result.descriptor[0] = result.relation[0] = result.batch[0] = 1;
    result.descriptor_generation = result.availability_generation = 1;
    result.rows = 2;
    result.eof = 1;
    result.batch_sha[0] = result.continuation[0] = 1;
    auto result_bytes = EncodeSblrReadRangeResultV1(result);
    SblrReadRangeResultV1 decoded_result;
    if (result_bytes.size() != 184 ||
        !DecodeSblrReadRangeResultV1(result_bytes.data(), result_bytes.size(), &decoded_result, &detail)) return 6;
    return 0;
}
