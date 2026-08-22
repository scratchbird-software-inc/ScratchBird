#include "sblr_ddl_alter_continuous_view_runtime.hpp"
#include <cstring>
namespace scratchbird::engine::sblr { namespace { std::vector<std::uint8_t> magic(const std::uint8_t*p,std::size_t n,std::uint32_t m){std::vector<std::uint8_t>b(p,p+n);if(b.size()>=4)std::memcpy(b.data(),&m,4);return b;} }
std::vector<std::uint8_t> EncodeSblrDdlAlterContinuousViewRequestV1(const SblrDdlAlterContinuousViewRequestV1&v){auto b=EncodeSblrDdlCreateContinuousViewRequestV1(v);if(b.size()>=4)std::memcpy(b.data(),"AVRQ",4);return b;}
bool DecodeSblrDdlAlterContinuousViewRequestV1(const std::uint8_t*p,std::size_t n,SblrDdlAlterContinuousViewRequestV1*o,std::string*d){auto b=magic(p,n,0x43565251u);return DecodeSblrDdlCreateContinuousViewRequestV1(b.data(),b.size(),o,d);}
std::vector<std::uint8_t> EncodeSblrDdlAlterContinuousViewDescriptorV1(const SblrDdlAlterContinuousViewDescriptorV1&v,bool operand){auto b=EncodeSblrDdlCreateContinuousViewDescriptorV1(v,operand);if(b.size()>=4)std::memcpy(b.data(),operand?"AVDO":"AVDD",4);return b;}
bool DecodeSblrDdlAlterContinuousViewDescriptorV1(const std::uint8_t*p,std::size_t n,SblrDdlAlterContinuousViewDescriptorV1*o,std::string*d,bool operand){auto b=magic(p,n,operand?0x4356444fu:0x43564444u);return DecodeSblrDdlCreateContinuousViewDescriptorV1(b.data(),b.size(),o,d,operand);}
std::vector<std::uint8_t> EncodeSblrDdlAlterContinuousViewResultV1(const SblrDdlAlterContinuousViewResultV1&v){auto b=EncodeSblrDdlCreateContinuousViewResultV1(v);if(b.size()>=4)std::memcpy(b.data(),"AVRS",4);return b;}
bool DecodeSblrDdlAlterContinuousViewResultV1(const std::uint8_t*p,std::size_t n,SblrDdlAlterContinuousViewResultV1*o,std::string*d){auto b=magic(p,n,0x43565253u);return DecodeSblrDdlCreateContinuousViewResultV1(b.data(),b.size(),o,d);}
}
