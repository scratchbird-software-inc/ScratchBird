#include "engine/sblr/sblr_ddl_drop_continuous_view_runtime.hpp"

#include <cassert>

int main()
{
    using namespace scratchbird::engine::sblr;

    std::string detail;
    SblrDdlDropContinuousViewRequestV1 request;
    request.occurrence = 1;
    request.view_occurrence = 2;
    const auto encoded_request = EncodeSblrDdlDropContinuousViewRequestV1(request);
    assert(encoded_request.size() == 64);
    assert(encoded_request[0] == 'D' && encoded_request[1] == 'V' &&
           encoded_request[2] == 'R' && encoded_request[3] == 'Q');
    SblrDdlDropContinuousViewRequestV1 decoded_request;
    assert(DecodeSblrDdlDropContinuousViewRequestV1(encoded_request.data(),
                                                    encoded_request.size(),
                                                    &decoded_request,
                                                    &detail));

    SblrDdlDropContinuousViewDescriptorV1 descriptor;
    descriptor.availability = 1;
    descriptor.body[16] = 7;
    descriptor.body[247] = 19;
    for (const bool operand : {false, true}) {
        auto encoded_descriptor =
            EncodeSblrDdlDropContinuousViewDescriptorV1(descriptor, operand);
        assert(encoded_descriptor.size() == 488);
        assert(encoded_descriptor[0] == 'D' && encoded_descriptor[1] == 'V' &&
               encoded_descriptor[2] == 'D' &&
               encoded_descriptor[3] == (operand ? 'O' : 'D'));
        SblrDdlDropContinuousViewDescriptorV1 decoded_descriptor;
        assert(DecodeSblrDdlDropContinuousViewDescriptorV1(encoded_descriptor.data(),
                                                           encoded_descriptor.size(),
                                                           &decoded_descriptor,
                                                           &detail,
                                                           operand));
        assert(EncodeSblrDdlDropContinuousViewDescriptorV1(decoded_descriptor, operand) ==
               encoded_descriptor);

        encoded_descriptor[20] ^= 1;
        assert(!DecodeSblrDdlDropContinuousViewDescriptorV1(encoded_descriptor.data(),
                                                            encoded_descriptor.size(),
                                                            &decoded_descriptor,
                                                            &detail,
                                                            operand));
    }

    SblrDdlDropContinuousViewResultV1 result;
    result.body[0] = 11;
    result.body[173] = 23;
    result.availability = 1;
    result.publication_barrier[0] = 29;
    auto encoded_result = EncodeSblrDdlDropContinuousViewResultV1(result);
    assert(encoded_result.size() == 320);
    SblrDdlDropContinuousViewResultV1 decoded_result;
    assert(DecodeSblrDdlDropContinuousViewResultV1(encoded_result.data(),
                                                   encoded_result.size(),
                                                   &decoded_result,
                                                   &detail));
    assert(EncodeSblrDdlDropContinuousViewResultV1(decoded_result) == encoded_result);
    encoded_result[20] ^= 1;
    assert(!DecodeSblrDdlDropContinuousViewResultV1(encoded_result.data(),
                                                    encoded_result.size(),
                                                    &decoded_result,
                                                    &detail));
    return 0;
}
