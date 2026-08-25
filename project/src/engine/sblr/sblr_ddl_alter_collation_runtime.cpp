#include "engine/sblr/sblr_ddl_alter_collation_runtime.hpp"
#include "engine/sblr/sblr_ddl_create_collation_runtime.hpp"
#include <algorithm>
#include <cstring>
namespace scratchbird::engine::sblr {
std::vector<std::uint8_t> EncodeSblrDdlAlterCollationRequestV1(const SblrDdlAlterCollationRequestV1& v){SblrDdlCreateCollationRequestV1 x; x.operation=v.operation;x.receipt=v.receipt;x.descriptor_length=v.descriptor_length;auto b=EncodeSblrDdlCreateCollationRequestV1(x);if(b.size()!=64){return{};}b[8]=0x9f;b[9]=0x1d;b[10]=0;b[11]=0;b[12]=0x38;b[13]=0x02;return b;}
bool DecodeSblrDdlAlterCollationRequestV1(const std::uint8_t*b,std::size_t n,SblrDdlAlterCollationRequestV1*o,std::string*e){if(!o||n!=64||std::memcmp(b,"SBDQ",4)!=0||b[8]!=0x9f||b[9]!=0x1d||b[12]!=0x38||b[13]!=0x02)return false;std::copy_n(b+16,16,o->operation.begin());std::copy_n(b+32,16,o->receipt.begin());o->descriptor_length=static_cast<std::uint32_t>(b[48]|(b[49]<<8)|(b[50]<<16)|(b[51]<<24));return o->descriptor_length==384;}
std::vector<std::uint8_t> EncodeSblrDdlAlterCollationDescriptorV1(const SblrDdlAlterCollationDescriptorV1& v){SblrDdlCreateCollationDescriptorV1 x;x.body=v.body;return EncodeSblrDdlCreateCollationDescriptorV1(x);}
bool DecodeSblrDdlAlterCollationDescriptorV1(const std::uint8_t*b,std::size_t n,SblrDdlAlterCollationDescriptorV1*o,std::string*e){if(!o)return false;SblrDdlCreateCollationDescriptorV1 x; if(!DecodeSblrDdlCreateCollationDescriptorV1(b,n,&x,e))return false;o->body=x.body;return true;}
std::vector<std::uint8_t> EncodeSblrDdlAlterCollationResultV1(const SblrDdlAlterCollationResultV1& v){SblrDdlCreateCollationResultV1 x;x.body=v.body;return EncodeSblrDdlCreateCollationResultV1(x);}
bool DecodeSblrDdlAlterCollationResultV1(const std::uint8_t*b,std::size_t n,SblrDdlAlterCollationResultV1*o,std::string*e){if(!o)return false;SblrDdlCreateCollationResultV1 x;if(!DecodeSblrDdlCreateCollationResultV1(b,n,&x,e))return false;o->body=x.body;return true;}
}
