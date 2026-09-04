#include "engine/sblr/sblr_ddl_alter_continuous_view_runtime.hpp"

#include <cassert>

int main()
{
    using namespace scratchbird::engine::sblr;

    std::string detail;
    SblrDdlAlterContinuousViewRequestV1 request;
    request.occurrence = 1;
    request.view_occurrence = 2;
    const auto encoded_request = EncodeSblrDdlAlterContinuousViewRequestV1(request);
    assert(encoded_request.size() == 64);
    assert(encoded_request[0] == 'A' && encoded_request[1] == 'V' &&
           encoded_request[2] == 'R' && encoded_request[3] == 'Q');
    SblrDdlAlterContinuousViewRequestV1 decoded_request;
    assert(DecodeSblrDdlAlterContinuousViewRequestV1(encoded_request.data(),
                                                     encoded_request.size(),
                                                     &decoded_request,
                                                     &detail));

    SblrDdlAlterContinuousViewDescriptorV1 descriptor;
    descriptor.availability = 1;
    descriptor.body[16] = 7;
    descriptor.body[247] = 19;
    for (const bool operand : {false, true}) {
        auto encoded_descriptor =
            EncodeSblrDdlAlterContinuousViewDescriptorV1(descriptor, operand);
        assert(encoded_descriptor.size() == 488);
        assert(encoded_descriptor[0] == 'A' && encoded_descriptor[1] == 'V' &&
               encoded_descriptor[2] == 'D' &&
               encoded_descriptor[3] == (operand ? 'O' : 'D'));
        SblrDdlAlterContinuousViewDescriptorV1 decoded_descriptor;
        assert(DecodeSblrDdlAlterContinuousViewDescriptorV1(encoded_descriptor.data(),
                                                            encoded_descriptor.size(),
                                                            &decoded_descriptor,
                                                            &detail,
                                                            operand));
        assert(EncodeSblrDdlAlterContinuousViewDescriptorV1(decoded_descriptor, operand) ==
               encoded_descriptor);

        encoded_descriptor[20] ^= 1;
        assert(!DecodeSblrDdlAlterContinuousViewDescriptorV1(encoded_descriptor.data(),
                                                             encoded_descriptor.size(),
                                                             &decoded_descriptor,
                                                             &detail,
                                                             operand));
    }

    SblrDdlAlterContinuousViewResultV1 result;
    result.body[0] = 11;
    result.body[173] = 23;
    result.availability = 1;
    result.publication_barrier[0] = 29;
    auto encoded_result = EncodeSblrDdlAlterContinuousViewResultV1(result);
    assert(encoded_result.size() == 320);
    SblrDdlAlterContinuousViewResultV1 decoded_result;
    assert(DecodeSblrDdlAlterContinuousViewResultV1(encoded_result.data(),
                                                    encoded_result.size(),
                                                    &decoded_result,
                                                    &detail));
    assert(EncodeSblrDdlAlterContinuousViewResultV1(decoded_result) == encoded_result);
    encoded_result[20] ^= 1;
    assert(!DecodeSblrDdlAlterContinuousViewResultV1(encoded_result.data(),
                                                     encoded_result.size(),
                                                     &decoded_result,
                                                     &detail));
    return 0;
}
